/*
 * The race commentary (openscreamer docs/NOTES.md section 17; STARTL.EXE 0x2479c queues an
 * event, 0x24820 plays the queue, 0x24a24 picks the recording, 0x2425c the triggers each frame).
 * The recordings are the rows of SPFXBASE.DTR, then SPFXFULL.DTR (GEN.DAT), from SND_SPEECH on
 * (sound.h); the game's table at 0x1fb0 gives each of the 27 events up to three of them. A
 * queued event has a mode (0..2 that recording, 3 take turns) and a delay: while the voice is
 * silent a clock runs, the event with the shortest delay goes first once the clock has passed
 * its delay, and the clock starts again when it has been spoken. The voice is in the middle.
 * Each line spoken goes to the debug log once it has ended: "say <from>-<to> <event> <recording>",
 * the race clock when it began and ended.
 */
#pragma once

/* the events (0x1fb0); 6 and 18 have no recordings, beat and well no trigger in the game,
   cones and all belong to modes the core does not have */
enum { SAY_THREE, SAY_TWO, SAY_ONE, SAY_GO, SAY_CHECK, SAY_LAP, SAY_NEWLAP = 7, SAY_BEAT, SAY_CRASH,
       SAY_SKID, SAY_WIN, SAY_LOSE, SAY_BAD, SAY_WELL, SAY_HITCAR, SAY_BACK, SAY_STOP, SAY_CONES = 19,
       SAY_BEND, SAY_LEAD, SAY_HURRY, SAY_LAPPED, SAY_LOSLEA, SAY_NOTIME, SAY_ALL, SAY_EVENTS };

/* A new race (0x246bc): the queue empty, the voice stopped, every trigger's timer at 0 */
void speech_reset(void);
/* Queue event ev with mode (0..3) and delay in seconds, unless it is queued already (0x2479c) */
void speech_say(int ev, int mode, float delay);
/* Say it again the next time it comes (0x24d1c: the hurry after each time bonus) */
void speech_rearm(int ev);
/* One step of 1/50 s, after the player's: the countdown with the start lights (0x20d45), the
   end (0x23e78, 0x20f88), what the commentator notices (0x2425c), the queue (0x24820) */
void speech_step(void);
/* 1 while a line is queued or being spoken (0x24740: the queue's first event, -1 when empty) */
int speech_busy(void);
