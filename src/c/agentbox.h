#ifndef AGENTBOX_H
#define AGENTBOX_H

#include <sys/types.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AGENTBOX_MAX_CHECKPOINTS 256
#define AGENTBOX_ID_LEN 64
#define AGENTBOX_TAG_LEN 128
#define AGENTBOX_PATH_LEN 4096

/*
 * Name of the dedicated wrapper binary the AppArmor profile targets.
 *
 * The profile is keyed on the ELF binary that performs unshare(), not on
 * whatever process forked it. Targeting sys.executable (python3) would grant
 * userns creation to every program invoked through that same interpreter
 * binary, not just this project. So the supervisor logic that calls
 * agentbox_ns_enter() lives in its own single-purpose executable, and
 * agentbox_create() forks + execve()s into it rather than running the
 * namespace/mount setup inside the caller's own process image.
 */
#define AGENTBOX_EXEC_BINARY_NAME "agentbox-exec"

/* ---- Error codes ------------------------------------------------------- */

typedef enum {
    AGENTBOX_OK = 0,
    AGENTBOX_ERR_GENERIC = 1,
    AGENTBOX_ERR_NAMESPACE_UNAVAILABLE = 2,   /* unshare()/uid_map/gid_map path blocked; see agentbox setup */
    AGENTBOX_ERR_MOUNT_FAILED = 3,
    AGENTBOX_ERR_CHECKPOINT_NOT_FOUND = 4,
    AGENTBOX_ERR_IO = 5,
    AGENTBOX_ERR_TIMEOUT = 6,
    AGENTBOX_ERR_SUPERVISOR_DOWN = 7,
    AGENTBOX_ERR_INVALID_ARGUMENT = 8,
    AGENTBOX_ERR_FILESYSTEM_MISMATCH = 9,     /* workspace_dir/runtime_dir not on the same st_dev */
    AGENTBOX_ERR_PERMISSION_DENIED = 10,
    AGENTBOX_ERR_CHECKPOINT_LIMIT_REACHED = 11 /* checkpoint_count == AGENTBOX_MAX_CHECKPOINTS */
} agentbox_errcode_t;

/*
 * Fine-grained exit codes for agentbox_ns_probe(), which forks a throwaway
 * child to exercise the exact sequence agentbox_ns_enter() will perform,
 * against an isolated /tmp fixture, before any real Sandbox work begins.
 *
 * These are deliberately more granular than agentbox_errcode_t: measured
 * against a real Ubuntu 24.04+/26.04 target, unshare(CLONE_NEWUSER) itself
 * SUCCEEDS even when blocked -- the kernel transitions the calling process
 * into a synthetic "unprivileged_userns" AppArmor profile the instant the
 * namespace is created, and THAT profile denies CAP_SYS_ADMIN. The failure
 * actually surfaces later, as EPERM on the /proc/self/uid_map (or
 * setgroups/gid_map) write, not as a failure of unshare() itself. Collapsing
 * that into one "unshare failed" code (as a naive reading of the AppArmor
 * restriction might suggest) would misdiagnose the single most common
 * failure mode this project exists to handle -- so it gets its own code.
 */
typedef enum {
    AGENTBOX_PROBE_OK = 0,
    AGENTBOX_PROBE_UNSHARE_FAILED = 1,        /* unshare() syscall itself returned non-zero */
    AGENTBOX_PROBE_ID_MAP_DENIED = 2,         /* setgroups/uid_map/gid_map write EPERM after unshare succeeded -- the AppArmor case; fix is `agentbox setup` */
    AGENTBOX_PROBE_MOUNT_PERM_DENIED = 3,     /* overlay mount EPERM/EINVAL and fuse-overlayfs fallback also failed */
    AGENTBOX_PROBE_MOUNT_OTHER_FAILED = 4,    /* mount failed with an unrelated errno; do not swallow into a setup suggestion */
    AGENTBOX_PROBE_FORK_FAILED = 5
} agentbox_probe_result_t;

/* ---- Wire protocol verbs (client <-> supervisor, over ctrl_fd) --------- */

typedef enum {
    AGENTBOX_VERB_CHECKPOINT = 1,
    AGENTBOX_VERB_ROLLBACK = 2,
    AGENTBOX_VERB_COMMIT = 3,
    AGENTBOX_VERB_DIFF = 4,
    AGENTBOX_VERB_RUN = 5,
    AGENTBOX_VERB_SHUTDOWN = 6
} agentbox_verb_t;

/* ---- Public structures -------------------------------------------------- */

typedef struct {
    char id[AGENTBOX_ID_LEN];
    char tag[AGENTBOX_TAG_LEN];
    char layer_dir[AGENTBOX_PATH_LEN];  /* sealed upperdir backing this checkpoint */
    long long created_at_ms;
} agentbox_checkpoint_t;

typedef struct {
    int exit_code;
    int timed_out;
    long long duration_ms;
    char stdout_path[AGENTBOX_PATH_LEN];
    char stderr_path[AGENTBOX_PATH_LEN];
} agentbox_exec_result_t;

