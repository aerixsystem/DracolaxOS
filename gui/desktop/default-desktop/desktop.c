/* gui/desktop/default-desktop/desktop.c
 * DracolaxOS V1 — Glassmorphism Desktop
 *
 * FIX v1.2 (verified against ChatGPT audit — all 5 core bugs confirmed real):
 *
 *   [1] CURSOR OWNERSHIP BUG — arrow keys now persistently own the cursor
 *       until the mouse physically moves.  Old code called mouse_get_x()
 *       every frame and then applied the arrow-key delta, so the NEXT frame's
 *       mouse_get_x() would silently overwrite the adjusted position and snap
 *       the cursor back.  New code tracks g_mouse_x_raw / g_mouse_y_raw and
 *       only updates cursor from mouse when the raw value actually changed.
 *
 *   [2] ROUNDED-CORNER OVERWRITE — glass_panel() already drew a rounded inner
 *       fill via fb_rounded_rect(), but every caller immediately followed with
 *       fb_fill_rect() (a square fill) that overwrote those curved corners.
 *       glass_panel() now accepts a fill_col parameter and all callers have
 *       the redundant square fb_fill_rect() removed.
 *
 *   [3] BACKGROUND IMAGE — draw_wallpaper() and draw_login() now render the
 *       1536×1024 bg_pixels[] array from background.h using nearest-neighbour
 *       scaling directly into the shadow-buffer pointer (avoids per-pixel
 *       function-call overhead).  A g_wallpaper_dirty flag skips the expensive
 *       scaled blit on frames where the desktop area has not changed.
 *
 *   [4] DOCK BUTTON ACTIONS — Start, Search, and Terminal buttons now do
 *       something visible:
 *         * Start    → opens/closes a glass app-launcher grid (Start Menu)
 *         * Search   → opens/closes a search-query input bar
 *         * Terminal → toggles the built-in Debug Console (same as F12)
 *         * Info     → About panel (unchanged)
 *       Right-click CTX_TERMINAL also opens the Debug Console (FIX [5]).
 *
 *   [5] LOG SPAM IN GRAPHICAL MODE — fb_console_lock(1) was already called on
 *       desktop entry which stops fb_console_print().  Additionally the
 *       irq_watchdog_task in init.c now skips its kinfo() calls when
 *       bootmode_wants_desktop() is true, eliminating the 5-second tick spam
 *       in text / graphical modes.
 */

#include "../../../kernel/types.h"
#include "../../../kernel/drivers/vga/fb.h"
#include "../../../kernel/drivers/vga/vga.h"
#include "../../../kernel/sched/sched.h"
#include "../../../kernel/drivers/ps2/keyboard.h"
#include "../../../kernel/drivers/ps2/mouse.h"
#include "../../../kernel/drivers/ps2/vmmouse.h"
#include "../../../kernel/klibc.h"
#include "../../../kernel/log.h"
#include "../../../kernel/arch/x86_64/pic.h"
#include "../../../kernel/security/dracoauth.h"
#include "../../compositor/compositor.h"
#include "../../wm/wm.h"
#include "../../../apps/appman/appman.h"
#include "../../../apps/installer/installer.h"
#include "../../../apps/debug_console/debug_console.h"
#include "../../../services/power_manager.h"
#include "../../../kernel/draco_logo.h"

/* FIX [3]: background pixel array (1536×1024, 0xAARRGGBB row-major).
 * WARNING: background.h is 100 000+ lines. Only include here. */
#include "background.h"
#include "../../../kernel/drivers/vga/cursor.h"
#include "../../../kernel/arch/x86_64/rtc.h"

/* =========================================================================
 * Colour palette — Glassmorphism
 * ========================================================================= */
#define COL_VOID        0x04040Cu
#define COL_GLASS_BG    0x0F1020u
#define COL_GLASS_PANEL 0x1A1D3Au
#define COL_GLASS_EDGE  0x3A3F7Au
#define COL_GLASS_SHINE 0x5A60C0u
#define COL_ACCENT      0x7828C8u
#define COL_ACCENT_LT   0xA050F0u
#define COL_ACCENT_DIM  0x3A1460u
#define COL_TEXT_HI     0xF0F0FFu
#define COL_TEXT_MED    0xA0A0C8u
#define COL_TEXT_DIM    0x60607Au
#define COL_OK          0x28C878u
#define COL_ERR         0xC82828u
#define COL_WARN        0xC8A020u
#define COL_TOPBAR      0x10122Au
#define COL_DOCK        0x10122Au
#define COL_SEP         0x2A2C50u

/* Dark overlay applied over bg image so text/panels stay readable */
#define BG_OVERLAY_R  4u
#define BG_OVERLAY_G  4u
#define BG_OVERLAY_B  16u
#define BG_OVERLAY_A   70u   /* 0-255 — reduced from 140; was too dark */

/* =========================================================================
 * Layout constants
 * ========================================================================= */
#define TOPBAR_H   28
#define DOCK_W     72
#define DOCK_PAD    8
#define BTN_SZ     52
#define BTN_GAP     8
#define CORNER_R    8

#define FONT_W      8
#define FONT_H     16
#define FONT2_W    16
#define FONT2_H    32

/* =========================================================================
 * UI State
 * ========================================================================= */
#define NUM_WORKSPACES 4
static int  g_ws        = 0;
static int  g_logged_in = 0;
static int  g_ticks     = 0;

/* ---- cursor ------------------------------------------------------------ */
static int g_cx = 0, g_cy = 0;

/* FIX [1]: raw mouse position tracking */
static int g_mouse_x_raw = -1;
static int g_mouse_y_raw = -1;

/* ---- wallpaper dirty flag (FIX [3]) ------------------------------------ */
static int      g_wallpaper_dirty = 1;
static uint32_t g_bg_overlay      = BG_OVERLAY_A;  /* adjustable overlay alpha */

/* ---- right-click menu -------------------------------------------------- */
static int g_ctx_open = 0, g_ctx_x = 0, g_ctx_y = 0;

typedef struct { const char *label; int id; } ctx_item_t;
#define CTX_CHANGE_BG  1
#define CTX_TERMINAL   2
#define CTX_ABOUT      3
#define CTX_LOGOUT     4
#define CTX_REBOOT     5
#define CTX_SHUTDOWN   6

static const ctx_item_t CTX_ITEMS[] = {
    { "  Change Background  ", CTX_CHANGE_BG },
    { "  Open Terminal      ", CTX_TERMINAL  },
    { "  About DracolaxOS   ", CTX_ABOUT     },
    { "  Log Out            ", CTX_LOGOUT    },
    { "  Restart            ", CTX_REBOOT    },
    { "  Shut Down          ", CTX_SHUTDOWN  },
};
#define CTX_COUNT 6
#define CTX_W   200
#define CTX_IH   26

