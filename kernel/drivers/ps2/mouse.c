/* kernel/mouse.c — PS/2 Mouse driver
 *
 * Initialisation sequence:
 *   1. Disable both PS/2 devices
 *   2. Read + patch controller config (enable aux IRQ, FORCE translation ON)
 *   3. Re-enable auxiliary device
 *   4. Send RESET + SET_DEFAULT + ENABLE_STREAM to mouse
 *   5. Re-enable keyboard device
 *   6. Register IRQ12 (vector 44) handler
 *
 * Packet format (3-byte, standard PS/2):
 *   Byte 0: flags  bit3=always1 bit2=mid bit1=right bit0=left
 *                  bit4=Xsign  bit5=Ysign bit6=Xovf bit7=Yovf
 *   Byte 1: X delta (magnitude; sign in byte0 bit4)
 *   Byte 2: Y delta (magnitude; sign in byte0 bit5) — screen Y is INVERTED
 */
#include "../../types.h"
#include "mouse.h"
#include "../../arch/x86_64/irq.h"
#include "../../arch/x86_64/pic.h"
#include "../vga/fb.h"
#include "../../log.h"

/* ---- PS/2 ports & commands ---------------------------------------------- */
#define PS2_DATA        0x60
#define PS2_STATUS      0x64
#define PS2_CMD         0x64

#define PS2_DISABLE_KB  0xAD
#define PS2_ENABLE_KB   0xAE
#define PS2_DISABLE_AUX 0xA7
#define PS2_ENABLE_AUX  0xA8
#define PS2_READ_CFG    0x20
#define PS2_WRITE_CFG   0x60
#define PS2_WRITE_AUX   0xD4  /* next byte to 0x60 goes to mouse */

#define MOUSE_RESET     0xFF
#define MOUSE_DEFAULT   0xF6
#define MOUSE_ENABLE    0xF4
#define MOUSE_ACK       0xFA
#define MOUSE_SELF_OK   0xAA

