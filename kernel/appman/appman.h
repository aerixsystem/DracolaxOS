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

/* Built-in GUI app entry points (implemented in apps.c) */
void app_terminal     (void);
void app_text_editor  (void);
void app_file_manager (void);
void app_system_monitor(void);
void app_calculator   (void);
void app_settings     (void);
void app_pkg_manager  (void);
void app_shield_ui    (void);
void app_draco_manager(void);
void app_login_manager(void);
void app_paint        (void);
void app_image_viewer (void);
void app_media_player (void);

#endif /* APPMAN_H */
