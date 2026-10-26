/*
 * Texture demo (texdemo.h). The file (tools/make_textures.py): "SCRT", count, then per
 * texture {routine, width, height, offset} from byte 8, the palette at 0x400. Expected
 * order: checkered flag, banner, sign (see-through), guardrail (see-through), asphalt.
 *
 * The scene stands on y = 0 and turns about the y axis; the camera looks at it from
 * above and in front. Ground first, then the upright quads from far to near, like the
 * game's painter's algorithm.
 */
#include "texdemo.h"

#define TEX_MAX     16
#define N_TEX       5

static struct rs_texture tex[TEX_MAX];
static const volatile uint32_t *palette;
static float sin_q[65];                 /* sin(2 pi i / 256) for i = 0..64 */
int texdemo_mode;                       /* TEXDEMO_FLOAT, TEXDEMO_FIXED, TEXDEMO_HW */
uint32_t (*texdemo_hw)(const struct su_corner c[4], const struct rs_texture *t);
void (*texdemo_hw_piece)(const struct gu_piece *p);
static const uint8_t *mesh_mlb, *mesh_obj, *mesh_blk;  /* the cube (texdemo_mesh) */

struct v3 { float x, y, z; };
struct quad { struct v3 p[4]; int tex; };   /* corners from top left, clockwise as seen */

int texdemo_init(const volatile uint8_t *file)
{
    const volatile uint32_t *w = (const volatile uint32_t *)file;
    if(w[0] != 0x54524353u) return 0;   /* "SCRT" */
    uint32_t n = w[1];
    if(n < N_TEX || n > TEX_MAX) return 0;
    for(uint32_t i = 0; i < n; i++) {
        tex[i].routine = (int)w[2 + 4 * i];
        tex[i].width = (int)w[3 + 4 * i];
        tex[i].height = (int)w[4 + 4 * i];
        tex[i].data = file + w[5 + 4 * i];
    }
    palette = (const volatile uint32_t *)(file + 0x400);

    const float dc = 0.99969881869620f, ds = 0.02454122852291f;    /* cos, sin of 2 pi / 256 */
    float c = 1.0f, s = 0.0f;
    for(int i = 0; i <= 64; i++) {
        sin_q[i] = s;
        float t = c * dc - s * ds;
        s = s * dc + c * ds;
        c = t;
    }
    return (int)n;
}

const volatile uint32_t *texdemo_palette(void)
{
    return palette;
}

static float sin256(uint32_t a)
{
    a &= 255;
    float s = sin_q[a & 64 ? 64 - (a & 63) : a & 63];
    return a & 128 ? -s : s;
}

/* world to view: turned by (sa, ca) about y, then the camera at (0, 2.2, -4.5) tilted 25 degrees down */
static struct v3 view(struct v3 p, float sa, float ca)
{
    const float cy = 2.2f, cz = -4.5f, sp = 0.42261826f, cp = 0.90630779f;
    struct v3 t = { p.x * ca + p.z * sa, p.y, p.z * ca - p.x * sa };
    float dy = t.y - cy, dz = t.z - cz;
    struct v3 v = { t.x, dy * cp + dz * sp, dz * cp - dy * sp };
    return v;
}

#define FOCAL 250.0f

void texdemo_view(const struct rs_clip *clip, struct su_view *v)
{
    v->cx = (clip->x0 + clip->x1) * 8;      /* the centre in 1/16 pixel */
    v->cy = (clip->y0 + clip->y1) * 8;
    v->scale = (uint32_t)(FOCAL * 16.0f);
    v->far8 = 10 * 256 * 256;               /* depth 10 (scaled by 256, 8 fraction bits) */
}

/* a quad: in view space to the setup unit or its C model (scaled by 256: the setup unit
   takes 8 fraction bits, the projection does not change) */