/* ---- dock buttons ------------------------------------------------------ */
typedef struct { const char *icon; const char *label; uint32_t accent; } dock_btn_t;
static const dock_btn_t DOCK_BTNS[] = {
    { "[>]", "Start",  0x7828C8u },
    { "[/]", "Search", 0x2878C8u },
    { ">_ ", "Term",   0x28C878u },
    { "[i]", "Info",   0xC8A020u },
};
#define DOCK_BTN_COUNT 4

/* ---- overlay state (FIX [4]) ------------------------------------------ */
static int g_about_open  = 0;
static int g_start_open  = 0;
static int g_search_open = 0;

#define SEARCH_BUF 64
static char g_search_query[SEARCH_BUF] = "";

/* ---- boot logo phase --------------------------------------------------- */
static int g_boot_phase = 0, g_boot_ticks = 0;
#define BOOT_LOGO_FRAMES 60
#define BOOT_FADE_FRAMES 20

/* ---- login input ------------------------------------------------------- */
#define LOGIN_BUF 64
static char g_login_user[LOGIN_BUF] = "root";
static char g_login_pass[LOGIN_BUF] = "";
static int  g_login_field   = 1;
static char g_login_msg[128] = "";
static int  g_login_msg_err  = 0;

/* =========================================================================
 * FIX [3]: Fast scaled bg blit via shadow-buffer pointer
 * Nearest-neighbour scales bg_pixels[BG_W×BG_H] into any dest rect.
 * An additive dark overlay makes panels readable over the image.
 * ========================================================================= */
static void blit_bg_scaled(uint32_t dx0, uint32_t dy0,
                            uint32_t dw,  uint32_t dh, uint32_t overlay_a) {
    uint32_t  W      = fb.width;
    uint32_t *shadow = fb_shadow_ptr();
    if (!shadow || dw == 0 || dh == 0) return;

    for (uint32_t dy = 0; dy < dh; dy++) {
        uint32_t        sy      = dy * BG_H / dh;
        if (sy >= BG_H) sy = BG_H - 1;
        const uint32_t *src_row = bg_pixels + sy * BG_W;
        uint32_t       *dst_row = shadow + (dy0 + dy) * W + dx0;

        for (uint32_t dx = 0; dx < dw; dx++) {
            uint32_t sx = dx * BG_W / dw;
            if (sx >= BG_W) sx = BG_W - 1;
            uint32_t raw = src_row[sx];
            uint8_t r = (uint8_t)((raw >> 16) & 0xFF);
            uint8_t g = (uint8_t)((raw >>  8) & 0xFF);
            uint8_t b = (uint8_t)( raw        & 0xFF);
            /* Blend toward dark overlay colour */
            uint8_t a = (overlay_a > 255u) ? 255u : (uint8_t)overlay_a;
            r = (uint8_t)((r*(255-a) + BG_OVERLAY_R*a) / 255);
            g = (uint8_t)((g*(255-a) + BG_OVERLAY_G*a) / 255);
            b = (uint8_t)((b*(255-a) + BG_OVERLAY_B*a) / 255);
            dst_row[dx] = fb_color(r, g, b);
        }
    }
}

/* =========================================================================
 * Drawing helpers
 * ========================================================================= */
static void hline(uint32_t x, uint32_t y, uint32_t w, uint32_t c) {
    fb_fill_rect(x, y, w, 1, c);
}

/* FIX [2]: fill_col parameter eliminates the square fb_fill_rect() that was
 * overwriting the rounded corners drawn by fb_rounded_rect() inside. */
static void glass_panel(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         uint32_t border_col, uint32_t fill_col) {
    uint32_t ir = CORNER_R > 2 ? CORNER_R - 2 : 1;
    fb_rounded_rect(x+1, y+1, w-2, h-2, ir, fill_col); /* rounded inner fill */

    /* Border as thin lines — does not clip rounded inner corners */
    hline(x+CORNER_R,   y,       w-2*CORNER_R, border_col);
    hline(x+CORNER_R,   y+h-1,   w-2*CORNER_R, border_col);
    fb_fill_rect(x,       y+CORNER_R, 1, h-2*CORNER_R, border_col);
    fb_fill_rect(x+w-1,   y+CORNER_R, 1, h-2*CORNER_R, border_col);
    for (uint32_t i = 1; i <= CORNER_R; i++) {
        fb_put_pixel(x+CORNER_R-i,       y+CORNER_R-i,       border_col);
        fb_put_pixel(x+w-CORNER_R+i-1,   y+CORNER_R-i,       border_col);
        fb_put_pixel(x+CORNER_R-i,       y+h-CORNER_R+i-1,   border_col);
        fb_put_pixel(x+w-CORNER_R+i-1,   y+h-CORNER_R+i-1,   border_col);
    }
    hline(x+CORNER_R, y,     w-2*CORNER_R, COL_GLASS_SHINE); /* top shine */
    hline(x+CORNER_R, y+h-1, w-2*CORNER_R, COL_ACCENT_DIM);  /* bottom shadow */
}

static void text_center(uint32_t rx, uint32_t ry, uint32_t rw,
                          const char *s, uint32_t fg, uint32_t bg_col) {
    uint32_t tw = (uint32_t)strlen(s) * FONT_W;
    uint32_t tx = (tw < rw) ? rx + (rw-tw)/2 : rx;
    fb_print(tx, ry, s, fg, bg_col);
}

/* =========================================================================
 * Wallpaper  (FIX [3])
 * ========================================================================= */
static void draw_wallpaper(void) {
    if (!g_wallpaper_dirty) return;
    uint32_t W = fb.width, H = fb.height;
    blit_bg_scaled(DOCK_W, TOPBAR_H, W-DOCK_W, H-TOPBAR_H, g_bg_overlay);
    g_wallpaper_dirty = 0;
}

/* =========================================================================
 * Top bar
 * ========================================================================= */
static void draw_topbar(void) {
    uint32_t W = fb.width;
    fb_fill_rect(0, 0, W, TOPBAR_H, COL_TOPBAR);
    hline(0, TOPBAR_H-1, W, COL_SEP);
    hline(0, 0, W, COL_GLASS_SHINE);
    fb_print(DOCK_W+10, 6, "DracolaxOS V1", COL_TEXT_HI, COL_TOPBAR);

    uint32_t dot_x = W/2 - (uint32_t)NUM_WORKSPACES*14/2;
    for (int i=0;i<NUM_WORKSPACES;i++) {
        uint32_t dc = (i==g_ws)?COL_ACCENT_LT:COL_TEXT_DIM;
        fb_rounded_rect(dot_x+(uint32_t)i*14, 10, 8, 8, 3, dc);
        if (i==g_ws) fb_fill_rect(dot_x+(uint32_t)i*14+1, 11, 6, 6, dc);
    }

    const char *who = g_logged_in ? dracoauth_whoami() : "guest";
    /* FIX 3.13: real wall clock — RTC read each topbar refresh */
    rtc_time_t _dt;
    rtc_read(&_dt);
    char _clock[24];
    snprintf(_clock, sizeof(_clock), "%02u:%02u:%02u  |  %s",
             _dt.hour, _dt.min, _dt.sec, who);
    uint32_t rw = (uint32_t)strlen(_clock)*FONT_W;
    fb_print(W > rw+12 ? W-rw-12 : 0u, 6, _clock, COL_TEXT_MED, COL_TOPBAR);
}

