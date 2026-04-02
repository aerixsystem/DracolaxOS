/* kernel/paging.c — x86_64 paging (stub for 4GB identity-mapped range)
 *
 * boot.s establishes 4-level paging with 2MB huge pages covering all 4 GB
 * of physical memory before calling kmain().  No further setup is required
 * for the current kernel address space (everything lives below 4 GB).
 */
#include "../types.h"
#include "paging.h"
#include "../log.h"

void paging_init(void) {
    /* Already done in boot.s — 4 GB identity map active via 2MB huge pages */
    kinfo("Paging active (4 GB identity map, 2MB huge pages via boot.s)\n");
}

void paging_map(uint64_t virt, uint64_t phys, uint32_t flags) {
    /* The entire 4 GB physical range is already identity-mapped by boot.s.
     * Any address 0x0 – 0xFFFFFFFF is accessible at its physical address.
     * Fine-grained 4K mappings above 4 GB can be implemented here later. */
    (void)virt; (void)phys; (void)flags;
}

void paging_invlpg(uint64_t virt) {
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}
