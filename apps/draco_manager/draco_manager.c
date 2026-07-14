/* Draco Manager — STUB (see draco_manager.h) */
#include "../../kernel/sched/sched.h"
#include "draco_manager.h"

void app_draco_manager(void) {
    __asm__ volatile ("sti");
    sched_exit();
}
