/*
 * The rasterizer as a C model (see raster.h for what it does and for the number formats).
 * Which pixels a triangle covers is decided in integer arithmetic only, so the hardware can
 * match it bit for bit. Only the texture setup of rs_tex_setup() uses floating point.
 * Entry points: rs_triangle() (one colour), rs_textured_fx() (texture, fixed point),
 * rs_tex_setup() (texture planes from floating-point corners) and rs_recip() (reciprocal).
 *
 * The fill rule is the game's (original: STARTL.EXE; docs/original_filler.md 5). The fillers
 * from 0xd8844 walk the polygon column by column (0xda330) and fill each column from the
 * truncated upper to the truncated lower edge, both ends drawn (0xda478). So pixel (x, r) is
 * drawn when the triangle touches the segment from (x, r) to (x, r + 1), its lower end
 * excluded. Neighbours overlap along a shared edge and leave no gap, also where a corner of
 * one lies on the edge of the other.
 *
 * - Corners are cut to whole pixels (as the game's projection does) after sorting by y
 *   (a.y <= b.y <= c.y). The triangle covers the rows a.y .. c.y, both drawn.
 * - Edges are always evaluated from their upper end (x0, y0) to their lower end (x1, y1):
 *   slope = ((x1 - x0) << 16) / (y1 - y0), truncated towards zero. At the top of row r,
 *   x = (x0 << 16) + slope (r - y0), in 16.16 pixels (64 bits, because a nearly flat edge has
 *   a large slope). At its lower end x is the corner's x itself. The same edge gives the same
 *   x in every triangle that uses it, and stepping one row adds exactly slope.
 * - In a row above c.y the left edge runs from xt at its top to xb at its bottom. The span
 *   starts at min(ceil(xt), floor(xb) + 1). The right edge ends at max(floor(xt), ceil(xb) - 1),
 *   and that pixel is drawn. Row c.y is the bottom corner (or edge) alone.
 * - Routine 224 (raster.h, blend): rows a.y .. c.y - 1, from ceil(xb) of the left edge to
 *   ceil(xb) - 1 of the right one.
 */
#include "raster.h"

#define FB_STRIDE 320

struct edge { int64_t x, slope; };

/* The edge from (x0, y0) to (x1, y1), with y0 < y1, at the top of row r. The corners are in
   1/16 pixel and lie on whole pixels. x is in 16.16 pixels, the slope is per row. */
static struct edge edge_at(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int r)
{
    struct edge e;
    int32_t dx = x1 - x0;               /* |dx| < 2^16, so |dx| << 16 fits 32 bits unsigned */
    uint32_t q = ((uint32_t)(dx < 0 ? -dx : dx) << 16) / (uint32_t)(y1 - y0);
    e.slope = dx < 0 ? -(int64_t)q : (int64_t)q;
    e.x = ((int64_t)x0 << 12) + ((e.slope * (16 * r - y0)) >> 4);
    return e;
}

/* x rounded up to a whole pixel; x is in 16.16 pixels. */
static int64_t ceil16(int64_t x) { return (x + 0xFFFF) >> 16; }

static int clamp_col(int64_t c, int lo, int hi)
{
    return c < lo ? lo : c > hi ? hi : (int)c;
}

/* Draws the pixels x0 .. x1 - 1 of row r (the span of one row). ctx is the drawing's own data.
   Returns the number of pixels written. */
typedef uint32_t (*span_fn)(volatile uint8_t *fb, int r, int x0, int x1, const void *ctx);

static uint32_t span_flat(volatile uint8_t *fb, int r, int x0, int x1, const void *ctx)
{
    volatile uint8_t *row = fb + r * FB_STRIDE;
    uint8_t colour = *(const uint8_t *)ctx;
    uint32_t n = (uint32_t)(x1 - x0), c4 = colour * 0x01010101u;
    int x = x0;
    while(x < x1 && (x & 3)) row[x++] = colour;                         /* up to a word boundary */
    for(; x + 4 <= x1; x += 4) *(volatile uint32_t *)(row + x) = c4;    /* whole words */
    while(x < x1) row[x++] = colour;                                    /* the rest */
    return n;
}

