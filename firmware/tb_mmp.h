/*
 * The track file's sections from MAPn.MMP (in GFX/MAPnL.DAT / MAPnH.DAT), made from the game's
 * own file as tools/make_track.py's cells(), placements(), ground_index() and cell_arrays()
 * make them, byte for byte.
 *
 * MAPn.MMP: 256 int32 row offsets (-1 an empty row), each row 256 int32 cell offsets (-1 an
 * empty cell); a cell: +0x08 offset of array 2, +0x0C of array 3, +0x14 records of run 0,
 * +0x18 of run 1, +0x1C entries of array 2, +0x20 of array 3; from +0x28 its 64-byte records,
 * run 0 then run 1 (record +0x00 object, +0x08 X, +0x0C height, +0x10 Y, +0x18 heading).
 *
 * Each function reads the whole MMP (mmp, size bytes), writes its section to out (at most cap
 * bytes, any alignment) and its count to *count, and returns the bytes written: 0 when the MMP
 * is malformed (an offset or a count outside the file) or the section does not fit. No scratch
 * memory: the ground index sorts in its own output.
 */
#pragma once
#include <stdint.h>

/* The placements (header 0x1C): int32 object, X, Y, height, heading (2048 = one turn), 20 bytes
   each; the cells row by row, each cell's run 1 before its run 0 */
uint32_t tb_mmp_placements(const uint8_t *mmp, uint32_t size, uint8_t *out, uint32_t cap,
                           uint32_t *count);

/* The ground index (header 0x2C): u16 first[65537], then *count u16 placement numbers (as
   tb_mmp_placements() numbers them): the drivable pieces (run 0) by the grid cell of their
   position, ((Y + 2^20) >> 13) x 256 + ((X + 2^20) >> 13), each cell's in file order; pieces
   off the grid are left out. 131074 + 2 x count bytes; 0 also with 65536 placements or more. */
#define TB_MMP_GROUND_FIRST     (2u * 65537u)
uint32_t tb_mmp_ground(const uint8_t *mmp, uint32_t size, uint8_t *out, uint32_t cap,
                       uint32_t *count);

/* The road points (header 0x4C): every cell's array 2 unchanged, 12 bytes each (float angle,
   int32 x, y); the walls (header 0x54): array 3, 28 bytes each (float angle, int32 x, y, x1,
   y1, x2, y2); the cells row by row */
uint32_t tb_mmp_dirs(const uint8_t *mmp, uint32_t size, uint8_t *out, uint32_t cap, uint32_t *count);
uint32_t tb_mmp_walls(const uint8_t *mmp, uint32_t size, uint8_t *out, uint32_t cap, uint32_t *count);

/* The angle of the road point nearest to (x, y) as STARTL.EXE's 0x120c0 (mode 0) finds it
   for the start heading: the 3 x 3 cells round the point, rows then columns, each cell's
   array 2 in order, the first with the smallest squared distance (int32, wrapping, as the
   game's imul); 2 pi or more less 2 pi. 1 and *angle, or 0 if malformed or none is there. */
int tb_mmp_road_angle(const uint8_t *mmp, uint32_t size, int32_t x, int32_t y, float *angle);
