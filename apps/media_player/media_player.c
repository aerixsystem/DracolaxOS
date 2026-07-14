/* Media Player — STUB (see media_player.h) */
#include "../../kernel/sched/sched.h"
#include "media_player.h"

void app_media_player(void) {
    __asm__ volatile ("sti");
    sched_exit();
}
