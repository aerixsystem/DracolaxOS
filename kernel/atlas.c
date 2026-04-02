/* kernel/atlas.c — Boot animation atlas + boot screen
 *
 * Boot screen features (v1.0):
 *   - Deep gradient background (top-to-bottom dark navy)
 *   - Actual dragon logo from draco_logo.h (80x80 1bpp bitmap)
 *   - "DracolaxOS" title + "booting..." subtitle (scaled bitmap font)
 *   - Rounded progress bar (320px wide, fills 0→100%)
 *   - Spinner ring around progress bar (8 arc segments, gradient color)
 *     Spinner bg matches gradient at that Y — no rectangle artifact
 *     Spinner fades out when bar reaches 100%
 *   - Smooth fade to black before handing over to desktop
 */
#include "types.h"
#include "klibc.h"
#include "log.h"
#include "drivers/vga/fb.h"
#include "drivers/vga/vga.h"
#include "sched/sched.h"
#include "arch/x86_64/pic.h"
#include "atlas.h"
#include "drivers/vga/cursor.h"
#include "draco_logo.h"

atlas_t g_atlas = { .nanim = 0 };

/* ---- Embedded atlas XML ------------------------------------------------ */
static const char *ATLAS_XML =
    "<atlas>"
    "<animation name=\"boot/appear\" loop=\"true\">"
    "<frame x=\"2\" y=\"2\" w=\"360\" h=\"360\"/>"
    "<frame x=\"364\" y=\"2\" w=\"360\" h=\"360\"/>"
    "<frame x=\"726\" y=\"2\" w=\"360\" h=\"360\"/>"
    "</animation>"
    "<animation name=\"boot/loading\" loop=\"true\">"
    "<frame x=\"2\" y=\"364\" w=\"360\" h=\"360\"/>"
    "<frame x=\"364\" y=\"364\" w=\"360\" h=\"360\"/>"
    "<frame x=\"726\" y=\"364\" w=\"360\" h=\"360\"/>"
    "</animation>"
    "<animation name=\"boot/finish\" loop=\"false\">"
    "<frame x=\"2\" y=\"726\" w=\"360\" h=\"360\"/>"
    "</animation>"
    "</atlas>";

/* ---- Mini XML parser ---------------------------------------------------- */
static int xml_attr(const char *tag_start, const char *attr,
                    char *out, size_t outsz) {
    size_t alen = strlen(attr);
    const char *p = tag_start;
    while (*p) {
        if (strncmp(p, attr, alen) == 0 && p[alen] == '=') {
            p += alen + 1;
            char delim = (*p == '"') ? '"' : '\'';
            if (*p == delim) p++;
            size_t i = 0;
            while (*p && *p != delim && i < outsz-1)
                out[i++] = *p++;
            out[i] = '\0';
            return 1;
        }
        p++;
    }
    return 0;
}
static uint32_t xml_atou(const char *tag, const char *attr) {
    char buf[16];
    if (xml_attr(tag, attr, buf, sizeof(buf))) return (uint32_t)atoi(buf);
    return 0;
}

void atlas_init(void) {
    const char *p = ATLAS_XML;
    atlas_anim_t *cur = NULL;
    while (*p) {
        while (*p && *p != '<') p++;
        if (!*p) break;
        p++;
        if (strncmp(p, "animation", 9) == 0) {
            if (g_atlas.nanim >= ATLAS_MAX_ANIMS) { while(*p && *p!='>') p++; continue; }
            cur = &g_atlas.anims[g_atlas.nanim++];
            memset(cur, 0, sizeof(*cur));
            char name[ATLAS_NAME_LEN];
            if (xml_attr(p, "name", name, sizeof(name)))
                strncpy(cur->name, name, ATLAS_NAME_LEN-1);
            char loop_s[8];
            cur->loop = (xml_attr(p, "loop", loop_s, sizeof(loop_s)) &&
                         strcmp(loop_s, "true") == 0) ? 1 : 0;
        } else if (strncmp(p, "frame", 5) == 0 && cur) {
            if (cur->nframes < ATLAS_MAX_FRAMES) {
                atlas_frame_t *fr = &cur->frames[cur->nframes++];
                fr->x = xml_atou(p, "x"); fr->y = xml_atou(p, "y");
                fr->w = xml_atou(p, "w"); fr->h = xml_atou(p, "h");
            }
        } else if (strncmp(p, "/animation", 10) == 0) {
            cur = NULL;
        }
        while (*p && *p != '>') p++;
    }
    kinfo("ATLAS: parsed %d animations\n", g_atlas.nanim);
}

