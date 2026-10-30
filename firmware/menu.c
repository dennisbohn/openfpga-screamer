/*
 * The pages, the track and car screens (menu.h). The numbers are the game's as openscreamer
 * read them (NOTES.md sections 18 and 26) and STARTL.EXE has them: the pages of 0x2de4, their
 * letters laid out by 0x2af30 and moved by 0x2b8b8, their keys 0x28080 .. 0x28df0, the pull-back
 * of 0x293e8; the rows and cameras of 0x2bdcc, 0x2c078, 0x2ca10 and 0x2cb88, the slide of
 * 0x29530 and its keys 0x2859e .. 0x287cd, the 2D layouts of 0x2df38, 0x2e08c and 0x2e384.
 */
#include "menu.h"
#include "hw.h"
#include "dat.h"
#include "track.h"
#include "car.h"
#include "ghost.h"
#include "tb_hud.h"
#include "tb_spl.h"
#include "sound.h"
#include "efx.h"
#include "champ.h"
#include <math.h>
#include <string.h>

#define ROW_Z       2900.0f         /* the rows at height 0 and z 2900 (our y); the camera over
                                       the chosen model's x looks along -z (yaw 0) */
#define SLIDE       0.95f           /* the camera's slide: a factor from 1 to 0 at this a second, the */
#define TAKE        0.8f            /* place by its fourth power; a press is taken under 0.8 */
#define LIT         0.6f            /* the arrow of the slide's way is lit above this (0x12e3) */
#define DT          0.02f           /* a step */
#define PI          3.1415927f
#define TWO_PI      6.2831853f
#define PIC_W       320             /* MENU1L.PCX, MENU3L.PCX, MENU4L.PCX */
#define PIC_H       200
#define MAX_PARTS   (3 * CAR_BLOCKS)    /* blocks drawn: three models (the chosen one and its neighbours) */
#define MAX_DRAWN   (LETTERS > MAX_PARTS ? LETTERS : MAX_PARTS)     /* or a page's cubes */
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
#define LINE_TRACK_WORDS 31         /* PALM TOWN .. SANDROCK: the records' page and titles */
#define LINE_BEST   42              /* BEST TOTAL TIME, BEST LAP TIME */
#define LINE_NAME   46              /* ENTER YOUR NAME */
#define LOADING_Y   0x50            /* the loading screen's LOADING: its row, font 3, colour 0 (0x2a264) */
#define SPR_GRIP    4               /* MENUL.SPR: the grip bar's five steps, the acceleration's */
#define SPR_ACCEL   9
#define CAR_MODELS  7               /* AAUTO00 .. AAUTO06 */
#define LETTERS     80              /* the game's letter cubes (records of 100 bytes at 0x1db74) */
#define CUBE_GAP    0x280           /* a page's letters 640 apart along x, its rows 384 along y, */
#define ROW_GAP     0x180           /* line i of n on row 8 - n + 2i, row 0 at y 0x820 (0x2af30) */
#define PAGE_CAM_Z  4100.0f         /* the pages' camera at (0, 0, 4100) looking along -z (0x29ea0); */
#define BACK_Z      20000.0f        /* leaving them it pulls back to z 20000, its factor from 1 to 0 */
#define BACK_RATE   0.6f            /* at this a second, the place by its fourth power, while it rolls
                                       from -4 pi on at pi a second (0x293e8, 0x294ab) */
#define TURN_RATE   12.566371f      /* a letter turns at 4 pi rad a second (0x2b8b8) */
#define WAVE_RATE   9.424778f       /* the ripple's clock: 3 pi a second, round at 8 pi */
#define OPTION      1234            /* an option's item id (0x4d2) */
#define WORDS       37              /* MENU.TXT's lines 0 .. 36: the pages' words */
#define PAGES       7               /* the game's pages (0x2de4) */
#define LINE_NETWORK 55             /* NETWORK NOT FOUND */
#define SPR_LOGO    14              /* MENUL.SPR: the logos 14 and 15 (0x2df38) */
#define FONT_MSG    0
#define LINE_ENTER  56              /* PRESS ENTER TO CONTINUE */
#define CHAMP_LINES 66              /* CHAMP.TXT's lines, its '*' lines not counted, */
#define CHAMP_BLOCKS 20             /* its blocks (each ends with a line with a '#') */
#define IDLE_STEPS  500             /* 10 s without a key on the championship's screen: ENTER */
#define IDLE_INTRO  1500            /* 30 s on the main menu: the intro (0x2802f, 0x100edc) */
#define ROW_COLOUR  15              /* the standings' table (0x2ce58) */
#define FONT_ROW    1               /* the records' rows (0x2d348) */
#define RING        26              /* the name screen's cubes A .. Z in a ring (0x2c320): radius */
#define RING_R      3500.0f         /* 3500, one every 2 pi / 26; the camera outside at 4500, 1800 */
#define RING_STEP   0.24166098f     /* up, looking 36 degrees down at the chosen letter */
#define RING_CAM_R  4500.0f
#define RING_CAM_H  1800.0f
#define RING_PITCH  -0.6283185f
#define RING_WALK   8.0f            /* its walk: a factor from 1 to 0 at this a second, linear; */
#define RING_TAKE   0.1f            /* a press is taken under 0.1 */
#define REST_Y      160             /* a cube comes to rest at height 160 from 16 times that (an odd */
#define TILT_ON     -0.6283185f     /* one's 500 higher); the chosen letter's tilts to -36 degrees, */
#define TILT_OFF    2.1991148f      /* the rest to 126 (0x2b54c) */

/* per screen (the tracks, the cars) the row and the camera: the gap between two models along x,
   the camera's height and z, its pitch, the models' turn in rad a second (from a quarter turn
   on) */
