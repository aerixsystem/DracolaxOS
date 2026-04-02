#ifndef NOTIFICATION_DAEMON_H
#define NOTIFICATION_DAEMON_H
#include "../kernel/types.h"
typedef struct { char title[64]; char body[256]; int level; } notif_t;
void notif_push(const char *title, const char *body, int level);
int  notif_pop(notif_t *out);
void notification_daemon_task(void);
#endif
