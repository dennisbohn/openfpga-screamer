/*
 * The FM music's sequencer and driver (firmware/fm.c) on the PC, against a model of the
 * hardware's queue (core/src/fpga/core/opl.v): plays MUSICn.HMP of a MUSIC.DAT for some seconds
 * with the firmware calling fm_frame() every 20 ms, and
 *   trace:  prints every write the firmware queues:  tick register value   (tools/fmcheck.sh
 *           compares it with openscreamer's driver)
 *   else:   a summary: the song's rounds and notes, the writes, how late the model releases them
 *           (the queue spaces them 129 chip clocks apart) and how true the ticks' times are.
 * Usage: fmdump <MUSIC.DAT> <song 1..7> <seconds> [trace|events]
 *   events: every write with its time in chip samples from the song's first tick (sim/opl/tb.cpp plays
 *   them on the real chip into a WAV file)
 *   FMJITTER=1: the frames come 20 to 45 ms apart instead of 20 ms.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/fm.h"

uint32_t fm_host_tick, fm_host_rounds, fm_host_notes;

/* the model: time in cens (chip clocks), 72 to a sample */
#define CEN_PER_SAMPLE 72
#define SPACE_CENS     129
#define QUEUE_LEN      256
static uint64_t cens;                       /* the clock */
static int run;
static struct { uint32_t word; double due; } q[QUEUE_LEN];
static int head, fill;
static uint64_t next_free;
static int tracing;                         /* 1 trace: tick, register, value; 2 events: time in samples, register, value */
static double song_start;
static long writes, released, late_1ms, late_8ms, dropped_full, max_fill, pushed_not_run;
static double max_late, sum_late;
static double first_due = -1, last_due;
static uint32_t last_tick_pushed;

static double samples(void) { return (double)cens / CEN_PER_SAMPLE; }

uint32_t fm_host_now(void) { return (uint32_t)(cens / CEN_PER_SAMPLE) & 0xFFFF; }
uint32_t fm_host_fill(void) { return (uint32_t)fill; }
void fm_host_gain(uint32_t g) { (void)g; }

void fm_host_ctrl(uint32_t r)
{
    run = r & 1;
    if (!run) {
        head = fill = 0;
    }
}

void fm_host_put(uint32_t word)
{
    if (!run) {
        pushed_not_run++;
        return;
    }
    if (fill >= QUEUE_LEN) {
        dropped_full++;
        return;
    }
    uint32_t now16 = fm_host_now();
    int16_t ahead = (int16_t)((word >> 16) - now16);
    double due = samples() + ahead;
    q[(head + fill) % QUEUE_LEN].word = word;
    q[(head + fill) % QUEUE_LEN].due = due;
    fill++;
    if (fill > max_fill) max_fill = fill;
    writes++;
    if (first_due < 0) first_due = due;
    last_due = due;
    last_tick_pushed = fm_host_tick;
    if (tracing == 1) printf("%u %02x %02x\n", fm_host_tick, (word >> 8) & 0xFF, word & 0xFF);
    if (tracing == 2) printf("%.1f %02x %02x\n", due - song_start, (word >> 8) & 0xFF, word & 0xFF);
}

/* the queue's hardware up to time `to` (in cens): the first write is made when its time has come
   and 129 cens after the one before */
static void advance(uint64_t to)
{
    while (cens < to) {
        if (!run || fill == 0) {                                /* nothing to release: to the end */
            cens = to;
            break;
        }
        if (cens >= next_free) {                                /* idle until the first write is due */
            int16_t ahead = (int16_t)((q[head].word >> 16) - (uint16_t)(cens / CEN_PER_SAMPLE));
            if (ahead > 1) {
                uint64_t until = (cens / CEN_PER_SAMPLE + (uint64_t)(ahead - 1)) * CEN_PER_SAMPLE;
                cens = until < to ? until : to;
                continue;
            }
        }
        cens++;
        if (run && fill > 0) {
            uint16_t now16 = (uint16_t)(cens / CEN_PER_SAMPLE);
            int16_t late = (int16_t)(now16 - (q[head].word >> 16));
            if (late >= 0 && cens >= next_free) {
                double rel = samples();
                double l = rel - q[head].due;
                released++;
                sum_late += l;
                if (l > max_late) max_late = l;
                if (l > 50) late_1ms++;
                if (l > 400) late_8ms++;
                next_free = cens + SPACE_CENS;
                head = (head + 1) % QUEUE_LEN;
                fill--;
            }
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: fmdump <MUSIC.DAT> <song 1..7> <seconds> [trace]\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc((size_t)n);
    if (fread(file, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    int song = atoi(argv[2]);
    double seconds = atof(argv[3]);
    tracing = argc > 4 ? !strcmp(argv[4], "trace") ? 1 : !strcmp(argv[4], "events") ? 2 : 0 : 0;
    int jitter = getenv("FMJITTER") != NULL;

    if (!fm_init(file, (uint32_t)n)) {
        fprintf(stderr, "fm_init: no banks and songs in %s\n", argv[1]);
        return 1;
    }
    cens = (uint64_t)(12345.6 * CEN_PER_SAMPLE);                /* (not at zero: the 16 bits count on) */
    double t0 = samples();
    song_start = t0 + 1500;                                     /* (the first tick's time, fm.c LEAD) */
    if (!fm_play(song)) {
        fprintf(stderr, "fm_play(%d) failed\n", song);
        return 1;
    }
    double frame_samples = 3579545.0 / 72 / 50;
    double t = t0, end = t0 + seconds * 3579545.0 / 72;
    uint32_t rng = 12345;
    long frames = 0;
    while (t < end) {
        fm_frame();
        frames++;
        double dt = frame_samples;
        if (jitter) {
            rng = rng * 1664525u + 1013904223u;
            dt = frame_samples * (1.0 + (rng >> 16) % 1000 / 1000.0 * 1.25);
        }
        t += dt;
        advance((uint64_t)(t * CEN_PER_SAMPLE));
    }
    if (tracing) {
        if (tracing == 1) fprintf(stderr, "%u\n", fm_host_tick);
        return 0;
    }

    uint32_t lost, full, behind;
    fm_stats(&lost, &full, &behind);
    printf("song %d: %.1f s, %ld frames, %u ticks run, %u rounds, %u notes, %u without a voice\n",
           song, seconds, frames, fm_host_tick, fm_host_rounds, fm_host_notes, lost);
    printf("  writes %ld (%.1f a tick), released %ld, dropped (full) %ld, queue at most %ld of %d\n",
           writes, (double)writes / (fm_host_tick ? fm_host_tick : 1), released, dropped_full, max_fill, QUEUE_LEN);
    printf("  release lateness: mean %.1f samples, max %.1f (%.2f ms), over 1 ms %ld, over 8 ms %ld; "
           "frames without room %u, times behind %u\n",
           released ? sum_late / (double)released : 0.0, max_late, max_late / 49.7159, late_1ms, late_8ms, full, behind);
    /* the ticks' times: the last write queued is of tick `last_tick_pushed` or of the one before;
       it should be due at start + tick x 3579545 / 8640 (120 ticks a second) samples after the start */
    double expect = (t0 + 1500) + (double)last_tick_pushed * 3579545.0 / 8640.0;
    printf("  last write queued: tick %u due at %.1f, ideal %.1f (error %.1f samples)\n",
           last_tick_pushed, last_due - t0, expect - t0, last_due - expect);
    return 0;
}
