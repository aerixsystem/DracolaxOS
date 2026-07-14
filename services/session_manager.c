/* services/session_manager.c */
#include "../kernel/types.h"
#include "../kernel/log.h"
#include "../kernel/sched/sched.h"
void session_manager_task(void) {
    kinfo("SESSION: manager running\n");
    for (;;) sched_sleep(1000);
}
