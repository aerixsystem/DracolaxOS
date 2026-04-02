/* kernel/fb.c — VESA linear framebuffer driver
 *
 * Initialised from the multiboot2 framebuffer tag.
 * Provides pixel-level drawing primitives used by the compositor.
 *
 * Multiboot2 tag type 8 = framebuffer info:
 *   offset  0  uint32  type  = 8
 *   offset  4  uint32  size
 *   offset  8  uint64  framebuffer_addr
 *   offset 16  uint32  framebuffer_pitch
 *   offset 20  uint32  framebuffer_width
 *   offset 24  uint32  framebuffer_height
 *   offset 28  uint8   framebuffer_bpp
 *   offset 29  uint8   framebuffer_type
 */
#include "../../types.h"
#include "fb.h"
#include "../../log.h"
#include "../../klibc.h"
#include "../../mm/vmm.h"

fb_info_t fb = { .available = 0 };

/* ---- Shadow / double-buffer -------------------------------------------- */
static uint32_t *fb_shadow     = NULL;  /* allocated in fb_init */
static int       fb_use_shadow = 0;

void fb_enable_shadow(void)  { fb_use_shadow = 1; }
void fb_disable_shadow(void) { fb_use_shadow = 0; }
uint32_t *fb_shadow_ptr(void) { return fb_shadow; }

void fb_flip(void) {
    if (!fb.available || !fb_shadow) return;
    memcpy((void *)(uintptr_t)fb.addr, fb_shadow, fb.height * fb.pitch);
}

uint32_t fb_read_pixel(uint32_t x, uint32_t y) {
    if (!fb.available || x >= fb.width || y >= fb.height) return 0;
    if (fb_shadow)
        return fb_shadow[y * (fb.pitch >> 2) + x];
    uint8_t *p = (uint8_t *)(uintptr_t)fb.addr + y * fb.pitch + x * 4;
    return *(uint32_t *)p;
}

/* ---- Embedded 8x16 VGA bitmap font ------------------------------------- */
/* Standard IBM VGA BIOS 8x16 font — first 128 ASCII chars.
 * Each char is 16 bytes (one byte per row, MSB = leftmost pixel).
 * This data is the canonical CP437/VGA ROM font, public domain.
 */
