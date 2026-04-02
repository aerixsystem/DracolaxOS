/* kernel/pmm.h — Physical Memory Manager (bitmap allocator) */
#ifndef PMM_H
#include "../types.h"
#define PMM_H


/* Initialise the PMM from the Multiboot2 memory map */
void pmm_init(uint64_t mbi_addr);

/* Allocate one 4KB physical page; returns physical address or 0 on OOM */
uint64_t pmm_alloc(void);

/* Free a 4KB physical page */
void pmm_free(uint64_t addr);

/* Mark a region as used (e.g. kernel image, MMIO, legacy area) */
void pmm_mark_used(uint64_t addr, uint64_t size);

/* Statistics */
uint64_t pmm_total_bytes(void);
uint32_t pmm_free_pages(void);
uint32_t pmm_used_pages(void);

#endif /* PMM_H */
