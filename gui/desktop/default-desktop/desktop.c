/* gui/desktop/default-desktop/desktop.c
 * DracolaxOS Glassmorphism Desktop — v2.0
 *
 * Architecture (split for easy updating):
 *   dock.c         — floating side panel, app icons, clock, user
 *   ws_switcher.c  — Tab-triggered workspace switcher overlay
 *   desktop.c      — wallpaper, login, ctx menu, about, search + main loop
 *
 * Key changes from v1:
 *   • No top bar — all chrome lives inside the dock panel.
 *   • Dock is a floating rounded glass panel, not a full-height strip.
 *   • Tab (with desktop focus) → workspace switcher centred overlay.
 *   • Right-click dock icon  → pin/unpin (no restart needed).
 *   • Scroll wheel over dock → scroll app list.
 *   • App launch bug fixed   — dock.c uses correct slot→name mapping.
 *
 * Fix inheritance from v1:
 *   [1] Cursor ownership delta-check.
 *   [2] sched_yield() throughout startup.
 *   [3] Nearest-neighbour wallpaper blit.
 *   [5] SHELL/TEXT fb_console guard.
 */

#include "../../../kernel/types.h"
#include "../../../kernel/drivers/vga/fb.h"
#include "../../../kernel/drivers/vga/vga.h"
#include "../../../kernel/sched/sched.h"
#include "../../../kernel/drivers/ps2/keyboard.h"
#include "../../../kernel/drivers/ps2/mouse.h"
#include "../../../kernel/drivers/ps2/vmmouse.h"
#include "../../../kernel/klibc.h"
#include "../../../kernel/log.h"
#include "../../../kernel/arch/x86_64/pic.h"
#include "../../../kernel/security/dracoauth.h"
#include "../../compositor/compositor.h"
#include "../../wm/wm.h"
#include "appman.h"
#include "../../../apps/installer/installer.h"
#include "../../../apps/debug_console/debug_console.h"
#include "../../../services/power_manager.h"
#include "../../../kernel/draco_logo.h"
#include "../../../kernel/dxi/dxi.h"
#include "../../../kernel/drivers/vga/cursor.h"
#include "../../../kernel/arch/x86_64/rtc.h"

#include "../../../kernel/drivers/ps2/input_router.h"
#include "dock.h"
#include "ws_switcher.h"
#include "desktop.h"
#include "background.h"
#include "ctx_resolver.h"   /* Layer 2: context resolver  */
#include "ctx_menu.h"        /* Layer 3: context menu UI   */

/* forward declaration — defined after UI State section */
static void blit_bg_scaled(uint32_t dx0, uint32_t dy0,
                            uint32_t dw,  uint32_t dh,
                            uint32_t overlay_a);
/* Hoisted so desktop_blit_bg_at (below) can reference it before the full
 * UI State block where it would otherwise be declared. */
static uint32_t g_bg_overlay = 70u;
/* Debug flag — toggled via Ctrl+B; declared here so desktop_blit_bg_at can see it */
static int g_bg_disabled = 0;

/* =========================================================================
 * desktop_blit_bg_at — expose raw wallpaper blit to dock/overlays.
 * Called by dock_draw() before blur so each frame starts with fresh pixels.
 * ========================================================================= */
void desktop_blit_bg_at(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (g_bg_disabled) {
        fb_fill_rect(x, y, w, h, 0x202020u);
        return;
    }
#ifndef DRACO_STABLE
    blit_bg_scaled(x, y, w, h, g_bg_overlay);
#else
    fb_fill_rect(x, y, w, h, COL_GLASS_BG);
#endif
}

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

#define BG_OVERLAY_R  4u
#define BG_OVERLAY_G  4u
#define BG_OVERLAY_B  16u

#define FONT_W  8
#define FONT_H  16
#define CORNER_R 8

/* =========================================================================
 * UI State
 * ========================================================================= */
#define NUM_WORKSPACES WS_COUNT

static int  g_ws        = 0;
static int  g_logged_in = 0;
static int  g_ticks     = 0;

static int g_cx = 0, g_cy = 0;
static int g_mouse_x_raw = -1, g_mouse_y_raw = -1;

static int      g_wallpaper_dirty = 1;
/* g_bg_overlay declared early (before desktop_blit_bg_at) — not re-declared here */

/* g_bg_overlay and g_bg_disabled declared early (before desktop_blit_bg_at) */

/* Window drag state */
static int g_drag_win   = -1;   /* handle of window being dragged, or -1 */
static int g_drag_off_x =  0;   /* cursor offset from window x when drag started */
static int g_drag_off_y =  0;   /* cursor offset from window y when drag started */

/* Window resize state ─────────────────────────────────────────────────────
 * g_resize_win  : compositor handle of window being resized (-1 = none)
 * g_resize_edge : which edge/corner is being dragged
 * g_resize_ox/oy: window origin (x,y) at the moment resize started
 * g_resize_ow/oh: window size   (w,h) at the moment resize started
 * g_resize_mx/my: cursor position at the moment resize started         */
static int           g_resize_win  = -1;
static resize_edge_t g_resize_edge = RESIZE_NONE;
static int           g_resize_ox   = 0, g_resize_oy = 0;
static int           g_resize_ow   = 0, g_resize_oh = 0;
static int           g_resize_mx   = 0, g_resize_my = 0;

static int g_about_open = 0;

/* Search */
static int  g_search_open = 0;
#define SEARCH_BUF 64
static char g_search_query[SEARCH_BUF] = "";
#define SEARCH_MAX_RESULTS 8
static char  g_sr_names[SEARCH_MAX_RESULTS][APP_NAME_LEN];
static int   g_sr_count = 0;
static int   g_sr_sel   = 0;

/* Login */
#define LOGIN_BUF 64
static char g_login_user[LOGIN_BUF] = "root";
static char g_login_pass[LOGIN_BUF] = "";
static int  g_login_field   = 1;
static char g_login_msg[128] = "";
static int  g_login_msg_err  = 0;

/* =========================================================================
 * Drawing helpers
 * ========================================================================= */
