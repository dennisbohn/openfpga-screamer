/*
 * The track (track.h). The file (tools/make_track.py): "SCRK", version 6, then MAPn.MLB and
 * MAPn.UNR unchanged, the palette, the placements, the camera path, the ground index, the
 * markers, the cars, the car models, road points and walls, the start, the race screen, the
 * backdrop and its ground colour, the opponents' curves and numbers.
 *
 * The game's structures (openscreamer docs/NOTES.md, "How the track is built"):
 *   UNR    int32 blob size, int32 count, count offsets into the blob; an object's block at
 *          object + [object + 0x18]; block +0x04 its middle (int16 x, y, z), +0x10 vertex
 *          count, +0x1C vertex array and +0x24 texture coordinates (both from the object, 6
 *          bytes a vertex: int16 x, y up, z and int16 u, v, 0), +0x28..+0x64 sixteen face
 *          rows (from the block, uint16 face offsets, 0xFFFF ends a row), +0x68 its name
 *   face   +0x02 colour (routine 128), +0x04 material (byte offset into MLB), +0x06 corners
 *          (3 or 4), +0x08 corner vertex byte offsets, +0x14/+0x18/+0x1C normal (16384 = 1),
 *          pointing away from the side it is seen from: seen when (corner - eye) . N >= 0
 *   MLB    material +0 routine (16 texture, 64 texel 0 see-through, 128 one colour, 224 see-
 *          through through MAPn.SMG), +4 texture record, +8/+12 size in texels; texture
 *          record +8 the pixels of frame 0, rows of 256 bytes; texel = raw u, v * size / 256
 *   piece  world = (X + x cos a - z sin a, Y + x sin a + z cos a, height + y), a = T * 2 pi / 2048
 * A block's sixteen rows are its faces in drawing order, back to front, each sorted for one
 * direction of the eye in the block's frame (face_eye, the game's 0x8a598): rows 0..7 for an
 * eye above the block's x-z plane, 8..15 below it, and in each half one row per 45-degree
 * sector around the block's y axis, starting at +z and turning towards +x. A row holds all
 * the faces, or nearly (the data); the normal test leaves out those turned away.
 */
#include "track.h"
#ifdef TRACK_DUMP
#include <stdio.h>
#endif
#include "crc32.h"
#include <math.h>
#include <string.h>

#define BLOCK       0x40000     /* bytes per CRC block (tools/make_track.py) */
#define MAX_PIECES  2560        /* placements (H set): track 1 has 1164, track 5 the most, 2359 */
#define MAX_OBJ     512         /* meshes; up to 360 on a track */
#define MAX_BLOCKS  8           /* blocks of a mesh drawn (track 1: two, the football helmet's) */

/* How far the game draws (openscreamer docs/NOTES.md, DETAIL): a piece only while its
   position is within 52000 units of the camera (0x83164: dist^2 >> 2 < 676000000), and
   every face cut at a depth set by the DETAIL option (the table at 0x21d4: -27808, -27808,
   -43808, the view's z from the camera, 0x8baa4) */
#define REACH2      2704000000.0f
static const float far_depth[3] = { 27808.0f, 27808.0f, 43808.0f };     /* LOW, MID, HI */
/* The original's lens: screen = centre + 128 x / (128 + z), z the depth from the camera. The
   corners' divide (0x83420) reads TABS\PERSP01.TAB (loaded by 0x4cd4c, its address patched in
   by 0x8ba28): entry z = -65536 x 128 / (128 + z); a cut at the near plane z = 0 (0xd7470)
   projects with 128 / 128 (0xd7493, ds:0x829ac = -128), one at the far plane with
   128 / (far - 128) (0xd7837, ds:0x829b0 = far - 128, 0x8bad6). So the eye is EYE_BACK
   behind the camera along the view: the near plane through the camera (depth 128 from the
   eye), the scale 128 at the eye. The camera itself stays the origin of the view's z, the
   reach, the drawing order (the places less the camera, 0x8ab8e) and the faces' eye (0x834d6:
   the camera in the block's frame, from those places). The eye line 65 % down the view, in
   whole lines (0x19cbe: 0x4c610(320, h, h x 0.65 cut by 0x4d7b4): 130 of 200, 85 of the
   cockpit's 132) */
#define NEAR        128.0f
#define FOCAL       128.0f
#define EYE_BACK    128.0f
#define EYE_LINE(h) ((h) * 65 / 100)
#define CAM_UP      700.0f      /* camera over the player's line */
#define LOOK        12          /* path points ahead for the direction of view */
#define PI          3.14159265f

/* The game's view angles (0x829e3): each cut to a 2048th of a turn (the camera record's float,
   0 .. 2 pi, times 2048 / 2 pi, cut towards 0) and its sine and cosine from the table at
   0x30190 (16 bits, 32768 = 1, halved: 16384 = 1); the table is sin x 32768 cut towards 0
   (32 of its 2048 entries are 1 larger) */
static int view_angle(float a)
{
    a = fmodf(a, 2.0f * PI);
    if(a < 0.0f) a += 2.0f * PI;
    return (int)(a * (2048.0f / (2.0f * PI))) & 2047;
}

