/*
 * The race screen of the outside views (original: STARTL.EXE 0x23400 and 0x2367c, as
 * OpenScreamer's hud_draw has them). It is made of blitter commands (blit.h) that come after
 * the scene: the labels, the rev counter with its needle, the speed and gear, the countdown,
 * the lap, the position, the three clocks, the start lights and the end of the race.
 *
 * In the cockpit (car_cockpit(), overlay 1 of the view table), 0x2367c draws the labels and
 * then the car's dashboard, its two needles and its gear lamp, instead of the rev counter,
 * speed and gear. The rest of the race screen is the same, and its radar darkens the
 * dashboard as well. So the dashboard is a list of its own: the scene, the dashboard, the
 * radar's window (efx_radar_piece), then the rest.
 */
#pragma once
#include <stdint.h>
#include "blit.h"
#include "drive.h"

/* Builds the race screen into the list l for the framebuffer `buffer`, without the
   dashboard's parts. What it draws depends on the view's overlay (car_overlay()):
     2  the labels and the outside views' instruments
     1  (the cockpit) neither
     0  (the trackside camera) the labels alone (original: STARTL.EXE 0x2367c)
   In the replay (car_replay()) it draws only a blinking R, the start lights and the end
   (original: 0x2346c). */
void hud_build(struct blit_list *l, int buffer, int overlay);

/*
 * Prepares the cockpit's dashboard for the car sp (the car's own, original: STARTL.EXE
 * 0x136c8). It loads two things.
 *   - The picture COCKnn{A,B}L.PKC of the GFX container GAME.DAT, as blitter boxes in mem
 *     (cap bytes, 48 KB for every car). nn is the model of "AUTOnn", and B is for the second
 *     paint job. The game keeps the picture as it is packed (original: 0x1b22c).
 *   - The section COCKnnL of CARS.DTR in GEN.DAT (original: 0x1545c, 29 numbers into 0x118f18):
 *     the needles' length, the first gear lamp sprite, the speed needle's pivot, the rev
 *     needle's pivot, and the lamp's place for N and for 1 .. 6.
 * Both are unpacked into scratch (scratch_cap bytes, 32 KB) first. Returns 1 if all of it is
 * there. Otherwise there is no dashboard, and only the labels are drawn.
 */
int hud_cockpit_init(const uint8_t *game, uint32_t game_size, const uint8_t *gen, uint32_t gen_size,
                     const struct car_spec *sp, uint8_t *mem, uint32_t cap, uint8_t *scratch,
                     uint32_t scratch_cap);

/* Builds the labels and the dashboard into the list l (original: STARTL.EXE 0x236dc): the
   picture, the speed needle and the rev needle as three lines each in colour 255 (0x2390c),
   and the lamp of the gear (the sprite first lamp + gear, at that gear's place, 0x237b4). */
void hud_dash(struct blit_list *l, int buffer);

/*
 * The pause: the game's CD player (original: STARTL.EXE 0x21328; P in a race). It is drawn over
 * the race's last picture pic (320 x 200 bytes in the SDRAM), darkened twice. hud_cd_init()
 * makes the car stereo picture CDFRONTL.PKC of the GFX container GAME.DAT into boxes in mem
 * (cap bytes, 64 KB), through scratch, and returns 1 if it is there.
 * hud_cd() draws, as main.c's pause_race keeps the state in struct cd_player:
 *   - The disc's audio tracks, first .. last, 13 pixels apart from (64, 78) in font 5, colour
 *     9. From the eleventh track on they are a row lower (original: 0x21712). The chosen track
 *     is left out while blink is set (0x2172b).
 *   - For a disc of one track or none: GAME.TXT's line 16 at (64, 70) in font 5, colour 3,
 *     instead, while the cursor is left of the - button (0x217a1). After EJECT, line 17 is
 *     drawn there (0x218a6).
 *   - The cursor is on button 0..6 (PLAY, EJECT, previous, next, -, +, which volume) on row
 *     97. The button's sprite is at its x (table 0x1f1c), and its pressed sprite while it is
 *     pressed.
 *   - On the -, + and third buttons, the volume chan (0 CD, 1 FX, 2 MU; original: 0x1f70) as
 *     0x23dac shows it: its name at (64, 69) in font 5, colour 3, then vol / 1024 strokes,
 *     each 1 pixel wide and 6 pixels high from row 69, 3 pixels apart from x 85. Ten strokes
 *     have each colour of 0x1fa0 (22, 214, 208).
 */
struct cd_player { int first, last, track, blink, button, pressed, eject, chan; int32_t vol; };
int hud_cd_init(const uint8_t *game, uint32_t game_size, uint8_t *mem, uint32_t cap, uint8_t *scratch,
                uint32_t scratch_cap);
void hud_cd(struct blit_list *l, int buffer, const uint8_t *pic, const struct cd_player *c);

/* PLAY AGAIN? (original: STARTL.EXE 0x219d0), drawn over pic, the race's last picture,
   darkened three times. The text is GAME.TXT's lines 19, 20 and 21 (PLAY AGAIN?, YES, NO),
   centred on rows 52, 92 and 122 in font 0. The question is in colour 2 and the answers are in
   colour 14. The chosen answer (YES unless no is set) is in colour 2 while blink is set. */
void hud_question(struct blit_list *l, int buffer, const uint8_t *pic, int no, int blink);
