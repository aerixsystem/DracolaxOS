/* kernel/pic.c */
#include "../../types.h"
#include "pic.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define PIT_CH0  0x40
#define PIT_CMD  0x43
#define PIT_FREQ 1193182u

static volatile uint32_t ticks = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void io_wait(void) { outb(0x80, 0); }

void pic_remap(void) {
    /* ICW1: init + cascade + ICW4 needed */
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    /* ICW2: remap IRQ0-7 -> vectors 32-39, IRQ8-15 -> vectors 40-47 */
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();
    /* ICW3: slave on IRQ2 */
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    /* Mask ALL IRQs after remap.
     * Each driver unmasks its own IRQ line explicitly.
     * Prevents spurious interrupts before handlers are registered. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

void pic_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (uint8_t)(1u << (irq & 7u));
    outb(port, inb(port) | bit);
}

void pic_unmask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (uint8_t)(1u << (irq & 7u));
    outb(port, inb(port) & ~bit);
    /* Slave IRQs (8-15) route through the cascade line (IRQ2 on master).
     * If IRQ2 is masked on the master PIC, no slave interrupt can reach
     * the CPU regardless of the slave IMR.  Auto-unmask IRQ2 here so
     * callers don't have to remember to do it separately. */
    if (irq >= 8)
        outb(PIC1_DATA, inb(PIC1_DATA) & ~(uint8_t)0x04u);
}

void pit_init(uint32_t hz) {
    uint32_t div = PIT_FREQ / hz;
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (uint8_t)(div & 0xFF));
    outb(PIT_CH0, (uint8_t)(div >> 8));
    pic_unmask(0);   /* unmask IRQ0 (timer) — must happen here */
}

uint32_t pit_ticks(void) { return ticks; }
void     pit_tick (void) { ticks++; }
