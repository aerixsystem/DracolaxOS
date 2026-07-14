/* gui/desktop/default-desktop/desktop.c
 * DracolaxOS Glassmorphism Desktop — v3.0
 *
 * Changes from v2:
 *   • Dock removed — apps launched from search or desktop icon grid.
 *   • Widgets panel (top-right): username + clock. Extensible.
 *   • Desktop icon grid: shortcuts on 96px cells, double-click launches.
 *   • Super key shortcuts:
 *       Super+S   → toggle search overlay
 *       Super+W   → debug test window
 *       Super+B   → toggle background
 *       F1-F4     → switch workspace
 *   • Bug fixes:
 *       [BUG2] Window [X] sends ESC to app task via input_router_push_to
 *              so readline unblocks and app exits cleanly — no freeze.
 *       [BUG3] Window title/move/resize/minimize/maximize all work.
 *       [BUG4] ctx_menu_draw called inside draw block (before flip).
 *       [BUG5] Arrow keys guarded behind !g_search_open; Up/Down in
 *              search selects the result list, not moves the cursor.
 */

#include "../../../kernel/types.h"
#include "../../../kernel/drivers/vga/fb.h"
#include "../../../kernel/drivers/vga/vga.h"
#include "../../../kernel/sched/sched.h"
#include "../../../kernel/drivers/ps2/keyboard.h"
#include "../../../kernel/drivers/ps2/mouse.h"
#include "../../../kernel/drivers/ps2/vmmouse.h"
#include "../../../kernel/drivers/ps2/input_router.h"
#include "../../../kernel/klibc.h"
#include "../../../kernel/log.h"
#include "../../../kernel/arch/x86_64/pic.h"
#include "../../../kernel/security/dracoauth.h"
#include "../../compositor/compositor.h"
#include "../../widgets/widgets.h"
#include "../../wm/wm.h"
#include "appman.h"
#include "../../../apps/installer/installer.h"
#include "../../../services/power_manager.h"
#include "../../../kernel/draco_logo.h"
#include "../../../kernel/dxi/dxi.h"
#include "../../../kernel/drivers/vga/cursor.h"
#include "../../../kernel/arch/x86_64/rtc.h"
#include "ws_switcher.h"
#include "dock.h"
#include "desktop.h"
#include "background.h"
#include "ctx_resolver.h"
#include "ctx_menu.h"

/* Super+letter keycode emitted by keyboard driver: 0xC0|(letter-'a') */
#define SUPER_KEY(ch) ((int)(0xC0u | (uint8_t)((ch)-'a')))
/* Ctrl+Super+letter (see kernel/drivers/ps2/keyboard.c) — same
 * always-reaches-the-desktop bypass as SUPER_KEY, distinct encoded range. */
#define CTRL_SUPER_KEY(ch) ((int)(0xE0u | (uint8_t)((ch)-'a')))

/* =========================================================================
 * Palette
 * ========================================================================= */
#define COL_VOID        0x04040Cu
#define COL_GLASS_BG    0x0F1020u
#define COL_GLASS_PANEL 0x1A1D3Au
#define COL_GLASS_EDGE  0x3A3F7Au
#define COL_GLASS_SHINE 0x5A60C0u
#define COL_ACCENT      0x7828C8u
#define COL_ACCENT_LT   0xA050F0u
#define COL_ACCENT_DIM  0x3A1460u
#define COL_TEXT_HI     0xF0F0FFu
#define COL_TEXT_MED    0xA0A0C8u
#define COL_TEXT_DIM    0x60607Au
#define COL_OK          0x28C878u
#define COL_ERR         0xC82828u
#define COL_SEP         0x2A2C50u
#define BG_OVERLAY_R    4u
#define BG_OVERLAY_G    4u
#define BG_OVERLAY_B    16u
#define FONT_W          8
#define FONT_H          16
#define CORNER_R        8

/* ── Cached RTC time (update once per second) ──────────────────────────── */
static rtc_time_t g_cached_time = {0,0,0,0,0,0};
/* BUG FIX (clock frozen after first tick): this used to be
 * "g_last_rtc_sec = g_cached_time.sec" set immediately after every RTC
 * read, then gated the next read on
 * "g_cached_time.sec != g_last_rtc_sec" — comparing the cached value to
 * a copy of ITSELF. That's true only on the very first call (when
 * g_last_rtc_sec starts at -1) and never again afterward, so the RTC was
 * read exactly once, ever, and the clock displayed that one frozen time
 * forever after. Fixed by gating on elapsed frames (this desktop loop
 * runs at ~30fps via sched_sleep(33)) instead of comparing state to
 * itself. */
static int g_rtc_tick_last = -1000;
#define RTC_REREAD_INTERVAL_TICKS 30   /* ~1s at the desktop's ~33ms frame pace */

/* forward declare so desktop_blit_bg_at can reference it */
static void blit_bg_scaled(uint32_t dx0,uint32_t dy0,uint32_t dw,uint32_t dh,uint32_t oa);
static uint32_t g_bg_overlay  = 70u;
static int      g_bg_disabled = 0;

void desktop_blit_bg_at(uint32_t x,uint32_t y,uint32_t w,uint32_t h){
    if(g_bg_disabled){fb_fill_rect(x,y,w,h,0x202020u);return;}
#ifndef DRACO_STABLE
    blit_bg_scaled(x,y,w,h,g_bg_overlay);
#else
    fb_fill_rect(x,y,w,h,COL_GLASS_BG);
#endif
}

/* =========================================================================
 * UI state
 * ========================================================================= */
#define NUM_WORKSPACES  WS_COUNT
static int  g_ws=0, g_logged_in=0, g_ticks=0;
static int  g_cx=0, g_cy=0, g_mouse_x_raw=-1, g_mouse_y_raw=-1;
static int  g_wallpaper_dirty=1;
static int  desktop_tid = 0;  /* this task's scheduler ID — set at startup */
static int  g_drag_win=-1, g_drag_off_x=0, g_drag_off_y=0;
/* BUG FIX (desktop icons couldn't be dragged to another grid cell): this
 * never existed at all — icon click launched the app immediately on
 * mouse PRESS, with no concept of drag vs. click. g_icon_drag_idx tracks
 * a press that landed on an icon; g_icon_dragged flips true once the
 * mouse has moved more than a small threshold, at which point release
 * repositions the icon to the nearest grid cell instead of launching it
 * (see the lheld/lclick handling below). A press that releases without
 * crossing that threshold is treated as a plain click, same as before. */
static int  g_icon_drag_idx=-1, g_icon_dragged=0;
static int  g_icon_drag_start_cx=0, g_icon_drag_start_cy=0;
static int  g_resize_win=-1;
static resize_edge_t g_resize_edge=RESIZE_NONE;
static int  g_resize_ox=0,g_resize_oy=0,g_resize_ow=0,g_resize_oh=0;
static int  g_resize_mx=0,g_resize_my=0;
static int  g_about_open=0;
/* GUI debug mode (Ctrl+Super+D) — see draw_debug_panel() below.
 * g_dbg_show_hitboxes is fully implemented (comp_debug_draw_hitboxes()).
 * The other two are real, working TOGGLES (persisted, checkbox actually
 * flips) but currently have no overlay wired up yet — implementing
 * genuine dirty-region-flash and per-widget layout-bounds visualization
 * needs every draw call to also record its own bounds for later replay,
 * which nothing in the compositor or widget toolkit does today. Rather
 * than silently drop them, they're left as explicit, working checkboxes
 * with a clear TODO marking what real data they'd need — not absent,
 * just not wired to a visual effect yet. */
static int  g_debug_open=0;
static int  g_dbg_show_hitboxes=0;
static int  g_dbg_show_render_update=0;   /* TODO: needs per-frame dirty-rect tracking */
static int  g_dbg_show_layout_bounds=0;   /* TODO: needs per-widget bounds recording */

/* Search */
static int  g_search_open=0;
#define SEARCH_BUF 64
static char g_search_query[SEARCH_BUF]="";
#define SEARCH_MAX_RESULTS 8
static char g_sr_names[SEARCH_MAX_RESULTS][APP_NAME_LEN];
static int  g_sr_count=0, g_sr_sel=0;

/* Login */
#define LOGIN_BUF 64
static char g_login_user[LOGIN_BUF]="root";
static char g_login_pass[LOGIN_BUF]="";
static int  g_login_field=1;
static char g_login_msg[128]="";
static int  g_login_msg_err=0;

/* =========================================================================
 * Desktop icon grid
 * ========================================================================= */
#define ICON_CELL_W     96
#define ICON_CELL_H     96
#define ICON_SZ         48
#define ICON_GRID_X     24
#define ICON_GRID_Y     24
#define DESKTOP_ICON_MAX 20
#define DCLICK_TICKS    30   /* ~1 s at 33ms/frame */

typedef struct { char name[APP_NAME_LEN]; int col,row,active; } desktop_icon_t;
static desktop_icon_t g_icons[DESKTOP_ICON_MAX];
static int g_icon_count=0;

/* Per-slot DXI icon cache — loaded once from VFS on first draw */
static uint32_t    g_dxi_pixels[DESKTOP_ICON_MAX][ICON_SZ * ICON_SZ];
static dxi_icon_t  g_dxi_icons [DESKTOP_ICON_MAX];
static int         g_dxi_loaded = 0;  /* 0 = not yet attempted */

