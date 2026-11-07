/*
 * FM music: HMI's song player and the driver for the OPL2 chip, as the original has them
 * (interface, data flow and timing rules: fm.h). The addresses in the comments are the
 * original's:
 *   - The player is in STARTL.EXE at 0xd2000..0xe4000: the timer routine 0xe3d51, a message on
 *     its way 0xe2ca9, a song's end 0xe37eb, the volumes 0xe29d9 and 0xd3a54. The original
 *     restarts a song that has stopped playing from the game's poll (original: STARTL.EXE 0x2fcd4,
 *     repeat flag 0x2fc7c). Here song_tick starts the next round itself.
 *   - The driver is fmmidi.com, the first module of HMIMDRV.386. Its addresses count from the
 *     module's start. A comment (original: fmmidi.com 0x...) names its routine or table.
 * The file has three parts: the driver (d_*), the player (song_*, tick and track), and the
 * interface (fm_*).
 */
#include "fm.h"
#include "dat.h"

/* The hardware's side. On the Pocket the macros use the registers of hw.h. On the PC,
   tools/host/fmdump.c (FM_HOST) supplies functions that model them.
   HW_NOW   the chip's clock, low 16 bits (FM_TIME), in chip samples;
   HW_FILL  the number of writes in the hardware's queue;
   HW_PUT   puts a write into the queue: bits 31..16 its time, 15..8 the register, 7..0 the value;
   HW_RUN   1 runs the chip; 0 holds it in reset (silent) and empties the queue;
   HW_GAIN  the output times this / 32. */
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
/* counters of the PC run: the ticks run, the rounds of the song, the notes struck */
extern uint32_t fm_host_tick, fm_host_rounds, fm_host_notes;
#else
#include "hw.h"
#define HW_NOW()        (FM_TIME & 0xFFFFu)
#define HW_FILL()       (FM_STATUS & 0x1FFu)
#define HW_PUT(w)       (FM_WRITE = (w))
#define HW_RUN(r)       (FM_CTRL = (r))
#define HW_GAIN(g)      (FM_GAIN = (g))
#endif

/*
 * Time is counted in the chip's samples (opl.v): the chip's clock divided by 72, which is
 * 49715.9 samples a second. The hardware's counter has 16 bits and wraps after 1.3 s.
 *   - The song's ticks come at the rate of its header. A write of the song is queued with the
 *     time of its tick.
 *   - fm_frame queues the ticks that are due up to AHEAD samples (60 ms) ahead of the clock.
 *   - A song starts LEAD samples (30 ms) after its call; a pause's end likewise.
 *   - If the song's time is more than BEHIND samples (120 ms) behind the clock, it is set to
 *     the clock plus LEAD.
 *   - The hardware's queue holds QUEUE writes. A message is handled only while ROOM places are
 *     free. A note takes 25 writes.
 */
#define OPL_CLOCK   3579545u                /* the chip's clock; 72 clocks a sample */
#define AHEAD       3000                    /* events are queued up to 60 ms ahead of the clock */
#define LEAD        1500                    /* a song starts 30 ms after its call; a pause's end likewise */
#define BEHIND      6000                    /* 120 ms: the song's time more behind than this is reset */
#define ROOM        48                      /* free places the queue needs for one message */
#define QUEUE       256                     /* writes the hardware's queue holds */
/* The gain of the chip's output (HW_GAIN), in 1/32. 32 is the chip's level as opl.v makes it. The
   original had the sound card's analog mixer, so the level is a choice here. For comparison, with
   gain 50 the songs 2 and 5 through the real chip (sim/opl play) have rms 1913 and 2971 and peak
   10985 and 14862. The CD tracks 2 and 6 as the mixer plays them have rms 2585 and 2818 and
   peak 16384. */
#define FM_GAIN_DEFAULT 50

#define MAXTRACK    32                      /* tracks of a song that are used */

/* ------------------------------------------------------------- the driver */

/*
 * The driver's state. A channel is a MIDI channel (16, number 9 is the drums); a voice is one
 * of the chip's nine channels; an operator is one of the chip's 18 oscillators, numbered by
 * its register offset (0..0x15, with gaps). Each voice has two operators, d_ops[v]: the
 * modulator and the carrier.
 */
