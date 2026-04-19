/* kernel/keyboard.h — PS/2 keyboard driver
 *
 * Special key constants (KB_KEY_*) use values >= 0x80 so they never
 * collide with printable ASCII.  They are pushed into the ring buffer
 * and consumed by desktop / compositor input handlers.
 */
#ifndef KEYBOARD_H
#define KEYBOARD_H
#include "../../types.h"

/* ---- Special key codes (>= 0x80, safe above ASCII printable range) ------ */
#define KB_KEY_UP       0x80
#define KB_KEY_DOWN     0x81
#define KB_KEY_LEFT     0x82
#define KB_KEY_RIGHT    0x83
#define KB_KEY_HOME     0x84
#define KB_KEY_END      0x85
#define KB_KEY_PGUP     0x86
#define KB_KEY_PGDN     0x87
#define KB_KEY_INS      0x88
#define KB_KEY_DEL      0x89
#define KB_KEY_F1       0x8A
#define KB_KEY_F2       0x8B
#define KB_KEY_F3       0x8C
#define KB_KEY_F4       0x8D
#define KB_KEY_F5       0x8E
#define KB_KEY_F6       0x8F
#define KB_KEY_F7       0x90
#define KB_KEY_F8       0x91
#define KB_KEY_F9       0x92
#define KB_KEY_F10      0x93
#define KB_KEY_F11      0x94
#define KB_KEY_F12      0x95
#define KB_KEY_KPENTER  0x96
#define KB_KEY_KPSLASH  0x97
#define KB_KEY_ALTTAB   0x98   /* synthesised: Alt+Tab for task switcher */

/* Modifier-key indices (used only in key_state[], not pushed to ring) */
#define KB_KEY_SHIFT    0xA0
#define KB_KEY_CTRL     0xA1
#define KB_KEY_ALT      0xA2
#define KB_KEY_CAPS     0xA3   /* Caps Lock — key_state[KB_KEY_CAPS] = 1 when active */

/* ---- API ---------------------------------------------------------------- */

/** Initialise keyboard driver and register IRQ1 handler. */
void keyboard_init(void);

/** Non-blocking: dequeue one character / special code; returns 0 if empty.
 *  Return type is int (not char) so that KB_KEY_* values >= 0x80 compare
 *  correctly without an explicit cast: (int)0x80 == KB_KEY_UP is true,
 *  whereas (char)(int8_t)0x80 == KB_KEY_UP was always false on signed-char
 *  targets (GCC x86-64 default). */
int keyboard_getchar(void);

/** Blocking: spin until a character is available. Same widening rationale. */
int keyboard_read(void);

/** Poll whether a special key (KB_KEY_*) is currently held down. */
int  keyboard_key_down(uint8_t keycode);

/** Convenience: test modifier state (1 = pressed). */
int  keyboard_shift(void);
int  keyboard_ctrl(void);
int  keyboard_alt(void);
int  keyboard_caps_lock(void);   /* 1 when Caps Lock is active */

/** Re-enable PS/2 kbd IRQ after mouse init. Call once from main.c. */
void keyboard_reinit(void);

/** IRQ1 fire counter — incremented in the IRQ1 handler, read by watchdog. */
extern volatile uint32_t g_irq1_count;

#endif /* KEYBOARD_H */