/*
 * Walks the rows of a triangle and calls span() for the pixels of each row. Returns the
 * number of pixels written. blend selects the rule of routine 224 (see the top of this file).
 * For the other routines the left and the right edge of a row are found as follows. The long
 * edge a-c and the short edge (a-b, then b-c) are each taken from the top to the bottom of
 * the row. The one that starts further left is the left edge. The span then runs from the
 * start of the left edge to the end of the right edge.
 */
static uint32_t walk(volatile uint8_t *fb, const struct rs_clip *clip, const struct rs_vertex v[3],
                     span_fn span, const void *ctx, int blend)
{
    const struct rs_vertex *p = &v[0], *q = &v[1], *u = &v[2], *t;
    if(q->y < p->y) { t = p; p = q; q = t; }
    if(u->y < q->y) { t = q; q = u; u = t; }
    if(q->y < p->y) { t = p; p = q; q = t; }
    struct rs_vertex a = { p->x & ~15, p->y & ~15 }, b = { q->x & ~15, q->y & ~15 },
                     c = { u->x & ~15, u->y & ~15 };

    /* The rows to draw: those of the triangle that lie inside the clip rectangle. */
    int ya = a.y >> 4, yb = b.y >> 4, yc = c.y >> 4;
    int r0 = ya < clip->y0 ? clip->y0 : ya, r1 = yc + !blend > clip->y1 ? clip->y1 : yc + !blend;
    if(r0 >= r1) return 0;

    /* Row yc: only the corners that lie on it. last0 and last1 are the leftmost and the
       rightmost of them, as pixel columns. */
    int last0 = c.x >> 4, last1 = last0;
    if(b.y == c.y) { if(b.x >> 4 < last0) last0 = b.x >> 4; else last1 = b.x >> 4; }
    if(a.y == c.y) { if(a.x >> 4 < last0) last0 = a.x >> 4; if(a.x >> 4 > last1) last1 = a.x >> 4; }

    /* l is the long edge a-c. s is the short edge: a-b in the rows before yb, b-c from yb on.
       An edge is only evaluated when it has a row above yc, which means its y1 > y0. */
    struct edge l = { 0, 0 }, s = { 0, 0 };
    if(r0 < yc) {
        l = edge_at(a.x, a.y, c.x, c.y, r0);
        if(r0 < yb) s = edge_at(a.x, a.y, b.x, b.y, r0);
        else s = edge_at(b.x, b.y, c.x, c.y, r0);
    }
    int64_t xb = (int64_t)b.x << 12, xc = (int64_t)c.x << 12;
    uint32_t n = 0;
    for(int r = r0; r < r1; r++) {
        int64_t left, right;
        if(r == yc) {
            left = last0;
            right = last1;
        } else {
            if(r == yb && r > r0) s = edge_at(b.x, b.y, c.x, c.y, r);
            /* lt, lb: the long edge at the top and the bottom of the row. st, sb: the same for
               the short edge. At a corner the bottom is the corner's x itself. */
            int64_t lt = l.x, lb = r + 1 == yc ? xc : l.x + l.slope;
            int64_t st = s.x, sb = r + 1 == yb ? xb : r + 1 == yc ? xc : s.x + s.slope;
            int long_left = lt < st || (lt == st && lb < sb);
            int64_t L0 = long_left ? lt : st, L1 = long_left ? lb : sb;
            int64_t R0 = long_left ? st : lt, R1 = long_left ? sb : lb;
            int64_t la = ceil16(L0), lz = (L1 >> 16) + 1, ra = R0 >> 16, rz = ceil16(R1) - 1;
            left = blend ? ceil16(L1) : la < lz ? la : lz;
            right = blend ? rz : ra > rz ? ra : rz;
        }
        int x0 = clamp_col(left, clip->x0, clip->x1), x1 = clamp_col(right + 1, clip->x0, clip->x1);
        if(x0 < x1) n += span(fb, r, x0, x1, ctx);
        l.x += l.slope;
        s.x += s.slope;
    }
    return n;
}

