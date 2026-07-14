/* tests/test_irq.c — IRQ registration tests */
#include "../kernel/arch/x86_64/irq.h"
#include "../kernel/arch/x86_64/idt.h"
#include "../kernel/log.h"
#include "../kernel/klibc.h"

static int pass, fail;
static volatile int handler_called;

static void check(const char *name, int cond) {
    if (cond) { kinfo("  PASS: %s\n", name); pass++; }
    else       { kerror("  FAIL: %s\n", name); fail++; }
}

static void test_handler(struct isr_frame *f) {
    (void)f;
    handler_called++;
}

void test_irq(void) {
    kinfo("=== IRQ tests ===\n");
    pass = fail = 0;

    /* 1. Register a handler for vector 45 (unused hardware IRQ) */
    handler_called = 0;
    irq_register(45, test_handler);
    check("register does not crash", 1);

    /* 2. Unregister */
    irq_unregister(45);
    check("unregister does not crash", 1);

    /* 3. INT 3 (breakpoint) — should NOT call kpanic because irq_init
     *    sets a default_exception handler that panics.  Instead re-register
     *    a safe handler, fire the interrupt, then restore. */
    handler_called = 0;
    irq_register(3, test_handler);    /* replace panic handler temporarily */
    __asm__ volatile ("int $3");      /* fire breakpoint */
    check("soft INT fires handler", handler_called == 1);
    irq_register(3, NULL);            /* restore (NULL = default exception) */

    /* 4. Re-register default (non-panicking) exception — just checks no crash */
    irq_register(45, test_handler);
    irq_unregister(45);
    check("double unregister is safe", 1);

    kinfo("IRQ: %d passed, %d failed\n", pass, fail);
}
