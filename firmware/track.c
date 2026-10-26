/*
 * Track flythrough (track.h). The file (tools/make_track.py): "SCRK", version, then
 * MAPn.MLB and MAPn.UNR unchanged, the palette, the placements and the camera path.
 *
 * The game's structures (openscreamer docs/NOTES.md, "How the track is built"):
 *   UNR    int32 blob size, int32 count, count offsets into the blob; an object's block at
 *          object + [object + 0x18]; block +0x10 vertex count, +0x1C vertex array and
 *          +0x24 texture coordinates (both from the object, 6 bytes a vertex: int16 x, y up,
 *          z and int16 u, v, 0), +0x28..+0x44 eight face rows (from the block, uint16 face
 *          offsets, 0xFFFF ends a row)
 *   face   +0x02 colour (routine 128), +0x04 material (byte offset into MLB), +0x06 corners
 *          (3 or 4), +0x08 corner vertex byte offsets, +0x14/+0x18/+0x1C normal (16384 = 1),
 *          pointing away from the side it is seen from: seen when (corner - eye) . N >= 0
 *   MLB    material +0 routine (16 texture, 64 texel 0 see-through, 128 one colour, 224 see-
 *          through through MAPn.SMG), +4 texture record, +8/+12 size in texels; texture
 *          record +8 the pixels of frame 0, rows of 256 bytes; texel = raw u, v * size / 256
 *   piece  world = (X + x cos a - z sin a, Y + x sin a + z cos a, height + y), a = T * 2 pi / 2048
 * A block's eight rows are its faces back to front, one row per 45-degree sector of the
 * eye's direction around the vertical axis, starting at local +z and turning towards +x
 * (found from the data: each row leaves out the faces seen only from elsewhere).
 */
#include "track.h"
#include "crc32.h"

#define BLOCK       0x40000     /* bytes per CRC block (tools/make_track.py) */
#define MAX_PIECES  2048        /* placements; track 1 has 809 (L set), 1164 (H set) */
#define MAX_OBJ     512         /* meshes; up to 360 on a track */

/* How far the game draws (openscreamer docs/NOTES.md, DETAIL): a piece only while its
   position is within 52000 units of the camera (0x83164: dist^2 >> 2 < 676000000), and
   every face cut at a depth set by the DETAIL option (the table at 0x21d4) */
#define REACH2      2704000000.0f
static const float far_depth[3] = { 27808.0f, 27808.0f, 43808.0f };     /* LOW, MID, HI */
/* the original's lens (openscreamer docs/NOTES.md, section 10): screen = centre + 128 x /
   depth, the eye line 65 % down the view; the near plane at depth 128 */
#define NEAR        128.0f
#define FOCAL       128.0f
#define EYE_LINE    0.65f
#define CAM_UP      700.0f      /* camera over the player's line */
#define LOOK        12          /* path points ahead for the direction of view */

struct piece { int32_t obj, x, y, h, t; };
struct pt { int32_t x, y, h; };

static const uint8_t *file_start, *mlb, *unr_blob;
static uint32_t file_size;
static int nblocks;
static const int32_t *unr_off;
static int unr_count;
static const struct piece *pieces;
static int npieces;
static const struct pt *path;
static int npath;
static const uint32_t *pal;
static float sin_q[513];                /* sin(2 pi i / 2048) for i = 0..512 */
static float far_z = 43808.0f;          /* the far plane of the DETAIL level */
static float radius[MAX_OBJ];           /* of each mesh around its origin */

int track_mode;
void (*track_hw_piece)(const struct gu_piece *p);

#ifdef __riscv
static inline uint32_t now(void)
{
    uint32_t c;
    __asm__ volatile ("csrr %0, cycle" : "=r"(c));
    return c;
}
#else
static inline uint32_t now(void) { return 0; }
#endif

static int32_t rd32(const uint8_t *p) { return *(const int32_t *)p; }
static int16_t rd16(const uint8_t *p) { return *(const int16_t *)p; }

static float sin2048(uint32_t a)
{
    a &= 2047;
    float s = sin_q[a & 512 ? 512 - (a & 511) : a & 511];
    return a & 1024 ? -s : s;
}

static const uint8_t *block_of(int k, const uint8_t **obj)
{
    *obj = unr_blob + unr_off[k];
    return *obj + rd32(*obj + 0x18);
}

