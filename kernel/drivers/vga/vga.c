#include "../../types.h"
#include "vga.h"
#include "../../klibc.h"
#include "fb.h"   /* fb_console_print + fb.available */

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_ADDR   0xB8000

static uint16_t *const vga = (uint16_t *)VGA_ADDR;
static size_t  row, col;
static uint8_t color;   /* fg | (bg << 4) */

/* --- port I/O helpers ----------------------------------------------------- */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

/* --- internal helpers ----------------------------------------------------- */
static inline uint16_t entry(char c, uint8_t attr) {
    return (uint16_t)(unsigned char)c | ((uint16_t)attr << 8);
}

static void scroll(void) {
    /* move every line up by one */
    memmove(vga, vga + VGA_WIDTH,
            sizeof(uint16_t) * VGA_WIDTH * (VGA_HEIGHT - 1));
    /* blank last line */
    for (size_t x = 0; x < VGA_WIDTH; x++)
        vga[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = entry(' ', color);
    row = VGA_HEIGHT - 1;
}

/* --- public API ----------------------------------------------------------- */

void vga_init(void) {
    color = VGA_LIGHT_GREY | (VGA_BLACK << 4);
    row = col = 0;
    vga_clear();
}

void vga_set_color(uint8_t fg, uint8_t bg) {
    color = fg | (uint8_t)(bg << 4);
}

void vga_clear(void) {
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga[i] = entry(' ', color);
    row = col = 0;
    vga_move_cursor(0, 0);
}

void vga_putchar(char c) {
    if (c == '\n') {
        col = 0;
        if (++row >= VGA_HEIGHT) scroll();
        vga_move_cursor((uint8_t)col, (uint8_t)row);
        return;
    }
    if (c == '\r') {
        col = 0;
        vga_move_cursor((uint8_t)col, (uint8_t)row);
        return;
    }
    if (c == '\b') {
        if (col > 0) {
            col--;
            vga[row * VGA_WIDTH + col] = entry(' ', color);
            vga_move_cursor((uint8_t)col, (uint8_t)row);
        }
        return;
    }
    if (c == '\t') {
        col = (col + 8) & ~7u;
        if (col >= VGA_WIDTH) { col = 0; if (++row >= VGA_HEIGHT) scroll(); }
        vga_move_cursor((uint8_t)col, (uint8_t)row);
        return;
    }

    vga[row * VGA_WIDTH + col] = entry(c, color);
    if (++col >= VGA_WIDTH) {
        col = 0;
        if (++row >= VGA_HEIGHT) scroll();
    }
    vga_move_cursor((uint8_t)col, (uint8_t)row);
}

void vga_print(const char *s) {
    while (*s) vga_putchar(*s++);
}

/* Forward text-mode output to FB console so text/shell modes show
 * log output on the VESA screen (not just the VGA text buffer). */
void vga_print_fb(const char *s, uint32_t fg) {
    /* Only route to FB when fb_console is active (fb.available set before
     * fb_console_init is called) and we are NOT in graphical desktop mode
     * (graphical mode uses the compositor instead). */
    if (fb.available) fb_console_print(s, fg);
}

void vga_move_cursor(uint8_t c, uint8_t r) {
    uint16_t pos = (uint16_t)(r * VGA_WIDTH + c);
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)(pos >> 8));
}
