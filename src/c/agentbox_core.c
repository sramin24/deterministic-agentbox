#define _GNU_SOURCE
#include "agentbox.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <libgen.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_FRAME_LEN (16u * 1024u * 1024u)

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ======================= wire protocol =================================
 * 4-byte big-endian length prefix + payload. Requests are one text blob
 * with a fixed number of leading newline-separated fields followed by a
 * final "rest of the buffer" field for anything that might itself contain
 * newlines (a shell command). Responses are "OK\n<data>" or
 * "ERR\n<code>\n<message>".
 * ======================================================================= */

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        sent += (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len) {
    char *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1; /* peer closed */
        got += (size_t)n;
    }
    return 0;
}

static int send_frame(int fd, const char *payload, size_t len) {
    uint32_t belen = htonl((uint32_t)len);
    if (send_all(fd, &belen, 4) != 0) return -1;
    if (len > 0 && send_all(fd, payload, len) != 0) return -1;
    return 0;
}

static int send_frame_str(int fd, const char *s) { return send_frame(fd, s, strlen(s)); }

/* Caller owns the returned buffer (free() it). NULL on any I/O error. */
static char *recv_frame(int fd, size_t *out_len) {
    uint32_t belen;
    if (recv_all(fd, &belen, 4) != 0) return NULL;
    uint32_t len = ntohl(belen);
    if (len > MAX_FRAME_LEN) return NULL;
    char *buf = malloc((size_t)len + 1);
    if (!buf) return NULL;
    if (len > 0 && recv_all(fd, buf, len) != 0) { free(buf); return NULL; }
    buf[len] = 0;
    if (out_len) *out_len = len;
    return buf;
}

/* Peels one newline-terminated field off *cursor, advancing it past the
 * delimiter. The final field of a request (e.g. a shell command) is read
 * directly from *cursor instead, so it can contain further newlines. */
static char *next_field(char **cursor) {
    char *start = *cursor;
    char *nl = strchr(start, '\n');
    if (nl) { *nl = 0; *cursor = nl + 1; }
    else { *cursor = start + strlen(start); }
    return start;
}

static void send_ok(int fd, const char *data) {
    char *buf = malloc(strlen(data) + 4);
    sprintf(buf, "OK\n%s", data);
    send_frame_str(fd, buf);
    free(buf);
}

static void send_err(int fd, agentbox_errcode_t code, const char *msg) {
    char buf[512];
    snprintf(buf, sizeof(buf), "ERR\n%d\n%s", (int)code, msg);
    send_frame_str(fd, buf);
}

/* agentbox_proc_run()'s streaming callback, wired up only while a RUN is in
 * flight: forwards each chunk of output to the client as its own frame,
 * "CHUNK\n<O|E>\n<raw bytes>", the instant it's read from the command's
 * pipe -- not after the command finishes. data is not NUL-terminated and
 * may contain arbitrary bytes (including newlines), which is fine here
 * since it's the last field of the frame and nothing parses past it. */
static void send_chunk_to_client(int is_stderr, const char *data, size_t len, void *userdata) {
    int fd = *(int *)userdata;
    char *buf = malloc(len + 8);
    memcpy(buf, "CHUNK\n", 6);
    buf[6] = is_stderr ? 'E' : 'O';
    buf[7] = '\n';
    memcpy(buf + 8, data, len);
    send_frame(fd, buf, len + 8);
    free(buf);
}

/* ======================= supervisor-side state =========================
 * Everything below this point runs ONLY inside the exec'd agentbox-exec
 * process, after agentbox_ns_enter() has succeeded -- never in the
 * Python-facing client process.
 * ======================================================================= */

