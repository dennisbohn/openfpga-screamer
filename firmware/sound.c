#include "sound.h"
#include <math.h>
#include "hw.h"
#include "dat.h"
#include "track.h"

#define SND_MAX     72                  /* the 12 effects, the 58 recordings, a track's 2 */
#define MASTER      0x4000              /* the effects' master volume (sound.h) */
#define SPEECH_MASTER 0x3000            /* the commentary's (sound.h) */

/* a row: SDRAM byte address and length, where it ends or loops (from the start), back by how
   much and how often (AU_LOOP), its volume */
static struct snd { uint32_t addr, len, end, back, times, vol; } fx[SND_MAX];
static int nfx;
static int speech_end;                  /* the rows from SND_SPEECH up to here are the commentary */
static const uint8_t *dat;             /* SOUND.DAT */
static uint32_t dat_size;

/* per voice: which start of it a handle names, its effect, loudness and pan */
static uint32_t serial[AU_VOICES];
static int voice_fx[AU_VOICES];
static int32_t voice_vol[AU_VOICES], voice_pan[AU_VOICES];
static int32_t fade = 0x8000;           /* snd_fade: all of them at this of 0x8000 */

/* the next token of the text from *p up to the line's end (spaces and tabs between): its length */
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

/* a field: x and hex digits, or d and a decimal number (maybe negative) */
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

/* row k: its sample NAME.RAW in SOUND.DAT and the loop as the game's driver plays it: with
   loops and a loop length, from the start to the loop's end, then the loop; else to the end
   (a loop length of 0 loops the whole sample) */
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

/* the rows of the first SOUND_FX section of a DTR text after the ones there are: how many have
   their sample */
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

/* the rows of GEN.DAT's list name after the ones there are: how many have their sample */
static int gen_list(const uint8_t *gen, uint32_t gen_size, const char *name)
{
    static uint8_t text[2560];          /* the longest, SPFXFULL.DTR, has 2356 bytes */
    struct dat_entry e;
    if (!dat_find(gen, gen_size, name, &e) || e.size >= sizeof text) return 0;
    uint32_t size = dat_extract(gen, gen_size, &e, text, sizeof text);
    return list((const char *)text, (const char *)text + size);
}

int snd_init(const uint8_t *sound, uint32_t sound_size, const uint8_t *gen, uint32_t gen_size)
{
    AU_STOP = 0xFFFFFFFFu;
    nfx = 0;
    dat = sound;
    dat_size = sound_size;
    int n = gen_list(gen, gen_size, "SFXSBASE.DTR");
    if (nfx == SND_SPEECH) {
        n += gen_list(gen, gen_size, "SPFXBASE.DTR");
        n += gen_list(gen, gen_size, "SPFXFULL.DTR");
    }
    speech_end = nfx;
    return n;
}

int snd_add_list(const uint8_t *text, uint32_t size)
{
    int first = nfx;
    if (!dat || !list((const char *)text, (const char *)text + size)) return -1;
    return first;
}

/* the voice of handle h while it plays that start, else -1 */
static int voice_of(int h)
{
    if (h < 0) return -1;
    int v = h & (AU_VOICES - 1);
    return serial[v] == (uint32_t)h >> 5 && (AU_PLAY & 1u << v) ? v : -1;
}

/* AU_VOL from the row's volume, the voice's own and its pan, times the master volume (the
   effects' or the commentary's): a centred byte times up to 127 */
static uint32_t vol_reg(const struct snd *s, int32_t vol, int32_t pan)
{
    int id = (int)(s - fx);
    uint32_t master = id >= SND_SPEECH && id < speech_end ? SPEECH_MASTER : MASTER;
    uint32_t t = (((s->vol * (uint32_t)vol) >> 15) * master >> 15) * (uint32_t)fade >> 15;
    uint32_t fl = pan <= 0x8000 ? 0x7FFF : (uint32_t)(0xFFFF - pan), fr = pan >= 0x8000 ? 0x7FFF : (uint32_t)pan;
    return (t * fl >> 22) | (t * fr >> 22) << 8;
}

/* the effect's sample and loop on voice v, (re)started */
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

/* the step per output frame: pitch x 11025 / 48000 = pitch x 147 / 640, below 4 samples */
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

int snd_play_once(int id, int h)
{
    int v = voice_of(h);
    return v >= 0 && voice_fx[v] == id ? h : snd_play(id);
}

/* the ear (snd_ear): where the sounds with a place are heard from, the camera's level right
   (track_cam_axes), and whether it has just jumped */
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

/* the pan (0x21cfb, 0x1dffc): the line to the source in the ear's frame, 0x8000 +- 32767 x
   sqrt(|sideways| / distance); the Doppler shift is the game's per frame of a 30 fps PC, here
   per step of 1/50 s, so x 50 / 30 */
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
