#include "fm.h"
#include "dat.h"

#ifdef FM_HOST                              /* tools/host/fmdump.c: the hardware's side on the PC */
extern uint32_t fm_host_now(void);
extern uint32_t fm_host_fill(void);
extern void fm_host_put(uint32_t word);
extern void fm_host_ctrl(uint32_t run);
extern void fm_host_gain(uint32_t gain);
#define HW_NOW()        fm_host_now()
#define HW_FILL()       fm_host_fill()
#define HW_PUT(w)       fm_host_put(w)
#define HW_RUN(r)       fm_host_ctrl(r)
#define HW_GAIN(g)      fm_host_gain(g)
extern uint32_t fm_host_tick, fm_host_rounds, fm_host_notes;    /* the ticks run, the rounds of the song, the notes struck */
#else
#include "hw.h"
#define HW_NOW()        (FM_TIME & 0xFFFFu)
#define HW_FILL()       (FM_STATUS & 0x1FFu)
#define HW_PUT(w)       (FM_WRITE = (w))
#define HW_RUN(r)       (FM_CTRL = (r))
#define HW_GAIN(g)      (FM_GAIN = (g))
#endif

/*
 * Time is counted in the chip's samples (opl.v: 3579545 / 72 = 49715.9 a second). The song's
 * ticks come at the rate of its header; a write of the song is queued with the time of its tick.
 */
#define OPL_CLOCK   3579545u                /* the chip's clock; 72 clocks a sample */
#define AHEAD       3000                    /* events are queued up to 60 ms ahead of the clock */
#define LEAD        1500                    /* a song starts 30 ms after its call; a pause's end likewise */
#define BEHIND      6000                    /* the song's time more than 120 ms behind the clock: it is set to it */
#define ROOM        48                      /* free places the hardware's queue needs for one message (a note: 25 writes) */
#define QUEUE       256
#define FM_GAIN_DEFAULT 50                  /* opl.v: 32 = as the chip makes it; the original's was the card's analog mixer */

#define MAXTRACK    32

/* ------------------------------------------------------------- the driver */

static const uint8_t *banks_file;
static uint32_t banks_size;
static uint8_t  d_melodic[128][30], d_drum[128][30];    /* packed */
static uint8_t  d_drum_pitch[128];
static int      d_program[16], d_bend[16], d_bent[16], d_range[16], d_volume[16];
static int      d_voice_chan[9], d_voice_vel[9];
static uint8_t  d_voice_note[9];                        /* 0 = free */
static uint8_t  d_level[0x16], d_release[0x16];         /* an operator's 0x40 and 0x80 as the instrument has them */
static uint8_t  d_a0[9], d_b0[9];
static uint32_t d_dropped;

static const uint8_t d_ops[9][2] = { {0, 3}, {1, 4}, {2, 5}, {8, 11}, {9, 12}, {10, 13}, {16, 19}, {17, 20}, {18, 21} };

/* 0x2f00: channel volume x velocity (halved) -> what comes off the level */
static const uint8_t d_soft[64] = {
    63, 58, 53, 48, 44, 41, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16,
    15, 14, 14, 13, 13, 12, 12, 11, 11, 10, 10, 9, 9, 8, 8, 7, 7, 6, 6, 6, 5, 5, 5, 4, 4, 4, 4, 3, 3, 3, 2, 2, 2, 1, 1, 0
};
/* 0x2f4c: octave and frequency number of notes 12.. (eight octaves and seven notes), and
   straight after it, at 0x30e8, the numbers the pitch bend uses where a bend crosses into the
   next octave */
