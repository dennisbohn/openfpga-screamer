/*
 * The track image's race screen section (header 0x68), built on the Pocket from the game's own
 * files as tools/make_track.py's race_screen() and pkc() build it, byte for byte. The files are
 * GAMEL.SPR, GAMEL.FNT, IMPLET_L.PKC and IMPRES_L.PKC of GFX/GAME.DAT, and GAME.TXT of
 * DATA/LOCAL.DAT. The caller unpacks each from its container.
 */
#pragma once
#include <stdint.h>

/* Makes the section into out (cap bytes). The section holds, in this order:
   1. u32 number of images, u32 number of fonts.
   2. The image table. Per image: u32 offset of its pixels (from the section), u16 width,
      u16 height, int16 x, int16 y.
   3. The font table. Per font: u32 height, first character, count, widest, first image.
   4. The two pictures' box ranges: u32 first box, count of IMPLET_L, then the same of IMPRES_L.
   5. GAME.TXT's first 26 lines, 32 bytes each.
   6. The images' pixels: the sprites, the fonts' characters, the two pictures' boxes.
   Returns the section's size. Returns 0 if a file is malformed (a read outside it, a number the
   section cannot hold, a .PKC run past the file's end), or if the section does not fit.
   It needs no scratch memory: the pictures are walked run by run, not unpacked. */
uint32_t tb_hud_build(const uint8_t *spr, uint32_t spr_size, const uint8_t *fnt, uint32_t fnt_size,
                      const uint8_t *implet, uint32_t implet_size, const uint8_t *impres,
                      uint32_t impres_size, const uint8_t *txt, uint32_t txt_size, uint8_t *out,
                      uint32_t cap);

/* One .PKC picture alone as boxes into out (cap bytes), cut as the section's two pictures are:
   u32 count; per box u32 offset (from out), u16 width, u16 height, int16 x, int16 y; then the
   boxes' pixels. Returns the size. Returns 0 if the file is malformed or the boxes do not fit.
   (It is used for the cockpit's dashboard at the race's start, see hud.h.) */
uint32_t tb_hud_picture(const uint8_t *pkc, uint32_t size, uint8_t *out, uint32_t cap);
