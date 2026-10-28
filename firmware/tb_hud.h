/*
 * The track image's race screen section (header 0x68), built on the Pocket from the game's own
 * files as tools/make_track.py's race_screen() and pkc() build it, byte for byte: GAMEL.SPR,
 * GAMEL.FNT, IMPLET_L.PKC and IMPRES_L.PKC of GFX/GAME.DAT and GAME.TXT of DATA/LOCAL.DAT,
 * each unpacked from its container.
 */
#pragma once
#include <stdint.h>

/* The section into out (cap bytes): u32 images, u32 fonts; per image u32 offset (from the
   section), u16 width, height, int16 x, y; per font u32 height, first character, count,
   widest, first image; u32 first box, count of IMPLET_L, then of IMPRES_L; GAME.TXT's first
   26 lines, 32 bytes each; the images' pixels: the sprites, the fonts' characters, the two
   pictures' boxes. Returns the section's size, 0 if a file is malformed (a read outside it,
   a number the section cannot hold, a .PKC run past the file's end) or the section does not
   fit. Works without scratch memory: the pictures are walked run by run, not unpacked. */
uint32_t tb_hud_build(const uint8_t *spr, uint32_t spr_size, const uint8_t *fnt, uint32_t fnt_size,
                      const uint8_t *implet, uint32_t implet_size, const uint8_t *impres, uint32_t impres_size,
                      const uint8_t *txt, uint32_t txt_size, uint8_t *out, uint32_t cap);
