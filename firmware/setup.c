/*
 * Setup unit C model (setup.h). Every >> of a signed value is arithmetic (rounds down), as
 * in the hardware; the products fit 64 bits.
 */
#include "setup.h"

#define FB_STRIDE 320

static int64_t clamp64(int64_t v, int64_t lo, int64_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* bit length: 0 for 0, else 1 + the index of the highest one */
static int bitlen(uint64_t v)
{
    int n = 0;
    while(v) {
        n++;
        v >>= 1;
    }
    return n;
}

/* v (bit length l >= 1) shifted to 16 bits, its highest one at bit 15; bits below are lost */
static uint32_t norm16(uint64_t v, int l)
{
    return (uint32_t)(l >= 16 ? v >> (l - 16) : v << (16 - l));
}

/* float to 8 fraction bits: |v| 256 truncated, below 1/256 zero, from 2^23 on (and infinity,
   NaN) 2^31 - 1, the sign applied after */
int32_t su_fix8(float v)
{
    union { float f; uint32_t u; } b = { v };
    uint32_t e = b.u >> 23 & 255, m = (b.u & 0x7FFFFFu) | 0x800000u, a;
    if(e >= 150) a = 0x7FFFFFFFu;
    else if(e >= 142) a = m << (e - 142);
    else if(e > 118) a = m >> (142 - e);
    else a = 0;
    return b.u >> 31 ? -(int32_t)a : (int32_t)a;
}

void su_fix(const struct su_corner *c, struct su_fixed *x)
{
    x->r = su_fix8(c->r);
    x->u = su_fix8(c->u);
    x->f = su_fix8(c->f);
    x->s = (int32_t)clamp64(su_fix8(c->s), -(1 << 23), (1 << 23) - 1);
    x->t = (int32_t)clamp64(su_fix8(c->t), -(1 << 23), (1 << 23) - 1);
}

/* distance from plane k, >= 0 inside: near (depth 128), the guard band (|r|, |u| up to
   SU_GUARD times the depth), far */
static int64_t plane(const struct su_view *v, const struct su_fixed *c, int k)
{
    int64_t g = (int64_t)c->f * SU_GUARD;
    switch(k) {
    case 0: return (int64_t)c->f - SU_NEAR8;
    case 1: return g - c->r;
    case 2: return g + c->r;
    case 3: return g - c->u;
    case 4: return g + c->u;
    default: return (int64_t)v->far8 - c->f;
    }
}

static int32_t lerp(int32_t a, int32_t b, int64_t t16)
{
    return (int32_t)(a + ((((int64_t)b - a) * t16) >> 16));
}

int su_clip(const struct su_view *v, const struct su_fixed *in, int n, struct su_fixed *out)
{
    struct su_fixed buf[2][SU_MAX_POLY];
    int any = 0, all = 0x3F;
    for(int i = 0; i < n; i++) {
        int code = 0;
        for(int k = 0; k < 6; k++)
            if(plane(v, &in[i], k) < 0) code |= 1 << k;
        any |= code;
        all &= code;
        buf[0][i] = in[i];
    }
    if(all) return 0;                       /* wholly outside one plane */
    int cur = 0;
    for(int k = 0; k < 6; k++) {
        if(!(any >> k & 1)) continue;
        const struct su_fixed *p = buf[cur];
        struct su_fixed *q = buf[!cur];
        int m = 0;
        for(int i = 0; i < n; i++) {
            const struct su_fixed *a = &p[i], *b = &p[i + 1 < n ? i + 1 : 0];
            int64_t da = plane(v, a, k), db = plane(v, b, k);
            if(da >= 0) q[m++] = *a;
            if((da >= 0) != (db >= 0)) {
                /* t = |da| / (|da| + |db|) with 16 fraction bits from the table */
                uint64_t ea = (uint64_t)(da < 0 ? -da : da), d = ea + (uint64_t)(db < 0 ? -db : db);
                int l = bitlen(d);
                int64_t t16 = (int64_t)((ea * rs_recip(norm16(d, l))) >> l);
                if(t16 > 65536) t16 = 65536;
                q[m].r = lerp(a->r, b->r, t16);
                q[m].u = lerp(a->u, b->u, t16);
                q[m].f = lerp(a->f, b->f, t16);
                q[m].s = lerp(a->s, b->s, t16);
                q[m].t = lerp(a->t, b->t, t16);
                m++;
            }
        }
        n = m;
        cur = !cur;
        if(n < 3) return 0;
    }
    for(int i = 0; i < n; i++) out[i] = buf[cur][i];
    return n;
}

void su_project(const struct su_view *v, const struct su_fixed *c, struct su_point *p)
{
    int32_t r = c->r, u = c->u, f = c->f;
    int64_t s = c->s, t = c->t;
    if(f < 1) f = 1;
    int l = bitlen((uint32_t)f);
    uint32_t rc = rs_recip(norm16((uint32_t)f, l));         /* 2^(16 + l) / f */
    /* r / f and u / f with 16 fraction bits, within +-16 */
    int64_t px = clamp64(((int64_t)r * rc) >> l, -(1 << 20) + 1, (1 << 20) - 1);
    int64_t py = clamp64(((int64_t)u * rc) >> l, -(1 << 20) + 1, (1 << 20) - 1);
    int64_t x = (((int64_t)v->cx << 16) + (int64_t)v->scale * px + 32768) >> 16;
    int64_t y = (((int64_t)v->cy << 16) - (int64_t)v->scale * py + 32768) >> 16;
    p->x = (int32_t)clamp64(x, -32768, 32767);
    p->y = (int32_t)clamp64(y, -32768, 32767);
    p->rc = rc;
    p->l = l;
    p->sr = s * rc;
    p->tr = t * rc;
}

int su_triangle(const struct su_point p[3], struct rs_vertex pos[3], struct rs_tex_setup *s, int textured)
{
    for(int i = 0; i < 3; i++) {
        pos[i].x = p[i].x;
        pos[i].y = p[i].y;
    }
    if(!textured) return 1;

    int64_t dx1 = p[1].x - p[0].x, dy1 = p[1].y - p[0].y;
    int64_t dx2 = p[2].x - p[0].x, dy2 = p[2].y - p[0].y;
    int64_t d = dx1 * dy2 - dx2 * dy1;
    if(d == 0) return 0;
    uint64_t ad = (uint64_t)(d < 0 ? -d : d);
    int ld = bitlen(ad);
    uint32_t rcd = rs_recip(norm16(ad, ld));                 /* 2^(16 + ld) / |d| */

    /* Q, U, V at the corners: Q = rc scaled to the triangle's largest */
    int lmin = p[0].l < p[1].l ? p[0].l : p[1].l;
    if(p[2].l < lmin) lmin = p[2].l;
    int64_t a[3][3];
    for(int i = 0; i < 3; i++) {
        int di = p[i].l - lmin;
        a[0][i] = ((int64_t)p[i].rc << 6) >> di;
        a[1][i] = clamp64(p[i].sr >> (10 + di), -(1 << 26), (1 << 26) - 1);
        a[2][i] = clamp64(p[i].tr >> (10 + di), -(1 << 26), (1 << 26) - 1);
    }

    /* the reference pixel: corner 0's, on the screen; offsets of its centre from corner 0 */
    int32_t xr = p[0].x >> 4, yr = p[0].y >> 4;
    xr = xr < 0 ? 0 : xr > FB_STRIDE - 1 ? FB_STRIDE - 1 : xr;
    yr = yr < 0 ? 0 : yr > 199 ? 199 : yr;
    s->xr = xr;
    s->yr = yr;
    int64_t fx = 16 * xr + 8 - p[0].x, fy = 16 * yr + 8 - p[0].y;

    for(int k = 0; k < 3; k++) {
        int64_t da1 = a[k][1] - a[k][0], da2 = a[k][2] - a[k][0];
        int64_t nx = da1 * dy2 - da2 * dy1, ny = da2 * dx1 - da1 * dx2;
        if(d < 0) {
            nx = -nx;
            ny = -ny;
        }
        /* per pixel, 8 fraction bits: 4096 n / d */
        int64_t gx = clamp64((nx * rcd + ((int64_t)1 << (ld + 3))) >> (ld + 4), INT32_MIN, INT32_MAX);
        int64_t gy = clamp64((ny * rcd + ((int64_t)1 << (ld + 3))) >> (ld + 4), INT32_MIN, INT32_MAX);
        s->dx[k] = (int32_t)gx;
        s->dy[k] = (int32_t)gy;
        s->a[k] = (int32_t)clamp64(a[k][0] + ((gx * fx + gy * fy + 2048) >> 12), INT32_MIN, INT32_MAX);
    }
    return 1;
}

uint32_t su_polygon(volatile uint8_t *fb, const struct rs_clip *clip, const struct su_view *v,
                    const struct su_corner *c, int n, const struct rs_texture *tex, uint8_t colour)
{
    struct su_fixed in[SU_MAX_IN], out[SU_MAX_POLY];
    struct su_point p[3];
    uint32_t pixels = 0;
    if(n < 3 || n > SU_MAX_IN) return 0;
    for(int i = 0; i < n; i++) su_fix(&c[i], &in[i]);
    n = su_clip(v, in, n, out);
    if(n < 3) return 0;
    su_project(v, &out[0], &p[0]);
    su_project(v, &out[1], &p[1]);
    for(int i = 2; i < n; i++) {
        struct rs_vertex pos[3];
        struct rs_tex_setup s;
        su_project(v, &out[i], &p[2]);
        if(su_triangle(p, pos, &s, tex != 0))
            pixels += tex ? rs_textured_fx(fb, clip, pos, &s, tex) : rs_triangle(fb, clip, pos, colour);
        p[1] = p[2];
    }
    return pixels;
}
