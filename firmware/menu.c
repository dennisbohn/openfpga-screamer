/*
 * The track and car screens (menu.h). The numbers are the game's as openscreamer read them
 * (NOTES.md sections 18 and 26) and STARTL.EXE has them: the rows and cameras of 0x2bdcc,
 * 0x2c078, 0x2ca10 and 0x2cb88, the slide of 0x29530 and its keys 0x2859e .. 0x287cd, the 2D
 * layouts of 0x2e08c and 0x2e384.
 */
#include "menu.h"
#include "hw.h"
#include "dat.h"
#include "track.h"
#include "car.h"
#include "tb_hud.h"
#include "tb_spl.h"
#include "sound.h"
#include <math.h>
#include <string.h>

#define ROW_Z       2900.0f         /* the rows at height 0 and z 2900 (our y); the camera over
                                       the chosen model's x looks along -z (yaw 0) */
#define SLIDE       0.95f           /* the camera's slide: a factor from 1 to 0 at this a second, the */
#define TAKE        0.8f            /* place by its fourth power; a press is taken under 0.8 */
#define LIT         0.6f            /* the arrow of the slide's way is lit above this (0x12e3) */
#define DT          0.02f           /* a step */
#define TWO_PI      6.2831853f
#define PIC_W       320             /* MENU3L.PCX, MENU4L.PCX */
#define PIC_H       200
#define MAX_PARTS   (3 * CAR_BLOCKS)    /* blocks drawn: three models (the chosen one and its neighbours) */
#define FONT_TITLE  3               /* MENUL.FNT's fonts: 3 is 30 high, 2 is 19, 1 is 9, 0 is 11 */
#define FONT_NAME   2
#define FONT_CAR    0
#define FONT_DATA   1
#define COLOUR      2               /* the texts' colour: palette 0x20 + shade */
#define LINE_GEARS  37              /* MENU.TXT's lines, its '*' lines not counted: AUTOMATIC and */
#define LINE_LABELS 39              /* MANUAL TRANSMISSION; MAX SPEED, GRIP, ACCELERATION; */
#define LINE_CARS   44              /* CAR SELECTION; */
#define LINE_TRACKS 45              /* TRACK SELECTION; LOADING */
#define LINE_LOADING 47
#define LOADING_Y   0x50            /* the loading screen's LOADING: its row, font 3, colour 0 (0x2a264) */
#define SPR_GRIP    4               /* MENUL.SPR: the grip bar's five steps, the acceleration's */
#define SPR_ACCEL   9
#define CAR_MODELS  7               /* AAUTO00 .. AAUTO06 */

/* per screen (the tracks, the cars) the row and the camera: the gap between two models along x,
   the camera's height and z, its pitch, the models' turn in rad a second (from a quarter turn
   on) */
static const struct row { float gap, cam_h, cam_z, pitch, turn; } rows[2] = {
    { 40000.0f, 4800.0f, 13000.0f, -0.6283185f, 0.7853982f },  /* 0x2c078, 0x2cb88: 36 degrees down */
    { 4000.0f, 500.0f, 4100.0f, -0.3926991f, 1.5707964f },     /* 0x2bdcc, 0x2ca10: 22.5 degrees */
};

const struct menu_part menu_dat_parts[] = {
    { "MENU.MLB", ~0u }, { "AMAP1.3DX", ~0u }, { "AMAP2.3DX", ~0u }, { "AMAP3.3DX", ~0u },
    { "AMAP4.3DX", ~0u }, { "AMAP5.3DX", ~0u }, { "AMAP6.3DX", ~0u }, { "AAUTO00.3DX", ~0u },
    { "AAUTO01.3DX", ~0u }, { "AAUTO02.3DX", ~0u }, { "AAUTO03.3DX", ~0u }, { "AAUTO04.3DX", ~0u },
    { "AAUTO05.3DX", ~0u }, { "AAUTO06.3DX", ~0u }, { 0, 0 } };
const struct menu_part menu_graf_parts[] = {
    { "MENU3L.PCX", ~0u }, { "MENU4L.PCX", ~0u }, { "MENUL.FNT", ~0u }, { "MENUL.SPR", ~0u },
    { "LOAD1L.PCX", ~0u }, { "LOAD2L.PCX", ~0u }, { "LOAD3L.PCX", ~0u }, { "LOAD4L.PCX", ~0u },
    { "LOAD5L.PCX", ~0u }, { "LOAD6L.PCX", ~0u }, { 0, 0 } };