static const uint16_t d_freq[] = {
    0x157, 0x16b, 0x181, 0x198, 0x1b0, 0x1ca, 0x1e5, 0x202, 0x220, 0x241, 0x263, 0x287,
    0x557, 0x56b, 0x581, 0x598, 0x5b0, 0x5ca, 0x5e5, 0x602, 0x620, 0x641, 0x663, 0x687,
    0x957, 0x96b, 0x981, 0x998, 0x9b0, 0x9ca, 0x9e5, 0xa02, 0xa20, 0xa41, 0xa63, 0xa87,
    0xd57, 0xd6b, 0xd81, 0xd98, 0xdb0, 0xdca, 0xde5, 0xe02, 0xe20, 0xe41, 0xe63, 0xe87,
    0x1157, 0x116b, 0x1181, 0x1198, 0x11b0, 0x11ca, 0x11e5, 0x1202, 0x1220, 0x1241, 0x1263, 0x1287,
    0x1557, 0x156b, 0x1581, 0x1598, 0x15b0, 0x15ca, 0x15e5, 0x1602, 0x1620, 0x1641, 0x1663, 0x1687,
    0x1957, 0x196b, 0x1981, 0x1998, 0x19b0, 0x19ca, 0x19e5, 0x1a02, 0x1a20, 0x1a41, 0x1a63, 0x1a87,
    0x1d57, 0x1d6b, 0x1d81, 0x1d98, 0x1db0, 0x1dca, 0x1de5, 0x1e02, 0x1e20, 0x1e41, 0x1e63, 0x1e87,
    0x1eae, 0x1eb7, 0x1f02, 0x1f30, 0x1f60, 0x1f94, 0x1fca,
    0x144, 0x132, 0x121, 0x110, 0x101, 0xf8, 0xe5, 0xd8, 0xcc, 0xc1, 0xb6, 0xac,
    0xa1, 0x90, 0x99, 0x86, 0x81, 0x7c, 0x73, 0x6c, 0x66, 0x60, 0x5b, 0x56,
    0x2ae, 0x2d6, 0x302, 0x330, 0x360, 0x394, 0x3de, 0x404, 0x440, 0x482, 0x4c6, 0x50e
};
#define D_FREQS     ((int)(sizeof d_freq / sizeof d_freq[0]))
#define D_BEND      103                     /* where the pitch bend's numbers start in d_freq */

/* the time of the writes being made: a song's tick, or for what the CPU does between ticks the
   clock plus a little, never before the last write queued */
static uint32_t now32;                      /* the hardware clock, 16 bits, as a running count */
static uint32_t stamp;                      /* of the writes now */
static uint32_t q_last;                     /* of the last write queued */
static uint32_t full_drops;                 /* writes the hardware's queue had no room for */

static void chip(int reg, int val)
{
    if (HW_FILL() >= QUEUE) {
        full_drops++;
        return;
    }
    if ((int32_t)(stamp - q_last) > 0) q_last = stamp;
    HW_PUT((stamp & 0xFFFFu) << 16 | (uint32_t)(reg & 0xFF) << 8 | (uint32_t)(val & 0xFF));
}

/* the word the driver reads at 0x2f1c + 4 x note */
static unsigned d_note_word(int note)
{
    if (note >= 12) return note - 12 < D_FREQS ? d_freq[note - 12] : 0;
    if (note < 0 || note > 8) return 0;
    const uint8_t *b = d_soft + 28 + 4 * note;          /* the end of the level table */
    return b[0] | b[1] << 8 | b[2] << 16 | (unsigned)b[3] << 24;
}

/* 0xd37: a bank's instrument, one value a byte, into register bytes */
static void d_pack(uint8_t *r)
{
    r[0x0b] = (uint8_t)(r[0x0b] << 7 | r[0x0c] << 6 | r[0x07] << 5 | r[0x0d] << 4 | r[0x03]);
    r[0x02] = (uint8_t)(r[0x02] << 6 | r[0x0a]);
    r[0x05] = (uint8_t)(r[0x05] << 4 | r[0x08]);
    r[0x06] = (uint8_t)(r[0x06] << 4 | r[0x09]);
    r[0x0e] = (uint8_t)(r[0x04] * 2 | r[0x0e]);
    r[0x18] = (uint8_t)(r[0x18] << 7 | r[0x19] << 6 | r[0x14] << 5 | r[0x1a] << 4 | r[0x10]);
    r[0x0f] = (uint8_t)(r[0x0f] << 6 | r[0x17]);
    r[0x12] = (uint8_t)(r[0x12] << 4 | r[0x15]);
    r[0x13] = (uint8_t)(r[0x13] << 4 | r[0x16]);
}

