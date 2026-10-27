/*
 * The track (track.h). The file (tools/make_track.py): "SCRK", version 5, then MAPn.MLB and
 * MAPn.UNR unchanged, the palette, the placements, the camera path, the ground index, the
 * markers, the cars, the car models, road points and walls, the start, the race screen, the
 * backdrop and its ground colour.
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
#include <math.h>

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
struct start { int32_t x, y; float yaw; };

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
static const uint16_t *ground_first, *ground_list;     /* first[65537], then the pieces */
static const struct track_marker *markers;
static int nmarkers;
static const struct car_spec *cars;
static int ncars;
static const struct track_model *models;
static int nmodels;
static const struct start *start;
static const uint8_t *models_at;
static const struct track_dir *dirs;
static int ndirs;
static const struct track_wall *walls;
static int nwalls;
static float anim;                      /* the animation clock, 0..1 */
static uint32_t piece_phase;            /* the pieces' texture phase: the clock (record +0x20 is -1) */
static int16_t class_key[MAX_OBJ];      /* the key's change for each mesh's class (+0x32c) */

/* What track_under() reads, made once from the drivable meshes: each mesh's faces of its
   first face row in that order, with corners (x, z), normal, first corner, a box around them
   and whether the face is convex with an area (only then a point outside the box cannot be
   over it: a flat or bent face always gets the full test, so the answers stay the same) */
