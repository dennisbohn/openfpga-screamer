/*
 * The music: the CD's audio tracks as the game plays them (openscreamer docs/NOTES.md section
 * 17, MUSIC), from GOG's image of the disc, read straight off the SD card. SCREAM.gog (data
 * slot 22) is the raw disc, 2352 bytes a sector, the audio 44.1 kHz 16-bit stereo as the mixer
 * takes it; SCREAM.DAT (slot 21) is its cue sheet, which says where each track begins (INDEX 01;
 * a track ends where the next begins, the last at the end of the image). The game's song number
 * n is CD_TRACKS in GEN.DAT's MENU.DTR (menu 2, tracks 1-6: 5, 3, 4, 6, 7, 1), and it asks the
 * drive for CD track n + 1; the drive's poll plays it again when it ends, so a song loops.
 *
 * The mixer (audio.v, hw.h AU_MUSIC*) plays the music from a ring of 256 KB in the SDRAM at
 * 0x4000 of 0x7fff: the game sets the CD master to 0x6000 (0.75 as MSCDEX takes it), and like
 * the effects' master (sound.h) it is two thirds of that here, which keeps the game's balance of
 * music and effects and the sum mostly inside 16 bits (at 0x6000 the start line's engines and
 * the music clipped in the simulation, 267 frames in a second). music_frame() keeps the ring
 * filled ahead of the mixer's place with reads of 32 KB that the CPU only starts (slot_start):
 * the APF writes them into the SDRAM while the frame is drawn, the CPU looks once a frame.
 */
#pragma once
#include <stdint.h>

/* The cue sheet from its data slot and the song numbers from GEN.DAT (whole in memory; without
   MENU.DTR the game's own values): the CD's audio tracks, 0 without the image */
int music_init(const uint8_t *gen, uint32_t gen_size);
/* The options page's MUSIC (menu.h): 0 off, music_play only stops the music */
extern int music_on;
/* Song k: 0 the menu's, 1-6 the race tracks'. Unless it plays already, the music before stops,
   the ring is filled (224 KB, 1.3 s of music; waiting, about 170 ms) and the song starts; no
   read is left running. */
void music_play(int k);
/* The CD player's (hud.h): CD track n from its start, even if it plays already, and again at
   its end (0x2636c with the repeat flag, as music_play) */
void music_cd(int n);
/* The CD player's stop (0x26458, MSCDEX's STOP AUDIO while it plays: a pause) and resume
   (0x26498): the music stops, its place kept; music_resume goes on from there (to the sector)
   with the same track, if music_pause stopped one and nothing played or stopped since */
void music_pause(void);
void music_resume(void);
/* Once a frame while a song plays: a read done is taken, the next one started if the ring
   has room */
void music_frame(void);
/* The music stops; a read still running is waited for (before any other slot call) */
void music_stop(void);
/* The music at f of 0x8000 of its volume (the pause's fade); a song started has it whole */
void music_fade(int32_t f);
/* The game's CD master volume v of 0x7fff (0x1d2bc, CHOICE.DAT's +0x448; the CD player
   changes it): the music's volume is two thirds of it, MASTER at the game's 0x6000 */
void music_master(int32_t v);
/* The game's MIDI master volume (0x1d2c0, CHOICE.DAT's +0x44c, the CD player's MU): the FM
   music's, see fm.h */
void music_master_mu(int32_t v);
/* For the CD player: the disc's first and last audio track (0 without the image) and the CD
   track playing (0 none) */
int music_disc(int *lo, int *hi);