/* A bank: "ADLIB-" at +2, the instruments in use at +8, the offsets of the name records (12
   bytes; the third a drum's pitch) at +12 and of the instruments (30 bytes) at +16 */
static int d_load_bank(const uint8_t *b, uint32_t n, uint8_t out[128][30], uint8_t *pitch)
{
    static const char magic[6] = { 'A', 'D', 'L', 'I', 'B', '-' };
    if (n < 28) return 0;
    for (int i = 0; i < 6; i++)
        if (b[2 + i] != (uint8_t)magic[i]) return 0;
    int used = (int16_t)(b[8] | b[9] << 8);
    uint32_t names = b[12] | b[13] << 8, data = b[16] | b[17] << 8;
    for (int i = 0; i < 128; i++) {
        if (data + 30 * (uint32_t)i + 30 > n) break;
        for (int k = 0; k < 30; k++)
            out[i][k] = b[data + 30 * (uint32_t)i + (uint32_t)k];
        if (i < used - 2) d_pack(out[i]);               /* the last two in use stay as they are in the file */
        if (pitch && names + 12 * (uint32_t)i + 12 <= n) pitch[i] = b[names + 12 * (uint32_t)i + 2];
    }
    return 1;
}

/* 0x1c35, 0x1cb4, and the values the driver's own memory starts with */
static void d_init(void)
{
    for (int v = 0; v < 9; v++) {
        d_voice_note[v] = 0;
        d_a0[v] = d_b0[v] = 0;
        d_voice_chan[v] = 0;
        d_voice_vel[v] = 0x7f;
    }
    for (int i = 0; i < 0x16; i++)
        d_level[i] = d_release[i] = 0;
    for (int c = 0; c < 16; c++) {
        d_program[c] = 0;
        d_bend[c] = 0x40;
        d_bent[c] = 0;
        d_range[c] = 2;
        d_volume[c] = 0x7f;
    }
    chip(0x01, 0x20);
    for (int v = 0; v < 9; v++)
        chip(0xb0 + v, 0);
    chip(0xbd, 0xc0);
}

/* 0x1f9e */
static void d_key_off(int v)
{
    if (!d_voice_note[v]) return;
    d_b0[v] &= 0xdf;
    chip(0xb0 + v, d_b0[v]);
    d_voice_note[v] = 0;
}

/* the level register of operator `op` for a voice's loudness (0xf6b, 0x237f) */
static void d_write_level(int op, unsigned loud)
{
    unsigned t = d_soft[(loud & 0xff) >> 1];
    unsigned level = (0x2000 - (0x40 - t) * 2 * (0x40 - (d_level[op] & 0x3fu))) >> 7;
    chip(0x40 + op, (int)((level & 0xff) | (d_level[op] & 0xc0u)));
}

static void d_voice_levels(int v, int chan_volume)
{
    unsigned loud = (unsigned)((chan_volume << 7) / 0x7f) * (unsigned)d_voice_vel[v] >> 7;
    if (d_melodic[d_program[d_voice_chan[v]] & 127][0x0e] & 1) d_write_level(d_ops[v][0], loud);
    d_write_level(d_ops[v][1], loud);
}

/* 0x201e */
static void d_instrument(const uint8_t *r, int v)
{
    int m = d_ops[v][0], c = d_ops[v][1];
    d_release[c] = r[0x13];
    d_release[m] = r[0x06];
    chip(0x20 + m, r[0x0b]);
    chip(0x40 + m, r[0x02]);
    d_level[m] = r[0x02];
    chip(0x60 + m, r[0x05]);
    chip(0x80 + m, r[0x06]);
    chip(0xc0 + v, r[0x0e]);
    chip(0xe0 + m, r[0x1c]);
    chip(0x20 + c, r[0x18]);
    d_level[c] = r[0x0f];
    chip(0x60 + c, r[0x12]);
    chip(0x80 + c, r[0x13]);
    chip(0xe0 + c, r[0x1d]);
}

