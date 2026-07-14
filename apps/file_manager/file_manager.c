/* File Manager — STUB (see file_manager.h) */
#include "../../kernel/sched/sched.h"
#include "file_manager.h"

void app_file_manager(void) {
    __asm__ volatile ("sti");
    sched_exit();
}
