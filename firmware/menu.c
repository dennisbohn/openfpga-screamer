/*
 * The track screen (menu.h). The numbers are the game's as openscreamer read them (NOTES.md
 * section 26): the row and the camera of 0x2cb88, the slide of 0x29530, the 2D layout of
 * 0x2e384.
 */
#include "menu.h"
#include "hw.h"
#include "dat.h"
#include "track.h"
#include "tb_hud.h"
#include <math.h>
#include <string.h>

#define ROW_GAP     40000.0f        /* between two tracks' models along x; the row at height 0 */
#define ROW_Z       2900.0f         /* and at z 2900 (our y) */
#define CAM_H       4800.0f         /* the camera over the chosen model's x, at z 13000, looking */
#define CAM_Z       13000.0f        /* along -z (yaw 0), 36 degrees down */
#define CAM_PITCH   -0.6283185f
#define TURN        0.7853982f      /* the models' turn, rad a second; from a quarter turn on */
#define SLIDE       0.95f           /* the camera's slide: a factor from 1 to 0 at this a second, the */
#define TAKE        0.8f            /* place by its fourth power; a press is taken under 0.8 */
#define DT          0.02f           /* a step */
#define SIDE        20000.0f        /* a model this far to the camera's side is out of the view */
#define TWO_PI      6.2831853f
#define PIC_W       320             /* MENU3L.PCX */
#define PIC_H       200
#define MLB_MOST    0x8000
#define MAX_PARTS   24              /* blocks drawn: three models (the chosen one and its neighbours) */
#define FONT_TITLE  3               /* MENUL.FNT's fonts: 3 is 30 high, 2 is 19 */
#define FONT_NAME   2
#define COLOUR      2               /* the texts' colour: palette 0x20 + shade */
#define TITLE_LINE  45              /* MENU.TXT's TRACK SELECTION, its '*' lines not counted */

const struct menu_part menu_dat_parts[] = {
    { "MENU.MLB", MLB_MOST }, { "AMAP1.3DX", ~0u }, { "AMAP2.3DX", ~0u }, { "AMAP3.3DX", ~0u },
    { "AMAP4.3DX", ~0u }, { "AMAP5.3DX", ~0u }, { "AMAP6.3DX", ~0u }, { 0, 0 } };
const struct menu_part menu_graf_parts[] = {
    { "MENU3L.PCX", ~0u }, { "MENUL.FNT", ~0u }, { "MENUL.SPR", ~0u }, { 0, 0 } };

static const uint8_t *mlb, *model[MENU_TRACKS], *pic, *hud;
static const struct blit_image *img;
static const struct blit_font *font;
static uint32_t nimg, nfont, pal[256];
static char title[32], name[MENU_TRACKS][32];
static int pick;
static float turn = 1.5707964f, cam_x, from_x, to_x, slide;

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

/* A 320 x 200 PCX picture (8 bits, RLE, the 256 colours after the byte 12 at its end) into
   out and its palette into pal; a run does not go on into the next line (the port's pcx_load) */
static int pcx(const uint8_t *d, uint32_t n, uint8_t *out)
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
              const uint8_t *local, uint32_t local_size, uint8_t *mem, uint32_t cap, uint8_t *scratch,
              uint32_t scratch_cap)
{
    uint32_t len;
    if(!(mlb = in_place(menu_dat, menu_size, "MENU.MLB", MLB_MOST, &len))) return 0;
    for(int k = 0; k < MENU_TRACKS; k++) {
        char n[16] = "AMAP1.3DX";
        n[4] = (char)('1' + k);
        if(!(model[k] = in_place(menu_dat, menu_size, n, 0x80, &len)) || rd32(model[k] + 0x14) < 1) return 0;
    }
    /* the picture, then the sprites and fonts as the race screen's (tb_hud.h, no pictures, no text) */
    const uint8_t *p = in_place(graf, graf_size, "MENU3L.PCX", 0, &len);
    if(cap < PIC_W * PIC_H || !p || !pcx(p, len, mem)) return 0;
    pic = mem;
    uint32_t spr_n = unpack(graf, graf_size, "MENUL.SPR", scratch, scratch_cap);
    uint32_t used = (spr_n + 3) & ~3u;
    uint32_t fnt_n = used < scratch_cap ? unpack(graf, graf_size, "MENUL.FNT", scratch + used, scratch_cap - used) : 0;
    if(!spr_n || !fnt_n ||
       !tb_hud_build(scratch, spr_n, scratch + used, fnt_n, scratch, 0, scratch, 0, scratch, 0, mem + PIC_W * PIC_H,
                     cap - PIC_W * PIC_H)) return 0;
    hud = mem + PIC_W * PIC_H;
    nimg = (uint32_t)rd32(hud);
    nfont = (uint32_t)rd32(hud + 4);
    img = (const struct blit_image *)(hud + 8);
    font = (const struct blit_font *)(hud + 8 + 12 * nimg);
    /* the texts */
    uint32_t tn = unpack(local, local_size, "MENU.TXT", scratch, scratch_cap);
    int ok = text_line(scratch, tn, TITLE_LINE, title, sizeof title);
    tn = unpack(local, local_size, "TRACKDAT.TXT", scratch, scratch_cap);
    for(int k = 0; k < MENU_TRACKS; k++) ok &= text_line(scratch, tn, 4 * k, name[k], sizeof name[k]);
    return ok && nimg > 1 && nfont > FONT_TITLE;
}

