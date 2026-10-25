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
uint32_t (*texdemo_hw)(const struct rs_vertex pos[3], const struct rs_tex_setup *s, const struct rs_texture *t);

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

static uint32_t draw_quad(volatile uint8_t *fb, const struct rs_clip *clip, const struct v3 v[4],
                          const struct rs_texture *t)
{
    const float f = 250.0f, cx = (float)(clip->x0 + clip->x1) / 2, cy = (float)(clip->y0 + clip->y1) / 2;
    const float u[4] = { 0.0f, (float)t->width, (float)t->width, 0.0f };
    const float w[4] = { 0.0f, 0.0f, (float)t->height, (float)t->height };
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
#ifdef TEXDEMO_FLOAT_REF                 /* only for the comparison on the PC */
    if(texdemo_mode == TEXDEMO_FLOAT)
        return rs_textured(fb, clip, t0, t) + rs_textured(fb, clip, t1, t);
#endif
    uint32_t n = 0;
    for(int k = 0; k < 2; k++) {
        const struct rs_tvertex *tv = k ? t1 : t0;
        struct rs_tex_setup ts;
        struct rs_vertex pos[3] = { { tv[0].x, tv[0].y }, { tv[1].x, tv[1].y }, { tv[2].x, tv[2].y } };
        if(!rs_tex_setup(&ts, tv)) continue;
        n += texdemo_mode == TEXDEMO_HW ? texdemo_hw(pos, &ts, t) : rs_textured_fx(fb, clip, pos, &ts, t);
    }
    return n;
}

uint32_t texdemo_draw(volatile uint8_t *fb, const struct rs_clip *clip, uint32_t a, uint32_t *tris)
{
    static const struct quad ground[2] = {
        { { { -2.0f, 0, 2.0f }, { 2.0f, 0, 2.0f }, { 2.0f, 0, -2.0f }, { -2.0f, 0, -2.0f } }, 4 },  /* asphalt */
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
    *tris = 2 * 5;
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
