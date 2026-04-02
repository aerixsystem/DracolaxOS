# DXI Icon Format Specification

**Version:** 1.0  
**Magic:** `DRCO`  
**Use:** Native icon format for DracolaxOS. Stores BGRA 8888 pixel data with a fixed 16-byte header.

---

## Binary Layout

```
Offset  Size  Type      Field
------  ----  -------   -----
0       4     char[4]   magic      — must be "DRCO" (0x44 0x52 0x43 0x4F)
4       2     uint16_t  width      — image width in pixels
6       2     uint16_t  height     — image height in pixels
8       1     uint8_t   version    — format version (currently 1)
9       1     uint8_t   flags      — reserved, set to 0
10      2     uint16_t  reserved   — reserved, set to 0
12      4     uint32_t  data_size  — byte length of pixel data = width * height * 4
16      …     uint8_t[] pixels     — row-major BGRA 8888, top-left origin
```

Total header size: **16 bytes**.  
Pixel data immediately follows the header with no padding.

---

## Pixel Format

Each pixel is 4 bytes: **B G R A** (little-endian byte order).

| Byte offset within pixel | Channel | Range |
|--------------------------|---------|-------|
| 0 | Blue  | 0–255 |
| 1 | Green | 0–255 |
| 2 | Red   | 0–255 |
| 3 | Alpha | 0–255 (0 = transparent, 255 = opaque) |

Row-major order: pixel at (x, y) is at byte offset `16 + (y * width + x) * 4`.

---

## Creating .dxi Files

Use the host-side converter:

```bash
python3 tools/dxi-convert/img-to-dxi.py input.png output.dxi
```

The converter accepts any Pillow-readable image format and outputs a valid `.dxi` file.  
Images are converted to BGRA 8888 internally. Transparency is preserved.

---

## Loading in the Kernel

The atlas loader (`kernel/atlas.c`) reads `.dxi` files from RAMFS at boot and builds a sprite sheet used by the GUI. To add a new icon:

1. Convert source PNG → `.dxi` with `img-to-dxi.py`
2. Place the `.dxi` file in `storage/main/system/images/icons/`
3. Register it in the atlas XML (`storage/main/system/images/atlas/images.xml`)

---

## Validation

A valid `.dxi` file must satisfy:

- `magic[0..3] == "DRCO"`
- `version == 1`
- `width > 0 && height > 0`
- `data_size == width * height * 4`
- File size == `16 + data_size`

The atlas loader silently skips files that fail validation.

---

## Recommended Sizes

| Use case        | Size       |
|-----------------|------------|
| Dock button     | 48 × 48    |
| App grid icon   | 64 × 64    |
| File manager    | 32 × 32    |
| Taskbar icon    | 16 × 16    |

Standard BGRA encoding means no byte-swap is needed when blitting to the framebuffer (which also uses BGRA 8888 internally).