/* ---- port I/O ----------------------------------------------------------- */
static inline uint8_t inb(uint16_t p)           { uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void    outb(uint16_t p, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }

static void ps2_wait_write(void) {
    int t = 100000;
    while (--t && (inb(PS2_STATUS) & 0x02));
}
static void ps2_wait_read(void) {
    int t = 100000;
    while (--t && !(inb(PS2_STATUS) & 0x01));
}
static void ps2_cmd(uint8_t cmd) {
    ps2_wait_write(); outb(PS2_CMD, cmd);
}
static void ps2_write_mouse(uint8_t byte) {
    ps2_cmd(PS2_WRITE_AUX);
    ps2_wait_write(); outb(PS2_DATA, byte);
}
static uint8_t ps2_read(void) {
    ps2_wait_read(); return inb(PS2_DATA);
}

/* ---- IRQ counter (read from irq_watchdog_task in init.c) ---------------- */
volatile uint32_t g_irq12_count = 0;

/* ---- mouse state -------------------------------------------------------- */
static volatile int     mx = 0, my = 0;
static volatile uint8_t mbuttons = 0;
static volatile uint8_t mbuttons_prev = 0;

static uint8_t pkt[3];
static uint8_t pkt_idx = 0;
static uint32_t pkt_timeout = 0;  /* tick count when last byte arrived */

static void mouse_handler(struct isr_frame *f) {
    (void)f;
    g_irq12_count++;
    uint8_t st = inb(PS2_STATUS);
    if (!(st & 0x01)) return;   /* output buffer empty */
    if (!(st & 0x20)) return;   /* aux bit NOT set — keyboard byte on IRQ12, discard */
    uint8_t byte = inb(PS2_DATA);

    /* Timeout desync recovery: if too many timer ticks have elapsed since
     * the last mouse byte, the packet state machine is probably desynced
     * (e.g. a keyboard byte was misread as a mouse byte).  Reset to byte 0.
     * 50 ticks @ 100Hz = 500ms — any real packet arrives within ~10ms. */
    uint32_t now = pit_ticks();
    if (pkt_idx > 0 && (now - pkt_timeout) > 50) {
        pkt_idx = 0;
    }
    pkt_timeout = now;

    /* First byte must have bit3 set; skip desynced bytes */
    if (pkt_idx == 0 && !(byte & 0x08)) return;

    pkt[pkt_idx++] = byte;
    if (pkt_idx < 3) return;
    pkt_idx = 0;

    uint8_t flags = pkt[0];

    /* Discard overflow packets */
    if ((flags & 0xC0)) return;

    /* Signed deltas: extend from 9-bit (byte + sign bit in flags) */
    int dx = (int)(int8_t)pkt[1];
    int dy = -(int)(int8_t)pkt[2];   /* screen Y inverted vs mouse Y */

    int nx = mx + dx;
    int ny = my + dy;

    int max_x = (int)fb.width  - 1;
    int max_y = (int)fb.height - 1;
    if (nx < 0) nx = 0;
    if (nx > max_x) nx = max_x;
    if (ny < 0) ny = 0;
    if (ny > max_y) ny = max_y;

    mx = nx;
    my = ny;
    mbuttons = flags & 0x07;
}

/* ---- init --------------------------------------------------------------- */
void mouse_init(void) {
    /* Disable both devices during setup */
    ps2_cmd(PS2_DISABLE_KB);
    ps2_cmd(PS2_DISABLE_AUX);

    /* Flush output buffer */
    for (int i = 0; i < 16 && (inb(PS2_STATUS) & 0x01); i++)
        inb(PS2_DATA);

    /* Read config.  Force all three critical bits in one write:
     *   bit0 = kbd IRQ1 enable  (may be off on VirtualBox / real hw)
     *   bit1 = aux IRQ12 enable
     *   bit6 = kbd scancode translation ON
     *          MUST be explicitly set here — if BIOS/GRUB left it clear the
     *          controller passes raw set-2 codes; our driver expects set-1.
     *          With translation off every key maps wrong and the driver state
     *          machine eventually diverges and goes silent.
     * Also clear bit5 (aux clock disable) to enable the mouse port. */
    ps2_cmd(PS2_READ_CFG);
    uint8_t cfg = ps2_read();
    cfg |=  0x43;   /* bit0=kbd IRQ1, bit1=aux IRQ12, bit6=kbd translation */
    cfg &= ~0x20;   /* bit5=clear aux clock disable */
    ps2_cmd(PS2_WRITE_CFG);
    ps2_wait_write(); outb(PS2_DATA, cfg);

    /* Readback: confirm bit6 stuck (some emulators/controllers ignore it) */
    ps2_cmd(PS2_READ_CFG);
    uint8_t cfg_v = ps2_read();
    if (!(cfg_v & 0x40))
        kwarn("MOUSE: PS/2 translation bit6 did not stick (cfg=0x%02x) "
              "— keyboard scancodes may be misread\n", (unsigned)cfg_v);

    /* Enable mouse device */
    ps2_cmd(PS2_ENABLE_AUX);

    /* Reset mouse */
    ps2_write_mouse(MOUSE_RESET);
    uint8_t ack = ps2_read();          /* 0xFA */
    uint8_t self_test = ps2_read();    /* 0xAA */
    uint8_t device_id = ps2_read();    /* 0x00 for standard */
    (void)ack; (void)self_test; (void)device_id;

    /* Set defaults (100 samples/s, 4 counts/mm, 1:1 scaling) */
    ps2_write_mouse(MOUSE_DEFAULT);
    ps2_read(); /* ACK */

    /* Enable streaming */
    ps2_write_mouse(MOUSE_ENABLE);
    ps2_read(); /* ACK */

    /* Second authoritative config write: some controllers reset config bits
     * during the mouse init sequence (reset + enable-stream).  Re-lock bits
     * 0, 1, and 6 now that the mouse is fully initialised. */
    ps2_cmd(PS2_READ_CFG);
    cfg = ps2_read();
    cfg |=  0x43;
    cfg &= ~0x20;
    ps2_cmd(PS2_WRITE_CFG);
    ps2_wait_write(); outb(PS2_DATA, cfg);

    /* Re-enable keyboard */
    ps2_cmd(PS2_ENABLE_KB);

    /* Register IRQ12 handler */
    irq_register(44, mouse_handler);
    pic_unmask(12);

    /* Start in center of screen */
    mx = (int)fb.width  / 2;
    my = (int)fb.height / 2;

    /* Re-assert keyboard IRQ1 unmask — mouse init may have touched PIC */
    pic_unmask(1);

    kinfo("MOUSE: PS/2 init OK (%dx%d start)\n", mx, my);
}

/* ---- public API --------------------------------------------------------- */
int     mouse_get_x(void)       { return mx; }
int     mouse_get_y(void)       { return my; }
uint8_t mouse_get_buttons(void) { return mbuttons; }

/* Called by vmmouse.c to inject absolute coordinates */
void mouse_set_pos(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (fb.available) {
        if (x > (int)fb.width  - 1) x = (int)fb.width  - 1;
        if (y > (int)fb.height - 1) y = (int)fb.height - 1;
    }
    mx = x;
    my = y;
}

void mouse_set_buttons(uint8_t b) {
    mbuttons = b;
}

void mouse_update_edges(void) {
    mbuttons_prev = mbuttons;
}

int mouse_btn_pressed(uint8_t mask) {
    return (int)((mbuttons & mask) && !(mbuttons_prev & mask));
}
int mouse_btn_released(uint8_t mask) {
    return (int)(!(mbuttons & mask) && (mbuttons_prev & mask));
}
int mouse_btn_held(uint8_t mask) {
    return (int)((mbuttons & mask) != 0);
}
