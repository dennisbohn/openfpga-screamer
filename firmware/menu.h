/*
 * The game's menu: its pages (screen 0), the track and car screens (its screens 3 and 2;
 * openscreamer docs/NOTES.md sections 18 and 26, port/src/main.c menu_frame and ms_*). The
 * pages: the main menu and the options (0x2de4), each word spelt in cubes of ALETTER.3DX of
 * GFX/MENU.DAT (0x2b33c), one a letter, its face the letter's frame of the cube's texture,
 * before the picture MENU1L.PCX, the two SCREAMER logos of MENUL.SPR sliding in over them
 * (0x2df38). On the other two screens the models stand in a row, each
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

/* the game's numbers of the screens (0x1fac0) */
#define MENU_PAGE_SCREEN    0
#define MENU_CAR_SCREEN     2
#define MENU_TRACK_SCREEN   3

/* The options page's choices (the game's page 1, kept for the session): DETAIL 0 LOW .. 2 HI
   (shown only: the core always draws the H set whole), the difficulty 0 ROOKIE, 1 NORMAL, 2 PRO
   (MAPnA..C.DTR, 0x13251), the laps of a race (3, 5, 10 or 25: 0x21e0 + 1), speech and music 1
   on, 0 off. The game's defaults: HI, NORMAL, 3, on, on. */
struct menu_options { int detail, difficulty, laps, speech, music; };
void menu_options(struct menu_options *o);

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
   MENU_TRACKS, the car's row + 1), the camera at it; onto MENU_PAGE_SCREEN at page n - 1 (1
   the main menu, 2 the options), its words flying in (0x29ea0), each page's line kept */
void menu_enter(int screen, int n);

/* One step of 1/50 s with the keys (IN_KEY) held now and on the step before. Track and car
   screen: left and right choose (a press is taken once the camera is most of the way to the
   last one; the menu's sound SND_MENU_MOVE with it, sound.h), B or START take the choice: its
   number as menu_enter's n; A or SELECT go back: -1 (both without a sound, as the game). The
   pages (0x28080): up and down go from line to line (SND_MENU_LINE), left and right change an
   option, B or START too, as the game's ENTER (SND_MENU_OPTION; once the words have landed,
   0x297d4), B or START on OPTIONS opens its page (SND_MENU_PAGE), A or SELECT go back to the
   main menu (the same sound); NORMAL GAME: 1 (SND_MENU_GAME), from then on the camera pulls back
   and rolls (0x293e8) while the caller fades out at 2 a second, the keys not taken. NETWORK
   GAME shows the game's NETWORK NOT FOUND; CHAMPIONSHIP, SPECIAL MODES and VIEW RECORDS only
   sound, for now. 0 while none is taken. */
int menu_step(uint32_t keys, uint32_t keys_before);

/* The frame in the order it is drawn: the picture (for the blitter), the projection and the
   models or cubes (through track_hw_piece in TRACK_HW, else the C models into fb, track.h), the
   texts, bars, arrows and logos (for the blitter) */
void menu_backdrop(struct blit_list *l);
void menu_view(struct su_view *v);
void menu_scene(volatile uint8_t *fb);
void menu_text(struct blit_list *l);

/* The loading screen of track n (1 .. MENU_TRACKS) for the blitter (0x2a264, the game's before
   every race but PLAY AGAIN; no fade): its picture LOADnL.PCX of GFX/GRAF.DAT and LOADING
   (MENU.TXT) centred over it in font 3, colour 0. Returns the picture's palette, 0 without it
   (no menu_init). The picture is in the SDRAM once the CPU's writes are. */
const uint32_t *menu_loading(int n, struct blit_list *l);
