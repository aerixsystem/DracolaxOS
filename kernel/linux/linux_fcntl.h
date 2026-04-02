/* kernel/linux/linux_fcntl.h — Linux open() flags → VFS flags */
#ifndef LINUX_FCNTL_H
#define LINUX_FCNTL_H

#include "include-uapi/asm-generic/fcntl.h"

/* Translate Linux O_ flags to Draco VFS-understood values.
 * For now VFS open is simple; we map the common set. */
#define LX_O_RDONLY   O_RDONLY
#define LX_O_WRONLY   O_WRONLY
#define LX_O_RDWR     O_RDWR
#define LX_O_CREAT    O_CREAT
#define LX_O_TRUNC    O_TRUNC
#define LX_O_APPEND   O_APPEND

int lx_flags_to_draco(int linux_flags);

#endif /* LINUX_FCNTL_H */
