/* gui/widgets/widgets.h — Immediate-mode widget toolkit for DracolaxOS
 *
 * Dear-ImGui-style API: no widget tree, no retained state for layout.
 * Every widget call both DRAWS the widget for this frame AND returns
 * whatever interaction happened this frame (clicked, value changed, etc).
 * Call the same function with the same id every frame from your app's
 * main loop; the widget toolkit handles hover/press/focus tracking for
 * you via the persistent widget_ctx_t your app owns.
 *
 * ─────────────────────────────────────────────────────────────────────
 * USAGE PATTERN — DO NOT USE `static` FOR PER-INSTANCE STATE
 * ─────────────────────────────────────────────────────────────────────
 * DracolaxOS is a single-address-space kernel: every task shares one
 * global/static memory space. A `static` local inside an app's function
 * is ONE memory location shared by EVERY task that ever calls that
 * function — not "one per running instance." Opening the same app twice
 * spawns two separate tasks that both call the same function, and if its
 * state (widget_ctx_t, textbox/dropdown state, counters, buffers — the
 * window handle included) is `static`, both windows end up reading and
 * writing the exact same memory: clicking a button in one window changes
 * what the other displays, typing in one textbox appears in the other's,
 * and since even the window handle to draw into is shared, widget calls
 * from the two tasks fight over which window they actually paint into.
 * (An earlier version of this file recommended `static` here — that was
 * wrong and caused exactly this bug; see docs/CHANGELOG.md.)
 *
 * Declare all per-instance state as ordinary (non-static) locals in your
 * task's entry function instead. Each task has its own stack, so plain
 * locals are correctly isolated per running instance, and they persist
 * for the task's whole lifetime because the `while` loop keeps that
 * stack frame alive — exactly the "declare once, persists across every
 * frame" behaviour you want, without the sharing bug. A `static` local
 * is only safe for something you deliberately want shared across every
 * instance and every task (e.g. widget_clipboard's backing buffer,
 * further down — an actual OS-wide clipboard is supposed to be shared).
 *
 *   void my_app_task(void) {
 *       int win = comp_create_window("My App", x, y, w, h);
 *       widget_ctx_t wctx;              // plain local — one per task instance
 *       widget_ctx_init(&wctx);
 *       int counter = 0;                // also plain — per-instance state
 *       int running = 1;
 *       while (running) {
 *           comp_window_clear(win);
 *           widget_begin_frame(&wctx, win);   // refresh mouse/keyboard for this frame
 *
 *           widget_label(&wctx, 10, 10, "Hello!", WIDGET_COL_TEXT);
 *           if (widget_button(&wctx, WID(1), 10, 40, 100, 28, "Click me"))
 *               counter++;
 *
 *           widget_end_frame(&wctx);          // draw deferred overlays (open dropdowns) on top
 *           if (widget_should_close(&wctx)) running = 0;
 *
 *           sched_yield();   // draw into your OWN window only — do NOT call
 *       }                    // comp_render()/fb_flip() yourself; the desktop
 *                            // task is the sole compositor (see widgets.h's
 *                            // "RENDERING — DO NOT CALL comp_render() YOURSELF"
 *                            // note below for why)
 *       comp_destroy_window(win);
 *       sched_exit();
 *   }
 *
 * ─────────────────────────────────────────────────────────────────────
 * RENDERING — DO NOT CALL comp_render() / fb_flip() YOURSELF
 * ─────────────────────────────────────────────────────────────────────
 * The desktop task is the SOLE compositor: it calls comp_render() (which
 * composites every window's backbuffer, the wallpaper, icons, dock, and
 * cursor into the shared shadow framebuffer) and fb_flip() (which blits
 * that shadow buffer to VRAM) once per its own frame, in its own main
 * loop. Your app's job is only to draw into its OWN window's backbuffer
 * via comp_window_fill/print/set_pixel (which every widget_* call does
 * for you) and then yield. If your app ALSO calls comp_render()/fb_flip()
 * in its own loop, you get two uncoordinated tasks compositing and
 * flipping the same shared framebuffer at different, competing
 * frequencies — this causes visible flicker/tearing across the WHOLE
 * desktop (not just your window) and roughly doubles the per-frame
 * compositing cost (every window gets re-composited twice as often),
 * which is exactly the "entire desktop flickers and lags when this app is
 * open" symptom. Just call sched_yield() (or sched_sleep() for a lower
 * frame rate) at the end of your loop and let the desktop task do the
 * actual screen update.
 *
 * ─────────────────────────────────────────────────────────────────────
 * WIDGET IDS
 * ─────────────────────────────────────────────────────────────────────
 * Every interactive widget (button/checkbox/slider/textbox/scrollbar/
 * dropdown/color picker) takes a `uint32_t id`, used internally to track
 * which widget is hovered/pressed/focused across frames. IDs only need to
 * be unique WITHIN one widget_ctx_t (i.e. within one window). Two easy
 * conventions:
 *   - One-off widgets at fixed call sites: WID(__LINE__) — each call site
 *     is a different source line, so ids never collide.
 *   - Widgets generated in a loop (e.g. a list of N buttons): WID(base + i)
 *     with a distinct `base` per loop (e.g. 1000, 2000, ...) so different
 *     loops can't collide either.
 *
 * ─────────────────────────────────────────────────────────────────────
 * PER-WIDGET EXTRA STATE
 * ─────────────────────────────────────────────────────────────────────
 * Textboxes and dropdowns need a little persistent state of their own
 * (cursor position, scroll offset, open/closed) beyond what fits in the
 * shared hot/active/focused tracking. That state is NOT stored inside
 * widget_ctx_t (which is shared by every widget in the window) — instead
 * you declare one small state struct per textbox/dropdown instance, as
 * an ordinary (non-static) local right alongside widget_ctx_t in your
 * task's function — see "USAGE PATTERN" above for why `static` is wrong
 * here — and pass a pointer to it. This mirrors how the data the widget
 * edits (the text buffer, the int*, the float*) is also always owned by
 * the caller, not the toolkit.
 */
