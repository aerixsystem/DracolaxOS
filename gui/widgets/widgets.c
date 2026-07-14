/* gui/widgets/widgets.c — Immediate-mode widget toolkit for DracolaxOS
 *
 * Built on top of the existing per-window drawing primitives in
 * gui/compositor/compositor.c (comp_window_fill, comp_window_print,
 * comp_window_set_pixel/get_pixel) plus the global mouse/keyboard drivers.
 * See widgets.h for the usage pattern and id conventions.
 *
 * Hot/active/focused tracking (the three persistent ids in widget_ctx_t):
 *   hot_id     — whichever widget the mouse is over THIS frame. Reset to 0
 *                at the start of widget_begin_frame(); each widget sets it
 *                if the (already window-relative) mouse position is inside
 *                its rect. Purely cosmetic (hover highlight).
 *   active_id  — whichever widget currently "owns" the mouse button. Set
 *                when mouse_pressed happens while hovering; cleared on
 *                mouse_released. Buttons use this for press-visual + the
 *                press+release-while-hovering click rule. Sliders and
 *                scrollbars use it to keep dragging even if the mouse
 *                strays outside the track rect mid-drag (normal slider
 *                feel — without this, fast drags would "drop" the thumb).
 *   focused_id — whichever textbox currently owns keyboard input. Set when
 *                a textbox is clicked. Cleared when Enter/Esc is pressed
 *                in it, or when a DIFFERENT widget becomes active this
 *                frame (clicking a button/checkbox/slider/another textbox
 *                defocuses the previous one). Clicking truly empty space
 *                does NOT defocus — documented limitation, acceptable for
 *                the simple single-window forms this toolkit targets.
 */
#include "widgets.h"
#include "../compositor/compositor.h"
#include "../../kernel/drivers/ps2/keyboard.h"
#include "../../kernel/drivers/ps2/mouse.h"
#include "../../kernel/drivers/vga/fb.h"
#include "../../kernel/klibc.h"

/* ───────────────────────────── context ──────────────────────────────── */

