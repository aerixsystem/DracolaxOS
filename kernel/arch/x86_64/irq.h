/* kernel/irq.h — IRQ handler registration and dispatch (x86_64) */
#ifndef IRQ_H
#define IRQ_H
#include "../../types.h"

/*
 * CPU state frame delivered to every interrupt handler.
 * Must exactly match the stack layout built by isr_stubs.s.
 * Offsets from the pointer passed to isr_dispatch (all qwords = 8 bytes):
 *   0   r15     8   r14    16   r13    24   r12
 *  32   r11    40   r10    48    r9    56    r8
 *  64   rbp    72   rdi    80   rsi    88   rdx
 *  96   rcx   104   rbx   112   rax
 * 120   int_no
 * 128   err_code
 * 136   rip   (CPU-pushed)
 * 144   cs    (CPU-pushed)
 * 152   rflags(CPU-pushed)
 * 160   rsp   (CPU-pushed, always in x86_64)
 * 168   ss    (CPU-pushed, always in x86_64)
 */
struct isr_frame {
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9,  r8;
    uint64_t rbp, rdi, rsi, rdx;
    uint64_t rcx, rbx, rax;
    uint64_t int_no,   err_code;
    uint64_t rip, cs, rflags, rsp, ss;   /* CPU-pushed */
} __attribute__((packed));

typedef void (*irq_handler_t)(struct isr_frame *frame);

void irq_register  (uint8_t n, irq_handler_t fn);
void irq_unregister(uint8_t n);
void isr_dispatch  (struct isr_frame *frame);
void irq_init      (void);

#endif /* IRQ_H */