/* CARDAT.TXT's six lines a car: name, gearbox (0 automatic, 1 manual), km/h, grip and
   acceleration steps (0..4), 0; of its CARS.DTR row the model and the paint job */
static struct car { char name[28]; int gears, kmh, grip, accel; const uint8_t *mesh; uint32_t livery; } car[MENU_CARS];
static const uint8_t *mlb, *model[MENU_TRACKS], *pic[3], *hud, *graf_dat;
static const struct blit_image *img;
static const struct blit_font *font;
static uint32_t nimg, nfont, graf_len, pal[3][256];
static char title[2][32], name[MENU_TRACKS][32], gears[2][32], label[3][32], loading[32];
static int ncars, cars_on, pick, way;
static float turn[2] = { 1.5707964f, 1.5707964f }, cam_x, from_x, to_x, slide;

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static int32_t rd32(const uint8_t *p)
{
    return (int32_t)(rd16(p) | rd16(p + 2) << 16);
}

/* a stored entry where it lies in the container, at least min bytes (*len its size); 0 if
   missing or packed */
static const uint8_t *in_place(const uint8_t *file, uint32_t size, const char *n, uint32_t min, uint32_t *len)
{
    struct dat_entry e;
    if(!dat_find(file, size, n, &e) || e.method != 0 || e.size < min || e.offset > size || e.size > size - e.offset)
        return 0;
    *len = e.size;
    return file + e.offset;
}

/* an entry unpacked to out (at most max bytes): its size, 0 if missing */
static uint32_t unpack(const uint8_t *file, uint32_t size, const char *n, uint8_t *out, uint32_t max)
{
    struct dat_entry e;
    return dat_find(file, size, n, &e) ? dat_extract(file, size, &e, out, max) : 0;
}

/* line k of a text file into out (at most max - 1 characters), lines starting with '*' not
   counted (the port's text_lines) */
static int text_line(const uint8_t *t, uint32_t n, int k, char *out, int max)
{
    for(uint32_t i = 0; i < n; ) {
        uint32_t e = i;
        while(e < n && t[e] != '\r' && t[e] != '\n')
            e++;
        if(t[i] != '*' && k-- == 0) {
            int m = 0;
            while(i < e && m < max - 1)
                out[m++] = (char)t[i++];
            out[m] = 0;
            return 1;
        }
        i = e + (e < n && t[e] == '\r' && e + 1 < n && t[e + 1] == '\n' ? 2 : 1);
    }
    out[0] = 0;
    return 0;
}

/* the number at the start of line k (text_line) */
static int text_number(const uint8_t *t, uint32_t n, int k)
{
    char s[16];
    int v = 0;
    text_line(t, n, k, s, sizeof s);
    for(const char *p = s; *p >= '0' && *p <= '9'; p++) v = 10 * v + (*p - '0');
    return v;
}

/* A 320 x 200 PCX picture (8 bits, RLE, the 256 colours after the byte 12 at its end) into
   out and its palette into pal; a run does not go on into the next line (the port's pcx_load) */
static int pcx(const uint8_t *d, uint32_t n, uint8_t *out, uint32_t *pal)
{
    if(n < 128 + 769 || d[0] != 10 || d[2] != 1 || d[3] != 8 || d[65] != 1 || d[n - 769] != 12) return 0;
    uint32_t w = rd16(d + 8) - rd16(d + 4) + 1, h = rd16(d + 10) - rd16(d + 6) + 1, stride = rd16(d + 66);
    if(w != PIC_W || h != PIC_H || stride < w) return 0;
    uint32_t p = 128, end = n - 769;
    for(uint32_t y = 0; y < h; y++)
        for(uint32_t x = 0; x < stride; ) {
            if(p >= end) return 0;
            uint32_t c = d[p++], run = 1;
            if((c & 0xC0) == 0xC0) {
                if(p >= end) return 0;
                run = c & 0x3F;
                c = d[p++];
            }
            for(; run && x < stride; run--, x++)
                if(x < w) out[y * w + x] = (uint8_t)c;
        }
    const uint8_t *q = d + end + 1;
    for(int i = 0; i < 256; i++)
        pal[i] = (uint32_t)q[3 * i] << 16 | (uint32_t)q[3 * i + 1] << 8 | q[3 * i + 2];
    return 1;
}