static void desktop_icons_init(void){
    /* GUI finalization pass: Terminal, File Manager, Text Editor,
     * Calculator, Settings, System Monitor, Package Manager, Media
     * Player, Paint, and Image Viewer were all removed from the OS (see
     * docs/CHANGELOG.md) — their desktop icons go with them. Widget Demo
     * is the only currently-registered app, so it's the only icon; the
     * app launcher/search (Super+S) still lists everything in the
     * appman registry generically and will pick up new apps
     * automatically as they're reimplemented and re-registered. */
    static const char *defs[]={
        "Widget Demo",NULL
    };
    g_icon_count=0;
    for(int i=0;defs[i]&&g_icon_count<DESKTOP_ICON_MAX;i++){
        strncpy(g_icons[g_icon_count].name,defs[i],APP_NAME_LEN-1);
        g_icons[g_icon_count].name[APP_NAME_LEN-1]='\0';
        g_icons[g_icon_count].col=g_icon_count%2;
        g_icons[g_icon_count].row=g_icon_count/2;
        g_icons[g_icon_count].active=1;
        g_icon_count++;
    }
    /* Reset DXI cache */
    for(int i=0;i<DESKTOP_ICON_MAX;i++){
        g_dxi_icons[i].pixels = g_dxi_pixels[i];
        g_dxi_icons[i].loaded = 0;
    }
    g_dxi_loaded = 0;
}

/* Convert "App Name" → "app-name.dxi" path */
static void name_to_dxi_path(const char *name, char *out, size_t max){
    char slug[64]; int j=0;
    for(int i=0;name[i]&&j<59;i++){
        char c=name[i];
        if(c>='A'&&c<='Z') c=(char)(c+32);
        if(c==' ') c='-';
        slug[j++]=c;
    }
    slug[j]='\0';
    snprintf(out, max, "/storage/main/system/shared/images/%s.dxi", slug);
}

static void load_dxi_icons(void){
    if(g_dxi_loaded) return;
    g_dxi_loaded = 1;  /* only attempt once */
    for(int i=0;i<g_icon_count;i++){
        char path[128];
        name_to_dxi_path(g_icons[i].name, path, sizeof(path));
        g_dxi_icons[i].pixels = g_dxi_pixels[i];
        g_dxi_icons[i].loaded = 0;
        dxi_load(path, &g_dxi_icons[i]);
        sched_yield();  /* avoid starving watchdog during icon scan */
    }
}

static void icon_cell_pos(int col,int row,int *x,int *y){
    *x=ICON_GRID_X+col*ICON_CELL_W;
    *y=ICON_GRID_Y+row*ICON_CELL_H;
}

typedef struct{uint32_t bg,fg;const char *sym;}app_icon_style_t;
static const app_icon_style_t _def_style={0x2A2C50u,0xA0A0C8u,"??"};
/* BUG FIX (no real icons): desktop icons, dock entries, and window title
 * bars previously fell back to a plain 2-letter text abbreviation
 * (get_app_style()'s .sym field, e.g. "Wd") whenever no .dxi bitmap icon
 * was found on disk — which is always, right now, since no app ships a
 * real bitmap icon file. Authoring actual bitmap/DXI assets isn't
 * possible in this environment (no image pipeline here), so instead:
 * real, distinct, HAND-DRAWN VECTOR icons, composed from the same rect/
 * rounded-rect primitives already used everywhere else in this file
 * (fb_fill_rect/fb_rounded_rect — no diagonal-line primitive exists in
 * fb.h, so every icon here is deliberately axis-aligned). Not a
 * placeholder: these are real, rendered, visually distinct icons per
 * app — just vector instead of bitmap. Falls back to a generic "app
 * window" pictogram (rounded frame + title strip + two corner dots) for
 * any name without a specific icon, so newly-added apps always get
 * something better than a blank/2-letter glyph even before anyone
 * bothers to draw them a dedicated icon.
 *
 * (x,y) is the icon's top-left, sz its width/height (square). */
void desktop_draw_vector_icon(const char *name, uint32_t x, uint32_t y,
                             uint32_t sz, uint32_t fg, uint32_t bg){
    uint32_t r = sz/5u;
    fb_rounded_rect(x,y,sz,sz,r,bg);

    if(strcmp(name,"Widget Demo")==0){
        /* Pictogram: a slider + a button + a checkbox, echoing the app's
         * own content — three small controls stacked inside the frame. */
        uint32_t pad=sz/6u, iw=sz-2u*pad;
        /* slider track + thumb */
        uint32_t sy=y+pad;
        fb_fill_rect(x+pad, sy+iw/6u, iw, iw/10u>1u?iw/10u:1u, fg);
        fb_fill_rect(x+pad+iw/3u, sy, iw/6u, iw/3u, fg);
        /* button bar */
        uint32_t by=y+pad+iw/2u;
        fb_rounded_rect(x+pad, by, iw, iw/4u, iw/8u, fg);
        /* checkbox */
        uint32_t chy=y+sz-pad-iw/4u;
        fb_rounded_rect(x+pad, chy, iw/4u, iw/4u, 2u, fg);
        fb_fill_rect(x+pad+iw/4u+2u, chy+2u, iw-iw/4u-4u, iw/4u-4u, fb_blend(fg,bg,140u));
    } else {
        /* Generic "app window" pictogram — rounded frame with a title-bar
         * strip along the top and two small corner dots (echoing a
         * window's close/minimize buttons), used for any app that
         * doesn't have a dedicated icon drawn for it yet. */
        uint32_t pad=sz/6u;
        uint32_t fx=x+pad, fy=y+pad, fw=sz-2u*pad, fh=sz-2u*pad;
        uint32_t tb=fh/4u>2u?fh/4u:2u;
        fb_rounded_rect(fx,fy,fw,fh,fh/8u,fg);
        fb_fill_rect(fx+2u,fy+2u,fw-4u,tb,fb_blend(fg,bg,140u));
        uint32_t dot=tb/2u>2u?tb/2u:2u;
        fb_fill_rect(fx+fw-2u*dot-3u,fy+2u+(tb-dot)/2u,dot,dot,bg);
        fb_fill_rect(fx+fw-4u*dot-6u,fy+2u+(tb-dot)/2u,dot,dot,bg);
    }
}

static app_icon_style_t get_app_style(const char *name){
    /* GUI finalization pass: all entries for removed apps were dropped
     * (see docs/CHANGELOG.md). Unrecognised names fall back to
     * _def_style, so re-adding an app here is optional polish, not
     * required for it to display. */
    struct{const char *key;app_icon_style_t s;}tbl[]={
        {"Widget Demo",    {0x2A0A3Au,0xC080FFu,"Wd"}},
    };
    for(size_t i=0;i<sizeof(tbl)/sizeof(tbl[0]);i++)
        if(strcmp(tbl[i].key,name)==0)return tbl[i].s;
    return _def_style;
}

static void draw_desktop_icons(void){
    if(!g_dxi_loaded) load_dxi_icons();

    for(int i=0;i<g_icon_count;i++){
        if(!g_icons[i].active)continue;
        int cx,cy; icon_cell_pos(g_icons[i].col,g_icons[i].row,&cx,&cy);
        int hover=(g_cx>=cx&&g_cx<cx+ICON_CELL_W&&g_cy>=cy&&g_cy<cy+ICON_CELL_H);

        /* Hover/selected cell background */
        if(hover) fb_rounded_rect((uint32_t)cx,(uint32_t)cy,
                                  (uint32_t)ICON_CELL_W,(uint32_t)ICON_CELL_H,
                                  12u, 0x20244Au);

        uint32_t ix=(uint32_t)(cx+(ICON_CELL_W-ICON_SZ)/2);
        uint32_t iy=(uint32_t)(cy+8);

        if(g_dxi_icons[i].loaded){
            /* Render DXI icon — circular bg then blit pixels */
            app_icon_style_t st=get_app_style(g_icons[i].name);
            fb_rounded_rect(ix, iy, (uint32_t)ICON_SZ, (uint32_t)ICON_SZ,
                            (uint32_t)ICON_SZ/2u, st.bg);
            uint32_t iw=g_dxi_icons[i].width, ih=g_dxi_icons[i].height;
            if(iw>(uint32_t)ICON_SZ) iw=(uint32_t)ICON_SZ;
            if(ih>(uint32_t)ICON_SZ) ih=(uint32_t)ICON_SZ;
            blit_icon_bgra(ix+(uint32_t)ICON_SZ/2u-iw/2u,
                           iy+(uint32_t)ICON_SZ/2u-ih/2u,
                           g_dxi_icons[i].pixels, iw, ih, fb.width);
        } else {
            /* BUG FIX (no real icons): real hand-drawn vector icon
             * instead of a flat rounded square + 2-letter symbol — see
             * draw_vector_icon() above. */
            app_icon_style_t st=get_app_style(g_icons[i].name);
            desktop_draw_vector_icon(g_icons[i].name, ix, iy, (uint32_t)ICON_SZ, st.fg, st.bg);
        }

        /* Label under icon — truncate with '~' if too wide for the cell */
        const char *nm = g_icons[i].name;
        int max_chars = (ICON_CELL_W - 4) / FONT_W;
        char label[APP_NAME_LEN + 1];
        int nmlen = (int)strlen(nm);
        if(nmlen <= max_chars){
            strncpy(label, nm, APP_NAME_LEN); label[APP_NAME_LEN]='\0';
        } else {
            strncpy(label, nm, (size_t)(max_chars-1));
            label[max_chars-1]='~'; label[max_chars]='\0';
        }
        uint32_t lw=(uint32_t)strlen(label)*(uint32_t)FONT_W;
        uint32_t lx=(lw<(uint32_t)ICON_CELL_W)
                    ?(uint32_t)cx+((uint32_t)ICON_CELL_W-lw)/2u
                    :(uint32_t)cx+2u;
        uint32_t ly=(uint32_t)(cy+8+ICON_SZ+4);
        fb_print(lx,ly,label,hover?COL_TEXT_HI:COL_TEXT_MED,0u);
    }
}

