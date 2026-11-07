/*
 * The geometry unit as a C model (see geom.h for what it does and for its number formats).
 * Every >> of a signed value rounds down, as in the hardware.
 * Entry point: gu_draw(), one piece of the track through the geometry unit, the setup unit and
 * the rasterizer. The routine 160 reflection is computed in integers by default and in floats
 * when GU_REFL_FLOAT is defined.
 */
#include "geom.h"
#include "bytes.h"

static int32_t clamp_size(int32_t v)
{
    return v < 1 ? 1 : v > 256 ? 256 : v;
}

/* Routine 160: the reflection's u and v for each vertex number. The table is kept from block
   to block, like the original's table (original: STARTL.EXE 0xc3bd4) and the hardware's RAM. */
static int16_t refl_u[GU_MAX_VERT], refl_v[GU_MAX_VERT];

#ifdef GU_REFL_FLOAT
/* The original's form in floats, rounded to nearest as its fistp does (original: STARTL.EXE
   0xc2230). It is there to compare with the integer form in the #else branch. */
#include <math.h>
static void reflection(const struct gu_piece *p, const struct su_fixed *vt, const uint8_t *nrm, int i)
{
    float n[3], e[3], q[3], d = 0.0f, l;
    for(int k = 0; k < 3; k++) q[k] = *(const float *)(nrm + 12 * i + 4 * k);
    for(int k = 0; k < 3; k++)
        n[k] = ((float)p->m[3 * k] * q[0] + (float)p->m[3 * k + 1] * q[1] +
                (float)p->m[3 * k + 2] * q[2]) / 65536.0f;
    e[0] = (float)vt[i].r; e[1] = (float)vt[i].u; e[2] = (float)vt[i].f - 32768.0f;
    l = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    for(int k = 0; k < 3; k++) e[k] /= l;
    for(int k = 0; k < 3; k++) d += 2.0f * e[k] * n[k];
    refl_u[i] = (int16_t)(lrintf(127.0f * (e[0] - d * n[0])) + 127 - (int32_t)(p->refl >> 16 & 255));
    refl_v[i] = (int16_t)(lrintf(127.0f * (e[2] - d * n[2])) + 127 - (int32_t)(p->refl >> 24));
}
#else
/* 2^16 / sqrt(x) at x = (i + 0.5) / 32 for i = 32..127. The hardware's table has 128 entries;
   an index below 32 uses the entry for 32, so this table starts there. */
static const uint16_t rsq[96] = {
    65030, 64052, 63117, 62222, 61363, 60540, 59748, 58987, 58254, 57548, 56867, 56210,
    55574, 54960, 54366, 53791, 53233, 52693, 52169, 51660, 51165, 50685, 50218, 49763,
    49321, 48890, 48470, 48061, 47663, 47273, 46894, 46523, 46161, 45807, 45462, 45124,
    44793, 44470, 44153, 43843, 43540, 43243, 42951, 42666, 42386, 42112, 41843, 41579,
    41320, 41065, 40816, 40571, 40330, 40093, 39861, 39632, 39408, 39187, 38970, 38756,
    38546, 38340, 38136, 37936, 37739, 37545, 37354, 37166, 36980, 36798, 36618, 36441,
    36266, 36093, 35924, 35756, 35591, 35428, 35267, 35109, 34953, 34798, 34646, 34496,
    34347, 34201, 34056, 33913, 33772, 33633, 33496, 33360, 33225, 33093, 32962, 32832,
};

/* A normal's component, given as float bits, rounded to Q14 (14 fraction bits). The input must
   be smaller than 2 in size. It gives 0 below 2^-15. */
static int32_t q14(uint32_t w)
{
    uint32_t e = w >> 23 & 255;
    int32_t q = e >> 4 == 7
                    ? (int32_t)((((w & 0x7FFFFFu) | 0x800000u) >> (8 + (15 - (e & 15))) & 0xFFFF) + 1) >> 1
                    : 0;
    return (int16_t)(w >> 31 ? -q : q);         /* 16 bits, like the unit's register */
}

/* The low `bits` bits of v as a signed number: a register of the unit that is that wide. */
static int64_t sx(int64_t v, int bits)
{
    uint64_t m = 1ull << (bits - 1);
    return (int64_t)((((uint64_t)v & ((m << 1) - 1)) ^ m) - m);
}

/* Vertex i's reflection in integers (geom.h has the steps). Every sx() cuts a value to the
   width of the hardware register that holds it, so the result is the hardware's bit for bit. */
