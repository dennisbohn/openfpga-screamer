/*
 * The track image's map sections, made from MAPn.MMP (tb_mmp.h; the file is in GFX/MAPnH.DAT).
 * MAPn.MMP is the map: a grid of 256 x 256 cells, each cell with its placements (the records),
 * road points, walls and view box. Entry points:
 *   tb_mmp_placements   the placements, and the cell table that ends them
 *   tb_mmp_ground       the ground index of the drivable placements
 *   tb_mmp_dirs         the road points
 *   tb_mmp_walls        the walls
 *   tb_mmp_road_angle   the heading of the road point nearest to a place (the start heading)
 * Every offset and count read from the file is checked against its size before anything behind
 * it is read.
 */
#include "tb_mmp.h"
#include "bytes.h"
#include <string.h>

#define GRID        256                 /* cells a side, of the MMP and of the ground index */
#define REC         64                  /* bytes of a cell's record */
#define DIR_LEN     12                  /* an entry of array 2 (a road point) */
#define WALL_LEN    28                  /* an entry of array 3 (a wall) */
#define PL_LEN      20                  /* a placement in the track file */
#define BOX_LEN     24                  /* a cell's view box (array 4): int32 x0, -, z0, x1, -, z1 */
#define CELL_LEN    24                  /* an entry of the cell table that ends the placements */

/* Reads the int32 at off. Beyond the end of the file it returns 0 and sets *bad. */
static int32_t rd32_or_bad(const uint8_t *p, uint32_t size, uint32_t off, int *bad)
{
    if(size < 4 || off > size - 4) { *bad = 1; return 0; }
    return (int32_t)le32u(p + off);
}

/* Whether n entries of len bytes, starting at off, lie in the file. */
static int fits(uint32_t size, uint32_t off, uint32_t n, uint32_t len)
{
    return off <= size && n <= (size - off) / len;
}

/* A cell that holds something. It has:
   - its records, from rec: n0 records of run 0, then n1 records of run 1;
   - array 2 (the road points) and array 3 (the walls), each with an offset and a number of entries;
   - its view box (array 4): the offset of its 24 bytes, or 0 if the cell has none. */
struct cell { uint32_t rec, n0, n1, dirs, ndirs, walls, nwalls, box; };

/* Reads the header of the cell at offset a into c, and checks its offsets and counts against the
   file. Returns 1, or 0 if the cell is malformed. */
static int read_cell(const uint8_t *p, uint32_t size, uint32_t a, struct cell *c)
{
    int bad = 0;
    int32_t dirs = rd32_or_bad(p, size, a + 0x08, &bad);
    int32_t walls = rd32_or_bad(p, size, a + 0x0C, &bad);
    int32_t n0 = rd32_or_bad(p, size, a + 0x14, &bad);
    int32_t n1 = rd32_or_bad(p, size, a + 0x18, &bad);
    int32_t ndirs = rd32_or_bad(p, size, a + 0x1C, &bad);
    int32_t nwalls = rd32_or_bad(p, size, a + 0x20, &bad);
    int32_t nbox = rd32_or_bad(p, size, a + 0x24, &bad);
    int32_t box = rd32_or_bad(p, size, a + 0x10, &bad);
    if(bad || n0 < 0 || n1 < 0 || ndirs < 0 || nwalls < 0) return 0;
    /* The offset of an empty array is never read. It is 0 in the game's files. */
    if((n0 || n1) && !fits(size, a + 0x28, (uint32_t)n0 + (uint32_t)n1, REC)) return 0;
    if(ndirs && (dirs < 0 || !fits(size, (uint32_t)dirs, (uint32_t)ndirs, DIR_LEN))) return 0;
    if(nwalls && (walls < 0 || !fits(size, (uint32_t)walls, (uint32_t)nwalls, WALL_LEN))) return 0;
    if(nbox > 0 && (box < 0 || !fits(size, (uint32_t)box, 1, BOX_LEN))) return 0;
    c->rec = a + 0x28;
    c->n0 = (uint32_t)n0;
    c->n1 = (uint32_t)n1;
    c->dirs = ndirs ? (uint32_t)dirs : 0;
    c->ndirs = (uint32_t)ndirs;
    c->walls = nwalls ? (uint32_t)walls : 0;
    c->nwalls = (uint32_t)nwalls;
    c->box = nbox > 0 ? (uint32_t)box : 0;
    return 1;
}

/* A walk over the grid, as make_track.py's cells() does it. k is the next grid cell (gy * 256 + gx),
   row the current row's offset, and bad is set when the data is malformed. */
struct walk { const uint8_t *p; uint32_t size, k; int32_t row; int bad; };

