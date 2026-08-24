"""ctypes bindings for libagentbox.so.

Structure definitions mirror src/c/agentbox.h field-for-field (same order,
same types) so ctypes' default platform struct layout matches the C
compiler's exactly -- there is no manual padding here because there is no
padding to add: this only works because the two sides agree on order and
type, not because of anything clever.
"""
import ctypes
import ctypes.util
import os

AGENTBOX_ID_LEN = 64
AGENTBOX_TAG_LEN = 128
AGENTBOX_PATH_LEN = 4096
AGENTBOX_MAX_CHECKPOINTS = 256


class AgentboxCheckpoint(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_char * AGENTBOX_ID_LEN),
        ("tag", ctypes.c_char * AGENTBOX_TAG_LEN),
        ("layer_dir", ctypes.c_char * AGENTBOX_PATH_LEN),
        ("created_at_ms", ctypes.c_longlong),
    ]


class AgentboxExecResult(ctypes.Structure):
    _fields_ = [
        ("exit_code", ctypes.c_int),
        ("timed_out", ctypes.c_int),
        ("duration_ms", ctypes.c_longlong),
        ("stdout_path", ctypes.c_char * AGENTBOX_PATH_LEN),
        ("stderr_path", ctypes.c_char * AGENTBOX_PATH_LEN),
    ]


class AgentboxBox(ctypes.Structure):
    _fields_ = [
        ("workspace_dir", ctypes.c_char * AGENTBOX_PATH_LEN),
        ("runtime_dir", ctypes.c_char * AGENTBOX_PATH_LEN),
        ("merged_dir", ctypes.c_char * AGENTBOX_PATH_LEN),
        ("supervisor_pid", ctypes.c_int),  # pid_t is a 32-bit int on Linux
        ("ctrl_fd", ctypes.c_int),
        ("active", ctypes.c_int),
        ("checkpoints", AgentboxCheckpoint * AGENTBOX_MAX_CHECKPOINTS),
        ("checkpoint_count", ctypes.c_int),
    ]


# void (*)(int is_stderr, const void *data, size_t len, void *userdata)
# data is declared void* rather than char* deliberately: it is NOT
# NUL-terminated and may contain arbitrary bytes, so the Python side must
# read it with ctypes.string_at(ptr, len), never treat it as a C string.
STREAM_CALLBACK_TYPE = ctypes.CFUNCTYPE(
    None, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p
)


def _find_library() -> str:
    pkg_dir = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.normpath(os.path.join(pkg_dir, "libagentbox.so")),  # installed package layout
        os.path.normpath(os.path.join(pkg_dir, "..", "..", "out", "libagentbox.so")),  # repo checkout, `make build`
    ]
    for path in candidates:
        if os.path.isfile(path):
            return path
    tried = "\n".join(f"  - {p}" for p in candidates)
    raise OSError(
        "Could not find libagentbox.so. Looked in:\n" + tried +
        "\n\nBuild it with `make build` from the repository root."
    )


def _find_exec_binary() -> str:
    pkg_dir = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.normpath(os.path.join(pkg_dir, "agentbox-exec")),
        os.path.normpath(os.path.join(pkg_dir, "..", "..", "out", "agentbox-exec")),
    ]
    for path in candidates:
        if os.path.isfile(path):
            return path
    tried = "\n".join(f"  - {p}" for p in candidates)
    raise OSError("Could not find the agentbox-exec binary. Looked in:\n" + tried)


_LIB_PATH = _find_library()
lib = ctypes.CDLL(_LIB_PATH)

lib.agentbox_probe_capabilities.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
lib.agentbox_probe_capabilities.restype = ctypes.c_int

lib.agentbox_install_apparmor_profile.argtypes = [ctypes.c_char_p]
lib.agentbox_install_apparmor_profile.restype = ctypes.c_int

lib.agentbox_create.argtypes = [ctypes.POINTER(AgentboxBox), ctypes.c_char_p, ctypes.c_char_p]
lib.agentbox_create.restype = ctypes.c_int

lib.agentbox_destroy.argtypes = [ctypes.POINTER(AgentboxBox)]
lib.agentbox_destroy.restype = ctypes.c_int

lib.agentbox_checkpoint.argtypes = [
    ctypes.POINTER(AgentboxBox), ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t
]
lib.agentbox_checkpoint.restype = ctypes.c_int

lib.agentbox_rollback.argtypes = [ctypes.POINTER(AgentboxBox), ctypes.c_char_p]
lib.agentbox_rollback.restype = ctypes.c_int

lib.agentbox_commit.argtypes = [ctypes.POINTER(AgentboxBox)]
lib.agentbox_commit.restype = ctypes.c_int

lib.agentbox_diff.argtypes = [
    ctypes.POINTER(AgentboxBox),
    ctypes.POINTER(ctypes.POINTER(ctypes.c_char_p)),
    ctypes.POINTER(ctypes.c_size_t),
]
lib.agentbox_diff.restype = ctypes.c_int

lib.agentbox_free_string_list.argtypes = [ctypes.POINTER(ctypes.c_char_p), ctypes.c_size_t]
lib.agentbox_free_string_list.restype = None

lib.agentbox_exec.argtypes = [
    ctypes.POINTER(AgentboxBox), ctypes.c_char_p, ctypes.c_char_p,
    ctypes.c_int, ctypes.POINTER(AgentboxExecResult),
]
lib.agentbox_exec.restype = ctypes.c_int

lib.agentbox_exec_streaming.argtypes = [
    ctypes.POINTER(AgentboxBox), ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int,
    STREAM_CALLBACK_TYPE, ctypes.c_void_p, ctypes.POINTER(AgentboxExecResult),
]
lib.agentbox_exec_streaming.restype = ctypes.c_int

lib.agentbox_strerror.argtypes = [ctypes.c_int]
lib.agentbox_strerror.restype = ctypes.c_char_p


def make_stream_callback(on_chunk):
    """Wraps a Python callable on_chunk(is_stderr: bool, data: bytes) as a
    STREAM_CALLBACK_TYPE. The caller must keep the returned object alive
    for the duration of the C call it's passed to -- ctypes does not do
    this automatically, and a garbage-collected callback is a crash the
    C side has no way to detect."""
    def _trampoline(is_stderr, data_ptr, length, _userdata):
        data = ctypes.string_at(data_ptr, length) if length else b""
        on_chunk(bool(is_stderr), data)
    return STREAM_CALLBACK_TYPE(_trampoline)
