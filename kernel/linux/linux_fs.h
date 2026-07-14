/* kernel/linux/linux_fs.h */
#ifndef LINUX_FS_H
#define LINUX_FS_H

#include "../fs/vfs.h"
#include "../sched/task.h"
#include "linux_types.h"

vfs_node_t *lx_path_resolve(const char *path);
int         lx_fd_alloc(struct task *t, vfs_node_t *node);
vfs_node_t *lx_fd_get(struct task *t, int fd);
void        lx_fd_free(struct task *t, int fd);
void        lx_fd_table_copy(struct task *dst, const struct task *src);
void        lx_fill_stat(lx_stat_t *st, vfs_node_t *node);

#endif /* LINUX_FS_H */
