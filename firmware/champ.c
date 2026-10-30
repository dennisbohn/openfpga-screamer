/*
 * The championship (champ.h): the stage table of 0x22b8 and its rules, 0x29acc (a race's
 * result), 0x292c8 (ENTER on the standings), 0x290a5 and 0x29141 (a league or everything won).
 */
#include "champ.h"
#include <string.h>

_Static_assert(sizeof(struct choice_dat) == CHOICE_SIZE, "CHOICE.DAT's layout");

static const int points[11] = { 24, 9, 7, 5, 0, 0, 0, 0, 0, 0, 0 };    /* per place (0x21ec) */

void choice_defaults(void)
{
    memset(&choice, 0, sizeof choice);
    memcpy(choice.name, "GRA", 4);
    for(int i = 0; i < 6; i++) {
        choice.pos[i] = -1;
        for(int k = 0; k < 5; k++) {
            choice.lap[i].time[k] = 180.0f;
            choice.race[i].time[k] = 900.0f;
            memcpy(choice.lap[i].name[k], "GRA", 4);
            memcpy(choice.race[i].name[k], "GRA", 4);
        }
    }
    choice.view = 2;
    choice.speech = choice.music = 1;
    for(int k = 0; k < 3; k++) choice.volume[k] = 0x6000;
    choice.unused1 = 1;
    choice.difficulties = 2;
}

/* The table is regular (the dump of 0x22b8 checked against this): league L's thirteen stages
   from 13 L, a text screen and the standings for each race in turn, the league won after the
   sixth; the Bullet league's sixth standings lead to stage 51 instead. A text screen's league
   picks the opponents, the standings' heads their title (block 6 + L). */
struct champ_stage champ_at(int s)
{
    struct champ_stage st = { CHAMP_END, 0, 0, 0, 0, s == 51 ? 18 : 19, 0, 0, s == 51 ? 6 : 4, 2 };
    if(s < 0 || s > 50) return st;
    int L = s / 13, i = s % 13;
    st = (struct champ_stage){ CHAMP_TEXT, L, i / 2 + 1, i / 2, 42 + 4 * L, i ? 17 : 10 + L, 0, 0, i ? 5 : L, 3 };
    if(i == 12) {                               /* the league won: the next one's level (+0x08) */
        static const int pic[3] = { 0, 1, 7 };
        st = (struct champ_stage){ CHAMP_LEAGUE_WON, L, L + 1, 0, 0, 14 + L, s + 1, s + 1, pic[L], 2 };
    } else if(i & 1) {
        st.kind = CHAMP_STANDINGS;
        st.track = 0;
        st.race = (i - 1) / 2;
        st.text = i == 1 ? 10 + L : 10;
        st.next = s == 50 ? 51 : s + 1;
        st.fail = 52;
        st.pic = L;
    }
    return st;
}

void champ_new(void)
{
    for(int i = 0; i < 6; i++) choice.pos[i] = -1;
    choice.stage = choice.prev = 0;
}

void champ_resume(void)
{
    choice.stage = choice.prev;
}

int champ_total(void)
{
    struct champ_stage st = champ_at(choice.stage);
    int t = 0;
    for(int i = 0; i < 6; i++)
        if(choice.pos[i] != -1 && !(choice.failed == 1 && i == st.race)) t += choice.pts[i];
    return choice.total = t;
}

/* 0x29acc: the place, time and points into the race's row; the stage on to the standings,
   which RESUME comes back to; out of reach (2) once even wins in the races left would not
   make the league's score */
void champ_result(int left, int time_over, int place, float time)
{
    if(left) return;
    struct champ_stage st = champ_at(choice.stage);
    int reach = 9 * (5 - st.race);
    choice.failed = time_over != 0;
    choice.pos[st.race] = place;
    choice.time[st.race] = time;
    choice.pts[st.race] = place >= 0 && place < 11 ? points[place] : 0;
    for(int i = 0; i <= st.race; i++) reach += choice.pts[i];
    choice.stage = choice.prev = choice.stage + 1;
    if(reach < champ_at(choice.stage).need) choice.failed = 2;
}

int champ_enter(void)
{
    struct champ_stage st = champ_at(choice.stage);
    switch(st.kind) {
    case CHAMP_TEXT:
        return CHAMP_RACE;
    case CHAMP_STANDINGS: {                     /* 0x292c8: in the first three (the Bullet league:
                                                   first), in time, the score after the sixth */
        int pos = choice.pos[st.race];
        if(pos < 4 && !choice.failed && (st.league < 3 || pos == 1) && (st.race < 5 || champ_total() >= st.need)) {
            if(st.race == 5) {                  /* a new league: its table empty */
                choice.stage = choice.prev = st.next;
                for(int i = 0; i < 6; i++) choice.pos[i] = -1;
            } else {
                choice.prev = choice.stage;
                choice.stage = st.next;
            }
        } else {                                /* LOSER!!, RESUME: the race again */
            choice.prev = choice.stage - 1;
            choice.stage = st.fail;
        }
        return CHAMP_NEXT;
    }
    case CHAMP_LEAGUE_WON:                      /* 0x290a5: one more track to choose from */
        if(choice.level < st.track) choice.level = st.track;
        choice.prev = choice.stage;
        choice.stage = st.next;
        return CHAMP_NEXT;
    default:                                    /* 0x29141: all won, the Bullet, the mirrored
                                                   tracks and AFTERBURNER; then the main menu */
        if(st.text == 18) {
            choice.bullet = 1;
            choice.level = 9;
            choice.difficulties = 3;
        }
        return CHAMP_MENU;
    }
}
