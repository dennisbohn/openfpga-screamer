/*
 * The track and car screens of the game's menu (its screens 3 and 2; openscreamer docs/NOTES.md
 * sections 18 and 26, port/src/main.c menu_frame and ms_*): the models stand in a row, each
 * turning on the spot, the camera looks down at the chosen one and slides to its neighbour on
 * left or right (0x29530). The track screen: the six tracks' AMAPn.3DX of GFX/MENU.DAT, 40000
 * units apart (0x2c078, 0x2cb88), behind them the picture MENU3L.PCX of GFX/GRAF.DAT, over them
 * (0x2e384) the title (MENU.TXT of DATA/LOCAL.DAT) and the track's name (TRACKDAT.TXT). The car
 * screen: a car for each row of CARS.DTR (GEN.DAT), its AAUTOnn.3DX in its paint job, 4000 apart
 * (0x2bdcc, 0x2ca10), behind them MENU4L.PCX, over them (0x2e08c) the title, the car's name,
 * gearbox, top speed and its grip and acceleration bars (CARDAT.TXT). The texts in the fonts of
 * MENUL.FNT, the arrows and bars of MENUL.SPR; each screen has its picture's palette. The models
 * go through the geometry unit with MENU.MLB as its material file (the tracks' faces are one
 * colour each, the cars' textured); the rest is the blitter's. Then, while the race is set up,
 * the loading screen (0x2a264): LOADnL.PCX of GRAF.DAT with LOADING (MENU.TXT) over it.
 */
#pragma once
#include <stdint.h>
#include "blit.h"
#include "setup.h"

#define MENU_TRACKS 6
#define MENU_CARS   14          /* CARDAT.TXT's entries (the game's 14 records at 0x1d54c) */

/* the game's numbers of the two screens (0x1fac0) */
#define MENU_CAR_SCREEN     2
#define MENU_TRACK_SCREEN   3

/* What menu_init() reads of the two containers: these entries, MENU.MLB whole (the cars'
   textures lie all over it); a NULL name ends the list */
struct menu_part { const char *name; uint32_t most; };
extern const struct menu_part menu_dat_parts[], menu_graf_parts[];

/* From GFX/MENU.DAT, GFX/GRAF.DAT, DATA/LOCAL.DAT and DATA/GEN.DAT as loaded (of the first two
   only the parts above need be there, at their places in the file): MENU.MLB and the models
   used where they are, the pictures unpacked and the sprites and fonts laid out in mem (cap
   bytes of the SDRAM, for the blitter), the packed entries through scratch. 1 if all of it is
   there. */
int menu_init(const uint8_t *menu_dat, uint32_t menu_size, const uint8_t *graf, uint32_t graf_size,
              const uint8_t *local, uint32_t local_size, const uint8_t *gen, uint32_t gen_size, uint8_t *mem,
              uint32_t cap, uint8_t *scratch, uint32_t scratch_cap);

/* The cars that can be chosen: CARS.DTR's rows that CARDAT.TXT describes (all of them, the
   Bullet too, as every track is offered) */
int menu_cars(void);

/* The screen's palette (256 x 0x00RRGGBB) and the material file for the geometry unit */
const uint32_t *menu_palette(void);
const uint8_t *menu_mlb(void);

/* Onto screen MENU_TRACK_SCREEN or MENU_CAR_SCREEN with choice n chosen (the track 1 ..
   MENU_TRACKS, the car's row + 1), the camera at it */
void menu_enter(int screen, int n);

/* One step of 1/50 s with the keys (IN_KEY) held now and on the step before: left and right
   choose (a press is taken once the camera is most of the way to the last one), B or START take
   the choice: its number as menu_enter's n; on the car screen Y or SELECT go back: -1. 0 while
   none is taken. */
int menu_step(uint32_t keys, uint32_t keys_before);

/* The frame in the order it is drawn: the picture (for the blitter), the projection and the
   models (through track_hw_piece in TRACK_HW, else the C models into fb, track.h), the texts,
   bars and arrows (for the blitter) */
void menu_backdrop(struct blit_list *l);
void menu_view(struct su_view *v);
void menu_scene(volatile uint8_t *fb);
void menu_text(struct blit_list *l);

/* The loading screen of track n (1 .. MENU_TRACKS) for the blitter (0x2a264, the game's before
   every race but PLAY AGAIN; no fade): its picture LOADnL.PCX of GFX/GRAF.DAT and LOADING
   (MENU.TXT) centred over it in font 3, colour 0. Returns the picture's palette, 0 without it
   (no menu_init). The picture is in the SDRAM once the CPU's writes are. */
const uint32_t *menu_loading(int n, struct blit_list *l);