static void reflection(const struct gu_piece *p, const struct su_fixed *vt, const uint8_t *nrm, int i)
{
    int64_t q[3], n[3], pv[3], pn[3];
    for(int k = 0; k < 3; k++) q[k] = q14((uint32_t)ld32s(nrm + 12 * i + 4 * k));
    for(int k = 0; k < 3; k++)                  /* the normal into view space: Q16, 18 bits */
        n[k] = sx((sx(p->m[3 * k], 18) * q[0] + sx(p->m[3 * k + 1], 18) * q[1] +
                   sx(p->m[3 * k + 2], 18) * q[2]) >> 14, 18);
    pv[0] = vt[i].r; pv[1] = vt[i].u; pv[2] = (int32_t)((uint32_t)vt[i].f - 32768u);
    uint32_t o = 0;
    for(int k = 0; k < 3; k++) o |= (uint32_t)(pv[k] ^ (pv[k] >> 31));
    int l = 0;
    while(o >> l) l++;
    for(int k = 0; k < 3; k++) pn[k] = sx((pv[k] * 65536) >> l, 17);
    int64_t S = sx(pn[0] * n[0], 34) + sx(pn[1] * n[1], 34) + sx(pn[2] * n[2], 34);
    int64_t sh = sx(S >> 16, 18), Q = pn[0] * pn[0] + pn[1] * pn[1] + pn[2] * pn[2];
    int64_t br = sx(pn[0] - (sx(sh * n[0], 34) >> 15), 18), bf = sx(pn[2] - (sx(sh * n[2], 34) >> 15), 18);
    int h = Q < (1ll << 32);
    int64_t qn = h ? Q << 2 : Q;
    int ix = (int)(qn >> 27);
    int64_t x = qn >> 17, y0 = rsq[ix < 32 ? 0 : ix - 32];
    int64_t t1 = (x * y0) >> 16, t2 = (t1 * y0) >> 15;
    int64_t t3 = (3 * 65536 - t2) >> 1;
    int64_t y = sx((y0 * sx(t3 - (t3 >> 7), 18)) >> 16, 18);
    int64_t u = ((sx(br * y, 34) * (1 << h)) + (1 << 24)) >> 25,
            v = ((sx(bf * y, 34) * (1 << h)) + (1 << 24)) >> 25;
    refl_u[i] = (int16_t)sx(u + 127 - (int64_t)(p->refl >> 16 & 255), 10);
    refl_v[i] = (int16_t)sx(v + 127 - (int64_t)(p->refl >> 24), 10);
}
#endif

/*
 * Draws one piece. The steps are the hardware's:
 *   1. The block's vertices into view space (vt[]), then the reflections if the block has them.
 *   2. For each face of the piece's row: the facing test, the material and texture frame,
 *      and the corners (position and texel position).
 *   3. The face goes to su_polygon8() as a polygon of 3 or 4 corners.
 * Layout of the data it reads (offsets in bytes):
 *   block  +0x10 vertex count, +0x18 flags, +0x1C vertices and +0x20 normals and +0x24 texture
 *          coordinates (offsets from the object), +0x28 + 4 row: the row's face list (offset
 *          from the block; 16-bit face offsets from the block, ended by 0xFFFF)
 *   vertex 6 bytes (x, y, z as int16); the texture coordinates lie at the same offsets
 *   face   +2 colour (low byte), +4 material (offset in mlb), +6 corner count, +8 the corners
 *          (vertex offsets, 16 bits each), +0x14, +0x18, +0x1C the normal
 *   material  +0 routine, +4 texture record (offset in mlb), +8 width, +12 height
 *   texture record  +0 frame count n, +8 + 4 frame: the frame's texels (offset in mlb)
 * Adds the counts to *st and returns the pixels drawn.
 */
