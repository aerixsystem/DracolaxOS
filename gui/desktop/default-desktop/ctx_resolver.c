/* gui/desktop/default-desktop/ctx_resolver.c
 * DracolaxOS Context Menu System — Layer 2: Context Resolver
 *
 * Pure classification logic.  No drawing.  No side effects.
 */
#include "ctx_resolver.h"
#include "../../compositor/compositor.h"
#include "dock.h"
#include "../../../kernel/klibc.h"

ctx_hit_t ctx_resolve(int x, int y)
{
    ctx_hit_t hit;
    memset(&hit, 0, sizeof(hit));
    hit.type       = CTX_TARGET_NONE;
    hit.win_handle = -1;
    hit.dock_slot  = -1;
    hit.x          = x;
    hit.y          = y;
    hit.item_name[0] = '\0';

    /* ── Priority 1: Dock slot ─────────────────────────────────────────── */
    {
        /* First try the cached hover index (set each draw frame). */
        int slot = dock_hover_slot();

        /* If not set (stale frame), compute from panel geometry directly. */
        if (slot < 0) {
            int px = dock_panel_x(), py = dock_panel_y();
            int pw = dock_panel_w(), ph = dock_panel_h();
            if (pw > 0 && ph > 0 &&
                x >= px && x < px + pw &&
                y >= py && y < py + ph) {
                /* Walk visible slots to find which one y falls in */
                int icon_sz  = 40;   /* DOCK_ICON_SZ */
                int icon_gap =  6;   /* DOCK_ICON_GAP */
                int pad_top  = 12;   /* DOCK_PAD_TOP  */
                int max_vis  = 10;   /* DOCK_MAX_VISIBLE */
                int cy_base  = py + pad_top + icon_sz / 2;
                int step     = icon_sz + icon_gap;
                int half     = icon_sz / 2 + 5;
                int scroll   = 0;    /* can't read g_scroll_off here; 0 is safe */
                for (int vi = 0; vi < max_vis; vi++) {
                    int cy_icon = cy_base + vi * step;
                    if (y >= cy_icon - half && y < cy_icon + half) {
                        slot = vi + scroll;
                        break;
                    }
                }
            }
        }

        if (slot >= 0) {
            hit.type      = CTX_TARGET_TASKBAR_ITEM;
            hit.dock_slot = slot;
            /* Copy the app name from the dock slot. */
            const char *nm = dock_slot_name(slot);
            if (nm && nm[0]) {
                size_t len = strlen(nm);
                if (len >= sizeof(hit.item_name)) len = sizeof(hit.item_name) - 1;
                memcpy(hit.item_name, nm, len);
                hit.item_name[len] = '\0';
            }
            return hit;
        }
    }

    /* ── Priority 2: Window title bar ─────────────────────────────────── */
    {
        int tbar = comp_title_bar_at(x, y);
        if (tbar >= 0) {
            hit.type       = CTX_TARGET_TITLEBAR;
            hit.win_handle = tbar;
            return hit;
        }
    }

    /* ── Priority 3: Window body (client area) ─────────────────────────── */
    {
        int wbody = comp_window_body_at(x, y);
        if (wbody >= 0) {
            hit.type       = CTX_TARGET_WINDOW_BODY;
            hit.win_handle = wbody;
            return hit;
        }
    }

    /* ── Priority 4: Desktop fallback ──────────────────────────────────── */
    hit.type = CTX_TARGET_DESKTOP;
    return hit;
}
