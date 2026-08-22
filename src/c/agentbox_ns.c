#define _GNU_SOURCE
#include "agentbox.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_proc_self(const char *leaf, const char *data) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/%s", leaf);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, data, strlen(data));
    int saved_errno = errno;
    close(fd);
    if (n < 0) { errno = saved_errno; return -1; }
    return 0;
}

static void mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

static void rm_rf(const char *path) {
    /* Best-effort scratch cleanup for the probe fixture only; the real
     * recursive purge used by the engine proper is agentbox_cow_purge()
     * (Phase 3), which needs nftw() to handle arbitrary workspace trees. */
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    int rc = system(cmd);
    (void)rc;
}

/*
 * Runs unshare + id-map + a real scratch overlay mount inside a throwaway
 * child, exactly mirroring what agentbox_ns_enter() plus the first
 * agentbox_cow_mount() call will do for a real Sandbox. Isolated under /tmp
 * because this is a pure capability check, not a real workspace mount -- it
 * never needs to share a filesystem with anything the caller owns.
 *
 * Measured on the real target (see spike/ns_overlay_probe.c): unshare()
 * itself succeeds even when blocked; the failure surfaces as EPERM on the
 * uid_map/setgroups write once the kernel transitions the process into the
 * restrictive `unprivileged_userns` AppArmor profile. That is
 * AGENTBOX_PROBE_ID_MAP_DENIED below, distinct from an actual unshare()
 * failure (AGENTBOX_PROBE_UNSHARE_FAILED), which is a different, rarer
 * condition (e.g. kernel.unprivileged_userns_clone=0).
 */
static agentbox_probe_result_t probe_child_body(const char *scratch, char *msg, size_t msg_len) {
    /* Must capture real uid/gid BEFORE unshare(CLONE_NEWUSER): once inside
     * the new (still-unmapped) namespace, getuid()/getgid() return the
     * overflow id (65534), not the real one, because there is no mapping
     * yet for the kernel to translate through. Writing that bogus overflow
     * id into uid_map/gid_map is rejected by the kernel with a plain EPERM
     * that has nothing to do with AppArmor -- and produces no LSM denial
     * in dmesg, which is what actually made this bug non-obvious. */
    uid_t real_uid = getuid();
    gid_t real_gid = getgid();

    if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID) != 0) {
        snprintf(msg, msg_len, "unshare failed: %s", strerror(errno));
        return AGENTBOX_PROBE_UNSHARE_FAILED;
    }

    if (write_proc_self("setgroups", "deny") != 0) {
        snprintf(msg, msg_len, "setgroups=deny failed: %s", strerror(errno));
        return AGENTBOX_PROBE_ID_MAP_DENIED;
    }
    char line[64];
    snprintf(line, sizeof(line), "0 %d 1", real_uid);
    if (write_proc_self("uid_map", line) != 0) {
        snprintf(msg, msg_len, "uid_map write failed: %s (this is the AppArmor-blocked case; run: agentbox setup)", strerror(errno));
        return AGENTBOX_PROBE_ID_MAP_DENIED;
    }
    snprintf(line, sizeof(line), "0 %d 1", real_gid);
    if (write_proc_self("gid_map", line) != 0) {
        snprintf(msg, msg_len, "gid_map write failed: %s", strerror(errno));
        return AGENTBOX_PROBE_ID_MAP_DENIED;
    }

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        snprintf(msg, msg_len, "MS_PRIVATE remount failed: %s", strerror(errno));
        return AGENTBOX_PROBE_MOUNT_OTHER_FAILED;
    }

    char lower[PATH_MAX], upper[PATH_MAX], work[PATH_MAX], merged[PATH_MAX];
    snprintf(lower, sizeof(lower), "%s/lower", scratch);
    snprintf(upper, sizeof(upper), "%s/upper", scratch);
    snprintf(work, sizeof(work), "%s/work", scratch);
    snprintf(merged, sizeof(merged), "%s/merged", scratch);
    mkdir_p(lower);
    mkdir_p(upper);
    mkdir_p(work);
    mkdir_p(merged);

    char opts[PATH_MAX * 3];
    /* userxattr: trusted.overlay.* xattrs are not settable from inside a
     * nested user namespace (confirmed empirically -- capable() for the
     * `trusted` xattr namespace requires CAP_SYS_ADMIN in the namespace that
     * owns the filesystem's superblock, not merely the calling namespace).
     * With userxattr, the kernel uses user.overlay.* instead, which nested
     * namespaces can set. agentbox_cow.c must mount the same way and check
     * for user.overlay.opaque, not trusted.overlay.opaque. */
    snprintf(opts, sizeof(opts), "lowerdir=%s,upperdir=%s,workdir=%s,userxattr", lower, upper, work);

    int rc = mount("overlay", merged, "overlay", 0, opts);
    if (rc != 0) {
        int mount_errno = errno;
        if (mount_errno == EPERM || mount_errno == EINVAL) {
            char cmd[PATH_MAX * 4];
            snprintf(cmd, sizeof(cmd), "fuse-overlayfs -o %s %s", opts, merged);
            int frc = system(cmd);
            if (frc != 0) {
                snprintf(msg, msg_len, "native mount failed (%s) and fuse-overlayfs fallback also failed (rc=%d)",
                         strerror(mount_errno), frc);
                return AGENTBOX_PROBE_MOUNT_PERM_DENIED;
            }
        } else {
            snprintf(msg, msg_len, "mount failed with unrelated errno: %s", strerror(mount_errno));
            return AGENTBOX_PROBE_MOUNT_OTHER_FAILED;
        }
    }

    umount2(merged, MNT_DETACH);
    snprintf(msg, msg_len, "ok");
    return AGENTBOX_PROBE_OK;
}

