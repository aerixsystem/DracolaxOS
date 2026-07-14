#ifndef GFX_DRIVER_H
#define GFX_DRIVER_H
#include "../../types.h"
typedef struct gfx_driver {
    const char *name;
    int  (*init)(void);
    void (*set_mode)(uint32_t w, uint32_t h, uint8_t bpp);
    void (*put_pixel)(uint32_t x, uint32_t y, uint32_t argb);
    void (*fill_rect)(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb);
    void (*flip)(void);
    void (*shutdown)(void);
} gfx_driver_t;
void gfx_register(gfx_driver_t *drv);
gfx_driver_t *gfx_get(void);
extern gfx_driver_t gfx_vesa_driver;
#endif
