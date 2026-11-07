/*
 * The game's sound effects, played by the hardware mixer (core/src/fpga/core/audio.v, hw.h AU_*).
 *
 * Data flow: SOUND.DAT is loaded whole into the SDRAM. Its samples are the .RAW files in it:
 * 8-bit unsigned mono at 11025 Hz, stored uncompressed. So the mixer reads each sample straight
 * out of that container, and the CPU only sets the registers of a voice (address, end, loop,
 * step, volume).
 *
 * Effect numbers: GEN.DAT's SFXSBASE.DTR lists the effects, and an effect's number is its row
 * there. A row has: name, volume (0x7fff is full), loop (-1 for ever, 0 once, n times), loop
 * start, loop length, pitch, pan. The calls below follow the game's calls to its sound driver
 * (openscreamer docs/NOTES.md 17).
 *
 * Master volumes. The game starts both at 0x6000 of 0x7fff (original: STARTL.EXE 0x1bc2f).
 *   - Effects: 0x4000, two thirds of the game's. The player's engine and the three opponents'
 *     engines at full volume then stay inside 16 bits even with every sample at its peak: the sum
 *     is at most 32 000. A full sample at full volume moves the output by 128 per 8-bit step.
 *   - Commentary (speech.h): 0x3000. Its recordings are the rows of SPFXBASE.DTR and
 *     SPFXFULL.DTR, after the effects. A recording peaks at full scale. At 0x4000 (+-16 300) the
 *     countdown over the revving grid and the music clip. At 0x3000 (+-12 200, the voice
 *     2.5 dB under the game's balance) the same scene peaks at 28 631.
 *
 * Sounds with a place (the opponents' engines, the moving scenery; NOTES.md 15 and 17):
 *   - They are heard from the camera, by the 2D distance, and only they are panned. The line to
 *     the source is turned into the camera's frame: pan = 0x8000 +- 32767 x sqrt(|sideways| /
 *     distance).
 *   - Their pitch rises by 9 per unit the source came nearer in a frame (the Doppler shift).
 *   - A camera that jumped over 1000 units in a step has cut, and that step has no shift.
 */
#pragma once
#include <stdint.h>

/* Effect numbers 0..11, the rows of SFXSBASE.DTR. */
enum { SND_SKID, SND_CRASH, SND_OPP_ENGINE, SND_ENGINE, SND_WALL, SND_HIT, SND_CLICK, SND_ENGINE_TUNNEL,
       SND_DENT, SND_DOOR, SND_TAZ3, SND_TAZ4 };
/* The commentary's recordings come after them: SPFXBASE.DTR's 11 rows, then SPFXFULL.DTR's 47. */
#define SND_SPEECH  12

/* Loads the table for a race: the effects of GEN.DAT's SFXSBASE.DTR, then the recordings of its
   SPFXBASE.DTR and SPFXFULL.DTR. Both files must be whole in memory, and SOUND.DAT in the SDRAM
   where the mixer reads it. Stops every voice. Returns how many rows have their sample. */
int snd_init(const uint8_t *sound, uint32_t sound_size, const uint8_t *gen, uint32_t gen_size);

/* Effect numbers of the menu's table. Each comment names the row and the places in STARTL.EXE
   that play it (original: STARTL.EXE). */
#define SND_MENU_PAGE   0       /* smenu1: a page opened or left (0x28b5f, 0x280ec) */
#define SND_MENU_LINE   1       /* smenu2: up or down a line (0x283ca, 0x284ba) */
#define SND_MENU_OPTION 2       /* smenu3: an option changed (0x28df0) */
#define SND_MENU_GAME   3       /* smenu4: an item that starts a game (0x28ba2) */
#define SND_MENU_RECORD 4       /* smenu5: up or down a record (0x28409, 0x284f7) */
#define SND_MENU_LETTER 5       /* smenu6: the name's next or last letter (0x283e9, 0x284d9) */
#define SND_MENU_MOVE   6       /* smenu7: the camera on to the next track or car (0x2859e, 0x2874b) */
/* Loads the menu's effects instead of the race's. This is the table the game loads while a menu
   is up (original: STARTL.EXE 0x29e1d, 0x1f2d0): the nine rows smenu1..9 of the first SOUND_FX
   section of GEN.DAT's MENU.DTR. Of SOUND.DAT only their samples need be there, each at its
   place. Stops every voice. Returns how many rows have their sample. */
int snd_menu(const uint8_t *sound, uint32_t sound_size, const uint8_t *gen, uint32_t gen_size);
/* Adds the rows of the first SOUND_FX section of another DTR text (the track's MAPnDAT.DTR) to
   the table. Returns the effect number of its first row, or -1. */
int snd_add_list(const uint8_t *text, uint32_t size);

/* Starts effect id on a free voice, at its row's volume, pitch 0x10000, in the middle (original:
   STARTL.EXE 0x1f708). Returns a handle, or -1 if there is no such effect or no free voice. A
   handle is good while its voice plays that start. */
int snd_play(int id);
/* Stops the sound (original: STARTL.EXE 0x1f870). Returns -1, the handle of no voice. */
int snd_stop(int h);
/* 1 while the handle's voice still plays that start. */
int snd_playing(int h);
/* Sets the voice's pitch (original: STARTL.EXE 0x1f7d4). It is 16.16: 0x10000 plays the sample
   at its own 11025 Hz. */
void snd_pitch(int h, int32_t pitch);
/* Sets the voice's own loudness, 0..0x7fff (it is multiplied with the row's), and its pan,
   0..0xffff with 0x8000 the middle. In the middle both sides are full. A voice off the middle
   fades out of the far side in a straight line. */
void snd_volume(int h, int32_t vol, int32_t pan);
/* Makes the voice go on with effect id from its start, unless it plays id already (original:
   STARTL.EXE 0x1f754). Its pitch, loudness and pan are kept. */
void snd_swap(int h, int id);
/* Returns h if it still plays effect id, else snd_play(id) (original: STARTL.EXE 0x1f79c). */
int snd_play_once(int id, int h);
/* Sets every voice, playing or not, to f of 0x8000 of its loudness from now on. This is the
   pause's fade and the game's master volumes going down (original: STARTL.EXE 0x21328). f =
   0x8000 restores the loudness. */
void snd_fade(int32_t f);
/* Sets the game's digital master volume v of 0x7fff (original: STARTL.EXE 0x1d2b8, CHOICE.DAT's
   +0x444; the CD player changes it). The effects and the commentary then play at v / 0x6000 of
   their masters. */
void snd_master(int32_t v);

/* Sets where the sounds with a place are heard from: the camera. Called once a step. */
struct track_cam;
void snd_ear(const struct track_cam *cam);
/* The 2D distance of (x, y) from the ear. */
float snd_dist(float x, float y);
/* Plays effect id from (x, y) on handle h and returns the handle.
   Within range the effect plays (it is started if it does not) at ((range - d) / range)^2 x loud,
   with loud 0..0x7fff, panned, at pitch (16.16) plus the Doppler shift from *last. *last is the
   distance one step before; it is set to this step's. Beyond range the effect stops. */
int snd_at(int h, int id, float x, float y, float range, float loud, float pitch, float *last);
