#ifndef TRASH_MANAGER_H
#define TRASH_MANAGER_H
#include "../../kernel/types.h"
int  trash_send(const char *path);     /* move file to .Trash */
void trash_empty(void);
void trash_show_gui(void);
#endif