const uint8_t vga_font_8x16[128][16] = {
    /* 0x00 */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x01 */ {0,0,0x7e,0xc3,0x99,0xbd,0xbd,0x99,0xc3,0x7e,0,0,0,0,0,0},
    /* 0x02 */ {0,0,0x7e,0xff,0xdb,0x81,0x81,0xdb,0xff,0x7e,0,0,0,0,0,0},
    /* 0x03 */ {0,0,0,0x6c,0xfe,0xfe,0x7c,0x38,0x10,0,0,0,0,0,0,0},
    /* 0x04 */ {0,0,0,0x10,0x38,0x7c,0xfe,0x7c,0x38,0x10,0,0,0,0,0,0},
    /* 0x05 */ {0,0,0x18,0x3c,0x3c,0xe7,0xe7,0xe7,0x18,0x3c,0,0,0,0,0,0},
    /* 0x06 */ {0,0,0,0x18,0x3c,0x7e,0xff,0x7e,0x18,0x3c,0,0,0,0,0,0},
    /* 0x07 */ {0,0,0,0,0,0x18,0x3c,0x18,0,0,0,0,0,0,0,0},
    /* 0x08 */ {0xff,0xff,0xff,0xff,0xff,0xe7,0xc3,0xe7,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
    /* 0x09 */ {0,0,0,0x3c,0x66,0x42,0x42,0x66,0x3c,0,0,0,0,0,0,0},
    /* 0x0A */ {0xff,0xff,0xff,0xc3,0x99,0xbd,0xbd,0x99,0xc3,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
    /* 0x0B */ {0,0,0x1e,0x0e,0x1a,0x32,0x78,0xcc,0xcc,0x78,0,0,0,0,0,0},
    /* 0x0C */ {0,0,0x3c,0x66,0x66,0x66,0x3c,0x18,0x7e,0x18,0,0,0,0,0,0},
    /* 0x0D */ {0,0,0x3f,0x33,0x3f,0x30,0x30,0x70,0xf0,0xe0,0,0,0,0,0,0},
    /* 0x0E */ {0,0,0x7f,0x63,0x7f,0x63,0x63,0x67,0xe6,0xc0,0,0,0,0,0,0},
    /* 0x0F */ {0,0,0,0x18,0xdb,0x3c,0xe7,0x3c,0xdb,0x18,0,0,0,0,0,0},
    /* 0x10..0x1F: control chars, leave blank */
    {0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},
    /* 0x20 space */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* ! */ {0,0,0x18,0x3c,0x3c,0x18,0x18,0,0x18,0,0,0,0,0,0,0},
    /* " */ {0,0x66,0x66,0x24,0,0,0,0,0,0,0,0,0,0,0,0},
    /* # */ {0,0,0x6c,0x6c,0xfe,0x6c,0xfe,0x6c,0x6c,0,0,0,0,0,0,0},
    /* $ */ {0x18,0x18,0x7c,0xc0,0x70,0x1c,0xfe,0x18,0x18,0,0,0,0,0,0,0},
    /* % */ {0,0x60,0xc4,0x6c,0x18,0x36,0x4c,0x06,0,0,0,0,0,0,0,0},
    /* & */ {0,0,0x38,0x6c,0x6c,0x38,0x76,0xdc,0xcc,0x76,0,0,0,0,0,0},
    /* ' */ {0,0x18,0x18,0x10,0,0,0,0,0,0,0,0,0,0,0,0},
    /* ( */ {0,0,0x0e,0x1c,0x38,0x38,0x38,0x1c,0x0e,0,0,0,0,0,0,0},
    /* ) */ {0,0,0x70,0x38,0x1c,0x1c,0x1c,0x38,0x70,0,0,0,0,0,0,0},
    /* * */ {0,0,0,0x6c,0x38,0xfe,0x38,0x6c,0,0,0,0,0,0,0,0},
    /* + */ {0,0,0,0x18,0x18,0x7e,0x18,0x18,0,0,0,0,0,0,0,0},
    /* , */ {0,0,0,0,0,0,0,0x18,0x18,0x30,0,0,0,0,0,0},
    /* - */ {0,0,0,0,0,0x7e,0,0,0,0,0,0,0,0,0,0},
    /* . */ {0,0,0,0,0,0,0,0,0x18,0,0,0,0,0,0,0},
    /* / */ {0,0,0x06,0x06,0x0c,0x18,0x30,0x60,0x60,0,0,0,0,0,0,0},
    /* 0 */ {0,0,0x7c,0xce,0xde,0xf6,0xe6,0x7c,0,0,0,0,0,0,0,0},
    /* 1 */ {0,0,0x18,0x38,0x18,0x18,0x18,0x7e,0,0,0,0,0,0,0,0},
    /* 2 */ {0,0,0x7c,0xc6,0x06,0x3c,0x60,0xfe,0,0,0,0,0,0,0,0},
    /* 3 */ {0,0,0xfc,0x06,0x7c,0x06,0x06,0xfc,0,0,0,0,0,0,0,0},
    /* 4 */ {0,0,0x0e,0x1e,0x36,0x66,0xff,0x06,0,0,0,0,0,0,0,0},
    /* 5 */ {0,0,0xfe,0xc0,0xfc,0x06,0xc6,0x7c,0,0,0,0,0,0,0,0},
    /* 6 */ {0,0,0x3c,0x60,0xfc,0xc6,0xc6,0x7c,0,0,0,0,0,0,0,0},
    /* 7 */ {0,0,0xff,0x06,0x0c,0x18,0x30,0x30,0,0,0,0,0,0,0,0},
    /* 8 */ {0,0,0x7c,0xc6,0x7c,0xc6,0xc6,0x7c,0,0,0,0,0,0,0,0},
    /* 9 */ {0,0,0x7c,0xc6,0xc6,0x7e,0x06,0x7c,0,0,0,0,0,0,0,0},
    /* : */ {0,0,0,0,0x18,0,0,0x18,0,0,0,0,0,0,0,0},
    /* ; */ {0,0,0,0,0x18,0,0,0x18,0x18,0x30,0,0,0,0,0,0},
    /* < */ {0,0,0,0x0e,0x1c,0x38,0x1c,0x0e,0,0,0,0,0,0,0,0},
    /* = */ {0,0,0,0,0x7e,0,0x7e,0,0,0,0,0,0,0,0,0},
    /* > */ {0,0,0,0x70,0x38,0x1c,0x38,0x70,0,0,0,0,0,0,0,0},
    /* ? */ {0,0,0x7c,0xc6,0x0c,0x18,0,0x18,0,0,0,0,0,0,0,0},
    /* @ */ {0,0,0x7c,0xc2,0x9e,0xa6,0x9e,0x80,0x7c,0,0,0,0,0,0,0},
    /* A */ {0,0,0x38,0x6c,0xc6,0xfe,0xc6,0xc6,0,0,0,0,0,0,0,0},
    /* B */ {0,0,0xfc,0xc6,0xfc,0xc6,0xc6,0xfc,0,0,0,0,0,0,0,0},
    /* C */ {0,0,0x7c,0xc6,0xc0,0xc0,0xc6,0x7c,0,0,0,0,0,0,0,0},
    /* D */ {0,0,0xf8,0xcc,0xc6,0xc6,0xcc,0xf8,0,0,0,0,0,0,0,0},
    /* E */ {0,0,0xfe,0xc0,0xfc,0xc0,0xc0,0xfe,0,0,0,0,0,0,0,0},
    /* F */ {0,0,0xfe,0xc0,0xfc,0xc0,0xc0,0xc0,0,0,0,0,0,0,0,0},
    /* G */ {0,0,0x7c,0xc6,0xc0,0xce,0xc6,0x7e,0,0,0,0,0,0,0,0},
    /* H */ {0,0,0xc6,0xc6,0xfe,0xc6,0xc6,0xc6,0,0,0,0,0,0,0,0},
    /* I */ {0,0,0x7e,0x18,0x18,0x18,0x18,0x7e,0,0,0,0,0,0,0,0},
    /* J */ {0,0,0x1e,0x06,0x06,0x06,0xc6,0x7c,0,0,0,0,0,0,0,0},
    /* K */ {0,0,0xc6,0xcc,0xf8,0xcc,0xc6,0xc6,0,0,0,0,0,0,0,0},
    /* L */ {0,0,0xc0,0xc0,0xc0,0xc0,0xc0,0xfe,0,0,0,0,0,0,0,0},
    /* M */ {0,0,0xc6,0xee,0xfe,0xd6,0xc6,0xc6,0,0,0,0,0,0,0,0},
    /* N */ {0,0,0xc6,0xe6,0xf6,0xde,0xce,0xc6,0,0,0,0,0,0,0,0},
    /* O */ {0,0,0x7c,0xc6,0xc6,0xc6,0xc6,0x7c,0,0,0,0,0,0,0,0},
    /* P */ {0,0,0xfc,0xc6,0xfc,0xc0,0xc0,0xc0,0,0,0,0,0,0,0,0},
    /* Q */ {0,0,0x7c,0xc6,0xc6,0xce,0xde,0x7c,0x06,0,0,0,0,0,0,0},
    /* R */ {0,0,0xfc,0xc6,0xfc,0xd8,0xcc,0xc6,0,0,0,0,0,0,0,0},
    /* S */ {0,0,0x7c,0xc0,0x7c,0x06,0x06,0xfc,0,0,0,0,0,0,0,0},
    /* T */ {0,0,0x7e,0x18,0x18,0x18,0x18,0x18,0,0,0,0,0,0,0,0},
    /* U */ {0,0,0xc6,0xc6,0xc6,0xc6,0xc6,0x7c,0,0,0,0,0,0,0,0},
    /* V */ {0,0,0xc6,0xc6,0xc6,0x6c,0x38,0x10,0,0,0,0,0,0,0,0},
    /* W */ {0,0,0xc6,0xc6,0xd6,0xfe,0xee,0xc6,0,0,0,0,0,0,0,0},
    /* X */ {0,0,0xc6,0x6c,0x38,0x38,0x6c,0xc6,0,0,0,0,0,0,0,0},
    /* Y */ {0,0,0x66,0x66,0x3c,0x18,0x18,0x18,0,0,0,0,0,0,0,0},
    /* Z */ {0,0,0xfe,0x0c,0x18,0x30,0x60,0xfe,0,0,0,0,0,0,0,0},
    /* [ */ {0,0,0x3c,0x30,0x30,0x30,0x30,0x3c,0,0,0,0,0,0,0,0},
    /* \ */ {0,0,0x60,0x60,0x30,0x18,0x0c,0x06,0,0,0,0,0,0,0,0},
    /* ] */ {0,0,0x3c,0x0c,0x0c,0x0c,0x0c,0x3c,0,0,0,0,0,0,0,0},
    /* ^ */ {0,0x18,0x3c,0x66,0,0,0,0,0,0,0,0,0,0,0,0},
    /* _ */ {0,0,0,0,0,0,0,0,0,0xff,0,0,0,0,0,0},
    /* ` */ {0,0x18,0x18,0x0c,0,0,0,0,0,0,0,0,0,0,0,0},
    /* a */ {0,0,0,0x7c,0x06,0x7e,0xc6,0x7e,0,0,0,0,0,0,0,0},
    /* b */ {0,0,0xc0,0xfc,0xc6,0xc6,0xc6,0xfc,0,0,0,0,0,0,0,0},
    /* c */ {0,0,0,0x7c,0xc0,0xc0,0xc0,0x7c,0,0,0,0,0,0,0,0},
    /* d */ {0,0,0x06,0x7e,0xc6,0xc6,0xc6,0x7e,0,0,0,0,0,0,0,0},
    /* e */ {0,0,0,0x7c,0xc6,0xfe,0xc0,0x7c,0,0,0,0,0,0,0,0},
    /* f */ {0,0,0x1e,0x30,0x7c,0x30,0x30,0x30,0,0,0,0,0,0,0,0},
    /* g */ {0,0,0,0x7e,0xc6,0xc6,0x7e,0x06,0xfc,0,0,0,0,0,0,0},
    /* h */ {0,0,0xc0,0xfc,0xc6,0xc6,0xc6,0xc6,0,0,0,0,0,0,0,0},
    /* i */ {0,0,0x18,0,0x38,0x18,0x18,0x7e,0,0,0,0,0,0,0,0},
    /* j */ {0,0,0x06,0,0x0e,0x06,0x06,0xc6,0x7c,0,0,0,0,0,0,0},
    /* k */ {0,0,0xc0,0xc6,0xcc,0xf8,0xcc,0xc6,0,0,0,0,0,0,0,0},
    /* l */ {0,0,0x38,0x18,0x18,0x18,0x18,0x7e,0,0,0,0,0,0,0,0},
    /* m */ {0,0,0,0xec,0xfe,0xd6,0xc6,0xc6,0,0,0,0,0,0,0,0},
    /* n */ {0,0,0,0xdc,0xe6,0xc6,0xc6,0xc6,0,0,0,0,0,0,0,0},
    /* o */ {0,0,0,0x7c,0xc6,0xc6,0xc6,0x7c,0,0,0,0,0,0,0,0},
    /* p */ {0,0,0,0xfc,0xc6,0xc6,0xfc,0xc0,0xc0,0,0,0,0,0,0,0},
    /* q */ {0,0,0,0x7e,0xc6,0xc6,0x7e,0x06,0x07,0,0,0,0,0,0,0},
    /* r */ {0,0,0,0xdc,0xe0,0xc0,0xc0,0xc0,0,0,0,0,0,0,0,0},
    /* s */ {0,0,0,0x78,0xc0,0x7c,0x06,0xfc,0,0,0,0,0,0,0,0},
    /* t */ {0,0,0x30,0x7c,0x30,0x30,0x30,0x1e,0,0,0,0,0,0,0,0},
    /* u */ {0,0,0,0xc6,0xc6,0xc6,0xc6,0x7e,0,0,0,0,0,0,0,0},
    /* v */ {0,0,0,0xc6,0xc6,0x6c,0x38,0x10,0,0,0,0,0,0,0,0},
    /* w */ {0,0,0,0xc6,0xc6,0xd6,0xfe,0x6c,0,0,0,0,0,0,0,0},
    /* x */ {0,0,0,0xc6,0x6c,0x38,0x6c,0xc6,0,0,0,0,0,0,0,0},
    /* y */ {0,0,0,0xc6,0xc6,0x7e,0x06,0x7c,0,0,0,0,0,0,0,0},
    /* z */ {0,0,0,0xfe,0x0c,0x38,0x60,0xfe,0,0,0,0,0,0,0,0},
    /* { */ {0,0,0x0e,0x18,0x18,0x70,0x18,0x18,0x0e,0,0,0,0,0,0,0},
    /* | */ {0,0,0x18,0x18,0x18,0,0x18,0x18,0x18,0,0,0,0,0,0,0},
    /* } */ {0,0,0x70,0x18,0x18,0x0e,0x18,0x18,0x70,0,0,0,0,0,0,0},
    /* ~ */ {0,0,0x76,0xdc,0,0,0,0,0,0,0,0,0,0,0,0},
    /* DEL */ {0},
};

/* ---- init ---------------------------------------------------------------- */

void fb_init(uint64_t mbi_addr) {
    fb.available = 0;

    if (!mbi_addr) {
        kerror("FB: mbi_addr is NULL — cannot parse framebuffer tag\n");
        return;
    }

    uint32_t mbi_total = *(uint32_t *)mbi_addr;
    kdebug("FB: parsing MB2 info at 0x%x (total=%u bytes)\n",
           mbi_addr, mbi_total);

    uint8_t *p   = (uint8_t *)(mbi_addr + 8); /* skip fixed 8-byte header */
    uint8_t *end = (uint8_t *)mbi_addr + mbi_total;

    int tag_count = 0;
    while (p < end) {
        uint32_t tag_type = *(uint32_t *)p;
        uint32_t tag_size = *(uint32_t *)(p + 4);

        if (tag_size < 8) {
            kerror("FB: malformed tag at 0x%llx (size=%u) — stopping scan\n",
                   (unsigned long long)(uintptr_t)p, tag_size);
            break;
        }

        kdebug("FB: tag #%d type=%u size=%u at 0x%llx\n",
               tag_count, tag_type, tag_size, (unsigned long long)(uintptr_t)p);
        tag_count++;

        if (tag_type == 0) {
            kdebug("FB: end tag reached after %d tags\n", tag_count);
            break;
        }

        if (tag_type == 8) { /* framebuffer info tag */
            uint64_t addr64 = *(uint64_t *)(p + 8);
            fb.pitch   = *(uint32_t *)(p + 16);
            fb.width   = *(uint32_t *)(p + 20);
            fb.height  = *(uint32_t *)(p + 24);
            fb.bpp     = *(uint8_t  *)(p + 28);
            fb.type    = *(uint8_t  *)(p + 29);
            fb.addr    = addr64;

            kinfo("FB: tag found: %ux%u bpp=%u type=%u pitch=%u addr=0x%llx\n",
                  fb.width, fb.height, fb.bpp, fb.type,
                  fb.pitch, (unsigned long long)fb.addr);

            /*
             * type: 0=indexed palette, 1=direct RGB pixel, 2=EGA text
             * Reject EGA text (type=2) — that is 0xB8000 and cannot
             * accept pixel drawing.  Also reject implausibly small modes.
             */
            if (fb.type != 1) {
                kinfo("FB: type=%u is not RGB pixel mode — desktop disabled\n",
                      fb.type);
                kinfo("FB: Boot with 'Graphical' entry to get VESA pixel mode\n");
                break;
            }
            if (fb.width < 640 || fb.bpp < 15) {
                kinfo("FB: resolution %ux%u bpp=%u too small — desktop disabled\n",
                      fb.width, fb.height, fb.bpp);
                break;
            }
            if (fb.addr < 0x1000) {
                kerror("FB: framebuffer address 0x%x is suspiciously low\n",
                       fb.addr);
                break;
            }

            /* boot.s already identity-maps all 4 GB (0–0xFFFFFFFF)
             * with 2MB huge pages, so the physical FB address IS the
             * virtual address — no additional mapping required. */
            uint32_t fb_size = fb.pitch * fb.height;
            kdebug("FB: phys=0x%llx size=%u bytes — identity-mapped, ready\n",
                   (unsigned long long)fb.addr, fb_size);

            fb.available = 1;
            kinfo("FB: %ux%u @0x%llx pitch=%u bpp=%u — VESA pixel mode ACTIVE\n",
                  fb.width, fb.height, (unsigned long long)fb.addr, fb.pitch, fb.bpp);
            break;
        }

        /* Advance to next tag (8-byte aligned) */
        uint32_t next_off = (tag_size + 7u) & ~7u;
        if (next_off == 0) next_off = 8; /* safety: never loop forever */
        p += next_off;
    }

    if (!fb.available) {
        kinfo("FB: no usable pixel framebuffer\n");
        kinfo("FB: -> select 'DracolaxOS V1 (Graphical)' in GRUB for desktop\n");
        kinfo("FB: -> headless text mode active\n");
    }

    /* Allocate shadow buffer (double buffer) for flicker-free rendering */
    if (fb.available && fb.pitch > 0 && fb.height > 0) {
        size_t shadow_sz = (size_t)fb.pitch * fb.height;
        fb_shadow = (uint32_t *)kmalloc(shadow_sz);
        if (fb_shadow) {
            memset(fb_shadow, 0, shadow_sz);
            /* Enable shadow mode immediately so no code accidentally writes
             * directly to VRAM (which may not be mapped in all page table
             * configurations). All draws go to shadow; call fb_flip() to present. */
            fb_use_shadow = 1;
            kinfo("FB: shadow buffer allocated (%u KB), shadow mode ON\n",
                  (uint32_t)(shadow_sz / 1024));
        } else {
            kwarn("FB: shadow buffer OOM — flicker-free mode disabled\n");
        }
    }
}

/* ---- drawing primitives ------------------------------------------------- */

uint32_t fb_color(uint8_t r, uint8_t g, uint8_t b) {
    if (fb.bpp == 32) return ((uint32_t)r<<16)|((uint32_t)g<<8)|b;
    if (fb.bpp == 24) return ((uint32_t)r<<16)|((uint32_t)g<<8)|b;
    if (fb.bpp == 16) {
        /* RGB565 */
        return (((uint32_t)r>>3)<<11)|(((uint32_t)g>>2)<<5)|(b>>3);
    }
    return 0;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb.available || x >= fb.width || y >= fb.height) return;
    if (fb_use_shadow && fb_shadow) {
        fb_shadow[y * (fb.pitch >> 2) + x] = color;
        return;
    }
    uint8_t *row = (uint8_t *)(uintptr_t)fb.addr + y * fb.pitch + x * (fb.bpp / 8);
    if (fb.bpp == 32) { *(uint32_t *)row = color; }
    else if (fb.bpp == 24) {
        row[0] = color & 0xff;
        row[1] = (color >> 8) & 0xff;
        row[2] = (color >> 16) & 0xff;
    }
    else if (fb.bpp == 16) { *(uint16_t *)row = (uint16_t)color; }
}

/* Direct VRAM access — bypasses shadow buffer entirely.
 * Used by the cursor system so the cursor is always stamped
 * onto VRAM after fb_flip() and never corrupts the shadow. */
void fb_put_pixel_vram(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb.available || x >= fb.width || y >= fb.height) return;
    uint8_t *p = (uint8_t *)(uintptr_t)fb.addr + y * fb.pitch + x * (fb.bpp / 8);
    if (fb.bpp == 32)      *(uint32_t *)p = color;
    else if (fb.bpp == 24) { p[0]=color&0xff; p[1]=(color>>8)&0xff; p[2]=(color>>16)&0xff; }
    else if (fb.bpp == 16) *(uint16_t *)p = (uint16_t)color;
}

