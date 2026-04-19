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
    int       focused;
    int       desktop;       /* virtual desktop index */
    int       task_id;       /* scheduler task that owns this window (-1=none) */
    /* saved geometry for maximize/restore */
    uint32_t  saved_x, saved_y, saved_w, saved_h;
    int       maximized;
    int       always_on_top; /* 1 = z pinned above all normal windows */
    uint32_t  border_color;
    uint32_t  title_color;
} window_t;

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

/* Get the task ID stored in a window (for dock notification on close) */
int  comp_get_task_id(int handle);
void comp_get_pos     (int handle, int *ox, int *oy); /* get window top-left */

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

/* Run compositor main loop as a kernel task */
void comp_task(void);

#endif /* COMPOSITOR_H */