atlas_anim_t *atlas_get(const char *name) {
    for (int i = 0; i < g_atlas.nanim; i++)
        if (strcmp(g_atlas.anims[i].name, name) == 0)
            return &g_atlas.anims[i];
    return NULL;
}

const atlas_frame_t *atlas_next_frame(atlas_anim_t *anim) {
    if (!anim || anim->nframes == 0) return NULL;
    const atlas_frame_t *fr = &anim->frames[anim->current];
    anim->current++;
    if (anim->current >= anim->nframes) {
        if (anim->loop) anim->current = 0;
        else            anim->current = anim->nframes - 1;
    }
    return fr;
}

/* ---- Gradient row color (matches background) ---------------------------- */
/* bg_top and bg_bot define the vertical gradient used across the full screen.
 * Given a screen Y, returns the exact gradient color at that row.
 * Used to correctly erase the spinner background without a solid rectangle. */
static uint32_t grad_color(uint32_t screen_y, uint32_t H,
                            uint32_t bg_top, uint32_t bg_bot) {
    uint8_t t = (uint8_t)(screen_y * 255 / H);
    return fb_blend(bg_bot, bg_top, t);
}

/* ---- Dragon logo renderer (from draco_logo.h 1bpp bitmap) -------------- */
/* Renders the 80x80 1bpp bitmap scaled to fit a box of radius `sz`.
 * White pixels are rendered as an accent purple-to-white gradient.
 * Black pixels are transparent (bg shows through). */
static void draw_logo_bitmap(uint32_t cx, uint32_t cy, uint32_t sz,
                              uint32_t H, uint32_t bg_top, uint32_t bg_bot) {
    if (!fb.available) return;
    uint32_t box = sz * 2;   /* render into box x box pixels */
    uint32_t ox = cx - sz;
    uint32_t oy = cy - sz;

    for (uint32_t py = 0; py < box; py++) {
        for (uint32_t px = 0; px < box; px++) {
            /* Map render pixel → bitmap pixel */
            uint32_t bx = px * DRACO_LOGO_W  / box;
            uint32_t by = py * DRACO_LOGO_H_PX / box;
            uint32_t bit_idx = by * DRACO_LOGO_W + bx;
            uint8_t  byte    = draco_logo_1bpp[bit_idx >> 3];
            uint8_t  bit     = (byte >> (7 - (bit_idx & 7))) & 1;

            if (!bit) {
                /* Transparent — draw background gradient */
                uint32_t screen_y = oy + py;
                fb_put_pixel(ox + px, screen_y, grad_color(screen_y, H, bg_top, bg_bot));
                continue;
            }

            /* White logo pixel → purple accent, brighter toward center */
            int dx = (int)px - (int)sz;
            int dy = (int)py - (int)sz;
            uint32_t dist2 = (uint32_t)(dx*dx + dy*dy);
            uint32_t max2  = sz * sz;
            /* alpha: 255 at center → 180 at edge */
            uint8_t alpha = (dist2 < max2)
                ? (uint8_t)(255 - (dist2 * 75 / max2))
                : 180u;
            uint32_t logo_col = fb_blend(0xFFFFFFu, 0xAA66FFu, alpha);
            fb_put_pixel(ox + px, oy + py, logo_col);
        }
    }
}

/* ---- Spinner (8 arc segments, gradient-correct background) -------------- */
/* frame   : 0-based animation frame (determines which segment lights up)
 * alpha   : 0=invisible 255=full (for fade-out effect)
 * cx,cy   : center position on screen
 * H,bg_*  : gradient params to match the bg exactly                         */
