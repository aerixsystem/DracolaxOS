/* Paint — STUB (see paint.h) */
#include "../../kernel/sched/sched.h"
#include "paint.h"

void app_paint(void) {
    __asm__ volatile ("sti");
    sched_exit();
}