int menu_init(const uint8_t *menu_dat, uint32_t menu_size, const uint8_t *graf, uint32_t graf_size,
              const uint8_t *local, uint32_t local_size, const uint8_t *gen, uint32_t gen_size, uint8_t *mem,
              uint32_t cap, uint8_t *scratch, uint32_t scratch_cap)
{
    uint32_t len;
    const uint8_t *cars[CAR_MODELS];
    if(!(mlb = in_place(menu_dat, menu_size, "MENU.MLB", 0x8000, &len))) return 0;
    for(int k = 0; k < MENU_TRACKS; k++) {
        char n[16] = "AMAP1.3DX";
        n[4] = (char)('1' + k);
        if(!(model[k] = in_place(menu_dat, menu_size, n, 0x80, &len)) || rd32(model[k] + 0x14) < 1) return 0;
    }
    for(int k = 0; k < CAR_MODELS; k++) {
        char n[16] = "AAUTO00.3DX";
        n[6] = (char)('0' + k);
        if(!(cars[k] = in_place(menu_dat, menu_size, n, 0x80, &len)) || rd32(cars[k] + 0x14) < 1) return 0;
    }
    /* the pictures and room for the loading screen's (menu_loading), then the sprites and fonts
       as the race screen's (tb_hud.h, no pictures, no text) */
    const uint8_t *p = in_place(graf, graf_size, "MENU3L.PCX", 0, &len);
    if(cap < 3 * PIC_W * PIC_H || !p || !pcx(p, len, mem, pal[0])) return 0;
    p = in_place(graf, graf_size, "MENU4L.PCX", 0, &len);
    if(!p || !pcx(p, len, mem + PIC_W * PIC_H, pal[1])) return 0;
    pic[0] = mem;
    pic[1] = mem + PIC_W * PIC_H;
    pic[2] = mem + 2 * PIC_W * PIC_H;
    graf_dat = graf;
    graf_len = graf_size;
    uint8_t *at = mem + 3 * PIC_W * PIC_H;
    uint32_t spr_n = unpack(graf, graf_size, "MENUL.SPR", scratch, scratch_cap);
    uint32_t used = (spr_n + 3) & ~3u;
    uint32_t fnt_n = used < scratch_cap ? unpack(graf, graf_size, "MENUL.FNT", scratch + used, scratch_cap - used) : 0;
    if(!spr_n || !fnt_n ||
       !tb_hud_build(scratch, spr_n, scratch + used, fnt_n, scratch, 0, scratch, 0, scratch, 0, at,
                     cap - 3 * PIC_W * PIC_H)) return 0;
    hud = at;
    nimg = (uint32_t)rd32(hud);
    nfont = (uint32_t)rd32(hud + 4);
    img = (const struct blit_image *)(hud + 8);
    font = (const struct blit_font *)(hud + 8 + 12 * nimg);
    /* the texts */
    uint32_t tn = unpack(local, local_size, "MENU.TXT", scratch, scratch_cap);
    int ok = text_line(scratch, tn, LINE_TRACKS, title[0], sizeof title[0]) &
             text_line(scratch, tn, LINE_CARS, title[1], sizeof title[1]);
    for(int k = 0; k < 2; k++) ok &= text_line(scratch, tn, LINE_GEARS + k, gears[k], sizeof gears[k]);
    for(int k = 0; k < 3; k++) ok &= text_line(scratch, tn, LINE_LABELS + k, label[k], sizeof label[k]);
    ok &= text_line(scratch, tn, LINE_LOADING, loading, sizeof loading);
    tn = unpack(local, local_size, "TRACKDAT.TXT", scratch, scratch_cap);
    for(int k = 0; k < MENU_TRACKS; k++) ok &= text_line(scratch, tn, 4 * k, name[k], sizeof name[k]);
    /* the cars: CARS.DTR's rows as the track image has them (tb_spl.h), CARDAT.TXT's lines */
    uint32_t dn = unpack(gen, gen_size, "CARS.DTR", scratch, scratch_cap), rows_at = (dn + 3) & ~3u;
    uint32_t rn = dn && rows_at < scratch_cap ? tb_cars(scratch, dn, scratch + rows_at, scratch_cap - rows_at) / 68 : 0;
    const struct car_spec *row = (const struct car_spec *)(scratch + rows_at);
    for(ncars = 0; ncars < (int)rn && ncars < MENU_CARS; ncars++) {
        int m = car_model_index(&row[ncars]);
        car[ncars].mesh = cars[m >= 0 && m < CAR_MODELS ? m : 0];
        car[ncars].livery = (uint32_t)row[ncars].livery;
    }
    tn = unpack(local, local_size, "CARDAT.TXT", scratch, scratch_cap);
    for(int k = 0; k < ncars; k++) {
        struct car *c = &car[k];
        if(!text_line(scratch, tn, 6 * k, c->name, sizeof c->name)) {
            ncars = k;
            break;
        }
        c->gears = text_number(scratch, tn, 6 * k + 1) != 0;
        c->kmh = text_number(scratch, tn, 6 * k + 2);
        c->grip = text_number(scratch, tn, 6 * k + 3) % 5;
        c->accel = text_number(scratch, tn, 6 * k + 4) % 5;
    }
    return ok && ncars > 0 && nimg > SPR_ACCEL + 4 && nfont > FONT_TITLE;
}

