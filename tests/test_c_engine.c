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

#define MAX_RECORDED_CHUNKS 32
static long long g_chunk_times_ms[MAX_RECORDED_CHUNKS];
static int g_chunk_count = 0;

static long long wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void record_chunk_timing(int is_stderr, const char *data, size_t len, void *userdata) {
    (void)is_stderr; (void)data; (void)len; (void)userdata;
    if (g_chunk_count < MAX_RECORDED_CHUNKS) g_chunk_times_ms[g_chunk_count++] = wall_ms();
}

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
    /* Start from a clean slate every run -- otherwise a previous run's
     * commit (step 9) leaves newfile.txt/newdir sitting in the real
     * workspace, and step 5's "nothing committed yet" check fails purely
     * from stale leftovers, not an actual regression. */
    {
        char rm_cmd[4200];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", base);
        system(rm_cmd);
    }
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

    /* ---- live streaming: chunks arrive as produced, not all at the end --- */
    printf("== live streaming: agentbox_exec_streaming delivers output as it happens ==\n");
    {
        g_chunk_count = 0;
        long long call_start = wall_ms();
        agentbox_exec_result_t result;
        agentbox_exec_streaming(&box, NULL, "echo one; sleep 1; echo two; sleep 1; echo three",
                                 10, record_chunk_timing, NULL, &result);
        long long call_end = wall_ms();

        check(g_chunk_count >= 3, "at least 3 chunks were delivered");
        long long total_wall = call_end - call_start;
        long long first_to_last = g_chunk_count >= 2
            ? g_chunk_times_ms[g_chunk_count - 1] - g_chunk_times_ms[0] : 0;
        printf("    chunks=%d total_call_ms=%lld first_to_last_chunk_ms=%lld\n",
               g_chunk_count, total_wall, first_to_last);
        /* If output were only buffered and delivered in one lump at the end
         * (the old, non-streaming behavior), every chunk timestamp would
         * cluster within a few ms of call_end, and first_to_last would be
         * near 0 even though the command itself took ~2s. Seeing the
         * chunks spread out over a large fraction of that 2s is the actual
         * proof this is live, not just "eventually correct". */
        check(first_to_last > 1500, "chunks are spread across the command's real ~2s runtime, not bunched at the end");
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
