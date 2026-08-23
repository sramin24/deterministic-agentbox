#define _GNU_SOURCE
#include "agentbox.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

/* ---- mount / unmount ---------------------------------------------------- */

agentbox_errcode_t agentbox_cow_mount(const char *lowerdir_stack, const char *upperdir,
                                       const char *workdir, const char *merged) {
    char opts[PATH_MAX * 4];
    int n = snprintf(opts, sizeof(opts), "lowerdir=%s,upperdir=%s,workdir=%s,userxattr",
                      lowerdir_stack, upperdir, workdir);
    if (n < 0 || (size_t)n >= sizeof(opts)) return AGENTBOX_ERR_INVALID_ARGUMENT;

    int rc = mount("overlay", merged, "overlay", 0, opts);
    if (rc == 0) return AGENTBOX_OK;

    int mount_errno = errno;
    if (mount_errno != EPERM && mount_errno != EINVAL) {
        /* Anything else (e.g. ENOENT for a missing dir) is a real problem
         * of its own; falling back to fuse-overlayfs would just fail again
         * for an unrelated reason and hide the actual cause. */
        return AGENTBOX_ERR_MOUNT_FAILED;
    }

    pid_t pid = fork();
    if (pid < 0) return AGENTBOX_ERR_MOUNT_FAILED;
    if (pid == 0) {
        execlp("fuse-overlayfs", "fuse-overlayfs", "-o", opts, merged, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return AGENTBOX_ERR_MOUNT_FAILED;
    }
    return AGENTBOX_OK;
}

agentbox_errcode_t agentbox_cow_unmount(const char *merged) {
    int rc = umount2(merged, MNT_DETACH);
    if (rc == 0 || errno == EINVAL) return AGENTBOX_OK;
    return AGENTBOX_ERR_MOUNT_FAILED;
}

/* ---- purge --------------------------------------------------------------- */

static int purge_visit(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    if (typeflag == FTW_DP || typeflag == FTW_D) {
        if (rmdir(path) != 0 && errno != ENOENT) return -1;
    } else {
        if (unlink(path) != 0 && errno != ENOENT) return -1;
    }
    return 0;
}

agentbox_errcode_t agentbox_cow_purge(const char *dir) {
    struct stat st;
    if (lstat(dir, &st) != 0) {
        if (errno == ENOENT) return AGENTBOX_OK;
        return AGENTBOX_ERR_IO;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(dir) != 0 && errno != ENOENT) return AGENTBOX_ERR_IO;
        return AGENTBOX_OK;
    }
    /* FTW_DEPTH: children before their parent, so each rmdir() sees an
     * already-empty directory. FTW_PHYS: never follow symlinks into
     * something outside the tree being purged. */
    if (nftw(dir, purge_visit, 64, FTW_DEPTH | FTW_PHYS) != 0) {
        return AGENTBOX_ERR_IO;
    }
    return AGENTBOX_OK;
}

/* ---- shared helpers for commit_merge and diff ---------------------------- */

/* trusted.overlay.opaque: what a privileged (non-userns) overlay mount uses.
 * user.overlay.opaque: what the `userxattr` mount option makes the kernel
 * use instead, which is the only one settable/readable from inside a
 * nested user namespace (see spike/ns-overlay-feasibility -- trusted.*
 * requires CAP_SYS_ADMIN in the namespace owning the filesystem's
 * superblock, which a nested namespace never has). Checking both makes
 * this correct regardless of which mount mode produced the layer. */
static int dir_is_opaque(const char *path) {
    char val[8];
    if (getxattr(path, "user.overlay.opaque", val, sizeof(val)) > 0) return 1;
    if (getxattr(path, "trusted.overlay.opaque", val, sizeof(val)) > 0) return 1;
    return 0;
}

static int is_whiteout(const struct stat *sb) {
    return S_ISCHR(sb->st_mode) && major(sb->st_rdev) == 0 && minor(sb->st_rdev) == 0;
}

static void mkdir_p(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, mode);
}

