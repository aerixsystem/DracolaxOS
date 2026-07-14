/* kernel/idt.h — 64-bit Interrupt Descriptor Table */
#ifndef IDT_H
#define IDT_H
#include "../../types.h"

#define IDT_ENTRIES 256

/* 64-bit IDT gate descriptor (16 bytes) */
struct idt_entry {
    uint16_t base_lo;    /* handler offset bits 0-15            */
    uint16_t selector;   /* kernel code segment (GDT_KERNEL_CODE) */
    uint8_t  ist;        /* interrupt stack table index (0=none)  */
    uint8_t  flags;      /* type | DPL | present                  */
    uint16_t base_mid;   /* handler offset bits 16-31             */
    uint32_t base_hi;    /* handler offset bits 32-63             */
    uint32_t reserved;   /* must be 0                             */
} __attribute__((packed));

/* IDTR register value */
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);

#endif /* IDT_H */