static uint32_t draw_quad(volatile uint8_t *fb, const struct rs_clip *clip, const struct v3 v[4],
                          const struct rs_texture *t)
{
    const float u[4] = { 0.0f, (float)t->width, (float)t->width, 0.0f };
    const float w[4] = { 0.0f, 0.0f, (float)t->height, (float)t->height };
#ifdef TEXDEMO_FLOAT_REF                 /* only for the comparison on the PC */
    if(texdemo_mode == TEXDEMO_FLOAT) {
        const float f = FOCAL, cx = (float)(clip->x0 + clip->x1) / 2, cy = (float)(clip->y0 + clip->y1) / 2;
        struct rs_tvertex s[4];
        for(int i = 0; i < 4; i++) {
            float q = 1.0f / v[i].z;
            s[i].x = (int32_t)((cx + f * v[i].x * q) * 16.0f + 0.5f);
            s[i].y = (int32_t)((cy - f * v[i].y * q) * 16.0f + 0.5f);
            s[i].q = q;
            s[i].uq = u[i] * q;
            s[i].vq = w[i] * q;
        }
        struct rs_tvertex t0[3] = { s[0], s[1], s[2] }, t1[3] = { s[0], s[2], s[3] };
        return rs_textured(fb, clip, t0, t) + rs_textured(fb, clip, t1, t);
    }
#endif
    struct su_corner c[4];
    for(int i = 0; i < 4; i++)
        c[i] = (struct su_corner){ v[i].x * 256.0f, v[i].y * 256.0f, v[i].z * 256.0f, u[i], w[i] };
    if(texdemo_mode == TEXDEMO_HW) return texdemo_hw(c, t);
    struct su_view sv;
    texdemo_view(clip, &sv);
    return su_polygon(fb, clip, &sv, c, 4, t, 0);
}

/*
 * A cube for the geometry unit, in the game's mesh format (track.c, geom.h): 1 scene unit
 * wide (256 mesh units a scene unit), its faces' normals pointing inwards (away from the
 * side they are seen from), all eight face rows the same list of the six faces; the
 * materials (textured, one colour, texel 0 see-through) at 0x100 and their texture records
 * at 0x180 of the texture file (free bytes before the palette), so the file is the MLB.
 */
static void put32(volatile uint8_t *p, uint32_t v) { *(volatile uint32_t *)p = v; }
static void put16(volatile uint8_t *p, uint32_t v) { *(volatile uint16_t *)p = (uint16_t)v; }

void texdemo_mesh(volatile uint8_t *file, volatile uint8_t *mem)
{
    static const uint8_t corner[6][4] = { { 4, 5, 7, 6 }, { 0, 2, 3, 1 }, { 2, 6, 7, 3 },
                                          { 0, 1, 5, 4 }, { 1, 3, 7, 5 }, { 0, 4, 6, 2 } };
    static const int16_t normal[6][3] = { { -16384, 0, 0 }, { 16384, 0, 0 }, { 0, -16384, 0 },
                                          { 0, 16384, 0 }, { 0, 0, -16384 }, { 0, 0, 16384 } };
    static const uint32_t routine[6] = { 16, 16, 128, 64, 16, 16 };
    static const uint8_t texture[6] = { 0, 1, 0, 2, 3, 4 };
    for(int k = 0; k < 6; k++) {                /* materials and texture records */
        volatile uint8_t *m = file + 0x100 + 16 * k, *r = file + 0x180 + 16 * k;
        const struct rs_texture *tx = &tex[texture[k]];
        put32(m, routine[k]);
        put32(m + 4, 0x180u + 16u * (uint32_t)k);
        put32(m + 8, (uint32_t)tx->width);
        put32(m + 12, (uint32_t)tx->height);
        put32(r, 1);
        put32(r + 4, 0);
        put32(r + 8, (uint32_t)(tx->data - file));
    }
    volatile uint8_t *obj = mem, *blk = mem + 0x40;
    put32(obj + 0x18, 0x40);                    /* the block */
    put32(blk + 0x10, 8);                       /* vertices */
    put32(blk + 0x1C, 0x100);                   /* vertex array, from the object */
    put32(blk + 0x24, 0x140);                   /* texture coordinates, from the object */
    for(int r = 0; r < 8; r++) put32(blk + 0x28 + 4 * r, 0x300);    /* the face rows */
    for(int i = 0; i < 8; i++) {
        put16(obj + 0x100 + 6 * i, i & 4 ? 128 : -128);
        put16(obj + 0x102 + 6 * i, i & 2 ? 128 : -128);
        put16(obj + 0x104 + 6 * i, i & 1 ? 128 : -128);
        put16(obj + 0x140 + 6 * i, ((i >> 2) ^ i) & 1 ? 255 : 0);
        put16(obj + 0x142 + 6 * i, i & 2 ? 0 : 255);
        put16(obj + 0x144 + 6 * i, 0);
    }
    for(int k = 0; k < 6; k++) {
        volatile uint8_t *f = blk + 0x200 + 0x20 * k;
        put16(f + 2, 200);                      /* colour (one-colour material) */
        put16(f + 4, 0x100u + 16u * (uint32_t)k);
        put16(f + 6, 4);
        for(int j = 0; j < 4; j++) put16(f + 8 + 2 * j, 6u * corner[k][j]);
        put16(f + 0x14, (uint16_t)normal[k][0]);
        put16(f + 0x18, (uint16_t)normal[k][1]);
        put16(f + 0x1C, (uint16_t)normal[k][2]);
        put16(blk + 0x300 + 2 * k, 0x200u + 0x20u * (uint32_t)k);
    }
    put16(blk + 0x30C, 0xFFFF);
    mesh_mlb = (const uint8_t *)file;
    mesh_obj = (const uint8_t *)obj;
    mesh_blk = (const uint8_t *)blk;
}

