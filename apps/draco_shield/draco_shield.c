/* Draco Shield — STUB (see draco_shield.h) */
#include "../../kernel/sched/sched.h"
#include "draco_shield.h"

void app_shield_ui(void) {
    __asm__ volatile ("sti");
    sched_exit();
}