/* =========================================================================
 * Left dock  (FIX [2]: no fb_fill_rect overwrite; FIX [4]: active states)
 * ========================================================================= */
static void draw_dock(void) {
    uint32_t H = fb.height;
    fb_fill_rect(0, TOPBAR_H, DOCK_W, H-TOPBAR_H, COL_DOCK);
    hline(0, TOPBAR_H, DOCK_W, COL_SEP);   /* BUG FIX: was H-TOPBAR_H (height), must be DOCK_W (width) */
    for (int i=0;i<3;i++)
        fb_fill_rect(DOCK_W-1-i, TOPBAR_H, 1, H-TOPBAR_H,
                     fb_blend(COL_GLASS_EDGE, COL_DOCK, (uint8_t)(80-i*25)));

    int cx=g_cx, cy=g_cy;
    uint32_t btn_x = (DOCK_W-BTN_SZ)/2;
    for (int i=0;i<DOCK_BTN_COUNT;i++) {
        uint32_t btn_y = TOPBAR_H+DOCK_PAD+(uint32_t)i*(BTN_SZ+BTN_GAP);
        int hover  = (cx>=(int)btn_x && cx<(int)(btn_x+BTN_SZ) &&
                      cy>=(int)btn_y  && cy<(int)(btn_y+BTN_SZ));
        int active = (i==0 && g_start_open) || (i==1 && g_search_open);
        uint32_t ac       = DOCK_BTNS[i].accent;
        uint32_t fill_col = (hover||active) ? fb_blend(ac,COL_DOCK,80) : COL_GLASS_PANEL;
        uint32_t bord_col = (active||hover) ? COL_ACCENT_LT : COL_GLASS_EDGE;

        /* FIX [2]: fill passed directly — rounded corners no longer overwritten */
        glass_panel(btn_x, btn_y, BTN_SZ, BTN_SZ, bord_col, fill_col);

        uint32_t icon_w = (uint32_t)strlen(DOCK_BTNS[i].icon)*FONT2_W;
        uint32_t icon_x = btn_x + (BTN_SZ>icon_w?(BTN_SZ-icon_w)/2:0);
        fb_print_s(icon_x, btn_y+6, DOCK_BTNS[i].icon,
                   (hover||active)?COL_TEXT_HI:ac, fill_col, 2);
        uint32_t lbl_w = (uint32_t)strlen(DOCK_BTNS[i].label)*FONT_W;
        uint32_t lbl_x = btn_x + (BTN_SZ>lbl_w?(BTN_SZ-lbl_w)/2:0);
        fb_print(lbl_x, btn_y+BTN_SZ-FONT_H-4, DOCK_BTNS[i].label,
                 (hover||active)?COL_TEXT_HI:COL_TEXT_MED, fill_col);
    }

    /* Workspace switcher — 20px buttons, 4px gap for easier clicking */
#define WS_BTN_H  20
#define WS_BTN_GAP 4
    uint32_t ws_total = (uint32_t)NUM_WORKSPACES*(WS_BTN_H+WS_BTN_GAP);
    uint32_t ws_start = H > ws_total+16u ? H - ws_total - 8u : H/2;
    for (int i=0;i<NUM_WORKSPACES;i++) {
        uint32_t wy = ws_start+(uint32_t)i*(WS_BTN_H+WS_BTN_GAP);
        uint32_t wc = (i==g_ws)?COL_ACCENT:COL_GLASS_PANEL;
        fb_fill_rect(8, wy, DOCK_W-16, WS_BTN_H, wc);
        fb_rounded_rect(8, wy, DOCK_W-16, WS_BTN_H, 3, (i==g_ws)?COL_ACCENT_LT:COL_GLASS_EDGE);
        char nb[4]; snprintf(nb, sizeof(nb), "%d", i+1);
        text_center(8, wy+(WS_BTN_H-FONT_H)/2, DOCK_W-16, nb,
                    (i==g_ws)?COL_TEXT_HI:COL_TEXT_MED, wc);
    }
}

/* =========================================================================
 * Context menu  (FIX [2]: fill_col param; FIX [5]: terminal wired)
 * ========================================================================= */
static void draw_ctx_menu(void) {
    if (!g_ctx_open) return;
    uint32_t mh = (uint32_t)CTX_COUNT*CTX_IH+10;
    uint32_t mx = (uint32_t)g_ctx_x, my = (uint32_t)g_ctx_y;
    if (mx+CTX_W > fb.width)  mx = fb.width  - CTX_W;
    if (my+mh    > fb.height) my = fb.height - mh;
    glass_panel(mx, my, CTX_W, mh, COL_GLASS_EDGE, COL_GLASS_PANEL);
    fb_fill_rect(mx+2, my+mh, CTX_W, 3, fb_blend(0x000000,COL_VOID,80));
    for (int i=0;i<CTX_COUNT;i++) {
        uint32_t iy = my+5+(uint32_t)i*CTX_IH;
        int hover = (g_cx>=(int)mx && g_cx<(int)(mx+CTX_W) &&
                     g_cy>=(int)iy  && g_cy<(int)(iy+CTX_IH));
        if (hover) fb_fill_rect(mx+2, iy, CTX_W-4, CTX_IH, COL_ACCENT_DIM);
        fb_print(mx+6, iy+5, CTX_ITEMS[i].label,
                 hover?COL_TEXT_HI:COL_TEXT_MED, 0);
        if (i<CTX_COUNT-1) hline(mx+4, iy+CTX_IH-1, CTX_W-8, COL_SEP);
    }
}

/* =========================================================================
 * About overlay  (FIX [2]: no fb_fill_rect overwrite)
 * ========================================================================= */