const uint32_t *menu_palette(void)
{
    return pal;
}

const uint8_t *menu_mlb(void)
{
    return mlb;
}

void menu_enter(int n)
{
    pick = n < 1 ? 0 : n > MENU_TRACKS ? MENU_TRACKS - 1 : n - 1;
    cam_x = from_x = to_x = -ROW_GAP * (float)pick;
    slide = 0.0f;
}

int menu_step(uint32_t keys, uint32_t keys_before)
{
    uint32_t press = keys & ~keys_before;
    turn += DT * TURN;
    if(turn >= TWO_PI) turn = 0.0f;
    float f2 = slide * slide;
    cam_x = to_x + (from_x - to_x) * f2 * f2;
    if(slide > 0.0f && (slide -= DT * SLIDE) < 0.0f) slide = 0.0f;
    if((press & (KEY_LEFT | KEY_RIGHT)) && slide < TAKE) {
        int to = pick + (press & KEY_RIGHT ? 1 : -1);
        if(to >= 0 && to < MENU_TRACKS) {
            pick = to;
            from_x = cam_x;
            to_x = -ROW_GAP * (float)to;
            slide = 1.0f;
        }
    }
    return press & (KEY_B | KEY_START) ? pick + 1 : 0;
}

void menu_backdrop(struct blit_list *l)
{
    blit_copy(l, pic, PIC_W, PIC_H, PIC_W, 0, 0, BLIT_COPY | BLIT_OPAQUE, 0);
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

/* The chosen model and its neighbours, as far as they are in view, block by block the furthest
   block's middle first, as the game draws an object's blocks (0x8ac58) */
void menu_scene(volatile uint8_t *fb)
{
    static const struct rs_clip clip = { 0, 0, PIC_W, PIC_H };
    const struct track_cam cam = { cam_x, CAM_Z, CAM_H, 0.0f, CAM_PITCH, 0.0f };
    struct track_block b[MAX_PARTS];
    float key[MAX_PARTS];
    int n = 0;
    const float c = cosf(turn), s = sinf(turn);
    for(int k = pick - 1; k <= pick + 1; k++) {
        float x = -ROW_GAP * (float)k;
        if(k < 0 || k >= MENU_TRACKS || fabsf(x - cam_x) > SIDE) continue;
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
            float dx = t.o[0] + c * mx - s * mz - cam.x, dy = t.o[1] + s * mx + c * mz - cam.y, dh = t.o[2] + my - cam.h;
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

/* a string centred on the screen at row y (0x2e598) */
static void centred(struct blit_list *l, int y, const char *s, uint32_t f)
{
    int w = blit_text(l, hud, img, &font[f], 0, 0, s, COLOUR, 0);
    blit_text(l, hud, img, &font[f], (PIC_W - w) / 2, y, s, COLOUR, 1);
}

static void sprite(struct blit_list *l, int x, int y, uint32_t k)
{
    const struct blit_image *s = &img[k];
    blit_copy(l, hud + s->off, s->w, s->h, s->w, x + s->x, y + s->y, BLIT_COPY, 0);
}

void menu_text(struct blit_list *l)
{
    centred(l, 2, title, FONT_TITLE);
    centred(l, PIC_H - 2 * (int)font[FONT_NAME].height, name[pick], FONT_NAME);
    if(pick > 0) sprite(l, 8, 100, 0);
    if(pick < MENU_TRACKS - 1) sprite(l, 0xEE, 100, 1);
}
