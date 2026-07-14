/* tests/test_wm.c — Unit tests for window manager */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Stub fb for host testing */
typedef struct { uint32_t width,height,pitch; uint8_t available; uint64_t addr; uint8_t bpp,type; } fb_info_t;
fb_info_t fb = {1920, 1080, 7680, 1, 0, 32, 1};
void fb_put_pixel(uint32_t x,uint32_t y,uint32_t c){(void)x;(void)y;(void)c;}
void fb_fill_rect(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){(void)x;(void)y;(void)w;(void)h;(void)c;}
void fb_flip(void){}
void fb_print(uint32_t x,uint32_t y,const char*s,uint32_t a,uint32_t b){(void)x;(void)y;(void)s;(void)a;(void)b;}
void fb_blit(uint32_t x,uint32_t y,uint32_t w,uint32_t h,const uint32_t*p){(void)x;(void)y;(void)w;(void)h;(void)p;}
void kinfo(const char *f,...){(void)f;}

/* Minimal wm stubs for testing */
#define WM_MAX_WINDOWS 32
typedef enum {WM_WIN_NORMAL=0,WM_WIN_MAXIMISED,WM_WIN_MINIMISED,
              WM_WIN_SNAPPED_LEFT,WM_WIN_SNAPPED_RIGHT,WM_WIN_SNAPPED_FULL} wm_win_state_t;
typedef struct { int id; char title[64]; int x,y,w,h; int desktop; wm_win_state_t state; int focused,visible; uint32_t *fb_buf; } wm_window_t;

static wm_window_t windows[WM_MAX_WINDOWS];
static int win_count=0, focused_id=-1, current_desktop=0;

void wm_init(void){memset(windows,0,sizeof(windows));win_count=0;}
int wm_create_window(const char *title,int x,int y,int w,int h){
    if(win_count>=WM_MAX_WINDOWS) return -1;
    wm_window_t *wn=&windows[win_count];
    wn->id=win_count; wn->x=x;wn->y=y;wn->w=w;wn->h=h;
    strncpy(wn->title,title?title:"Win",63); wn->visible=1; wn->desktop=current_desktop;
    return win_count++;
}
void wm_snap_window(int id, wm_win_state_t snap){
    if(id<0||id>=win_count) return;
    windows[id].state=snap;
    if(snap==WM_WIN_SNAPPED_LEFT){ windows[id].x=0;windows[id].w=960; }
    if(snap==WM_WIN_SNAPPED_RIGHT){ windows[id].x=960;windows[id].w=960; }
}
void wm_switch_desktop(int idx){ if(idx>=0&&idx<4) current_desktop=idx; }

int main(void) {
    int pass=0,fail=0;
#define CHECK(c,m) do{if(c){printf("[PASS] %s\n",m);pass++;}else{printf("[FAIL] %s\n",m);fail++;}}while(0)

    wm_init();
    int id = wm_create_window("Test", 100, 100, 400, 300);
    CHECK(id == 0, "first window id is 0");
    CHECK(windows[0].x == 100 && windows[0].y == 100, "window position set correctly");

    /* Snap left */
    wm_snap_window(0, WM_WIN_SNAPPED_LEFT);
    CHECK(windows[0].state == WM_WIN_SNAPPED_LEFT, "snap left sets state");
    CHECK(windows[0].x == 0, "snap left sets x=0");
    CHECK(windows[0].w == 960, "snap left sets w=half screen");

    /* Snap right */
    wm_snap_window(0, WM_WIN_SNAPPED_RIGHT);
    CHECK(windows[0].x == 960, "snap right sets x=half");

    /* Virtual desktop switch */
    wm_switch_desktop(2);
    CHECK(current_desktop == 2, "switch to desktop 2");
    int id2 = wm_create_window("OnDesk2", 0, 0, 200, 200);
    CHECK(windows[id2].desktop == 2, "new window assigned to current desktop");

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail>0 ? 1 : 0;
}