#ifndef WIDGETS_H
#define WIDGETS_H

#include "../../kernel/types.h"

/* ---- font metrics (matches the compositor's embedded 8x16 bitmap font) -- */
#define WIDGET_FONT_W   8
#define WIDGET_FONT_H  16

/* ---- default palette (override by passing explicit colours) ------------- */
#define WIDGET_COL_BG        0x14141Cu
#define WIDGET_COL_PANEL     0x1C1C28u
#define WIDGET_COL_BORDER    0x3A3F5Au
#define WIDGET_COL_TEXT      0xF0F0FFu
#define WIDGET_COL_TEXT_DIM  0x80809Au
#define WIDGET_COL_ACCENT    0x7828C8u
#define WIDGET_COL_ACCENT_HI 0x9450E0u
#define WIDGET_COL_HOVER     0x28283Cu
#define WIDGET_COL_PRESS     0x32325Au
#define WIDGET_COL_DANGER    0xD03050u
#define WIDGET_COL_OK        0x30C070u

/* Convenience id cast — see "WIDGET IDS" above. */
#define WID(n) ((uint32_t)(n))

/* ───────────────────────────── context ──────────────────────────────── */

#define WIDGET_MAX_PENDING_OVERLAYS 4

/* Internal — a queued "draw this on top of everything else this frame"
 * request. Currently only dropdown option lists use this; see
 * widget_dropdown() and widget_end_frame() in widgets.c. Not part of the
 * public API — don't construct these directly. */
typedef struct {
    int x, y, w;
    const char **options;
    int n_options;
    int selected;
} widget_pending_overlay_t;

typedef struct {
    int win;             /* compositor window handle this ctx drives     */

    /* Per-frame input snapshot — refreshed by widget_begin_frame()      */
    int mx, my;           /* mouse position, window-relative             */
    int mouse_down;        /* left button currently held                  */
    int mouse_pressed;     /* left button: pressed THIS frame (edge)      */
    int mouse_released;    /* left button: released THIS frame (edge)    */
    int key_char;           /* ASCII char typed this frame, 0 = none      */
    int key_code;            /* KB_KEY_* special key this frame, 0 = none */
    uint32_t frame;           /* incrementing frame counter (caret blink) */

    /* Persistent interaction state — DO NOT touch directly              */
    uint32_t hot_id;            /* widget currently moused-over           */
    uint32_t active_id;          /* widget currently pressed/dragging     */
    uint32_t focused_id;          /* widget currently owns keyboard input */
    int      prev_mouse_down;      /* mouse_down as of last frame — used to
                                    * compute press/release edges locally,
                                    * see widget_begin_frame() in widgets.c */

    /* Set by a widget that wants the host app to close the window       */
    int want_close;

    /* Internal — queued overlay draws for this frame, flushed by
     * widget_end_frame(). DO NOT touch directly. */
    widget_pending_overlay_t pending[WIDGET_MAX_PENDING_OVERLAYS];
    int pending_count;
} widget_ctx_t;

