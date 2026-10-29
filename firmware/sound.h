/*
 * The game's sound effects through the mixer (audio.v, hw.h AU_*). The samples are the .RAW
 * files of SOUND.DAT: 8-bit unsigned mono at 11025 Hz, stored, so they play straight out of the
 * container in the SDRAM. SFXSBASE.DTR in GEN.DAT lists the effects, and a sample's number is
 * its row there: name, volume (0x7fff full), loop (-1 for ever, 0 once, n times), loop start,
 * loop length, pitch, pan. The calls are the game's to its sound driver (openscreamer
 * docs/NOTES.md section 17): start (0x1f708), stop (0x1f870), pitch (0x1f7d4), swap the
 * sample (0x1f754), start unless playing (0x1f79c), and a voice's volume and pan. The
 * effects' master volume is where the game starts it, 0x6000 of 0x7fff (0x1bc2f): a full
 * sample at full volume moves the output by 192 per 8-bit step.
 */
#pragma once
#include <stdint.h>

/* SFXSBASE.DTR's rows */
enum { SND_SKID, SND_CRASH, SND_OPP_ENGINE, SND_ENGINE, SND_WALL, SND_HIT, SND_CLICK, SND_ENGINE_TUNNEL,
       SND_DENT, SND_DOOR, SND_TAZ3, SND_TAZ4 };

/* The effects of GEN.DAT's SFXSBASE.DTR from SOUND.DAT (both whole in memory, SOUND.DAT in the
   SDRAM where the mixer reads it); every voice stopped. Returns how many rows have their sample. */
int snd_init(const uint8_t *sound, uint32_t sound_size, const uint8_t *gen, uint32_t gen_size);

/* Start effect id on a free voice at its row's volume, pitch 0x10000, in the middle: a handle,
   or -1 (no such effect, no free voice). A handle is good while its voice plays that start. */
int snd_play(int id);
/* Stop it; returns -1, the handle of no voice */
int snd_stop(int h);
int snd_playing(int h);
/* The voice's pitch, 16.16: 0x10000 plays the sample at its own 11025 Hz */
void snd_pitch(int h, int32_t pitch);
/* The voice's own loudness 0..0x7fff (times the row's) and its pan 0..0xffff, 0x8000 the middle:
   the middle is full on both sides, a voice off it fades out of the far side in a straight line */
void snd_volume(int h, int32_t vol, int32_t pan);
/* The voice goes on with effect id from its start (unless it plays id already), its pitch,
   loudness and pan kept */
void snd_swap(int h, int id);
/* h if it still plays effect id, else snd_play(id) */
int snd_play_once(int id, int h);
