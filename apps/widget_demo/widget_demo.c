/* Widget Demo — exercises every widget in gui/widgets/widgets.c.
 * See widget_demo.h and gui/widgets/widgets.h for the documented usage
 * pattern this app follows. */
#include "../../kernel/types.h"
#include "../../kernel/klibc.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/drivers/vga/fb.h"
#include "../../gui/compositor/compositor.h"
#include "../../gui/widgets/widgets.h"
#include "widget_demo.h"

void app_widget_demo(void) {
    __asm__ volatile ("sti");

    uint32_t W = 520, H = 460;
    uint32_t base_wx = fb.width  > W ? (fb.width  - W) / 2 : 0;
    uint32_t base_wy = fb.height > H ? (fb.height - H) / 2 : 0;
    /* BUG FIX (windows spawn perfectly stacked): every instance used to
     * spawn at this exact centred position, so opening Widget Demo twice
     * produced two windows perfectly overlapping pixel-for-pixel — with
     * chrome not yet draggable at the time this was reported, the
     * background one was completely unreachable. comp_next_spawn_pos()
     * cascades each new window from the same base position instead. */
    uint32_t wx, wy;
    comp_next_spawn_pos(base_wx, base_wy, W, H, &wx, &wy);
    int win = comp_create_window("Widget Demo", wx, wy, W, H);
    if (win < 0) { sched_exit(); return; }

    widget_ctx_t wctx;
    widget_ctx_init(&wctx);

    int    click_count   = 0;
    int    chk_a = 1, chk_b = 0;
    float  slider_val    = 35.0f;
    char   name_buf[64]  = "DracolaxOS";
    widget_textbox_state_t name_st;
    widget_textbox_state_init(&name_st);
    int    scroll        = 0;
    static const char *fruit[]  = { "Apple", "Banana", "Cherry", "Mango", "Pear" };
    int    fruit_sel     = 0;
    widget_dropdown_state_t dd_st;
    widget_dropdown_state_init(&dd_st);
    uint32_t picked_color = 0x7828C8u;

    int running = 1;
    while (running) {
        comp_window_clear(win);
        widget_begin_frame(&wctx, win);
        /* Esc has two jobs depending on state: if a textbox is focused,
         * it should defocus the textbox (handled inside widget_textbox)
         * WITHOUT also closing the window on the same keypress. So we
         * snapshot focus state here, before any widget_* calls run this
         * frame, and only treat Esc as "close" if nothing was focused
         * when the frame started. */
        int had_focus = (wctx.focused_id != 0);

        widget_label(&wctx, 12, 10, "Widget Toolkit Demo — Esc to close", WIDGET_COL_TEXT);

        /* Buttons + click counter */
        if (widget_button(&wctx, WID(__LINE__), 12, 36, 100, 28, "Click me"))
            click_count++;
        char cbuf[32];
        snprintf(cbuf, sizeof(cbuf), "Clicks: %d", click_count);
        widget_label(&wctx, 124, 44, cbuf, WIDGET_COL_TEXT_DIM);

        /* Checkboxes */
        widget_checkbox(&wctx, WID(__LINE__), 12, 76, &chk_a, "Option A");
        widget_checkbox(&wctx, WID(__LINE__), 140, 76, &chk_b, "Option B");

        /* Slider + live value label */
        widget_slider(&wctx, WID(__LINE__), 12, 112, 200, &slider_val, 0.0f, 100.0f);
        /* BUG FIX: this used to be snprintf(cbuf, ..., "%.0f", (double)slider_val).
         * The kernel's freestanding klibc vsnprintf() has no '%f'/precision
         * support at all (this build also passes -mno-sse, so adding
         * float varargs support safely is a separate, riskier change — see
         * docs/CHANGELOG.md). With no 'f' case, the parser fell through
         * character-by-character and printed the literal text ".0f"
         * instead of the number. Round to the nearest int in plain float
         * arithmetic (already used throughout this file/widgets.c without
         * issue) and format that with the well-supported "%d". */
        snprintf(cbuf, sizeof(cbuf), "%d", (int)(slider_val + 0.5f));
        widget_label(&wctx, 220, 112, cbuf, WIDGET_COL_TEXT);

        /* Progress bar driven by the slider, just to show it live */
        widget_progress_bar(&wctx, 12, 140, 200, 14, slider_val / 100.0f);

        /* Textbox */
        widget_label(&wctx, 12, 166, "Name:", WIDGET_COL_TEXT_DIM);
        widget_textbox(&wctx, WID(__LINE__), 70, 162, 160, name_buf, sizeof(name_buf), &name_st);

        /* Dropdown — its open list (if any) is queued, not drawn here; see
         * widget_end_frame() below for why. */
        widget_label(&wctx, 250, 166, "Fruit:", WIDGET_COL_TEXT_DIM);
        widget_dropdown(&wctx, WID(__LINE__), 300, 162, 120, fruit, 5, &fruit_sel, &dd_st);

        /* Vertical scrollbar next to a fake content list */
        widget_label(&wctx, 12, 206, "Scrollable list (drag the bar):", WIDGET_COL_TEXT_DIM);
        int list_x = 12, list_y = 226, list_w = 220, list_h = 90;
        widget_rect(&wctx, list_x, list_y, list_w, list_h, WIDGET_COL_PANEL, 1);
        widget_rect_border(&wctx, list_x, list_y, list_w, list_h, WIDGET_COL_BORDER, 1);
        int content_h = 300, row_h = 18;
        int first_row = scroll / row_h;
        /* BUG FIX: when `scroll` isn't an exact multiple of row_h, the
         * first visible row's on-screen y (list_y + row*row_h - scroll)
         * comes out slightly NEGATIVE relative to list_y — that's correct
         * for a partially-scrolled-past row, but widget_label has no
         * concept of bounds on its own, so that row's text overflowed
         * above the list box's top border instead of being cut off at it.
         * widget_begin_clip()/widget_end_clip() (gui/widgets/) confine
         * every draw call in between to the list's rect, so the
         * partially-scrolled row is now correctly clipped instead of
         * spilling out. */
        widget_begin_clip(&wctx, list_x, list_y, list_w, list_h);
        for (int r = 0; r * row_h < list_h + row_h; r++) {
            int row = first_row + r;
            if (row * row_h - scroll > list_h) break;
            char rbuf[24];
            snprintf(rbuf, sizeof(rbuf), "Item %d", row);
            widget_label(&wctx, list_x + 6, list_y + row * row_h - scroll, rbuf, WIDGET_COL_TEXT);
        }
        widget_end_clip(&wctx);
        widget_scrollbar_v(&wctx, WID(__LINE__), list_x + list_w + 4, list_y, list_h,
                           &scroll, content_h, list_h);

        /* Colour picker */
        widget_label(&wctx, 260, 206, "Colour:", WIDGET_COL_TEXT_DIM);
        widget_color_picker(&wctx, WID(__LINE__), 260, 226, 18, &picked_color);

        /* Shape primitives, tinted with the picked colour */
        widget_label(&wctx, 12, 330, "Shapes:", WIDGET_COL_TEXT_DIM);
        widget_rect  (&wctx, 12, 350, 60, 40, picked_color, 1);
        widget_rect_border(&wctx, 84, 350, 60, 40, picked_color, 2);
        widget_line  (&wctx, 156, 350, 216, 390, picked_color);
        widget_line  (&wctx, 216, 350, 156, 390, picked_color);
        widget_circle(&wctx, 260, 370, 20, picked_color, 1);
        widget_circle(&wctx, 312, 370, 20, picked_color, 0);

        /* Must be called after every other widget_* call this frame —
         * draws the open dropdown list (if any) on top of everything
         * else, instead of wherever it happened to be drawn at its own
         * call site. See widgets.h's widget_end_frame() doc. */
        widget_end_frame(&wctx);

        if (wctx.key_char == 0x1B && !had_focus) running = 0;
        if (widget_should_close(&wctx)) running = 0;

        /* BUG FIX (whole-desktop flicker/lag while this app is open): this
         * app must NOT call comp_render()/fb_flip() itself — the desktop
         * task is the sole compositor and does that once per ITS frame.
         * Calling them here too raced two uncoordinated compositing+flip
         * passes against the shared framebuffer, causing visible tearing
         * across the whole desktop (not just this window) and roughly
         * doubling per-frame compositing cost. Just draw into this
         * window's own backbuffer (every widget_* call above already did
         * that) and yield/sleep — see widgets.h's "RENDERING" section.
         *
         * BUG FIX (residual flicker after fixing the above): each window
         * has exactly ONE backbuffer, written by this app and read by the
         * desktop task's compositor — there's no front/back buffer swap,
         * so if the desktop task's comp_render() happens to run WHILE
         * this app is partway through a redraw (comp_window_clear + ~15
         * widget_* calls), it composites a half-drawn frame for that one
         * cycle: a brief visible flicker. A bare sched_yield() let this
         * app redraw as fast as the scheduler would allow — far more
         * often than the desktop task's own ~30fps (sched_sleep(33) in
         * desktop.c's main loop) — which maximised how often a redraw-in-
         * progress could get caught mid-frame. Sleeping for the same
         * ~33ms instead means this app spends nearly all its time NOT
         * redrawing, shrinking that race window to a small fraction of
         * its lifetime. This doesn't eliminate the race (that needs real
         * per-window double-buffering, tracked as future work alongside
         * dirty-rect rendering — see docs/GUIState.md section 1/10), but
         * it makes it rare enough not to be visible in normal use. */
        sched_sleep(33);
    }

    comp_destroy_window(win);
    sched_exit();
}
