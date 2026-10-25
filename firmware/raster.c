/*
 * Rasterizer C model (raster.h). Which pixels a triangle covers is integer arithmetic only,
 * so the hardware can match it bit for bit (the texture lookup is still floating point):
 *
 * - Rows: pixel row r has its centre at yc = 16r + 8; a triangle with vertices sorted by y
 *   (a.y <= b.y <= c.y) covers the rows with a.y <= yc < c.y.
 * - Edges are always evaluated from their upper end (x0, y0) to their lower end (x1, y1):
 *   slope = ((x1 - x0) << 16) / (y1 - y0), truncated towards zero, and at row centre yc
 *   x = (x0 << 12) + floor(slope * (yc - y0) / 16), in 16.16 pixels (64 bits: a nearly flat
 *   edge has a large slope). The same edge gives the same x in every triangle that uses it,
 *   and stepping one row adds exactly slope.
 * - In each row the span runs from the left to the right edge; pixel x is drawn when its
 *   centre lies in [left, right): from ceil(left - 0.5) up to ceil(right - 0.5) - 1.
 */
#include "raster.h"

#define FB_STRIDE 320

struct edge { int64_t x, slope; };

/* the edge from (x0, y0) to (x1, y1), y0 < y1, at the centre of row r */
static struct edge edge_at(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int r)
{
    struct edge e;
    e.slope = ((int64_t)(x1 - x0) << 16) / (y1 - y0);
    e.x = ((int64_t)x0 << 12) + ((e.slope * (16 * r + 8 - y0)) >> 4);
    return e;
}

/* first row whose centre is at or below y (1/16 pixel) */
static int row_from(int32_t y)
{
    return (y + 7) >> 4;                /* ceil((y - 8) / 16) */
}

/* first pixel whose centre is at or right of x (16.16), limited to [lo, hi] */
static int col_from(int64_t x, int lo, int hi)
{
    int64_t c = (x + 0x7FFF) >> 16;     /* ceil(x - 0.5) */
    return c < lo ? lo : c > hi ? hi : (int)c;
}

/* a row's pixels [x0, x1) of row r */
typedef uint32_t (*span_fn)(volatile uint8_t *fb, int r, int x0, int x1, const void *ctx);

static uint32_t span_flat(volatile uint8_t *fb, int r, int x0, int x1, const void *ctx)
{
    volatile uint8_t *row = fb + r * FB_STRIDE;
    uint8_t colour = *(const uint8_t *)ctx;
    uint32_t n = (uint32_t)(x1 - x0), c4 = colour * 0x01010101u;
    int x = x0;
    while(x < x1 && (x & 3)) row[x++] = colour;
    for(; x + 4 <= x1; x += 4) *(volatile uint32_t *)(row + x) = c4;
    while(x < x1) row[x++] = colour;
    return n;
}

/* the rows and spans of a triangle; span() draws each */
static uint32_t walk(volatile uint8_t *fb, const struct rs_clip *clip, const struct rs_vertex v[3],
                     span_fn span, const void *ctx)
{
    const struct rs_vertex *a = &v[0], *b = &v[1], *c = &v[2], *t;
    if(b->y < a->y) { t = a; a = b; b = t; }
    if(c->y < b->y) { t = b; b = c; c = t; }
    if(b->y < a->y) { t = a; a = b; b = t; }

    /* rows to draw: those of the triangle inside the clip rectangle */
    int r0 = row_from(a->y), rb = row_from(b->y), r1 = row_from(c->y);
    if(r0 < clip->y0) r0 = clip->y0;
    if(r1 > clip->y1) r1 = clip->y1;
    if(r0 >= r1) return 0;              /* also every triangle without a row centre */

    /* rows before rb use the short edge a-b, from rb on b-c; an edge only gets evaluated
       when it has a row, so its y1 > y0 */
    struct edge l = edge_at(a->x, a->y, c->x, c->y, r0), s;
    if(r0 < rb) s = edge_at(a->x, a->y, b->x, b->y, r0);
    else s = edge_at(b->x, b->y, c->x, c->y, r0);
    uint32_t n = 0;
    for(int r = r0; r < r1; r++) {
        if(r == rb && r > r0) s = edge_at(b->x, b->y, c->x, c->y, r);
        int64_t left = l.x < s.x ? l.x : s.x, right = l.x < s.x ? s.x : l.x;
        int x0 = col_from(left, clip->x0, clip->x1), x1 = col_from(right, clip->x0, clip->x1);
        if(x0 < x1) n += span(fb, r, x0, x1, ctx);
        l.x += l.slope;
        s.x += s.slope;
    }
    return n;
}

