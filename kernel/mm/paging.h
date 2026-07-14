/* kernel/paging.h — x86_64 paging API
 *
 * boot.s sets up 4-level paging with 2MB huge pages covering all 4GB physical
 * memory (identity mapped: virt == phys for 0x0 – 0xFFFFFFFF).
 * paging_init() is therefore a no-op kept for source compatibility.
 *
 * paging_map() performs fine-grained 4KB mapping using the hardware PML4
 * already active. Since boot.s uses huge pages for the first 4GB, any fine-
 * grained mapping in that range first requires a huge-page split — not yet
 * implemented. Callers in the FB/paging-fix path are no longer needed because
 * the framebuffer at ~0xFD000000 is already identity-mapped.
 */
#ifndef PAGING_H
#define PAGING_H
#include "../types.h"

#define PAGE_SIZE    4096u
#define PAGE_PRESENT 0x01u
#define PAGE_WRITE   0x02u
#define PAGE_USER    0x04u
#define PAGE_HUGE    0x80u

/* No-op — boot.s already set up paging before kmain() */
void paging_init(void);

/* Map virt → phys with flags. Currently a log-only stub for the 4GB range
 * covered by identity huge pages; non-identity mappings above 4GB can be
 * added here when a higher-level allocator is in place. */
void paging_map(uint64_t virt, uint64_t phys, uint32_t flags);

/* Invalidate a single TLB entry */
void paging_invlpg(uint64_t virt);

#endif /* PAGING_H */
