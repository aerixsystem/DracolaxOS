/* kernel/gdt.c — 64-bit GDT stub
 * The real GDT is built and loaded in kernel/boot.s before kmain() is called.
 * This file exists so the rest of the kernel can call gdt_init() without
 * #ifdef guards everywhere.
 */
#include "gdt.h"
#include "../../log.h"

void gdt_init(void) {
    /* GDT already active (loaded by boot.s long-mode entry sequence).
     * Nothing to reload — just log confirmation. */
    kinfo("GDT loaded (64-bit, via boot.s)\n");
}
