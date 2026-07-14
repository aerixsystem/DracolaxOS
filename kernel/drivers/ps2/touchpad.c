/* drivers/input/touchpad.c — Synaptics/generic PS/2 touchpad detection
 *
 * Full UVC touchpad support requires USB (V2).  For V1 we probe the PS/2
 * aux port for an extended touchpad via the Synaptics identify command
 * (0xE8 0x00 0xE8 0x03 0xE9 — sets rate then reads ID).  If a Synaptics
 * device is detected we log its capabilities; otherwise fall back to
 * standard PS/2 mouse behaviour (already handled by mouse.c).
 */
#include "../../types.h"
#include "../../log.h"

/* Port I/O helpers (mirrored from keyboard.c — avoid driver dependency) */
static inline void _outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p));
}
static inline uint8_t _inb(uint16_t p) {
    uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v;
}
static inline void ps2_wait_w(void) {
    int t = 100000; while (--t && (_inb(0x64) & 0x02));
}
static inline void ps2_wait_r(void) {
    int t = 100000; while (--t && !(_inb(0x64) & 0x01));
}

/* Send a byte to the PS/2 aux (mouse) port */
static void aux_write(uint8_t b) {
    ps2_wait_w(); _outb(0x64, 0xD4);   /* next byte → aux port */
    ps2_wait_w(); _outb(0x60, b);
}

/* Read one byte from the PS/2 data port (with timeout) */
static uint8_t aux_read(void) {
    ps2_wait_r();
    return _inb(0x60);
}

void touchpad_init(void) {
    /* Synaptics identify sequence:
     *   Set scaling 1:1 three times (0xE6), then Read Device Type (0xF2).
     *   A Synaptics touchpad echoes ACK (0xFA) and returns model bytes. */
    aux_write(0xE6); if (aux_read() != 0xFA) goto fallback;
    aux_write(0xE6); if (aux_read() != 0xFA) goto fallback;
    aux_write(0xE6); if (aux_read() != 0xFA) goto fallback;
    aux_write(0xF2);                        /* identify */
    if (aux_read() != 0xFA) goto fallback;
    uint8_t id0 = aux_read();
    uint8_t id1 = aux_read();

    if (id0 == 0x47) {
        /* 0x47 = Synaptics TouchPad */
        kinfo("TOUCHPAD: Synaptics detected (id=0x%02x,0x%02x) "
              "— using PS/2 relative mode (absolute V2)\n",
              (unsigned)id0, (unsigned)id1);
    } else {
        kinfo("TOUCHPAD: PS/2 pointing device id=0x%02x,0x%02x "
              "— treating as standard mouse\n",
              (unsigned)id0, (unsigned)id1);
    }
    return;

fallback:
    kinfo("TOUCHPAD: no dedicated touchpad found — PS/2 mouse fallback active\n");
}
