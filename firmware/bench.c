/*
 * The benchmark's result lines (bench.h).
 */
#include "bench.h"
#include "console.h"
#include "dlog.h"
#include "ftime.h"

static void log_ms(uint32_t cycles)
{
    char b[16];
    ft_ms(b, cycles);
    dlog_str(b);
}

static void log_scale(float v)
{
    char b[16];
    ft_scale(b, v);
    dlog_str(b);
}

void bench_log_row(int track, const struct ft_summary *s, float far_scale, float lod_scale)
{
    dlog_str("bench track ");
    dlog_dec(track);
    dlog_str(": far ");
    log_scale(far_scale);
    dlog_str(" lod ");
    log_scale(lod_scale);
    dlog_str(", ");
    dlog_dec((int32_t)s->frames);
    dlog_str(" frames avg ");
    log_ms(s->avg);
    dlog_str(" max ");
    log_ms(s->max);
    dlog_str(" late ");
    dlog_dec((int32_t)s->late);
    dlog_str(" faces ");
    dlog_dec((int32_t)s->faces_avg);
    dlog_str(" / ");
    dlog_dec((int32_t)s->faces_worst);
    dlog_str(" pieces ");
    dlog_dec((int32_t)s->pieces_avg);
    dlog_str(" / ");
    dlog_dec((int32_t)s->pieces_worst);
    dlog_str("\n");
}

/* v right-aligned in width w at p (a longer one is written whole) */
static char *pad_udec(char *p, uint32_t v, int w)
{
    char b[12];
    char *e = fmt_udec(b, v);
    for (int n = (int)(e - b); n < w; n++) *p++ = ' ';
    for (char *q = b; q < e; q++) *p++ = *q;
    *p = 0;
    return p;
}

static char *pad_ms(char *p, uint32_t cycles, int w)
{
    char b[16];
    char *e = ft_ms(b, cycles);
    for (int n = (int)(e - b); n < w; n++) *p++ = ' ';
    for (char *q = b; q < e; q++) *p++ = *q;
    *p = 0;
    return p;
}

void bench_screen_row(char *line, int track, const struct ft_summary *s)
{
    char *p = pad_udec(line, (uint32_t)track, 1);
    if (!s || !s->frames) {
        fmt_str(p, "  -");
        return;
    }
    p = pad_ms(p, s->avg, 6);
    p = pad_ms(p, s->max, 6);
    p = pad_udec(p, s->late, 5);
    p = pad_udec(p, s->faces_avg, 7);
    pad_udec(p, s->pieces_avg, 7);
}

void bench_log(const struct ft_summary *rows, const int *ok, float far_scale, float lod_scale)
{
    dlog_str("bench: ");
    dlog_dec(BENCH_FRAMES);
    dlog_str(" frames a track, gas held, ten cars; faces and pieces: mean per frame / the longest frame's\n");
    for (int t = 0; t < BENCH_TRACKS; t++) {
        if (ok[t]) {
            bench_log_row(t + 1, &rows[t], far_scale, lod_scale);
        } else {
            dlog_str("bench track ");
            dlog_dec(t + 1);
            dlog_str(": could not be set up\n");
        }
    }
}