static const uint8_t *banks_file;           /* MUSIC.DAT in memory */
static uint32_t banks_size;
static uint8_t  d_melodic[128][30], d_drum[128][30];    /* the banks' instruments, packed (d_pack) */
static uint8_t  d_drum_pitch[128];                      /* the note at which a drum instrument plays */
/* per channel: program, pitch bend (0x40 is the middle), whether it ever had a bend, the bend's
   range in semitones, volume (controller 7) */
static int      d_program[16], d_bend[16], d_bent[16], d_range[16], d_volume[16];
static int      d_voice_chan[9], d_voice_vel[9];        /* per voice: its channel and velocity */
static uint8_t  d_voice_note[9];                        /* per voice: the note it plays, 0 = free */
/* per operator: its registers 0x40 and 0x80 as the instrument has them */
static uint8_t  d_level[0x16], d_release[0x16];
static uint8_t  d_a0[9], d_b0[9];                       /* per voice: the last values of 0xa0 and 0xb0 */
static uint32_t d_dropped;                              /* notes that found no voice */

/* the two operators (register offsets) of each voice: modulator, carrier */
static const uint8_t d_ops[9][2] = {
    {0, 3}, {1, 4}, {2, 5}, {8, 11}, {9, 12}, {10, 13}, {16, 19}, {17, 20}, {18, 21}
};

/* What comes off an operator's level for a loudness (original: fmmidi.com 0x2f00). The index is
   the loudness, channel volume x velocity halved: 0 is silent, 63 the loudest. The value is 63
   for silent down to 0 for the loudest. d_write_level calls it t. */
static const uint8_t d_soft[64] = {
    63, 58, 53, 48, 44, 41, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28,
    27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 14, 13,
    13, 12, 12, 11, 11, 10, 10, 9, 9, 8, 8, 7, 7, 6, 6, 6,
    5, 5, 5, 4, 4, 4, 4, 3, 3, 3, 2, 2, 2, 1, 1, 0
};
/* Frequency words. A word has the octave (the chip's block) in bits 12..10 and the frequency
   number in bits 9..0. Its low byte goes to register 0xa0 and its high byte, with the key-on bit,
   to register 0xb0. The first 103 entries are the notes 12.., eight octaves of twelve notes and
   seven more (original: fmmidi.com 0x2f4c). Straight after them, from D_BEND on, are the numbers
   that the pitch bend uses where a bend crosses into the next octave (original: fmmidi.com
   0x30e8). */
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

/*
 * The time of the writes being made and the state of the queue:
 *   - the writes of a song's tick have the tick's time;
 *   - what the CPU writes between ticks (a pause, a volume) has the clock plus a little, but
 *     never a time before the last write queued, so that the writes stay in order.
 */
static uint32_t now32;                      /* the hardware clock, 16 bits, as a running count */
static uint32_t stamp;                      /* the time of the writes being made */
static uint32_t q_last;                     /* the time of the last write queued */
static uint32_t full_drops;                 /* writes the hardware's queue had no room for */

/* Queues one register write of the chip, with the time stamp. If the queue is full the write is
   lost and counted. (The driver's own routine for one register write is original: fmmidi.com
   0x1d64.) */
static void chip(int reg, int val)
{
    if (HW_FILL() >= QUEUE) {
        full_drops++;
        return;
    }
    if ((int32_t)(stamp - q_last) > 0) q_last = stamp;
    HW_PUT((stamp & 0xFFFFu) << 16 | (uint32_t)(reg & 0xFF) << 8 | (uint32_t)(val & 0xFF));
}

/* The frequency word of a note. The driver reads it at 0x2f1c + 4 x note (original: fmmidi.com
   0x2f1c). Notes from 12 on are in d_freq. For the notes 0..8 the original reads four bytes inside
   its level table (d_soft starts at 0x2f00), and so does this. Other notes give 0. */