static void hline(uint32_t x, uint32_t y, uint32_t w, uint32_t c) {
    fb_fill_rect(x, y, w, 1, c);
}

static void glass_panel(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         uint32_t border_col, uint32_t fill_col) {
    uint32_t ir = CORNER_R > 2 ? CORNER_R - 2 : 1;
    fb_rounded_rect(x+1, y+1, w-2, h-2, ir, fill_col);
    hline(x+CORNER_R, y,     w-2*CORNER_R, border_col);
    hline(x+CORNER_R, y+h-1, w-2*CORNER_R, border_col);
    fb_fill_rect(x,     y+CORNER_R, 1, h-2*CORNER_R, border_col);
    fb_fill_rect(x+w-1, y+CORNER_R, 1, h-2*CORNER_R, border_col);
    for (uint32_t i = 1; i <= CORNER_R; i++) {
        fb_put_pixel(x+CORNER_R-i,     y+CORNER_R-i,     border_col);
        fb_put_pixel(x+w-CORNER_R+i-1, y+CORNER_R-i,     border_col);
        fb_put_pixel(x+CORNER_R-i,     y+h-CORNER_R+i-1, border_col);
        fb_put_pixel(x+w-CORNER_R+i-1, y+h-CORNER_R+i-1, border_col);
    }
    hline(x+CORNER_R, y,     w-2*CORNER_R, COL_GLASS_SHINE);
    hline(x+CORNER_R, y+h-1, w-2*CORNER_R, COL_ACCENT_DIM);
}

/* =========================================================================
 * Wallpaper
 * ========================================================================= */
static void blit_bg_scaled(uint32_t dx0, uint32_t dy0,
                             uint32_t dw,  uint32_t dh,
                             uint32_t overlay_a) {
    uint32_t  W      = fb.width;
    uint32_t *shadow = fb_shadow_ptr();
    if (!shadow || dw == 0 || dh == 0) return;
    for (uint32_t dy = 0; dy < dh; dy++) {
        uint32_t sy = dy * BG_H / dh;
        if (sy >= BG_H) sy = BG_H - 1;
        const uint32_t *src_row = bg_pixels + sy * BG_W;
        uint32_t       *dst_row = shadow + (dy0 + dy) * W + dx0;
        for (uint32_t dx = 0; dx < dw; dx++) {
            uint32_t sx = dx * BG_W / dw;
            if (sx >= BG_W) sx = BG_W - 1;
            uint32_t raw = src_row[sx];
            uint8_t r = (uint8_t)((raw >> 16) & 0xFF);
            uint8_t g = (uint8_t)((raw >>  8) & 0xFF);
            uint8_t b = (uint8_t)( raw        & 0xFF);
            uint8_t a = (overlay_a > 255u) ? 255u : (uint8_t)overlay_a;
            r = (uint8_t)((r*(255-a) + BG_OVERLAY_R*a) / 255);
            g = (uint8_t)((g*(255-a) + BG_OVERLAY_G*a) / 255);
            b = (uint8_t)((b*(255-a) + BG_OVERLAY_B*a) / 255);
            dst_row[dx] = fb_color(r, g, b);
        }
    }
}

static void draw_wallpaper(void) {
    if (!g_wallpaper_dirty) return;
    uint32_t W = fb.width, H = fb.height;
    if (g_bg_disabled) {
        /* DEBUG Ctrl+B: solid dark grey — windows visible even if same colour as bg */
        fb_fill_rect(0, 0, W, H, 0x202020u);
    } else {
#ifndef DRACO_STABLE
        blit_bg_scaled(0, 0, W, H, g_bg_overlay);
#else
        fb_fill_rect(0, 0, W, H, COL_GLASS_BG);
#endif
    }
    g_wallpaper_dirty = 0;
}

/* =========================================================================
 * Desktop action callbacks (called by ctx_menu.c action dispatcher)
 * ========================================================================= */

void desktop_refresh(void) {
    g_wallpaper_dirty = 1;
    kinfo("DESKTOP: refresh requested\n");
}

void desktop_change_bg(void) {
    g_bg_overlay = (g_bg_overlay > 180u) ? 90u
                 : (g_bg_overlay >  40u) ? 10u : 200u;
    g_wallpaper_dirty = 1;
}

void desktop_about_open(void) {
    g_about_open = 1;
}

void desktop_logout(void) {
    g_logged_in = 0;
    g_login_pass[0] = '\0';
    g_wallpaper_dirty = 1;
    g_about_open = 0;
}

/* Begin dragging a window — called from ctx_menu dispatch while inside
 * the main loop so g_cx/g_cy already hold the current cursor position. */
void desktop_begin_drag(int win_handle) {
    if (win_handle < 0) return;
    int ox, oy;
    comp_get_pos(win_handle, &ox, &oy);
    g_drag_win   = win_handle;
    g_drag_off_x = g_cx - ox;
    g_drag_off_y = g_cy - oy;
    /* Abort any concurrent resize */
    g_resize_win  = -1;
    g_resize_edge = RESIZE_NONE;
}

/* =========================================================================
 * About overlay
 * ========================================================================= */
static void draw_about(void) {
    if (!g_about_open) return;
    uint32_t W = fb.width, H = fb.height;
    uint32_t pw = (W > 560u) ? 480u : W - 40u;
    uint32_t ph = (H > 340u) ? 280u : H - 60u;
    uint32_t px = (W-pw)/2, py = (H-ph)/2;
    fb_fill_rect(0, 0, W, H, fb_blend(0x000000u, 0u, 160));
    glass_panel(px, py, pw, ph, COL_ACCENT, COL_GLASS_PANEL);
    fb_print_s(px+20, py+16, "DracolaxOS V1", COL_ACCENT_LT, COL_GLASS_PANEL, 2);
    hline(px+16, py+54, pw-32, COL_SEP);
    static const char *lines[] = {
        "  Kernel : Draco-1.0 x86_64 (64-bit preemptive)",
        "  ABI    : Draco native + Linux x86_64 (SYSCALL)",
        "  GUI    : Glassmorphism compositor v2",
        "  Dock   : Floating panel, Windows 11 style",
        "  Memory : PMM bitmap + bump-allocator heap",
        "  FS     : RAMFS + VFS mount tree",
        "  Author : Lunax (Yunis) + Amilcar",
        "",
        "  Press ESC or click [X] to close",
    };
    for (int i = 0; i < 9; i++)
        fb_print(px+16, py+62+(uint32_t)i*20, lines[i],
                 i == 8 ? COL_TEXT_DIM : COL_TEXT_MED, 0);
    fb_fill_rect(px+pw-26, py+8, 18, 18, COL_ERR);
    fb_rounded_rect(px+pw-26, py+8, 18, 18, 3, 0xFF8080u);
    fb_print(px+pw-23, py+11, "X", COL_TEXT_HI, COL_ERR);
}

