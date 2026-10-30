#include "speech.h"
#include "car.h"
#include "dlog.h"
#include "hw.h"
#include "opp.h"
#include "sound.h"
#include "track.h"

/* each event's recordings, rows counted through SPFXBASE.DTR then SPFXFULL.DTR (0x1fb0) */
static const int8_t recording[SAY_EVENTS][3] = {
    {  2, -1, -1 }, {  3, -1, -1 }, {  4, -1, -1 }, {  5, -1, -1 },     /* three two one go */
    {  8, 13, 14 }, {  6, 48,  7 }, { -1, -1, -1 }, { 46, 47, 53 },     /* check lap - newlap */
    { 44, 45, -1 }, { 41, 42, 43 }, { 21, 51, -1 }, {  1, 11, 12 },     /* beat crash skid win */
    {  0, 27, 52 }, { 17, 18, 54 }, { 15, 16, -1 }, { 25, 26, -1 },     /* lose bad well hitcar */
    { 20, 49, -1 }, { 19, 50, -1 }, { -1, -1, -1 }, { 55, 56, 57 },     /* back stop - cones */
    { 22, 23, 24 }, { 31, 32, 33 }, { 38, 39, 40 }, { 37, -1, -1 },     /* bend lead hurry lapped */
    { 34, 35, 36 }, {  9, 30, -1 }, { 10, 28, 29 },                     /* loslea notime all */
};
static const char *const event_name[SAY_EVENTS] = {
    "three", "two", "one", "go", "check", "lap", "-", "newlap", "beat", "crash", "skid", "win", "lose",
    "bad", "well", "hitcar", "back", "stop", "-", "cones", "bend", "lead", "hurry", "lapped", "loslea",
    "notime", "all",
};

/* the queue (0x11907c): every event at most once, so it never holds more than all of them */
static struct { int ev, mode; float delay; } queue[SAY_EVENTS];
static int queued;
static int voice = -1;                  /* the line being spoken (0x11906c) */
static float quiet;                     /* how long the voice has been silent (0x119078) */
static int turn[SAY_EVENTS];            /* the recording an event said last in mode 3 (0x1191bc) */
static float timer[SAY_EVENTS];         /* the triggers' (0x119228); -1: said, wait */
static int stage;                       /* the start lights' state last said */
static int said_end, had_lead;

void speech_reset(void)
{
    voice = snd_stop(voice);
    queued = 0;
    quiet = 0.7f;
    /* the turns start at 2, or at 0 or 1, as the game's timer tick (0x107fd) has it at the
       set-up; ours is the cycle counter */
    uint32_t tick = rdcycle() >> 20;
    for (int e = 0; e < SAY_EVENTS; e++) {
        timer[e] = 0.0f;
        turn[e] = (tick & 2) ? (int)(tick & 1) : 2;
    }
    stage = -1;
    said_end = had_lead = 0;
}

void speech_say(int ev, int mode, float delay)
{
    for (int i = 0; i < queued; i++)
        if (queue[i].ev == ev) return;
    queue[queued].ev = ev;
    queue[queued].mode = mode;
    queue[queued].delay = delay;
    queued++;
}

void speech_rearm(int ev)
{
    timer[ev] = 0.0f;
}

/* 0x24bec: once cond has held for t seconds, say ev, and not again until cond has gone; 1 when
   it says it */
static int hold(int cond, int ev, float t)
{
    if (!cond) {
        timer[ev] = 0.0f;
        return 0;
    }
    if (timer[ev] < 0.0f) return 0;
    timer[ev] += DRIVE_DT;
    if (timer[ev] < t) return 0;
    speech_say(ev, 3, 0.7f);
    timer[ev] = -1.0f;
    return 1;
}

/* 0x24c68: every t seconds while cond holds */
static void repeat(int cond, int ev, float t)
{
    if (!cond) {
        timer[ev] = 0.0f;
        return;
    }
    timer[ev] += DRIVE_DT;
    if (timer[ev] >= t) {
        speech_say(ev, 3, 0.7f);
        timer[ev] = 0.0f;
    }
}

/* 0x24cd0: once, until re-armed */
static void once(int ev, int mode)
{
    if (timer[ev] < 0.0f) return;
    speech_say(ev, mode, 0.7f);
    timer[ev] = -1.0f;
}