/* Gets the next cell that holds something, row by row. Returns 1 and its header in c, checked
   against the file. Returns 0 after the last cell, or with w->bad set if the data is malformed. */
static int next_cell(struct walk *w, struct cell *c)
{
    while(w->k < GRID * GRID) {
        uint32_t gx = w->k % GRID;
        if(gx == 0) w->row = rd32_or_bad(w->p, w->size, 4 * (w->k / GRID), &w->bad);
        if(w->bad || w->row < -1) goto bad;
        if(w->row == -1) { w->k += GRID; continue; }
        int32_t at = rd32_or_bad(w->p, w->size, (uint32_t)w->row + 4 * gx, &w->bad);
        w->k++;
        if(w->bad || at < -1) goto bad;
        if(at == -1) continue;
        if(!read_cell(w->p, w->size, (uint32_t)at, c)) goto bad;
        return 1;
    }
    return 0;
bad:
    w->bad = 1;
    return 0;
}

/* The grid cell column or row of a coordinate: (v + 2^20) >> 13, with the shift of Python. */
static int32_t grid_of(int32_t v) { return (int32_t)(((int64_t)v + 0x100000) >> 13); }

/* The angle of the road point nearest to (x, y); the rule is described in tb_mmp.h. */
int tb_mmp_road_angle(const uint8_t *mmp, uint32_t size, int32_t x, int32_t y, float *angle)
{
    int bad = 0, found = 0;
    int32_t best = 0x7FFFFFF8, kx = grid_of(x), ky = grid_of(y);
    float a = 0.0f;
    for(int32_t gy = ky - 1; gy <= ky + 1; gy++) {
        if(gy < 0 || gy >= GRID) continue;
        int32_t row = rd32_or_bad(mmp, size, 4 * (uint32_t)gy, &bad);
        if(bad || row < -1) return 0;
        if(row == -1) continue;
        for(int32_t gx = kx - 1; gx <= kx + 1; gx++) {
            if(gx < 0 || gx >= GRID) continue;
            int32_t at = rd32_or_bad(mmp, size, (uint32_t)row + 4 * (uint32_t)gx, &bad);
            struct cell c;
            if(bad || at < -1) return 0;
            if(at == -1) continue;
            if(!read_cell(mmp, size, (uint32_t)at, &c)) return 0;
            for(uint32_t k = 0; k < c.ndirs; k++) {
                const uint8_t *e = mmp + c.dirs + DIR_LEN * k;
                /* The game's imul and add: int32, and wrapping. */
                uint32_t dx = le32u(e + 4) - (uint32_t)x, dy = le32u(e + 8) - (uint32_t)y;
                int32_t d = (int32_t)(dx * dx + dy * dy);
                if(d < best) {
                    best = d;
                    memcpy(&a, e, 4);
                    found = 1;
                }
            }
        }
    }
    if(!found) return 0;
    if(a >= 6.28318548f) a -= 6.28318548f;      /* 2 pi in float32 (original: STARTL.EXE 0x100410) */
    *angle = a;
    return 1;
}

/* The file offset of the cell's j-th record in the game's draw-list order: run 1 first, then run 0. */
static uint32_t record(const struct cell *c, uint32_t j)
{
    return c->rec + REC * (j < c->n1 ? c->n0 + j : j - c->n1);
}

/* Makes the placements section (the layout is at tb_mmp_placements in tb_mmp.h). The first walk
   over the grid writes the placements, 20 bytes each. The second walk writes the cell table that
   ends the section. */
uint32_t tb_mmp_placements(const uint8_t *mmp, uint32_t size, uint8_t *out, uint32_t cap,
                           uint32_t *count)
{
    static const uint8_t field[5] = { 0x00, 0x08, 0x10, 0x0C, 0x18 };  /* object, X, Y, height, heading */
    struct walk w = { mmp, size, 0, 0, 0 };
    struct cell c;
    uint32_t n = 0;
    while(next_cell(&w, &c)) {
        for(uint32_t j = 0; j < c.n0 + c.n1; j++) {
            if(n >= cap / PL_LEN) return 0;
            const uint8_t *r = mmp + record(&c, j);
            for(int f = 0; f < 5; f++) memcpy(out + PL_LEN * n + 4 * f, r + field[f], 4);
            n++;
        }
    }
    if(w.bad) return 0;
    /* The cell table. Per cell: its key (row x 256 + column), its first placement and how many it
       has, the flag for a view box, and the box's 4 values. The game reads array 4 of the cell
       that the camera is in (original: STARTL.EXE 0x82e88). */
    uint32_t cells = 0, at = PL_LEN * n, first = 0;
    struct walk w2 = { mmp, size, 0, 0, 0 };
    if(cap - at < 12) return 0;
    memcpy(out + at, "CELL", 4);
    put_le32(out + at + 8, 0);
    at += 12;
    while(next_cell(&w2, &c)) {
        if(cap - at < CELL_LEN) return 0;
        put_le16(out + at, w2.k - 1);
        put_le16(out + at + 2, first);
        put_le16(out + at + 4, c.n0 + c.n1);
        put_le16(out + at + 6, c.box != 0);
        /* The box's x0, z0, x1, z1 are the int32 numbers 0, 2, 3, 5 of the 24 bytes. */
        for(int f = 0; f < 4; f++)
            put_le32(out + at + 8 + 4 * f,
                  c.box ? le32u(mmp + c.box + 4 * (f == 0 ? 0 : f == 1 ? 2 : f == 2 ? 3 : 5)) : 0);
        first += c.n0 + c.n1;
        at += CELL_LEN;
        cells++;
    }
    if(w2.bad) return 0;
    put_le32(out + PL_LEN * n + 4, cells);
    *count = n;
    return at;
}

