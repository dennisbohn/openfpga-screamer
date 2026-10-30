/*
 * The intro, the game's attract sequence (0x276a0; openscreamer docs/NOTES.md sections 26 and
 * 30): a real race without a player (mode 0x100, flags 0x2001: started at once, attract), on
 * track CHOICE.DAT +0x434 + 1, difficulty NORMAL; each frame only the opponents, the props and
 * moving scenery and the camera run (0x2780c). The camera (view-table row 4, routine 8:
 * 0x1aeb4) shows one car from its own camera (0x1a920): the post of MAPnCAM.SPL nearest it,
 * turned to it, closing in to 1500 units by the post's code; every 10 s (0x27e64) another car at
 * another post is lined up (0x19d82) and taken at the shown car's next post (0x1aef3). Over it
 * (0x279d4) the logos of LOGOL.SPR (0x279fc) and the credits, ATTR.TXT's '#'-separated blocks in
 * ATTRL.FNT (0x27b64). A key, or the credits' end, fades picture and sound out in a second
 * (0x27d28); then the main menu.
 */
#pragma once
#include <stdint.h>
#include "blit.h"
#include "track.h"

/* LOGOL.SPR and ATTRL.FNT of GFX/GRAF.DAT (at their places in the file, as loaded) laid out for
   the blitter in mem (cap bytes of the SDRAM), ATTR.TXT of DATA/LOCAL.DAT split into lines;
   unpacked through scratch. 1 if all of it is there. */
int intro_init(const uint8_t *graf, uint32_t graf_size, const uint8_t *local, uint32_t local_size, uint8_t *mem,
               uint32_t cap, uint8_t *scratch, uint32_t scratch_cap);

/* The start on track n, its posts MAPnCAM.SPL from its DATA container dat (after opp_init) */
void intro_start(const uint8_t *dat, uint32_t dat_size, int n);

/* One step of 1/50 s, after the opponents' (opp_step): the logos and credits, the cameras, the
   sounds heard from the one shown (snd_ear), the fade out once leaving. 1 once it is over. */
int intro_step(void);

/* A key: leave (the fade out, 0x211d0 sets bit 2 of 0x11ce40) */
void intro_leave(void);

/* The picture's and the sound's level while it fades out, 0x8000 whole */
int32_t intro_level(void);

/* The camera shown, and the logos and credits over the picture for the blitter */
void intro_camera(struct track_cam *cam);
void intro_text(struct blit_list *l);