/* =========================================================================
 * Search overlay (Ctrl+F)
 * ========================================================================= */
static int search_match(const char *hay, const char *needle) {
    if (!needle[0]) return 0;
    size_t nl = strlen(needle);
    for (size_t i = 0; hay[i]; i++) {
        int ok = 1;
        for (size_t j = 0; j < nl && ok; j++) {
            char h = hay[i+j], n = needle[j];
            if (!h) { ok = 0; break; }
            if (h >= 'A' && h <= 'Z') h += 32;
            if (n >= 'A' && n <= 'Z') n += 32;
            if (h != n) ok = 0;
        }
        if (ok) return 1;
    }
    return 0;
}

static void search_update(void) {
    g_sr_count = 0; g_sr_sel = 0;
    if (!g_search_query[0]) return;
    int total = appman_count();
    for (int i = 0; i < total && g_sr_count < SEARCH_MAX_RESULTS; i++) {
        const app_entry_t *a = appman_get(i);
        if (a && search_match(a->name, g_search_query)) {
            strncpy(g_sr_names[g_sr_count], a->name, APP_NAME_LEN - 1);
            g_sr_names[g_sr_count][APP_NAME_LEN - 1] = '\0';
            g_sr_count++;
        }
    }
}

static void draw_search(void) {
    if (!g_search_open) return;
    uint32_t dock_right = (uint32_t)(DOCK_PANEL_X + DOCK_W + 12);
    uint32_t avail = fb.width - dock_right;
    uint32_t pw = (avail > 440u) ? 400u : avail - 40u;
    uint32_t result_h = g_sr_count > 0 ? (uint32_t)g_sr_count * 24u + 8u : 0u;
    uint32_t ph = 64u + result_h;
    uint32_t px = dock_right + (avail - pw) / 2;
    uint32_t py = 20u;

    glass_panel(px, py, pw, ph, COL_ACCENT, COL_GLASS_PANEL);
    fb_print(px+10, py+20, "[/]", COL_ACCENT_LT, COL_GLASS_PANEL);
    uint32_t fx = px + 40, fw = pw - 56;
    fb_fill_rect(fx, py+14, fw, 28, 0x0C0E22u);
    fb_rounded_rect(fx, py+14, fw, 28, 4, COL_ACCENT_LT);
    fb_print(fx+6, py+20,
             g_search_query[0] ? g_search_query : "Type app name...",
             g_search_query[0] ? COL_TEXT_HI : COL_TEXT_DIM, 0);
    if (g_ticks % 60 < 40) {
        uint32_t cx3 = fx+6 + (uint32_t)strlen(g_search_query)*FONT_W;
        fb_fill_rect(cx3, py+20, 2, FONT_H, COL_ACCENT_LT);
    }
    fb_print(px+pw-80, py+20, "ESC to close", COL_TEXT_DIM, COL_GLASS_PANEL);

    if (g_sr_count > 0) {
        hline(px+8, py+56, pw-16, COL_SEP);
        for (int i = 0; i < g_sr_count; i++) {
            uint32_t ry = py+64 + (uint32_t)i*24;
            int hov = (g_cy>=(int)ry&&g_cy<(int)(ry+24)&&
                       g_cx>=(int)px&&g_cx<(int)(px+pw));
            int sel = (i == g_sr_sel);
            if (hov||sel) fb_fill_rect(px+2, ry, pw-4, 24, COL_ACCENT_DIM);
            fb_print(px+14, ry+4, g_sr_names[i],
                     (hov||sel)?COL_TEXT_HI:COL_TEXT_MED,
                     (hov||sel)?COL_ACCENT_DIM:COL_GLASS_PANEL);
        }
    } else if (g_search_query[0]) {
        hline(px+8, py+56, pw-16, COL_SEP);
        fb_print(px+14, py+64, "No matching apps", COL_TEXT_DIM, COL_GLASS_PANEL);
    }
}

/* =========================================================================
 * Login screen
 * ========================================================================= */
