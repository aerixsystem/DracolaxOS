/* kernel/gdt.h — 64-bit GDT (loaded by boot.s, C stub for compatibility) */
#ifndef GDT_H
#define GDT_H
#include "../../types.h"

/* Segment selectors (same values as defined in boot.s gdt64_table) */
#define GDT_KERNEL_CODE  0x08   /* 64-bit code, DPL=0 */
#define GDT_KERNEL_DATA  0x10   /* 64-bit data, DPL=0 */

/* boot.s loads the GDT and sets up long mode before calling kmain.
 * gdt_init() is a no-op kept for source compatibility. */
void gdt_init(void);

#endif /* GDT_H */
