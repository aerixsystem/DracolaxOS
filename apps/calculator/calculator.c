/* Calculator — STUB (see calculator.h) */
#include "../../kernel/sched/sched.h"
#include "calculator.h"

void app_calculator(void) {
    __asm__ volatile ("sti");
    sched_exit();
}
