/* kernel/tss.h — 64-bit Task State Segment (TSS)
 *
 * In 64-bit mode the TSS is used exclusively for:
 *   1. RSP0 — the kernel stack address the CPU loads on ring-3→ring-0 transition
 *              (interrupt, SYSCALL, or exception while running user code).
 *   2. IST  — Interrupt Stack Table entries for NMI / double-fault stacks.
 *
 * One TSS serves the entire kernel because DracolaxOS is single-core.
 * On SMP, each CPU would need its own TSS.
 *
 * GDT slot layout (selector / 8):
 *   0x00  null
 *   0x08  kernel code  DPL=0
 *   0x10  kernel data  DPL=0
 *   0x18  user data    DPL=3
 *   0x20  user code    DPL=3
 *   0x28  TSS low  (16-byte 64-bit TSS descriptor spans 0x28 and 0x30)
 *   0x30  TSS high
 *
 * SYSCALL / SYSRET selector arithmetic (AMD64 vol.2 §2.4):
 *   STAR[47:32] = 0x08  → SYSCALL:  CS=0x08  SS=0x10
 *   STAR[63:48] = 0x10  → SYSRET64: CS=0x23  SS=0x1B  (|3 added by CPU)
 */
#ifndef TSS_H
#define TSS_H
#include "../../types.h"

/* Segment selectors */
#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_DATA    0x1B   /* 0x18 | RPL=3 */
#define GDT_USER_CODE    0x23   /* 0x20 | RPL=3 */
#define GDT_TSS_SEL      0x28   /* TSS base selector (no RPL needed for ltr) */

/* 64-bit TSS layout (Intel Vol.3 §8.7) */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;          /* kernel stack ptr — loaded on ring-3 → ring-0  */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];        /* IST1..IST7 — alternate stacks for NMI etc.    */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;    /* offset to I/O permission bitmap (set to 0x68)  */
} tss64_t;

/* Initialise TSS structure, install into GDT, and load with ltr.
 * Must be called after gdt_ring3_init() has rebuilt the GDT. */
void tss_init(void);

/* Update RSP0 to point at the top of the current kernel task stack.
 * Called by the scheduler on every context switch so ring-3 code always
 * returns to the correct kernel stack on the next interrupt/syscall. */
void tss_set_rsp0(uint64_t rsp0);

/* Returns a pointer to the global TSS (used by gdt.c to encode the base) */
tss64_t *tss_get(void);

#endif /* TSS_H */
