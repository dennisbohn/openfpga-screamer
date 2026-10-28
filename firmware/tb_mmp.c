/*
 * The track file's sections from MAPn.MMP (tb_mmp.h). Every offset and count read from the
 * file is checked against its size before anything behind it is read.
 */
#include "tb_mmp.h"
#include <string.h>

#define GRID        256                 /* cells a side, of the MMP and of the ground index */
#define REC         64                  /* bytes of a cell's record */
#define DIR_LEN     12                  /* array 2 entry */
#define WALL_LEN    28                  /* array 3 entry */
#define PL_LEN      20                  /* a placement in the track file */

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* the int32 at off; beyond the end of the file 0 and *bad set */
static int32_t rd32(const uint8_t *p, uint32_t size, uint32_t off, int *bad)
{
    if(size < 4 || off > size - 4) { *bad = 1; return 0; }
    return (int32_t)get32(p + off);
}

/* whether n entries of len bytes from off lie in the file */
static int fits(uint32_t size, uint32_t off, uint32_t n, uint32_t len)
{
    return off <= size && n <= (size - off) / len;
}

/* a cell that holds something: its records (from rec, n0 of run 0, then n1 of run 1) and its
   arrays 2 and 3 (offset, entries) */
struct cell { uint32_t rec, n0, n1, dirs, ndirs, walls, nwalls; };

/* the walk over the grid as make_track.py's cells(): k the next grid cell (gy * 256 + gx) */
struct walk { const uint8_t *p; uint32_t size, k; int32_t row; int bad; };

/* the next cell that holds something, row by row: 1 and its header in c, checked against the
   file; 0 after the last cell, or with w->bad on malformed data */
static int next_cell(struct walk *w, struct cell *c)
{
    while(w->k < GRID * GRID) {
        uint32_t gx = w->k % GRID;
        if(gx == 0) w->row = rd32(w->p, w->size, 4 * (w->k / GRID), &w->bad);
        if(w->bad || w->row < -1) goto bad;
        if(w->row == -1) { w->k += GRID; continue; }
        int32_t at = rd32(w->p, w->size, (uint32_t)w->row + 4 * gx, &w->bad);
        w->k++;
        if(w->bad || at < -1) goto bad;
        if(at == -1) continue;
        uint32_t a = (uint32_t)at;
        int32_t dirs = rd32(w->p, w->size, a + 0x08, &w->bad);
        int32_t walls = rd32(w->p, w->size, a + 0x0C, &w->bad);
        int32_t n0 = rd32(w->p, w->size, a + 0x14, &w->bad);
        int32_t n1 = rd32(w->p, w->size, a + 0x18, &w->bad);
        int32_t ndirs = rd32(w->p, w->size, a + 0x1C, &w->bad);
        int32_t nwalls = rd32(w->p, w->size, a + 0x20, &w->bad);
        if(w->bad || n0 < 0 || n1 < 0 || ndirs < 0 || nwalls < 0) goto bad;
        /* an empty array's offset is never read (0 in the game's files) */
        if((n0 || n1) && !fits(w->size, a + 0x28, (uint32_t)n0 + (uint32_t)n1, REC)) goto bad;
        if(ndirs && (dirs < 0 || !fits(w->size, (uint32_t)dirs, (uint32_t)ndirs, DIR_LEN))) goto bad;
        if(nwalls && (walls < 0 || !fits(w->size, (uint32_t)walls, (uint32_t)nwalls, WALL_LEN))) goto bad;
        c->rec = a + 0x28;
        c->n0 = (uint32_t)n0;
        c->n1 = (uint32_t)n1;
        c->dirs = ndirs ? (uint32_t)dirs : 0;
        c->ndirs = (uint32_t)ndirs;
        c->walls = nwalls ? (uint32_t)walls : 0;
        c->nwalls = (uint32_t)nwalls;
        return 1;
    }
    return 0;
bad:
    w->bad = 1;
    return 0;
}

/* the cell's j-th record in the game's draw-list order: run 1 first, then run 0 */
static uint32_t record(const struct cell *c, uint32_t j)
{
    return c->rec + REC * (j < c->n1 ? c->n0 + j : j - c->n1);
}

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
    *count = n;
    return PL_LEN * n;
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
                int32_t x = (int32_t)get32(r + 0x08), y = (int32_t)get32(r + 0x10);
                if(x < -0x100000 || x >= 0x100000 || y < -0x100000 || y >= 0x100000) continue;
                uint32_t g = ((uint32_t)(y + 0x100000) >> 13) * GRID + ((uint32_t)(x + 0x100000) >> 13);
                if(pass == 0) {
                    if(++n > 0xFFFF) return 0;
                    put16(out + 2 * (g + 1), get16(out + 2 * (g + 1)) + 1);
                } else {
                    uint32_t at = get16(out + 2 * g);
                    put16(list + 2 * at, i);
                    put16(out + 2 * g, at + 1);
                }
            }
        }
        if(w.bad || i > 0xFFFF) return 0;
        if(pass == 0) {
            if(cap - TB_MMP_GROUND_FIRST < 2 * n) return 0;
            for(uint32_t k = 1; k <= GRID * GRID; k++)
                put16(out + 2 * k, get16(out + 2 * k) + get16(out + 2 * (k - 1)));
        }
    }
    memmove(out + 2, out, TB_MMP_GROUND_FIRST - 2);
    put16(out, 0);
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
