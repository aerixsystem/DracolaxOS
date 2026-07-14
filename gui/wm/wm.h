/* gui/wm/wm.h — DracolaxOS window manager (LXQt backend stub)
 *
 * Features:
 *   - window resize (drag corners / edges)
 *   - window snapping (snap to screen halves/quarters)
 *   - virtual desktops
 *   - task switcher (Alt+Tab)
 *   - window shadows
 *   - open/close animations
 */
#ifndef WM_H
#define WM_H
#include "../../kernel/types.h"

#define WM_MAX_WINDOWS 32

typedef enum {
    WM_WIN_NORMAL = 0,
    WM_WIN_MAXIMISED,
    WM_WIN_MINIMISED,
    WM_WIN_SNAPPED_LEFT,
    WM_WIN_SNAPPED_RIGHT,
    WM_WIN_SNAPPED_FULL,
} wm_win_state_t;

typedef struct {
    int          id;
    char         title[64];
    int          x, y, w, h;
    int          desktop;       /* virtual desktop index */
    uint32_t     z;             /* z-order: higher = drawn on top */
    wm_win_state_t state;
    int          focused;
    int          visible;
    uint32_t    *fb_buf;        /* window back buffer */
} wm_window_t;

/* Lifecycle */
void wm_init(void);
int  wm_create_window(const char *title, int x, int y, int w, int h);
void wm_destroy_window(int id);
void wm_focus_window(int id);

/* Layout operations */
void wm_snap_window(int id, wm_win_state_t snap);
void wm_resize_window(int id, int w, int h);
void wm_move_window(int id, int x, int y);

/* Virtual desktops */
void wm_switch_desktop(int idx);
int  wm_current_desktop(void);

/* Task switcher (Alt+Tab) */
void wm_task_switcher_open(void);
void wm_task_switcher_next(void);
void wm_task_switcher_commit(void);

/* Compositing */
void wm_render_frame(void);  /* composite all windows to FB */

/* Shadows */
void wm_draw_shadow(int x, int y, int w, int h);

#endif
