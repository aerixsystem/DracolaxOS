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
#include "../../kernel/drivers/ps2/input_router.h"
#include "../../kernel/arch/x86_64/pic.h"
#include "../../kernel/dxi/dxi.h"
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
/* Window body background — medium glass blue, clearly visible on dark wallpaper */
#define WIN_BODY_COLOR  0x0E1428u   /* dark blue-gray, contrast against wallpaper */
#define WIN_BODY_FOCUS  0x161B38u   /* slightly lighter when focused */

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
    win->desktop  = current_desktop;
    win->task_id  = sched_current_id();  /* auto-assign owner task */
    win->maximized = 0;
    win->border_color = WIN_BORDER;
    win->title_color  = WIN_TITLE_BG;
    win->backbuf = kmalloc(w * h * 4);
    if (win->backbuf) {
        /* Fill with the same colour as the body background so the window
         * always appears cleanly filled. WIN_BODY_FOCUS used since new
         * windows are auto-focused. Use a slightly lighter shade than
         * WIN_BODY_COLOR so content written via comp_window_print is visible. */
        uint32_t fill_px = WIN_BODY_FOCUS;   /* 0x161B38 — slightly lighter than body */
        uint32_t *p = win->backbuf;
        for (uint32_t n = 0; n < w * h; n++) p[n] = fill_px;
    }
    /* Increment win_count FIRST so comp_focus_window's guard (h < win_count)
     * accepts the new window's index. Previously this was called before the
     * increment, so h == win_count was not < win_count → focus silently rejected. */
    int new_id = win_count++;
    comp_focus_window(new_id);
    return new_id;
}

void comp_destroy_window(int h) {
    if (h < 0 || h >= win_count) return;
    windows[h].visible = 0;
}

/* ── 2.4: Per-pixel BGRA alpha blend ─────────────────────────────────────
 *
 * Blits a BGRA 8888 icon into the shadow buffer at (dst_x, dst_y).
 * Alpha channel of each source pixel drives the blend:
 *
 *   C_out = (C_src * A + C_dst * (255 - A)) / 255
 *
 * Applied per channel (B, G, R) independently. Integer math only.
 *
 * Fast paths:
 *   A == 255 → direct write (opaque pixel, no blending)
 *   A ==   0 → skip pixel  (fully transparent)
 *
 * dst_stride: pixels per scanline of the destination (= fb.width).
 * Clips automatically to shadow buffer bounds.
 * ─────────────────────────────────────────────────────────────────────── */
void blit_icon_bgra(uint32_t dst_x, uint32_t dst_y,
                    const uint32_t *src, uint32_t src_w, uint32_t src_h,
                    uint32_t dst_stride) {
    uint32_t *shadow = fb_shadow_ptr();
    if (!shadow || !src) return;

    uint32_t screen_w = fb.width;
    uint32_t screen_h = fb.height;

    /* Clip region to screen */
    uint32_t draw_w = src_w;
    uint32_t draw_h = src_h;
    if (dst_x >= screen_w || dst_y >= screen_h) return;
    if (dst_x + draw_w > screen_w) draw_w = screen_w - dst_x;
    if (dst_y + draw_h > screen_h) draw_h = screen_h - dst_y;

    for (uint32_t row = 0; row < draw_h; row++) {
        const uint32_t *src_row = src      + row * src_w;
        uint32_t       *dst_row = shadow   + (dst_y + row) * dst_stride + dst_x;

        for (uint32_t col = 0; col < draw_w; col++) {
            uint32_t spx = src_row[col];
            uint8_t  a   = (uint8_t)(spx >> 24);

            /* Fast path: fully opaque */
            if (a == 0xFFu) {
                dst_row[col] = spx & 0x00FFFFFFu;  /* strip alpha, keep BGR */
                continue;
            }
            /* Fast path: fully transparent */
            if (a == 0u) continue;

            /* General blend: C_out = (C_src*A + C_dst*(255-A)) / 255 */
            uint32_t dpx = dst_row[col];
            uint32_t inv = 255u - (uint32_t)a;

            uint8_t sb = (uint8_t)( spx        & 0xFFu);
            uint8_t sg = (uint8_t)((spx >>  8) & 0xFFu);
            uint8_t sr = (uint8_t)((spx >> 16) & 0xFFu);

            uint8_t db = (uint8_t)( dpx        & 0xFFu);
            uint8_t dg = (uint8_t)((dpx >>  8) & 0xFFu);
            uint8_t dr = (uint8_t)((dpx >> 16) & 0xFFu);

            uint8_t ob = (uint8_t)(((uint32_t)sb * a + (uint32_t)db * inv) / 255u);
            uint8_t og = (uint8_t)(((uint32_t)sg * a + (uint32_t)dg * inv) / 255u);
            uint8_t or_ = (uint8_t)(((uint32_t)sr * a + (uint32_t)dr * inv) / 255u);

            dst_row[col] = ((uint32_t)ob)
                         | ((uint32_t)og << 8)
                         | ((uint32_t)or_ << 16);
        }
    }
}


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
        /* Raise z unless the window is always-on-top (its z is already pinned high)
         * or another window has always_on_top set (keep that one above). */
        if (!windows[h].always_on_top) {
            uint32_t maxz = 0;
            for (int i = 0; i < win_count; i++)
                if (!windows[i].always_on_top && windows[i].z > maxz)
                    maxz = windows[i].z;
            windows[h].z = maxz + 1;
        }
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