uint32_t rs_triangle(volatile uint8_t *fb, const struct rs_clip *clip,
                     const struct rs_vertex v[3], uint8_t colour)
{
    return walk(fb, clip, v, span_flat, &colour, 0);
}

/* The see-through table of routine 224 (raster.h). */
const uint8_t *rs_smg;

/* Routine 224: the pixel taken through row t of the table. Without a table the result is 0. */
static uint8_t smg(uint8_t t, uint8_t pixel)
{
    return rs_smg ? rs_smg[(t & 63) << 8 | pixel] : 0;
}

/* Writes texel c to *p. For texel 0, routines 64 and 224 leave the pixel as it is.
   Returns 1 if a pixel was written. */
static int put_texel(volatile uint8_t *p, uint8_t c, int routine)
{
    if(!c && (routine == 64 || routine == 224)) return 0;
    *p = routine == 224 ? smg(c, *p) : c;
    return 1;
}

/*
 * The floating-point form of a texture plane: q, u q and v q are planes over the screen,
 * a(px, py) = a0 + ax (px - x0) + ay (py - y0) around vertex 0, sampled at pixel centres.
 */
struct plane { float a0, ax, ay; };

static struct plane plane_of(const float x[3], const float y[3], float a0, float a1, float a2, float det)
{
    struct plane p;
    p.a0 = a0;
    p.ax = ((a1 - a0) * (y[2] - y[0]) - (a2 - a0) * (y[1] - y[0])) / det;
    p.ay = ((a2 - a0) * (x[1] - x[0]) - (a1 - a0) * (x[2] - x[0])) / det;
    return p;
}

#ifdef RS_TRACE
void rs_trace(const volatile uint8_t *p);
#endif

/*
 * The hardware's reciprocal of m, which is in [2^15, 2^16): about 2^32 / m.
 *   r0 = floor((2^32 - 1) / (2^15 + 32 m[14:5] + 16))     (a table of 1024 entries in hardware)
 *   r = r0 (2^33 - m r0) >> 32                              (one Newton step)
 */
uint32_t rs_recip(uint32_t m)
{
    uint32_t r0 = 0xFFFFFFFFu / (0x8000u + ((m >> 5) & 1023u) * 32u + 16u);
    uint64_t p = (uint64_t)m * r0;
    return (uint32_t)(((uint64_t)r0 * ((1ull << 33) - p)) >> 32);
}

struct texfx_ctx { const struct rs_tex_setup *s; const struct rs_texture *tex; };

/*
 * The span of a textured triangle in fixed point, what the hardware computes. With
 * acc = A << 8 (8 fraction bits) for Q, U and V, each pixel does this:
 *   Q = max(acc_Q >> 8, 1); n = bit length of Q, e = n - 16, m = Q shifted to 16 bits
 *   r = rs_recip(m), about 2^32 / m
 *   U = acc_U >> 8 limited to 27 bits signed, u = (U r) >> (16 + e), in texels with 8
 *   fraction bits; v is the same with V
 * Then the texel is the one at (u >> 8, v >> 8), clamped to the texture.
 */
