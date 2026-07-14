/* gui/desktop/default-desktop/dock.c
 * Floating glassmorphism side dock panel.
 *
 * Visual anatomy:
 *
 *   ┌──────┐  ← rounded panel edge (DOCK_CORNER_R)
 *   │      │
 *   │  ●   │  icon slot (dxi or initials fallback)
 *   │      │
 *   │▌ ●   │  ← active: 4 px left-rim bar + highlight box
 *   │      │
 *   │ [●]  │  ← hover: semi-transparent rounded highlight
 *   │      │
 *   │  ●   │
 *   │  ·   │  ← running dot below icon
 *   │──────│  separator
 *   │ user │  username (truncated to fit)
 *   │ HH:MM│  clock
 *   └──────┘
 */
#include "dock.h"
#include "../../../kernel/drivers/vga/fb.h"
#include "../../../kernel/klibc.h"
#include "../../../kernel/log.h"
#include "../../../kernel/sched/sched.h"
#include "../../../kernel/drivers/ps2/input_router.h"
#include "../../../kernel/dxi/dxi.h"
#include "../../compositor/compositor.h"
#include "desktop.h"

/* ── palette ──────────────────────────────────────────────── */
#define COL_PANEL_EDGE  0x2A2D56u
#define COL_ICON_BG     0x1C1F3Eu
#define COL_ICON_HOVER  0x2E3368u
#define COL_ICON_ACTIVE 0x3C1868u
#define COL_ACTIVE_BAR  0xA050F0u
#define COL_ACCENT      0x7828C8u
#define COL_ACCENT_LT   0xA050F0u
#define COL_TEXT_HI     0xF0F0FFu
#define COL_TEXT_MED    0xA0A0C8u
#define COL_TEXT_DIM    0x60607Au
#define COL_SEP         0x2A2C50u
#define COL_DOT_IDLE    0x6060A0u

/* Frosted-glass tint components (dark navy) */
#define TINT_R  0x0Du
#define TINT_G  0x0Fu
#define TINT_B  0x22u
#define TINT_A  155u   /* 0-255: reduced from 210 — lets wallpaper bleed through glass */

#define FONT_W  8
#define FONT_H  16

/* ── module state ─────────────────────────────────────────── */
static dock_slot_t g_slots[DOCK_SLOTS_MAX];
static int         g_slot_count  = 0;
static int         g_scroll_off  = 0;
static int         g_hover_idx   = -1;
/* BUG FIX (dock keyboard navigation didn't exist): Up/Down + Enter did
 * nothing because nothing tracked a keyboard-driven selection at all —
 * only mouse hover/click were ever handled. g_sel_idx is that selection,
 * separate from mouse hover so the two can coexist (moving the mouse
 * doesn't fight with arrow-key navigation, matching how most switcher
 * UIs behave). Reset to 0 whenever the panel opens. */
static int         g_sel_idx     = 0;
/* REPURPOSED: hidden by default, shown only while toggled on (Alt+Tab —
 * see desktop.c). Previously this panel was always drawn every frame. */
static int         g_dock_open   = 0;

/* Icon pixel storage — fixed allocation, no per-frame kmalloc */
static uint32_t   g_icon_px[DOCK_SLOTS_MAX][DOCK_ICON_SZ * DOCK_ICON_SZ];
static dxi_icon_t g_icons  [DOCK_SLOTS_MAX];
static int        g_icons_loaded = 0;

/* Cached panel geometry filled in dock_draw() */
static int g_px = DOCK_PANEL_X, g_py = 0, g_pw = DOCK_W, g_ph = 0;

/* ── helpers ──────────────────────────────────────────────── */

/* Convert "App Name" → "app-name.dxi" */
static void name_to_dxi_path(const char *name, char *out, size_t max) {
    char fname[64];
    int j = 0;
    for (int i = 0; name[i] && j < 59; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c == ' ') c = '-';
        fname[j++] = c;
    }
    fname[j] = '\0';
    snprintf(out, max, "/storage/main/system/shared/images/%s.dxi", fname);
}

static void load_icons(void) {
    for (int i = 0; i < g_slot_count; i++) {
        char path[128];
        name_to_dxi_path(g_slots[i].name, path, sizeof(path));
        g_icons[i].pixels = g_icon_px[i];
        g_icons[i].loaded = 0;
        dxi_load(path, &g_icons[i]);
        sched_yield(); /* don't starve the watchdog during icon scan */
    }
    g_icons_loaded = 1;
}