int menu_cars(void)
{
    return ncars;
}

const uint32_t *menu_palette(void)
{
    return pal[cars_on];
}

const uint8_t *menu_mlb(void)
{
    return mlb;
}

/* the choices of the screen shown */
static int choices(void)
{
    return cars_on ? ncars : MENU_TRACKS;
}

void menu_enter(int screen, int n)
{
    cars_on = screen == MENU_CAR_SCREEN;
    pick = n < 1 ? 0 : n > choices() ? choices() - 1 : n - 1;
    cam_x = from_x = to_x = -rows[cars_on].gap * (float)pick;
    slide = 0.0f;
    way = 0;
}

int menu_step(uint32_t keys, uint32_t keys_before)
{
    const struct row *r = &rows[cars_on];
    uint32_t press = keys & ~keys_before;
    float *t = &turn[cars_on];
    *t += DT * r->turn;
    if(*t >= TWO_PI) *t = 0.0f;
    float f2 = slide * slide;
    cam_x = to_x + (from_x - to_x) * f2 * f2;
    if(slide > 0.0f && (slide -= DT * SLIDE) < 0.0f) slide = 0.0f;
    if((press & (KEY_LEFT | KEY_RIGHT)) && slide < TAKE) {
        int to = pick + (press & KEY_RIGHT ? 1 : -1);
        if(to >= 0 && to < choices()) {
            way = to - pick;
            pick = to;
            from_x = cam_x;
            to_x = -r->gap * (float)to;
            slide = 1.0f;
            snd_play(SND_MENU_MOVE);
        }
    }
    if(press & (KEY_B | KEY_START)) return pick + 1;
    return cars_on && (press & (KEY_Y | KEY_SELECT)) ? -1 : 0;
}

void menu_backdrop(struct blit_list *l)
{
    blit_copy(l, pic[cars_on], PIC_W, PIC_H, PIC_W, 0, 0, BLIT_COPY | BLIT_OPAQUE, 0);
}

/* the race's lens (track.c: the eye 128 behind the camera, 128 / depth) round the menu's centre,
   column 159 and row 100 */
void menu_view(struct su_view *v)
{
    v->cx = (PIC_W / 2 - 1) * 16;
    v->cy = PIC_H / 2 * 16;
    v->scale = 128 * 16;
    v->far8 = (int32_t)((43808.0f + 128.0f) * 256.0f);
}

/* The chosen track and its neighbours, as far as they are in view (half a gap to the camera's
   side), block by block the furthest block's middle first, as the game draws an object's blocks
   (0x8ac58) */