typedef struct {
    char workspace_dir[AGENTBOX_PATH_LEN];
    char runtime_dir[AGENTBOX_PATH_LEN];
    char merged_dir[AGENTBOX_PATH_LEN];
    pid_t supervisor_pid;
    int  ctrl_fd;
    int  active;
    agentbox_checkpoint_t checkpoints[AGENTBOX_MAX_CHECKPOINTS];
    int  checkpoint_count;
} agentbox_t;

/* ---- Public API (client-side; implemented in agentbox_core.c) ---------- */

agentbox_probe_result_t agentbox_probe_capabilities(char *detail_buf, size_t detail_buf_len);

/* geteuid() == 0 required. binary_path must be the real path to the
 * agentbox-exec wrapper binary (never sys.executable / a script path). */
agentbox_errcode_t agentbox_install_apparmor_profile(const char *binary_path);

agentbox_errcode_t agentbox_create(agentbox_t *box, const char *workspace_dir, const char *runtime_dir);
agentbox_errcode_t agentbox_destroy(agentbox_t *box);

agentbox_errcode_t agentbox_checkpoint(agentbox_t *box, const char *tag, char *out_id, size_t out_id_len);
agentbox_errcode_t agentbox_rollback(agentbox_t *box, const char *checkpoint_id /* NULL = most recent */);
agentbox_errcode_t agentbox_commit(agentbox_t *box);
agentbox_errcode_t agentbox_diff(agentbox_t *box, char ***out_paths, size_t *out_count);
void agentbox_free_string_list(char **paths, size_t count);

agentbox_errcode_t agentbox_exec(agentbox_t *box, const char *cwd, const char *cmd,
                                  int timeout_sec, agentbox_exec_result_t *result);

/*
 * Called synchronously, from within agentbox_exec_streaming(), for every
 * chunk of output the running command produces -- possibly many times
 * before the command finishes. is_stderr distinguishes which stream a
 * chunk came from (0 = stdout, 1 = stderr); data is NOT NUL-terminated and
 * may contain arbitrary bytes, so len must be respected. Never called after
 * agentbox_exec_streaming() returns.
 */
typedef void (*agentbox_stream_chunk_cb)(int is_stderr, const char *data, size_t len, void *userdata);

/* Same contract as agentbox_exec(), plus live delivery of output as the
 * command produces it (on_chunk may be NULL to skip streaming and just
 * wait for the final result, same as agentbox_exec()). */
agentbox_errcode_t agentbox_exec_streaming(agentbox_t *box, const char *cwd, const char *cmd,
                                            int timeout_sec, agentbox_stream_chunk_cb on_chunk,
                                            void *userdata, agentbox_exec_result_t *result);

const char *agentbox_strerror(agentbox_errcode_t code);

/* ---- Internal API (shared across engine .c files, not part of the ABI
 * boundary crossed by ctypes -- declared here so agentbox_core.c,
 * agentbox_ns.c, agentbox_cow.c, agentbox_proc.c and agentbox_exec_main.c
 * can all see one shared, consistent surface) ---------------------------- */

/* agentbox_ns.c */
agentbox_errcode_t agentbox_ns_enter(void);
agentbox_probe_result_t agentbox_ns_probe(char *detail_buf, size_t detail_buf_len);
agentbox_errcode_t agentbox_ns_install_apparmor_profile(const char *binary_path);

/* agentbox_cow.c */
agentbox_errcode_t agentbox_cow_mount(const char *lowerdir_stack, const char *upperdir,
                                       const char *workdir, const char *merged);
agentbox_errcode_t agentbox_cow_unmount(const char *merged);
agentbox_errcode_t agentbox_cow_purge(const char *dir);
agentbox_errcode_t agentbox_cow_commit_merge(const char *const *layer_dirs, size_t count,
                                              const char *target_dir);
agentbox_errcode_t agentbox_cow_diff(const char *const *layer_dirs, size_t count,
                                      char ***out_paths, size_t *out_count);

/* agentbox_proc.c: runs the command with its stdout/stderr connected to
 * pipes (not files) so output can be forwarded as it's produced; each
 * chunk read is both appended to stdout_path/stderr_path (so the full
 * captured output is always available afterward, streamed or not) and,
 * if on_chunk is non-NULL, handed to it immediately. */
agentbox_errcode_t agentbox_proc_run(const char *cwd, const char *cmd, int timeout_sec,
                                      const char *stdout_path, const char *stderr_path,
                                      agentbox_stream_chunk_cb on_chunk, void *userdata,
                                      agentbox_exec_result_t *result);

/* agentbox_core.c: entry point called by agentbox_exec_main.c's main() after
 * it parses argv (ctrl_fd, workspace_dir, runtime_dir) -- this is what
 * actually runs inside the agentbox-exec process image, i.e. the thing the
 * AppArmor profile is scoped to. Never returns except on SHUTDOWN or fatal
 * error; the calling main() should exit with its return value. */
int agentbox_supervisor_main(int ctrl_fd, const char *workspace_dir, const char *runtime_dir);

#ifdef __cplusplus
}
#endif

#endif /* AGENTBOX_H */