/* 0x1a7d: the frequency word of a voice's note under a pitch bend */
static unsigned d_bend_word(unsigned bend, int note, int v)
{
    int range = d_range[d_voice_chan[v]];
    if (note < 12 || note - range < 0 || note + range - 12 >= D_FREQS || range < 0 || range > 12)
        return d_note_word(note);
    unsigned f = d_note_word(note), d;
    if (bend < 0x40) {
        d = f - d_note_word(note - range);
        if (d > 0x2cf) d = ((f & 0x3ff) - d_freq[D_BEND - 1 + range]) & 0x3ff;
        f -= d * ((0x3f - bend) * 1000 >> 6) / 1000;
    } else {
        d = d_note_word(note + range) - f;
        if (d > 0x2cf) {
            f = ((f & 0x1c00) + 0x400) | d_freq[D_BEND + 11 - (note - 12) % 12];
            d = d_note_word(note + range) - f;
        }
        f += d * ((bend - 0x40) * 1000 >> 6) / 1000;
    }
    return f;
}

/* 0x22e9 */
static void d_frequency(int v, unsigned word)
{
    d_a0[v] = (uint8_t)word;
    d_b0[v] = (uint8_t)(word >> 8) | 0x20;
    chip(0xa0 + v, d_a0[v]);
    chip(0xb0 + v, d_b0[v]);
}

/* 0xf6b */
static void d_note_on(int chan, int note, int velocity)
{
    /* 0x1ed7: the first free voice, else the first voice of the lowest channel that never had a bend */
    int v = -1;
    for (int i = 0; i < 9 && v < 0; i++)
        if (!d_voice_note[i]) v = i;
    for (int c = 0; c < 16 && v < 0; c++) {
        if (d_bent[c]) continue;
        for (int i = 0; i < 9 && v < 0; i++)
            if (d_voice_chan[i] == c) v = i;
    }
    if (v < 0) {                                        /* (the original writes over its own memory here) */
        d_dropped++;
        return;
    }

#ifdef FM_HOST
    fm_host_notes++;
#endif
    d_key_off(v);
    d_voice_chan[v] = chan;
    for (int i = 0; i < 5; i++) {                       /* both releases to the fastest, five times over */
        chip(0x80 + d_ops[v][1], d_release[d_ops[v][1]] | 0x0f);
        chip(0x80 + d_ops[v][0], d_release[d_ops[v][0]] | 0x0f);
    }
    d_instrument(chan == 9 ? d_drum[note] : d_melodic[d_program[chan] & 127], v);
    d_voice_vel[v] = velocity;
    d_voice_levels(v, d_volume[chan]);

    /* 0x2227 */
    unsigned word = d_note_word(chan == 9 ? d_drum_pitch[note] : note);
    d_a0[v] = (uint8_t)word;
    d_b0[v] = (uint8_t)(word >> 8) | 0x20;
    chip(0xa0 + v, d_a0[v]);
    chip(0xb0 + v, d_b0[v] & 0xdf);
    chip(0xb0 + v, d_b0[v]);
    d_voice_note[v] = (uint8_t)note;

    if (chan != 9 && d_bent[chan]) d_frequency(v, d_bend_word((unsigned)d_bend[chan], note, v));
}

