/* services/notification_daemon.c — notification queue + visual toast banner */
#include "../kernel/types.h"
#include "../kernel/log.h"
#include "../kernel/sched/sched.h"
#include "../kernel/klibc.h"
#include "../kernel/drivers/vga/fb.h"
#include "../kernel/arch/x86_64/pic.h"
#include "notification_daemon.h"

#define NOTIF_MAX    32
#define TOAST_W      320u
#define TOAST_H      48u
#define TOAST_MARGIN 12u
#define TOAST_BG     0x1A1A2Eu   /* dark navy, matches glassmorphism palette */
#define TOAST_BORDER 0x5588FFu   /* accent blue                              */
#define TOAST_FG     0xEEEEEEu
#define TOAST_FG_DIM 0x8899AAu
#define TOAST_TICKS  300u        /* display for ~3 s at 100 Hz               */

static notif_t notif_queue[NOTIF_MAX];
static int     notif_head = 0, notif_tail = 0;

/* Active toast state */
static notif_t  g_toast;
static int      g_toast_active = 0;
static uint32_t g_toast_expire = 0;   /* tick count when toast hides */

void notif_push(const char *title, const char *body, int level) {
    int next = (notif_tail + 1) % NOTIF_MAX;
    if (next == notif_head) return;  /* queue full, drop */
    notif_t *n = &notif_queue[notif_tail];
    strncpy(n->title, title ? title : "", 63);
    strncpy(n->body,  body  ? body  : "", 255);
    n->level = level;
    notif_tail = next;
}

int notif_pop(notif_t *out) {
    if (notif_head == notif_tail) return 0;
    *out = notif_queue[notif_head];
    notif_head = (notif_head + 1) % NOTIF_MAX;
    return 1;
}

/* Draw the current active toast in the top-right corner */
static void draw_toast(void) {
    if (!g_toast_active) return;
    if (!fb.width || !fb.height) return;

    uint32_t tx = fb.width  - TOAST_W - TOAST_MARGIN;
    uint32_t ty = TOAST_MARGIN + 32u;   /* below the topbar */

    /* Background + border */
    fb_fill_rect(tx, ty, TOAST_W, TOAST_H, TOAST_BG);
    fb_rounded_rect(tx, ty, TOAST_W, TOAST_H, 4, TOAST_BORDER);

    /* Level indicator strip on left edge */
    uint32_t strip_col = (g_toast.level >= 2) ? 0xFF4444u :
                         (g_toast.level == 1)  ? 0xFFAA00u : TOAST_BORDER;
    fb_fill_rect(tx, ty, 4u, TOAST_H, strip_col);

    /* Title */
    fb_print(tx + 10u, ty + 8u,  g_toast.title, TOAST_FG,     TOAST_BG);
    /* Body (truncated to fit) */
    char body_trunc[40];
    strncpy(body_trunc, g_toast.body, 38);
    body_trunc[38] = '\0';
    if (strlen(g_toast.body) > 38) {
        body_trunc[36] = '.'; body_trunc[37] = '.'; body_trunc[38] = '.';
        body_trunc[39] = '\0';
    }
    fb_print(tx + 10u, ty + 26u, body_trunc,     TOAST_FG_DIM, TOAST_BG);
}

/* Erase the toast region (called when it expires) */
static void erase_toast(void) {
    if (!fb.width) return;
    uint32_t tx = fb.width - TOAST_W - TOAST_MARGIN;
    uint32_t ty = TOAST_MARGIN + 32u;
    fb_fill_rect(tx, ty, TOAST_W, TOAST_H, 0x00000000u);
}

void notification_daemon_task(void) {
    kinfo("NOTIF: daemon running\n");
    for (;;) {
        uint32_t now = (uint32_t)pit_ticks();

        /* Expire current toast */
        if (g_toast_active && now >= g_toast_expire) {
            g_toast_active = 0;
            erase_toast();
        }

        /* Pop next pending notification */
        notif_t n;
        while (notif_pop(&n)) {
            kinfo("NOTIF[%d]: %s — %s\n", n.level, n.title, n.body);
            g_toast        = n;
            g_toast_active = 1;
            g_toast_expire = now + TOAST_TICKS;
        }

        /* Redraw toast every tick so it stays visible over desktop redraws */
        draw_toast();

        sched_sleep(100);   /* ~1 Hz refresh — low overhead */
    }
}
