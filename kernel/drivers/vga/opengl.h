/* kernel/opengl.h — DracolaxOS Software OpenGL-like API
 *
 * This is a software renderer backed by the kernel framebuffer.
 * Goal: provide a GL-like drawing API for simple 2D/3D primitives.
 *
 * Current status: STUB (Phase 1 — 2D software renderer)
 *   - GL context backed by fb.c framebuffer
 *   - Basic primitives: points, lines, filled triangles
 *   - Color with alpha blending
 *   - No texture mapping yet (Phase 2)
 *   - No depth buffer (Phase 3 for 3D)
 *
 * Usage:
 *   dgl_init();
 *   dgl_clear_color(0.1f, 0.1f, 0.2f, 1.0f);
 *   dgl_clear();
 *   dgl_color4f(1.0f, 0.5f, 0.0f, 1.0f);
 *   dgl_begin(DGL_TRIANGLES);
 *     dgl_vertex2f(100.0f, 100.0f);
 *     dgl_vertex2f(200.0f, 100.0f);
 *     dgl_vertex2f(150.0f, 200.0f);
 *   dgl_end();
 *   dgl_flush();
 */
#ifndef OPENGL_H
#define OPENGL_H
#include "../../types.h"

/* Primitive modes */
#define DGL_POINTS         0x0000
#define DGL_LINES          0x0001
#define DGL_LINE_STRIP     0x0002
#define DGL_LINE_LOOP      0x0003
#define DGL_TRIANGLES      0x0004
#define DGL_TRIANGLE_STRIP 0x0005
#define DGL_TRIANGLE_FAN   0x0006
#define DGL_QUADS          0x0007

/* Blend factors (for future use) */
#define DGL_SRC_ALPHA      0x0302
#define DGL_ONE_MINUS_SRC_ALPHA 0x0303

/* Max vertices in a single begin/end block */
#define DGL_MAX_VERTS 256

/* Float-based 2D vertex */
typedef struct { float x, y; } dgl_vec2_t;

/* Initialise DGL — must be called after fb_init() */
void dgl_init(void);

/* Set clear color (RGBA, [0.0, 1.0]) */
void dgl_clear_color(float r, float g, float b, float a);

/* Clear color buffer to clear color */
void dgl_clear(void);

/* Set current drawing color */
void dgl_color4f(float r, float g, float b, float a);
void dgl_color3f(float r, float g, float b);

/* Point / line size */
void dgl_point_size(float size);
void dgl_line_width(float width);

/* Vertex submission */
void dgl_begin(uint32_t mode);
void dgl_vertex2f(float x, float y);
void dgl_vertex2i(int x, int y);
void dgl_end(void);

/* Push current FB content to screen (no-op for now — FB is immediate) */
void dgl_flush(void);

/* Viewport */
void dgl_viewport(int x, int y, int w, int h);

/* Scissor rectangle */
void dgl_scissor(int x, int y, int w, int h);
void dgl_enable_scissor(int enable);

/* Error query (always 0 for now) */
uint32_t dgl_get_error(void);

#endif /* OPENGL_H */
