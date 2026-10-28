/*
 * The track file's pictures and car models, made from the game's own files as
 * tools/make_track.py makes them (byte for byte the same): the palette (header 0x18), the car
 * models (0x44) and the backdrop (0x78). Each function reads one or more files as they come
 * out of a .DAT container and writes its section to out (cap bytes there); it returns the
 * bytes written, 0 if a file is malformed or the section does not fit. No scratch memory.
 */
#pragma once
#include <stdint.h>

/* MAPn.COL (256 x r, g, b, 6-bit VGA values) as 256 x u32 0x00RRGGBB, each value times 4 */
#define TB_PALETTE_SIZE     1024
uint32_t tb_palette(const uint8_t *col, uint32_t size, uint8_t *out, uint32_t cap);

/* The car models section of the n files AUTO00.3DX..: n records of 24 bytes, float
   wheelbase, front axle (ahead of the origin), rear axle (behind it), half track (from the
   offsets of the off-centre, wheel, blocks; 935, 445, 490, 226 if there are none), u32 offset
   of the file (from the section's start) and its size; then the files, each padded with zeros
   to a multiple of 256 bytes. A file may lie outside out or already at its own place in it
   (tb_car_models_at(): decompressed straight there), nowhere else in out. */
uint32_t tb_car_models(const uint8_t *const x3d[], const uint32_t size[], int n,
                       uint8_t *out, uint32_t cap);
/* where file k goes in that section; 0 if the section would be 4 GB or more */
uint32_t tb_car_models_at(const uint32_t size[], int n, int k);

/* MAPn.BCK, a PCX picture (8 bits, one plane, run-length coded) of 512 x 256, unpacked: rows
   of 512 palette indices, then the last row once more (the game's copy, 0x83a68, runs on
   past a row's end into the next row). Bytes per line above 512 are left out; fewer, or
   another size of the picture, count as malformed. */
#define TB_BACKDROP_W       512
#define TB_BACKDROP_H       256
#define TB_BACKDROP_SIZE    (TB_BACKDROP_W * (TB_BACKDROP_H + 1))
uint32_t tb_backdrop(const uint8_t *bck, uint32_t size, uint8_t *out, uint32_t cap);
