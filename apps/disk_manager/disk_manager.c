/* gui/apps/disk_manager.c — Disk/mount info panel */
#include "../../kernel/types.h"
#include "../../kernel/drivers/vga/fb.h"
#include "../../kernel/klibc.h"
#include "../../kernel/log.h"
#include "../../kernel/mm/vmm.h"
#include "../../kernel/mm/pmm.h"
#include "disk_manager.h"

void disk_manager_show(void) {
    if (!fb.available) return;
    uint32_t W = fb.width, H = fb.height;
    uint32_t px = W/2 - 280, py = H/2 - 190;
    uint32_t pw = 560, ph = 380;

    /* Glass panel — unified palette */
    fb_fill_rect(px, py, pw, ph, 0x0F1020u);
    fb_fill_rect(px, py, pw, 26, 0x7828C8u);
    fb_fill_rect(px, py, pw, 1, 0x5A60C0u);   /* shine strip */
    fb_print(px+8, py+5, "Disk & Mount Manager", 0xF0F0FFu, 0x7828C8u);
    fb_fill_rect(px, py+ph-1, pw, 1, 0x3A3F7Au);
    fb_fill_rect(px, py, 1, ph, 0x3A3F7Au);
    fb_fill_rect(px+pw-1, py, 1, ph, 0x3A3F7Au);

    uint32_t y = py + 34;
    uint32_t x = px + 12;
    uint32_t col = 0xF0F0FFu;
    uint32_t dim = 0x7070A0u;
    uint32_t hi  = 0xA050F0u;
    uint32_t BG  = 0x0F1020u;

    /* ---- Mount table ---- */
    fb_print(x, y, "Filesystem Mounts:", hi, BG); y += 20;
    fb_fill_rect(x, y, pw-24, 1, 0x2A2C50u); y += 6;

    /* Column header */
    fb_print(x+4,   y, "Mount",      dim, BG);
    fb_print(x+120, y, "Type",       dim, BG);
    fb_print(x+210, y, "Size",       dim, BG);
    fb_print(x+290, y, "Used",       dim, BG);
    fb_print(x+360, y, "Flags",      dim, BG);
    y += 18;
    fb_fill_rect(x, y, pw-24, 1, 0x2A2C50u); y += 6;

    /* ramfs sizes: RAMFS_MAX_SIZE * node_count (approximate via heap) */
    uint32_t heap_kb  = (uint32_t)(vmm_heap_used() / 1024);
    uint32_t total_kb = (uint32_t)(pmm_total_bytes() / 1024);
    uint32_t free_kb  = (uint32_t)(pmm_free_pages() * 4);

    struct { const char *mnt; const char *type; const char *sz; const char *fl; } mounts[] = {
        { "/ramfs",   "ramfs",  "4096 KB", "rw,volatile" },
        { "/proc",    "procfs", "dynamic", "ro,virtual"  },
        { "/storage", "ramfs",  "4096 KB", "rw,volatile" },
    };
    for (int i = 0; i < 3; i++) {
        char used_s[16];
        snprintf(used_s, sizeof(used_s), "%u KB", heap_kb / 3);
        fb_print(x+4,   y, mounts[i].mnt,  col, BG);
        fb_print(x+120, y, mounts[i].type, dim, BG);
        fb_print(x+210, y, mounts[i].sz,   dim, BG);
        fb_print(x+290, y, used_s,          dim, BG);
        fb_print(x+360, y, mounts[i].fl,   0x60A060u, BG);
        y += 18;
    }
    y += 6;
    fb_fill_rect(x, y, pw-24, 1, 0x2A2C50u); y += 10;

    /* ---- Memory summary ---- */
    fb_print(x, y, "Memory:", hi, BG); y += 20;
    char buf[96];
    snprintf(buf, sizeof(buf), "  RAM total : %6u KB   free : %6u KB   used : %6u KB",
             total_kb, free_kb, total_kb - free_kb);
    fb_print(x+4, y, buf, dim, BG); y += 18;
    snprintf(buf, sizeof(buf), "  Heap used : %6u KB", heap_kb);
    fb_print(x+4, y, buf, dim, BG); y += 10;

    /* Memory bar */
    uint32_t bar_w_total = pw - 24;
    uint32_t bar_used  = total_kb ? bar_w_total * (total_kb - free_kb) / total_kb : 0;
    fb_fill_rect(x, y, bar_w_total, 10, 0x1A1A2Au);
    fb_fill_rect(x, y, bar_used,    10, 0x7828C8u);
    y += 18;
    fb_fill_rect(x, y, pw-24, 1, 0x2A2C50u); y += 10;

    /* ---- Block devices ---- */
    fb_print(x, y, "Block Devices:", hi, BG); y += 20;
    fb_print(x+4, y, "  No ATA/SATA/NVMe driver loaded.", dim, BG); y += 16;
    fb_print(x+4, y, "  Block device support: roadmap item.", 0x6060A0u, BG);

    fb_print(x, py+ph-18, "  ESC to close", 0x404060u, BG);
    fb_flip();
}