int track_open(const volatile uint8_t *file)
{
    const uint8_t *f = (const uint8_t *)file;
    if(rd32(f) != 0x4B524353 || rd32(f + 4) != 2) return 0;    /* "SCRK", version 2 */
    file_start = f;
    file_size = (uint32_t)rd32(f + 0x2C);
    nblocks = rd32(f + 0x30);
    if(nblocks < 1 || 0x34 + 4 * nblocks > 0x100) return 0;
    return 1;
}

int track_init(void)
{
    const uint8_t *f = file_start;
    mlb = f + rd32(f + 0x08);
    const uint8_t *unr = f + rd32(f + 0x10);
    pal = (const uint32_t *)(f + rd32(f + 0x18));
    pieces = (const struct piece *)(f + rd32(f + 0x1C));
    npieces = rd32(f + 0x20);
    path = (const struct pt *)(f + rd32(f + 0x24));
    npath = rd32(f + 0x28);
    unr_count = rd32(unr + 4);
    if(unr_count > MAX_OBJ || npieces > MAX_PIECES || npath < LOOK + 2) return 0;
    unr_off = (const int32_t *)(unr + 8);
    unr_blob = unr + 8 + 4 * unr_count;

    const float dc = 0.99999529380957617f, ds = 0.0030679567629659761f;  /* cos, sin of 2 pi / 2048 */
    float c = 1.0f, s = 0.0f;
    for(int i = 0; i <= 512; i++) {
        sin_q[i] = s;
        float t = c * dc - s * ds;
        s = s * dc + c * ds;
        c = t;
    }

    for(int k = 0; k < unr_count; k++) {
        const uint8_t *obj, *blk = block_of(k, &obj), *v = obj + rd32(blk + 0x1C);
        int n = rd32(blk + 0x10);
        float r2 = 0.0f;
        for(int i = 0; i < n && i < GU_MAX_VERT; i++, v += 6) {
            float x = rd16(v), y = rd16(v + 2), z = rd16(v + 4), d = x * x + y * y + z * z;
            if(d > r2) r2 = d;
        }
        radius[k] = __builtin_sqrtf(r2);
    }
    return npieces;
}

const uint32_t *track_palette(void)
{
    return pal;
}

const uint8_t *track_mlb(void)
{
    return mlb;
}

uint32_t track_path_length(void)
{
    return (uint32_t)npath;
}

void track_detail(int level)
{
    far_z = far_depth[level < 0 ? 0 : level > 2 ? 2 : level];
}

uint32_t track_size(void)
{
    return file_size;
}

int track_blocks(void)
{
    return nblocks;
}

int track_block_ok(int k)
{
    uint32_t o = 0x100 + (uint32_t)k * BLOCK, e = o + BLOCK < file_size ? o + BLOCK : file_size;
    return o < e && crc32(file_start + o, file_start + e) == (uint32_t)rd32(file_start + 0x34 + 4 * k);
}

void track_view(const struct rs_clip *clip, struct su_view *v)
{
    v->cx = (clip->x0 + clip->x1) * 8;      /* the centre in 1/16 pixel */
    v->cy = clip->y0 * 16 + (int32_t)((float)((clip->y1 - clip->y0) * 16) * EYE_LINE);
    v->scale = (uint32_t)(FOCAL * 16.0f);
    v->far8 = (int32_t)(far_z * 256.0f);
}

struct vis { float d2; int i; };
static struct vis vis[MAX_PIECES];