/* the cube at (0, 0.5, 0): its place in view space from where view() takes the axes */
static uint32_t draw_mesh(volatile uint8_t *fb, const struct rs_clip *clip, float sa, float ca)
{
    const struct v3 pos = { 0.0f, 0.5f, 0.0f };
    struct v3 o = view(pos, sa, ca), b = view((struct v3){ 0, 0, 0 }, sa, ca);
    struct v3 ax[3] = { view((struct v3){ pos.x + 1, pos.y, pos.z }, sa, ca),
                        view((struct v3){ pos.x, pos.y + 1, pos.z }, sa, ca),
                        view((struct v3){ pos.x, pos.y, pos.z + 1 }, sa, ca) };
    float a[3][3];                              /* scene axis c in view space: a[.][c] */
    for(int c = 0; c < 3; c++) {
        a[0][c] = ax[c].x - o.x;
        a[1][c] = ax[c].y - o.y;
        a[2][c] = ax[c].z - o.z;
    }
    struct gu_piece g;
    for(int r = 0; r < 3; r++)
        for(int c = 0; c < 3; c++) g.m[3 * r + c] = (int32_t)(a[r][c] * 65536.0f);
    g.t[0] = (int32_t)(o.x * 65536.0f);
    g.t[1] = (int32_t)(o.y * 65536.0f);
    g.t[2] = (int32_t)(o.z * 65536.0f);
    /* the camera in the scene: -a^T b; in the cube's frame with 8 fraction bits */
    for(int c = 0; c < 3; c++) {
        float cam = -(a[0][c] * b.x + a[1][c] * b.y + a[2][c] * b.z);
        float p = c == 0 ? pos.x : c == 1 ? pos.y : pos.z;
        g.e[c] = (int32_t)((cam - p) * 65536.0f);
    }
    g.row = 0;
    g.obj = mesh_obj;
    g.blk = mesh_blk;
    if(texdemo_mode == TEXDEMO_HW) {
        texdemo_hw_piece(&g);
        return 0;
    }
    struct su_view sv;
    struct gu_stats st = { 0, 0, 0, 0 };
    texdemo_view(clip, &sv);
    return gu_draw(fb, clip, &sv, mesh_mlb, &g, &st);
}

