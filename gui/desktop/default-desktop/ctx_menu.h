/* gui/desktop/default-desktop/ctx_menu.h
 * DracolaxOS Context Menu System — Layer 3: UI Layer
 *
 * Design rule: this layer ONLY renders and reacts to clicks.
 * It never calls keyboard/mouse polling code directly.
 * All positional data arrives via function parameters.
 */
#ifndef CTX_MENU_H
#define CTX_MENU_H

#include "../../../kernel/types.h"
#include "ctx_resolver.h"

/* ── Action IDs ─────────────────────────────────────────────────────────
 * Each menu item maps to one of these.  The action handler in ctx_menu.c
 * dispatches them to the appropriate subsystem (compositor, dock, appman…).
 * IDs are stable — add new ones only at the end of each group.
 */
typedef enum {
    CMID_NONE = 0,

    /* Desktop */
    CMID_DESKTOP_REFRESH,
    CMID_DESKTOP_TERMINAL,
    CMID_DESKTOP_NEW_FOLDER,
    CMID_DESKTOP_NEW_FILE,
    CMID_DESKTOP_DISPLAY_SETTINGS,
    CMID_DESKTOP_SYSTEM_SETTINGS,
    CMID_DESKTOP_CHANGE_BG,
    CMID_DESKTOP_ABOUT,
    CMID_DESKTOP_LOGOUT,
    CMID_DESKTOP_REBOOT,
    CMID_DESKTOP_SHUTDOWN,

    /* File */
    CMID_FILE_OPEN,
    CMID_FILE_OPEN_WITH,
    CMID_FILE_CUT,
    CMID_FILE_COPY,
    CMID_FILE_RENAME,
    CMID_FILE_DELETE,
    CMID_FILE_PROPERTIES,

    /* Folder */
    CMID_FOLDER_OPEN,
    CMID_FOLDER_OPEN_NEW_WINDOW,
    CMID_FOLDER_CUT,
    CMID_FOLDER_COPY,
    CMID_FOLDER_RENAME,
    CMID_FOLDER_DELETE,
    CMID_FOLDER_NEW_FILE,
    CMID_FOLDER_NEW_FOLDER,
    CMID_FOLDER_PROPERTIES,

    /* Window body (generic app area) */
    CMID_WIN_MINIMIZE,
    CMID_WIN_MAXIMIZE,
    CMID_WIN_CLOSE,
    CMID_WIN_ALWAYS_ON_TOP,
    CMID_WIN_INSPECT,

    /* Title bar */
    CMID_TB_MOVE,
    CMID_TB_MINIMIZE,
    CMID_TB_MAXIMIZE,
    CMID_TB_CLOSE,

    /* Taskbar / Dock */
    CMID_DOCK_OPEN_APP,
    CMID_DOCK_PIN_UNPIN,
    CMID_DOCK_CLOSE_WINDOW,
    CMID_DOCK_APP_SETTINGS,

    /* File-manager empty space */
    CMID_FM_REFRESH,
    CMID_FM_SORT_BY,
    CMID_FM_NEW_FILE,
    CMID_FM_NEW_FOLDER,
    CMID_FM_PASTE,
} ctx_menu_id_t;

/* ── Public API ──────────────────────────────────────────────────────── */

/* Open a context menu positioned near (hit->x, hit->y) for the resolved hit.
 * A previously open menu is closed and replaced. */
void ctx_menu_open (const ctx_hit_t *hit);

/* Dismiss the active menu without executing any action. */
void ctx_menu_close(void);

/* Returns non-zero while a menu is visible. */
int  ctx_menu_is_open(void);

/* Render the menu (and any open submenu) onto the shadow framebuffer.
 * cursor_x/y drive hover highlights. Call every frame while the menu
 * is open, after compositing windows and before fb_flip(). */
void ctx_menu_draw(int cursor_x, int cursor_y);

/* Process a left-click at (px, py).
 * Returns 1 if the click was consumed (inside the menu or submenu).
 * Returns 0 if the click was outside — caller should dismiss and re-handle.
 * Automatically calls ctx_menu_close() after a selection is made. */
int  ctx_menu_click(int px, int py);

#endif /* CTX_MENU_H */
