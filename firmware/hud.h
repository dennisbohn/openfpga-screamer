/*
 * The race screen of the outside views (the game's 0x23400 and 0x2367c, as OpenScreamer's
 * hud_draw has them): the labels, the rev counter with its needle, speed and gear, the
 * countdown, the lap, the position, the three clocks, the start lights and the end of the
 * race; as blitter commands (blit.h) that come after the scene. Not the game's: the frame
 * rate in the bottom left corner, frames drawn in the last second (0: none yet).
 *
 * In the cockpit (car_cockpit(), overlay 1 of the view table) 0x2367c draws the labels, then
 * the car's dashboard, its two needles and its gear lamp instead of the rev counter, speed and
 * gear; the rest of the race screen is the same, and its radar darkens the dashboard as well.
 * So the dashboard is a list of its own: the scene, the dashboard, the radar's window
 * (efx_radar_piece), then the rest.
 */
#pragma once
#include <stdint.h>
#include "blit.h"
#include "drive.h"

/* The race screen without the dashboard's parts, by the view's overlay (car_overlay()): 2 the
   labels and the outside views' instruments, 1 (the cockpit) neither, 0 (the trackside camera)
   the labels alone (0x2367c). In the replay (car_replay()) only a blinking R, the start lights
   and the end (0x2346c). */
void hud_build(struct blit_list *l, int buffer, int fps, int overlay);

/* The cockpit's dashboard for car sp (the car's own, 0x136c8): the picture COCKnn{A,B}L.PKC
   of the GFX container GAME.DAT (nn the model of "AUTOnn", B for the second paint job; 0x1b22c
   keeps it as it is packed) as blitter boxes into mem (cap bytes, 48 KB for every car), and
   the section COCKnnL of CARS.DTR in GEN.DAT (0x1545c, 29 numbers into 0x118f18): the needles'
   length, the first gear lamp sprite, the speed needle's pivot, the rev needle's pivot, the
   lamp's place for N, 1 .. 6. Both are unpacked into scratch (scratch_cap bytes, 32 KB) first.
   Returns 1 if all of it is there; else there is no dashboard (the labels alone). */
int hud_cockpit_init(const uint8_t *game, uint32_t game_size, const uint8_t *gen, uint32_t gen_size,
                     const struct car_spec *sp, uint8_t *mem, uint32_t cap, uint8_t *scratch,
                     uint32_t scratch_cap);

/* The labels and the dashboard (0x236dc): the picture, the speed needle and the rev needle as
   three lines each in colour 255 (0x2390c), the lamp of the gear (sprite first lamp + gear at
   that gear's place, 0x237b4) */
void hud_dash(struct blit_list *l, int buffer);

/* The pause: the game's CD player (0x21328, P in a race), over the race's last picture pic
   (320 x 200 bytes in the SDRAM) darkened twice. The car stereo CDFRONTL.PKC of the GFX
   container GAME.DAT, made boxes in mem (cap bytes, 64 KB) through scratch by hud_cd_init() (1
   if it is there); the disc's audio tracks first .. last 13 apart from (64, 78) in font 5 colour
   9, track (the one playing) left out while blink (0x2172b); the cursor on PLAY, the first of
   its seven buttons (table 0x1f1c: x 59, sprite 7) on row 97. Not the buttons' work: START
   leaves it, as PLAY does. */
int hud_cd_init(const uint8_t *game, uint32_t game_size, uint8_t *mem, uint32_t cap, uint8_t *scratch,
                uint32_t scratch_cap);
void hud_cd(struct blit_list *l, int buffer, const uint8_t *pic, int first, int last, int track, int blink);

/* PLAY AGAIN? (0x219d0) over pic, the race's last picture darkened three times: GAME.TXT's
   lines 19, 20 and 21 (PLAY AGAIN?, YES, NO) centred on rows 52, 92 and 122 in font 0, the
   question in colour 2, the answers in 14, the chosen one (YES unless no) in 2 while blink */
void hud_question(struct blit_list *l, int buffer, const uint8_t *pic, int no, int blink);