static const struct row { float gap, cam_h, cam_z, pitch, turn; } rows[2] = {
    { 40000.0f, 4800.0f, 13000.0f, -0.6283185f, 0.7853982f },  /* 0x2c078, 0x2cb88: 36 degrees down */
    { 4000.0f, 500.0f, 4100.0f, -0.3926991f, 1.5707964f },     /* 0x2bdcc, 0x2ca10: 22.5 degrees */
};

const struct menu_part menu_dat_parts[] = {
    { "MENU.MLB", ~0u }, { "ALETTER.3DX", ~0u }, { "AMAP1.3DX", ~0u }, { "AMAP2.3DX", ~0u }, { "AMAP3.3DX", ~0u },
    { "AMAP4.3DX", ~0u }, { "AMAP5.3DX", ~0u }, { "AMAP6.3DX", ~0u }, { "AAUTO00.3DX", ~0u },
    { "AAUTO01.3DX", ~0u }, { "AAUTO02.3DX", ~0u }, { "AAUTO03.3DX", ~0u }, { "AAUTO04.3DX", ~0u },
    { "AAUTO05.3DX", ~0u }, { "AAUTO06.3DX", ~0u }, { 0, 0 } };
const struct menu_part menu_graf_parts[] = {
    { "MENU3L.PCX", ~0u }, { "MENU4L.PCX", ~0u }, { "MENU1L.PCX", ~0u }, { "MENUL.FNT", ~0u }, { "MENUL.SPR", ~0u },
    { "LOAD1L.PCX", ~0u }, { "LOAD2L.PCX", ~0u }, { "LOAD3L.PCX", ~0u }, { "LOAD4L.PCX", ~0u },
    { "LOAD5L.PCX", ~0u }, { "LOAD6L.PCX", ~0u }, { "CHAMP0L.PCX", ~0u }, { "CHAMP1L.PCX", ~0u },
    { "CHAMP2L.PCX", ~0u }, { "CHAMP3L.PCX", ~0u }, { "CHAMP4L.PCX", ~0u }, { "CHAMP5L.PCX", ~0u },
    { "CHAMP6L.PCX", ~0u }, { "CHAMP7L.PCX", ~0u }, { "MENU2L.PCX", ~0u }, { "LOGOL.SPR", ~0u },
    { "ATTRL.FNT", ~0u }, { 0, 0 } };

/* CARDAT.TXT's six lines a car: name, gearbox (0 automatic, 1 manual), km/h, grip and
   acceleration steps (0..4), 0; of its CARS.DTR row the model and the paint job */
static struct car { char name[28]; int gears, kmh, grip, accel; const uint8_t *mesh; uint32_t livery; } car[MENU_CARS];
static const uint8_t *mlb, *model[MENU_TRACKS], *cube, *pic[4], *hud, *graf_dat;
static const struct blit_image *img;
static const struct blit_font *font;
static uint32_t nimg, nfont, graf_len, pal[4][256];     /* the tracks', the cars', the pages', loading */
static char title[2][32], name[MENU_TRACKS][32], gears[2][32], label[3][32], loading[32], best[2][32];
static char name_title[32];
static char words[WORDS][16], network_text[32], enter_text[32], champ_text[CHAMP_LINES][24];
static int on, ncars, cars_on, pages_on, pick, way, block[CHAMP_BLOCKS + 1], idle;   /* on: the screen */
static float turn[2] = { 1.5707964f, 1.5707964f }, cam_x, from_x, to_x, slide;

/* The game's pages (0x2de4, items of 16 bytes: MENU.TXT's line, id, value, highest value; an
   option, id 1234, shows its line + its value), by the game's numbers: 0 the main menu (NORMAL
   GAME 0, CHAMPIONSHIP 1, SPECIAL MODES 2, NETWORK GAME 3, OPTIONS 4), 1 the options with the
   game's values at the start (the difficulty, LAP NUMBER, SPEECH, MUSIC; VIEW RECORDS 21). The
   game's first option, DETAIL, is left out: the core always draws everything, as far as HI
   reaches; CHOICE.DAT keeps it at HI (2). 3 time attack's NEW 24 and RESUME 25, 4 the special modes (TIME ATTACK 28, CONE CARNAGE
   29, SLALOM 30), 5 the championship's NEW 26 and RESUME 27, 6 VIEW RECORDS' six tracks (id 31,
   the track 0 .. 5 as the highest value) */
static struct item { int line, id, value, max; } items[PAGES][6] = {
    [0] = { { 0, 0, 0, 0 }, { 1, 1, 0, 0 }, { 2, 2, 0, 0 }, { 3, 3, 0, 0 }, { 4, 4, 0, 0 } },
    [1] = { { 8, OPTION, 1, 2 }, { 13, OPTION, 0, 3 }, { 17, OPTION, 1, 1 }, { 19, OPTION, 1, 1 },
            { 21, 21, 0, 0 } },
    [3] = { { 24, 24, 0, 0 }, { 25, 25, 0, 0 } },
    [4] = { { 28, 28, 0, 0 }, { 29, 29, 0, 0 }, { 30, 30, 0, 0 } },
    [5] = { { 26, 26, 0, 0 }, { 27, 27, 0, 0 } },
    [6] = { { 31, 31, 0, 0 }, { 32, 31, 0, 1 }, { 33, 31, 0, 2 }, { 34, 31, 0, 3 }, { 35, 31, 0, 4 },
            { 36, 31, 0, 5 } } };
static const int page_lines[PAGES] = { [0] = 5, [1] = 5, [3] = 2, [4] = 3, [5] = 2, [6] = 6 };
static int mode = CAR_NORMAL;           /* the game mode the last game was started in (0x1d25c) */
static const int laps[4] = { 3, 5, 10, 25 };            /* 0x21e0 + 1 */

/* where a page's words fly in from (x, y, z; an x or y of 0: from where the letter lands) and
   their swing in x and y, a row a page in turn (0x2218) */