/* shadow_blit — guaranteed write to the shadow buffer, bypassing any
 * fb_blit implementation ambiguity about VRAM vs shadow target.
 * This is the only safe way to composite windows when double-buffering. */
static void shadow_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         const uint32_t *src) {
    uint32_t *shadow = fb_shadow_ptr();
    if (!shadow || !src || w == 0 || h == 0) return;
    uint32_t SW = fb.width, SH = fb.height;
    if (x >= SW || y >= SH) return;
    uint32_t draw_w = (x + w > SW) ? SW - x : w;
    uint32_t draw_h = (y + h > SH) ? SH - y : h;
    for (uint32_t row = 0; row < draw_h; row++)
        memcpy(shadow + (y + row) * SW + x,
               src   + row * w,
               draw_w * sizeof(uint32_t));
}

/* =========================================================================
 * Window chrome constants — per user spec:
 *   [icon] Title  [_][ ][x]   in a 28px title bar
 * ========================================================================= */
#define WIN_TITLE_H   28u   /* title bar pixel height */
#define WIN_BTN_SZ    18u   /* button circle diameter */
#define WIN_BTN_Y      5u   /* button y offset within title bar */
#define WIN_ICON_SZ   18u   /* app icon square size */
#define WIN_ICON_X     6u   /* icon x offset from window left */

/* Button x positions measured FROM the window's right edge */
#define WIN_BTN_CLOSE_RX  22u   /* close button right offset */
#define WIN_BTN_MAX_RX    44u   /* maximize right offset */
#define WIN_BTN_MIN_RX    66u   /* minimize right offset */

/* Returns absolute x of a button's left edge given window x and width */
#define WIN_CLOSE_X(wx, ww)  ((wx) + (ww) - WIN_BTN_CLOSE_RX)
#define WIN_MAX_X(wx, ww)    ((wx) + (ww) - WIN_BTN_MAX_RX)
#define WIN_MIN_X(wx, ww)    ((wx) + (ww) - WIN_BTN_MIN_RX)