/* Zero-initialise a context. Equivalent to `= {0}` but kept as a function
 * so the layout can grow later without breaking callers. */
void widget_ctx_init(widget_ctx_t *ctx);

/* Zero-initialise a context for RAW mode: drawing directly onto the
 * screen framebuffer at absolute coordinates, instead of into a
 * compositor window's backbuffer. For desktop CHROME — things drawn
 * directly by the desktop task alongside the wallpaper/icons/dock, with
 * no comp_create_window() of their own (e.g. the top-right clock/user
 * widget) — not for regular apps, which should always use
 * widget_ctx_init() + a real window.
 *
 * Only the non-interactive drawing functions support raw mode:
 * widget_label, widget_rect, widget_rect_border, widget_line,
 * widget_circle. Interactive widgets (button, checkbox, slider, textbox,
 * scrollbar, dropdown, color_picker) all require widget_begin_frame()'s
 * window-relative mouse math, which has no meaning without a real
 * compositor window — don't use them on a raw ctx. There is no
 * widget_begin_frame() call for raw mode either; just call the drawing
 * functions directly with absolute screen x/y. */
void widget_ctx_init_raw(widget_ctx_t *ctx);

/* Call ONCE per frame, before any widget_* calls, right after you know
 * which window handle you're drawing into. Reads mouse/keyboard state via
 * mouse_get_x/y(), mouse_btn_held/pressed/released(), keyboard_getchar(),
 * and comp_get_pos() (to convert the absolute mouse position into
 * window-relative coordinates). */
void widget_begin_frame(widget_ctx_t *ctx, int win);

/* True if a widget this frame requested the window be closed (e.g. the
 * built-in title-bar-less close affordance some widgets expose, or your
 * own "Close" button calling widget_request_close()). Most apps will
 * instead just check their own Esc handling; this is provided for widgets
 * that want to signal close directly (e.g. a modal's [X]). */
int  widget_should_close(widget_ctx_t *ctx);
void widget_request_close(widget_ctx_t *ctx);

/* Call ONCE per frame, AFTER all other widget_* calls, before yielding.
 * Draws anything that was deferred to render on top of everything else
 * this frame — currently, open dropdown option lists (see
 * widget_dropdown()). Without this call, a dropdown's open list is drawn
 * immediately at its own call site, so any widget drawn LATER in the same
 * frame that happens to overlap its screen area will paint over part of
 * it — the "list renders on the wrong layer" z-order bug. Safe to call
 * even if nothing is pending (no-op). */
void widget_end_frame(widget_ctx_t *ctx);

/* ───────────────────────────── label ────────────────────────────────── */

void widget_label(widget_ctx_t *ctx, int x, int y, const char *text, uint32_t color);

/* ───────────────────────────── button ───────────────────────────────── */

/* Returns 1 on the frame the button is clicked (press+release while
 * still hovering), else 0. */
int widget_button(widget_ctx_t *ctx, uint32_t id, int x, int y, int w, int h,
                  const char *label);

/* ───────────────────────────── checkbox ─────────────────────────────── */

/* Toggles *checked in place. Returns 1 on the frame the value changed. */
int widget_checkbox(widget_ctx_t *ctx, uint32_t id, int x, int y,
                    int *checked, const char *label);

/* ───────────────────────────── slider ───────────────────────────────── */

/* Horizontal slider, track width `w`, fixed height. Click-to-jump and
 * drag-to-adjust both supported (drag continues even if the mouse leaves
 * the track vertically, standard slider feel). Returns 1 on the frame
 * *value changed. */
int widget_slider(widget_ctx_t *ctx, uint32_t id, int x, int y, int w,
                  float *value, float min, float max);

/* ───────────────────────────── progress bar ─────────────────────────── */

