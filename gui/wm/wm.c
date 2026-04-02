/* gui/wm/wm.c — Window manager implementation */
#include "../../kernel/types.h"
#include "../../kernel/drivers/vga/fb.h"
#include "../../kernel/klibc.h"
#include "../../kernel/log.h"
#include "../../kernel/mm/vmm.h"
#include "wm.h"

static wm_window_t windows[WM_MAX_WINDOWS];
static int win_count = 0;
static int focused_id = -1;
static int current_desktop = 0;
static int task_switcher_idx = -1;

/* ---- Shadow ------------------------------------------------------------ */
#define SHADOW_BLUR  8
#define SHADOW_COLOR 0x22000000u  /* dark semi-transparent */

void wm_draw_shadow(int x, int y, int w, int h) {
    for (int i = 1; i <= SHADOW_BLUR; i++) {
        uint8_t alpha = (uint8_t)(20 - i * 2);
        uint32_t col = 0x000000u | ((uint32_t)alpha << 24);
        /* Draw shadow frame around window */
        int sx = x + i, sy = y + i;
        if (sx >= 0 && sy >= 0)
            fb_fill_rect((uint32_t)sx, (uint32_t)(sy + h),
                         (uint32_t)w, 1, col & 0xFFFFFF);
        if (sx >= 0 && sy >= 0)
            fb_fill_rect((uint32_t)(sx + w), (uint32_t)sy,
                         1, (uint32_t)h, col & 0xFFFFFF);
    }
}

/* ---- Animation --------------------------------------------------------- */
static void animate_open(wm_window_t *win) {
    /* Scale-in: 4 frames growing from 50% to 100% size */
    for (int f = 0; f < 4; f++) {
        int wf = win->w * (50 + f * 12) / 100;
        int hf = win->h * (50 + f * 12) / 100;
        int xf = win->x + (win->w - wf) / 2;
        int yf = win->y + (win->h - hf) / 2;
        fb_fill_rect((uint32_t)xf, (uint32_t)yf, (uint32_t)wf, (uint32_t)hf, 0x1A1D3Au);
        fb_flip();
    }
}

/* ---- Public API -------------------------------------------------------- */

void wm_init(void) {
    memset(windows, 0, sizeof(windows));
    win_count = 0;
    focused_id = -1;
    current_desktop = 0;
    kinfo("WM: window manager initialised\n");
}

int wm_create_window(const char *title, int x, int y, int w, int h) {
    if (win_count >= WM_MAX_WINDOWS) return -1;
    wm_window_t *win = &windows[win_count];
    win->id      = win_count;
    win->x = x; win->y = y; win->w = w; win->h = h;
    strncpy(win->title, title ? title : "Window", 63);
    win->desktop = current_desktop;
    win->z       = (uint32_t)win_count;  /* BUG FIX 1.2: assign initial z */
    win->state   = WM_WIN_NORMAL;
    win->visible = 1;
    win->focused = 0;
    /* Allocate back buffer */
    win->fb_buf = (uint32_t *)kzalloc((size_t)(w * h * 4));
    animate_open(win);
    kinfo("WM: created window '%s' id=%d\n", win->title, win->id);
    return win_count++;
}

void wm_destroy_window(int id) {
    if (id < 0 || id >= win_count) return;
    windows[id].visible = 0;
    kinfo("WM: destroyed window id=%d\n", id);
}

void wm_focus_window(int id) {
    if (focused_id >= 0 && focused_id < win_count)
        windows[focused_id].focused = 0;
    focused_id = id;
    if (id >= 0 && id < win_count) {
        windows[id].focused = 1;
        /* BUG FIX 1.2: raise z so this window renders on top */
        uint32_t maxz = 0;
        for (int i = 0; i < win_count; i++)
            if (windows[i].z > maxz) maxz = windows[i].z;
        windows[id].z = maxz + 1;
    }
}

void wm_snap_window(int id, wm_win_state_t snap) {
    if (id < 0 || id >= win_count) return;
    wm_window_t *w = &windows[id];
    uint32_t SW = fb.width, SH = fb.height;
    switch (snap) {
    case WM_WIN_SNAPPED_LEFT:
        w->x=0; w->y=28; w->w=(int)SW/2; w->h=(int)SH-28; break;
    case WM_WIN_SNAPPED_RIGHT:
        w->x=(int)SW/2; w->y=28; w->w=(int)SW/2; w->h=(int)SH-28; break;
    case WM_WIN_MAXIMISED:
    case WM_WIN_SNAPPED_FULL:
        w->x=0; w->y=28; w->w=(int)SW; w->h=(int)SH-28; break;
    default: break;
    }
    w->state = snap;
    kinfo("WM: snapped window %d to state %d\n", id, (int)snap);
}

void wm_resize_window(int id, int w, int h) {
    if (id < 0 || id >= win_count) return;
    windows[id].w = w; windows[id].h = h;
}

void wm_move_window(int id, int x, int y) {
    if (id < 0 || id >= win_count) return;
    windows[id].x = x; windows[id].y = y;
}

void wm_switch_desktop(int idx) {
    if (idx < 0 || idx >= 4) return;
    current_desktop = idx;
    kinfo("WM: switched to desktop %d\n", idx);
}

int wm_current_desktop(void) { return current_desktop; }

void wm_task_switcher_open(void) {
    task_switcher_idx = focused_id;
    kinfo("WM: task switcher opened (current=%d)\n", focused_id);
}

void wm_task_switcher_next(void) {
    if (win_count == 0) return;
    int next = (task_switcher_idx + 1) % win_count;
    /* Skip invisible windows */
    for (int i = 0; i < win_count; i++) {
        if (windows[next].visible && windows[next].desktop == current_desktop) break;
        next = (next + 1) % win_count;
    }
    task_switcher_idx = next;
}

void wm_task_switcher_commit(void) {
    wm_focus_window(task_switcher_idx);
    task_switcher_idx = -1;
}

void wm_render_frame(void) {
    /* BUG FIX 1.2: sort by z so focused/raised windows draw on top */
    int order[WM_MAX_WINDOWS];
    for (int i = 0; i < win_count; i++) order[i] = i;
    for (int i = 0; i < win_count - 1; i++)
        for (int j = i + 1; j < win_count; j++)
            if (windows[order[j]].z < windows[order[i]].z) {
                int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }

    for (int oi = 0; oi < win_count; oi++) {
        wm_window_t *w = &windows[order[oi]];
        if (!w->visible || w->desktop != current_desktop) continue;
        /* Shadow */
        wm_draw_shadow(w->x, w->y, w->w, w->h);
        /* Title bar */
        uint32_t tbcol = w->focused ? 0x7828C8u : 0x21262Du;
        fb_fill_rect((uint32_t)w->x, (uint32_t)w->y, (uint32_t)w->w, 24, tbcol);
        fb_print((uint32_t)(w->x + 8), (uint32_t)(w->y + 4),
                 w->title, 0xE6EDF3u, tbcol);
        /* Close button */
        fb_fill_rect((uint32_t)(w->x + w->w - 20), (uint32_t)(w->y + 4),
                     14, 14, 0xFF5F57u);
        /* Body */
        if (w->fb_buf) {
            fb_blit((uint32_t)w->x, (uint32_t)(w->y + 24),
                    (uint32_t)w->w, (uint32_t)(w->h - 24), w->fb_buf);
        } else {
            fb_fill_rect((uint32_t)w->x, (uint32_t)(w->y + 24),
                         (uint32_t)w->w, (uint32_t)(w->h - 24), 0x0D1117u);
        }
    }
}