static void render_window(window_t *win) {
    if (!win->visible || !fb.available) return;
    uint32_t x = win->x, y = win->y, w = win->w, h = win->h;
    /* Bounds check: reject fully off-screen windows but do NOT clamp w/h.
     * Clamping w/h here causes rendered button positions to differ from the
     * unclamped positions used by comp_close_at / comp_maximize_at /
     * comp_minimize_at / comp_resize_edge_at — making chrome unclickable.
     * fb_fill_rect and shadow_blit already clip to screen bounds safely. */
    if (x >= fb.width || y >= fb.height || w == 0 || h == 0) return;

    uint32_t title_h = WIN_TITLE_H;
    /* Compute draw height clamped to screen for fb operations only.
     * We keep w/h unclamped above so chrome hit-tests stay correct. */
    uint32_t draw_w = (x + w > fb.width)  ? fb.width  - x : w;
    uint32_t draw_h = (y + h > fb.height) ? fb.height - y : h;
    uint32_t body_h  = (draw_h > title_h) ? draw_h - title_h : 0u;

    /* 1 - Drop shadow */
    {
        uint32_t sc = 0x05050Au;
        if (x + draw_w + 8u <= fb.width && y + draw_h <= fb.height)
            fb_fill_rect(x + 6u, y + draw_h, draw_w, 7u, sc);
        if (x + draw_w <= fb.width && y + draw_h + 7u <= fb.height)
            fb_fill_rect(x + draw_w, y + 6u, 7u, draw_h, sc);
    }

    /* 2 - Body background */
    uint32_t body_bg = win->focused ? WIN_BODY_FOCUS : WIN_BODY_COLOR;
    if (body_h > 0) fb_fill_rect(x, y + title_h, draw_w, body_h, body_bg);

    /* 3 - Backbuf content (blit rows 0..body_h from backbuf to screen body) */
    if (win->backbuf && body_h > 0) {
        uint32_t bh = (win->h > title_h) ? win->h - title_h : 0u;
        if (bh > body_h) bh = body_h;
        if (bh > 0) shadow_blit(x, y + title_h, draw_w, bh, win->backbuf);
    }

    /* 4 - Title bar drawn AFTER backbuf so it is always on top */
    uint32_t tbar = win->focused ? 0x3C1870u : 0x1E1040u;
    fb_fill_rect(x, y, draw_w, title_h, tbar);
    /* Bright 2px top-edge so the title bar is always visible on dark bg */
    fb_fill_rect(x, y,      draw_w, 1u, win->focused ? 0x9060E0u : 0x503878u);
    fb_fill_rect(x, y + 1u, draw_w, 1u, win->focused ? 0x6030A0u : 0x301850u);
    /* Bottom separator */
    fb_fill_rect(x, y + title_h - 1u, draw_w, 1u,
                 win->focused ? 0x7838C8u : 0x402860u);

    /* 5 - App icon (coloured rounded square, left of title) */
    uint32_t icon_col = win->focused ? 0x8830F0u : 0x501888u;
    fb_fill_rect(x + WIN_ICON_X, y + WIN_BTN_Y,
                 WIN_ICON_SZ, WIN_ICON_SZ, icon_col);
    fb_rounded_outline(x + WIN_ICON_X, y + WIN_BTN_Y,
                       WIN_ICON_SZ, WIN_ICON_SZ, 3u,
                       win->focused ? 0xD070FFu : 0x7040A0u);

    /* 6 - Title text */
    {
        uint32_t tx = x + WIN_ICON_X + WIN_ICON_SZ + 6u;
        uint32_t ty = y + (title_h > FONT_H ? (title_h - FONT_H) / 2u : 0u);
        uint32_t right_reserved = WIN_BTN_MIN_RX + 8u;
        uint32_t avail = (w > right_reserved + WIN_ICON_X + WIN_ICON_SZ + 6u)
                         ? w - right_reserved - WIN_ICON_X - WIN_ICON_SZ - 6u : 0u;
        uint32_t mc = avail / FONT_W;
        if (mc > 0) {
            char cl[COMP_TITLE_MAX];
            size_t ml = mc < COMP_TITLE_MAX - 1u ? mc : COMP_TITLE_MAX - 1u;
            strncpy(cl, win->title, ml);
            cl[ml] = '\0';
            fb_print(tx, ty, cl,
                     win->focused ? 0xF8F0FFu : 0xB0A0D0u, tbar);
        }
    }

    /* 7 - Control buttons [_] [O] [x] */
    {
        uint32_t by2 = y + WIN_BTN_Y;

        /* Close [x] -- red */
        uint32_t clx = WIN_CLOSE_X(x, w);
        fb_fill_rect(clx, by2, WIN_BTN_SZ, WIN_BTN_SZ, 0xDD2222u);
        fb_rounded_outline(clx, by2, WIN_BTN_SZ, WIN_BTN_SZ,
                           WIN_BTN_SZ / 2u, 0xFF7070u);
        fb_print(clx + 5u, by2 + 1u, "x", 0xFFFFFFu, 0xDD2222u);

        /* Maximise [O] -- green */
        uint32_t max2 = WIN_MAX_X(x, w);
        fb_fill_rect(max2, by2, WIN_BTN_SZ, WIN_BTN_SZ, 0x229944u);
        fb_rounded_outline(max2, by2, WIN_BTN_SZ, WIN_BTN_SZ,
                           WIN_BTN_SZ / 2u, 0x60FF88u);
        fb_print(max2 + 4u, by2 + 1u, "O", 0xFFFFFFu, 0x229944u);

        /* Minimise [_] -- amber */
        uint32_t min2 = WIN_MIN_X(x, w);
        fb_fill_rect(min2, by2, WIN_BTN_SZ, WIN_BTN_SZ, 0xCC8800u);
        fb_rounded_outline(min2, by2, WIN_BTN_SZ, WIN_BTN_SZ,
                           WIN_BTN_SZ / 2u, 0xFFCC44u);
        fb_print(min2 + 4u, by2 + 1u, "_", 0xFFFFFFu, 0xCC8800u);
    }

    /* 8 - Border outline (fb_rounded_outline = outline only, never fills) */
    {
        uint32_t bc = win->focused ? COL_ACCENT_LT : 0x604890u;
        fb_rounded_outline(x, y, w, h, WIN_CORNER_R, bc);
        if (win->focused && w > 2u && h > 2u)
            fb_rounded_outline(x + 1u, y + 1u, w - 2u, h - 2u,
                               WIN_CORNER_R > 1u ? WIN_CORNER_R - 1u : 1u,
                               0x7038B0u);
    }
}

