/*
 * Geometry unit C model (geom.h). Every >> of a signed value rounds down, as in the
 * hardware.
 */
#include "geom.h"

static int32_t rd32(const uint8_t *p) { return *(const int32_t *)p; }
static int16_t rd16(const uint8_t *p) { return *(const int16_t *)p; }
static uint16_t ru16(const uint8_t *p) { return *(const uint16_t *)p; }

static int32_t clamp_size(int32_t v)
{
    return v < 1 ? 1 : v > 256 ? 256 : v;
}

uint32_t gu_draw(volatile uint8_t *fb, const struct rs_clip *clip, const struct su_view *v,
                 const uint8_t *mlb, const struct gu_piece *p, struct gu_stats *st)
{
    uint32_t pixels = 0;
    static struct su_fixed vt[GU_MAX_VERT];     /* the vertices in view space */
    const uint8_t *verts = p->obj + rd32(p->blk + 0x1C), *uvs = p->obj + rd32(p->blk + 0x24);
    int32_t nvert = rd32(p->blk + 0x10), ro = rd32(p->blk + 0x28 + 4 * p->row);
    if(nvert < 0 || nvert > GU_MAX_VERT || ro <= 0) return 0;

    for(int i = 0; i < nvert; i++) {
        int64_t x = rd16(verts + 6 * i), y = rd16(verts + 6 * i + 2), z = rd16(verts + 6 * i + 4);
        for(int k = 0; k < 3; k++) {
            int64_t s = x * p->m[3 * k] + y * p->m[3 * k + 1] + z * p->m[3 * k + 2];
            int32_t w = (int32_t)(p->t[k] + (s >> 8));
            if(k == 0) vt[i].r = w;
            else if(k == 1) vt[i].u = w;
            else vt[i].f = w;
        }
    }
    st->vertices += (uint32_t)nvert;

    const uint8_t *e = p->blk + ro;
    for(int nf = 0; nf < GU_MAX_FACES; nf++, e += 2) {
        uint16_t fo = ru16(e);
        if(fo == 0xFFFF) break;
        const uint8_t *fc = p->blk + fo;
        int nc = rd16(fc + 6);
        if(nc < 3 || nc > 4) continue;

        /* turned away? */
        const uint8_t *c0 = verts + ru16(fc + 8);
        int64_t bx = ((int64_t)rd16(c0) << 8) - p->e[0], by = ((int64_t)rd16(c0 + 2) << 8) - p->e[1];
        int64_t bz = ((int64_t)rd16(c0 + 4) << 8) - p->e[2];
        if(bx * rd16(fc + 0x14) + by * rd16(fc + 0x18) + bz * rd16(fc + 0x1C) < 0) continue;

        /* the material */
        const uint8_t *mat = mlb + rd16(fc + 4);
        int32_t routine = rd32(mat), tw = 0, th = 0;
        if(routine == 224) {
            st->see_through++;
            continue;
        }
        struct rs_texture tex;
        if(routine != 128) {
            tw = clamp_size(rd32(mat + 8));
            th = clamp_size(rd32(mat + 12));
            tex.data = mlb + rd32(mlb + rd32(mat + 4) + 8);
            tex.width = tw;
            tex.height = th;
            tex.routine = routine == 64 ? 64 : 16;
        }

        /* the corners */
        struct su_fixed c[4];
        int ok = 1;
        for(int j = 0; j < nc; j++) {
            uint16_t vo = ru16(fc + 8 + 2 * j);
            int vi = vo / 6;
            if(vi >= nvert) ok = 0;
            c[j].r = vt[vi % GU_MAX_VERT].r;
            c[j].u = vt[vi % GU_MAX_VERT].u;
            c[j].f = vt[vi % GU_MAX_VERT].f;
            int32_t s = rd16(uvs + vo) * tw, t = rd16(uvs + vo + 2) * th;
            c[j].s = s < -(1 << 23) ? -(1 << 23) : s > (1 << 23) - 1 ? (1 << 23) - 1 : s;
            c[j].t = t < -(1 << 23) ? -(1 << 23) : t > (1 << 23) - 1 ? (1 << 23) - 1 : t;
        }
        if(!ok) continue;
        st->faces++;
        st->triangles += (uint32_t)(nc - 2);
        pixels += su_polygon8(fb, clip, v, c, nc, routine == 128 ? 0 : &tex, (uint8_t)(rd16(fc + 2) & 255));
    }
    return pixels;
}
