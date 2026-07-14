/* gui/desktop/default-desktop/ctx_resolver.h
 * DracolaxOS Context Menu System — Layer 2: Context Resolver
 *
 * Design rule: this layer ONLY decides what was right-clicked.
 * No drawing, no input polling, no UI state.
 *
 * Flow:
 *   Input layer (desktop.c)
 *       ↓  calls ctx_resolve(x, y)
 *   Context Resolver (ctx_resolver.c)
 *       ↓  returns ctx_hit_t
 *   UI Layer (ctx_menu.c)
 *       ctx_menu_open(&hit) → renders appropriate menu
 */
#ifndef CTX_RESOLVER_H
#define CTX_RESOLVER_H

#include "../../../kernel/types.h"

/* All surface types that can receive a right-click. */
typedef enum {
    CTX_TARGET_NONE = 0,
    CTX_TARGET_DESKTOP,           /* empty wallpaper / desktop background */
    CTX_TARGET_FILE,              /* a file icon on the desktop / file manager */
    CTX_TARGET_FOLDER,            /* a folder icon on the desktop / file manager */
    CTX_TARGET_WINDOW_BODY,       /* inside a window's client area (non-titlebar) */
    CTX_TARGET_TITLEBAR,          /* a window's title bar strip */
    CTX_TARGET_TASKBAR_ITEM,      /* a dock slot that has an app entry */
    CTX_TARGET_FILEMANAGER_EMPTY, /* empty space inside a file-manager window */
} ctx_target_t;

/* Result struct returned by ctx_resolve(). */
typedef struct {
    ctx_target_t type;
    int          win_handle;   /* compositor window handle, -1 if N/A */
    int          dock_slot;    /* dock slot index,          -1 if N/A */
    int          x, y;         /* screen coordinates of the click     */
    char         item_name[64];/* file / folder / app name, if known  */
} ctx_hit_t;

/*
 * ctx_resolve  — query what is at screen position (x, y).
 *
 * Priority order (highest to lowest):
 *   1. Dock slot
 *   2. Window title bar
 *   3. Window body (client area)
 *   4. Desktop fallback
 *
 * Caller must ensure the desktop is in logged-in state before calling.
 * Returns a fully-populated ctx_hit_t; type == CTX_TARGET_NONE only if
 * something unexpected prevents classification.
 */
ctx_hit_t ctx_resolve(int x, int y);

#endif /* CTX_RESOLVER_H */
