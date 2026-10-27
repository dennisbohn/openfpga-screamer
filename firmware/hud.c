/*
 * The race screen (hud.h). Places, fonts and colours from the game (docs/NOTES.md section 12,
 * OpenScreamer's hud_draw); not yet here: the radar (it darkens the picture under it), the
 * record from the records file (the best lap of this race stands in), the cockpit.
 */
#include "hud.h"
#include <math.h>
#include "car.h"
#include "opp.h"
#include "track.h"

static const uint8_t *base;
static const struct blit_image *img;
static const struct blit_font *font;
static int nfont;

static int text(struct blit_list *l, int x, int y, const char *s, int f, int colour)
{
    return f < nfont ? blit_text(l, base, img, &font[f], x, y, s, colour, 1) : 0;
}

static void centred(struct blit_list *l, int y, const char *s, int f, int colour)
{
    if(f >= nfont) return;
    int w = blit_text(l, base, img, &font[f], 0, 0, s, colour, 0);
    text(l, (320 - w) / 2, y, s, f, colour);
}

/* the picture's boxes at their places */
static void picture(struct blit_list *l, int k)
{
    int first, count;
    track_hud_picture(k, &first, &count);
    for(int i = first; i < first + count; i++)
        blit_copy(l, base + img[i].off, img[i].w, img[i].h, img[i].w, img[i].x, img[i].y, BLIT_COPY, 0);
}

/* sprite n with its offset, at (x, y) */
static void sprite(struct blit_list *l, int x, int y, int n)
{
    const struct blit_image *s = &img[n];
    blit_copy(l, base + s->off, s->w, s->h, s->w, x + s->x, y + s->y, BLIT_COPY, 0);
}

/* a few numbers as text (no printf in the firmware) */
static char *put_dec(char *p, uint32_t v, int digits)
{
    char t[10];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while(v || n < digits);
    while(n) *p++ = t[--n];
    *p = 0;
    return p;
}

/* mm:ss:hh, the seconds cut off as the game's float to int (0x4d7b4) does */
static void clock_text(char *p, float t)
{
    int whole = (int)t, hund = (int)((t - (float)whole) * 100.0f);
    p = put_dec(p, (uint32_t)(whole / 60), 2);
    *p++ = ':';
    p = put_dec(p, (uint32_t)(whole % 60), 2);
    *p++ = ':';
    put_dec(p, (uint32_t)hund, 2);
}

/* dash_needle: three lines from the pivot (and a pixel to each side of it) to the tip */
static void needle(struct blit_list *l, int px, int py, int len, float angle, int colour)
{
    float a = angle - 2.0f, b = a + 1.5707964f;
    int tx = (int)((float)px + (float)len * sinf(a)), ty = (int)((float)py - (float)len * cosf(a));
    blit_line(l, px, py, tx, ty, 0, 0, 320, 200, colour);
    blit_line(l, (int)((float)px + sinf(b)), (int)((float)py - cosf(b)), tx, ty, 0, 0, 320, 200, colour);
    blit_line(l, (int)((float)px - sinf(b)), (int)((float)py + cosf(b)), tx, ty, 0, 0, 320, 200, colour);
}

void hud_build(struct blit_list *l, int buffer)
{
    int nimg;
    blit_start(l, buffer);
    base = track_hud();
    img = track_hud_images(&nimg);
    font = track_hud_fonts(&nfont);
    const struct car_state *c = car_state();
    const struct car_race *r = car_race();
    char buf[16];

    picture(l, 0);                                      /* IMPLET_L: LAP, POSITION, the clocks */
    picture(l, 1);                                      /* IMPRES_L: the rev counter */
    buf[0] = 1;                                         /* fixed pitch */
    put_dec(buf + 1, (uint32_t)(c->speed * 0.017f), 3);
    text(l, 0x10F, 0xBB, buf, 2, 7);
    put_dec(buf + 1, (uint32_t)c->gear, 1);
    text(l, 0x130, 0xBB, buf, 2, 1);
    needle(l, 0x10F, 0xA8, 0x1E, car_needle(), 0x75);

    put_dec(buf, (uint32_t)r->timer, 1);                /* the countdown */
    centred(l, 8, buf, 6, 2);
    char *p = put_dec(buf, (uint32_t)(r->laps < r->total_laps ? r->laps + 1 : r->laps), 1);
    p[0] = ':';
    p[1] = 0;
    int w = text(l, 8, 0x0B, buf, 1, 5);
    put_dec(buf, (uint32_t)r->total_laps, 1);
    text(l, w + 5, 0x14, buf, 4, 5);
    int rank = opp_player_rank();
    put_dec(buf, (uint32_t)rank, 1);                    /* the position in the running order */
    text(l, 0x114, 0x0B, buf, 1, 5);
    clock_text(buf, r->total);
    text(l, 8, 0x38, buf, 3, 2);
    clock_text(buf, r->lap);
    text(l, 8, 0x48, buf, 3, 2);
    clock_text(buf, r->best);
    text(l, 8, 0x58, buf, 3, 2);

    if(r->clock < 8.0f) sprite(l, 0x78, 0x36, (int)r->clock / 2);     /* the start lights */

    /* the end (0x21048): GREAT! for the winner, the position (1ST .. 10TH POSITION), the total
       time; or TIME OVER */
    if(r->finished) {
        if(rank == 1) centred(l, 0x3E, track_hud_text(11), 0, 2);
        p = buf;
        const char *t = track_hud_text(rank >= 1 && rank <= 10 ? rank : 10), *pos = track_hud_text(0);
        while(*t && p < buf + 6) *p++ = *t++;
        *p++ = ' ';
        *p = 0;
        static char line[48];
        char *q = line;
        for(const char *s = buf; *s; ) *q++ = *s++;
        while(*pos && q < line + 46) *q++ = *pos++;
        *q = 0;
        centred(l, 0x5A, line, 0, 2);
        clock_text(buf, r->total);
        centred(l, 0x78, buf, 0, 2);
    } else if(r->game_over)
        centred(l, 0x5A, track_hud_text(22), 0, 2);
    (void)nimg;
}
