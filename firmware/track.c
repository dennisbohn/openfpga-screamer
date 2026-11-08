/*
 * The track: the reader of the track image, the culling of its placements (which ones are drawn),
 * and the drawing loop that hands the placements to the geometry unit (track.h).
 * The image is made at start-up by track_build.c from the game's files (tools/make_track.py makes
 * the same image on the PC). Entry points: track_open and track_init (read the image),
 * track_draw_cam (draw), track_under (what lies under a point).
 *
 * The image: a header, then the sections from offset 0x100, each on a 256-byte boundary.
 * All numbers are little-endian. An offset counts from the start of the image.
 *
 *   header   what                                     section
 *   0x00     "SCRK" (0x4B524353)
 *   0x04     version, 6
 *   0x08     MAPn.MLB: offset, 0x0C size in bytes      the game's materials, unchanged
 *   0x10     MAPn.UNR: offset, 0x14 size in bytes      the game's meshes, unchanged
 *   0x18     palette: offset                           256 x 0x00RRGGBB
 *   0x1C     placements: offset, 0x20 count            20 bytes each: int32 mesh, X, Y, height, heading;
 *                                                      then the cell table (below)
 *   0x24     camera path: offset, 0x28 count           12 bytes each: int32 x, y, height
 *   0x2C     ground index: offset, 0x30 count          u16 first[65537], then count u16 placement numbers
 *   0x34     markers: offset, 0x38 count               32 bytes each (struct track_marker)
 *   0x3C     cars: offset, 0x40 count                  68 bytes each (struct car_spec)
 *   0x44     car models: offset, 0x48 count            24 bytes each (struct track_model), then the .3DX
 *                                                      files
 *   0x4C     road points: offset, 0x50 count           12 bytes each (struct track_dir)
 *   0x54     walls: offset, 0x58 count                 28 bytes each (struct track_wall)
 *   0x5C     the start: int32 x, y, float yaw          12 bytes in the header itself
 *   0x68     race screen: offset, 0x6C size            (track.h, track_hud)
 *   0x70     bonus time, 0x74 camera height            int32 each (TRACK_DATA)
 *   0x78     backdrop: offset                          512 x 257 palette indices
 *   0x7C     ground colour                             the low 8 bits are used
 *   0x80     opponents: offset, 0x84 size              curves and numbers (track_opp_count)
 *   0x88     size of the image in bytes
 *   0x8C     number of CRC blocks (TRACK_CRC_BLOCK bytes each, from offset 0x100)
 *   0x90     one CRC-32 per block, up to 0x100
 *
 * The cell table ends the placements: "CELL" (0x4C4C4543), int32 number of cells, int32 0,
 * then 24 bytes a cell (struct vcell), in the order of the cells' keys.
 *
 * The original's data structures (openscreamer docs/NOTES.md, "How the track is built").
 *
 * UNR, the meshes:
 *   file     int32 blob size, int32 count, then count offsets into the blob.
 *   object   at its offset in the blob. +0x18: offset of its block (from the object).
 *   block    +0x04  its middle (int16 x, y, z)
 *            +0x10  vertex count
 *            +0x1C  vertex array, +0x24 texture coordinates (both offsets from the object);
 *                   6 bytes a vertex: int16 x, y (up), z and int16 u, v, 0
 *            +0x28 .. +0x64  sixteen face rows (offsets from the block); a row is a list of
 *                   uint16 face offsets (from the block), 0xFFFF ends a row
 *            +0x68  its name
 *   face     +0x02  colour (routine 128)
 *            +0x04  material (byte offset into the MLB)
 *            +0x06  number of corners (3 or 4)
 *            +0x08  corner vertex byte offsets
 *            +0x14, +0x18, +0x1C  the normal (16384 = 1); it points away from the side the face is
 *                   seen from: the face is seen when (corner - eye) . N >= 0
 *
 * MLB, the materials:
 *   material +0 routine (16 texture, 64 texel 0 see-through, 128 one colour,
 *            224 see-through through MAPn.SMG), +4 texture record, +8 and +12 size in texels.
 *   texture record, +8: the pixels of frame 0, rows of 256 bytes.
 *   texel = raw u, v * size / 256.
 *
 * Placement (piece): world = (X + x cos a - z sin a, Y + x sin a + z cos a, height + y),
 *   with a = T * 2 pi / 2048 (T is the heading).
 *
 * A block's sixteen rows list its faces in drawing order, back to front. Each row is sorted for
 * one direction of the eye in the block's frame (face_eye; original: STARTL.EXE 0x8a598):
 *   rows 0 .. 7    the eye is above the block's x-z plane
 *   rows 8 .. 15   the eye is below it
 * In each half there is one row per 45-degree sector round the block's y axis, starting at +z and
 * turning towards +x. A row holds all the faces of the block, or nearly all, as the game's data has
 * it. The normal test then leaves out the faces that are turned away.
 */
#include "track.h"
#ifdef TRACK_DUMP
#include <stdio.h>
#endif
#include "bytes.h"
#include "crc32.h"
#include "hw.h"
#include <math.h>
#include <string.h>

#define MAX_PIECES  2560        /* placements (H set): track 1 has 1164, track 5 the most, 2359 */
#define MAX_OBJ     512         /* meshes; up to 360 on a track */
#define MAX_BLOCKS  12          /* blocks of a mesh drawn (the most: track 4's wheel, ten; track 1's helmet has two) */

/* How far the game draws (openscreamer docs/NOTES.md, DETAIL). It has two limits:
   1. The reach. A piece is looked at only while its position is within 52000 units of the camera
      (original: STARTL.EXE 0x83164, dist^2 >> 2 < 676000000). The number sits in three immediates
      of the code, which nothing writes.
   2. The far plane. Every face is cut at a depth that the DETAIL option sets, measured along the
      view's z from the camera (original: STARTL.EXE 0x8baa4). The original's table at 0x21d4
      holds -27808, -27808, -43808 (LOW, MID, HI); far_depth holds the depths as positive numbers.
   The core draws FAR_SCALE times as far (track.h). */
#define REACH       52000.0f
static const float far_depth[3] = { 27808.0f, 27808.0f, 43808.0f };     /* LOW, MID, HI */

/* The corners of the game's cell window (original: STARTL.EXE 0x82e88, docs/original_geometry.md 2.1):
   two points in the ground plan, ahead of the camera and to its sides. The box round these points and
   the 8192 units round the camera is the part of the map that is looked at (cell_window). */
#define WINDOW_X    57433.0f
#define WINDOW_Z    56096.0f

/* The original's lens: screen = centre + 128 x / (128 + z), with z the depth from the camera.
   1. The corners' divide (original: STARTL.EXE 0x83420) reads TABS\PERSP01.TAB. The file is loaded
      by 0x4cd4c, and 0x8ba28 patches its address into the divide. Entry z of the table is
      -65536 x 128 / (128 + z).
   2. A cut at the near plane z = 0 (0xd7470) projects with 128 / 128 (0xd7493, ds:0x829ac = -128).
   3. A cut at the far plane projects with 128 / (far - 128) (0xd7837, ds:0x829b0 = far - 128, 0x8bad6).
   So the lens has its eye EYE_BACK behind the camera, along the view. The near plane goes through
   the camera (depth 128 from the eye), and the scale is 128 at the eye.
   The camera itself stays the origin of four things:
   - the view's z,
   - the reach,
   - the drawing order (the places less the camera; original: 0x8ab8e),
   - the faces' row (the camera in the block's frame, worked out from those places; face_eye;
     original: 0x834d6).
   The eye line lies 65 % down the view, in whole lines (original: STARTL.EXE 0x19cbe calls
   0x4c610(320, h, h x 0.65 cut by 0x4d7b4)). That is line 130 of 200, and line 85 of the cockpit's 132. */
#define NEAR        128.0f
#define FOCAL       128.0f
#define EYE_BACK    128.0f
#define EYE_LINE(h) ((h) * 65 / 100)
#define LOOK        12          /* a track's camera path has at least LOOK + 2 points (checked in track_init) */

/* The game's view angles (original: STARTL.EXE 0x829e3).
   1. The camera record holds each angle as a float, 0 .. 2 pi. The game multiplies it by 2048 / 2 pi
      and cuts it towards 0: an angle in 2048ths of a turn (view_angle).
   2. Its sine and cosine come from the table at 0x30190. The table has 16 bits (32768 = 1), and
      the game halves the values (16384 = 1; view_sin16).
   The table holds sin x 32768 cut towards 0, except that 32 of its 2048 entries are 1 larger. */
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

/* The image's records: a placement (t the heading in 2048ths of a turn), a camera path point,
   and the start. */
struct piece { int32_t obj, x, y, h, t; };
struct pt { int32_t x, y, h; };
struct start { int32_t x, y; float yaw; };

