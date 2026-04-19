/* gui/desktop/default-desktop/ctx_menu.c
 * DracolaxOS Context Menu System — Layer 3: UI Layer
 *
 * Responsibilities:
 *   - Build the correct item list for each ctx_target_t.
 *   - Render the glass panel + items + optional submenu.
 *   - Dispatch action IDs to the appropriate subsystem.
 *
 * Prohibited:
 *   - Calling mouse_btn_pressed / keyboard_getchar directly.
 *   - Accessing framebuffer outside of draw functions.
 */
#include "ctx_menu.h"
#include "ctx_resolver.h"
#include "../../compositor/compositor.h"
#include "dock.h"
#include "desktop.h"
#include "appman.h"
#include "../../../kernel/drivers/vga/fb.h"
#include "../../../kernel/klibc.h"
#include "../../../kernel/log.h"
#include "../../../services/power_manager.h"
#include "../../../apps/debug_console/debug_console.h"

/* ── Glassmorphism palette (mirrors desktop.c / compositor.c) ─────────── */
#define COL_VOID         0x04040Cu
#define COL_GLASS_BG     0x0F1020u
#define COL_GLASS_PANEL  0x1A1D3Au
#define COL_GLASS_EDGE   0x3A3F7Au
#define COL_GLASS_SHINE  0x5A60C0u
#define COL_ACCENT       0x7828C8u
#define COL_ACCENT_LT    0xA050F0u
#define COL_ACCENT_DIM   0x3A1460u
#define COL_TEXT_HI      0xF0F0FFu
#define COL_TEXT_MED     0xA0A0C8u
#define COL_TEXT_DIM     0x60607Au
#define COL_SEP          0x2A2C50u
#define COL_ERR          0xC82828u
#define COL_WARN         0xC8A020u

/* ── Layout constants ────────────────────────────────────────────────── */
#define MENU_W        210    /* main menu panel pixel width         */
#define ITEM_H         28    /* height of one menu row (px)         */
#define MENU_PAD_V      6    /* vertical padding inside panel        */
#define MENU_PAD_H      8    /* horizontal text indent               */
#define CORNER_R        8    /* panel corner radius                  */
#define SEP_H           9    /* height reserved for a separator row  */
#define SUB_W         160    /* submenu panel width                  */
#define FONT_W          8
#define FONT_H         16

/* ── Item descriptor ─────────────────────────────────────────────────── */
/* label == NULL → render a separator line in the item slot */
typedef struct {
    const char   *label;   /* display text; NULL = separator              */
    ctx_menu_id_t id;      /* action ID; 0 = separator / no-op            */
    int           has_sub; /* 1 = opens a secondary submenu to the right  */
    int           danger;  /* 1 = render label in red (destructive actions)*/
} item_t;

/* ── Menu definitions ────────────────────────────────────────────────── */

/* Desktop */
static const item_t MENU_DESKTOP[] = {
    { "  Refresh",            CMID_DESKTOP_REFRESH,         0, 0 },
    { "  Open terminal",      CMID_DESKTOP_TERMINAL,        0, 0 },
    { "  Create new",         CMID_NONE,                    1, 0 },
    { NULL, 0, 0, 0 },
    { "  Display settings",   CMID_DESKTOP_DISPLAY_SETTINGS,0, 0 },
    { "  System settings",    CMID_DESKTOP_SYSTEM_SETTINGS, 0, 0 },
    { NULL, 0, 0, 0 },
    { "  Change background",  CMID_DESKTOP_CHANGE_BG,       0, 0 },
    { "  About DracolaxOS",   CMID_DESKTOP_ABOUT,           0, 0 },
    { NULL, 0, 0, 0 },
    { "  Log out",            CMID_DESKTOP_LOGOUT,          0, 1 },
    { "  Restart",            CMID_DESKTOP_REBOOT,          0, 1 },
    { "  Shut down",          CMID_DESKTOP_SHUTDOWN,        0, 1 },
};
#define MENU_DESKTOP_COUNT  13

