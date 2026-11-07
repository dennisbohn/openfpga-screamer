/*
 * Builds the track image (track.h) on the Pocket from the game's original containers, byte for
 * byte as tools/make_track.py builds it. The layout of the image is at the top of track.c.
 * 1. The entries are unpacked with dat.c.
 * 2. The sections are made by tb_mmp.c, tb_spl.c, tb_gfx.c and tb_hud.c, in make_track.py's order,
 *    each on a 256-byte boundary from offset 0x100. The gaps are zero.
 * 3. The header (version 6) and the CRC-32 of each 256 KB block are written last.
 * Entry point: track_build. track_build.c lists which entry makes which section.
 */
#pragma once
#include <stdint.h>

/* The five containers as the data slots load them, each with its size, in this order: gfx is
   GFX/MAPnH.DAT, game is GFX/GAME.DAT, data is DATA/MAPn.DAT, gen is DATA/GEN.DAT, local is
   DATA/LOCAL.DAT. */
struct track_files {
    const uint8_t *gfx, *game, *data, *gen, *local;
    uint32_t gfx_size, game_size, data_size, gen_size, local_size;
};

/* The game's difficulty, 0 ROOKIE, 1 NORMAL (the default), 2 PRO. The opponents and TRACK_DATA
   come from MAPnA.DTR, MAPnB.DTR or MAPnC.DTR (original: STARTL.EXE 0x13251). */
extern int track_build_difficulty;

/* What a build did: the entry or section it stopped at ("" when it got through), and the CPU
   cycles of the whole build and of the unpacking in it (0 on the PC). */
struct track_build_info {
    char fail[20];
    uint32_t cycles, unpack_cycles;
};

/* Builds track n (1..9, MAPn) into out (cap bytes, any alignment). Returns the image's size. Returns
   0 if a container or entry is missing or malformed, or if the image does not fit (at most 7 MB,
   that is 28 blocks).
   The small entries are unpacked into scratch (scratch_cap bytes, 270 KB for track 1).
   MAPn.MLB, MAPn.UNR and the cars' .3DX files are unpacked straight to their place in out.
   out and scratch are read back while they are written, so they must be ordinary memory (the SDRAM
   through the cache).
   In the simulation, track 1 (4,166,400 bytes) takes 237 M cycles, 3.2 s: 73 M to unpack, and the
   rest mostly for the camera path (soft-float doubles) and the CRC-32 over 4 MB. */
uint32_t track_build(const struct track_files *f, int n, uint8_t *out, uint32_t cap,
                     uint8_t *scratch, uint32_t scratch_cap, struct track_build_info *info);