/* The pointers into the image and the counts of its sections, set by track_open and track_init. */
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
static const uint16_t *ground_first, *ground_list;     /* first[65537], then the placement numbers */
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
static uint32_t piece_phase;            /* the placed pieces' texture phase: the clock (record +0x20 is -1) */
static uint32_t clock16;                /* the animation clock, 16 bits (original: STARTL.EXE 0x21538) */
static uint8_t multi[MAX_OBJ];          /* the mesh has several blocks or moving parts */
static int16_t class_key[MAX_OBJ];      /* the key's change for each mesh's class (header +0x32c) */

/* The tables that track_under reads. They are made once, in track_init, from the drivable meshes.
   Per mesh: the faces of its first face row, in that order. Per face (struct cface):
   - the corners (x, z), the normal n, and the first corner v,
   - a box round the corners (x0, x1, z0, z1),
   - boxed: whether the face is convex and has an area. Only for such a face a point outside the
     box cannot be over the face. A flat or bent face always gets the full test.
   So the answers are the same as with the full test for every face. */
struct cface { float x0, x1, z0, z1; int16_t x[4], z[4], n[3], v[3]; uint8_t nc, boxed; };
#define MAX_CFACES  8192
static struct cface cfaces[MAX_CFACES];
static int ncfaces;
/* The mesh's faces are cfaces[cobj_first .. cobj_first + cobj_count - 1]. cobj_first is -1 if the mesh has no
   table (it is not drivable, or cfaces had no room). cobj_grass: the mesh's name says grass. */
static int32_t cobj_first[MAX_OBJ];
static int16_t cobj_count[MAX_OBJ];
static uint8_t cobj_grass[MAX_OBJ];

/* A grid per drivable mesh, to find the faces a point may be over faster.
   The box round the mesh's boxed faces is cut into CGRID x CGRID cells. Each cell lists, in the
   faces' order, the faces that a point in the cell may be over: the faces whose box reaches into
   the cell, and the faces without a box. Only faces that the box test would reject are left
   out, so a point finds the same face as through the full list.
   at[0] is -1 if the mesh has no grid (it is not drivable, or clist had no room). */
#define CGRID       8
#define MAX_CLIST   65536
static struct cgrid { float x0, z0, sx, sz; int32_t at[CGRID * CGRID + 1]; } cgrid[MAX_OBJ];
static uint16_t clist[MAX_CLIST];       /* the cells' faces, as numbers counted from the mesh's first face */

/* The placements of the ground list as track_under reads them, worked out once:
   x, y the place; ox, oy the block's middle; r2 the square of its reach; c, s the cosine and
   sine of the turn; obj the mesh (-1 if it has no faces); h the height. The values are the
   same as if track_under worked them out at every call (gpiece_make). */
struct gpiece { float x, y, ox, oy, r2, c, s; int32_t obj, h; };
#define MAX_GROUND  4096
static struct gpiece gpieces[MAX_GROUND];
static float sin_q[513];                /* sin(2 pi i / 2048) for i = 0..512 */
static float under_n[3];                /* track_under's face: its normal in world axes (x, y, up) */

/* The factors and levels in force. They are variables so that the PC tools can try other
   values (tools/perf). */
static float far_scale = FAR_SCALE;
static float lod_scale = LOD_SCALE;     /* the level of detail's factor in force (0: none) */
static float lod_inv;                   /* 1 / lod_scale; 0 for none (no level of detail) */
static int detail_level = 2;            /* the DETAIL option in force (track_detail) */
static float far_z = 43808.0f * FAR_SCALE;      /* the far plane of the DETAIL level */
static float reach2 = (REACH * FAR_SCALE) * (REACH * FAR_SCALE);       /* the reach, squared */
static float radius[MAX_OBJ];           /* the reach of each mesh round its origin */

/* The placements as the culling reads them, made in track_init: the place as floats (the same
   numbers as in the image), the mesh's reach (-1: no mesh) and the class key. */
static struct cpiece { float x, y, h, rad; } cpieces[MAX_PIECES];
static int16_t cpiece_key[MAX_PIECES];

/* The map's cells, from the cell table that ends the image's placements (tools/make_track.py).
   Per cell:
   - key: row x 256 + column, in the map's grid of 8192 units,
   - first, count: its placements, a run in the order in which the game walks them,
   - has_box, box: the box of what the game draws while the camera is in this cell, if the map
     gives one (x0, z0, x1, z1).
   The game looks only at the placements of the cells that its window covers (cell_window).
   The cells lie in the order of their keys. */
struct vcell { uint16_t key, first, count, has_box; int32_t box[4]; };
#define CELL_MAGIC  0x4C4C4543u         /* "CELL" */
#define MAX_CELLS   1024                /* a track has up to 573 */
static const struct vcell *vcells;
static int nvcells;

int track_mode;
void (*track_hw_piece)(const struct gu_piece *p);

#ifdef __riscv
/* INT_NOWn asks for the FPU's answers to n comparisons (whole numbers) before any of them is used.
   An answer comes back late, and a use right after its question waits for it. When the questions
   are asked one after the other, the answers come back side by side and the CPU waits once.
   The macros are empty on the PC. */
#define INT_NOW2(a, b) __asm__ ("" : "+r"(a), "+r"(b))
#define INT_NOW4(a, b, c, d) __asm__ ("" : "+r"(a), "+r"(b), "+r"(c), "+r"(d))
#define INT_NOW6(a, b, c, d, e, f) __asm__ ("" : "+r"(a), "+r"(b), "+r"(c), "+r"(d), "+r"(e), "+r"(f))
#define INT_NOW7(a, b, c, d, e, f, g) \
    __asm__ ("" : "+r"(a), "+r"(b), "+r"(c), "+r"(d), "+r"(e), "+r"(f), "+r"(g))
#define INT_NOW8(a, b, c, d, e, f, g, h) \
    __asm__ ("" : "+r"(a), "+r"(b), "+r"(c), "+r"(d), "+r"(e), "+r"(f), "+r"(g), "+r"(h))
#else
#define INT_NOW2(a, b) ((void)0)
#define INT_NOW4(a, b, c, d) ((void)0)
#define INT_NOW6(a, b, c, d, e, f) ((void)0)
#define INT_NOW7(a, b, c, d, e, f, g) ((void)0)
#define INT_NOW8(a, b, c, d, e, f, g, h) ((void)0)
#endif

static float sin2048(uint32_t a)
{
    a &= 2047;
    float s = sin_q[a & 512 ? 512 - (a & 511) : a & 511];
    return a & 1024 ? -s : s;
}

static const uint8_t *block_of(int k, const uint8_t **obj)
{
    *obj = unr_blob + unr_off[k];
    return *obj + ld32s(*obj + 0x18);
}

/* The cell of v along one axis of a cgrid: v0 is the grid's start, s the cells per unit.
   The cell number never gets smaller for a larger v. So a face whose box runs from lo to hi
   reaches into the cells cell(lo) .. cell(hi), and a point between lo and hi lies in one of them.
   (v - v0) * s stays well inside an int:
   - the boxes are whole numbers at least 1 apart,
   - s is at most CGRID / 1,
   - track_under asks only for places nearer than 4,000,000. */
static inline int cgrid_clamp(int i)
{
    return i < 0 ? 0 : i > CGRID - 1 ? CGRID - 1 : i;
}

static inline int cgrid_cell(float v, float v0, float s)
{
    return cgrid_clamp((int)((v - v0) * s));
}

/* The cells of grid g that face q reaches into, as r = { first column, last column, first row,
   last row }. A face without a box reaches into all cells. */
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

/* Makes the grid of every drivable mesh (see cgrid). For each mesh:
   1. The grid's box is the box round the mesh's boxed faces.
   2. The counting pass counts the faces that reach into each cell.
   3. The writing pass writes the face numbers into clist, cell by cell, in the faces' order.
   at[c] is where cell c's faces start in clist, and at[CGRID * CGRID] is where the last one ends.
   A mesh stays without a grid (at[0] = -1) if clist would overflow. */
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
        for(int j = 0; j < nf; j++) {                   /* counting pass; a face without corners is skipped */
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
        for(int j = 0; j < nf; j++) {                   /* the writing pass: in the faces' order */
            if(!f0[j].nc) continue;
            cgrid_cells(g, &f0[j], r);
            for(int cz = r[2]; cz <= r[3]; cz++)
                for(int cx = r[0]; cx <= r[1]; cx++) clist[at[cz * CGRID + cx]++] = (uint16_t)j;
        }
    }
}

/* Works out entry at of the ground list as track_under reads it, into g (see struct gpiece).
   g->obj is -1 if the placement has no mesh, or its mesh has no face table (cobj_first). */