/* Desktop → Create new (submenu) */
static const item_t SUB_CREATE_NEW[] = {
    { "  Folder", CMID_DESKTOP_NEW_FOLDER, 0, 0 },
    { "  File",   CMID_DESKTOP_NEW_FILE,   0, 0 },
};
#define SUB_CREATE_NEW_COUNT 2

/* File */
static const item_t MENU_FILE[] = {
    { "  Open",       CMID_FILE_OPEN,       0, 0 },
    { "  Open with",  CMID_FILE_OPEN_WITH,  0, 0 },
    { NULL, 0, 0, 0 },
    { "  Cut",        CMID_FILE_CUT,        0, 0 },
    { "  Copy",       CMID_FILE_COPY,       0, 0 },
    { NULL, 0, 0, 0 },
    { "  Rename",     CMID_FILE_RENAME,     0, 0 },
    { "  Delete",     CMID_FILE_DELETE,     0, 1 },
    { NULL, 0, 0, 0 },
    { "  Properties", CMID_FILE_PROPERTIES, 0, 0 },
};
#define MENU_FILE_COUNT 10

/* Folder */
static const item_t MENU_FOLDER[] = {
    { "  Open",             CMID_FOLDER_OPEN,            0, 0 },
    { "  Open in new window",CMID_FOLDER_OPEN_NEW_WINDOW,0, 0 },
    { NULL, 0, 0, 0 },
    { "  Cut",              CMID_FOLDER_CUT,             0, 0 },
    { "  Copy",             CMID_FOLDER_COPY,            0, 0 },
    { NULL, 0, 0, 0 },
    { "  Rename",           CMID_FOLDER_RENAME,          0, 0 },
    { "  Delete",           CMID_FOLDER_DELETE,          0, 1 },
    { "  New inside",       CMID_NONE,                   1, 0 },
    { NULL, 0, 0, 0 },
    { "  Properties",       CMID_FOLDER_PROPERTIES,      0, 0 },
};
#define MENU_FOLDER_COUNT 11

/* Folder → New inside (submenu) */
static const item_t SUB_NEW_INSIDE[] = {
    { "  File",   CMID_FOLDER_NEW_FILE,   0, 0 },
    { "  Folder", CMID_FOLDER_NEW_FOLDER, 0, 0 },
};
#define SUB_NEW_INSIDE_COUNT 2

/* Window body */
static const item_t MENU_WIN_BODY[] = {
    { "  Minimize",     CMID_WIN_MINIMIZE,     0, 0 },
    { "  Maximize",     CMID_WIN_MAXIMIZE,     0, 0 },
    { "  Close",        CMID_WIN_CLOSE,        0, 1 },
    { NULL, 0, 0, 0 },
    { "  Always on top",CMID_WIN_ALWAYS_ON_TOP,0, 0 },
    { NULL, 0, 0, 0 },
    { "  Inspect",      CMID_WIN_INSPECT,      0, 0 },
};
#define MENU_WIN_BODY_COUNT 7

/* Title bar */
static const item_t MENU_TITLEBAR[] = {
    { "  Move window", CMID_TB_MOVE,     0, 0 },
    { NULL, 0, 0, 0 },
    { "  Minimize",    CMID_TB_MINIMIZE, 0, 0 },
    { "  Maximize",    CMID_TB_MAXIMIZE, 0, 0 },
    { "  Close",       CMID_TB_CLOSE,    0, 1 },
};
#define MENU_TITLEBAR_COUNT 5

/* Taskbar / Dock */
static const item_t MENU_DOCK[] = {
    { "  Open app",      CMID_DOCK_OPEN_APP,     0, 0 },
    { "  Pin / Unpin",   CMID_DOCK_PIN_UNPIN,    0, 0 },
    { "  Close window",  CMID_DOCK_CLOSE_WINDOW, 0, 1 },
    { NULL, 0, 0, 0 },
    { "  App settings",  CMID_DOCK_APP_SETTINGS, 0, 0 },
};
#define MENU_DOCK_COUNT 5

