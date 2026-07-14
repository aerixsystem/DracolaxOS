/* kernel/pic.h — 8259A PIC and 8253 PIT */
#ifndef PIC_H
#include "../../types.h"
#define PIC_H


/* Remap PIC IRQs to vectors 32-47 (above CPU exceptions) */
void pic_remap(void);

/* Send End-of-Interrupt for IRQ line n (0-15) */
void pic_eoi(uint8_t irq);

/* Mask / unmask individual IRQ lines */
void pic_mask  (uint8_t irq);
void pic_unmask(uint8_t irq);

/* Program PIT channel 0 to fire IRQ0 at the given frequency (Hz) */
void pit_init(uint32_t hz);

/* Increment tick counter — call once per timer IRQ */
void     pit_tick (void);

/* Current tick count */
uint32_t pit_ticks(void);

#endif /* PIC_H */