void widget_ctx_init(widget_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

void widget_ctx_init_raw(widget_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->win = -1;   /* sentinel: draw directly to the framebuffer */
}

/* ───────────────────────────── cursor shape ─────────────────────────── */

static int g_wants_text_cursor = 0;

int widget_wants_text_cursor(void) { return g_wants_text_cursor; }

void widget_begin_frame(widget_ctx_t *ctx, int win) {
    ctx->win = win;
    ctx->frame++;

    int ox = 0, oy = 0;
    /* BUG FIX (hit-testing offset / "must hover above the widget to
     * interact"): comp_get_pos() returns the window's OUTER top-left
     * (including the title bar), but comp_window_fill/print/set_pixel —
     * and therefore every widget's drawn position — are in CONTENT-space
     * coordinates, whose screen origin is WIN_TITLE_H pixels further down.
     * Using comp_get_pos() here under-subtracted by the title bar height,
     * so ctx->my came out too large by that amount: to land a click on a
     * widget drawn at content-y=Y, the mouse actually had to be at
     * screen-y = win.y + Y (i.e. ABOVE where the widget visually appears,
     * by one title-bar's worth) for the hit-test math to land on Y.
     * comp_get_content_pos() returns the correct content-space origin. */
    comp_get_content_pos(win, &ox, &oy);
    ctx->mx = mouse_get_x() - ox;
    ctx->my = mouse_get_y() - oy;

    int held = mouse_btn_held(MOUSE_BTN_LEFT);
    /* BUG FIX (window chrome / multi-window click routing unreliable):
     * mouse_btn_pressed()/released() (kernel/drivers/ps2/mouse.c) are
     * edge-detected against a single SHARED global "previous button
     * state", updated only when mouse_update_edges() is called — which
     * desktop_task does once per its own frame. Every app window
     * (independently scheduled, on its own cadence) ALSO called these
     * same shared-state edge functions directly, with no coordination:
     * depending on scheduling interleaving relative to
     * desktop_task's next mouse_update_edges() call, a single physical
     * click could be seen as a fresh press by multiple independent
     * readers at inconsistent times, or missed by one of them entirely.
     * Since mouse_btn_held() is simple, level-triggered, NOT
     * shared/consumed state, each widget_ctx_t now computes its own
     * press/release edge locally by diffing held() against what IT saw
     * last frame — completely independent of any other consumer,
     * including desktop_task and every other open window. */
    ctx->mouse_pressed  = held && !ctx->prev_mouse_down;
    ctx->mouse_released = !held && ctx->prev_mouse_down;
    ctx->mouse_down      = held;
    ctx->prev_mouse_down  = held;

    int c = keyboard_getchar();
    if (c >= 0x80) { ctx->key_code = c; ctx->key_char = 0; }
    else            { ctx->key_char = c; ctx->key_code = 0; }

    /* hot_id is recomputed fresh every frame by whichever widget the
     * mouse is currently over — reset here so a widget that's no longer
     * hovered (e.g. the window was scrolled, or the mouse moved away)
     * doesn't stay "hot" forever. */
    ctx->hot_id = 0;

    /* Reset here so a textbox that's no longer hovered (mouse moved away,
     * or this window lost the mouse to another one) doesn't leave the
     * OS cursor stuck as a text-beam forever. widget_textbox() below
     * sets it back to 1 if the mouse is still over one this frame. */
    g_wants_text_cursor = 0;

    /* If the mouse was pressed this frame and lands on nothing (every
     * widget call below will set active_id if it's hit), we still want
     * a still-focused textbox to lose focus on outside clicks where
     * possible. We approximate this by clearing focus pre-emptively on
     * any press; widget_textbox() re-claims it immediately if the press
     * was actually on that textbox. This makes "click elsewhere" reliably
     * defocus text input, not just "click another widget". */
    if (ctx->mouse_pressed) ctx->focused_id = 0;

    ctx->want_close = 0;
    ctx->pending_count = 0;
}

int  widget_should_close(widget_ctx_t *ctx)   { return ctx->want_close; }
void widget_request_close(widget_ctx_t *ctx)  { ctx->want_close = 1; }

/* ---- internal helpers ----------------------------------------------- */

static int hit(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

/* Raw/window-aware drawing primitives. ctx->win == -1 (set by
 * widget_ctx_init_raw()) means "draw directly to the screen framebuffer at
 * absolute coordinates" — used for desktop chrome like the clock widget,
 * which has no compositor window of its own. Every drawing function below
 * goes through these instead of calling comp_window_* directly, so the
 * same widget_label/widget_rect/etc. calls work in both modes. */
static void w_fill(widget_ctx_t *ctx, int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    if (ctx->win < 0) fb_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, color);
    else               comp_window_fill(ctx->win, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, color);
}

static void w_pixel(widget_ctx_t *ctx, int x, int y, uint32_t color) {
    if (ctx->win < 0) fb_put_pixel((uint32_t)x, (uint32_t)y, color);
    else               comp_window_set_pixel(ctx->win, x, y, color);
}

static void w_text(widget_ctx_t *ctx, int x, int y, const char *s, uint32_t color) {
    if (ctx->win < 0) fb_print((uint32_t)x, (uint32_t)y, s, color, 0x00000000u); /* transparent bg */
    else               comp_window_print(ctx->win, (uint32_t)x, (uint32_t)y, s, color);
}

/* ───────────────────────────── label ────────────────────────────────── */

void widget_label(widget_ctx_t *ctx, int x, int y, const char *text, uint32_t color) {
    w_text(ctx, x, y, text, color);
}

/* ───────────────────────────── button ───────────────────────────────── */

int widget_button(widget_ctx_t *ctx, uint32_t id, int x, int y, int w, int h,
                  const char *label) {
    int hovered = hit(ctx->mx, ctx->my, x, y, w, h);
    if (hovered) ctx->hot_id = id;

    if (ctx->mouse_pressed && hovered) ctx->active_id = id;

    int clicked = 0;
    if (ctx->mouse_released) {
        if (ctx->active_id == id && hovered) clicked = 1;
        if (ctx->active_id == id) ctx->active_id = 0;
    }

    uint32_t bg = WIDGET_COL_PANEL;
    if (ctx->active_id == id && ctx->mouse_down) bg = WIDGET_COL_PRESS;
    else if (hovered)                              bg = WIDGET_COL_HOVER;

    comp_window_fill(ctx->win, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, bg);
    widget_rect_border(ctx, x, y, w, h, WIDGET_COL_BORDER, 1);

    int tw = (int)strlen(label) * WIDGET_FONT_W;
    int tx = x + (w - tw) / 2; if (tx < x + 2) tx = x + 2;
    int ty = y + (h - WIDGET_FONT_H) / 2; if (ty < y) ty = y;
    comp_window_print(ctx->win, (uint32_t)tx, (uint32_t)ty, label, WIDGET_COL_TEXT);

    return clicked;
}

/* ───────────────────────────── checkbox ─────────────────────────────── */

#define CHK_BOX 16

int widget_checkbox(widget_ctx_t *ctx, uint32_t id, int x, int y,
                    int *checked, const char *label) {
    int hovered = hit(ctx->mx, ctx->my, x, y, CHK_BOX, CHK_BOX);
    if (hovered) ctx->hot_id = id;

    if (ctx->mouse_pressed && hovered) ctx->active_id = id;
    int changed = 0;
    if (ctx->mouse_released) {
        if (ctx->active_id == id && hovered) { *checked = !*checked; changed = 1; }
        if (ctx->active_id == id) ctx->active_id = 0;
    }

    uint32_t box_bg = hovered ? WIDGET_COL_HOVER : WIDGET_COL_PANEL;
    comp_window_fill(ctx->win, (uint32_t)x, (uint32_t)y, CHK_BOX, CHK_BOX, box_bg);
    widget_rect_border(ctx, x, y, CHK_BOX, CHK_BOX, WIDGET_COL_BORDER, 1);
    if (*checked) {
        comp_window_fill(ctx->win, (uint32_t)x + 3, (uint32_t)y + 3,
                         CHK_BOX - 6, CHK_BOX - 6, WIDGET_COL_ACCENT);
    }
    if (label && label[0])
        comp_window_print(ctx->win, (uint32_t)(x + CHK_BOX + 8),
                          (uint32_t)(y + (CHK_BOX - WIDGET_FONT_H) / 2),
                          label, WIDGET_COL_TEXT);
    return changed;
}

/* ───────────────────────────── slider ───────────────────────────────── */

#define SLIDER_H     16
#define SLIDER_THUMB 10

int widget_slider(widget_ctx_t *ctx, uint32_t id, int x, int y, int w,
                  float *value, float min, float max) {
    if (max <= min) max = min + 1.0f;
    if (*value < min) *value = min;
    if (*value > max) *value = max;

    int track_y = y + (SLIDER_H - 4) / 2;
    int hit_w = w, hit_h = SLIDER_H;
    int hovered = hit(ctx->mx, ctx->my, x, y, hit_w, hit_h);
    if (hovered) ctx->hot_id = id;

    if (ctx->mouse_pressed && hovered) ctx->active_id = id;

    int changed = 0;
    if (ctx->active_id == id) {
        if (ctx->mouse_down) {
            int rel = ctx->mx - x;
            if (rel < 0) rel = 0;
            if (rel > w) rel = w;
            float nv = min + (max - min) * ((float)rel / (float)w);
            if (nv != *value) { *value = nv; changed = 1; }
        }
        if (ctx->mouse_released) ctx->active_id = 0;
    }

    /* Track */
    comp_window_fill(ctx->win, (uint32_t)x, (uint32_t)track_y, (uint32_t)w, 4, WIDGET_COL_PANEL);
    widget_rect_border(ctx, x, track_y, w, 4, WIDGET_COL_BORDER, 1);

    /* Filled portion up to the thumb */
    float frac = (*value - min) / (max - min);
    int fill_w = (int)(frac * (float)w);
    if (fill_w > 0)
        comp_window_fill(ctx->win, (uint32_t)x, (uint32_t)track_y, (uint32_t)fill_w, 4, WIDGET_COL_ACCENT);

    /* Thumb */
    int thumb_x = x + fill_w - SLIDER_THUMB / 2;
    if (thumb_x < x) thumb_x = x;
    if (thumb_x > x + w - SLIDER_THUMB) thumb_x = x + w - SLIDER_THUMB;
    uint32_t thumb_col = (ctx->active_id == id) ? WIDGET_COL_ACCENT_HI : WIDGET_COL_TEXT;
    comp_window_fill(ctx->win, (uint32_t)thumb_x, (uint32_t)y, SLIDER_THUMB, SLIDER_H, thumb_col);
    widget_rect_border(ctx, thumb_x, y, SLIDER_THUMB, SLIDER_H, WIDGET_COL_BORDER, 1);

    return changed;
}

/* ───────────────────────────── progress bar ─────────────────────────── */

void widget_progress_bar(widget_ctx_t *ctx, int x, int y, int w, int h, float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    comp_window_fill(ctx->win, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, WIDGET_COL_PANEL);
    int fw = (int)((float)w * value);
    if (fw > 0) comp_window_fill(ctx->win, (uint32_t)x, (uint32_t)y, (uint32_t)fw, (uint32_t)h, WIDGET_COL_ACCENT);
    widget_rect_border(ctx, x, y, w, h, WIDGET_COL_BORDER, 1);
}

/* ───────────────────────────── textbox ──────────────────────────────── */

#define TEXTBOX_H 24

/* ───────────────────────────── clipboard ─────────────────────────────── */
/* Process-wide (not per-window) so Ctrl+C in one textbox/window and
 * Ctrl+V in another both work, like a normal OS clipboard. See
 * widgets.h's widget_clipboard_get()/set() doc. */
#define WIDGET_CLIPBOARD_MAX 256
static char widget_clipboard[WIDGET_CLIPBOARD_MAX] = "";

const char *widget_clipboard_get(void) { return widget_clipboard; }

void widget_clipboard_set(const char *text) {
    if (!text) { widget_clipboard[0] = '\0'; return; }
    strncpy(widget_clipboard, text, WIDGET_CLIPBOARD_MAX - 1);
    widget_clipboard[WIDGET_CLIPBOARD_MAX - 1] = '\0';
}

/* Delete buf[a..b) (a<=b, both clamped by caller) and place the caret at
 * a. Shared by Backspace/Delete-with-selection, typing-over-a-selection,
 * and Ctrl+X. Returns the new length. */
static int textbox_delete_range(char *buf, int len, int a, int b) {
    if (a < 0) a = 0;
    if (b > len) b = len;
    if (b <= a) return len;
    memmove(buf + a, buf + b, (size_t)(len - b + 1)); /* +1 for NUL */
    return len - (b - a);
}

void widget_textbox_state_init(widget_textbox_state_t *st) {
    st->cursor = 0;
    st->scroll = 0;
    st->sel_anchor = -1;
}

int widget_textbox(widget_ctx_t *ctx, uint32_t id, int x, int y, int w,
                   char *buf, int buf_sz, widget_textbox_state_t *st) {
    int len = (int)strlen(buf);
    if (st->cursor > len) st->cursor = len;
    if (st->cursor < 0)   st->cursor = 0;
    if (st->sel_anchor > len) st->sel_anchor = len;

    int hovered = hit(ctx->mx, ctx->my, x, y, w, TEXTBOX_H);
    if (hovered) { ctx->hot_id = id; g_wants_text_cursor = 1; }

    if (ctx->mouse_pressed && hovered) {
        ctx->active_id = id;
        ctx->focused_id = id;   /* re-claim focus pre-cleared in begin_frame */
        st->cursor = len;        /* simplest: click anywhere -> caret to end */
        st->sel_anchor = -1;     /* a plain click always clears any selection */
    }
    if (ctx->mouse_released && ctx->active_id == id) ctx->active_id = 0;

    int submitted = 0;
    if (ctx->focused_id == id) {
        /* sel_lo/sel_hi: normalised [lo,hi) selection range, or both -1 if
         * there's no active selection (sel_anchor == -1 or collapsed onto
         * the cursor). Computed once up front since several branches below
         * need it. */
        int has_sel = (st->sel_anchor >= 0 && st->sel_anchor != st->cursor);
        int sel_lo = has_sel ? (st->sel_anchor < st->cursor ? st->sel_anchor : st->cursor) : -1;
        int sel_hi = has_sel ? (st->sel_anchor < st->cursor ? st->cursor : st->sel_anchor) : -1;

        int ctrl  = keyboard_ctrl();
        int shift = keyboard_shift();

        /* BUG FIX (missing clipboard/selection shortcuts): keyboard_ctrl()
         * turns Ctrl+letter into a control code 1-26 at the driver level
         * (see kernel/drivers/ps2/keyboard.c) — Ctrl+A=1, Ctrl+C=3,
         * Ctrl+V=22, Ctrl+X=24 — so these are checked against key_char,
         * not as a separate "ctrl is held" + plain letter combination.
         * Checked BEFORE the plain-character branch further down so they
         * can't accidentally be typed as literal control bytes. */
        if (ctrl && ctx->key_char == 1) {                 /* Ctrl+A: select all */
            st->sel_anchor = 0;
            st->cursor = len;
        } else if (ctrl && ctx->key_char == 3) {           /* Ctrl+C: copy */
            if (has_sel) {
                char tmp[WIDGET_CLIPBOARD_MAX];
                int n = sel_hi - sel_lo;
                if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
                memcpy(tmp, buf + sel_lo, (size_t)n);
                tmp[n] = '\0';
                widget_clipboard_set(tmp);
            } else {
                widget_clipboard_set(buf); /* nothing selected: copy whole field */
            }
        } else if (ctrl && ctx->key_char == 24) {          /* Ctrl+X: cut */
            if (has_sel) {
                char tmp[WIDGET_CLIPBOARD_MAX];
                int n = sel_hi - sel_lo;
                if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
                memcpy(tmp, buf + sel_lo, (size_t)n);
                tmp[n] = '\0';
                widget_clipboard_set(tmp);
                len = textbox_delete_range(buf, len, sel_lo, sel_hi);
                st->cursor = sel_lo;
                st->sel_anchor = -1;
            } else {
                widget_clipboard_set(buf);
                buf[0] = '\0';
                len = 0;
                st->cursor = 0;
            }
        } else if (ctrl && ctx->key_char == 22) {          /* Ctrl+V: paste */
            if (has_sel) { len = textbox_delete_range(buf, len, sel_lo, sel_hi); st->cursor = sel_lo; st->sel_anchor = -1; }
            const char *clip = widget_clipboard_get();
            int clip_len = (int)strlen(clip);
            int room = buf_sz - 1 - len;
            if (room < 0) room = 0;
            if (clip_len > room) clip_len = room;
            if (clip_len > 0) {
                memmove(buf + st->cursor + clip_len, buf + st->cursor, (size_t)(len - st->cursor + 1));
                memcpy(buf + st->cursor, clip, (size_t)clip_len);
                st->cursor += clip_len;
                len += clip_len;
            }
        } else if (ctx->key_char == '\n') {
            submitted = 1;
        } else if (ctx->key_char == 0x1B) {
            ctx->focused_id = 0;
        } else if (ctx->key_char == '\b' || ctx->key_char == 0x08) {
            if (has_sel) {
                len = textbox_delete_range(buf, len, sel_lo, sel_hi);
                st->cursor = sel_lo;
                st->sel_anchor = -1;
            } else if (st->cursor > 0) {
                len = textbox_delete_range(buf, len, st->cursor - 1, st->cursor);
                st->cursor--;
            }
        } else if (ctx->key_code == KB_KEY_DEL) {
            if (has_sel) {
                len = textbox_delete_range(buf, len, sel_lo, sel_hi);
                st->cursor = sel_lo;
                st->sel_anchor = -1;
            } else if (st->cursor < len) {
                len = textbox_delete_range(buf, len, st->cursor, st->cursor + 1);
            }
        } else if (ctx->key_code == KB_KEY_LEFT) {
            if (shift) {
                if (st->sel_anchor < 0) st->sel_anchor = st->cursor;
                if (st->cursor > 0) st->cursor--;
            } else if (has_sel) {
                st->cursor = sel_lo;   /* collapse to selection start */
                st->sel_anchor = -1;
            } else if (st->cursor > 0) {
                st->cursor--;
            }
        } else if (ctx->key_code == KB_KEY_RIGHT) {
            if (shift) {
                if (st->sel_anchor < 0) st->sel_anchor = st->cursor;
                if (st->cursor < len) st->cursor++;
            } else if (has_sel) {
                st->cursor = sel_hi;   /* collapse to selection end */
                st->sel_anchor = -1;
            } else if (st->cursor < len) {
                st->cursor++;
            }
        } else if (ctx->key_code == KB_KEY_HOME) {
            if (shift) { if (st->sel_anchor < 0) st->sel_anchor = st->cursor; }
            else         st->sel_anchor = -1;
            st->cursor = 0;
        } else if (ctx->key_code == KB_KEY_END) {
            if (shift) { if (st->sel_anchor < 0) st->sel_anchor = st->cursor; }
            else         st->sel_anchor = -1;
            st->cursor = len;
        } else if (ctx->key_char >= 0x20 && ctx->key_char < 0x7F) {
            if (has_sel) {
                len = textbox_delete_range(buf, len, sel_lo, sel_hi);
                st->cursor = sel_lo;
                st->sel_anchor = -1;
            }
            if (len + 1 < buf_sz) {
                memmove(buf + st->cursor + 1, buf + st->cursor, (size_t)(len - st->cursor + 1));
                buf[st->cursor] = (char)ctx->key_char;
                st->cursor++;
                len++;
            }
        }
        buf[len] = '\0';
    }

    /* Horizontal scroll so the caret is always visible */
    int visible_chars = (w - 12) / WIDGET_FONT_W;
    if (visible_chars < 1) visible_chars = 1;
    if (st->cursor < st->scroll) st->scroll = st->cursor;
    if (st->cursor > st->scroll + visible_chars) st->scroll = st->cursor - visible_chars;
    if (st->scroll < 0) st->scroll = 0;

    uint32_t bg = (ctx->focused_id == id) ? WIDGET_COL_PANEL : WIDGET_COL_PANEL;
    comp_window_fill(ctx->win, (uint32_t)x, (uint32_t)y, (uint32_t)w, TEXTBOX_H, bg);
    uint32_t border = (ctx->focused_id == id) ? WIDGET_COL_ACCENT : WIDGET_COL_BORDER;
    widget_rect_border(ctx, x, y, w, TEXTBOX_H, border, ctx->focused_id == id ? 2 : 1);

    /* Selection highlight, drawn before the text so glyphs render on top
     * of it (comp_window_print fills its own glyph background, so the
     * highlight only needs to cover the parts of a selected glyph cell
     * that aren't already painted by the glyph itself — drawing it first
     * and letting the glyph's own opaque background paint over it where
     * they overlap gives correct results without needing a separate
     * "highlighted text colour" pass). */
    if (st->sel_anchor >= 0 && st->sel_anchor != st->cursor) {
        int sel_lo = st->sel_anchor < st->cursor ? st->sel_anchor : st->cursor;
        int sel_hi = st->sel_anchor < st->cursor ? st->cursor : st->sel_anchor;
        int vis_lo = sel_lo - st->scroll; if (vis_lo < 0) vis_lo = 0;
        int vis_hi = sel_hi - st->scroll; if (vis_hi > visible_chars) vis_hi = visible_chars;
        if (vis_hi > vis_lo) {
            int hl_x = x + 6 + vis_lo * WIDGET_FONT_W;
            int hl_w = (vis_hi - vis_lo) * WIDGET_FONT_W;
            comp_window_fill(ctx->win, (uint32_t)hl_x, (uint32_t)(y + 4), (uint32_t)hl_w, TEXTBOX_H - 8, WIDGET_COL_ACCENT);
        }
    }

    char visible[256];
    int vlen = len - st->scroll;
    if (vlen < 0) vlen = 0;
    if (vlen > visible_chars) vlen = visible_chars;
    if (vlen > (int)sizeof(visible) - 1) vlen = (int)sizeof(visible) - 1;
    memcpy(visible, buf + st->scroll, (size_t)vlen);
    visible[vlen] = '\0';
    comp_window_print(ctx->win, (uint32_t)(x + 6), (uint32_t)(y + (TEXTBOX_H - WIDGET_FONT_H) / 2),
                      visible, WIDGET_COL_TEXT);

    /* Blinking caret, only while focused and nothing is selected (a
     * selection highlight already shows where things stand — a blinking
     * caret on top of/next to it is just visual noise, same convention
     * most text editors follow). */
    if (ctx->focused_id == id && !(st->sel_anchor >= 0 && st->sel_anchor != st->cursor) &&
        ((ctx->frame >> 4) & 1)) {
        int caret_col = st->cursor - st->scroll;
        int caret_x = x + 6 + caret_col * WIDGET_FONT_W;
        comp_window_fill(ctx->win, (uint32_t)caret_x, (uint32_t)(y + 4), 1, TEXTBOX_H - 8, WIDGET_COL_TEXT);
    }

    return submitted;
}

/* ───────────────────────────── scrollbar (vertical) ─────────────────── */

#define SCROLLBAR_W 14

int widget_scrollbar_v(widget_ctx_t *ctx, uint32_t id, int x, int y, int h,
                       int *scroll, int content_h, int view_h) {
    int max_scroll = content_h - view_h;
    if (max_scroll < 0) max_scroll = 0;
    if (*scroll > max_scroll) *scroll = max_scroll;
    if (*scroll < 0) *scroll = 0;

    comp_window_fill(ctx->win, (uint32_t)x, (uint32_t)y, SCROLLBAR_W, (uint32_t)h, WIDGET_COL_PANEL);
    widget_rect_border(ctx, x, y, SCROLLBAR_W, h, WIDGET_COL_BORDER, 1);

    if (content_h <= view_h || content_h <= 0) {
        /* Nothing to scroll — draw an inert full-height track, no thumb. */
        return 0;
    }

    int thumb_h = (int)((float)h * (float)view_h / (float)content_h);
    if (thumb_h < 16) thumb_h = 16;
    if (thumb_h > h) thumb_h = h;
    int track_range = h - thumb_h;
    int thumb_y = y + (track_range > 0 ? (int)((float)track_range * (float)*scroll / (float)max_scroll) : 0);

    int hovered = hit(ctx->mx, ctx->my, x, y, SCROLLBAR_W, h);
    if (hovered) ctx->hot_id = id;
    if (ctx->mouse_pressed && hovered) ctx->active_id = id;

    int changed = 0;
    if (ctx->active_id == id) {
        if (ctx->mouse_down) {
            int rel = ctx->my - y - thumb_h / 2;
            if (rel < 0) rel = 0;
            if (rel > track_range) rel = track_range;
            int ns = (track_range > 0) ? (int)((float)max_scroll * (float)rel / (float)track_range) : 0;
            if (ns != *scroll) { *scroll = ns; changed = 1; }
        }
        if (ctx->mouse_released) ctx->active_id = 0;
    }

    uint32_t thumb_col = (ctx->active_id == id) ? WIDGET_COL_ACCENT_HI :
                          (hovered ? WIDGET_COL_ACCENT : WIDGET_COL_TEXT_DIM);
    comp_window_fill(ctx->win, (uint32_t)(x + 2), (uint32_t)thumb_y, SCROLLBAR_W - 4, (uint32_t)thumb_h, thumb_col);

    return changed;
}

/* ───────────────────────────── dropdown ─────────────────────────────── */

#define DROPDOWN_ROW_H 24

void widget_dropdown_state_init(widget_dropdown_state_t *st) {
    st->open = 0;
}

int widget_dropdown(widget_ctx_t *ctx, uint32_t id, int x, int y, int w,
                    const char **options, int n_options, int *selected,
                    widget_dropdown_state_t *st) {
    if (*selected < 0) *selected = 0;
    if (*selected >= n_options) *selected = n_options - 1;

    int head_hovered = hit(ctx->mx, ctx->my, x, y, w, DROPDOWN_ROW_H);
    if (head_hovered) ctx->hot_id = id;

    int changed = 0;

    if (ctx->mouse_pressed) {
        if (head_hovered) {
            st->open = !st->open;
        } else if (st->open) {
            /* Click somewhere else: select if it landed on an open row,
             * otherwise just close. */
            int hit_row = -1;
            for (int i = 0; i < n_options; i++) {
                int ry = y + DROPDOWN_ROW_H * (i + 1);
                if (hit(ctx->mx, ctx->my, x, ry, w, DROPDOWN_ROW_H)) { hit_row = i; break; }
            }
            if (hit_row >= 0 && hit_row != *selected) { *selected = hit_row; changed = 1; }
            st->open = 0;
        }
    }

    /* Head */
    comp_window_fill(ctx->win, (uint32_t)x, (uint32_t)y, (uint32_t)w, DROPDOWN_ROW_H,
                     head_hovered ? WIDGET_COL_HOVER : WIDGET_COL_PANEL);
    widget_rect_border(ctx, x, y, w, DROPDOWN_ROW_H, WIDGET_COL_BORDER, 1);
    comp_window_print(ctx->win, (uint32_t)(x + 6), (uint32_t)(y + (DROPDOWN_ROW_H - WIDGET_FONT_H) / 2),
                      options[*selected], WIDGET_COL_TEXT);
    comp_window_print(ctx->win, (uint32_t)(x + w - 16), (uint32_t)(y + (DROPDOWN_ROW_H - WIDGET_FONT_H) / 2),
                      st->open ? "^" : "v", WIDGET_COL_TEXT_DIM);

    /* BUG FIX (z-order): the open option list used to be drawn right here,
     * immediately at this call site. Any widget drawn LATER in the same
     * frame that happened to overlap this screen area (e.g. a colour
     * picker positioned below/beside the dropdown) would paint over part
     * of the open list, since draw order = call order with no z-index.
     * Instead, queue the open list as a pending overlay; widget_end_frame()
     * draws all pending overlays last, after every other widget_* call
     * this frame, so the open list always wins regardless of layout. */
    if (st->open && ctx->pending_count < WIDGET_MAX_PENDING_OVERLAYS) {
        widget_pending_overlay_t *p = &ctx->pending[ctx->pending_count++];
        p->x = x; p->y = y; p->w = w;
        p->options = options;
        p->n_options = n_options;
        p->selected = *selected;
    }

    return changed;
}

void widget_end_frame(widget_ctx_t *ctx) {
    for (int p = 0; p < ctx->pending_count; p++) {
        widget_pending_overlay_t *ov = &ctx->pending[p];
        for (int i = 0; i < ov->n_options; i++) {
            int ry = ov->y + DROPDOWN_ROW_H * (i + 1);
            int row_hovered = hit(ctx->mx, ctx->my, ov->x, ry, ov->w, DROPDOWN_ROW_H);
            uint32_t row_bg = (i == ov->selected) ? WIDGET_COL_PRESS :
                               (row_hovered ? WIDGET_COL_HOVER : WIDGET_COL_PANEL);
            comp_window_fill(ctx->win, (uint32_t)ov->x, (uint32_t)ry, (uint32_t)ov->w, DROPDOWN_ROW_H, row_bg);
            widget_rect_border(ctx, ov->x, ry, ov->w, DROPDOWN_ROW_H, WIDGET_COL_BORDER, 1);
            comp_window_print(ctx->win, (uint32_t)(ov->x + 6), (uint32_t)(ry + (DROPDOWN_ROW_H - WIDGET_FONT_H) / 2),
                              ov->options[i], WIDGET_COL_TEXT);
        }
    }
    ctx->pending_count = 0;
}

/* ───────────────────────────── color picker ─────────────────────────── */

static const uint32_t WIDGET_PALETTE[16] = {
    0xFFFFFFu, 0xC0C0C0u, 0x808080u, 0x404040u, 0x000000u, 0x7828C8u, 0x9450E0u, 0x4A1E80u,
    0xD03050u, 0xE08030u, 0xE0C030u, 0x30C070u, 0x30A0E0u, 0x3050D0u, 0xA050D0u, 0x60483Cu,
};

int widget_color_picker(widget_ctx_t *ctx, uint32_t id, int x, int y,
                        int swatch_size, uint32_t *color) {
    int changed = 0;
    int gap = 2;
    for (int i = 0; i < 16; i++) {
        int col = i % 8, row = i / 8;
        int sx = x + col * (swatch_size + gap);
        int sy = y + row * (swatch_size + gap);
        int hovered = hit(ctx->mx, ctx->my, sx, sy, swatch_size, swatch_size);
        if (hovered) ctx->hot_id = id;

        if (ctx->mouse_pressed && hovered) {
            if (*color != WIDGET_PALETTE[i]) { *color = WIDGET_PALETTE[i]; changed = 1; }
        }

        comp_window_fill(ctx->win, (uint32_t)sx, (uint32_t)sy, (uint32_t)swatch_size, (uint32_t)swatch_size, WIDGET_PALETTE[i]);
        int selected = (*color == WIDGET_PALETTE[i]);
        widget_rect_border(ctx, sx, sy, swatch_size, swatch_size,
                           selected ? WIDGET_COL_ACCENT_HI : WIDGET_COL_BORDER,
                           selected ? 2 : 1);
    }
    return changed;
}

/* ───────────────────────────── clipping ─────────────────────────────── */

void widget_begin_clip(widget_ctx_t *ctx, int x, int y, int w, int h) {
    if (ctx->win < 0) return;   /* not supported in raw mode, see widgets.h */
    comp_window_set_clip(ctx->win, x, y, w, h);
}

void widget_end_clip(widget_ctx_t *ctx) {
    if (ctx->win < 0) return;
    comp_window_clear_clip(ctx->win);
}

/* ───────────────────────────── shapes ───────────────────────────────── */

void widget_rect(widget_ctx_t *ctx, int x, int y, int w, int h, uint32_t color, int filled) {
    if (filled) {
        w_fill(ctx, x, y, w, h, color);
    } else {
        widget_rect_border(ctx, x, y, w, h, color, 1);
    }
}

void widget_rect_border(widget_ctx_t *ctx, int x, int y, int w, int h, uint32_t color, int thickness) {
    if (thickness < 1) thickness = 1;
    if (w <= 0 || h <= 0) return;
    w_fill(ctx, x, y, w, thickness, color);                          /* top    */
    w_fill(ctx, x, y + h - thickness, w, thickness, color);           /* bottom */
    w_fill(ctx, x, y, thickness, h, color);                           /* left   */
    w_fill(ctx, x + w - thickness, y, thickness, h, color);           /* right  */
}

/* Bresenham line */
void widget_line(widget_ctx_t *ctx, int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x1 >= x0) ? 1 : -1;
    int sy = (y1 >= y0) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        w_pixel(ctx, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Midpoint circle algorithm */
void widget_circle(widget_ctx_t *ctx, int cx, int cy, int r, uint32_t color, int filled) {
    if (r <= 0) { w_pixel(ctx, cx, cy, color); return; }
    int x = r, y = 0;
    int err = 0;

    while (x >= y) {
        if (filled) {
            widget_line(ctx, cx - x, cy + y, cx + x, cy + y, color);
            widget_line(ctx, cx - x, cy - y, cx + x, cy - y, color);
            widget_line(ctx, cx - y, cy + x, cx + y, cy + x, color);
            widget_line(ctx, cx - y, cy - x, cx + y, cy - x, color);
        } else {
            w_pixel(ctx, cx + x, cy + y, color);
            w_pixel(ctx, cx + y, cy + x, color);
            w_pixel(ctx, cx - y, cy + x, color);
            w_pixel(ctx, cx - x, cy + y, color);
            w_pixel(ctx, cx - x, cy - y, color);
            w_pixel(ctx, cx - y, cy - x, color);
            w_pixel(ctx, cx + y, cy - x, color);
            w_pixel(ctx, cx + x, cy - y, color);
        }
        y++;
        err += 1 + 2 * y;
        if (2 * (err - x) + 1 > 0) { x--; err += 1 - 2 * x; }
    }
}
