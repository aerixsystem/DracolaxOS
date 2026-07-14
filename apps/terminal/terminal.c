/* Terminal — STUB (see terminal.h) */
#include "../../kernel/sched/sched.h"
#include "terminal.h"

void app_terminal(void) {
    __asm__ volatile ("sti");
    sched_exit();
}