static unsigned d_note_word(int note)
{
    if (note >= 12) return note - 12 < D_FREQS ? d_freq[note - 12] : 0;
    if (note < 0 || note > 8) return 0;
    const uint8_t *b = d_soft + 28 + 4 * note;          /* four bytes of the table, low byte first */
    return b[0] | b[1] << 8 | b[2] << 16 | (unsigned)b[3] << 24;
}

/* Packs a bank's instrument r (30 bytes, one value a byte) into register bytes (original:
   fmmidi.com 0xd37). The instrument has 2 bytes, then 13 for each operator (key scaling,
   multiplier, feedback, attack, sustain level, sustaining, decay, release, level, tremolo,
   vibrato, rate scaling, connection), then the two wave shapes. d_instrument lists which packed
   byte is which register. */
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

/* Reads a bank file b of n bytes into out: 128 instruments of 30 bytes, packed. The file has
   "ADLIB-" at +2, the number of instruments in use at +8, the offset of the name records at +12
   (12 bytes each; for a drum, the third byte is its pitch) and the offset of the instruments at
   +16. For a drum bank, pitch gets each drum's pitch. Returns 1 if the file is a bank. */
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
        /* As in the original, the last two instruments in use are not packed. */
        if (i < used - 2) d_pack(out[i]);
        if (pitch && names + 12 * (uint32_t)i + 12 <= n) pitch[i] = b[names + 12 * (uint32_t)i + 2];
    }
    return 1;
}

/* Sets the driver's state to its start values and the chip to its start state (original:
   fmmidi.com 0x1c35, 0x1cb4, and the values the driver's own memory starts with): register 0x01
   to 0x20 (waveform select on), every voice's key off, and register 0xbd to 0xc0 (the deep
   tremolo and vibrato, no rhythm mode). */
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

/* Key off for voice v, if it plays a note: clears the key bit (0x20) of its register 0xb0
   (original: fmmidi.com 0x1f9e). */
static void d_key_off(int v)
{
    if (!d_voice_note[v]) return;
    d_b0[v] &= 0xdf;
    chip(0xb0 + v, d_b0[v]);
    d_voice_note[v] = 0;
}

/* Writes the level register (0x40 + op) of operator `op` for a voice's loudness loud, 0..127
   (original: fmmidi.com 0xf6b, 0x237f). With t from d_soft for the loudness and the instrument's
   own level own (the low 6 bits of d_level), the level is 64 - (64 - t) x (64 - own) / 64. The
   instrument's key scaling bits (7..6) stay. */
static void d_write_level(int op, unsigned loud)
{
    unsigned t = d_soft[(loud & 0xff) >> 1];
    unsigned level = (0x2000 - (0x40 - t) * 2 * (0x40 - (d_level[op] & 0x3fu))) >> 7;
    chip(0x40 + op, (int)((level & 0xff) | (d_level[op] & 0xc0u)));
}

/* Sets the levels of voice v for the channel volume chan_volume and the voice's velocity. The
   carrier always gets a level; the modulator also if the instrument adds the two operators
   (bit 0 of register 0xc0). Whether it does is asked of the melodic instrument of the channel's
   program, also for a drum. This slip of the original is kept. */
static void d_voice_levels(int v, int chan_volume)
{
    unsigned loud = (unsigned)((chan_volume << 7) / 0x7f) * (unsigned)d_voice_vel[v] >> 7;
    if (d_melodic[d_program[d_voice_chan[v]] & 127][0x0e] & 1) d_write_level(d_ops[v][0], loud);
    d_write_level(d_ops[v][1], loud);
}

/* Writes a packed instrument r into the registers of voice v (original: fmmidi.com 0x201e), and
   remembers the operators' level and release (d_level, d_release) for later. The packed bytes
   are, for the modulator m and the carrier c:
     register 0x20 + op  tremolo, vibrato, sustaining, rate scaling, multiplier:  r[0x0b], r[0x18]
     register 0x40 + op  key scaling and level:  r[0x02], and for the carrier r[0x0f]
     register 0x60 + op  attack and decay:  r[0x05], r[0x12]
     register 0x80 + op  sustain level and release:  r[0x06], r[0x13]
     register 0xe0 + op  wave shape:  r[0x1c], r[0x1d]
     register 0xc0 + v   feedback and connection of the voice:  r[0x0e]
   The carrier's register 0x40 is not written here; d_write_level writes it. */
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

