/* kernel/procfs.h — Minimal /proc filesystem */
#ifndef PROCFS_H
#define PROCFS_H

#include "vfs.h"

/* Initialise procfs and mount at /proc */
void procfs_init(void);

/* Return the procfs root node */
vfs_node_t *procfs_root(void);

#endif /* PROCFS_H */