static int32_t view_sin16(int i)          /* 16384 = 1 */
{
    return (int32_t)(sinf((float)i * (PI / 1024.0f)) * 32768.0f) >> 1;
}

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
static uint32_t clock16;                /* the animation clock, 16 bits (0x21538) */
static uint8_t multi[MAX_OBJ];          /* the mesh has several blocks or moving parts */
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
/* and by place: each drivable mesh's box round its boxed faces in CGRID x CGRID cells, each cell
   with the faces a point in it may be over, in their order: those whose box reaches into the
   cell and those without a box. Only faces the box test would pass over are left out, so a
   point finds the same face as through all of them. at[0] -1: no grid (the list was full) */
#define CGRID       8
#define MAX_CLIST   65536
static struct cgrid { float x0, z0, sx, sz; int32_t at[CGRID * CGRID + 1]; } cgrid[MAX_OBJ];
static uint16_t clist[MAX_CLIST];       /* the cells' faces, from the mesh's first */
/* the ground list's placements as track_under reads them, worked out once (the same values):
   place, its block's middle, the square of its reach, its turn; obj -1 if it has no faces */
struct gpiece { float x, y, ox, oy, r2, c, s; int32_t obj, h; };
#define MAX_GROUND  4096
static struct gpiece gpieces[MAX_GROUND];
static float sin_q[513];                /* sin(2 pi i / 2048) for i = 0..512 */
static float under_n[3];                /* track_under's face: its normal in world axes (x, y, up) */
static float far_z = 43808.0f;          /* the far plane of the DETAIL level */
static float radius[MAX_OBJ];           /* of each mesh around its origin */
/* the placements as the culling reads them, from the track's set-up: place as floats (the same
   values), its mesh's reach (-1: no mesh) and class key */
static struct cpiece { float x, y, h, rad; } cpieces[MAX_PIECES];
static int16_t cpiece_key[MAX_PIECES];
/* the placements in groups of 16 as they lie in the file (cell by cell, so close together),
   each group's box round their places: a group out of reach is passed over at once */
#define GROUP       16
static struct { float x0, y0, x1, y1; } groups[MAX_PIECES / GROUP + 1];

int track_mode;
void (*track_hw_piece)(const struct gu_piece *p);

#ifdef __riscv
static inline uint32_t now(void)
{
    uint32_t c;
    __asm__ volatile ("csrr %0, cycle" : "=r"(c));
    return c;
}
/* the three worked out here, before the branches after it: the FPU's pipeline takes them side
   by side, where one after the other each waits for the last (the values stay the same) */
#define FP_NOW(a, b, c) __asm__ ("" : "+f"(a), "+f"(b), "+f"(c))
#else
static inline uint32_t now(void) { return 0; }
#define FP_NOW(a, b, c) ((void)0)
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

/* the cell of v along an axis of a grid: never smaller for a larger v, so a face's box from lo
   to hi reaches into the cells cell(lo) .. cell(hi), and a point between lies in one of those.
   (v - v0) * s stays well inside an int: the boxes are whole numbers at least 1 apart, s at
   most CGRID / 1, and track_under asks only for places nearer than 4,000,000 */
static inline int cgrid_cell(float v, float v0, float s)
{
    int i = (int)((v - v0) * s);
    return i < 0 ? 0 : i > CGRID - 1 ? CGRID - 1 : i;
}

/* the cells a face reaches into: all for one without a box */
static void cgrid_cells(const struct cgrid *g, const struct cface *q, int r[4])
{
    if(!q->boxed) {
        r[0] = r[2] = 0;
        r[1] = r[3] = CGRID - 1;
        return;
    }
    r[0] = cgrid_cell(q->x0, g->x0, g->sx);
    r[1] = cgrid_cell(q->x1, g->x0, g->sx);
    r[2] = cgrid_cell(q->z0, g->z0, g->sz);
    r[3] = cgrid_cell(q->z1, g->z0, g->sz);
}

static void cgrid_build(void)
{
    int n = 0;
    for(int k = 0; k < unr_count; k++) {
        struct cgrid *g = &cgrid[k];
        g->at[0] = -1;
        if(cobj_first[k] < 0) continue;
        const struct cface *f0 = &cfaces[cobj_first[k]];
        int nf = cobj_count[k], any = 0, r[4], cnt[CGRID * CGRID] = { 0 }, total = 0;
        float x0 = 0.0f, x1 = 0.0f, z0 = 0.0f, z1 = 0.0f;
        for(int j = 0; j < nf; j++) {
            const struct cface *q = &f0[j];
            if(!q->boxed) continue;
            if(!any || q->x0 < x0) x0 = q->x0;
            if(!any || q->x1 > x1) x1 = q->x1;
            if(!any || q->z0 < z0) z0 = q->z0;
            if(!any || q->z1 > z1) z1 = q->z1;
            any = 1;
        }
        g->x0 = x0;
        g->z0 = z0;
        g->sx = x1 > x0 ? (float)CGRID / (x1 - x0) : 0.0f;
        g->sz = z1 > z0 ? (float)CGRID / (z1 - z0) : 0.0f;
        for(int j = 0; j < nf; j++) {                   /* a face without corners is never found */
            if(!f0[j].nc) continue;
            cgrid_cells(g, &f0[j], r);
            for(int cz = r[2]; cz <= r[3]; cz++)
                for(int cx = r[0]; cx <= r[1]; cx++) cnt[cz * CGRID + cx]++;
            total += (r[1] - r[0] + 1) * (r[3] - r[2] + 1);
        }
        if(n + total > MAX_CLIST) continue;
        int at[CGRID * CGRID];
        for(int c = 0; c < CGRID * CGRID; c++) {
            g->at[c] = at[c] = n;
            n += cnt[c];
        }
        g->at[CGRID * CGRID] = n;
        for(int j = 0; j < nf; j++) {                   /* in the faces' order */
            if(!f0[j].nc) continue;
            cgrid_cells(g, &f0[j], r);
            for(int cz = r[2]; cz <= r[3]; cz++)
                for(int cx = r[0]; cx <= r[1]; cx++) clist[at[cz * CGRID + cx]++] = (uint16_t)j;
        }
    }
}

