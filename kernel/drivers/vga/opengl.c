/* kernel/opengl.c — DracolaxOS Software OpenGL-like Renderer
 *
 * Backed by the kernel framebuffer (fb.c).
 * All coordinates are in screen-pixel space (origin = top-left).
 */
#include "../../types.h"
#include "opengl.h"
#include "fb.h"
#include "../../klibc.h"

/* ---- Internal state ------------------------------------------------------ */

static struct {
    float clear_r, clear_g, clear_b, clear_a;
    float cur_r,   cur_g,   cur_b,   cur_a;
    float point_sz;
    float line_w;
    uint32_t mode;
    int  in_begin;
    int  vcount;
    dgl_vec2_t verts[DGL_MAX_VERTS];
    /* viewport */
    int vp_x, vp_y, vp_w, vp_h;
    /* scissor */
    int sc_x, sc_y, sc_w, sc_h;
    int sc_enabled;
} g;

/* ---- Helpers ------------------------------------------------------------- */

static uint32_t rgba_f(float r, float g, float b, float a) {
    uint8_t R = (r > 1.f) ? 255 : (r < 0.f) ? 0 : (uint8_t)(r * 255.f);
    uint8_t G = (g > 1.f) ? 255 : (g < 0.f) ? 0 : (uint8_t)(g * 255.f);
    uint8_t B = (b > 1.f) ? 255 : (b < 0.f) ? 0 : (uint8_t)(b * 255.f);
    (void)a;
    return ((uint32_t)R << 16) | ((uint32_t)G << 8) | B;
}

static int in_scissor(int x, int y) {
    if (!g.sc_enabled) return 1;
    return (x >= g.sc_x && x < g.sc_x + g.sc_w &&
            y >= g.sc_y && y < g.sc_y + g.sc_h);
}

static void plot(int x, int y, uint32_t col) {
    if (!fb.available) return;
    if (x < g.vp_x || x >= g.vp_x + g.vp_w) return;
    if (y < g.vp_y || y >= g.vp_y + g.vp_h) return;
    if (!in_scissor(x, y)) return;
    fb_put_pixel((uint32_t)x, (uint32_t)y, col);
}

