/*
 * The intro, the game's attract sequence (original: STARTL.EXE 0x276a0; openscreamer
 * docs/NOTES.md sections 26 and 30). It is a real race without a player (mode 0x100, flags
 * 0x2001: started at once, attract) on track CHOICE.DAT +0x434 + 1, difficulty NORMAL. In each
 * frame only the opponents, the props and moving scenery and the camera run (original: 0x2780c).
 *
 * The camera (view-table row 4, routine 8: original 0x1aeb4) shows one car from its own
 * camera (0x1a920). A camera stands on a post, a node of MAPnCAM.SPL. The car's camera is on
 * the post nearest the car. It is turned to the car and closes in to 1500 units, as the post's
 * code says. Every 10 s (0x27e64) another car at another post is lined up (0x19d82), and it
 * is shown when the shown car moves on to its next post (0x1aef3).
 *
 * Over the picture (0x279d4) come the logos of LOGOL.SPR (0x279fc) and the credits. The
 * credits are ATTR.TXT's blocks, separated by '#' lines, in the font ATTRL.FNT (0x27b64).
 * A key, or the end of the credits, fades picture and sound out in a second (0x27d28). Then
 * the main menu follows.
 */
#pragma once
#include <stdint.h>
#include "blit.h"
#include "track.h"

/* Loads LOGOL.SPR and ATTRL.FNT of GFX/GRAF.DAT (at their places in the file, as loaded),
   laid out for the blitter in mem (cap bytes of the SDRAM). Splits ATTR.TXT of DATA/LOCAL.DAT
   into lines. The files are unpacked through scratch. Returns 1 if all of it is there. */
int intro_init(const uint8_t *graf, uint32_t graf_size, const uint8_t *local, uint32_t local_size,
               uint8_t *mem, uint32_t cap, uint8_t *scratch, uint32_t scratch_cap);

/* Starts the intro on track n. Its posts, MAPnCAM.SPL, come from the DATA container dat.
   Call it after opp_init(). */
void intro_start(const uint8_t *dat, uint32_t dat_size, int n);

/* One step of 1/50 s, after the opponents' (opp_step): the logos and credits, the cameras,
   the sounds heard from the car shown (snd_ear), and the fade out once the intro is leaving.
   Returns 1 once the intro is over. */
int intro_step(void);

/* A key was pressed: leave (the fade out; original: STARTL.EXE 0x211d0 sets bit 2 of
   0x11ce40). */
void intro_leave(void);

/* The picture's and the sound's level while it fades out: 0x8000 is the whole level. */
int32_t intro_level(void);

/* The camera shown, and the logos and credits over the picture, for the blitter. */
void intro_camera(struct track_cam *cam);
void intro_text(struct blit_list *l);