/* Pure display, no interaction. value clamped to [0,1]. */
void widget_progress_bar(widget_ctx_t *ctx, int x, int y, int w, int h,
                         float value);

/* ───────────────────────────── textbox ──────────────────────────────── */

typedef struct {
    int cursor;   /* character index of the caret within buf             */
    int scroll;   /* index of the first visible character (horiz scroll) */
    int sel_anchor; /* selection start index, or -1 = no selection. The
                     * selected range is always [min(cursor,sel_anchor),
                     * max(cursor,sel_anchor)) — cursor is the "live" end. */
} widget_textbox_state_t;

void widget_textbox_state_init(widget_textbox_state_t *st);

/* Single-line text input. buf/buf_sz is the caller-owned text buffer
 * (always kept NUL-terminated). st is per-textbox persistent state the
 * caller owns — declare it as an ordinary (non-static) local alongside
 * widget_ctx_t in your task's function, NOT `static` (see the
 * "USAGE PATTERN" note at the top of this file for why `static` here
 * causes every instance of your app to share one textbox's state).
 * Click to focus; typing edits the
 * buffer; Left/Right move the caret; Home/End jump; Backspace/Delete
 * erase. Returns 1 on the frame Enter was pressed while focused (the
 * conventional "submit" signal — caller decides what that means).
 *
 * Selection and clipboard: Shift+Left/Right/Home/End extends or shrinks a
 * selection from the caret (highlighted in WIDGET_COL_ACCENT). Ctrl+A
 * selects the whole field. Typing or Backspace/Delete while a selection
 * is active replaces/erases the selection instead of acting at the
 * caret. Ctrl+C copies the selection (or, if nothing is selected, the
 * entire field) to a SHARED, toolkit-wide clipboard (see
 * widget_clipboard_get()/widget_clipboard_set() — it's process-wide
 * within this kernel, not per-window, so copy/paste works across
 * different textboxes and different app windows, matching normal OS
 * clipboard behaviour). Ctrl+X cuts (copies, then deletes the
 * selection). Ctrl+V pastes the clipboard contents at the caret,
 * replacing the selection if any, truncated to fit buf_sz.
 *
 * Esc interaction: if a textbox is focused and Esc is pressed, this
 * function defocuses it (ctx->focused_id becomes 0) instead of letting
 * Esc propagate to "close the window" — the common DracolaxOS app
 * convention. If your app's own loop also checks for Esc-to-close, guard
 * it against this case: snapshot `ctx->focused_id != 0` BEFORE calling
 * any widget_* functions this frame, and only treat Esc as "close
 * window" if nothing was focused at the start of the frame. Otherwise a
 * single Esc press both defocuses the textbox AND closes the window
 * (since by the time you check focused_id after the widget calls ran,
 * it's already back to 0). See app_widget_demo() in apps/widget_demo/
 * for a worked example of this exact pattern. */
int widget_textbox(widget_ctx_t *ctx, uint32_t id, int x, int y, int w,
                   char *buf, int buf_sz, widget_textbox_state_t *st);

/* ───────────────────────────── clipboard ────────────────────────────── */

/* A single, process-wide text clipboard shared by every widget_textbox()
 * across every window — this is what makes Ctrl+C in one textbox and
 * Ctrl+V in another (even in a different app) work, matching normal OS
 * clipboard behaviour. Exposed publicly in case an app wants to
 * read/write the clipboard outside of a textbox (e.g. a "Copy" button
 * that copies something that isn't a textbox's contents). Returns "" /
 * does nothing safely if never set. */
const char *widget_clipboard_get(void);
void        widget_clipboard_set(const char *text);

/* ───────────────────────────── cursor shape ─────────────────────────── */

/* True if the mouse was hovering a textbox as of the LATEST
 * widget_begin_frame()/widget_textbox() call, in any window, regardless
 * of focus. Process-wide (not per-ctx) so the desktop's own mouse-cursor-
 * shape logic (it owns drawing the actual OS pointer, which lives
 * outside any single app's window) can pick a text-beam cursor while
 * hovering ANY app's textbox, not just the desktop's own search field —
 * desktop.c has no visibility into what's drawn inside an app's window
 * otherwise. Reset to 0 at the start of every widget_begin_frame() call
 * and set to 1 by widget_textbox() if the mouse is over it that frame,
 * so it always reflects only the most recent frame checked, from
 * whichever window/app most recently ran. */
