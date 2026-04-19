/* kernel/vfs.c — Virtual Filesystem Layer (v1.2 — hardened)
 *
 * Hardening changes (Phase X):
 *  - vfs_path_sanitize(): normalises path in-place, collapses double slashes,
 *    strips trailing slash, blocks ".." components (returns -1 if found).
 *  - vfs_open() sanitizes before resolving — syscall paths from user space
 *    cannot escape the mounted tree via "/../.." traversal.
 *  - Null-pointer guards on all public ops.
 *
 * Mount resolution: longest-prefix-first ensures /storage/main beats /storage.
 */
#include "../types.h"
#include "vfs.h"
#include "../klibc.h"
#include "../log.h"

#define MAX_MOUNTS 16

typedef struct { char path[64]; vfs_node_t *root; } mount_t;
static mount_t mounts[MAX_MOUNTS];
static int     nmounts;

/* ── Path sanitisation ────────────────────────────────────────────────── *
 * Writes a cleaned version of `src` into `dst` (max `dstsz` bytes).
 * Rules:
 *   1. Must start with '/'
 *   2. Collapses consecutive '/' into one
 *   3. Removes trailing '/' (except root "/")
 *   4. Strips "." components silently
 *   5. REJECTS ".." components — returns -1 (path traversal attempt)
 *   6. Rejects NUL path or empty path
 *
 * Returns 0 on success, -1 on traversal attempt or malformed input.
 * ──────────────────────────────────────────────────────────────────────── */
int vfs_path_sanitize(const char *src, char *dst, size_t dstsz) {
    if (!src || !dst || dstsz < 2) return -1;
    if (src[0] != '/') return -1;

    size_t di = 0;
    dst[di++] = '/';

    const char *p = src + 1;
    while (*p) {
        /* Skip redundant slashes */
        while (*p == '/') p++;
        if (!*p) break;

        /* Extract component */
        const char *comp_start = p;
        size_t comp_len = 0;
        while (*p && *p != '/') { p++; comp_len++; }

        if (comp_len == 1 && comp_start[0] == '.') {
            continue;   /* "." — skip silently */
        }
        if (comp_len == 2 && comp_start[0] == '.' && comp_start[1] == '.') {
            kwarn("VFS: path traversal attempt blocked: %s\n", src);
            return -1;  /* ".." — REJECTED */
        }

        /* Append separator (not before root) */
        if (di > 1) {
            if (di >= dstsz - 1) return -1;   /* overflow */
            dst[di++] = '/';
        }

        /* Append component */
        if (di + comp_len >= dstsz) return -1;
        memcpy(dst + di, comp_start, comp_len);
        di += comp_len;
    }

    dst[di] = '\0';
    return 0;
}

int vfs_mount(const char *path, vfs_node_t *root) {
    if (!path || !root) { kerror("VFS: mount: NULL path or root\n"); return -1; }
    if (nmounts >= MAX_MOUNTS) { kerror("VFS: mount table full\n"); return -1; }
    strncpy(mounts[nmounts].path, path, 63);
    mounts[nmounts].path[63] = '\0';
    mounts[nmounts].root = root;
    nmounts++;
    kinfo("VFS: mounted '%s' at '%s'\n", root->name, path);
    return 0;
}

/* Recursively walk remaining path components under a mount root. */
static vfs_node_t *vfs_walk(vfs_node_t *start, const char *path) {
    if (!start || !path || path[0] == '\0') return start;

    char comp[VFS_NAME_MAX];
    vfs_node_t *cur = start;

    while (*path) {
        while (*path == '/') path++;
        if (*path == '\0') break;

        int i = 0;
        while (*path && *path != '/' && i < VFS_NAME_MAX - 1)
            comp[i++] = *path++;
        comp[i] = '\0';
        if (i == 0) break;

        /* "." and ".." should have been stripped by vfs_path_sanitize;
         * guard here as defence-in-depth. */
        if (strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0) {
            kwarn("VFS: '..' in walk path — blocked\n");
            return NULL;
        }

        cur = vfs_finddir(cur, comp);
        if (!cur) return NULL;
    }
    return cur;
}

vfs_node_t *vfs_open(const char *path) {
    if (!path || path[0] != '/') return NULL;

    /* Sanitize first — reject traversal, normalise slashes */
    char clean[128];
    if (vfs_path_sanitize(path, clean, sizeof(clean)) != 0) return NULL;

    int best_len = -1;
    int best_idx = -1;

    for (int i = 0; i < nmounts; i++) {
        const char *mp    = mounts[i].path;
        int         mplen = (int)strlen(mp);
        if (mplen <= best_len) continue;
        if (strncmp(clean, mp, (size_t)mplen) != 0) continue;
        char next = clean[mplen];
        if (next != '\0' && next != '/') continue;
        best_len = mplen;
        best_idx = i;
    }

    if (best_idx < 0) return NULL;

    const char *rest = clean + best_len;
    while (*rest == '/') rest++;

    if (*rest == '\0') return mounts[best_idx].root;
    return vfs_walk(mounts[best_idx].root, rest);
}

int vfs_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
    if (!n || !n->ops || !n->ops->read) return -1;
    return n->ops->read(n, off, len, buf);
}

int vfs_write(vfs_node_t *n, uint32_t off, uint32_t len, const uint8_t *buf) {
    if (!n || !n->ops || !n->ops->write) return -1;
    return n->ops->write(n, off, len, buf);
}

int vfs_readdir(vfs_node_t *n, uint32_t idx, char *out, size_t maxlen) {
    if (!n || !n->ops || !n->ops->readdir) return -1;
    return n->ops->readdir(n, idx, out, maxlen);
}

vfs_node_t *vfs_finddir(vfs_node_t *n, const char *name) {
    if (!n || !n->ops || !n->ops->finddir) return NULL;
    return n->ops->finddir(n, name);
}