static void draw_login(void) {
    uint32_t W = fb.width, H = fb.height;
    /* Render wallpaper with a heavier dark overlay for login contrast.
     * DO NOT follow this with a solid fb_fill_rect — that would overwrite
     * the wallpaper with a solid colour and leave a black background. */
    blit_bg_scaled(0, 0, W, H, 180u);  /* higher overlay_a = darker tint */

    uint32_t cw = (W>440u)?380u:W-40u;
    uint32_t ch = (H>380u)?320u:H-40u;
    uint32_t cx = (W-cw)/2, cy = (H-ch)/2;
    glass_panel(cx, cy, cw, ch, COL_ACCENT, COL_GLASS_PANEL);
    fb_print_s(cx+60, cy+20, "DracolaxOS", COL_ACCENT_LT, COL_GLASS_PANEL, 2);
    hline(cx+16, cy+62, cw-32, COL_SEP);
    fb_print(cx+16, cy+72, "Sign in to your account", COL_TEXT_MED, 0);

    int uf = (g_login_field == 0);
    uint32_t fy0 = cy+104;
    fb_print(cx+16, fy0, "Username", COL_TEXT_DIM, 0);
    fb_fill_rect(cx+16, fy0+18, cw-32, 28, uf?0x1E1E40u:0x141430u);
    fb_rounded_rect(cx+16, fy0+18, cw-32, 28, 4, uf?COL_ACCENT_LT:COL_SEP);
    fb_print(cx+22, fy0+24, g_login_user, COL_TEXT_HI, 0);
    if (uf && g_ticks%60<40) {
        uint32_t cx4 = cx+22+(uint32_t)strlen(g_login_user)*FONT_W;
        fb_fill_rect(cx4, fy0+24, 2, FONT_H, COL_ACCENT_LT);
    }

    int pf = (g_login_field == 1);
    uint32_t fy1 = fy0+64;
    fb_print(cx+16, fy1, "Password", COL_TEXT_DIM, 0);
    fb_fill_rect(cx+16, fy1+18, cw-32, 28, pf?0x1E1E40u:0x141430u);
    fb_rounded_rect(cx+16, fy1+18, cw-32, 28, 4, pf?COL_ACCENT_LT:COL_SEP);
    char stars[LOGIN_BUF];
    size_t plen = strlen(g_login_pass);
    for (size_t si = 0; si < plen && si < LOGIN_BUF-1; si++) stars[si] = '*';
    stars[plen] = '\0';
    fb_print(cx+22, fy1+24, stars, COL_TEXT_HI, 0);
    if (pf && g_ticks%60<40) {
        uint32_t cx4 = cx+22+(uint32_t)plen*FONT_W;
        fb_fill_rect(cx4, fy1+24, 2, FONT_H, COL_ACCENT_LT);
    }

    uint32_t bx=cx+16, by=fy1+60, bw=cw-32, bh=32;
    fb_fill_rect(bx, by, bw, bh, COL_ACCENT);
    fb_rounded_rect(bx, by, bw, bh, 6, COL_ACCENT_LT);
    uint32_t lw = 6u*FONT_W;
    fb_print(bx+(bw-lw)/2, by+8, "Log In", COL_TEXT_HI, COL_ACCENT);

    if (g_login_msg[0]) {
        uint32_t mc = g_login_msg_err ? COL_ERR : COL_OK;
        fb_print(cx+(cw-(uint32_t)strlen(g_login_msg)*FONT_W)/2,
                 by+40, g_login_msg, mc, 0);
    }
    fb_print(cx+16, cy+ch-22,
             "Tab = switch field   Enter = login", COL_TEXT_DIM, 0);
}

static void handle_login_key(char c) {
    if (c == '\t') { g_login_field = 1-g_login_field; return; }
    if (c == '\n') {
        if (dracoauth_login(g_login_user, g_login_pass) == 0) {
            g_logged_in=1; g_wallpaper_dirty=1;
            snprintf(g_login_msg, sizeof(g_login_msg),
                     "Welcome, %s!", dracoauth_whoami());
            g_login_msg_err=0;
        } else {
            snprintf(g_login_msg, sizeof(g_login_msg), "Invalid credentials.");
            g_login_msg_err=1; g_login_pass[0]='\0';
        }
        return;
    }
    if (c == '\b') {
        char *buf = (g_login_field==0) ? g_login_user : g_login_pass;
        size_t l = strlen(buf); if (l>0) buf[l-1]='\0'; return;
    }
    if (c >= 32 && c < 127) {
        char *buf = (g_login_field==0) ? g_login_user : g_login_pass;
        size_t l = strlen(buf);
        if (l < LOGIN_BUF-1) { buf[l]=c; buf[l+1]='\0'; }
    }
}

/* =========================================================================
 * Boot logo
 * ========================================================================= */
static int g_boot_phase = 0, g_boot_ticks = 0;
#define BOOT_LOGO_FRAMES 60
#define BOOT_FADE_FRAMES 20

static void draw_boot_logo(int fa) {
    uint32_t W=fb.width, H=fb.height;
    fb_clear(COL_VOID);
    uint32_t sz=(H<W?H:W)/5, box=sz*2;
    uint32_t ox=W/2-sz, oy=H/2-sz-20;
    for (uint32_t py2=0; py2<box; py2++) {
        for (uint32_t px2=0; px2<box; px2++) {
            uint32_t bx=px2*DRACO_LOGO_W/box, by2=py2*DRACO_LOGO_H_PX/box;
            uint32_t bidx=by2*DRACO_LOGO_W+bx;
            uint8_t byte=draco_logo_1bpp[bidx>>3];
            uint8_t bit=(byte>>(7-(bidx&7)))&1;
            if (!bit) continue;
            int dx=(int)px2-(int)sz, dy2=(int)py2-(int)sz;
            uint32_t d2=(uint32_t)(dx*dx+dy2*dy2),m2=sz*sz;
            uint8_t blend=(d2<m2)?(uint8_t)(200+55*(m2-d2)/m2):200u;
            uint32_t col=fb_blend(0xFFFFFFu,0xAA66FFu,blend);
            if (fa<255){uint8_t a=(uint8_t)fa;col=fb_color((uint8_t)(((col>>16)&0xFF)*a/255),(uint8_t)(((col>>8)&0xFF)*a/255),(uint8_t)((col&0xFF)*a/255));}
            if (ox+px2<W&&oy+py2<H) fb_put_pixel(ox+px2,oy+py2,col);
        }
    }
    const char *nm="DracolaxOS"; uint32_t nw=(uint32_t)strlen(nm)*16;
    uint32_t ct=COL_TEXT_HI;
    if(fa<255){uint8_t a=(uint8_t)fa;ct=fb_color((uint8_t)(0xF0*a/255),(uint8_t)(0xF0*a/255),(uint8_t)(0xFF*a/255));}
    fb_print_s((W-nw)/2,oy+box+16,nm,ct,0,2);
}

/* =========================================================================
 * Desktop task — entry point
 * ========================================================================= */