static void draw_spinner(uint32_t cx, uint32_t cy, int frame, int alpha,
                         uint32_t fg, uint32_t H,
                         uint32_t bg_top, uint32_t bg_bot) {
    if (!fb.available || alpha <= 0) return;

    static const int8_t seg_dx[8] = { 0, 18, 26, 18,  0,-18,-26,-18};
    static const int8_t seg_dy[8] = {-26,-18,  0, 18, 26, 18,  0,-18};
    static const uint8_t seg_r[8] = {  5,  4,  5,  4,  5,  4,  5,  4};

    for (int i = 0; i < 8; i++) {
        /* Brightness based on distance from current frame segment */
        int dist = (i - (frame % 8) + 8) % 8;
        uint8_t brightness = (dist == 0) ? 255 :
                             (dist == 1) ? 180 :
                             (dist == 7) ? 140 :
                             (dist == 2) ? 80  : 30;

        /* Scale by global alpha for fade-out */
        brightness = (uint8_t)((int)brightness * alpha / 255);

        int sx = (int)cx + seg_dx[i];
        int sy = (int)cy + seg_dy[i];
        uint8_t r = seg_r[i];

        /* Draw a small filled circle for each segment */
        for (int dy = -(int)r; dy <= (int)r; dy++) {
            for (int dx2 = -(int)r; dx2 <= (int)r; dx2++) {
                if (dx2*dx2 + dy*dy > (int)((uint32_t)r * r)) continue;
                uint32_t px = (uint32_t)(sx + dx2);
                uint32_t py = (uint32_t)(sy + dy);
                if (px >= fb.width || py >= fb.height) continue;
                /* Blend segment color over exact gradient bg */
                uint32_t bg = grad_color(py, H, bg_top, bg_bot);
                uint32_t seg_col = fb_blend(fg, bg, brightness);
                fb_put_pixel(px, py, seg_col);
            }
        }
    }
}

/* ---- VGA text spinner --------------------------------------------------- */
static void vga_spinner(int frame) {
    static const char sp[] = "-\\|/";
    char buf[4] = " \b\0";
    buf[0] = sp[frame & 3];
    vga_print(buf);
}

void atlas_play(const char *name, uint32_t screen_x, uint32_t screen_y,
                int nframes_to_play, uint32_t frame_delay_ms) {
    atlas_anim_t *anim = atlas_get(name);
    uint32_t bg_col = fb_color(10, 10, 20);
    uint32_t fg_col = fb_color(120, 60, 200);
    uint32_t H = fb.available ? fb.height : 768;
    uint32_t bg_top = fb_color(4, 4, 12);
    uint32_t bg_bot = fb_color(10, 6, 28);
    for (int f = 0; f < nframes_to_play; f++) {
        if (fb.available)
            draw_spinner(screen_x, screen_y, f, 255, fg_col, H, bg_top, bg_bot);
        else
            vga_spinner(f);
        if (anim) atlas_next_frame(anim);
        if (frame_delay_ms > 0) sched_sleep(frame_delay_ms);
    }
    (void)bg_col;
}