/* ground list entry at as track_under reads it */
static void gpiece_make(int at, struct gpiece *g)
{
    const struct piece *pc = &pieces[ground_list[at]];
    g->obj = -1;
    if(pc->obj < 0 || pc->obj >= unr_count || cobj_first[pc->obj] < 0) return;
    const uint8_t *obj, *blk = block_of(pc->obj, &obj);
    float r = (float)rd32(blk);
    g->x = (float)pc->x;
    g->y = (float)pc->y;
    g->ox = (float)rd16(blk + 4);
    g->oy = (float)rd16(blk + 8);
    g->r2 = r * r;
    g->c = sin2048((uint32_t)pc->t + 512);
    g->s = sin2048((uint32_t)pc->t);
    g->obj = pc->obj;
    g->h = pc->h;
}

int track_open(const volatile uint8_t *file)
{
    const uint8_t *f = (const uint8_t *)file;
    if(rd32(f) != 0x4B524353 || rd32(f + 4) != 6) return 0;    /* "SCRK", version 6 */
    file_start = f;
    file_size = (uint32_t)rd32(f + 0x88);
    nblocks = rd32(f + 0x8C);
    if(nblocks < 1 || 0x90 + 4 * nblocks > 0x100) return 0;
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
    cgrid_build();
    for(int at = 0; at < ground_first[65536] && at < MAX_GROUND; at++) gpiece_make(at, &gpieces[at]);

    for(int k = 0; k < unr_count; k++) {
        const uint8_t *obj, *blk = block_of(k, &obj);
        int nb = rd32(obj + 0x14), frames = rd32(obj + 0x10);
        multi[k] = nb > 1 || frames > 0;
        /* the reach round the origin: each block's vertices, and how far its place or frames
           move it (mesh_reach) */
        radius[k] = 0.0f;
        for(int b = 0; b < (multi[k] ? nb : 1) && b < MAX_BLOCKS; b++) {
            blk = obj + rd32(obj + 0x18 + 4 * b);
            const uint8_t *v = obj + rd32(blk + 0x1C);
            int n = rd32(blk + 0x10);
            int32_t so = rd32(blk + 0x0C);
            float r2 = 0.0f, t2 = 0.0f;
            for(int i = 0; i < n && i < GU_MAX_VERT; i++, v += 6) {
                float x = rd16(v), y = rd16(v + 2), z = rd16(v + 4), d = x * x + y * y + z * z;
                if(d > r2) r2 = d;
            }
            if(multi[k] && so > 0)
                for(int fr = 0; fr < (frames > 0 ? frames : 1); fr++) {
                    float tx, ty, tz;
                    if(frames > 0) {
                        const uint8_t *q = obj + so + 0x1C + 0x30 * fr;
                        tx = rd16(q + 18); ty = rd16(q + 20); tz = rd16(q + 22);
                    } else {
                        tx = (float)rd32(obj + so + 0x10); ty = (float)rd32(obj + so + 0x14); tz = (float)rd32(obj + so + 0x18);
                    }
                    if(tx * tx + ty * ty + tz * tz > t2) t2 = tx * tx + ty * ty + tz * tz;
                }
            float rb = __builtin_sqrtf(r2) + __builtin_sqrtf(t2);
            if(rb > radius[k]) radius[k] = rb;
        }
        class_key[k] = (int16_t)track_class_key(obj);
    }
    for(int i = 0; i < npieces; i++) {
        float x = (float)pieces[i].x, y = (float)pieces[i].y;
        int g = i / GROUP;
        if(i % GROUP == 0) { groups[g].x0 = groups[g].x1 = x; groups[g].y0 = groups[g].y1 = y; }
        if(x < groups[g].x0) groups[g].x0 = x;
        if(x > groups[g].x1) groups[g].x1 = x;
        if(y < groups[g].y0) groups[g].y0 = y;
        if(y > groups[g].y1) groups[g].y1 = y;
        int k = pieces[i].obj, ok = k >= 0 && k < unr_count;
        cpieces[i] = (struct cpiece){ x, y, (float)pieces[i].h, ok ? radius[k] : -1.0f };
        cpiece_key[i] = ok ? class_key[k] : 0;
    }
    return npieces;
}