int widget_wants_text_cursor(void);

/* ───────────────────────────── scrollbar (vertical) ─────────────────── */

/* Draws a vertical scrollbar track+thumb at (x,y), track height `h`.
 * `content_h`/`view_h` describe the scrollable content vs. visible area
 * (same units, e.g. pixels or rows — caller's choice, must be consistent).
 * *scroll is the current scroll offset in those same units; clamped to
 * [0, max(0, content_h - view_h)]. Returns 1 if *scroll changed. */
int widget_scrollbar_v(widget_ctx_t *ctx, uint32_t id, int x, int y, int h,
                       int *scroll, int content_h, int view_h);

/* ───────────────────────────── dropdown ─────────────────────────────── */

typedef struct {
    int open;   /* 1 = option list currently expanded                    */
} widget_dropdown_state_t;

void widget_dropdown_state_init(widget_dropdown_state_t *st);

/* Closed dropdown is one row (w x ~24px); when open, draws up to
 * `n_options` rows below it (caller is responsible for leaving enough
 * room in the window layout, or for accepting overdraw onto whatever's
 * below — simplest is to draw dropdowns last). *selected is an index into
 * options[0..n_options-1]. Returns 1 on the frame the selection changed.
 *
 * Z-order: the open option list is NOT drawn immediately at this call
 * site — it's queued and actually drawn by widget_end_frame(), which your
 * app must call once after all other widget_* calls each frame. This
 * guarantees the open list always renders on top of every other widget,
 * regardless of call order (you don't need to "draw dropdowns last"
 * manually). Click-to-select still works correctly immediately (hit-
 * testing uses this frame's already-snapshotted mouse position, not
 * drawing order). */
int widget_dropdown(widget_ctx_t *ctx, uint32_t id, int x, int y, int w,
                    const char **options, int n_options, int *selected,
                    widget_dropdown_state_t *st);

/* ───────────────────────────── color picker ─────────────────────────── */

/* Fixed 8x2 swatch grid of common UI colours (see widgets.c) plus the
 * currently-selected colour highlighted with a border. swatch_size is the
 * pixel size of each square (16-24 is reasonable). *color is read AND
 * written: pass the current colour in (used to highlight the matching
 * swatch if any), get the newly clicked colour out. Returns 1 if changed. */
int widget_color_picker(widget_ctx_t *ctx, uint32_t id, int x, int y,
                        int swatch_size, uint32_t *color);

/* ───────────────────────────── clipping ─────────────────────────────── */

/* Confine subsequent drawing (widget_label/rect/rect_border/line/circle,
 * and the internals of every other widget_* call) to (x,y,w,h) until
 * widget_end_clip(). Anything that would draw outside this rect is
 * silently dropped instead of overflowing past it — use this around any
 * scrollable or otherwise-not-naturally-bounded content (e.g. a list
 * whose rows may be partially scrolled past the top/bottom of its
 * container). Coordinates are in the same space as everything else for
 * this ctx (window-relative normally, or absolute screen coordinates in
 * raw mode). Not valid in raw mode (no window to clip against — raw-mode
 * callers are expected to only draw inside their own intended bounds).
 * Clipping does not nest: a second widget_begin_clip() before
 * widget_end_clip() replaces the previous clip rect, it doesn't intersect
 * with it. Always pair with widget_end_clip() — leaving clipping on
 * affects every subsequent draw call in the window, not just the ones
 * you intended. */
void widget_begin_clip(widget_ctx_t *ctx, int x, int y, int w, int h);
void widget_end_clip(widget_ctx_t *ctx);

/* ───────────────────────────── shapes ───────────────────────────────── */
/* Pure drawing helpers, no interaction, window-relative coordinates.     */

void widget_rect      (widget_ctx_t *ctx, int x, int y, int w, int h,
                       uint32_t color, int filled);
void widget_rect_border(widget_ctx_t *ctx, int x, int y, int w, int h,
                        uint32_t color, int thickness);
void widget_line      (widget_ctx_t *ctx, int x0, int y0, int x1, int y1,
                       uint32_t color);
void widget_circle    (widget_ctx_t *ctx, int cx, int cy, int r,
                       uint32_t color, int filled);

#endif /* WIDGETS_H */
