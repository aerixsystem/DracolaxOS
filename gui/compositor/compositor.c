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
#include "../../kernel/drivers/ps2/mouse.h"
#include "../../kernel/drivers/ps2/input_router.h"
#include "../../kernel/arch/x86_64/pic.h"
#include "../../kernel/dxi/dxi.h"
#include "../../gui/icons/icon_data.h"   /* builtin_icons[] embedded DXI data */
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
static int      current_desktop = 0;  /* active virtual desktop */

/* Returns 1 if window-content pixel (x,y) should be drawn given the
 * window's current clip rect (or always, if no clip is active). Used by
 * comp_window_print/set_pixel, which already iterate pixel-by-pixel.
 * comp_window_fill instead intersects its target rect with the clip rect
 * up front — cheaper than a per-pixel check for a solid fill. See
 * comp_window_set_clip() for the public API this backs. */
static int clip_allows(const window_t *win, int x, int y) {
    if (!win->clip_active) return 1;
    return x >= win->clip_x && x < win->clip_x + win->clip_w &&
           y >= win->clip_y && y < win->clip_y + win->clip_h;
}

/* ── DXI icon cache for window title bars ─────────────────────────────────
 * Declared here (before all functions) so comp_destroy_window and comp_init
 * can reference them.  Pixel buffer uses 32×32 to match generated icons. */
#define WICON_CACHE_SZ  (32u * 32u)
static dxi_icon_t wicon_cache[COMP_MAX_WINDOWS];
static uint32_t   wicon_px   [COMP_MAX_WINDOWS][WICON_CACHE_SZ];
static int        wicon_tried[COMP_MAX_WINDOWS];

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
    win->minimized = 0;
    win->focused = 0;
    win->desktop  = current_desktop;
    win->task_id  = sched_current_id();  /* auto-assign owner task */
    win->maximized = 0;
    win->border_color = WIN_BORDER;
    win->title_color  = WIN_TITLE_BG;
    win->win_flags    = 0;
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
    /* Reset DXI icon cache so if this slot is reused the new window gets its own icon */
    wicon_tried[h] = 0;
    wicon_cache[h].loaded = 0;
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

