/* gui/desktop/default-desktop/dock.h
 * Floating "open apps" panel — Alt+Tab style switcher.
 *
 * REPURPOSED (see docs/CHANGELOG.md, GUI Finalization follow-up): this
 * used to be a persistent, always-visible taskbar showing pinned +
 * registered apps plus a clock/username footer. It's now a HIDDEN-BY-
 * DEFAULT panel that only appears while toggled on (Alt+Tab — see
 * kernel/drivers/ps2/keyboard.c, where KB_KEY_ALTTAB was already defined
 * but never actually wired to anything), lists only currently OPEN
 * windows (not the full app registry), and draws in the overlay tier —
 * above every window, not as a permanent bottom/side layer competing
 * with window content. The clock/username footer was removed entirely;
 * that's owned by draw_widgets() in desktop.c (the top-right clock
 * panel), not duplicated here.
 *
 * Visual anatomy (unchanged from before, minus the clock/user footer):
 *   • Active indicator : 4 px accent bar on the panel's left rim
 *   • Hover highlight  : semi-transparent rounded box behind icon
 *   • Scrollable       : scroll wheel / arrow indicators when overflow
 */
#ifndef DOCK_H
#define DOCK_H

#include "../../../kernel/types.h"
#include "../../../kernel/dxi/dxi.h"

/* ── geometry ──────────────────────────────────────────── */
#define DOCK_W            64      /* panel pixel width                */
#define DOCK_ICON_SZ      40      /* icon bounding-box size (px)      */
#define DOCK_ICON_GAP      6      /* gap between icon slots           */
#define DOCK_PAD_TOP      12      /* panel top inner padding          */
#define DOCK_PAD_BOT      10      /* panel bottom inner padding       */
#define DOCK_CORNER_R     14      /* panel corner radius              */
#define DOCK_PANEL_X       8      /* panel x offset from screen edge  */
#define DOCK_MAX_VISIBLE  10      /* max visible icons before scroll  */
/* Max open-app slots == the compositor's own window cap, since every
 * slot now maps 1:1 to an actually-open window. */
#define DOCK_SLOTS_MAX    16

/* ── per-slot state ─────────────────────────────────────── */
typedef struct {
    char name[32];    /* window title, used as both label and icon key */
    int  win_handle;  /* compositor window handle this slot represents */
    int  task_id;     /* scheduler id of the owning task                */
} dock_slot_t;

/* ─── public API ─────────────────────────────────────────── */

/* Call once after desktop fb setup. Resets state; no default pins
 * anymore (there's nothing to pin — the panel only ever shows windows
 * that actually exist right now). */
void dock_init(void);

/* Show / hide the panel. Toggled by Alt+Tab in desktop.c. */
void dock_open(void);
void dock_close(void);
int  dock_is_open(void);

/* Draw panel onto the shadow buffer each frame. No-op while closed. */
void dock_draw(int cursor_x, int cursor_y);

/* Handle a left mouse click. Returns 1 if the click was inside the panel
 * (even if it hit nothing actionable) — callers should treat that as
 * "consumed, don't fall through to window/desktop click handling."
 * Clicking a slot focuses + un-minimises that window and closes the
 * panel (standard alt-tab-switcher behaviour). No-op (returns 0) while
 * closed. */
int  dock_click(int x, int y);

/* Scroll the open-apps list up(-1) / down(+1). */
void dock_scroll(int delta);

/* Keyboard navigation — Up/Down move a keyboard-driven selection
 * (independent of mouse hover), Enter activates it (same effect as
 * clicking that slot: focus + un-minimise + close panel). Returns 1 if
 * something was activated, matching dock_click()'s return convention. */
void dock_move_selection(int delta);
int  dock_activate_selected(void);

/* Panel geometry — other systems may need these to avoid overlap. */
int  dock_panel_x(void);
int  dock_panel_y(void);
int  dock_panel_w(void);
int  dock_panel_h(void);

/* Returns the index of the slot currently under the cursor (-1 = none). */
int  dock_hover_slot(void);

#endif /* DOCK_H */
