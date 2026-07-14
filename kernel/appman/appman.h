/* kernel/appman/appman.h — Application manager: register and launch GUI apps */
#ifndef APPMAN_H
#define APPMAN_H

#include "../types.h"

#define APP_MAX      16
#define APP_NAME_LEN 32

typedef void (*app_fn_t)(void);

typedef struct {
    char      name[APP_NAME_LEN];
    char      category[APP_NAME_LEN];
    app_fn_t  entry;
    uint8_t   active;
} app_entry_t;

void appman_register(const char *name, const char *category, app_fn_t fn);
int  appman_launch  (const char *name);
void appman_list    (char *buf, size_t sz);
void appman_init    (void);
int               appman_count(void);
const app_entry_t *appman_get (int idx);

/* App entry points are no longer declared here. Each app under apps/
 * declares its own app_*(void) entry point in its own <name>.h, included
 * directly by kernel/appman/appman.c's appman_init(). This keeps "what
 * apps exist" discoverable by looking at the apps/ directory rather than
 * a shared header everyone has to edit — see docs/CHANGELOG.md for the
 * GUI finalization pass that introduced this structure. */

#endif /* APPMAN_H */
