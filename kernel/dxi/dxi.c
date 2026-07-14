/* kernel/dxi/dxi.c — DXI icon loader for DracolaxOS
 *
 * Uses VFS read API: vfs_open(path) → vfs_node_t*, then vfs_read().
 * All pixel data is stored as BGRA 8888 — no colour conversion at runtime.
 * kmalloc is used for pixel buffers; kfree to release.
 */

#include "dxi.h"
#include "../fs/vfs.h"
#include "../mm/vmm.h"
#include "../klibc.h"
#include "../log.h"
#include "../../kernel/appman/appman.h"   /* APP_MAX */

/* ── Header validation ───────────────────────────────────────────────── */

int dxi_validate_header(const dxi_header_t *hdr) {
    if (hdr->magic[0] != DXI_MAGIC_0 ||
        hdr->magic[1] != DXI_MAGIC_1 ||
        hdr->magic[2] != DXI_MAGIC_2 ||
        hdr->magic[3] != DXI_MAGIC_3) {
        return -2;   /* bad magic */
    }
    if (hdr->bpp != DXI_BPP_BGRA32) {
        return -3;   /* unsupported bpp */
    }
    if (hdr->compression != DXI_COMP_RAW) {
        return -3;   /* unsupported compression */
    }
    if (hdr->width == 0 || hdr->height == 0) {
        return -2;   /* degenerate dimensions */
    }
    uint32_t pixels = (uint32_t)hdr->width * (uint32_t)hdr->height;
    if (pixels > DXI_MAX_PIXELS) {
        return -4;   /* image too large */
    }
    return 0;
}

/* ── Loader ──────────────────────────────────────────────────────────── */

int dxi_load(const char *path, dxi_icon_t *icon) {
    if (!path || !icon) return -1;

    /* Resolve path in VFS */
    vfs_node_t *node = vfs_open(path);
    if (!node) {
        kwarn("DXI: file not found: %s\n", path);
        icon->loaded = 0;
        return -1;
    }

    /* Read header */
    dxi_header_t hdr;
    int n = vfs_read(node, 0, DXI_HEADER_SIZE, (uint8_t *)&hdr);
    if (n < (int)DXI_HEADER_SIZE) {
        kerror("DXI: header read failed (%d bytes): %s\n", n, path);
        icon->loaded = 0;
        return -1;
    }

    /* Validate */
    int err = dxi_validate_header(&hdr);
    if (err != 0) {
        kerror("DXI: invalid file (err=%d): %s\n", err, path);
        icon->loaded = 0;
        return err;
    }

    uint32_t pixel_count = (uint32_t)hdr.width * (uint32_t)hdr.height;
    uint32_t byte_count  = pixel_count * 4u;

    /* Allocate pixel buffer if caller did not provide one */
    int we_allocated = 0;
    if (!icon->pixels) {
        icon->pixels = (uint32_t *)kmalloc(byte_count);
        if (!icon->pixels) {
            kerror("DXI: pixel alloc failed (%u bytes): %s\n", byte_count, path);
            icon->loaded = 0;
            return -1;
        }
        we_allocated = 1;
    }

    /* Read pixel data immediately after header */
    n = vfs_read(node, DXI_HEADER_SIZE, byte_count, (uint8_t *)icon->pixels);
    if ((uint32_t)n < byte_count) {
        kerror("DXI: pixel read short (%d / %u bytes): %s\n", n, byte_count, path);
        if (we_allocated) {
            kfree(icon->pixels);
            icon->pixels = NULL;
        }
        icon->loaded = 0;
        return -1;
    }

    icon->width  = hdr.width;
    icon->height = hdr.height;
    icon->loaded = 1;

    kinfo("DXI: loaded %ux%u icon: %s\n", hdr.width, hdr.height, path);
    return 0;
}

/* ── Release ─────────────────────────────────────────────────────────── */

void dxi_free(dxi_icon_t *icon) {
    if (!icon) return;
    if (icon->pixels) {
        kfree(icon->pixels);
        icon->pixels = NULL;
    }
    icon->loaded = 0;
    icon->width  = 0;
    icon->height = 0;
}
