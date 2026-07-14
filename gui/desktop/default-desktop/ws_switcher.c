/* gui/desktop/default-desktop/ws_switcher.c
 * Workspace switcher overlay — Tab opens a 2×2 tile grid showing all four
 * virtual desktops. Click or 1-4 keys switch; Tab/Esc close without switching.
 *
 * Draw order each frame while open (desktop.c):
 *   draw_wallpaper()      — fresh wallpaper blit (always dirty while open)
 *   comp_render()         — compositor windows on current desktop
 *   ws_switcher_draw()    — tint + panel drawn on top
 *   dock_draw()           — dock floats above everything
 */
#include "../../../kernel/types.h"
#include "../../../kernel/klibc.h"
#include "../../../kernel/drivers/vga/fb.h"
#include "../../../kernel/drivers/ps2/keyboard.h"
#include "../../compositor/compositor.h"
#include "ws_switcher.h"

/* ── Layout constants ─────────────────────────────────────────────────── */
#define WS_COUNT     4
#define TILE_COLS    2
#define TILE_ROWS    2
#define TILE_W       110
#define TILE_H        90
#define TILE_GAP      14
#define PANEL_PAD_X   16
#define PANEL_PAD_Y   12
#define CORNER_R      14
#define FONT_W         8
#define FONT_H        16

/* ── Colours ──────────────────────────────────────────────────────────── */
#define TINT_R        0x0Au
#define TINT_G        0x0Cu
#define TINT_B        0x1Cu
#define TINT_A        210u

#define COL_OVERLAY_BG   0x08u        /* blue component of full-screen tint */
#define COL_PANEL_BG     0x12183Cu    /* panel fill */
#define COL_PANEL_EDGE   0x3A2870u    /* panel border */
#define COL_TILE_NORMAL  0x1A2050u
#define COL_TILE_HOVER   0x2A3070u
#define COL_TILE_CURRENT 0x7828C8u
#define COL_ACCENT       0x6030A0u
#define COL_ACCENT_LT    0xB060FFu
#define COL_TEXT_HI      0xF0E8FFu
#define COL_TEXT_MED     0xA090C0u
#define COL_TEXT_DIM     0x605080u
#define COL_SEP          0x302050u

/* ── State ────────────────────────────────────────────────────────────── */
static int g_open = 0;
static int g_px = 0, g_py = 0, g_pw = 0, g_ph = 0;
/* BUG FIX (arrow keys + Enter didn't work for workspace switching, and
 * stole the keys from the cursor-movement feature instead): Up/Down now
 * move this keyboard-driven selection (independent of mouse hover, same
 * pattern as the dock's g_sel_idx), Enter activates it. Reset to the
 * current workspace whenever the switcher opens. */
static int g_kbd_sel = 0;

/* ── Helpers ──────────────────────────────────────────────────────────── */

/* Apply a full-screen dark tint to the current shadow buffer contents. */
static void tint_screen(void) {
    if (!fb.available) return;
    uint32_t *shadow = fb_shadow_ptr();
    if (!shadow) return;
    uint32_t W = fb.width, H = fb.height;
    uint32_t inv = 255u - TINT_A;
    for (uint32_t i = 0; i < W * H; i++) {
        uint32_t p = shadow[i];
        uint8_t r = (uint8_t)(((( p >> 16) & 0xFF) * inv + TINT_R * TINT_A) / 255u);
        uint8_t g = (uint8_t)((((  p >>  8) & 0xFF) * inv + TINT_G * TINT_A) / 255u);
        uint8_t b = (uint8_t)(((   p        & 0xFF) * inv + TINT_B * TINT_A) / 255u);
        shadow[i] = fb_color(r, g, b);
    }
}

/* Compute top-left pixel of workspace tile ws inside the panel. */
static void tile_geometry(int ws, int px, int py,
                           int content_x0, int content_y0,
                           int *tx, int *ty) {
    int col = ws % TILE_COLS;
    int row = ws / TILE_COLS;
    *tx = content_x0 + col * (TILE_W + TILE_GAP);
    *ty = content_y0 + row * (TILE_H + TILE_GAP);
    (void)px; (void)py;
}

/* ── Public API ───────────────────────────────────────────────────────── */

void ws_switcher_open(int current_ws)  {
    g_open = 1;
    g_kbd_sel = (current_ws >= 0 && current_ws < WS_COUNT) ? current_ws : 0;
}
void ws_switcher_close(void)   { g_open = 0; }
int  ws_switcher_is_open(void) { return g_open; }

