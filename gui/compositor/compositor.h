/* gui/compositor/compositor.h — Simple kernel-mode compositor */
#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include "../../kernel/types.h"
#include "../../kernel/drivers/vga/fb.h"

#define COMP_MAX_WINDOWS  16
#define COMP_TITLE_MAX    64

typedef struct window {
    uint32_t  x, y, w, h;   /* screen position and size  */
    uint32_t  z;             /* z-order (higher = in front) */
    char      title[COMP_TITLE_MAX];
    uint32_t *backbuf;       /* pixel buffer (w*h)         */
    int       visible;
    int       minimized;     /* 1 = hidden by user (minimize btn / Super+H) */
    int       focused;
    int       desktop;       /* virtual desktop index */
    int       task_id;       /* scheduler task that owns this window (-1=none) */
    /* saved geometry for maximize/restore */
    uint32_t  saved_x, saved_y, saved_w, saved_h;
    int       maximized;
    int       always_on_top; /* 1 = z pinned above all normal windows */
    uint32_t  border_color;
    uint32_t  title_color;
    uint32_t  win_flags;    /* WIN_FLAG_* bitmask */
    /* Content clip rect (content/backbuf-relative coords), see
     * comp_window_set_clip(). clip_active=0 (the zero-init default for any
     * freshly created window) means "no clipping" — comp_window_fill/print/
     * set_pixel behave exactly as before. */
    int       clip_active;
    int       clip_x, clip_y, clip_w, clip_h;
} window_t;

/* win_flags bits */
#define WIN_FLAG_TERMINAL  0x01u  /* text-input app → TEXT cursor over body */

/* Resize edge / corner identifiers — returned by comp_resize_edge_at(). */
typedef enum {
    RESIZE_NONE = 0,
    RESIZE_N,   /* top edge */
    RESIZE_S,   /* bottom edge */
    RESIZE_E,   /* right edge */
    RESIZE_W,   /* left edge */
    RESIZE_NW,  /* top-left corner */
    RESIZE_NE,  /* top-right corner */
    RESIZE_SW,  /* bottom-left corner */
    RESIZE_SE,  /* bottom-right corner */
} resize_edge_t;

/* Create a window; returns handle ≥ 0 or -1 on failure */
int  comp_create_window(const char *title, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h);
void comp_destroy_window(int handle);
void comp_move_window   (int handle, uint32_t x, uint32_t y);
void comp_focus_window  (int handle);

/* Draw a string into a window's back buffer */
void comp_window_print(int handle, uint32_t x, uint32_t y,
                       const char *s, uint32_t fg);

/* Fill a window region */
void comp_window_fill(int handle, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint32_t color);

/* Single-pixel write/read (bounds-checked; out-of-range coords are a no-op
 * for set, return 0 for get). Used by gui/widgets/ for line/circle drawing
 * that comp_window_fill's rect-only fill can't express. */
void     comp_window_set_pixel(int handle, int x, int y, uint32_t color);
uint32_t comp_window_get_pixel(int handle, int x, int y);

/* Content clipping. Once set, comp_window_fill/print/set_pixel only ever
 * affect pixels inside (x,y,w,h) (content/backbuf-relative coordinates,
 * same space as every other comp_window_* call) — anything outside is
 * silently dropped, even if the unclipped call would have drawn there.
 * This is what lets a scrollable list (or any other content that doesn't
 * neatly align to its container's bounds) stay visually contained instead
 * of overflowing past its border. Clipping is per-window and persists
 * across draw calls until comp_window_clear_clip() is called or the
 * window is destroyed — so a typical scroll-region usage is:
 *   comp_window_set_clip(win, list_x, list_y, list_w, list_h);
 *   ... draw rows, some of which may fall partially outside ...
 *   comp_window_clear_clip(win);
 *   ... draw the rest of the window's content unclipped ... */
void comp_window_set_clip  (int handle, int x, int y, int w, int h);
void comp_window_clear_clip(int handle);

/* Clear window body to its background colour */
void comp_window_clear(int handle);

/* Formatted print into a window — wraps comp_window_print */
void comp_window_printf(int handle, uint32_t x, uint32_t y,
                        uint32_t fg, const char *fmt, ...);

/* Terminal-mode text console inside a window.
 * comp_win_term_init  — set up a per-window scrolling terminal state.
 * comp_win_term_print — append text (handles \n, auto-scroll).
 * comp_win_term_getchar — blocking read from the focused task's input queue.
 * comp_win_term_readline — read a line with echo into buf[max]. */
