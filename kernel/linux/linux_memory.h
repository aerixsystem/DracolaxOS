/* kernel/linux/linux_memory.h — mmap/brk/munmap interface */
#ifndef LINUX_MEMORY_H
#define LINUX_MEMORY_H

#include "../types.h"
#include "include-uapi/asm-generic/mman-common.h"

/* mmap backing for Linux tasks */
uint32_t lx_mmap(uint32_t addr, uint32_t length, int prot,
                 int flags, int fd, uint32_t offset);
int      lx_munmap(uint32_t addr, uint32_t length);
void     lx_munmap_all(void);  /* free all mappings for current task on exit */
uint32_t lx_brk(uint32_t new_brk);

#endif /* LINUX_MEMORY_H */
