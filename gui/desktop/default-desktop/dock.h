/* gui/desktop/default-desktop/dock.h
 * Floating side dock — Windows 11 style, glassmorphism.
 *
 * The dock is a floating rounded panel on the left edge.
 * It shows pinned apps + any running apps, with:
 *   • Active indicator : 4 px accent bar on the panel's left rim
 *   • Hover highlight  : semi-transparent rounded box behind icon
 *   • Running dot      : small accent circle below icon
 *   • Scrollable       : scroll wheel / arrow indicators when overflow
 *   • Clock + user     : at the bottom inside the panel
 */
#ifndef DOCK_H
#define DOCK_H

#include "../../../kernel/types.h"
#include "../../../kernel/dxi/dxi.h"
#include "appman.h"

/* ── geometry ──────────────────────────────────────────── */
#define DOCK_W            64      /* panel pixel width                */
#define DOCK_ICON_SZ      40      /* icon bounding-box size (px)      */
#define DOCK_ICON_GAP      6      /* gap between icon slots           */
#define DOCK_PAD_TOP      12      /* panel top inner padding          */
#define DOCK_PAD_BOT      10      /* panel bottom inner padding       */
#define DOCK_CORNER_R     14      /* panel corner radius              */
#define DOCK_PANEL_X       8      /* panel x offset from screen edge  */
#define DOCK_MAX_VISIBLE  10      /* max visible icons before scroll  */
#define DOCK_PINS_MAX     16      /* max pinned slots                 */

/* ── per-slot state ─────────────────────────────────────── */
typedef struct {
    char name[32];    /* registered app name                 */
    int  pinned;      /* 1 = user-pinned, stays even if idle */
    int  running;     /* 1 = a task is alive for this app    */
    int  task_id;     /* scheduler id of running task (-1)   */
} dock_slot_t;

/* ─── public API ─────────────────────────────────────────── */

/* Call once after desktop fb setup. Loads default pins & icons. */
void dock_init(void);

/* Draw panel onto the shadow buffer each frame. */
void dock_draw(int cursor_x, int cursor_y);

/* Handle a left mouse click. Returns 1 if the click was inside the panel. */
int  dock_click(int x, int y);

/* Handle right-click inside panel — for pin/unpin context. */
int  dock_right_click(int x, int y);

/* Scroll dock icon list up(-1) / down(+1). */
void dock_scroll(int delta);

/* Notify the dock that a task has died (removes running indicator). */
void dock_task_died(int task_id);

/* Panel geometry — other systems may need these to avoid overlap. */
int  dock_panel_x(void);
int  dock_panel_y(void);
int  dock_panel_w(void);
int  dock_panel_h(void);

/* Returns the index of the slot currently under the cursor (-1 = none). */
int  dock_hover_slot(void);

/* Per-slot metadata — used by the context menu layer. */
const char *dock_slot_name     (int slot);  /* app name string, or "" */
int         dock_slot_is_pinned(int slot);  /* 1 if user-pinned */
int         dock_slot_is_running(int slot); /* 1 if a task is alive */
int         dock_slot_task_id  (int slot);  /* scheduler task id, or -1 */

/* Toggle pin/unpin for a slot (mirrors dock_right_click but explicit). */
void dock_pin_toggle(int slot);

#endif /* DOCK_H */