/* ── Hit-test helpers — used by desktop.c for click handling ──────────── */

/* Returns handle of the topmost visible window whose title bar contains (px,py),
 * or -1 if none. Searches in reverse z-order (highest z = topmost). */
int comp_title_bar_at(int px, int py) {
    int best = -1; uint32_t best_z = 0;
    for (int i = 0; i < win_count; i++) {
        window_t *w = &windows[i];
        if (!w->visible || w->desktop != current_desktop) continue;
        if (px >= (int)w->x && px < (int)(w->x + w->w) &&
            py >= (int)w->y && py < (int)(w->y + WIN_TITLE_H)) {
            if (best < 0 || w->z > best_z) { best = i; best_z = w->z; }
        }
    }
    return best;
}

/* Returns 1 if (px,py) is on the close button of window handle h */
int comp_close_at(int h, int px, int py) {
    if (h < 0 || h >= win_count) return 0;
    window_t *w = &windows[h];
    uint32_t bx = WIN_CLOSE_X(w->x, w->w);
    uint32_t by = w->y + WIN_BTN_Y;
    return (px >= (int)bx && px < (int)(bx + WIN_BTN_SZ) &&
            py >= (int)by && py < (int)(by + WIN_BTN_SZ)) ? 1 : 0;
}

/* Returns 1 if (px,py) is on the minimize button of window handle h */
int comp_minimize_at(int h, int px, int py) {
    if (h < 0 || h >= win_count) return 0;
    window_t *w = &windows[h];
    uint32_t bx = WIN_MIN_X(w->x, w->w);
    uint32_t by = w->y + WIN_BTN_Y;
    return (px >= (int)bx && px < (int)(bx + WIN_BTN_SZ) &&
            py >= (int)by && py < (int)(by + WIN_BTN_SZ)) ? 1 : 0;
}

/* Get window top-left position */
void comp_get_pos(int h, int *ox, int *oy) {
    if (h < 0 || h >= win_count) { *ox = 0; *oy = 0; return; }
    *ox = (int)windows[h].x;
    *oy = (int)windows[h].y;
}

int comp_maximize_at(int h, int px, int py) {
    if (h < 0 || h >= win_count) return 0;
    window_t *w = &windows[h];
    uint32_t bx = WIN_MAX_X(w->x, w->w);
    uint32_t by = w->y + WIN_BTN_Y;
    return (px >= (int)bx && px < (int)(bx + WIN_BTN_SZ) &&
            py >= (int)by && py < (int)(by + WIN_BTN_SZ)) ? 1 : 0;
}

/* ── Body hit-test ─────────────────────────────────────────────────────
 * Returns the handle of the topmost visible window whose CLIENT AREA
 * (the region below the title bar) contains (px, py), or -1.          */
int comp_window_body_at(int px, int py) {
    int best = -1; uint32_t best_z = 0;
    for (int i = 0; i < win_count; i++) {
        window_t *w = &windows[i];
        if (!w->visible || w->desktop != current_desktop) continue;
        if (px >= (int)w->x && px < (int)(w->x + w->w) &&
            py >= (int)(w->y + WIN_TITLE_H) && py < (int)(w->y + w->h)) {
            if (best < 0 || w->z > best_z) { best = i; best_z = w->z; }
        }
    }
    return best;
}

