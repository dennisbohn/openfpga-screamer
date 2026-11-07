/*
 * The championship (the game's mode 2; openscreamer docs/NOTES.md section 21), as STARTL.EXE
 * runs it. The game has a table of 53 stages at 0x22b8 (44 bytes each), in four leagues of
 * thirteen stages.
 *   - Before each of the six races of a league comes a text screen: WELCOME TO <league> before
 *     the first and PREPARE FOR THE NEXT RACE! before the others. ENTER there opens the car
 *     screen (original: 0x291ae). Its choice starts the race, on tracks 1..6 in turn, with three
 *     laps and the league's opponents, MAPnA..D.DTR (0x28ef6: 0x2a264(2, track, league)).
 *   - The race's result (0x29acc) goes to the standings. 0x2ce58 draws them and 0x292c8
 *     decides what follows.
 *   - After the sixth race comes CONGRATULATIONS! (0x290a5). After the Bullet league's last race
 *     everything is won (stage 51, 0x29141), or the player gets LOSER!! (stage 52).
 * The state is kept as the game's settings file CONFIG\CHOICE.DAT has it: 0x460 bytes, read to
 * 0x203c0 by 0x1bbb8 and written by 0x1be5c before and after every race and on quitting. It
 * holds the options, the championship and the records. The core keeps it in the nonvolatile
 * data slot CHOICE.DAT (save.h).
 */
#pragma once
#include <stdint.h>
#include "save.h"

#define CHOICE_SIZE 0x460

/* A track's five best laps or totals (original: STARTL.EXE 0x2042c, 0x2060c + 0x50 x track):
   the times, the names (3 letters and a 0), a flag each (for the totals: the number of a saved
   replay) and the mode */
struct choice_rec { float time[5]; char name[5][4]; int32_t flag[5], mode[5]; };

/* CHOICE.DAT as the game reads it to 0x203c0 (little-endian, floats as on the CPU) */
struct choice_dat {
    int32_t car, track;                     /* +0x000 the car and track screens' choices */
    char name[4];                           /* +0x008 ENTER YOUR NAME's, "GRA" */
    int32_t detail, difficulty, laps;       /* +0x00c the options page's */
    int32_t stage, prev, total;             /* +0x018 the championship: its stage, the stage RESUME
                                               goes back to, the score as last added up */
    float time[6];                          /* +0x024 for each race: the total time */
    int32_t pos[6], pts[6];                 /* +0x03c for each race: the place (-1: not raced),
                                               +0x054 the points */
    struct choice_rec lap[6], race[6];      /* +0x06c, +0x24c the records */
    int32_t bullet, view, intro, speech, music;     /* +0x42c the Bullet won, the start view, the
                                               intro's track - 1 (one more after each, 0x27f06),
                                               the speech and music options */
    int32_t failed;                         /* +0x440 the last race: 1 time over, 2 out of reach */
    int32_t volume[3], unused1, level, level_shown, difficulties;   /* +0x444 .. +0x45c */
};
/* the copy in the SDRAM that the APF saves on exit (data slot 23, save.h) */
#define choice (*(struct choice_dat *)(uintptr_t)SAVE_CHOICE_ADDR)

/* Sets the settings as the game has them without the file (original: STARTL.EXE 0x1bbb8). */
void choice_defaults(void);

/*
 * Enters a finished race of the normal game or the championship into track n's records
 * (original: STARTL.EXE 0x2e5e4; the callers in the game are 0x2a1a8, 0x1febf before PLAY AGAIN
 * and 0x28f42). The game never calls it for a race that ran out of time or was left (flags
 * 0x1020). The best lap is always entered, with flag 1. The total is entered only for a race of
 * three laps (0x1d2ac == 2), with flag 0 (as the game leaves it without a saved replay) and
 * the game mode. Each time goes ahead of the first time it is not slower than, under the name
 * choice.name.
 */
void choice_record(int n, float lap, float total, int three_laps, int mode);

/* The stages' kinds (+0x00) */
enum { CHAMP_TEXT, CHAMP_LEAGUE_WON, CHAMP_END, CHAMP_STANDINGS };

/*
 * Stage s of the table. Its fields:
 *   kind    one of CHAMP_*
 *   league  0..3, which gives the opponents' file A..D
 *   track   1..6, the track that a text screen leads to
 *   race    0..5, the race that a text screen leads to or that the standings follow
 *   need    the score needed after the sixth race (42, 46, 50, 54)
 *   text    CHAMP.TXT's block of the stage's lines
 *   next    the next stage
 *   fail    the stage on failing (52)
 *   pic     the picture CHAMPnL.PCX
 *   font    the font of the lines
 * Where the kinds use fields differently, champ.c has the details.
 */
struct champ_stage { int kind, league, track, race, need, text, next, fail, pic, font; };
struct champ_stage champ_at(int s);

/* NEW (original: STARTL.EXE 0x28c6b, 0x2c698): stage 0, no race run. RESUME (0x28c9e): back to
   choice.prev. */
void champ_new(void);
void champ_resume(void);

/* Returns the score of the races run so far, as the standings add it up (original: 0x2ce58).
   A race that ran out of time does not count. */
int champ_total(void);

/* A race of the championship is over (original: STARTL.EXE 0x29acc). Either it was left
   (nothing is kept), or it ran out of time, or it was finished in place (1..) with the total
   time. */
void champ_result(int left, int time_over, int place, float time);

/* ENTER on the stage's screen (original: STARTL.EXE 0x28ef6). Returns CHAMP_RACE (a text
   screen: the car screen and the race come next), CHAMP_NEXT (on to the next stage's screen)
   or CHAMP_MENU (the end: the main menu). */
enum { CHAMP_RACE = 1, CHAMP_NEXT, CHAMP_MENU };
int champ_enter(void);
