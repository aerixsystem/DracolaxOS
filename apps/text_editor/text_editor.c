/* Text Editor — STUB (see text_editor.h) */
#include "../../kernel/sched/sched.h"
#include "text_editor.h"

void app_text_editor(void) {
    __asm__ volatile ("sti");
    sched_exit();
}