void track_draw(volatile uint8_t *fb, const struct rs_clip *clip, uint32_t pos, struct track_stats *st)
{
    uint32_t t_start = now();
    st->pieces = st->faces = st->triangles = st->see_through = st->vertices = 0;

    /* the camera on the player's line, looking LOOK points ahead and a little down */
    uint32_t i0 = (pos >> 8) % (uint32_t)npath, i1 = (i0 + 1) % (uint32_t)npath;
    uint32_t il = (i0 + LOOK) % (uint32_t)npath;
    float fr = (float)(pos & 255) / 256.0f;
    float cx = path[i0].x + (path[i1].x - path[i0].x) * fr;
    float cy = path[i0].y + (path[i1].y - path[i0].y) * fr;
    float ch = path[i0].h + (path[i1].h - path[i0].h) * fr + CAM_UP;
    float fx = path[il].x - cx, fy = path[il].y - cy, fh = path[il].h + CAM_UP - 250.0f - ch;
    float l = __builtin_sqrtf(fx * fx + fy * fy + fh * fh);
    fx /= l; fy /= l; fh /= l;
    float rx = fy, ry = -fx;                    /* right = forward x up, level */
    l = __builtin_sqrtf(rx * rx + ry * ry);
    rx /= l; ry /= l;
    float ux = ry * fh, uy = -rx * fh, uh = rx * fy - ry * fx;   /* up = right x forward */
    st->cam[0] = (int32_t)cx;
    st->cam[1] = (int32_t)cy;
    st->cam[2] = (int32_t)ch;

    /* the pieces in reach and in view, far to near */
    const float half_w = (float)(clip->x1 - clip->x0) / 2.0f / FOCAL;
    const float up = (float)(clip->y1 - clip->y0) * EYE_LINE / FOCAL;          /* above the eye line */
    const float down = (float)(clip->y1 - clip->y0) * (1.0f - EYE_LINE) / FOCAL;
    int nv = 0;
    float nearest = 1e30f;
    for(int i = 0; i < npieces; i++) {
        const struct piece *pc = &pieces[i];
        if(pc->obj < 0 || pc->obj >= unr_count) continue;
        float dx = pc->x - cx, dy = pc->y - cy, dh = pc->h - ch, rad = radius[pc->obj];
        float d2 = dx * dx + dy * dy + dh * dh;
        if(d2 < nearest) nearest = d2;
        if(d2 >= REACH2) continue;
        float f = dx * fx + dy * fy + dh * fh, r = dx * rx + dy * ry, u = dx * ux + dy * uy + dh * uh;
        if(f + rad < NEAR || f - rad > far_z) continue;
        if(r - rad > half_w * (f + rad) || r + rad < -half_w * (f + rad)) continue;
        if(u - rad > up * (f + rad) || u + rad < -down * (f + rad)) continue;
        vis[nv].d2 = d2;
        vis[nv].i = i;
        nv++;
    }
    st->nearest = nearest < 4e18f ? (uint32_t)__builtin_sqrtf(nearest) : 0xFFFFFFFFu;
    for(int gap = nv / 2; gap > 0; gap /= 2)    /* Shell sort, far first */
        for(int i = gap; i < nv; i++) {
            struct vis t = vis[i];
            int j = i;
            for(; j >= gap && vis[j - gap].d2 < t.d2; j -= gap) vis[j] = vis[j - gap];
            vis[j] = t;
        }
    uint32_t t_pieces = now();
    st->cyc_cull = t_pieces - t_start;

    struct su_view view;
    struct gu_stats gst = { 0, 0, 0, 0 };
    track_view(clip, &view);
    uint32_t t_out = 0;
    for(int n = 0; n < nv; n++) {
        const struct piece *pc = &pieces[vis[n].i];
        struct gu_piece g;
        g.blk = block_of(pc->obj, &g.obj);
        float ca = sin2048((uint32_t)pc->t + 512), sa = sin2048((uint32_t)pc->t);
        float dx = pc->x - cx, dy = pc->y - cy, dh = pc->h - ch;

        /* local to view: per axis A, A . D + x (Ax cos + Ay sin) + y Ah + z (Ay cos - Ax sin) */
        const float m[9] = { rx * ca + ry * sa, 0.0f, ry * ca - rx * sa,
                             ux * ca + uy * sa, uh, uy * ca - ux * sa,
                             fx * ca + fy * sa, fh, fy * ca - fx * sa };
        const float o[3] = { dx * rx + dy * ry, dx * ux + dy * uy + dh * uh, dx * fx + dy * fy + dh * fh };
        for(int k = 0; k < 9; k++) g.m[k] = (int32_t)(m[k] * 65536.0f);
        for(int k = 0; k < 3; k++) g.t[k] = (int32_t)(o[k] * 256.0f);

        /* the eye in the piece's frame picks the row and tells the faces turned away */
        float ex = -dx * ca - dy * sa, ez = dx * sa - dy * ca, ey = -dh;
        g.row = ex >= 0.0f ? (ez >= 0.0f ? (ex < ez ? 0 : 1) : (ex > -ez ? 2 : 3))
                           : (ez < 0.0f ? (-ex < -ez ? 4 : 5) : (-ex > ez ? 6 : 7));
        g.e[0] = (int32_t)(ex * 256.0f);
        g.e[1] = (int32_t)(ey * 256.0f);
        g.e[2] = (int32_t)(ez * 256.0f);
        st->pieces++;
        uint32_t t0 = now();
        if(track_mode == TRACK_HW) track_hw_piece(&g);
        else gu_draw(fb, clip, &view, mlb, &g, &gst);
        t_out += now() - t0;
    }
    st->faces = gst.faces;
    st->triangles = gst.triangles;
    st->see_through = gst.see_through;
    st->vertices = gst.vertices;
    st->cyc_out = t_out;
    st->cyc_piece = now() - t_pieces - t_out;
}