void desktop_task(void) {
    __asm__ volatile ("sti");
    fb_console_lock(1);
    kinfo("DESKTOP: v2.0 starting\n");

    if (!fb.available) {
        vga_print("\n    DracolaxOS — No VESA framebuffer.\n");
        vga_print("    Use mode=graphical in GRUB entry.\n");
        for (;;) sched_sleep(1000);
    }

    fb_enable_shadow();
    fb_clear(COL_VOID);
    fb_flip();
    cursor_init();
    /* Explicitly initialise both subsystems — comp_task/wm_task are NOT
     * spawned; desktop_task owns the render loop and drives them directly. */
    comp_init();
    wm_init();
    sched_yield(); /* heartbeat after early hw init */

    /* Init dock (loads icons, sets default pins) */
    dock_init();
    sched_yield(); /* heartbeat after dock init */

    /* First-boot installer */
    {
        int ns = (dracoauth_login("__probe__","")!=0 &&
                  dracoauth_login("root","dracolax")==0);
        dracoauth_logout();
        if (ns) { kinfo("DESKTOP: first boot installer\n"); installer_run(); sched_yield(); }
    }

    g_cx = (int)fb.width  / 2;
    g_cy = (int)fb.height / 2;

    for (;;) {
        g_ticks++;

        /* ── Boot logo ── */
#ifndef DRACO_STABLE
        if (g_boot_phase < 2) {
            g_boot_ticks++;
            if (g_boot_phase == 0) {
                draw_boot_logo(255); fb_flip();
                if (g_boot_ticks >= BOOT_LOGO_FRAMES) { g_boot_phase=1; g_boot_ticks=0; }
            } else {
                int alpha=255-g_boot_ticks*255/BOOT_FADE_FRAMES;
                if(alpha<0)alpha=0;
                draw_boot_logo(alpha); fb_flip();
                if (g_boot_ticks>=BOOT_FADE_FRAMES) {
                    g_boot_phase=2; g_boot_ticks=0;
                    fb_clear(COL_VOID); fb_flip(); g_wallpaper_dirty=1;
                }
            }
            sched_sleep(33); continue;
        }
#else
        g_boot_phase=2; g_wallpaper_dirty=1;
#endif

        /* ── Mouse input ── */
        /* CRITICAL ORDER: mouse_update_edges() FIRST to snapshot previous button
         * state, THEN vmmouse_poll() to update current state.
         * Reversed order caused mbuttons == mbuttons_prev always → btn_pressed = 0. */
        mouse_update_edges();
        vmmouse_poll();
        int nmx=mouse_get_x(), nmy=mouse_get_y();
        if (nmx!=g_mouse_x_raw||nmy!=g_mouse_y_raw) {
            g_cx=nmx; g_cy=nmy;
            g_mouse_x_raw=nmx; g_mouse_y_raw=nmy;
        }

        /* ── Keyboard input ── */
        int c;
        while ((c=keyboard_getchar())!=0) {
            uint8_t uc=(uint8_t)c;

            /* Arrow keys move cursor */
            if(uc==KB_KEY_UP)   {g_cy-=4;if(g_cy<0)g_cy=0;g_mouse_x_raw=g_cx;g_mouse_y_raw=g_cy;continue;}
            if(uc==KB_KEY_DOWN) {g_cy+=4;if(g_cy>=(int)fb.height)g_cy=(int)fb.height-1;g_mouse_x_raw=g_cx;g_mouse_y_raw=g_cy;continue;}
            if(uc==KB_KEY_LEFT) {g_cx-=4;if(g_cx<0)g_cx=0;g_mouse_x_raw=g_cx;g_mouse_y_raw=g_cy;continue;}
            if(uc==KB_KEY_RIGHT){g_cx+=4;if(g_cx>=(int)fb.width)g_cx=(int)fb.width-1;g_mouse_x_raw=g_cx;g_mouse_y_raw=g_cy;continue;}

            if(uc==KB_KEY_F12){dbgcon_toggle();continue;}

            if(!g_logged_in){handle_login_key((char)c);continue;}

            /* Workspace switcher swallows all keys while open */
            if(ws_switcher_is_open()){
                int nws=ws_switcher_key(c,g_ws);
                if(nws>=0){
                    g_ws=nws;g_wallpaper_dirty=1;
                    wm_switch_desktop(g_ws);comp_switch_desktop(g_ws);
                } else if(!ws_switcher_is_open()){
                    /* Closed via ESC/Tab without switching — must redraw
                     * wallpaper to clear the tint applied by ws_switcher_draw(). */
                    g_wallpaper_dirty=1;
                }
                continue;
            }

            /* Tab → workspace switcher — mark wallpaper dirty so the
             * switcher's tint starts from a fresh background not an
             * already-tinted shadow buffer. */
            if(c=='\t'){g_wallpaper_dirty=1;ws_switcher_open();continue;}

            /* Escape → close top overlay */
            if(c==0x1B){
                if(g_about_open){g_about_open=0;g_wallpaper_dirty=1;}
                else if(ctx_menu_is_open()){ctx_menu_close();}
                else if(g_search_open){g_search_open=0;g_search_query[0]='\0';g_sr_count=0;g_wallpaper_dirty=1;}
                continue;
            }

            /* Ctrl+F → toggle search */
            if(c==0x06){
                g_search_open=!g_search_open;
                if(!g_search_open){g_search_query[0]='\0';g_sr_count=0;}
                g_wallpaper_dirty=1; continue;
            }

            /* Ctrl+W — DEBUG: open a plain 300×300 test window.
             * If this window appears, the compositor pipeline is working
             * and the problem is in the app launch chain.
             * If it does NOT appear, the compositor itself is broken. */
            if(c==0x17){
                int dbg_w = comp_create_window("DEBUG TEST WINDOW",
                    (int)(fb.width/2)  - 150,
                    (int)(fb.height/2) - 150,
                    300u, 300u);
                if(dbg_w >= 0){
                    /* Fill bright magenta so it can't be confused with anything */
                    comp_window_fill(dbg_w, 0, 0, 300u, 300u, 0xFF00FFu);
                    comp_window_print(dbg_w, 10u, 10u,
                        "Ctrl+W debug window", 0xFFFFFFu);
                    comp_window_print(dbg_w, 10u, 30u,
                        "If visible: compositor OK", 0xFFFF00u);
                    comp_window_print(dbg_w, 10u, 50u,
                        "Close: right-click dock", 0xCCCCCCu);
                    kinfo("DESKTOP: debug window created id=%d\n", dbg_w);
                } else {
                    kinfo("DESKTOP: debug window FAILED (compositor full or heap)\n");
                }
                g_wallpaper_dirty=1; continue;
            }

            /* Ctrl+B — DEBUG: toggle wallpaper off/on.
             * With bg disabled the screen fills solid dark grey. Any window
             * that was hidden behind the wallpaper will become visible.
             * If a window appears after Ctrl+B → draw order bug (window rendered
             * before wallpaper, then overwritten). */
            if(c==0x02){
                g_bg_disabled = !g_bg_disabled;
                g_wallpaper_dirty=1;
                kinfo("DESKTOP: bg %s\n", g_bg_disabled?"OFF":"ON");
                continue;
            }

            /* Search typing */
            if(g_search_open){
                if(c>=32&&c<127){
                    size_t l=strlen(g_search_query);
                    if(l<SEARCH_BUF-1){g_search_query[l]=(char)c;g_search_query[l+1]='\0';}
                    search_update();
                    /* BUG FIX: panel height changes when results appear/disappear.
                     * Without a fresh wallpaper blit the old (larger or smaller)
                     * panel area leaves ghost pixels behind the new one. */
                    g_wallpaper_dirty = 1;
                }else if(c=='\b'){
                    size_t l=strlen(g_search_query);
                    if(l>0)g_search_query[l-1]='\0';
                    search_update();
                    g_wallpaper_dirty = 1; /* same reason as above */
                }else if(c=='\n'||c=='\r'){
                    int t=(g_sr_sel<g_sr_count)?g_sr_sel:(g_sr_count>0?0:-1);
                    if(t>=0)appman_launch(g_sr_names[t]);
                    g_search_open=0;g_search_query[0]='\0';g_sr_count=0;
                }else if(uc==KB_KEY_DOWN&&g_sr_sel<g_sr_count-1){g_sr_sel++;}
                else if(uc==KB_KEY_UP&&g_sr_sel>0){g_sr_sel--;}
                continue;
            }

            /* F1..F4 → direct workspace switch (was Ctrl+1..4, which intercepted Ctrl+C/D) */
            if(uc==KB_KEY_F1||uc==KB_KEY_F2||uc==KB_KEY_F3||uc==KB_KEY_F4){
                g_ws=(int)(uc-KB_KEY_F1);
                g_wallpaper_dirty=1;
                wm_switch_desktop(g_ws);
                comp_switch_desktop(g_ws);
            }
        }

        /* ── Clamp cursor ── */
        if(g_cx<0)g_cx=0;
        if(g_cy<0)g_cy=0;
        if(g_cx>=(int)fb.width) g_cx=(int)fb.width-1;
        if(g_cy>=(int)fb.height)g_cy=(int)fb.height-1;

        /* ── Window drag: update position each frame while left button held ── */
        if(g_drag_win >= 0) {
            if(mouse_btn_held(MOUSE_BTN_LEFT)) {
                int nx = g_cx - g_drag_off_x;
                int ny = g_cy - g_drag_off_y;
                if(nx < 0) nx = 0;
                if(ny < 0) ny = 0;
                if(nx > (int)fb.width  - 40) nx = (int)fb.width  - 40;
                if(ny > (int)fb.height - 30) ny = (int)fb.height - 30;
                comp_move_window(g_drag_win, (uint32_t)nx, (uint32_t)ny);
                g_wallpaper_dirty = 1;
            } else {
                g_drag_win = -1;   /* button released — stop dragging */
            }
        }

        /* ── Window resize: update geometry each frame while left button held ─
         * Only active when a resize edge was captured on the mouse-down event.
         * The resize is computed as a delta from the geometry at drag-start so
         * that accumulated floating-point error never creeps in.               */
        if(g_resize_win >= 0) {
            if(mouse_btn_held(MOUSE_BTN_LEFT)) {
                int dx = g_cx - g_resize_mx;
                int dy = g_cy - g_resize_my;
                int nx = g_resize_ox, ny = g_resize_oy;
                int nw = g_resize_ow, nh = g_resize_oh;
                switch(g_resize_edge) {
                case RESIZE_E:  nw = g_resize_ow + dx; break;
                case RESIZE_S:  nh = g_resize_oh + dy; break;
                case RESIZE_W:  nx = g_resize_ox + dx; nw = g_resize_ow - dx; break;
                case RESIZE_N:  ny = g_resize_oy + dy; nh = g_resize_oh - dy; break;
                case RESIZE_SE: nw = g_resize_ow + dx; nh = g_resize_oh + dy; break;
                case RESIZE_SW: nx = g_resize_ox + dx; nw = g_resize_ow - dx;
                                nh = g_resize_oh + dy; break;
                case RESIZE_NE: nh = g_resize_oh - dy; ny = g_resize_oy + dy;
                                nw = g_resize_ow + dx; break;
                case RESIZE_NW: nx = g_resize_ox + dx; nw = g_resize_ow - dx;
                                ny = g_resize_oy + dy; nh = g_resize_oh - dy; break;
                default: break;
                }
                /* Clamp: keep on screen and enforce minimum size */
                if(nw < 120) { if(nx != g_resize_ox) nx = g_resize_ox + g_resize_ow - 120; nw = 120; }
                if(nh <  60) { if(ny != g_resize_oy) ny = g_resize_oy + g_resize_oh -  60; nh =  60; }
                if(nx < 0) nx = 0;
                if(ny < 0) ny = 0;
                comp_set_geometry(g_resize_win,
                                  (uint32_t)nx, (uint32_t)ny,
                                  (uint32_t)nw, (uint32_t)nh);
                g_wallpaper_dirty = 1;
            } else {
                g_resize_win  = -1;
                g_resize_edge = RESIZE_NONE;
            }
        }

        /* ── Mouse clicks ── */
        if(mouse_btn_pressed(MOUSE_BTN_LEFT)){
            if(!g_logged_in){
                /* Login button hit-test */
                uint32_t W=fb.width,H=fb.height;
                uint32_t cw=(W>440u)?380u:W-40u, ch=(H>380u)?320u:H-40u;
                uint32_t lcx=(W-cw)/2, lcy=(H-ch)/2;
                uint32_t fy1=lcy+104u+64u;
                uint32_t bx=lcx+16,by=fy1+60,bw=cw-32,bh=32; (void)ch;
                if(g_cx>=(int)bx&&g_cx<(int)(bx+bw)&&g_cy>=(int)by&&g_cy<(int)(by+bh))
                    handle_login_key('\n');
            } else if(ws_switcher_is_open()){
                int nws=ws_switcher_click(g_cx,g_cy);
                if(nws>=0){
                    g_ws=nws;g_wallpaper_dirty=1;
                    wm_switch_desktop(g_ws);comp_switch_desktop(g_ws);
                } else if(!ws_switcher_is_open()){
                    g_wallpaper_dirty=1;
                }
            } else if(ctx_menu_is_open()){
                /* ── Layer 1 → 3: forward click to context menu ──────────── */
                int consumed = ctx_menu_click(g_cx, g_cy);
                if(!consumed){
                    /* Click was outside the menu — dismiss and fall through
                     * so the click still interacts with whatever is below. */
                    ctx_menu_close();
                    /* Do NOT fall-through into window/dock handling on this
                     * same frame — avoids accidentally activating elements
                     * that were under the menu. */
                }
                g_wallpaper_dirty = 1;
            } else if(g_about_open){
                uint32_t W=fb.width,H=fb.height,pw=480,ph=280;
                uint32_t px=(W-pw)/2,py=(H-ph)/2;
                if(g_cx>=(int)(px+pw-26)&&g_cx<(int)(px+pw-8)&&g_cy>=(int)(py+8)&&g_cy<(int)(py+26)){g_about_open=0;g_wallpaper_dirty=1;}
            } else if(g_search_open){
                uint32_t dock_right=(uint32_t)(DOCK_PANEL_X+DOCK_W+12);
                uint32_t avail=fb.width-dock_right;
                uint32_t pw=(avail>440u)?400u:avail-40u;
                uint32_t rh=g_sr_count>0?(uint32_t)g_sr_count*24u+8u:0u;
                uint32_t ph=64u+rh, px2=dock_right+(avail-pw)/2, py2=20u;
                if(g_sr_count>0&&g_cx>=(int)px2&&g_cx<(int)(px2+pw)&&g_cy>=(int)(py2+64u)&&g_cy<(int)(py2+ph)){
                    int row=((int)g_cy-(int)(py2+64u))/24;
                    if(row>=0&&row<g_sr_count)appman_launch(g_sr_names[row]);
                    g_search_open=0;g_search_query[0]='\0';g_sr_count=0;g_wallpaper_dirty=1;
                }else if(g_cx<(int)px2||g_cx>=(int)(px2+pw)||g_cy<(int)py2||g_cy>=(int)(py2+ph)){
                    g_search_open=0;g_search_query[0]='\0';g_sr_count=0;g_wallpaper_dirty=1;
                }
            } else {
                /* ── Window title bar click: close / maximize / minimize / drag ── */
                int wid = comp_title_bar_at(g_cx, g_cy);
                if(wid >= 0) {
                    comp_focus_window(wid);
                    g_wallpaper_dirty = 1;
                    if(comp_close_at(wid, g_cx, g_cy)) {
                        /* Close: destroy window, notify dock.
                         * BUG FIX (freeze on exit): clear drag/resize state
                         * that may hold this handle — if left dangling the
                         * next frame tries to move/resize a dead window and
                         * the compositor index is reused unpredictably.
                         * Also return keyboard focus to the desktop task (0)
                         * so the main loop receives keys again. */
                        int tid = comp_get_task_id(wid);
                        dock_task_died(tid);
                        comp_destroy_window(wid);
                        if(g_drag_win   == wid) g_drag_win   = -1;
                        if(g_resize_win == wid) { g_resize_win = -1; g_resize_edge = RESIZE_NONE; }
                        input_router_set_focus(0); /* return focus to desktop */
                    } else if(comp_maximize_at(wid, g_cx, g_cy)) {
                        /* Maximize/restore toggle */
                        comp_toggle_maximize(wid);
                    } else if(comp_minimize_at(wid, g_cx, g_cy)) {
                        /* Minimize: hide window (toggle visible) */
                        comp_set_visible(wid, 0);
                    } else {
                        /* Not on a button — start drag */
                        int ox, oy;
                        comp_get_pos(wid, &ox, &oy);
                        g_drag_win   = wid;
                        g_drag_off_x = g_cx - ox;
                        g_drag_off_y = g_cy - oy;
                    }
                } else {
                    /* ── Dock gets FIRST priority — its 6px resize halo
                     * must not swallow dock clicks.  Check the dock before
                     * comp_window_at so icon clicks always reach dock_click(). */
                    if(dock_click(g_cx, g_cy)) {
                        /* dock handled it — nothing more to do */
                    } else {
                    /* ── Resize edge check (window border) ── */
                    int rwid = comp_window_at(g_cx, g_cy);
                    if(rwid >= 0) {
                        resize_edge_t edge = comp_resize_edge_at(rwid, g_cx, g_cy);
                        if(edge != RESIZE_NONE) {
                            comp_focus_window(rwid);
                            g_resize_win  = rwid;
                            g_resize_edge = edge;
                            g_resize_mx   = g_cx;
                            g_resize_my   = g_cy;
                            int rx,ry,rw,rh;
                            comp_get_geometry(rwid,&rx,&ry,&rw,&rh);
                            g_resize_ox = rx; g_resize_oy = ry;
                            g_resize_ow = rw; g_resize_oh = rh;
                            g_wallpaper_dirty = 1;
                        } else {
                            /* Click inside body — focus but no drag/resize */
                            comp_focus_window(rwid);
                            g_wallpaper_dirty = 1;
                        }
                    }
                    /* else: click was on empty desktop — no action */
                    }
                }
            }
        }

        /* ── Right-click: 3-layer context menu system ──────────────────────
         *   Layer 1 (input): detected here — mouse_btn_pressed(RIGHT)
         *   Layer 2 (resolver): ctx_resolve(x, y) classifies the target
         *   Layer 3 (UI): ctx_menu_open(&hit) renders correct menu
         *
         * Also forward to dock_right_click() so pin/unpin still works
         * via direct right-click on a dock slot (legacy path).
         */
        if(mouse_btn_pressed(MOUSE_BTN_RIGHT) && g_logged_in && !ws_switcher_is_open()){
            /* Close any open menu first (toggle behaviour). */
            if(ctx_menu_is_open()){
                ctx_menu_close();
            } else {
                /* Let dock handle pin/unpin right-click internally */
                int dock_consumed = dock_right_click(g_cx, g_cy);
                /* Always open a context menu regardless — dock_right_click
                 * does its pin toggle, ctx_menu shows the dock menu too. */
                (void)dock_consumed;
                ctx_hit_t hit = ctx_resolve(g_cx, g_cy);
                ctx_menu_open(&hit);
            }
            g_wallpaper_dirty = 1;
        }

        /* ── Draw ── */
        if(!g_logged_in){
            draw_login();
        } else {
            /* When the workspace switcher is open: redraw wallpaper every frame
             * so ws_switcher_draw always tints from a clean base.
             * This prevents the "frozen dark screen" that happened when we tinted
             * an already-tinted buffer, AND prevents the one-frame flash that
             * happened when wallpaper was redrawn but tint was only applied once. */
            if (ws_switcher_is_open()) g_wallpaper_dirty = 1;
            else if (comp_has_windows()) g_wallpaper_dirty = 1;
            draw_wallpaper();                    /* full-screen bg, marks clean */
            wm_render_frame();                   /* WM windows */
            comp_render();                       /* compositor windows */
            draw_about();                        /* about overlay */
            draw_search();                       /* search overlay */
            if(ws_switcher_is_open())
                ws_switcher_draw(g_cx,g_cy,g_ws); /* workspace switcher */
            dock_draw(g_cx,g_cy);   /* dock drawn above windows */
            ctx_menu_draw(g_cx, g_cy); /* context menu TOPMOST — above dock */
        }

        dbgcon_draw();
        fb_flip();

        /* ── Cursor shape update ────────────────────────────────────────
         * Determine the correct cursor for the current hover target and
         * apply it every frame AFTER fb_flip() so cursor_move() stamps
         * the right bitmap onto VRAM.
         *
         * Priority (highest first):
         *  1. Active drag     → GRAB
         *  2. Active resize   → RESIZE_H / RESIZE_V (kept during drag)
         *  3. Resize edge     → RESIZE_H / RESIZE_V
         *  4. Title bar       → ARROW
         *  5. Window body     → TEXT (all app windows are terminal-style)
         *  6. Dock hover      → HAND
         *  7. Search input    → TEXT (only the text-entry rect)
         *  8. Elsewhere       → ARROW
         */
        {
            cursor_type_t ctype = CURSOR_ARROW;

            if (g_drag_win >= 0) {
                /* Active window drag — closed fist */
                ctype = CURSOR_GRAB;
            } else if (g_resize_win >= 0) {
                /* Active resize — keep resize direction cursor */
                switch (g_resize_edge) {
                case RESIZE_E: case RESIZE_W:
                case RESIZE_NW: case RESIZE_SE: ctype = CURSOR_RESIZE_H; break;
                case RESIZE_N: case RESIZE_S:
                case RESIZE_NE: case RESIZE_SW: ctype = CURSOR_RESIZE_V; break;
                default: ctype = CURSOR_ARROW; break;
                }
            } else if (g_logged_in) {
                /* Check resize edges on any hovered window */
                int hwid = comp_window_at(g_cx, g_cy);
                if (hwid >= 0) {
                    resize_edge_t re = comp_resize_edge_at(hwid, g_cx, g_cy);
                    switch (re) {
                    case RESIZE_E: case RESIZE_W:
                    case RESIZE_NW: case RESIZE_SE: ctype = CURSOR_RESIZE_H; break;
                    case RESIZE_N: case RESIZE_S:
                    case RESIZE_NE: case RESIZE_SW: ctype = CURSOR_RESIZE_V; break;
                    default: ctype = CURSOR_ARROW; break;
                    }
                    if (re == RESIZE_NONE) {
                        /* Inside window — title bar gets arrow, body gets text */
                        if (comp_title_bar_at(g_cx, g_cy) >= 0)
                            ctype = CURSOR_ARROW;
                        else
                            ctype = CURSOR_TEXT; /* terminal/app body */
                    }
                } else if (dock_hover_slot() >= 0) {
                    ctype = CURSOR_HAND;
                } else if (g_search_open) {
                    /* TEXT only over the actual text-input field, not the
                     * whole panel.  Recompute search input rect here. */
                    uint32_t dock_right = (uint32_t)(DOCK_PANEL_X + DOCK_W + 12);
                    uint32_t avail = fb.width - dock_right;
                    uint32_t pw = (avail > 440u) ? 400u : avail - 40u;
                    uint32_t px_s = dock_right + (avail - pw) / 2;
                    uint32_t py_s = 20u;
                    /* Input field: fx = px_s+40, fy = py_s+14, fw = pw-56, fh = 28 */
                    uint32_t fx = px_s + 40u;
                    uint32_t fy = py_s + 14u;
                    uint32_t fw = pw - 56u;
                    uint32_t fh = 28u;
                    if (g_cx >= (int)fx && g_cx < (int)(fx + fw) &&
                        g_cy >= (int)fy && g_cy < (int)(fy + fh)) {
                        ctype = CURSOR_TEXT;
                    }
                    /* else arrow over result list, close hint, etc. */
                }
            }

            cursor_set_type(ctype);
        }

        cursor_move((uint32_t)g_cx,(uint32_t)g_cy);
        sched_sleep(33);
    }
}
