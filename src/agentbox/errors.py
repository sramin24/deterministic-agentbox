"""Typed exceptions mirroring agentbox_errcode_t in src/c/agentbox.h."""


class AgentboxError(Exception):
    """Base class for every exception this package raises."""

    def __init__(self, message: str, code: int | None = None):
        super().__init__(message)
        self.code = code


class NamespaceUnavailableError(AgentboxError):
    """unshare()/uid_map/gid_map path is blocked. Fix: `sudo agentbox setup`."""


class MountFailedError(AgentboxError):
    """The OverlayFS (or fuse-overlayfs fallback) mount failed."""


class CheckpointNotFoundError(AgentboxError):
    """No checkpoint with the given id exists on this Sandbox."""


class TimeoutError(AgentboxError):
    """A Sandbox operation exceeded its allotted time.

    Distinct from builtins.TimeoutError -- Sandbox.run()'s own timeout is
    reported through ExecResult.timed_out, not by raising this; this is for
    the rarer case of a control-plane operation itself (not the command
    being run) failing to complete.
    """


class SupervisorDownError(AgentboxError):
    """The persistent supervisor process for this Sandbox is not responding."""


class InvalidArgumentError(AgentboxError):
    """A Sandbox call was given an argument the engine rejected."""


class FilesystemMismatchError(AgentboxError):
    """workspace_dir and runtime_dir must be on the same filesystem (same
    st_dev) -- OverlayFS requires lowerdir/upperdir/workdir to share one.
    Pass an explicit runtime_dir alongside workspace_dir to fix this."""


class PermissionError(AgentboxError):
    """Distinct from builtins.PermissionError -- raised for engine-level
    permission failures (e.g. calling the AppArmor installer without
    root), not raw OS filesystem permission errors."""


class CheckpointLimitError(AgentboxError):
    """This Sandbox has reached AGENTBOX_MAX_CHECKPOINTS (256) open
    checkpoints. Commit or roll back before checkpointing again."""


class GenericError(AgentboxError):
    """Catch-all for AGENTBOX_ERR_GENERIC and any I/O failure without a
    more specific mapping."""


# Mirrors agentbox_errcode_t in src/c/agentbox.h exactly -- keep in sync.
_CODE_TO_EXCEPTION = {
    1: GenericError,
    2: NamespaceUnavailableError,
    3: MountFailedError,
    4: CheckpointNotFoundError,
    5: GenericError,  # AGENTBOX_ERR_IO
    6: TimeoutError,
    7: SupervisorDownError,
    8: InvalidArgumentError,
    9: FilesystemMismatchError,
    10: PermissionError,
    11: CheckpointLimitError,
}


def raise_for_code(code: int, context: str) -> None:
    """Raises the exception matching a non-zero agentbox_errcode_t, with a
    message built from the C side's own agentbox_strerror() text."""
    from . import _core

    if code == 0:
        return
    detail = _core.lib.agentbox_strerror(code)
    detail_str = detail.decode() if detail else "unknown error"
    exc_cls = _CODE_TO_EXCEPTION.get(code, GenericError)
    raise exc_cls(f"{context}: {detail_str}", code=code)
