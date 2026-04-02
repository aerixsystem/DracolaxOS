/* kernel/cursor.h — Hardware cursor system (ARGB bitmap, 32x32) */
#ifndef CURSOR_DRIVER_H
#define CURSOR_DRIVER_H
#include "../../types.h"

#define CURSOR_W  32
#define CURSOR_DH 32   /* cursor pixel height — CURSOR_H reserved as include guard */

typedef enum {
    CURSOR_ARROW    = 0,
    CURSOR_HAND     = 1,
    CURSOR_TEXT     = 2,
    CURSOR_CROSSHAIR= 3,
    CURSOR_BUSY     = 4,
    CURSOR_RESIZE_H = 5,
    CURSOR_RESIZE_V = 6,
} cursor_type_t;

typedef struct {
    int           width, height;
    int           hotspot_x, hotspot_y;
    const uint32_t *pixels;
} cursor_def_t;

/* Initialise cursor system. Must be called after fb_init(). */
void cursor_init(void);

/* Set cursor shape. */
void cursor_set_type(cursor_type_t type);

/* Move cursor to pixel position (x, y) on screen.
 * Saves background, erases old cursor, draws new cursor. */
void cursor_move(uint32_t x, uint32_t y);

/* Show / hide cursor */
void cursor_show(void);
void cursor_hide(void);

/* Force redraw at current position (e.g. after screen update) */
void cursor_redraw(void);

/* Current position */
uint32_t cursor_x(void);
uint32_t cursor_y(void);

#endif /* CURSOR_DRIVER_H */