static int map_blocks(const struct track_cam *cam, struct track_block *b)
{
    float key[MAX_PARTS];
    int n = 0;
    const float c = cosf(turn[0]), s = sinf(turn[0]);
    for(int k = pick - 1; k <= pick + 1; k++) {
        float x = -rows[0].gap * (float)k;
        if(k < 0 || k >= MENU_TRACKS || fabsf(x - cam_x) > 0.5f * rows[0].gap) continue;
        const uint8_t *obj = model[k];
        for(int j = 0; j < rd32(obj + 0x14) && n < MAX_PARTS; j++) {
            /* world from the model: x along (cos, sin), its y up, its z along (-sin, cos); each
               block moved by its place, the position record's +0x10 (track.c multi_pieces) */
            const uint8_t *blk = obj + rd32(obj + 0x18 + 4 * j);
            int32_t so = rd32(blk + 0x0C);
            float px = so > 0 ? (float)rd32(obj + so + 0x10) : 0.0f, py = so > 0 ? (float)rd32(obj + so + 0x14) : 0.0f;
            float pz = so > 0 ? (float)rd32(obj + so + 0x18) : 0.0f;
            struct track_block t = { obj, blk, { c, 0.0f, -s, s, 0.0f, c, 0.0f, 1.0f, 0.0f },
                                     { x + c * px - s * pz, ROW_Z + s * px + c * pz, py }, 0 };
            float mx = (float)(int16_t)rd16(blk + 4), my = (float)(int16_t)rd16(blk + 6);
            float mz = (float)(int16_t)rd16(blk + 8);
            float dx = t.o[0] + c * mx - s * mz - cam->x, dy = t.o[1] + s * mx + c * mz - cam->y, dh = t.o[2] + my - cam->h;
            float d = dx * dx + dy * dy + dh * dh;
            int i = n++;
            for(; i > 0 && key[i - 1] < d; i--) {
                b[i] = b[i - 1];
                key[i] = key[i - 1];
            }
            b[i] = t;
            key[i] = d;
        }
    }
    return n;
}

/* The chosen car and its neighbours less than a gap to the camera's side, the further first,
   each standing in its paint job as the race draws a car (car.h; 0x2ca10 turns them all; the
   texture phase of 0x6000: not braking, slow) */
static int car_blocks(const struct track_cam *cam, struct track_block *b)
{
    int k[3], m = 0, n = 0;
    for(int j = pick - 1; j <= pick + 1; j++) {
        float d = fabsf(-rows[1].gap * (float)j - cam_x);
        if(j < 0 || j >= ncars || d >= rows[1].gap) continue;
        int i = m++;
        for(; i > 0 && fabsf(-rows[1].gap * (float)k[i - 1] - cam_x) < d; i--) k[i] = k[i - 1];
        k[i] = j;
    }
    for(int i = 0; i < m; i++) {
        const struct car *c = &car[k[i]];
        const struct car_look lk = { -rows[1].gap * (float)k[i], ROW_Z, 0.0f, turn[1], 0.0f, 0.0f, 0.0f, 0.0f,
                                     0.0f, 0.0f, (c->livery | 0x6000u) >> 2 };
        n += car_mesh_draw(c->mesh, &lk, cam, b + n);
    }
    return n;
}

void menu_scene(volatile uint8_t *fb)
{
    static const struct rs_clip clip = { 0, 0, PIC_W, PIC_H };
    const struct row *r = &rows[cars_on];
    const struct track_cam cam = { cam_x, r->cam_z, r->cam_h, 0.0f, r->pitch, 0.0f };
    static struct track_block b[MAX_PARTS];
    int n = cars_on ? car_blocks(&cam, b) : map_blocks(&cam, b);
    struct su_view v;
    struct gu_stats st = { 0, 0, 0, 0 };
    menu_view(&v);
    for(int i = 0; i < n; i++) {
        struct gu_piece g;
        track_block_piece(&cam, &b[i], &g);
        if(track_mode == TRACK_HW) track_hw_piece(&g);
        else gu_draw(fb, &clip, &v, mlb, &g, &st);
    }
}

static int text(struct blit_list *l, int x, int y, const char *s, uint32_t f)
{
    return blit_text(l, hud, img, &font[f], x, y, s, COLOUR, 1);
}