uint32_t texdemo_draw(volatile uint8_t *fb, const struct rs_clip *clip, uint32_t a, uint32_t *tris)
{
    static const struct quad ground[2] = {
        /* asphalt from behind the camera to past the far plane: the setup unit cuts it */
        { { { -12.0f, 0, 12.0f }, { 12.0f, 0, 12.0f }, { 12.0f, 0, -12.0f }, { -12.0f, 0, -12.0f } }, 4 },
        { { { -1.0f, 0, 1.0f }, { 1.0f, 0, 1.0f }, { 1.0f, 0, -1.0f }, { -1.0f, 0, -1.0f } }, 0 },  /* flag */
    };
    static const struct quad upright[3] = {
        { { { -1.2f, 1.2f, 1.6f }, { 1.2f, 1.2f, 1.6f }, { 1.2f, 0, 1.6f }, { -1.2f, 0, 1.6f } }, 1 },         /* banner */
        { { { -1.8f, 0.73f, -1.2f }, { -0.6f, 0.73f, -1.2f }, { -0.6f, 0.3f, -1.2f }, { -1.8f, 0.3f, -1.2f } }, 2 },  /* sign */
        { { { 1.8f, 0.45f, -1.8f }, { 1.8f, 0.45f, 1.8f }, { 1.8f, 0, 1.8f }, { 1.8f, 0, -1.8f } }, 3 },     /* guardrail */
    };
    float sa = sin256(a), ca = sin256(a + 64);
    struct v3 v[4];
    uint32_t n = 0;

    for(int i = 0; i < 2; i++) {
        for(int k = 0; k < 4; k++) v[k] = view(ground[i].p[k], sa, ca);
        n += draw_quad(fb, clip, v, &tex[ground[i].tex]);
    }
    if(mesh_obj) n += draw_mesh(fb, clip, sa, ca);

    /* upright quads from far to near by the depth of their centres */
    float depth[3];
    int order[3] = { 0, 1, 2 };
    for(int i = 0; i < 3; i++) {
        struct v3 c = { 0, 0, 0 };
        for(int k = 0; k < 4; k++) {
            c.x += upright[i].p[k].x / 4;
            c.y += upright[i].p[k].y / 4;
            c.z += upright[i].p[k].z / 4;
        }
        depth[i] = view(c, sa, ca).z;
    }
    for(int i = 0; i < 2; i++)
        for(int j = 0; j < 2 - i; j++)
            if(depth[order[j]] < depth[order[j + 1]]) {
                int t = order[j];
                order[j] = order[j + 1];
                order[j + 1] = t;
            }
    for(int i = 0; i < 3; i++) {
        const struct quad *qd = &upright[order[i]];
        for(int k = 0; k < 4; k++) v[k] = view(qd->p[k], sa, ca);
        n += draw_quad(fb, clip, v, &tex[qd->tex]);
    }
    *tris = 2 * 5 + (mesh_obj ? 6 : 0);
    return n;
}

/* A stand-in for the game's textures, in the same format: checkerboard, stripes, an
   ellipse and bars with see-through texels, noise. For the simulation and a Pocket without
   textures.bin; the palette is the test palette of docs/soc.md. */
static uint8_t synth_texel(int k, int u, int v, uint32_t *xs)
{
    switch(k) {
    case 0: return ((u + v) & 63) < 6 ? 120 : ((u >> 5) ^ (v >> 5)) & 1 ? 30 : 220;
    case 1: return u % 25 < 3 ? 250 : (uint8_t)(60 + ((v >> 3) & 7) * 20);
    case 2: {
        int du = 2 * u - 121, dv = 2 * v - 43;
        return du * du * 43 * 43 + dv * dv * 121 * 121 < 121 * 121 * 43 * 43 ? (uint8_t)(150 + (u >> 4)) : 0;
    }
    case 3: return (v >= 4 && v < 9) || (v >= 20 && v < 25) ? 200 : u % 64 < 6 ? 180 : 0;
    default:
        *xs ^= *xs << 13;
        *xs ^= *xs >> 17;
        *xs ^= *xs << 5;
        return (uint8_t)(100 + (*xs & 15));
    }
}

void texdemo_synth(volatile uint8_t *file)
{
    static const uint16_t size[N_TEX][3] = {     /* width, height, routine */
        { 254, 254, 16 }, { 200, 100, 16 }, { 122, 44, 64 }, { 254, 32, 64 }, { 254, 254, 16 },
    };
    volatile uint32_t *w = (volatile uint32_t *)file;
    uint32_t off = 0x800, xs = 0x2545F491u;
    w[0] = 0x54524353u;                         /* "SCRT" */
    w[1] = N_TEX;
    for(int k = 0; k < N_TEX; k++) {
        w[2 + 4 * k] = size[k][2];
        w[3 + 4 * k] = size[k][0];
        w[4 + 4 * k] = size[k][1];
        w[5 + 4 * k] = off;
        for(int v = 0; v < size[k][1]; v++)
            for(int u = 0; u < 256; u += 4) {
                uint32_t word = 0;
                for(int b = 0; b < 4; b++)
                    word |= (uint32_t)(u + b < size[k][0] ? synth_texel(k, u + b, v, &xs) : 0) << (8 * b);
                w[(off + (uint32_t)v * 256 + (uint32_t)u) / 4] = word;
            }
        off += 256u * size[k][1];
    }
    for(int i = 0; i < 256; i++)
        w[0x100 + i] = (uint32_t)i << 16 | (uint32_t)(255 - i) << 8 | (uint32_t)((i * 3) & 255);
}
