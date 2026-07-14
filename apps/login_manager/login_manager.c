/* Login Manager — STUB (see login_manager.h) */
#include "../../kernel/sched/sched.h"
#include "login_manager.h"

void app_login_manager(void) {
    __asm__ volatile ("sti");
    sched_exit();
}
