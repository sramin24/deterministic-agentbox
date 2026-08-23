/*
 * Phase 3 functional test for agentbox_cow.c, run inside a real namespace
 * entered via the already-verified agentbox_ns_enter() (Phase 2). A
 * precursor to the full tests/test_c_engine.c.
 *
 * Usage:
 *   test_cow_engine                        run the full scenario, unprivileged
 *   test_cow_engine --install-apparmor P   (root) install profile for P
 */
#define _POSIX_C_SOURCE 200809L
#include "../src/c/agentbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "write_file(%s) failed\n", path); exit(90); }
    fputs(content, f);
    fclose(f);
}

static int read_file_matches(const char *path, const char *expect) {
    FILE *f = fopen(path, "r");
    if (!f) return expect == NULL;
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    (void)n;
    return expect != NULL && strcmp(buf, expect) == 0;
}

static int path_exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--install-apparmor") == 0) {
        agentbox_errcode_t rc = agentbox_ns_install_apparmor_profile(argv[2]);
        printf("INSTALL_RESULT=%d\n", rc);
        return rc == AGENTBOX_OK ? 0 : 1;
    }

    const char *base = argc >= 2 ? argv[1] : "/tmp/cow_engine_scratch";

    agentbox_errcode_t ns_rc = agentbox_ns_enter();
    if (ns_rc != AGENTBOX_OK) {
        fprintf(stderr, "agentbox_ns_enter failed: %d (run: test_cow_engine --install-apparmor <this binary path>, as root, first)\n", ns_rc);
        return 2;
    }

    /* Per spec Part 0.4: the first process forked after unshare(CLONE_NEWPID)
     * becomes PID 1 of the new pid namespace. If it's allowed to be one of
     * this test's own system() calls, it will exit normally when that
     * command finishes, silently tearing down the pid namespace and making
     * every subsequent fork() in this test fail. This anchor -- forked here,
     * before anything else forks -- takes that PID 1 slot permanently and
     * never exits on its own, so real commands land as PID 2, 3, ... and can
     * exit freely without taking the namespace down with them. */
    pid_t anchor_pid = fork();
    if (anchor_pid < 0) { fprintf(stderr, "anchor fork failed\n"); return 3; }
    if (anchor_pid == 0) {
        for (;;) pause();
    }

    char workspace[4096], upper1[4096], work1[4096], merged1[4096];
    snprintf(workspace, sizeof(workspace), "%s/workspace", base);
    snprintf(upper1, sizeof(upper1), "%s/upper1", base);
    snprintf(work1, sizeof(work1), "%s/work1", base);
    snprintf(merged1, sizeof(merged1), "%s/merged1", base);

    /* ---- fixture: original workspace, file + subdir + file ---- */
    mkdir_p(workspace);
    {
        char p[4096];
        snprintf(p, sizeof(p), "%s/file.txt", workspace);
        write_file(p, "original");
        snprintf(p, sizeof(p), "%s/subdir", workspace);
        mkdir_p(p);
        snprintf(p, sizeof(p), "%s/subdir/inside.txt", workspace);
        write_file(p, "inside-original");
        snprintf(p, sizeof(p), "%s/keep_subdir", workspace);
        mkdir_p(p);
        snprintf(p, sizeof(p), "%s/keep_subdir/keepme.txt", workspace);
        write_file(p, "keep-original");
    }

    mkdir_p(upper1);
    mkdir_p(work1);
    mkdir_p(merged1);

    printf("== mount ==\n");
    agentbox_errcode_t rc = agentbox_cow_mount(workspace, upper1, work1, merged1);
    check(rc == AGENTBOX_OK, "agentbox_cow_mount succeeds");
    if (rc != AGENTBOX_OK) return 1;

    printf("== mutate through merged view ==\n");
    {
        char p[4096];
        /* 1. modify existing top-level file */
        snprintf(p, sizeof(p), "%s/file.txt", merged1);
        write_file(p, "modified");

        /* 2. delete a subdir entirely -> whiteout in upper1 */
        char cmd[8192];
        snprintf(p, sizeof(p), "%s/subdir", merged1);
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", p);
        system(cmd);

        /* 3. delete-and-recreate keep_subdir -> should end up opaque in
         *    upper1, with fresh (different) contents replacing the
         *    original. Deliberately done as two SEPARATE commands (like two
         *    separate Sandbox.run() calls), rather than one "rm && mkdir"
         *    chain, since that's the realistic shape of agent usage. This
         *    only marks opaque correctly if the anchor process above is
         *    doing its job -- without it, the first of these two system()
         *    calls becomes PID 1, exits, and silently breaks every fork
         *    after it (see the anchor comment above); that failure mode
         *    was mistaken for a kernel opaque-marking bug during initial
         *    testing until isolating the anchor fixed it. */
        snprintf(p, sizeof(p), "%s/keep_subdir", merged1);
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", p);
        system(cmd);
        snprintf(cmd, sizeof(cmd), "mkdir '%s'", p);
        system(cmd);
        snprintf(p, sizeof(p), "%s/keep_subdir/replaced.txt", merged1);
        write_file(p, "replaced-content");

        /* 4. create a brand new top-level file and dir */
        snprintf(p, sizeof(p), "%s/newfile.txt", merged1);
        write_file(p, "brand-new");
        snprintf(p, sizeof(p), "%s/newdir", merged1);
        mkdir_p(p);
        snprintf(p, sizeof(p), "%s/newdir/newinside.txt", merged1);
        write_file(p, "new-inside");
    }

    printf("== read back through merged before unmount ==\n");
    {
        char p[4096];
        snprintf(p, sizeof(p), "%s/file.txt", merged1);
        check(read_file_matches(p, "modified"), "merged/file.txt reads back 'modified'");
    }

    printf("== unmount ==\n");
    rc = agentbox_cow_unmount(merged1);
    check(rc == AGENTBOX_OK, "agentbox_cow_unmount succeeds");

    printf("== diff before commit ==\n");
    {
        const char *layers[1] = {upper1};
        char **paths = NULL;
        size_t count = 0;
        rc = agentbox_cow_diff(layers, 1, &paths, &count);
        check(rc == AGENTBOX_OK, "agentbox_cow_diff succeeds");
        int saw_modified_file = 0, saw_deleted_subdir = 0, saw_new_file = 0;
        for (size_t i = 0; i < count; i++) {
            printf("    diff: %s\n", paths[i]);
            if (strcmp(paths[i], "M file.txt") == 0) saw_modified_file = 1;
            /* The whole subdir is whited out as ONE entry (a single
             * char-device marker named "subdir"), not per file inside it --
             * overlayfs doesn't descend into a directory it's about to hide
             * entirely. */
            if (strcmp(paths[i], "D subdir") == 0) saw_deleted_subdir = 1;
            if (strcmp(paths[i], "M newfile.txt") == 0) saw_new_file = 1;
        }
        check(saw_modified_file, "diff reports M file.txt");
        check(saw_deleted_subdir, "diff reports D subdir (whiteout)");
        check(saw_new_file, "diff reports M newfile.txt (new file)");
        agentbox_free_string_list(paths, count);
    }

    printf("== commit_merge onto the real workspace ==\n");
    {
        const char *layers[1] = {upper1};
        rc = agentbox_cow_commit_merge(layers, 1, workspace);
        check(rc == AGENTBOX_OK, "agentbox_cow_commit_merge succeeds");
    }

    printf("== verify real workspace reflects every change ==\n");
    {
        char p[4096];
        snprintf(p, sizeof(p), "%s/file.txt", workspace);
        check(read_file_matches(p, "modified"), "workspace/file.txt == 'modified'");

        snprintf(p, sizeof(p), "%s/subdir/inside.txt", workspace);
        check(!path_exists(p), "workspace/subdir/inside.txt gone (whiteout applied)");
        snprintf(p, sizeof(p), "%s/subdir", workspace);
        check(!path_exists(p), "workspace/subdir gone entirely");

        snprintf(p, sizeof(p), "%s/keep_subdir/keepme.txt", workspace);
        check(!path_exists(p), "workspace/keep_subdir/keepme.txt gone (opaque reset, not merged)");
        snprintf(p, sizeof(p), "%s/keep_subdir/replaced.txt", workspace);
        check(read_file_matches(p, "replaced-content"), "workspace/keep_subdir/replaced.txt == 'replaced-content'");

        snprintf(p, sizeof(p), "%s/newfile.txt", workspace);
        check(read_file_matches(p, "brand-new"), "workspace/newfile.txt == 'brand-new'");
        snprintf(p, sizeof(p), "%s/newdir/newinside.txt", workspace);
        check(read_file_matches(p, "new-inside"), "workspace/newdir/newinside.txt == 'new-inside'");
    }

    printf("== agentbox_cow_purge on a scratch subtree ==\n");
    {
        char p[4096];
        snprintf(p, sizeof(p), "%s/purge_target", base);
        mkdir_p(p);
        char inner_dir[4096], inner_file[4096];
        snprintf(inner_dir, sizeof(inner_dir), "%s/a/b", p);
        mkdir_p(inner_dir);
        snprintf(inner_file, sizeof(inner_file), "%s/c.txt", inner_dir);
        write_file(inner_file, "x");
        rc = agentbox_cow_purge(p);
        check(rc == AGENTBOX_OK, "agentbox_cow_purge succeeds");
        check(!path_exists(p), "purge target directory fully gone");
    }

    kill(anchor_pid, SIGKILL);
    waitpid(anchor_pid, NULL, 0);

    printf("\n===== %s (%d failure%s) =====\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
