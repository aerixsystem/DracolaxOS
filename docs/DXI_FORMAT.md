# DXI Icon Format Specification

**Version:** 1.0  
**Magic:** `DRCO`  
**Status:** Implemented (Phase 2)

---

## Binary Layout

```
Offset  Size  Type          Field
------  ----  ----------    -----
0       4     char[4]       magic        — "DRCO" (0x44 0x52 0x43 0x4F)
4       2     uint16_t LE   width        — image width in pixels
6       2     uint16_t LE   height       — image height in pixels
8       1     uint8_t       bpp          — bits per pixel (32 only in v1)
9       1     uint8_t       compression  — 0=raw, 1=RLE (reserved)
10      6     uint8_t[6]    reserved     — zero-padded to 16-byte boundary
16      …     uint32_t[]    pixels       — BGRA 8888, row-major
```

Total header: **16 bytes** (`__attribute__((packed))`).  
Pixel data immediately follows the header with no alignment gap.

---

## Pixel Format

Each pixel is one `uint32_t` in BGRA 8888 little-endian order:

```
Bits 31–24   A  alpha (0=transparent, 255=opaque)
Bits 23–16   R  red
Bits  15–8   G  green
Bits   7–0   B  blue
```

This matches the x86_64 framebuffer's native `0xAARRGGBB` layout directly —
no byte-swap is needed when blitting to the shadow buffer.

Pixel at column `x`, row `y`:  
`offset = 16 + (y * width + x) * 4`

---

## Kernel Header Struct

Defined in `kernel/dxi/dxi.h`:

```c
typedef struct __attribute__((packed)) {
    uint8_t  magic[4];     /* "DRCO"                         */
    uint16_t width;        /* icon width in pixels           */
    uint16_t height;       /* icon height in pixels          */
    uint8_t  bpp;          /* 32 only (BGRA 8888)            */
    uint8_t  compression;  /* 0 = raw, 1 = RLE (future)      */
    uint8_t  reserved[6];  /* zero padding to 16 bytes       */
} dxi_header_t;
```

---

## Kernel API

```c
/* kernel/dxi/dxi.h */

int  dxi_load(const char *path, dxi_icon_t *icon);
void dxi_free(dxi_icon_t *icon);
int  dxi_validate_header(const dxi_header_t *hdr);

typedef struct {
    uint16_t  width;
    uint16_t  height;
    uint32_t *pixels;   /* BGRA 8888, row-major; NULL if not loaded */
    int       loaded;
} dxi_icon_t;
```

### Return codes from `dxi_load()`

| Code | Meaning |
|------|---------|
|  0   | Success |
| -1   | File not found or read error |
| -2   | Bad magic or degenerate dimensions |
| -3   | Unsupported bpp or compression |
| -4   | Image exceeds `DXI_MAX_PIXELS` (64×64) |

### `icon->pixels` ownership

If `icon->pixels` is **NULL** when `dxi_load()` is called, it allocates via
`kmalloc()` — caller must call `dxi_free()` to release.

If `icon->pixels` is **pre-set** to a caller-owned buffer (e.g. the static
`g_icon_pixels` array in `desktop.c`), `dxi_load()` writes directly into it —
`dxi_free()` will not free it.

---

## Alpha Blending (Compositor)

`blit_icon_bgra()` in `gui/compositor/compositor.c`:

```c
void blit_icon_bgra(uint32_t dst_x, uint32_t dst_y,
                    const uint32_t *src, uint32_t src_w, uint32_t src_h,
                    uint32_t dst_stride);
```

Per-pixel blend formula (integer math, no floats):

```
C_out = (C_src × A + C_dst × (255 − A)) / 255
```

Applied per-channel (B, G, R). `dst_stride = fb.width`.

Fast paths:
- `A == 255` → direct write (no multiply)
- `A == 0`   → skip pixel entirely

---

## Host-Side Converter

`tools/dxi-convert/dxi_convert.c` — C tool using `stb_image.h`.

```bash
# Build (downloads stb_image.h automatically)
cd tools/dxi-convert
make

# Convert a PNG to .dxi at 48×48 (dock size)
./dxi_convert input.png output.dxi 48 48

# Convert at original size
./dxi_convert input.png output.dxi

# Proportional resize (width only)
./dxi_convert input.png output.dxi 64
```

The tool converts RGBA source pixels to BGRA before writing.

---

## Icon Naming Convention

The desktop icon cache maps registered app names to `.dxi` filenames:

- Spaces → hyphens
- Uppercase → lowercase
- `.dxi` suffix appended

| App name | Icon filename |
|----------|---------------|
| `Text Editor`   | `text-editor.dxi`   |
| `File Manager`  | `file-manager.dxi`  |
| `Terminal`      | `terminal.dxi`      |
| `System Monitor`| `system-monitor.dxi`|
| `Calculator`    | `calculator.dxi`    |
| `Settings`      | `settings.dxi`      |

Icons are loaded from: `/storage/main/system/shared/images/<name>.dxi`

---

## Recommended Sizes

| Use case           | Width × Height |
|--------------------|---------------|
| Start menu grid    | 48 × 48       |
| Dock button        | 48 × 48       |
| Taskbar (future)   | 16 × 16       |
| File manager       | 32 × 32       |

The kernel enforces `DXI_MAX_PIXELS = 64 × 64 = 4096 pixels` per icon.
Icons larger than this are rejected by `dxi_load()` with code `-4`.

---

## Adding Icons to the OS

1. Create a 48×48 PNG with transparency (RGBA).
2. Convert: `./dxi_convert icon.png terminal.dxi 48 48`
3. Copy the `.dxi` file to `storage/main/system/images/icons/` in the source
   tree (mounted by `init.c` at `/storage/main/system/shared/images/` at runtime).
4. Rebuild and boot — `icons_load_all()` in `desktop.c` will find and cache it.

For automation, `drx install <package>` will eventually copy `.dxi` files to
the live VFS path as part of the package install step.

---

## Validation Rules

A valid `.dxi` file must satisfy **all** of:

- `magic == "DRCO"`
- `bpp == 32`
- `compression == 0`
- `width > 0 && height > 0`
- `width * height ≤ DXI_MAX_PIXELS` (kernel side)
- `file_size == 16 + width * height * 4`
