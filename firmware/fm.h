/*
 * The game's MIDI music for the FM chip (the sound card's OPL2, core/src/fpga/core/opl.v):
 * the fallback of music.h when there is no CD image. DATA/MUSIC.DAT holds MUSICn.HMP, the songs
 * in HMI's format, and the two instrument banks MELODIC.BNK and DRUM.BNK. fm.c is the player
 * and the driver of the original, as openscreamer's docs/NOTES.md section 17 (MUSIC) and 32
 * (the music as the game played it) have them:
 *
 *  - the player (HMI's, linked into STARTL.EXE at 0xd2000..0xe4000; its timer routine 0xe3d51,
 *    a message on its way 0xe2ca9, a song's end 0xe37eb, the volumes 0xe29d9 / 0xd3a54) steps
 *    the tracks 120 times a second (the song's header, +0x38), sends their MIDI messages to
 *    the driver and, when all tracks have ended, resets every track's channel and has the
 *    song started again (the game's poll, 0x2fcd4: a song that is no longer playing is started
 *    anew with the repeat flag, 0x2fc7c), so a song goes round and round;
 *  - the driver (HMIMDRV.386's fmmidi.com, the one for the OPL2) turns the messages into the
 *    chip's register writes: nine voices, no rhythm mode, instruments from the banks.
 *
 * The CPU runs once a frame (20 ms) but the notes are due to 8 ms, so the writes are not made
 * to the chip at once: each goes into the hardware's queue with the time at which the chip is to
 * get it (opl.v, docs/soc.md), a few frames ahead. The hardware spaces the writes 36 us apart as
 * the original's driver did (it waited for the chip 40 us after each).
 */
#pragma once
#include <stdint.h>

#define FM_SONGS    7                   /* MUSIC1.HMP .. MUSIC7.HMP */

/* MUSIC.DAT whole in memory (stored entries, as GOG's copy has them): the banks and the songs
   it has; 1 if the banks and at least one song are usable */
int fm_init(const uint8_t *file, uint32_t size);
/* Song n (1..FM_SONGS) from its start, the music before stopped; 0 if it is not in the file */
int fm_play(int n);
/* Once a frame while a song plays: the song's events that are due within the next 60 ms go
   into the hardware's queue */
void fm_frame(void);
/* The chip is silent and its queue empty (the hardware is held in reset) */
void fm_stop(void);
/* The CD player's stop and play (music.h): all notes off and the song's place kept; resume goes
   on from there (the notes that sounded are not struck again) */
void fm_pause(void);
void fm_resume(void);
/* The game's MIDI master volume v of 0x7fff (0x1d2c0, CHOICE.DAT's +0x44c; the CD player changes
   it): 0x6000 is the game's start value, 96 of 127 at the driver. f of 0x8000 is the pause's
   fade (music.h): the master times f. Both reach the chip as the levels of the notes. */
void fm_master(int32_t v);
void fm_fade(int32_t f);
/* The song playing (not paused), 0 none */
int fm_playing(void);
/* For the log: notes the driver had no voice for, frames in which the hardware's queue had no
   room, times the song's time ran behind the clock (the CPU did not come for 120 ms) */
void fm_stats(uint32_t *no_voice, uint32_t *full, uint32_t *behind);
