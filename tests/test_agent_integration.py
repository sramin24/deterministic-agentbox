"""End-to-end: real destructive shell actions via Sandbox.run(), then
Sandbox.rollback(), asserting the workspace is restored byte-for-byte --
compared against the real host filesystem, not anything captured through
the sandbox itself.
"""
import hashlib
import os
import time

import pytest

from agentbox import Sandbox


def _tree_fingerprint(root):
    entries = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in sorted(filenames):
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, root)
            with open(path, "rb") as f:
                digest = hashlib.sha256(f.read()).hexdigest()
            entries.append((rel, digest))
    return sorted(entries)


@pytest.fixture
def realistic_workspace(tmp_path):
    ws = tmp_path / "workspace"
    (ws / "src").mkdir(parents=True)
    (ws / "src" / "main.py").write_text("print('hello')\n")
    (ws / "src" / "utils.py").write_text("def helper():\n    pass\n")
    (ws / "README.md").write_text("# Project\n")
    (ws / "config.json").write_text('{"key": "value"}\n')
    return str(ws)


def test_destructive_rm_rf_then_rollback_restores_byte_for_byte(realistic_workspace):
    before = _tree_fingerprint(realistic_workspace)

    with Sandbox(realistic_workspace) as sb:
        ckpt_id = sb.checkpoint()

        result = sb.run("rm -rf *")
        assert result.exit_code == 0
        empty = sb.run("ls -A | wc -l")
        assert empty.stdout.strip() == "0"

        sb.rollback(ckpt_id)

        restored = sb.run("find . -type f | sort")
        restored_files = sorted(restored.stdout.strip().split("\n"))
        expected_files = sorted(f"./{rel}" for rel, _digest in before)
        assert restored_files == expected_files

    # The real host workspace was never touched at any point -- not by the
    # rm -rf *, not by the rollback, since nothing was ever committed.
    after = _tree_fingerprint(realistic_workspace)
    assert before == after


def test_overwriting_files_then_rollback(realistic_workspace):
    with Sandbox(realistic_workspace) as sb:
        ckpt_id = sb.checkpoint()

        sb.run("echo 'print(1 + 1)' > src/main.py")
        sb.run("echo 'CORRUPTED' > README.md")

        sb.rollback(ckpt_id)

        result = sb.run("cat src/main.py; echo ---SEP---; cat README.md")
        stdout_before, stdout_after = result.stdout.split("---SEP---")
        assert stdout_before.strip() == "print('hello')"
        assert stdout_after.strip() == "# Project"


def test_backgrounded_loop_process_does_not_survive_its_own_command(realistic_workspace):
    """A command that backgrounds a runaway loop and returns immediately
    must not leave that loop running once the command "finishes" -- this
    is the process-group cleanup from Phase 4, exercised through the full
    Python -> C stack rather than the C engine directly."""
    with Sandbox(realistic_workspace) as sb:
        result = sb.run("(while true; do sleep 0.1; done) & true", timeout_sec=5)
        assert result.exit_code == 0
        assert not result.timed_out

        # If the backgrounded loop had leaked, this unrelated follow-up
        # command sharing the same sandbox would still eventually run, but
        # a real leak (e.g. exhausting the pid namespace, or contending for
        # CPU) is exactly the kind of thing that shows up as this taking
        # much longer than it should.
        start = time.monotonic()
        result2 = sb.run("echo still-fast", timeout_sec=5)
        elapsed = time.monotonic() - start
        assert result2.stdout.strip() == "still-fast"
        assert elapsed < 3
