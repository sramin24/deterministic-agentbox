/*
 * Thin entry point for the dedicated `agentbox-exec` binary. This is the
 * ONLY thing the AppArmor profile installed by `agentbox setup` grants
 * `userns,` to (see agentbox.h's AGENTBOX_EXEC_BINARY_NAME comment) --
 * scoping the grant to this single-purpose binary rather than to
 * sys.executable, which would otherwise hand the same privilege to
 * anything else invoked through the same shared Python interpreter.
 *
 * agentbox_create() (agentbox_core.c, running inside the Python process)
 * forks and execve()s into this binary; everything past that point --
 * unshare(), the anchor process, the overlay mount, the supervisor's
 * request loop -- runs inside THIS process's image, which is what makes
 * the AppArmor profile's binary match apply correctly.
 */
#include "agentbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--probe") == 0) {
        char detail[256] = {0};
        agentbox_probe_result_t rc = agentbox_ns_probe(detail, sizeof(detail));
        printf("%s", detail);
        fflush(stdout);
        return (int)rc;
    }

    if (argc < 4) {
        fprintf(stderr, "usage: %s <workspace_dir> <runtime_dir> <ctrl_fd>\n", argv[0]);
        return 2;
    }
    const char *workspace_dir = argv[1];
    const char *runtime_dir = argv[2];
    int ctrl_fd = atoi(argv[3]);

    return agentbox_supervisor_main(ctrl_fd, workspace_dir, runtime_dir);
}
