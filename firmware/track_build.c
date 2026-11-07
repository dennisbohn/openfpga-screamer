/*
 * Builds the track image (the layout is at the top of track.c) from the game's five .DAT
 * containers (track_build.h), as tools/make_track.py main() assembles it. Entry point: track_build.
 * The sections go straight into out, one after the other. Each section's offset and count wait in
 * offs[] and counts[] for the header, which is written last, with the CRC of each 256 KB block.
 *
 * Which entry of which container makes which section (n is the track number):
 *   section (header offset)  entry                                        made by
 *   MLB (0x08)               MAPn.MLB, GFX/MAPnH.DAT                      copied unchanged
 *   UNR (0x10)               MAPn.UNR, GFX/MAPnH.DAT                      copied unchanged
 *   palette (0x18)           MAPn.COL, GFX/MAPnH.DAT                      tb_palette
 *   placements (0x1C)        MAPn.MMP, GFX/MAPnH.DAT                      tb_mmp_placements
 *   camera path (0x24)       MAPnOPP.SPL, DATA/MAPn.DAT                   tb_path
 *   ground index (0x2C)      MAPn.MMP                                     tb_mmp_ground
 *   markers (0x34)           MAPn.MRK, DATA/MAPn.DAT                      tb_markers
 *   cars (0x3C)              CARS.DTR, DATA/GEN.DAT                       tb_cars
 *   car models (0x44)        AUTO00.3DX .. AUTO06.3DX, GFX/MAPnH.DAT      tb_car_models
 *   road points (0x4C)       MAPn.MMP                                     tb_mmp_dirs
 *   walls (0x54)             MAPn.MMP                                     tb_mmp_walls
 *   race screen (0x68)       GAMEL.SPR, GAMEL.FNT, IMPLET_L.PKC, IMPRES_L.PKC (GFX/GAME.DAT),
 *                            GAME.TXT (DATA/LOCAL.DAT)                    tb_hud_build
 *   backdrop (0x78)          MAPn.BCK, GFX/MAPnH.DAT                      tb_backdrop
 *   opponents (0x80)         MAPnOPP.SPL and MAPnA/B/C.DTR (DATA/MAPn.DAT) tb_opponents
 * The header also holds the start (0x5C: MAPnOPP.SPL and MAPn.MMP, tb_grid_pose) and TRACK_DATA from
 * MAPnA/B/C.DTR (0x70, 0x74, 0x7C: tb_track_data).
 */
#include "track_build.h"
#include "dat.h"
#include "tb_mmp.h"
#include "tb_spl.h"
#include "tb_gfx.h"
#include "tb_hud.h"
#include "crc32.h"
#include <string.h>

int track_build_difficulty = 1;         /* NORMAL, as make_track.py */

#define FIRST       0x100       /* the first section, after the header */
#define BLOCK       0x40000     /* bytes per CRC block */
#define MAX_BLOCKS  28          /* the CRCs that fit into the header (0x90 .. 0x100) */
#define SECTIONS    14          /* the sections in offs[] and counts[], in the order of the table above */
#define MODELS      7           /* AUTO00.3DX .. AUTO06.3DX */

/* The CPU's cycle counter, for the cycles in track_build_info. It is 0 on the PC. */
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

/* The state of one build: out (cap bytes, pos the next free byte) takes the image, scratch
   (scratch_cap bytes, used of them taken) the unpacked entries. */
struct build {
    uint8_t *out, *scratch;
    uint32_t cap, pos, scratch_cap, used;
    int sections;                           /* the number of sections written so far */
    uint32_t offs[SECTIONS], counts[SECTIONS];
    char track[8];                          /* "MAPn" */
    struct track_build_info *info;
};

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Records what stopped the build, for the log (info->fail). Returns 0, so that a caller can
   write `return fail(...)`. */
static int fail(struct build *b, const char *what)
{
    size_t n = strlen(what);
    if(n > sizeof b->info->fail - 1) n = sizeof b->info->fail - 1;
    memcpy(b->info->fail, what, n);
    b->info->fail[n] = 0;
    return 0;
}

/* The track's name with an ending, for example "MAP1" + ".MMP". The result is buf. */
static const char *name(struct build *b, const char *end, char buf[16])
{
    size_t n = strlen(b->track);
    memcpy(buf, b->track, n);
    strcpy(buf + n, end);
    return buf;
}

/* Unpacks entry e of the container (file, size) into `to` (room bytes) and adds the cycles to the
   statistics. Returns 1. Returns 0, and records the entry's name as the failure, if the entry does
   not fit, is malformed, or is empty. */
static int extract(struct build *b, const uint8_t *file, uint32_t size, const struct dat_entry *e,
                   uint8_t *to, uint32_t room)
{
    uint32_t t0 = now();
    int ok = e->size > 0 && dat_extract(file, size, e, to, room) == e->size;
    b->info->unpack_cycles += now() - t0;
    return ok || fail(b, e->name);
}

/* Unpacks the entry called `what` into the scratch. Returns where it starts, and *size is its
   size in bytes. Returns 0 if the entry is missing or malformed. */