static void draw_about(void) {
    if (!g_about_open) return;
    uint32_t W=fb.width, H=fb.height;
    uint32_t pw = (W > 560u) ? 480u : W - 40u;
    uint32_t ph = (H > 340u) ? 280u : H - 60u;
    uint32_t px=(W-pw)/2, py=(H-ph)/2;
    fb_fill_rect(0, 0, W, H, fb_blend(0x000000,0,160));
    glass_panel(px, py, pw, ph, COL_ACCENT, COL_GLASS_PANEL);
    fb_print_s(px+20, py+16, "DracolaxOS V1", COL_ACCENT_LT, COL_GLASS_PANEL, 2);
    hline(px+16, py+54, pw-32, COL_SEP);
    const char *lines[] = {
        "  Kernel : Draco-1.0 x86_64 (64-bit preemptive)",
        "  ABI    : Draco native + Linux x86_64 (SYSCALL)",
        "  GUI    : Glassmorphism compositor",
        "  Memory : PMM bitmap + bump-allocator heap",
        "  FS     : RAMFS + VFS mount tree",
        "  Author : Lunax (Yunis) + Amilcar",
        "  Fonts  : Exo 2, Orbitron, Future Z",
        "",
        "  Press  ESC  or click outside to close",
    };
    for (int i=0;i<9;i++)
        fb_print(px+16, py+62+(uint32_t)i*20, lines[i],
                 i==8?COL_TEXT_DIM:COL_TEXT_MED, 0);
    fb_fill_rect(px+pw-26, py+8, 18, 18, COL_ERR);
    fb_rounded_rect(px+pw-26, py+8, 18, 18, 3, 0xFF8080u);
    fb_print(px+pw-23, py+11, "X", COL_TEXT_HI, COL_ERR);
}

/* =========================================================================
 * Start Menu — driven by appman (no hardcoded list; always in sync)
 * ========================================================================= */
/* Responsive cell sizing: larger on bigger screens */
#define START_COLS   3
/* Cell dimensions computed per-draw from fb size — see draw_start_menu() */

/* Appman name cache — filled once per open event, reused while menu is open */
#define AM_MAX_APPS  APP_MAX
#define AM_NAME_LEN  APP_NAME_LEN
static char  g_am_names[AM_MAX_APPS][AM_NAME_LEN];
static int   g_am_count = 0;