static const int16_t fly[8][5] = {
    { 200, 200, -4500, 1000, 1000 }, { 3000, -3000, 1500, 200, 800 }, { 1000, 1000, -8000, 3000, 3000 },
    { 3000, 0, 5000, 0, 0 }, { 2000, 50, 4500, 0, 0 }, { 0, 0, -10500, 500, 800 },
    { -2000, 2000, 3500, 0, 0 }, { 50, 5000, 4500, 3000, 3000 } };

/* A letter's cube (0x1db74): its line, its character - 32 (the frame of the cube's texture),
   where it lands, where it flies in from and its swing, the fly's progress from 1 to 0 in a
   second, its turn about x (0 the front to the camera, the letter bright; pi the back, dim),
   its place now (x left, y up, z to the camera, as the game's) and its heading (the name's
   ring; turned about y after x) */
static struct letter { int line, code; float t[3], s[3], ax, ay, p, a, pos[3], h; } let[LETTERS];
static int nlet, page, shown, sel[PAGES], style, turning, leaving, network;    /* shown: the letters' page */
static float wave, logo[2], cam_z = PAGE_CAM_Z, back_f, roll;
static uint32_t ticks;                                  /* 100 a second, for the blinking */
static int rec_track, rec_sel;          /* VIEW RECORDS: the track (0x20988), the total chosen (0x2098c) */
static int page_idle, idle_out;         /* steps on the main menu without a key (0x1d248); gone */
/* the name screen's cubes (records of 0x1db74 too): the wait before it drops, its fall's
   progress p (the height top + p^2 (rest - top)), the height it drops from, falling 1, rising 0,
   at rest -1, its tilt; the alphabet's place (0x1d238, all three letters'), the letter being
   set (0x1d240), the camera's walk (0x29530): its factor 1 to 0, the place it set out from */
static struct ring { float wait, p, tilt; int top, state; } ring[RING];
static int alpha, name_at, walk_from;
static float walk;
static void records_enter(int n);
static void name_enter(void);
static void room(const char *file, int x0, int y0, int w, int h);

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
            while(i < e && m < max - 1) {
                /* the Pocket's button for the PC's key: "PRESS ENTER" -> "PRESS START" (START
                   takes a menu's choice, as ENTER does) */
                if(e - i >= 11 && !memcmp(t + i, "PRESS ENTER", 11)) {
                    for(const char *r = "PRESS START"; *r && m < max - 1; r++) out[m++] = *r;
                    i += 11;
                    if(e - i >= 9 && !memcmp(t + i, " TO START", 9)) i += 9;    /* not START TO START */
                    continue;
                }
                out[m++] = (char)t[i++];
            }
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
    if(!(cube = in_place(menu_dat, menu_size, "ALETTER.3DX", 0x80, &len)) || rd32(cube + 0x14) < 1) return 0;
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
    static const char *const pics[3] = { "MENU3L.PCX", "MENU4L.PCX", "MENU1L.PCX" };
    if(cap < 4 * PIC_W * PIC_H) return 0;
    for(int k = 0; k < 4; k++) pic[k] = mem + k * PIC_W * PIC_H;
    for(int k = 0; k < 3; k++) {
        const uint8_t *p = in_place(graf, graf_size, pics[k], 0, &len);
        if(!p || !pcx(p, len, (uint8_t *)pic[k], pal[k])) return 0;
    }
    graf_dat = graf;
    graf_len = graf_size;
    uint8_t *at = mem + 4 * PIC_W * PIC_H;
    uint32_t spr_n = unpack(graf, graf_size, "MENUL.SPR", scratch, scratch_cap);
    uint32_t used = (spr_n + 3) & ~3u;
    uint32_t fnt_n = used < scratch_cap ? unpack(graf, graf_size, "MENUL.FNT", scratch + used, scratch_cap - used) : 0;
    if(!spr_n || !fnt_n ||
       !tb_hud_build(scratch, spr_n, scratch + used, fnt_n, scratch, 0, scratch, 0, scratch, 0, at,
                     cap - 4 * PIC_W * PIC_H)) return 0;
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
    for(int k = 0; k < WORDS; k++) ok &= text_line(scratch, tn, k, words[k], sizeof words[k]);
    ok &= text_line(scratch, tn, LINE_NETWORK, network_text, sizeof network_text);
    ok &= text_line(scratch, tn, LINE_ENTER, enter_text, sizeof enter_text);
    for(int k = 0; k < 2; k++) ok &= text_line(scratch, tn, LINE_BEST + k, best[k], sizeof best[k]);
    ok &= text_line(scratch, tn, LINE_NAME, name_title, sizeof name_title);
    ok &= in_place(graf, graf_size, "MENU2L.PCX", 0, &len) != 0;
    /* CHAMP.TXT's blocks, as 0x2c698 splits it: each up to a line with a '#' */
    tn = unpack(local, local_size, "CHAMP.TXT", scratch, scratch_cap);
    int nb = 0;
    for(int k = 0; k < CHAMP_LINES && nb < CHAMP_BLOCKS && text_line(scratch, tn, k, champ_text[k], sizeof champ_text[k]); k++)
        if(strchr(champ_text[k], '#')) block[++nb] = k + 1;
    ok &= nb == CHAMP_BLOCKS;
    for(int k = 0; k < 8; k++) {
        char n[16] = "CHAMP0L.PCX";
        n[5] = (char)('0' + k);
        ok &= in_place(graf, graf_size, n, 0, &len) != 0;
    }
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
    return ok && ncars > 0 && nimg > SPR_LOGO + 1 && nfont > FONT_TITLE;
}

int menu_cars(void)
{
    return ncars;
}

const uint32_t *menu_palette(void)
{
    return pal[on == MENU_PAGE_SCREEN ? 2 : on == MENU_CAR_SCREEN ? 1 : on == MENU_TRACK_SCREEN ? 0 : 3];
}

void menu_options(struct menu_options *o)
{
    const struct item *it = items[1];
    *o = (struct menu_options){ 2, it[0].value, laps[it[1].value], it[2].value, it[3].value };
}

