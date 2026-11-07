/*
 * The race commentary: the event queue and the triggers (interface and rules: speech.h).
 * Triggers (notice, speech_step) queue events; play() takes them off the queue one at a time and
 * starts the event's recording as an effect of the mixer (snd_play, sound.c); pop() ends a line.
 * The variables from 0x119xxx noted below are the original's memory (STARTL.EXE).
 */
#include "speech.h"
#include "car.h"
#include "dlog.h"
#include "hw.h"
#include "opp.h"
#include "scenery.h"
#include "sound.h"
#include "track.h"

/* Each event's up to three recordings, by event number (original: STARTL.EXE 0x1fb0). A
   recording is a row counted through SPFXBASE.DTR and then SPFXFULL.DTR, so it is added to
   SND_SPEECH for the effect number. -1: none. The comments name the events of each line. */
static const int8_t recording[SAY_EVENTS][3] = {
    {  2, -1, -1 }, {  3, -1, -1 }, {  4, -1, -1 }, {  5, -1, -1 },     /* three, two, one, go */
    {  8, 13, 14 }, {  6, 48,  7 }, { -1, -1, -1 }, { 46, 47, 53 },     /* check, lap, (6), newlap */
    { 44, 45, -1 }, { 41, 42, 43 }, { 21, 51, -1 }, {  1, 11, 12 },     /* beat, crash, skid, win */
    {  0, 27, 52 }, { 17, 18, 54 }, { 15, 16, -1 }, { 25, 26, -1 },     /* lose, bad, well, hitcar */
    { 20, 49, -1 }, { 19, 50, -1 }, { -1, -1, -1 }, { 55, 56, 57 },     /* back, stop, (18), cones */
    { 22, 23, 24 }, { 31, 32, 33 }, { 38, 39, 40 }, { 37, -1, -1 },     /* bend, lead, hurry, lapped */
    { 34, 35, 36 }, {  9, 30, -1 }, { 10, 28, 29 },                     /* loslea, notime, all */
};
/* The events' names for the debug log; "-" for the two without recordings. */
static const char *const event_name[SAY_EVENTS] = {
    "three", "two", "one", "go", "check", "lap", "-", "newlap", "beat", "crash", "skid", "win", "lose",
    "bad", "well", "hitcar", "back", "stop", "-", "cones", "bend", "lead", "hurry", "lapped", "loslea",
    "notime", "all",
};

/* The queue (original: STARTL.EXE 0x11907c). Every event is in it at most once, so it never
   holds more than all of them. Each entry has the event, its mode and its delay in seconds. */
static struct { int ev, mode; float delay; } queue[SAY_EVENTS];
static int queued;
int speech_on = 1;
static int voice = -1;                  /* the handle of the line being spoken (0x11906c), -1 none */
static int said;                        /* its recording (0..2) */
static float said_at;                   /* the race clock when it began */
static float quiet;                     /* how long the voice has been silent (0x119078) */
static int turn[SAY_EVENTS];            /* the recording an event said last in mode 3 (0x1191bc) */
static float timer[SAY_EVENTS];         /* the triggers' timers (0x119228); -1: said, wait for a re-arm */
static int stage;                       /* the start lights' state last said */
/* said_end: the race's end has been queued. had_lead: lead has been said and not yet lost
   (loslea). */
static int said_end, had_lead;

