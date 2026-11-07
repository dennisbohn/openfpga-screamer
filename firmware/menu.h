/*
 * The game's menus (menu.c): the pages (screen 0), the car screen (2), the track screen (3), the
 * name screen (4), the championship screen (5), the records screen (6) and the loading screen.
 * The entry points are menu_init() to load, menu_enter() and menu_step() to run, and
 * menu_backdrop(), menu_view(), menu_scene() and menu_text() to draw.
 * The layout follows openscreamer (docs/NOTES.md sections 18 and 26, port/src/main.c menu_frame
 * and ms_*). Addresses in the form (original: STARTL.EXE 0x...) are places in the original's
 * executable.
 *
 * The pages: the main menu and the options (original: STARTL.EXE 0x2de4).
 * - Each word is spelt in cubes of ALETTER.3DX of GFX/MENU.DAT, one cube a letter. The face of
 *   a cube is the letter's frame of the cube's texture (original: STARTL.EXE 0x2b33c).
 * - The picture MENU1L.PCX is behind the cubes. The two SCREAMER logos of MENUL.SPR slide in
 *   over them (original: STARTL.EXE 0x2df38).
 *
 * The track and car screens: the models stand in a row, each turning on the spot. The camera
 * looks down at the chosen one and slides to its neighbour on the left or right (original:
 * STARTL.EXE 0x29530).
 * - Track screen: the six tracks' AMAPn.3DX of GFX/MENU.DAT, 40000 units apart (original:
 *   STARTL.EXE 0x2c078, 0x2cb88). Behind them is the picture MENU3L.PCX of GFX/GRAF.DAT. Over
 *   them are the title (MENU.TXT of DATA/LOCAL.DAT) and the track's name (TRACKDAT.TXT)
 *   (original: STARTL.EXE 0x2e384).
 * - Car screen: a car for each row of CARS.DTR (GEN.DAT), its AAUTOnn.3DX in its paint job,
 *   4000 apart (original: STARTL.EXE 0x2bdcc, 0x2ca10). Behind them is MENU4L.PCX. Over them
 *   are the title, the car's name, gearbox, top speed and its grip and acceleration bars
 *   (CARDAT.TXT) (original: STARTL.EXE 0x2e08c).
 *
 * The texts are in the fonts of MENUL.FNT, the arrows and bars come from MENUL.SPR. Each screen
 * has its picture's palette. The models go through the geometry unit with MENU.MLB as its
 * material file (the tracks' faces are one colour each, the cars' are textured). The rest is
 * the blitter's.
 *
 * The other screens:
 * - Loading screen, while the race is set up (original: STARTL.EXE 0x2a264): LOADnL.PCX of
 *   GRAF.DAT with LOADING (MENU.TXT) over it.
 * - Championship screen (screen 5, original: STARTL.EXE 0x2ce58, champ.h): the stage's
 *   CHAMPnL.PCX with the stage's lines of CHAMP.TXT (LOCAL.DAT), or the standings, over it.
 * - VIEW RECORDS (the options' last line) opens the page of the six tracks (the game's
 *   page 6). Each track there has a screen 6 (original: STARTL.EXE 0x2d348): its LOADnL.PCX
 *   darkened, with the five best totals and laps of CHOICE.DAT (champ.h) over it.
 * - ENTER YOUR NAME (screen 4, before every game, original: STARTL.EXE 0x2c320): the cubes of
 *   the 26 letters stand in a ring and drop in. The camera walks round outside to the
 *   alphabet's letter. MENU2L.PCX is behind them, and the title and the name are over them
 *   (original: STARTL.EXE 0x2e4f8).
 */
#pragma once
#include <stdint.h>
#include "blit.h"
#include "setup.h"

#define MENU_TRACKS 6
#define MENU_CARS   14          /* CARDAT.TXT's entries (the game's 14 records at 0x1d54c) */

/* The game's numbers of the screens (original: STARTL.EXE 0x1fac0). */
#define MENU_PAGE_SCREEN    0
#define MENU_CAR_SCREEN     2
#define MENU_TRACK_SCREEN   3
#define MENU_NAME_SCREEN    4       /* ENTER YOUR NAME, before every game (0x2c320, 0x2e4f8) */
#define MENU_CHAMP_SCREEN   5       /* the championship's stage (champ.h) */
#define MENU_RECORDS_SCREEN 6       /* VIEW RECORDS: a track's (0x2c844, 0x2d348) */