typedef struct {
    char workspace_dir[AGENTBOX_PATH_LEN];
    char runtime_dir[AGENTBOX_PATH_LEN];
    char merged_dir[AGENTBOX_PATH_LEN];
    char live_upper[AGENTBOX_PATH_LEN];
    char live_work[AGENTBOX_PATH_LEN];
    int generation;
    int next_checkpoint_num;
    int run_counter;
    agentbox_checkpoint_t checkpoints[AGENTBOX_MAX_CHECKPOINTS]; /* oldest first */
    int checkpoint_count;
} supervisor_state_t;

static void new_generation_dirs(supervisor_state_t *st) {
    st->generation++;
    snprintf(st->live_upper, sizeof(st->live_upper), "%s/upper_%d", st->runtime_dir, st->generation);
    snprintf(st->live_work, sizeof(st->live_work), "%s/work_%d", st->runtime_dir, st->generation);
    mkdir(st->live_upper, 0700);
    mkdir(st->live_work, 0700);
}

/* Newest-first, as OverlayFS's lowerdir= option requires (highest priority
 * first), ending in workspace_dir itself as the base. */
static void build_lower_stack(const supervisor_state_t *st, char *out, size_t out_len) {
    size_t off = 0;
    for (int i = st->checkpoint_count - 1; i >= 0 && off < out_len; i--) {
        int n = snprintf(out + off, out_len - off, "%s:", st->checkpoints[i].layer_dir);
        if (n > 0) off += (size_t)n;
    }
    snprintf(out + off, out_len - off, "%s", st->workspace_dir);
}

static agentbox_errcode_t remount_current(supervisor_state_t *st) {
    char stack[AGENTBOX_PATH_LEN * AGENTBOX_MAX_CHECKPOINTS];
    build_lower_stack(st, stack, sizeof(stack));
    return agentbox_cow_mount(stack, st->live_upper, st->live_work, st->merged_dir);
}

static agentbox_errcode_t handle_checkpoint(supervisor_state_t *st, const char *tag, char *out_id, size_t out_id_len) {
    if (st->checkpoint_count >= AGENTBOX_MAX_CHECKPOINTS) {
        return AGENTBOX_ERR_CHECKPOINT_LIMIT_REACHED;
    }
    agentbox_errcode_t rc = agentbox_cow_unmount(st->merged_dir);
    if (rc != AGENTBOX_OK) return rc;

    agentbox_checkpoint_t *ckpt = &st->checkpoints[st->checkpoint_count];
    snprintf(ckpt->id, sizeof(ckpt->id), "ckpt-%d", st->next_checkpoint_num++);
    snprintf(ckpt->tag, sizeof(ckpt->tag), "%s", tag);
    snprintf(ckpt->layer_dir, sizeof(ckpt->layer_dir), "%s", st->live_upper); /* sealed in place, not moved */
    ckpt->created_at_ms = now_ms();
    st->checkpoint_count++;

    /* The paired workdir is pure kernel scratch space, meaningless once
     * unmounted -- nothing ever reads it again, whether this checkpoint is
     * later kept, rolled back past, or committed. Only the sealed upperdir
     * (ckpt->layer_dir) is ever needed again. Purging it here, once, is
     * what keeps checkpoint/rollback/commit's own cleanup loops complete
     * without each of them separately having to know about workdirs. */
    agentbox_cow_purge(st->live_work);

    new_generation_dirs(st);
    rc = remount_current(st);
    if (rc != AGENTBOX_OK) return rc;

    snprintf(out_id, out_id_len, "%s", ckpt->id);
    return AGENTBOX_OK;
}

static agentbox_errcode_t handle_rollback(supervisor_state_t *st, const char *target_id) {
    int target_index; /* -1 means "no checkpoint, reset to bare workspace_dir" */
    if (target_id == NULL || target_id[0] == '\0') {
        target_index = st->checkpoint_count - 1;
    } else {
        target_index = -2; /* not found sentinel */
        for (int i = 0; i < st->checkpoint_count; i++) {
            if (strcmp(st->checkpoints[i].id, target_id) == 0) { target_index = i; break; }
        }
        if (target_index == -2) return AGENTBOX_ERR_CHECKPOINT_NOT_FOUND;
    }

    agentbox_errcode_t rc = agentbox_cow_unmount(st->merged_dir);
    if (rc != AGENTBOX_OK) return rc;

    agentbox_cow_purge(st->live_upper);
    agentbox_cow_purge(st->live_work);

    for (int i = st->checkpoint_count - 1; i > target_index; i--) {
        agentbox_cow_purge(st->checkpoints[i].layer_dir);
    }
    st->checkpoint_count = target_index + 1;

    new_generation_dirs(st);
    return remount_current(st);
}

