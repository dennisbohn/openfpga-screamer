/*
 * The game's music (openscreamer docs/NOTES.md 17, MUSIC): the CD's audio tracks as the game
 * plays them, or, as the other source, its MIDI songs on the FM chip (fm.h).
 *
 * Data flow of the CD source: SD card, then a ring in the SDRAM, then the mixer.
 *   - SCREAM.gog (data slot 22) is GOG's image of the disc, read straight off the SD card. It is
 *     the raw disc, 2352 bytes a sector. The audio is 44.1 kHz 16-bit stereo, as the mixer takes it.
 *   - SCREAM.DAT (slot 21) is its cue sheet. It says where each track begins (INDEX 01). A track
 *     ends where the next one begins, the last at the end of the image.
 *   - The APF reads the track into a ring of 256 KB in the SDRAM. The mixer (audio.v, hw.h
 *     AU_MUSIC*) plays the ring and reports its place in it.
 *   - The game's song number n is CD_TRACKS in GEN.DAT's MENU.DTR (menu 2, tracks 1-6: 5, 3, 4,
 *     6, 7, 1), and it asks the drive for CD track n + 1. The drive's poll plays the track again
 *     when it ends, so a song loops.
 * The FM source plays the songs of DATA/MUSIC.DAT (data slot 25). The Pocket menu's Music Source
 * chooses: Auto takes the CD image if it has audio tracks and else the FM songs; FM Synth takes
 * the FM songs whenever MUSIC.DAT is usable.
 *
 * Timing rules of the CD source:
 *   - music_frame runs once a frame (20 ms) while a song plays.
 *   - A read is 32 KB, 0.19 s of music. The CPU only starts it (slot_start). The APF writes the
 *     data into the SDRAM while the frame is drawn, 25 ms at its 1.3 MB/s. The CPU looks at the
 *     next frame whether the read is done.
 *   - A read starts only if the ring has room for all of it: 1 KB stays free behind the mixer's
 *     place. No read crosses the end of the ring or of the track.
 *   - One read runs at a time. No other slot call may be made while it runs (music_stop waits).
 *   - A song starts after the ring has been filled with 224 KB (1.3 s of music).
 *
 * Volume. The mixer plays the music at 0x4000 of 0x7fff. The game sets the CD master to 0x6000
 * (0.75 as MSCDEX takes it). Like the effects' master (sound.h), the music's is two thirds of the
 * game's here. This keeps the game's balance of music and effects, and the sum mostly inside
 * 16 bits: at 0x6000 the start line's engines and the music clip (267 frames in a second).
 */
#pragma once
#include <stdint.h>

/* Reads the cue sheet from its data slot and the song numbers from GEN.DAT (whole in memory;
   without MENU.DTR the game's own values are used). Also reads MUSIC.DAT if it is needed from
   the first song on. Returns 1 if some music can play (the image has audio tracks, or the FM
   songs are usable), else 0. */
int music_init(const uint8_t *gen, uint32_t gen_size);
/* The options page's MUSIC (menu.h): 0 is off, then music_play only stops the music. */
extern int music_on;
/* Plays song k: 0 the menu's, 1-6 the race tracks'. Unless it plays already, any music playing
   stops, the ring is filled (224 KB, 1.3 s of music; waiting, about 170 ms) and the song
   starts. No read is left running. */
void music_play(int k);
/* The CD player's play (hud.h): CD track n from its start, even if it plays already, and again
   at its end (original: STARTL.EXE 0x2636c with the repeat flag, as music_play). */
void music_cd(int n);
/* The CD player's stop and resume. The stop (original: STARTL.EXE 0x26458, MSCDEX's STOP AUDIO
   while it plays: a pause) stops the music and keeps its place. The resume (0x26498) goes on
   from there, to the sector, with the same track, if music_pause stopped one and nothing played
   or stopped since. */
void music_pause(void);
void music_resume(void);
/* Called once a frame while a song plays: takes a read that is done, and starts the next one if
   the ring has room. */
void music_frame(void);
/* Stops the music. A read still running is waited for, so this comes before any other slot
   call. */
void music_stop(void);
/* Sets the music to f of 0x8000 of its volume (the pause's fade). A song that starts has it
   whole. */
void music_fade(int32_t f);
/* Sets the game's CD master volume v of 0x7fff (original: STARTL.EXE 0x1d2bc, CHOICE.DAT's
   +0x448; the CD player changes it). The music's volume is two thirds of it, so the game's
   0x6000 gives MASTER (music.c). */
void music_master(int32_t v);
/* Sets the game's MIDI master volume (original: STARTL.EXE 0x1d2c0, CHOICE.DAT's +0x44c, the CD
   player's MU). It is the FM music's volume, see fm.h. */
void music_master_mu(int32_t v);
/* For the CD player: returns the CD track playing (0 none) and sets *lo and *hi to the disc's
   first and last audio track (0 without the image). */
int music_disc(int *lo, int *hi);
