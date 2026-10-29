/*
 * The race screen (hud.h). Places, fonts and colours from the game (docs/NOTES.md section 12,
 * OpenScreamer's hud_draw), the radar and the cockpit's dashboard from STARTL.EXE; not yet
 * here: the record from the records file (the best lap of this race stands in).
 */
#include "hud.h"
#include <math.h>
#include "car.h"
#include "opp.h"
#include "track.h"
#include "efx.h"
#include "dat.h"
#include "tb_hud.h"
#include "hw.h"

/* the radar's zoom: the option at 0x1d2a0 (0 off, 1, 2, 3 = 24, 48, 72 through the table at
   0x23a58), 1 when there is no CHOICE.DAT (0x1bc3e sets 0x20810, 0x27775 takes it): 24, one
   pixel 32768 / 24 units */
#define RADAR_ZOOM  24
#define MAX_WALLS   1024

static const uint8_t *base;
static const struct blit_image *img;
static const struct blit_font *font;
static int nfont;

/* the cockpit's dashboard: its boxes (tb_hud_picture()'s table, then the pixels) and the
   numbers of its section of CARS.DTR */
#define DASH_NUMBERS    29
static struct { const uint8_t *mem; int32_t at[DASH_NUMBERS]; int ok; } dash;

/* the list started for framebuffer buffer, the race screen section at hand */
static void begin(struct blit_list *l, int buffer)
{
    int nimg;
    blit_start(l, buffer);
    base = track_hud();
    img = track_hud_images(&nimg);
    font = track_hud_fonts(&nfont);
}

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

/* sprite n with its offset at (x, y), cut at the box [x0, x1) x [y0, y1) as the game's sprites
   are (0x25ab0) */
static void sprite_cut(struct blit_list *l, int x, int y, int n, int x0, int y0, int x1, int y1)
{
    const struct blit_image *s = &img[n];
    int px = x + s->x, py = y + s->y, w = s->w, h = s->h, sx = 0, sy = 0;
    if(px < x0) { sx = x0 - px; w -= sx; px = x0; }
    if(py < y0) { sy = y0 - py; h -= sy; py = y0; }
    if(px + w > x1) w = x1 - px;
    if(py + h > y1) h = y1 - py;
    if(w > 0 && h > 0) blit_copy(l, base + s->off + sy * s->w + sx, w, h, s->w, px, py, BLIT_COPY, 0);
}

/* The game's shift by 14 in the radar's walls: arithmetic, and one lower again when the result
   is negative (0x1062e: sar, then dec if negative) */
static int32_t sar14(int32_t v)
{
    int32_t r = v >> 14;
    return r < 0 ? r - 1 : r;
}

/*
 * The radar (0x23a68, last in the race screen 0x23400). Its window is darkened by the 3D
 * units before the race screen (efx_radar_piece). The walls (0x1057c): those of the 17 x 17
 * grid cells round the player's cell (a wall belongs to the cell its middle is in: so on all
 * six tracks), each end turned nose-up round (40, 160) in the game's whole numbers: (end - car)
 * >> 2 times the engine's sine and cosine (amplitude 32768) >> 14, times the zoom >> 14; lines
 * in colour 255 (ds:0x55b) cut at the window (the game cuts them before it draws, here each
 * pixel is; lines wholly outside are left out). Then each opponent as sprite 5 at 39 - x zoom
 * / 32768, 159 + z zoom / 32768 of its place turned the same way (0x2216c, floats, cut to
 * whole numbers), and the player as sprite 4 at (38, 158) (0x23d59), cut at the window.
 */