static agentbox_errcode_t handle_commit(supervisor_state_t *st) {
    agentbox_errcode_t rc = agentbox_cow_unmount(st->merged_dir);
    if (rc != AGENTBOX_OK) return rc;

    const char *layers[AGENTBOX_MAX_CHECKPOINTS + 1];
    int n = 0;
    for (int i = 0; i < st->checkpoint_count; i++) layers[n++] = st->checkpoints[i].layer_dir;
    layers[n++] = st->live_upper;

    rc = agentbox_cow_commit_merge(layers, (size_t)n, st->workspace_dir);
    if (rc != AGENTBOX_OK) {
        remount_current(st); /* best-effort: leave the mount usable even if commit failed */
        return rc;
    }

    for (int i = 0; i < st->checkpoint_count; i++) agentbox_cow_purge(st->checkpoints[i].layer_dir);
    agentbox_cow_purge(st->live_upper);
    agentbox_cow_purge(st->live_work);
    st->checkpoint_count = 0;

    new_generation_dirs(st);
    return remount_current(st);
}

static agentbox_errcode_t handle_diff(supervisor_state_t *st, char ***out_paths, size_t *out_count) {
    const char *layers[AGENTBOX_MAX_CHECKPOINTS + 1];
    int n = 0;
    for (int i = 0; i < st->checkpoint_count; i++) layers[n++] = st->checkpoints[i].layer_dir;
    layers[n++] = st->live_upper;
    return agentbox_cow_diff(layers, (size_t)n, out_paths, out_count);
}

/* Unlike the targeted per-layer purges in checkpoint/rollback/commit
 * (which must leave still-referenced layers alone), shutdown discards
 * everything -- so rather than track every scratch path individually
 * (upper dirs, the merged mount point, captured run_*.txt output files),
 * it's simpler and more robust to just sweep every entry runtime_dir
 * currently contains. Leaves runtime_dir itself in place, empty, for the
 * client (agentbox_destroy) to rmdir from its own, unprivileged view --
 * see Part 0.3's ordering requirement. */
