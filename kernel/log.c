/* kernel/log.c */
#include "types.h"
#include "log.h"
#include "drivers/vga/vga.h"
#include "drivers/vga/fb.h"
#include "drivers/serial/serial.h"
#include "klibc.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"

static const char *level_prefix[] = {
    "[INFO ] ", "[WARN ] ", "[ERROR] ", "[DEBUG] "
};
static uint8_t level_fg[] = {
    VGA_LIGHT_GREY, VGA_LIGHT_BROWN, VGA_LIGHT_RED, VGA_DARK_GREY
};
/* FB colors per log level: INFO, WARN, ERROR, DEBUG */
static const uint32_t level_fb_fg[] = {
    0xCCCCCCu, 0xFFCC44u, 0xFF4444u, 0x666688u
};

void log_init(void) {}

void printk(int level, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(buf, sizeof(buf), "%s", level_prefix[level & 3]);
    vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
    va_end(ap);

    vga_set_color(level_fg[level & 3], VGA_BLACK);
    vga_print(buf);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    serial_print(buf);

    /* FIX: Guard fb_console_print() with is_locked() so that when the
     * desktop owns the shadow buffer (kcon_locked=1) printk() does not
     * attempt to draw into the GUI framebuffer.  The internal check inside
     * fb_console_print() is a safety net; this outer guard avoids the call
     * overhead entirely in the hot path (every kinfo/kdebug during desktop). */
    if (fb.available && !fb_console_is_locked())
        fb_console_print(buf, level_fb_fg[level & 3]);
}

/* ---- Kernel panic screen ------------------------------------------------ */

#define KPANIC_SCOLS 80
#define KPANIC_SROWS 25

static volatile uint16_t *const vga_mem = (volatile uint16_t *)0xB8000;

static inline uint16_t kp_attr(uint8_t fg, uint8_t bg) {
    return (uint16_t)((bg << 4) | fg);
}
static void kp_put(uint8_t x, uint8_t y, char c, uint16_t attr) {
    if (x >= KPANIC_SCOLS || y >= KPANIC_SROWS) return;
    vga_mem[y * KPANIC_SCOLS + x] = (uint16_t)(unsigned char)c | (attr << 8);
}
static void kp_fill(uint8_t row, char c, uint16_t attr) {
    for (uint8_t x = 0; x < KPANIC_SCOLS; x++) kp_put(x, row, c, attr);
}
static void kp_print(uint8_t x, uint8_t y, const char *s, uint16_t attr) {
    while (*s && x < KPANIC_SCOLS) { kp_put(x++, y, *s++, attr); }
}
static void kp_print_hex(uint8_t x, uint8_t y, uint64_t v, uint16_t attr) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 15; i >= 0; i--) { buf[2+i] = hex[v & 0xF]; v >>= 4; }
    buf[18] = '\0';
    kp_print(x, y, buf, attr);
}

