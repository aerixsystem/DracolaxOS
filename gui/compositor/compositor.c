/* gui/compositor/compositor.c
 *
 * Minimal window compositor for DracolaxOS.
 * Runs as a kernel task, composites windows to the VESA framebuffer.
 *
 * Features:
 *   - Up to 16 windows with z-ordering
 *   - Per-window back buffer (blitted to framebuffer on render)
 *   - Glassmorphism style: translucent title bar with blur approximation
 *   - Rounded corners via fb_rounded_rect
 *   - Input routing: keyboard events forwarded to focused window
 */
#include "../../kernel/types.h"
#include "../../kernel/drivers/vga/fb.h"
#include "../../kernel/mm/vmm.h"
#include "../../kernel/klibc.h"
#include "../../kernel/log.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/drivers/ps2/keyboard.h"
#include "../../kernel/arch/x86_64/pic.h"
#include "compositor.h"
#include "../../kernel/arch/x86_64/rtc.h"

/* ---- Unified Glassmorphism palette (matches desktop.c) -----------------
 * Old compositor used GitHub-dark (0x0d1117 family).  Replaced with the
 * same purple-navy tokens as the desktop so that compositor windows, the
 * floating dock, and the top bar all share one visual language.
 * --------------------------------------------------------------------- */
#define COL_VOID         0x04040Cu
#define COL_GLASS_BG     0x0F1020u
#define COL_GLASS_PANEL  0x1A1D3Au
#define COL_GLASS_EDGE   0x3A3F7Au
#define COL_GLASS_SHINE  0x5A60C0u
#define COL_ACCENT       0x7828C8u
#define COL_ACCENT_LT    0xA050F0u
#define COL_ACCENT_DIM   0x3A1460u
#define COL_TEXT_HI      0xF0F0FFu
#define COL_TEXT_MED     0xA0A0C8u
#define COL_TEXT_DIM     0x60607Au
#define COL_SEP          0x2A2C50u

/* Aliases so render_* functions below stay readable */
#define BG_COLOR        COL_VOID
#define DOCK_BG         COL_GLASS_BG
#define DOCK_BTN_NORMAL COL_GLASS_PANEL
#define DOCK_BTN_HOVER  COL_ACCENT_DIM
#define DOCK_BTN_FG     COL_TEXT_HI
#define BAR_BG          COL_GLASS_BG
#define BAR_FG          COL_TEXT_HI
#define WIN_TITLE_BG    COL_GLASS_PANEL
#define WIN_TITLE_FG    COL_TEXT_HI
#define WIN_BODY_BG     COL_VOID
#define WIN_BORDER      COL_GLASS_EDGE
#define WIN_CLOSE_BTN   0xC82828u   /* error red  (matches desktop COL_ERR) */
#define COL_WARN_BTN    0xC8A020u   /* amber  — minimise dot              */
#define COL_MAX_BTN     0x28C878u   /* green  — maximise dot              */
#define WIN_CORNER_R    8

/* Screen layout */
#define TOPBAR_H        28
#define DOCK_W          64
#define DOCK_MARGIN     12
#define DOCK_BTN_SZ     44
#define FONT_W          8
#define FONT_H          16

static window_t windows[COMP_MAX_WINDOWS];
static int      win_count = 0;
static int      current_desktop = 0;  /* BUG FIX 1.10: active virtual desktop */

/* ---- window API --------------------------------------------------------- */

int comp_create_window(const char *title, uint32_t x, uint32_t y,
                       uint32_t w, uint32_t h) {
    if (win_count >= COMP_MAX_WINDOWS) return -1;
    window_t *win = &windows[win_count];
    memset(win, 0, sizeof(*win));
    strncpy(win->title, title ? title : "Window", COMP_TITLE_MAX - 1);
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->z = (uint32_t)win_count;
    win->visible = 1;
    win->focused = 0;
    win->desktop = current_desktop;  /* BUG FIX 1.10: assign to current desktop */
    win->border_color = WIN_BORDER;
    win->title_color  = WIN_TITLE_BG;
    win->backbuf = kmalloc(w * h * 4);
    if (win->backbuf) memset(win->backbuf, 0x0d, w * h * 4);
    return win_count++;
}

void comp_destroy_window(int h) {
    if (h < 0 || h >= win_count) return;
    windows[h].visible = 0;
}

