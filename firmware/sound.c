/*
 * Sound effects (interface and volume rules: sound.h). This file has three parts:
 *   - the effect table: rows read from DTR texts (lists of effects in GEN.DAT and the track's
 *     MAPnDAT.DTR), each pointing at a sample inside SOUND.DAT, which stays in the SDRAM;
 *   - the voices: snd_play and the calls after it write the mixer's registers (hw.h AU_*);
 *   - the ear: sounds with a place get volume, pan and pitch from the camera (snd_at).
 */
#include "sound.h"
#include <math.h>
#include "hw.h"
#include "dat.h"
#include "track.h"

#define SND_MAX     72                  /* the 12 effects, the 58 recordings, a track's 2 */
#define MASTER      0x4000              /* the effects' master volume; the game's is 0x6000 (sound.h) */
#define SPEECH_MASTER 0x3000            /* the commentary's master volume (sound.h) */

/* One row of the effect table, all values ready for the mixer:
   addr, len   SDRAM byte address and length of the sample;
   end         where the sample ends, or where its loop ends (bytes from the start);
   back, times at the end, jump back by this many bytes, so often (AU_LOOP);
   vol         the row's volume, 0..0x7fff. */
static struct snd { uint32_t addr, len, end, back, times, vol; } fx[SND_MAX];
static int nfx;
static int speech_end;                  /* the rows from SND_SPEECH up to here are the commentary */
static const uint8_t *dat;              /* SOUND.DAT */
static uint32_t dat_size;

/* Per voice:
   serial   counts the starts on the voice, so that a handle names one start only;
   voice_fx the effect it plays;
   voice_vol, voice_pan  its own loudness and pan (snd_volume). */
static uint32_t serial[AU_VOICES];
static int voice_fx[AU_VOICES];
static int32_t voice_vol[AU_VOICES], voice_pan[AU_VOICES];
static int32_t fade = 0x8000;           /* snd_fade: every voice is at this of 0x8000 */
static uint32_t level = 0x8000;         /* snd_master: the game's digital master; 0x8000 at its 0x6000 */

/* The next token of the text, from *p to the end of the line; spaces and tabs before it are
   skipped. Sets *tok to its start and moves *p behind it. Returns its length. */
static uint32_t token(const char **p, const char *end, const char **tok)
{
    const char *s = *p;
    while (s < end && (*s == ' ' || *s == '\t'))
        s++;
    *tok = s;
    while (s < end && *s != ' ' && *s != '\t' && *s != '\r')
        s++;
    *p = s;
    return (uint32_t)(s - *tok);
}

/* Value of a field of a row: 'x' and hex digits, or 'd' and a decimal number (maybe negative). */
static int32_t field(const char *t, uint32_t n)
{
    uint32_t v = 0, i = 1;
    int neg = 0;
    if (n && t[0] == 'x') {
        for (; i < n; i++) {
            int c = t[i], d = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
            v = v * 16 + (uint32_t)d;
        }
        return (int32_t)v;
    }
    if (i < n && t[i] == '-') {
        neg = 1;
        i++;
    }
    for (; i < n; i++)
        v = v * 10 + (uint32_t)(t[i] - '0');
    return neg ? -(int32_t)v : (int32_t)v;
}

/* Adds the next row of the table: the sample NAME.RAW in SOUND.DAT and its loop, as the game's
   driver plays it. f[0..3] are the row's volume, loop count, loop start and loop length.
   With a loop count and a loop length, the sample plays from its start to the loop's end and
   then jumps back by the loop length, so often. Otherwise it plays to its end; with a loop count
   but a loop length of 0 the whole sample loops. If the sample is missing, the row gets
   len = 0 and snd_play refuses it. */
static void row(const char *name, uint32_t n, const int32_t f[6])
{
    char file[24];
    struct dat_entry e;
    struct snd *s = &fx[nfx++];
    s->len = 0;
    if (n > 15) return;
    for (uint32_t i = 0; i < n; i++)
        file[i] = name[i];
    const char *ext = ".RAW";
    for (int i = 0; i < 5; i++)
        file[n + (uint32_t)i] = ext[i];
    if (!dat_find(dat, dat_size, file, &e) || e.method != 0 || e.size == 0 || e.offset > dat_size ||
        e.size > dat_size - e.offset)
        return;
    int32_t loop = f[1], ls = f[2], ll = f[3];
    uint32_t end = loop != 0 && ll > 0 ? (uint32_t)(ls + ll) : e.size, start = ll > 0 ? (uint32_t)ls : 0;
    if (end > e.size) end = e.size;
    if (start >= end) start = 0;
    s->addr = (uint32_t)(uintptr_t)(dat + e.offset) - SDRAM_BASE;
    s->len = e.size;
    s->end = end;
    s->back = end - start;
    s->times = loop < 0 ? 255 : loop > 254 ? 254 : (uint32_t)loop;
    s->vol = (uint32_t)f[0] & 0x7FFF;
}