static int icon_at(int x,int y){
    for(int i=0;i<g_icon_count;i++){
        if(!g_icons[i].active)continue;
        int cx,cy; icon_cell_pos(g_icons[i].col,g_icons[i].row,&cx,&cy);
        if(x>=cx&&x<cx+ICON_CELL_W&&y>=cy&&y<cy+ICON_CELL_H)return i;
    }
    return -1;
}

/* =========================================================================
 * Widgets — top-right: clock + username
 * ========================================================================= */
#define WIDGET_W   160
#define WIDGET_H    52
#define WIDGET_PAD  10
static void draw_widgets(void){
    if(!fb.available)return;

    /* Polished to use gui/widgets/ instead of hand-drawn fb_* calls (see
     * docs/CHANGELOG.md) — this is desktop CHROME (no compositor window
     * of its own), so it uses a raw-mode widget_ctx_t
     * (widget_ctx_init_raw(), see widgets.h) which draws straight to the
     * framebuffer at absolute coordinates instead of into a window
     * backbuffer. Only the non-interactive drawing functions
     * (widget_rect/widget_label) are valid in raw mode — there's no
     * window to hit-test a mouse against, so this widget stays purely
     * decorative, matching its previous behaviour. The rounded outer
     * border has no equivalent in the widget toolkit (it only does
     * sharp-cornered rects), so that one decorative flourish stays a
     * direct fb_rounded_rect() call. */
    static widget_ctx_t rctx;
    widget_ctx_init_raw(&rctx);

    uint32_t wx=fb.width-(uint32_t)WIDGET_W-(uint32_t)WIDGET_PAD;
    uint32_t wy=(uint32_t)WIDGET_PAD;
    widget_rect(&rctx,(int)wx,(int)wy,WIDGET_W,WIDGET_H,COL_GLASS_PANEL,1);
    fb_rounded_rect(wx,wy,(uint32_t)WIDGET_W,(uint32_t)WIDGET_H,
                    (uint32_t)CORNER_R,COL_GLASS_EDGE);
    widget_rect(&rctx,(int)(wx+(uint32_t)CORNER_R),(int)wy,
               WIDGET_W-2*CORNER_R,1,COL_GLASS_SHINE,1);

    /* Only hit the RTC hardware once per second — see the g_rtc_tick_last
     * comment above for why this used to freeze after the first read. */
    if(g_ticks - g_rtc_tick_last >= RTC_REREAD_INTERVAL_TICKS || g_rtc_tick_last < -500) {
        rtc_read(&g_cached_time);
        g_rtc_tick_last = g_ticks;
    }
    char clk[9];
    clk[0]=(char)('0'+(g_cached_time.hour/10)%10); clk[1]=(char)('0'+g_cached_time.hour%10);
    clk[2]=':';
    clk[3]=(char)('0'+(g_cached_time.min/10)%10);  clk[4]=(char)('0'+g_cached_time.min%10);
    clk[5]=':';
    clk[6]=(char)('0'+(g_cached_time.sec/10)%10);  clk[7]=(char)('0'+g_cached_time.sec%10);
    clk[8]='\0';
    uint32_t cw=8u*(uint32_t)FONT_W;
    widget_label(&rctx,(int)(wx+((uint32_t)WIDGET_W-cw)/2u),(int)(wy+6u),clk,COL_TEXT_HI);
    const char *who=dracoauth_whoami();
    char who12[13]; strncpy(who12,who,12); who12[12]='\0';
    uint32_t uw=(uint32_t)strlen(who12)*(uint32_t)FONT_W;
    widget_label(&rctx,(int)(wx+((uint32_t)WIDGET_W-uw)/2u),(int)(wy+28u),who12,COL_TEXT_MED);
}

/* =========================================================================
 * Drawing helpers
 * ========================================================================= */
static void hline(uint32_t x,uint32_t y,uint32_t w,uint32_t c){fb_fill_rect(x,y,w,1,c);}

static void glass_panel(uint32_t x,uint32_t y,uint32_t w,uint32_t h,
                         uint32_t bc,uint32_t fc){
    uint32_t ir=CORNER_R>2?CORNER_R-2:1;
    fb_rounded_rect(x+1,y+1,w-2,h-2,ir,fc);
    hline(x+CORNER_R,y,w-2*CORNER_R,bc);
    hline(x+CORNER_R,y+h-1,w-2*CORNER_R,bc);
    fb_fill_rect(x,y+CORNER_R,1,h-2*CORNER_R,bc);
    fb_fill_rect(x+w-1,y+CORNER_R,1,h-2*CORNER_R,bc);
    for(uint32_t i=1;i<=(uint32_t)CORNER_R;i++){
        fb_put_pixel(x+CORNER_R-i,y+CORNER_R-i,bc);
        fb_put_pixel(x+w-CORNER_R+i-1,y+CORNER_R-i,bc);
        fb_put_pixel(x+CORNER_R-i,y+h-CORNER_R+i-1,bc);
        fb_put_pixel(x+w-CORNER_R+i-1,y+h-CORNER_R+i-1,bc);
    }
    hline(x+CORNER_R,y,w-2*CORNER_R,COL_GLASS_SHINE);
    hline(x+CORNER_R,y+h-1,w-2*CORNER_R,COL_ACCENT_DIM);
}

/* =========================================================================
 * Wallpaper
 * ========================================================================= */
static void blit_bg_scaled(uint32_t dx0,uint32_t dy0,uint32_t dw,uint32_t dh,uint32_t oa){
    uint32_t W=fb.width;
    uint32_t *shadow=fb_shadow_ptr();
    if(!shadow||dw==0||dh==0)return;
    for(uint32_t dy=0;dy<dh;dy++){
        uint32_t sy=dy*BG_H/dh; if(sy>=BG_H)sy=BG_H-1;
        const uint32_t *sr=bg_pixels+sy*BG_W;
        uint32_t *dr=shadow+(dy0+dy)*W+dx0;
        for(uint32_t dx=0;dx<dw;dx++){
            uint32_t sx=dx*BG_W/dw; if(sx>=BG_W)sx=BG_W-1;
            uint32_t raw=sr[sx];
            uint8_t r=(uint8_t)((raw>>16)&0xFF),g=(uint8_t)((raw>>8)&0xFF),b=(uint8_t)(raw&0xFF);
            uint8_t a=(oa>255u)?255u:(uint8_t)oa;
            r=(uint8_t)((r*(255-a)+BG_OVERLAY_R*a)/255);
            g=(uint8_t)((g*(255-a)+BG_OVERLAY_G*a)/255);
            b=(uint8_t)((b*(255-a)+BG_OVERLAY_B*a)/255);
            dr[dx]=fb_color(r,g,b);
        }
    }
}
static void draw_wallpaper(void){
    if(!g_wallpaper_dirty)return;
    uint32_t W=fb.width,H=fb.height;
    if(g_bg_disabled){fb_fill_rect(0,0,W,H,0x202020u);}
    else{
#ifndef DRACO_STABLE
        blit_bg_scaled(0,0,W,H,g_bg_overlay);
#else
        fb_fill_rect(0,0,W,H,COL_GLASS_BG);
#endif
    }
    g_wallpaper_dirty=0;
}

/* =========================================================================
 * Desktop action callbacks
 * ========================================================================= */
void desktop_refresh(void){g_wallpaper_dirty=1;}
void desktop_restore_windows(void){comp_show_all_windows(g_ws);g_wallpaper_dirty=1;}
void desktop_change_bg(void){
    g_bg_overlay=(g_bg_overlay>180u)?90u:(g_bg_overlay>40u)?10u:200u;
    g_wallpaper_dirty=1;
}
void desktop_about_open(void){g_about_open=1;}
void desktop_logout(void){g_logged_in=0;g_login_pass[0]='\0';g_wallpaper_dirty=1;g_about_open=0;}
void desktop_begin_drag(int h){
    if(h<0)return;
    int ox,oy; comp_get_pos(h,&ox,&oy);
    g_drag_win=h; g_drag_off_x=g_cx-ox; g_drag_off_y=g_cy-oy;
    g_resize_win=-1; g_resize_edge=RESIZE_NONE;
}

/* =========================================================================
 * About overlay
 * ========================================================================= */
static void draw_about(void){
    if(!g_about_open)return;
    uint32_t W=fb.width,H=fb.height;
    uint32_t pw=(W>560u)?480u:W-40u,ph=(H>340u)?280u:H-60u;
    uint32_t px=(W-pw)/2,py=(H-ph)/2;
    fb_fill_rect(0,0,W,H,fb_blend(0x000000u,0u,160));
    glass_panel(px,py,pw,ph,COL_ACCENT,COL_GLASS_PANEL);
    fb_print_s(px+20,py+16,"DracolaxOS V1",COL_ACCENT_LT,COL_GLASS_PANEL,2);
    hline(px+16,py+54,pw-32,COL_SEP);
    static const char *lines[]={
        "  Kernel : Draco-1.0 x86_64 (64-bit preemptive)",
        "  GUI    : Glassmorphism compositor v3",
        "  Desktop: Icon grid + Widget bar",
        "  Super+S=Search  Super+I=Workspaces  Super+B=BG",
        "  Super+Q=Close   Super+M=Maximise    Super+H=Hide",
        "  Super+R=Open/hidden apps  F1-F4=Switch workspace",
        "  Alt+Tab=Open/hidden apps  Ctrl+Super+D=GUI debug mode",
        "  Author : Lunax (Yunis) + Amilcar",
        "",
        "  Press ESC or click [X] to close",
    };
    for(int i=0;i<10;i++)
        fb_print(px+16,py+62+(uint32_t)i*20,lines[i],i==9?COL_TEXT_DIM:COL_TEXT_MED,0);
    fb_fill_rect(px+pw-26,py+8,18,18,COL_ERR);
    fb_rounded_rect(px+pw-26,py+8,18,18,3,0xFF8080u);
    fb_print(px+pw-23,py+11,"X",COL_TEXT_HI,COL_ERR);
}

