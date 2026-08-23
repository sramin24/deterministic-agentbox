"""The Sandbox class: Python's entry point to the C engine."""
import ctypes
from collections.abc import Callable, Iterator
from contextlib import contextmanager

from . import _core
from .errors import NamespaceUnavailableError, raise_for_code


class ExecResult:
    """Result of Sandbox.run(). stdout/stderr are read from disk lazily and
    cached -- most callers only look at one of them, or neither."""

    def __init__(self, exit_code: int, timed_out: bool, duration_ms: int,
                 stdout_path: str, stderr_path: str):
        self.exit_code = exit_code
        self.timed_out = timed_out
        self.duration_ms = duration_ms
        self.stdout_path = stdout_path
        self.stderr_path = stderr_path
        self._stdout: str | None = None
        self._stderr: str | None = None

    @property
    def stdout(self) -> str:
        if self._stdout is None:
            with open(self.stdout_path, "r", errors="replace") as f:
                self._stdout = f.read()
        return self._stdout

    @property
    def stderr(self) -> str:
        if self._stderr is None:
            with open(self.stderr_path, "r", errors="replace") as f:
                self._stderr = f.read()
        return self._stderr

    def __repr__(self) -> str:
        return (f"ExecResult(exit_code={self.exit_code}, timed_out={self.timed_out}, "
                f"duration_ms={self.duration_ms})")


class Sandbox:
    """A transactional, local-first execution sandbox for one workspace
    directory. See README.md for the full picture; in short: open one,
    run shell commands and let the agent mutate files freely inside it,
    checkpoint before risky steps, roll back instantly if they fail, and
    commit only the changes actually worth keeping.
    """

    def __init__(self, workspace_dir: str, runtime_dir: str | None = None):
        detail_buf = ctypes.create_string_buffer(256)
        probe_rc = _core.lib.agentbox_probe_capabilities(detail_buf, 256)
        if probe_rc != 0:
            detail = detail_buf.value.decode(errors="replace")
            raise NamespaceUnavailableError(
                "Sandbox is not usable on this machine yet "
                f"(capability probe: {detail or 'blocked'}). "
                "Run: sudo agentbox setup"
            )

        self._box = _core.AgentboxBox()
        rc = _core.lib.agentbox_create(
            ctypes.byref(self._box),
            workspace_dir.encode(),
            runtime_dir.encode() if runtime_dir else None,
        )
        raise_for_code(rc, "failed to create Sandbox")
        self._closed = False

    def checkpoint(self, tag: str | None = None) -> str:
        out_id = ctypes.create_string_buffer(_core.AGENTBOX_ID_LEN)
        rc = _core.lib.agentbox_checkpoint(
            ctypes.byref(self._box), (tag or "").encode(), out_id, _core.AGENTBOX_ID_LEN
        )
        raise_for_code(rc, "checkpoint failed")
        return out_id.value.decode()

    def run(self, cmd: str, timeout_sec: int = 30,
            on_output: Callable[[bool, bytes], None] | None = None) -> ExecResult:
        """Runs cmd inside the sandbox. If on_output is given, it's called
        as on_output(is_stderr, chunk) for every piece of output the
        command produces, live, as it's produced -- not just once at the
        end. Either way, the full captured output is also available
        afterward via the returned ExecResult's .stdout/.stderr."""
        result = _core.AgentboxExecResult()
        if on_output is not None:
            callback = _core.make_stream_callback(on_output)
            rc = _core.lib.agentbox_exec_streaming(
                ctypes.byref(self._box), None, cmd.encode(), timeout_sec,
                callback, None, ctypes.byref(result),
            )
        else:
            rc = _core.lib.agentbox_exec(
                ctypes.byref(self._box), None, cmd.encode(), timeout_sec, ctypes.byref(result)
            )
        raise_for_code(rc, "run failed")
        return ExecResult(
            result.exit_code, bool(result.timed_out), result.duration_ms,
            result.stdout_path.decode(), result.stderr_path.decode(),
        )

    def rollback(self, checkpoint_id: str | None = None) -> None:
        rc = _core.lib.agentbox_rollback(ctypes.byref(self._box), (checkpoint_id or "").encode())
        raise_for_code(rc, "rollback failed")

    def commit(self) -> None:
        rc = _core.lib.agentbox_commit(ctypes.byref(self._box))
        raise_for_code(rc, "commit failed")

    def diff(self) -> list[str]:
        paths_ptr = ctypes.POINTER(ctypes.c_char_p)()
        count = ctypes.c_size_t()
        rc = _core.lib.agentbox_diff(ctypes.byref(self._box), ctypes.byref(paths_ptr), ctypes.byref(count))
        raise_for_code(rc, "diff failed")
        try:
            return [paths_ptr[i].decode() for i in range(count.value)]
        finally:
            _core.lib.agentbox_free_string_list(paths_ptr, count.value)

    @contextmanager
    def transaction(self) -> Iterator["Sandbox"]:
        """Checkpoints on entry; rolls back to that checkpoint if the with
        block raises, leaves the change in place (uncommitted) otherwise."""
        ckpt_id = self.checkpoint()
        try:
            yield self
        except BaseException:
            self.rollback(ckpt_id)
            raise

    def close(self) -> None:
        if not self._closed:
            _core.lib.agentbox_destroy(ctypes.byref(self._box))
            self._closed = True

    def __enter__(self) -> "Sandbox":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    def __del__(self):
        # Best-effort: don't rely on this for correctness, an explicit
        # close()/`with` block should always be used, but a supervisor
        # process and its scratch directory shouldn't outlive a Sandbox
        # object that was simply dropped.
        try:
            self.close()
        except Exception:
            pass