/* The frequency word of a voice's note under a pitch bend (original: fmmidi.com 0x1a7d). The
   bend is 0..127, 0x40 the middle, and nearly reaches the channel's range in semitones at its
   ends. The word is found by a straight line between the note's own word and that of the note
   `range` semitones lower (below the middle) or higher (above it). Where the two are more than
   0x2cf apart (the line crosses into another octave) the numbers from D_BEND on are used. If the
   note or the one `range` away is outside the table, or the range is not 0..12, the bend is
   ignored and the note's own word is returned. */
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

/* Sets the frequency of voice v to the word, with the key on (original: fmmidi.com 0x22e9). */
static void d_frequency(int v, unsigned word)
{
    d_a0[v] = (uint8_t)word;
    d_b0[v] = (uint8_t)(word >> 8) | 0x20;
    chip(0xa0 + v, d_a0[v]);
    chip(0xb0 + v, d_b0[v]);
}

/* Note on (original: fmmidi.com 0xf6b): takes a voice, sets the instrument, the levels and the
   frequency, and keys the note on. */
static void d_note_on(int chan, int note, int velocity)
{
    /* Which voice (original: fmmidi.com 0x1ed7): the first free one, else the first voice of the
       lowest channel that never had a bend. */
    int v = -1;
    for (int i = 0; i < 9 && v < 0; i++)
        if (!d_voice_note[i]) v = i;
    for (int c = 0; c < 16 && v < 0; c++) {
        if (d_bent[c]) continue;
        for (int i = 0; i < 9 && v < 0; i++)
            if (d_voice_chan[i] == c) v = i;
    }
    /* Here the original uses the note's number as a voice and writes over its own memory. No song
       gets here, so the note is dropped and counted. */
    if (v < 0) {
        d_dropped++;
        return;
    }

#ifdef FM_HOST
    fm_host_notes++;
#endif
    d_key_off(v);
    d_voice_chan[v] = chan;
    /* Both releases to the fastest, five times over (in effect a delay loop). */
    for (int i = 0; i < 5; i++) {
        chip(0x80 + d_ops[v][1], d_release[d_ops[v][1]] | 0x0f);
        chip(0x80 + d_ops[v][0], d_release[d_ops[v][0]] | 0x0f);
    }
    d_instrument(chan == 9 ? d_drum[note] : d_melodic[d_program[chan] & 127], v);
    d_voice_vel[v] = velocity;
    d_voice_levels(v, d_volume[chan]);

    /* Frequency and key on (original: fmmidi.com 0x2227). A drum plays at the pitch of its
       name record. The key is switched off and on again. */
    unsigned word = d_note_word(chan == 9 ? d_drum_pitch[note] : note);
    d_a0[v] = (uint8_t)word;
    d_b0[v] = (uint8_t)(word >> 8) | 0x20;
    chip(0xa0 + v, d_a0[v]);
    chip(0xb0 + v, d_b0[v] & 0xdf);
    chip(0xb0 + v, d_b0[v]);
    d_voice_note[v] = (uint8_t)note;

    if (chan != 9 && d_bent[chan]) d_frequency(v, d_bend_word((unsigned)d_bend[chan], note, v));
}

/* Handles one MIDI message (original: fmmidi.com 0x80d): note on and off, the controllers 7
   (volume), 0x66 (the bend's range in semitones), 0x79 (reset) and 0x7b (all notes off), program
   change and pitch bend. Modulation, pan, reverb and channel pressure are ignored: the chip is
   mono. */