void ws_switcher_draw(int cursor_x, int cursor_y, int current_ws) {
    if (!g_open || !fb.available) return;

    uint32_t W = fb.width, H = fb.height;

    /* Full-screen dim overlay — applied every frame.
     * The desktop marks wallpaper dirty each frame while the switcher is open,
     * so the shadow buffer always holds a fresh wallpaper blit before we reach
     * here. Tinting every frame is safe and avoids the one-frame flash that
     * occurred with the previous single-tint-per-open-session approach. */
    tint_screen();

    /* ── panel sizing ── */
    int grid_w  = TILE_COLS * TILE_W  + (TILE_COLS - 1) * TILE_GAP;
    int grid_h  = TILE_ROWS * TILE_H  + (TILE_ROWS - 1) * TILE_GAP;
    int pw      = grid_w  + PANEL_PAD_X * 2;
    int ph      = grid_h  + PANEL_PAD_Y * 3 + FONT_H * 2 + 8;
    int px      = ((int)W - pw) / 2;
    int py      = ((int)H - ph) / 2;

    g_px = px; g_py = py; g_pw = pw; g_ph = ph;

    /* ── panel background (glass fill + border ring) ── */
    uint32_t *shadow = fb_shadow_ptr();
    if (shadow) {
        for (uint32_t dy = 0; dy < (uint32_t)ph; dy++) {
            for (uint32_t dx = 0; dx < (uint32_t)pw; dx++) {
                uint32_t p = shadow[((uint32_t)py + dy) * W + (uint32_t)px + dx];
                uint8_t r = (uint8_t)(((( p >> 16) & 0xFF) * 20u + COL_OVERLAY_BG * 235u) / 255u);
                uint8_t g = (uint8_t)((((  p >>  8) & 0xFF) * 20u) / 255u + 0x0Cu);
                uint8_t b = (uint8_t)(((   p        & 0xFF) * 20u) / 255u + 0x1Cu);
                shadow[((uint32_t)py + dy) * W + (uint32_t)px + dx] = fb_color(r, g, b);
            }
        }
    }
    fb_rounded_rect((uint32_t)px, (uint32_t)py,
                    (uint32_t)pw, (uint32_t)ph,
                    (uint32_t)CORNER_R, COL_PANEL_EDGE);

    /* ── title ── */
    const char *title = "Switch Workspace";
    uint32_t tw = (uint32_t)strlen(title) * FONT_W;
    fb_print((uint32_t)(px + (pw - (int)tw) / 2),
             (uint32_t)(py + PANEL_PAD_Y),
             title, COL_TEXT_HI, 0);

    /* separator */
    fb_fill_rect((uint32_t)(px + 12),
                 (uint32_t)(py + PANEL_PAD_Y + FONT_H + 4),
                 (uint32_t)(pw - 24), 1, COL_SEP);

    /* ── tiles ── */
    int content_x0 = px + PANEL_PAD_X;
    int content_y0 = py + PANEL_PAD_Y + FONT_H + 10;

    for (int ws = 0; ws < WS_COUNT; ws++) {
        int tx, ty;
        tile_geometry(ws, px, py, content_x0, content_y0, &tx, &ty);

        int hover   = (cursor_x >= tx && cursor_x < tx + TILE_W &&
                       cursor_y >= ty && cursor_y < ty + TILE_H);
        int current = (ws == current_ws);
        /* BUG FIX (arrow keys + Enter didn't work for workspace
         * switching): also highlight the keyboard-driven selection
         * (g_kbd_sel, moved by Up/Down), same visual treatment as mouse
         * hover, so arrow-key navigation is visible. */
        if (ws == g_kbd_sel) hover = 1;

        uint32_t tile_col = current ? COL_TILE_CURRENT
                          : hover   ? COL_TILE_HOVER
                                    : COL_TILE_NORMAL;
        uint32_t bord_col = current ? COL_ACCENT_LT
                          : hover   ? COL_ACCENT
                                    : COL_PANEL_EDGE;

        fb_rounded_rect((uint32_t)tx, (uint32_t)ty,
                        (uint32_t)TILE_W, (uint32_t)TILE_H, 10, tile_col);
        fb_rounded_rect((uint32_t)tx, (uint32_t)ty,
                        (uint32_t)TILE_W, (uint32_t)TILE_H, 10, bord_col);

        /* Workspace number — centred at 2× scale.
         * BUG FIX (workspace switcher doesn't mark workspaces with open
         * windows): append "?" next to the number when this workspace
         * has at least one open window, per the reported z-order/UX
         * spec's requested indicator. */
        char num[6];
        int nwins_here = comp_count_windows_on_desktop(ws);
        if (nwins_here > 0) snprintf(num, sizeof(num), "%d?", ws + 1);
        else                 snprintf(num, sizeof(num), "%d", ws + 1);
        uint32_t nw = (uint32_t)strlen(num) * (FONT_W * 2);
        uint32_t nx = (uint32_t)tx + ((uint32_t)TILE_W - nw) / 2;
        uint32_t ny = (uint32_t)ty + ((uint32_t)TILE_H - FONT_H * 2) / 2;
        fb_print_s(nx, ny, num,
                   (current || hover) ? COL_TEXT_HI : COL_TEXT_MED,
                   tile_col, 2);

        /* "active" label for current workspace */
        if (current) {
            const char *act = "active";
            uint32_t aw = (uint32_t)strlen(act) * FONT_W;
            fb_print((uint32_t)tx + ((uint32_t)TILE_W - aw) / 2,
                     ny + FONT_H * 2 + 4,
                     act, COL_ACCENT_LT, tile_col);
        }

        /* App count */
        int nwins = comp_count_windows_on_desktop(ws);
        if (nwins > 0) {
            char wbuf[16];
            snprintf(wbuf, sizeof(wbuf), "%d app%s", nwins, nwins == 1 ? "" : "s");
            uint32_t ww = (uint32_t)strlen(wbuf) * FONT_W;
            fb_print((uint32_t)tx + ((uint32_t)TILE_W > ww
                                     ? ((uint32_t)TILE_W - ww) / 2 : 2u),
                     (uint32_t)(ty + TILE_H - FONT_H * 2 - 6),
                     wbuf,
                     current ? COL_ACCENT_LT : COL_TEXT_DIM, tile_col);
        }

        /* Tiny workspace number in corner */
        char hint[2] = { (char)('1' + ws), '\0' };
        fb_print((uint32_t)(tx + TILE_W - FONT_W - 6),
                 (uint32_t)(ty + TILE_H - FONT_H - 4),
                 hint, COL_TEXT_DIM, tile_col);
    }

    /* ── hint text ── */
    const char *hint_msg = "1-4 or click to switch   Tab/Esc to close";
    uint32_t hm_w = (uint32_t)strlen(hint_msg) * FONT_W;
    int hm_y = py + ph - PANEL_PAD_Y - FONT_H;
    fb_print((uint32_t)(px + (pw - (int)hm_w) / 2),
             (uint32_t)hm_y,
             hint_msg, COL_TEXT_DIM, 0);
}