/* REPURPOSED: this used to build the slot list from pinned names + the
 * full app registry (appman_count()/appman_get()), showing every
 * launchable app whether or not it was actually running. Now it mirrors
 * reality directly: one slot per currently-open window on the current
 * desktop — SHOWN or MINIMIZED (comp_window_exists(), not just
 * comp_window_is_visible() — a minimized window is still "open," and
 * this panel doubles as the hidden/minimized-apps switcher: clicking a
 * minimized slot un-minimises + focuses it) — sourced straight from the
 * compositor (see gui/compositor/compositor.h, added for this). Called
 * fresh each time the panel is drawn (cheap: capped at
 * COMP_MAX_WINDOWS==16, no heap allocation), so it can never show a
 * stale/closed window or miss one that just opened or was minimized. */
static void sync_slots_from_windows(void) {
    g_slot_count = 0;
    int total = comp_window_count();
    for (int h = 0; h < total && g_slot_count < DOCK_SLOTS_MAX; h++) {
        if (!comp_window_exists(h)) continue;
        const char *title = comp_window_title(h);
        strncpy(g_slots[g_slot_count].name, title, 31);
        g_slots[g_slot_count].name[31] = '\0';
        g_slots[g_slot_count].win_handle = h;
        g_slots[g_slot_count].task_id    = comp_get_task_id(h);
        g_slot_count++;
    }
    g_icons_loaded = 0; /* invalidate cache — next draw reloads */
}

/* 3×3 box-blur a rect of the shadow buffer in-place */
static void blur_region(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h) {
    uint32_t *shadow = fb_shadow_ptr();
    if (!shadow || w < 3 || h < 3) return;
    uint32_t SW = fb.width;
    /* Clamp to framebuffer */
    if (x0 + w > SW) w = SW - x0;
    if (y0 + h > fb.height) h = fb.height - y0;

    uint32_t tmp[128]; /* dock is 64 px wide — 128 is safe */
    uint32_t use_w = (w < 128) ? w : 128;

    for (uint32_t row = y0 + 1; row < y0 + h - 1; row++) {
        for (uint32_t col = x0 + 1; col < x0 + use_w - 1; col++) {
            uint32_t sr = 0, sg = 0, sb = 0;
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    uint32_t p = shadow[(row + dr) * SW + (col + dc)];
                    sr += (p >> 16) & 0xFFu;
                    sg += (p >>  8) & 0xFFu;
                    sb +=  p        & 0xFFu;
                }
            }
            tmp[col - x0] = fb_color((uint8_t)(sr / 9),
                                      (uint8_t)(sg / 9),
                                      (uint8_t)(sb / 9));
        }
        for (uint32_t col = x0 + 1; col < x0 + use_w - 1; col++)
            shadow[row * SW + col] = tmp[col - x0];
    }
}

/* Apply frosted-glass tint over a rect of the shadow buffer */
static void tint_region(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h) {
    uint32_t *shadow = fb_shadow_ptr();
    if (!shadow) return;
    uint32_t SW = fb.width;
    uint32_t inv = 255u - TINT_A;
    for (uint32_t row = y0; row < y0 + h; row++) {
        for (uint32_t col = x0; col < x0 + w; col++) {
            uint32_t p = shadow[row * SW + col];
            uint8_t r = (uint8_t)(((( p >> 16) & 0xFF) * inv + TINT_R * TINT_A) / 255u);
            uint8_t g = (uint8_t)((((p >>  8) & 0xFF) * inv + TINT_G * TINT_A) / 255u);
            uint8_t b = (uint8_t)(((  p        & 0xFF) * inv + TINT_B * TINT_A) / 255u);
            shadow[row * SW + col] = fb_color(r, g, b);
        }
    }
}

/* ── Per-app icon colours and symbols ──────────────────────────────────
 * Matches the glassmorphism icon style in the OS design reference.
 * Each app gets a unique gradient base colour and a 2-char symbol.
 * When a .dxi icon file is present it overrides this fallback. */
typedef struct { uint32_t bg; uint32_t fg; const char *sym; } app_icon_style_t;

static const app_icon_style_t _default_style = { 0x2A2C50u, 0xA0A0C8u, "??" };

/* GUI finalization pass: all entries for removed apps were dropped (see
 * docs/CHANGELOG.md). Unrecognised names fall back to _default_style. */