void comp_next_spawn_pos(uint32_t base_x, uint32_t base_y,
                         uint32_t win_w, uint32_t win_h,
                         uint32_t *out_x, uint32_t *out_y) {
    /* Count windows already open on the current desktop to decide how far
     * to cascade. Cheap linear scan — win_count is capped at
     * COMP_MAX_WINDOWS (16). */
    int n = 0;
    for (int i = 0; i < win_count; i++)
        if (windows[i].visible && windows[i].desktop == current_desktop) n++;

    const uint32_t STEP = 32;      /* px offset per already-open window */
    const uint32_t MAX_STEPS = 8;  /* wrap back to base after this many */
    uint32_t step = (uint32_t)n % MAX_STEPS;

    uint32_t x = base_x + step * STEP;
    uint32_t y = base_y + step * STEP;

    /* Keep the cascade on-screen: if it would run past the right/bottom
     * edge, wrap back toward the base position instead of letting the
     * window spawn partially or fully off-screen. */
    if (fb.available) {
        if (x + win_w > fb.width)  x = base_x;
        if (y + win_h > fb.height) y = base_y;
    }

    *out_x = x;
    *out_y = y;
}

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
                        if (brow < win->h && bcol < win->w &&
                            clip_allows(win, (int)bcol, (int)brow)) {
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

void comp_window_set_clip(int h, int x, int y, int w, int h_val) {
    if (h < 0 || h >= win_count) return;
    if (w < 0) w = 0;
    if (h_val < 0) h_val = 0;
    windows[h].clip_active = 1;
    windows[h].clip_x = x; windows[h].clip_y = y;
    windows[h].clip_w = w; windows[h].clip_h = h_val;
}

void comp_window_clear_clip(int h) {
    if (h < 0 || h >= win_count) return;
    windows[h].clip_active = 0;
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

    /* BUG FIX (scrollable-list/clipped-content overflow): intersect with
     * the active clip rect, if any, BEFORE filling. Without this, content
     * meant to be confined to a sub-region (e.g. a scrollable list inside
     * a bordered box) could paint outside that region whenever its
     * unclipped position fell partially outside (e.g. a partially
     * scrolled-past row whose text starts a few pixels above the list's
     * top edge) — there was previously no way to contain it short of the
     * caller manually clamping every single draw call. */
    if (win->clip_active) {
        uint32_t cx1 = (uint32_t)(win->clip_x < 0 ? 0 : win->clip_x);
        uint32_t cy1 = (uint32_t)(win->clip_y < 0 ? 0 : win->clip_y);
        uint32_t cx2 = (uint32_t)(win->clip_x < 0 ? 0 : win->clip_x) + (uint32_t)win->clip_w;
        uint32_t cy2 = (uint32_t)(win->clip_y < 0 ? 0 : win->clip_y) + (uint32_t)win->clip_h;
        if (x1 < cx1) x1 = cx1;
        if (y1 < cy1) y1 = cy1;
        if (x2 > cx2) x2 = cx2;
        if (y2 > cy2) y2 = cy2;
    }
    if (x2 <= x1 || y2 <= y1) return;

    for (uint32_t row = y1; row < y2; row++)
        for (uint32_t col = x1; col < x2; col++)
            win->backbuf[row * win->w + col] = color;
}

/* Single-pixel write/read into a window's backbuffer, bounds-checked.
 * Added for the immediate-mode widget toolkit (gui/widgets/) — shapes like
 * lines and circles need per-pixel access that comp_window_fill (rect-only)
 * and comp_window_print (font glyphs only) don't provide. Negative
 * coordinates are accepted (and silently dropped) so callers doing Bresenham
 * line/circle math don't need to clamp at every step themselves. */
void comp_window_set_pixel(int h, int x, int y, uint32_t color) {
    if (h < 0 || h >= win_count) return;
    window_t *win = &windows[h];
    if (!win->backbuf) return;
    if (x < 0 || y < 0 || (uint32_t)x >= win->w || (uint32_t)y >= win->h) return;
    if (!clip_allows(win, x, y)) return;
    win->backbuf[(uint32_t)y * win->w + (uint32_t)x] = color;
}

uint32_t comp_window_get_pixel(int h, int x, int y) {
    if (h < 0 || h >= win_count) return 0;
    window_t *win = &windows[h];
    if (!win->backbuf) return 0;
    if (x < 0 || y < 0 || (uint32_t)x >= win->w || (uint32_t)y >= win->h) return 0;
    return win->backbuf[(uint32_t)y * win->w + (uint32_t)x];
}

/* ── Backbuffer reallocation (BUG FIX 9) ──────────────────────────────────
 * comp_set_geometry() and comp_toggle_maximize() change win->w/win->h, but
 * win->backbuf was allocated at the *old* w*h*4 size. Any subsequent
 * comp_window_print/comp_window_fill/shadow_blit indexes the buffer using
 * the *new* dimensions, causing out-of-bounds heap reads/writes whenever a
 * window grows (heap corruption) and stale/garbage content when it shrinks
 * (the old data isn't laid out for the new stride, so each row appears
 * shifted).
 *
 * comp_resize_backbuf() reallocates to new_w*new_h*4 and refills with the
 * focus-appropriate body colour. We deliberately do NOT try to preserve old
 * pixel content across a stride change — the window owner is expected to
 * redraw on the next frame (apps already do this every loop iteration via
 * comp_window_clear + comp_window_print/fill). Preserving content correctly
 * would require a row-by-row copy with old/new strides; not worth the
 * complexity for a redraw-every-frame UI model. */
static void comp_resize_backbuf(window_t *win, uint32_t new_w, uint32_t new_h) {
    if (new_w == 0 || new_h == 0) return;

    size_t new_sz = (size_t)new_w * (size_t)new_h * 4u;
    uint32_t *nb = (uint32_t *)krealloc(win->backbuf, new_sz);
    if (!nb) {
        /* Out of memory: keep the old buffer rather than leaking/crashing.
         * The window will render with stale/garbage content at the new
         * size until memory frees up and a future resize succeeds, but at
         * least we avoid OOB access by NOT updating win->w/win->h here.
         * Caller (comp_set_geometry/comp_toggle_maximize) checks the
         * return implicitly by re-reading win->w/win->h afterwards — so we
         * signal failure by leaving dimensions unchanged. */
        kwarn("COMPOSITOR: backbuf realloc failed (%ux%u, %u bytes) — "
              "keeping old geometry\n", new_w, new_h, (uint32_t)new_sz);
        return;
    }

    win->backbuf = nb;
    win->w = new_w;
    win->h = new_h;

    /* Refill with the appropriate body colour so the new/regrown region
     * doesn't show garbage heap memory before the app redraws. */
    uint32_t fill_px = win->focused ? WIN_BODY_FOCUS : WIN_BODY_COLOR;
    uint32_t n = new_w * new_h;
    for (uint32_t i = 0; i < n; i++) win->backbuf[i] = fill_px;
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

    /* 5 - App icon: load from embedded builtin_icons[] (no VFS needed) */
    {
        int wi_h = (int)(win - windows);
        if (!wicon_tried[wi_h]) {
            wicon_tried[wi_h] = 1;
            wicon_cache[wi_h].pixels = wicon_px[wi_h];
            wicon_cache[wi_h].loaded = 0;

            /* Match window title against builtin_icons[] app_name */
            for (int bi = 0; bi < BUILTIN_ICON_COUNT; bi++) {
                if (strcmp(builtin_icons[bi].app_name, win->title) == 0) {
                    /* Parse the DXI from embedded bytes */
                    const uint8_t *raw = builtin_icons[bi].data;
                    uint32_t       sz  = builtin_icons[bi].size;
                    if (sz >= 16 && raw[0]=='D' && raw[1]=='R' &&
                        raw[2]=='C' && raw[3]=='O') {
                        uint16_t iw = (uint16_t)(raw[4] | (raw[5]<<8));
                        uint16_t ih = (uint16_t)(raw[6] | (raw[7]<<8));
                        uint32_t npx = (uint32_t)iw * (uint32_t)ih;
                        if (npx <= WICON_CACHE_SZ && sz >= 16 + npx*4) {
                            /* Copy BGRA pixels — convert BGRA→RGBA for blit_icon_bgra */
                            const uint8_t *src = raw + 16;
                            for (uint32_t pi = 0; pi < npx; pi++) {
                                uint8_t b=src[pi*4+0],g=src[pi*4+1],
                                        r=src[pi*4+2],a=src[pi*4+3];
                                wicon_px[wi_h][pi] =
                                    ((uint32_t)a<<24)|((uint32_t)r<<16)|
                                    ((uint32_t)g<<8) |  (uint32_t)b;
                            }
                            wicon_cache[wi_h].width  = iw;
                            wicon_cache[wi_h].height = ih;
                            wicon_cache[wi_h].loaded = 1;
                        }
                    }
                    break;
                }
            }
        }

        if (wicon_cache[wi_h].loaded) {
            uint32_t src_w = wicon_cache[wi_h].width;
            uint32_t src_h = wicon_cache[wi_h].height;
            uint32_t dst_sz = WIN_ICON_SZ;
            static uint32_t scaled[WIN_ICON_SZ * WIN_ICON_SZ];
            for (uint32_t dy = 0; dy < dst_sz; dy++) {
                uint32_t sy = dy * src_h / dst_sz;
                if (sy >= src_h) sy = src_h - 1;
                for (uint32_t dx2 = 0; dx2 < dst_sz; dx2++) {
                    uint32_t sx = dx2 * src_w / dst_sz;
                    if (sx >= src_w) sx = src_w - 1;
                    scaled[dy * dst_sz + dx2] =
                        wicon_cache[wi_h].pixels[sy * src_w + sx];
                }
            }
            /* Blit scaled icon directly (pixels are RGBA in wicon_px) */
            for (uint32_t dy = 0; dy < dst_sz; dy++) {
                for (uint32_t dx2 = 0; dx2 < dst_sz; dx2++) {
                    uint32_t px2 = scaled[dy*dst_sz+dx2];
                    uint8_t  a   = (uint8_t)((px2>>24)&0xFF);
                    if (a > 0) {
                        uint8_t r=(uint8_t)((px2>>16)&0xFF);
                        uint8_t g=(uint8_t)((px2>>8)&0xFF);
                        uint8_t b=(uint8_t)(px2&0xFF);
                        fb_put_pixel(x+WIN_ICON_X+dx2, y+WIN_BTN_Y+dy, fb_color(r,g,b));
                    }
                }
            }
        } else {
            /* BUG FIX (no real icons): this used to draw nothing at all,
             * leaving an empty gap in the title bar. compositor.c sits
             * below desktop.c architecturally (desktop.c depends on the
             * compositor, not the other way around), so it can't reuse
             * desktop.c's per-app desktop_draw_vector_icon() without an
             * awkward backwards dependency — this is a small, self-
             * contained generic "app window" pictogram instead: a
             * rounded frame with a title-bar strip and two corner dots,
             * matching the same generic fallback desktop icons/dock use
             * for any app without a dedicated icon. No diagonal-line
             * primitive exists in fb.h, so this is deliberately built
             * from axis-aligned rect calls only, same as everywhere else
             * in this file. */
            uint32_t ix = x + WIN_ICON_X, iy = y + WIN_BTN_Y;
            uint32_t isz = WIN_ICON_SZ;
            uint32_t ifg = win->focused ? 0xC080FFu : 0x8060A0u;
            uint32_t ibg = win->focused ? 0x2A0A3Au : 0x1A0A2Au;
            uint32_t ipad = isz/6u;
            uint32_t ifx = ix+ipad, ify = iy+ipad, ifw = isz-2u*ipad, ifh = isz-2u*ipad;
            uint32_t itb = ifh/4u>2u?ifh/4u:2u;
            fb_rounded_rect(ix,iy,isz,isz,isz/5u,ibg);
            fb_rounded_rect(ifx,ify,ifw,ifh,ifh/8u,ifg);
            fb_fill_rect(ifx+2u,ify+2u,ifw>4u?ifw-4u:1u,itb,ibg);
        }
    }

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

    /* 7 - Control buttons [_] [O] [x] with hover highlight */
    {
        uint32_t by2 = y + WIN_BTN_Y;
        /* Query current mouse coords for hover detection */
        int mx = mouse_get_x(), my = mouse_get_y();

        /* Close [x] -- red, brighter on hover */
        uint32_t clx = WIN_CLOSE_X(x, w);
        int cl_hov = (mx>=(int)clx && mx<(int)(clx+WIN_BTN_SZ) &&
                      my>=(int)by2  && my<(int)(by2+WIN_BTN_SZ));
        uint32_t cl_col = cl_hov ? 0xFF4444u : 0xDD2222u;
        fb_fill_rect(clx, by2, WIN_BTN_SZ, WIN_BTN_SZ, cl_col);
        fb_rounded_outline(clx, by2, WIN_BTN_SZ, WIN_BTN_SZ,
                           WIN_BTN_SZ / 2u, cl_hov ? 0xFFAAAAu : 0xFF7070u);
        fb_print(clx + 5u, by2 + 1u, "x", 0xFFFFFFu, cl_col);

        /* Maximise [O] -- green, brighter on hover */
        uint32_t max2 = WIN_MAX_X(x, w);
        int mx2_hov = (mx>=(int)max2 && mx<(int)(max2+WIN_BTN_SZ) &&
                       my>=(int)by2  && my<(int)(by2+WIN_BTN_SZ));
        uint32_t mx2_col = mx2_hov ? 0x33CC66u : 0x229944u;
        fb_fill_rect(max2, by2, WIN_BTN_SZ, WIN_BTN_SZ, mx2_col);
        fb_rounded_outline(max2, by2, WIN_BTN_SZ, WIN_BTN_SZ,
                           WIN_BTN_SZ / 2u, mx2_hov ? 0xAAFFCCu : 0x60FF88u);
        fb_print(max2 + 4u, by2 + 1u, "O", 0xFFFFFFu, mx2_col);

        /* Minimise [_] -- amber, brighter on hover */
        uint32_t min2 = WIN_MIN_X(x, w);
        int mn_hov = (mx>=(int)min2 && mx<(int)(min2+WIN_BTN_SZ) &&
                      my>=(int)by2  && my<(int)(by2+WIN_BTN_SZ));
        uint32_t mn_col = mn_hov ? 0xFFAA00u : 0xCC8800u;
        fb_fill_rect(min2, by2, WIN_BTN_SZ, WIN_BTN_SZ, mn_col);
        fb_rounded_outline(min2, by2, WIN_BTN_SZ, WIN_BTN_SZ,
                           WIN_BTN_SZ / 2u, mn_hov ? 0xFFEE88u : 0xFFCC44u);
        fb_print(min2 + 4u, by2 + 1u, "_", 0xFFFFFFu, mn_col);
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

/* BUG FIX (widget hit-testing offset / "must hover above the widget"):
 * render_window() blits win->backbuf (where comp_window_fill/print/
 * set_pixel write, origin (0,0) = top-left of CONTENT) to screen position
 * (win->x, win->y + WIN_TITLE_H) — see render_window() below. Any caller
 * that wants window-relative coordinates matching backbuf space (e.g. the
 * widget toolkit's mouse hit-testing) must subtract the title bar height
 * too, not just win->x/win->y. comp_get_pos() intentionally still returns
 * the outer (title-bar-included) position for callers that need it (e.g.
 * window-dragging code); this is the separate, correct helper for
 * content-space conversion. */
void comp_get_content_pos(int h, int *cx, int *cy) {
    if (h < 0 || h >= win_count) { *cx = 0; *cy = 0; return; }
    *cx = (int)windows[h].x;
    *cy = (int)windows[h].y + (int)WIN_TITLE_H;
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
    /* BUG FIX (resize cursor/hitbox appears on a maximized window's
     * edges): a maximized window fills the screen — there's no
     * meaningful "resize" affordance for it (and comp_toggle_maximize()
     * doesn't even preserve a maximized window's own geometry as
     * something a drag-resize could sensibly adjust; the whole point of
     * maximized is "not manually sized"). This was missing entirely, so
     * the edge math below happily matched screen-edge coordinates that,
     * for a maximized window, coincide with the actual screen edges. */
    if (w->maximized) return RESIZE_NONE;

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

/* GUI debug mode — see compositor.h. Deliberately draws using the SAME
 * constants (WIN_TITLE_H, WIN_BTN_SZ, WIN_BTN_Y, WIN_CLOSE_X/MAX_X/MIN_X,
 * RESIZE_BORDER) the real hit-test functions above use, rather than
 * separately-maintained approximations — so the overlay can never drift
 * out of sync with what's actually clickable. */
void comp_debug_draw_hitboxes(void) {
    const uint32_t OUTLINE = 0x00FF88u;   /* bright green-cyan, stands out */
    const uint32_t RESIZE_OUTLINE = 0xFFAA00u; /* amber, distinguishes resize zone */

    for (int i = 0; i < win_count; i++) {
        window_t *w = &windows[i];
        if (!w->visible || w->desktop != current_desktop) continue;

        int wx = (int)w->x, wy = (int)w->y, ww = (int)w->w, wh = (int)w->h;

        /* Title bar */
        fb_rounded_outline((uint32_t)wx, (uint32_t)wy, (uint32_t)ww, WIN_TITLE_H, 0u, OUTLINE);

        /* Buttons */
        uint32_t by2 = (uint32_t)wy + WIN_BTN_Y;
        fb_rounded_outline(WIN_CLOSE_X((uint32_t)wx,(uint32_t)ww), by2, WIN_BTN_SZ, WIN_BTN_SZ, WIN_BTN_SZ/2u, OUTLINE);
        fb_rounded_outline(WIN_MAX_X((uint32_t)wx,(uint32_t)ww),   by2, WIN_BTN_SZ, WIN_BTN_SZ, WIN_BTN_SZ/2u, OUTLINE);
        fb_rounded_outline(WIN_MIN_X((uint32_t)wx,(uint32_t)ww),   by2, WIN_BTN_SZ, WIN_BTN_SZ, WIN_BTN_SZ/2u, OUTLINE);

        /* Resize-edge perimeter (RESIZE_BORDER px strip around the whole
         * window, matching comp_resize_edge_at()'s bounding-box check) */
        fb_rounded_outline((uint32_t)(wx-RESIZE_BORDER), (uint32_t)wy,
                           (uint32_t)(ww+2*RESIZE_BORDER), (uint32_t)(wh+RESIZE_BORDER),
                           0u, RESIZE_OUTLINE);

        /* Window body (content area, below the title bar) */
        fb_rounded_outline((uint32_t)wx, (uint32_t)wy+WIN_TITLE_H,
                           (uint32_t)ww, (uint32_t)(wh>(int)WIN_TITLE_H?wh-(int)WIN_TITLE_H:0),
                           0u, fb_blend(OUTLINE,0x000000u,120u));
    }
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
    window_t *win = &windows[h];
    win->x = x; win->y = y;
    /* BUG FIX 9: reallocate backbuf when size actually changes. Position-only
     * moves (w/h unchanged) skip the realloc+refill entirely. */
    if (w != win->w || h_val != win->h) {
        if (win->backbuf) comp_resize_backbuf(win, w, h_val);
        else { win->w = w; win->h = h_val; }
    }
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
        /* BUG FIX 9: reallocate backbuf for the new fullscreen size. */
        if (w->backbuf) comp_resize_backbuf(w, fb.width, fb.height);
        else { w->w = fb.width; w->h = fb.height; }
        w->maximized = 1;
    } else {
        /* Restore saved geometry */
        w->x = w->saved_x; w->y = w->saved_y;
        if (w->backbuf) comp_resize_backbuf(w, w->saved_w, w->saved_h);
        else { w->w = w->saved_w; w->h = w->saved_h; }
        w->maximized = 0;
    }
}

void comp_set_visible(int h, int vis) {
    if (h < 0 || h >= win_count) return;
    windows[h].visible   = vis ? 1 : 0;
    /* Track user minimize: hiding a visible window sets minimized.
     * Showing it clears the flag. */
    if (!vis) windows[h].minimized = 1;
    else      windows[h].minimized = 0;
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
    memset(wicon_tried, 0, sizeof(wicon_tried));
    memset(wicon_cache, 0, sizeof(wicon_cache));
    win_count = 0;
    kinfo("COMPOSITOR: init (fb %s)\n", fb.available ? "available" : "N/A");
}

/* Count visible windows on a given virtual desktop — used by ws_switcher
 * to display open-app counts on each workspace tile. */
int comp_count_windows_on_desktop(int desktop) {
    /* Count visible + minimized (task still running) */
    int count = 0;
    for (int i = 0; i < win_count; i++)
        if ((windows[i].visible || windows[i].minimized) &&
             windows[i].desktop == desktop)
            count++;
    return count;
}

int comp_has_windows(void) {
    for (int i = 0; i < win_count; i++)
        if (windows[i].visible && windows[i].desktop == current_desktop)
            return 1;
    return 0;
}

int comp_window_count(void) { return win_count; }

const char *comp_window_title(int h) {
    if (h < 0 || h >= win_count) return "";
    return windows[h].title;
}

int comp_window_is_visible(int h) {
    if (h < 0 || h >= win_count) return 0;
    return windows[h].visible && windows[h].desktop == current_desktop;
}

int comp_window_exists(int h) {
    if (h < 0 || h >= win_count) return 0;
    if (windows[h].desktop != current_desktop) return 0;
    return windows[h].visible || windows[h].minimized;
}

int comp_window_is_minimized(int h) {
    if (h < 0 || h >= win_count) return 0;
    return windows[h].minimized;
}

uint32_t comp_frame_signature(void) {
    uint32_t sig = 2166136261u;   /* FNV-1a offset basis */
    sig ^= (uint32_t)win_count; sig *= 16777619u;
    for (int i = 0; i < win_count; i++) {
        window_t *w = &windows[i];
        /* Fold every field that affects what's visible on screen into the
         * running hash. Order matters here (which is fine — slot index i
         * is stable across a window's lifetime since comp_destroy_window()
         * only clears visible, it never compacts the array). */
        uint32_t fields[8] = {
            w->x, w->y, w->w, w->h,
            (uint32_t)w->visible, (uint32_t)w->minimized,
            (uint32_t)w->desktop, w->z
        };
        for (int f = 0; f < 8; f++) { sig ^= fields[f]; sig *= 16777619u; }
    }
    return sig;
}

/* Restore only user-minimized windows, not all hidden windows */
void comp_show_all_windows(int desktop) {
    for (int i = 0; i < win_count; i++) {
        if (windows[i].desktop == desktop && windows[i].minimized) {
            windows[i].visible   = 1;
            windows[i].minimized = 0;
        }
    }
}

void comp_clear_minimized(int h) {
    if (h >= 0 && h < win_count) windows[h].minimized = 0;
}

void comp_set_flags(int h, uint32_t flags) {
    if (h >= 0 && h < win_count) windows[h].win_flags = flags;
}
uint32_t comp_get_flags(int h) {
    if (h >= 0 && h < win_count) return windows[h].win_flags;
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

/* Blocking readline with echo into window, with Up/Down arrow history */
int comp_win_term_readline(comp_term_t *t, char *buf, int max) {
#define TERM_HIST_N 8
    static char s_hist[TERM_HIST_N][256];
    static int  s_hist_count = 0;
    static int  s_hist_head  = 0;
    int pos = 0, hist_pos = -1;
    char saved[256]; saved[0] = '\0';
    buf[0] = '\0';
    while (1) {
        int c;
        while ((c = input_router_getchar(t->task_id)) < 0)
            sched_yield();
        if (c == '\n' || c == '\r') {
            comp_win_term_print(t, "\n");
            buf[pos] = '\0';
            if (pos > 0) {
                strncpy(s_hist[s_hist_head], buf, 255);
                s_hist[s_hist_head][255] = '\0';
                s_hist_head = (s_hist_head + 1) % TERM_HIST_N;
                if (s_hist_count < TERM_HIST_N) s_hist_count++;
            }
            return pos;
        }
        if (c == 0x1B) { buf[0] = '\0'; return -1; }
        if (c == '\b') {
            if (pos > 0) { pos--; buf[pos] = '\0'; comp_win_term_print(t, "\b"); }
            continue;
        }
        if ((uint8_t)c == KB_KEY_UP) {
            if (s_hist_count == 0) continue;
            if (hist_pos < 0) { strncpy(saved,buf,255); saved[255]='\0'; hist_pos=0; }
            else if (hist_pos < s_hist_count-1) hist_pos++;
            else continue;
            for (int e=0;e<pos;e++) comp_win_term_print(t,"\b");
            int slot=(s_hist_head-1-hist_pos+TERM_HIST_N*2)%TERM_HIST_N;
            strncpy(buf,s_hist[slot],(size_t)(max-1)); buf[max-1]='\0';
            pos=(int)strlen(buf); comp_win_term_print(t,buf); continue;
        }
        if ((uint8_t)c == KB_KEY_DOWN) {
            if (hist_pos < 0) continue;
            for (int e=0;e<pos;e++) comp_win_term_print(t,"\b");
            if (hist_pos > 0) {
                hist_pos--;
                int slot=(s_hist_head-1-hist_pos+TERM_HIST_N*2)%TERM_HIST_N;
                strncpy(buf,s_hist[slot],(size_t)(max-1)); buf[max-1]='\0';
            } else { hist_pos=-1; strncpy(buf,saved,(size_t)(max-1)); buf[max-1]='\0'; }
            pos=(int)strlen(buf); comp_win_term_print(t,buf); continue;
        }
        if ((unsigned char)c >= 0x20 && pos < max - 1) {
            buf[pos++] = (char)c; buf[pos] = '\0';
            char echo[2] = { (char)c, '\0' };
            comp_win_term_print(t, echo);
        }
    }
#undef TERM_HIST_N
}
