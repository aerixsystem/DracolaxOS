/* Settings — STUB (see settings.h) */
#include "../../kernel/sched/sched.h"
#include "settings.h"

void app_settings(void) {
    __asm__ volatile ("sti");
    sched_exit();
}