static const uint8_t *unpack(struct build *b, const uint8_t *file, uint32_t file_size, const char *what,
                             uint32_t *size)
{
    struct dat_entry e;
    uint8_t *p = b->scratch + b->used;
    uint32_t room = b->scratch_cap - b->used;
    if(!dat_find(file, file_size, what, &e)) {
        fail(b, what);
        return 0;
    }
    if(!extract(b, file, file_size, &e, p, room)) return 0;
    b->used += e.size + 3 > room ? room : (e.size + 3) & ~3u;   /* the next entry starts word-aligned */
    *size = e.size;
    return p;
}

/* Where the next section goes, and the room it has. */
static uint8_t *at(const struct build *b) { return b->out + b->pos; }
static uint32_t room(const struct build *b) { return b->cap - b->pos; }

/* Closes the section that has been written at at(b): it is n bytes long, and `count` goes to the
   header. n is 0 if the section could not be made; then this fails with `what` as the failure. The
   gap to the next 256-byte boundary is zeroed, and the next section starts there. Returns 1, or 0
   on failure. */
static int section(struct build *b, uint32_t n, uint32_t count, const char *what)
{
    if(n == 0 || n > room(b)) return fail(b, what);
    uint32_t end = b->pos + n, next = (end + 255) & ~255u;
    if(next < end || next > b->cap) return fail(b, what);
    memset(b->out + end, 0, next - end);
    b->offs[b->sections] = b->pos;
    b->counts[b->sections++] = count;
    b->pos = next;
    return 1;
}

/* Takes an entry of the graphics container, MAPn plus `end`, unchanged as the next section. Used
   for the MLB and the UNR. */
static int raw(struct build *b, const struct track_files *f, const char *end)
{
    char buf[16];
    struct dat_entry e;
    if(!dat_find(f->gfx, f->gfx_size, name(b, end, buf), &e)) return fail(b, buf);
    return extract(b, f->gfx, f->gfx_size, &e, at(b), room(b)) && section(b, e.size, e.size, buf);
}

/* The car models section. Each .3DX is unpacked straight to its place in the section (the place
   comes from tb_car_models_at), then tb_car_models writes the records in front of the files. */
static int models(struct build *b, const struct track_files *f)
{
    struct dat_entry e[MODELS];
    uint32_t size[MODELS];
    const uint8_t *x3d[MODELS];
    for(int k = 0; k < MODELS; k++) {
        char buf[16] = "AUTO00.3DX";
        buf[5] = (char)('0' + k);
        if(!dat_find(f->gfx, f->gfx_size, buf, &e[k])) return fail(b, buf);
        size[k] = e[k].size;
    }
    for(int k = 0; k < MODELS; k++) {
        uint32_t o = tb_car_models_at(size, MODELS, k);
        if(o == 0 || o > room(b) || !extract(b, f->gfx, f->gfx_size, &e[k], at(b) + o, room(b) - o)) return 0;
        x3d[k] = at(b) + o;
    }
    return section(b, tb_car_models(x3d, size, MODELS, at(b), room(b)), MODELS, "car models");
}

/* Builds track n into out and returns the image's size, or 0 on failure (see track_build.h). The
   sections are written in the order of the table at the top of the file. Each small entry is
   unpacked into the scratch and then turned into its section; the MLB, the UNR and the car models
   are unpacked straight into out. */
