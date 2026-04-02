/* kernel/ramfs.h — RAM filesystem */
#ifndef RAMFS_H
#define RAMFS_H

#include "vfs.h"

#define RAMFS_MAX_FILES  128
#define RAMFS_MAX_SIZE   65536  /* max bytes per file (was 8192 — too small) */

/* Initialise RAMFS and return the root directory node */
vfs_node_t *ramfs_init(void);

/* Create a file; returns 0 on success */
int ramfs_create(vfs_node_t *root, const char *name);

/* Delete a file; returns 0 on success */
int ramfs_delete(vfs_node_t *root, const char *name);

/* Create an additional independent RAMFS volume (for /storage etc.) */
vfs_node_t *ramfs_new(const char *label);

#endif /* RAMFS_H */
