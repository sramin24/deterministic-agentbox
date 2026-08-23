/*
 * The canonical functional test harness for the whole engine, linked
 * directly against the engine sources by the `test-c` Makefile target (not
 * a mock). Exercises only the client-side public API, exactly as Python
 * will -- this test process itself never calls agentbox_ns_enter() or
 * touches namespaces; only the separate agentbox-exec binary it forks
 * does, so this test needs no AppArmor profile of its own.
 *
 * Usage:
 *   test_c_engine                       run the full 12-step scenario
 *   test_c_engine --install-apparmor P  (root) install profile for P,
 *                                       then exit -- exercises the real
 *                                       `agentbox setup` code path
 */
#define _POSIX_C_SOURCE 200809L
#include "../src/c/agentbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    fputs(content, f);
    fclose(f);
}

static int host_file_matches(const char *path, const char *expect) {
    FILE *f = fopen(path, "r");
    if (!f) return expect == NULL;
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    (void)n;
    return expect != NULL && strcmp(buf, expect) == 0;
}

static int host_path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int captured_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[4096] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    (void)n;
    return strstr(buf, needle) != NULL;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--install-apparmor") == 0) {
        agentbox_errcode_t rc = agentbox_ns_install_apparmor_profile(argv[2]);
        printf("INSTALL_RESULT=%d\n", rc);
        return rc == AGENTBOX_OK ? 0 : 1;
    }

    const char *base = argc >= 2 ? argv[1] : "/tmp/agentbox_test_c_engine";
    char workspace[4096], file_path[4096], subdir_path[4096], subdir_file_path[4096];
    snprintf(workspace, sizeof(workspace), "%s/workspace", base);
    snprintf(file_path, sizeof(file_path), "%s/file.txt", workspace);
    snprintf(subdir_path, sizeof(subdir_path), "%s/subdir", workspace);
    snprintf(subdir_file_path, sizeof(subdir_file_path), "%s/subdir/inside.txt", workspace);

    /* ---- 1: capability probe ---------------------------------------- */
    printf("== 1: agentbox_probe_capabilities ==\n");
    {
        char detail[256] = {0};
        agentbox_probe_result_t rc = agentbox_probe_capabilities(detail, sizeof(detail));
        printf("    detail: %s\n", detail);
        check(rc == AGENTBOX_PROBE_OK,
              "probe succeeds (run: out/agentbox-exec --install-apparmor \"$(pwd)/out/agentbox-exec\" as root first)");
    }

    /* ---- 2: fixture ---------------------------------------------------- */
    printf("== 2: fixture workspace (top-level file + subdir with a file) ==\n");
    mkdir_p(subdir_path);
    write_file(file_path, "original");
    write_file(subdir_file_path, "inside-original");
    check(host_file_matches(file_path, "original") && host_file_matches(subdir_file_path, "inside-original"),
          "fixture created correctly");

    /* ---- 3: create ------------------------------------------------------ */
    printf("== 3: agentbox_create ==\n");
    agentbox_t box;
    agentbox_errcode_t rc = agentbox_create(&box, workspace, NULL);
    check(rc == AGENTBOX_OK, "agentbox_create succeeds");
    if (rc != AGENTBOX_OK) { printf("    (%s)\n", agentbox_strerror(rc)); return 1; }

    /* ---- 4: read original content through the sandbox -------------- */
    printf("== 4: read fixture's original content through agentbox_exec ==\n");
    {
        agentbox_exec_result_t result;
        agentbox_exec(&box, NULL, "cat file.txt; echo ---; cat subdir/inside.txt", 10, &result);
        check(captured_contains(result.stdout_path, "original") &&
              captured_contains(result.stdout_path, "inside-original"),
              "sandbox sees the original fixture content");
    }

    /* ---- 5: mutate through the sandbox, verify host untouched -------- */
    printf("== 5: mutate file, delete subdir, create new file+dir -- all via agentbox_exec ==\n");
    {
        agentbox_exec_result_t result;
        agentbox_exec(&box, NULL, "echo -n modified > file.txt", 10, &result);
        agentbox_exec(&box, NULL, "rm -rf subdir", 10, &result);
        agentbox_exec(&box, NULL, "echo -n brand-new > newfile.txt", 10, &result);
        agentbox_exec(&box, NULL, "mkdir newdir && echo -n new-inside > newdir/newinside.txt", 10, &result);

        check(host_file_matches(file_path, "original"), "REAL host file.txt still 'original'");
        check(host_path_exists(subdir_path), "REAL host subdir still present");
        check(host_file_matches(subdir_file_path, "inside-original"), "REAL host subdir/inside.txt untouched");
        char newfile_host[4096];
        snprintf(newfile_host, sizeof(newfile_host), "%s/newfile.txt", workspace);
        check(!host_path_exists(newfile_host), "REAL host workspace has no newfile.txt yet (uncommitted)");
    }

    /* ---- 6: checkpoint --------------------------------------------------- */
    printf("== 6: agentbox_checkpoint ==\n");
    char ckpt_id[AGENTBOX_ID_LEN];
    rc = agentbox_checkpoint(&box, "after-first-mutation", ckpt_id, sizeof(ckpt_id));
    check(rc == AGENTBOX_OK, "agentbox_checkpoint succeeds");
    check(ckpt_id[0] != '\0', "checkpoint id is non-empty");

    /* ---- 7: destructive rm -rf * after the checkpoint ------------------- */
    printf("== 7: destructive rm -rf * after checkpoint ==\n");
    {
        agentbox_exec_result_t result;
        agentbox_exec(&box, NULL, "rm -rf * .[!.]* 2>/dev/null; ls -A | wc -l", 10, &result);
        check(captured_contains(result.stdout_path, "0"), "sandbox is empty after rm -rf *");
    }

    /* ---- 8: rollback ------------------------------------------------------ */
    printf("== 8: agentbox_rollback(NULL) -- most recent checkpoint ==\n");
    {
        long long t0 = (long long)time(NULL);
        rc = agentbox_rollback(&box, NULL);
        long long elapsed_s = (long long)time(NULL) - t0;
        check(rc == AGENTBOX_OK, "agentbox_rollback succeeds");
        printf("    elapsed: %llds\n", elapsed_s);

        agentbox_exec_result_t result;
        agentbox_exec(&box, NULL,
            "cat file.txt 2>/dev/null; echo ---; test -d subdir && echo SUBDIR_PRESENT; "
            "cat newfile.txt 2>/dev/null; echo ---; cat newdir/newinside.txt 2>/dev/null",
            10, &result);
        check(captured_contains(result.stdout_path, "modified"), "file.txt restored to checkpointed content");
        check(!captured_contains(result.stdout_path, "SUBDIR_PRESENT"), "subdir stays deleted (as checkpointed)");
        check(captured_contains(result.stdout_path, "brand-new"), "newfile.txt restored to checkpointed content");
        check(captured_contains(result.stdout_path, "new-inside"), "newdir/newinside.txt restored to checkpointed content");
    }

    /* ---- 9: commit -------------------------------------------------------- */
    printf("== 9: agentbox_commit onto the real workspace ==\n");
    {
        rc = agentbox_commit(&box);
        check(rc == AGENTBOX_OK, "agentbox_commit succeeds");
        check(host_file_matches(file_path, "modified"), "REAL host file.txt == 'modified'");
        check(!host_path_exists(subdir_path), "REAL host subdir gone");
        char newfile_host[4096], newdir_file_host[4096];
        snprintf(newfile_host, sizeof(newfile_host), "%s/newfile.txt", workspace);
        snprintf(newdir_file_host, sizeof(newdir_file_host), "%s/newdir/newinside.txt", workspace);
        check(host_file_matches(newfile_host, "brand-new"), "REAL host newfile.txt == 'brand-new'");
        check(host_file_matches(newdir_file_host, "new-inside"), "REAL host newdir/newinside.txt == 'new-inside'");
    }

    /* ---- 10: diff is empty right after commit ---------------------------- */
    printf("== 10: agentbox_diff returns zero entries immediately after commit ==\n");
    {
        char **paths = NULL;
        size_t count = 0;
        agentbox_diff(&box, &paths, &count);
        check(count == 0, "diff is empty");
        agentbox_free_string_list(paths, count);
    }

    /* ---- 11: timeout + process-group cleanup ------------------------------ */
    printf("== 11: backgrounded long-running command respects timeout ==\n");
    {
        agentbox_exec_result_t result;
        long long t0 = (long long)time(NULL);
        agentbox_exec(&box, NULL, "sleep 30 & wait", 2, &result);
        long long elapsed_s = (long long)time(NULL) - t0;
        check(result.timed_out == 1, "timed_out == 1");
        check(elapsed_s < 10, "returned promptly, did not hang for the full 30s");
    }

    /* ---- 12: destroy removes runtime_dir entirely ------------------------- */
    printf("== 12: agentbox_destroy removes runtime_dir entirely ==\n");
    {
        char runtime_dir_copy[4096];
        snprintf(runtime_dir_copy, sizeof(runtime_dir_copy), "%s", box.runtime_dir);
        rc = agentbox_destroy(&box);
        check(rc == AGENTBOX_OK, "agentbox_destroy succeeds");
        check(!host_path_exists(runtime_dir_copy), "runtime_dir is fully gone");
    }

    printf("\n===== %s (%d failure%s) =====\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
