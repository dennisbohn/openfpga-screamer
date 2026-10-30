/*
 * The track screen of the game's menu (its screen 3; openscreamer docs/NOTES.md sections 18 and
 * 26, port/src/main.c menu_frame and ms_*): the six tracks' models AMAPn.3DX of GFX/MENU.DAT
 * stand in a row 40000 units apart, each turning on the spot (0x2c078, 0x2cb88); the camera
 * (0x29530) looks down at the chosen one and slides to its neighbour on left or right. Behind
 * them the picture MENU3L.PCX of GFX/GRAF.DAT, whose palette the screen uses; over them (0x2e384)
 * the title (MENU.TXT of DATA/LOCAL.DAT), the track's name (TRACKDAT.TXT) in the fonts of
 * MENUL.FNT and the arrows of MENUL.SPR. The models go through the geometry unit with MENU.MLB as
 * its material file (every face of theirs is one colour, routine 128); the rest is the blitter's.
 */
#pragma once
#include <stdint.h>
#include "blit.h"
#include "setup.h"

#define MENU_TRACKS 6

/* What menu_init() reads of the two containers: these entries, MENU.MLB only its first 32 KB
   (its materials: a face's is a 16-bit offset into it); a NULL name ends the list */
struct menu_part { const char *name; uint32_t most; };
extern const struct menu_part menu_dat_parts[], menu_graf_parts[];

/* From GFX/MENU.DAT, GFX/GRAF.DAT and DATA/LOCAL.DAT as loaded (of the first two only the
   parts above need be there, at their places in the file): MENU.MLB and the models used where
   they are, the picture unpacked and the sprites and fonts laid out in mem (cap bytes of the
   SDRAM, for the blitter), the packed entries through scratch. 1 if all of it is there. */
int menu_init(const uint8_t *menu_dat, uint32_t menu_size, const uint8_t *graf, uint32_t graf_size,
              const uint8_t *local, uint32_t local_size, uint8_t *mem, uint32_t cap, uint8_t *scratch,
              uint32_t scratch_cap);

/* The screen's palette (256 x 0x00RRGGBB) and the material file for the geometry unit */
const uint32_t *menu_palette(void);
const uint8_t *menu_mlb(void);

/* Onto the screen with track n (1 .. MENU_TRACKS) chosen, the camera at it */
void menu_enter(int n);

/* One step of 1/50 s with the keys (IN_KEY) held now and on the step before: left and right
   choose (a press is taken once the camera is most of the way to the last one), B or START take
   the track. Returns its number, 0 while none is taken. */
int menu_step(uint32_t keys, uint32_t keys_before);

/* The frame in the order it is drawn: the picture (for the blitter), the projection and the
   models (through track_hw_piece in TRACK_HW, else the C models into fb, track.h), the title,
   name and arrows (for the blitter) */
void menu_backdrop(struct blit_list *l);
void menu_view(struct su_view *v);
void menu_scene(volatile uint8_t *fb);
void menu_text(struct blit_list *l);
