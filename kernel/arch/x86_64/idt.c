/* kernel/idt.c — 64-bit IDT setup */
#include "idt.h"
#include "gdt.h"

/* ISR stubs from isr_stubs.s */
#define DECL_ISR(n) extern void isr##n(void)
DECL_ISR(0);  DECL_ISR(1);  DECL_ISR(2);  DECL_ISR(3);
DECL_ISR(4);  DECL_ISR(5);  DECL_ISR(6);  DECL_ISR(7);
DECL_ISR(8);  DECL_ISR(9);  DECL_ISR(10); DECL_ISR(11);
DECL_ISR(12); DECL_ISR(13); DECL_ISR(14); DECL_ISR(15);
DECL_ISR(16); DECL_ISR(17); DECL_ISR(18); DECL_ISR(19);
DECL_ISR(20); DECL_ISR(21); DECL_ISR(22); DECL_ISR(23);
DECL_ISR(24); DECL_ISR(25); DECL_ISR(26); DECL_ISR(27);
DECL_ISR(28); DECL_ISR(29); DECL_ISR(30); DECL_ISR(31);
DECL_ISR(32); DECL_ISR(33); DECL_ISR(34); DECL_ISR(35);
DECL_ISR(36); DECL_ISR(37); DECL_ISR(38); DECL_ISR(39);
DECL_ISR(40); DECL_ISR(41); DECL_ISR(42); DECL_ISR(43);
DECL_ISR(44); DECL_ISR(45); DECL_ISR(46); DECL_ISR(47);
DECL_ISR(128);

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idtp;

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_lo  = (uint16_t)(base & 0xFFFF);
    idt[num].base_mid = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].base_hi  = (uint32_t)(base >> 32);
    idt[num].selector = sel;
    idt[num].ist      = 0;
    idt[num].flags    = flags;
    idt[num].reserved = 0;
}

#define SET(n) idt_set_gate(n, (uint64_t)(uintptr_t)isr##n, GDT_KERNEL_CODE, 0x8E)

void idt_init(void) {
    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    idtp.base  = (uint64_t)(uintptr_t)&idt;

    SET(0);  SET(1);  SET(2);  SET(3);
    SET(4);  SET(5);  SET(6);  SET(7);
    SET(8);  SET(9);  SET(10); SET(11);
    SET(12); SET(13); SET(14); SET(15);
    SET(16); SET(17); SET(18); SET(19);
    SET(20); SET(21); SET(22); SET(23);
    SET(24); SET(25); SET(26); SET(27);
    SET(28); SET(29); SET(30); SET(31);
    SET(32); SET(33); SET(34); SET(35);
    SET(36); SET(37); SET(38); SET(39);
    SET(40); SET(41); SET(42); SET(43);
    SET(44); SET(45); SET(46); SET(47);
    /* Syscall — DPL=3 allows user-space INT 0x80 */
    idt_set_gate(128, (uint64_t)(uintptr_t)isr128, GDT_KERNEL_CODE, 0xEE);

    __asm__ volatile ("lidt %0" :: "m"(idtp) : "memory");
}
