#include "../../types.h"
#include "../../log.h"
#include "fb.h"
#include "gfx_driver.h"
static int  vi(void){return fb.available?0:-1;}
static void vp(uint32_t x,uint32_t y,uint32_t c){fb_put_pixel(x,y,c&0xFFFFFF);}
static void vr(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){fb_fill_rect(x,y,w,h,c&0xFFFFFF);}
static void vf(void){fb_flip();}
static void vn(void){}
static void vm(uint32_t a,uint32_t b,uint8_t c){(void)a;(void)b;(void)c;}
gfx_driver_t gfx_vesa_driver={"vesa-vram",vi,vm,vp,vr,vf,vn};
static gfx_driver_t *active=&gfx_vesa_driver;
void gfx_register(gfx_driver_t *d){if(d&&d->init&&d->init()==0){active=d;kinfo("GFX: '%s'\n",d->name);}}
gfx_driver_t *gfx_get(void){return active;}