/* Appends the rows of the first SOUND_FX section of a DTR text to the table. A row is a line
   "sNAME" and six fields; the section ends at END_LIST. Returns how many of the new rows found
   their sample. */
static int list(const char *p, const char *end)
{
    int in = 0, ok = 0;
    while (p < end && nfx < SND_MAX) {
        const char *line_end = p, *t;
        while (line_end < end && *line_end != '\n')
            line_end++;
        uint32_t n = token(&p, line_end, &t);
        if (!in) {
            in = n == 8 && !__builtin_memcmp(t, "SOUND_FX", 8);
        } else if (n == 8 && !__builtin_memcmp(t, "END_LIST", 8)) {
            break;
        } else if (n > 1 && t[0] == 's') {
            int32_t f[6];
            const char *name = t + 1, *u;
            int k = 0;
            uint32_t m;
            while (k < 6 && (m = token(&p, line_end, &u)) != 0)
                f[k++] = field(u, m);
            if (k == 6) {
                row(name, n - 1, f);
                ok += fx[nfx - 1].len != 0;
            }
        }
        p = line_end + 1;
    }
    return ok;
}

/* Appends the rows of the DTR text `name` in GEN.DAT to the table (see list). Returns how many
   of the new rows found their sample. */
static int gen_list(const uint8_t *gen, uint32_t gen_size, const char *name)
{
    static uint8_t text[2560];          /* the longest text, SPFXFULL.DTR, has 2356 bytes */
    struct dat_entry e;
    if (!dat_find(gen, gen_size, name, &e) || e.size >= sizeof text) return 0;
    uint32_t size = dat_extract(gen, gen_size, &e, text, sizeof text);
    return list((const char *)text, (const char *)text + size);
}

/* Stops every voice, empties the table and takes this SOUND.DAT. The commentary has no rows yet. */
static void restart(const uint8_t *sound, uint32_t sound_size)
{
    AU_STOP = 0xFFFFFFFFu;
    nfx = speech_end = 0;
    dat = sound;
    dat_size = sound_size;
}

int snd_init(const uint8_t *sound, uint32_t sound_size, const uint8_t *gen, uint32_t gen_size)
{
    restart(sound, sound_size);
    int n = gen_list(gen, gen_size, "SFXSBASE.DTR");
    if (nfx == SND_SPEECH) {            /* the commentary's numbers must start at SND_SPEECH */
        n += gen_list(gen, gen_size, "SPFXBASE.DTR");
        n += gen_list(gen, gen_size, "SPFXFULL.DTR");
    }
    speech_end = nfx;
    return n;
}

int snd_menu(const uint8_t *sound, uint32_t sound_size, const uint8_t *gen, uint32_t gen_size)
{
    restart(sound, sound_size);
    return gen_list(gen, gen_size, "MENU.DTR");
}

int snd_add_list(const uint8_t *text, uint32_t size)
{
    int first = nfx;
    if (!dat || !list((const char *)text, (const char *)text + size)) return -1;
    return first;
}

/* A handle is the voice number in its low 5 bits and the voice's start counter (serial) above
   them. Returns the voice of handle h while it plays that start, else -1. */
static int voice_of(int h)
{
    if (h < 0) return -1;
    int v = h & (AU_VOICES - 1);
    return serial[v] == (uint32_t)h >> 5 && (AU_PLAY & 1u << v) ? v : -1;
}

/* The value for AU_VOL: the left side in bits 7..0, the right side in bits 15..8, each a byte.
   A voice in the middle at full volume gets 127 per side while snd_master is at the game's
   0x6000. The loudness t is the row's volume times the voice's own, times the master volume
   (the effects' or the commentary's), times snd_fade and snd_master; after each product the
   result is shifted right by 15 bits. The pan then fades the far side (0 is full left, 0xffff
   full right). */
static uint32_t vol_reg(const struct snd *s, int32_t vol, int32_t pan)
{
    int id = (int)(s - fx);
    uint32_t master = id >= SND_SPEECH && id < speech_end ? SPEECH_MASTER : MASTER;
    uint32_t t = (((s->vol * (uint32_t)vol) >> 15) * master >> 15) * ((uint32_t)fade * level >> 15) >> 15;
    uint32_t fl = pan <= 0x8000 ? 0x7FFF : (uint32_t)(0xFFFF - pan),
             fr = pan >= 0x8000 ? 0x7FFF : (uint32_t)pan;
    return (t * fl >> 22) | (t * fr >> 22) << 8;
}

/* Starts (or restarts) effect id on voice v: its sample, loop and volume. */
static void start(int v, int id)
{
    const struct snd *s = &fx[id];
    AU_ADDR(v) = s->addr;
    AU_END(v) = s->addr + s->end;
    AU_LOOP(v) = s->times << 24 | s->back;
    AU_VOL(v) = vol_reg(s, voice_vol[v], voice_pan[v]);
    AU_PLAY = 1u << v;
    voice_fx[v] = id;
}

