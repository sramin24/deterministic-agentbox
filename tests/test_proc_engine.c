/*
 * Phase 4 functional test for agentbox_proc.c, run inside a real namespace
 * (mirrors how the real supervisor will call it). A precursor to the full
 * tests/test_c_engine.c.
 *
 * Usage:
 *   test_proc_engine                       run the full scenario, unprivileged
 *   test_proc_engine --install-apparmor P  (root) install profile for P
 */
#define _POSIX_C_SOURCE 200809L
#include "../src/c/agentbox.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_failures = 0;

static void check(int cond, const char *what) {
    if (cond) { printf("  PASS: %s\n", what); }
    else { printf("  FAIL: %s\n", what); g_failures++; }
}

static void mkdir_p(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

static int read_file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[4096] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    (void)n;
    return strstr(buf, needle) != NULL;
}

static int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--install-apparmor") == 0) {
        agentbox_errcode_t rc = agentbox_ns_install_apparmor_profile(argv[2]);
        printf("INSTALL_RESULT=%d\n", rc);
        return rc == AGENTBOX_OK ? 0 : 1;
    }

    const char *base = argc >= 2 ? argv[1] : "/tmp/proc_engine_scratch";

    if (agentbox_ns_enter() != AGENTBOX_OK) {
        fprintf(stderr, "agentbox_ns_enter failed (run --install-apparmor as root first)\n");
        return 2;
    }

    /* Same anchor requirement as every other test: forked first, before
     * anything else forks, so it -- not a real command -- takes the PID 1
     * slot of the new pid namespace and never exits on its own. */
    pid_t anchor_pid = fork();
    if (anchor_pid < 0) return 3;
    if (anchor_pid == 0) { for (;;) pause(); }

    mkdir_p(base);
    char cwd[4096], out_path[4096], err_path[4096];
    snprintf(cwd, sizeof(cwd), "%s", base);
    snprintf(out_path, sizeof(out_path), "%s/stdout.txt", base);
    snprintf(err_path, sizeof(err_path), "%s/stderr.txt", base);

    printf("== basic command: stdout/stderr capture + exit code ==\n");
    {
        agentbox_exec_result_t result;
        agentbox_errcode_t rc = agentbox_proc_run(
            cwd, "echo hello-stdout; echo hello-stderr >&2; exit 3",
            10, out_path, err_path, &result);
        check(rc == AGENTBOX_OK, "agentbox_proc_run succeeds");
        check(result.exit_code == 3, "exit_code == 3");
        check(result.timed_out == 0, "timed_out == 0");
        check(result.duration_ms >= 0 && result.duration_ms < 5000, "duration_ms is sane");
        check(read_file_contains(out_path, "hello-stdout"), "stdout captured correctly");
        check(read_file_contains(err_path, "hello-stderr"), "stderr captured correctly");
    }

    printf("== cwd is honored ==\n");
    {
        char subdir[4096];
        snprintf(subdir, sizeof(subdir), "%s/subdir", base);
        mkdir_p(subdir);
        agentbox_exec_result_t result;
        agentbox_proc_run(subdir, "pwd", 10, out_path, err_path, &result);
        check(read_file_contains(out_path, "subdir"), "command ran with the requested cwd");
    }

    printf("== timeout + process-group cleanup ==\n");
    {
        /* If cleanup only killed the immediate /bin/sh and not its whole
         * process group, the backgrounded subshell (sleep 3 && touch
         * marker) would survive and create the marker file ~3s later even
         * though agentbox_proc_run itself returned after the 1s timeout. */
        char marker[4096];
        snprintf(marker, sizeof(marker), "%s/marker.txt", base);
        remove(marker);

        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "( sleep 3 && touch '%s' ) & wait", marker);

        agentbox_exec_result_t result;
        long long t0 = time(NULL);
        agentbox_proc_run(cwd, cmd, 1, out_path, err_path, &result);
        long long elapsed = time(NULL) - t0;

        check(result.timed_out == 1, "timed_out == 1");
        check(elapsed < 5, "returned promptly (did not wait for the full 3s background job)");

        struct timespec nap = {4, 0};
        nanosleep(&nap, NULL); /* well past when the background job would have finished */
        check(!path_exists(marker), "backgrounded child was truly killed, not just detached");
    }

    kill(anchor_pid, SIGKILL);
    waitpid(anchor_pid, NULL, 0);

    printf("\n===== %s (%d failure%s) =====\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
