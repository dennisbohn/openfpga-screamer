#include "music.h"
#include "hw.h"
#include "dat.h"
#include "dlog.h"
#include "slot.h"

#define CUE_SLOT    21                      /* SCREAM.DAT, core/data.json */
#define DISC_SLOT   22                      /* SCREAM.gog */
#define RING_MEM    (SDRAM_BASE + 0x1E00000)    /* the ring: 256 KB right after SOUND.DAT's 2 MB (main.c) */
#define CHUNK       0x8000u                 /* a read: 0.19 s of music, 25 ms at the APF's 1.3 MB/s */
#define MARGIN      0x400u                  /* left free behind the mixer's place (it holds 64 bytes) */
#define MASTER      0x4000                  /* the CD master volume, of 0x7fff (music.h) */
#define SECTOR      2352u
#define MAX_TRACKS  99

static int32_t first[MAX_TRACKS + 2];       /* per CD track its first sector (INDEX 01), -1 none */
static uint8_t audio[MAX_TRACKS + 2];       /* 1 for an AUDIO track */
static uint32_t sectors;                    /* the image's */
/* CD_TRACKS: [0] a mode, [1] the track screen's song, [2..7] tracks 1-6'; these are MENU.DTR's,
   kept for a GEN.DAT without it */
static int songs[8] = { 1, 2, 5, 3, 4, 6, 7, 1 };

static int playing;                         /* the CD track playing, 0 none */
static uint32_t from, to, at;               /* its bytes in the image; the next byte to read */
static uint32_t written, played, place;     /* frames into the ring, frames played, the mixer's place */
static uint32_t reading;                    /* bytes of the read running, 0 none */
static uint32_t short_frames;               /* frames in which the mixer had less than 16 frames ahead */

/* the text's next line from *p, without its line break: 0 at the end */
static int line(const char **p, const char *end, const char **s, const char **e)
{
    if (*p >= end) return 0;
    *s = *p;
    while (*p < end && **p != '\n')
        (*p)++;
    *e = *p;
    if (*p < end) (*p)++;
    return 1;
}

/* the line's next token from *p (spaces, tabs and CRs between): its length, 0 at the end */
static uint32_t token(const char **p, const char *end, const char **t)
{
    while (*p < end && (**p == ' ' || **p == '\t' || **p == '\r'))
        (*p)++;
    *t = *p;
    while (*p < end && **p != ' ' && **p != '\t' && **p != '\r')
        (*p)++;
    return (uint32_t)(*p - *t);
}

static int is(const char *t, uint32_t n, const char *word)
{
    uint32_t i = 0;
    while (i < n && word[i] && t[i] == word[i])
        i++;
    return i == n && !word[i];
}

static uint32_t dec(const char *t, uint32_t n)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < n && t[i] >= '0' && t[i] <= '9'; i++)
        v = v * 10 + (uint32_t)(t[i] - '0');
    return v;
}

/* the cue sheet: TRACK nn type, INDEX 01 mm:ss:ff (75 frames, that is sectors, a second) */
static void cue(const char *p, const char *end)
{
    const char *s, *e, *t;
    uint32_t cur = 0;
    while (line(&p, end, &s, &e)) {
        uint32_t n = token(&s, e, &t);
        if (is(t, n, "TRACK")) {
            n = token(&s, e, &t);
            cur = dec(t, n);
            if (cur > MAX_TRACKS) cur = 0;
            n = token(&s, e, &t);
            audio[cur] = (uint8_t)is(t, n, "AUDIO");
        } else if (is(t, n, "INDEX") && cur) {
            n = token(&s, e, &t);
            if (dec(t, n) != 1) continue;
            n = token(&s, e, &t);
            uint32_t f[3] = { 0, 0, 0 }, k = 0;
            for (uint32_t i = 0; i < n && k < 3; i++) {
                if (t[i] == ':') k++;
                else f[k] = f[k] * 10 + (uint32_t)(t[i] - '0');
            }
            first[cur] = (int32_t)((f[0] * 60 + f[1]) * 75 + f[2]);
        }
    }
}