int track_class_key(const uint8_t *obj)
{
    int32_t cl = rd32(obj + 0x32C);                     /* 0x8a8d7 */
    return cl == 3 ? -6000 : cl == 1 ? -1500 : cl == 2 ? -800 : cl == 4 ? -200 : 0;
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

uint32_t track_phase(void)
{
    return piece_phase;
}

void track_clock(uint32_t steps)
{
    anim += (float)steps * 0.02f * 0.3f;
    while(anim >= 1.0f) anim -= 1.0f;
    clock16 = (uint32_t)(anim * 65535.0f);
    piece_phase = clock16 & 0x3FFF;
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
    int w = clip->x1 - clip->x0, h = clip->y1 - clip->y0, eye = EYE_LINE(h);
    int32_t s = view_sin16(view_angle(cam->pitch));             /* the game's sine, [0x8228c] */
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

/* track_under's test of face q of placement gp at (lx, lz) in its frame (c, s its turn): 1 the
   point is over it (*h, under_n set if a height is asked for), 0 the next face */
static inline int under_face(const struct cface *q, const struct gpiece *gp, float lx, float lz, float c, float s,
                             float *h, int *grass)
{
    /* (the comparisons with | and the counts without branches: the FPU answers them side by
       side, where each branch would wait for one) */
    if(q->boxed && ((lx < q->x0) | (lx > q->x1) | (lz < q->z0) | (lz > q->z1))) return 0;
    int nc = q->nc, pos = 0, neg = 0;
    if(!nc) return 0;
    for(int j = 0; j < nc; j++) {
        int b = j + 1 < nc ? j + 1 : 0;
        float ax = q->x[j], az = q->z[j];
        float cr = (q->x[b] - ax) * (lz - az) - (q->z[b] - az) * (lx - ax);
        pos += cr > 0.0f;
        neg += cr < 0.0f;
    }
    if(pos && neg) return 0;
    if(grass) *grass = cobj_grass[gp->obj];
    if(!h) return 1;
    if(q->n[1] == 0) return 0;
    int num = ((int)lx - q->v[0]) * q->n[0] + ((int)lz - q->v[2]) * q->n[2];
    *h = (float)gp->h + (float)q->v[1] + (float)(-num / q->n[1]);
    under_n[0] = (float)q->n[0] * c - (float)q->n[2] * s;
    under_n[1] = (float)q->n[0] * s + (float)q->n[2] * c;
    under_n[2] = (float)q->n[1];
    return 1;
}

int track_under(float x, float y, float *h, int *grass)
{
    static const int8_t cell[9][2] = { {0,0}, {1,0}, {-1,0}, {0,1}, {0,-1}, {1,1}, {-1,-1}, {1,-1}, {-1,1} };
    int cx = ((int)x + 0x100000) >> 13, cy = ((int)y + 0x100000) >> 13;
    int near = (fabsf(x) < 4.0e6f) & (fabsf(y) < 4.0e6f);  /* (else, or not a number: all faces) */
    for(int ci = 0; ci < 9; ci++) {
        int kx = cx + cell[ci][0], ky = cy + cell[ci][1];
        if(kx < 0 || kx > 255 || ky < 0 || ky > 255) continue;
        for(int at = ground_first[ky * 256 + kx]; at < ground_first[ky * 256 + kx + 1]; at++) {
            struct gpiece made;
            const struct gpiece *gp = at < MAX_GROUND ? &gpieces[at] : (gpiece_make(at, &made), &made);
            if(gp->obj < 0) continue;
            float dx = x - gp->x, dy = y - gp->y;
            float bx = dx - gp->ox, by = dy - gp->oy;
            if(bx * bx + by * by > gp->r2) continue;
            float c = gp->c, s = gp->s;
            float lx = dx * c + dy * s, lz = -dx * s + dy * c;  /* into the piece's frame */
            const struct cface *f0 = &cfaces[cobj_first[gp->obj]], *fe = f0 + cobj_count[gp->obj];
            const struct cgrid *g = &cgrid[gp->obj];
            for(int tries = 0; tries < 3; tries++, lx += 10.0f, lz += 10.0f)
                if(g->at[0] >= 0 && near) {
                    int k = cgrid_cell(lz, g->z0, g->sz) * CGRID + cgrid_cell(lx, g->x0, g->sx);
                    for(const uint16_t *li = clist + g->at[k], *le = clist + g->at[k + 1]; li < le; li++)
                        if(under_face(f0 + *li, gp, lx, lz, c, s, h, grass)) return 1;
                } else
                    for(const struct cface *q = f0; q < fe; q++)
                        if(under_face(q, gp, lx, lz, c, s, h, grass)) return 1;
        }
    }
    return 0;
}

/* how the game stands a thing heading yaw on the face track_under found (0x1cb9c, an
   opponent): the face's normal turned into its frame, each level part x pi / 46080 of the
   normal's 16384 units an angle (1.117 x the slope); pitch + nose up, roll + right side up */
void track_under_tilt(float fx, float fy, float *pitch, float *roll)
{
    float nx = under_n[0], ny = under_n[1], nz = under_n[2];
    float len = sqrtf(nx * nx + ny * ny + nz * nz);
    if(nz < 0.0f) len = -len;
    if(len != 0.0f) { nx /= len; ny /= len; }
    *pitch = -1.117f * (nx * fx + ny * fy);
    *roll = 1.117f * (nx * -fy + ny * fx);
}

int track_opp_count(int *rows)
{
    const uint8_t *o = file_start + rd32(file_start + 0x80);
    *rows = rd32(o + 4);
    return rd32(o);
}

const struct track_opp_row *track_opp_rows(void)
{
    return (const struct track_opp_row *)(file_start + rd32(file_start + 0x80) + 8);
}

const struct track_spline_node *track_opp_spline(int k, int *start, int *n)
{
    const uint8_t *o = file_start + rd32(file_start + 0x80), *p = o + 8 + 20 * rd32(o + 4);
    for(int j = 0; j < k; j++) p += 8 + 32 * rd32(p + 4);
    *start = rd32(p);
    *n = rd32(p + 4);
    return (const struct track_spline_node *)(p + 8);
}

/* the camera's frame (track_draw_cam): forward, right, up */
static void cam_frame(const struct track_cam *cam, float f[3], float r[3], float u[3])
{
    /* the last angles' frame kept (the effects and the scene ask for the same camera's): for
       the same bits the same values, without the six sines and cosines */
    static uint32_t last[3];
    static float lf[3], lr[3], lu[3];
    static int kept;
    uint32_t a[3];
    memcpy(a, &cam->yaw, sizeof a);                     /* yaw, pitch, roll */
    if(kept && a[0] == last[0] && a[1] == last[1] && a[2] == last[2]) {
        memcpy(f, lf, sizeof lf);
        memcpy(r, lr, sizeof lr);
        memcpy(u, lu, sizeof lu);
        return;
    }
    float sy = sinf(cam->yaw), cy = cosf(cam->yaw), sp = sinf(cam->pitch), cp = cosf(cam->pitch);
    float sr = sinf(cam->roll), cr = cosf(cam->roll);
    f[0] = sy * cp; f[1] = -cy * cp; f[2] = sp;
    /* level right = forward x up, level up = right x forward, then the roll about forward */
    const float r0[3] = { -cy, -sy, 0.0f };
    const float u0[3] = { r0[1] * f[2], -r0[0] * f[2], r0[0] * f[1] - r0[1] * f[0] };
    r[0] = r0[0] * cr + u0[0] * sr; r[1] = r0[1] * cr + u0[1] * sr; r[2] = u0[2] * sr;
    u[0] = u0[0] * cr - r0[0] * sr; u[1] = u0[1] * cr - r0[1] * sr; u[2] = u0[2] * cr;
    memcpy(last, a, sizeof a);
    memcpy(lf, f, sizeof lf);
    memcpy(lr, r, sizeof lr);
    memcpy(lu, u, sizeof lu);
    kept = 1;
}

void track_cam_axes(const struct track_cam *cam, float f[3], float r[3], float u[3])
{
    cam_frame(cam, f, r, u);
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
    return o < e && crc32(file_start + o, file_start + e) == (uint32_t)rd32(file_start + 0x90 + 4 * k);
}

void track_view(const struct rs_clip *clip, struct su_view *v)
{
    v->cx = (clip->x0 + clip->x1) * 8 - 16; /* the centre in 1/16 pixel: column 159 on the screen,
                                               as the game's projection (0x83431) */
    v->cy = (clip->y0 + EYE_LINE(clip->y1 - clip->y0)) * 16;
    v->scale = (uint32_t)(FOCAL * 16.0f);
    v->far8 = (int32_t)((far_z + EYE_BACK) * 256.0f);  /* depths from the eye */
}

/* the list drawn: key << 16 | i, i (the low half) a piece, or npieces + an object */
#define MAX_OBJS    64
static int32_t vis[MAX_PIECES + MAX_OBJS];

/* The eye of a block as the game takes it for its faces (0x834d6..0x83547): the eye in the
   block's frame (x, y, z) plus the block's middle, times 1.2 (imul 0x13333, sar 16; unscaled
   if that overflows 32 bits on any axis). It picks the face row (0x8a598): 8 more when it is
   below the block's x-z plane, then the 45-degree sector round its y axis, on a diagonal the
   row next to the x axis; and it is the eye of the normal test (0x835a0 pops it into the
   test's operands). Sets g->row and g->e (8 fraction bits). */
static void face_eye(const uint8_t *blk, float x, float y, float z, struct gu_piece *g)
{
    float p[3] = { x + (float)rd16(blk + 4), y + (float)rd16(blk + 6), z + (float)rd16(blk + 8) };
    /* (all comparisons first, with | and without branches: the FPU answers them side by side,
       where a branch on each would wait for it) */
    int big = 0;
    for(int i = 0; i < 3; i++) big |= (p[i] * 78643.0f >= 2147483648.0f) | (p[i] * 78643.0f < -2147483648.0f);
    for(int i = 0; i < 3; i++) g->e[i] = (int32_t)((big ? p[i] : p[i] * (78643.0f / 65536.0f)) * 256.0f);
    int below = p[1] < 0.0f, xpos = p[0] >= 0.0f, zpos = p[2] >= 0.0f, zneg = p[2] < 0.0f;
    int c0 = p[2] > p[0], c2 = -p[2] <= p[0], c4 = -p[2] > -p[0], c6 = p[2] <= -p[0];
    int row = xpos ? (zpos ? !c0 : 2 + !c2) : (zneg ? 4 + !c4 : 6 + !c6);
    g->row = below * 8 + row;
}

/* A block placed at o with m (world from local) as a piece for the geometry unit: its place in
   the view from the eye (EYE_BACK further along forward), its faces' eye the camera c */
static void block_piece(const float c[3], const float f[3], const float r[3], const float u[3],
                        const float m[9], const float o[3], const uint8_t *blk, struct gu_piece *g)
{
    const float *basis[3] = { r, u, f };
#pragma GCC unroll 3                            /* (the conversions side by side in the FPU) */
    for(int k = 0; k < 3; k++) {
#pragma GCC unroll 3
        for(int j = 0; j < 3; j++)
            g->m[3 * k + j] = (int32_t)((basis[k][0] * m[j] + basis[k][1] * m[3 + j] + basis[k][2] * m[6 + j]) * 65536.0f);
        g->t[k] = (int32_t)((basis[k][0] * (o[0] - c[0]) + basis[k][1] * (o[1] - c[1]) + basis[k][2] * (o[2] - c[2])
                             + (k == 2 ? EYE_BACK : 0.0f)) * 256.0f);
    }
    float e[3];
    for(int j = 0; j < 3; j++) e[j] = m[j] * (c[0] - o[0]) + m[3 + j] * (c[1] - o[1]) + m[6 + j] * (c[2] - o[2]);
    face_eye(blk, e[0], e[1], e[2], g);
    g->blk = blk;
    g->mirror = 0;
    g->refl = 0;
}

void track_block_piece(const struct track_cam *cam, const struct track_block *b, struct gu_piece *g)
{
    const float c[3] = { cam->x, cam->y, cam->h };
    float f[3], r[3], u[3];
    cam_frame(cam, f, r, u);
    block_piece(c, f, r, u, b->m, b->o, b->blk, g);
    g->obj = b->file;
    g->phase = b->phase;
}

/* A placement whose mesh has several blocks or moving parts (track 1's football helmet V220:
   its base and the helmet turning on it): block by block, each turned by its frame (0x8ad14:
   a frame's 12 words are the block's whole matrix, the vertex as stored a row multiplied into
   it, 16383 = 1, then its place; frame = frames x clock >> 16, a turn of the clock in 3.33 s;
   the frames turn z up into y up, a mirror) or else moved by its place (the position record),
   the furthest block's middle first (0x8ac58). Returns the pieces in gs (MAX_BLOCKS). */
static int multi_pieces(const float c[3], const float f[3], const float r[3], const float u[3],
                        const struct piece *pc, struct gu_piece *gs)
{
    const uint8_t *obj = unr_blob + unr_off[pc->obj];
    int nb = rd32(obj + 0x14), frames = rd32(obj + 0x10), n = 0, ord[MAX_BLOCKS];
    float key[MAX_BLOCKS];
    struct gu_piece tmp[MAX_BLOCKS];
    float ca = sin2048((uint32_t)pc->t + 512), sa = sin2048((uint32_t)pc->t);
    const float R[9] = { ca, 0.0f, -sa, sa, 0.0f, ca, 0.0f, 1.0f, 0.0f };   /* world from the piece's frame */
    int fr = frames > 0 ? (int)(((uint32_t)frames * clock16) >> 16) : -1;
    for(int b = 0; b < nb && b < MAX_BLOCKS; b++) {
        const uint8_t *blk = obj + rd32(obj + 0x18 + 4 * b);
        int32_t so = rd32(blk + 0x0C);
        float F[9] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f }, t[3] = { 0.0f, 0.0f, 0.0f };
        int moving = so > 0 && fr >= 0;
        if(moving) {                                /* rows x, up, z: the words down the columns */
            const uint8_t *q = obj + so + 0x1C + 0x30 * fr;
            for(int i = 0; i < 3; i++)
                for(int j = 0; j < 3; j++) F[3 * i + j] = (float)rd16(q + 2 * (3 * j + i)) / 16383.0f;
            for(int i = 0; i < 3; i++) t[i] = (float)rd16(q + 18 + 2 * i);
        } else if(so > 0)
            for(int i = 0; i < 3; i++) t[i] = (float)rd32(obj + so + 0x10 + 4 * i);
        float m[9], o[3];
        for(int i = 0; i < 3; i++) {
            for(int j = 0; j < 3; j++) m[3 * i + j] = R[3 * i] * F[j] + R[3 * i + 1] * F[3 + j] + R[3 * i + 2] * F[6 + j];
            o[i] = (i == 0 ? (float)pc->x : i == 1 ? (float)pc->y : (float)pc->h) +
                   R[3 * i] * t[0] + R[3 * i + 1] * t[1] + R[3 * i + 2] * t[2];
        }
        struct gu_piece *g = &tmp[b];
        block_piece(c, f, r, u, m, o, blk, g);
        g->obj = obj;
        g->phase = piece_phase;
        g->mirror = moving;
        float w[3];
        for(int i = 0; i < 3; i++)
            w[i] = o[i] + m[3 * i] * rd16(blk + 4) + m[3 * i + 1] * rd16(blk + 6) + m[3 * i + 2] * rd16(blk + 8) - c[i];
        key[b] = -(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        int i = b;
        while(i > 0 && key[ord[i - 1]] > key[b]) { ord[i] = ord[i - 1]; i--; }
        ord[i] = b;
        n++;
    }
    for(int b = 0; b < n; b++) gs[b] = tmp[ord[b]];
    return n;
}

static float refl_rate = 0.008f;

void track_reflection_rate(float k)
{
    refl_rate = k;
}

/* The reflection's offsets of an object (0xc1fc0, before the faces of its first block with
   flag 0x2000): its way since it was last drawn (whole units, as the record keeps its place)
   turned into the view (the original's axes x left, y up, z back), times the rate, added to
   its three accumulators; each sum rounded (to nearest) & 0xff, the first and the third the
   offsets of u and v, as gu_piece.refl's [23:16] and [31:24] */
static uint32_t refl_offsets(const struct track_object *ob, const float f[3], const float r[3], const float u[3])
{
    struct track_refl *s = ob->refl;
    int32_t p[3] = { (int32_t)ob->x, (int32_t)ob->y, (int32_t)ob->h };
    float d[3] = { (float)(p[0] - s->prev[0]), (float)(p[1] - s->prev[1]), (float)(p[2] - s->prev[2]) };
    int32_t n[3];
    if(d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f) {
        const float w[3] = { -(r[0] * d[0] + r[1] * d[1] + r[2] * d[2]), u[0] * d[0] + u[1] * d[1] + u[2] * d[2],
                             -(f[0] * d[0] + f[1] * d[1] + f[2] * d[2]) };
        for(int k = 0; k < 3; k++) {
            float a = w[k] * refl_rate + s->acc[k];
            n[k] = (int32_t)lrintf(a);
            s->acc[k] = a;
            s->prev[k] = p[k];
        }
    } else
        for(int k = 0; k < 3; k++) n[k] = (int32_t)lrintf(s->acc[k]);
    return (uint32_t)(n[0] & 255) << 16 | (uint32_t)(n[2] & 255) << 24;
}

/* The view from the point c along forward f, with right r and up u (unit vectors) */
static void draw(volatile uint8_t *fb, const struct rs_clip *clip, const struct track_cam *cam, const float c[3],
                 const float f[3], const float r[3], const float u[3], struct track_object *objs, int nobj,
                 struct track_stats *st)
{
    uint32_t t_start = now();
    st->pieces = st->faces = st->triangles = st->see_through = st->vertices = st->objects = 0;
    const float cx = c[0], cy = c[1], ch = c[2];
    const float fx = f[0], fy = f[1], fh = f[2], rx = r[0], ry = r[1], rh = r[2];
    const float ux = u[0], uy = u[1], uh = u[2];
    st->cam[0] = (int32_t)cx;
    st->cam[1] = (int32_t)cy;
    st->cam[2] = (int32_t)ch;

    /* the pieces in reach and in view, far to near: vf the depth from the eye */
    const float far_eye = far_z + EYE_BACK;
    const float half_w = (float)(clip->x1 - clip->x0) / 2.0f / FOCAL;
    const int eye = EYE_LINE(clip->y1 - clip->y0);
    const float up = (float)eye / FOCAL;                                       /* above the eye line */
    const float down = (float)(clip->y1 - clip->y0 - eye) / FOCAL;
    int nv = 0;
    float nearest = 1e30f;
    /* the game's key (0x8a7b0): -(sqrt(dist^2 >> 2), rounded, >> 2) */
#define KEY(d2) (-((int32_t)lrintf(sqrtf((d2) * 0.25f)) >> 2))
    for(int i = 0; i < npieces; i++) {
        if(i % GROUP == 0) {                        /* the whole group out of reach (in the plane) */
            int g = i / GROUP;
            float gx = cx < groups[g].x0 ? groups[g].x0 - cx : cx > groups[g].x1 ? cx - groups[g].x1 : 0.0f;
            float gy = cy < groups[g].y0 ? groups[g].y0 - cy : cy > groups[g].y1 ? cy - groups[g].y1 : 0.0f;
            if(gx * gx + gy * gy >= REACH2) {
                i += GROUP - 1;
                continue;
            }
        }
        const struct cpiece *pc = &cpieces[i];
        float rad = pc->rad;
        if(rad < 0.0f) continue;
        float dx = pc->x - cx, dy = pc->y - cy, dh = pc->h - ch;
        float d2 = dx * dx + dy * dy + dh * dh;
        if(d2 < nearest) nearest = d2;
        if(d2 >= REACH2) continue;
        /* behind the eye or past the far plane (more than half of those in reach), then out at a
           side: each one's tests at once (| not ||, one branch: the FPU answers them side by side) */
        float vf = dx * fx + dy * fy + dh * fh + EYE_BACK;
        if((vf + rad < NEAR) | (vf - rad > far_eye)) continue;
        float vr = dx * rx + dy * ry + dh * rh, vu = dx * ux + dy * uy + dh * uh;
        if((vr - rad > half_w * (vf + rad)) | (vr + rad < -half_w * (vf + rad)) | (vu - rad > up * (vf + rad)) |
           (vu + rad < -down * (vf + rad))) continue;
        vis[nv++] = (KEY(d2) + cpiece_key[i]) * 65536 + i;
    }
    for(int k = 0; k < nobj && k < MAX_OBJS; k++) {
        const struct track_object *ob = &objs[k];
        float dx = ob->x - cx, dy = ob->y - cy, dh = ob->h - ch, rad = ob->radius;
        float vf = dx * fx + dy * fy + dh * fh + EYE_BACK, vr = dx * rx + dy * ry + dh * rh;
        float vu = dx * ux + dy * uy + dh * uh;
        FP_NOW(vf, vr, vu);
        if((vf + rad < NEAR) | (vf - rad > far_eye) | (vr - rad > half_w * (vf + rad)) |
           (vr + rad < -half_w * (vf + rad)) | (vu - rad > up * (vf + rad)) | (vu + rad < -down * (vf + rad))) continue;
        float d2 = dx * dx + dy * dy + dh * dh;
        /* 0x8a8c3: an object counts nearer; 0x8a8d7: its mesh's class */
        vis[nv++] = (KEY(d2) + 800 + ob->key) * 65536 + npieces + k;
        st->objects++;
    }
    st->nearest = nearest < 4e18f ? (uint32_t)__builtin_sqrtf(nearest) : 0xFFFFFFFFu;
    /* by key, far first. The list is in the order of i, the keys' low half, and the keys differ:
       sorted by their high half, keeping that order (two passes of 8 bits, radix), it is in the
       order of the whole key, as any sort by it has it */
    static int32_t vis2[MAX_PIECES + MAX_OBJS];
    static int cnt[2][256];                     /* both passes' counts in one go; the sums clear
                                                   them (no memset: picolibc's goes byte by byte) */
    for(int n = 0; n < nv; n++) {
        uint32_t u = (uint32_t)vis[n] ^ 0x80000000u;
        cnt[0][u >> 16 & 255]++;
        cnt[1][u >> 24]++;
    }
    for(int pass = 0; pass < 2; pass++) {
        const int sh = 16 + 8 * pass;
        const int32_t *from = pass ? vis2 : vis;
        int32_t *to = pass ? vis : vis2;
        int at[256];
        for(int d = 0, s = 0; d < 256; d++) {
            at[d] = s;
            s += cnt[pass][d];
            cnt[pass][d] = 0;
        }
        for(int n = 0; n < nv; n++) to[at[((uint32_t)from[n] ^ 0x80000000u) >> sh & 255]++] = from[n];
    }
    uint32_t t_pieces = now();
    st->cyc_cull = t_pieces - t_start;
#ifdef TRACK_DUMP                               /* the PC renderer (tools/render.sh): the list drawn */
    for(int k = 0; k < nv; k++)
        if((vis[k] & 0xFFFF) < npieces)
            printf("drawn piece obj %d at %d %d %d\n", (int)pieces[vis[k] & 0xFFFF].obj, (int)pieces[vis[k] & 0xFFFF].x,
                   (int)pieces[vis[k] & 0xFFFF].h, (int)pieces[vis[k] & 0xFFFF].y);
        else
            printf("drawn object at %.0f %.0f %.0f\n", objs[(vis[k] & 0xFFFF) - npieces].x,
                   objs[(vis[k] & 0xFFFF) - npieces].h, objs[(vis[k] & 0xFFFF) - npieces].y);
#endif

    struct su_view view;
    struct gu_stats gst = { 0, 0, 0, 0 };
    track_view(clip, &view);
    uint32_t t_out = 0;
    for(int n = 0; n < nv; n++) {
        const int vi = vis[n] & 0xFFFF;
        if(vi >= npieces) {                         /* an object: its blocks as they come */
            struct track_object *ob = &objs[vi - npieces];
            if(ob->make) ob->make(ob->id, cam, ob);
            uint32_t refl = ob->refl_phase & 0xFFFF;
            for(int b = 0; b < ob->n && ob->refl; b++)
                if(rd32(ob->b[b].blk + 0x18) & 0x2000) {
                    refl |= refl_offsets(ob, f, r, u);
                    break;
                }
            for(int b = 0; b < ob->n; b++) {
                struct gu_piece g;
                block_piece(c, f, r, u, ob->b[b].m, ob->b[b].o, ob->b[b].blk, &g);
                g.obj = ob->b[b].file;
                g.phase = ob->b[b].phase;
                g.refl = refl;
                st->pieces++;
                uint32_t t0 = now();
                if(track_mode == TRACK_HW) track_hw_piece(&g);
                else gu_draw(fb, clip, &view, mlb, &g, &gst);
                t_out += now() - t0;
            }
            continue;
        }
        const struct piece *pc = &pieces[vi];
        if(multi[pc->obj]) {
            struct gu_piece gs[MAX_BLOCKS];
            int nb = multi_pieces(c, f, r, u, pc, gs);
            for(int b = 0; b < nb; b++) {
                st->pieces++;
                uint32_t t0 = now();
                if(track_mode == TRACK_HW) track_hw_piece(&gs[b]);
                else gu_draw(fb, clip, &view, mlb, &gs[b], &gst);
                t_out += now() - t0;
            }
            continue;
        }
        struct gu_piece g;
        g.phase = piece_phase;
        g.mirror = 0;
        g.refl = 0;
        g.blk = block_of(pc->obj, &g.obj);
        float ca = sin2048((uint32_t)pc->t + 512), sa = sin2048((uint32_t)pc->t);
        float dx = pc->x - cx, dy = pc->y - cy, dh = pc->h - ch;

        /* local to view: per axis A, A . D + x (Ax cos + Ay sin) + y Ah + z (Ay cos - Ax sin) */
        const float m[9] = { rx * ca + ry * sa, rh, ry * ca - rx * sa,
                             ux * ca + uy * sa, uh, uy * ca - ux * sa,
                             fx * ca + fy * sa, fh, fy * ca - fx * sa };
        const float o[3] = { dx * rx + dy * ry + dh * rh, dx * ux + dy * uy + dh * uh,
                             dx * fx + dy * fy + dh * fh + EYE_BACK };
#pragma GCC unroll 9                            /* (the conversions side by side in the FPU) */
        for(int k = 0; k < 9; k++) g.m[k] = (int32_t)(m[k] * 65536.0f);
        for(int k = 0; k < 3; k++) g.t[k] = (int32_t)(o[k] * 256.0f);

        /* the eye in the piece's frame picks the row and tells the faces turned away */
        face_eye(g.blk, -dx * ca - dy * sa, -dh, dx * sa - dy * ca, &g);
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
                    struct track_object *objs, int nobj, struct track_stats *st)
{
    const float c[3] = { cam->x, cam->y, cam->h };
    float f[3], r[3], u[3];
    cam_frame(cam, f, r, u);
    draw(fb, clip, cam, c, f, r, u, objs, nobj, st);
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
    draw(fb, clip, 0, c, f, r, u, 0, 0, st);
}
