/*
 * The game's sound effects through the mixer (audio.v, hw.h AU_*). The samples are the .RAW
 * files of SOUND.DAT: 8-bit unsigned mono at 11025 Hz, stored, so they play straight out of the
 * container in the SDRAM. SFXSBASE.DTR in GEN.DAT lists the effects, and a sample's number is
 * its row there: name, volume (0x7fff full), loop (-1 for ever, 0 once, n times), loop start,
 * loop length, pitch, pan. The calls are the game's to its sound driver (openscreamer
 * docs/NOTES.md section 17): start (0x1f708), stop (0x1f870), pitch (0x1f7d4), swap the
 * sample (0x1f754), start unless playing (0x1f79c), and a voice's volume and pan. The
 * effects' master volume is 0x4000 of 0x7fff, two thirds of where the game starts it (0x6000,
 * 0x1bc2f), so that the player's engine and the three opponents' at full volume stay under the
 * 16 bits even with every sample at its peak (at most 32 000); a full sample at full volume
 * moves the output by 128 per 8-bit step. The commentary (speech.h) comes after the effects,
 * the rows of SPFXBASE.DTR and SPFXFULL.DTR, at a master volume of its own, 0x3000: the game
 * starts the speech's at 0x6000 like the others (0x1bc2f), but a recording peaks at full scale,
 * and at 0x4000 (+-16 300) the countdown over the revving grid and the music clipped in the
 * simulation; at 0x3000 (+-12 200, the voice 2.5 dB under the game's balance) it peaks at
 * 28 631.
 *
 * Sounds with a place (the opponents' engines, the moving scenery; NOTES.md sections 15 and 17)
 * are heard from the camera, by the 2D distance, and only they are panned: the line to the
 * source turned into the camera's frame, 0x8000 +- 32767 x sqrt(|sideways| / distance). Their
 * pitch rises by 9 per unit the source came nearer in a frame (the Doppler shift); a camera that
 * jumped over 1000 units in a step has cut, and that step has no shift.
 */
#pragma once
#include <stdint.h>

/* SFXSBASE.DTR's rows */
enum { SND_SKID, SND_CRASH, SND_OPP_ENGINE, SND_ENGINE, SND_WALL, SND_HIT, SND_CLICK, SND_ENGINE_TUNNEL,
       SND_DENT, SND_DOOR, SND_TAZ3, SND_TAZ4 };
/* the commentary's recordings after them: SPFXBASE.DTR's 11 rows, then SPFXFULL.DTR's 47 */
#define SND_SPEECH  12

/* The effects of GEN.DAT's SFXSBASE.DTR and the recordings of its SPFXBASE.DTR and SPFXFULL.DTR
   from SOUND.DAT (both whole in memory, SOUND.DAT in the SDRAM where the mixer reads it); every
   voice stopped. Returns how many rows have their sample. */
int snd_init(const uint8_t *sound, uint32_t sound_size, const uint8_t *gen, uint32_t gen_size);
/* The rows of the first SOUND_FX section of another DTR text (the track's MAPnDAT.DTR) after
   those: the effect number of its first row, or -1 */
int snd_add_list(const uint8_t *text, uint32_t size);

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

/* Where the sounds with a place are heard from: the camera, once a step */
struct track_cam;
void snd_ear(const struct track_cam *cam);
/* the 2D distance of (x, y) from there */
float snd_dist(float x, float y);
/* Effect id from (x, y) on handle h (the handle back): within range it plays (started unless it
   does) at ((range - d) / range)^2 x loud (0..0x7fff) and panned, at pitch (16.16) + the Doppler
   shift from *last, the distance a step before (set to this one's); beyond range it stops */
int snd_at(int h, int id, float x, float y, float range, float loud, float pitch, float *last);