static app_icon_style_t get_app_style(const char *name) {
    struct { const char *key; app_icon_style_t s; } table[] = {
        { "Widget Demo",     { 0x2A0A3Au, 0xC080FFu, "Wd" } },
    };
    for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++)
        if (strcmp(table[i].key, name) == 0) return table[i].s;
    return _default_style;
}

/* Draw one icon slot centred at (cx, cy) */
static void draw_icon_slot(int slot_idx,
                            uint32_t cx, uint32_t cy,
                            int hover, int active) {
    uint32_t half = (uint32_t)(DOCK_ICON_SZ / 2);

    /* ── Active indicator bar (4 px, left panel rim) ── */
    if (active) {
        uint32_t bar_h = (uint32_t)(DOCK_ICON_SZ - 12);
        uint32_t bar_y = cy - bar_h / 2;
        fb_rounded_rect((uint32_t)g_px, bar_y, 4, bar_h, 2, COL_ACTIVE_BAR);
    }

    /* ── Hover / active highlight box ── */
    if (hover || active) {
        uint32_t hx = cx - half - 6;
        uint32_t hy = cy - half - 4;
        uint32_t hw = (uint32_t)(DOCK_ICON_SZ + 12);
        uint32_t hh = (uint32_t)(DOCK_ICON_SZ + 8);
        fb_rounded_rect(hx, hy, hw, hh, 10,
                        hover ? COL_ICON_HOVER : COL_ICON_ACTIVE);
    }

    /* ── Icon content ── */
    if (slot_idx >= 0 && slot_idx < g_slot_count &&
        g_icons_loaded && g_icons[slot_idx].loaded) {
        /* DXI icon available — draw circular bg then blit icon */
        fb_rounded_rect(cx - half, cy - half,
                        (uint32_t)DOCK_ICON_SZ, (uint32_t)DOCK_ICON_SZ,
                        half, COL_ICON_BG);
        uint32_t iw = g_icons[slot_idx].width;
        uint32_t ih = g_icons[slot_idx].height;
        if (iw > (uint32_t)DOCK_ICON_SZ) iw = (uint32_t)DOCK_ICON_SZ;
        if (ih > (uint32_t)DOCK_ICON_SZ) ih = (uint32_t)DOCK_ICON_SZ;
        blit_icon_bgra(cx - iw/2, cy - ih/2,
                       g_icons[slot_idx].pixels, iw, ih, fb.width);
    } else {
        /* BUG FIX (no real icons): real hand-drawn vector icon, shared
         * with desktop icons and window titlebars — see
         * desktop_draw_vector_icon() in desktop.c. */
        const char *nm = (slot_idx >= 0 && slot_idx < g_slot_count)
                         ? g_slots[slot_idx].name : "??";
        app_icon_style_t style = get_app_style(nm);
        uint32_t sq  = (uint32_t)(DOCK_ICON_SZ - 4);
        uint32_t sqx = cx - sq / 2;
        uint32_t sqy = cy - sq / 2;
        desktop_draw_vector_icon(nm, sqx, sqy, sq, style.fg, style.bg);
    }

    /* REPURPOSED: the old "running dot" (every slot was running by
     * definition once repurposed, so it always showed and conveyed
     * nothing) is now a MINIMIZED indicator instead — meaningful again,
     * since the panel now lists both shown and minimized windows
     * together (see sync_slots_from_windows()). */
    if (slot_idx >= 0 && slot_idx < g_slot_count &&
        comp_window_is_minimized(g_slots[slot_idx].win_handle)) {
        uint32_t dot_y = cy + half + 3;
        uint32_t dot_x = cx - 3;
        fb_rounded_rect(dot_x, dot_y, 6, 6, 3, COL_DOT_IDLE);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════ */

void dock_init(void) {
    /* REPURPOSED: no more default pins — there's nothing to pin, the
     * panel only ever shows windows that actually exist right now (see
     * sync_slots_from_windows()). */
    g_slot_count = 0;
    g_dock_open  = 0;
}

void dock_open(void)   { g_dock_open = 1; g_scroll_off = 0; g_sel_idx = 0; }
void dock_close(void)  { g_dock_open = 0; }
int  dock_is_open(void){ return g_dock_open; }

/* BUG FIX (dock keyboard navigation didn't exist): Up/Down + Enter move
 * and activate a keyboard-driven selection, same slot list mouse click
 * already uses. sync_slots_from_windows() must have been called (by
 * dock_draw()) at least once since the panel opened for g_slot_count to
 * be meaningful — in practice this is always true, since desktop.c only
 * calls these after a frame where dock_draw() has already run. */
void dock_move_selection(int delta) {
    if (g_slot_count == 0) return;
    g_sel_idx += delta;
    if (g_sel_idx < 0) g_sel_idx = 0;
    if (g_sel_idx >= g_slot_count) g_sel_idx = g_slot_count - 1;
    /* Keep the selection scrolled into view */
    if (g_sel_idx < g_scroll_off) g_scroll_off = g_sel_idx;
    if (g_sel_idx >= g_scroll_off + DOCK_MAX_VISIBLE)
        g_scroll_off = g_sel_idx - DOCK_MAX_VISIBLE + 1;
}

int dock_activate_selected(void) {
    if (g_sel_idx < 0 || g_sel_idx >= g_slot_count) return 0;
    int h = g_slots[g_sel_idx].win_handle;
    kinfo("DOCK: focusing '%s' (window %d, task %d) via keyboard\n",
          g_slots[g_sel_idx].name, h, g_slots[g_sel_idx].task_id);
    comp_focus_window(h);
    comp_set_visible(h, 1);
    comp_clear_minimized(h);
    if (g_slots[g_sel_idx].task_id >= 0)
        input_router_set_focus(g_slots[g_sel_idx].task_id);
    dock_close();
    return 1;
}

int dock_panel_x(void) { return g_px; }
int dock_panel_y(void) { return g_py; }
int dock_panel_w(void) { return g_pw; }
int dock_panel_h(void) { return g_ph; }
int dock_hover_slot(void) { return g_hover_idx; }

void dock_draw(int cursor_x, int cursor_y) {
    if (!g_dock_open) return;
    if (!fb.available) return;
    if (!g_icons_loaded) load_icons();

    /* REPURPOSED: slots used to be built once and then patched frame-by-
     * frame to clear ones whose task died. Now they're rebuilt fresh from
     * the compositor's actual window list every draw — cheap (capped at
     * 16 windows) and can never go stale or show a window that's already
     * closed. */
    sync_slots_from_windows();

    uint32_t FH = fb.height;

    /* ── panel sizing ── */
    int visible = g_slot_count - g_scroll_off;
    if (visible > DOCK_MAX_VISIBLE) visible = DOCK_MAX_VISIBLE;
    if (visible < 1) visible = 1;

    int icon_area = visible * (DOCK_ICON_SZ + DOCK_ICON_GAP) - DOCK_ICON_GAP;
    /* REPURPOSED: no more clock/user footer (that's draw_widgets() in
     * desktop.c's job — see the top-right clock panel). This panel is
     * just the open-window icon list now. */
    int panel_h   = DOCK_PAD_TOP + icon_area + DOCK_PAD_BOT;

    /* Cap at 90% of screen height */
    int max_h = (int)FH * 9 / 10;
    if (panel_h > max_h) panel_h = max_h;

    int panel_x = DOCK_PANEL_X;
    int panel_y = ((int)FH - panel_h) / 2;
    int panel_w = DOCK_W;

    /* Cache for click detection */
    g_px = panel_x; g_py = panel_y;
    g_pw = panel_w; g_ph = panel_h;

    /* ── frosted glass: restore raw bg → blur → tint → border ──
     * CRITICAL: restore the raw wallpaper pixels FIRST each frame.
     * Without this, blur() re-blurs already-blurred pixels from the
     * previous frame, causing the panel background to darken each
     * frame until it becomes solid black within ~10 frames. */
    desktop_blit_bg_at((uint32_t)panel_x, (uint32_t)panel_y,
                       (uint32_t)panel_w, (uint32_t)panel_h);
    blur_region((uint32_t)panel_x, (uint32_t)panel_y,
                (uint32_t)panel_w, (uint32_t)panel_h);
    blur_region((uint32_t)panel_x, (uint32_t)panel_y,
                (uint32_t)panel_w, (uint32_t)panel_h); /* 2 passes = softer */
    tint_region((uint32_t)panel_x, (uint32_t)panel_y,
                (uint32_t)panel_w, (uint32_t)panel_h);
    /* Rounded border ring */
    fb_rounded_rect((uint32_t)panel_x, (uint32_t)panel_y,
                    (uint32_t)panel_w, (uint32_t)panel_h,
                    (uint32_t)DOCK_CORNER_R, COL_PANEL_EDGE);

    /* ── icon slots ── */
    int slot_cx  = panel_x + panel_w / 2;
    int icon_y0  = panel_y + DOCK_PAD_TOP + DOCK_ICON_SZ / 2;
    g_hover_idx  = -1;

    for (int vi = 0; vi < visible; vi++) {
        int si = vi + g_scroll_off;
        if (si >= g_slot_count) break;

        int cy_icon = icon_y0 + vi * (DOCK_ICON_SZ + DOCK_ICON_GAP);
        int half    = DOCK_ICON_SZ / 2 + 5;

        int hover = (cursor_x >= panel_x + 2 &&
                     cursor_x <  panel_x + panel_w - 2 &&
                     cursor_y >= cy_icon - half &&
                     cursor_y <  cy_icon + half);
        if (hover) g_hover_idx = si;
        /* BUG FIX (dock keyboard navigation): also highlight the
         * keyboard-driven selection (g_sel_idx, moved by Up/Down — see
         * dock_move_selection() in desktop.c), not just mouse hover, so
         * arrow-key navigation is visible. */
        if (si == g_sel_idx) hover = 1;

        /* REPURPOSED: every slot is now a real open window (there's no
         * more "pinned but not running" state) — the active-bar
         * indicator means "this is the currently focused window"
         * instead of the old "is a task running for this pin" check. */
        int active = (g_slots[si].win_handle == comp_focused_window());
        draw_icon_slot(si, (uint32_t)slot_cx, (uint32_t)cy_icon,
                       hover, active);
    }

    /* ── scroll arrows ── */
    uint32_t arr_x = (uint32_t)(panel_x + panel_w / 2 - FONT_W / 2);
    if (g_scroll_off > 0)
        fb_print(arr_x, (uint32_t)(panel_y + 2), "^", COL_TEXT_DIM, 0);
    if (g_scroll_off + visible < g_slot_count)
        fb_print(arr_x,
                 (uint32_t)(panel_y + DOCK_PAD_TOP + icon_area + 2),
                 "v", COL_TEXT_DIM, 0);
}

int dock_click(int x, int y) {
    if (!g_dock_open) return 0;
    /* Return 0 immediately if outside panel bounds */
    if (x < g_px || x >= g_px + g_pw) return 0;
    if (y < g_py || y >= g_py + g_ph) return 0;

    int visible = g_slot_count - g_scroll_off;
    if (visible > DOCK_MAX_VISIBLE) visible = DOCK_MAX_VISIBLE;

    int slot_cy_base = g_py + DOCK_PAD_TOP + DOCK_ICON_SZ / 2;

    for (int vi = 0; vi < visible; vi++) {
        int si      = vi + g_scroll_off;
        if (si >= g_slot_count) break;
        int cy_icon = slot_cy_base + vi * (DOCK_ICON_SZ + DOCK_ICON_GAP);
        int half    = DOCK_ICON_SZ / 2 + 5;

        if (y >= cy_icon - half && y < cy_icon + half) {
            /* REPURPOSED: every slot is a real, already-open window now
             * (see sync_slots_from_windows()) — clicking one focuses +
             * un-minimises it and routes keyboard input there, standard
             * alt-tab-switcher behaviour. There's no more "not running
             * yet, launch it" branch, since the panel never lists
             * anything that isn't already open. */
            int h = g_slots[si].win_handle;
            kinfo("DOCK: focusing '%s' (window %d, task %d)\n",
                  g_slots[si].name, h, g_slots[si].task_id);
            comp_focus_window(h);
            comp_set_visible(h, 1);       /* un-minimise if needed */
            comp_clear_minimized(h);
            if (g_slots[si].task_id >= 0)
                input_router_set_focus(g_slots[si].task_id);
            dock_close();
            return 1;
        }
    }
    return 1; /* click was inside panel but hit nothing actionable */
}

void dock_scroll(int delta) {
    g_scroll_off += delta;
    if (g_scroll_off < 0) g_scroll_off = 0;
    int max_off = g_slot_count - DOCK_MAX_VISIBLE;
    if (max_off < 0) max_off = 0;
    if (g_scroll_off > max_off) g_scroll_off = max_off;
}
