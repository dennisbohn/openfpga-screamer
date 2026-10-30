/*
 * The track file's parts from MAPnOPP.SPL, MAPn.MRK and the .DTR texts (tb_spl.h), as
 * player_path(), grid_pose(), cars_data(), track_data() and opponents() of
 * tools/make_track.py make them. The maths in double as Python does it; the texts are
 * latin-1, one byte a character, with Python's whitespace (str.split(), strip()) and line
 * breaks (str.splitlines()) of that range.
 */
#include "tb_spl.h"
#include "tb_mmp.h"
#include <math.h>
#include <string.h>

#define NODE    56      /* a spline node: int32 ?, x, y, z, next, in x, in y, ?, out x, out y, 4 x ? */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* an int32 of the file as Python's float() of it: exact */
static double rdd(const uint8_t *p) { return (int32_t)rd32(p); }

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* struct.pack('<f', d): rounded to float32; 0 where Python finds it too large; a NaN as
   x86 converts it (quiet, its sign kept) */
static int put_f32(uint8_t *p, double d)
{
    union { float f; uint32_t u; } v;
    v.f = (float)d;
    if (isnan(d)) v.u = signbit(d) ? 0xFFC00000u : 0x7FC00000u;
    else if ((v.u & 0x7FFFFFFFu) == 0x7F800000u && !isinf(d)) return 0;
    wr32(p, v.u);
    return 1;
}

