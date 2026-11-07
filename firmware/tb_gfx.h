/*
 * The track image's palette, car models and backdrop, made from the game's own files as
 * tools/make_track.py makes them (byte for byte the same): the palette (header 0x18), the car
 * models (0x44) and the backdrop (0x78). Each function reads one or more files as they come
 * out of a .DAT container and writes its section to out (cap bytes there). It returns the
 * bytes written, or 0 if a file is malformed or the section does not fit. No scratch memory.
 */
#pragma once
#include <stdint.h>

/* MAPn.COL (256 x r, g, b, 6-bit VGA values) as 256 x u32 0x00RRGGBB, each value times 4 */
#define TB_PALETTE_SIZE     1024
uint32_t tb_palette(const uint8_t *col, uint32_t size, uint8_t *out, uint32_t cap);

/* The car models section, made from the n files AUTO00.3DX .. (x3d points to each file, size
   is each file's size).
   1. n records of 24 bytes: float wheelbase, float front axle (ahead of the origin), float rear
      axle (behind it), float half track, u32 offset of the file (from the section's start), u32
      size of the file. The four floats come from the offsets of the off-centre blocks, which are
      the wheels; they are 935, 445, 490, 226 if there are no such blocks.
   2. The files, each padded with zeros to a multiple of 256 bytes.
   A file may lie outside out. Or it may already be at its own place in out (tb_car_models_at():
   unpacked straight there). It may be nowhere else in out. */
uint32_t tb_car_models(const uint8_t *const x3d[], const uint32_t size[], int n,
                       uint8_t *out, uint32_t cap);
/* Where file k goes in that section. Returns 0 if the section would be 4 GB or more. */
uint32_t tb_car_models_at(const uint32_t size[], int n, int k);

/* MAPn.BCK is a PCX picture (8 bits, one plane, run-length coded) of 512 x 256. It is unpacked
   to rows of 512 palette indices, followed by the last row once more: the game's copy
   (original: STARTL.EXE 0x83a68) runs on past a row's end into the next row. Bytes per line
   above 512 are left out. Fewer bytes per line, or another size of the picture, count as
   malformed. */
#define TB_BACKDROP_W       512
#define TB_BACKDROP_H       256
#define TB_BACKDROP_SIZE    (TB_BACKDROP_W * (TB_BACKDROP_H + 1))
uint32_t tb_backdrop(const uint8_t *bck, uint32_t size, uint8_t *out, uint32_t cap);
