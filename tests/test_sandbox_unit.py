"""Sandbox lifecycle, error mapping, and transaction() semantics.

Runs against the real engine (this project's whole testing philosophy has
been "no mocks" against the actual Linux target) except where the point
of the test IS a failure path that would otherwise require deliberately
breaking the shared dev environment's AppArmor setup -- there, a
monkeypatched C call stands in.
"""
import ctypes
import os

import pytest

from agentbox import Sandbox, errors, _core


@pytest.fixture
def workspace(tmp_path):
    ws = tmp_path / "workspace"
    ws.mkdir()
    (ws / "file.txt").write_text("original")
    return str(ws)


def test_create_and_close(workspace):
    sb = Sandbox(workspace)
    assert sb._box.active == 1
    sb.close()
    assert sb._box.active == 0
    sb.close()  # idempotent, must not raise or double-free


def _tree_pss_kb(pid):
    total, stack = 0, [pid]
    while stack:
        p = stack.pop()
        with open(f"/proc/{p}/smaps_rollup") as f:
            total += next(int(line.split()[1]) for line in f if line.startswith("Pss:"))
        for tid in os.listdir(f"/proc/{p}/task"):
            with open(f"/proc/{p}/task/{tid}/children") as f:
                stack.extend(int(c) for c in f.read().split())
    return total


def test_idle_sandbox_memory_stays_small(workspace):
    # Regression guard for docs/benchmarks/concurrency.md: fixed-size
    # 256-checkpoint tables used to cost ~3.3 MB per supervisor (+ anchor)
    # and ~1 MB per client-side struct, whether or not checkpoints were used.
    assert ctypes.sizeof(_core.AgentboxBox) < 64 * 1024
    with Sandbox(workspace) as sb:
        assert _tree_pss_kb(sb._box.supervisor_pid) < 1024


def test_many_checkpoints_then_rollback_to_an_early_one(workspace):
    with Sandbox(workspace) as sb:
        ids = []
        for i in range(20):
            sb.run(f"echo -n v{i} > file.txt")
            ids.append(sb.checkpoint(f"step {i}"))
        sb.rollback(ids[4])
        assert sb.run("cat file.txt").stdout == "v4"
        sb.run("echo -n after > file.txt")
        sb.rollback()  # most recent remaining checkpoint is still ids[4]
        assert sb.run("cat file.txt").stdout == "v4"


def test_context_manager_closes_and_never_commits_implicitly(workspace):
    with Sandbox(workspace) as sb:
        result = sb.run("echo -n hi > out.txt")
        assert result.exit_code == 0
    # Never committed, so the real workspace must be untouched.
    assert not os.path.exists(os.path.join(workspace, "out.txt"))


def test_checkpoint_rollback_commit_flow(workspace):
    with Sandbox(workspace) as sb:
        sb.run("echo -n modified > file.txt")
        ckpt_id = sb.checkpoint("mid")
        assert ckpt_id

        sb.run("echo -n further > file.txt")
        sb.rollback(ckpt_id)

        result = sb.run("cat file.txt")
        assert result.stdout == "modified"
        sb.commit()

    with open(os.path.join(workspace, "file.txt")) as f:
        assert f.read() == "modified"


def test_diff_reports_changes_and_clears_after_commit(workspace):
    with Sandbox(workspace) as sb:
        sb.run("echo -n modified > file.txt")
        assert any("file.txt" in entry for entry in sb.diff())
        sb.commit()
        assert sb.diff() == []


def test_transaction_rolls_back_on_exception(workspace):
    with Sandbox(workspace) as sb:
        with pytest.raises(RuntimeError):
            with sb.transaction():
                sb.run("echo -n should-not-persist > file.txt")
                raise RuntimeError("simulated failure mid-transaction")

        result = sb.run("cat file.txt")
        assert result.stdout == "original"


def test_transaction_keeps_changes_without_exception(workspace):
    with Sandbox(workspace) as sb:
        with sb.transaction():
            sb.run("echo -n kept > file.txt")
        result = sb.run("cat file.txt")
        assert result.stdout == "kept"


def test_rollback_unknown_checkpoint_raises(workspace):
    with Sandbox(workspace) as sb:
        with pytest.raises(errors.CheckpointNotFoundError):
            sb.rollback("no-such-checkpoint")


def test_run_streaming_delivers_chunks(workspace):
    chunks = []
    with Sandbox(workspace) as sb:
        result = sb.run("echo one; echo two", on_output=lambda is_err, data: chunks.append((is_err, data)))
        assert result.exit_code == 0
    assert len(chunks) >= 1
    assert all(is_err is False for is_err, _ in chunks)
    assert b"one" in b"".join(d for _, d in chunks)
    assert b"two" in b"".join(d for _, d in chunks)


def test_capability_probe_failure_raises_actionable_error(workspace, monkeypatch):
    def fake_probe(detail_buf, detail_buf_len):
        detail_buf.value = b"simulated: unshare blocked"
        return 2  # AGENTBOX_PROBE_ID_MAP_DENIED

    monkeypatch.setattr(_core.lib, "agentbox_probe_capabilities", fake_probe)
    with pytest.raises(errors.NamespaceUnavailableError) as exc_info:
        Sandbox(workspace)
    assert "agentbox setup" in str(exc_info.value)


@pytest.mark.parametrize(
    "code,expected_exc",
    [
        (1, errors.GenericError),
        (2, errors.NamespaceUnavailableError),
        (3, errors.MountFailedError),
        (4, errors.CheckpointNotFoundError),
        (5, errors.GenericError),
        (6, errors.TimeoutError),
        (7, errors.SupervisorDownError),
        (8, errors.InvalidArgumentError),
        (9, errors.FilesystemMismatchError),
        (10, errors.PermissionError),
        (11, errors.CheckpointLimitError),
    ],
)
def test_error_code_mapping(code, expected_exc):
    with pytest.raises(expected_exc) as exc_info:
        errors.raise_for_code(code, "test context")
    assert exc_info.value.code == code


def test_raise_for_code_is_noop_on_success():
    errors.raise_for_code(0, "should not raise")
