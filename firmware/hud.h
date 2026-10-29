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

/* The race screen without the dashboard's parts: with cockpit, without the labels and the
   outside views' instruments */
void hud_build(struct blit_list *l, int buffer, int fps, int cockpit);

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