/* What the pages' NEW and RESUME of CHAMPIONSHIP return: the game's items 26 and 27, plus 1. */
#define MENU_CHAMP_NEW      27
#define MENU_CHAMP_RESUME   28
/* A track's line on the page of VIEW RECORDS returns this plus the track (1 .. MENU_TRACKS). */
#define MENU_RECORDS        40
/* 30 s on the main menu without a key return this: the intro follows (intro.h). */
#define MENU_IDLE           50

/* The choices of the options page (the game's page 1, kept in CHOICE.DAT):
   - detail: 0 LOW .. 2 HI. It is only shown; the core always draws the H set whole.
   - difficulty: 0 ROOKIE, 1 NORMAL, 2 PRO (MAPnA..C.DTR, original: STARTL.EXE 0x13251),
     3 AFTERBURNER (MAPnE.DTR).
   - laps: the laps of a race (3, 5, 10 or 25: original: STARTL.EXE 0x21e0 + 1).
   - speech and music: 1 on, 0 off.
   The game's defaults are HI, NORMAL, 3, on, on. */
struct menu_options { int detail, difficulty, laps, speech, music; };
void menu_options(struct menu_options *o);
/* Loads the options from CHOICE.DAT (champ.h, save.h) at boot. Every change goes back there.
   The highest value of the difficulty is CHOICE.DAT's: 3 (AFTERBURNER, MAPnE.DTR) once the
   Bullet is won. */
void menu_options_load(void);
/* The game mode of the game that menu_step() started last (car.h's CAR_NORMAL and so on; the
   game's word 0x1d25c). */
int menu_mode(void);
/* Puts the menu as the intro leaves it (original: STARTL.EXE 0x27eb4): each page at its first
   line. */
void menu_fresh(void);

/* What menu_init() reads of the two containers: these entries, and MENU.MLB whole (the cars'
   textures lie all over it). A NULL name ends the list. */
struct menu_part { const char *name; uint32_t most; };
extern const struct menu_part menu_dat_parts[], menu_graf_parts[];

/* Loads the menus from the containers GFX/MENU.DAT, GFX/GRAF.DAT, DATA/LOCAL.DAT and
   DATA/GEN.DAT, given as the data slots loaded them. Of the first two only the parts listed
   above need be there, at their places in the file. MENU.MLB and the models are used where
   they are. The pictures are unpacked, and the sprites and fonts are laid out in mem (cap
   bytes of the SDRAM, for the blitter). The packed entries go through scratch. Returns 1 if
   all of it is there. */
int menu_init(const uint8_t *menu_dat, uint32_t menu_size, const uint8_t *graf, uint32_t graf_size,
              const uint8_t *local, uint32_t local_size, const uint8_t *gen, uint32_t gen_size, uint8_t *mem,
              uint32_t cap, uint8_t *scratch, uint32_t scratch_cap);

/* Loads only the loading screen's part of menu_init() (menu_loading), for the boot's first
   picture before the rest is loaded. Of GRAF.DAT only MENUL.SPR, MENUL.FNT and the LOADnL.PCX
   to be shown need be there; LOADING comes from LOCAL.DAT. Returns 1 if they are there.
   menu_init() does it again. */
int menu_loading_init(const uint8_t *graf, uint32_t graf_size, const uint8_t *local, uint32_t local_size,
                      uint8_t *mem, uint32_t cap, uint8_t *scratch, uint32_t scratch_cap);

/* Returns the number of cars on the car screen: the rows of CARS.DTR that CARDAT.TXT describes.
   Of them the first 12 can be chosen, and the Bullet's row 12 too once it is won. The tracks
   up to CHOICE.DAT's level + 3 can be chosen (choices() in menu.c). */
int menu_cars(void);

/* The screen's palette (256 entries of 0x00RRGGBB) and the material file for the geometry unit. */
const uint32_t *menu_palette(void);
const uint8_t *menu_mlb(void);