/* =========================================================================
 * GUI debug mode panel (Ctrl+Super+D)
 * ========================================================================= */
#define DBG_ROW_H 26
static void draw_debug_panel(void){
    if(!g_debug_open)return;
    uint32_t W=fb.width,H=fb.height;
    uint32_t pw=280u,ph=64u+3u*DBG_ROW_H+16u;
    if(pw>W-40u)pw=W-40u;
    if(ph>H-40u)ph=H-40u;
    uint32_t px=(W-pw)/2,py=(H-ph)/2;
    fb_fill_rect(0,0,W,H,fb_blend(0x000000u,0u,160));
    glass_panel(px,py,pw,ph,COL_ACCENT,COL_GLASS_PANEL);
    fb_print_s(px+16,py+14,"GUI Debug",COL_ACCENT_LT,COL_GLASS_PANEL,2);
    hline(px+16,py+44,pw-32,COL_SEP);

    /* Raw-mode widget context: this is desktop chrome (no window of its
     * own), so it uses widget_ctx_init_raw() to draw directly to the
     * framebuffer at absolute coordinates — see widgets.h. Only the
     * non-interactive drawing calls (widget_rect/widget_label) are used
     * here; the actual click handling below is done manually (matching
     * how the other desktop overlays like this one and the about panel
     * already work), since raw mode has no window to hit-test a mouse
     * against for the interactive widgets. */
    static widget_ctx_t rctx;
    widget_ctx_init_raw(&rctx);

    static const struct{const char *label;int *flag;}rows[]={
        {"Show hitboxes (buttons/resize zones)",&g_dbg_show_hitboxes},
        {"Render update flash (TODO: no data yet)",&g_dbg_show_render_update},
        {"Layout bounds (TODO: no data yet)",&g_dbg_show_layout_bounds},
    };
    for(int i=0;i<3;i++){
        int ry=(int)(py+54)+i*DBG_ROW_H;
        int box=16;
        widget_rect(&rctx,(int)px+16,ry,box,box,*rows[i].flag?COL_ACCENT:COL_GLASS_PANEL,1);
        widget_rect_border(&rctx,(int)px+16,ry,box,box,COL_ACCENT_LT,1);
        widget_label(&rctx,(int)px+16+box+8,ry+1,rows[i].label,COL_TEXT_MED);
    }

    fb_print(px+16,(uint32_t)(py+54)+3u*DBG_ROW_H+6u,"Click a box to toggle. Esc to close.",COL_TEXT_DIM,0);
}

/* =========================================================================
 * Search overlay (Super+S)
 * ========================================================================= */
static int search_match(const char *hay,const char *needle){
    if(!needle[0])return 0;
    size_t nl=strlen(needle);
    for(size_t i=0;hay[i];i++){
        int ok=1;
        for(size_t j=0;j<nl&&ok;j++){
            char h=hay[i+j],n=needle[j];
            if(!h){ok=0;break;}
            if(h>='A'&&h<='Z')h+=32;
            if(n>='A'&&n<='Z')n+=32;
            if(h!=n)ok=0;
        }
        if(ok)return 1;
    }
    return 0;
}
static void search_update(void){
    g_sr_count=0;g_sr_sel=0;
    if(!g_search_query[0])return;
    int total=appman_count();
    for(int i=0;i<total&&g_sr_count<SEARCH_MAX_RESULTS;i++){
        const app_entry_t *a=appman_get(i);
        if(a&&search_match(a->name,g_search_query)){
            strncpy(g_sr_names[g_sr_count],a->name,APP_NAME_LEN-1);
            g_sr_names[g_sr_count][APP_NAME_LEN-1]='\0';
            g_sr_count++;
        }
    }
}
static void draw_search(void){
    if(!g_search_open)return;
    uint32_t W=fb.width;
    uint32_t pw=(W>440u)?400u:W-40u;
    uint32_t rh=g_sr_count>0?(uint32_t)g_sr_count*28u+8u:0u;
    uint32_t ph=64u+rh;
    uint32_t px=(W-pw)/2u,py=20u;
    glass_panel(px,py,pw,ph,COL_ACCENT,COL_GLASS_PANEL);
    fb_print(px+10,py+20,"[/]",COL_ACCENT_LT,COL_GLASS_PANEL);
    uint32_t fx=px+40,fw=pw-56;
    fb_fill_rect(fx,py+14,fw,28,0x0C0E22u);
    fb_rounded_rect(fx,py+14,fw,28,4,COL_ACCENT_LT);
    fb_print(fx+6,py+20,
             g_search_query[0]?g_search_query:"Type app name...",
             g_search_query[0]?COL_TEXT_HI:COL_TEXT_DIM,0);
    if(g_ticks%60<40){
        uint32_t cx3=fx+6+(uint32_t)strlen(g_search_query)*FONT_W;
        fb_fill_rect(cx3,py+20,2,FONT_H,COL_ACCENT_LT);
    }
    fb_print(px+pw-88,py+20,"ESC close",COL_TEXT_DIM,COL_GLASS_PANEL);
    if(g_sr_count>0){
        hline(px+8,py+56,pw-16,COL_SEP);
        for(int i=0;i<g_sr_count;i++){
            uint32_t ry=py+64+(uint32_t)i*28u;
            int hov=(g_cy>=(int)ry&&g_cy<(int)(ry+28)&&g_cx>=(int)px&&g_cx<(int)(px+pw));
            int sel=(i==g_sr_sel);
            if(hov||sel)fb_fill_rect(px+2,ry,pw-4,28,sel?COL_ACCENT_DIM:0x22264Au);
            fb_print(px+14,ry+6,g_sr_names[i],
                     (hov||sel)?COL_TEXT_HI:COL_TEXT_MED,
                     sel?COL_ACCENT_DIM:COL_GLASS_PANEL);
        }
    }else if(g_search_query[0]){
        hline(px+8,py+56,pw-16,COL_SEP);
        fb_print(px+14,py+64,"No matching apps",COL_TEXT_DIM,COL_GLASS_PANEL);
    }
}

/* =========================================================================
 * Login screen
 * ========================================================================= */
static void draw_login(void){
    uint32_t W=fb.width,H=fb.height;
    blit_bg_scaled(0,0,W,H,180u);
    uint32_t cw=(W>440u)?380u:W-40u,ch=(H>380u)?320u:H-40u;
    uint32_t cx=(W-cw)/2,cy=(H-ch)/2;
    glass_panel(cx,cy,cw,ch,COL_ACCENT,COL_GLASS_PANEL);
    fb_print_s(cx+60,cy+20,"DracolaxOS",COL_ACCENT_LT,COL_GLASS_PANEL,2);
    hline(cx+16,cy+62,cw-32,COL_SEP);
    fb_print(cx+16,cy+72,"Sign in to your account",COL_TEXT_MED,0);
    int uf=(g_login_field==0);
    uint32_t fy0=cy+104;
    fb_print(cx+16,fy0,"Username",COL_TEXT_DIM,0);
    fb_fill_rect(cx+16,fy0+18,cw-32,28,uf?0x1E1E40u:0x141430u);
    fb_rounded_rect(cx+16,fy0+18,cw-32,28,4,uf?COL_ACCENT_LT:COL_SEP);
    fb_print(cx+22,fy0+24,g_login_user,COL_TEXT_HI,0);
    if(uf&&g_ticks%60<40){uint32_t cx4=cx+22+(uint32_t)strlen(g_login_user)*FONT_W;fb_fill_rect(cx4,fy0+24,2,FONT_H,COL_ACCENT_LT);}
    int pf=(g_login_field==1);
    uint32_t fy1=fy0+64;
    fb_print(cx+16,fy1,"Password",COL_TEXT_DIM,0);
    fb_fill_rect(cx+16,fy1+18,cw-32,28,pf?0x1E1E40u:0x141430u);
    fb_rounded_rect(cx+16,fy1+18,cw-32,28,4,pf?COL_ACCENT_LT:COL_SEP);
    char stars[LOGIN_BUF]; size_t plen=strlen(g_login_pass);
    { size_t si; for(si=0;si<plen&&si<LOGIN_BUF-1;si++) stars[si]='*'; stars[si]='\0'; }
    fb_print(cx+22,fy1+24,stars,COL_TEXT_HI,0);
    if(pf&&g_ticks%60<40){uint32_t cx4=cx+22+(uint32_t)plen*FONT_W;fb_fill_rect(cx4,fy1+24,2,FONT_H,COL_ACCENT_LT);}
    uint32_t bx=cx+16,by=fy1+60,bw=cw-32,bh=32;
    fb_fill_rect(bx,by,bw,bh,COL_ACCENT);
    fb_rounded_rect(bx,by,bw,bh,6,COL_ACCENT_LT);
    fb_print(bx+(bw-6u*FONT_W)/2,by+8,"Log In",COL_TEXT_HI,COL_ACCENT);
    if(g_login_msg[0]){uint32_t mc=g_login_msg_err?COL_ERR:COL_OK;fb_print(cx+(cw-(uint32_t)strlen(g_login_msg)*FONT_W)/2,by+40,g_login_msg,mc,0);}
    fb_print(cx+16,cy+ch-22,"Tab = switch field   Enter = login",COL_TEXT_DIM,0);
}
static void handle_login_key(char c){
    if(c=='\t'){g_login_field=1-g_login_field;return;}
    if(c=='\n'){
        if(dracoauth_login(g_login_user,g_login_pass)==0){
            g_logged_in=1;g_wallpaper_dirty=1;
            snprintf(g_login_msg,sizeof(g_login_msg),"Welcome, %s!",dracoauth_whoami());
            g_login_msg_err=0;
        }else{snprintf(g_login_msg,sizeof(g_login_msg),"Invalid credentials.");g_login_msg_err=1;g_login_pass[0]='\0';}
        return;
    }
    if(c=='\b'){char *buf=(g_login_field==0)?g_login_user:g_login_pass;size_t l=strlen(buf);if(l>0)buf[l-1]='\0';return;}
    if(c>=32&&c<127){char *buf=(g_login_field==0)?g_login_user:g_login_pass;size_t l=strlen(buf);if(l<LOGIN_BUF-1){buf[l]=c;buf[l+1]='\0';}}
}

