/* kernel/linux/linux_memory.c
 *
 * mmap / munmap / brk implementations for Linux ABI tasks.
 * Backed by the kernel VMM and PMM.
 *
 * munmap now tracks mapped regions per task (up to LX_MMAP_MAX regions)
 * and calls kfree() on the matching allocation, preventing the heap leak
 * that occurred when long-running musl-linked programs repeatedly mmap'd.
 */
#include "../types.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../mm/paging.h"
#include "../log.h"
#include "../sched/sched.h"
#include "linux_memory.h"
#include "include-uapi/asm-generic/errno.h"
#include "../klibc.h"

/* Per-task mmap region table — stored inline in the task extra_data region.
 * We use a simple fixed-size array; 32 active mappings per task is ample. */
#define LX_MMAP_MAX 32

typedef struct {
    uint32_t addr;   /* kernel-virtual base returned by kmalloc */
    uint32_t length; /* rounded-up length in bytes               */
} lx_mmap_region_t;

/* Retrieve the region table for the current task.
 * We store it in a small heap buffer pointed to by task->mmap_regions.
 * On first access it is allocated and zeroed. */
static lx_mmap_region_t *get_regions(void) {
    task_t *t = sched_current();
    if (!t->mmap_regions) {
        t->mmap_regions = (lx_mmap_region_t *)
            kmalloc(sizeof(lx_mmap_region_t) * LX_MMAP_MAX);
        if (t->mmap_regions)
            memset(t->mmap_regions, 0,
                   sizeof(lx_mmap_region_t) * LX_MMAP_MAX);
    }
    return (lx_mmap_region_t *)t->mmap_regions;
}

/* Register a new mapping in the per-task table */
static void region_add(uint32_t addr, uint32_t length) {
    lx_mmap_region_t *r = get_regions();
    if (!r) return;
    for (int i = 0; i < LX_MMAP_MAX; i++) {
        if (r[i].addr == 0) {
            r[i].addr   = addr;
            r[i].length = length;
            return;
        }
    }
    kwarn("lx_mmap: region table full — munmap will leak this region\n");
}

/* Remove and free a mapping from the per-task table */
static int region_remove(uint32_t addr) {
    lx_mmap_region_t *r = get_regions();
    if (!r) return -1;
    for (int i = 0; i < LX_MMAP_MAX; i++) {
        if (r[i].addr == addr) {
            kfree((void *)(uintptr_t)r[i].addr);
            r[i].addr   = 0;
            r[i].length = 0;
            return 0;
        }
    }
    return -1; /* not found — may have been a partial unmap; ignore */
}

/*
 * lx_mmap — allocate a region of virtual memory.
 */
uint32_t lx_mmap(uint32_t addr, uint32_t length, int prot,
                 int flags, int fd, uint32_t offset) {
    (void)addr; (void)offset;

    if (!(flags & MAP_ANONYMOUS)) {
        kwarn("lx_mmap: file-backed mmap not supported (fd=%d)\n", fd);
        return (uint32_t)-ENOSYS;
    }

    if (length == 0)
        return (uint32_t)-EINVAL;

    uint32_t npages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t alloc_size = npages * PAGE_SIZE;

    void *mem = kmalloc(alloc_size);
    if (!mem) {
        kwarn("lx_mmap: OOM for %u pages\n", npages);
        return (uint32_t)-ENOMEM;
    }

    memset(mem, 0, alloc_size);

    uint32_t vaddr = (uint32_t)(uintptr_t)mem;
    uint32_t pflags = PAGE_PRESENT;
    if (prot & PROT_WRITE) pflags |= PAGE_WRITE;
    for (uint32_t i = 0; i < npages; i++)
        paging_map(vaddr + i * PAGE_SIZE, vaddr + i * PAGE_SIZE, pflags);

    region_add(vaddr, alloc_size);

    kdebug("lx_mmap: mapped %u pages @ 0x%x (prot=%d)\n", npages, vaddr, prot);
    return vaddr;
}

/*
 * lx_munmap — release a mapped region and reclaim its heap memory.
 */
int lx_munmap(uint32_t addr, uint32_t length) {
    (void)length;
    if (addr == 0) return -EINVAL;
    int r = region_remove(addr);
    if (r != 0)
        kwarn("lx_munmap: 0x%x not found in region table\n", addr);
    return 0;
}

/*
 * lx_munmap_all — free all mmap regions for the current task.
 * Called from the Linux process exit path.
 */
void lx_munmap_all(void) {
    task_t *t = sched_current();
    lx_mmap_region_t *r = (lx_mmap_region_t *)t->mmap_regions;
    if (!r) return;
    for (int i = 0; i < LX_MMAP_MAX; i++) {
        if (r[i].addr) {
            kfree((void *)(uintptr_t)r[i].addr);
            r[i].addr = 0;
        }
    }
    kfree(r);
    t->mmap_regions = NULL;
}

/*
 * lx_brk — adjust the data-segment break point of the current task.
 */
uint32_t lx_brk(uint32_t new_brk) {
    task_t *t = sched_current();

    if (t->brk_start == 0) {
        t->brk_start   = 0x600000;
        t->brk_current = t->brk_start;
    }

    if (new_brk == 0)
        return t->brk_current;

    if (new_brk < t->brk_start)
        return t->brk_current;

    if (new_brk > t->brk_current) {
        uint32_t old = t->brk_current;
        uint32_t aligned = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        for (uint32_t pa = (old + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
             pa < aligned; pa += PAGE_SIZE) {
            uint32_t phys = pmm_alloc();
            if (!phys) {
                kwarn("lx_brk: OOM at 0x%x\n", pa);
                return t->brk_current;
            }
            paging_map(pa, phys, PAGE_PRESENT | PAGE_WRITE);
            memset((void *)(uintptr_t)pa, 0, PAGE_SIZE);
        }
        t->brk_current = aligned;
    }

    return t->brk_current;
}
