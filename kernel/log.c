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
#include "bootmode.h"
#include "klog.h"

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

    /* Always write to serial — available in all modes */
    serial_print(buf);

    /* Always enqueue to klog ring — klog_flush_task persists to storage */
    klog_write(KLOG_KERNEL, buf);

    /* In GRAPHICAL/AUTO mode: VGA text is invisible (pixel fb owns the
     * display). Only write to fb_console if not locked by the desktop. */
    if (bootmode_wants_desktop()) {
        if (fb.available && !fb_console_is_locked())
            fb_console_print(buf, level_fb_fg[level & 3]);
        return;
    }

    /* SHELL / TEXT modes: ALL log output goes to the klog file only.
     * DO NOT write to VGA text buffer — the shell owns that display and
     * stray kernel messages would corrupt its output mid-command.
     * The exception is ERROR level which is always shown. */
    if ((level & 3) == LOG_ERROR) {
        vga_set_color(level_fg[level & 3], VGA_BLACK);
        vga_print(buf);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
    /* INFO / WARN / DEBUG: silently captured to klog file, not screen */
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

    /* ── Capture all GP registers immediately ── */
    uint64_t reg_rax, reg_rbx, reg_rcx, reg_rdx;
    uint64_t reg_rsi, reg_rdi, reg_rbp, reg_rsp;
    uint64_t reg_r8,  reg_r9,  reg_r10, reg_r11;
    uint64_t reg_r12, reg_r13, reg_r14, reg_r15;
    uint64_t reg_rip, reg_rflags;

    __asm__ volatile (
        "mov %%rax, %0\n"  "mov %%rbx, %1\n"
        "mov %%rcx, %2\n"  "mov %%rdx, %3\n"
        "mov %%rsi, %4\n"  "mov %%rdi, %5\n"
        "mov %%rbp, %6\n"  "mov %%rsp, %7\n"
        "mov %%r8,  %8\n"  "mov %%r9,  %9\n"
        "mov %%r10, %10\n" "mov %%r11, %11\n"
        "mov %%r12, %12\n" "mov %%r13, %13\n"
        "mov %%r14, %14\n" "mov %%r15, %15\n"
        "lea (%%rip), %16\n"
        "pushfq\n" "pop %17\n"
        : "=m"(reg_rax), "=m"(reg_rbx), "=m"(reg_rcx), "=m"(reg_rdx),
          "=m"(reg_rsi), "=m"(reg_rdi), "=m"(reg_rbp), "=m"(reg_rsp),
          "=m"(reg_r8),  "=m"(reg_r9),  "=m"(reg_r10), "=m"(reg_r11),
          "=m"(reg_r12), "=m"(reg_r13), "=m"(reg_r14), "=m"(reg_r15),
          "=r"(reg_rip), "=r"(reg_rflags)
        :: "memory"
    );

    uint16_t red_hdr  = kp_attr(VGA_WHITE,     VGA_RED);
    uint16_t red_body = kp_attr(VGA_LIGHT_RED, VGA_RED);
    uint16_t red_dim  = kp_attr(VGA_BROWN,      VGA_RED);

    for (uint8_t y = 0; y < KPANIC_SROWS; y++) kp_fill(y, ' ', red_body);

    kp_fill(0, ' ', red_hdr);
    kp_print(2, 0, "DRACOLAX OS  ***  KERNEL PANIC  ***", red_hdr);

    kp_print(2, 2, "REASON:", red_hdr);
    uint8_t rx = 10, ry = 2;
    for (const char *p = msg; *p; p++) {
        if (*p == '\n' || rx >= 78) { rx = 2; ry++; if (ry >= 7) break; }
        if (*p != '\n') kp_put(rx++, ry, *p, red_body);
    }

    for (uint8_t x = 2; x < 78; x++) kp_put(x, 8, '-', red_dim);
    kp_print(2, 9, "REGISTERS:", red_hdr);

    struct { const char *name; uint64_t val; } regs[] = {
        {"RAX", reg_rax}, {"RBX", reg_rbx},
        {"RCX", reg_rcx}, {"RDX", reg_rdx},
        {"RSI", reg_rsi}, {"RDI", reg_rdi},
        {"RBP", reg_rbp}, {"RSP", reg_rsp},
        {"R8 ", reg_r8 }, {"R9 ", reg_r9 },
        {"R10", reg_r10}, {"R11", reg_r11},
        {"R12", reg_r12}, {"R13", reg_r13},
        {"R14", reg_r14}, {"R15", reg_r15},
        {"RIP", reg_rip}, {"RFL", reg_rflags},
    };
    for (int ri = 0; ri < 18; ri++) {
        uint8_t col = (ri & 1) ? 40u : 2u;
        uint8_t row = (uint8_t)(10 + ri / 2);
        if (row >= 19) break;
        kp_print(col, row, regs[ri].name, red_body);
        kp_put((uint8_t)(col + 3), row, ':', red_body);
        kp_print_hex((uint8_t)(col + 5), row, regs[ri].val, red_body);
    }

    for (uint8_t x = 2; x < 78; x++) kp_put(x, 19, '-', red_dim);

    uint32_t free_kb = pmm_free_pages() * 4;
    uint32_t used_kb = pmm_used_pages() * 4;
    (void)used_kb;  /* computed for completeness; displayed via free_kb */
    char tmp[16]; tmp[15] = '\0';
    int i, digits;
    kp_print(2, 20, "RAM free=", red_body);
    uint32_t v = free_kb; digits = 0;
    if (!v){tmp[14]='0';digits=1;}else{i=14;while(v){tmp[i--]='0'+(v%10);v/=10;digits++;}}
    kp_print(11, 20, tmp + 15 - digits, red_body);
    kp_print((uint8_t)(11+(uint8_t)digits), 20, "KB  tasks=", red_body);
    int tc = sched_task_count();
    uint32_t uv = (uint32_t)(tc < 0 ? 0 : tc); digits = 0;
    if (!uv){tmp[14]='0';digits=1;}else{i=14;while(uv){tmp[i--]='0'+(uv%10);uv/=10;digits++;}}
    kp_print((uint8_t)(23+(uint8_t)digits), 20, tmp + 15 - digits, red_body);

    kp_fill((uint8_t)(KPANIC_SROWS - 1), ' ', red_hdr);
    kp_print(2, (uint8_t)(KPANIC_SROWS - 1),
             "System halted. Check serial (COM1) for full dump.", red_hdr);

    /* Framebuffer fallback */
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

    /* Full serial register dump */
    serial_print("\n\n=== KERNEL PANIC ===\nREASON: ");
    serial_print(msg);
    serial_print("\n--- REGISTERS ---\n");
    for (int ri = 0; ri < 18; ri++) {
        serial_print(regs[ri].name);
        serial_print(": 0x");
        char hbuf[18]; hbuf[16] = '\n'; hbuf[17] = '\0';
        uint64_t hv = regs[ri].val;
        for (int hi = 15; hi >= 0; hi--) {
            uint8_t nibble = (uint8_t)(hv & 0xF);
            hbuf[hi] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
            hv >>= 4;
        }
        serial_print(hbuf);
    }
    serial_print("=== END PANIC ===\n");

    /* Drain the klog ring buffer to serial so crash context is preserved.
     * klog_flush() writes to VFS which may be unavailable; we do a raw
     * ring-buffer drain here instead using the klog emergency drain. */
    extern void klog_drain_to_serial(void);
    klog_drain_to_serial();

    for (;;) __asm__ volatile ("hlt");
}
