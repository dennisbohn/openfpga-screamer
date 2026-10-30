/*
 * The championship (the game's mode 2; openscreamer docs/NOTES.md section 21), as STARTL.EXE
 * runs it: the table of 53 stages at 0x22b8 (44 bytes each), four leagues of thirteen. Before
 * each of the six races a text screen (WELCOME TO <league> before the first, PREPARE FOR THE
 * NEXT RACE! before the others); ENTER there opens the car screen (0x291ae), whose choice
 * starts the race on tracks 1..6 in turn with three laps and the league's opponents, MAPnA..D.DTR
 * (0x28ef6: 0x2a264(2, track, league)); its result (0x29acc) and the standings (0x2ce58 draws,
 * 0x292c8 decides); after the sixth race CONGRATULATIONS! (0x290a5), after the Bullet league's
 * all won (stage 51, 0x29141), or LOSER!! (stage 52). The state is kept as the game's settings
 * file CONFIG\CHOICE.DAT has it (0x460 bytes read to 0x203c0 by 0x1bbb8, written by 0x1be5c):
 * the options, the championship and the records.
 */
#pragma once
#include <stdint.h>

#define CHOICE_SIZE 0x460

/* a track's five best laps or totals (0x2042c, 0x2060c + 0x50 x track): the times, the names
   (3 letters and 0), a flag each (the totals': the number of a saved replay) and the mode */
struct choice_rec { float time[5]; char name[5][4]; int32_t flag[5], mode[5]; };

/* CHOICE.DAT as the game reads it to 0x203c0 (little-endian, floats as on the CPU) */
struct choice {
    int32_t car, track;                     /* +0x000 the car and track screens' choices */
    char name[4];                           /* +0x008 ENTER YOUR NAME's, "GRA" */
    int32_t detail, difficulty, laps;       /* +0x00c the options page's */
    int32_t stage, prev, total;             /* +0x018 the championship: its stage, the one RESUME
                                               goes back to, the score as last added up */
    float time[6];                          /* +0x024 per race the total time, */
    int32_t pos[6], pts[6];                 /* +0x03c the place (-1: not raced) and the points */
    struct choice_rec lap[6], race[6];      /* +0x06c, +0x24c the records */
    int32_t bullet, view, unused0, speech, music;   /* +0x42c the Bullet won, the start view, */
    int32_t failed;                         /* +0x440 the last race: 1 time over, 2 out of reach */
    int32_t volume[3], unused1, level, level_shown, difficulties;   /* +0x444 .. +0x45c */
};
extern struct choice choice;

/* The settings as the game has them without the file (0x1bbb8) */
void choice_defaults(void);

/* The stages' kinds (+0x00) */
enum { CHAMP_TEXT, CHAMP_LEAGUE_WON, CHAMP_END, CHAMP_STANDINGS };

/* Stage s of the table: its kind; the league (0..3, the opponents' file A..D), the track (1..6)
   and the race (0..5) a text screen leads to or the standings follow; the score needed after
   the sixth race (42, 46, 50, 54); CHAMP.TXT's block of its lines, the next stage, the one on
   failing (52), its picture CHAMPnL.PCX and the font of its lines */
struct champ_stage { int kind, league, track, race, need, text, next, fail, pic, font; };
struct champ_stage champ_at(int s);

/* NEW (0x28c6b, 0x2c698): stage 0, no race run; RESUME (0x28c9e): back to choice.prev */
void champ_new(void);
void champ_resume(void);

/* The score of the races run so far, as the standings add it up (0x2ce58): a race that ran out
   of time does not count */
int champ_total(void);

/* A race of the championship over (0x29acc): left (nothing kept), out of time, or finished in
   place (1..) with the total time */
void champ_result(int left, int time_over, int place, float time);

/* ENTER on the stage's screen (0x28ef6): CHAMP_RACE (a text screen: the car screen and the
   race next), CHAMP_NEXT (on to the next stage's screen) or CHAMP_MENU (the end: the main
   menu) */
enum { CHAMP_RACE = 1, CHAMP_NEXT, CHAMP_MENU };
int champ_enter(void);
