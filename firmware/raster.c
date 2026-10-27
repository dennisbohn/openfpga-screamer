/*
 * Rasterizer C model (raster.h). Which pixels a triangle covers is integer arithmetic only,
 * so the hardware can match it bit for bit (the texture lookup is still floating point).
 * The rule is the game's (STARTL.EXE: the fillers from 0xd8844 walk the polygon column by
 * column, 0xda330, and fill each column from the truncated upper to the truncated lower edge,
 * both ends drawn, 0xda478): pixel (x, r) is drawn when the triangle touches the segment from
 * (x, r) to (x, r + 1), its lower end excluded. So neighbours overlap along a shared edge and
 * leave no gap, also where a corner of one lies on the edge of the other.
 *
 * - Corners are cut to whole pixels (as the game's projection does) after sorting by y
 *   (a.y <= b.y <= c.y); the triangle covers the rows a.y .. c.y, both drawn.
 * - Edges are always evaluated from their upper end (x0, y0) to their lower end (x1, y1):
 *   slope = ((x1 - x0) << 16) / (y1 - y0), truncated towards zero, and at the top of row r
 *   x = (x0 << 16) + slope (r - y0), in 16.16 pixels (64 bits: a nearly flat edge has a large
 *   slope); at its lower end the corner's x itself. The same edge gives the same x in every
 *   triangle that uses it, and stepping one row adds exactly slope.
 * - In a row above c.y the left edge runs from xt at its top to xb at its bottom: the span
 *   starts at min(ceil(xt), floor(xb) + 1); the right one ends at max(floor(xt), ceil(xb) - 1),
 *   that pixel drawn. Row c.y is the bottom corner (or edge) alone.
 */
#include "raster.h"

#define FB_STRIDE 320

struct edge { int64_t x, slope; };

/* the edge from (x0, y0) to (x1, y1), y0 < y1 (1/16 pixel, whole pixels), at the top of row r */
static struct edge edge_at(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int r)
{
    struct edge e;
    int32_t dx = x1 - x0;               /* |dx| < 2^16, so |dx| << 16 fits 32 bits unsigned */
    uint32_t q = ((uint32_t)(dx < 0 ? -dx : dx) << 16) / (uint32_t)(y1 - y0);
    e.slope = dx < 0 ? -(int64_t)q : (int64_t)q;
    e.x = ((int64_t)x0 << 12) + ((e.slope * (16 * r - y0)) >> 4);
    return e;
}

static int64_t ceil16(int64_t x) { return (x + 0xFFFF) >> 16; }

static int clamp_col(int64_t c, int lo, int hi)
{
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
    const struct rs_vertex *p = &v[0], *q = &v[1], *u = &v[2], *t;
    if(q->y < p->y) { t = p; p = q; q = t; }
    if(u->y < q->y) { t = q; q = u; u = t; }
    if(q->y < p->y) { t = p; p = q; q = t; }
    struct rs_vertex a = { p->x & ~15, p->y & ~15 }, b = { q->x & ~15, q->y & ~15 },
                     c = { u->x & ~15, u->y & ~15 };

    /* rows to draw: those of the triangle inside the clip rectangle */
    int ya = a.y >> 4, yb = b.y >> 4, yc = c.y >> 4;
    int r0 = ya < clip->y0 ? clip->y0 : ya, r1 = yc + 1 > clip->y1 ? clip->y1 : yc + 1;
    if(r0 >= r1) return 0;

    /* row yc: the corners on it */
    int last0 = c.x >> 4, last1 = last0;
    if(b.y == c.y) { if(b.x >> 4 < last0) last0 = b.x >> 4; else last1 = b.x >> 4; }
    if(a.y == c.y) { if(a.x >> 4 < last0) last0 = a.x >> 4; if(a.x >> 4 > last1) last1 = a.x >> 4; }

    /* rows before yb use the short edge a-b, from yb on b-c; an edge only gets evaluated
       when it has a row above yc, so its y1 > y0 */
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
            int64_t lt = l.x, lb = r + 1 == yc ? xc : l.x + l.slope;
            int64_t st = s.x, sb = r + 1 == yb ? xb : r + 1 == yc ? xc : s.x + s.slope;
            int long_left = lt < st || (lt == st && lb < sb);
            int64_t L0 = long_left ? lt : st, L1 = long_left ? lb : sb;
            int64_t R0 = long_left ? st : lt, R1 = long_left ? sb : lb;
            int64_t la = ceil16(L0), lz = (L1 >> 16) + 1, ra = R0 >> 16, rz = ceil16(R1) - 1;
            left = la < lz ? la : lz;
            right = ra > rz ? ra : rz;
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

/*
 * Fixed point, what the hardware computes. Per pixel, with acc = A << 8 (8 fraction bits):
 *   Q = max(acc_Q >> 8, 1); n = bit length of Q, e = n - 16, m = Q shifted to 16 bits
 *   r0 = floor((2^32 - 1) / (2^15 + 32 m[14:5] + 16))     (a table of 1024 in hardware)
 *   r = r0 (2^33 - m r0) >> 32                              (one Newton step, about 2^32 / m)
 *   U = acc_U >> 8 limited to 27 bits signed, u = (U r) >> (16 + e), in texels with 8
 *   fraction bits; v the same
 * then the texel at (u >> 8, v >> 8), clamped to the texture.
 */
#ifdef RS_TRACE
void rs_trace(const volatile uint8_t *p);
#endif

uint32_t rs_recip(uint32_t m)
{
    uint32_t r0 = 0xFFFFFFFFu / (0x8000u + ((m >> 5) & 1023u) * 32u + 16u);
    uint64_t p = (uint64_t)m * r0;
    return (uint32_t)(((uint64_t)r0 * ((1ull << 33) - p)) >> 32);
}

struct texfx_ctx { const struct rs_tex_setup *s; const struct rs_texture *tex; };

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
        int tu = u < 0 ? 0 : u >= t->width ? t->width - 1 : (int)u;
        int tv = v < 0 ? 0 : v >= t->height ? t->height - 1 : (int)v;
#ifdef RS_TRACE                            /* PC only: the texel addresses for a cache study */
        rs_trace(&t->data[tv * 256 + tu]);
#endif
        uint8_t c = t->data[tv * 256 + tu];
        if(c || t->routine != 64) {
            row[x] = c;
            n++;
        }
        for(int k = 0; k < 3; k++) acc[k] += s->dx[k];
    }
    return n;
}

uint32_t rs_textured_fx(volatile uint8_t *fb, const struct rs_clip *clip, const struct rs_vertex pos[3],
                        const struct rs_tex_setup *s, const struct rs_texture *tex)
{
    struct texfx_ctx c = { s, tex };
    return walk(fb, clip, pos, span_texfx, &c);
}

static int32_t sat_round(float f)
{
    if(f >= 2147483520.0f) return 0x7FFFFFFF;
    if(f <= -2147483520.0f) return -0x7FFFFFFF;
    return (int32_t)(f + (f < 0.0f ? -0.5f : 0.5f));
}

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
