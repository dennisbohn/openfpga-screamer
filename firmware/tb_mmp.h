/*
 * The track image's map sections, made from MAPn.MMP (in GFX/MAPnL.DAT / MAPnH.DAT) as
 * tools/make_track.py's cells(), placements(), ground_index() and cell_arrays() make them,
 * byte for byte.
 *
 * The file MAPn.MMP is the game's map:
 *   file     256 int32 row offsets (-1: an empty row). Each row is 256 int32 cell offsets
 *            (-1: an empty cell).
 *   cell     +0x08  offset of array 2 (the road points)
 *            +0x0C  offset of array 3 (the walls)
 *            +0x14  number of records of run 0
 *            +0x18  number of records of run 1
 *            +0x1C  number of entries of array 2
 *            +0x20  number of entries of array 3
 *            +0x28  its 64-byte records, run 0 first, then run 1
 *   record   +0x00 object, +0x08 X, +0x0C height, +0x10 Y, +0x18 heading
 *
 * Each function reads the whole MMP (mmp, size bytes). It writes its section to out (at most cap
 * bytes, any alignment) and its count to *count, and returns the bytes written. It returns 0 when
 * the MMP is malformed (an offset or a count outside the file) or the section does not fit. No
 * scratch memory is needed: the ground index sorts in its own output.
 */
#pragma once
#include <stdint.h>

/* The placements (header 0x1C). Each placement is 20 bytes: int32 object, X, Y, height, heading
   (2048 = one turn). The cells come row by row, and each cell's run 1 comes before its run 0.
   Then the cell table follows (the count stays the number of placements):
   - "CELL", u32 number of cells, u32 0;
   - per cell, 24 bytes, in the order of the keys: u16 key (row x 256 + column), u16 first
     placement, u16 number of placements, u16 1 if the cell has a view box (array 4), then the
     box's int32 x0, z0, x1, z1 (0 without a box). */
uint32_t tb_mmp_placements(const uint8_t *mmp, uint32_t size, uint8_t *out, uint32_t cap,
                           uint32_t *count);

/* The ground index (header 0x2C): u16 first[65537], then *count u16 placement numbers (as
   tb_mmp_placements() numbers them). These are the drivable pieces (run 0), ordered by the grid
   cell of their position, ((Y + 2^20) >> 13) x 256 + ((X + 2^20) >> 13), and in file order within
   a cell. Pieces off the grid are left out. The size is 131074 + 2 x count bytes. The result is
   also 0 with 65536 placements or more. */
#define TB_MMP_GROUND_FIRST     (2u * 65537u)
uint32_t tb_mmp_ground(const uint8_t *mmp, uint32_t size, uint8_t *out, uint32_t cap,
                       uint32_t *count);

/* The road points (header 0x4C): every cell's array 2 unchanged, 12 bytes each (float angle,
   int32 x, y). The walls (header 0x54): every cell's array 3 unchanged, 28 bytes each (float
   angle, int32 x, y, x1, y1, x2, y2). Both come with the cells row by row. */
uint32_t tb_mmp_dirs(const uint8_t *mmp, uint32_t size, uint8_t *out, uint32_t cap, uint32_t *count);
uint32_t tb_mmp_walls(const uint8_t *mmp, uint32_t size, uint8_t *out, uint32_t cap, uint32_t *count);

/* The angle of the road point nearest to (x, y), for the start heading. It follows STARTL.EXE's
   0x120c0 (mode 0):
   1. The 3 x 3 cells round the point are searched, rows first, then columns. Each cell's array 2
      is taken in order.
   2. The first point with the smallest squared distance wins. The distance is an int32 that wraps,
      as the game's imul does.
   3. An angle of 2 pi or more is reduced by 2 pi.
   Returns 1 and *angle, or 0 if the file is malformed or no road point is there. */
int tb_mmp_road_angle(const uint8_t *mmp, uint32_t size, int32_t x, int32_t y, float *angle);
