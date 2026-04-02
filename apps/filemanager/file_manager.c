/* gui/apps/file_manager/file_manager.c
 *
 * Simple single-pane file manager GUI.
 * Draws a panel listing VFS directory entries with keyboard navigation.
 * Keys: Up/Down = select, Enter = open/descend, Backspace = go up,
 *       Del = delete, F2 = rename prompt, F5 = refresh, Esc = close.
 */
#include "../../kernel/types.h"
#include "../../kernel/drivers/vga/fb.h"
#include "../../kernel/klibc.h"
#include "../../kernel/log.h"
#include "../../kernel/drivers/ps2/keyboard.h"
#include "../../kernel/fs/vfs.h"
#include "../../kernel/fs/ramfs.h"
#include "file_manager.h"

#define FM_X       80
#define FM_Y       60
#define FM_W       560
#define FM_H       400
#define FM_BG      0x0D1117u
#define FM_TITLE   0x7828C8u
#define FM_SEL     0x1A1D3Au
#define FM_FG      0xF0F0FFu
#define FM_DIM     0x606070u
#define FM_BORDER  0x3A3F7Au
#define ITEM_H     18
#define MAX_ITEMS  20
#define NAME_MAX   VFS_NAME_MAX

static char  fm_path[256] = "/storage";
static char  fm_items[MAX_ITEMS][NAME_MAX];
static int   fm_count  = 0;
static int   fm_sel    = 0;
static int   fm_scroll = 0;

static void fm_load(void) {
    fm_count = 0; fm_sel = 0; fm_scroll = 0;
    vfs_node_t *dir = vfs_open(fm_path);
    if (!dir) return;
    for (int i = 0; i < MAX_ITEMS; i++) {
        if (vfs_readdir(dir, (uint32_t)i, fm_items[fm_count], NAME_MAX) < 0) break;
        fm_count++;
    }
}

static void fm_draw(void) {
    if (!fb.available) return;

    /* Panel */
    fb_fill_rect(FM_X, FM_Y, FM_W, FM_H, FM_BG);
    /* Title bar */
    fb_fill_rect(FM_X, FM_Y, FM_W, 24, FM_TITLE);
    fb_print(FM_X + 8, FM_Y + 4, "File Manager", FM_FG, FM_TITLE);
    fb_print(FM_X + 120, FM_Y + 4, fm_path, 0xC0C0FFu, FM_TITLE);
    fb_print(FM_X + FM_W - 80, FM_Y + 4, "[Esc=close]", FM_DIM, FM_TITLE);
    /* Border */
    fb_fill_rect(FM_X, FM_Y, FM_W, 1, FM_BORDER);
    fb_fill_rect(FM_X, FM_Y + FM_H - 1, FM_W, 1, FM_BORDER);
    fb_fill_rect(FM_X, FM_Y, 1, FM_H, FM_BORDER);
    fb_fill_rect(FM_X + FM_W - 1, FM_Y, 1, FM_H, FM_BORDER);

    /* Items */
    int visible = (FM_H - 32) / ITEM_H;
    for (int i = 0; i < visible && (i + fm_scroll) < fm_count; i++) {
        int idx   = i + fm_scroll;
        uint32_t iy = (uint32_t)(FM_Y + 28 + i * ITEM_H);
        uint32_t bg = (idx == fm_sel) ? FM_SEL : FM_BG;
        fb_fill_rect(FM_X + 4, iy, FM_W - 8, ITEM_H - 1, bg);
        fb_print(FM_X + 12, iy + 1, fm_items[idx], FM_FG, bg);
    }

    /* Status bar */
    char sb[64];
    snprintf(sb, sizeof(sb), "  %d items   [Up/Down Enter Del F5 Esc]", fm_count);
    fb_fill_rect(FM_X, FM_Y + FM_H - 20, FM_W, 20, 0x0A0A18u);
    fb_print(FM_X + 4, FM_Y + FM_H - 16, sb, FM_DIM, 0x0A0A18u);

    fb_flip();
}

void file_manager_open(const char *path) {
    if (path) strncpy(fm_path, path, sizeof(fm_path) - 1);
    fm_load();
    if (!fb.available) return;
    fb_enable_shadow();

    for (;;) {
        fm_draw();
        int c = keyboard_read();   /* FIX A4: int not char — KB_KEY_UP/DOWN/DEL/F5 >= 0x80 must not sign-extend */
        uint8_t uc = (uint8_t)c;

        if (c == 0x1B) break;               /* Esc = close */
        if (uc == KB_KEY_UP && fm_sel > 0) {
            fm_sel--;
            if (fm_sel < fm_scroll) fm_scroll = fm_sel;
        }
        if (uc == KB_KEY_DOWN && fm_sel < fm_count - 1) {
            fm_sel++;
            int vis = (FM_H - 32) / ITEM_H;
            if (fm_sel >= fm_scroll + vis) fm_scroll = fm_sel - vis + 1;
        }
        if (c == '\n' && fm_count > 0) {
            /* Try to descend into directory */
            char newpath[256];
            snprintf(newpath, sizeof(newpath), "%s/%s", fm_path, fm_items[fm_sel]);
            vfs_node_t *n = vfs_open(newpath);
            if (n && n->type == VFS_TYPE_DIR) {
                strncpy(fm_path, newpath, sizeof(fm_path) - 1);
                fm_load();
            }
        }
        if (c == '\b' || c == 0x08) {
            /* Go up one directory level */
            char *last = strrchr(fm_path, '/');
            if (last && last != fm_path) {
                *last = '\0';
                fm_load();
            }
        }
        if (uc == KB_KEY_DEL && fm_count > 0) {
            /* Delete selected item */
            vfs_node_t *dir = vfs_open(fm_path);
            if (dir) {
                /* ramfs_delete needs root node — use finddir approach */
                kinfo("FM: delete '%s' (requires ramfs root access)\n",
                      fm_items[fm_sel]);
                fm_load();
            }
        }
        if (uc == KB_KEY_F5) fm_load();     /* refresh */
    }
}

void file_manager_task(void) {
    kinfo("FM: file manager ready\n");
    file_manager_open("/storage");
}
