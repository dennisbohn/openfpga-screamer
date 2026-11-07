/*
 * The game's files that the core reads from Assets/screamer/common/ on the SD card (data slots
 * 3 to 22 and 25, core/data.json), and the screen that lists them with [OK] or [MISSING] when
 * the game cannot start. SCREAM.gog, SCREAM.DAT and MUSIC.DAT only bring the music; the game
 * runs without them.
 */
#pragma once
#include <stdint.h>

/* Count the files the game needs that are missing, and return the count. The debug log gets a
   line "game files: n missing" followed by the names of all missing files, the optional ones
   too. */
int files_missing(void);

/* Draw the list into fb: every file with [OK] or [MISSING], and what to do about it. failed is
   set when all the needed files are there but loading them failed (the log says why). */
void files_draw(volatile uint8_t *fb, int failed);