static void purge_runtime_dir_contents(const char *runtime_dir) {
    DIR *d = opendir(runtime_dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char path[AGENTBOX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", runtime_dir, ent->d_name);
        agentbox_cow_purge(path);
    }
    closedir(d);
}

static void handle_shutdown(supervisor_state_t *st) {
    agentbox_cow_unmount(st->merged_dir);
    purge_runtime_dir_contents(st->runtime_dir);
}

int agentbox_supervisor_main(int ctrl_fd, const char *workspace_dir, const char *runtime_dir) {
    agentbox_errcode_t ns_rc = agentbox_ns_enter();
    if (ns_rc != AGENTBOX_OK) {
        send_err(ctrl_fd, ns_rc, "namespace entry failed; run: sudo agentbox setup");
        return 1;
    }

    /* Anchor process: see Part 0.4. Must be the first fork after
     * unshare(CLONE_NEWPID), before anything else (including RUN
     * commands) can fork, or the first real command to exit would take
     * the whole pid namespace down with it. */
    pid_t anchor_pid = fork();
    if (anchor_pid < 0) {
        send_err(ctrl_fd, AGENTBOX_ERR_GENERIC, "anchor fork failed");
        return 1;
    }
    if (anchor_pid == 0) {
        for (;;) pause();
    }

    supervisor_state_t st;
    memset(&st, 0, sizeof(st));
    snprintf(st.workspace_dir, sizeof(st.workspace_dir), "%s", workspace_dir);
    snprintf(st.runtime_dir, sizeof(st.runtime_dir), "%s", runtime_dir);
    snprintf(st.merged_dir, sizeof(st.merged_dir), "%s/merged", runtime_dir);
    mkdir(st.merged_dir, 0700);
    new_generation_dirs(&st);

    agentbox_errcode_t mount_rc = remount_current(&st);
    if (mount_rc != AGENTBOX_OK) {
        send_err(ctrl_fd, mount_rc, "initial overlay mount failed");
        kill(anchor_pid, SIGKILL);
        waitpid(anchor_pid, NULL, 0);
        return 1;
    }

    send_ok(ctrl_fd, "ready");

    int shutting_down = 0;
    while (!shutting_down) {
        size_t req_len = 0;
        char *req = recv_frame(ctrl_fd, &req_len);
        if (!req) break; /* client gone */

        char *cursor = req;
        char *verb = next_field(&cursor);

        if (strcmp(verb, "CHECKPOINT") == 0) {
            char *tag = next_field(&cursor);
            char id[AGENTBOX_ID_LEN];
            agentbox_errcode_t rc = handle_checkpoint(&st, tag, id, sizeof(id));
            if (rc == AGENTBOX_OK) send_ok(ctrl_fd, id);
            else send_err(ctrl_fd, rc, "checkpoint failed");

        } else if (strcmp(verb, "ROLLBACK") == 0) {
            char *id = next_field(&cursor);
            agentbox_errcode_t rc = handle_rollback(&st, id);
            if (rc == AGENTBOX_OK) send_ok(ctrl_fd, "");
            else send_err(ctrl_fd, rc, "rollback failed");

        } else if (strcmp(verb, "COMMIT") == 0) {
            agentbox_errcode_t rc = handle_commit(&st);
            if (rc == AGENTBOX_OK) send_ok(ctrl_fd, "");
            else send_err(ctrl_fd, rc, "commit failed");

        } else if (strcmp(verb, "DIFF") == 0) {
            char **paths = NULL;
            size_t count = 0;
            agentbox_errcode_t rc = handle_diff(&st, &paths, &count);
            if (rc == AGENTBOX_OK) {
                size_t bufcap = 64;
                for (size_t i = 0; i < count; i++) bufcap += strlen(paths[i]) + 1;
                char *buf = malloc(bufcap);
                int off = snprintf(buf, bufcap, "%zu\n", count);
                for (size_t i = 0; i < count; i++) {
                    off += snprintf(buf + off, bufcap - (size_t)off, "%s\n", paths[i]);
                }
                send_ok(ctrl_fd, buf);
                free(buf);
                agentbox_free_string_list(paths, count);
            } else {
                send_err(ctrl_fd, rc, "diff failed");
            }

        } else if (strcmp(verb, "RUN") == 0) {
            char *timeout_str = next_field(&cursor);
            char *cmd = cursor; /* rest of the buffer */
            int timeout_sec = atoi(timeout_str);
            char stdout_path[AGENTBOX_PATH_LEN], stderr_path[AGENTBOX_PATH_LEN];
            snprintf(stdout_path, sizeof(stdout_path), "%s/run_%d_stdout.txt", st.runtime_dir, st.run_counter);
            snprintf(stderr_path, sizeof(stderr_path), "%s/run_%d_stderr.txt", st.runtime_dir, st.run_counter);
            st.run_counter++;
            agentbox_exec_result_t result;
            agentbox_errcode_t rc = agentbox_proc_run(st.merged_dir, cmd, timeout_sec, stdout_path, stderr_path,
                                                       send_chunk_to_client, &ctrl_fd, &result);
            if (rc == AGENTBOX_OK) {
                char buf[AGENTBOX_PATH_LEN * 2 + 128];
                snprintf(buf, sizeof(buf), "%d\n%d\n%lld\n%s\n%s",
                         result.exit_code, result.timed_out, result.duration_ms,
                         result.stdout_path, result.stderr_path);
                send_ok(ctrl_fd, buf);
            } else {
                send_err(ctrl_fd, rc, "run failed");
            }

        } else if (strcmp(verb, "SHUTDOWN") == 0) {
            handle_shutdown(&st);
            send_ok(ctrl_fd, "");
            shutting_down = 1;

        } else {
            send_err(ctrl_fd, AGENTBOX_ERR_INVALID_ARGUMENT, "unknown verb");
        }

        free(req);
    }

    kill(anchor_pid, SIGKILL);
    waitpid(anchor_pid, NULL, 0);
    return 0;
}

/* ======================= client-side public API =========================
 * Everything below runs in the CALLER's process (the Python interpreter,
 * via ctypes, in production) and only ever talks to the supervisor over
 * ctrl_fd -- it never touches namespaces, mounts, or the filesystem layer
 * directly.
 * ======================================================================= */

static agentbox_errcode_t locate_exec_binary(char *out, size_t out_len) {
    Dl_info info;
    if (dladdr((void *)agentbox_create, &info) == 0 || !info.dli_fname) {
        return AGENTBOX_ERR_GENERIC;
    }
    char buf[AGENTBOX_PATH_LEN];
    snprintf(buf, sizeof(buf), "%s", info.dli_fname);
    char *dir = dirname(buf);
    snprintf(out, out_len, "%s/%s", dir, AGENTBOX_EXEC_BINARY_NAME);
    return AGENTBOX_OK;
}

agentbox_probe_result_t agentbox_probe_capabilities(char *detail_buf, size_t detail_buf_len) {
    char exec_path[AGENTBOX_PATH_LEN];
    if (locate_exec_binary(exec_path, sizeof(exec_path)) != AGENTBOX_OK) {
        if (detail_buf) snprintf(detail_buf, detail_buf_len, "could not locate agentbox-exec binary");
        return AGENTBOX_PROBE_FORK_FAILED;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) return AGENTBOX_PROBE_FORK_FAILED;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return AGENTBOX_PROBE_FORK_FAILED; }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execl(exec_path, exec_path, "--probe", (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    char buf[256] = {0};
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    if (n < 0) n = 0;
    buf[n] = 0;
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (detail_buf) snprintf(detail_buf, detail_buf_len, "%s", buf);
    if (!WIFEXITED(status)) return AGENTBOX_PROBE_FORK_FAILED;
    return (agentbox_probe_result_t)WEXITSTATUS(status);
}

agentbox_errcode_t agentbox_install_apparmor_profile(const char *binary_path) {
    return agentbox_ns_install_apparmor_profile(binary_path);
}

agentbox_errcode_t agentbox_create(agentbox_t *box, const char *workspace_dir, const char *runtime_dir_in) {
    memset(box, 0, sizeof(*box));

    struct stat ws_st;
    if (stat(workspace_dir, &ws_st) != 0 || !S_ISDIR(ws_st.st_mode)) {
        return AGENTBOX_ERR_INVALID_ARGUMENT;
    }

    char runtime_dir[AGENTBOX_PATH_LEN];
    if (runtime_dir_in && runtime_dir_in[0]) {
        snprintf(runtime_dir, sizeof(runtime_dir), "%s", runtime_dir_in);
    } else {
        /* Sibling of workspace_dir, per Part 0.5 -- must share a
         * filesystem with it, and /tmp very often does not. */
        char ws_copy[AGENTBOX_PATH_LEN];
        snprintf(ws_copy, sizeof(ws_copy), "%s", workspace_dir);
        char base_copy[AGENTBOX_PATH_LEN];
        snprintf(base_copy, sizeof(base_copy), "%s", workspace_dir);
        char *parent = dirname(ws_copy);
        char *base = basename(base_copy);
        snprintf(runtime_dir, sizeof(runtime_dir), "%s/.agentbox_rt_%s_%d", parent, base, (int)getpid());
    }

    if (mkdir(runtime_dir, 0700) != 0 && errno != EEXIST) {
        return AGENTBOX_ERR_IO;
    }
    struct stat rt_st;
    if (stat(runtime_dir, &rt_st) != 0) {
        return AGENTBOX_ERR_IO;
    }
    if (rt_st.st_dev != ws_st.st_dev) {
        rmdir(runtime_dir);
        return AGENTBOX_ERR_FILESYSTEM_MISMATCH;
    }

    char exec_path[AGENTBOX_PATH_LEN];
    if (locate_exec_binary(exec_path, sizeof(exec_path)) != AGENTBOX_OK) {
        rmdir(runtime_dir);
        return AGENTBOX_ERR_GENERIC;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        rmdir(runtime_dir);
        return AGENTBOX_ERR_GENERIC;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(sv[0]); close(sv[1]);
        rmdir(runtime_dir);
        return AGENTBOX_ERR_GENERIC;
    }
    if (pid == 0) {
        close(sv[0]);
        char fdstr[16];
        snprintf(fdstr, sizeof(fdstr), "%d", sv[1]);
        execl(exec_path, exec_path, workspace_dir, runtime_dir, fdstr, (char *)NULL);
        _exit(127);
    }
    close(sv[1]);

    size_t len = 0;
    char *resp = recv_frame(sv[0], &len);
    if (!resp) {
        close(sv[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        rmdir(runtime_dir);
        return AGENTBOX_ERR_SUPERVISOR_DOWN;
    }
    agentbox_errcode_t result = AGENTBOX_OK;
    if (strncmp(resp, "OK", 2) != 0) {
        char *cursor = resp;
        next_field(&cursor); /* "ERR" */
        char *code_str = next_field(&cursor);
        result = (agentbox_errcode_t)atoi(code_str);
        close(sv[0]);
        waitpid(pid, NULL, 0);
        rmdir(runtime_dir);
    }
    free(resp);
    if (result != AGENTBOX_OK) return result;

    snprintf(box->workspace_dir, sizeof(box->workspace_dir), "%s", workspace_dir);
    snprintf(box->runtime_dir, sizeof(box->runtime_dir), "%s", runtime_dir);
    snprintf(box->merged_dir, sizeof(box->merged_dir), "%s/merged", runtime_dir);
    box->supervisor_pid = pid;
    box->ctrl_fd = sv[0];
    box->active = 1;
    box->checkpoint_count = 0;
    return AGENTBOX_OK;
}

agentbox_errcode_t agentbox_destroy(agentbox_t *box) {
    if (!box->active) return AGENTBOX_OK;
    send_frame_str(box->ctrl_fd, "SHUTDOWN");
    size_t len = 0;
    char *resp = recv_frame(box->ctrl_fd, &len);
    free(resp);
    close(box->ctrl_fd);
    waitpid(box->supervisor_pid, NULL, 0);
    box->active = 0;
    if (rmdir(box->runtime_dir) != 0 && errno != ENOENT) {
        return AGENTBOX_ERR_IO;
    }
    return AGENTBOX_OK;
}

agentbox_errcode_t agentbox_checkpoint(agentbox_t *box, const char *tag, char *out_id, size_t out_id_len) {
    char req[AGENTBOX_TAG_LEN + 32];
    snprintf(req, sizeof(req), "CHECKPOINT\n%s", tag ? tag : "");
    if (send_frame_str(box->ctrl_fd, req) != 0) return AGENTBOX_ERR_SUPERVISOR_DOWN;

    size_t len = 0;
    char *resp = recv_frame(box->ctrl_fd, &len);
    if (!resp) return AGENTBOX_ERR_SUPERVISOR_DOWN;

    char *cursor = resp;
    char *status = next_field(&cursor);
    agentbox_errcode_t rc;
    if (strcmp(status, "OK") == 0) {
        char *id = next_field(&cursor);
        if (out_id) snprintf(out_id, out_id_len, "%s", id);
        if (box->checkpoint_count < AGENTBOX_MAX_CHECKPOINTS) {
            agentbox_checkpoint_t *c = &box->checkpoints[box->checkpoint_count++];
            snprintf(c->id, sizeof(c->id), "%s", id);
            snprintf(c->tag, sizeof(c->tag), "%s", tag ? tag : "");
            c->layer_dir[0] = '\0';
            c->created_at_ms = now_ms();
        }
        rc = AGENTBOX_OK;
    } else {
        char *code_str = next_field(&cursor);
        rc = (agentbox_errcode_t)atoi(code_str);
    }
    free(resp);
    return rc;
}

agentbox_errcode_t agentbox_rollback(agentbox_t *box, const char *checkpoint_id) {
    char req[AGENTBOX_ID_LEN + 32];
    snprintf(req, sizeof(req), "ROLLBACK\n%s", checkpoint_id ? checkpoint_id : "");
    if (send_frame_str(box->ctrl_fd, req) != 0) return AGENTBOX_ERR_SUPERVISOR_DOWN;

    size_t len = 0;
    char *resp = recv_frame(box->ctrl_fd, &len);
    if (!resp) return AGENTBOX_ERR_SUPERVISOR_DOWN;

    char *cursor = resp;
    char *status = next_field(&cursor);
    agentbox_errcode_t rc;
    if (strcmp(status, "OK") == 0) {
        if (checkpoint_id == NULL || checkpoint_id[0] == '\0') {
            if (box->checkpoint_count > 0) box->checkpoint_count--;
        } else {
            for (int i = 0; i < box->checkpoint_count; i++) {
                if (strcmp(box->checkpoints[i].id, checkpoint_id) == 0) {
                    box->checkpoint_count = i + 1;
                    break;
                }
            }
        }
        rc = AGENTBOX_OK;
    } else {
        char *code_str = next_field(&cursor);
        rc = (agentbox_errcode_t)atoi(code_str);
    }
    free(resp);
    return rc;
}

agentbox_errcode_t agentbox_commit(agentbox_t *box) {
    if (send_frame_str(box->ctrl_fd, "COMMIT") != 0) return AGENTBOX_ERR_SUPERVISOR_DOWN;
    size_t len = 0;
    char *resp = recv_frame(box->ctrl_fd, &len);
    if (!resp) return AGENTBOX_ERR_SUPERVISOR_DOWN;
    char *cursor = resp;
    char *status = next_field(&cursor);
    agentbox_errcode_t rc;
    if (strcmp(status, "OK") == 0) {
        box->checkpoint_count = 0;
        rc = AGENTBOX_OK;
    } else {
        char *code_str = next_field(&cursor);
        rc = (agentbox_errcode_t)atoi(code_str);
    }
    free(resp);
    return rc;
}

agentbox_errcode_t agentbox_diff(agentbox_t *box, char ***out_paths, size_t *out_count) {
    if (send_frame_str(box->ctrl_fd, "DIFF") != 0) return AGENTBOX_ERR_SUPERVISOR_DOWN;
    size_t len = 0;
    char *resp = recv_frame(box->ctrl_fd, &len);
    if (!resp) return AGENTBOX_ERR_SUPERVISOR_DOWN;

    char *cursor = resp;
    char *status = next_field(&cursor);
    if (strcmp(status, "OK") != 0) {
        char *code_str = next_field(&cursor);
        agentbox_errcode_t rc = (agentbox_errcode_t)atoi(code_str);
        free(resp);
        return rc;
    }

    char *count_str = next_field(&cursor);
    size_t count = (size_t)strtoul(count_str, NULL, 10);
    char **paths = count ? calloc(count, sizeof(char *)) : NULL;
    for (size_t i = 0; i < count; i++) {
        paths[i] = strdup(next_field(&cursor));
    }
    free(resp);
    *out_paths = paths;
    *out_count = count;
    return AGENTBOX_OK;
}

agentbox_errcode_t agentbox_exec_streaming(agentbox_t *box, const char *cwd, const char *cmd,
                                            int timeout_sec, agentbox_stream_chunk_cb on_chunk,
                                            void *userdata, agentbox_exec_result_t *result) {
    (void)cwd; /* commands always run at the sandbox root; see Part 5 note */
    size_t req_cap = strlen(cmd) + 64;
    char *req = malloc(req_cap);
    snprintf(req, req_cap, "RUN\n%d\n%s", timeout_sec, cmd);
    int rc = send_frame_str(box->ctrl_fd, req);
    free(req);
    if (rc != 0) return AGENTBOX_ERR_SUPERVISOR_DOWN;

    for (;;) {
        size_t len = 0;
        char *resp = recv_frame(box->ctrl_fd, &len);
        if (!resp) return AGENTBOX_ERR_SUPERVISOR_DOWN;

        char *cursor = resp;
        char *status = next_field(&cursor);

        if (strcmp(status, "CHUNK") == 0) {
            /* "CHUNK\n<O|E>\n<raw bytes, length known from the frame, not
             * NUL-scanned -- may contain anything>" */
            char *which = next_field(&cursor);
            int is_stderr = (which[0] == 'E');
            size_t data_len = len - (size_t)(cursor - resp);
            if (on_chunk) on_chunk(is_stderr, cursor, data_len, userdata);
            free(resp);
            continue;
        }

        agentbox_errcode_t out_rc;
        if (strcmp(status, "OK") == 0) {
            memset(result, 0, sizeof(*result));
            result->exit_code = atoi(next_field(&cursor));
            result->timed_out = atoi(next_field(&cursor));
            result->duration_ms = atoll(next_field(&cursor));
            snprintf(result->stdout_path, sizeof(result->stdout_path), "%s", next_field(&cursor));
            snprintf(result->stderr_path, sizeof(result->stderr_path), "%s", next_field(&cursor));
            out_rc = AGENTBOX_OK;
        } else {
            char *code_str = next_field(&cursor);
            out_rc = (agentbox_errcode_t)atoi(code_str);
        }
        free(resp);
        return out_rc;
    }
}

agentbox_errcode_t agentbox_exec(agentbox_t *box, const char *cwd, const char *cmd,
                                  int timeout_sec, agentbox_exec_result_t *result) {
    return agentbox_exec_streaming(box, cwd, cmd, timeout_sec, NULL, NULL, result);
}

const char *agentbox_strerror(agentbox_errcode_t code) {
    switch (code) {
        case AGENTBOX_OK: return "success";
        case AGENTBOX_ERR_GENERIC: return "generic error";
        case AGENTBOX_ERR_NAMESPACE_UNAVAILABLE: return "namespace unavailable (run: sudo agentbox setup)";
        case AGENTBOX_ERR_MOUNT_FAILED: return "overlay mount failed";
        case AGENTBOX_ERR_CHECKPOINT_NOT_FOUND: return "checkpoint not found";
        case AGENTBOX_ERR_IO: return "I/O error";
        case AGENTBOX_ERR_TIMEOUT: return "timed out";
        case AGENTBOX_ERR_SUPERVISOR_DOWN: return "supervisor process is not responding";
        case AGENTBOX_ERR_INVALID_ARGUMENT: return "invalid argument";
        case AGENTBOX_ERR_FILESYSTEM_MISMATCH: return "workspace_dir and runtime_dir are on different filesystems";
        case AGENTBOX_ERR_PERMISSION_DENIED: return "permission denied";
        case AGENTBOX_ERR_CHECKPOINT_LIMIT_REACHED: return "checkpoint limit reached";
        default: return "unknown error";
    }
}