/* ── Full window hit-test ──────────────────────────────────────────────
 * Returns the topmost visible window that contains (px, py) anywhere
 * within its bounding box (titlebar + body + border zone), or -1.      */
int comp_window_at(int px, int py) {
    int best = -1; uint32_t best_z = 0;
    for (int i = 0; i < win_count; i++) {
        window_t *w = &windows[i];
        if (!w->visible || w->desktop != current_desktop) continue;
        /* Expand by RESIZE_BORDER so the edge is detectable */
        int brd = 6;
        if (px >= (int)w->x - brd && px < (int)(w->x + w->w) + brd &&
            py >= (int)w->y       && py < (int)(w->y + w->h) + brd) {
            if (best < 0 || w->z > best_z) { best = i; best_z = w->z; }
        }
    }
    return best;
}

/* ── Resize edge detection ─────────────────────────────────────────────
 * Returns which resize edge or corner of window 'h' the point (px,py)
 * sits on.  RESIZE_BORDER pixels wide on each edge.                    */
#define RESIZE_BORDER 6

resize_edge_t comp_resize_edge_at(int h, int px, int py) {
    if (h < 0 || h >= win_count) return RESIZE_NONE;
    window_t *w = &windows[h];
    if (!w->visible) return RESIZE_NONE;

    int wx  = (int)w->x,     wy  = (int)w->y;
    int ww  = (int)w->w,     wh  = (int)w->h;

    /* Must be within the extended bounding box */
    if (px < wx - RESIZE_BORDER || px > wx + ww + RESIZE_BORDER) return RESIZE_NONE;
    if (py < wy                  || py > wy + wh + RESIZE_BORDER) return RESIZE_NONE;

    int on_left   = (px >= wx            && px <  wx + RESIZE_BORDER);
    int on_right  = (px >= wx + ww - RESIZE_BORDER && px < wx + ww + RESIZE_BORDER);
    int on_top    = (py >= wy            && py <  wy + RESIZE_BORDER);
    int on_bottom = (py >= wy + wh - RESIZE_BORDER && py < wy + wh + RESIZE_BORDER);

    /* Skip the title bar area for top-edge resize (drag would conflict) */
    if (on_top && !on_left && !on_right) return RESIZE_NONE;

    if (on_top    && on_left)  return RESIZE_NW;
    if (on_top    && on_right) return RESIZE_NE;
    if (on_bottom && on_left)  return RESIZE_SW;
    if (on_bottom && on_right) return RESIZE_SE;
    if (on_bottom)             return RESIZE_S;
    if (on_left)               return RESIZE_W;
    if (on_right)              return RESIZE_E;

    return RESIZE_NONE;
}

/* ── Get/set geometry ──────────────────────────────────────────────── */
#define WIN_MIN_W 120u
#define WIN_MIN_H  60u

void comp_get_geometry(int h, int *ox, int *oy, int *ow, int *oh) {
    if (h < 0 || h >= win_count) {
        *ox = 0; *oy = 0; *ow = 0; *oh = 0; return;
    }
    *ox = (int)windows[h].x; *oy = (int)windows[h].y;
    *ow = (int)windows[h].w; *oh = (int)windows[h].h;
}

void comp_set_geometry(int h, uint32_t x, uint32_t y, uint32_t w, uint32_t h_val) {
    if (h < 0 || h >= win_count) return;
    if (w < WIN_MIN_W) w = WIN_MIN_W;
    if (h_val < WIN_MIN_H) h_val = WIN_MIN_H;
    windows[h].x = x; windows[h].y = y;
    windows[h].w = w; windows[h].h = h_val;
}

/* ── Focused window ────────────────────────────────────────────────── */
int comp_focused_window(void) {
    for (int i = 0; i < win_count; i++)
        if (windows[i].focused && windows[i].visible &&
            windows[i].desktop == current_desktop)
            return i;
    return -1;
}

/* ── Always-on-top ─────────────────────────────────────────────────── */
void comp_toggle_always_on_top(int h) {
    if (h < 0 || h >= win_count) return;
    windows[h].always_on_top ^= 1;
    kinfo("COMPOSITOR: window %d always_on_top=%d\n",
          h, windows[h].always_on_top);
}

int comp_is_always_on_top(int h) {
    if (h < 0 || h >= win_count) return 0;
    return windows[h].always_on_top;
}

