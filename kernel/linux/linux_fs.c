/* kernel/linux/linux_fs.c
 *
 * Linux filesystem ABI layer.
 * Translates Linux open() flags and paths to DracolaxOS VFS calls.
 */
#include "../types.h"
#include "../fs/vfs.h"
#include "../fs/ramfs.h"
#include "../klibc.h"
#include "../sched/task.h"
#include "../log.h"
#include "linux_fcntl.h"
#include "linux_types.h"
#include "include-uapi/asm-generic/errno.h"

/* Translate Linux O_ flags → we just use the same numeric values since
 * our VFS doesn't use flags yet — stored for future use. */
int lx_flags_to_draco(int linux_flags) {
    return linux_flags; /* values are identical in our minimal VFS */
}

/*
 * lx_path_resolve — find a VFS node from an absolute or relative path.
 *
 * Supports:
 *   /ramfs/<name>        → RAMFS files
 *   /proc/...            → procfs (handled by procfs.c)
 *   /dev/...             → device nodes (stub)
 *   /storage/...         → storage VFS (stub)
 *
 * Returns a vfs_node_t* or NULL if not found.
 */
vfs_node_t *lx_path_resolve(const char *path) {
    if (!path) return NULL;

    /* Absolute paths under /ramfs */
    if (strncmp(path, "/ramfs/", 7) == 0) {
        extern vfs_node_t *ramfs_root;
        if (!ramfs_root) return NULL;
        return vfs_finddir(ramfs_root, path + 7);
    }

    /* Try the VFS mount table first */
    vfs_node_t *n = vfs_open(path);
    if (n) return n;

    return NULL;
}

/*
 * lx_fd_alloc — find the lowest free fd slot in the current task.
 * Returns fd number or -EMFILE if table is full.
 */
int lx_fd_alloc(struct task *t, vfs_node_t *node) {
    for (int i = 3; i < TASK_FD_MAX; i++) {
        if (!t->fd_table[i]) {
            t->fd_table[i] = node;
            return i;
        }
    }
    return -EMFILE;
}

/*
 * lx_fd_get — return the VFS node for an fd.
 * fd 0/1/2 → NULL (handled as stdin/stdout/stderr by callers).
 */
vfs_node_t *lx_fd_get(struct task *t, int fd) {
    if (fd < 0 || fd >= TASK_FD_MAX) return NULL;
    return t->fd_table[fd]; /* may be NULL for 0/1/2 */
}

/* Free an fd slot */
void lx_fd_free(struct task *t, int fd) {
    if (fd >= 0 && fd < TASK_FD_MAX)
        t->fd_table[fd] = NULL;
}

/* Copy fd table from parent to child (used by fork) */
void lx_fd_table_copy(struct task *dst, const struct task *src) {
    for (int i = 0; i < TASK_FD_MAX; i++)
        dst->fd_table[i] = src->fd_table[i];
}

/*
 * lx_fill_stat — fill an lx_stat_t from a vfs_node_t.
 */
void lx_fill_stat(lx_stat_t *st, vfs_node_t *node) {
    memset(st, 0, sizeof(*st));
    st->st_ino    = 1;
    st->st_nlink  = 1;
    st->st_size   = (lx_off_t)node->size;
    st->st_blksize = 512;
    st->st_blocks  = (node->size + 511) / 512;
    if (node->type == VFS_TYPE_DIR) {
        st->st_mode = 0040755; /* directory */
    } else {
        st->st_mode = 0100644; /* regular file */
    }
}