/* Goes to a screen. The meaning of n depends on the screen:
   - MENU_TRACK_SCREEN or MENU_CAR_SCREEN: choice n is chosen (the track 1 .. MENU_TRACKS, or
     the car's row + 1) and the camera is at it.
   - MENU_PAGE_SCREEN: page n - 1 (1 the main menu, 2 the options, 7 the tracks of VIEW
     RECORDS; 0 the page shown last). Its words fly in (original: STARTL.EXE 0x29ea0). Each
     page keeps its line.
   - MENU_CHAMP_SCREEN: the championship's stage now (original: STARTL.EXE 0x2c698). n is not
     used.
   - MENU_RECORDS_SCREEN: track n's records.
   - MENU_NAME_SCREEN: n is not used. */
void menu_enter(int screen, int n);

/* One step of 1/50 s with the keys (IN_KEY) held now and on the step before. Returns 0 while
   no choice is taken. The keys of each screen:

   Track and car screen:
   - Left and right choose among the unlocked ones. A press is taken once the camera is most of
     the way to the last one. The menu's sound SND_MENU_MOVE goes with it (sound.h).
   - B or START take the choice: the result is its number, as menu_enter's n.
   - A or SELECT go back: -1. Neither makes a sound, as in the game. Leaving the track screen
     marks its NEW TRACK AVAILABLE as seen.

   Pages (original: STARTL.EXE 0x28080):
   - Up and down go from line to line (SND_MENU_LINE).
   - Left and right change an option. B or START do too, as the game's ENTER (SND_MENU_OPTION).
     This works once the words have landed (original: STARTL.EXE 0x297d4).
   - B or START on OPTIONS, SPECIAL MODES, CHAMPIONSHIP or TIME ATTACK opens its page (the
     game's pages 1, 4, 5 and 3; SND_MENU_PAGE).
   - A or SELECT go back a page (the same sound).
   - NORMAL GAME, time attack's NEW and RESUME (ghost_resume, ghost.h), CONE CARNAGE and SLALOM
     return 1. The championship's NEW and RESUME return MENU_CHAMP_NEW and MENU_CHAMP_RESUME.
     They play SND_MENU_GAME. menu_mode() gives the game's mode. From then on the camera pulls
     back and rolls (original: STARTL.EXE 0x293e8) while the caller fades out at 2 a second,
     and the keys are not taken. The game's NETWORK GAME is left out.
   - VIEW RECORDS opens the page of the six tracks (SND_MENU_PAGE); A or SELECT there go back
     to the options. A track there leaves as a game does: it returns MENU_RECORDS + the track.
   - 30 s on the main menu without a key return MENU_IDLE, and the caller fades out (the intro
     is next).

   Championship screen: B or START, or 10 s without a key (original: STARTL.EXE 0x2d301), act
   as ENTER and return 1. A or SELECT on a race's text screen acts as ESC (original:
   STARTL.EXE 0x2829d) and returns -1. Both are silent.

   A track's records: up and down choose a total (SND_MENU_RECORD). A or SELECT return -1.

   Name screen: up and down choose the letter being set (SND_MENU_LETTER). Left and right turn
   the alphabet and set the letter (CHOICE.DAT's name). B or START return 1, A or SELECT -1. */
int menu_step(uint32_t keys, uint32_t keys_before);

/* The frame, in the order it is drawn:
   1. menu_backdrop(): the picture (for the blitter).
   2. menu_view() and menu_scene(): the projection, then the models or cubes. They go through
      track_hw_piece in TRACK_HW, else the C models draw into fb (track.h).
   3. menu_text(): the texts, bars, arrows and logos (for the blitter). */
void menu_backdrop(struct blit_list *l);
void menu_view(struct su_view *v);
void menu_scene(volatile uint8_t *fb);
void menu_text(struct blit_list *l);

/* Makes the loading screen of track n (1 .. MENU_TRACKS) for the blitter (original: STARTL.EXE
   0x2a264). The game shows it before every race but PLAY AGAIN; there is no fade. It is the
   picture LOADnL.PCX of GFX/GRAF.DAT with LOADING (MENU.TXT) centred over it in font 3,
   colour 0. Returns the picture's palette, or 0 without it (no menu_init). The picture is in
   the SDRAM once the CPU's writes are. */
const uint32_t *menu_loading(int n, struct blit_list *l);