/* File-manager empty space */
static const item_t MENU_FM_EMPTY[] = {
    { "  Refresh",    CMID_FM_REFRESH,    0, 0 },
    { "  Sort by",    CMID_FM_SORT_BY,    0, 0 },
    { NULL, 0, 0, 0 },
    { "  New file",   CMID_FM_NEW_FILE,   0, 0 },
    { "  New folder", CMID_FM_NEW_FOLDER, 0, 0 },
    { NULL, 0, 0, 0 },
    { "  Paste",      CMID_FM_PASTE,      0, 0 },
};
#define MENU_FM_EMPTY_COUNT 7

/* ── State ───────────────────────────────────────────────────────────── */
static int           g_open      = 0;     /* is a menu active?           */
static ctx_hit_t     g_hit;               /* resolved hit from last open */
static const item_t *g_items     = 0;     /* active item array           */
static int           g_item_cnt  = 0;     /* length of g_items           */
static int           g_mx        = 0;     /* menu panel origin x         */
static int           g_my        = 0;     /* menu panel origin y         */
static int           g_mh        = 0;     /* menu panel total height     */

/* Submenu state */
static const item_t *g_sub_items    = 0;  /* NULL if no submenu active   */
static int           g_sub_cnt      = 0;
static int           g_sub_parent   = -1; /* index in g_items of parent  */
static int           g_sub_x        = 0;
static int           g_sub_y        = 0;
static int           g_sub_h        = 0;

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Compute panel height in pixels for a given item list. */
static int panel_height(const item_t *items, int count)
{
    int h = MENU_PAD_V * 2;
    for (int i = 0; i < count; i++)
        h += items[i].label ? ITEM_H : SEP_H;
    return h;
}

/* Draw a glassmorphism panel at (px, py, pw, ph). */
static void draw_panel(uint32_t px, uint32_t py, uint32_t pw, uint32_t ph)
{
    /* Inner fill */
    fb_rounded_rect(px + 1, py + 1, pw - 2, ph - 2,
                    CORNER_R > 2u ? CORNER_R - 2u : 1u, COL_GLASS_PANEL);
    /* Border */
    uint32_t ir = CORNER_R;
    /* top / bottom horizontal lines */
    fb_fill_rect(px + ir, py,      pw - 2 * ir, 1, COL_GLASS_EDGE);
    fb_fill_rect(px + ir, py + ph - 1, pw - 2 * ir, 1, COL_GLASS_EDGE);
    /* left / right vertical lines */
    fb_fill_rect(px,      py + ir, 1, ph - 2 * ir, COL_GLASS_EDGE);
    fb_fill_rect(px + pw - 1, py + ir, 1, ph - 2 * ir, COL_GLASS_EDGE);
    /* Approximate corners with single pixels */
    for (uint32_t i = 1; i <= ir; i++) {
        fb_put_pixel(px + ir - i,       py + ir - i,           COL_GLASS_EDGE);
        fb_put_pixel(px + pw - ir + i - 1, py + ir - i,        COL_GLASS_EDGE);
        fb_put_pixel(px + ir - i,       py + ph - ir + i - 1,  COL_GLASS_EDGE);
        fb_put_pixel(px + pw - ir + i - 1, py + ph - ir + i - 1, COL_GLASS_EDGE);
    }
    /* Top shine line */
    fb_fill_rect(px + ir, py,      pw - 2 * ir, 1, COL_GLASS_SHINE);
    /* Bottom shadow line */
    fb_fill_rect(px + ir, py + ph - 1, pw - 2 * ir, 1, COL_ACCENT_DIM);
}

/* Draw one menu panel (items+count) at screen (ox, oy).
 * hover_x/y is the cursor position for hit-testing highlights. */
