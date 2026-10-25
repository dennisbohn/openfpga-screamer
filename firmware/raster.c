/*
 * Rasterizer C model (raster.h). Integer arithmetic only, so the hardware can match it bit
 * for bit:
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

static uint32_t span(volatile uint8_t *row, int x0, int x1, uint8_t colour)
{
    uint32_t n = (uint32_t)(x1 - x0), c4 = colour * 0x01010101u;
    int x = x0;
    while(x < x1 && (x & 3)) row[x++] = colour;
    for(; x + 4 <= x1; x += 4) *(volatile uint32_t *)(row + x) = c4;
    while(x < x1) row[x++] = colour;
    return n;
}

uint32_t rs_triangle(volatile uint8_t *fb, const struct rs_clip *clip,
                     const struct rs_vertex v[3], uint8_t colour)
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
        if(x0 < x1) n += span(fb + r * FB_STRIDE, x0, x1, colour);
        l.x += l.slope;
        s.x += s.slope;
    }
    return n;
}
