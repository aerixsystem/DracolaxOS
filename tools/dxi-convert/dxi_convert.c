/* tools/dxi-convert/dxi_convert.c — Host-side PNG/JPG → .dxi converter
 *
 * Converts any image readable by stb_image (PNG, JPG, BMP, TGA, …)
 * into a DracolaxOS .dxi icon file.
 *
 * Build:
 *   gcc -O2 -o dxi_convert dxi_convert.c -lm
 *   (stb_image.h must be in the same directory or on the include path)
 *
 * Download stb_image.h (single-header, public domain):
 *   curl -O https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
 *
 * Usage:
 *   ./dxi_convert <input.png> <output.dxi> [width] [height]
 *
 *   width/height are optional. If omitted the image is not resized.
 *   If only one dimension is given the other is scaled proportionally.
 *
 * DXI spec:
 *   Offset  Size  Field
 *      0     4    magic       "DRCO"
 *      4     2    width       uint16_t LE
 *      6     2    height      uint16_t LE
 *      8     1    bpp         32
 *      9     1    compression 0 = raw
 *     10     6    reserved    zero
 *     16     …    pixels      BGRA 8888, row-major
 */

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_resize2.h"   /* optional — only used if resize is requested */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* ── DXI header (matches kernel/dxi/dxi.h exactly) ──────────────────── */
#pragma pack(push, 1)
typedef struct {
    uint8_t  magic[4];
    uint16_t width;
    uint16_t height;
    uint8_t  bpp;
    uint8_t  compression;
    uint8_t  reserved[6];
} dxi_header_t;
#pragma pack(pop)

/* ── Simple nearest-neighbour resize (no stb_image_resize dependency) ── */
static uint8_t *nn_resize(const uint8_t *src, int sw, int sh,
                           int dw, int dh) {
    uint8_t *dst = (uint8_t *)malloc((size_t)(dw * dh * 4));
    if (!dst) return NULL;
    for (int dy = 0; dy < dh; dy++) {
        int sy = dy * sh / dh;
        for (int dx = 0; dx < dw; dx++) {
            int sx = dx * sw / dw;
            const uint8_t *s = src + (sy * sw + sx) * 4;
            uint8_t       *d = dst + (dy * dw + dx) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
    return dst;
}

/* ── RGBA → BGRA in-place swap ────────────────────────────────────────── */
static void rgba_to_bgra(uint8_t *pixels, int w, int h) {
    int total = w * h;
    for (int i = 0; i < total; i++) {
        uint8_t *p = pixels + i * 4;
        uint8_t  r = p[0];
        p[0] = p[2];   /* R ↔ B */
        p[2] = r;
        /* p[1] = G (unchanged), p[3] = A (unchanged) */
    }
}

/* ── Write header ─────────────────────────────────────────────────────── */
static int write_header(FILE *fp, int w, int h) {
    dxi_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic[0] = 'D'; hdr.magic[1] = 'R';
    hdr.magic[2] = 'C'; hdr.magic[3] = 'O';
    hdr.width       = (uint16_t)w;
    hdr.height      = (uint16_t)h;
    hdr.bpp         = 32;
    hdr.compression = 0;
    return fwrite(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr) ? 0 : -1;
}

/* ── Entry point ─────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "Usage: %s <input> <output.dxi> [width] [height]\n"
            "  Supported inputs: PNG, JPG, BMP, TGA, GIF\n"
            "  width/height: optional target size (nearest-neighbour resize)\n"
            "  Recommended sizes: 48x48 (dock), 64x64 (start menu), 32x32 (taskbar)\n",
            argv[0]);
        return 1;
    }

    const char *infile  = argv[1];
    const char *outfile = argv[2];
    int target_w = (argc >= 4) ? atoi(argv[3]) : 0;
    int target_h = (argc >= 5) ? atoi(argv[4]) : 0;

    /* Load source image as RGBA */
    int src_w, src_h, channels;
    uint8_t *pixels = stbi_load(infile, &src_w, &src_h, &channels, 4);
    if (!pixels) {
        fprintf(stderr, "Error: cannot load '%s': %s\n", infile, stbi_failure_reason());
        return 1;
    }
    printf("Loaded: %s  (%dx%d, %d ch)\n", infile, src_w, src_h, channels);

    /* Resolve target dimensions */
    int out_w = src_w, out_h = src_h;
    if (target_w > 0 && target_h <= 0) {
        /* Width only — proportional height */
        out_w = target_w;
        out_h = src_h * target_w / src_w;
        if (out_h < 1) out_h = 1;
    } else if (target_h > 0 && target_w <= 0) {
        /* Height only — proportional width */
        out_h = target_h;
        out_w = src_w * target_h / src_h;
        if (out_w < 1) out_w = 1;
    } else if (target_w > 0 && target_h > 0) {
        out_w = target_w;
        out_h = target_h;
    }

    /* Clamp to sane values (DXI_MAX_PIXELS = 64x64 in kernel; host has no limit) */
    if (out_w > 1024 || out_h > 1024) {
        fprintf(stderr,
            "Warning: output size %dx%d exceeds 1024x1024 — clamping.\n",
            out_w, out_h);
        if (out_w > 1024) out_w = 1024;
        if (out_h > 1024) out_h = 1024;
    }

    /* Resize if needed */
    uint8_t *out_pixels = pixels;
    int      need_free  = 0;
    if (out_w != src_w || out_h != src_h) {
        printf("Resizing: %dx%d → %dx%d (nearest-neighbour)\n",
               src_w, src_h, out_w, out_h);
        out_pixels = nn_resize(pixels, src_w, src_h, out_w, out_h);
        if (!out_pixels) {
            fprintf(stderr, "Error: resize allocation failed\n");
            stbi_image_free(pixels);
            return 1;
        }
        need_free = 1;
    }

    /* Convert RGBA → BGRA in-place */
    rgba_to_bgra(out_pixels, out_w, out_h);

    /* Write .dxi */
    FILE *fp = fopen(outfile, "wb");
    if (!fp) {
        fprintf(stderr, "Error: cannot create '%s'\n", outfile);
        if (need_free) free(out_pixels);
        stbi_image_free(pixels);
        return 1;
    }

    if (write_header(fp, out_w, out_h) != 0) {
        fprintf(stderr, "Error: header write failed\n");
        fclose(fp);
        if (need_free) free(out_pixels);
        stbi_image_free(pixels);
        return 1;
    }

    size_t pixel_bytes = (size_t)(out_w * out_h * 4);
    if (fwrite(out_pixels, 1, pixel_bytes, fp) != pixel_bytes) {
        fprintf(stderr, "Error: pixel write failed\n");
        fclose(fp);
        if (need_free) free(out_pixels);
        stbi_image_free(pixels);
        return 1;
    }

    fclose(fp);
    if (need_free) free(out_pixels);
    stbi_image_free(pixels);

    printf("Written: %s  (%dx%d, %zu bytes)\n",
           outfile, out_w, out_h, sizeof(dxi_header_t) + pixel_bytes);
    return 0;
}