typedef struct {
    int      handle;    /* compositor window handle */
    uint32_t cur_x;     /* current text cursor col (pixels) */
    uint32_t cur_y;     /* current text cursor row (pixels) */
    uint32_t body_h;    /* body pixel height (window h - title bar) */
    uint32_t fg;        /* default foreground colour */
    uint32_t bg;        /* background colour */
    int      task_id;   /* owner task id for input routing */
} comp_term_t;

void comp_win_term_init   (comp_term_t *t, int handle, uint32_t fg, uint32_t bg);
void comp_win_term_print  (comp_term_t *t, const char *s);
void comp_win_term_printf (comp_term_t *t, const char *fmt, ...);
int  comp_win_term_getchar(comp_term_t *t);      /* non-blocking; -1 if empty */
int  comp_win_term_readline(comp_term_t *t, char *buf, int max); /* blocking */

/* Virtual desktop control (BUG FIX 1.10) */
void comp_switch_desktop(int idx);

/* BUG FIX (new windows spawn perfectly stacked on top of each other):
 * apps were each independently computing a centred spawn position (e.g.
 * "(fb.width-W)/2, (fb.height-H)/2"), so two windows of the same app (or
 * any two apps that both centre themselves) land at IDENTICAL screen
 * coordinates, perfectly overlapping — combined with chrome not being
 * draggable, the background one becomes completely unreachable.
 *
 * comp_next_spawn_pos() gives every app a shared, cascading placement:
 * offsets each new window down-right from a base (centred, or caller-
 * supplied) position by a fixed step per already-open window on the
 * current desktop, wrapping back near the top-left before it would run
 * off-screen. Call this instead of hand-deriving a spawn position;
 * ignore it (pass your own coordinates to comp_create_window as before)
 * if an app genuinely needs a fixed position (e.g. a modal that must be
 * centred every time). */
void comp_next_spawn_pos(uint32_t base_x, uint32_t base_y,
                         uint32_t win_w, uint32_t win_h,
                         uint32_t *out_x, uint32_t *out_y);
int  comp_current_desktop(void);

/* Alpha-blended icon blit — BGRA 8888 src onto shadow buffer.
 * Integer math only, no floats. Fast paths for A=255 and A=0.
 * dst_stride = fb.width (pixels per scanline, not bytes). */
void blit_icon_bgra(uint32_t dst_x, uint32_t dst_y,
                    const uint32_t *src, uint32_t src_w, uint32_t src_h,
                    uint32_t dst_stride);

/* Window hit-test — used by desktop for click/drag handling */
int  comp_title_bar_at(int px, int py);       /* handle of window whose title bar contains (px,py), or -1 */
int  comp_close_at    (int handle, int px, int py);   /* 1 if close button hit */
int  comp_maximize_at (int handle, int px, int py);   /* 1 if maximize button hit */
int  comp_minimize_at (int handle, int px, int py);   /* 1 if minimize button hit */

/* Body hit-test: topmost visible window whose client area (below title bar)
 * contains (px, py).  Returns handle or -1. */
int  comp_window_body_at(int px, int py);

/* Full window hit-test (titlebar + body + border): topmost visible window
 * that contains (px, py) anywhere.  Returns handle or -1. */
int  comp_window_at(int px, int py);

/* Resize edge detection.
 * Returns which resize edge/corner of window 'handle' the point (px,py) sits on,
 * or RESIZE_NONE if the point is not within the resize border zone. */
resize_edge_t comp_resize_edge_at(int handle, int px, int py);

/* Toggle maximize (full-screen) / restore */
void comp_toggle_maximize(int handle);

/* Hide/show a window without destroying it */
void comp_set_visible(int handle, int visible);
void comp_clear_minimized(int handle);  /* unset minimized flag (e.g. on close) */

/* Get the task ID stored in a window (for dock notification on close) */
int  comp_get_task_id(int handle);
void comp_get_pos     (int handle, int *ox, int *oy); /* get window top-left */

/* Get the screen position of (0,0) in the window's CONTENT coordinate
 * space — i.e. where comp_window_fill/print/set_pixel's own (x,y) origin
 * actually lands on screen. This is NOT the same as comp_get_pos(): that
 * returns the window's OUTER top-left (the top of the title bar), while
 * content drawing starts WIN_TITLE_H pixels below that. Any code that
 * converts an absolute mouse position into window-relative coordinates
 * for hit-testing against widgets/content (not the title bar itself)
 * must use this, not comp_get_pos() — using comp_get_pos() there
 * under-subtracts by the title bar height, an off-by-a-titlebar bug. */