agentbox_probe_result_t agentbox_ns_probe(char *detail_buf, size_t detail_buf_len) {
    char scratch[PATH_MAX];
    snprintf(scratch, sizeof(scratch), "/tmp/agentbox_probe_%d", getpid());
    mkdir_p(scratch);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        if (detail_buf) snprintf(detail_buf, detail_buf_len, "pipe() failed: %s", strerror(errno));
        rm_rf(scratch);
        return AGENTBOX_PROBE_FORK_FAILED;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (detail_buf) snprintf(detail_buf, detail_buf_len, "fork() failed: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        rm_rf(scratch);
        return AGENTBOX_PROBE_FORK_FAILED;
    }

    if (pid == 0) {
        close(pipefd[0]);
        char msg[256] = {0};
        agentbox_probe_result_t r = probe_child_body(scratch, msg, sizeof(msg));
        ssize_t written = write(pipefd[1], msg, strlen(msg));
        (void)written;
        close(pipefd[1]);
        _exit((int)r);
    }

    close(pipefd[1]);
    char msg[256] = {0};
    ssize_t n = read(pipefd[0], msg, sizeof(msg) - 1);
    if (n < 0) n = 0;
    msg[n] = 0;
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    rm_rf(scratch);

    if (detail_buf) snprintf(detail_buf, detail_buf_len, "%s", msg);
    if (!WIFEXITED(status)) return AGENTBOX_PROBE_FORK_FAILED;
    return (agentbox_probe_result_t)WEXITSTATUS(status);
}

agentbox_errcode_t agentbox_ns_enter(void) {
    /* Capture before unshare() -- see the comment in probe_child_body(). */
    uid_t real_uid = getuid();
    gid_t real_gid = getgid();

    if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID) != 0) {
        return AGENTBOX_ERR_NAMESPACE_UNAVAILABLE;
    }
    if (write_proc_self("setgroups", "deny") != 0) {
        return AGENTBOX_ERR_NAMESPACE_UNAVAILABLE;
    }
    char line[64];
    snprintf(line, sizeof(line), "0 %d 1", real_uid);
    if (write_proc_self("uid_map", line) != 0) {
        return AGENTBOX_ERR_NAMESPACE_UNAVAILABLE;
    }
    snprintf(line, sizeof(line), "0 %d 1", real_gid);
    if (write_proc_self("gid_map", line) != 0) {
        return AGENTBOX_ERR_NAMESPACE_UNAVAILABLE;
    }
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        return AGENTBOX_ERR_MOUNT_FAILED;
    }
    return AGENTBOX_OK;
}

agentbox_errcode_t agentbox_ns_install_apparmor_profile(const char *binary_path) {
    if (geteuid() != 0) {
        return AGENTBOX_ERR_PERMISSION_DENIED;
    }
    if (binary_path == NULL || binary_path[0] != '/') {
        return AGENTBOX_ERR_INVALID_ARGUMENT;
    }
    struct stat st;
    if (stat(binary_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return AGENTBOX_ERR_INVALID_ARGUMENT;
    }

    if (stat("/sys/kernel/security/apparmor", &st) != 0) {
        /* AppArmor isn't active on this system; nothing to install, and
         * this is not a failure -- unprivileged userns likely already works
         * (or is blocked by something else the probe will report). */
        return AGENTBOX_OK;
    }

    const char *profile_path = "/etc/apparmor.d/agentbox-exec";
    FILE *f = fopen(profile_path, "w");
    if (!f) return AGENTBOX_ERR_IO;
    fprintf(f,
            "abi <abi/4.0>,\n"
            "include <tunables/global>\n"
            "\n"
            "\"%s\" flags=(unconfined) {\n"
            "  userns,\n"
            "}\n",
            binary_path);
    fclose(f);

    pid_t pid = fork();
    if (pid < 0) return AGENTBOX_ERR_GENERIC;
    if (pid == 0) {
        execlp("apparmor_parser", "apparmor_parser", "-r", "-W", profile_path, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return AGENTBOX_ERR_GENERIC;
    }
    return AGENTBOX_OK;
}
