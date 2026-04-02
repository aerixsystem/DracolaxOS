/* gui/apps/debug_console.c — Live debug console
 *
 * Draws a scrolling log overlay when triggered (e.g. F12).
 * Reads from klog ring buffer and prints to a semi-transparent panel.
 * Activated by KB_KEY_F12 from the desktop keyboard handler.
 */
#include "../../kernel/types.h"
#include "../../kernel/drivers/vga/fb.h"
#include "../../kernel/klibc.h"
#include "../../kernel/log.h"
#include "../../kernel/klog.h"
#include "debug_console.h"

#define CON_X      20
#define CON_Y      50
#define CON_W      (fb.width  > 40u ? fb.width  - 40u : 400u)
#define CON_H      (fb.height > 100u ? fb.height - 100u : 300u)
#define CON_BG     0x0A0A14u
#define CON_BORDER 0x7828C8u
#define CON_FG     0x28FF80u   /* terminal green */
#define CON_FG_ERR 0xFF4040u
#define FONT_W     8
#define FONT_H     16
#define MAX_LINES  64
#define LINE_SZ    128

static char  con_lines[MAX_LINES][LINE_SZ];
static int   con_head = 0;
static int   con_count = 0;
static int   con_visible = 0;

void dbgcon_toggle(void) {
    con_visible = !con_visible;
    kinfo("DBGCON: %s\n", con_visible ? "shown" : "hidden");
}

void dbgcon_push(const char *line) {
    strncpy(con_lines[con_head], line, LINE_SZ - 1);
    con_lines[con_head][LINE_SZ - 1] = '\0';
    con_head = (con_head + 1) % MAX_LINES;
    if (con_count < MAX_LINES) con_count++;
}

void dbgcon_draw(void) {
    if (!con_visible || !fb.available) return;

    /* Semi-transparent panel background */
    fb_fill_rect(CON_X, CON_Y, CON_W, CON_H, CON_BG);

    /* Border */
    fb_fill_rect(CON_X, CON_Y,          CON_W, 1,     CON_BORDER);
    fb_fill_rect(CON_X, CON_Y + CON_H - 1, CON_W, 1,  CON_BORDER);
    fb_fill_rect(CON_X, CON_Y,          1, CON_H,     CON_BORDER);
    fb_fill_rect(CON_X + CON_W - 1, CON_Y, 1, CON_H, CON_BORDER);

    /* Title bar */
    fb_fill_rect(CON_X + 1, CON_Y + 1, CON_W - 2, FONT_H + 4, 0x1A0030u);
    fb_print(CON_X + 8, CON_Y + 4, "DracolaxOS Debug Console  [F12 to close]",
             CON_BORDER, 0x1A0030u);

    /* Log lines */
    uint32_t max_lines_vis = (CON_H - FONT_H - 8) / FONT_H;
    uint32_t start_y = CON_Y + FONT_H + 8;

    int start = (con_head - (int)max_lines_vis + MAX_LINES) % MAX_LINES;
    int shown = (con_count < (int)max_lines_vis) ? con_count : (int)max_lines_vis;

    for (int i = 0; i < shown; i++) {
        int idx = (start + i) % MAX_LINES;
        uint32_t fg = (con_lines[idx][0] == '[' &&
                       con_lines[idx][1] == 'E') ? CON_FG_ERR : CON_FG;
        fb_print(CON_X + 4, start_y + (uint32_t)i * FONT_H,
                 con_lines[idx], fg, CON_BG);
    }
}

int dbgcon_is_visible(void) { return con_visible; }