/* Two walks over the placements: the first counts each grid cell's pieces into first[cell + 1]
   and sums them up, so first[cell] is where the cell's list starts; the second writes the
   numbers, first[cell] counting along to the end of its list; first[] then moves up by one. */
uint32_t tb_mmp_ground(const uint8_t *mmp, uint32_t size, uint8_t *out, uint32_t cap,
                       uint32_t *count)
{
    if(cap < TB_MMP_GROUND_FIRST) return 0;
    memset(out, 0, TB_MMP_GROUND_FIRST);
    uint8_t *list = out + TB_MMP_GROUND_FIRST;
    uint32_t n = 0;
    for(int pass = 0; pass < 2; pass++) {
        struct walk w = { mmp, size, 0, 0, 0 };
        struct cell c;
        uint32_t i = 0;                         /* the placement's number */
        while(next_cell(&w, &c)) {
            for(uint32_t j = 0; j < c.n0 + c.n1; j++, i++) {
                if(i > 0xFFFF) return 0;
                if(j < c.n1) continue;          /* run 1: not drivable */
                const uint8_t *r = mmp + record(&c, j);
                int32_t x = (int32_t)le32u(r + 0x08), y = (int32_t)le32u(r + 0x10);
                if(x < -0x100000 || x >= 0x100000 || y < -0x100000 || y >= 0x100000) continue;
                uint32_t g = ((uint32_t)(y + 0x100000) >> 13) * GRID + ((uint32_t)(x + 0x100000) >> 13);
                if(pass == 0) {
                    if(++n > 0xFFFF) return 0;
                    put_le16(out + 2 * (g + 1), le16u(out + 2 * (g + 1)) + 1);
                } else {
                    uint32_t at = le16u(out + 2 * g);
                    put_le16(list + 2 * at, i);
                    put_le16(out + 2 * g, at + 1);
                }
            }
        }
        if(w.bad || i > 0xFFFF) return 0;
        if(pass == 0) {
            if(cap - TB_MMP_GROUND_FIRST < 2 * n) return 0;
            for(uint32_t k = 1; k <= GRID * GRID; k++)
                put_le16(out + 2 * k, le16u(out + 2 * k) + le16u(out + 2 * (k - 1)));
        }
    }
    memmove(out + 2, out, TB_MMP_GROUND_FIRST - 2);
    put_le16(out, 0);
    *count = n;
    return TB_MMP_GROUND_FIRST + 2 * n;
}

/* every cell's array 2 (walls 0) or 3 (walls 1), unchanged */
static uint32_t arrays(const uint8_t *mmp, uint32_t size, int walls, uint8_t *out, uint32_t cap,
                       uint32_t *count)
{
    uint32_t len = walls ? WALL_LEN : DIR_LEN, n = 0;
    struct walk w = { mmp, size, 0, 0, 0 };
    struct cell c;
    while(next_cell(&w, &c)) {
        uint32_t at = walls ? c.walls : c.dirs, k = walls ? c.nwalls : c.ndirs;
        if(k > (cap - len * n) / len) return 0;
        memcpy(out + len * n, mmp + at, len * k);
        n += k;
    }
    if(w.bad) return 0;
    *count = n;
    return len * n;
}

uint32_t tb_mmp_dirs(const uint8_t *mmp, uint32_t size, uint8_t *out, uint32_t cap, uint32_t *count)
{
    return arrays(mmp, size, 0, out, cap, count);
}

uint32_t tb_mmp_walls(const uint8_t *mmp, uint32_t size, uint8_t *out, uint32_t cap, uint32_t *count)
{
    return arrays(mmp, size, 1, out, cap, count);
}