void comp_toggle_maximize(int h) {
    if (h < 0 || h >= win_count) return;
    window_t *w = &windows[h];
    if (!w->maximized) {
        /* Save current geometry and go fullscreen */
        w->saved_x = w->x; w->saved_y = w->y;
        w->saved_w = w->w; w->saved_h = w->h;
        w->x = 0; w->y = 0;
        w->w = fb.width; w->h = fb.height;
        w->maximized = 1;
    } else {
        /* Restore saved geometry */
        w->x = w->saved_x; w->y = w->saved_y;
        w->w = w->saved_w; w->h = w->saved_h;
        w->maximized = 0;
    }
}

void comp_set_visible(int h, int vis) {
    if (h < 0 || h >= win_count) return;
    windows[h].visible = vis ? 1 : 0;
}

int comp_get_task_id(int h) {
    if (h < 0 || h >= win_count) return -1;
    return windows[h].task_id;
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
    /* Sort by z-order; always_on_top windows get an artificial z boost
     * during sorting so they always render last (i.e. on top). */
    for (int i = 0; i < visible_count; i++)
        for (int j = i + 1; j < visible_count; j++) {
            window_t *wi = &windows[sorted[i]];
            window_t *wj = &windows[sorted[j]];
            uint32_t zi = wi->z + (wi->always_on_top ? 0xFFFF0000u : 0u);
            uint32_t zj = wj->z + (wj->always_on_top ? 0xFFFF0000u : 0u);
            if (zj < zi) {
                int tmp = sorted[i]; sorted[i] = sorted[j]; sorted[j] = tmp;
            }
        }
    for (int i = 0; i < visible_count; i++)
        render_window(&windows[sorted[i]]);
}

void comp_init(void) {
    memset(windows, 0, sizeof(windows));
    win_count = 0;
    kinfo("COMPOSITOR: init (fb %s)\n", fb.available ? "available" : "N/A");
}

/* Count visible windows on a given virtual desktop — used by ws_switcher
 * to display open-app counts on each workspace tile. */
int comp_count_windows_on_desktop(int desktop) {
    int count = 0;
    for (int i = 0; i < win_count; i++)
        if (windows[i].visible && windows[i].desktop == desktop)
            count++;
    return count;
}

