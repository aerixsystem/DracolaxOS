/* kernel/vfs.c — Virtual Filesystem Layer (improved)
 *
 * Improvements over v1:
 *  - Longest-prefix-first mount resolution (fixes /storage vs / overlap)
 *  - Recursive path traversal (splits on '/' and walks one level at a time)
 *  - strlcpy/strlcat helpers added to klibc.h
 *  - vfs_open returns mount root for exact mount-point paths
 *  - path normalisation: collapses // and ignores trailing /
 */
#include "../types.h"
#include "vfs.h"
#include "../klibc.h"
#include "../log.h"

#define MAX_MOUNTS 16   /* increased from 8 — storage tree needs more */

typedef struct { char path[64]; vfs_node_t *root; } mount_t;
static mount_t mounts[MAX_MOUNTS];
static int     nmounts;

int vfs_mount(const char *path, vfs_node_t *root) {
    if (nmounts >= MAX_MOUNTS) { kerror("VFS: mount table full\n"); return -1; }
    strncpy(mounts[nmounts].path, path, 63);
    mounts[nmounts].path[63] = '\0';
    mounts[nmounts].root = root;
    nmounts++;
    kinfo("VFS: mounted '%s' at '%s'\n", root ? root->name : "?", path);
    return 0;
}

/* Recursively walk a path like "a/b/c" one component at a time.
 * Each component calls finddir on the current node. */
static vfs_node_t *vfs_walk(vfs_node_t *start, const char *path) {
    if (!start || !path || path[0] == '\0') return start;

    char comp[VFS_NAME_MAX];
    vfs_node_t *cur = start;

    while (*path) {
        /* Skip leading slashes */
        while (*path == '/') path++;
        if (*path == '\0') break;

        /* Extract next path component */
        int i = 0;
        while (*path && *path != '/' && i < VFS_NAME_MAX - 1)
            comp[i++] = *path++;
        comp[i] = '\0';
        if (i == 0) break;

        /* Handle . and .. */
        if (strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0) { cur = start; continue; } /* simplified */

        cur = vfs_finddir(cur, comp);
        if (!cur) return NULL;
    }
    return cur;
}

/* Resolve path against mount table — longest-prefix-first match.
 * Mount table is searched for the longest matching prefix, which ensures
 * /storage/main matches before /storage which matches before /. */
vfs_node_t *vfs_open(const char *path) {
    if (!path || path[0] != '/') return NULL;

    int   best_len  = -1;
    int   best_idx  = -1;

    /* Find the mount with the longest matching prefix */
    for (int i = 0; i < nmounts; i++) {
        const char *mp    = mounts[i].path;
        int         mplen = (int)strlen(mp);
        if (mplen <= best_len) continue;

        if (strncmp(path, mp, (size_t)mplen) != 0) continue;

        /* Ensure match is at a path boundary */
        char next = path[mplen];
        if (next != '\0' && next != '/') continue;

        best_len = mplen;
        best_idx = i;
    }

    if (best_idx < 0) return NULL;

    const char *rest = path + best_len;
    while (*rest == '/') rest++;

    if (*rest == '\0') return mounts[best_idx].root;   /* exact mount point */

    /* Walk remaining path components recursively */
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
