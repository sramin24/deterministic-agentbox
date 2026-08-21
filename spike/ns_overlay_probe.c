/*
 * THROWAWAY PROTOTYPE -- not part of the agentbox engine, not linked by the
 * Makefile, not part of the ABI. Exists to answer one question before the
 * real agentbox_ns.c / agentbox_cow.c / agentbox_core.c get written:
 *
 *   Does a scoped AppArmor profile (granting `userns,` to this one binary)
 *   actually restore CAP_SYS_ADMIN inside a freshly-unshared user namespace
 *   on this real Ubuntu 26.04 kernel, and does a real OverlayFS mount +
 *   copy-on-write then work end to end?
 *
 * Verdict goes in the FEASIBILITY VERDICT block printed at the end.
 * Delete this whole directory once the answer is folded into the real
 * engine and the verdict is recorded.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/xattr.h>

static int g_unshare_ok = 0;
static int g_idmap_ok = 0;
static int g_mount_ok = 0;
static int g_cow_ok = 0;
static int g_xattr_ok = 0;
static int g_anchor_survives = 0;

static void step(const char *name) { printf("\n== %s ==\n", name); }

static int write_file(const char *path, const char *data) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) { printf("  open(%s) failed: %s\n", path, strerror(errno)); return -1; }
    ssize_t n = write(fd, data, strlen(data));
    int saved_errno = errno;
    close(fd);
    if (n < 0) { printf("  write(%s) failed: %s\n", path, strerror(saved_errno)); return -1; }
    printf("  wrote '%s' -> %s\n", data, path);
    return 0;
}

static void mkdir_p(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <scratch_base_dir>\n", argv[0]);
        return 2;
    }
    const char *base = argv[1];
    uid_t real_uid = getuid();
    gid_t real_gid = getgid();

    printf("pid=%d uid=%d gid=%d scratch_base=%s\n", getpid(), real_uid, real_gid, base);

    step("unshare(CLONE_NEWUSER|CLONE_NEWNS|CLONE_NEWPID)");
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID) != 0) {
        printf("  FAILED: %s (errno=%d)\n", strerror(errno), errno);
        goto verdict;
    }
    printf("  unshare() returned 0 (namespace created)\n");
    g_unshare_ok = 1;

    step("write /proc/self/setgroups = deny");
    if (write_file("/proc/self/setgroups", "deny") != 0) goto verdict;

    step("write /proc/self/uid_map (this is where AppArmor's post-unshare CAP_SYS_ADMIN denial actually bites)");
    {
        char line[128];
        snprintf(line, sizeof(line), "0 %d 1", real_uid);
        if (write_file("/proc/self/uid_map", line) != 0) {
            printf("  --> THIS is the AppArmor-blocked case if EPERM. Needs the profile.\n");
            goto verdict;
        }
    }

    step("write /proc/self/gid_map");
    {
        char line[128];
        snprintf(line, sizeof(line), "0 %d 1", real_gid);
        if (write_file("/proc/self/gid_map", line) != 0) goto verdict;
    }
    g_idmap_ok = 1;
    printf("  --> uid_map/gid_map succeeded: CAP_SYS_ADMIN is present in the new userns.\n");

    step("mount(NULL, \"/\", MS_REC|MS_PRIVATE)");
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        printf("  FAILED: %s\n", strerror(errno));
        goto verdict;
    }
    printf("  OK\n");

    step("fork idle anchor (PID 1 of new pidns must never exit on its own)");
    pid_t anchor_pid = fork();
    if (anchor_pid < 0) { printf("  fork FAILED: %s\n", strerror(errno)); goto verdict; }
    if (anchor_pid == 0) {
        for (;;) pause();
        _exit(0);
    }
    printf("  anchor forked, pid(as seen by supervisor)=%d\n", anchor_pid);

    step("set up overlay fixture on scratch base (must be same filesystem as base)");
    char lower[4096], upper[4096], work[4096], merged[4096], lowerfile[4096];
    snprintf(lower, sizeof(lower), "%s/lower", base);
    snprintf(upper, sizeof(upper), "%s/upper", base);
    snprintf(work, sizeof(work), "%s/work", base);
    snprintf(merged, sizeof(merged), "%s/merged", base);
    mkdir_p(lower); mkdir_p(upper); mkdir_p(work); mkdir_p(merged);
    snprintf(lowerfile, sizeof(lowerfile), "%s/hello.txt", lower);
    write_file(lowerfile, "");
    {
        FILE *f = fopen(lowerfile, "w");
        fputs("original", f);
        fclose(f);
    }
    printf("  lower/hello.txt = 'original'\n");

    /* lower/subdir + file, set up BEFORE mount, to test opaque-dir marking
     * via the natural overlayfs "delete a non-empty lower dir, recreate it"
     * sequence -- not a manual setxattr call. */
    {
        char lowersub[4096], lowersubfile[4096];
        snprintf(lowersub, sizeof(lowersub), "%s/subdir", lower);
        mkdir_p(lowersub);
        snprintf(lowersubfile, sizeof(lowersubfile), "%s/insidefile.txt", lowersub);
        FILE *f = fopen(lowersubfile, "w");
        fputs("lower-subdir-content", f);
        fclose(f);
        printf("  lower/subdir/insidefile.txt created for opaque-dir test\n");
    }

    step("mount overlay with userxattr (native mount() first, fuse-overlayfs fallback on EPERM/EINVAL)");
    {
        char opts[8192];
        snprintf(opts, sizeof(opts), "lowerdir=%s,upperdir=%s,workdir=%s,userxattr", lower, upper, work);
        int rc = mount("overlay", merged, "overlay", 0, opts);
        if (rc != 0) {
            int e = errno;
            printf("  native mount FAILED: %s (errno=%d)\n", strerror(e), e);
            if (e == EPERM || e == EINVAL) {
                printf("  falling back to fuse-overlayfs...\n");
                char cmd[8192];
                snprintf(cmd, sizeof(cmd), "fuse-overlayfs -o %s %s", opts, merged);
                int frc = system(cmd);
                if (frc != 0) { printf("  fuse-overlayfs FAILED (rc=%d)\n", frc); goto verdict; }
                printf("  fuse-overlayfs mount OK\n");
                g_mount_ok = 1;
            } else {
                printf("  unrelated errno, not falling back\n");
                goto verdict;
            }
        } else {
            printf("  native overlay mount OK\n");
            g_mount_ok = 1;
        }
    }

    step("prove copy-on-write: read merged (should be lower's content), write merged, confirm lower untouched");
    {
        char mergedfile[4096], buf[256] = {0};
        snprintf(mergedfile, sizeof(mergedfile), "%s/hello.txt", merged);
        FILE *f = fopen(mergedfile, "r");
        if (!f) { printf("  can't read merged/hello.txt: %s\n", strerror(errno)); goto verdict; }
        fgets(buf, sizeof(buf), f); fclose(f);
        printf("  merged/hello.txt (pre-write) = '%s'\n", buf);

        f = fopen(mergedfile, "w");
        fputs("modified", f);
        fclose(f);
        printf("  wrote 'modified' via merged view\n");

        char lowbuf[256] = {0};
        f = fopen(lowerfile, "r");
        fgets(lowbuf, sizeof(lowbuf), f); fclose(f);
        printf("  lower/hello.txt (after write through merged) = '%s'\n", lowbuf);

        char upperfile[4096], upbuf[256] = {0};
        snprintf(upperfile, sizeof(upperfile), "%s/hello.txt", upper);
        f = fopen(upperfile, "r");
        if (f) { fgets(upbuf, sizeof(upbuf), f); fclose(f); }
        printf("  upper/hello.txt (should hold the copied-up modified data) = '%s'\n", upbuf);

        if (strcmp(buf, "original") == 0 && strcmp(lowbuf, "original") == 0 && strcmp(upbuf, "modified") == 0) {
            printf("  --> COW confirmed: lower untouched, upper holds the delta.\n");
            g_cow_ok = 1;
        } else {
            printf("  --> COW NOT as expected.\n");
        }
    }

    step("manual setxattr comparison: trusted.overlay.opaque vs user.overlay.opaque, direct on a plain dir");
    {
        char testdir[4096];
        snprintf(testdir, sizeof(testdir), "%s/opaquetest", upper);
        mkdir_p(testdir);
        int rc_trusted = setxattr(testdir, "trusted.overlay.opaque", "y", 1, 0);
        printf("  setxattr(trusted.overlay.opaque) -> %s\n", rc_trusted == 0 ? "OK" : strerror(errno));
        int rc_user = setxattr(testdir, "user.overlay.opaque", "y", 1, 0);
        printf("  setxattr(user.overlay.opaque)    -> %s\n", rc_user == 0 ? "OK" : strerror(errno));
    }

    step("NATURAL opaque-dir marking: rm -rf merged/subdir (exists in lower), mkdir merged/subdir, inspect real upper/subdir xattrs");
    {
        char mergedsub[4096], uppersub[4096];
        snprintf(mergedsub, sizeof(mergedsub), "%s/subdir", merged);
        snprintf(uppersub, sizeof(uppersub), "%s/subdir", upper);

        char rmcmd[8192];
        snprintf(rmcmd, sizeof(rmcmd), "rm -rf '%s'", mergedsub);
        int rc = system(rmcmd);
        printf("  rm -rf merged/subdir -> rc=%d\n", rc);

        rc = mkdir(mergedsub, 0755);
        printf("  mkdir merged/subdir -> rc=%d (%s)\n", rc, rc == 0 ? "ok" : strerror(errno));

        char val[64];
        ssize_t n_trusted = getxattr(uppersub, "trusted.overlay.opaque", val, sizeof(val));
        printf("  real upper/subdir trusted.overlay.opaque -> %s\n",
               n_trusted > 0 ? "PRESENT" : strerror(errno));
        ssize_t n_user = getxattr(uppersub, "user.overlay.opaque", val, sizeof(val));
        printf("  real upper/subdir user.overlay.opaque    -> %s\n",
               n_user > 0 ? "PRESENT" : strerror(errno));

        if (n_trusted > 0 || n_user > 0) {
            g_xattr_ok = 1;
            printf("  --> kernel marks opaque dirs using the %s namespace under this mount config\n",
                   n_user > 0 ? "user.overlay.*" : "trusted.overlay.*");
        } else {
            printf("  --> NEITHER xattr present -- opaque marking not detected at all, needs investigation\n");
        }
    }

    step("unmount + kill/reap anchor, confirm anchor was alive right up to teardown");
    {
        int rc = umount2(merged, MNT_DETACH);
        printf("  umount2 rc=%d (%s)\n", rc, rc == 0 ? "ok" : strerror(errno));
        if (kill(anchor_pid, 0) == 0) {
            printf("  anchor (pid %d) still alive right before teardown -- good\n", anchor_pid);
            g_anchor_survives = 1;
        } else {
            printf("  anchor (pid %d) already gone: %s -- BAD, this would have broken any run() after the first\n", anchor_pid, strerror(errno));
        }
        kill(anchor_pid, SIGKILL);
        waitpid(anchor_pid, NULL, 0);
        printf("  anchor reaped\n");
    }

verdict:
    printf("\n===================== FEASIBILITY VERDICT =====================\n");
    printf("unshare(NEWUSER|NEWNS|NEWPID) succeeded : %s\n", g_unshare_ok ? "YES" : "NO");
    printf("uid_map/gid_map write succeeded         : %s%s\n", g_idmap_ok ? "YES" : "NO",
           g_idmap_ok ? "" : "  <-- if NO, agentbox setup / AppArmor profile is not yet effective");
    printf("overlay mount succeeded                 : %s\n", g_mount_ok ? "YES" : "NO");
    printf("copy-on-write behaves correctly          : %s\n", g_cow_ok ? "YES" : "NO");
    printf("trusted.overlay.opaque xattr settable    : %s\n", g_xattr_ok ? "YES" : "NO");
    printf("anchor process kept pidns alive          : %s\n", g_anchor_survives ? "YES" : "NO");
    printf("=================================================================\n");

    int all_ok = g_unshare_ok && g_idmap_ok && g_mount_ok && g_cow_ok && g_xattr_ok && g_anchor_survives;
    return all_ok ? 0 : 1;
}