/* ---- Boot sequence ------------------------------------------------------ */
void atlas_boot_sequence(void) {
    if (fb.available) {
        uint32_t W  = fb.width;
        uint32_t H  = fb.height;
        uint32_t cx = W / 2;
        uint32_t cy = H / 2;

        /* ── Background gradient ──────────────────────────────────────── */
        uint32_t bg_top = fb_color(4,  4, 12);
        uint32_t bg_bot = fb_color(10, 6, 28);
        for (uint32_t y = 0; y < H; y++) {
            uint8_t t = (uint8_t)(y * 255 / H);
            fb_fill_rect(0, y, W, 1, fb_blend(bg_bot, bg_top, t));
        }

        /* ── Build info — top-left ──────────────────────────────────── */
        fb_print(6, 5, "DracolaxOS v1.0 [x86_64]", 0x444466u, bg_top);

        /* ── Dragon logo ─────────────────────────────────────────────── */
        uint32_t logo_sz = 52;          /* half-size → 104x104 box */
        uint32_t logo_cy = cy - 108;
        draw_logo_bitmap(cx, logo_cy, logo_sz, H, bg_top, bg_bot);

        /* ── Title: "DracolaxOS" ─────────────────────────────────────── */
        static const char *title = "DracolaxOS";
        uint32_t title_w = (uint32_t)(strlen(title)) * 8 * 3;
        uint32_t title_x = (W > title_w) ? (W - title_w) / 2 : 0;
        uint32_t title_y = logo_cy + logo_sz + 20;
        /* Purple → white two-tone: draw twice with slight offset for depth */
        fb_print_s(title_x + 1, title_y + 1, title, 0x5512A0u, 0x00000000u, 3);
        fb_print_s(title_x,     title_y,     title, 0xCC88FFu, 0x00000000u, 3);

        /* ── Subtitle: "booting..." ──────────────────────────────────── */
        static const char *sub = "booting...";
        uint32_t sub_w = (uint32_t)(strlen(sub)) * 8 * 2;
        uint32_t sub_x = (W > sub_w) ? (W - sub_w) / 2 : 0;
        uint32_t sub_y = title_y + 16 * 3 + 12;
        fb_print_s(sub_x, sub_y, sub, 0x7788BBu, 0x00000000u, 2);

        /* ── Progress bar ────────────────────────────────────────────── */
        uint32_t bar_w  = 320;
        uint32_t bar_h  = 10;
        uint32_t bar_x  = (W - bar_w) / 2;
        uint32_t bar_y  = sub_y + 16 * 2 + 22;
        uint32_t bar_bg = fb_color(18, 12, 36);
        uint32_t bar_lo = fb_color(80,  30, 180);
        uint32_t bar_hi = fb_color(170, 80, 255);

        /* Bar track — rounded, with subtle border */
        fb_rounded_rect(bar_x - 2, bar_y - 2, bar_w + 4, bar_h + 4, 6,
                        fb_color(40, 24, 70));
        fb_rounded_rect(bar_x, bar_y, bar_w, bar_h, 5, bar_bg);

        /* Spinner position: centered below bar with proper gap */
        uint32_t sp_cx = cx;
        uint32_t sp_cy = bar_y + bar_h + 38;

        /* ── Phase 1: appear  0% → 50%  (12 frames × 28ms) ─────────── */
        for (int f = 0; f < 12; f++) {
            uint32_t filled = (uint32_t)((f + 1) * (bar_w / 2) / 12);
            uint32_t col    = fb_blend(bar_lo, bar_hi, (uint8_t)(f * 16));
            fb_rounded_rect(bar_x, bar_y, filled > 5 ? filled : 5, bar_h, 5, col);
            draw_spinner(sp_cx, sp_cy, f, 255, bar_hi, H, bg_top, bg_bot);
            fb_flip();
            sched_sleep(28);
        }

        /* ── Phase 2: loading  50% → 90%  (8 frames × 22ms) ────────── */
        uint32_t accent2 = fb_color(150, 60, 255);
        for (int f = 0; f < 8; f++) {
            uint32_t filled = bar_w / 2 +
                              (uint32_t)((f + 1) * (bar_w * 4 / 10) / 8);
            uint32_t col = fb_blend(accent2, bar_hi, (uint8_t)(f * 24));
            fb_rounded_rect(bar_x, bar_y, filled, bar_h, 5, col);
            draw_spinner(sp_cx, sp_cy, f + 12, 255, accent2, H, bg_top, bg_bot);
            fb_flip();
            sched_sleep(22);
        }

        /* ── Phase 3: finish  90% → 100%, spinner fades out ─────────── */
        for (int f = 0; f < 8; f++) {
            uint32_t filled = bar_w * 9 / 10 +
                              (uint32_t)((f + 1) * (bar_w / 10) / 8);
            if (filled > bar_w) filled = bar_w;
            uint32_t col = fb_blend(bar_hi, 0xFFFFFFu, (uint8_t)(f * 30));
            fb_rounded_rect(bar_x, bar_y, filled, bar_h, 5, col);
            /* Spinner alpha fades from 255 → 0 over these 8 frames */
            int sp_alpha = 255 - f * 34;
            if (sp_alpha < 0) sp_alpha = 0;
            /* Erase spinner area with exact gradient before redraw */
            for (uint32_t ey = sp_cy - 34; ey <= sp_cy + 34; ey++) {
                if (ey >= H) break;
                fb_fill_rect(sp_cx - 34, ey, 68, 1, grad_color(ey, H, bg_top, bg_bot));
            }
            if (sp_alpha > 0)
                draw_spinner(sp_cx, sp_cy, f + 20, sp_alpha, bar_hi, H, bg_top, bg_bot);
            fb_flip();
            sched_sleep(18);
        }

        /* ── Full bar flash ──────────────────────────────────────────── */
        fb_rounded_rect(bar_x, bar_y, bar_w, bar_h, 5, 0xFFFFFFu);
        fb_flip();
        sched_sleep(60);

        /* ── Fade to black (12 frames × 14ms) ───────────────────────── */
        for (int f = 0; f < 12; f++) {
            uint8_t darkness = (uint8_t)(f * 21);
            /* Overdraw gradient with progressively darker overlay */
            uint32_t overlay = fb_blend(0x000000u, bg_top, darkness);
            fb_fill_rect(0, 0, W, H, overlay);
            fb_flip();
            sched_sleep(14);
        }

        fb_fill_rect(0, 0, W, H, 0x000000u);
        fb_flip();

    } else {
        vga_set_color(VGA_LIGHT_MAGENTA, VGA_BLACK);
        vga_print("DracolaxOS v1.0 — text mode\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }

    kinfo("ATLAS: boot sequence complete\n");
}

/* ---- Progressive boot API ---------------------------------------------- */
/* State shared between start / set_progress / finish */
static uint32_t g_bar_x   = 0;
static uint32_t g_bar_y   = 0;
static uint32_t g_bar_w   = 0;
static uint32_t g_bar_h   = 0;
static uint32_t g_sp_cx   = 0;
static uint32_t g_sp_cy   = 0;
static uint32_t g_H       = 0;
static uint32_t g_bg_top  = 0;
static uint32_t g_bg_bot  = 0;
static int      g_sp_frame = 0;
static int      g_last_pct = 0;

/* Draw static portions of the boot screen (background, logo, title, bar track).
 * Called once before real init steps begin. */
void atlas_boot_start(void) {
    if (!fb.available) {
        vga_set_color(VGA_LIGHT_MAGENTA, VGA_BLACK);
        vga_print("DracolaxOS v1.0\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    uint32_t W = fb.width;
    uint32_t H = fb.height;
    g_H      = H;
    g_bg_top = fb_color(4,  4, 12);
    g_bg_bot = fb_color(10, 6, 28);

    /* Gradient background */
    for (uint32_t y = 0; y < H; y++) {
        uint8_t t = (uint8_t)(y * 255 / H);
        fb_fill_rect(0, y, W, 1, fb_blend(g_bg_bot, g_bg_top, t));
    }

    /* Build info top-left (debug overlay — boot mode 0) */
    fb_print(6, 5, "DracolaxOS v1.0 [x86_64] booting...", 0x333355u, g_bg_top);

    /* Dragon logo */
    uint32_t cx = W / 2;
    uint32_t cy = H / 2;
    uint32_t sz = 48;
    uint32_t logo_cy = cy - 110;
    draw_logo_bitmap(cx, logo_cy, sz, H, g_bg_top, g_bg_bot);

    /* Title */
    static const char *title = "DracolaxOS";
    uint32_t title_w = (uint32_t)strlen(title) * 8 * 3;
    uint32_t title_x = (W > title_w) ? (W - title_w) / 2 : 0;
    uint32_t title_y = logo_cy + sz + 20;
    fb_print_s(title_x + 1, title_y + 1, title, 0x5512A0u, 0u, 3);
    fb_print_s(title_x,     title_y,     title, 0xCC88FFu, 0u, 3);

    /* Subtitle */
    static const char *sub = "Starting up...";
    uint32_t sub_w = (uint32_t)strlen(sub) * 8 * 2;
    fb_print_s((W - sub_w) / 2, title_y + 16*3 + 12, sub, 0x7788BBu, 0u, 2);

    /* Bar */
    g_bar_w = 320;
    g_bar_h = 10;
    g_bar_x = (W - g_bar_w) / 2;
    g_bar_y = title_y + 16*3 + 12 + 16*2 + 22;
    uint32_t bar_track = fb_color(18, 12, 36);
    fb_rounded_rect(g_bar_x - 2, g_bar_y - 2, g_bar_w + 4, g_bar_h + 4, 6, fb_color(40, 24, 70));
    fb_rounded_rect(g_bar_x, g_bar_y, g_bar_w, g_bar_h, 5, bar_track);

    /* ── Bottom debug strip ─────────────────────────────────────
     * Reserve the bottom 28px as a log canvas. This area shows
     * error/warning messages during boot without disturbing the
     * animation area above. Errors go here AND to serial/VGA. */
    uint32_t dbg_y = H - 28;
    fb_fill_rect(0, dbg_y, W, 28, fb_color(8, 4, 18));
    fb_print(8, dbg_y + 6, "DracolaxOS v1.0 | booting | errors shown here",
             0x333355u, fb_color(8, 4, 18));

    g_sp_cx    = cx;
    g_sp_cy    = g_bar_y + g_bar_h + 38;
    g_sp_frame = 0;
    g_last_pct = 0;

    fb_flip();
}

/* Advance the progress bar to pct (0-100). Animates spinner. */
void atlas_boot_set_progress(int pct) {
    if (!fb.available || g_bar_w == 0) return;
    if (pct < g_last_pct) pct = g_last_pct;
    if (pct > 100) pct = 100;

    uint32_t filled = (uint32_t)((uint64_t)pct * g_bar_w / 100);
    if (filled < 4) filled = 4;

    uint32_t bar_col = fb_blend(fb_color(80, 30, 180), fb_color(170, 80, 255),
                                (uint8_t)(pct * 2 < 255 ? pct * 2 : 255));
    /* Redraw bar track then fill */
    fb_rounded_rect(g_bar_x, g_bar_y, g_bar_w, g_bar_h, 5, fb_color(18, 12, 36));
    fb_rounded_rect(g_bar_x, g_bar_y, filled, g_bar_h, 5, bar_col);

    /* Erase spinner area with exact gradient rows */
    for (uint32_t ey = g_sp_cy - 34; ey <= g_sp_cy + 34 && ey < g_H; ey++)
        fb_fill_rect(g_sp_cx - 34, ey, 68, 1, grad_color(ey, g_H, g_bg_top, g_bg_bot));

    /* Draw spinner (fades out near 100%) */
    int sp_alpha = (pct >= 90) ? (100 - pct) * 26 : 255;
    if (sp_alpha > 0)
        draw_spinner(g_sp_cx, g_sp_cy, g_sp_frame++, sp_alpha,
                     fb_color(150, 60, 255), g_H, g_bg_top, g_bg_bot);

    fb_flip();
    g_last_pct = pct;
}

/* Fill bar to 100%, flash white, fade to black. */
void atlas_boot_finish(void) {
    if (!fb.available || g_bar_w == 0) return;
    atlas_boot_set_progress(100);
    /* Flash */
    fb_rounded_rect(g_bar_x, g_bar_y, g_bar_w, g_bar_h, 5, 0xFFFFFFu);
    fb_flip();
    sched_sleep(60);
    /* Fade */
    for (int f = 0; f < 12; f++) {
        fb_fill_rect(0, 0, fb.width, g_H,
                     fb_blend(0x000000u, g_bg_top, (uint8_t)(f * 21)));
        fb_flip();
        sched_sleep(14);
    }
    fb_fill_rect(0, 0, fb.width, g_H, 0x000000u);
    fb_flip();
    kinfo("ATLAS: progressive boot complete\n");
}

/* ---- atlas_boot_log ---------------------------------------------------- */
/* Static state for the debug strip row tracker */
static uint32_t g_log_col = 0;

void atlas_boot_log(const char *msg, int is_error) {
    if (!fb.available || g_H == 0) return;

    uint32_t dbg_y   = g_H - 28;
    uint32_t dbg_bg  = fb_color(8, 4, 18);
    uint32_t msg_col = is_error ? 0xFF6040u : 0x8888CCu;

    /* On first error clear the placeholder text */
    static int dbg_cleared = 0;
    if (!dbg_cleared) {
        fb_fill_rect(0, dbg_y, fb.width, 28, dbg_bg);
        g_log_col = 0;
        dbg_cleared = 1;
    }

    /* Wrap to new line if needed */
    size_t msg_len   = strlen(msg);
    uint32_t msg_px  = (uint32_t)(msg_len * 8);
    if (g_log_col + msg_px > fb.width - 8) {
        /* Second line overwrites first — strip is only 28px (1 text line + pad) */
        fb_fill_rect(0, dbg_y + 6, fb.width, 16, dbg_bg);
        g_log_col = 0;
    }

    fb_print(g_log_col + 8, dbg_y + 6, msg, msg_col, dbg_bg);
    g_log_col += msg_px + 8;

    /* Stamp directly to VRAM so it appears over the animation immediately */
    {
        /* Copy the strip from shadow to VRAM */
        if (fb_shadow_ptr()) {
            uint8_t *shadow = (uint8_t *)fb_shadow_ptr() + dbg_y * fb.pitch;
            uint8_t *vram   = (uint8_t *)(uintptr_t)fb.addr + dbg_y * fb.pitch;
            for (uint32_t r = 0; r < 28 && (dbg_y + r) < g_H; r++) {
                uint8_t *s = shadow + r * fb.pitch;
                uint8_t *v = vram   + r * fb.pitch;
                for (uint32_t b = 0; b < fb.pitch; b++) v[b] = s[b];
            }
        }
    }
}
