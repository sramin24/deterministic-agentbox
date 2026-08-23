"""deterministic-agentbox: a transactional, local-first execution sandbox
for AI coding agents. Zero-copy checkpoint and instant rollback for
shell commands and file mutations, without Docker or root.
"""
from .errors import (
    AgentboxError,
    CheckpointLimitError,
    CheckpointNotFoundError,
    FilesystemMismatchError,
    GenericError,
    InvalidArgumentError,
    MountFailedError,
    NamespaceUnavailableError,
    PermissionError,
    SupervisorDownError,
    TimeoutError,
)
from .sandbox import ExecResult, Sandbox

__all__ = [
    "Sandbox",
    "ExecResult",
    "AgentboxError",
    "NamespaceUnavailableError",
    "MountFailedError",
    "CheckpointNotFoundError",
    "TimeoutError",
    "SupervisorDownError",
    "InvalidArgumentError",
    "FilesystemMismatchError",
    "PermissionError",
    "CheckpointLimitError",
    "GenericError",
]