/* CD_TRACKS of GEN.DAT's MENU.DTR: eight numbers dN */
static void cd_tracks(const uint8_t *gen, uint32_t gen_size)
{
    static uint8_t text[1024];
    struct dat_entry e;
    if (!gen || !dat_find(gen, gen_size, "MENU.DTR", &e) || e.size > sizeof text) return;
    uint32_t size = dat_extract(gen, gen_size, &e, text, sizeof text);
    const char *p = (const char *)text, *end = p + size, *s, *l, *t;
    int in = 0, k = 0, v[8];
    while (line(&p, end, &s, &l)) {
        uint32_t n = token(&s, l, &t);
        if (!in) {
            in = is(t, n, "CD_TRACKS");
        } else if (is(t, n, "END_LIST")) {
            break;
        } else if (n > 1 && t[0] == 'd' && k < 8) {
            v[k++] = (int)dec(t + 1, n - 1);
        }
    }
    if (k == 8)
        for (int i = 0; i < 8; i++)
            songs[i] = v[i];
}

int music_init(const uint8_t *gen, uint32_t gen_size)
{
    uint32_t size = slot_size(CUE_SLOT);
    int n = 0;
    sectors = slot_size(DISC_SLOT) / SECTOR;
    for (int i = 0; i <= MAX_TRACKS; i++) {
        first[i] = -1;
        audio[i] = 0;
    }
    cd_tracks(gen, gen_size);
    if (size && size <= 4096 && sectors && slot_read(CUE_SLOT, 0, RING_MEM, SLOT_REST) == 0) {
        cue((const char *)RING_MEM, (const char *)RING_MEM + size);
        for (int i = 1; i <= MAX_TRACKS; i++)
            n += audio[i] && first[i] >= 0 && (uint32_t)first[i] < sectors;
    }
    if (!n) dlog_str("music: no audio tracks (SCREAM.gog, SCREAM.DAT)\n");     /* (the log has 4 KB) */
    return n;
}

/* the next read into the ring, if there is room for it: its bytes, else 0 */
static uint32_t next(void)
{
    uint32_t off = written * 4 % AU_MUSIC_RING, len = CHUNK;
    uint32_t room = AU_MUSIC_RING - MARGIN - (written - played) * 4;
    if (len > AU_MUSIC_RING - off) len = AU_MUSIC_RING - off;
    if (len > to - at) len = to - at;
    return len <= room ? len : 0;
}

/* a read of len bytes is done, with the APF's result r */
static void done(uint32_t len, int r)
{
    if (r) {
        AU_MUSIC = 0;
        playing = 0;
        dlog_str("music: a read of SCREAM.gog failed (result ");
        dlog_dec(r);
        dlog_str("), it stops\n");
        return;
    }
    written += len / 4;
    at += len;
    if (at == to) at = from;                /* the song again, as the drive's poll plays it */
}

void music_play(int k)
{
    int n = k >= 0 && k <= 6 ? songs[k + 1] + 1 : 0, m = n + 1;
    if (n == playing) return;
    music_stop();
    if (n < 1 || n > MAX_TRACKS || !audio[n] || first[n] < 0 || (uint32_t)first[n] >= sectors) return;
    while (m <= MAX_TRACKS && first[m] < 0)
        m++;
    uint32_t last = m <= MAX_TRACKS && (uint32_t)first[m] < sectors ? (uint32_t)first[m] : sectors;
    from = at = (uint32_t)first[n] * SECTOR;
    to = last * SECTOR;
    written = played = place = short_frames = 0;
    playing = n;
    for (uint32_t len; playing && (len = next()) != 0;)
        done(len, slot_read(DISC_SLOT, at, RING_MEM + written * 4 % AU_MUSIC_RING, len));
    if (!playing) return;
    AU_MUSIC_ADDR = RING_MEM - SDRAM_BASE;
    AU_MUSIC_VOL = MASTER;
    AU_MUSIC = 1;
}

void music_frame(void)
{
    if (!playing) return;
    uint32_t p = AU_MUSIC & 0xFFFF;
    played += (p - place) & 0xFFFF;
    place = p;
    if (reading) {
        if (slot_busy()) return;
        uint32_t len = reading;
        reading = 0;
        done(len, slot_result());
        if (!playing) return;
    }
    short_frames += (int32_t)(written - played) < 16;
    uint32_t len = next();
    if (len) {
        slot_start(DISC_SLOT, at, RING_MEM + written * 4 % AU_MUSIC_RING, len);
        reading = len;
    }
}

void music_stop(void)
{
    AU_MUSIC = 0;
    if (reading) {
        uint32_t t0 = rdcycle();
        while (slot_busy() && rdcycle() - t0 < 20u * CPU_HZ)
            ;
        reading = 0;
    }
    if (playing && short_frames) {
        dlog_str("music: CD track ");
        dlog_dec(playing);
        dlog_str(" ran short of data in ");
        dlog_dec((int32_t)short_frames);
        dlog_str(" frames\n");
    }
    playing = 0;
}