int snd_play(int id)
{
    if (id < 0 || id >= nfx || !fx[id].len) return -1;
    uint32_t free = ~AU_PLAY;
    if (!free) return -1;
    int v = __builtin_ctz(free);
    voice_vol[v] = 0x7FFF;
    voice_pan[v] = 0x8000;
    AU_STEP(v) = 0x10000u * 147 / 640;
    start(v, id);
    return (int)(++serial[v] << 5 | (uint32_t)v);
}

int snd_stop(int h)
{
    int v = voice_of(h);
    if (v >= 0) AU_STOP = 1u << v;
    return -1;
}

int snd_playing(int h)
{
    return voice_of(h) >= 0;
}

/* The mixer's step per output frame (AU_STEP, 2.16) is the pitch (16.16) x 11025 / 48000, that
   is pitch x 147 / 640. The pitch is limited to 1..0xfffff so that the step stays below 4
   samples. */
void snd_pitch(int h, int32_t pitch)
{
    int v = voice_of(h);
    if (v < 0) return;
    if (pitch < 1) pitch = 1;
    if (pitch > 0xFFFFF) pitch = 0xFFFFF;
    AU_STEP(v) = (uint32_t)pitch * 147 / 640;
}

void snd_volume(int h, int32_t vol, int32_t pan)
{
    int v = voice_of(h);
    if (v < 0) return;
    voice_vol[v] = vol < 0 ? 0 : vol > 0x7FFF ? 0x7FFF : vol;
    voice_pan[v] = pan < 0 ? 0 : pan > 0xFFFF ? 0xFFFF : pan;
    AU_VOL(v) = vol_reg(&fx[voice_fx[v]], voice_vol[v], voice_pan[v]);
}

void snd_swap(int h, int id)
{
    int v = voice_of(h);
    if (v < 0 || id < 0 || id >= nfx || !fx[id].len || voice_fx[v] == id) return;
    AU_STOP = 1u << v;
    start(v, id);
}

void snd_fade(int32_t f)
{
    fade = f < 0 ? 0 : f > 0x8000 ? 0x8000 : f;
    for(int v = 0; v < AU_VOICES; v++)
        AU_VOL(v) = vol_reg(&fx[voice_fx[v]], voice_vol[v], voice_pan[v]);
}

void snd_master(int32_t v)
{
    level = (uint32_t)(v < 0 ? 0 : v > 0x7FFF ? 0x7FFF : v) * 4 / 3;
    snd_fade(fade);
}

int snd_play_once(int id, int h)
{
    int v = voice_of(h);
    return v >= 0 && voice_fx[v] == id ? h : snd_play(id);
}

/* The ear, set by snd_ear: where the sounds with a place are heard from.
   ear_x, ear_y   the camera's position;
   ear_rx, ear_ry the camera's right direction, level (track_cam_axes);
   ear_ok         1 once snd_ear has been called;
   ear_cut        1 if the camera jumped in the last step (the first call counts as a jump). */
static float ear_x, ear_y, ear_rx, ear_ry;
static int ear_ok, ear_cut;

void snd_ear(const struct track_cam *cam)
{
    float dx = cam->x - ear_x, dy = cam->y - ear_y;
    ear_cut = !ear_ok || dx * dx + dy * dy > 1000.0f * 1000.0f;
    ear_ok = 1;
    ear_x = cam->x;
    ear_y = cam->y;
    ear_rx = -cosf(cam->yaw);
    ear_ry = -sinf(cam->yaw);
}

float snd_dist(float x, float y)
{
    float dx = x - ear_x, dy = y - ear_y;
    return sqrtf(dx * dx + dy * dy);
}

/* Plays or stops the sound of a source at (x, y); the arguments are described in sound.h.
   The pan is the line to the source in the ear's frame: 0x8000 +- 32767 x sqrt(|sideways| /
   distance) (original: STARTL.EXE 0x21cfb, 0x1dffc). The Doppler shift is the game's: 9 per
   unit of approach in a frame of a 30 fps PC. Here a step is 1/50 s, so it is multiplied by
   50 / 30. A camera cut sets *last to the new distance, so that step has no shift. */
int snd_at(int h, int id, float x, float y, float range, float loud, float pitch, float *last)
{
    float dx = x - ear_x, dy = y - ear_y, d = sqrtf(dx * dx + dy * dy);
    if (d >= range) {
        *last = d;
        return snd_stop(h);
    }
    if (!snd_playing(h)) {
        h = snd_play(id);
        *last = d;
    }
    if (ear_cut) *last = d;
    float q = (range - d) / range, side = d >= 1.0f ? (dx * ear_rx + dy * ear_ry) / d : 0.0f;
    float s = 32767.0f * sqrtf(fabsf(side));
    snd_volume(h, (int32_t)(q * q * loud), 0x8000 + (int32_t)(side < 0.0f ? -s : s));
    snd_pitch(h, (int32_t)(pitch + 9.0f * (*last - d) * (50.0f / 30.0f)));
    *last = d;
    return h;
}