void comp_get_content_pos(int handle, int *cx, int *cy);

/* Get full window geometry (position + size). */
void comp_get_geometry(int handle, int *ox, int *oy, int *ow, int *oh);

/* Set full window geometry.  Enforces minimum size (120 × 60). */
void comp_set_geometry(int handle, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Returns handle of the currently focused window, or -1. */
int  comp_focused_window(void);

/* Always-on-top: window stays above all normal windows regardless of focus.
 * Toggling twice restores normal z-order behaviour. */
void comp_toggle_always_on_top(int handle);
int  comp_is_always_on_top(int handle);

/* Composite all visible windows to framebuffer */
void comp_render(void);

/* Initialise compositor (requires fb_init to have run) */
void comp_init(void);

/* Count visible windows on a given desktop (for workspace switcher UI) */
int comp_count_windows_on_desktop(int desktop);

/* Returns 1 if any window is visible on the current desktop */
int comp_has_windows(void);

/* Window enumeration — added for the dock's "open apps" panel (see
 * dock.c): it needs to list currently-open windows directly rather than
 * going through the app registry. handle must be in [0, comp_window_count()).
 * Note slots are never compacted (comp_destroy_window() only clears
 * visible, it doesn't shift later windows down), so iterate the full
 * range and skip any handle where comp_window_is_visible() is false. */
int         comp_window_count(void);
const char *comp_window_title(int handle);
int         comp_window_is_visible(int handle);

/* comp_window_is_visible() is true only while a window is currently
 * SHOWN. A minimized window and a destroyed one both read visible==0 —
 * comp_destroy_window() and minimizing (comp_set_visible(h,0), which
 * auto-sets minimized=1) look identical from visible alone. Anything
 * that needs to distinguish "exists but minimized" from "actually gone"
 * (e.g. the open-apps/hidden-apps panel in dock.c) needs these instead:
 *   comp_window_exists()      — visible OR minimized (still a real,
 *                               live window, whether shown or hidden)
 *   comp_window_is_minimized() — specifically the minimized flag */
int comp_window_exists(int handle);
int comp_window_is_minimized(int handle);

/* BUG FIX (closed windows leave a visual ghost until an unrelated
 * redraw): apps can close their own window directly
 * (comp_destroy_window() + sched_exit(), e.g. on Esc) with no way to
 * notify the desktop task that anything changed — desktop.c's
 * g_wallpaper_dirty is desktop-task-local state, and there's no
 * cross-task signal for "a window just disappeared." The stale pixels
 * stay in the shadow framebuffer until something else happens to force a
 * full repaint (opening search/switcher, etc).
 *
 * comp_frame_signature() returns a cheap, order-sensitive checksum of
 * every window's count/position/size/visibility/minimized/z state.
 * desktop.c compares this against the previous frame's value each
 * iteration; any difference (window opened, closed, moved, resized,
 * shown, hidden, re-ordered) forces a repaint — catching every case
 * regardless of which code path (app-driven or desktop-driven) caused
 * it, with no new cross-task plumbing needed. Not a hash in the
 * cryptographic sense — collisions are astronomically unlikely for this
 * many small integer fields and the cost of a false negative (a missed
 * repaint) is just a stale frame until the next real change, not
 * incorrect behaviour. */
uint32_t comp_frame_signature(void);

/* GUI debug mode (Ctrl+Super+D — see desktop.c) — draws thin outline
 * rects over every visible window's title bar, close/maximize/minimize
 * buttons, and resize-edge zones, using the exact same geometry the real
 * hit-test functions (comp_title_bar_at/comp_close_at/etc.) use — so
 * what's drawn is guaranteed to match what's actually clickable, not a
 * separate approximation that could drift out of sync. */
void comp_debug_draw_hitboxes(void);

/* Show all hidden windows on a given desktop (Super+R restore). */
void comp_show_all_windows(int desktop);

/* Set window flags (WIN_FLAG_* bits).  Call after comp_create_window. */
void comp_set_flags(int handle, uint32_t flags);
uint32_t comp_get_flags(int handle);

#endif /* COMPOSITOR_H */
