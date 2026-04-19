/* kernel/vfs.h — Virtual Filesystem Interface */
#ifndef VFS_H
#include "../types.h"
#define VFS_H


#define VFS_NAME_MAX  64
#define VFS_TYPE_FILE 0
#define VFS_TYPE_DIR  1

struct vfs_node;

typedef struct vfs_ops {
    int    (*read)   (struct vfs_node *, uint32_t off, uint32_t len, uint8_t *buf);
    int    (*write)  (struct vfs_node *, uint32_t off, uint32_t len, const uint8_t *buf);
    int    (*readdir)(struct vfs_node *, uint32_t idx, char *name_out, size_t name_max);
    struct vfs_node *(*finddir)(struct vfs_node *, const char *name);
} vfs_ops_t;

typedef struct vfs_node {
    char        name[VFS_NAME_MAX];
    uint32_t    type;    /* VFS_TYPE_FILE or VFS_TYPE_DIR */
    uint32_t    size;    /* byte size for files            */
    vfs_ops_t  *ops;
    void       *priv;    /* filesystem-private data        */
} vfs_node_t;

/* Mount a filesystem root at a path (simple flat mount table) */
int  vfs_mount(const char *path, vfs_node_t *root);

/* Sanitize and normalize a VFS path.
 * Collapses double slashes, strips ".", REJECTS ".." (returns -1).
 * Always call this on paths received from user space before vfs_open(). */
int  vfs_path_sanitize(const char *src, char *dst, size_t dstsz);

/* Resolve a path to a node; returns NULL if not found.
 * Uses longest-prefix-first mount matching + recursive path walking. */
vfs_node_t *vfs_open(const char *path);

/* Thin wrappers that call through ops */
int  vfs_read   (vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf);
int  vfs_write  (vfs_node_t *n, uint32_t off, uint32_t len, const uint8_t *buf);
int  vfs_readdir(vfs_node_t *n, uint32_t idx, char *name_out, size_t name_max);
vfs_node_t *vfs_finddir(vfs_node_t *n, const char *name);

#endif /* VFS_H */