static void gpiece_make(int at, struct gpiece *g)
{
    const struct piece *pc = &pieces[ground_list[at]];
    g->obj = -1;
    if(pc->obj < 0 || pc->obj >= unr_count || cobj_first[pc->obj] < 0) return;
    const uint8_t *obj, *blk = block_of(pc->obj, &obj);
    float r = (float)ld32s(blk);
    g->x = (float)pc->x;
    g->y = (float)pc->y;
    g->ox = (float)ld16s(blk + 4);
    g->oy = (float)ld16s(blk + 8);
    g->r2 = r * r;
    g->c = sin2048((uint32_t)pc->t + 512);
    g->s = sin2048((uint32_t)pc->t);
    g->obj = pc->obj;
    g->h = pc->h;
}

/* Checks the image's header and remembers the image. Only the header is read: the CRC blocks can then
   be checked with track_block_ok before track_init reads the rest. Returns 1 if the header is right. */
int track_open(const volatile uint8_t *file)
{
    const uint8_t *f = (const uint8_t *)file;
    if(ld32s(f) != 0x4B524353 || ld32s(f + 4) != 6) return 0;  /* "SCRK", version 6 */
    file_start = f;
    file_size = (uint32_t)ld32s(f + 0x88);
    nblocks = ld32s(f + 0x8C);
    if(nblocks < 1 || 0x90 + 4 * nblocks > 0x100) return 0;
    return 1;
}

static float lod_scale_inv(float scale)
{
    return scale > 0.0f ? 1.0f / scale : 0.0f;
}

/* Reads the image that track_open accepted. It sets the pointers and counts of the sections (the
   header's offsets are listed at the top of the file), and makes the tables that the drawing and
   track_under read:
   1. sin_q, the sine table.
   2. cfaces, cobj_*, cgrid and gpieces, for track_under: the faces of the drivable meshes.
   3. multi, radius and class_key for each mesh.
   4. cpieces and cpiece_key for each placement, for the culling.
   5. vcells, the cell table that ends the placements.
   Returns the number of placements. Returns 0 if the image does not fit the limits (MAX_OBJ,
   MAX_PIECES, the camera path, the cars and car models), or has no valid cell table. */
