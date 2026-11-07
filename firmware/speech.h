/*
 * The race commentary (openscreamer docs/NOTES.md 17). The original's routines (STARTL.EXE):
 * 0x2479c queues an event, 0x24820 plays the queue, 0x24a24 picks the recording, and 0x2425c
 * checks the triggers each frame.
 *
 * Data flow: the recordings are rows of the effect table in sound.c, from SND_SPEECH on
 * (sound.h): first the rows of SPFXBASE.DTR, then those of SPFXFULL.DTR (both in GEN.DAT). The
 * game's table at 0x1fb0 gives each of the 27 events up to three of them. A line is played
 * with snd_play, so the mixer reads it from SOUND.DAT in the SDRAM. The voice is in the middle.
 *
 * Queue and timing rules:
 *   - speech_step runs once per step of 1/50 s, after the player's step.
 *   - A queued event has a mode (0..2: that recording, 3: take turns) and a delay in seconds.
 *   - While no line is spoken, a silence clock runs. The event with the shortest delay goes
 *     first, once the clock has passed its delay.
 *   - When a line has ended, the clock starts again from 0.
 *   - Each line spoken goes to the debug log once it has ended: "say <from>-<to> <event>
 *     <recording>", with the race clock when it began and ended.
 */
#pragma once

/* The events (original table: STARTL.EXE 0x1fb0). Events 6 and 18 have no recordings, and
   beat and well have no trigger in the game. Cones and all belong to the special modes: cone
   carnage says cones once the player is past the line and all once every prop is down; slalom
   says all once every gate is done and no prop was touched. */
enum { SAY_THREE, SAY_TWO, SAY_ONE, SAY_GO, SAY_CHECK, SAY_LAP, SAY_NEWLAP = 7, SAY_BEAT, SAY_CRASH,
       SAY_SKID, SAY_WIN, SAY_LOSE, SAY_BAD, SAY_WELL, SAY_HITCAR, SAY_BACK, SAY_STOP, SAY_CONES = 19,
       SAY_BEND, SAY_LEAD, SAY_HURRY, SAY_LAPPED, SAY_LOSLEA, SAY_NOTIME, SAY_ALL, SAY_EVENTS };

/* The options page's SPEECH (menu.h): 0 is off, then nothing is said (original: STARTL.EXE
   0x2425c, 0x245ab). */
extern int speech_on;
/* A new race (original: STARTL.EXE 0x246bc): empties the queue, stops the voice and sets every
   trigger's timer to 0. */
void speech_reset(void);
/* Queues event ev with a mode (0..3) and a delay in seconds, unless it is queued already
   (original: STARTL.EXE 0x2479c). */
void speech_say(int ev, int mode, float delay);
/* Lets an event be said again the next time it comes (original: STARTL.EXE 0x24d1c). The hurry
   uses it after each time bonus. */
void speech_rearm(int ev);
/* One step of 1/50 s, after the player's. It does, in this order: the end of the race (original:
   STARTL.EXE 0x23e78, 0x20f88), the countdown with the start lights (0x20d45), what the
   commentator notices (0x2425c), and the queue (0x24820). */
void speech_step(void);
/* 1 while a line is queued or being spoken, else 0 (original: STARTL.EXE 0x24740, which gives
   the queue's first event, or -1 when it is empty). */
int speech_busy(void);
