/* gui/apps/trash_manager.c — Trash / recycle bin */
#include "../../kernel/types.h"
#include "../../kernel/drivers/vga/fb.h"
#include "../../kernel/klibc.h"
#include "../../kernel/log.h"
#include "../../kernel/fs/vfs.h"
#include "../../kernel/fs/ramfs.h"
#include "trash_manager.h"

#define TRASH_MAX  16
static char trash_items[TRASH_MAX][64];
static int  trash_count = 0;
extern vfs_node_t *storage_root;

int trash_send(const char *path) {
    if (trash_count >= TRASH_MAX) return -1;
    strncpy(trash_items[trash_count++], path, 63);
    kinfo("TRASH: moved '%s' to trash\n", path);
    return 0;
}

void trash_empty(void) {
    for (int i = 0; i < trash_count; i++) {
        /* Best-effort: find and delete from storage */
        if (storage_root) {
            const char *name = strrchr(trash_items[i], '/');
            name = name ? name + 1 : trash_items[i];
            ramfs_delete(storage_root, name);
        }
        kinfo("TRASH: deleted '%s'\n", trash_items[i]);
    }
    trash_count = 0;
    kinfo("TRASH: emptied\n");
}

void trash_show_gui(void) {
    if (!fb.available) return;
    uint32_t W = fb.width, H = fb.height;
    uint32_t px = W/2-200, py = H/2-120, pw = 400, ph = 240;

    fb_fill_rect(px, py, pw, ph, 0x0D1117u);
    fb_fill_rect(px, py, pw, 24, 0x7828C8u);
    fb_print(px+8, py+4, "Trash", 0xF0F0FFu, 0x7828C8u);

    uint32_t y = py + 34;
    if (trash_count == 0) {
        fb_print(px+12, y, "Trash is empty.", 0x606070u, 0x0D1117u);
    } else {
        char buf[80];
        for (int i = 0; i < trash_count && i < 8; i++) {
            snprintf(buf, sizeof(buf), "  %s", trash_items[i]);
            fb_print(px+12, y, buf, 0xF0F0FFu, 0x0D1117u);
            y += 18;
        }
    }
    fb_print(px+12, py+ph-20, "[E=empty  Esc=close]", 0x606070u, 0x0D1117u);
    fb_flip();
}