uint32_t fb_read_pixel_vram(uint32_t x, uint32_t y) {
    if (!fb.available || x >= fb.width || y >= fb.height) return 0;
    uint8_t *p = (uint8_t *)(uintptr_t)fb.addr + y * fb.pitch + x * (fb.bpp / 8);
    if (fb.bpp == 32)  return *(uint32_t *)p;
    if (fb.bpp == 24)  return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16);
    if (fb.bpp == 16)  return *(uint16_t *)p;
    return 0;
}

uint32_t fb_blend(uint32_t src, uint32_t dst, uint8_t a) {
    uint32_t rb = ((src & 0xff00ff) * a + (dst & 0xff00ff) * (255-a)) >> 8;
    uint32_t g  = ((src & 0x00ff00) * a + (dst & 0x00ff00) * (255-a)) >> 8;
    return (rb & 0xff00ff) | (g & 0x00ff00);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c) {
    if (!fb.available) return;
    uint32_t y_end = (h > fb.height - y) ? fb.height : y + h;
    uint32_t x_end = (w > fb.width  - x) ? fb.width  : x + w;
    for (uint32_t row = y; row < y_end; row++)
        for (uint32_t col = x; col < x_end; col++)
            fb_put_pixel(col, row, c);
}

void fb_clear(uint32_t color) {
    fb_fill_rect(0, 0, fb.width, fb.height, color);
}

