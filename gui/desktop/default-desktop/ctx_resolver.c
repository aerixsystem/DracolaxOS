/* gui/desktop/default-desktop/ctx_resolver.c
 * DracolaxOS Context Menu System — Layer 2: Context Resolver
 * Dock removed in v3.0 — only windows and desktop are resolved.
 */
#include "ctx_resolver.h"
#include "../../compositor/compositor.h"
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

    /* Priority 1: Window title bar */
    {
        int tbar = comp_title_bar_at(x, y);
        if (tbar >= 0) {
            hit.type       = CTX_TARGET_TITLEBAR;
            hit.win_handle = tbar;
            return hit;
        }
    }

    /* Priority 2: Window body (client area) */
    {
        int wbody = comp_window_body_at(x, y);
        if (wbody >= 0) {
            hit.type       = CTX_TARGET_WINDOW_BODY;
            hit.win_handle = wbody;
            return hit;
        }
    }

    /* Priority 3: Desktop fallback */
    hit.type = CTX_TARGET_DESKTOP;
    return hit;
}