/* =========================================================================
 * Boot logo
 * ========================================================================= */
static int g_boot_phase=0,g_boot_ticks=0;
#define BOOT_LOGO_FRAMES 60
#define BOOT_FADE_FRAMES 20
static void draw_boot_logo(int fa){
    uint32_t W=fb.width,H=fb.height;
    fb_clear(COL_VOID);
    uint32_t sz=(H<W?H:W)/5,box=sz*2;
    uint32_t ox=W/2-sz,oy=H/2-sz-20;
    for(uint32_t py2=0;py2<box;py2++){
        for(uint32_t px2=0;px2<box;px2++){
            uint32_t bx=px2*DRACO_LOGO_W/box,by2=py2*DRACO_LOGO_H_PX/box;
            uint32_t bidx=by2*DRACO_LOGO_W+bx;
            uint8_t byte=draco_logo_1bpp[bidx>>3];
            uint8_t bit=(byte>>(7-(bidx&7)))&1;
            if(!bit)continue;
            int dx=(int)px2-(int)sz,dy2=(int)py2-(int)sz;
            uint32_t d2=(uint32_t)(dx*dx+dy2*dy2),m2=sz*sz;
            uint8_t blend=(d2<m2)?(uint8_t)(200+55*(m2-d2)/m2):200u;
            uint32_t col=fb_blend(0xFFFFFFu,0xAA66FFu,blend);
            if(fa<255){uint8_t a=(uint8_t)fa;col=fb_color((uint8_t)(((col>>16)&0xFF)*a/255),(uint8_t)(((col>>8)&0xFF)*a/255),(uint8_t)((col&0xFF)*a/255));}
            if(ox+px2<W&&oy+py2<H)fb_put_pixel(ox+px2,oy+py2,col);
        }
    }
    const char *nm="DracolaxOS";uint32_t nw=(uint32_t)strlen(nm)*16;
    uint32_t ct=COL_TEXT_HI;
    if(fa<255){uint8_t a=(uint8_t)fa;ct=fb_color((uint8_t)(0xF0*a/255),(uint8_t)(0xF0*a/255),(uint8_t)(0xFF*a/255));}
    fb_print_s((W-nw)/2,oy+box+16,nm,ct,0,2);
}

/* =========================================================================
 * Window close — BUG2 FIX
 * Signal the app task to exit via ESC so readline unblocks.
 * Hide the window immediately for responsive feel.
 * ========================================================================= */
static void close_window(int wid){
    int tid=comp_get_task_id(wid);
    if(tid>=0) input_router_push_to(tid,(char)0x1B);
    comp_set_visible(wid,0);
    comp_clear_minimized(wid);
    if(g_drag_win==wid) g_drag_win=-1;
    if(g_resize_win==wid){g_resize_win=-1;g_resize_edge=RESIZE_NONE;}
    input_router_set_focus(desktop_tid);
    g_wallpaper_dirty=1;  /* force repaint so closed window disappears immediately */
}

/* =========================================================================
 * Desktop task — entry point
 * ========================================================================= */