/* 0x2425c: what the commentator notices while the race is on. Back: heading within 45 degrees
   of the road reversed, every 5 s; stop: under 1000 units/s, every 8 s; hurry: the countdown at
   5 s or under; lead: first for 5 s; loslea: after that second or worse for 5 s; bend: on a
   marker with flag 4; skid: sliding for the car's grip x 2.9 s; bad: last (behind
   max(2, opponents) others) for 8 s; lapped: a lap up on an opponent for 2 s; hitcar: whenever
   the contact (opp.h) is over 0.8 */
static void notice(const struct car_race *rc)
{
    const struct car_state *c = car_state();
    int back = car_backwards(), rank = opp_player_rank(), nopp = opp_count(), nm;
    const struct track_marker *mk = track_markers(&nm);
    repeat(back, SAY_BACK, 5.0f);
    repeat(c->speed < 1000.0f, SAY_STOP, 8.0f);
    if (rc->timer <= 5.0f) once(SAY_HURRY, 3);
    if (hold(rank == 1, SAY_LEAD, 5.0f)) had_lead = 1;
    if (hold(had_lead && rank >= 2, SAY_LOSLEA, 5.0f)) had_lead = 0;
    if (!back) hold(nm && (mk[car_marker()].flags & 4), SAY_BEND, 0.0f);
    hold(c->slide_want != 0.0f, SAY_SKID, c->spec.grip * 2.9f);
    hold((nopp < 2 ? 2 : nopp) < rank, SAY_BAD, 8.0f);
    hold(opp_lapped(), SAY_LAPPED, 2.0f);
    repeat(opp_hitcar() > 0.8f, SAY_HITCAR, 0.0f);
}

/* 0x24a24 / 0x24ae8: which of the event's recordings (0..2): the mode's, or in mode 3 the next
   one there is */
static int pick(int ev, int mode)
{
    if (mode < 3) return mode;
    for (int tries = 0; tries < 3; tries++) {
        if (++turn[ev] == 3) turn[ev] = 0;
        if (recording[ev][turn[ev]] >= 0) break;
    }
    return turn[ev];
}

/* the debug log: "say <race clock to 1/100 s> <event> <recording>" */
static void log_line(int ev, int k)
{
    int32_t t = (int32_t)(car_race()->clock * 100.0f + 0.5f);
    dlog_str("say ");
    dlog_dec(t / 100);
    dlog_char('.');
    dlog_char((char)('0' + t / 10 % 10));
    dlog_char((char)('0' + t % 10));
    dlog_char(' ');
    dlog_str(event_name[ev]);
    dlog_char(' ');
    dlog_dec(k);
    if (voice < 0) dlog_str(" silent");
    dlog_char('\n');
}

/* the first in the queue is done with */
static void pop(void)
{
    queued--;
    for (int i = 0; i < queued; i++)
        queue[i] = queue[i + 1];
    voice = -1;
    quiet = 0.0f;
}

/* 0x24820: wait for the line being spoken to end; else the clock runs, the event with the
   shortest delay comes first (0x2491c) and is spoken once the clock has passed its delay */
static void play(void)
{
    if (voice >= 0) {
        if (!snd_playing(voice)) pop();
        return;
    }
    quiet += DRIVE_DT;
    if (!queued) return;
    int first = 0;
    for (int i = 1; i < queued; i++)
        if (queue[i].delay < queue[first].delay) first = i;
    if (first) {
        __typeof__(queue[0]) q = queue[0];
        queue[0] = queue[first];
        queue[first] = q;
    }
    if (quiet < queue[0].delay) return;
    int ev = queue[0].ev, k = pick(ev, queue[0].mode), r = recording[ev][k];
    voice = r >= 0 ? snd_play(SND_SPEECH + r) : -1;
    log_line(ev, k);
    if (voice < 0) pop();
}

void speech_step(void)
{
    const struct car_race *rc = car_race();
    int over = rc->finished || rc->game_over;
    if (over && !said_end) {                    /* 0x20f88 time over, 0x23e78 the finish */
        said_end = 1;
        speech_say(rc->game_over ? SAY_NOTIME : opp_player_rank() == 1 ? SAY_WIN : SAY_LOSE, 3, 0.0f);
    }
    if (!over && rc->clock < 8.0f && (int)rc->clock / 2 != stage) {    /* 0x20d45: the lights */
        stage = (int)rc->clock / 2;
        speech_say(SAY_THREE + stage, 0, 0.0f);
    }
    if (rc->on && !over) notice(rc);
    play();
}