void speech_reset(void)
{
    voice = snd_stop(voice);
    queued = 0;
    quiet = 0.7f;
    /* Each event's turn starts at 2 (its first line is then recording 0), or at 0 or 1. The
       game takes this from its timer tick at the set-up (original: STARTL.EXE 0x107fd); here
       the tick is the cycle counter. */
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
    if (!speech_on) return;
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

/* Trigger "hold" (original: STARTL.EXE 0x24bec). Once cond has held for t seconds, queues ev
   (mode 3, delay 0.7 s) and does not say it again until cond has gone. Returns 1 when it queues
   the event. */
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

/* Trigger "repeat" (original: STARTL.EXE 0x24c68). Queues ev (mode 3, delay 0.7 s) every t
   seconds while cond holds. */
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

/* Trigger "once" (original: STARTL.EXE 0x24cd0). Queues ev with the given mode (delay 0.7 s)
   once, until speech_rearm or speech_reset. */
static void once(int ev, int mode)
{
    if (timer[ev] < 0.0f) return;
    speech_say(ev, mode, 0.7f);
    timer[ev] = -1.0f;
}

/* What the commentator notices while the race is on (original: STARTL.EXE 0x2425c). Called
   once per step. The events and their conditions:
     back      heading within 45 degrees of the road reversed: every 5 s;
     stop      speed under 1000 units/s: every 8 s;
     hurry     the countdown is at 5 s or under: once;
     lead      the player is first for 5 s;
     loslea    after that, second or worse for 5 s;
     bend      the car is on a marker with flag 4;
     skid      sliding for the car's grip x 2.9 s;
     bad       last (behind max(2, number of opponents) others) for 8 s;
     lapped    a lap up on an opponent for 2 s;
     hitcar    whenever the contact with an opponent (opp.h) is over 0.8.
   Some depend on the game mode (car.h):
     hurry     not in cone carnage and slalom (mask 0x17);
     lead      not in the special modes (mask 0x13);
     cones     cone carnage, once the player is past the line;
     all       cone carnage, once every prop is down; slalom, once every gate is done and no prop
               was touched. */
static void notice(const struct car_race *rc)
{
    const struct car_state *c = car_state();
    int back = car_backwards(), rank = opp_player_rank(), nopp = opp_count(), nm;
    const struct track_marker *mk = track_markers(&nm);
    repeat(back, SAY_BACK, 5.0f);
    repeat(c->speed < 1000.0f, SAY_STOP, 8.0f);
    if ((car_mode & (CAR_NORMAL | CAR_CHAMP | CAR_ATTACK | CAR_NETWORK)) && rc->timer <= 5.0f)
        once(SAY_HURRY, 3);
    if ((car_mode & (CAR_NORMAL | CAR_CHAMP | CAR_NETWORK)) && hold(rank == 1, SAY_LEAD, 5.0f))
        had_lead = 1;
    if (hold(had_lead && rank >= 2, SAY_LOSLEA, 5.0f)) had_lead = 0;
    if (!back) hold(nm && (mk[car_marker()].flags & 4), SAY_BEND, 0.0f);
    hold(c->slide_want != 0.0f, SAY_SKID, c->spec.grip * 2.9f);
    hold((nopp < 2 ? 2 : nopp) < rank, SAY_BAD, 8.0f);
    hold(opp_lapped(), SAY_LAPPED, 2.0f);
    repeat(opp_hitcar() > 0.8f, SAY_HITCAR, 0.0f);
    int all, down = scenery_hits(&all), gates, through = car_gates(&gates);
    if ((car_mode == CAR_CONES && down == all) || (car_mode == CAR_SLALOM && through == gates && !down))
        once(SAY_ALL, 3);                               /* original: STARTL.EXE 0x2437b */
    if (car_mode == CAR_CONES && rc->clock_on)                  /* past the line (0x24570) */
        once(SAY_CONES, 3);
}

/* Picks which of the event's recordings (0..2) is spoken (original: STARTL.EXE 0x24a24,
   0x24ae8): in modes 0..2 that recording, in mode 3 the next one after the one said last, skipping
   the ones the event does not have. */
static int pick(int ev, int mode)
{
    if (mode < 3) return mode;
    for (int tries = 0; tries < 3; tries++) {
        if (++turn[ev] == 3) turn[ev] = 0;
        if (recording[ev][turn[ev]] >= 0) break;
    }
    return turn[ev];
}

/* Writes a race clock in seconds to the debug log, to 1/100 s. */
static void log_time(float s)
{
    int32_t t = (int32_t)(s * 100.0f + 0.5f);
    dlog_dec(t / 100);
    dlog_char('.');
    dlog_char((char)('0' + t / 10 % 10));
    dlog_char((char)('0' + t % 10));
}

/* The first event in the queue is done: its line has ended, or it found no voice. The debug log
   gets "say <from>-<to> <event> <recording>" with the race clock when the line began and ended,
   and " silent" if no voice was found. The log is written when the line has ended, so that
   the frame report at the start keeps its room. Then the event leaves the queue, and the
   silence clock starts again from 0. */
static void pop(void)
{
    dlog_str("say ");
    log_time(said_at);
    dlog_char('-');
    log_time(car_race()->clock);
    dlog_char(' ');
    dlog_str(event_name[queue[0].ev]);
    dlog_char(' ');
    dlog_dec(said);
    if (voice < 0) dlog_str(" silent");
    dlog_char('\n');
    queued--;
    for (int i = 0; i < queued; i++)
        queue[i] = queue[i + 1];
    voice = -1;
    quiet = 0.0f;
}

/* Plays the queue (original: STARTL.EXE 0x24820). While a line is spoken, it waits for the line
   to end. Otherwise the silence clock runs. The event with the shortest delay is moved to the
   front of the queue (0x2491c), and is spoken once the clock has passed its delay. */
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
    said = pick(queue[0].ev, queue[0].mode);
    said_at = car_race()->clock;
    int r = recording[queue[0].ev][said];
    voice = r >= 0 ? snd_play(SND_SPEECH + r) : -1;
    if (voice < 0) pop();
}

void speech_step(void)
{
    const struct car_race *rc = car_race();
    int over = rc->finished || rc->game_over;
    /* The end of the race is said once: time over (original: STARTL.EXE 0x20f88) says notime,
       and the finish (0x23e78) says win or lose. In the special modes, where the player races
       alone, the finish counts as won if made_it is set (0x23ef0 and following). */
    if (over && !said_end) {
        said_end = 1;
        int won = car_mode & CAR_ALONE ? rc->made_it : opp_player_rank() == 1;
        speech_say(rc->game_over ? SAY_NOTIME : won ? SAY_WIN : SAY_LOSE, 3, 0.0f);
    }
    /* The start lights (original: STARTL.EXE 0x20d45): every 2 s of the race clock before 8 s
       says the next of three, two, one, go (mode 0, no delay). */
    if (!over && rc->clock < 8.0f && (int)rc->clock / 2 != stage) {
        stage = (int)rc->clock / 2;
        speech_say(SAY_THREE + stage, 0, 0.0f);
    }
    if (rc->on && !over) notice(rc);
    play();
}

int speech_busy(void)
{
    return queued > 0;
}
