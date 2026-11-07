/* Host test of the benchmark's result lines (firmware/bench.c). It checks, for a known summary:
   the debug.txt line and the screen's row, the row of a track that could not be set up, and that the
   whole log has a line for each of the six tracks. Stubs stand in for the console and the log.
   Build in tools/perf: gcc -Wall -Wextra -DDEBUG=1 -I../../firmware bench_test.c -o bench_test -lm
   The program exits with 1 if a check fails. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static char logbuf[4096];
void dlog_str(const char *s) { strcat(logbuf, s); }
void dlog_dec(int32_t v) { char b[16]; sprintf(b, "%d", v); strcat(logbuf, b); }
/* The real fmt_str, fmt_udec and fmt_scale are used; the text drawing is a stub. */
#define con_text real_con_text
#include "../../firmware/console.c"
#undef con_text
void dlog_scale(float v) { char b[16]; fmt_scale(b, v); strcat(logbuf, b); }
int con_text(volatile uint8_t *fb, int col, int row, const char *s, uint8_t fg, uint8_t bg)
{
    (void)fb; (void)col; (void)row; (void)s; (void)fg; (void)bg;
    return 0;
}
#include "../../firmware/ftime.c"
#include "../../firmware/bench.c"

static int fails;

static void same(const char *what, const char *got, const char *want)
{
    int ok = strcmp(got, want) == 0;
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) printf("  got  \"%s\"\n  want \"%s\"\n", got, want);
    fails += !ok;
}

int main(void)
{
    struct ft_summary s = { 250, 1232550, 1603800, 2, 1730, 1961, 340, 377 };
    char line[64];
    bench_log_row(1, &s, 2.0f, 2.0f);
    same("log row", logbuf, "bench track 1: far 2.00 lod 2.00, 250 frames avg 16.6 max 21.6 late 2 faces 1730 / 1961 pieces 340 / 377\n");
    bench_screen_row(line, 1, &s);
    same("screen row", line, "1  16.6  21.6    2   1730    340");
    same("header and row have the same width", strlen(line) == strlen(BENCH_SCREEN_HEAD) ? "same" : "differ", "same");
    bench_screen_row(line, 4, 0);
    same("a track that did not run", line, "4  -");
    struct ft_summary rows[BENCH_TRACKS];
    int ok[BENCH_TRACKS] = { 1, 1, 0, 1, 1, 1 };
    for (int i = 0; i < BENCH_TRACKS; i++) rows[i] = s;
    logbuf[0] = 0;
    bench_log(rows, ok, 1.5f, 0.0f);
    int lines = 0;
    for (const char *p = logbuf; *p; p++) lines += *p == '\n';
    same("the whole log has a header and a line per track", lines == 1 + BENCH_TRACKS ? "7 lines" : "other", "7 lines");
    same("a track off, lod off", strstr(logbuf, "bench track 3: could not be set up\n") && strstr(logbuf, "lod off") ? "found" : "missing", "found");
    printf("the whole log is %zu bytes (kept free: %d)\n", strlen(logbuf), BENCH_LOG_BYTES);
    fails += strlen(logbuf) > BENCH_LOG_BYTES;
    /* a summary of real frames */
    ft_reset();
    for (int i = 0; i < 10; i++) {
        struct ft_frame f = { 0 };
        f.work = 742500 + 74250 * (uint32_t)i;   /* 10.0 .. 10.9 ms */
        f.faces = 1000 + 10 * (uint32_t)i;
        f.npieces = 200;
        ft_frame(&f);
    }
    ft_summary(&s);
    printf("summary: %u frames avg %u max %u late %u faces %u / %u pieces %u / %u\n", s.frames, s.avg, s.max, s.late,
           s.faces_avg, s.faces_worst, s.pieces_avg, s.pieces_worst);
    fails += !(s.frames == 10 && s.avg == 742500 + 74250 * 9 / 2 + 0 && s.max == 742500 + 74250 * 9 && s.late == 0 &&
               s.faces_avg == 1045 && s.faces_worst == 1090 && s.pieces_avg == 200);
    return fails ? 1 : 0;
}
