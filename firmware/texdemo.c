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
static float sin_t[256];                /* sin(2 pi i / 256) */

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
    for(int i = 0; i < 256; i++) {
        sin_t[i] = s;
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
    return rs_textured(fb, clip, t0, t) + rs_textured(fb, clip, t1, t);
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
    float sa = sin_t[a & 255], ca = sin_t[(a + 64) & 255];
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