void fb_putchar(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) {
    if (!fb.available) return;
    uint8_t idx = (uint8_t)c;
    if (idx >= 128) idx = '?';
    for (int row = 0; row < 16; row++) {
        uint8_t bits = vga_font_8x16[idx][row];
        for (int col = 0; col < 8; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            fb_put_pixel(x + col, y + row, color);
        }
    }
}

void fb_print(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg) {
    uint32_t cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += 16; }
        else { fb_putchar(cx, y, *s, fg, bg); cx += 8; }
        s++;
        if (cx + 8 > fb.width) { cx = x; y += 16; }
    }
}

void fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
             const uint32_t *pixels) {
    for (uint32_t row = 0; row < h; row++)
        for (uint32_t col = 0; col < w; col++)
            fb_put_pixel(x + col, y + row, pixels[row * w + col]);
}

void fb_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     uint32_t r, uint32_t color) {
    if (!fb.available) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* Fill the central vertical strip (full height, no corners needed) */
    if (w > 2 * r)
        fb_fill_rect(x + r, y, w - 2 * r, h, color);

    /* Fill left and right side strips (excluding corner regions) */
    if (h > 2 * r) {
        fb_fill_rect(x,         y + r, r, h - 2 * r, color);
        fb_fill_rect(x + w - r, y + r, r, h - 2 * r, color);
    }

    /* Fill the four rounded corner quadrants using the circle equation.
     * For each row offset dy from the corner center, we compute how far
     * the circle extends horizontally (dx) and fill that many pixels. */
    int ir = (int)r;
    for (int dy = 0; dy < ir; dy++) {
        /* integer sqrt: find largest dx such that dx^2+dy^2 <= r^2 */
        int dx = 0;
        while ((dx + 1) * (dx + 1) + (ir - 1 - dy) * (ir - 1 - dy) <= ir * ir)
            dx++;

        uint32_t row_w = (uint32_t)dx;
        uint32_t row_x_l = x + r - row_w;
        uint32_t row_x_r = x + w - r;

        /* Top corners */
        fb_fill_rect(row_x_l, y + (uint32_t)dy,           row_w, 1, color);
        fb_fill_rect(row_x_r, y + (uint32_t)dy,           row_w, 1, color);
        /* Bottom corners */
        fb_fill_rect(row_x_l, y + h - 1 - (uint32_t)dy,  row_w, 1, color);
        fb_fill_rect(row_x_r, y + h - 1 - (uint32_t)dy,  row_w, 1, color);
    }
}

