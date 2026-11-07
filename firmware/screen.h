/*
 * What the boot loader and the program both draw: waiting for a buffer swap, and the test
 * band at the top of the screen.
 */
#pragma once
#include <stdint.h>

/* Height of the test band: rows 0..7. Column x holds palette index x for x < 256, else 0. */
#define BAND_H      8

/* Spin until the last buffer swap is done and return VID_STATUS. If this takes more than
   0.5 s, log it once. */
uint32_t wait_swap_done(void);

/* Draw the test band of docs/soc.md into the top BAND_H rows of fb */
void draw_band(volatile uint8_t *fb);