uint32_t track_build(const struct track_files *f, int n, uint8_t *out, uint32_t cap,
                     uint8_t *scratch, uint32_t scratch_cap, struct track_build_info *info)
{
    uint32_t t0 = now();
    struct build b = { .out = out, .scratch = scratch, .cap = cap, .pos = FIRST, .scratch_cap = scratch_cap,
                       .track = "MAP0", .info = info };
    info->fail[0] = 0;
    info->cycles = info->unpack_cycles = 0;
    if(n < 1 || n > 9 || cap < FIRST) return (uint32_t)fail(&b, "track number");
    b.track[3] = (char)('0' + n);
    memset(out, 0, FIRST);

    char buf[16];
    /* The unpacked entries (col, mmp, ...) with their sizes (col_n, ...). len is the size of the
       section just made, and count its count for the header. */
    uint32_t col_n = 0, mmp_n = 0, spl_n = 0, mrk_n = 0, cars_n = 0, spr_n = 0, fnt_n = 0, let_n = 0,
             res_n = 0, txt_n = 0, bck_n = 0, dtr_n = 0, len, count = 0;
    const uint8_t *col, *mmp, *spl, *mrk, *cars, *spr, *fnt, *let, *res, *txt, *bck, *dtr;
    uint8_t td[12];                         /* TRACK_DATA: bonus_time, camera_hi, colour */
    if(!raw(&b, f, ".MLB") || !raw(&b, f, ".UNR")) return 0;
    if(!(col = unpack(&b, f->gfx, f->gfx_size, name(&b, ".COL", buf), &col_n)) ||
       !section(&b, tb_palette(col, col_n, at(&b), room(&b)), 0, "palette")) return 0;
    if(!(mmp = unpack(&b, f->gfx, f->gfx_size, name(&b, ".MMP", buf), &mmp_n))) return 0;
    len = tb_mmp_placements(mmp, mmp_n, at(&b), room(&b), &count);
    if(!section(&b, len, count, "placements")) return 0;
    if(!(spl = unpack(&b, f->data, f->data_size, name(&b, "OPP.SPL", buf), &spl_n))) return 0;
    len = tb_path(spl, spl_n, at(&b), room(&b));
    if(!section(&b, len, len / 12, "camera path")) return 0;
    len = tb_mmp_ground(mmp, mmp_n, at(&b), room(&b), &count);
    if(!section(&b, len, count, "ground index")) return 0;
    if(!(mrk = unpack(&b, f->data, f->data_size, name(&b, ".MRK", buf), &mrk_n))) return 0;
    len = tb_markers(mrk, mrk_n, at(&b), room(&b));
    if(!section(&b, len, len / 32, "markers")) return 0;
    if(!(cars = unpack(&b, f->gen, f->gen_size, "CARS.DTR", &cars_n))) return 0;
    len = tb_cars(cars, cars_n, at(&b), room(&b));
    if(!section(&b, len, len / 68, "cars") || !models(&b, f)) return 0;
    len = tb_mmp_dirs(mmp, mmp_n, at(&b), room(&b), &count);
    if(!section(&b, len, count, "road points")) return 0;
    len = tb_mmp_walls(mmp, mmp_n, at(&b), room(&b), &count);
    if(!section(&b, len, count, "walls")) return 0;
    if(!(spr = unpack(&b, f->game, f->game_size, "GAMEL.SPR", &spr_n)) ||
       !(fnt = unpack(&b, f->game, f->game_size, "GAMEL.FNT", &fnt_n)) ||
       !(let = unpack(&b, f->game, f->game_size, "IMPLET_L.PKC", &let_n)) ||
       !(res = unpack(&b, f->game, f->game_size, "IMPRES_L.PKC", &res_n)) ||
       !(txt = unpack(&b, f->local, f->local_size, "GAME.TXT", &txt_n))) return 0;
    len = tb_hud_build(spr, spr_n, fnt, fnt_n, let, let_n, res, res_n, txt, txt_n, at(&b), room(&b));
    if(!section(&b, len, len, "race screen")) return 0;
    if(!(bck = unpack(&b, f->gfx, f->gfx_size, name(&b, ".BCK", buf), &bck_n)) ||
       !section(&b, tb_backdrop(bck, bck_n, at(&b), room(&b)), 0, "backdrop")) return 0;
    /* The name is MAPn, then 'A' plus the difficulty, then .DTR (original: STARTL.EXE 0x13251). */
    char dtr_end[8] = "A.DTR";
    dtr_end[0] = (char)('A' + track_build_difficulty);
    if(!(dtr = unpack(&b, f->data, f->data_size, name(&b, dtr_end, buf), &dtr_n))) return 0;
    len = tb_opponents(spl, spl_n, dtr, dtr_n, at(&b), room(&b));
    if(!section(&b, len, len, "opponents")) return 0;
    /* The start goes into the header itself, at 0x5C (12 bytes). */
    if(tb_grid_pose(spl, spl_n, mmp, mmp_n, out + 0x5C, 12) != 12) return (uint32_t)fail(&b, "grid pose");
    if(tb_track_data(dtr, dtr_n, td, sizeof td) != sizeof td) return (uint32_t)fail(&b, "TRACK_DATA");

    /* The header: the offsets and counts as make_track.py packs them (the layout is at the top
       of track.c), then the CRC of each block. */
    uint32_t size = b.pos, blocks = (size - FIRST + BLOCK - 1) / BLOCK;
    if(blocks > MAX_BLOCKS) return (uint32_t)fail(&b, "image size");
    memcpy(out, "SCRK", 4);
    wr32(out + 0x04, 6);
    wr32(out + 0x08, b.offs[0]);
    wr32(out + 0x0C, b.counts[0]);
    wr32(out + 0x10, b.offs[1]);
    wr32(out + 0x14, b.counts[1]);
    wr32(out + 0x18, b.offs[2]);
    for(int k = 3; k <= 10; k++) {          /* placements to walls: offset, count */
        wr32(out + 0x1C + 8 * (k - 3), b.offs[k]);
        wr32(out + 0x20 + 8 * (k - 3), b.counts[k]);
    }
    wr32(out + 0x68, b.offs[11]);
    wr32(out + 0x6C, b.counts[11]);
    memcpy(out + 0x70, td, 8);              /* bonus_time, camera_hi */
    wr32(out + 0x78, b.offs[12]);
    memcpy(out + 0x7C, td + 8, 4);          /* the ground colour */
    wr32(out + 0x80, b.offs[13]);
    wr32(out + 0x84, b.counts[13]);
    wr32(out + 0x88, size);
    wr32(out + 0x8C, blocks);
    for(uint32_t k = 0; k < blocks; k++) {  /* the last block may be shorter */
        uint32_t o = FIRST + k * BLOCK, e = size - o < BLOCK ? size : o + BLOCK;
        wr32(out + 0x90 + 4 * k, crc32(out + o, out + e));
    }
    info->cycles = now() - t0;
    return size;
}
