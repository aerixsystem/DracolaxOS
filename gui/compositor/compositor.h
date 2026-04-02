/* gui/compositor/compositor.h — Simple kernel-mode compositor */
#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include "../../kernel/types.h"
#include "../../kernel/drivers/vga/fb.h"

#define COMP_MAX_WINDOWS  16
#define COMP_TITLE_MAX    64

typedef struct window {
    uint32_t  x, y, w, h;   /* screen position and size  */
    uint32_t  z;             /* z-order (higher = in front) */
    char      title[COMP_TITLE_MAX];
    uint32_t *backbuf;       /* pixel buffer (w*h)         */
    int       visible;
    int       focused;
    int       desktop;       /* BUG FIX 1.10: virtual desktop index */
    uint32_t  border_color;
    uint32_t  title_color;
} window_t;

/* Create a window; returns handle ≥ 0 or -1 on failure */
int  comp_create_window(const char *title, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h);
void comp_destroy_window(int handle);
void comp_move_window   (int handle, uint32_t x, uint32_t y);
void comp_focus_window  (int handle);

/* Draw a string into a window's back buffer */
void comp_window_print(int handle, uint32_t x, uint32_t y,
                       const char *s, uint32_t fg);

/* Fill a window region */
void comp_window_fill(int handle, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint32_t color);

/* Virtual desktop control (BUG FIX 1.10) */
void comp_switch_desktop(int idx);
int  comp_current_desktop(void);

/* Composite all visible windows to framebuffer */
void comp_render(void);

/* Initialise compositor (requires fb_init to have run) */
void comp_init(void);

/* Run compositor main loop as a kernel task */
void comp_task(void);

#endif /* COMPOSITOR_H */