/* BUG FIX 1.10: virtual desktop control */
void comp_switch_desktop(int idx) {
    if (idx < 0 || idx >= 4) return;
    current_desktop = idx;
    kinfo("COMPOSITOR: switched to desktop %d\n", idx);
}

int comp_current_desktop(void) { return current_desktop; }

void comp_move_window(int h, uint32_t x, uint32_t y) {
    if (h < 0 || h >= win_count) return;
    windows[h].x = x; windows[h].y = y;
}

void comp_focus_window(int h) {
    for (int i = 0; i < win_count; i++) windows[i].focused = 0;
    if (h >= 0 && h < win_count) {
        windows[h].focused = 1;
        /* Bring to front */
        uint32_t maxz = 0;
        for (int i = 0; i < win_count; i++)
            if (windows[i].z > maxz) maxz = windows[i].z;
        windows[h].z = maxz + 1;
    }
}

void comp_window_print(int h, uint32_t x, uint32_t y, const char *s,
                       uint32_t fg) {
    /* Renders text directly into the window backbuf using the shared VGA font.
     * render_window() blits the backbuf to screen, so text drawn here
     * survives every comp_render() call without being erased. */
    if (h < 0 || h >= win_count) return;
    window_t *win = &windows[h];
    if (!win->backbuf) return;

    uint32_t col = x;
    uint32_t title_h = (uint32_t)FONT_H + 10u;

    while (*s) {
        if (*s == '\n') {
            col = x;
            y += (uint32_t)FONT_H;
        } else {
            if (col + FONT_W <= win->w && y + FONT_H <= win->h) {
                uint32_t bg_color = (y < title_h) ? WIN_TITLE_BG : WIN_BODY_BG;
                uint8_t idx = (uint8_t)*s;
                if (idx >= 128) idx = '?';

                /* Blit glyph into backbuf pixel by pixel */
                for (uint32_t gy = 0; gy < (uint32_t)FONT_H; gy++) {
                    uint8_t bits = vga_font_8x16[idx][gy];
                    for (uint32_t gx = 0; gx < (uint32_t)FONT_W; gx++) {
                        uint32_t brow = y + gy;
                        uint32_t bcol = col + gx;
                        if (brow < win->h && bcol < win->w) {
                            int lit = (bits >> (7u - gx)) & 1;
                            win->backbuf[brow * win->w + bcol] =
                                lit ? fg : bg_color;
                        }
                    }
                }
                col += (uint32_t)FONT_W;
            }
        }
        s++;
        if (col + (uint32_t)FONT_W > win->w) { col = x; y += (uint32_t)FONT_H; }
        if (y + (uint32_t)FONT_H > win->h) break;
    }
}

void comp_window_fill(int h, uint32_t x, uint32_t y, uint32_t w, uint32_t hh,
                      uint32_t color) {
    /* FIX: write into the window backbuf, not raw framebuffer.
     * render_window() will blit the backbuf to the screen during comp_render(). */
    if (h < 0 || h >= win_count) return;
    window_t *win = &windows[h];
    if (!win->backbuf) return;
    uint32_t x1 = x, y1 = y;
    uint32_t x2 = (x + w < win->w) ? x + w : win->w;
    uint32_t y2 = (y + hh < win->h) ? y + hh : win->h;
    for (uint32_t row = y1; row < y2; row++)
        for (uint32_t col = x1; col < x2; col++)
            win->backbuf[row * win->w + col] = color;
}

/* ---- render -------------------------------------------------------------- */