uint32_t gu_draw(volatile uint8_t *fb, const struct rs_clip *clip, const struct su_view *v,
                 const uint8_t *mlb, const struct gu_piece *p, struct gu_stats *st)
{
    uint32_t pixels = 0;
    static struct su_fixed vt[GU_MAX_VERT];     /* the vertices in view space */
    const uint8_t *verts = p->obj + ld32s(p->blk + 0x1C), *uvs = p->obj + ld32s(p->blk + 0x24);
    int32_t nvert = ld32s(p->blk + 0x10), ro = ld32s(p->blk + 0x28 + 4 * p->row);
    if(nvert < 0 || nvert > GU_MAX_VERT || ro <= 0) return 0;

    for(int i = 0; i < nvert; i++) {
        int64_t x = ld16s(verts + 6 * i), y = ld16s(verts + 6 * i + 2), z = ld16s(verts + 6 * i + 4);
        for(int k = 0; k < 3; k++) {
            int64_t s = x * p->m[3 * k] + y * p->m[3 * k + 1] + z * p->m[3 * k + 2];
            int32_t w = (int32_t)(p->t[k] + (s >> 8));
            if(k == 0) vt[i].r = w;
            else if(k == 1) vt[i].u = w;
            else vt[i].f = w;
        }
    }
    st->vertices += (uint32_t)nvert;
    if(ld32s(p->blk + 0x18) & 0x2000) {         /* the reflections, for the vertices with a normal */
        const uint8_t *nrm = p->obj + ld32s(p->blk + 0x20);
        int32_t cnt = ld32s(nrm);
        for(int i = 0; i < nvert && i < cnt; i++) reflection(p, vt, nrm + 4, i);
    }

    const uint8_t *e = p->blk + ro;
    for(int nf = 0; nf < GU_MAX_FACES; nf++, e += 2) {
        uint16_t fo = ld16u(e);
        if(fo == 0xFFFF) break;
        const uint8_t *fc = p->blk + fo;
        int nc = ld16s(fc + 6);
        if(nc < 3 || nc > 4) continue;

        /* Facing test (docs/original_geometry.md 6.2): leave out a face that is turned away
           from the eye. A mirrored piece turns the test round. */
        const uint8_t *c0 = verts + ld16u(fc + 8);
        int64_t bx = ((int64_t)ld16s(c0) << 8) - p->e[0], by = ((int64_t)ld16s(c0 + 2) << 8) - p->e[1];
        int64_t bz = ((int64_t)ld16s(c0 + 4) << 8) - p->e[2];
        if((bx * ld16s(fc + 0x14) + by * ld16s(fc + 0x18) + bz * ld16s(fc + 0x1C) < 0) != (p->mirror != 0))
            continue;

        /* The material. */
        const uint8_t *mat = mlb + ld16s(fc + 4);
        int32_t routine = ld32s(mat), tw = 0, th = 0;
        /* Routine 192 (original: STARTL.EXE 0xcae84, the bridge of track 5) takes the face's
           colour and corners like routine 128: one colour and no texture record. */
        int flat = routine == 128 || routine == 192;
        if(routine == 224) st->see_through++;   /* drawn through the see-through table (raster.h) */
        struct rs_texture tex;
        if(!flat) {
            tw = clamp_size(ld32s(mat + 8));
            th = clamp_size(ld32s(mat + 12));
            /* The texture frame (original: STARTL.EXE 0xd5980, docs/original_geometry.md 8.3):
               n x phase >> 14, where n is the number of frames in the texture record. Only the
               low 16 bits of n count, and only when n is above 0. */
            const uint8_t *trec = mlb + ld32s(mat + 4);
            int32_t n = ld32s(trec);
            /* Routine 160 (a car's windows) takes its phase from the object's reflection
               picture (original: STARTL.EXE [0x84288], record +0x3c) and uses n x phase >> 16.
               The other routines use the paint job's phase, which includes the brake lights
               (original: STARTL.EXE [0x84284]). */
            uint32_t ph = routine == 160 ? p->refl & 0xFFFF : (p->phase & 0x3FFF) << 2;
            uint32_t frame = n > 0 ? ((uint32_t)n & 0xFFFF) * ph >> 16 : 0;
            tex.data = mlb + ld32s(trec + 8 + 4 * frame);
            tex.width = tw;
            tex.height = th;
            /* Routine 48 is drawn as routine 64, texel 0 see-through. Any routine other than 64,
               48, 224 and 160 is drawn as the plain texture, routine 16. */
            tex.routine = routine == 64 || routine == 48 ? 64 : routine == 224 ? 224 :
                          routine == 160 ? 160 : 16;
        }

        /* The corners: position in view space and texel position. */
        struct su_fixed c[4];
        int ok = 1;
        for(int j = 0; j < nc; j++) {
            uint16_t vo = ld16u(fc + 8 + 2 * j);
            int vi = vo / 6;
            if(vi >= nvert) ok = 0;             /* a corner beyond the vertices leaves the face out */
            c[j].r = vt[vi % GU_MAX_VERT].r;    /* the modulo only keeps the read inside vt[] */
            c[j].u = vt[vi % GU_MAX_VERT].u;
            c[j].f = vt[vi % GU_MAX_VERT].f;
            int32_t s = ld16s(uvs + vo) * tw, t = ld16s(uvs + vo + 2) * th;
            /* Routine 160: the reflection in whole texels (original: STARTL.EXE 0xdb310, u << 3). */
            if(routine == 160) {
                s = refl_u[vi % GU_MAX_VERT] * 256;
                t = refl_v[vi % GU_MAX_VERT] * 256;
            }
            c[j].s = s < -(1 << 23) ? -(1 << 23) : s > (1 << 23) - 1 ? (1 << 23) - 1 : s;
            c[j].t = t < -(1 << 23) ? -(1 << 23) : t > (1 << 23) - 1 ? (1 << 23) - 1 : t;
        }
        if(!ok) continue;
        st->faces++;
        st->triangles += (uint32_t)(nc - 2);
        pixels += su_polygon8(fb, clip, v, c, nc, flat ? 0 : &tex, (uint8_t)(ld16s(fc + 2) & 255));
    }
    return pixels;
}