static uint32_t span_texfx(volatile uint8_t *fb, int r, int x0, int x1, const void *ctx)
{
    const struct rs_tex_setup *s = ((const struct texfx_ctx *)ctx)->s;
    const struct rs_texture *t = ((const struct texfx_ctx *)ctx)->tex;
    volatile uint8_t *row = fb + r * FB_STRIDE;
    int64_t acc[3];
    for(int k = 0; k < 3; k++)
        acc[k] = ((int64_t)s->a[k] << 8) + (int64_t)s->dx[k] * (x0 - s->xr) + (int64_t)s->dy[k] * (r - s->yr);
    uint32_t n = 0;
    for(int x = x0; x < x1; x++) {
        int64_t qa = acc[0] >> 8;
        uint32_t q = qa < 1 ? 1u : qa > 0x7FFFFFFF ? 0x7FFFFFFFu : (uint32_t)qa;
        int e = -16;                                /* bit length - 16 */
        while(e < 16 && q >> (e + 16)) e++;
        uint32_t m = e >= 0 ? q >> e : q << -e;
        int64_t rc = rs_recip(m);
        int64_t ua = acc[1] >> 8, va = acc[2] >> 8;
        ua = ua < -(1 << 26) ? -(1 << 26) : ua > (1 << 26) - 1 ? (1 << 26) - 1 : ua;
        va = va < -(1 << 26) ? -(1 << 26) : va > (1 << 26) - 1 ? (1 << 26) - 1 : va;
        int64_t u = ((ua * rc) >> (16 + e)) >> 8;
        int64_t v = ((va * rc) >> (16 + e)) >> 8;
        int tu = t->routine == 160 ? (int)(u & 255) : u < 0 ? 0 : u >= t->width ? t->width - 1 : (int)u;
        int tv = t->routine == 160 ? (int)(v & 255) : v < 0 ? 0 : v >= t->height ? t->height - 1 : (int)v;
#ifdef RS_TRACE                            /* PC only: the texel addresses for a cache study */
        rs_trace(&t->data[tv * 256 + tu]);
#endif
        n += (uint32_t)put_texel(&row[x], t->data[tv * 256 + tu], t->routine);
        for(int k = 0; k < 3; k++) acc[k] += s->dx[k];
    }
    return n;
}

uint32_t rs_textured_fx(volatile uint8_t *fb, const struct rs_clip *clip, const struct rs_vertex pos[3],
                        const struct rs_tex_setup *s, const struct rs_texture *tex)
{
    struct texfx_ctx c = { s, tex };
    return walk(fb, clip, pos, span_texfx, &c, tex->routine == 224);
}

/* Rounds f to the nearest integer (halves away from zero) and saturates at +-(2^31 - 1). */
static int32_t sat_round(float f)
{
    if(f >= 2147483520.0f) return 0x7FFFFFFF;
    if(f <= -2147483520.0f) return -0x7FFFFFFF;
    return (int32_t)(f + (f < 0.0f ? -0.5f : 0.5f));
}

/*
 * The texture setup from floating-point corners (struct rs_tvertex): the reference pixel and,
 * for Q, U and V, the value at its centre and the change per pixel in x and y.
 * Returns 0 for a triangle without area or without a positive q.
 */
int rs_tex_setup(struct rs_tex_setup *s, const struct rs_tvertex v[3])
{
    float x[3], y[3], qmax = 0.0f;
    for(int i = 0; i < 3; i++) {
        x[i] = (float)v[i].x / 16.0f;
        y[i] = (float)v[i].y / 16.0f;
        if(v[i].q > qmax) qmax = v[i].q;
    }
    float det = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
    if(det == 0.0f || qmax <= 0.0f) return 0;
    float k = 11863283.0f / qmax;               /* 2^23.5: Q stays below 2^24 */
    s->xr = v[0].x >> 4;
    s->yr = v[0].y >> 4;
    s->xr = s->xr < 0 ? 0 : s->xr > FB_STRIDE - 1 ? FB_STRIDE - 1 : s->xr;
    s->yr = s->yr < 0 ? 0 : s->yr > 199 ? 199 : s->yr;
    float fx = (float)s->xr + 0.5f - x[0], fy = (float)s->yr + 0.5f - y[0];
    for(int c = 0; c < 3; c++) {
        float a[3];
        for(int i = 0; i < 3; i++)
            a[i] = c == 0 ? v[i].q * k : (c == 1 ? v[i].uq : v[i].vq) * k / 256.0f;
        struct plane p = plane_of(x, y, a[0], a[1], a[2], det);
        s->a[c] = sat_round(p.a0 + p.ax * fx + p.ay * fy);
        s->dx[c] = sat_round(p.ax * 256.0f);
        s->dy[c] = sat_round(p.ay * 256.0f);
    }
    return 1;
}