void kpanic(const char *msg) {
    __asm__ volatile ("cli");

    uint16_t red_hdr  = kp_attr(VGA_WHITE,      VGA_RED);
    uint16_t red_body = kp_attr(VGA_LIGHT_RED,  VGA_RED);
    uint16_t red_dim  = kp_attr(VGA_BROWN,       VGA_RED);

    /* Fill entire screen red */
    for (uint8_t y = 0; y < KPANIC_SROWS; y++) kp_fill(y, ' ', red_body);

    /* Row 0: header */
    kp_fill(0, ' ', red_hdr);
    kp_print(2, 0, "DRACOLAX OS  ***  KERNEL PANIC  ***", red_hdr);

    /* Row 2-6: reason (wrap at 76 chars) */
    kp_print(2, 2, "REASON:", red_hdr);
    uint8_t rx = 10, ry = 2;
    for (const char *p = msg; *p; p++) {
        if (*p == '\n' || rx >= 78) { rx = 2; ry++; if (ry >= 7) break; }
        if (*p != '\n') kp_put(rx++, ry, *p, red_body);
    }

    /* Row 8: separator */
    for (uint8_t x = 2; x < 78; x++) kp_put(x, 8, '-', red_dim);

    /* Row 9: debug info header */
    kp_print(2, 9, "DEBUG INFORMATION:", red_hdr);

    /* RIP via call+pop trick */
    uint64_t rip = 0;
    __asm__ volatile ("lea (%%rip), %0" : "=r"(rip));
    kp_print(2,  10, "RIP  :", red_body);
    kp_print_hex(10, 10, rip, red_body);

    uint64_t rsp = 0;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    kp_print(2,  11, "RSP  :", red_body);
    kp_print_hex(10, 11, rsp, red_body);

    /* Memory info */
    uint32_t free_kb = pmm_free_pages() * 4;
    uint32_t used_kb = pmm_used_pages() * 4;
    char tmp[16]; tmp[15] = '\0';
    int i, digits;

    kp_print(2, 12, "RAM  : free=", red_body);
    uint32_t v = free_kb; digits = 0;
    if (!v) { tmp[14]='0'; digits=1; } else { i=14; while(v){tmp[i--]='0'+(v%10);v/=10;digits++;} }
    kp_print(14, 12, tmp + 15 - digits, red_body);
    kp_print(14 + (uint8_t)digits, 12, " KB", red_body);

    kp_print(20, 12, "used=", red_body);
    v = used_kb; digits = 0;
    if (!v) { tmp[14]='0'; digits=1; } else { i=14; while(v){tmp[i--]='0'+(v%10);v/=10;digits++;} }
    kp_print(25, 12, tmp + 15 - digits, red_body);
    kp_print(25 + (uint8_t)digits, 12, " KB", red_body);

    kp_print(2, 13, "TASKS:", red_body);
    int tc = sched_task_count();
    tmp[15]='\0'; i=14;
    uint32_t uv = (uint32_t)(tc < 0 ? 0 : tc);
    if (!uv){tmp[14]='0';}
    else{while(uv){tmp[i--]='0'+(uv%10);uv/=10;i=i<0?0:i;}}
    kp_print(9, 13, tmp + 14 - (tc>9?1:0), red_body);

    /* Row 15: separator */
    for (uint8_t x = 2; x < 78; x++) kp_put(x, 15, '-', red_dim);

    kp_print(2, 16, "WHAT TO DO:", red_hdr);
    kp_print(4, 17, "1. Note the REASON above.", red_body);
    kp_print(4, 18, "2. Check serial output (COM1) for full debug log.", red_body);
    kp_print(4, 19, "3. Report at: github.com/lunax/dracolaxos/issues", red_body);

    /* Also show on FB if available */
    if (fb.available) {
        fb_fill_rect(0, 0, fb.width, fb.height, 0x1A0000u);
        fb_fill_rect(0, 0, fb.width, 32, 0xCC0000u);
        fb_print_s(8, 4, "*** KERNEL PANIC ***", 0xFFFFFFu, 0xCC0000u, 2);
        fb_fill_rect(0, 40, fb.width, 2, 0xFF0000u);
        fb_print_s(8, 50, "REASON:", 0xFF8888u, 0x1A0000u, 1);
        fb_print(8, 68, msg, 0xFFAAAAu, 0x1A0000u);
        fb_fill_rect(0, fb.height - 20, fb.width, 20, 0x440000u);
        fb_print(4, fb.height - 16,
            "System halted. Power off and restart.", 0xFF8888u, 0x440000u);
    }

    serial_print("\n\n=== KERNEL PANIC ===\nREASON: ");
    serial_print(msg);
    serial_print("\n");

    /* Bottom bar */
    kp_fill(KPANIC_SROWS - 1, ' ', red_hdr);
    kp_print(2, KPANIC_SROWS - 1, "System halted. Power off and restart.", red_hdr);

    for (;;) __asm__ volatile ("hlt");
}