/* a string centred on the screen at row y (0x2e598) */
static void centred(struct blit_list *l, int y, const char *s, uint32_t f)
{
    int w = blit_text(l, hud, img, &font[f], 0, 0, s, COLOUR, 0);
    text(l, (PIC_W - w) / 2, y, s, f);
}

static void sprite(struct blit_list *l, int x, int y, uint32_t k)
{
    const struct blit_image *s = &img[k];
    blit_copy(l, hud + s->off, s->w, s->h, s->w, x + s->x, y + s->y, BLIT_COPY, 0);
}

/* v in decimal at p: the end */
static char *dec(char *p, int v)
{
    char d[12];
    int n = 0;
    do d[n++] = (char)('0' + v % 10); while((v /= 10) > 0);
    while(n) *p++ = d[--n];
    return p;
}

/* The car's data (0x2e08c): its name 3 font heights down, the gearbox 5 heights over the
   bottom (font 0); MAX SPEED, GRIP and ACCELERATION one under the other from 210 - 5 heights of
   font 1, centred with their values, whose column starts as far right as the longest label
   written in A's is wide and is given 64 pixels: "%d KMH", the bars (a pixel lower) */
static void car_text(struct blit_list *l, const struct car *c)
{
    int h0 = (int)font[FONT_CAR].height, h1 = (int)font[FONT_DATA].height;
    centred(l, 3 * h0, c->name, FONT_CAR);
    centred(l, PIC_H - 5 * h0, gears[c->gears], FONT_CAR);
    char as[32], kmh[20];
    size_t most = 0;
    for(int k = 0; k < 3; k++) if(strlen(label[k]) > most) most = strlen(label[k]);
    memset(as, 'A', most);
    as[most] = 0;
    int lw = blit_text(l, hud, img, &font[FONT_DATA], 0, 0, as, COLOUR, 0);
    int x = (PIC_W - (lw + 0x40)) / 2, y = 0xD2 - 5 * h1;
    memcpy(dec(kmh, c->kmh), " KMH", 5);
    text(l, x, y, label[0], FONT_DATA);
    text(l, x + lw, y, kmh, FONT_DATA);
    y += h1;
    text(l, x, y, label[1], FONT_DATA);
    sprite(l, x + lw, y + 1, SPR_GRIP + (uint32_t)c->grip);
    y += h1;
    text(l, x, y, label[2], FONT_DATA);
    sprite(l, x + lw, y + 1, SPR_ACCEL + (uint32_t)c->accel);
}

/* The title, the track's name 2 heights of font 2 over the bottom (0x2e384) or the car's data;
   the arrows where there is a neighbour, lit (sprites 2 and 3) while the camera slides that way
   (0x2e2f0, 0x2e33c) */
void menu_text(struct blit_list *l)
{
    int lit = slide > LIT;
    centred(l, 2, title[cars_on], FONT_TITLE);
    if(cars_on) car_text(l, &car[pick]);
    else centred(l, PIC_H - 2 * (int)font[FONT_NAME].height, name[pick], FONT_NAME);
    if(pick > 0) sprite(l, 8, 100, lit && way < 0 ? 2 : 0);
    if(pick < choices() - 1) sprite(l, 0xEE, 100, lit && way > 0 ? 3 : 1);
}

/* The loading screen (0x2a264): LOADnL.PCX, unpacked from its place in GRAF.DAT now (the boot
   loaded the six stored), LOADING over it centred at row 0x50 */
const uint32_t *menu_loading(int n, struct blit_list *l)
{
    char file[16] = "LOAD1L.PCX";
    uint32_t len;
    file[4] = (char)('0' + n);
    const uint8_t *p = graf_dat && n >= 1 && n <= MENU_TRACKS ? in_place(graf_dat, graf_len, file, 0, &len) : 0;
    if(!p || !pcx(p, len, (uint8_t *)pic[2], pal[2])) return 0;
    blit_copy(l, pic[2], PIC_W, PIC_H, PIC_W, 0, 0, BLIT_COPY | BLIT_OPAQUE, 0);
    int w = blit_text(l, hud, img, &font[FONT_TITLE], 0, 0, loading, 0, 0);
    blit_text(l, hud, img, &font[FONT_TITLE], (PIC_W - w) / 2, LOADING_Y, loading, 0, 1);
    return pal[2];
}
