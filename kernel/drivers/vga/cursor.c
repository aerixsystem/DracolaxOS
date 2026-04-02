/* kernel/cursor.c — Pure-overlay cursor (no background save/restore)
 *
 * Model: every frame the UI redraws everything to shadow, calls fb_flip(),
 * then calls cursor_move(x, y).  cursor_move() stamps the cursor bitmap
 * directly onto VRAM.  Because the full UI is flipped first, VRAM always
 * has the clean frame as the background — there is nothing to save or
 * restore.  Transparent pixels (alpha == 0) are simply skipped so the
 * frame content shows through correctly.
 *
 * This eliminates:
 *   - Flicker from restore_bg overwriting freshly flipped pixels
 *   - Holes from stale saved pixels punching into the new frame
 *   - Complexity of double-buffering the cursor separately
 *
 * Frame call order:
 *   1. Draw full UI to shadow (cursor is NOT drawn here)
 *   2. fb_flip()            → shadow → VRAM (clean frame, no cursor)
 *   3. cursor_move(mx, my)  → stamp cursor directly onto VRAM
 */
#include "../../types.h"
#include "cursor.h"
#include "cursor_data.h"
#include "fb.h"
#include "../../klibc.h"

/* ── State ──────────────────────────────────────────────────────── */
static uint32_t      cur_x       = 0;
static uint32_t      cur_y       = 0;
static uint8_t       cur_visible = 0;
static cursor_type_t cur_type    = CURSOR_ARROW;

/* ── Cursor definitions ─────────────────────────────────────────── */
static const cursor_def_t cursor_defs[] = {
    /* CURSOR_ARROW    */ { 32, 32,  0,  0, cursor_arrow_data    },
    /* CURSOR_HAND     */ { 32, 32,  8,  2, cursor_hand_data     },
    /* CURSOR_TEXT     */ { 32, 32,  4, 16, cursor_text_data     },
    /* CURSOR_CROSSHAIR*/ { 32, 32, 16, 16, cursor_crosshair_data},
    /* CURSOR_BUSY     */ { 32, 32,  8,  8, cursor_busy_data     },
    /* CURSOR_RESIZE_H */ { 32, 32, 16,  7, cursor_resize_h_data },
    /* CURSOR_RESIZE_V */ { 32, 32,  7, 16, cursor_resize_v_data },
};
#define CURSOR_NDEFS ((int)(sizeof(cursor_defs)/sizeof(cursor_defs[0])))

/* ── Internal stamp ─────────────────────────────────────────────── */
static void stamp_cursor(uint32_t x, uint32_t y) {
    if (!fb.available) return;
    const cursor_def_t *def =
        &cursor_defs[cur_type < CURSOR_NDEFS ? (int)cur_type : 0];

    uint32_t draw_x = (x >= (uint32_t)def->hotspot_x)
                      ? x - (uint32_t)def->hotspot_x : 0;
    uint32_t draw_y = (y >= (uint32_t)def->hotspot_y)
                      ? y - (uint32_t)def->hotspot_y : 0;

    for (int row = 0; row < def->height; row++) {
        for (int col = 0; col < def->width; col++) {
            uint32_t pixel = def->pixels[row * def->width + col];
            uint8_t  alpha = (uint8_t)(pixel >> 24);
            if (alpha == 0) continue;  /* transparent — show frame behind */

            uint32_t px = draw_x + (uint32_t)col;
            uint32_t py = draw_y + (uint32_t)row;
            if (px >= fb.width || py >= fb.height) continue;

            uint32_t color = pixel & 0x00FFFFFFu;
            if (alpha < 255) {
                /* Read VRAM background for semi-transparent pixels */
                uint32_t bg = fb_read_pixel_vram(px, py);
                color = fb_blend(color, bg, alpha);
            }
            fb_put_pixel_vram(px, py, color);
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────── */

void cursor_init(void) {
    if (!fb.available) return;
    cur_x       = fb.width  / 2;
    cur_y       = fb.height / 2;
    cur_type    = CURSOR_ARROW;
    cur_visible = 1;
}

void cursor_set_type(cursor_type_t type) {
    if (type < CURSOR_NDEFS) cur_type = type;
}

/* cursor_move: update position and stamp onto VRAM.
 * Must be called AFTER fb_flip() every frame. */
void cursor_move(uint32_t x, uint32_t y) {
    if (!fb.available || !cur_visible) { cur_x = x; cur_y = y; return; }
    if (x >= fb.width)  x = fb.width  - 1;
    if (y >= fb.height) y = fb.height - 1;
    cur_x = x;
    cur_y = y;
    stamp_cursor(cur_x, cur_y);
}

void cursor_show(void) { cur_visible = 1; }
void cursor_hide(void) { cur_visible = 0; }

/* cursor_redraw: re-stamp at current position (call after fb_flip). */
void cursor_redraw(void) {
    if (!fb.available || !cur_visible) return;
    stamp_cursor(cur_x, cur_y);
}

uint32_t cursor_x(void) { return cur_x; }
uint32_t cursor_y(void) { return cur_y; }
