/*
 * The game's MIDI music for the FM chip: the sound card's OPL2 (core/src/fpga/core/opl.v). It is
 * the other source of music.h, used when there is no CD image or when the Pocket menu asks for
 * FM Synth. fm.c is the player and the driver of the original, as openscreamer's
 * docs/NOTES.md 17 (MUSIC) and 32 (the music as the game played it) describe them.
 *
 * Data flow: MUSIC.DAT in memory (DATA/MUSIC.DAT, loaded by music.c) holds the songs MUSICn.HMP
 * in HMI's format and the two instrument banks MELODIC.BNK and DRUM.BNK. Then:
 *   - the player (HMI's) steps the tracks of the song and sends their MIDI messages to the
 *     driver. When all tracks have ended, it resets every track's channel and has the song
 *     started again, so a song goes round and round;
 *   - the driver (fmmidi.com of HMIMDRV.386, the one for the OPL2) turns the messages into the
 *     chip's register writes: nine voices, no rhythm mode, instruments from the banks;
 *   - each register write goes into the hardware's queue with the time at which the chip is to
 *     get it (opl.v, docs/soc.md). The chip's output goes to the mixer.
 * The addresses of the original's routines are in fm.c.
 *
 * Timing rules:
 *   - The song's ticks come 120 times a second (the rate in the song's header, +0x38).
 *   - The CPU runs once a frame (20 ms), but the notes are due to 8 ms. So the writes are not
 *     made to the chip at once. fm_frame puts all events that are due within the next 60 ms
 *     into the queue, each with its own time, a few frames ahead.
 *   - The hardware gives the writes to the chip 36 us apart, as the original's driver did (it
 *     waited for the chip 40 us after each).
 */
#pragma once
#include <stdint.h>

#define FM_SONGS    7                   /* MUSIC1.HMP .. MUSIC7.HMP */

/* Takes MUSIC.DAT, which must be whole in memory with stored (uncompressed) entries, as GOG's
   copy has them. Reads the two banks and checks which songs the file has. Returns 1 if the banks
   and at least one song are usable. */
int fm_init(const uint8_t *file, uint32_t size);
/* Starts song n (1..FM_SONGS) from its start. Any music playing is stopped first. Returns 0 if
   the song is not in the file (or not usable), else 1. */
int fm_play(int n);
/* Called once a frame while a song plays: the song's events that are due within the next 60 ms
   go into the hardware's queue. */
void fm_frame(void);
/* Stops the song. The chip is silent and its queue empty (the hardware is held in reset). */
void fm_stop(void);
/* The CD player's stop and play (music.h). The pause switches all notes off and keeps the song's
   place. The resume goes on from there; the notes that sounded are not struck again. */
void fm_pause(void);
void fm_resume(void);
/* fm_master sets the game's MIDI master volume v of 0x7fff (original: STARTL.EXE 0x1d2c0,
   CHOICE.DAT's +0x44c; the CD player changes it). The game's start value 0x6000 is 96 of 127 at
   the driver. fm_fade sets f of 0x8000 as the pause's fade (music.h): the master is multiplied
   by f. Both reach the chip as the levels of the notes. */
void fm_master(int32_t v);
void fm_fade(int32_t f);
/* Returns the song playing (not paused), 0 if none. */
int fm_playing(void);
/* For the log: the number of notes the driver had no voice for, of frames in which the hardware's
   queue had no room and the song waited, of register writes lost because the queue had no room,
   and of times the song's time ran behind the clock (the CPU did not come for 120 ms). */
void fm_stats(uint32_t *no_voice, uint32_t *full, uint32_t *lost, uint32_t *behind);