/* the options page's values as CHOICE.DAT has them (0x1bbb8 copies them to 0x102de8's
   items), and back on every change (0x1be5c copies them there) */
void menu_options_load(void)
{
    struct item *it = items[1];
    const int32_t v[4] = { choice.difficulty, choice.laps, choice.speech, choice.music };
    it[0].max = choice.difficulties;
    for(int k = 0; k < 4; k++) it[k].value = v[k] >= 0 && v[k] <= it[k].max ? v[k] : it[k].value;
}

static void options_store(void)
{
    const struct item *it = items[1];
    choice.detail = 2;                          /* HI: what the core draws */
    choice.difficulty = it[0].value;
    choice.laps = it[1].value;
    choice.speech = it[2].value;
    choice.music = it[3].value;
}

int menu_mode(void)
{
    return mode;
}

void menu_fresh(void)
{
    memset(sel, 0, sizeof sel);
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

/* The page's letters (0x29838, 0x2af30): each line's centred, a cube a character, the spaces
   too; flying in (the page entered) from the next row of fly, the start turned 0.09 rad (in
   whole units) a letter, the chosen line's front to the camera, the others' backs; else (a page
   turned to, 0x2b380) simply there, the chosen line a quarter turned, to finish the turn */
static void page_build(int flying)
{
    const struct item *it = items[page];
    const int16_t *f = fly[style];
    const float c = cosf(0.09f), s = sinf(0.09f);
    float sx = f[0], sy = f[1];
    nlet = 0;
    shown = page;
    for(int i = 0; i < page_lines[page]; i++) {
        const char *w = words[it[i].line + (it[i].id == OPTION ? it[i].value : 0)];
        int len = (int)strlen(w);
        float x = (float)(CUBE_GAP / 2 * (len - 1)), y = (float)(0x820 - ROW_GAP * (8 - page_lines[page] + 2 * i));
        for(int k = 0; k < len && nlet < LETTERS; k++, x -= CUBE_GAP) {
            struct letter *l = &let[nlet++];
            int ch = (unsigned char)w[k];
            *l = (struct letter){ i, ch < 32 || ch > 107 ? 0 : ch - 32, { x, y, 0.0f }, { sx != 0.0f ? sx : x,
                                  sy != 0.0f ? sy : y, f[2] }, f[3], f[4], flying ? 1.0f : 0.0f,
                                  i != sel[page] ? PI : flying ? 0.0f : 0.5f * PI, { x, y, 0.0f }, 0.0f };
            float ox = sx;
            sx = (float)(int)(ox * c + sy * s);
            sy = (float)(int)(sy * c - ox * s);
        }
    }
    style = (style + 1) % 8;
}

static void stage_enter(void);

void menu_enter(int screen, int n)
{
    on = screen;
    pages_on = screen == MENU_PAGE_SCREEN;
    if(screen == MENU_CHAMP_SCREEN) stage_enter();
    if(screen == MENU_RECORDS_SCREEN) records_enter(n);
    if(screen == MENU_NAME_SCREEN) name_enter();
    if(screen == MENU_CHAMP_SCREEN || screen == MENU_RECORDS_SCREEN || screen == MENU_NAME_SCREEN) return;
    if(pages_on) {                              /* 0x29ea0: the logos 400 and 800 left of their places */
        if(n >= 1 && n <= PAGES && page_lines[n - 1]) page = n - 1;
        page_build(1);
        turning = leaving = network = page_idle = idle_out = 0;
        cam_z = PAGE_CAM_Z;
        roll = 0.0f;
        logo[0] = (float)((PIC_W - (int)img[SPR_LOGO + 1].w) / 2 - 400);
        logo[1] = (float)((PIC_W - (int)img[SPR_LOGO].w) / 2 - 800);
        return;
    }
    cars_on = screen == MENU_CAR_SCREEN;
    pick = n < 1 ? 0 : n > choices() ? choices() - 1 : n - 1;
    cam_x = from_x = to_x = -rows[cars_on].gap * (float)pick;
    slide = 0.0f;
    way = 0;
}

/* One step of the page's letters (0x2b8b8): each on its way in (the swing round its path,
   1 - cos of the progress towards where it lands), rippling in z by 150 sin(2 pi fx + pi fy + the
   clock k) (the chosen line fx 0.5, k 1/4); the chosen line turning to its front, the others to
   their backs, at 4 pi a second. While the page is turned (another page, an option changed) all
   turn away, and once the chosen line is past a quarter turn the new page is there. */
static void letters_step(void)
{
    float least = PI;
    if((wave += DT * WAVE_RATE) > 4.0f * TWO_PI) wave = 0.0f;
    for(int i = 0; i < nlet; i++) {
        struct letter *l = &let[i];
        int chosen = l->line == sel[shown];
        float e = 1.0f - cosf(0.5f * PI * l->p), w = TWO_PI * l->p;
        l->pos[0] = cosf(w) * l->ax * l->p + l->t[0] + e * (l->s[0] - l->t[0]);
        l->pos[1] = sinf(w) * l->ay * l->p + l->t[1] + e * (l->s[1] - l->t[1]);
        float fx = chosen ? 0.5f : (l->pos[0] + 4608.0f) / 9216.0f, fy = (l->pos[1] + 2080.0f) / 4160.0f;
        l->pos[2] = l->t[2] + e * (l->s[2] - l->t[2]) + 150.0f * sinf(TWO_PI * fx + PI * fy + wave * (chosen ? 0.25f : 1.0f));
        if(l->p > 0.0f && (l->p -= DT) < 0.0f) l->p = 0.0f;
        if(chosen && !turning) {
            if((l->a -= DT * TURN_RATE) < 0.0f) l->a = 0.0f;
        } else if((l->a += DT * TURN_RATE) > PI) l->a = PI;
        if(chosen && l->a < least) least = l->a;
    }
    if(turning && least > 0.5f * PI) {
        page_build(0);
        turning = 0;
    }
}

/* whether every letter has landed (0x2b2d0: their progress adds up to 0) */
static int landed(void)
{
    for(int i = 0; i < nlet; i++)
        if(let[i].p > 0.0f) return 0;
    return 1;
}

/* A step of the pages (menu_step): the letters, the logos sliding in (0x2df38), the camera
   pulling back once a game is chosen; then the keys (0x28080, 0x28ac4) */
static int page_step(uint32_t press)
{
    struct item *it = &items[page][sel[page]];
    if(!leaving) letters_step();                /* leaving, the letters stand (0x294ab) */
    ticks += 2;
    for(int k = 0; k < 2; k++) {
        float to = (float)((PIC_W - (int)img[SPR_LOGO + 1 - k].w) / 2);
        if((logo[k] += DT * (k ? 700.0f : 350.0f)) > to) logo[k] = to;
    }
    if(leaving) {
        float f2 = back_f * back_f;
        cam_z = BACK_Z + (PAGE_CAM_Z - BACK_Z) * f2 * f2;
        if((back_f -= DT * BACK_RATE) < 0.0f) back_f = 0.0f;
        roll += DT * PI;
        return 0;
    }
    /* 0x288bf: the time without a key counts on the main menu alone (screen 0, no page
       opened); over 30 s the menu is left for the intro (0x2802f) */
    page_idle = press ? 0 : page_idle + (page == 0);
    if(page_idle > IDLE_INTRO) idle_out = 1;
    if(idle_out) return MENU_IDLE;
    if((press & KEY_UP) && sel[page] > 0) {     /* no wrap (0x2848e) */
        sel[page]--;
        network = 0;
        snd_play(SND_MENU_LINE);
    } else if((press & KEY_DOWN) && sel[page] < page_lines[page] - 1) {
        sel[page]++;
        network = 0;
        snd_play(SND_MENU_LINE);
    } else if(it->id == OPTION && (press & (KEY_LEFT | KEY_RIGHT | KEY_B | KEY_START))) {
        if(landed()) {                          /* 0x297d4, the game's ENTER: the next value */
            items[1][0].max = choice.difficulties;      /* AFTERBURNER once the Bullet is won */
            it->value = (it->value + (press & KEY_LEFT ? it->max : 1)) % (it->max + 1);
            options_store();
            turning = 1;
            snd_play(SND_MENU_OPTION);
        }
    } else if(press & (KEY_B | KEY_START)) {
        /* the items that start a game, by id: NORMAL GAME (0x28b80), time attack's NEW and RESUME
           (0x28c0f, 0x28c3c), the championship's (0x28c6b, 0x28c9e), CONE CARNAGE (0x28bb0),
           SLALOM (0x28bdf), and a track's records (0x28d0b) the same way; those that open a
           page: CHAMPIONSHIP (0x28b3a), SPECIAL MODES (0x28eab), OPTIONS (0x28db2), TIME ATTACK
           (0x28e0d), VIEW RECORDS (0x28cd8) */
        int id = it->id, game = id == 0 ? CAR_NORMAL : id == 24 || id == 25 ? CAR_ATTACK :
                 id == 26 || id == 27 ? CAR_CHAMP : id == 29 ? CAR_CONES : id == 30 ? CAR_SLALOM : 0;
        if(game || id == 31) {
            if(game) mode = game;
            if(game == CAR_ATTACK) ghost_resume = id == 25;
            leaving = 1;
            back_f = 1.0f;
            roll = -4.0f * PI;
            snd_play(SND_MENU_GAME);
            return id == 31 ? MENU_RECORDS + it->max + 1 : mode == CAR_CHAMP ? id + 1 : 1;
        }
        if(id == 3) network = 1;                /* the game without a network (0x28d78), silent */
        else snd_play(SND_MENU_PAGE);
        int to = id == 1 ? 5 : id == 2 ? 4 : id == 4 ? 1 : id == 28 ? 3 : id == 21 ? 6 : -1;
        if(to >= 0) {
            page = to;
            turning = 1;
        }
    } else if((press & (KEY_A | KEY_SELECT)) && page != 0) {    /* the game's ESC (0x280cb): */
        page = page == 3 ? 4 : page == 6 ? 1 : 0;               /* the page before */
        turning = 1;
        snd_play(SND_MENU_PAGE);
    }
    return 0;
}

/* The camera of the name screen (0x29530): outside the ring at the alphabet's place k, turned
   to it */
static void ring_cam(int k, float *x, float *z, float *h)
{
    float a = -RING_STEP * (float)k;
    *x = sinf(a) * RING_CAM_R;
    *z = cosf(a) * RING_CAM_R;
    *h = RING_STEP * (float)k;
}

/* where it is on its walk from the place it last stood at to the alphabet's place (linear; the
   heading the shorter way round) */
static struct track_cam name_view(void)
{
    float x0, z0, h0, x1, z1, h1;
    ring_cam(walk_from, &x0, &z0, &h0);
    ring_cam(alpha, &x1, &z1, &h1);
    if(h0 - h1 > PI) h1 += TWO_PI;
    if(h1 - h0 > PI) h0 += TWO_PI;
    return (struct track_cam){ x1 + (x0 - x1) * walk, z1 + (z0 - z1) * walk, RING_CAM_H, h1 + (h0 - h1) * walk,
                               RING_PITCH, 0.0f };
}

/* ENTER YOUR NAME (0x2c320): MENU2L.PCX; the alphabet at the name's first letter, the first
   letter being set; the cubes dropping in one after the other, 0.04 s apart */
static void name_enter(void)
{
    room("MENU2L.PCX", 0, 0, 0, 0);
    alpha = walk_from = choice.name[0] >= 'A' && choice.name[0] <= 'Z' ? choice.name[0] - 'A' : 0;
    name_at = 0;
    walk = 0.0f;
    for(int k = 0; k < RING; k++)
        ring[k] = (struct ring){ 0.04f * (float)k, 0.0f, TILT_OFF, 16 * REST_Y + (k & 1) * 500, 1 };
    nlet = 0;
}

/* A step of the name screen. The keys (0x28080): B or START go on (1), A or SELECT back (-1),
   both silent; down and up the letter being set (smenu6), left and right turn the alphabet one
   place, its letter then the one being set (silent), once the camera has walked most of the way
   (0x2865a, 0x28810). Then the camera's walk, then each cube (0x2cd74, 0x2b54c): once its wait is
   over it drops (p to 1 at 2.5 a second), bounces up again to 0.4 of the height (p to 0 at 2 a
   second) until that is under where it rests; the alphabet's letter tilts to the camera, the
   rest away, at 4 pi a second. */
static int name_step(uint32_t press)
{
    if(press & (KEY_B | KEY_START)) return 1;
    if(press & (KEY_A | KEY_SELECT)) return -1;
    if((press & KEY_DOWN) && name_at < 2) {
        name_at++;
        snd_play(SND_MENU_LETTER);
    } else if((press & KEY_UP) && name_at > 0) {
        name_at--;
        snd_play(SND_MENU_LETTER);
    } else if((press & (KEY_LEFT | KEY_RIGHT)) && walk < RING_TAKE) {
        walk_from = alpha;
        alpha = (alpha + (press & KEY_LEFT ? RING - 1 : 1)) % RING;
        choice.name[name_at] = (char)('A' + alpha);
        walk = 1.0f;
    }
    if(walk > 0.0f && (walk -= DT * RING_WALK) < 0.0f) walk = 0.0f;
    nlet = 0;
    for(int k = 0; k < RING; k++) {
        struct ring *r = &ring[k];
        if((r->wait -= DT) >= 0.0f) continue;   /* not there yet */
        r->wait = 0.0f;
        float a = -RING_STEP * (float)k, y = (float)r->top + r->p * r->p * (float)(REST_Y - r->top);
        let[nlet++] = (struct letter){ .code = k + 'A' - 32, .a = r->tilt,
                                       .pos = { sinf(a) * RING_R, y, cosf(a) * RING_R }, .h = RING_STEP * (float)k };
        if(r->state > 0 && (r->p += DT * 2.5f) > 1.0f) {
            r->p = 1.0f;
            r->state = 0;
            if((r->top = (int)((float)r->top * 0.4f)) < REST_Y) {
                r->state = -1;
                r->top = REST_Y;
            }
        } else if(r->state == 0 && (r->p -= DT * 2.0f) < 0.0f) {
            r->p = 0.0f;
            r->state = 1;
        }
        if(k != alpha) {
            if((r->tilt += DT * TURN_RATE) > TILT_OFF) r->tilt = TILT_OFF;
        } else if((r->tilt -= DT * TURN_RATE) < TILT_ON) r->tilt = TILT_ON;
    }
    return 0;
}

int menu_step(uint32_t keys, uint32_t keys_before)
{
    const struct row *r = &rows[cars_on];
    uint32_t press = keys & ~keys_before;
    if(on == MENU_NAME_SCREEN) return name_step(press);
    if(on == MENU_RECORDS_SCREEN) {             /* 0x28359, 0x2844f: up and down choose a total */
        if(press & (KEY_A | KEY_SELECT)) return -1;
        if((press & KEY_DOWN) && rec_sel < 4) {
            rec_sel++;
            snd_play(SND_MENU_RECORD);
        } else if((press & KEY_UP) && rec_sel > 0) {
            rec_sel--;
            snd_play(SND_MENU_RECORD);
        }
        return 0;
    }
    if(on == MENU_CHAMP_SCREEN) {               /* 0x2d301, 0x2829d */
        ticks += 2;
        idle = press ? 0 : idle + 1;
        if((press & (KEY_B | KEY_START)) || idle > IDLE_STEPS) return 1;
        return (press & (KEY_A | KEY_SELECT)) && champ_at(choice.stage).kind == CHAMP_TEXT ? -1 : 0;
    }
    if(pages_on) return page_step(press);
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
    return press & (KEY_A | KEY_SELECT) ? -1 : 0;
}

void menu_backdrop(struct blit_list *l)
{
    blit_copy(l, pic[on == MENU_PAGE_SCREEN ? 2 : on == MENU_CAR_SCREEN ? 1 : on == MENU_TRACK_SCREEN ? 0 : 3], PIC_W,
              PIC_H, PIC_W, 0, 0, BLIT_COPY | BLIT_OPAQUE, 0);
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

/* The page's or the name's cubes, the furthest first (by the distance from the camera to the
   cube's middle), each turned about x by its turn (y up towards z), then about y by its heading,
   its face the frame of its character: the record's +0x20 0x35e (character - 32) + 0x226, of
   which the phase is a quarter (geom.h) */
static int letter_blocks(const struct track_cam *cam, struct track_block *b)
{
    float key[LETTERS];
    const uint8_t *blk = cube + rd32(cube + 0x18);
    int n = 0;
    for(int k = 0; k < nlet; k++) {
        const struct letter *l = &let[k];
        const float c = cosf(l->a), s = sinf(l->a), ch = cosf(l->h), sh = sinf(l->h);
        /* the world's x, y and height are the game's x, z and y */
        struct track_block t = { cube, blk, { ch, -sh * s, -sh * c, sh, ch * s, ch * c, 0.0f, c, -s },
                                 { l->pos[0], l->pos[2], l->pos[1] }, (0x35Eu * (uint32_t)l->code + 0x226u) >> 2 };
        float dx = t.o[0] - cam->x, dy = t.o[1] - cam->y, dh = t.o[2] - cam->h, d = dx * dx + dy * dy + dh * dh;
        int i = n++;
        for(; i > 0 && key[i - 1] < d; i--) {
            b[i] = b[i - 1];
            key[i] = key[i - 1];
        }
        b[i] = t;
        key[i] = d;
    }
    return n;
}

void menu_scene(volatile uint8_t *fb)
{
    static const struct rs_clip clip = { 0, 0, PIC_W, PIC_H };
    const struct row *r = &rows[cars_on];
    const struct track_cam cam = pages_on ? (struct track_cam){ 0.0f, cam_z, 0.0f, 0.0f, 0.0f, roll }
                               : on == MENU_NAME_SCREEN ? name_view()
                                                        : (struct track_cam){ cam_x, r->cam_z, r->cam_h, 0.0f, r->pitch, 0.0f };
    static struct track_block b[MAX_DRAWN];
    int n = pages_on || on == MENU_NAME_SCREEN ? letter_blocks(&cam, b) : on == MENU_CAR_SCREEN ? car_blocks(&cam, b)
          : on == MENU_TRACK_SCREEN ? map_blocks(&cam, b) : 0;
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

/* a string centred on the screen at row y (0x2e598) in a colour, or in the texts' */
static void centred_in(struct blit_list *l, int y, const char *s, uint32_t f, int colour)
{
    int w = blit_text(l, hud, img, &font[f], 0, 0, s, colour, 0);
    blit_text(l, hud, img, &font[f], (PIC_W - w) / 2, y, s, colour, 1);
}

static void centred(struct blit_list *l, int y, const char *s, uint32_t f)
{
    centred_in(l, y, s, f, COLOUR);
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
static void stage_text(struct blit_list *l);
static void records_text(struct blit_list *l);
static void name_text(struct blit_list *l);

void menu_text(struct blit_list *l)
{
    int lit = slide > LIT;
    if(on == MENU_CHAMP_SCREEN) stage_text(l);
    if(on == MENU_RECORDS_SCREEN) records_text(l);
    if(on == MENU_NAME_SCREEN) name_text(l);
    if(on == MENU_CHAMP_SCREEN || on == MENU_RECORDS_SCREEN || on == MENU_NAME_SCREEN) return;
    if(pages_on) {          /* the logos (0x2df38), off the screen not drawn; NETWORK NOT FOUND blinking */
        for(int k = 0; k < 2; k++) {
            const struct blit_image *s = &img[SPR_LOGO + 1 - k];
            int x = (int)(logo[k] + (k ? 0.0f : 4.0f));
            if(x + s->x + (int)s->w > 0) sprite(l, x, k ? 0x10 : 0, SPR_LOGO + 1 - (uint32_t)k);
        }
        if(network && (ticks & 0x7F) < 0x48) centred(l, 0xBE, network_text, FONT_MSG);
        return;
    }
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
    if(!p || !pcx(p, len, (uint8_t *)pic[3], pal[3])) return 0;
    blit_copy(l, pic[3], PIC_W, PIC_H, PIC_W, 0, 0, BLIT_COPY | BLIT_OPAQUE, 0);
    int w = blit_text(l, hud, img, &font[FONT_TITLE], 0, 0, loading, 0, 0);
    blit_text(l, hud, img, &font[FONT_TITLE], (PIC_W - w) / 2, LOADING_Y, loading, 0, 1);
    return pal[3];
}

/* A picture of GFX/GRAF.DAT, unpacked from its place there, into the loading screen's room (black
   without it), the box (x0, y0, w x h) darkened twice through its palette (0x25cc8 with 0x25870's
   table, 0.75), as the game darkens it every frame */
static void room(const char *file, int x0, int y0, int w, int h)
{
    uint32_t len;
    const uint8_t *p = graf_dat ? in_place(graf_dat, graf_len, file, 0, &len) : 0;
    uint8_t *px = (uint8_t *)pic[3];
    if(!p || !pcx(p, len, px, pal[3])) memset(px, 0, PIC_W * PIC_H);
    if(!w) return;
    uint8_t dark[256];
    efx_darken_table(pal[3], dark, 0.75f);
    for(int y = y0; y < y0 + h; y++)
        for(int x = x0; x < x0 + w; x++) px[y * PIC_W + x] = dark[dark[px[y * PIC_W + x]]];
}

/* The championship's screen (0x2c698): the stage's picture CHAMPnL.PCX, for the standings the box
   (16, 36, 288 x 136) darkened */
static void stage_enter(void)
{
    struct champ_stage st = champ_at(choice.stage);
    char file[16] = "CHAMP0L.PCX";
    file[5] = (char)('0' + st.pic);
    room(file, 0x10, 0x24, st.kind == CHAMP_STANDINGS ? 0x120 : 0, 0x88);
    idle = 0;
}

/* VIEW RECORDS of track n (0x2c844): its LOADnL.PCX, the box (48, 36, 226 x 160) darkened, the
   first total chosen */
static void records_enter(int n)
{
    char file[16] = "LOAD1L.PCX";
    rec_track = n >= 1 && n <= MENU_TRACKS ? n - 1 : 0;
    rec_sel = 0;
    file[4] = (char)('1' + rec_track);
    room(file, 0x30, 0x24, 0xE2, 0xA0);
}

/* v at p with at least two digits (%02d): the end */
static char *dec02(char *p, int v)
{
    if(v < 10) *p++ = '0';
    return dec(p, v);
}

/* A record's row as the game prints it (0x2d348: "\x01%d. %3s %02d.%02d.%02d R", or two spaces
   at the end; fixed pitch): its place i + 1, the name right-aligned in three, the time in
   minutes, seconds and hundredths, cut off; then a space and r */
static void record_row(char *s, int i, const char *who, float t, char r)
{
    int n = 0, whole = (int)t;
    while(n < 4 && who[n]) n++;
    *s++ = 1;
    s = dec(s, i + 1);
    *s++ = '.';
    *s++ = ' ';
    for(int k = n; k < 3; k++) *s++ = ' ';
    memcpy(s, who, (size_t)n);
    s += n;
    *s++ = ' ';
    s = dec02(s, whole / 60);
    *s++ = '.';
    s = dec02(s, whole % 60);
    *s++ = '.';
    s = dec02(s, (int)((t - (float)whole) * 100.0f));
    *s++ = ' ';
    *s++ = r;
    *s = 0;
}

/* VIEW RECORDS (0x2d348): the track's name centred at row 2 (font 3), BEST TOTAL TIME at row 0x28
   and BEST LAP TIME at 0x78 (font 0), under each its five times, 10 rows apart from 0x3c and
   0x8c, centred in font 1: the totals with an R for a saved replay (the core keeps none); all in
   colour 0, the total chosen in colour 1 */
static void records_text(struct blit_list *l)
{
    char s[40];
    const struct choice_rec *race = &choice.race[rec_track], *lap = &choice.lap[rec_track];
    centred_in(l, 2, words[LINE_TRACK_WORDS + rec_track], FONT_TITLE, 0);
    centred_in(l, 0x28, best[0], FONT_MSG, 0);
    centred_in(l, 0x78, best[1], FONT_MSG, 0);
    for(int i = 0; i < 5; i++) {
        record_row(s, i, race->name[i], race->time[i], race->flag[i] ? 'R' : ' ');
        centred_in(l, 0x3C + 10 * i, s, FONT_ROW, i == rec_sel);
        record_row(s, i, lap->name[i], lap->time[i], ' ');
        centred_in(l, 0x8C + 10 * i, s, FONT_ROW, 0);
    }
}

/* ENTER YOUR NAME (0x2e4f8): the title centred at row 2 (font 3), the name at (0x83, 0x4c) in
   font 2, fixed pitch, a dash 12 rows under the letter being set, 21 pixels a letter */
static void name_text(struct blit_list *l)
{
    char s[8] = { 1, choice.name[0], choice.name[1], choice.name[2], 0 };
    centred(l, 2, name_title, FONT_TITLE);
    text(l, 0x83, 0x4C, s, FONT_NAME);
    text(l, 0x83 + 21 * name_at, 0x58, "\1-", FONT_NAME);
}

/* line i of CHAMP.TXT's block b */
static const char *champ_line(int b, int i)
{
    return champ_text[block[b] + i];
}

/* the standings' table in font 0, colour 15 */
static int row_text(struct blit_list *l, int x, int y, const char *s, int draw)
{
    return blit_text(l, hud, img, &font[FONT_MSG], x, y, s, ROW_COLOUR, draw);
}

/* v as two digits at p: the end */
static char *dec2(char *p, int v)
{
    *p++ = (char)('0' + v / 10 % 10);
    *p++ = (char)('0' + v % 10);
    return p;
}

/* s, a ':' and the end of s at p (the standings' last two lines) */
static char *with_colon(char *p, const char *s)
{
    size_t n = strlen(s);
    memcpy(p, s, n);
    p[n] = ':';
    return p + n + 1;
}

/* The stage's texts (0x2ce58). The standings: the league's name centred at row 2 (font 2);
   the heads TRACK, POS, TIME, SCORE at row 48; a row 14 high from 62 on for each race: -n-,
   the place, the time (min.s.hundredths) and the points in fixed pitch (byte 1), dashes for
   a race not run, 10, 00.00.00 and 0 for one out of time; 4 lower TOTAL SCORE:nn ending at
   x 276, 24 lower QUALIFY:n centred. Else the block's lines centred from row 32, a font height
   apart (font 3, 2 for the leagues won and the end). PRESS ENTER TO CONTINUE blinks at row 190. */
static void stage_text(struct blit_list *l)
{
    struct champ_stage st = champ_at(choice.stage);
    char s[40], *e;
    if(st.kind == CHAMP_STANDINGS) {
        static const int16_t head_x[4] = { 0x58, 0x1C, 0xAA, 0xF6 };   /* POS, TRACK, TIME, SCORE */
        centred(l, 2, champ_line(6 + st.league, 0), FONT_NAME);
        for(int k = 0; k < 4; k++) row_text(l, head_x[k], 0x30, champ_line(k, 0), 1);
        int y = 0x3E;
        for(int i = 0; i < 6; i++, y += 0xE) {
            s[0] = '-';
            memcpy(dec(s + 1, i + 1), "-", 2);
            row_text(l, 0x26, y, s, 1);
            s[0] = 1;
            if(choice.pos[i] == -1 || (choice.failed == 1 && i == st.race)) {
                int out = choice.pos[i] != -1;
                strcpy(s + 1, out ? "10" : "-");
                row_text(l, 0x58, y, s, 1);
                strcpy(s + 1, out ? "00.00.00" : "--.--.--");
                row_text(l, 0x8C, y, s, 1);
                strcpy(s + 1, out ? "0" : "-");
                row_text(l, 0x10A, y, s, 1);
                continue;
            }
            *dec(s + 1, choice.pos[i]) = 0;
            row_text(l, 0x58, y, s, 1);
            float t = choice.time[i];
            int whole = (int)t;
            e = dec2(s + 1, whole / 60);
            *e++ = '.';
            e = dec2(e, whole % 60);
            *e++ = '.';
            *dec2(e, (int)((t - (float)whole) * 100.0f)) = 0;
            row_text(l, 0x8C, y, s, 1);
            *dec(s + 1, choice.pts[i]) = 0;
            row_text(l, 0x10A, y, s, 1);
        }
        e = with_colon(s, champ_line(5, 0));
        *e++ = 1;
        *dec2(e, champ_total()) = 0;
        row_text(l, 0x114 - row_text(l, 0, 0, s, 0), y + 4, s, 1);
        *dec(with_colon(s, champ_line(4, 0)), st.need) = 0;
        row_text(l, (PIC_W - row_text(l, 0, 0, s, 0)) / 2, y + 4 + 0x18, s, 1);
    } else {
        int y = 0x20;
        for(int i = block[st.text]; i + 1 < block[st.text + 1]; i++, y += (int)font[st.font].height)
            centred(l, y, champ_text[i], (uint32_t)st.font);
    }
    if((ticks & 0x7F) < 0x48) centred(l, 0xBE, enter_text, FONT_MSG);
}