/* ---- Scaled character/string rendering ---------------------------------- */
/* Renders the embedded 8x16 bitmap font at an integer scale factor.
 * scale=1 → 8×16px, scale=2 → 16×32px, scale=3 → 24×48px, etc.
 * Used by the boot screen to fake multiple "font sizes" without TTF. */

void fb_putchar_s(uint32_t x, uint32_t y, char ch,
                  uint32_t fg, uint32_t bg, uint32_t scale) {
    if (!fb.available) return;
    if (scale < 1) scale = 1;

    uint8_t idx = (uint8_t)ch;
    if (idx >= 128) idx = '?';

    for (uint32_t row = 0; row < 16; row++) {
        uint8_t bits = vga_font_8x16[idx][row];
        for (uint32_t col = 0; col < 8; col++) {
            uint32_t color = (bits & (0x80u >> col)) ? fg : bg;
            /* Skip transparent bg (0x00000000) */
            if (color == 0x00000000u && !(bits & (0x80u >> col))) continue;
            fb_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

void fb_print_s(uint32_t x, uint32_t y, const char *s,
                uint32_t fg, uint32_t bg, uint32_t scale) {
    if (!fb.available) return;
    uint32_t cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += 16 * scale; }
        else { fb_putchar_s(cx, y, *s, fg, bg, scale); cx += 8 * scale; }
        s++;
    }
}

/* ---- FB text console (for text/shell modes) ----------------------------- */
/* Provides a scrollable terminal on the framebuffer.
 * Used when desktop is not started but we still want visible text output.
 *
 * FIX: kcon_locked flag lets the desktop suppress fb_console_print() calls
 * that race against the desktop's own shadow-buffer draws.  The desktop
 * calls fb_console_lock(1) on entry to desktop_task() so that any stray
 * klog/serial-mirror writes do not corrupt the GUI frame mid-draw. */

static int kcon_locked = 0;   /* 0 = console active, 1 = suppressed */

void fb_console_lock(int lock)   { kcon_locked = lock; }
int  fb_console_is_locked(void)  { return kcon_locked; }

#define KCON_COLS  (fb.width  / 8)
#define KCON_ROWS  (fb.height / 16)

static uint32_t kcon_row = 0;
static uint32_t kcon_col = 0;

static const uint32_t KCON_FG = 0xCCCCCCu;  /* light grey */
static const uint32_t KCON_BG = 0x0A0A0Fu;  /* near-black dark blue */

void fb_console_init(void) {
    if (!fb.available) return;
    fb_clear(KCON_BG);
    kcon_row = 0;
    kcon_col = 0;
    /* Header bar */
    fb_fill_rect(0, 0, fb.width, 18, 0x1A1A2Eu);
    fb_print(4, 1, "DracolaxOS Terminal", 0x7EB8FFu, 0x1A1A2Eu);
    /* Push dark bg to VRAM immediately — without this, GRUB's gray background
     * remains visible until the first fb_console_print() call. */
    fb_flip();
}

void fb_console_clear(void) {
    if (!fb.available) return;
    fb_clear(KCON_BG);
    kcon_row = 1;  /* leave header */
    kcon_col = 0;
    fb_fill_rect(0, 0, fb.width, 18, 0x1A1A2Eu);
    fb_print(4, 1, "DracolaxOS Terminal", 0x7EB8FFu, 0x1A1A2Eu);
}

static void kcon_scroll(void) {
    if (!fb.available) return;
    uint32_t line_h  = 16;
    uint32_t start_y = 20;
    uint32_t rows    = KCON_ROWS - 2;
    uint32_t bytes_per_line = fb.pitch * line_h;

    if (fb_use_shadow && fb_shadow) {
        /* Scroll within shadow */
        uint8_t *base = (uint8_t *)fb_shadow + start_y * fb.pitch;
        for (uint32_t r = 0; r + 1 < rows; r++) {
            uint8_t *dst = base + r * bytes_per_line;
            uint8_t *src = base + (r + 1) * bytes_per_line;
            for (uint32_t b = 0; b < bytes_per_line; b++) dst[b] = src[b];
        }
    } else {
        uint8_t *base = (uint8_t *)(uintptr_t)fb.addr + start_y * fb.pitch;
        for (uint32_t r = 0; r + 1 < rows; r++) {
            uint8_t *dst = base + r * bytes_per_line;
            uint8_t *src = base + (r + 1) * bytes_per_line;
            for (uint32_t b = 0; b < bytes_per_line; b++) dst[b] = src[b];
        }
    }
    /* Clear last row */
    fb_fill_rect(0, start_y + (rows - 1) * line_h, fb.width, line_h, KCON_BG);
    /* Redraw header — scroll copies shadow rows which may overwrite header area
     * on systems where start_y is small relative to line_h alignment. */
    fb_fill_rect(0, 0, fb.width, 18, 0x1A1A2Eu);
    fb_print(4, 1, "DracolaxOS Terminal", 0x7EB8FFu, 0x1A1A2Eu);
    kcon_row = rows - 1;
}

void fb_console_print(const char *s, uint32_t fg) {
    if (!fb.available) return;
    if (kcon_locked) return;   /* FIX: desktop owns the shadow buffer; skip */
    if (fg == 0) fg = KCON_FG;

    uint32_t max_cols = KCON_COLS;
    uint32_t max_rows = KCON_ROWS - 2;
    uint32_t start_y  = 20;

    while (*s) {
        if (*s == '\b') {
            /* Backspace: erase previous character */
            if (kcon_col > 0) {
                kcon_col--;
                fb_putchar(kcon_col * 8, start_y + kcon_row * 16,
                           ' ', KCON_FG, KCON_BG);
            }
            s++;
            continue;
        }
        if (*s == '\n' || kcon_col >= max_cols) {
            kcon_col = 0;
            kcon_row++;
            if (kcon_row >= max_rows) kcon_scroll();
        }
        if (*s != '\n' && *s != '\r') {
            fb_putchar(kcon_col * 8, start_y + kcon_row * 16, *s, fg, KCON_BG);
            kcon_col++;
        }
        s++;
    }
    /* FIX B: In text/shell mode the desktop never calls fb_flip(), so draws
     * to the shadow buffer are invisible.  Auto-flip here whenever the console
     * owns the shadow (kcon_locked == 0 guarantees the desktop is not active). */
    if (fb_use_shadow && fb_shadow) fb_flip();
}