uint32_t rs_triangle(volatile uint8_t *fb, const struct rs_clip *clip,
                     const struct rs_vertex v[3], uint8_t colour)
{
    return walk(fb, clip, v, span_flat, &colour);
}

/*
 * Textured: q, u q and v q are planes over the screen, a(px, py) = a0 + ax (px - x0) +
 * ay (py - y0) around vertex 0, sampled at pixel centres
 */
struct plane { float a0, ax, ay; };

struct tex_ctx {
    float x0, y0;                       /* vertex 0 in pixels */
    struct plane q, uq, vq;
    const struct rs_texture *tex;
};

static struct plane plane_of(const float x[3], const float y[3], float a0, float a1, float a2, float det)
{
    struct plane p;
    p.a0 = a0;
    p.ax = ((a1 - a0) * (y[2] - y[0]) - (a2 - a0) * (y[1] - y[0])) / det;
    p.ay = ((a2 - a0) * (x[1] - x[0]) - (a1 - a0) * (x[2] - x[0])) / det;
    return p;
}

static uint32_t span_tex(volatile uint8_t *fb, int r, int x0, int x1, const void *ctx)
{
    const struct tex_ctx *s = ctx;
    const struct rs_texture *t = s->tex;
    volatile uint8_t *row = fb + r * FB_STRIDE;
    float fx = (float)x0 + 0.5f - s->x0, fy = (float)r + 0.5f - s->y0;
    float q = s->q.a0 + s->q.ax * fx + s->q.ay * fy;
    float uq = s->uq.a0 + s->uq.ax * fx + s->uq.ay * fy;
    float vq = s->vq.a0 + s->vq.ax * fx + s->vq.ay * fy;
    uint32_t n = 0;
    for(int x = x0; x < x1; x++) {
        float z = 1.0f / q;
        int u = (int)(uq * z), v = (int)(vq * z);
        u = u < 0 ? 0 : u >= t->width ? t->width - 1 : u;
        v = v < 0 ? 0 : v >= t->height ? t->height - 1 : v;
        uint8_t c = t->data[v * 256 + u];
        if(c || t->routine != 64) {
            row[x] = c;
            n++;
        }
        q += s->q.ax;
        uq += s->uq.ax;
        vq += s->vq.ax;
    }
    return n;
}

uint32_t rs_textured(volatile uint8_t *fb, const struct rs_clip *clip,
                     const struct rs_tvertex v[3], const struct rs_texture *tex)
{
    struct rs_vertex pos[3];
    float x[3], y[3];
    for(int i = 0; i < 3; i++) {
        pos[i].x = v[i].x;
        pos[i].y = v[i].y;
        x[i] = (float)v[i].x / 16.0f;
        y[i] = (float)v[i].y / 16.0f;
    }
    float det = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
    if(det == 0.0f) return 0;           /* no area, no pixel centre inside */
    struct tex_ctx s;
    s.x0 = x[0];
    s.y0 = y[0];
    s.q = plane_of(x, y, v[0].q, v[1].q, v[2].q, det);
    s.uq = plane_of(x, y, v[0].uq, v[1].uq, v[2].uq, det);
    s.vq = plane_of(x, y, v[0].vq, v[1].vq, v[2].vq, det);
    s.tex = tex;
    return walk(fb, clip, pos, span_tex, &s);
}
