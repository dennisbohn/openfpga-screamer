/*
 * What boot loader and program both draw: waiting for a swap, the test band.
 */
#pragma once
#include <stdint.h>

#define BAND_H      8           /* test band: rows 0..7, index x for x < 256, else 0 */

/* Spin until the last swap is done; returns VID_STATUS. Logs once if it takes 0.5 s. */
uint32_t wait_swap_done(void);

/* The test band of docs/soc.md in the top rows */
void draw_band(volatile uint8_t *fb);
