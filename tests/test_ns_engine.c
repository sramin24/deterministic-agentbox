/*
 * Phase 2 functional test for agentbox_ns.c specifically (a precursor to the
 * full tests/test_c_engine.c, which needs agentbox_cow.c/agentbox_proc.c/
 * agentbox_core.c to exist first). Exercises the real production functions,
 * not a spike reimplementation.
 *
 * Usage:
 *   test_ns_engine                        run the probe, print+exit its result
 *   test_ns_engine --install-apparmor P    (root) install the profile for
 *                                          binary path P, print+exit result
 */
#include "../src/c/agentbox.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--install-apparmor") == 0) {
        agentbox_errcode_t rc = agentbox_ns_install_apparmor_profile(argv[2]);
        printf("INSTALL_RESULT=%d\n", rc);
        return rc == AGENTBOX_OK ? 0 : 1;
    }

    char detail[256] = {0};
    agentbox_probe_result_t rc = agentbox_ns_probe(detail, sizeof(detail));
    printf("PROBE_RESULT=%d\n", (int)rc);
    printf("PROBE_DETAIL=%s\n", detail);
    return (int)rc;
}
