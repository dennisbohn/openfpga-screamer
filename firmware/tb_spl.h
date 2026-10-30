/*
 * The track file's parts from DATA/MAPn.DAT and DATA/GEN.DAT, made on the Pocket from the
 * raw container entries as tools/make_track.py makes them (byte for byte): the camera path
 * (header 0x24), the markers (0x34), the cars (0x3C), the grid pose (0x5C), TRACK_DATA (0x70,
 * 0x7C) and the opponents (0x80). The inputs: MAPnOPP.SPL, MAPn.MRK, MAPnB.DTR (the
 * difficulty's file) and GEN.DAT's CARS.DTR (and for the grid pose MAPn.MMP), unpacked, at
 * any alignment.
 *
 * Each function writes its part little-endian to out (cap bytes) and returns the bytes
 * written; 0 if the input is malformed (where make_track.py fails), the part would be
 * empty, or cap is too small. No scratch memory beyond a few hundred bytes of stack.
 *
 * Numbers in the text files (.DTR) are read as Python's float() and int() read them, then
 * rounded to float32 as struct.pack('f') does. A decimal must be exact as one product or
 * quotient of an integer up to 2^53 and a power of ten up to 10^22 (10^37 for a product),
 * a hex number must fit 63 bits; anything beyond counts as malformed here (make_track.py
 * would round it). The game's numbers are all well within.
 */
#pragma once
#include <stdint.h>

/* The camera path: int32 x, y, height a point, 12 bytes (the count is bytes / 12); along
   the tenth spline of spl from its start node, each node's two pieces in steps of about 100
   units, rounded half to even. A square is x * x where make_track.py has x ** 2; Python's
   pow() on Windows is off by 1 ulp in rare near-ties (none changes a point of the six
   tracks). Soft-float double: about 4800 instructions a point, 24 M for track 1. */
uint32_t tb_path(const uint8_t *spl, uint32_t size, uint8_t *out, uint32_t cap);

/* The grid pose, 12 bytes, as STARTL.EXE sets it (0x13e07): int32 x, y of the tenth
   spline's start node, float heading of the MMP road point nearest to it
   (tb_mmp_road_angle(), the game's 0x120c0) */
uint32_t tb_grid_pose(const uint8_t *spl, uint32_t size, const uint8_t *mmp, uint32_t mmp_size,
                      uint8_t *out, uint32_t cap);

/* MAPn.MRK's 32-byte records unchanged (the count is bytes / 32); 0 as well if the file
   holds fewer than its first word says */
uint32_t tb_markers(const uint8_t *mrk, uint32_t size, uint8_t *out, uint32_t cap);

/* CARS.DTR's CARS_DATA rows, 68 bytes each (the count is bytes / 68): char name[12] (the
   first token without its letter, UTF-8, at most 11 bytes), int32 livery, float maxspd,
   maxrpm, grip, accel, int32 automatic, nshift, float ratio[7] (ratio[0] = 0) */
uint32_t tb_cars(const uint8_t *dtr, uint32_t size, uint8_t *out, uint32_t cap);

/* TRACK_DATA of the difficulty's .DTR, 12 bytes: int32 bonus_time, camera_hi (the header's
   0x70, 0x74), uint32 colour (0x7C); with room for 16 the carnage after them (car.h) */
uint32_t tb_track_data(const uint8_t *dtr, uint32_t size, uint8_t *out, uint32_t cap);

/* The opponents section (its size is the bytes written): u32 splines, u32 rows; rows x
   float max_spd, accel, brakes, skill, sband (OPPONENTS of dtr); per spline of spl u32
   start node, u32 nodes, nodes x int32 x, y, z, next, in x, in y, out x, out y */
uint32_t tb_opponents(const uint8_t *spl, uint32_t spl_size, const uint8_t *dtr, uint32_t dtr_size,
                      uint8_t *out, uint32_t cap);
