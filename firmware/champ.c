/*
 * The championship (see champ.h): the stage table of 0x22b8 and its rules, 0x29acc (a race's
 * result), 0x292c8 (ENTER on the standings), 0x290a5 and 0x29141 (a league or everything won).
 * All addresses are in STARTL.EXE.
 * Entry points: choice_defaults() and choice_record() for the saved settings and records,
 * champ_at() for the stage table, and champ_new(), champ_resume(), champ_total(),
 * champ_result() and champ_enter() for the progress through the stages.
 */
#include "champ.h"
#include <string.h>

_Static_assert(sizeof(struct choice_dat) == CHOICE_SIZE, "CHOICE.DAT's layout");

/* The points for a place, indexed by place (original: 0x21ec). */
static const int points[11] = { 24, 9, 7, 5, 0, 0, 0, 0, 0, 0, 0 };

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

/* Puts time t into the five times of r, ahead of the first time that t is not slower than.
   The slower times move down one place and the last one drops out. flag, the player's name
   and, if mode >= 0, mode are stored with the time. The totals pass a mode, the laps do not.
   Returns the place (0..4), or 5 if t is not among the five. */
static int record_put(struct choice_rec *r, float t, int flag, int mode)
{
    int at = 0;
    while(at < 5 && t > r->time[at]) at++;
    if(at == 5) return at;
    for(int k = 4; k > at; k--) {
        r->time[k] = r->time[k - 1];
        r->flag[k] = r->flag[k - 1];
        if(mode >= 0) r->mode[k] = r->mode[k - 1];
        memcpy(r->name[k], r->name[k - 1], 4);
    }
    r->time[at] = t;
    r->flag[at] = flag;
    if(mode >= 0) r->mode[at] = mode;
    memcpy(r->name[at], choice.name, 4);
    return at;
}

void choice_record(int n, float lap, float total, int three_laps, int mode)
{
    if(n < 1 || n > 6) return;
    record_put(&choice.lap[n - 1], lap, 1, -1);
    if(three_laps) record_put(&choice.race[n - 1], total, 0, mode);
}

/*
 * Returns stage s of the table. The table is regular, so the function computes it. It was
 * checked against a dump of the game's table at 0x22b8.
 * League L has its thirteen stages from 13 L on. For each of the six races in turn it has a
 * text screen and then the standings. The thirteenth stage says that the league is won, after
 * the sixth race. Its track field (+0x08 in the game's entry) is the next level. In the Bullet
 * league the sixth standings lead to stage 51 instead.
 * A text screen's league picks the opponents. The standings' heads show their title (block
 * 6 + L).
 */
struct champ_stage champ_at(int s)
{
    struct champ_stage st = { CHAMP_END, 0, 0, 0, 0, s == 51 ? 18 : 19, 0, 0, s == 51 ? 6 : 4, 2 };
    if(s < 0 || s > 50) return st;
    int L = s / 13, i = s % 13;
    st = (struct champ_stage){ CHAMP_TEXT, L, i / 2 + 1, i / 2, 42 + 4 * L,
                               i ? 17 : 10 + L, 0, 0, i ? 5 : L, 3 };
    if(i == 12) {                               /* league won: its track field is the next level */
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

/*
 * Stores a race's result (original: STARTL.EXE 0x29acc): the place, the time and the points
 * go into the race's row. The stage moves on to the standings, which RESUME comes back to.
 * The result is out of reach (failed = 2) when even wins in the races left would not make the
 * league's score. reach is the points so far plus 9 for each race left.
 */
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
    case CHAMP_STANDINGS: {                     /* original: 0x292c8. The player goes on if placed in
                                                   the first three (in the Bullet league: first), in
                                                   time, and with the score needed after the sixth
                                                   race. */
        int pos = choice.pos[st.race];
        if(pos < 4 && !choice.failed && (st.league < 3 || pos == 1) &&
           (st.race < 5 || champ_total() >= st.need)) {
            if(st.race == 5) {                  /* a new league: its table empty */
                choice.stage = choice.prev = st.next;
                for(int i = 0; i < 6; i++) choice.pos[i] = -1;
            } else {
                choice.prev = choice.stage;
                choice.stage = st.next;
            }
        } else {                                /* LOSER!!. RESUME then runs the race again. */
            choice.prev = choice.stage - 1;
            choice.stage = st.fail;
        }
        return CHAMP_NEXT;
    }
    case CHAMP_LEAGUE_WON:                      /* original: 0x290a5. One more track to choose from. */
        if(choice.level < st.track) choice.level = st.track;
        choice.prev = choice.stage;
        choice.stage = st.next;
        return CHAMP_NEXT;
    default:                                    /* original: 0x29141. Everything is won: the Bullet, the
                                                   mirrored tracks and AFTERBURNER. Then the main
                                                   menu. */
        if(st.text == 18) {
            choice.bullet = 1;
            choice.level = 9;
            choice.difficulties = 3;
        }
        return CHAMP_MENU;
    }
}
