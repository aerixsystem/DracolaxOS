/* kernel/linux/linux_errno.h — Linux→Draco errno translation */
#ifndef LINUX_ERRNO_H
#define LINUX_ERRNO_H

#include "include-uapi/asm-generic/errno.h"

/*
 * Convert a negative Linux errno value to a positive DracolaxOS error code.
 * For now DracolaxOS uses -1 for generic errors; callers check <0.
 * This function just preserves the value so musl/libc can interpret it.
 */
static inline int lx_errno(int e) { return e; }

#endif /* LINUX_ERRNO_H */