struct cface { float x0, x1, z0, z1; int16_t x[4], z[4], n[3], v[3]; uint8_t nc, boxed; };
#define MAX_CFACES  8192
static struct cface cfaces[MAX_CFACES];
static int ncfaces;
static int32_t cobj_first[MAX_OBJ];     /* -1: no table (not drivable, or the table was full) */
static int16_t cobj_count[MAX_OBJ];
static uint8_t cobj_grass[MAX_OBJ];
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
    if(rd32(f) != 0x4B524353 || rd32(f + 4) != 5) return 0;    /* "SCRK", version 5 */
    file_start = f;
    file_size = (uint32_t)rd32(f + 0x80);
    nblocks = rd32(f + 0x84);
    if(nblocks < 1 || 0x88 + 4 * nblocks > 0x100) return 0;
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
    ground_first = (const uint16_t *)(f + rd32(f + 0x2C));
    ground_list = ground_first + 65537;
    markers = (const struct track_marker *)(f + rd32(f + 0x34));
    nmarkers = rd32(f + 0x38);
    cars = (const struct car_spec *)(f + rd32(f + 0x3C));
    ncars = rd32(f + 0x40);
    models_at = f + rd32(f + 0x44);
    models = (const struct track_model *)models_at;
    nmodels = rd32(f + 0x48);
    dirs = (const struct track_dir *)(f + rd32(f + 0x4C));
    ndirs = rd32(f + 0x50);
    walls = (const struct track_wall *)(f + rd32(f + 0x54));
    nwalls = rd32(f + 0x58);
    start = (const struct start *)(f + 0x5C);
    unr_count = rd32(unr + 4);
    if(unr_count > MAX_OBJ || npieces > MAX_PIECES || npath < LOOK + 2 || ncars < 1 || nmodels < 1) return 0;
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

    ncfaces = 0;
    for(int k = 0; k < unr_count; k++) cobj_first[k] = -1;
    for(int i = 0; i < ground_first[65536]; i++) {
        int k = pieces[ground_list[i]].obj;
        if(k < 0 || k >= unr_count || cobj_first[k] >= 0) continue;
        const uint8_t *obj, *blk = block_of(k, &obj), *verts = obj + rd32(blk + 0x1C);
        int nface = rd32(blk + 0x14), ro = rd32(blk + 0x28), first = ncfaces;
        if(ro <= 0 || ncfaces + nface > MAX_CFACES) continue;
        for(int j = 0; j < nface; j++) {
            uint16_t e = *(const uint16_t *)(blk + ro + 2 * j);
            if(e == 0xFFFF) continue;                   /* an end mark, no face */
            const uint8_t *fc = blk + e;
            struct cface *q = &cfaces[ncfaces++];
            int nc = rd16(fc + 6);
            q->nc = (uint8_t)(nc < 3 ? 3 : nc > 4 ? 4 : nc);
            for(int c = 0; c < q->nc; c++) {
                int vo = rd16(fc + 8 + 2 * c);
                if(vo < 0) { q->nc = 0; break; }       /* a corner that is none: never over it */
                q->x[c] = rd16(verts + vo);
                q->z[c] = rd16(verts + vo + 4);
                if(c == 0) q->v[0] = q->x[0], q->v[1] = rd16(verts + vo + 2), q->v[2] = q->z[0];
            }
            for(int c = 0; c < 3; c++) q->n[c] = rd16(fc + 0x14 + 4 * c);
            q->boxed = 0;
            if(!q->nc) continue;
            int64_t s = 0;
            int pos = 0, neg = 0;
            q->x0 = q->x1 = q->x[0];
            q->z0 = q->z1 = q->z[0];
            for(int c = 0; c < q->nc; c++) {
                int a = c, b = (c + 1) % q->nc, d = (c + 2) % q->nc;
                int64_t t = (int64_t)(q->x[b] - q->x[a]) * (q->z[d] - q->z[b]) - (int64_t)(q->z[b] - q->z[a]) * (q->x[d] - q->x[b]);
                pos += t > 0; neg += t < 0;
                s += (int64_t)q->x[a] * q->z[b] - (int64_t)q->x[b] * q->z[a];
                if(q->x[c] < q->x0) q->x0 = q->x[c];
                if(q->x[c] > q->x1) q->x1 = q->x[c];
                if(q->z[c] < q->z0) q->z0 = q->z[c];
                if(q->z[c] > q->z1) q->z1 = q->z[c];
            }
            q->boxed = s != 0 && !(pos && neg) && pos + neg == q->nc;
        }
        cobj_first[k] = first;
        cobj_count[k] = (int16_t)(ncfaces - first);
        cobj_grass[k] = blk[0x6b] == 'e';
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
        int32_t cl = rd32(obj + 0x32C);                 /* 0x8a8d7 */
        class_key[k] = cl == 3 ? -6000 : cl == 1 ? -1500 : cl == 2 ? -800 : cl == 4 ? -200 : 0;
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

void track_path_point(uint32_t i, float *x, float *y, float *h)
{
    const struct pt *p = &path[i % (uint32_t)npath];
    *x = (float)p->x;
    *y = (float)p->y;
    *h = (float)p->h;
}

const struct track_marker *track_markers(int *n)
{
    *n = nmarkers;
    return markers;
}

const struct car_spec *track_cars(int *n)
{
    *n = ncars;
    return cars;
}

const struct track_model *track_models(int *n)
{
    *n = nmodels;
    return models;
}

const uint8_t *track_model_mesh(int k)
{
    return models_at + models[k].mesh;
}

const struct track_dir *track_dirs(int *n)
{
    *n = ndirs;
    return dirs;
}

const struct track_wall *track_walls(int *n)
{
    *n = nwalls;
    return walls;
}

void track_clock(uint32_t steps)
{
    anim += (float)steps * 0.02f * 0.3f;
    while(anim >= 1.0f) anim -= 1.0f;
    piece_phase = (uint32_t)(anim * 65535.0f) & 0x3FFF;
}

int track_bonus_time(void)
{
    return rd32(file_start + 0x70);
}

int track_camera_hi(void)
{
    return rd32(file_start + 0x74);
}

const uint8_t *track_backdrop(void)
{
    return file_start + rd32(file_start + 0x78);
}

int track_ground_colour(void)
{
    return rd32(file_start + 0x7C) & 255;
}

/*
 * The backdrop as the game copies it (0x83a68): the picture's column (2047 - heading) / 2
 * mod 512 under the view's left edge (heading in the game's units, 2048 a turn, = yaw + a
 * quarter turn: twice round the picture a turn), its top row at the eye line + 22 - 256 +
 * 128 sin(pitch), taken from row |top| on (a top below the view's top in every race), cut at
 * the view's bottom; each row read on past its end as the game does; the rest of the view
 * in the ground colour. Upright whatever the camera's roll: the game's leaning version
 * (0x83b44) is never called.
 */
void track_backdrop_blit(struct blit_list *l, const struct rs_clip *clip, const struct track_cam *cam)
{
    int w = clip->x1 - clip->x0, h = clip->y1 - clip->y0, eye = (int)((float)h * EYE_LINE);
    int32_t s = (int32_t)(sinf(cam->pitch) * 16384.0f);         /* the game's sine, [0x7228c] */
    int top = eye + 22 - 256 + ((-54152 * s) >> 7) / -54152;
    int heading = (int)lrintf((cam->yaw + 1.5707964f) * (1024.0f / 3.14159265f));
    int col = ((2047 - heading) >> 1) & 511;
    int rows = 256 + (top < 0 ? top : 0);
    if(top + 256 > h) rows -= top + 256 - h;
    if(rows > 0)
        blit_copy(l, track_backdrop() + (top < 0 ? -top : top) * 512 + col, w, rows, 512, clip->x0, clip->y0,
                  BLIT_COPY | BLIT_OPAQUE, 0);
    else
        rows = 0;
    if(rows < h) blit_fill(l, clip->x0, clip->y0 + rows, w, h - rows, track_ground_colour());
}

const uint8_t *track_hud(void)
{
    return file_start + rd32(file_start + 0x68);
}

const struct blit_image *track_hud_images(int *n)
{
    const uint8_t *h = track_hud();
    *n = rd32(h);
    return (const struct blit_image *)(h + 8);
}

const struct blit_font *track_hud_fonts(int *n)
{
    const uint8_t *h = track_hud();
    *n = rd32(h + 4);
    return (const struct blit_font *)(h + 8 + 12 * rd32(h));
}

void track_hud_picture(int k, int *first, int *count)
{
    const uint8_t *h = track_hud(), *p = h + 8 + 12 * rd32(h) + 20 * rd32(h + 4) + 8 * k;
    *first = rd32(p);
    *count = rd32(p + 4);
}

const char *track_hud_text(int k)
{
    const uint8_t *h = track_hud();
    return (const char *)(h + 8 + 12 * rd32(h) + 20 * rd32(h + 4) + 16 + 32 * k);
}

void track_start(float *x, float *y, float *yaw)
{
    *x = (float)start->x;
    *y = (float)start->y;
    *yaw = start->yaw;
}

int track_under(float x, float y, float *h, int *grass)
{
    static const int8_t cell[9][2] = { {0,0}, {1,0}, {-1,0}, {0,1}, {0,-1}, {1,1}, {-1,-1}, {1,-1}, {-1,1} };
    int cx = ((int)x + 0x100000) >> 13, cy = ((int)y + 0x100000) >> 13;
    for(int ci = 0; ci < 9; ci++) {
        int kx = cx + cell[ci][0], ky = cy + cell[ci][1];
        if(kx < 0 || kx > 255 || ky < 0 || ky > 255) continue;
        for(int at = ground_first[ky * 256 + kx]; at < ground_first[ky * 256 + kx + 1]; at++) {
            const struct piece *pc = &pieces[ground_list[at]];
            if(pc->obj < 0 || pc->obj >= unr_count) continue;
            if(cobj_first[pc->obj] < 0) continue;
            const uint8_t *obj, *blk = block_of(pc->obj, &obj);
            float dx = x - (float)pc->x, dy = y - (float)pc->y;
            float r = (float)rd32(blk), bx = dx - (float)rd16(blk + 4), by = dy - (float)rd16(blk + 8);
            if(bx * bx + by * by > r * r) continue;
            float c = sin2048((uint32_t)pc->t + 512), s = sin2048((uint32_t)pc->t);
            float lx = dx * c + dy * s, lz = -dx * s + dy * c;  /* into the piece's frame */
            const struct cface *f0 = &cfaces[cobj_first[pc->obj]], *fe = f0 + cobj_count[pc->obj];
            for(int tries = 0; tries < 3; tries++, lx += 10.0f, lz += 10.0f)
                for(const struct cface *q = f0; q < fe; q++) {
                    if(q->boxed && (lx < q->x0 || lx > q->x1 || lz < q->z0 || lz > q->z1)) continue;
                    int nc = q->nc, pos = 0, neg = 0;
                    if(!nc) continue;
                    for(int j = 0; j < nc; j++) {
                        int b = j + 1 < nc ? j + 1 : 0;
                        float ax = q->x[j], az = q->z[j];
                        float cr = (q->x[b] - ax) * (lz - az) - (q->z[b] - az) * (lx - ax);
                        if(cr > 0.0f) pos++; else if(cr < 0.0f) neg++;
                    }
                    if(pos && neg) continue;
                    if(grass) *grass = cobj_grass[pc->obj];
                    if(!h) return 1;
                    if(q->n[1] == 0) continue;
                    int num = ((int)lx - q->v[0]) * q->n[0] + ((int)lz - q->v[2]) * q->n[2];
                    *h = (float)pc->h + (float)q->v[1] + (float)(-num / q->n[1]);
                    return 1;
                }
        }
    }
    return 0;
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
    return o < e && crc32(file_start + o, file_start + e) == (uint32_t)rd32(file_start + 0x88 + 4 * k);
}

void track_view(const struct rs_clip *clip, struct su_view *v)
{
    v->cx = (clip->x0 + clip->x1) * 8 - 16; /* the centre in 1/16 pixel: column 159 on the screen,
                                               as the game's projection (0x83431) */
    v->cy = clip->y0 * 16 + (int32_t)((float)((clip->y1 - clip->y0) * 16) * EYE_LINE);
    v->scale = (uint32_t)(FOCAL * 16.0f);
    v->far8 = (int32_t)(far_z * 256.0f);
}

struct vis { int32_t k; int i; };              /* key << 16 | i; i: a piece, or npieces + an object */
#define MAX_OBJS    8
static struct vis vis[MAX_PIECES + MAX_OBJS];

/* A block placed at o with m (world from local) as a piece for the geometry unit */
static void block_piece(const float c[3], const float f[3], const float r[3], const float u[3],
                        const float m[9], const float o[3], struct gu_piece *g)
{
    const float *basis[3] = { r, u, f };
    for(int k = 0; k < 3; k++) {
        for(int j = 0; j < 3; j++)
            g->m[3 * k + j] = (int32_t)((basis[k][0] * m[j] + basis[k][1] * m[3 + j] + basis[k][2] * m[6 + j]) * 65536.0f);
        g->t[k] = (int32_t)((basis[k][0] * (o[0] - c[0]) + basis[k][1] * (o[1] - c[1]) + basis[k][2] * (o[2] - c[2])) * 256.0f);
    }
    float e[3];
    for(int j = 0; j < 3; j++) e[j] = m[j] * (c[0] - o[0]) + m[3 + j] * (c[1] - o[1]) + m[6 + j] * (c[2] - o[2]);
    float ex = e[0], ez = e[2];
    g->row = ex >= 0.0f ? (ez >= 0.0f ? (ex < ez ? 0 : 1) : (ex > -ez ? 2 : 3))
                        : (ez < 0.0f ? (-ex < -ez ? 4 : 5) : (-ex > ez ? 6 : 7));
    for(int j = 0; j < 3; j++) g->e[j] = (int32_t)(e[j] * 256.0f);
}

/* The view from the point c along forward f, with right r and up u (unit vectors) */
static void draw(volatile uint8_t *fb, const struct rs_clip *clip, const float c[3], const float f[3],
                 const float r[3], const float u[3], const struct track_object *objs, int nobj,
                 struct track_stats *st)
{
    uint32_t t_start = now();
    st->pieces = st->faces = st->triangles = st->see_through = st->vertices = 0;
    const float cx = c[0], cy = c[1], ch = c[2];
    const float fx = f[0], fy = f[1], fh = f[2], rx = r[0], ry = r[1], rh = r[2];
    const float ux = u[0], uy = u[1], uh = u[2];
    st->cam[0] = (int32_t)cx;
    st->cam[1] = (int32_t)cy;
    st->cam[2] = (int32_t)ch;

    /* the pieces in reach and in view, far to near */
    const float half_w = (float)(clip->x1 - clip->x0) / 2.0f / FOCAL;
    const float up = (float)(clip->y1 - clip->y0) * EYE_LINE / FOCAL;          /* above the eye line */
    const float down = (float)(clip->y1 - clip->y0) * (1.0f - EYE_LINE) / FOCAL;
    int nv = 0;
    float nearest = 1e30f;
    /* the game's key (0x8a7b0): -(sqrt(dist^2 >> 2), rounded, >> 2) */
#define KEY(d2) (-((int32_t)lrintf(sqrtf((d2) * 0.25f)) >> 2))
    for(int i = 0; i < npieces; i++) {
        const struct piece *pc = &pieces[i];
        if(pc->obj < 0 || pc->obj >= unr_count) continue;
        float dx = pc->x - cx, dy = pc->y - cy, dh = pc->h - ch, rad = radius[pc->obj];
        float d2 = dx * dx + dy * dy + dh * dh;
        if(d2 < nearest) nearest = d2;
        if(d2 >= REACH2) continue;
        float vf = dx * fx + dy * fy + dh * fh, vr = dx * rx + dy * ry + dh * rh;
        float vu = dx * ux + dy * uy + dh * uh;
        if(vf + rad < NEAR || vf - rad > far_z) continue;
        if(vr - rad > half_w * (vf + rad) || vr + rad < -half_w * (vf + rad)) continue;
        if(vu - rad > up * (vf + rad) || vu + rad < -down * (vf + rad)) continue;
        vis[nv].k = (KEY(d2) + class_key[pc->obj]) * 65536 + i;
        vis[nv].i = i;
        nv++;
    }
    for(int k = 0; k < nobj && k < MAX_OBJS; k++) {
        const struct track_object *ob = &objs[k];
        float dx = ob->x - cx, dy = ob->y - cy, dh = ob->h - ch, rad = ob->radius;
        float vf = dx * fx + dy * fy + dh * fh, vr = dx * rx + dy * ry + dh * rh;
        float vu = dx * ux + dy * uy + dh * uh;
        if(vf + rad < NEAR || vf - rad > far_z) continue;
        if(vr - rad > half_w * (vf + rad) || vr + rad < -half_w * (vf + rad)) continue;
        if(vu - rad > up * (vf + rad) || vu + rad < -down * (vf + rad)) continue;
        float d2 = dx * dx + dy * dy + dh * dh;
        vis[nv].k = (KEY(d2) + 800) * 65536 + npieces + k;     /* 0x8a8c3: an object counts nearer */
        vis[nv].i = npieces + k;
        nv++;
    }
    st->nearest = nearest < 4e18f ? (uint32_t)__builtin_sqrtf(nearest) : 0xFFFFFFFFu;
    for(int gap = nv / 2; gap > 0; gap /= 2)    /* Shell sort by key and order: far first */
        for(int i = gap; i < nv; i++) {
            struct vis t = vis[i];
            int j = i;
            for(; j >= gap && vis[j - gap].k > t.k; j -= gap) vis[j] = vis[j - gap];
            vis[j] = t;
        }
    uint32_t t_pieces = now();
    st->cyc_cull = t_pieces - t_start;

    struct su_view view;
    struct gu_stats gst = { 0, 0, 0, 0 };
    track_view(clip, &view);
    uint32_t t_out = 0;
    for(int n = 0; n < nv; n++) {
        if(vis[n].i >= npieces) {                   /* an object: its blocks as they come */
            const struct track_object *ob = &objs[vis[n].i - npieces];
            for(int b = 0; b < ob->n; b++) {
                struct gu_piece g;
                block_piece(c, f, r, u, ob->b[b].m, ob->b[b].o, &g);
                g.obj = ob->b[b].file;
                g.blk = ob->b[b].blk;
                g.phase = ob->b[b].phase;
                st->pieces++;
                uint32_t t0 = now();
                if(track_mode == TRACK_HW) track_hw_piece(&g);
                else gu_draw(fb, clip, &view, mlb, &g, &gst);
                t_out += now() - t0;
            }
            continue;
        }
        const struct piece *pc = &pieces[vis[n].i];
        struct gu_piece g;
        g.phase = piece_phase;
        g.blk = block_of(pc->obj, &g.obj);
        float ca = sin2048((uint32_t)pc->t + 512), sa = sin2048((uint32_t)pc->t);
        float dx = pc->x - cx, dy = pc->y - cy, dh = pc->h - ch;

        /* local to view: per axis A, A . D + x (Ax cos + Ay sin) + y Ah + z (Ay cos - Ax sin) */
        const float m[9] = { rx * ca + ry * sa, rh, ry * ca - rx * sa,
                             ux * ca + uy * sa, uh, uy * ca - ux * sa,
                             fx * ca + fy * sa, fh, fy * ca - fx * sa };
        const float o[3] = { dx * rx + dy * ry + dh * rh, dx * ux + dy * uy + dh * uh, dx * fx + dy * fy + dh * fh };
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

void track_draw_cam(volatile uint8_t *fb, const struct rs_clip *clip, const struct track_cam *cam,
                    const struct track_object *objs, int nobj, struct track_stats *st)
{
    float sy = sinf(cam->yaw), cy = cosf(cam->yaw), sp = sinf(cam->pitch), cp = cosf(cam->pitch);
    float sr = sinf(cam->roll), cr = cosf(cam->roll);
    const float c[3] = { cam->x, cam->y, cam->h };
    const float f[3] = { sy * cp, -cy * cp, sp };
    /* level right = forward x up, level up = right x forward, then the roll about forward */
    const float r0[3] = { -cy, -sy, 0.0f };
    const float u0[3] = { r0[1] * f[2], -r0[0] * f[2], r0[0] * f[1] - r0[1] * f[0] };
    const float r[3] = { r0[0] * cr + u0[0] * sr, r0[1] * cr + u0[1] * sr, u0[2] * sr };
    const float u[3] = { u0[0] * cr - r0[0] * sr, u0[1] * cr - r0[1] * sr, u0[2] * cr };
    draw(fb, clip, c, f, r, u, objs, nobj, st);
}

void track_draw(volatile uint8_t *fb, const struct rs_clip *clip, uint32_t pos, struct track_stats *st)
{
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
    const float c[3] = { cx, cy, ch }, f[3] = { fx, fy, fh }, r[3] = { rx, ry, 0.0f };
    const float u[3] = { ry * fh, -rx * fh, rx * fy - ry * fx };     /* up = right x forward */
    draw(fb, clip, c, f, r, u, 0, 0, st);
}