static agentbox_errcode_t copy_regular_file(const char *src, const char *dst, mode_t mode) {
    int in = open(src, O_RDONLY);
    if (in < 0) return AGENTBOX_ERR_IO;
    unlink(dst);
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out < 0) { close(in); return AGENTBOX_ERR_IO; }
    char buf[65536];
    ssize_t n;
    agentbox_errcode_t result = AGENTBOX_OK;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) { result = AGENTBOX_ERR_IO; goto done; }
            off += w;
        }
    }
    if (n < 0) result = AGENTBOX_ERR_IO;
done:
    close(in);
    close(out);
    return result;
}

/* ---- commit_merge ---------------------------------------------------------
 *
 * Walked one layer directory at a time, oldest to newest, in PRE-order
 * (parent before children -- unlike purge, which needs post-order). That
 * ordering is what makes the opaque-directory reset correct with a single
 * pass: when an opaque dir is hit, the target subtree is wiped and
 * recreated empty right then, and the entries this same layer walk visits
 * next (its children) repopulate it -- no second pass needed.
 */

static const char *g_merge_layer_root;
static const char *g_merge_target_root;
static agentbox_errcode_t g_merge_error;

static const char *rel_from_layer(const char *path) {
    size_t layer_len = strlen(g_merge_layer_root);
    if (strncmp(path, g_merge_layer_root, layer_len) != 0) return NULL;
    const char *rel = path + layer_len;
    while (*rel == '/') rel++;
    return rel;
}

static int merge_visit(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)ftwbuf;
    if (typeflag == FTW_DNR || typeflag == FTW_NS) {
        g_merge_error = AGENTBOX_ERR_IO;
        return -1;
    }
    const char *rel = rel_from_layer(path);
    if (rel == NULL || rel[0] == '\0') return 0; /* the layer root itself */

    char target[PATH_MAX];
    snprintf(target, sizeof(target), "%s/%s", g_merge_target_root, rel);

    if (is_whiteout(sb)) {
        agentbox_errcode_t rc = agentbox_cow_purge(target);
        if (rc != AGENTBOX_OK) { g_merge_error = rc; return -1; }
        return 0;
    }

    if (S_ISDIR(sb->st_mode)) {
        if (dir_is_opaque(path)) {
            agentbox_errcode_t rc = agentbox_cow_purge(target);
            if (rc != AGENTBOX_OK) { g_merge_error = rc; return -1; }
        }
        mkdir_p(target, sb->st_mode & 07777);
        return 0;
    }

    if (S_ISLNK(sb->st_mode)) {
        char link_target[PATH_MAX];
        ssize_t n = readlink(path, link_target, sizeof(link_target) - 1);
        if (n < 0) { g_merge_error = AGENTBOX_ERR_IO; return -1; }
        link_target[n] = 0;
        unlink(target);
        if (symlink(link_target, target) != 0) { g_merge_error = AGENTBOX_ERR_IO; return -1; }
        return 0;
    }

    if (S_ISREG(sb->st_mode)) {
        agentbox_errcode_t rc = copy_regular_file(path, target, sb->st_mode & 07777);
        if (rc != AGENTBOX_OK) { g_merge_error = rc; return -1; }
        return 0;
    }

    /* Sockets/fifos/other device nodes: not meaningful contents of an
     * agent's workspace commit; skip rather than fail the whole commit. */
    return 0;
}

agentbox_errcode_t agentbox_cow_commit_merge(const char *const *layer_dirs, size_t count,
                                              const char *target_dir) {
    for (size_t i = 0; i < count; i++) {
        g_merge_layer_root = layer_dirs[i];
        g_merge_target_root = target_dir;
        g_merge_error = AGENTBOX_OK;
        int rc = nftw(layer_dirs[i], merge_visit, 64, FTW_PHYS);
        if (rc != 0) {
            return g_merge_error != AGENTBOX_OK ? g_merge_error : AGENTBOX_ERR_IO;
        }
    }
    return AGENTBOX_OK;
}