int comp_has_windows(void) {
    for (int i = 0; i < win_count; i++)
        if (windows[i].visible && windows[i].desktop == current_desktop)
            return 1;
    return 0;
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

/* =========================================================================
 * Window terminal API — self-contained scrolling text console in a window.
 * Apps use this instead of vga_print/fb_fill_rect so they never touch the
 * raw framebuffer and always route through the compositor pipeline.
 * ========================================================================= */

#define TERM_FONT_W  8u
#define TERM_FONT_H  16u
#define TERM_TITLE_H WIN_TITLE_H   /* must match compositor render_window */
#define TERM_PAD_X   6u
#define TERM_PAD_Y   4u

void comp_window_clear(int h) {
    if (h < 0 || h >= win_count) return;
    window_t *win = &windows[h];
    if (!win->backbuf) return;
    uint32_t body_col = 0x0A0E1Eu;   /* dark sapphire body */
    uint32_t n = win->w * win->h;
    for (uint32_t i = 0; i < n; i++) win->backbuf[i] = body_col;
}

void comp_window_printf(int h, uint32_t x, uint32_t y,
                        uint32_t fg, const char *fmt, ...) {
    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    comp_window_print(h, x, y, buf, fg);
}

/* ── Terminal state ─────────────────────────────────────────────── */

void comp_win_term_init(comp_term_t *t, int handle, uint32_t fg, uint32_t bg) {
    t->handle  = handle;
    t->cur_x   = TERM_PAD_X;
    t->cur_y   = TERM_PAD_Y;
    t->fg      = fg  ? fg  : 0xD0D8FFu;
    t->bg      = bg  ? bg  : 0x0A0E1Eu;
    t->task_id = sched_current_id();
    if (handle >= 0 && handle < win_count) {
        t->body_h = windows[handle].h > TERM_TITLE_H
                    ? windows[handle].h - TERM_TITLE_H : 0;
    } else {
        t->body_h = 0;
    }
    /* Fill body with bg colour */
    if (handle >= 0) comp_window_clear(handle);
}

/* Scroll the body region up by one line in the backbuf */
static void term_scroll(comp_term_t *t) {
    if (t->handle < 0 || t->handle >= win_count) return;
    window_t *win = &windows[t->handle];
    if (!win->backbuf) return;
    uint32_t W  = win->w;
    uint32_t bh = t->body_h;
    if (bh <= TERM_FONT_H) return;

    /* Shift rows up by FONT_H */
    uint32_t rows_to_move = bh - TERM_FONT_H;
    memmove(win->backbuf,
            win->backbuf + TERM_FONT_H * W,
            rows_to_move * W * sizeof(uint32_t));

    /* Clear the last row */
    uint32_t *last = win->backbuf + rows_to_move * W;
    for (uint32_t i = 0; i < TERM_FONT_H * W; i++) last[i] = t->bg;

    t->cur_y -= TERM_FONT_H;
    if ((int)t->cur_y < (int)TERM_PAD_Y) t->cur_y = TERM_PAD_Y;
}

void comp_win_term_print(comp_term_t *t, const char *s) {
    if (t->handle < 0 || t->handle >= win_count) return;
    window_t *win = &windows[t->handle];
    if (!win->backbuf) return;

    uint32_t W    = win->w;
    uint32_t bh   = t->body_h;
    uint32_t max_x = (W > TERM_PAD_X) ? W - TERM_PAD_X : W;

    while (*s) {
        if (*s == '\n' || t->cur_x + TERM_FONT_W > max_x) {
            t->cur_x = TERM_PAD_X;
            t->cur_y += TERM_FONT_H;
            if (t->cur_y + TERM_FONT_H > bh) term_scroll(t);
        }
        if (*s == '\n') { s++; continue; }
        if (*s == '\b') {
            if (t->cur_x >= TERM_PAD_X + TERM_FONT_W) {
                t->cur_x -= TERM_FONT_W;
                /* Erase char at cur_x, cur_y in backbuf */
                uint8_t idx = ' ';
                for (uint32_t gy = 0; gy < TERM_FONT_H; gy++) {
                    uint8_t bits = vga_font_8x16[idx][gy];
                    for (uint32_t gx = 0; gx < TERM_FONT_W; gx++) {
                        uint32_t row = t->cur_y + gy;
                        uint32_t col = t->cur_x + gx;
                        if (row < bh && col < W) {
                            int lit = (bits >> (7u - gx)) & 1;
                            win->backbuf[row * W + col] = lit ? t->fg : t->bg;
                        }
                    }
                }
            }
            s++; continue;
        }
        if (*s == '\r') { s++; continue; }

        /* Print the character into the backbuf */
        uint8_t idx = (uint8_t)*s;
        if (idx >= 128) idx = '?';
        for (uint32_t gy = 0; gy < TERM_FONT_H; gy++) {
            uint8_t bits = vga_font_8x16[idx][gy];
            for (uint32_t gx = 0; gx < TERM_FONT_W; gx++) {
                uint32_t row = t->cur_y + gy;
                uint32_t col = t->cur_x + gx;
                if (row < bh && col < W) {
                    int lit = (bits >> (7u - gx)) & 1;
                    win->backbuf[row * W + col] = lit ? t->fg : t->bg;
                }
            }
        }
        t->cur_x += TERM_FONT_W;
        s++;
    }
}

void comp_win_term_printf(comp_term_t *t, const char *fmt, ...) {
    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    comp_win_term_print(t, buf);
}

/* Non-blocking: read from this task's input queue */
int comp_win_term_getchar(comp_term_t *t) {
    return input_router_getchar(t->task_id);
}

/* Blocking readline with echo into window */
int comp_win_term_readline(comp_term_t *t, char *buf, int max) {
    int pos = 0;
    buf[0] = '\0';
    while (1) {
        /* Yield until a char arrives in our queue */
        int c;
        while ((c = input_router_getchar(t->task_id)) < 0)
            sched_yield();

        if (c == '\n' || c == '\r') {
            comp_win_term_print(t, "\n");
            buf[pos] = '\0';
            return pos;
        }
        if (c == 0x1B) { buf[0] = '\0'; return -1; }  /* ESC = cancel */
        if (c == '\b') {
            if (pos > 0) {
                pos--;
                buf[pos] = '\0';
                comp_win_term_print(t, "\b");
            }
            continue;
        }
        if ((unsigned char)c >= 0x20 && pos < max - 1) {
            buf[pos++] = (char)c;
            buf[pos]   = '\0';
            char echo[2] = { (char)c, '\0' };
            comp_win_term_print(t, echo);
        }
    }
}
