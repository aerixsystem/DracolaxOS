#ifndef DESKTOP_H
#define DESKTOP_H
#include "../../../kernel/types.h"

/* Main compositor task — never returns. */
void desktop_task(void);

/* Blit raw wallpaper pixels into a shadow-buffer rect.
 * Used by dock and overlays to restore bg before blur/tint each frame. */
void desktop_blit_bg_at(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* BUG FIX (no real icons): hand-drawn vector icon (see draw_vector_icon()
 * in desktop.c) — real, rendered, distinct-per-app icons composed from
 * axis-aligned rect primitives, used wherever a bitmap/.dxi icon isn't
 * available (currently: always, since no app ships one). Exposed here so
 * dock.c's icon rendering can share the exact same icon set as desktop
 * icons and window titlebars instead of drawing its own separate
 * fallback. (x,y) is the icon's top-left, sz its width/height (square). */
void desktop_draw_vector_icon(const char *name, uint32_t x, uint32_t y,
                              uint32_t sz, uint32_t fg, uint32_t bg);

/* ── Desktop action callbacks ─────────────────────────────────────────
 * Called by the context-menu action dispatcher (ctx_menu.c).
 * Each function only flips internal desktop state — no rendering.
 * Rendering happens naturally in the next frame of desktop_task().
 */

/* Force a full wallpaper repaint on the next frame. */
void desktop_refresh(void);
void desktop_restore_windows(void); /* show all hidden windows on current workspace */

/* Cycle the background overlay opacity (dark → light → off → …). */
void desktop_change_bg(void);

/* Open the About DracolaxOS overlay. */
void desktop_about_open(void);

/* Log the current user out and return to the login screen. */
void desktop_logout(void);

/* Begin dragging the compositor window with the given handle.
 * The drag starts from the current cursor position recorded in the
 * desktop main loop — call only while a frame is being processed.
 * Safe to call from ctx_menu action dispatch (runs inside the loop). */
void desktop_begin_drag(int win_handle);

#endif