static void radar(struct blit_list *l)
{
    /* the walls' ends side by side and each one's grid cell; the file has them cell by cell,
       row by row, so row r's are row_at[r] .. row_at[r + 1] - 1 (else all are looked at) */
    static const struct track_wall *seen;
    static struct { int32_t x1, y1, x2, y2; } end[MAX_WALLS];
    static uint8_t cell[MAX_WALLS][2];
    static uint16_t row_at[257];
    static int in_rows;
    int nw;
    const struct track_wall *w = track_walls(&nw);
    if(nw > MAX_WALLS) nw = MAX_WALLS;
    if(w != seen) {
        int r = 0;
        in_rows = 1;
        row_at[0] = 0;
        for(int i = 0; i < nw; i++) {
            end[i].x1 = w[i].x1;
            end[i].y1 = w[i].y1;
            end[i].x2 = w[i].x2;
            end[i].y2 = w[i].y2;
            cell[i][0] = (uint8_t)((w[i].x + 0x100000) >> 13);
            cell[i][1] = (uint8_t)((w[i].y + 0x100000) >> 13);
            if(cell[i][1] < r) in_rows = 0;
            while(r < cell[i][1]) row_at[++r] = (uint16_t)i;
        }
        while(r < 256) row_at[++r] = (uint16_t)nw;
        seen = w;
    }
    const struct car_state *c = car_state();
    int32_t px = (int32_t)c->x, pz = (int32_t)c->y;
    int32_t cx = ((px + 0x100000) >> 13) - 8, cz = ((pz + 0x100000) >> 13) - 8;
    int first = 0, last = nw;                           /* the walls of rows cz .. cz + 16 */
    if(in_rows) {
        first = row_at[cz < 0 ? 0 : cz > 256 ? 256 : cz];
        last = row_at[cz + 17 < 0 ? 0 : cz + 17 > 256 ? 256 : cz + 17];
    }
    /* the heading in 2048ths of a turn (ds:0xe38, 0xe3c), cut, and its sine and cosine */
    int32_t a = (int32_t)(c->yaw * 1024.0f / 3.14159274f) & 0x7FF;
    float ar = (float)a * (3.14159265f / 1024.0f);
    int32_t sn = (int32_t)lrintf(sinf(ar) * 32768.0f), cs = (int32_t)lrintf(cosf(ar) * 32768.0f);
    if(sn > 32767) sn = 32767;
    if(cs > 32767) cs = 32767;
    uint32_t lines = 0;
    for(int i = first; i < last; i++) {
        if((uint32_t)(cell[i][0] - cx) > 16u || (uint32_t)(cell[i][1] - cz) > 16u) continue;
        int32_t sx[2], sy[2];
        for(int e = 0; e < 2; e++) {
            int32_t dx = ((e ? end[i].x2 : end[i].x1) - px) >> 2, dz = ((e ? end[i].y2 : end[i].y1) - pz) >> 2;
            int32_t rx = sar14(dx * cs) + sar14(dz * sn), rz = sar14(dz * cs - dx * sn);
            sx[e] = 40 - sar14(RADAR_ZOOM * rx);
            sy[e] = 160 + sar14(RADAR_ZOOM * rz);
        }
        if((sx[0] < EFX_RADAR_X0 && sx[1] < EFX_RADAR_X0) || (sx[0] >= EFX_RADAR_X1 && sx[1] >= EFX_RADAR_X1) ||
           (sy[0] < EFX_RADAR_Y0 && sy[1] < EFX_RADAR_Y0) || (sy[0] >= EFX_RADAR_Y1 && sy[1] >= EFX_RADAR_Y1))
            continue;
        blit_line(l, sx[0], sy[0], sx[1], sy[1], EFX_RADAR_X0, EFX_RADAR_Y0, EFX_RADAR_X1, EFX_RADAR_Y1, 255);
        lines++;
    }
    float fc = cosf(c->yaw), fs = sinf(c->yaw), k = (float)RADAR_ZOOM * 3.0517578125e-05f;    /* ds:0xe40 */
    float ox, oy;
    for(int j = 0; opp_where(j, &ox, &oy); j++) {
        float dx = (float)((int32_t)ox - px), dz = (float)((int32_t)oy - pz);
        int32_t rx = (int32_t)(dx * fc + dz * fs), rz = (int32_t)(dz * fc - dx * fs);
        sprite_cut(l, (int)(39.0f - (float)rx * k), (int)((float)rz * k + 159.0f), 5,
                   EFX_RADAR_X0, EFX_RADAR_Y0, EFX_RADAR_X1, EFX_RADAR_Y1);
    }
    sprite_cut(l, 38, 158, 4, EFX_RADAR_X0, EFX_RADAR_Y0, EFX_RADAR_X1, EFX_RADAR_Y1);
    efx_radar_lines = lines;
}