static void d_message(int status, int a, int b)
{
    int chan = status & 15;
    a &= 127;
    b &= 127;
    switch (status & 0xf0) {
    case 0x90:                                          /* a note on with velocity 0 is a note off */
        if (b) {
            d_note_on(chan, a, b);
            break;
        }
        /* fall through */
    case 0x80:                                          /* note off (original: fmmidi.com 0x157b) */
        for (int v = 0; v < 9; v++)
            if (d_voice_note[v] == a && d_voice_chan[v] == chan) d_key_off(v);
        break;
    case 0xb0:                                          /* controller (original: fmmidi.com 0x1846) */
        if (a == 7) {                                   /* volume (original: fmmidi.com 0x237f) */
            d_volume[chan] = b;
            for (int v = 0; v < 9; v++)
                if (d_voice_note[v] && d_voice_chan[v] == chan) d_voice_levels(v, b);
        } else if (a == 0x66) {
            d_range[chan] = b;
        } else if (a == 0x79) {                         /* reset (original: fmmidi.com 0x174f) */
            /* The original resets the velocity of the voice with the channel's number, not of
               the channel's voices. This slip is kept. */
            d_volume[chan] = 0x7f;
            if (chan < 9) d_voice_vel[chan] = 0x7f;
            d_bend[chan] = 0x40;
            d_range[chan] = 2;
        } else if (a == 0x7b) {                         /* all notes off (original: fmmidi.com 0x16e9) */
            for (int v = 0; v < 9; v++)
                if (d_voice_chan[v] == chan) {
                    d_key_off(v);
                    d_voice_note[v] = 0;
                }
        }
        break;
    case 0xc0:                                          /* program change (original: fmmidi.com 0x17f9) */
        d_program[chan] = a;
        break;
    case 0xe0:                                          /* pitch bend (original: fmmidi.com 0x19bb) */
        /* Only the high byte, b, is used. */
        if (chan == 9) break;
        d_bend[chan] = b;
        d_bent[chan] = 1;
        for (int v = 0; v < 9; v++)
            if (d_voice_note[v] && d_voice_chan[v] == chan)
                d_frequency(v, d_bend_word((unsigned)b, d_voice_note[v], v));
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------- the player */

/*
 * A song is a set of tracks. Each track is a list of events, each with a delay in ticks before
 * it (read_delay) and a MIDI message or a meta event (track_event).
 *   p, end, start  the next byte to read, the end of the track's data, and its first event;
 *   wait           ticks before the next event; it is due when wait is 0 or less;
 *   done           1 when the track has ended;
 *   chan           the track's channel from its header, used for the volume and the resets.
 */
struct track {
    const uint8_t *p, *end, *start;
    int32_t wait;
    int done, chan;
};

static struct track tracks[MAXTRACK];
static int ntrack;
static int song;                            /* the song loaded, 0 none */
static int paused;
/* The tick rate: a tick lasts tick_clock / tick_den chip samples, with tick_clock the chip's
   clock and tick_den 72 x the song's ticks a second. */
static uint32_t tick_clock, tick_den;
/* The next tick's time in chip samples (t_tick), and the fraction of a sample (in tick_den) that
   has been left over, so that no time is lost to rounding (t_frac). */
static uint32_t t_tick, t_frac;
/* The tick being run: the next track to do, and whether any track is still going. They are kept
   between calls, because a tick can stop for room in the queue and go on later. */
static int tr_i, alive;
static int master_v = 0x6000, fade = 0x8000;    /* the game's master volume (fm_master), the fade (fm_fade) */
static int g_master = 96;                   /* the driver's master, 0..127 (original: STARTL.EXE 0xd3a54) */
static uint8_t g_chan_volume[16];           /* controller 7 as the song last gave it (original: 0x4018) */
/* for fm_stats: times the song was behind the clock, frames in which the queue was full */
static uint32_t behind_count, full_frames;

/* Brings now32, the clock as a running count, up to the hardware's 16 bits. The hardware's count
   wraps after 1.3 s, so this is called at least every 0.6 s. */
static void clock_sync(void)
{
    now32 += (uint16_t)(HW_NOW() - (uint16_t)now32);
}

/* Sets the time of the writes that the CPU makes between ticks: just after the clock (100
   samples, 2 ms), but not before what is queued. */
static void stamp_now(void)
{
    clock_sync();
    stamp = now32 + 100;
    if ((int32_t)(stamp - q_last) < 0) stamp = q_last;
}

/* Passes a message on to the driver (original: STARTL.EXE 0xe2ca9). Controller 7, the channel
   volume, is the only message the player changes: it remembers the song's value for the channel
   and gives the driver master x (127 x value >> 7) >> 7. */
static void send(int status, int a, int b)
{
    if ((status & 0xf0) == 0xb0 && a == 7) {
        g_chan_volume[status & 15] = (uint8_t)b;
        b = (g_master * ((127 * b) >> 7)) >> 7;
    }
    d_message(status, a, b);
}

/* Ends a song (original: STARTL.EXE 0xe37eb, which also runs when a song is stopped). Each track
   but the first gets, for the channel in its header, all notes off, reset controllers, the pitch
   bend to the middle and channel volume 0. These go straight to the driver, not through the
   master. */
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

/* Reads a delay in ticks from the track. It is 7 bits a byte, the low part first, and the last
   byte has bit 7 set (the reverse of a MIDI file). */
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

/* Sets every track to its first event, with the delay before it. */
static void song_rewind(void)
{
    for (int i = 0; i < ntrack; i++) {
        struct track *t = &tracks[i];
        t->p = t->start;
        t->done = 0;
        t->wait = read_delay(t);
    }
}

/* Reads one event of track t (after its delay) and handles it. `number` is the track's number.
   - 0xff is a meta event: a type, a length (7 bits a byte, the high part first, bit 7 set means
     more bytes) and the data, which is skipped. Type 0x2f ends the track.
   - A byte under 0x80 is not a message: the track is given up.
   - Otherwise it is a MIDI message: the status byte, one data byte for program change (0xc0) and
     channel pressure (0xd0), else two. It goes to the driver (send), except for track 0, whose
     messages are never sent. */
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

/* 1 if the hardware's queue has room for one message (ROOM places). */
static int room(void)
{
    return HW_FILL() + ROOM <= QUEUE;
}

/* Runs one tick (original: STARTL.EXE 0xe3d51). For each track: the messages whose wait has run
   out are sent, then the wait goes down by one. A track with no more events is done. When all
   tracks are done, the song has ended and the next round begins. If the queue has no room for
   the next message, the tick stops there (tr_i and the tracks' waits keep the place) and goes on
   at the next call. Returns 1 when the tick is done, 0 when it stopped for room. */
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

/* The song file (MUSICn.HMP) starts with "HMIMIDIP013195". At +0x30 it has the number of tracks
   and at +0x38 the ticks a second (both 16 bits). The tracks follow from +0x388. Each track has
   a header of 12 bytes (+4 its length including the header, 32 bits; +8 its channel) and then
   its events. */
int fm_play(int n)
{
    struct dat_entry e;
    char name[12] = { 'M', 'U', 'S', 'I', 'C', (char)('0' + n), '.', 'H', 'M', 'P', 0 };
    fm_stop();
    if (!banks_file || n < 1 || n > FM_SONGS) return 0;
    if (!dat_find(banks_file, banks_size, name, &e) || e.method != 0 || e.offset + e.size > banks_size)
        return 0;
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

    /* Start the chip and the clock. The driver's own set-up writes are queued LEAD / 3 samples
       (10 ms) from now, before the first tick at LEAD. The tick rate defaults to 120 a second
       if the header has none. */
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

/* Called once a frame. Runs every tick that is due within AHEAD samples of the clock, each with
   its own time. If the song's time is more than BEHIND samples behind the clock (the CPU was
   away), it jumps to the clock plus LEAD. If the queue has no room, the tick stops and goes on
   in the next frame. */
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

/* The driver's master: the game's volume times the fade, 0x6000 -> 96. It is limited to 0..127. */
static int master_of(void)
{
    int m = (master_v * fade >> 15) >> 8;               /* at most 0x7fff x 0x8000: no overflow */
    return m < 0 ? 0 : m > 127 ? 127 : m;
}

/* Applies a change of the master (original: STARTL.EXE 0xe29d9). Each track still going gets
   its channel's volume again, through the new master. */
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
