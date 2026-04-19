/* gui/desktop/default-desktop/ws_switcher.h
 * Tab-triggered workspace switcher overlay.
 *
 * Press Tab to open a centred panel showing all workspaces.
 * Press Tab again (or Escape) to close without switching.
 * Press 1-4 or click a tile to switch workspaces.
 */
#ifndef WS_SWITCHER_H
#define WS_SWITCHER_H

#include "../../../kernel/types.h"

#define WS_COUNT 4   /* number of workspaces */

/* Open / close the overlay */
void ws_switcher_open (void);
void ws_switcher_close(void);
int  ws_switcher_is_open(void);

/* Draw the overlay.  current_ws is highlighted. */
void ws_switcher_draw(int cursor_x, int cursor_y, int current_ws);

/* Handle a left click.  Returns 0-3 (new ws) or -1 (no change / outside). */
int  ws_switcher_click(int x, int y);

/* Handle a keypress while the switcher is open.
 * Returns 0-3 if a workspace was selected, -1 if no action. */
int  ws_switcher_key(int c, int current_ws);

#endif /* WS_SWITCHER_H */