/* the palette index nearest to colour 0xRRGGBB */
static int nearest(uint32_t colour)
{
    const uint32_t *pal = track_palette();
    int best = 0;
    int32_t best_d = 0x7FFFFFFF;
    for(int i = 0; i < 256; i++) {
        int32_t dr = (int32_t)(pal[i] >> 16 & 255) - (int32_t)(colour >> 16 & 255);
        int32_t dg = (int32_t)(pal[i] >> 8 & 255) - (int32_t)(colour >> 8 & 255);
        int32_t db = (int32_t)(pal[i] & 255) - (int32_t)(colour & 255);
        int32_t d = dr * dr + dg * dg + db * db;
        if(d < best_d) { best_d = d; best = i; }
    }
    return best;
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

/* the frame rate (not the game's): frames drawn in the last second on a square, green at the
   full 50, yellow below */
static void fps_mark(struct blit_list *l, int fps)
{
    static int green = -1, yellow;
    char buf[8];
    if(green < 0) {
        green = nearest(0x00FF00);
        yellow = nearest(0xFFFF00);
    }
    blit_fill(l, 4, 191, 5, 5, fps >= 50 ? green : yellow);
    put_dec(buf, (uint32_t)fps, 1);
    text(l, 12, 190, buf, 3, 2);
}

void hud_build(struct blit_list *l, int buffer, int fps, int cockpit)
{
    begin(l, buffer);
    const struct car_state *c = car_state();
    const struct car_race *r = car_race();
    char buf[16];

    if(!cockpit) {                                      /* overlay 2 (0x237fc) */
        picture(l, 0);                                  /* IMPLET_L: LAP, POSITION, the clocks */
        picture(l, 1);                                  /* IMPRES_L: the rev counter */
        buf[0] = 1;                                     /* fixed pitch */
        put_dec(buf + 1, (uint32_t)(c->speed * 0.017f), 3);
        text(l, 0x10F, 0xBB, buf, 2, 7);
        put_dec(buf + 1, (uint32_t)c->gear, 1);
        text(l, 0x130, 0xBB, buf, 2, 1);
        needle(l, 0x10F, 0xA8, 0x1E, car_needle(), 0x75);
    }

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
    uint32_t t0 = rdcycle();
    radar(l);
    efx_cyc_radar = rdcycle() - t0;

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
    if(fps > 0) fps_mark(l, fps);
}

/* the word w (len bytes) is s */
static int same(const uint8_t *w, uint32_t len, const char *s)
{
    uint32_t k = 0;
    while(k < len && s[k] && w[k] == (uint8_t)s[k]) k++;
    return k == len && !s[k];
}

/* The numbers of section name in a .DTR text (t, n bytes): from the line after the one it
   heads, each word d<number> in turn up to END_LIST; a word from * on comments out the rest of
   its line. 1 if there are count of them. */
static int dtr_numbers(const uint8_t *t, uint32_t n, const char *name, int32_t *v, int count)
{
    int on = 0, got = 0;
    for(uint32_t at = 0; at < n && got < count; at++) {
        int words = 0, heads = 0;
        while(at < n && t[at] != '\n') {
            if(t[at] <= ' ') {
                at++;
                continue;
            }
            uint32_t w = at;
            while(at < n && t[at] > ' ') at++;
            uint32_t len = at - w;
            if(t[w] == '*') {
                while(at < n && t[at] != '\n') at++;
                break;
            }
            if(!on) heads |= words == 0 && same(t + w, len, name);
            else if(same(t + w, len, "END_LIST")) return 0;
            else if(t[w] == 'd' && got < count) {
                int32_t x = 0;
                int neg = len > 1 && t[w + 1] == '-';
                for(uint32_t k = 1 + (uint32_t)neg; k < len && t[w + k] >= '0' && t[w + k] <= '9'; k++)
                    x = x * 10 + (t[w + k] - '0');
                v[got++] = neg ? -x : x;
            }
            words++;
        }
        on |= heads;
    }
    return got == count;
}

int hud_cockpit_init(const uint8_t *game, uint32_t game_size, const uint8_t *gen, uint32_t gen_size,
                     const struct car_spec *sp, uint8_t *mem, uint32_t cap, uint8_t *scratch,
                     uint32_t scratch_cap)
{
    /* COCK and the name from its fifth letter on (the model's digits, 0x13725), A or B (0x1371a)
       and L.PKC (0x1b25f); the section COCK, the digits, L (0x1374c) */
    char pic[24] = "COCK", sec[24];
    int k = 4;
    for(int i = 4; i < 12 && sp->name[i] && k < 12; i++) pic[k++] = sp->name[i];
    for(int i = 0; i < k; i++) sec[i] = pic[i];
    sec[k] = 'L';
    sec[k + 1] = 0;
    pic[k++] = sp->livery ? 'B' : 'A';
    for(const char *p = "L.PKC"; (pic[k++] = *p) != 0; p++) ;
    dash.ok = 0;
    struct dat_entry e;
    uint32_t n;
    if(!dat_find(game, game_size, pic, &e) || !(n = dat_extract(game, game_size, &e, scratch, scratch_cap)) ||
       !tb_hud_picture(scratch, n, mem, cap))
        return 0;
    if(!dat_find(gen, gen_size, "CARS.DTR", &e) || !(n = dat_extract(gen, gen_size, &e, scratch, scratch_cap)) ||
       !dtr_numbers(scratch, n, sec, dash.at, DASH_NUMBERS))
        return 0;
    while(!(MEM_STATUS & MEM_WRITES_IDLE))              /* the boxes in the SDRAM for the blitter */
        ;
    dash.mem = mem;
    dash.ok = 1;
    return 1;
}

void hud_dash(struct blit_list *l, int buffer)
{
    begin(l, buffer);
    picture(l, 0);                                      /* IMPLET_L first (0x236b9) */
    if(!dash.ok) return;
    const uint8_t *m = dash.mem;
    const struct blit_image *b = (const struct blit_image *)(m + 4);
    uint32_t boxes = *(const uint32_t *)m;
    for(uint32_t i = 0; i < boxes; i++)
        blit_copy(l, m + b[i].off, b[i].w, b[i].h, b[i].w, b[i].x, b[i].y, BLIT_COPY, 0);
    const int32_t *d = dash.at;
    needle(l, d[2], d[3], d[0], car_speed_needle(), 255);
    needle(l, d[5], d[6], d[0], car_needle(), 255);
    /* the lamp: a sprite of GAMEL.SPR, the first image of the section's table up to the fonts' */
    int gear = car_state()->gear, n = d[1] + gear;
    if(gear >= 0 && gear <= 6 && n >= 0 && nfont > 0 && n < (int)font[0].first_image)
        sprite(l, d[8 + 3 * gear], d[9 + 3 * gear], n);
}
