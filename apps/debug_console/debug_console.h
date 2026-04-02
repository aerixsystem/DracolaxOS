#ifndef DEBUG_CONSOLE_H
#define DEBUG_CONSOLE_H
void dbgcon_toggle(void);
void dbgcon_push(const char *line);
void dbgcon_draw(void);
int  dbgcon_is_visible(void);
#endif
