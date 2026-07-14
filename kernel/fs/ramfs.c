/* kernel/ramfs.c — RAM filesystem
 *
 * Flat directory: up to RAMFS_MAX_FILES files, each up to RAMFS_MAX_SIZE bytes.
 * Each file embeds its own vfs_node_t so vfs_finddir can return a stable pointer.
 */
#include "../types.h"
#include "ramfs.h"
#include "../mm/vmm.h"
#include "../klibc.h"
#include "../log.h"

/* One file slot
 * BUG FIX (heap exhausted): data[] was embedded (65536 bytes each × 128 slots
 * = 8 MB per RAMFS instance).  4 instances consumed > 32 MB heap before the
 * shadow buffer could be allocated.  Now data is a pointer; memory is only
 * allocated on the first write, so empty volumes cost ~4 KB instead of 8 MB. */
typedef struct {
    vfs_node_t  node;               /* embedded — priv points back here */
    uint8_t    *data;               /* lazy-allocated on first write    */
    uint32_t    size;
    int         used;
} rfile_t;

/* Filesystem instance */
typedef struct {
    rfile_t    files[RAMFS_MAX_FILES];
    vfs_node_t root_node;
    int        count;
} rfs_t;

static vfs_ops_t file_ops;
static vfs_ops_t dir_ops;

/* ---- file ops ------------------------------------------------------------ */

static int rfs_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
    rfile_t *f = (rfile_t *)n->priv;
    if (!f->data || off >= f->size) return 0;
    if (off + len > f->size) len = f->size - off;
    memcpy(buf, f->data + off, len);
    return (int)len;
}

static int rfs_write(vfs_node_t *n, uint32_t off, uint32_t len,
                     const uint8_t *buf) {
    rfile_t *f = (rfile_t *)n->priv;
    if (off + len > RAMFS_MAX_SIZE) {
        if (off >= RAMFS_MAX_SIZE) return -1;
        len = RAMFS_MAX_SIZE - off;
    }
    /* BUG FIX: lazy-allocate backing storage on first write */
    if (!f->data) {
        f->data = (uint8_t *)kzalloc(RAMFS_MAX_SIZE);
        if (!f->data) { kerror("RAMFS: data alloc failed\n"); return -1; }
    }
    memcpy(f->data + off, buf, len);
    if (off + len > f->size) f->size = off + len;
    n->size = f->size;
    return (int)len;
}

/* ---- dir ops ------------------------------------------------------------- */

static int rfs_readdir(vfs_node_t *n, uint32_t idx, char *out, size_t maxlen) {
    rfs_t   *fs   = (rfs_t *)n->priv;
    uint32_t seen = 0;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!fs->files[i].used) continue;
        if (seen == idx) {
            strncpy(out, fs->files[i].node.name, maxlen - 1);
            out[maxlen - 1] = '\0';
            return 0;
        }
        seen++;
    }
    return -1; /* end of directory */
}

static vfs_node_t *rfs_finddir(vfs_node_t *n, const char *name) {
    rfs_t *fs = (rfs_t *)n->priv;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (fs->files[i].used &&
            strcmp(fs->files[i].node.name, name) == 0)
            return &fs->files[i].node;
    }
    return NULL;
}

/* ---- public API ----------------------------------------------------------- */

static rfs_t *g_rfs;

vfs_node_t *ramfs_init(void) {
    g_rfs = (rfs_t *)kzalloc(sizeof(rfs_t));
    if (!g_rfs) { kerror("RAMFS: alloc failed\n"); return NULL; }

    file_ops.read    = rfs_read;
    file_ops.write   = rfs_write;
    file_ops.readdir = NULL;
    file_ops.finddir = NULL;

    dir_ops.read    = NULL;
    dir_ops.write   = NULL;
    dir_ops.readdir = rfs_readdir;
    dir_ops.finddir = rfs_finddir;

    vfs_node_t *root = &g_rfs->root_node;
    strncpy(root->name, "ramfs", VFS_NAME_MAX - 1);
    root->type = VFS_TYPE_DIR;
    root->size = 0;
    root->ops  = &dir_ops;
    root->priv = g_rfs;

    kinfo("RAMFS: initialised (%d file slots, %d bytes each)\n",
          RAMFS_MAX_FILES, RAMFS_MAX_SIZE);
    return root;
}

int ramfs_create(vfs_node_t *root, const char *name) {
    rfs_t *fs = (rfs_t *)root->priv;
    if (rfs_finddir(root, name)) return -1; /* already exists */
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!fs->files[i].used) {
            memset(&fs->files[i], 0, sizeof(rfile_t));
            strncpy(fs->files[i].node.name, name, VFS_NAME_MAX - 1);
            fs->files[i].node.type = VFS_TYPE_FILE;
            fs->files[i].node.size = 0;
            fs->files[i].node.ops  = &file_ops;
            fs->files[i].node.priv = &fs->files[i];
            fs->files[i].used      = 1;
            fs->count++;
            return 0;
        }
    }
    return -1; /* table full */
}

int ramfs_delete(vfs_node_t *root, const char *name) {
    rfs_t *fs = (rfs_t *)root->priv;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (fs->files[i].used &&
            strcmp(fs->files[i].node.name, name) == 0) {
            kfree(fs->files[i].data);   /* BUG FIX: release lazy buffer */
            memset(&fs->files[i], 0, sizeof(rfile_t));
            fs->count--;
            return 0;
        }
    }
    return -1;
}

/* ramfs_new — create an additional independent RAMFS instance.
 * Used by init.c to mount /storage on a separate RAMFS volume.
 * Does NOT touch g_rfs or print an init message. */
vfs_node_t *ramfs_new(const char *label) {
    rfs_t *fs = (rfs_t *)kzalloc(sizeof(rfs_t));
    if (!fs) { kerror("RAMFS: ramfs_new alloc failed\n"); return NULL; }

    vfs_node_t *root = &fs->root_node;
    strncpy(root->name, label, VFS_NAME_MAX - 1);
    root->type = VFS_TYPE_DIR;
    root->size = 0;
    root->ops  = &dir_ops;
    root->priv = fs;

    kinfo("RAMFS: new volume '%s' (%d slots)\n", label, RAMFS_MAX_FILES);
    return root;
}
