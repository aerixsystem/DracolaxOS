/* Image Viewer — STUB (see image_viewer.h) */
#include "../../kernel/sched/sched.h"
#include "image_viewer.h"

void app_image_viewer(void) {
    __asm__ volatile ("sti");
    sched_exit();
}
