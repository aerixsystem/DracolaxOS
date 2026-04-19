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
#include "../../../kernel/arch/x86_64/rtc.h"
#include "../../../kernel/security/dracoauth.h"
#include "../../../kernel/klibc.h"
#include "../../../kernel/log.h"
#include "../../../kernel/sched/sched.h"
#include "../../../kernel/sched/task.h"     /* task_t, TASK_DEAD, TASK_EMPTY */
#include "../../../kernel/drivers/ps2/input_router.h"
#include "appman.h"
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
static dock_slot_t g_slots[DOCK_PINS_MAX + APP_MAX];
static int         g_slot_count  = 0;
static int         g_scroll_off  = 0;
static int         g_hover_idx   = -1;

/* Icon pixel storage — fixed allocation, no per-frame kmalloc */
static uint32_t   g_icon_px[DOCK_PINS_MAX + APP_MAX][DOCK_ICON_SZ * DOCK_ICON_SZ];
static dxi_icon_t g_icons  [DOCK_PINS_MAX + APP_MAX];
static int        g_icons_loaded = 0;

/* Default + user-pinned names */
static char g_pinned[DOCK_PINS_MAX][32];
static int  g_pin_count = 0;

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

static void rebuild_slots(void) {
    g_slot_count = 0;
    /* 1 — pinned entries first */
    for (int p = 0; p < g_pin_count; p++) {
        strncpy(g_slots[g_slot_count].name, g_pinned[p], 31);
        g_slots[g_slot_count].name[31] = '\0';
        g_slots[g_slot_count].pinned   = 1;
        g_slots[g_slot_count].running  = 0;
        g_slots[g_slot_count].task_id  = -1;
        g_slot_count++;
    }
    /* 2 — registered apps that are NOT pinned */
    int total = appman_count();
    for (int i = 0; i < total && g_slot_count < DOCK_PINS_MAX + APP_MAX; i++) {
        const app_entry_t *a = appman_get(i);
        if (!a) continue;
        int already = 0;
        for (int p = 0; p < g_pin_count; p++)
            if (strcmp(g_pinned[p], a->name) == 0) { already = 1; break; }
        if (!already) {
            strncpy(g_slots[g_slot_count].name, a->name, 31);
            g_slots[g_slot_count].name[31] = '\0';
            g_slots[g_slot_count].pinned   = 0;
            g_slots[g_slot_count].running  = 0;
            g_slots[g_slot_count].task_id  = -1;
            g_slot_count++;
        }
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

static app_icon_style_t get_app_style(const char *name) {
    struct { const char *key; app_icon_style_t s; } table[] = {
        { "Terminal",        { 0x1A0A3Au, 0xE040FFu, ">_" } },
        { "File Manager",    { 0x0A2A3Au, 0x40C8FFu, "//" } },
        { "Text Editor",     { 0x0A1A3Au, 0x80C0FFu, "Aa" } },
        { "Calculator",      { 0x3A0A1Au, 0xFF6090u, "+=" } },
        { "Settings",        { 0x1A2A3Au, 0x60D0FFu, "::" } },
        { "System Monitor",  { 0x0A2A1Au, 0x40FFB0u, "Mn" } },
        { "Package Manager", { 0x2A1A0Au, 0xFFB040u, "PM" } },
        { "Draco Shield",    { 0x1A0A0Au, 0xFF4040u, "DS" } },
        { "Draco Manager",   { 0x2A0A2Au, 0xC040FFu, "DM" } },
        { "Login Manager",   { 0x0A0A2Au, 0x8080FFu, "LM" } },
        { "Paint",           { 0x2A1A0Au, 0xFFCC40u, "Pa" } },
        { "Image Viewer",    { 0x0A2A2Au, 0x40FFEEu, "IV" } },
        { "Media Player",    { 0x1A1A0Au, 0xFFFF40u, "|>" } },
        { "Disk Manager",    { 0x1A1A1Au, 0xC0C0C0u, "Dk" } },
        { "Trash Manager",   { 0x2A1A1Au, 0xFF8060u, "Tr" } },
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
        /* Programmatic icon: filled rounded-square + symbol text */
        const char *nm = (slot_idx >= 0 && slot_idx < g_slot_count)
                         ? g_slots[slot_idx].name : "??";
        app_icon_style_t style = get_app_style(nm);

        uint32_t sq  = (uint32_t)(DOCK_ICON_SZ - 4);
        uint32_t sqx = cx - sq / 2;
        uint32_t sqy = cy - sq / 2;

        /* Step 1: solid fill with a rectangle then mask corners with bg tint.
         * Since fb_rounded_rect only draws an outline, we fill the inner rect
         * and let the rounded border overdraw the square corners visually.
         * Inner fill (slightly smaller to avoid square corners sticking out) */
        uint32_t ir = 9u;
        /* Fill full square */
        fb_fill_rect(sqx + ir/2, sqy, sq - ir, sq, style.bg);       /* vertical strip */
        fb_fill_rect(sqx, sqy + ir/2, sq, sq - ir, style.bg);       /* horizontal strip */
        /* Draw border ring (acts as rounded corner mask + glow) */
        uint32_t glow = fb_blend(style.fg, style.bg, 80u);
        fb_rounded_rect(sqx, sqy, sq, sq, ir, glow);

        /* Step 2: symbol centred */
        uint32_t sw2 = 2u * 8u;   /* 2 chars × FONT_W=8 */
        uint32_t sx  = (sw2 < sq) ? sqx + (sq - sw2) / 2 : sqx;
        uint32_t sy  = cy - 8u;   /* vertically centred: FONT_H=16, so -8 */
        fb_print(sx, sy, style.sym, style.fg, 0u);   /* bg=0 → transparent-ish */
    }

    /* ── Running dot below the icon ── */
    if (slot_idx >= 0 && slot_idx < g_slot_count &&
        g_slots[slot_idx].running) {
        uint32_t dot_y = cy + half + 3;
        uint32_t dot_x = cx - 3;
        fb_rounded_rect(dot_x, dot_y, 6, 6, 3,
                        active ? COL_ACCENT_LT : COL_DOT_IDLE);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════ */

void dock_init(void) {
    /* Default pinned apps — only Terminal until other apps are implemented */
    static const char *defaults[] = { "Terminal", NULL };
    g_pin_count = 0;
    for (int i = 0; defaults[i] && g_pin_count < DOCK_PINS_MAX; i++) {
        strncpy(g_pinned[g_pin_count], defaults[i], 31);
        g_pinned[g_pin_count][31] = '\0';
        g_pin_count++;
    }
    rebuild_slots();
}

int dock_panel_x(void) { return g_px; }
int dock_panel_y(void) { return g_py; }
int dock_panel_w(void) { return g_pw; }
int dock_panel_h(void) { return g_ph; }
int dock_hover_slot(void) { return g_hover_idx; }

void dock_draw(int cursor_x, int cursor_y) {
    if (!fb.available) return;
    if (!g_icons_loaded) load_icons();

    /* ── Sync running state: clear slots whose task has exited ──────────
     * dock_click() sets running=1 and task_id when an app launches.
     * We check each frame whether that task is still alive; if not,
     * clear the slot so the running dot disappears automatically. */
    for (int s = 0; s < g_slot_count; s++) {
        if (!g_slots[s].running) continue;
        int tid = g_slots[s].task_id;
        if (tid < 0) continue;
        task_t *t = sched_task_at(tid);
        if (!t || t->state == TASK_DEAD || t->state == TASK_EMPTY) {
            g_slots[s].running  = 0;
            g_slots[s].task_id  = -1;
        }
    }

    uint32_t FH = fb.height;

    /* ── panel sizing ── */
    int visible = g_slot_count - g_scroll_off;
    if (visible > DOCK_MAX_VISIBLE) visible = DOCK_MAX_VISIBLE;
    if (visible < 1) visible = 1;

    int icon_area = visible * (DOCK_ICON_SZ + DOCK_ICON_GAP) - DOCK_ICON_GAP;
    /* Info area: user line + clock line + spacing */
    int info_area = FONT_H + FONT_H + 10;
    int sep_h     = 12;
    int panel_h   = DOCK_PAD_TOP + icon_area + sep_h + info_area + DOCK_PAD_BOT;

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

        int active = g_slots[si].running;
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

    /* ── separator ── */
    int sep_y  = panel_y + DOCK_PAD_TOP + icon_area + sep_h / 2;
    int info_y = sep_y + sep_h / 2 + 2;   /* y of first text row */
    fb_fill_rect((uint32_t)(panel_x + 10), (uint32_t)sep_y,
                 (uint32_t)(panel_w - 20), 1, COL_SEP);

    /* ── Info area: solid fill strip so text background is exact ──
     * The glass effect blurs whatever wallpaper pixels are behind the panel,
     * making the per-pixel colour unpredictable. Drawing text with a guessed
     * TEXT_BG value causes unlit font pixels to be a slightly different shade
     * than the actual background — creating the "corrupted character" look.
     * Filling a solid strip first guarantees exact bg/fg contrast. */
    const uint32_t INFO_BG = 0x0D0F22u;  /* matches TINT_R/G/B packed as pixel */
    fb_fill_rect((uint32_t)panel_x, (uint32_t)info_y,
                 (uint32_t)panel_w, (uint32_t)(FONT_H * 2 + 6), INFO_BG);

    /* ── user label ── */
    const char *who = dracoauth_whoami();
    char who7[8];
    strncpy(who7, who, 7); who7[7] = '\0';
    uint32_t uw = (uint32_t)strlen(who7) * FONT_W;
    uint32_t ux = (uint32_t)panel_x +
                  ((uint32_t)panel_w > uw ? ((uint32_t)panel_w - uw) / 2 : 2);
    fb_print(ux, (uint32_t)info_y, who7, COL_TEXT_MED, INFO_BG);

    /* ── clock — HH:MM ──
     * BUG FIX: snprintf("%02u", dt.hour) passes uint8_t through a variadic —
     * klibc's vsnprintf may read the wrong number of bytes from the va_list,
     * producing garbled digits (e.g. "8:" instead of "08:").
     * Build the string manually to guarantee correct digit extraction. */
    rtc_time_t dt;
    rtc_read(&dt);
    char clk[6];
    clk[0] = (char)('0' + (dt.hour / 10) % 10);
    clk[1] = (char)('0' + dt.hour % 10);
    clk[2] = ':';
    clk[3] = (char)('0' + (dt.min / 10) % 10);
    clk[4] = (char)('0' + dt.min % 10);
    clk[5] = '\0';
    uint32_t cw2 = 5u * (uint32_t)FONT_W;   /* always 5 chars = 40px */
    uint32_t cx2 = (uint32_t)panel_x +
                   ((uint32_t)panel_w > cw2 ? ((uint32_t)panel_w - cw2) / 2 : 2);
    fb_print(cx2, (uint32_t)(info_y + FONT_H + 2), clk, COL_TEXT_HI, INFO_BG);
}

int dock_click(int x, int y) {
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
            /* ── Launch (or focus) the app ── */
            const char *name = g_slots[si].name;
            if (g_slots[si].running) {
                /* App already running — raise its compositor window and
                 * route keyboard input to it. */
                kinfo("DOCK: focusing '%s' (task %d)\n",
                      name, g_slots[si].task_id);
                input_router_set_focus(g_slots[si].task_id);
                /* Find the compositor window owned by this task and focus it */
                int fw = comp_focused_window();
                /* Walk all windows via comp_get_task_id to find the right one */
                for (int wi = 0; wi < COMP_MAX_WINDOWS; wi++) {
                    if (comp_get_task_id(wi) == g_slots[si].task_id) {
                        comp_focus_window(wi);
                        comp_set_visible(wi, 1); /* un-minimise if needed */
                        break;
                    }
                }
                (void)fw;
            } else {
                int tid = appman_launch(name);
                if (tid >= 0) {
                    g_slots[si].running  = 1;
                    g_slots[si].task_id  = tid;
                    kinfo("DOCK: launched '%s' (task %d)\n", name, tid);
                    /* Yield immediately so the app task runs its first slice
                     * (which calls comp_create_window) before we return. */
                    sched_yield();
                } else {
                    kwarn("DOCK: launch failed for '%s'\n", name);
                }
            }
            return 1;
        }
    }
    return 1; /* click was inside panel but hit nothing actionable */
}

int dock_right_click(int x, int y) {
    if (x < g_px || x >= g_px + g_pw) return 0;
    if (y < g_py || y >= g_py + g_ph) return 0;
    /* Only consume the click if the cursor is actually over an icon slot */
    if (g_hover_idx < 0 || g_hover_idx >= g_slot_count) return 0;
    /* Right-click on a slot → toggle pin */
    if (g_slots[g_hover_idx].pinned) {
        const char *nm = g_slots[g_hover_idx].name;
        for (int p = 0; p < g_pin_count; p++) {
            if (strcmp(g_pinned[p], nm) == 0) {
                for (int j = p; j < g_pin_count - 1; j++)
                    strncpy(g_pinned[j], g_pinned[j + 1], 31);
                g_pin_count--;
                break;
            }
        }
    } else {
        if (g_pin_count < DOCK_PINS_MAX) {
            strncpy(g_pinned[g_pin_count], g_slots[g_hover_idx].name, 31);
            g_pinned[g_pin_count][31] = '\0';
            g_pin_count++;
        }
    }
    rebuild_slots();
    return 1;
}

void dock_scroll(int delta) {
    g_scroll_off += delta;
    if (g_scroll_off < 0) g_scroll_off = 0;
    int max_off = g_slot_count - DOCK_MAX_VISIBLE;
    if (max_off < 0) max_off = 0;
    if (g_scroll_off > max_off) g_scroll_off = max_off;
}

void dock_task_died(int task_id) {
    for (int i = 0; i < g_slot_count; i++) {
        if (g_slots[i].task_id == task_id) {
            g_slots[i].running = 0;
            g_slots[i].task_id = -1;
        }
    }
}

/* ── Per-slot metadata (used by ctx_menu layer) ──────────────────────── */

const char *dock_slot_name(int slot) {
    if (slot < 0 || slot >= g_slot_count) return "";
    return g_slots[slot].name;
}

int dock_slot_is_pinned(int slot) {
    if (slot < 0 || slot >= g_slot_count) return 0;
    return g_slots[slot].pinned;
}

int dock_slot_is_running(int slot) {
    if (slot < 0 || slot >= g_slot_count) return 0;
    return g_slots[slot].running;
}

int dock_slot_task_id(int slot) {
    if (slot < 0 || slot >= g_slot_count) return -1;
    return g_slots[slot].task_id;
}

/* Toggle pin state for a slot. Mirrors the logic inside dock_right_click. */
void dock_pin_toggle(int slot) {
    if (slot < 0 || slot >= g_slot_count) return;
    const char *nm = g_slots[slot].name;
    if (g_slots[slot].pinned) {
        /* Unpin: remove from g_pinned */
        for (int p = 0; p < g_pin_count; p++) {
            if (strcmp(g_pinned[p], nm) == 0) {
                for (int j = p; j < g_pin_count - 1; j++)
                    strncpy(g_pinned[j], g_pinned[j + 1], 31);
                g_pin_count--;
                rebuild_slots();
                return;
            }
        }
    } else {
        /* Pin */
        if (g_pin_count < DOCK_PINS_MAX) {
            strncpy(g_pinned[g_pin_count], nm, 31);
            g_pinned[g_pin_count][31] = '\0';
            g_pin_count++;
            rebuild_slots();
        }
    }
}

void dock_pin(int slot_idx) {
    if (slot_idx < 0 || slot_idx >= g_slot_count) return;
    if (g_slots[slot_idx].pinned || g_pin_count >= DOCK_PINS_MAX) return;
    strncpy(g_pinned[g_pin_count], g_slots[slot_idx].name, 31);
    g_pinned[g_pin_count][31] = '\0';
    g_pin_count++;
    rebuild_slots();
}

void dock_unpin(int slot_idx) {
    if (slot_idx < 0 || slot_idx >= g_slot_count) return;
    if (!g_slots[slot_idx].pinned) return;
    dock_right_click(g_px + 1, g_py + g_ph / 2); /* triggers unpin path */
}