/* Python's round() (half to even) packed as int32; 0 outside the int32 range */
static int put_round(uint8_t *p, double v)
{
    double r = rint(v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return 0;
    wr32(p, (uint32_t)(int32_t)r);
    return 1;
}

/* ---- splines ---- */

/* The splines of a .SPL file back to back (make_track.py splines()): int32 nodes, start
   node, the nodes; up to 7 bytes after the last are left over. Returns how many, -1 if one
   runs past the end or has a negative count; *at the offset of spline k. */
static int spl_scan(const uint8_t *spl, uint32_t size, int k, uint32_t *at)
{
    uint32_t p = 0;
    int n = 0;
    while (size - p >= 8) {
        int32_t count = (int32_t)rd32(spl + p);
        if (count < 0 || (uint32_t)count > (size - p - 8) / NODE) return -1;
        if (n == k) *at = p;
        p += 8 + NODE * (uint32_t)count;
        n++;
    }
    return n;
}

/* node k of n as Python indexes a list (negative from the end); -1 outside */
static int32_t node_index(int32_t k, int32_t n)
{
    if (k < 0) k += n;
    return k >= 0 && k < n ? k : -1;
}

/* math.dist() of two handles (int32 x, y): the squares and their sum exact while the
   differences stay below 2^26, then one correctly rounded root */
static double dist(const uint8_t *a, const uint8_t *b)
{
    double dx = rdd(a) - rdd(b), dy = rdd(a + 4) - rdd(b + 4);
    return sqrt(dx * dx + dy * dy);
}

uint32_t tb_path(const uint8_t *spl, uint32_t size, uint8_t *out, uint32_t cap)
{
    uint32_t at = 0, w = 0;
    if (spl_scan(spl, size, 9, &at) < 10) return 0;
    const uint8_t *nodes = spl + at + 8;
    int32_t n = (int32_t)rd32(spl + at), k = (int32_t)rd32(spl + at + 4);
    for (int32_t i = 0; i < n; i++) {
        int32_t a = node_index(k, n);
        int32_t b = a < 0 ? -1 : node_index((int32_t)rd32(nodes + NODE * (uint32_t)a + 16), n);
        if (b < 0) return 0;
        const uint8_t *na = nodes + NODE * (uint32_t)a, *nb = nodes + NODE * (uint32_t)b;
        /* piece 0: a's in, a's out, b's in, at a's height; piece 1: a's out, b's in, b's out,
           from a's height to b's */
        for (int piece = 0; piece < 2; piece++) {
            const uint8_t *h0 = piece ? na + 32 : na + 20, *h1 = piece ? nb + 20 : na + 32,
                          *h2 = piece ? nb + 32 : nb + 20;
            double x0 = rdd(h0), x1 = rdd(h1), x2 = rdd(h2);
            double y0 = rdd(h0 + 4), y1 = rdd(h1 + 4), y2 = rdd(h2 + 4);
            double z0 = rdd(na + 12), z1 = rdd((piece ? nb : na) + 12);
            double q = (dist(h0, h1) + dist(h1, h2)) / 2 / 100.0;
            int32_t steps = q < 1.0 ? 1 : (int32_t)q;
            for (int32_t s = 0; s < steps; s++) {
                double u = (double)s / steps, v = 1 - u;
                double wa = v * v / 2, wc = u * u / 2, wb = 1 - wa - wc;
                if (cap - w < 12 ||
                    !put_round(out + w, x0 * wa + x1 * wb + x2 * wc) ||
                    !put_round(out + w + 4, y0 * wa + y1 * wb + y2 * wc) ||
                    !put_round(out + w + 8, z0 + (z1 - z0) * u)) return 0;
                w += 12;
            }
        }
        k = (int32_t)rd32(na + 16);
    }
    return w;
}

uint32_t tb_grid_pose(const uint8_t *spl, uint32_t size, const uint8_t *mmp, uint32_t mmp_size,
                      uint8_t *out, uint32_t cap)
{
    uint32_t at = 0;
    float yaw;
    if (cap < 12 || spl_scan(spl, size, 9, &at) < 10) return 0;
    int32_t k = node_index((int32_t)rd32(spl + at + 4), (int32_t)rd32(spl + at));
    if (k < 0) return 0;
    const uint8_t *nd = spl + at + 8 + NODE * (uint32_t)k;
    if (!tb_mmp_road_angle(mmp, mmp_size, (int32_t)rd32(nd + 4), (int32_t)rd32(nd + 8), &yaw) ||
        !put_f32(out + 8, (double)yaw)) return 0;
    memcpy(out, nd + 4, 8);
    return 12;
}

uint32_t tb_markers(const uint8_t *mrk, uint32_t size, uint8_t *out, uint32_t cap)
{
    if (size < 4) return 0;
    int32_t n = (int32_t)rd32(mrk);
    if (n <= 0 || (uint32_t)n > (size - 4) / 32 || (uint32_t)n > cap / 32) return 0;
    memcpy(out, mrk + 4, 32 * (uint32_t)n);
    return 32 * (uint32_t)n;
}

/* ---- text ---- */

struct tok { uint32_t at, len; };

static int is_space(uint8_t c) { return (c >= 9 && c <= 13) || (c >= 28 && c <= 32) || c == 0x85 || c == 0xA0; }
static int is_break(uint8_t c) { return (c >= 10 && c <= 13) || (c >= 28 && c <= 30) || c == 0x85; }
static int is_digit(uint8_t c) { return c >= '0' && c <= '9'; }

/* The line of t that starts at *at: returns its end, *at moves past its break. "\r\n" makes
   an empty line between the two, which no parser here minds. */
static uint32_t line_end(const uint8_t *t, uint32_t size, uint32_t *at)
{
    uint32_t e = *at;
    while (e < size && !is_break(t[e])) e++;
    *at = e < size ? e + 1 : size;
    return e;
}

/* the next token (str.split()) of t from *at to end: 1 and its place, 0 if none is left */
static int next_tok(const uint8_t *t, uint32_t end, uint32_t *at, struct tok *k)
{
    uint32_t p = *at;
    while (p < end && is_space(t[p])) p++;
    if (p >= end) return 0;
    k->at = p;
    while (p < end && !is_space(t[p])) p++;
    k->len = p - k->at;
    *at = p;
    return 1;
}

static int starts(const uint8_t *t, struct tok k, const char *s)
{
    uint32_t n = strlen(s);
    return k.len >= n && !memcmp(t + k.at, s, n);
}

/* where s first occurs in t (str.index()): 1 and *at, 0 if nowhere */
static int find(const uint8_t *t, uint32_t size, const char *s, uint32_t *at)
{
    uint32_t n = strlen(s);
    for (uint32_t i = 0; i + n <= size; i++)
        if (!memcmp(t + i, s, n)) {
            *at = i;
            return 1;
        }
    return 0;
}

/* Python's int(s, base), base 10 or 16: sign, for 16 a "0x" (and one '_') may lead, '_'
   only between digits. 1 and *v; 2 if valid but beyond 63 bits; 0 if Python refuses it. */
static int parse_int(const uint8_t *s, uint32_t n, uint32_t base, int64_t *v)
{
    uint32_t i = 0;
    int neg = 0, big = 0;
    uint64_t m = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    if (base == 16 && n - i >= 2 && s[i] == '0' && (s[i + 1] | 0x20) == 'x') {
        i += 2;
        if (i < n && s[i] == '_') i++;
    }
    if (i >= n) return 0;
    for (uint32_t j = i; j < n; j++) {
        uint8_t c = s[j];
        if (c == '_') {
            if (j == i || s[j - 1] == '_' || j + 1 == n) return 0;
            continue;
        }
        int l = c | 0x20;
        uint32_t d = (uint32_t)(is_digit(c) ? c - '0' : l >= 'a' && l <= 'f' ? l - 'a' + 10 : 99);
        if (d >= base) return 0;
        if (m > (UINT64_MAX - d) / base) big = 1;
        else m = m * base + d;
    }
    if (big || m > (uint64_t)INT64_MAX + (uint64_t)neg) return 2;
    *v = neg && m ? -(int64_t)(m - 1) - 1 : (int64_t)m;       /* -(m - 1) - 1: -2^63 too */
    return 1;
}

/* whether s[0, n) is the lower-case word w in any case */
static int word(const uint8_t *s, uint32_t n, const char *w)
{
    uint32_t i = 0;
    for (; w[i]; i++)
        if (i >= n || (s[i] | 0x20) != (uint8_t)w[i]) return 0;
    return i == n;
}

static const double p10[23] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

/* Python's float() of s[0, n): sign, digits with one '.', exponent, '_' only between
   digits; inf, infinity, nan in any case. The decimal as integer m times 10^e, then one
   multiplication or division by an exact power of ten: correctly rounded, as Python's.
   1 and *d; 0 if Python refuses it or it lies beyond that (tb_spl.h). */
static int parse_float(const uint8_t *s, uint32_t n, double *d)
{
    uint32_t i = 0;
    int neg = 0, point = 0, digits = 0, nsig = 0, lost = 0;
    uint64_t m = 0;
    int64_t e = 0;
    for (uint32_t j = 0; j < n; j++)
        if (s[j] == '_' && (j == 0 || j + 1 == n || !is_digit(s[j - 1]) || !is_digit(s[j + 1]))) return 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    if (word(s + i, n - i, "inf") || word(s + i, n - i, "infinity")) {
        *d = neg ? -(double)INFINITY : (double)INFINITY;
        return 1;
    }
    if (word(s + i, n - i, "nan")) {
        *d = neg ? -(double)NAN : (double)NAN;
        return 1;
    }
    for (; i < n; i++) {
        uint8_t c = s[i];
        if (c == '_') continue;
        if (c == '.' && !point) {
            point = 1;
            continue;
        }
        if (!is_digit(c)) break;
        digits++;
        if (m == 0 && c == '0') e -= point;             /* a leading zero */
        else if (nsig < 19) {
            m = m * 10 + (c - '0');
            nsig++;
            e -= point;
        } else {                                        /* past 19 digits: only zeros stay exact */
            lost |= c != '0';
            e += !point;
        }
    }
    if (!digits) return 0;
    if (i < n && (s[i] | 0x20) == 'e') {
        int eneg = 0, edigits = 0;
        int64_t x = 0;
        if (++i < n && (s[i] == '+' || s[i] == '-')) eneg = s[i++] == '-';
        for (; i < n && (is_digit(s[i]) || s[i] == '_'); i++) {
            if (s[i] == '_') continue;
            edigits++;
            if (x < 100000) x = x * 10 + (s[i] - '0');
        }
        if (!edigits) return 0;
        e += eneg ? -x : x;
    }
    if (i != n) return 0;
    if (m == 0) {
        *d = neg ? -0.0 : 0.0;
        return 1;
    }
    if (lost) return 0;
    while (m % 10 == 0) {
        m /= 10;
        e++;
    }
    if (m > 1ull << 53 || e < -22 || e > 22 + 15) return 0;
    double v = (double)m;
    if (e < 0) v /= p10[-e];
    else if (e <= 22) v *= p10[e];
    else {                                              /* m 10^(e - 22) still exact, then 10^22 */
        uint64_t f = 1;
        for (int64_t j = 22; j < e; j++) f *= 10;
        if (m > (1ull << 53) / f) return 0;
        v = (double)(m * f) * 1e22;
    }
    *d = neg ? -v : v;
    return 1;
}

/* ---- CARS.DTR ---- */

/* make_track.py num() of a token: after its first letter a hex number if that is 'x', else
   a float() */
struct num { int is_int; int64_t i; double d; };

static int num(const uint8_t *t, struct tok k, struct num *v)
{
    v->is_int = t[k.at] == 'x';
    if (v->is_int) return parse_int(t + k.at + 1, k.len - 1, 16, &v->i) == 1;
    return parse_float(t + k.at + 1, k.len - 1, &v->d);
}

/* struct.pack('<i', int(v)): truncated toward zero, 0 outside the int32 range */
static int put_int(uint8_t *p, const struct num *v)
{
    if (v->is_int) {
        if (v->i < INT32_MIN || v->i > INT32_MAX) return 0;
        wr32(p, (uint32_t)v->i);
        return 1;
    }
    if (!(v->d > -2147483649.0 && v->d < 2147483648.0)) return 0;
    wr32(p, (uint32_t)(int32_t)v->d);
    return 1;
}

/* one row of 14 tokens as struct '<12si4f2i7f' */
static int car_row(const uint8_t *t, const struct tok *r, uint8_t *o)
{
    uint32_t w = 0;
    for (uint32_t i = 1; i < r[0].len && w < 11; i++) {        /* str.encode(): UTF-8 */
        uint8_t c = t[r[0].at + i];
        if (c < 0x80) o[w++] = c;
        else {
            o[w++] = (uint8_t)(0xC0 | c >> 6);
            if (w < 11) o[w++] = (uint8_t)(0x80 | (c & 0x3F));
        }
    }
    while (w < 12) o[w++] = 0;
    memset(o + 40, 0, 4);                                       /* ratio[0] */
    for (int k = 1; k < 14; k++) {
        struct num v;
        uint8_t *p = o + (k < 8 ? 8 + 4 * k : 12 + 4 * k);
        if (!num(t, r[k], &v)) return 0;
        if (k == 1 || k == 6 || k == 7 ? !put_int(p, &v)                /* livery, automatic, nshift */
                                       : !put_f32(p, v.is_int ? (double)v.i : v.d)) return 0;
    }
    return 1;
}

uint32_t tb_cars(const uint8_t *dtr, uint32_t size, uint8_t *out, uint32_t cap)
{
    struct tok r[14], k;
    uint32_t at = 0, w = 0;
    int on = 0, nr = 0;
    while (at < size) {
        uint32_t p = at, end = line_end(dtr, size, &at);
        for (int first = 1; next_tok(dtr, end, &p, &k); first = 0) {
            if (!on) {                                          /* from the line after CARS_DATA */
                on = starts(dtr, k, "CARS_DATA");
                break;
            }
            if (first && starts(dtr, k, "END_LIST")) return w;
            if (dtr[k.at] == '*') break;                        /* a comment to the line's end */
            r[nr++] = k;
            if (nr == 14) {                                     /* rows may span lines */
                if (cap - w < 68 || !car_row(dtr, r, out + w)) return 0;
                w += 68;
                nr = 0;
            }
        }
    }
    return w;
}

/* ---- MAPn?.DTR ---- */

uint32_t tb_track_data(const uint8_t *dtr, uint32_t size, uint8_t *out, uint32_t cap)
{
    uint32_t at;
    if (cap < 12 || !find(dtr, size, "TRACK_DATA", &at)) return 0;
    line_end(dtr, size, &at);                                   /* the rest of its line */
    while (at < size) {
        struct tok f[4];
        uint32_t p = at, end = line_end(dtr, size, &at);
        int nf = 0;
        while (nf < 4 && next_tok(dtr, end, &p, &f[nf])) nf++;
        if (nf == 0 || dtr[f[0].at] == '*') continue;
        /* the first other line: color, camera_hi, bonus_time, carnage */
        int64_t v[4] = { 0 };
        if (nf < 4) return 0;
        for (int j = 0; j < 4; j++) {
            int ok = parse_int(dtr + f[j].at + 1, f[j].len - 1, 10, &v[j]);
            if (ok == 0 || (ok == 2 && j < 3)) return 0;
        }
        if (v[0] < 0 || v[0] > UINT32_MAX || v[1] < INT32_MIN || v[1] > INT32_MAX ||
            v[2] < INT32_MIN || v[2] > INT32_MAX) return 0;
        wr32(out, (uint32_t)v[2]);
        wr32(out + 4, (uint32_t)v[1]);
        wr32(out + 8, (uint32_t)v[0]);
        if (cap < 16) return 12;
        wr32(out + 12, v[3] < 0 || v[3] > INT32_MAX ? 0u : (uint32_t)v[3]);    /* carnage */
        return 16;
    }
    return 0;
}

uint32_t tb_opponents(const uint8_t *spl, uint32_t spl_size, const uint8_t *dtr, uint32_t dtr_size,
                      uint8_t *out, uint32_t cap)
{
    uint32_t at, w = 8, rows = 0;
    int nsp = spl_scan(spl, spl_size, -1, &at);
    if (nsp < 0 || cap < 8 || !find(dtr, dtr_size, "OPPONENTS", &at)) return 0;
    line_end(dtr, dtr_size, &at);                               /* the rest of its line */
    while (at < dtr_size) {
        struct tok f[5];
        uint32_t p = at, end = line_end(dtr, dtr_size, &at);
        int nf = 0, ok = 1;
        while (nf < 5 && next_tok(dtr, end, &p, &f[nf])) nf++;
        if (nf && starts(dtr, f[0], "END_LIST")) break;
        for (int j = 0; j < nf; j++) ok &= dtr[f[j].at] == 'f';
        if (nf < 5 || !ok) continue;                            /* a row: five 'f' numbers first */
        if (cap - w < 20) return 0;
        for (int j = 0; j < 5; j++) {
            double d;
            if (!parse_float(dtr + f[j].at + 1, f[j].len - 1, &d) || !put_f32(out + w + 4 * j, d)) return 0;
        }
        w += 20;
        rows++;
    }
    wr32(out, (uint32_t)nsp);
    wr32(out + 4, rows);
    uint32_t q = 0;
    for (int s = 0; s < nsp; s++) {
        uint32_t count = rd32(spl + q);
        int32_t start = (int32_t)rd32(spl + q + 4);
        if (start < 0 || cap - w < 8 || (cap - w - 8) / 32 < count) return 0;
        wr32(out + w, (uint32_t)start);
        wr32(out + w + 4, count);
        w += 8;
        for (uint32_t j = 0; j < count; j++) {
            const uint8_t *nd = spl + q + 8 + NODE * j;
            memcpy(out + w, nd + 4, 24);                        /* x, y, z, next, in x, in y */
            memcpy(out + w + 24, nd + 32, 8);                   /* out x, out y */
            w += 32;
        }
        q += 8 + NODE * count;
    }
    return w;
}
