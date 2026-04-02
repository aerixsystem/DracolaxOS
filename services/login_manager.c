/* services/login_manager.c */
#include "../kernel/types.h"
#include "../kernel/log.h"
#include "../kernel/sched/sched.h"
void login_manager_task(void) {
    kinfo("LOGIN-MGR: running\n");
    for (;;) sched_sleep(2000);
}
