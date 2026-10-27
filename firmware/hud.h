/*
 * The race screen of the outside views (the game's 0x23400 and 0x2367c, as OpenScreamer's
 * hud_draw has them): the labels, the rev counter with its needle, speed and gear, the
 * countdown, the lap, the position, the three clocks, the start lights and the end of the
 * race; as blitter commands (blit.h) that come after the scene.
 */
#pragma once
#include "blit.h"

void hud_build(struct blit_list *l, int buffer);