static void render_window(window_t *win) {
    if (!win->visible || !fb.available) return;
    uint32_t x = win->x, y = win->y, w = win->w, h = win->h;
    uint32_t title_h = (uint32_t)FONT_H + 10u;

    /* FIX: blit backbuf to framebuffer for the client area.
     * If backbuf has content (from comp_window_fill/print), blit it.
     * Otherwise fill with the default body colour. */
    if (win->backbuf) {
        fb_blit(x, y, w, h, win->backbuf);
    } else {
        fb_fill_rect(x, y + title_h, w, h - title_h,
                     win->focused ? COL_GLASS_BG : WIN_BODY_BG);
    }

    /* Title bar — glass fill */
    fb_fill_rect(x, y, w, title_h, WIN_TITLE_BG);
    /* Top shine strip — matches desktop glass_panel() style */
    fb_fill_rect(x + (uint32_t)WIN_CORNER_R, y,
                 w - 2u * (uint32_t)WIN_CORNER_R, 1u, COL_GLASS_SHINE);
    fb_print(x + (uint32_t)WIN_CORNER_R + 20u, y + 7u,
             win->title, WIN_TITLE_FG, WIN_TITLE_BG);

    /* Traffic-light dots — rounded, matching glassmorphism language */
    fb_rounded_rect(x + 8u,  y + 8u, 10u, 10u, 3u, WIN_CLOSE_BTN);
    fb_rounded_rect(x + 22u, y + 8u, 10u, 10u, 3u, COL_WARN_BTN);
    fb_rounded_rect(x + 36u, y + 8u, 10u, 10u, 3u, COL_MAX_BTN);

    /* Border — accent-purple when focused, dim edge when not */
    fb_rounded_rect(x, y, w, h, WIN_CORNER_R,
                    win->focused ? COL_ACCENT_LT : WIN_BORDER);
}

static void render_topbar(void) {
    if (!fb.available) return;
    /* Match desktop top bar: glass bg + bottom separator + top shine */
    fb_fill_rect(0, 0, fb.width, TOPBAR_H, BAR_BG);
    fb_fill_rect(0, TOPBAR_H - 1, fb.width, 1u, COL_SEP);
    fb_fill_rect(0, 0, fb.width, 1u, COL_GLASS_SHINE);

    /* OS name — starts after dock width to not overlap desktop dock */
    fb_print((uint32_t)DOCK_W + 10u, 6u, "DracolaxOS", BAR_FG, BAR_BG);

    /* Real wall clock — read RTC and format HH:MM:SS */
    rtc_time_t _rtc_t;
    rtc_read(&_rtc_t);
    char _clock_buf[12];
    snprintf(_clock_buf, sizeof(_clock_buf), "%02u:%02u:%02u",
             _rtc_t.hour, _rtc_t.min, _rtc_t.sec);
    uint32_t _cw = (uint32_t)strlen(_clock_buf) * 8u;
    fb_print(fb.width > _cw + 8u ? fb.width - _cw - 8u : 0u, 6u,
             _clock_buf, COL_TEXT_MED, BAR_BG);
}

static void render_dock(void) {
    /* Compositor dock is only shown when the full desktop task is NOT running
     * (i.e. in headless compositor-only mode).  In graphical boot the desktop
     * task owns its own left dock and this function is a no-op to avoid
     * drawing a second dock over the desktop one. */
    (void)0;
}

void comp_render(void) {
    if (!fb.available) return;

    /* BUG FIX (dock buttons): comp_render() now renders ONLY windows.
     * Background, dock, and topbar are owned by the desktop task.
     * Calling fb_fill_rect here would wipe the desktop's shadow buffer.
     * The desktop loop calls comp_render() after draw_dock/topbar/wm,
     * just before fb_flip(), so compositor windows land on top correctly. */

    int sorted[COMP_MAX_WINDOWS];
    int visible_count = 0;
    for (int i = 0; i < win_count; i++) {
        if (windows[i].visible && windows[i].desktop == current_desktop)
            sorted[visible_count++] = i;
    }
    for (int i = 0; i < visible_count; i++)
        for (int j = i + 1; j < visible_count; j++)
            if (windows[sorted[j]].z < windows[sorted[i]].z) {
                int tmp = sorted[i]; sorted[i] = sorted[j]; sorted[j] = tmp;
            }
    for (int i = 0; i < visible_count; i++)
        render_window(&windows[sorted[i]]);
}

void comp_init(void) {
    memset(windows, 0, sizeof(windows));
    win_count = 0;
    kinfo("COMPOSITOR: init (fb %s)\n", fb.available ? "available" : "N/A");
}

/* ---- compositor main task ----------------------------------------------- */

void comp_task(void) {
    comp_init();

    if (!fb.available) {
        /* Fallback: headless mode, just print notice to VGA text */
        kinfo("COMPOSITOR: running headless (no VESA framebuffer)\n");
        for (;;) sched_sleep(1000);
    }

    /* Initial render */
    comp_render();

    /* Event loop */
    for (;;) {
        /* Re-render every ~100ms (10 ticks at 100Hz) */
        sched_sleep(100);
        comp_render();
    }
}
