/* gui/apps/appman.h — Application manager: launch/register GUI apps */
#ifndef APPMAN_H
#define APPMAN_H

#include "../../kernel/types.h"

#define APP_MAX     16
#define APP_NAME_LEN 32

typedef void (*app_fn_t)(void);

typedef struct {
    char      name[APP_NAME_LEN];
    char      category[APP_NAME_LEN]; /* "System", "Accessories", etc. */
    app_fn_t  entry;
    uint8_t   active;
} app_entry_t;

/* Register an app in the global app table */
void appman_register(const char *name, const char *category, app_fn_t fn);

/* Launch an app as a new scheduler task */
int  appman_launch(const char *name);

/* List all registered apps */
void appman_list(char *buf, size_t sz);

/* Initialise: registers all built-in apps */
void appman_init(void);

/* Count and indexed access — used by desktop start menu */
int               appman_count(void);
const app_entry_t *appman_get(int idx);

/* Built-in app entry points */
void app_terminal(void);

#endif /* APPMAN_H */
