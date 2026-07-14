/* kernel/linux/include-uapi/asm-generic/mman-common.h */
#ifndef _ASM_GENERIC_MMAN_COMMON_H
#define _ASM_GENERIC_MMAN_COMMON_H

/* mmap prot flags */
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4
#define PROT_NONE       0x0

/* mmap flags */
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_ANON        MAP_ANONYMOUS
#define MAP_GROWSDOWN   0x0100
#define MAP_DENYWRITE   0x0800
#define MAP_EXECUTABLE  0x1000
#define MAP_LOCKED      0x2000
#define MAP_NORESERVE   0x4000
#define MAP_POPULATE    0x8000
#define MAP_STACK       0x20000

/* mmap error return */
#define MAP_FAILED      ((void *)-1)

/* mremap flags */
#define MREMAP_MAYMOVE  1
#define MREMAP_FIXED    2

/* msync flags */
#define MS_ASYNC        1
#define MS_INVALIDATE   2
#define MS_SYNC         4

#endif /* _ASM_GENERIC_MMAN_COMMON_H */
