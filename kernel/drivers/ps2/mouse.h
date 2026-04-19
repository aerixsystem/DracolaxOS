/* kernel/mouse.h — PS/2 Mouse driver */
#ifndef MOUSE_H
#define MOUSE_H
#include "../../types.h"

#define MOUSE_BTN_LEFT    0x01
#define MOUSE_BTN_RIGHT   0x02
#define MOUSE_BTN_MIDDLE  0x04

/* Initialise PS/2 mouse on IRQ12 */
void mouse_init(void);

/* Current screen position (clamped to fb bounds) */
int      mouse_get_x(void);
int      mouse_get_y(void);

/* Button state: bitmask of MOUSE_BTN_* */
uint8_t  mouse_get_buttons(void);

/* Edge-detect helpers: true on the FIRST frame a button is down/up */
int      mouse_btn_pressed (uint8_t mask);  /* pressed this frame   */
int      mouse_btn_released(uint8_t mask);  /* released this frame  */
int      mouse_btn_held    (uint8_t mask);  /* currently down       */
int      mouse_btn_released(uint8_t mask);  /* released this frame  */

/* Call once per frame BEFORE reading btn_pressed/released */
void     mouse_update_edges(void);

/** IRQ12 fire counter — incremented in the IRQ12 handler, read by watchdog. */
extern volatile uint32_t g_irq12_count;

/** Called by vmmouse to inject absolute position (skips PS/2 delta logic). */
void     mouse_set_pos    (int x, int y);
void     mouse_set_buttons(uint8_t b);

#endif /* MOUSE_H */
