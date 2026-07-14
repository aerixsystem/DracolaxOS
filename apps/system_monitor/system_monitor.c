/* System Monitor — STUB (see system_monitor.h) */
#include "../../kernel/sched/sched.h"
#include "system_monitor.h"

void app_system_monitor(void) {
    __asm__ volatile ("sti");
    sched_exit();
}
