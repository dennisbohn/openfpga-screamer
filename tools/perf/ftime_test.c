/* Host test of the timing overlay's mean (firmware/ftime.c) over more than a ring's frames. The mean of
   the last 50 frames must be that mean, whatever the frames before them were.
   FTIME_C names the source file (default the firmware's), so that another version can be run as a
   negative control. Stubs stand in for the console.
   Build in tools/perf: gcc -Wall -Wextra -DDEBUG=1 -I../../firmware ftime_test.c -o ftime_test -lm
   The program exits with 1 if a check fails. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static char drawn[3][64];
/* The real fmt_str, fmt_udec and fmt_scale are used; the text drawing is a stub. */
#define con_text real_con_text
#include "../../firmware/console.c"
#undef con_text
int con_text(volatile uint8_t *fb, int col, int row, const char *s, uint8_t fg, uint8_t bg)
{
    (void)fb; (void)col; (void)fg; (void)bg;
    snprintf(drawn[row], sizeof drawn[row], "%s", s);
    return 0;
}
#ifndef FTIME_C
#define FTIME_C "../../firmware/ftime.c"
#endif
#include FTIME_C

static int fails;

static void check(const char *what, const uint32_t *w, int n, double expect_ms)
{
    for(int i = 0; i < n; i++) {
        struct ft_frame f = { 0 };
        f.work = w[i];
        ft_frame(&f);
    }
    ft_draw(0, 1, 0, 1.5f, 0.0f);
    double ms = 0;
    const char *a = strstr(drawn[0], "avg ");
    if(a) ms = atof(a + 4);
    int ok = a && ms > expect_ms - 0.11 && ms < expect_ms + 0.11;
    fails += strstr(drawn[2], "F 1.50  L off") != drawn[2];     /* the third line shows the settings */
    printf("third line \"%s\"\n", drawn[2]);
    printf("%s: the line \"%s\", the mean should be %.1f: %s\n", what, drawn[0], expect_ms, ok ? "ok" : "WRONG");
    fails += !ok;
}

int main(void)
{
    enum { N = 400 };
    uint32_t w[N];
    const uint32_t ms10 = 742500, ms20 = 1485000, ms5 = 371250;
    /* A falling frame time: every new frame is shorter than the one that leaves the ring. */
    for(int i = 0; i < N; i++) w[i] = ms20 - (uint32_t)(i * (ms20 - ms5) / N);
    memset(&ft, 0, sizeof ft);
    check("falling", w, N, ((double)(ms20 - (N - 25) * (ms20 - ms5) / N)) / 74250.0 );
    /* Alternating 10 and 20 ms, with a long first frame (the countdown's). */
    memset(&ft, 0, sizeof ft);
    for(int i = 0; i < N; i++) w[i] = i == 0 ? 3 * ms20 : (i & 1) ? ms10 : ms20;
    check("alternating", w, N, 15.0);
    return fails ? 1 : 0;
}
