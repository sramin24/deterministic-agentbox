#define _GNU_SOURCE
#include "agentbox.h"

#include <errno.h>
#include <fcntl.h>
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

agentbox_errcode_t agentbox_proc_run(const char *cwd, const char *cmd, int timeout_sec,
                                      const char *stdout_path, const char *stderr_path,
                                      agentbox_exec_result_t *result) {
    memset(result, 0, sizeof(*result));
    snprintf(result->stdout_path, sizeof(result->stdout_path), "%s", stdout_path);
    snprintf(result->stderr_path, sizeof(result->stderr_path), "%s", stderr_path);

    long long start = now_ms();

    pid_t pid = fork();
    if (pid < 0) {
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

        int out_fd = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) _exit(126);
        int err_fd = open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (err_fd < 0) _exit(126);
        dup2(out_fd, STDOUT_FILENO);
        dup2(err_fd, STDERR_FILENO);
        close(out_fd);
        close(err_fd);

        execlp("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
        _exit(127); /* only reached if execlp itself failed */
    }

    /* Parent: poll rather than block, so a timeout can be enforced. A short
     * sleep between checks is fine -- Sandbox.run() has no sub-millisecond
     * latency requirement, unlike checkpoint/rollback. */
    int status = 0;
    int timed_out = 0;
    long long deadline = start + (long long)timeout_sec * 1000;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break; /* exited (or was signaled) within the deadline */
        if (r < 0) { return AGENTBOX_ERR_GENERIC; }
        if (now_ms() >= deadline) {
            timed_out = 1;
            break;
        }
        struct timespec nap = {0, 20 * 1000 * 1000}; /* 20ms */
        nanosleep(&nap, NULL);
    }

    if (timed_out) {
        /* Negative pid targets the whole process group, not just the one
         * child -- this is what actually reaches a backgrounded
         * grandchild like "sleep 30 &" that the immediate /bin/sh has
         * already returned from waiting on. */
        kill(-pid, SIGKILL);
        waitpid(pid, &status, 0);
    }

    /* Always sweep the process group once more, even on a normal exit:
     * the command may have spawned background children that outlived the
     * shell itself (e.g. "some_daemon & exit 0"). Nothing left alive means
     * this is a harmless no-op (ESRCH). */
    kill(-pid, SIGKILL);
    for (;;) {
        pid_t r = waitpid(-pid, NULL, WNOHANG);
        if (r <= 0) break;
    }

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