static void draw_menu_panel(const item_t *items, int count,
                             int ox, int oy, int mh,
                             int hover_x, int hover_y)
{
    draw_panel((uint32_t)ox, (uint32_t)oy, MENU_W, (uint32_t)mh);

    int cy = oy + MENU_PAD_V;
    for (int i = 0; i < count; i++) {
        const item_t *it = &items[i];
        if (!it->label) {
            /* Separator */
            int sy = cy + SEP_H / 2;
            fb_fill_rect((uint32_t)(ox + 8), (uint32_t)sy,
                         (uint32_t)(MENU_W - 16), 1, COL_SEP);
            cy += SEP_H;
            continue;
        }

        /* Hover detection */
        int hover = (hover_x >= ox && hover_x < ox + MENU_W &&
                     hover_y >= cy && hover_y < cy + ITEM_H);
        if (hover) {
            fb_rounded_rect((uint32_t)(ox + 3), (uint32_t)cy,
                            (uint32_t)(MENU_W - 6), (uint32_t)ITEM_H,
                            4u, COL_ACCENT_DIM);
        }

        /* Text colour */
        uint32_t fg = hover         ? COL_TEXT_HI :
                      it->danger    ? COL_ERR      : COL_TEXT_MED;

        /* Vertically centre the 16px font inside ITEM_H */
        uint32_t ty = (uint32_t)(cy + (ITEM_H - FONT_H) / 2);
        fb_print((uint32_t)(ox + MENU_PAD_H), ty, it->label, fg, 0);

        /* Submenu arrow */
        if (it->has_sub) {
            fb_print((uint32_t)(ox + MENU_W - 14),
                     ty, "\x10", /* ASCII DLE used as ▶ placeholder */
                     hover ? COL_TEXT_HI : COL_TEXT_DIM, 0);
            /* Draw a simple ▶ from three dots since we have no glyph */
            uint32_t ax = (uint32_t)(ox + MENU_W - 14);
            uint32_t ay = ty + (uint32_t)(FONT_H / 2) - 3u;
            fb_fill_rect(ax,     ay,     2, 7, fg);
            fb_fill_rect(ax + 2, ay + 1, 2, 5, fg);
            fb_fill_rect(ax + 4, ay + 2, 2, 3, fg);
            fb_fill_rect(ax + 6, ay + 3, 1, 1, fg);
        }

        cy += ITEM_H;
    }
}

/* ── Sub-menu helpers ────────────────────────────────────────────────── */

/* Compute the y position of item[idx] within a panel at origin py. */
static int item_y(const item_t *items, int idx, int py)
{
    int cy = py + MENU_PAD_V;
    for (int i = 0; i < idx; i++)
        cy += items[i].label ? ITEM_H : SEP_H;
    return cy;
}

/* Open or update the active submenu for parent item at index pidx.
 * sub_x/sub_y is the position of the submenu panel. */
static void open_submenu(int pidx,
                         const item_t *sub, int sub_cnt,
                         int sx, int sy)
{
    g_sub_items  = sub;
    g_sub_cnt    = sub_cnt;
    g_sub_parent = pidx;
    g_sub_x      = sx;
    g_sub_y      = sy;
    g_sub_h      = panel_height(sub, sub_cnt);
}

/* Close the submenu without closing the parent menu. */
static void close_submenu(void)
{
    g_sub_items  = 0;
    g_sub_cnt    = 0;
    g_sub_parent = -1;
}