void desktop_task(void){
    __asm__ volatile("sti");
    fb_console_lock(1);
    kinfo("DESKTOP: v3.0 starting\n");
    if(!fb.available){
        vga_print("\n    DracolaxOS — No VESA framebuffer.\n");
        vga_print("    Use mode=graphical in GRUB entry.\n");
        for(;;)sched_sleep(1000);
    }
    fb_enable_shadow();
    fb_clear(COL_VOID);
    fb_flip();
    cursor_init();
    comp_init();
    wm_init();
    sched_yield();
    desktop_icons_init();
    /* dock.c was repurposed from a persistent taskbar into a hidden-
     * until-toggled "open apps" switcher panel (Alt+Tab — see the
     * KB_KEY_ALTTAB handling below, and kernel/drivers/ps2/keyboard.c
     * where that keycode is generated). dock_init() just resets its
     * state; dock_draw() is a no-op while closed, called from the
     * overlay tier below (not the shell/panel tier — it's meant to float
     * above windows only while actively switching, not sit as a
     * permanent bottom/side bar). */
    dock_init();
    sched_yield();

    /* BUG FIX (GUI keyboard not working / "only shortcuts worked"):
     * desktop_tid/input_router_set_focus() used to be set AFTER
     * installer_run() below. installer_run() reads its own keyboard input
     * via keyboard_getchar() -> input_router_getchar(sched_current_id()),
     * i.e. it reads from desktop_task's queue — but until the lines that
     * used to be below ran, g_focused was still whatever input_router_init()
     * left it as (task 0), so the keyboard IRQ was pushing every typed
     * character into task 0's queue instead of desktop_task's. Mouse clicks
     * and Super+letter shortcuts (which bypass g_focused via
     * input_router_push_to(desktop_tid,...)) still worked, which is why only
     * shortcuts appeared functional while normal typing did nothing — this
     * hit the installer's username/password fields and would equally affect
     * the login screen and any future text input shown before this point.
     * Moving this block here ensures g_focused == desktop_tid before any
     * keyboard-driven UI (installer, login) runs. */
    desktop_tid = sched_current_id();
    input_router_set_desktop_task(desktop_tid);
    input_router_set_focus(desktop_tid);
    kinfo("DESKTOP: task id=%d, registered as Super-key target\n", desktop_tid);

    {
        int ns=(dracoauth_login("__probe__","")!=0&&dracoauth_login("root","dracolax")==0);
        dracoauth_logout();
        if(ns){installer_run();sched_yield();}
    }
    g_cx=(int)fb.width/2;
    g_cy=(int)fb.height/2;

    for(;;){
        g_ticks++;

        /* BUG FIX (workspace switching — and any other keyboard-driven
         * desktop overlay — blocked while an app window has focus):
         * ws_switcher_open()/g_search_open toggle/ctx_menu_open() etc.
         * never forced input focus to the desktop task, so once open,
         * keyboard navigation inside them (arrow keys, digit keys, Tab)
         * went to whichever app window currently held focus instead —
         * the overlay would open (Super+I/Super+S bypass focus like
         * every Super+letter combo) but then sit there unresponsive to
         * the keyboard until focus happened to revert to the desktop for
         * an unrelated reason (e.g. Super+H, which explicitly resets
         * focus). Checked once per frame here instead of at each
         * individual open/close call site (there are several, scattered
         * across desktop.c and ctx_menu.c) so this can't be missed by a
         * future call site that forgets to opt in. Idempotent:
         * input_router_set_focus() is a no-op if focus is already what's
         * requested, so this costs nothing on every other frame. */
        {
            static int modal_was_open = 0;
            static int pre_modal_focus_task = -1;
            int modal_open = ws_switcher_is_open() || g_search_open ||
                             ctx_menu_is_open() || g_about_open || dock_is_open() ||
                             g_debug_open;
            if (modal_open && !modal_was_open) {
                /* Transition into "a modal is open": remember which app
                 * window (if any) currently has focus so it can be
                 * restored once every modal closes again. */
                int fw = comp_focused_window();
                pre_modal_focus_task = (fw >= 0) ? comp_get_task_id(fw) : -1;
            }
            if (modal_open) {
                input_router_set_focus(desktop_tid);
            } else if (modal_was_open) {
                /* Transition into "no modal open": restore focus to
                 * whatever app window had it before, if that task is
                 * still alive; otherwise leave focus on the desktop. */
                if (pre_modal_focus_task >= 0)
                    input_router_set_focus(pre_modal_focus_task);
                pre_modal_focus_task = -1;
            }
            modal_was_open = modal_open;
        }

        /* ── Boot logo ── */
#ifndef DRACO_STABLE
        if(g_boot_phase<2){
            g_boot_ticks++;
            if(g_boot_phase==0){
                draw_boot_logo(255);fb_flip();
                if(g_boot_ticks>=BOOT_LOGO_FRAMES){g_boot_phase=1;g_boot_ticks=0;}
            }else{
                int alpha=255-g_boot_ticks*255/BOOT_FADE_FRAMES;
                if(alpha<0)alpha=0;
                draw_boot_logo(alpha);fb_flip();
                if(g_boot_ticks>=BOOT_FADE_FRAMES){
                    g_boot_phase=2;g_boot_ticks=0;
                    fb_clear(COL_VOID);fb_flip();g_wallpaper_dirty=1;
                }
            }
            sched_sleep(33);continue;
        }
#else
        g_boot_phase=2;g_wallpaper_dirty=1;
#endif

        /* ── Mouse ── */
        mouse_update_edges();
        vmmouse_poll();
        int nmx=mouse_get_x(),nmy=mouse_get_y();
        if(nmx!=g_mouse_x_raw||nmy!=g_mouse_y_raw){g_cx=nmx;g_cy=nmy;g_mouse_x_raw=nmx;g_mouse_y_raw=nmy;}

        /* Unified click detection: vmmouse latch catches brief clicks;
         * fall back to a LOCALLY-computed edge for PS/2 relative mode.
         *
         * BUG FIX (window chrome — title bar buttons, drag, resize,
         * click-to-focus — unresponsive to mouse clicks even though the
         * exact same actions work via keyboard shortcuts): the PS/2
         * fallback used to be mouse_btn_pressed()/mouse_btn_released(),
         * which are edge-detected against a snapshot taken by
         * mouse_update_edges() — called once per desktop_task loop
         * iteration, right above. If desktop_task's own scheduling is
         * jittery (the boot log shows frequent "heartbeat stalled"
         * warnings for the desktop task specifically, suggesting exactly
         * this under a slow/loaded host), the gap between two
         * consecutive mouse_update_edges() calls can grow large enough
         * that an entire press-then-release can happen inside it — the
         * next call's diff against that stale snapshot then shows no
         * edge at all, and the click is silently missed. Widget-drawn
         * app content didn't have this problem because it already used
         * a LOCALLY-computed edge (see widgets.c's widget_begin_frame())
         * diffed against its own last-seen state — however delayed a
         * check runs, it still correctly detects a transition relative
         * to what it itself last observed. Desktop.c's own chrome click
         * detection now does the same: static prev_lheld, diffed against
         * the current mouse_btn_held() read below, instead of trusting a
         * separately-timed snapshot. This doesn't fix a click-and-release
         * that completes entirely between two even LOCAL checks (no
         * polling-based approach can), but removes the double jeopardy
         * of ALSO depending on mouse_update_edges()'s own timing. */
        uint8_t vm_p  = vmmouse_get_pressed();
        uint8_t vm_r  = vmmouse_get_released();
        int held_now = mouse_btn_held(MOUSE_BTN_LEFT);
        int rheld_now = mouse_btn_held(MOUSE_BTN_RIGHT);
        static int prev_lheld = 0;
        static int prev_rheld = 0;
        int local_lclick   = held_now && !prev_lheld;
        int local_lrelease = !held_now && prev_lheld;
        int local_rclick   = rheld_now && !prev_rheld;
        prev_lheld = held_now;
        prev_rheld = rheld_now;
        int lclick = (vm_p & MOUSE_BTN_LEFT)  || (!vm_p && local_lclick);
        int rclick = (vm_p & MOUSE_BTN_RIGHT) || (!vm_p && local_rclick);
        int lheld  = held_now;
        /* Release detection for drag end */
        int lrelease = (vm_r & MOUSE_BTN_LEFT) || (!vm_r && local_lrelease);

        /* BUG FIX (desktop icons couldn't be opened by clicking, or
         * dragged to another grid cell): neither existed before — icon
         * click used to launch immediately on PRESS with no drag
         * concept at all. Decided here, on release: if the press-to-
         * release motion never crossed the drag threshold (see the lheld
         * handling above), treat it as a plain click and launch: this is
         * also what fixes plain icon-click, since it's now the ONLY path
         * that launches an icon's app (moved off the press handler, which
         * only starts a potential drag now). If it DID cross the
         * threshold, snap the icon to whichever grid cell the cursor
         * ended up over, instead of launching anything. */
        if(lrelease && g_icon_drag_idx>=0){
            if(g_icon_dragged){
                int col=(g_cx-ICON_GRID_X)/ICON_CELL_W;
                int row=(g_cy-ICON_GRID_Y)/ICON_CELL_H;
                if(col<0)col=0;
                if(row<0)row=0;
                g_icons[g_icon_drag_idx].col=col;
                g_icons[g_icon_drag_idx].row=row;
                g_wallpaper_dirty=1;
            }else{
                kinfo("DESKTOP: icon click idx=%d name='%s'\n",
                      g_icon_drag_idx,g_icons[g_icon_drag_idx].name);
                appman_launch(g_icons[g_icon_drag_idx].name);
            }
            g_icon_drag_idx=-1;
            g_icon_dragged=0;
        }

        /* ── Keyboard ── */
        int c;
        while((c=keyboard_getchar())!=0){
            uint8_t uc=(uint8_t)c;

            /* BUG5 FIX: arrow keys move cursor ONLY when search closed.
             * BUG FIX (dock keyboard navigation, and workspace-switcher
             * keyboard navigation): also skip while the dock panel or
             * the workspace switcher is open — Up/Down there should
             * navigate their own lists (see the dock_is_open() block
             * below, and the ws_switcher_is_open() block further down
             * this loop) instead of moving the mouse cursor, which is
             * why arrow-key navigation never did anything in either
             * before these guards existed. */
            if(!g_search_open && !dock_is_open() && !ws_switcher_is_open()){
                if(uc==KB_KEY_UP)   {g_cy-=4;if(g_cy<0)g_cy=0;g_mouse_x_raw=g_cx;g_mouse_y_raw=g_cy;continue;}
                if(uc==KB_KEY_DOWN) {g_cy+=4;if(g_cy>=(int)fb.height)g_cy=(int)fb.height-1;g_mouse_x_raw=g_cx;g_mouse_y_raw=g_cy;continue;}
                if(uc==KB_KEY_LEFT) {g_cx-=4;if(g_cx<0)g_cx=0;g_mouse_x_raw=g_cx;g_mouse_y_raw=g_cy;continue;}
                if(uc==KB_KEY_RIGHT){g_cx+=4;if(g_cx>=(int)fb.width)g_cx=(int)fb.width-1;g_mouse_x_raw=g_cx;g_mouse_y_raw=g_cy;continue;}
            }

            /* BUG FIX (dock keyboard navigation): Up/Down move the
             * selection, Enter activates it. Checked ahead of
             * !g_logged_in/other handling below since this is itself a
             * modal (see the per-frame focus-grab guard earlier in this
             * loop, which already forces input focus to the desktop
             * while the dock is open). */
            if(dock_is_open()){
                if(uc==KB_KEY_UP)  {dock_move_selection(-1);g_wallpaper_dirty=1;continue;}
                if(uc==KB_KEY_DOWN){dock_move_selection(1);g_wallpaper_dirty=1;continue;}
                if(c=='\n'||c=='\r'){dock_activate_selected();g_wallpaper_dirty=1;continue;}
            }

            if(!g_logged_in){handle_login_key((char)c);continue;}

            /* Workspace switcher eats all keys while open */
            if(ws_switcher_is_open()){
                int nws=ws_switcher_key(c,g_ws);
                if(nws>=0){g_ws=nws;g_wallpaper_dirty=1;wm_switch_desktop(g_ws);comp_switch_desktop(g_ws);}
                else if(!ws_switcher_is_open())g_wallpaper_dirty=1;
                continue;
            }

            if(c=='\t'){
                /* Tab: do nothing at desktop level (Ctrl+I = Tab = 0x09, harmless) */
                continue;
            } else if(c==SUPER_KEY('i')){
                /* Super+I → workspace switcher */
                g_wallpaper_dirty=1; ws_switcher_open(g_ws); continue;
            }

            if(c==0x1B){
                if(g_debug_open){g_debug_open=0;g_wallpaper_dirty=1;}
                else if(g_about_open){g_about_open=0;g_wallpaper_dirty=1;}
                else if(dock_is_open()){dock_close();g_wallpaper_dirty=1;}
                else if(ctx_menu_is_open()){ctx_menu_close();g_wallpaper_dirty=1;}
                else if(g_search_open){g_search_open=0;g_search_query[0]='\0';g_sr_count=0;g_wallpaper_dirty=1;}
                continue;
            }

            /* Ctrl+Super+D — toggle GUI debug mode panel */
            if(c==CTRL_SUPER_KEY('d')){
                g_debug_open=!g_debug_open;
                g_wallpaper_dirty=1;continue;
            }

            /* Super+letter shortcuts */
            if(c==SUPER_KEY('s')){
                g_search_open=!g_search_open;
                if(!g_search_open){g_search_query[0]='\0';g_sr_count=0;}
                g_wallpaper_dirty=1;continue;
            }
            if(c==SUPER_KEY('b')){g_bg_disabled=!g_bg_disabled;g_wallpaper_dirty=1;continue;}

            /* Super+Q — close focused window */
            if(c==SUPER_KEY('q')){
                int fw=comp_focused_window();
                if(fw>=0) close_window(fw);
                g_wallpaper_dirty=1;continue;
            }
            /* Super+M — toggle maximize focused window */
            if(c==SUPER_KEY('m')){
                int fw=comp_focused_window();
                if(fw>=0) comp_toggle_maximize(fw);
                g_wallpaper_dirty=1;continue;
            }
            /* Super+H — hide (minimize) focused window */
            if(c==SUPER_KEY('h')){
                int fw=comp_focused_window();
                if(fw>=0){comp_set_visible(fw,0);input_router_set_focus(desktop_tid);}
                g_wallpaper_dirty=1;continue;
            }
            /* BUG FIX (Super+R should show a panel, not silently restore
             * everything): previously this force-restored every
             * minimized window on the workspace at once with no way to
             * choose — the dock panel (repurposed into an open/hidden-
             * apps switcher, see dock.c) now lists them individually so
             * the user can pick which one to bring back. */
            if(c==SUPER_KEY('r')){
                if(dock_is_open()) dock_close(); else dock_open();
                g_wallpaper_dirty=1;continue;
            }

            /* Alt+Tab: same open-apps/hidden-apps switcher panel as
             * Super+R above (dock.c) — KB_KEY_ALTTAB was already
             * synthesised by the keyboard driver but never consumed by
             * anything until now. */
            if(uc==KB_KEY_ALTTAB){
                if(dock_is_open()) dock_close(); else dock_open();
                g_wallpaper_dirty=1;continue;
            }

            /* F1-F4: workspace switch */
            if(uc==KB_KEY_F1||uc==KB_KEY_F2||uc==KB_KEY_F3||uc==KB_KEY_F4){
                g_ws=(int)(uc-KB_KEY_F1);g_wallpaper_dirty=1;
                wm_switch_desktop(g_ws);comp_switch_desktop(g_ws);continue;
            }

            /* Search typing */
            if(g_search_open){
                if(c>=32&&c<127){
                    size_t l=strlen(g_search_query);
                    if(l<SEARCH_BUF-1){g_search_query[l]=(char)c;g_search_query[l+1]='\0';}
                    search_update();g_wallpaper_dirty=1;
                }else if(c=='\b'){
                    size_t l=strlen(g_search_query);
                    if(l>0)g_search_query[l-1]='\0';
                    search_update();g_wallpaper_dirty=1;
                }else if(c=='\n'||c=='\r'){
                    int t=(g_sr_sel<g_sr_count)?g_sr_sel:(g_sr_count>0?0:-1);
                    if(t>=0)appman_launch(g_sr_names[t]);
                    g_search_open=0;g_search_query[0]='\0';g_sr_count=0;g_wallpaper_dirty=1;
                }else if(uc==KB_KEY_DOWN){
                    /* BUG5 FIX: navigate result list */
                    if(g_sr_sel<g_sr_count-1)g_sr_sel++;
                    g_wallpaper_dirty=1;
                }else if(uc==KB_KEY_UP){
                    if(g_sr_sel>0)g_sr_sel--;
                    g_wallpaper_dirty=1;
                }
                continue;
            }
        }

        /* Clamp cursor */
        if(g_cx<0) g_cx=0;
        if(g_cy<0) g_cy=0;
        if(g_cx>=(int)fb.width)g_cx=(int)fb.width-1;
        if(g_cy>=(int)fb.height)g_cy=(int)fb.height-1;

        /* Window drag */
        if(g_drag_win>=0){
            if(lheld){
                int nx=g_cx-g_drag_off_x,ny=g_cy-g_drag_off_y;
                if(nx<0) nx=0;
                if(ny<0) ny=0;
                if(nx>(int)fb.width-40)nx=(int)fb.width-40;
                if(ny>(int)fb.height-30)ny=(int)fb.height-30;
                comp_move_window(g_drag_win,(uint32_t)nx,(uint32_t)ny);
                g_wallpaper_dirty=1;
            }else{g_drag_win=-1;}
        }

        /* BUG FIX (desktop icons couldn't be dragged): while a press on
         * an icon is being held, flag it as a real drag once the mouse
         * has moved more than a small threshold — this is what
         * distinguishes "clicked to launch" from "dragging to
         * reposition" (decided for real on release, further down). */
        if(g_icon_drag_idx>=0){
            if(lheld){
                int dx=g_cx-g_icon_drag_start_cx, dy=g_cy-g_icon_drag_start_cy;
                if(dx<0)dx=-dx;
                if(dy<0)dy=-dy;
                if(dx>6||dy>6){ g_icon_dragged=1; g_wallpaper_dirty=1; }
            }else{
                g_icon_drag_idx=-1; g_icon_dragged=0;
            }
        }

        /* Window resize */
        if(g_resize_win>=0){
            if(lheld){
                int dx=g_cx-g_resize_mx,dy=g_cy-g_resize_my;
                int nx=g_resize_ox,ny=g_resize_oy,nw=g_resize_ow,nh=g_resize_oh;
                switch(g_resize_edge){
                case RESIZE_E:  nw=g_resize_ow+dx;break;
                case RESIZE_S:  nh=g_resize_oh+dy;break;
                case RESIZE_W:  nx=g_resize_ox+dx;nw=g_resize_ow-dx;break;
                case RESIZE_N:  ny=g_resize_oy+dy;nh=g_resize_oh-dy;break;
                case RESIZE_SE: nw=g_resize_ow+dx;nh=g_resize_oh+dy;break;
                case RESIZE_SW: nx=g_resize_ox+dx;nw=g_resize_ow-dx;nh=g_resize_oh+dy;break;
                case RESIZE_NE: nh=g_resize_oh-dy;ny=g_resize_oy+dy;nw=g_resize_ow+dx;break;
                case RESIZE_NW: nx=g_resize_ox+dx;nw=g_resize_ow-dx;ny=g_resize_oy+dy;nh=g_resize_oh-dy;break;
                default:break;
                }
                if(nw<120){if(nx!=g_resize_ox)nx=g_resize_ox+g_resize_ow-120;nw=120;}
                if(nh<60){if(ny!=g_resize_oy)ny=g_resize_oy+g_resize_oh-60;nh=60;}
                if(nx<0) nx=0;
                if(ny<0) ny=0;
                comp_set_geometry(g_resize_win,(uint32_t)nx,(uint32_t)ny,(uint32_t)nw,(uint32_t)nh);
                g_wallpaper_dirty=1;
            }else{g_resize_win=-1;g_resize_edge=RESIZE_NONE;}
        }

        /* Left click */
        if(lclick){
            if(!g_logged_in){
                uint32_t W=fb.width,H=fb.height;
                uint32_t cw=(W>440u)?380u:W-40u,ch=(H>380u)?320u:H-40u;
                uint32_t lcx=(W-cw)/2,lcy=(H-ch)/2;
                uint32_t fy1=lcy+104u+64u;
                uint32_t bx=lcx+16,by=fy1+60,bw=cw-32,bh=32;(void)ch;
                if(g_cx>=(int)bx&&g_cx<(int)(bx+bw)&&g_cy>=(int)by&&g_cy<(int)(by+bh))
                    handle_login_key('\n');
            }else if(ws_switcher_is_open()){
                int nws=ws_switcher_click(g_cx,g_cy);
                if(nws>=0){g_ws=nws;g_wallpaper_dirty=1;wm_switch_desktop(g_ws);comp_switch_desktop(g_ws);}
                else if(!ws_switcher_is_open())g_wallpaper_dirty=1;
            }else if(ctx_menu_is_open()){
                if(!ctx_menu_click(g_cx,g_cy))ctx_menu_close();
                g_wallpaper_dirty=1;
            }else if(g_about_open){
                uint32_t W=fb.width,H=fb.height,pw=480,ph=280;
                uint32_t px=(W-pw)/2,py=(H-ph)/2;
                if(g_cx>=(int)(px+pw-26)&&g_cx<(int)(px+pw-8)&&g_cy>=(int)(py+8)&&g_cy<(int)(py+26)){g_about_open=0;g_wallpaper_dirty=1;}
            }else if(g_search_open){
                uint32_t W2=fb.width,pw=(W2>440u)?400u:W2-40u;
                uint32_t rh=g_sr_count>0?(uint32_t)g_sr_count*28u+8u:0u;
                uint32_t ph=64u+rh,px2=(W2-pw)/2u,py2=20u;
                if(g_sr_count>0&&g_cx>=(int)px2&&g_cx<(int)(px2+pw)&&g_cy>=(int)(py2+64u)&&g_cy<(int)(py2+ph)){
                    int row=((int)g_cy-(int)(py2+64u))/28;
                    if(row>=0&&row<g_sr_count)appman_launch(g_sr_names[row]);
                    g_search_open=0;g_search_query[0]='\0';g_sr_count=0;g_wallpaper_dirty=1;
                }else if(g_cx<(int)px2||g_cx>=(int)(px2+pw)||g_cy<(int)py2||g_cy>=(int)(py2+ph)){
                    g_search_open=0;g_search_query[0]='\0';g_sr_count=0;g_wallpaper_dirty=1;
                }
            }else if(g_debug_open){
                uint32_t W3=fb.width,H3=fb.height;
                uint32_t pw3=280u,ph3=64u+3u*DBG_ROW_H+16u;
                if(pw3>W3-40u)pw3=W3-40u;
                if(ph3>H3-40u)ph3=H3-40u;
                uint32_t px3=(W3-pw3)/2,py3=(H3-ph3)/2;
                if(g_cx>=(int)px3&&g_cx<(int)(px3+pw3)&&g_cy>=(int)py3&&g_cy<(int)(py3+ph3)){
                    int * const flags[3]={&g_dbg_show_hitboxes,&g_dbg_show_render_update,&g_dbg_show_layout_bounds};
                    for(int i=0;i<3;i++){
                        int ry=(int)(py3+54)+i*DBG_ROW_H;
                        if(g_cx>=(int)px3+16&&g_cx<(int)px3+16+16&&g_cy>=ry&&g_cy<ry+16){
                            *flags[i]=!*flags[i];
                            g_wallpaper_dirty=1;
                        }
                    }
                }else{
                    g_debug_open=0;g_wallpaper_dirty=1;
                }
            }else if(dock_is_open()){
                /* The open/hidden-apps switcher panel — floats above
                 * windows in the overlay tier while open (see the draw
                 * order above), so it must also intercept clicks ahead
                 * of window-chrome handling below. dock_click() closes
                 * the panel itself on a successful pick; a click outside
                 * the panel just dismisses it without acting on anything
                 * underneath, matching the other overlays' click-outside-
                 * to-dismiss behaviour above. */
                if(!dock_click(g_cx,g_cy)) dock_close();
                g_wallpaper_dirty=1;
            }else{
                /* Window title bar */
                int wid=comp_title_bar_at(g_cx,g_cy);
                if(wid>=0){
                    comp_focus_window(wid); g_wallpaper_dirty=1;
                    int cls=comp_close_at(wid,g_cx,g_cy);
                    int maxb=comp_maximize_at(wid,g_cx,g_cy);
                    int minb=comp_minimize_at(wid,g_cx,g_cy);
                    if(cls){
                        close_window(wid);
                    }else if(maxb){
                        comp_toggle_maximize(wid);
                    }else if(minb){
                        comp_set_visible(wid,0);
                        input_router_set_focus(desktop_tid);
                    }else{
                        /* Start drag — route focus to the app owning the window */
                        int ftid=comp_get_task_id(wid);
                        if(ftid>=0) input_router_set_focus(ftid);
                        int ox,oy; comp_get_pos(wid,&ox,&oy);
                        g_drag_win=wid;g_drag_off_x=g_cx-ox;g_drag_off_y=g_cy-oy;
                    }
                }else{
                    /* Resize edge or window body */
                    int rwid=comp_window_at(g_cx,g_cy);
                    if(rwid>=0){
                        resize_edge_t edge=comp_resize_edge_at(rwid,g_cx,g_cy);
                        if(edge!=RESIZE_NONE){
                            comp_focus_window(rwid);
                            int ftid=comp_get_task_id(rwid);
                            if(ftid>=0) input_router_set_focus(ftid);
                            g_resize_win=rwid;g_resize_edge=edge;
                            g_resize_mx=g_cx;g_resize_my=g_cy;
                            int rx,ry,rw,rh; comp_get_geometry(rwid,&rx,&ry,&rw,&rh);
                            g_resize_ox=rx;g_resize_oy=ry;g_resize_ow=rw;g_resize_oh=rh;
                            g_wallpaper_dirty=1;
                        }else{
                            comp_focus_window(rwid);
                            int ftid=comp_get_task_id(rwid);
                            if(ftid>=0) input_router_set_focus(ftid);
                            g_wallpaper_dirty=1;
                        }
                    }else{
                        /* Empty desktop click — clear focus */
                        comp_focus_window(-1);
                        input_router_set_focus(desktop_tid);
                        g_wallpaper_dirty=1;
                        /* Desktop icon — press starts a POTENTIAL drag;
                         * whether it ends up being a click (launch) or a
                         * drag (reposition) is decided on release, based
                         * on whether the mouse moved past a small
                         * threshold in between (see the lheld handling
                         * further up this loop, and lrelease below). */
                        int idx=icon_at(g_cx,g_cy);
                        if(idx>=0){
                            g_icon_drag_idx=idx;
                            g_icon_dragged=0;
                            g_icon_drag_start_cx=g_cx;
                            g_icon_drag_start_cy=g_cy;
                        }
                    }
                }
            }
        }

        /* Right click */
        if(rclick&&g_logged_in&&!ws_switcher_is_open()){
            if(ctx_menu_is_open()){ctx_menu_close();}
            else{ctx_hit_t hit=ctx_resolve(g_cx,g_cy);ctx_menu_open(&hit);}
            g_wallpaper_dirty=1;
        }

        /* ── Draw ── */
        if(!g_logged_in){
            draw_login();
        }else{
            /* BUG FIX (closed windows leave a visual ghost until an
             * unrelated redraw): comp_has_windows() below only forces a
             * repaint while at least one window is STILL open on the
             * current desktop. The exact frame a window closes and the
             * count drops from 1 to 0, comp_has_windows() flips to false
             * in that very same frame — so continuous-repaint switches
             * OFF right when it's needed to paint over the now-stale
             * pixels of the window that just disappeared. This is why
             * closing your LAST window left a ghost, but closing one of
             * several didn't (comp_has_windows() stayed true because
             * another window was still open, so the existing logic below
             * kept forcing repaints anyway). comp_frame_signature() closes
             * this gap by detecting the CHANGE itself (any window opened,
             * closed, moved, resized, shown, or hidden — not just "is one
             * currently open"), independent of which code path caused it
             * (this also covers apps that close themselves directly via
             * comp_destroy_window() on Esc, with no way to otherwise tell
             * desktop_task anything happened). */
            static uint32_t prev_win_sig = 0;
            uint32_t cur_win_sig = comp_frame_signature();
            if(cur_win_sig != prev_win_sig){ g_wallpaper_dirty=1; prev_win_sig=cur_win_sig; }

            /* Always repaint when windows are open so button hover highlights update */
            if(ws_switcher_is_open()) g_wallpaper_dirty=1;
            else if(comp_has_windows()) g_wallpaper_dirty=1;
            /* BUG FIX (search-bar caret never blinks): without this,
             * opening search only forces ONE repaint (the toggle itself);
             * while open+idle nothing re-triggers the blink animation, so
             * the caret freezes at whatever phase happened to be current
             * on that one frame — indistinguishable from "no cursor." */
            else if(g_search_open) g_wallpaper_dirty=1;
            else if(g_debug_open || g_dbg_show_hitboxes) g_wallpaper_dirty=1;
            else {
                static int prev_hover_idx = -1;
                int cur_hover = icon_at(g_cx, g_cy);
                if(cur_hover != prev_hover_idx){ g_wallpaper_dirty=1; prev_hover_idx=cur_hover; }
            }
            /* Z-order, bottom to top: wallpaper < desktop widgets (clock)
             * < desktop icons < normal windows < always-on-top windows
             * (handled by z within comp_render itself) < overlays
             * (about/search/ws-switcher/context-menu/open-apps
             * panel/debug panel). draw_widgets() used to run AFTER
             * comp_render(), putting the clock ABOVE every window and
             * even above desktop icons — two layers too high, now fixed.
             * dock_draw() (the open-apps switcher, repurposed from a
             * persistent taskbar — see dock.c) belongs in the overlay
             * tier, not a permanent shell/panel layer: it's a no-op
             * while closed and only floats above windows while actively
             * toggled on. */
            draw_wallpaper();
            draw_widgets();
            draw_desktop_icons();
            wm_render_frame();
            comp_render();
            /* GUI debug mode (Ctrl+Super+D): hitbox overlay draws right
             * after windows so it overlays them but sits below every
             * other overlay (about/search/switcher/dock/ctx-menu/the
             * debug panel itself), matching how it's meant to be read —
             * "what's clickable on the windows," not on top of other UI
             * chrome. */
            if(g_dbg_show_hitboxes) comp_debug_draw_hitboxes();
            draw_about();
            draw_search();
            if(ws_switcher_is_open())ws_switcher_draw(g_cx,g_cy,g_ws);
            dock_draw(g_cx,g_cy);
            draw_debug_panel();
            ctx_menu_draw(g_cx,g_cy);  /* BUG4 FIX: inside draw, before flip */
        }
        fb_flip();

        /* Cursor shape — only GRAB during active drag, otherwise ARROW over
         * windows and their buttons. TEXT/RESIZE only in specific zones. */
        {
            cursor_type_t ctype=CURSOR_ARROW;
            if(g_drag_win>=0){
                ctype=CURSOR_GRAB;   /* active window drag */
            }else if(g_resize_win>=0){
                switch(g_resize_edge){
                case RESIZE_E:case RESIZE_W:case RESIZE_NW:case RESIZE_SE:ctype=CURSOR_RESIZE_H;break;
                case RESIZE_N:case RESIZE_S:case RESIZE_NE:case RESIZE_SW:ctype=CURSOR_RESIZE_V;break;
                default:ctype=CURSOR_ARROW;break;
                }
            }else if(g_logged_in){
                int hwid=comp_window_at(g_cx,g_cy);
                if(hwid>=0){
                    /* Only show resize cursors on the thin resize border */
                    resize_edge_t re=comp_resize_edge_at(hwid,g_cx,g_cy);
                    switch(re){
                    case RESIZE_E:case RESIZE_W:case RESIZE_NW:case RESIZE_SE:ctype=CURSOR_RESIZE_H;break;
                    case RESIZE_N:case RESIZE_S:case RESIZE_NE:case RESIZE_SW:ctype=CURSOR_RESIZE_V;break;
                    default:
                        /* Title bar AND window body — all ARROW (buttons use ARROW too) */
                        if(comp_get_flags(hwid) & WIN_FLAG_TERMINAL){
                            /* Only TEXT inside the actual content area (not title bar) */
                            if(comp_title_bar_at(g_cx,g_cy)<0)
                                ctype=CURSOR_TEXT;
                            else
                                ctype=CURSOR_ARROW;
                        } else if(comp_title_bar_at(g_cx,g_cy)<0 && widget_wants_text_cursor()){
                            /* BUG FIX (cursor doesn't change to text-beam
                             * over any text input): the WIN_FLAG_TERMINAL
                             * branch above only covered one special-cased
                             * app type. widget_wants_text_cursor()
                             * (gui/widgets/widgets.h) is set by ANY
                             * widget_textbox() when the mouse hovers it,
                             * in any window — this covers every app built
                             * on the widget toolkit generically, not just
                             * one hardcoded case. Guarded by
                             * comp_title_bar_at()<0 so hovering the title
                             * bar of a window that happens to have a
                             * textbox somewhere inside it doesn't
                             * incorrectly show a text cursor there too. */
                            ctype=CURSOR_TEXT;
                        } else {
                            ctype=CURSOR_ARROW;
                        }
                        break;
                    }
                }else if(g_search_open){
                    uint32_t W2=fb.width,pw=(W2>440u)?400u:W2-40u;
                    uint32_t fx=(W2-pw)/2u+40u,fy=34u,fw=pw-56u,fh=28u;
                    if(g_cx>=(int)fx&&g_cx<(int)(fx+fw)&&g_cy>=(int)fy&&g_cy<(int)(fy+fh))ctype=CURSOR_TEXT;
                }else if(icon_at(g_cx,g_cy)>=0){
                    ctype=CURSOR_HAND;
                }
            }
            cursor_set_type(ctype);
        }
        cursor_move((uint32_t)g_cx,(uint32_t)g_cy);
        sched_sleep(33);
    }
}