/* Bresenham line */
static void draw_line(int x0, int y0, int x1, int y1, uint32_t col, int lw) {
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    int half = lw / 2;
    while (1) {
        for (int oy = -half; oy <= half; oy++)
            for (int ox = -half; ox <= half; ox++)
                plot(x0 + ox, y0 + oy, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Scanline triangle fill */
static void fill_triangle(int x0, int y0, int x1, int y1,
                           int x2, int y2, uint32_t col) {
    /* Sort by y */
    if (y1 < y0) { int t; t=x0;x0=x1;x1=t; t=y0;y0=y1;y1=t; }
    if (y2 < y0) { int t; t=x0;x0=x2;x2=t; t=y0;y0=y2;y2=t; }
    if (y2 < y1) { int t; t=x1;x1=x2;x2=t; t=y1;y1=y2;y2=t; }

    int total_h = y2 - y0;
    if (total_h == 0) return;

    for (int y = y0; y <= y2; y++) {
        int second_half = (y > y1 || y1 == y0);
        int seg_h = second_half ? (y2 - y1) : (y1 - y0);
        if (seg_h == 0) seg_h = 1;
        float alpha = (float)(y - y0) / total_h;
        float beta  = second_half
            ? (float)(y - y1) / seg_h
            : (float)(y - y0) / seg_h;
        int ax = x0 + (int)((x2 - x0) * alpha);
        int bx = second_half
            ? x1 + (int)((x2 - x1) * beta)
            : x0 + (int)((x1 - x0) * beta);
        if (ax > bx) { int t = ax; ax = bx; bx = t; }
        for (int x = ax; x <= bx; x++) plot(x, y, col);
    }
}

/* ---- Public API ---------------------------------------------------------- */

void dgl_init(void) {
    g.clear_r = 0.f; g.clear_g = 0.f; g.clear_b = 0.f; g.clear_a = 1.f;
    g.cur_r   = 1.f; g.cur_g   = 1.f; g.cur_b   = 1.f; g.cur_a   = 1.f;
    g.point_sz = 1.f;
    g.line_w   = 1.f;
    g.in_begin = 0;
    g.vcount   = 0;
    if (fb.available) {
        g.vp_x = 0; g.vp_y = 0;
        g.vp_w = (int)fb.width; g.vp_h = (int)fb.height;
        g.sc_x = 0; g.sc_y = 0;
        g.sc_w = (int)fb.width; g.sc_h = (int)fb.height;
    }
    g.sc_enabled = 0;
}

void dgl_clear_color(float r, float g2, float b, float a) {
    g.clear_r = r; g.clear_g = g2; g.clear_b = b; g.clear_a = a;
}

void dgl_clear(void) {
    if (!fb.available) return;
    fb_fill_rect((uint32_t)g.vp_x, (uint32_t)g.vp_y,
                 (uint32_t)g.vp_w, (uint32_t)g.vp_h,
                 rgba_f(g.clear_r, g.clear_g, g.clear_b, g.clear_a));
}

void dgl_color4f(float r, float g2, float b, float a) {
    g.cur_r = r; g.cur_g = g2; g.cur_b = b; g.cur_a = a;
}
void dgl_color3f(float r, float g2, float b) { dgl_color4f(r, g2, b, 1.f); }

void dgl_point_size(float size) { g.point_sz = size > 1.f ? size : 1.f; }
void dgl_line_width(float width){ g.line_w   = width > 1.f ? width : 1.f; }

void dgl_begin(uint32_t mode) {
    g.mode = mode;
    g.vcount = 0;
    g.in_begin = 1;
}

void dgl_vertex2f(float x, float y) {
    if (!g.in_begin || g.vcount >= DGL_MAX_VERTS) return;
    g.verts[g.vcount].x = x;
    g.verts[g.vcount].y = y;
    g.vcount++;
}

void dgl_vertex2i(int x, int y) { dgl_vertex2f((float)x, (float)y); }

void dgl_end(void) {
    if (!g.in_begin) return;
    g.in_begin = 0;

    uint32_t col = rgba_f(g.cur_r, g.cur_g, g.cur_b, g.cur_a);
    int lw = (int)g.line_w;
    int ps = (int)g.point_sz;

    switch (g.mode) {

    case DGL_POINTS:
        for (int i = 0; i < g.vcount; i++) {
            int x = (int)g.verts[i].x, y = (int)g.verts[i].y;
            for (int dy = -ps/2; dy <= ps/2; dy++)
                for (int dx = -ps/2; dx <= ps/2; dx++)
                    plot(x+dx, y+dy, col);
        }
        break;

    case DGL_LINES:
        for (int i = 0; i + 1 < g.vcount; i += 2)
            draw_line((int)g.verts[i].x,   (int)g.verts[i].y,
                      (int)g.verts[i+1].x, (int)g.verts[i+1].y, col, lw);
        break;

    case DGL_LINE_STRIP:
        for (int i = 0; i + 1 < g.vcount; i++)
            draw_line((int)g.verts[i].x,   (int)g.verts[i].y,
                      (int)g.verts[i+1].x, (int)g.verts[i+1].y, col, lw);
        break;

    case DGL_LINE_LOOP:
        for (int i = 0; i < g.vcount; i++) {
            int j = (i + 1) % g.vcount;
            draw_line((int)g.verts[i].x, (int)g.verts[i].y,
                      (int)g.verts[j].x, (int)g.verts[j].y, col, lw);
        }
        break;

    case DGL_TRIANGLES:
        for (int i = 0; i + 2 < g.vcount; i += 3)
            fill_triangle((int)g.verts[i].x,   (int)g.verts[i].y,
                          (int)g.verts[i+1].x, (int)g.verts[i+1].y,
                          (int)g.verts[i+2].x, (int)g.verts[i+2].y, col);
        break;

    case DGL_TRIANGLE_STRIP:
        for (int i = 0; i + 2 < g.vcount; i++) {
            if (i & 1)
                fill_triangle((int)g.verts[i].x,   (int)g.verts[i].y,
                              (int)g.verts[i+2].x, (int)g.verts[i+2].y,
                              (int)g.verts[i+1].x, (int)g.verts[i+1].y, col);
            else
                fill_triangle((int)g.verts[i].x,   (int)g.verts[i].y,
                              (int)g.verts[i+1].x, (int)g.verts[i+1].y,
                              (int)g.verts[i+2].x, (int)g.verts[i+2].y, col);
        }
        break;

    case DGL_TRIANGLE_FAN:
        for (int i = 1; i + 1 < g.vcount; i++)
            fill_triangle((int)g.verts[0].x,   (int)g.verts[0].y,
                          (int)g.verts[i].x,   (int)g.verts[i].y,
                          (int)g.verts[i+1].x, (int)g.verts[i+1].y, col);
        break;

    case DGL_QUADS:
        for (int i = 0; i + 3 < g.vcount; i += 4) {
            fill_triangle((int)g.verts[i].x,   (int)g.verts[i].y,
                          (int)g.verts[i+1].x, (int)g.verts[i+1].y,
                          (int)g.verts[i+2].x, (int)g.verts[i+2].y, col);
            fill_triangle((int)g.verts[i].x,   (int)g.verts[i].y,
                          (int)g.verts[i+2].x, (int)g.verts[i+2].y,
                          (int)g.verts[i+3].x, (int)g.verts[i+3].y, col);
        }
        break;

    default: break;
    }
}

void dgl_flush(void) { /* FB is immediate-mode — nothing to flush */ }

void dgl_viewport(int x, int y, int w, int h) {
    g.vp_x = x; g.vp_y = y; g.vp_w = w; g.vp_h = h;
}

void dgl_scissor(int x, int y, int w, int h) {
    g.sc_x = x; g.sc_y = y; g.sc_w = w; g.sc_h = h;
}

void dgl_enable_scissor(int enable) { g.sc_enabled = enable; }
uint32_t dgl_get_error(void) { return 0; }
