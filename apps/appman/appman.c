/* gui/apps/appman.c — Application manager
 *
 * FIX: strncpy() does not null-terminate when src >= dst capacity.
 *      Replaced with explicit null-termination after each strncpy.
 * FIX: appman_list() did not guard 'pos' before calling snprintf()
 *      when the buffer was nearly full; snprintf into sz-pos==0 is UB.
 *      Added explicit remaining-space check before each format call.
 */
#include "../../kernel/types.h"
#include "../../kernel/klibc.h"
#include "../../kernel/log.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/drivers/ps2/input_router.h"
#include "appman.h"

static app_entry_t apps[APP_MAX];
static int         naps = 0;

void appman_register(const char *name, const char *category, app_fn_t fn) {
    if (naps >= APP_MAX) return;
    /* FIX: guarantee null-termination regardless of name length */
    strncpy(apps[naps].name,     name,     APP_NAME_LEN - 1);
    apps[naps].name[APP_NAME_LEN - 1] = '\0';
    strncpy(apps[naps].category, category, APP_NAME_LEN - 1);
    apps[naps].category[APP_NAME_LEN - 1] = '\0';
    apps[naps].entry  = fn;
    apps[naps].active = 1;
    naps++;
}

int appman_launch(const char *name) {
    for (int i = 0; i < naps; i++) {
        if (strcmp(apps[i].name, name) == 0 && apps[i].active) {
            int id = sched_spawn(apps[i].entry, apps[i].name);
            if (id >= 0) {
                kinfo("APPMAN: launched '%s' (task %d)\n", name, id);
                /* Route keyboard to the newly spawned app */
                input_router_set_focus(id);
                return id;
            }
            kerror("APPMAN: sched_spawn failed for '%s'\n", name);
            return -1;
        }
    }
    kwarn("APPMAN: '%s' not found in registry\n", name);
    return -1;
}

void appman_list(char *buf, size_t sz) {
    if (!buf || sz == 0) return;
    buf[0] = '\0';
    size_t pos = 0;
    for (int i = 0; i < naps; i++) {
        /* FIX: guard remaining space before snprintf to avoid UB */
        if (pos + 4 >= sz) break;   /* 4 = minimum "x\n\0" + margin */
        int n = snprintf(buf + pos, sz - pos, "  %-20s [%s]\n",
                         apps[i].name, apps[i].category);
        if (n <= 0) break;
        pos += (size_t)n;
        if (pos >= sz - 1) { buf[sz - 1] = '\0'; break; }
    }
}

/* appman_count — return number of registered apps */
int appman_count(void) { return naps; }

/* appman_get — retrieve app entry by index (NULL if out of range) */
const app_entry_t *appman_get(int idx) {
    if (idx < 0 || idx >= naps) return NULL;
    return &apps[idx];
}

void appman_init(void) {
    appman_register("Terminal",        "System",      app_terminal);
    appman_register("Text Editor",     "Accessories", app_text_editor);
    appman_register("File Manager",    "System",      app_file_manager);
    appman_register("System Monitor",  "System",      app_system_monitor);
    appman_register("Calculator",      "Accessories", app_calculator);
    appman_register("Settings",        "System",      app_settings);
    appman_register("Package Manager", "System",      app_pkg_manager);
    appman_register("Draco Shield",    "System",      app_shield_ui);
    appman_register("Draco Manager",   "System",      app_draco_manager);
    appman_register("Login Manager",   "System",      app_login_manager);
    appman_register("Paint",           "Graphics",    app_paint);
    appman_register("Image Viewer",    "Graphics",    app_image_viewer);
    appman_register("Media Player",    "Multimedia",  app_media_player);
    kinfo("APPMAN: %d apps registered\n", naps);
}