/* ── Action dispatch ─────────────────────────────────────────────────── */
static void dispatch(ctx_menu_id_t id, const ctx_hit_t *hit)
{
    kinfo("CTX_MENU: dispatch id=%d target=%d win=%d slot=%d\n",
          (int)id, (int)hit->type, hit->win_handle, hit->dock_slot);

    switch (id) {
    /* ── Desktop ─────────────────────────────────────────── */
    case CMID_DESKTOP_REFRESH:
        desktop_refresh();
        break;
    case CMID_DESKTOP_TERMINAL:
        appman_launch("Terminal");
        break;
    case CMID_DESKTOP_NEW_FOLDER:
        kinfo("CTX_MENU: new folder (stub)\n");
        break;
    case CMID_DESKTOP_NEW_FILE:
        kinfo("CTX_MENU: new file (stub)\n");
        break;
    case CMID_DESKTOP_DISPLAY_SETTINGS:
    case CMID_DESKTOP_SYSTEM_SETTINGS:
        appman_launch("Settings");
        break;
    case CMID_DESKTOP_CHANGE_BG:
        desktop_change_bg();
        break;
    case CMID_DESKTOP_ABOUT:
        desktop_about_open();
        break;
    case CMID_DESKTOP_LOGOUT:
        desktop_logout();
        break;
    case CMID_DESKTOP_REBOOT:
        power_reboot();
        break;
    case CMID_DESKTOP_SHUTDOWN:
        power_shutdown();
        break;

    /* ── File ────────────────────────────────────────────── */
    case CMID_FILE_OPEN:
    case CMID_FILE_OPEN_WITH:
        kinfo("CTX_MENU: file open '%s' (stub)\n", hit->item_name);
        break;
    case CMID_FILE_CUT:
    case CMID_FILE_COPY:
        kinfo("CTX_MENU: file cut/copy (stub)\n");
        break;
    case CMID_FILE_RENAME:
        kinfo("CTX_MENU: file rename (stub)\n");
        break;
    case CMID_FILE_DELETE:
        kinfo("CTX_MENU: file delete (stub)\n");
        break;
    case CMID_FILE_PROPERTIES:
        kinfo("CTX_MENU: file properties (stub)\n");
        break;

    /* ── Folder ──────────────────────────────────────────── */
    case CMID_FOLDER_OPEN:
    case CMID_FOLDER_OPEN_NEW_WINDOW:
        kinfo("CTX_MENU: folder open (stub)\n");
        break;
    case CMID_FOLDER_CUT:
    case CMID_FOLDER_COPY:
        kinfo("CTX_MENU: folder cut/copy (stub)\n");
        break;
    case CMID_FOLDER_RENAME:
        kinfo("CTX_MENU: folder rename (stub)\n");
        break;
    case CMID_FOLDER_DELETE:
        kinfo("CTX_MENU: folder delete (stub)\n");
        break;
    case CMID_FOLDER_NEW_FILE:
    case CMID_FOLDER_NEW_FOLDER:
        kinfo("CTX_MENU: folder new item (stub)\n");
        break;
    case CMID_FOLDER_PROPERTIES:
        kinfo("CTX_MENU: folder properties (stub)\n");
        break;

    /* ── Window body ──────────────────────────────────────── */
    case CMID_WIN_MINIMIZE:
        if (hit->win_handle >= 0)
            comp_set_visible(hit->win_handle, 0);
        break;
    case CMID_WIN_MAXIMIZE:
        if (hit->win_handle >= 0)
            comp_toggle_maximize(hit->win_handle);
        break;
    case CMID_WIN_CLOSE:
        if (hit->win_handle >= 0) {
            int tid = comp_get_task_id(hit->win_handle);
            dock_task_died(tid);
            comp_destroy_window(hit->win_handle);
        }
        break;
    case CMID_WIN_ALWAYS_ON_TOP:
        if (hit->win_handle >= 0)
            comp_toggle_always_on_top(hit->win_handle);
        break;
    case CMID_WIN_INSPECT:
        dbgcon_toggle();
        break;

    /* ── Title bar ────────────────────────────────────────── */
    case CMID_TB_MOVE:
        if (hit->win_handle >= 0)
            desktop_begin_drag(hit->win_handle);
        break;
    case CMID_TB_MINIMIZE:
        if (hit->win_handle >= 0)
            comp_set_visible(hit->win_handle, 0);
        break;
    case CMID_TB_MAXIMIZE:
        if (hit->win_handle >= 0)
            comp_toggle_maximize(hit->win_handle);
        break;
    case CMID_TB_CLOSE:
        if (hit->win_handle >= 0) {
            int tid = comp_get_task_id(hit->win_handle);
            dock_task_died(tid);
            comp_destroy_window(hit->win_handle);
        }
        break;

    /* ── Dock ─────────────────────────────────────────────── */
    case CMID_DOCK_OPEN_APP:
        if (hit->item_name[0])
            appman_launch(hit->item_name);
        break;
    case CMID_DOCK_PIN_UNPIN:
        if (hit->dock_slot >= 0)
            dock_pin_toggle(hit->dock_slot);
        break;
    case CMID_DOCK_CLOSE_WINDOW: {
        /* Destroy the window owned by the dock slot's task */
        if (hit->dock_slot >= 0) {
            int tid = dock_slot_task_id(hit->dock_slot);
            if (tid >= 0) {
                /* Find compositor window with this task_id and close it */
                int fw = comp_focused_window();
                if (fw >= 0 && comp_get_task_id(fw) == tid) {
                    comp_destroy_window(fw);
                    dock_task_died(tid);
                }
            }
        }
        break;
    }
    case CMID_DOCK_APP_SETTINGS:
        kinfo("CTX_MENU: app settings (stub)\n");
        break;

    /* ── File manager empty space ─────────────────────────── */
    case CMID_FM_REFRESH:
        desktop_refresh();
        break;
    case CMID_FM_SORT_BY:
        kinfo("CTX_MENU: fm sort by (stub)\n");
        break;
    case CMID_FM_NEW_FILE:
        kinfo("CTX_MENU: fm new file (stub)\n");
        break;
    case CMID_FM_NEW_FOLDER:
        kinfo("CTX_MENU: fm new folder (stub)\n");
        break;
    case CMID_FM_PASTE:
        kinfo("CTX_MENU: fm paste (stub)\n");
        break;

    default:
        break;
    }
}