/* ---- diff -----------------------------------------------------------------
 *
 * Same oldest-to-newest walk, but instead of writing into a target it
 * records one "M <relpath>" or "D <relpath>" entry per path, deduplicated
 * by relpath with later layers overwriting earlier ones. A plain growable
 * array with linear-scan dedup: correct, and simple enough to read against
 * checkpoint counts bounded at AGENTBOX_MAX_CHECKPOINTS -- not the data
 * structure to reach for if this needs to scale to huge trees later.
 */

typedef struct {
    char *relpath;
    char status; /* 'M' or 'D' */
} diff_entry_t;

static diff_entry_t *g_diff_entries;
static size_t g_diff_count;
static size_t g_diff_cap;
static agentbox_errcode_t g_diff_error;

static void diff_set(const char *relpath, char status) {
    for (size_t i = 0; i < g_diff_count; i++) {
        if (strcmp(g_diff_entries[i].relpath, relpath) == 0) {
            g_diff_entries[i].status = status;
            return;
        }
    }
    if (g_diff_count == g_diff_cap) {
        size_t new_cap = g_diff_cap == 0 ? 64 : g_diff_cap * 2;
        diff_entry_t *grown = realloc(g_diff_entries, new_cap * sizeof(diff_entry_t));
        if (!grown) { g_diff_error = AGENTBOX_ERR_IO; return; }
        g_diff_entries = grown;
        g_diff_cap = new_cap;
    }
    g_diff_entries[g_diff_count].relpath = strdup(relpath);
    g_diff_entries[g_diff_count].status = status;
    g_diff_count++;
}

static int diff_visit(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)ftwbuf;
    if (typeflag == FTW_DNR || typeflag == FTW_NS) {
        g_diff_error = AGENTBOX_ERR_IO;
        return -1;
    }
    const char *rel = rel_from_layer(path);
    if (rel == NULL || rel[0] == '\0') return 0;

    if (is_whiteout(sb)) {
        diff_set(rel, 'D');
    } else if (!S_ISDIR(sb->st_mode)) {
        /* Directories themselves aren't reported; their changed contents
         * are what show up as individual M/D entries. */
        diff_set(rel, 'M');
    }
    if (g_diff_error != AGENTBOX_OK) return -1;
    return 0;
}

agentbox_errcode_t agentbox_cow_diff(const char *const *layer_dirs, size_t count,
                                      char ***out_paths, size_t *out_count) {
    g_diff_entries = NULL;
    g_diff_count = 0;
    g_diff_cap = 0;
    g_diff_error = AGENTBOX_OK;

    for (size_t i = 0; i < count; i++) {
        g_merge_layer_root = layer_dirs[i];
        int rc = nftw(layer_dirs[i], diff_visit, 64, FTW_PHYS);
        if (rc != 0 || g_diff_error != AGENTBOX_OK) {
            for (size_t j = 0; j < g_diff_count; j++) free(g_diff_entries[j].relpath);
            free(g_diff_entries);
            return g_diff_error != AGENTBOX_OK ? g_diff_error : AGENTBOX_ERR_IO;
        }
    }

    char **paths = calloc(g_diff_count ? g_diff_count : 1, sizeof(char *));
    if (!paths) {
        for (size_t j = 0; j < g_diff_count; j++) free(g_diff_entries[j].relpath);
        free(g_diff_entries);
        return AGENTBOX_ERR_IO;
    }
    for (size_t i = 0; i < g_diff_count; i++) {
        char line[PATH_MAX + 4];
        snprintf(line, sizeof(line), "%c %s", g_diff_entries[i].status, g_diff_entries[i].relpath);
        paths[i] = strdup(line);
        free(g_diff_entries[i].relpath);
    }
    free(g_diff_entries);
    *out_paths = paths;
    *out_count = g_diff_count;
    return AGENTBOX_OK;
}

void agentbox_free_string_list(char **paths, size_t count) {
    if (!paths) return;
    for (size_t i = 0; i < count; i++) free(paths[i]);
    free(paths);
}