/* Refresh app list from appman — called when the menu opens */
static void am_refresh(void) {
    char buf[512];
    appman_list(buf, sizeof(buf));
    g_am_count = 0;
    char *p = buf;
    while (*p && g_am_count < AM_MAX_APPS) {
        while (*p == ' ') p++;
        if (!*p) break;
        char *ns = p;
        while (*p && *p != '[' && *p != '\n') p++;
        char *ne = p;
        while (ne > ns && *(ne-1) == ' ') ne--;
        int nl = (int)(ne - ns);
        if (nl > 0 && nl < AM_NAME_LEN) {
            memcpy(g_am_names[g_am_count], ns, (size_t)nl);
            g_am_names[g_am_count][nl] = '\0';
            g_am_count++;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
}

static void draw_start_menu(void) {
    if (!g_start_open) return;
    if (g_am_count == 0) am_refresh();
    if (g_am_count == 0) { g_start_open = 0; return; }

    /* Responsive cell size */
    uint32_t cell_w = (fb.width  > 900u) ? 116u : 96u;
    uint32_t cell_h = (fb.height > 600u) ?  86u : 70u;
    uint32_t cols   = START_COLS;
    uint32_t rows   = ((uint32_t)g_am_count + cols - 1u) / cols;
    uint32_t pw     = cols * cell_w + 24u;
    uint32_t ph     = rows * cell_h + 52u;
    /* Clamp to visible area */
    uint32_t avail_w = fb.width  - (uint32_t)DOCK_W - 20u;
    uint32_t avail_h = fb.height - (uint32_t)TOPBAR_H - 12u;
    if (pw > avail_w) pw = avail_w;
    if (ph > avail_h) ph = avail_h;
    uint32_t px = (uint32_t)DOCK_W + 8u;
    uint32_t py = (fb.height > ph + (uint32_t)TOPBAR_H + 8u)
                  ? fb.height - ph - 8u : (uint32_t)TOPBAR_H + 4u;

    glass_panel(px, py, pw, ph, COL_ACCENT, COL_GLASS_BG);
    fb_print_s(px + 10u, py + 8u, "Apps", COL_ACCENT_LT, COL_GLASS_BG, 1);
    hline(px + 8u, py + 28u, pw - 16u, COL_SEP);

    for (int i = 0; i < g_am_count; i++) {
        uint32_t col2 = (uint32_t)(i % (int)cols);
        uint32_t row2 = (uint32_t)(i / (int)cols);
        uint32_t cx2  = px + 12u + col2 * cell_w;
        uint32_t cy2  = py + 38u + row2 * cell_h;
        uint32_t bw   = cell_w - 8u;
        uint32_t bh   = cell_h - 8u;
        if (cx2 + bw > px + pw - 4u || cy2 + bh > py + ph - 4u) continue;
        int hover = (g_cx >= (int)cx2 && g_cx < (int)(cx2 + bw) &&
                     g_cy >= (int)cy2 && g_cy < (int)(cy2 + bh));
        uint32_t cf = hover ? COL_ACCENT_DIM : COL_GLASS_PANEL;
        glass_panel(cx2, cy2, bw, bh,
                    hover ? COL_ACCENT_LT : COL_GLASS_EDGE, cf);
        /* Two-character icon from app name initials */
        char icon[3] = {
            g_am_names[i][0],
            (g_am_names[i][1] ? g_am_names[i][1] : ' '),
            '\0'
        };
        fb_print_s(cx2 + 4u, cy2 + 5u, icon,
                   hover ? COL_TEXT_HI : COL_ACCENT_LT, cf, 2);
        uint32_t lw2 = (uint32_t)strlen(g_am_names[i]) * FONT_W;
        uint32_t lx2 = cx2 + (bw > lw2 ? (bw - lw2) / 2u : 2u);
        fb_print(lx2, cy2 + bh - FONT_H - 3u, g_am_names[i],
                 hover ? COL_TEXT_HI : COL_TEXT_MED, cf);
    }
}

/* =========================================================================
 * Search overlay — live app filtering
 * Results: apps whose names contain the query string (case-insensitive).
 * Enter on a single match launches it; Enter with no query closes.
 * ========================================================================= */

/* Case-insensitive strstr using only kernel klibc (no toupper dependency) */
static int search_match(const char *haystack, const char *needle) {
    if (!needle[0]) return 0;  /* empty query matches nothing */
    size_t nl = strlen(needle);
    for (size_t i = 0; haystack[i]; i++) {
        int ok = 1;
        for (size_t j = 0; j < nl && ok; j++) {
            char h = haystack[i+j];
            char n = needle[j];
            if (!h) { ok = 0; break; }
            /* cheap lowercase: A-Z → a-z */
            if (h >= 'A' && h <= 'Z') h += 32;
            if (n >= 'A' && n <= 'Z') n += 32;
            if (h != n) ok = 0;
        }
        if (ok) return 1;
    }
    return 0;
}

/* Search result scratch buffer */
#define SEARCH_MAX_RESULTS 8
static char  g_sr_names[SEARCH_MAX_RESULTS][AM_NAME_LEN];
static int   g_sr_count = 0;
static int   g_sr_sel   = 0;   /* keyboard-highlighted result */

static void search_update_results(void) {
    g_sr_count = 0;
    g_sr_sel   = 0;
    if (!g_search_query[0]) return;
    int total = appman_count();
    for (int i = 0; i < total && g_sr_count < SEARCH_MAX_RESULTS; i++) {
        const app_entry_t *a = appman_get(i);
        if (a && search_match(a->name, g_search_query)) {
            strncpy(g_sr_names[g_sr_count], a->name, AM_NAME_LEN - 1);
            g_sr_names[g_sr_count][AM_NAME_LEN - 1] = '\0';
            g_sr_count++;
        }
    }
}

static void draw_search_overlay(void) {
    if (!g_search_open) return;
    uint32_t W = fb.width;
    uint32_t pw = (W - (uint32_t)DOCK_W > 440u) ? 400u : W - (uint32_t)DOCK_W - 40u;
    uint32_t result_h = (g_sr_count > 0) ? (uint32_t)g_sr_count * 24u + 8u : 0u;
    uint32_t ph = 64u + result_h;
    uint32_t px = (uint32_t)DOCK_W + (W - (uint32_t)DOCK_W - pw) / 2u;
    uint32_t py = (uint32_t)TOPBAR_H + 12u;

    glass_panel(px, py, pw, ph, COL_ACCENT, COL_GLASS_PANEL);

    /* Search icon */
    fb_print(px + 10u, py + 20u, "[/]", COL_ACCENT_LT, COL_GLASS_PANEL);

    /* Input field */
    uint32_t fx = px + 40u, fw = pw - 56u;
    fb_fill_rect(fx, py + 14u, fw, 28u, 0x0C0E22u);
    fb_rounded_rect(fx, py + 14u, fw, 28u, 4u, COL_ACCENT_LT);
    fb_print(fx + 6u, py + 20u,
             g_search_query[0] ? g_search_query : "Type app name...",
             g_search_query[0] ? COL_TEXT_HI : COL_TEXT_DIM, 0u);
    if (g_ticks % 60 < 40) {
        uint32_t cx3 = fx + 6u + (uint32_t)strlen(g_search_query) * FONT_W;
        fb_fill_rect(cx3, py + 20u, 2u, FONT_H, COL_ACCENT_LT);
    }
    fb_print(px + pw - 90u, py + 20u, "ESC to close", COL_TEXT_DIM, COL_GLASS_PANEL);

    /* Results list */
    if (g_sr_count > 0) {
        hline(px + 8u, py + 56u, pw - 16u, COL_SEP);
        for (int i = 0; i < g_sr_count; i++) {
            uint32_t ry = py + 64u + (uint32_t)i * 24u;
            int hover = (g_cy >= (int)ry && g_cy < (int)(ry + 24u) &&
                         g_cx >= (int)px && g_cx < (int)(px + pw));
            int sel   = (i == g_sr_sel);
            if (hover || sel)
                fb_fill_rect(px + 2u, ry, pw - 4u, 24u, COL_ACCENT_DIM);
            fb_print(px + 14u, ry + 4u, g_sr_names[i],
                     (hover || sel) ? COL_TEXT_HI : COL_TEXT_MED,
                     (hover || sel) ? COL_ACCENT_DIM : COL_GLASS_PANEL);
        }
    } else if (g_search_query[0]) {
        hline(px + 8u, py + 56u, pw - 16u, COL_SEP);
        fb_print(px + 14u, py + 64u, "No matching apps", COL_TEXT_DIM, COL_GLASS_PANEL);
    }
}

/* =========================================================================
 * Login screen  (FIX [3]: bg_pixels background; FIX [2]: no fill_rect)
 * ========================================================================= */
static void draw_login(void) {
    uint32_t W=fb.width, H=fb.height;
    /* FIX [3]: actual bg image, heavier overlay for login contrast */
    blit_bg_scaled(0, 0, W, H, (g_bg_overlay > 80u) ? g_bg_overlay + 60u : 140u);
    fb_fill_rect(0, 0, W, H, fb_blend(0x000000, 0, 110));

    uint32_t cw = (W > 440u) ? 380u : W - 40u;
    uint32_t ch = (H > 380u) ? 320u : H - 40u;
    uint32_t cx=(W-cw)/2, cy=(H-ch)/2;
    /* FIX [2]: fill_col param — rounded corners preserved */
    glass_panel(cx, cy, cw, ch, COL_ACCENT, COL_GLASS_PANEL);
    fb_print_s(cx+60, cy+20, "DracolaxOS", COL_ACCENT_LT, COL_GLASS_PANEL, 2);
    hline(cx+16, cy+62, cw-32, COL_SEP);
    fb_print(cx+16, cy+72, "Sign in to your account", COL_TEXT_MED, 0);

    int uf=(g_login_field==0);
    uint32_t fy0=cy+104;
    fb_print(cx+16, fy0, "Username", COL_TEXT_DIM, 0);
    fb_fill_rect(cx+16, fy0+18, cw-32, 28, uf?0x1E1E40u:0x141430u);
    fb_rounded_rect(cx+16, fy0+18, cw-32, 28, 4, uf?COL_ACCENT_LT:COL_SEP);
    fb_print(cx+22, fy0+24, g_login_user, COL_TEXT_HI, 0);
    if (uf && g_ticks%60<40) {
        uint32_t cx4=cx+22+(uint32_t)strlen(g_login_user)*FONT_W;
        fb_fill_rect(cx4, fy0+24, 2, FONT_H, COL_ACCENT_LT);
    }

    int pf=(g_login_field==1);
    uint32_t fy1=fy0+64;
    fb_print(cx+16, fy1, "Password", COL_TEXT_DIM, 0);
    fb_fill_rect(cx+16, fy1+18, cw-32, 28, pf?0x1E1E40u:0x141430u);
    fb_rounded_rect(cx+16, fy1+18, cw-32, 28, 4, pf?COL_ACCENT_LT:COL_SEP);
    char stars[LOGIN_BUF]; size_t plen=strlen(g_login_pass);
    for (size_t si=0;si<plen&&si<LOGIN_BUF-1;si++) stars[si]='*';
    stars[plen]='\0';
    fb_print(cx+22, fy1+24, stars, COL_TEXT_HI, 0);
    if (pf && g_ticks%60<40) {
        uint32_t cx4=cx+22+(uint32_t)plen*FONT_W;
        fb_fill_rect(cx4, fy1+24, 2, FONT_H, COL_ACCENT_LT);
    }

    uint32_t bx=cx+16, by=fy1+60, bw=cw-32, bh=32;
    fb_fill_rect(bx, by, bw, bh, COL_ACCENT);
    fb_rounded_rect(bx, by, bw, bh, 6, COL_ACCENT_LT);
    text_center(bx, by+8, bw, "Log In", COL_TEXT_HI, COL_ACCENT);

    if (g_login_msg[0]) {
        uint32_t mc=g_login_msg_err?COL_ERR:COL_OK;
        uint32_t mx2=cx+(cw-(uint32_t)strlen(g_login_msg)*FONT_W)/2;
        fb_print(mx2, by+40, g_login_msg, mc, 0);
    }
    fb_print(cx+16, cy+ch-22,
             "Tab = switch field   Enter = login", COL_TEXT_DIM, 0);
}

/* =========================================================================
 * Login input handler
 * ========================================================================= */
static void handle_login_key(char c) {
    if (c=='\t') { g_login_field=1-g_login_field; return; }
    if (c=='\n') {
        int r=dracoauth_login(g_login_user, g_login_pass);
        if (r==0) {
            g_logged_in=1; g_wallpaper_dirty=1;
            snprintf(g_login_msg, sizeof(g_login_msg),
                     "Welcome, %s!", dracoauth_whoami());
            g_login_msg_err=0;
        } else {
            snprintf(g_login_msg, sizeof(g_login_msg), "Invalid credentials.");
            g_login_msg_err=1; g_login_pass[0]='\0';
        }
        return;
    }
    if (c=='\b') {
        char *buf=(g_login_field==0)?g_login_user:g_login_pass;
        size_t l=strlen(buf); if (l>0) buf[l-1]='\0'; return;
    }
    if (c>=32 && c<127) {
        char *buf=(g_login_field==0)?g_login_user:g_login_pass;
        size_t l=strlen(buf);
        if (l<LOGIN_BUF-1) { buf[l]=c; buf[l+1]='\0'; }
    }
}

/* =========================================================================
 * Desktop interaction  (FIX [4][5])
 * ========================================================================= */
static void close_overlays(void) {
    g_start_open=0; g_search_open=0;
}

static void handle_desktop_click(int cx, int cy, int right) {
    if (right) {
        if (g_ctx_open) { g_ctx_open=0; return; }
        close_overlays();
        if (cx>=DOCK_W && cy>=TOPBAR_H) {
            g_ctx_open=1; g_ctx_x=cx; g_ctx_y=cy;
        }
        return;
    }

    /* Left: context menu */
    if (g_ctx_open) {
        uint32_t mh=(uint32_t)CTX_COUNT*CTX_IH+10;
        uint32_t mx=(uint32_t)g_ctx_x, my=(uint32_t)g_ctx_y;
        if (mx+CTX_W>fb.width)  mx=fb.width-CTX_W;
        if (my+mh>fb.height)    my=fb.height-mh;
        for (int i=0;i<CTX_COUNT;i++) {
            uint32_t iy=my+5+(uint32_t)i*CTX_IH;
            if (cx>=(int)mx && cx<(int)(mx+CTX_W) &&
                cy>=(int)iy  && cy<(int)(iy+CTX_IH)) {
                switch(CTX_ITEMS[i].id) {
                case CTX_ABOUT:    g_about_open=1; close_overlays(); break;
                case CTX_LOGOUT:
                    g_logged_in=0; g_login_pass[0]='\0';
                    g_wallpaper_dirty=1; close_overlays(); g_about_open=0; break;
                case CTX_TERMINAL: close_overlays(); appman_launch("Terminal"); break;
                /* CTX_CHANGE_BG: cycle overlay alpha (darker→lighter→off→darker) */
                case CTX_CHANGE_BG:
                    if (g_bg_overlay > 180u) g_bg_overlay = 90u;
                    else if (g_bg_overlay > 40u) g_bg_overlay = 10u;
                    else g_bg_overlay = 200u;
                    g_wallpaper_dirty = 1;
                    break;
                case CTX_REBOOT:   power_reboot();   break;
                case CTX_SHUTDOWN: power_shutdown(); break;
                }
            }
        }
        g_ctx_open=0; return;
    }

    /* About close button */
    if (g_about_open) {
        uint32_t W=fb.width,H=fb.height,pw=480,ph=280;
        uint32_t px=(W-pw)/2, py=(H-ph)/2;
        if (cx>=(int)(px+pw-26) && cx<(int)(px+pw-8) &&
            cy>=(int)(py+8)     && cy<(int)(py+26))
            { g_about_open=0; g_wallpaper_dirty=1; }
        return;
    }

    /* Start-menu app clicks — launch via appman */
    if (g_start_open) {
        uint32_t cell_w = (fb.width  > 900u) ? 116u : 96u;
        uint32_t cell_h = (fb.height > 600u) ?  86u : 70u;
        uint32_t cols   = START_COLS;
        uint32_t rows   = ((uint32_t)g_am_count + cols - 1u) / cols;
        uint32_t pw     = cols * cell_w + 24u;
        uint32_t ph     = rows * cell_h + 52u;
        uint32_t avail_w = fb.width  - (uint32_t)DOCK_W - 20u;
        uint32_t avail_h = fb.height - (uint32_t)TOPBAR_H - 12u;
        if (pw > avail_w) pw = avail_w;
        if (ph > avail_h) ph = avail_h;
        uint32_t px = (uint32_t)DOCK_W + 8u;
        uint32_t py = (fb.height > ph + (uint32_t)TOPBAR_H + 8u)
                      ? fb.height - ph - 8u : (uint32_t)TOPBAR_H + 4u;
        int clicked = 0;
        for (int i = 0; i < g_am_count; i++) {
            uint32_t col2 = (uint32_t)(i % (int)cols);
            uint32_t row2 = (uint32_t)(i / (int)cols);
            uint32_t acx  = px + 12u + col2 * cell_w;
            uint32_t acy  = py + 38u + row2 * cell_h;
            uint32_t bw   = cell_w - 8u;
            uint32_t bh   = cell_h - 8u;
            if (cx >= (int)acx && cx < (int)(acx + bw) &&
                cy >= (int)acy && cy < (int)(acy + bh)) {
                appman_launch(g_am_names[i]);   /* launch via appman — wires ALL entries */
                clicked = 1;
                break;
            }
        }
        (void)clicked;
        /* Click outside closes menu */
        if (cx < (int)px || cx >= (int)(px + pw) ||
            cy < (int)py || cy >= (int)(py + ph)) {
            g_am_count = 0;   /* force refresh next open */
        }
        g_start_open = 0; g_wallpaper_dirty = 1;
        return;
    }

    /* Search overlay clicks */
    if (g_search_open) {
        uint32_t W2 = fb.width;
        uint32_t pw = (W2 - (uint32_t)DOCK_W > 440u) ? 400u : W2 - (uint32_t)DOCK_W - 40u;
        uint32_t result_h = (g_sr_count > 0) ? (uint32_t)g_sr_count * 24u + 8u : 0u;
        uint32_t ph = 64u + result_h;
        uint32_t px = (uint32_t)DOCK_W + (W2 - (uint32_t)DOCK_W - pw) / 2u;
        uint32_t py = (uint32_t)TOPBAR_H + 12u;
        /* Click on a result row — launch that app */
        if (g_sr_count > 0 &&
            cx >= (int)px && cx < (int)(px + pw) &&
            cy >= (int)(py + 64u) && cy < (int)(py + ph)) {
            int row = ((int)cy - (int)(py + 64u)) / 24;
            if (row >= 0 && row < g_sr_count)
                appman_launch(g_sr_names[row]);
            g_search_open = 0; g_search_query[0] = '\0';
            g_sr_count = 0; g_wallpaper_dirty = 1;
            return;
        }
        /* Click outside overlay closes it */
        if (cx < (int)px || cx >= (int)(px + pw) ||
            cy < (int)py || cy >= (int)(py + ph)) {
            g_search_open = 0; g_search_query[0] = '\0';
            g_sr_count = 0; g_wallpaper_dirty = 1;
        }
        return;
    }

    /* FIX [4]: Dock button actions */
    uint32_t btn_x=(DOCK_W-BTN_SZ)/2;
    for (int i=0;i<DOCK_BTN_COUNT;i++) {
        uint32_t btn_y=TOPBAR_H+DOCK_PAD+(uint32_t)i*(BTN_SZ+BTN_GAP);
        if (cx>=(int)btn_x && cx<(int)(btn_x+BTN_SZ) &&
            cy>=(int)btn_y  && cy<(int)(btn_y+BTN_SZ)) {
            switch(i) {
            case 0: g_start_open=!g_start_open; g_search_open=0;
                    if (!g_start_open) g_am_count=0;  /* force refresh on next open */
                    g_wallpaper_dirty=1; break;
            case 1: g_search_open=!g_search_open; g_start_open=0;
                    g_search_query[0]='\0'; g_wallpaper_dirty=1; break;
            case 2:
                /* FIX: launch proper Terminal app via appman, not debug console */
                close_overlays();
                appman_launch("Terminal");
                break;
            case 3: g_about_open=1; close_overlays(); break;
            }
        }
    }

    /* Workspace switcher click — geometry matches draw_dock */
    uint32_t ws_total2 = (uint32_t)NUM_WORKSPACES*(20+4);
    uint32_t ws_start2 = fb.height > ws_total2+16u ? fb.height - ws_total2 - 8u : fb.height/2;
    for (int i=0;i<NUM_WORKSPACES;i++) {
        uint32_t wy2=ws_start2+(uint32_t)i*(20+4);
        if (cx>=4&&cx<DOCK_W-4&&cy>=(int)wy2&&cy<(int)(wy2+20))
            { g_ws=i; g_wallpaper_dirty=1; wm_switch_desktop(g_ws); comp_switch_desktop(g_ws); }
    }
}

/* =========================================================================
 * Boot logo
 * ========================================================================= */
static void draw_boot_logo(int fade_alpha) {
    uint32_t W=fb.width, H=fb.height;
    fb_clear(COL_VOID);
    uint32_t cx=W/2, cy=H/2, sz=(H<W?H:W)/5;
    uint32_t box=sz*2, ox=cx-sz, oy=cy-sz-20;
    for (uint32_t py=0;py<box;py++) {
        for (uint32_t px=0;px<box;px++) {
            uint32_t bx=px*DRACO_LOGO_W/box, by2=py*DRACO_LOGO_H_PX/box;
            uint32_t bidx=by2*DRACO_LOGO_W+bx;
            uint8_t byte=draco_logo_1bpp[bidx>>3];
            uint8_t bit=(byte>>(7-(bidx&7)))&1;
            if (!bit) continue;
            int dx=(int)px-(int)sz, dy2=(int)py-(int)sz;
            uint32_t d2=(uint32_t)(dx*dx+dy2*dy2), m2=sz*sz;
            uint8_t blend=(d2<m2)?(uint8_t)(200+55*(m2-d2)/m2):200u;
            uint32_t col=fb_blend(0xFFFFFFu,0xAA66FFu,blend);
            if (fade_alpha<255) {
                uint8_t a=(uint8_t)fade_alpha;
                col=fb_color((uint8_t)(((col>>16)&0xFF)*a/255),
                             (uint8_t)(((col>>8 )&0xFF)*a/255),
                             (uint8_t)((col&0xFF)*a/255));
            }
            if (ox+px<W && oy+py<H) fb_put_pixel(ox+px, oy+py, col);
        }
    }
    uint32_t col_txt=COL_TEXT_HI;
    if (fade_alpha<255) {
        uint8_t a=(uint8_t)fade_alpha;
        col_txt=fb_color((uint8_t)(0xF0*a/255),(uint8_t)(0xF0*a/255),(uint8_t)(0xFF*a/255));
    }
    const char *name="DracolaxOS";
    uint32_t nw=(uint32_t)strlen(name)*FONT2_W;
    fb_print_s((W-nw)/2, oy+box+16, name, col_txt, 0, 2);
    uint32_t col_ver=COL_TEXT_DIM;
    if (fade_alpha<255) {
        uint8_t a=(uint8_t)fade_alpha;
        col_ver=fb_color((uint8_t)(0x60*a/255),(uint8_t)(0x60*a/255),(uint8_t)(0x7A*a/255));
    }
    const char *ver="v1.0";
    uint32_t vw=(uint32_t)strlen(ver)*FONT_W;
    fb_print((W-vw)/2, oy+box+16+FONT2_H+6, ver, col_ver, 0);
}

/* =========================================================================
 * Desktop main task
 * ========================================================================= */
void desktop_task(void) {
    __asm__ volatile ("sti");

    /* FIX [5]: Lock fb_console BEFORE shadow-buffer ownership so no
     * klog / irq-watchdog output bleeds into the GUI framebuffer. */
    fb_console_lock(1);
    kinfo("DESKTOP: starting glassmorphism desktop\n");

    if (!fb.available) {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        vga_print("\n    DracolaxOS V1 — No VESA framebuffer.\n");
        vga_print("    Use mode=graphical in GRUB entry.\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        for (;;) sched_sleep(1000);
    }

    fb_enable_shadow();
    fb_clear(COL_VOID);
    fb_flip();
    cursor_init();   /* initialise kernel cursor driver */

    /* First-boot installer check */
    {
        int need_setup=(dracoauth_login("__probe__","")!=0 &&
                        dracoauth_login("root","dracolax")==0);
        dracoauth_logout();
        if (need_setup) { kinfo("DESKTOP: first boot — installer\n"); installer_run(); }
    }

    g_cx=(int)fb.width/2;
    g_cy=(int)fb.height/2;

    for (;;) {
        g_ticks++;

        /* Boot phase */
        if (g_boot_phase<2) {
            g_boot_ticks++;
            if (g_boot_phase==0) {
                draw_boot_logo(255); fb_flip();
                if (g_boot_ticks>=BOOT_LOGO_FRAMES){g_boot_phase=1;g_boot_ticks=0;}
            } else {
                int alpha=255-g_boot_ticks*255/BOOT_FADE_FRAMES;
                if (alpha<0) alpha=0;
                draw_boot_logo(alpha); fb_flip();
                if (g_boot_ticks>=BOOT_FADE_FRAMES){
                    g_boot_phase=2;g_boot_ticks=0;
                    fb_clear(COL_VOID);fb_flip();g_wallpaper_dirty=1;
                }
            }
            sched_sleep(33); continue;
        }

        /* Input */
        vmmouse_poll();
        mouse_update_edges();

        /* FIX [1]: Only update cursor from mouse on actual movement */
        int new_mx=mouse_get_x(), new_my=mouse_get_y();
        if (new_mx!=g_mouse_x_raw || new_my!=g_mouse_y_raw) {
            g_cx=new_mx; g_cy=new_my;
            g_mouse_x_raw=new_mx; g_mouse_y_raw=new_my;
        }

        int c;
        while ((c=keyboard_getchar())!=0) {
            uint8_t uc=(uint8_t)c;
            /* FIX [1]: arrow keys update raw tracker so next mouse move
             * starts from the keyboard-adjusted position, not the old one */
            if      (uc==KB_KEY_UP)    {g_cy-=4;if(g_cy<0)g_cy=0;g_mouse_x_raw=g_cx;g_mouse_y_raw=g_cy;}
            else if (uc==KB_KEY_DOWN)  {g_cy+=4;if(g_cy>=(int)fb.height)g_cy=(int)fb.height-1;g_mouse_x_raw=g_cx;g_mouse_y_raw=g_cy;}
            else if (uc==KB_KEY_LEFT)  {g_cx-=4;if(g_cx<0)g_cx=0;g_mouse_x_raw=g_cx;g_mouse_y_raw=g_cy;}
            else if (uc==KB_KEY_RIGHT) {g_cx+=4;if(g_cx>=(int)fb.width)g_cx=(int)fb.width-1;g_mouse_x_raw=g_cx;g_mouse_y_raw=g_cy;}
            else if (uc==KB_KEY_ALTTAB){g_ws=(g_ws+1)%NUM_WORKSPACES;g_wallpaper_dirty=1;wm_switch_desktop(g_ws);comp_switch_desktop(g_ws);}
            else if (uc==KB_KEY_F12)   {dbgcon_toggle();}
            else if (!g_logged_in)     {handle_login_key((char)c);}
            else {
                if (c==0x1B) {
                    if (g_about_open)       {g_about_open=0;g_wallpaper_dirty=1;}
                    else if (g_ctx_open)    {g_ctx_open=0;}
                    else if (g_start_open)  {g_start_open=0;g_wallpaper_dirty=1;}
                    else if (g_search_open) {g_search_open=0;g_search_query[0]='\0';g_sr_count=0;g_wallpaper_dirty=1;}
                }
                if (g_search_open && c >= 32 && c < 127) {
                    size_t l = strlen(g_search_query);
                    if (l < SEARCH_BUF - 1) {
                        g_search_query[l] = (char)c;
                        g_search_query[l+1] = '\0';
                    }
                    search_update_results();
                } else if (g_search_open && c == '\b') {
                    size_t l = strlen(g_search_query);
                    if (l > 0) g_search_query[l - 1] = '\0';
                    search_update_results();
                } else if (g_search_open && (c == '\n' || c == '\r')) {
                    /* Enter: launch selected (or first) result */
                    int target = (g_sr_sel < g_sr_count) ? g_sr_sel :
                                 (g_sr_count > 0 ? 0 : -1);
                    if (target >= 0) appman_launch(g_sr_names[target]);
                    g_search_open = 0;
                    g_search_query[0] = '\0';
                    g_sr_count = 0;
                    g_wallpaper_dirty = 1;
                } else if (g_search_open && (uint8_t)c == KB_KEY_DOWN) {
                    if (g_sr_sel < g_sr_count - 1) g_sr_sel++;
                } else if (g_search_open && (uint8_t)c == KB_KEY_UP) {
                    if (g_sr_sel > 0) g_sr_sel--;
                }
                if (c>=1 && c<=4) {g_ws=c-1;g_wallpaper_dirty=1;wm_switch_desktop(g_ws);comp_switch_desktop(g_ws);}
            }
        }

        /* Mouse clicks */
        /* BUG FIX 1.3: clamp cursor to screen bounds before any click dispatch,
         * guards against edge values from raw mouse_get_x/y or arrow key wrap. */
        if (g_cx < 0) g_cx = 0;
        if (g_cy < 0) g_cy = 0;
        if (g_cx >= (int)fb.width)  g_cx = (int)fb.width  - 1;
        if (g_cy >= (int)fb.height) g_cy = (int)fb.height - 1;

        if (mouse_btn_pressed(MOUSE_BTN_LEFT)) {
            if (!g_logged_in) {
                /* FIX 1.2: use the same responsive formula as draw_login()
                 * so the hit-box always matches the rendered button position. */
                uint32_t W=fb.width, H=fb.height;
                uint32_t cw=(W>440u)?380u:W-40u;
                uint32_t ch=(H>380u)?320u:H-40u;
                uint32_t lcx=(W-cw)/2, lcy=(H-ch)/2;
                uint32_t fy1=lcy+104u+64u;          /* same offsets as draw_login */
                uint32_t bx=lcx+16, by=fy1+60, bw=cw-32, bh=32;
                (void)ch;
                if (g_cx>=(int)bx&&g_cx<(int)(bx+bw)&&g_cy>=(int)by&&g_cy<(int)(by+bh))
                    handle_login_key('\n');
            } else { handle_desktop_click(g_cx,g_cy,0); }
        }
        if (mouse_btn_pressed(MOUSE_BTN_RIGHT)) {
            if (g_logged_in) handle_desktop_click(g_cx,g_cy,1);
        }

        /* Draw */
        if (!g_logged_in) {
            draw_login();
        } else {
            draw_wallpaper();
            draw_dock();
            draw_topbar();
            draw_ctx_menu();
            draw_about();
            draw_start_menu();
            draw_search_overlay();
            wm_render_frame();   /* BUG FIX 1.2/1.8: composite WM windows on top of desktop */
            comp_render();       /* BUG FIX (dock): composite app windows into shadow buffer */
        }

        dbgcon_draw();
        fb_flip();                              /* push shadow to VRAM first */
        cursor_move((uint32_t)g_cx, (uint32_t)g_cy); /* stamp cursor onto VRAM after flip */
        sched_sleep(33);
    }
}
