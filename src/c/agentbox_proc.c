#define _GNU_SOURCE
#include "agentbox.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void write_all_best_effort(int fd, const char *buf, ssize_t len) {
    if (fd < 0) return;
    ssize_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, (size_t)(len - off));
        if (w < 0) { if (errno == EINTR) continue; return; }
        off += w;
    }
}

/* Non-blocking: reads whatever is immediately available from fd (if
 * anything), writes it to capture_fd and forwards it to on_chunk. Returns
 * 1 if the pipe hit EOF (write end fully closed), 0 otherwise. */
static int drain_once(int fd, int capture_fd, int is_stderr,
                       agentbox_stream_chunk_cb on_chunk, void *userdata) {
    char buf[8192];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        write_all_best_effort(capture_fd, buf, n);
        if (on_chunk) on_chunk(is_stderr, buf, (size_t)n, userdata);
        return 0;
    }
    if (n == 0) return 1; /* EOF */
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : 1; /* treat other errors as done */
}

agentbox_errcode_t agentbox_proc_run(const char *cwd, const char *cmd, int timeout_sec,
                                      const char *stdout_path, const char *stderr_path,
                                      agentbox_stream_chunk_cb on_chunk, void *userdata,
                                      agentbox_exec_result_t *result) {
    memset(result, 0, sizeof(*result));
    snprintf(result->stdout_path, sizeof(result->stdout_path), "%s", stdout_path);
    snprintf(result->stderr_path, sizeof(result->stderr_path), "%s", stderr_path);

    int out_fd = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) return AGENTBOX_ERR_IO;
    int err_fd = open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (err_fd < 0) { close(out_fd); return AGENTBOX_ERR_IO; }

    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0) { close(out_fd); close(err_fd); return AGENTBOX_ERR_GENERIC; }
    if (pipe(err_pipe) != 0) {
        close(out_fd); close(err_fd);
        close(out_pipe[0]); close(out_pipe[1]);
        return AGENTBOX_ERR_GENERIC;
    }

    long long start = now_ms();

    pid_t pid = fork();
    if (pid < 0) {
        close(out_fd); close(err_fd);
        close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
        return AGENTBOX_ERR_GENERIC;
    }

    if (pid == 0) {
        /* New session + new process group, rooted at this child, detached
         * from any controlling terminal. This is what makes kill(-pid, ...)
         * below able to reach every descendant the command spawns
         * (including a backgrounded "sleep 30 &"), not just the immediate
         * /bin/sh -- as long as none of them calls setsid() itself, they
         * inherit this same process group. */
        if (setsid() < 0) _exit(126);
        if (chdir(cwd) != 0) _exit(126);

        close(out_pipe[0]); /* child only ever writes */
        close(err_pipe[0]);
        close(out_fd);
        close(err_fd);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);
        close(err_pipe[1]);

        execlp("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
        _exit(127); /* only reached if execlp itself failed */
    }

    /* Parent: only ever reads. */
    close(out_pipe[1]);
    close(err_pipe[1]);
    /* Non-blocking, so poll()'s wakeups drive reading rather than a read()
     * call itself blocking and stalling the timeout/child-exit checks. */
    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

    int out_open = 1, err_open = 1;
    int child_done = 0;
    int status = 0;
    int timed_out = 0;
    long long deadline = start + (long long)timeout_sec * 1000;

    /* Completion is driven by the DIRECT child's exit (matching a plain
     * `sh -c cmd` run outside a sandbox), not by both pipes reaching EOF --
     * a command that backgrounds a long-lived process and exits itself
     * (e.g. "some_daemon & exit 0") must be treated as finished quickly,
     * even though the daemon keeps the pipes open. Streaming still happens
     * live in the meantime via poll(), it just isn't what completion waits
     * on. */
    while (!child_done) {
        long long remaining = deadline - now_ms();
        if (remaining <= 0) { timed_out = 1; break; }
        int poll_timeout = remaining < 100 ? (int)remaining : 100;

        struct pollfd pfds[2];
        int n_pfds = 0;
        int out_idx = -1, err_idx = -1;
        if (out_open) { out_idx = n_pfds; pfds[n_pfds].fd = out_pipe[0]; pfds[n_pfds].events = POLLIN; n_pfds++; }
        if (err_open) { err_idx = n_pfds; pfds[n_pfds].fd = err_pipe[0]; pfds[n_pfds].events = POLLIN; n_pfds++; }

        if (n_pfds > 0) {
            int pr = poll(pfds, (nfds_t)n_pfds, poll_timeout);
            if (pr > 0) {
                if (out_idx >= 0 && (pfds[out_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
                    if (drain_once(out_pipe[0], out_fd, 0, on_chunk, userdata)) out_open = 0;
                }
                if (err_idx >= 0 && (pfds[err_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
                    if (drain_once(err_pipe[0], err_fd, 1, on_chunk, userdata)) err_open = 0;
                }
            }
        } else {
            struct timespec nap = {0, (long)poll_timeout * 1000 * 1000};
            nanosleep(&nap, NULL);
        }

        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) child_done = 1;
        else if (r < 0) { child_done = 1; status = 0; }
    }

    if (timed_out) {
        /* Negative pid targets the whole process group, not just the one
         * child -- this is what actually reaches a backgrounded
         * grandchild like "sleep 30 &" that the immediate /bin/sh is still
         * waiting on. */
        kill(-pid, SIGKILL);
        waitpid(pid, &status, 0);
    }

    /* Drain whatever output was already sitting in the pipes right at
     * completion, without blocking on anything further. */
    for (int i = 0; i < 4 && (out_open || err_open); i++) {
        if (out_open && drain_once(out_pipe[0], out_fd, 0, on_chunk, userdata)) out_open = 0;
        if (err_open && drain_once(err_pipe[0], err_fd, 1, on_chunk, userdata)) err_open = 0;
    }

    /* Always sweep the process group once more, even on a normal exit:
     * the command may have spawned background children that outlived the
     * shell itself. Nothing left alive means this is a harmless no-op
     * (ESRCH). */
    kill(-pid, SIGKILL);
    for (;;) {
        pid_t r = waitpid(-pid, NULL, WNOHANG);
        if (r <= 0) break;
    }

    close(out_pipe[0]);
    close(err_pipe[0]);
    close(out_fd);
    close(err_fd);

    result->timed_out = timed_out;
    result->duration_ms = now_ms() - start;
    if (timed_out) {
        result->exit_code = -1;
    } else if (WIFEXITED(status)) {
        result->exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result->exit_code = 128 + WTERMSIG(status);
    } else {
        result->exit_code = -1;
    }

    return AGENTBOX_OK;
}