int track_init(void)
{
    const uint8_t *f = file_start;
    mlb = f + ld32s(f + 0x08);
    const uint8_t *unr = f + ld32s(f + 0x10);
    pal = (const uint32_t *)(f + ld32s(f + 0x18));
    pieces = (const struct piece *)(f + ld32s(f + 0x1C));
    npieces = ld32s(f + 0x20);
    path = (const struct pt *)(f + ld32s(f + 0x24));
    npath = ld32s(f + 0x28);
    ground_first = (const uint16_t *)(f + ld32s(f + 0x2C));
    ground_list = ground_first + 65537;
    markers = (const struct track_marker *)(f + ld32s(f + 0x34));
    nmarkers = ld32s(f + 0x38);
    cars = (const struct car_spec *)(f + ld32s(f + 0x3C));
    ncars = ld32s(f + 0x40);
    models_at = f + ld32s(f + 0x44);
    models = (const struct track_model *)models_at;
    nmodels = ld32s(f + 0x48);
    dirs = (const struct track_dir *)(f + ld32s(f + 0x4C));
    ndirs = ld32s(f + 0x50);
    walls = (const struct track_wall *)(f + ld32s(f + 0x54));
    nwalls = ld32s(f + 0x58);
    start = (const struct start *)(f + 0x5C);
    lod_inv = lod_scale_inv(lod_scale);
    unr_count = ld32s(unr + 4);
    if(unr_count > MAX_OBJ || npieces > MAX_PIECES || npath < LOOK + 2 || ncars < 1 || nmodels < 1)
        return 0;
    unr_off = (const int32_t *)(unr + 8);
    unr_blob = unr + 8 + 4 * unr_count;

    /* 1. The sine table, by rotating (cosine, sine) by one step of 2 pi / 2048 at a time. */
    const float dc = 0.99999529380957617f, ds = 0.0030679567629659761f;  /* cos, sin of 2 pi / 2048 */
    float cosine = 1.0f, sine = 0.0f;
    for(int i = 0; i <= 512; i++) {
        sin_q[i] = sine;
        float t = cosine * dc - sine * ds;
        sine = sine * dc + cosine * ds;
        cosine = t;
    }

    /* 2. The faces of each drivable mesh, once per mesh: the first face row, in its order. The mesh
       is drivable if the ground list has a placement of it. */
    ncfaces = 0;
    for(int k = 0; k < unr_count; k++) cobj_first[k] = -1;
    for(int i = 0; i < ground_first[65536]; i++) {
        int k = pieces[ground_list[i]].obj;
        if(k < 0 || k >= unr_count || cobj_first[k] >= 0) continue;
        const uint8_t *obj, *blk = block_of(k, &obj), *verts = obj + ld32s(blk + 0x1C);
        int nface = ld32s(blk + 0x14), ro = ld32s(blk + 0x28), first = ncfaces;
        if(ro <= 0 || ncfaces + nface > MAX_CFACES) continue;
        for(int j = 0; j < nface; j++) {
            uint16_t e = *(const uint16_t *)(blk + ro + 2 * j);
            if(e == 0xFFFF) continue;                   /* an end mark, no face */
            const uint8_t *fc = blk + e;
            struct cface *q = &cfaces[ncfaces++];
            int nc = ld16s(fc + 6);
            q->nc = (uint8_t)(nc < 3 ? 3 : nc > 4 ? 4 : nc);
            for(int c = 0; c < q->nc; c++) {
                int vo = ld16s(fc + 8 + 2 * c);
                if(vo < 0) { q->nc = 0; break; }       /* a negative offset is no corner: never over it */
                q->x[c] = ld16s(verts + vo);
                q->z[c] = ld16s(verts + vo + 4);
                if(c == 0) q->v[0] = q->x[0], q->v[1] = ld16s(verts + vo + 2), q->v[2] = q->z[0];
            }
            for(int c = 0; c < 3; c++) q->n[c] = ld16s(fc + 0x14 + 4 * c);
            /* The box, and whether the face is boxed. At each corner, t is the cross product of the
               two edges that meet there. s is the sum over the edges of x[a] z[b] - x[b] z[a], twice
               the face's signed area. The face is boxed if it has an area (s != 0) and is convex
               (the turns at the corners all go one way, and none is straight). */
            q->boxed = 0;
            if(!q->nc) continue;
            int64_t s = 0;
            int pos = 0, neg = 0;
            q->x0 = q->x1 = q->x[0];
            q->z0 = q->z1 = q->z[0];
            for(int c = 0; c < q->nc; c++) {
                int a = c, b = (c + 1) % q->nc, d = (c + 2) % q->nc;
                int64_t t = (int64_t)(q->x[b] - q->x[a]) * (q->z[d] - q->z[b]) -
                            (int64_t)(q->z[b] - q->z[a]) * (q->x[d] - q->x[b]);
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
        cobj_grass[k] = blk[0x6b] == 'e';               /* the fourth letter of the block's name */
    }
    cgrid_build();
    for(int at = 0; at < ground_first[65536] && at < MAX_GROUND; at++) gpiece_make(at, &gpieces[at]);

    /* 3. Per mesh: whether it has several blocks or moving parts, and its reach. */
    for(int k = 0; k < unr_count; k++) {
        const uint8_t *obj, *blk = block_of(k, &obj);
        int nb = ld32s(obj + 0x14), frames = ld32s(obj + 0x10);
        multi[k] = nb > 1 || frames > 0;
        /* The reach round the origin is the largest, over the blocks, of two distances added: the
           farthest vertex of the block, and the farthest the block's place (the position record) or
           its frames can move it from the origin. */
        radius[k] = 0.0f;
        for(int b = 0; b < (multi[k] ? nb : 1) && b < MAX_BLOCKS; b++) {
            blk = obj + ld32s(obj + 0x18 + 4 * b);
            const uint8_t *v = obj + ld32s(blk + 0x1C);
            int n = ld32s(blk + 0x10);
            int32_t so = ld32s(blk + 0x0C);
            float r2 = 0.0f, t2 = 0.0f;
            for(int i = 0; i < n && i < GU_MAX_VERT; i++, v += 6) {
                float x = ld16s(v), y = ld16s(v + 2), z = ld16s(v + 4), d = x * x + y * y + z * z;
                if(d > r2) r2 = d;
            }
            if(multi[k] && so > 0)
                for(int fr = 0; fr < (frames > 0 ? frames : 1); fr++) {
                    float tx, ty, tz;
                    if(frames > 0) {
                        const uint8_t *q = obj + so + 0x1C + 0x30 * fr;
                        tx = ld16s(q + 18); ty = ld16s(q + 20); tz = ld16s(q + 22);
                    } else {
                        tx = (float)ld32s(obj + so + 0x10);
                        ty = (float)ld32s(obj + so + 0x14);
                        tz = (float)ld32s(obj + so + 0x18);
                    }
                    if(tx * tx + ty * ty + tz * tz > t2) t2 = tx * tx + ty * ty + tz * tz;
                }
            float rb = __builtin_sqrtf(r2) + __builtin_sqrtf(t2);
            if(rb > radius[k]) radius[k] = rb;
        }
        class_key[k] = (int16_t)track_class_key(obj);
    }
    /* 4. Per placement, as the culling reads it. */
    for(int i = 0; i < npieces; i++) {
        int k = pieces[i].obj, ok = k >= 0 && k < unr_count;
        cpieces[i] = (struct cpiece){ (float)pieces[i].x, (float)pieces[i].y, (float)pieces[i].h,
                                      ok ? radius[k] : -1.0f };
        cpiece_key[i] = ok ? class_key[k] : 0;
    }
    /* 5. The cell table: the image must have one, its placements must exist, and its keys must
       rise from cell to cell (first_cell searches by key). */
    const uint32_t *vh = (const uint32_t *)(pieces + npieces);
    if(vh[0] != CELL_MAGIC || vh[1] > MAX_CELLS) return 0;
    nvcells = (int)vh[1];
    vcells = (const struct vcell *)(vh + 3);
    for(int g = 0; g < nvcells; g++)
        if(vcells[g].first + vcells[g].count > npieces || (g && vcells[g].key <= vcells[g - 1].key)) return 0;
    return npieces;
}

int track_class_key(const uint8_t *obj)
{
    int32_t cl = ld32s(obj + 0x32C);                    /* (original: STARTL.EXE 0x8a8d7) */
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

/* Advances the animation clock by steps of 0.02 s. The clock runs 0.3 a second and wraps at 1
   (original: STARTL.EXE 0x1fcc4). clock16 is the clock as a 16-bit number; piece_phase takes its
   low 14 bits. */
void track_clock(uint32_t steps)
{
    anim += (float)steps * 0.02f * 0.3f;
    while(anim >= 1.0f) anim -= 1.0f;
    clock16 = (uint32_t)(anim * 65535.0f);
    piece_phase = clock16 & 0x3FFF;
}

int track_bonus_time(void)
{
    return ld32s(file_start + 0x70);
}

int track_camera_hi(void)
{
    return ld32s(file_start + 0x74);
}

const uint8_t *track_backdrop(void)
{
    return file_start + ld32s(file_start + 0x78);
}

int track_ground_colour(void)
{
    return ld32s(file_start + 0x7C) & 255;
}

/*
 * Adds the commands for the backdrop to the blitter list l, as the game copies it (original:
 * STARTL.EXE 0x83a68). The view is the clip, seen from cam.
 * 1. Column: column (2047 - heading) / 2 mod 512 of the picture goes under the view's left edge.
 *    The heading is in the game's units, 2048 a turn, and equals yaw + a quarter turn. So the
 *    picture goes twice round in a turn.
 * 2. Rows: the picture's top row lies at the eye line + 22 - 256 + 128 sin(pitch), and the copy
 *    takes the picture from row |top| on. In every race the top lies below the view's top.
 *    The copy is cut at the view's bottom.
 * 3. A row is read on past its end into the next row, as the game does. The picture's last row
 *    is stored twice for that (tb_gfx.h).
 * 4. The rest of the view is filled with the ground colour.
 * The backdrop stays upright whatever the camera's roll. The game has a leaning version
 * (original: 0x83b44), but it is never called.
 */
void track_backdrop_blit(struct blit_list *l, const struct rs_clip *clip, const struct track_cam *cam)
{
    int w = clip->x1 - clip->x0, h = clip->y1 - clip->y0, eye = EYE_LINE(h);
    int32_t s = view_sin16(view_angle(cam->pitch));             /* the game's sine (original: [0x8228c]) */
    int top = eye + 22 - 256 + ((-54152 * s) >> 7) / -54152;
    int heading = (int)lrintf((cam->yaw + HALF_PI) * (1024.0f / PI));
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

/* The accessors of the race screen section (header 0x68). Its layout is described in tb_hud.h:
   the counts of images and fonts, the image table (12 bytes each), the font table (20 bytes each),
   the two pictures' boxes (8 bytes each), 26 lines of GAME.TXT (32 bytes each), then the pixels. */
const uint8_t *track_hud(void)
{
    return file_start + ld32s(file_start + 0x68);
}

const struct blit_image *track_hud_images(int *n)
{
    const uint8_t *h = track_hud();
    *n = ld32s(h);
    return (const struct blit_image *)(h + 8);
}

const struct blit_font *track_hud_fonts(int *n)
{
    const uint8_t *h = track_hud();
    *n = ld32s(h + 4);
    return (const struct blit_font *)(h + 8 + 12 * ld32s(h));
}

void track_hud_picture(int k, int *first, int *count)
{
    const uint8_t *h = track_hud(), *p = h + 8 + 12 * ld32s(h) + 20 * ld32s(h + 4) + 8 * k;
    *first = ld32s(p);
    *count = ld32s(p + 4);
}

const char *track_hud_text(int k)
{
    const uint8_t *h = track_hud();
    return (const char *)(h + 8 + 12 * ld32s(h) + 20 * ld32s(h + 4) + 16 + 32 * k);
}

void track_start(float *x, float *y, float *yaw)
{
    *x = (float)start->x;
    *y = (float)start->y;
    *yaw = start->yaw;
}

/* track_under's test of face q of placement gp, for the point (lx, lz) in the placement's frame
   (c and s are the cosine and sine of its turn). Returns 1 if the point is over the face, and 0
   if track_under should try the next face. On 1 it sets *grass, and, if a height is asked for
   (h is not 0), *h and under_n. */
static inline int under_face(const struct cface *q, const struct gpiece *gp, float lx, float lz, float c,
                             float s, float *h, int *grass)
{
    /* The comparisons of a test are all asked for before any answer is used (INT_NOW). The answers
       come back side by side, and the CPU waits once. */
    if(q->boxed) {
        int w = lx < q->x0, e = lx > q->x1, s0 = lz < q->z0, n = lz > q->z1;
        INT_NOW4(w, e, s0, n);
        if(w | e | s0 | n) return 0;
    }
    int nc = q->nc;
    if(!nc) return 0;
    /* EDGE(j, b) is the point's side of the edge from corner j to corner b. A triangle has no
       corner 3: its fourth value is worked out but not counted. */
#define EDGE(j, b) \
    ((q->x[b] - (float)q->x[j]) * (lz - (float)q->z[j]) - (q->z[b] - (float)q->z[j]) * (lx - (float)q->x[j]))
    int four = nc > 3;
    float cr0 = EDGE(0, 1), cr1 = EDGE(1, 2), cr2 = EDGE(2, four ? 3 : 0), cr3 = EDGE(3, 0);
#undef EDGE
    int p0 = cr0 > 0.0f, p1 = cr1 > 0.0f, p2 = cr2 > 0.0f, p3 = cr3 > 0.0f;
    int n0 = cr0 < 0.0f, n1 = cr1 < 0.0f, n2 = cr2 < 0.0f, n3 = cr3 < 0.0f;
    INT_NOW8(p0, p1, p2, p3, n0, n1, n2, n3);
    int pos = p0 + p1 + p2 + (four & p3), neg = n0 + n1 + n2 + (four & n3);
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

/* What lies under the point (x, y); the search is described at track_under in track.h. The nine
   grid cells round the point are searched in the order of the table cell[] (the point's cell first). */
int track_under(float x, float y, float *h, int *grass)
{
    static const int8_t cell[9][2] = { {0,0}, {1,0}, {-1,0}, {0,1}, {0,-1}, {1,1}, {-1,-1}, {1,-1}, {-1,1} };
    int cx = ((int)x + 0x100000) >> 13, cy = ((int)y + 0x100000) >> 13;
    /* near is 0 for a point far off the map or not a number. The grids are not used for it:
       all the faces of a mesh are tested. */
    int near = (fabsf(x) < 4.0e6f) & (fabsf(y) < 4.0e6f);
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
            float lx = dx * c + dy * s, lz = -dx * s + dy * c;  /* the point in the piece's frame */
            const struct cface *f0 = &cfaces[cobj_first[gp->obj]], *fe = f0 + cobj_count[gp->obj];
            const struct cgrid *g = &cgrid[gp->obj];
            for(int tries = 0; tries < 3; tries++, lx += 10.0f, lz += 10.0f)
                if(g->at[0] >= 0 && near) {
                    int iz = (int)((lz - g->z0) * g->sz), ix = (int)((lx - g->x0) * g->sx);
                    INT_NOW2(iz, ix);                   /* the two numbers of cgrid_cell, asked for at once */
                    int k = cgrid_clamp(iz) * CGRID + cgrid_clamp(ix);
                    for(const uint16_t *li = clist + g->at[k], *le = clist + g->at[k + 1]; li < le; li++)
                        if(under_face(f0 + *li, gp, lx, lz, c, s, h, grass)) return 1;
                } else
                    for(const struct cface *q = f0; q < fe; q++)
                        if(under_face(q, gp, lx, lz, c, s, h, grass)) return 1;
        }
    }
    return 0;
}

/* How the game stands a thing on the face that track_under found (original: STARTL.EXE 0x1cb9c,
   for an opponent). The thing heads along (fx, fy) = (sin yaw, -cos yaw).
   1. The face's normal is turned into the thing's frame.
   2. Each level part of the normal, in units of 1/16384, times pi / 46080 is an angle. This is
      1.117 x the slope.
   Pitch is + nose up, roll is + right side up. */
void track_under_tilt(float fx, float fy, float *pitch, float *roll)
{
    float nx = under_n[0], ny = under_n[1], nz = under_n[2];
    float len = sqrtf(nx * nx + ny * ny + nz * nz);
    if(nz < 0.0f) len = -len;
    if(len != 0.0f) { nx /= len; ny /= len; }
    *pitch = -1.117f * (nx * fx + ny * fy);
    *roll = 1.117f * (nx * -fy + ny * fx);
}

/* The opponents section (header 0x80). Its layout is described at tb_opponents in tb_spl.h: the
   number of splines (the result) and of rows (*rows), the rows (20 bytes each), then per spline
   its start node, its node count and its nodes (32 bytes each). */
int track_opp_count(int *rows)
{
    const uint8_t *o = file_start + ld32s(file_start + 0x80);
    *rows = ld32s(o + 4);
    return ld32s(o);
}

const struct track_opp_row *track_opp_rows(void)
{
    return (const struct track_opp_row *)(file_start + ld32s(file_start + 0x80) + 8);
}

const struct track_spline_node *track_opp_spline(int k, int *first, int *n)
{
    const uint8_t *o = file_start + ld32s(file_start + 0x80), *p = o + 8 + 20 * ld32s(o + 4);
    for(int j = 0; j < k; j++) p += 8 + 32 * ld32s(p + 4);
    *first = ld32s(p);
    *n = ld32s(p + 4);
    return (const struct track_spline_node *)(p + 8);
}

/* The camera's frame (track_draw_cam): forward f, right r, up u, as unit vectors in the world. */
static void cam_frame(const struct track_cam *cam, float f[3], float r[3], float u[3])
{
    /* The frame of the last angles is kept, because the effects and the scene ask for the frame
       of the same camera. For the same bits of yaw, pitch and roll the values are the same, and
       the six sines and cosines are saved. */
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
    /* First the level frame: level right = forward x up, level up = right x forward. Then the
       roll about forward. */
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

/* Sets the DETAIL option (0 LOW, 1 MID, 2 HI; other values are cut to this range), and with it
   the far plane and the reach. */
void track_detail(int level)
{
    detail_level = level < 0 ? 0 : level > 2 ? 2 : level;
    far_z = far_depth[detail_level] * far_scale;
    reach2 = (REACH * far_scale) * (REACH * far_scale);
}

/* Sets the two factors in force (see FAR_SCALE and LOD_SCALE in track.h). */
void track_set_scales(float far, float lod)
{
    far_scale = far;
    lod_scale = lod;
    lod_inv = lod_scale_inv(lod);
    track_detail(detail_level);                 /* sets the far plane and the reach for the new factor */
}

float track_far_scale(void)
{
    return far_scale;
}

float track_lod_scale(void)
{
    return lod_scale;
}

float track_far(void)
{
    return far_z;
}

/* The level of detail of a block: 0 (the finest), 1 or 2. It is the original's rule (original:
   STARTL.EXE 0x8b04c for placed blocks, 0x8af24 for cars and the other moving objects).
   1. dist is the distance of the block's middle from the camera. It is divided by LOD_SCALE first,
      which moves all the switches out by that factor. The result is rounded to nearest (even).
   2. A placed block has the key -(dist >> 1), plus 0x2000 if its name starts with Z (zname).
      Level 0 above -6000, level 1 above -8000 (the distances 12,000 and 16,000), else level 2.
   3. A moving block has the key -dist. Level 0 above -4000, level 1 above -7000, else level 2. */
static int lod_level(float dist, int moving, int zname)
{
    int32_t d = (int32_t)lrintf(dist * lod_inv);
    int32_t key = moving ? -d : -(d >> 1) + (zname ? 0x2000 : 0);
    return key > (moving ? -4000 : -6000) ? 0 : key > (moving ? -7000 : -8000) ? 1 : 2;
}

/* The block of a mesh to draw for block b; see track_lod_block in track.h. The mesh header has
   one table of block offsets per level, 0x104 bytes apart. */
const uint8_t *track_lod_block(const uint8_t *mesh, int b, float dist, int moving)
{
    const uint8_t *blk = mesh + ld32s(mesh + 0x18 + 4 * b);
    if(lod_inv == 0.0f) return blk;
    int level = lod_level(dist, moving, !moving && blk[0x68] == 'Z');
    if(!level) return blk;
    int32_t off = ld32s(mesh + 0x18 + 4 * b + 0x104 * level);      /* block b at the level, 0: none */
    return off ? mesh + off : 0;
}

int track_blocks(void)
{
    return nblocks;
}

/* Whether CRC block k of the image (TRACK_CRC_BLOCK bytes from offset 0x100 on; the last one may be shorter)
   has the CRC-32 that the header holds at 0x90 + 4 k. */
int track_block_ok(int k)
{
    uint32_t o = 0x100 + (uint32_t)k * TRACK_CRC_BLOCK,
             e = o + TRACK_CRC_BLOCK < file_size ? o + TRACK_CRC_BLOCK : file_size;
    return o < e && crc32(file_start + o, file_start + e) == (uint32_t)ld32s(file_start + 0x90 + 4 * k);
}

void track_view(const struct rs_clip *clip, struct su_view *v)
{
    /* The centre in 1/16 pixel. On the full screen it is column 159, as in the game's projection
       (original: STARTL.EXE 0x83431). */
    v->cx = (clip->x0 + clip->x1) * 8 - 16;
    v->cy = (clip->y0 + EYE_LINE(clip->y1 - clip->y0)) * 16;
    v->scale = (uint32_t)(FOCAL * 16.0f);
    v->far8 = (int32_t)((far_z + EYE_BACK) * 256.0f);  /* depths from the eye, 8 fraction bits */
}

/* The number of the first cell whose key is not below key, or nvcells if there is none (a binary
   search: the cells lie in the order of their keys). */
static int first_cell(uint32_t key)
{
    int lo = 0, hi = nvcells;
    while(lo < hi) {
        int mid = (lo + hi) / 2;
        if(vcells[mid].key < key) lo = mid + 1; else hi = mid;
    }
    return lo;
}

/* Which cells of the map to draw: a window of 8192-unit cells round the camera at (cx, cy).
   It follows the game's walk (original: STARTL.EXE 0x82e88, docs/original_geometry.md 2.1):
   1. Two points ahead of the camera, to its left and right: (-WINDOW_X, -WINDOW_Z) and
      (WINDOW_X, -WINDOW_Z), times FAR_SCALE. They are turned by the camera's heading h, in 2048ths
      of a turn, cut towards 0. The sine and cosine are those of 0x7ff - h, which the game's
      table has.
   2. The box round these points and the 8192 units round the camera, moved to the camera's
      place and clamped to the map.
   3. At FAR_SCALE 1.0 or less, the box is cut to the one that the map gives the camera's cell
      (array 4 of the cell), if it has one. Each edge of that box is clamped to the window. The
      x edges take the window's place only if they differ. Only then the z edges are looked at,
      and they take the window's place only if they differ.
   4. The window in cells of 8192 units: the rows start at row0 and the columns at col0, the
      box's first cell. They run on for as many cells as the box is long (n further), and for one
      more if the first cell plus n is at most 0xfd.
   The result is rows row0 .. row1 and columns col0 .. col1, both ends included. */
static __attribute__((noinline)) void cell_window(float cx, float cy, float yaw, int *row0, int *row1,
                                                  int *col0, int *col1)
{
    const int h = (int)(yaw * 1024.0f / PI) & 0x7FF;
    const int a = 0x7FF - h;
    const int64_t s = view_sin16(a), c = view_sin16((a + 512) & 2047);
    const int64_t px = (int64_t)(WINDOW_X * far_scale + 0.5f), pz = (int64_t)(WINDOW_Z * far_scale + 0.5f);
    int64_t xa = (-px * c - pz * s) >> 14, za = (-pz * c + px * s) >> 14;      /* the point to the left */
    int64_t xb = (px * c - pz * s) >> 14, zb = (-pz * c - px * s) >> 14;       /* and to the right */
    int64_t x0 = xa < xb ? xa : xb, x1 = xa < xb ? xb : xa,
            z0 = za < zb ? za : zb, z1 = za < zb ? zb : za;
    if(x0 > -0x2000) x0 = -0x2000;
    if(z0 > -0x2000) z0 = -0x2000;
    if(x1 < 0x2000) x1 = 0x2000;
    if(z1 < 0x2000) z1 = 0x2000;
    const int32_t ix = (int32_t)cx, iz = (int32_t)cy;
    x0 += ix; x1 += ix; z0 += iz; z1 += iz;
    if(x0 < -0x100000) x0 = -0x100000;
    if(x1 > 0xFFFFF) x1 = 0xFFFFF;
    if(z0 < -0x100000) z0 = -0x100000;
    if(z1 > 0xFFFFF) z1 = 0xFFFFF;
    x0 += 0x100000; x1 += 0x100000; z0 += 0x100000; z1 += 0x100000;
    if(ix >= -0x100000 && ix < 0x100000 && iz >= -0x100000 && iz < 0x100000) {      /* the camera's cell */
        uint32_t key = (uint32_t)((iz + 0x100000) >> 13) << 8 | (uint32_t)((ix + 0x100000) >> 13);
        int g = first_cell(key);
        /* The map's box is the game's cut for its own far plane. When the core draws farther, the
           box's edge lies inside the part that is drawn. The edge moves in steps of a cell as the
           camera crosses cells, and the window's rows round it by the length of the window. A
           building at the edge is then drawn in one frame and not in the next. So the box is used
           only at the original's own distance, FAR_SCALE 1.0 or less. Farther, the window is the
           whole box of the view. */
        if(far_scale <= 1.0f && g < nvcells && vcells[g].key == key && vcells[g].has_box) {
            const int32_t *b = vcells[g].box;
            int64_t ex0 = b[0] + 0x100000, ex1 = b[2] + 0x100000;
            ex0 = ex0 < x0 ? x0 : ex0 > x1 ? x1 : ex0;
            ex1 = ex1 < x0 ? x0 : ex1 > x1 ? x1 : ex1;
            if(ex0 != ex1) {
                int64_t ez0 = b[1] + 0x100000, ez1 = b[3] + 0x100000;
                ez0 = ez0 < z0 ? z0 : ez0 > z1 ? z1 : ez0;
                ez1 = ez1 < z0 ? z0 : ez1 > z1 ? z1 : ez1;
                x0 = ex0;
                x1 = ex1;
                if(ez0 != ez1) {
                    z0 = ez0;
                    z1 = ez1;
                }
            }
        }
    }
    *row0 = (int)(z0 >> 13);
    *col0 = (int)(x0 >> 13);
    int nrow = (int)((z1 > z0 ? z1 - z0 : z0 - z1) >> 13), ncol = (int)((x1 > x0 ? x1 - x0 : x0 - x1) >> 13);
    if(*row0 + nrow <= 0xFD) nrow++;
    if(*col0 + ncol <= 0xFD) ncol++;
    *row1 = *row0 + nrow;
    *col1 = *col0 + ncol;
}

/* The list of the pieces to draw, sorted far to near. Each entry is key << 16 | i, with i (the
   low half) either the number of a placement, or npieces + the number of an object. */
#define MAX_OBJS    64
static int32_t vis[MAX_PIECES + MAX_OBJS];

/* The faces of a block as the game takes them (original: STARTL.EXE 0x834d6 .. 0x83547). Sets
   g->row, the face row, and g->e, the eye in the block's frame (8 fraction bits). It reads g->m;
   its row f is the view's forward in the block's frame. (x, y, z) is the camera in the block's frame.
   1. The point p = the camera in the block's frame plus the block's middle picks the face row
      (original: 0x8a598). The game scales p by 1.2 (imul 0x13333), which leaves the row as it is.
   2. The row is 8 more when p is below the block's x-z plane. It also depends on the 45-degree
      sector of p round the block's y axis. On a diagonal the row next to the x axis is taken.
   3. The game's normal test (original: 0x835a0) takes the same point p. But the game's polygon
      filler (original: 0xd55ff) draws nothing of a face whose corners run the other way round on
      the screen. The filler walks the corners forwards along one edge and backwards along the
      other, and it draws a span only where the first edge is right of the second (0xd5969,
      0xd59eb). Such a face is turned away from the lens's eye, which is EYE_BACK behind the
      camera.
   4. With the camera point alone, a car's nose shows through its front tyre on the car screen
      (AAUTO00 fauto). So the normal test here takes the lens's eye. The faces that it sees and
      that the camera point leaves out (the game draws neither) changed no pixel in the frames
      checked. */
static void face_eye(const uint8_t *blk, float x, float y, float z, struct gu_piece *g)
{
    const float e[3] = { x, y, z };
    float p[3] = { x + (float)ld16s(blk + 4), y + (float)ld16s(blk + 6), z + (float)ld16s(blk + 8) };
    for(int i = 0; i < 3; i++)
        g->e[i] = (int32_t)((e[i] - (float)g->m[6 + i] * (EYE_BACK / 65536.0f)) * 256.0f);
    /* All the comparisons come first, without branches: the FPU answers them side by side,
       where a branch on each would wait for the answer. */
    int below = p[1] < 0.0f, xpos = p[0] >= 0.0f, zpos = p[2] >= 0.0f, zneg = p[2] < 0.0f;
    int c0 = p[2] > p[0], c2 = -p[2] <= p[0], c4 = -p[2] > -p[0], c6 = p[2] <= -p[0];
    int row = xpos ? (zpos ? !c0 : 2 + !c2) : (zneg ? 4 + !c4 : 6 + !c6);
    g->row = below * 8 + row;
}

/* Makes the geometry unit's piece g from a block placed at o. m takes the block's local axes to
   the world (rows: world x, y, height; columns: the block's x, y up, z). c is the camera, and f,
   r, u are its forward, right and up. The piece holds the block's place in the view from the eye
   (EYE_BACK further along forward), and its faces' row, which is picked from the camera. */
static void block_piece(const float c[3], const float f[3], const float r[3], const float u[3],
                        const float m[9], const float o[3], const uint8_t *blk, struct gu_piece *g)
{
    const float *basis[3] = { r, u, f };
#pragma GCC unroll 3                            /* the conversions run side by side in the FPU */
    for(int k = 0; k < 3; k++) {
#pragma GCC unroll 3
        for(int j = 0; j < 3; j++)
            g->m[3 * k + j] = (int32_t)((basis[k][0] * m[j] + basis[k][1] * m[3 + j] +
                                         basis[k][2] * m[6 + j]) * 65536.0f);
        g->t[k] = (int32_t)((basis[k][0] * (o[0] - c[0]) + basis[k][1] * (o[1] - c[1]) +
                             basis[k][2] * (o[2] - c[2]) + (k == 2 ? EYE_BACK : 0.0f)) * 256.0f);
    }
    float e[3];
    for(int j = 0; j < 3; j++)
        e[j] = m[j] * (c[0] - o[0]) + m[3 + j] * (c[1] - o[1]) + m[6 + j] * (c[2] - o[2]);
    face_eye(blk, e[0], e[1], e[2], g);
    g->blk = blk;
    g->mirror = 0;
    g->refl = 0;
}

/* One block as a piece for the geometry unit, for a model drawn without a track (see track.h). */
void track_block_piece(const struct track_cam *cam, const struct track_block *b, struct gu_piece *g)
{
    const float c[3] = { cam->x, cam->y, cam->h };
    float f[3], r[3], u[3];
    cam_frame(cam, f, r, u);
    block_piece(c, f, r, u, b->m, b->o, b->blk, g);
    g->obj = b->file;
    g->phase = b->phase;
}

/* The matrix m and the origin o, in the world, of a block of the piece pc. so is the block's frames or position
   record (<= 0: none) and R the world from the piece's frame. Frame fr (>= 0) of the block's frames turns and
   places it; with fr < 0 the block is only moved by its position record. */
static void block_pose(const uint8_t *obj, int32_t so, int fr, const float R[9], const struct piece *pc,
                       float m[9], float o[3])
{
    float F[9] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f }, t[3] = { 0.0f, 0.0f, 0.0f };
    if(so > 0 && fr >= 0) {                         /* F: rows x, up, z; the words run down its columns */
        const uint8_t *q = obj + so + 0x1C + 0x30 * fr;
        for(int i = 0; i < 3; i++)
            for(int j = 0; j < 3; j++) F[3 * i + j] = (float)ld16s(q + 2 * (3 * j + i)) / 16383.0f;
        for(int i = 0; i < 3; i++) t[i] = (float)ld16s(q + 18 + 2 * i);
    } else if(so > 0)
        for(int i = 0; i < 3; i++) t[i] = (float)ld32s(obj + so + 0x10 + 4 * i);
    for(int i = 0; i < 3; i++) {
        for(int j = 0; j < 3; j++)
            m[3 * i + j] = R[3 * i] * F[j] + R[3 * i + 1] * F[3 + j] + R[3 * i + 2] * F[6 + j];
        o[i] = (i == 0 ? (float)pc->x : i == 1 ? (float)pc->y : (float)pc->h) +
               R[3 * i] * t[0] + R[3 * i + 1] * t[1] + R[3 * i + 2] * t[2];
    }
}

/* Makes the pieces of a placement whose mesh has several blocks or moving parts (an example is
   track 1's football helmet V220: its base, and the helmet that turns on it). c, f, r, u are the
   camera and its frame. The pieces go into gs (room for MAX_BLOCKS); the result is their number.
   For each block:
   1. A block with frames is turned by its frame (original: STARTL.EXE 0x8ad14). A frame's 12 words
      are the block's whole matrix, with 16383 = 1, and then its place. The vertex as stored is
      multiplied as a row into the matrix. The frame is frames x clock >> 16, so the clock makes one
      turn in 3.33 s. The frames turn z up into y up, which is a mirror (gu_piece.mirror).
   2. A block without frames is moved only by its place (the position record).
   3. The block at its level of detail is drawn; a block that has no mesh at its level is left out.
   The blocks come out with the furthest block's middle first (original: 0x8ac58). The middle that counts is the
   one in the block's first frame, its rest pose: the turn of the frames moves the block that is drawn, but not
   its place in the order. The sails of track 3's windmills turn about an axis that does not pass through their
   middle; with the turned middle they would pass the tower's place in the order twice a turn and go behind
   it. In the original they stay in front of it, from every side and at every turn (measured in DOSBox). */
static int multi_pieces(const float c[3], const float f[3], const float r[3], const float u[3],
                        const struct piece *pc, struct gu_piece *gs)
{
    const uint8_t *obj = unr_blob + unr_off[pc->obj];
    int nb = ld32s(obj + 0x14), frames = ld32s(obj + 0x10), n = 0, ord[MAX_BLOCKS];
    float key[MAX_BLOCKS];
    struct gu_piece tmp[MAX_BLOCKS];
    float ca = sin2048((uint32_t)pc->t + 512), sa = sin2048((uint32_t)pc->t);
    const float R[9] = { ca, 0.0f, -sa, sa, 0.0f, ca, 0.0f, 1.0f, 0.0f };   /* world from the piece's frame */
    int fr = frames > 0 ? (int)(((uint32_t)frames * clock16) >> 16) : -1;      /* the frame; -1: no frames */
    for(int b = 0; b < nb && b < MAX_BLOCKS; b++) {
        const uint8_t *blk = obj + ld32s(obj + 0x18 + 4 * b);
        int32_t so = ld32s(blk + 0x0C);             /* the block's frames or position record; <= 0: none */
        int moving = so > 0 && fr >= 0;
        float m[9], o[3];                           /* the block's matrix and origin in this frame */
        block_pose(obj, so, fr, R, pc, m, o);
        float m0[9], o0[3];                         /* and in its first frame, for the place in the order */
        const float *mk = m, *ok = o;
        if(moving) {
            block_pose(obj, so, 0, R, pc, m0, o0);
            mk = m0;
            ok = o0;
        }
        float w[3];                                 /* the block's middle less the camera */
        for(int i = 0; i < 3; i++)
            w[i] = ok[i] + mk[3 * i] * ld16s(blk + 4) + mk[3 * i + 1] * ld16s(blk + 6) +
                   mk[3 * i + 2] * ld16s(blk + 8) - c[i];
        key[b] = -(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        /* The block at its level of detail. The middle and the key stay those of level 0. */
        const uint8_t *lb = track_lod_block(obj, b, sqrtf(-key[b]), 0);
        if(!lb) continue;
        struct gu_piece *g = &tmp[b];
        block_piece(c, f, r, u, m, o, lb, g);
        g->obj = obj;
        g->phase = piece_phase;
        g->mirror = moving;
        int i = n;                                  /* insert b into ord, farthest first */
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

/* The reflection's offsets of an object, worked out before the faces of its first block with
   flag 0x2000 (original: STARTL.EXE 0xc1fc0).
   1. The object's way since it was last drawn. The record keeps the place in whole units.
   2. The way is turned into the view, whose axes in the original are x left, y up, z back.
   3. It is multiplied by the rate (refl_rate) and added to the object's three accumulators.
   4. Each sum is rounded to nearest and cut to 8 bits. The first and the third are the offsets
      of u and v, in bits [23:16] and [31:24] of gu_piece.refl.
   f, r, u are the camera's frame. */
static uint32_t refl_offsets(const struct track_object *ob, const float f[3], const float r[3],
                             const float u[3])
{
    struct track_refl *s = ob->refl;
    int32_t p[3] = { (int32_t)ob->x, (int32_t)ob->y, (int32_t)ob->h };
    float d[3] = { (float)(p[0] - s->prev[0]), (float)(p[1] - s->prev[1]), (float)(p[2] - s->prev[2]) };
    int32_t n[3];
    if(d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f) {
        const float w[3] = { -(r[0] * d[0] + r[1] * d[1] + r[2] * d[2]),
                             u[0] * d[0] + u[1] * d[1] + u[2] * d[2],
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

/* Draws the view from the point c along forward f, with right r and up u (unit vectors). yaw is
   the camera's heading, which the cell window needs. objs holds the nobj objects (cars and the
   like) that are drawn among the placements. st receives the counts and the CPU cycles.
   The drawing has three steps:
   1. Culling: the placements of the cells in the game's window (cell_window) and the objects are
      tested against the reach and the view. The ones that pass go into vis.
   2. Sorting: vis is sorted by key, far first.
   3. Drawing: each entry of vis is handed to the geometry unit, in that order. With TRACK_HW
      this is the hook track_hw_piece, otherwise the C models draw into fb. */
static void draw(volatile uint8_t *fb, const struct rs_clip *clip, const struct track_cam *cam, float yaw,
                 const float c[3], const float f[3], const float r[3], const float u[3],
                 struct track_object *objs, int nobj, struct track_stats *st)
{
    uint32_t t_start = rdcycle();
    st->pieces = st->faces = st->triangles = st->see_through = st->vertices = st->objects = 0;
    const float cx = c[0], cy = c[1], ch = c[2];
    const float fx = f[0], fy = f[1], fh = f[2], rx = r[0], ry = r[1], rh = r[2];
    const float ux = u[0], uy = u[1], uh = u[2];
    st->cam[0] = (int32_t)cx;
    st->cam[1] = (int32_t)cy;
    st->cam[2] = (int32_t)ch;

    /* Step 1, culling. For a piece at the offset (dx, dy, dh) from the camera, vf is its depth from
       the eye, vr its distance to the right of the view's axis and vu its distance above it.
       half_w, up and down are the tangents of the view's half angles: right and left, above and
       below the eye line. */
    const float far_eye = far_z + EYE_BACK;
    const float half_w = (float)(clip->x1 - clip->x0) / 2.0f / FOCAL;
    const int eye = EYE_LINE(clip->y1 - clip->y0);
    const float up = (float)eye / FOCAL;                                       /* above the eye line */
    const float down = (float)(clip->y1 - clip->y0 - eye) / FOCAL;
    int nv = 0;
    /* The game's key (original: STARTL.EXE 0x8a7b0) is -(sqrt(dist^2 >> 2), rounded, >> 2). */
#define KEY(d2) (-((int32_t)lrintf(sqrtf((d2) * 0.25f)) >> 2))
    /* The cells to look at are those that the game's window covers (cell_window). They are taken
       in the order of the file, so the pieces come in the order of the game's list:
       1. The cells of the rows row0 .. row1 are a range of the cell table. Two binary searches
          find the range (first_cell).
       2. Within the range, a cell is skipped if its column is not in col0 .. col1. */
    int row0, row1, col0, col1;
    cell_window(cx, cy, yaw, &row0, &row1, &col0, &col1);
    const int q0 = row0 > 0 ? first_cell((uint32_t)row0 << 8) : 0,
              q1 = row1 < 255 ? first_cell((uint32_t)(row1 + 1) << 8) : nvcells;
    for(int g = q0; g < q1; g++) {
        {
            int col = vcells[g].key & 255;
            if(col < col0 || col > col1) continue;
        }
        for(int i = vcells[g].first, end = i + vcells[g].count; i < end; i++) {
            const struct cpiece *pc = &cpieces[i];
            int32_t no_mesh;                            /* the reach, -1 without a mesh, as a whole number */
            memcpy(&no_mesh, &pc->rad, sizeof no_mesh);
            if(no_mesh < 0) continue;
            float rad = pc->rad, dx = pc->x - cx, dy = pc->y - cy, dh = pc->h - ch;
            float d2 = dx * dx + dy * dy + dh * dh;
            float vf = dx * fx + dy * fy + dh * fh + EYE_BACK, vr = dx * rx + dy * ry + dh * rh;
            float vu = dx * ux + dy * uy + dh * uh;
            /* Out of reach (o0), or out of view: nearer than the near plane (o1), farther than the far
               plane (o2), beyond the right (o3), left (o4), upper (o5) or lower (o6) side plane. The reach
               rad of the piece is the margin. All the comparisons are asked for at once, so that the CPU
               waits once for the answers. */
            int o0 = d2 >= reach2, o1 = vf + rad < NEAR, o2 = vf - rad > far_eye;
            int o3 = vr - rad > half_w * (vf + rad), o4 = vr + rad < -half_w * (vf + rad);
            int o5 = vu - rad > up * (vf + rad), o6 = vu + rad < -down * (vf + rad);
            INT_NOW7(o0, o1, o2, o3, o4, o5, o6);
            if(o0 | o1 | o2 | o3 | o4 | o5 | o6) continue;
            vis[nv++] = (KEY(d2) + cpiece_key[i]) * 65536 + i;
        }
    }
    /* The objects get the same view tests, but no test of the reach. */
    for(int k = 0; k < nobj && k < MAX_OBJS; k++) {
        const struct track_object *ob = &objs[k];
        float dx = ob->x - cx, dy = ob->y - cy, dh = ob->h - ch, rad = ob->radius;
        float vf = dx * fx + dy * fy + dh * fh + EYE_BACK, vr = dx * rx + dy * ry + dh * rh;
        float vu = dx * ux + dy * uy + dh * uh;
        int o1 = vf + rad < NEAR, o2 = vf - rad > far_eye, o3 = vr - rad > half_w * (vf + rad);
        int o4 = vr + rad < -half_w * (vf + rad), o5 = vu - rad > up * (vf + rad),
            o6 = vu + rad < -down * (vf + rad);
        INT_NOW6(o1, o2, o3, o4, o5, o6);               /* one wait for the answers, as above */
        if(o1 | o2 | o3 | o4 | o5 | o6) continue;
        float d2 = dx * dx + dy * dy + dh * dh;
        /* An object counts 800 nearer (original: STARTL.EXE 0x8a8c3). The key also holds the class
           of its mesh (0x8a8d7). */
        vis[nv++] = (KEY(d2) + 800 + ob->key) * 65536 + npieces + k;
        st->objects++;
    }
    /* Step 2, sorting by key, far first.
       1. vis is in the order of i, the low half of an entry. The low halves are all different, so
          the entries are all different.
       2. A sort by the high half (the key) that keeps the order of equal keys puts the list in
          the order of the whole entry, as any sort by the entry has it.
       The sort is a radix sort with two passes of 8 bits. XOR 0x80000000 makes the signed entries
       sort as unsigned numbers. */
    static int32_t vis2[MAX_PIECES + MAX_OBJS];
    /* cnt holds the counts of both passes, made in one loop. The sums below clear them, so there is
       no memset (picolibc's goes byte by byte). */
    static int cnt[2][256];
    for(int n = 0; n < nv; n++) {
        uint32_t e = (uint32_t)vis[n] ^ 0x80000000u;
        cnt[0][e >> 16 & 255]++;
        cnt[1][e >> 24]++;
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
    uint32_t t_pieces = rdcycle();
    st->cyc_cull = t_pieces - t_start;
#ifdef TRACK_DUMP                               /* the PC renderer (tools/render.sh): prints the list drawn */
    for(int k = 0; k < nv; k++)
        if((vis[k] & 0xFFFF) < npieces)
            printf("drawn piece obj %d at %d %d %d\n", (int)pieces[vis[k] & 0xFFFF].obj,
                   (int)pieces[vis[k] & 0xFFFF].x, (int)pieces[vis[k] & 0xFFFF].h,
                   (int)pieces[vis[k] & 0xFFFF].y);
        else
            printf("drawn object at %.0f %.0f %.0f\n", objs[(vis[k] & 0xFFFF) - npieces].x,
                   objs[(vis[k] & 0xFFFF) - npieces].h, objs[(vis[k] & 0xFFFF) - npieces].y);
#endif

    /* Step 3, drawing. Each entry of vis is an object, a placement with several blocks, or a
       single-block placement. */
    struct su_view view;
    struct gu_stats gst = { 0, 0, 0, 0 };
    track_view(clip, &view);
    uint32_t t_out = 0;
    for(int n = 0; n < nv; n++) {
        const int vi = vis[n] & 0xFFFF;
        if(vi >= npieces) {                         /* an object: its blocks in the order they are listed */
            struct track_object *ob = &objs[vi - npieces];
            if(ob->make) ob->make(ob->id, cam, ob);     /* an object with make has its blocks made here */
            /* The reflection word: the reflection picture's phase, plus the offsets if the object has
               a reflection state and one of its blocks has the flag 0x2000 (blk +0x18). */
            uint32_t refl = ob->refl_phase & 0xFFFF;
            for(int b = 0; b < ob->n && ob->refl; b++)
                if(ld32s(ob->b[b].blk + 0x18) & 0x2000) {
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
                uint32_t t0 = rdcycle();
                if(track_mode == TRACK_HW) track_hw_piece(&g);
                else gu_draw(fb, clip, &view, mlb, &g, &gst);
                t_out += rdcycle() - t0;
            }
            continue;
        }
        const struct piece *pc = &pieces[vi];
        if(multi[pc->obj]) {
            struct gu_piece gs[MAX_BLOCKS];
            int nb = multi_pieces(c, f, r, u, pc, gs);
            for(int b = 0; b < nb; b++) {
                st->pieces++;
                uint32_t t0 = rdcycle();
                if(track_mode == TRACK_HW) track_hw_piece(&gs[b]);
                else gu_draw(fb, clip, &view, mlb, &gs[b], &gst);
                t_out += rdcycle() - t0;
            }
            continue;
        }
        /* A placement with a single block. ca and sa are the cosine and sine of its turn. */
        struct gu_piece g;
        g.phase = piece_phase;
        g.mirror = 0;
        g.refl = 0;
        g.blk = block_of(pc->obj, &g.obj);
        float ca = sin2048((uint32_t)pc->t + 512), sa = sin2048((uint32_t)pc->t);
        float dx = pc->x - cx, dy = pc->y - cy, dh = pc->h - ch;

        /* The level of detail is picked by the distance of the block's middle (that of level 0) from
           the camera. The matrices, the key and the place in the sort stay those of level 0. A block
           for which the mesh has no block at its level is not drawn. */
        if(lod_inv != 0.0f) {
            float lx = ld16s(g.blk + 4), ly = ld16s(g.blk + 6), lz = ld16s(g.blk + 8);
            float wx = dx + lx * ca - lz * sa, wy = dy + lx * sa + lz * ca, wh = dh + ly;
            g.blk = track_lod_block(g.obj, 0, sqrtf(wx * wx + wy * wy + wh * wh), 0);
            if(!g.blk) continue;
        }

        /* Local to view. D is the placement's place less the camera, and x, y, z are a vertex of the
           block. For each view axis A (right, up, forward) the vertex has the coordinate
               A . D + x (Ax cos + Ay sin) + y Ah + z (Ay cos - Ax sin).
           m holds the three factors of x, y, z for each axis, and o the part A . D (for forward plus
           EYE_BACK). */
        const float m[9] = { rx * ca + ry * sa, rh, ry * ca - rx * sa,
                             ux * ca + uy * sa, uh, uy * ca - ux * sa,
                             fx * ca + fy * sa, fh, fy * ca - fx * sa };
        const float o[3] = { dx * rx + dy * ry + dh * rh, dx * ux + dy * uy + dh * uh,
                             dx * fx + dy * fy + dh * fh + EYE_BACK };
#pragma GCC unroll 9                            /* the conversions run side by side in the FPU */
        for(int k = 0; k < 9; k++) g.m[k] = (int32_t)(m[k] * 65536.0f);
        for(int k = 0; k < 3; k++) g.t[k] = (int32_t)(o[k] * 256.0f);

        /* face_eye takes the camera in the piece's frame. It picks the face row, and the lens's eye
           tells which faces are turned away. */
        face_eye(g.blk, -dx * ca - dy * sa, -dh, dx * sa - dy * ca, &g);
        st->pieces++;
        uint32_t t0 = rdcycle();
        if(track_mode == TRACK_HW) track_hw_piece(&g);
        else gu_draw(fb, clip, &view, mlb, &g, &gst);
        t_out += rdcycle() - t0;
    }
    st->faces = gst.faces;
    st->triangles = gst.triangles;
    st->see_through = gst.see_through;
    st->vertices = gst.vertices;
    st->cyc_out = t_out;                        /* handing the pieces on (the hook or the C models) */
    st->cyc_piece = rdcycle() - t_pieces - t_out; /* each piece's place in view space, the rest of the loop */
}

/* Draws the view from the camera; the arguments are described at track_draw_cam in track.h. */
void track_draw_cam(volatile uint8_t *fb, const struct rs_clip *clip, const struct track_cam *cam,
                    struct track_object *objs, int nobj, struct track_stats *st)
{
    const float c[3] = { cam->x, cam->y, cam->h };
    float f[3], r[3], u[3];
    cam_frame(cam, f, r, u);
    draw(fb, clip, cam, cam->yaw, c, f, r, u, objs, nobj, st);
}
