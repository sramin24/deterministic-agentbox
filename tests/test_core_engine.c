/*
 * Phase 5 functional test for agentbox_core.c -- exercises ONLY the
 * client-side public API (agentbox_create/checkpoint/exec/rollback/
 * commit/diff/destroy/probe_capabilities), exactly as a Python caller
 * eventually will. This test process itself never calls
 * agentbox_ns_enter() or touches namespaces -- only the separate
 * agentbox-exec binary it forks does, which is the entire point of the
 * dedicated-wrapper-binary architecture: this test runs fully
 * unprivileged, with no AppArmor profile of its own, as long as
 * agentbox-exec already has one installed.
 */
#define _POSIX_C_SOURCE 200809L
#include "../src/c/agentbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static int read_file_matches(const char *path, const char *expect) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    (void)n;
    return strcmp(buf, expect) == 0;
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

int main(int argc, char **argv) {
    const char *base = argc >= 2 ? argv[1] : "/tmp/core_engine_scratch";
    char workspace[4096];
    snprintf(workspace, sizeof(workspace), "%s/workspace", base);
    mkdir_p(workspace);
    char fixture[4096];
    snprintf(fixture, sizeof(fixture), "%s/file.txt", workspace);
    write_file(fixture, "original");

    printf("== capability probe (runs agentbox-exec --probe, not this process) ==\n");
    {
        char detail[256] = {0};
        agentbox_probe_result_t rc = agentbox_probe_capabilities(detail, sizeof(detail));
        printf("    detail: %s\n", detail);
        check(rc == AGENTBOX_PROBE_OK, "capability probe succeeds (run agentbox-exec --install-apparmor first if this fails)");
    }

    printf("== agentbox_create ==\n");
    agentbox_t box;
    agentbox_errcode_t rc = agentbox_create(&box, workspace, NULL);
    check(rc == AGENTBOX_OK, "agentbox_create succeeds");
    if (rc != AGENTBOX_OK) { printf("    (%s)\n", agentbox_strerror(rc)); return 1; }
    check(box.active, "box is active");

    printf("== run a command, verify isolation from the real workspace ==\n");
    {
        agentbox_exec_result_t result;
        rc = agentbox_exec(&box, NULL, "echo -n modified > file.txt", 10, &result);
        check(rc == AGENTBOX_OK, "agentbox_exec succeeds");
        check(result.exit_code == 0, "exit_code == 0");
        check(result.timed_out == 0, "not timed out");

        rc = agentbox_exec(&box, NULL, "cat file.txt", 10, &result);
        check(read_file_contains(result.stdout_path, "modified"), "sandbox sees the modified content");

        check(read_file_matches(fixture, "original"),
              "REAL host workspace file is untouched (read directly, not through the sandbox)");
    }

    printf("== checkpoint + further mutation + rollback ==\n");
    {
        char ckpt_id[AGENTBOX_ID_LEN];
        rc = agentbox_checkpoint(&box, "after-modify", ckpt_id, sizeof(ckpt_id));
        check(rc == AGENTBOX_OK, "agentbox_checkpoint succeeds");
        check(ckpt_id[0] != '\0', "checkpoint id is non-empty");

        agentbox_exec_result_t result;
        rc = agentbox_exec(&box, NULL, "rm -f file.txt; echo -n after-checkpoint > newfile.txt", 10, &result);
        check(rc == AGENTBOX_OK, "post-checkpoint mutation succeeds");

        rc = agentbox_exec(&box, NULL, "test -f newfile.txt && echo present", 10, &result);
        check(read_file_contains(result.stdout_path, "present"), "newfile.txt exists before rollback");

        rc = agentbox_rollback(&box, NULL);
        check(rc == AGENTBOX_OK, "agentbox_rollback (most recent) succeeds");

        rc = agentbox_exec(&box, NULL, "cat file.txt 2>/dev/null; test -f newfile.txt && echo LEAKED", 10, &result);
        check(read_file_contains(result.stdout_path, "modified"), "file.txt restored to checkpointed state");
        check(!read_file_contains(result.stdout_path, "LEAKED"), "newfile.txt from after the checkpoint is gone");

        check(read_file_matches(fixture, "original"),
              "REAL host workspace still untouched after checkpoint/rollback");
    }

    printf("== diff before commit ==\n");
    {
        char **paths = NULL;
        size_t count = 0;
        rc = agentbox_diff(&box, &paths, &count);
        check(rc == AGENTBOX_OK, "agentbox_diff succeeds");
        int saw_file = 0;
        for (size_t i = 0; i < count; i++) {
            printf("    diff: %s\n", paths[i]);
            if (strcmp(paths[i], "M file.txt") == 0) saw_file = 1;
        }
        check(saw_file, "diff reports M file.txt");
        agentbox_free_string_list(paths, count);
    }

    printf("== commit onto the real workspace ==\n");
    {
        rc = agentbox_commit(&box);
        check(rc == AGENTBOX_OK, "agentbox_commit succeeds");
        check(read_file_matches(fixture, "modified"),
              "REAL host workspace now reflects the committed change");

        char **paths = NULL;
        size_t count = 0;
        agentbox_diff(&box, &paths, &count);
        check(count == 0, "diff is empty immediately after commit");
        agentbox_free_string_list(paths, count);
    }

    printf("== destroy ==\n");
    {
        char runtime_dir_copy[4096];
        snprintf(runtime_dir_copy, sizeof(runtime_dir_copy), "%s", box.runtime_dir);
        rc = agentbox_destroy(&box);
        check(rc == AGENTBOX_OK, "agentbox_destroy succeeds");
        struct stat st;
        check(stat(runtime_dir_copy, &st) != 0, "runtime_dir is fully gone");
    }

    printf("\n===== %s (%d failure%s) =====\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
