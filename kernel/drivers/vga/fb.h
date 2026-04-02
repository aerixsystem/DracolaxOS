/* kernel/fb.h — VESA linear framebuffer driver (x86_64) */
#ifndef FB_H
#define FB_H
#include "../../types.h"

typedef struct {
    uint64_t  addr;       /* physical/virtual base (identity-mapped = same)  */
    uint32_t  pitch;      /* bytes per scanline                               */
    uint32_t  width;      /* horizontal resolution in pixels                  */
    uint32_t  height;     /* vertical resolution in pixels                    */
    uint8_t   bpp;        /* bits per pixel                                   */
    uint8_t   type;       /* 0=indexed 1=RGB 2=EGA text                       */
    uint8_t   available;  /* 1 if framebuffer was found and mapped             */
} fb_info_t;

extern fb_info_t fb;

/* 8x16 VGA bitmap font — exposed for subsystems that blit into their own
 * pixel buffers (e.g. compositor backbuf). Each entry is 16 bytes (one byte
 * per row, MSB = leftmost pixel). Defined in fb.c. */
extern const uint8_t vga_font_8x16[128][16];

void     fb_init       (uint64_t mbi_addr);
uint32_t fb_color      (uint8_t r, uint8_t g, uint8_t b);
void     fb_put_pixel  (uint32_t x, uint32_t y, uint32_t color);
uint32_t fb_read_pixel (uint32_t x, uint32_t y);   /* read shadow or fb */
/* Direct VRAM access — bypasses shadow (used by cursor system) */
void     fb_put_pixel_vram  (uint32_t x, uint32_t y, uint32_t color);
uint32_t fb_read_pixel_vram (uint32_t x, uint32_t y);
void     fb_fill_rect  (uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c);
void     fb_putchar    (uint32_t x, uint32_t y, char ch, uint32_t fg, uint32_t bg);
void     fb_print      (uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg);
/* Scale: 1=8x16 2=16x32 3=24x48 etc. */
void     fb_putchar_s  (uint32_t x, uint32_t y, char ch, uint32_t fg, uint32_t bg, uint32_t scale);
void     fb_print_s    (uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg, uint32_t scale);
void     fb_blit       (uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *pixels);
void     fb_clear      (uint32_t color);
uint32_t fb_blend      (uint32_t src, uint32_t dst, uint8_t alpha);
void     fb_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t color);

/* Double-buffer API.
 * Call fb_enable_shadow() once (e.g. at desktop start).
 * All fb_* draws go to shadow RAM; call fb_flip() to present one frame.
 * fb_disable_shadow() restores direct-to-VRAM writes. */
void     fb_enable_shadow (void);
void     fb_disable_shadow(void);
void     fb_flip          (void);          /* shadow → VRAM */

/** Returns the shadow buffer pointer (NULL if not allocated). Used by
 *  atlas_boot_log to copy the debug strip directly to VRAM. */
uint32_t *fb_shadow_ptr(void);

/* Text console (used in text/shell modes instead of VGA)
 * Call fb_console_lock(1) before desktop_task() to prevent stray
 * klog/console writes from racing against GUI shadow-buffer draws. */
void     fb_console_init    (void);
void     fb_console_print   (const char *s, uint32_t fg);
void     fb_console_clear   (void);
void     fb_console_lock    (int lock);   /* 1 = suppress writes, 0 = allow */
int      fb_console_is_locked(void);

#endif /* FB_H */
