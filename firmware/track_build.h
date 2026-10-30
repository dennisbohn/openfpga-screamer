/*
 * The track image (track.h) built on the Pocket from the game's original containers, byte for
 * byte as tools/make_track.py builds it: the entries unpacked with dat.c, the sections made by
 * tb_mmp.c, tb_spl.c, tb_gfx.c and tb_hud.c, in make_track.py's order on 256-byte boundaries
 * from 0x100, the gaps zero; then the header (version 6) and the CRC-32 of each 256 KB block.
 */
#pragma once
#include <stdint.h>

/* The five containers as the data slots load them, each with its size: GFX/MAPnH.DAT,
   GFX/GAME.DAT, DATA/MAPn.DAT, DATA/GEN.DAT, DATA/LOCAL.DAT */
struct track_files {
    const uint8_t *gfx, *game, *data, *gen, *local;
    uint32_t gfx_size, game_size, data_size, gen_size, local_size;
};

/* The game's difficulty, 0 ROOKIE, 1 NORMAL (the default), 2 PRO: the opponents and TRACK_DATA
   come from MAPnA.DTR, MAPnB.DTR or MAPnC.DTR (0x13251) */
extern int track_build_difficulty;

/* What a build did: the entry or section it stopped at ("" when it got through), and the
   CPU cycles of the whole build and of the unpacking in it (0 on the PC) */
struct track_build_info {
    char fail[20];
    uint32_t cycles, unpack_cycles;
};

/* Track n (1..9, MAPn) into out (cap bytes, any alignment): returns the image's size, 0 if a
   container or entry is missing or malformed, or the image does not fit (at most 7 MB: 28
   blocks). The small entries are unpacked into scratch (scratch_cap bytes, 270 KB for track
   1); MAPn.MLB, MAPn.UNR and the cars' .3DX files straight to their place in out. out and
   scratch are read back while they are written: ordinary memory, the SDRAM through the cache.
   Track 1 (4,166,400 bytes) took 237 M cycles in the simulation, 3.2 s: 73 M to unpack, the
   rest mostly the camera path (soft-float doubles) and the CRC-32 over 4 MB. */
uint32_t track_build(const struct track_files *f, int n, uint8_t *out, uint32_t cap,
                     uint8_t *scratch, uint32_t scratch_cap, struct track_build_info *info);
