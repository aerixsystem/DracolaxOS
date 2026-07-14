/* kernel/dxi/dxi.h — DXI icon format for DracolaxOS
 *
 * .dxi is a trivially-simple icon container designed for the kernel:
 *   - No external library required to decode
 *   - Pixel data is already in the framebuffer's native BGRA 8888 format
 *   - 16-byte header, zero allocation overhead at render time
 *
 * Format spec:
 *   Offset  Size  Field
 *      0     4    magic       "DRCO"
 *      4     2    width       pixels
 *      6     2    height      pixels
 *      8     1    bpp         always 32
 *      9     1    compression 0 = raw (RLE reserved for later)
 *     10     6    reserved    zero-padded
 *     16     …    pixel data  BGRA 8888, row-major, no padding between rows
 */
#ifndef DXI_H
#define DXI_H

#include "../types.h"

/* ── On-disk header (exactly 16 bytes, no padding needed) ─────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  magic[4];       /* "DRCO"                          */
    uint16_t width;          /* icon width  in pixels           */
    uint16_t height;         /* icon height in pixels           */
    uint8_t  bpp;            /* bits per pixel (32 only for v1) */
    uint8_t  compression;    /* 0 = raw, 1 = RLE (future)       */
    uint8_t  reserved[6];    /* zero, pads header to 16 bytes   */
} dxi_header_t;

#define DXI_MAGIC_0  'D'
#define DXI_MAGIC_1  'R'
#define DXI_MAGIC_2  'C'
#define DXI_MAGIC_3  'O'
#define DXI_HEADER_SIZE  16u
#define DXI_BPP_BGRA32   32u
#define DXI_COMP_RAW      0u

/* ── In-memory icon representation ───────────────────────────────────── */
#define DXI_MAX_ICONS    APP_MAX   /* one icon slot per registered app     */
#define DXI_MAX_PIXELS   (64u * 64u)  /* 64×64 px ceiling — 16 KB per icon */

typedef struct {
    uint16_t  width;
    uint16_t  height;
    uint32_t *pixels;   /* BGRA 8888, row-major; NULL if not loaded */
    int       loaded;
} dxi_icon_t;

/* ── API ──────────────────────────────────────────────────────────────── */

/* Load a .dxi file from VFS into a pre-allocated dxi_icon_t.
 *
 * path        — absolute VFS path (e.g. "/storage/main/system/shared/images/terminal.dxi")
 * icon        — caller-provided slot; pixels must point to a buffer of at
 *               least (width * height) uint32_t entries OR be NULL to have
 *               dxi_load allocate via kmalloc (caller must kfree later).
 *
 * Returns  0 on success,
 *         -1 on file-not-found / read error,
 *         -2 on bad magic,
 *         -3 on unsupported bpp or compression,
 *         -4 on image too large (> DXI_MAX_PIXELS).
 */
int dxi_load(const char *path, dxi_icon_t *icon);

/* Free pixel buffer allocated by dxi_load (if icon->pixels != NULL).
 * Safe to call even if the icon was never successfully loaded. */
void dxi_free(dxi_icon_t *icon);

/* Validate a raw header without loading pixel data.
 * Returns 0 if valid, negative error code otherwise. */
int dxi_validate_header(const dxi_header_t *hdr);

#endif /* DXI_H */
