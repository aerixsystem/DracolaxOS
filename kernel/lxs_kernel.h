/* kernel/lxs_kernel.h — Kernel-side LXScript execution API */
#ifndef LXS_KERNEL_H
#define LXS_KERNEL_H
#include "types.h"
#include "fs/vfs.h"

/* Execute a NUL-terminated LXScript source string. Returns 0 on success. */
int lxs_kernel_exec(const char *source);

/* Execute an LXScript file read from the given VFS node. */
int lxs_kernel_exec_file(vfs_node_t *node);

#endif
