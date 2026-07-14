/* Package Manager — STUB (see package_manager.h) */
#include "../../kernel/sched/sched.h"
#include "package_manager.h"

void app_pkg_manager(void) {
    __asm__ volatile ("sti");
    sched_exit();
}