/* 0x80d: one MIDI message */
static void d_message(int status, int a, int b)
{
    int chan = status & 15;
    a &= 127;
    b &= 127;
    switch (status & 0xf0) {
    case 0x90:
        if (b) {
            d_note_on(chan, a, b);
            break;
        }
        /* fall through */
    case 0x80:                                          /* 0x157b */
        for (int v = 0; v < 9; v++)
            if (d_voice_note[v] == a && d_voice_chan[v] == chan) d_key_off(v);
        break;
    case 0xb0:                                          /* 0x1846 */
        if (a == 7) {                                   /* 0x237f */
            d_volume[chan] = b;
            for (int v = 0; v < 9; v++)
                if (d_voice_note[v] && d_voice_chan[v] == chan) d_voice_levels(v, b);
        } else if (a == 0x66) {
            d_range[chan] = b;
        } else if (a == 0x79) {                         /* 0x174f */
            d_volume[chan] = 0x7f;
            if (chan < 9) d_voice_vel[chan] = 0x7f;
            d_bend[chan] = 0x40;
            d_range[chan] = 2;
        } else if (a == 0x7b) {                         /* 0x16e9 */
            for (int v = 0; v < 9; v++)
                if (d_voice_chan[v] == chan) {
                    d_key_off(v);
                    d_voice_note[v] = 0;
                }
        }
        break;
    case 0xc0:                                          /* 0x17f9 */
        d_program[chan] = a;
        break;
    case 0xe0:                                          /* 0x19bb: the high byte only */
        if (chan == 9) break;
        d_bend[chan] = b;
        d_bent[chan] = 1;
        for (int v = 0; v < 9; v++)
            if (d_voice_note[v] && d_voice_chan[v] == chan) d_frequency(v, d_bend_word((unsigned)b, d_voice_note[v], v));
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------- the player */

struct track {
    const uint8_t *p, *end, *start;
    int32_t wait;
    int done, chan;
};

static struct track tracks[MAXTRACK];
static int ntrack;
static int song;                            /* the song loaded, 0 none */
static int paused;
static uint32_t tick_clock, tick_den;       /* the chip's clock; 72 x the song's ticks a second */
static uint32_t t_tick, t_frac;             /* the next tick's time in chip samples, and its fraction (of tick_den) */
static int tr_i, alive;                     /* the tick being run: the next track, and whether any is still going */
static int master_v = 0x6000, fade = 0x8000;
static int g_master = 96;                   /* 0xd3a54: 0..127 */
static uint8_t g_chan_volume[16];           /* 0x4018: controller 7 as the song last gave it */
static uint32_t behind_count, full_frames;

/* the clock as a running count, from the hardware's 16 bits (called at least every 0.6 s) */
static void clock_sync(void)
{
    now32 += (uint16_t)(HW_NOW() - (uint16_t)now32);
}

/* what the CPU writes between ticks: just after the clock, not before what is queued */
static void stamp_now(void)
{
    clock_sync();
    stamp = now32 + 100;
    if ((int32_t)(stamp - q_last) < 0) stamp = q_last;
}

/* 0xe2ca9: a message on its way to the driver */
static void send(int status, int a, int b)
{
    if ((status & 0xf0) == 0xb0 && a == 7) {
        g_chan_volume[status & 15] = (uint8_t)b;
        b = (g_master * ((127 * b) >> 7)) >> 7;
    }
    d_message(status, a, b);
}

/* 0xe37eb: a song's end, or its being stopped */
static void song_reset(void)
{
    for (int i = 1; i < ntrack; i++) {
        int c = tracks[i].chan & 15;
        d_message(0xb0 | c, 0x7b, 0);
        d_message(0xb0 | c, 0x79, 0);
        d_message(0xe0 | c, 0x40, 0x40);
        d_message(0xb0 | c, 7, 0);
    }
}

static int32_t read_delay(struct track *t)
{
    int32_t v = 0;
    int shift = 0;
    while (t->p < t->end) {
        int c = *t->p++;
        v |= (int32_t)(c & 0x7f) << shift;
        shift += 7;
        if (c & 0x80) break;
    }
    return v;
}

static void song_rewind(void)
{
    for (int i = 0; i < ntrack; i++) {
        struct track *t = &tracks[i];
        t->p = t->start;
        t->done = 0;
        t->wait = read_delay(t);
    }
}

static void track_event(struct track *t, int number)
{
    if (t->p >= t->end) {
        t->done = 1;
        return;
    }
    int st = *t->p++;
    if (st == 0xff) {
        int type = t->p < t->end ? *t->p++ : 0x2f;
        uint32_t len = 0;
        while (t->p < t->end) {
            int c = *t->p++;
            len = len << 7 | (uint32_t)(c & 0x7f);
            if (!(c & 0x80)) break;
        }
        t->p += len < (uint32_t)(t->end - t->p) ? len : (uint32_t)(t->end - t->p);
        if (type == 0x2f) t->done = 1;
        return;
    }
    if (st < 0x80) {                                    /* not a message: give the track up */
        t->done = 1;
        return;
    }
    int a = t->p < t->end ? *t->p++ : 0, b = 0;
    int kind = st & 0xf0;
    if (kind != 0xc0 && kind != 0xd0) b = t->p < t->end ? *t->p++ : 0;
    if (number) send(st, a, b);                         /* track 0's messages are never sent */
}

static int room(void)
{
    return HW_FILL() + ROOM <= QUEUE;
}

/* One tick (0xe3d51): each track's wait goes down and its messages are sent while the wait has
   run out. It can stop for room in the queue between two messages and go on at the next call.
   1 when the tick is done. */
static int song_tick(void)
{
    for (; tr_i < ntrack; tr_i++) {
        struct track *t = &tracks[tr_i];
        while (!t->done && t->wait <= 0) {
            if (!room()) return 0;
            track_event(t, tr_i);
            if (!t->done) t->wait += read_delay(t);
        }
        if (!t->done) {
            t->wait--;
            alive = 1;
        }
    }
    tr_i = 0;
    if (!alive) {                                       /* round again */
        song_reset();
        song_rewind();
#ifdef FM_HOST
        fm_host_rounds++;
#endif
    }
    alive = 0;
#ifdef FM_HOST
    fm_host_tick++;
#endif
    return 1;
}

/* ------------------------------------------------------------ interface */

static int master_of(void);

int fm_init(const uint8_t *file, uint32_t size)
{
    struct dat_entry e;
    int ok = 0;
    banks_file = 0;
    if (!file) return 0;
    if (!dat_find(file, size, "MELODIC.BNK", &e) || e.method != 0 || e.offset + e.size > size) return 0;
    if (!d_load_bank(file + e.offset, e.size, d_melodic, 0)) return 0;
    if (!dat_find(file, size, "DRUM.BNK", &e) || e.method != 0 || e.offset + e.size > size) return 0;
    if (!d_load_bank(file + e.offset, e.size, d_drum, d_drum_pitch)) return 0;
    banks_file = file;
    banks_size = size;
    for (int n = 1; n <= FM_SONGS; n++) {
        char name[12] = { 'M', 'U', 'S', 'I', 'C', (char)('0' + n), '.', 'H', 'M', 'P', 0 };
        ok += dat_find(file, size, name, &e) && e.method == 0 && e.offset + e.size <= size;
    }
    return ok > 0;
}

void fm_stop(void)
{
    if (!song) return;
    HW_RUN(0);
    song = 0;
    paused = 0;
}

int fm_play(int n)
{
    struct dat_entry e;
    char name[12] = { 'M', 'U', 'S', 'I', 'C', (char)('0' + n), '.', 'H', 'M', 'P', 0 };
    fm_stop();
    if (!banks_file || n < 1 || n > FM_SONGS) return 0;
    if (!dat_find(banks_file, banks_size, name, &e) || e.method != 0 || e.offset + e.size > banks_size) return 0;
    const uint8_t *d = banks_file + e.offset;
    static const char magic[14] = { 'H', 'M', 'I', 'M', 'I', 'D', 'I', 'P', '0', '1', '3', '1', '9', '5' };
    if (e.size < 0x388) return 0;
    for (int i = 0; i < 14; i++)
        if (d[i] != (uint8_t)magic[i]) return 0;

    int count = d[0x30] | d[0x31] << 8;
    uint32_t rate = (uint32_t)(d[0x38] | d[0x39] << 8);
    ntrack = 0;
    uint32_t o = 0x388;
    for (int i = 0; i < count && i < MAXTRACK && o + 12 <= e.size; i++) {
        uint32_t len = d[o + 4] | d[o + 5] << 8 | d[o + 6] << 16 | (uint32_t)d[o + 7] << 24;
        if (len < 12 || len > e.size - o) break;
        struct track *t = &tracks[ntrack++];
        t->start = d + o + 12;
        t->end = d + o + len;
        t->chan = d[o + 8];
        o += len;
    }
    if (!ntrack) return 0;

    fade = 0x8000;                                      /* a song starts with the master whole */
    g_master = master_of();
    HW_RUN(0);                                          /* the chip anew, the queue empty */
    HW_GAIN(FM_GAIN_DEFAULT);
    HW_RUN(1);
    now32 = HW_NOW();
    q_last = now32;
    full_drops = d_dropped = behind_count = full_frames = 0;
    for (int c = 0; c < 16; c++)
        g_chan_volume[c] = 0x7f;
    stamp = now32 + LEAD / 3;
    d_init();
    song_rewind();
    tr_i = alive = 0;
    tick_clock = OPL_CLOCK;
    tick_den = 72u * (rate ? rate : 120u);
    t_tick = now32 + LEAD;
    t_frac = 0;
    paused = 0;
    song = n;
    return 1;
}

void fm_frame(void)
{
    if (!song || paused) return;
    clock_sync();
    int32_t ahead = (int32_t)(t_tick - now32);
    if (ahead < -BEHIND) {                              /* the CPU was away: the music goes on from now */
        t_tick = now32 + LEAD;
        ahead = LEAD;
        behind_count++;
    }
    while (ahead <= AHEAD) {
        stamp = t_tick;
        if (!song_tick()) {
            full_frames++;
            break;
        }
        t_tick += tick_clock / tick_den;
        t_frac += tick_clock % tick_den;
        if (t_frac >= tick_den) {
            t_frac -= tick_den;
            t_tick++;
        }
        ahead = (int32_t)(t_tick - now32);
    }
}

void fm_pause(void)
{
    if (!song || paused) return;
    stamp_now();
    for (int i = 1; i < ntrack; i++)
        d_message(0xb0 | (tracks[i].chan & 15), 0x7b, 0);
    paused = 1;
}

void fm_resume(void)
{
    if (!song || !paused) return;
    clock_sync();
    t_tick = now32 + LEAD;
    t_frac = 0;
    paused = 0;
}

/* the driver's master: the game's volume times the fade, 0x6000 -> 96 */
static int master_of(void)
{
    int m = (master_v * fade >> 15) >> 8;               /* (at most 0x7fff x 0x8000: no overflow) */
    return m < 0 ? 0 : m > 127 ? 127 : m;
}

/* 0xe29d9: when the master changes, each track still going gets its channel's volume again */
static void apply_master(void)
{
    int m = master_of();
    if (m == g_master) return;
    g_master = m;
    if (!song) return;
    stamp_now();
    for (int i = 0; i < ntrack; i++)
        if (!tracks[i].done) send(0xb0 | (tracks[i].chan & 15), 7, g_chan_volume[tracks[i].chan & 15]);
}

void fm_master(int32_t v)
{
    master_v = v < 0 ? 0 : v > 0x7FFF ? 0x7FFF : v;
    apply_master();
}

void fm_fade(int32_t f)
{
    fade = f < 0 ? 0 : f > 0x8000 ? 0x8000 : f;
    apply_master();
}

int fm_playing(void)
{
    return song && !paused ? song : 0;
}

void fm_stats(uint32_t *no_voice, uint32_t *full, uint32_t *behind)
{
    *no_voice = d_dropped;
    *full = full_frames + full_drops;
    *behind = behind_count;
}