int ws_switcher_click(int x, int y) {
    if (!g_open) return -1;
    /* Click outside the panel — close */
    if (x < g_px || x >= g_px + g_pw ||
        y < g_py || y >= g_py + g_ph) {
        g_open = 0;
        return -1;
    }
    /* Click on a tile */
    int content_x0 = g_px + PANEL_PAD_X;
    int content_y0 = g_py + PANEL_PAD_Y + FONT_H + 10;
    for (int ws = 0; ws < WS_COUNT; ws++) {
        int tx, ty;
        tile_geometry(ws, g_px, g_py, content_x0, content_y0, &tx, &ty);
        if (x >= tx && x < tx + TILE_W && y >= ty && y < ty + TILE_H) {
            g_open = 0;
            return ws;
        }
    }
    /* Click on panel background (not a tile) — also close */
    g_open = 0;
    return -1;
}

int ws_switcher_key(int c, int current_ws) {
    (void)current_ws;
    if (!g_open) return -1;
    /* Digit keys '1'-'4' */
    if (c >= '1' && c <= '4') {
        int ws = c - '1';
        if (ws < WS_COUNT) { ws_switcher_close(); return ws; }
    }
    /* F1-F4 keys (KB_KEY_F1=0x8A .. KB_KEY_F4=0x8D) */
    uint8_t uc = (uint8_t)c;
    if (uc >= KB_KEY_F1 && uc <= KB_KEY_F1 + 3) {
        int ws = (int)(uc - KB_KEY_F1);
        if (ws < WS_COUNT) { ws_switcher_close(); return ws; }
    }
    /* BUG FIX (arrow keys + Enter didn't work for workspace switching):
     * Up/Down move g_kbd_sel (wrapping so it's a quick way to cycle
     * through all 4 without needing to know which number is which),
     * Enter activates whichever one is currently selected — same
     * end result as pressing its digit/F-key directly, just reachable
     * without knowing the exact key up front. */
    if (uc == KB_KEY_UP)   { g_kbd_sel = (g_kbd_sel + WS_COUNT - 1) % WS_COUNT; return -1; }
    if (uc == KB_KEY_DOWN) { g_kbd_sel = (g_kbd_sel + 1) % WS_COUNT;            return -1; }
    if (c == '\n' || c == '\r') {
        int ws = g_kbd_sel;
        ws_switcher_close();
        return ws;
    }
    if (c == '\t' || c == 0x1B || (uint8_t)c == 0xC8u) {  /* 0xC8 = SUPER_KEY('i') */
        ws_switcher_close(); return -1;
    }
    return -1;
}