/* ── Clamp menu to screen ────────────────────────────────────────────── */
static void clamp_menu(int *mx, int *my, int mw, int mh)
{
    int sw = (int)fb.width;
    int sh = (int)fb.height;
    if (*mx + mw > sw) *mx = sw - mw;
    if (*my + mh > sh) *my = sh - mh;
    if (*mx < 0) *mx = 0;
    if (*my < 0) *my = 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void ctx_menu_open(const ctx_hit_t *hit)
{
    if (!hit) return;

    /* Select item list based on target type. */
    const item_t *items = 0;
    int           count = 0;

    switch (hit->type) {
    case CTX_TARGET_DESKTOP:
        items = MENU_DESKTOP; count = MENU_DESKTOP_COUNT; break;
    case CTX_TARGET_FILE:
        items = MENU_FILE;    count = MENU_FILE_COUNT;    break;
    case CTX_TARGET_FOLDER:
        items = MENU_FOLDER;  count = MENU_FOLDER_COUNT;  break;
    case CTX_TARGET_WINDOW_BODY:
        items = MENU_WIN_BODY; count = MENU_WIN_BODY_COUNT; break;
    case CTX_TARGET_TITLEBAR:
        items = MENU_TITLEBAR; count = MENU_TITLEBAR_COUNT; break;
    case CTX_TARGET_TASKBAR_ITEM:
        items = MENU_DOCK;    count = MENU_DOCK_COUNT;    break;
    case CTX_TARGET_FILEMANAGER_EMPTY:
        items = MENU_FM_EMPTY; count = MENU_FM_EMPTY_COUNT; break;
    default:
        return; /* CTX_TARGET_NONE — nothing to show */
    }

    g_hit      = *hit;
    g_items    = items;
    g_item_cnt = count;
    g_mh       = panel_height(items, count);
    g_mx       = hit->x;
    g_my       = hit->y;
    clamp_menu(&g_mx, &g_my, MENU_W, g_mh);

    close_submenu();
    g_open = 1;

    kinfo("CTX_MENU: opened for target=%d at (%d,%d) items=%d\n",
          (int)hit->type, g_mx, g_my, count);
}

void ctx_menu_close(void)
{
    g_open = 0;
    close_submenu();
}

int ctx_menu_is_open(void)
{
    return g_open;
}

void ctx_menu_draw(int cursor_x, int cursor_y)
{
    if (!g_open) return;

    /* Draw main panel */
    draw_menu_panel(g_items, g_item_cnt,
                    g_mx, g_my, g_mh,
                    cursor_x, cursor_y);

    /* Auto-open submenu when cursor hovers a has_sub item */
    int cy = g_my + MENU_PAD_V;
    int hovered_sub_parent = -1;
    const item_t *hovered_sub_items = 0;
    int           hovered_sub_cnt   = 0;

    for (int i = 0; i < g_item_cnt; i++) {
        const item_t *it = &g_items[i];
        if (!it->label) { cy += SEP_H; continue; }

        if (it->has_sub) {
            int hover = (cursor_x >= g_mx && cursor_x < g_mx + MENU_W &&
                         cursor_y >= cy   && cursor_y < cy + ITEM_H);
            if (hover) {
                hovered_sub_parent = i;
                /* Choose submenu content by context type + item index */
                if (g_hit.type == CTX_TARGET_DESKTOP) {
                    hovered_sub_items = SUB_CREATE_NEW;
                    hovered_sub_cnt   = SUB_CREATE_NEW_COUNT;
                } else if (g_hit.type == CTX_TARGET_FOLDER) {
                    hovered_sub_items = SUB_NEW_INSIDE;
                    hovered_sub_cnt   = SUB_NEW_INSIDE_COUNT;
                }
            }
        }
        cy += ITEM_H;
    }

    if (hovered_sub_parent >= 0 && hovered_sub_items) {
        /* Position submenu to the right of the parent panel */
        int sub_ix = g_mx + MENU_W + 2;
        int sub_iy = item_y(g_items, hovered_sub_parent, g_my);
        int sub_h  = panel_height(hovered_sub_items, hovered_sub_cnt);
        /* If cursor is also inside the submenu, keep it showing */
        int sub_hover_in = (g_sub_items &&
                            cursor_x >= g_sub_x && cursor_x < g_sub_x + SUB_W &&
                            cursor_y >= g_sub_y && cursor_y < g_sub_y + g_sub_h);

        if (g_sub_parent != hovered_sub_parent || !sub_hover_in)
            open_submenu(hovered_sub_parent, hovered_sub_items, hovered_sub_cnt,
                         sub_ix, sub_iy);

        /* Clamp submenu */
        int csx = g_sub_x, csy = g_sub_y;
        clamp_menu(&csx, &csy, SUB_W, sub_h);

        draw_menu_panel(g_sub_items, g_sub_cnt,
                        csx, csy, g_sub_h,
                        cursor_x, cursor_y);
    } else if (g_sub_items) {
        /* Cursor left the parent item; keep showing if cursor is inside submenu */
        int sub_hover_in = (cursor_x >= g_sub_x && cursor_x < g_sub_x + SUB_W &&
                            cursor_y >= g_sub_y && cursor_y < g_sub_y + g_sub_h);
        if (sub_hover_in) {
            draw_menu_panel(g_sub_items, g_sub_cnt,
                            g_sub_x, g_sub_y, g_sub_h,
                            cursor_x, cursor_y);
        } else {
            close_submenu();
        }
    }
}

int ctx_menu_click(int px, int py)
{
    if (!g_open) return 0;

    /* ── Check submenu first ─────────────────────────────── */
    if (g_sub_items) {
        int cy = g_sub_y + MENU_PAD_V;
        for (int i = 0; i < g_sub_cnt; i++) {
            const item_t *it = &g_sub_items[i];
            if (!it->label) { cy += SEP_H; continue; }
            if (px >= g_sub_x && px < g_sub_x + SUB_W &&
                py >= cy       && py < cy + ITEM_H) {
                ctx_menu_id_t id = it->id;
                ctx_menu_close();
                if (id != CMID_NONE) dispatch(id, &g_hit);
                return 1;
            }
            cy += ITEM_H;
        }
        /* Click was outside the submenu — fall through to main menu check */
    }

    /* ── Check main menu ────────────────────────────────── */
    {
        int cy = g_my + MENU_PAD_V;
        for (int i = 0; i < g_item_cnt; i++) {
            const item_t *it = &g_items[i];
            if (!it->label) { cy += SEP_H; continue; }
            if (px >= g_mx && px < g_mx + MENU_W &&
                py >= cy   && py < cy + ITEM_H) {
                if (it->has_sub) {
                    /* Parent-only item (no direct action) — do nothing,
                     * the submenu is already shown on hover. */
                    return 1;
                }
                ctx_menu_id_t id = it->id;
                ctx_menu_close();
                if (id != CMID_NONE) dispatch(id, &g_hit);
                return 1;
            }
            cy += ITEM_H;
        }
    }

    /* Click outside all panels → caller should dismiss and re-route. */
    return 0;
}
