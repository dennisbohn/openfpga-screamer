/*
 * The intro (intro.h): the numbers are STARTL.EXE's, read as openscreamer's port/src/main.c
 * (intro_*) has them and checked in the program: the logos 0x279fc, the credits 0x27b64, the
 * fade 0x27d28, the cars' cameras 0x1a920 and the one shown 0x1aeb4, 0x19d82, 0x27e64.
 */
#include "intro.h"
#include "dat.h"
#include "drive.h"
#include "opp.h"
#include "sound.h"
#include "tb_hud.h"
#include <math.h>
#include <string.h>

#define DT          DRIVE_DT
#define PI          3.1415927f
#define LINES       256             /* ATTR.TXT's lines, its '*' lines not counted */
#define BLOCK_LINES 20              /* a block's lines eased (0x11ce54) */
#define POSTS       64              /* MAPnCAM.SPL's nodes (14 .. 29) */
#define STAY        4.0f            /* a block stays 4 s (0x100ed6) */
#define NEAR        1500.0f         /* a camera closing in stops 1500 from its car (0x100934) */
#define CHANGE      10.0f           /* another car every 1000 ticks of 1/100 s (0x27e64) */
#define COLOUR      2               /* the credits' colour, font 0 (0x27c29) */

static const uint8_t *hud;
static const struct blit_image *img;
static const struct blit_font *font;
static uint32_t nimg, nfont;
static char text[2048];
static const char *line[LINES];
static int nline;
static int32_t post[POSTS][3];          /* x, y, height; |height| % 10 its code */
static int nposts;

/* 0x11ce40 (bit 2: leaving, 3: the credits are over), the block and its first line, out (the
   lines on their way out), the block's clock, the lines' and the logo's offsets, the fade */
static int state, block, first, out, xo[BLOCK_LINES];
static float clock, logo_x, level;
/* each car's camera (+0x4f8 ..): the post (+0x554), new there this step (+0x564), closing in
   (+0x568), its distance a step before (+0x55c), where the camera stands and looks and what is
   left of a flight from where it stood (0x19ad8, 0x19ef0); the car's place a step before */
static struct cam { int post, fresh, zoom, fly; float prev, px, py; struct track_cam at, d; } cams[OPP_MAX];
static int shown, pending, next_car;    /* 0x110a14, 0x110a18; lined up when the 10 s are up */
static float change;

int intro_init(const uint8_t *graf, uint32_t graf_size, const uint8_t *local, uint32_t local_size, uint8_t *mem,
               uint32_t cap, uint8_t *scratch, uint32_t scratch_cap)
{
    struct dat_entry e;
    uint32_t spr_n = dat_find(graf, graf_size, "LOGOL.SPR", &e) ? dat_extract(graf, graf_size, &e, scratch, scratch_cap) : 0;
    uint32_t used = (spr_n + 3) & ~3u, fnt_n = 0;
    if(used < scratch_cap && dat_find(graf, graf_size, "ATTRL.FNT", &e))
        fnt_n = dat_extract(graf, graf_size, &e, scratch + used, scratch_cap - used);
    if(!spr_n || !fnt_n || !tb_hud_build(scratch, spr_n, scratch + used, fnt_n, scratch, 0, scratch, 0, scratch, 0, mem, cap))
        return 0;
    hud = mem;
    nimg = (uint32_t)(hud[0] | hud[1] << 8 | hud[2] << 16 | (uint32_t)hud[3] << 24);
    nfont = (uint32_t)(hud[4] | hud[5] << 8 | hud[6] << 16 | (uint32_t)hud[7] << 24);
    img = (const struct blit_image *)(hud + 8);
    font = (const struct blit_font *)(hud + 8 + 12 * nimg);
    /* ATTR.TXT's lines, each ended where its \r or \n was */
    uint32_t n = dat_find(local, local_size, "ATTR.TXT", &e) ? dat_extract(local, local_size, &e, (uint8_t *)text,
                                                                           sizeof text - 1) : 0;
    nline = 0;
    for(uint32_t i = 0; i < n && nline < LINES;) {
        uint32_t end = i;
        while(end < n && text[end] != '\r' && text[end] != '\n') end++;
        if(text[i] != '*') line[nline++] = text + i;
        text[end] = 0;
        i = end + (end + 1 < n && text[end + 1] == '\n' ? 2 : 1);
    }
    text[n] = 0;
    return nimg >= 3 && nfont >= 1 && nline > 0;
}

void intro_start(const uint8_t *dat, uint32_t dat_size, int n)
{
    static int32_t spl[2 + 14 * POSTS];         /* count, start, nodes of 14 words: ?, x, y, height */
    char name[16] = "MAPnCAM.SPL";
    struct dat_entry e;
    name[3] = (char)('0' + n);
    uint32_t len = dat && dat_find(dat, dat_size, name, &e) && e.size <= sizeof spl ?
                   dat_extract(dat, dat_size, &e, (uint8_t *)spl, sizeof spl) : 0;
    nposts = 0;
    for(int32_t k = 0; len >= 8 && k < spl[0] && k < POSTS && 8 + 56 * (uint32_t)(k + 1) <= len; k++, nposts++)
        memcpy(post[k], spl + 2 + 14 * k + 1, sizeof post[k]);
    state = block = first = out = 0;
    clock = 0.0f;
    logo_x = -320.0f;
    level = 1.0f;
    for(int i = 0; i < BLOCK_LINES; i++) xo[i] = 350;
    for(int k = 0; k < OPP_MAX; k++) {
        cams[k] = (struct cam){ .post = -1 };
        opp_where(k, &cams[k].px, &cams[k].py, 0);
    }
    shown = pending = next_car = 0;
    change = 0.0f;
}

/* The camera at post p looking at the car at (x, y, h), the way the trackside one does
   (car.c): straight at it, down by the angle the height between them gives; closing in, moved
   on towards it until it is 1500 away (0x1a920) */
static struct track_cam place(int p, float x, float y, float h, int zoom)
{
    float dx = (float)post[p][0] - x, dy = (float)post[p][1] - y, dh = (float)post[p][2] - h;
    float flat = sqrtf(dx * dx + dy * dy), dist = sqrtf(dx * dx + dy * dy + dh * dh);
    struct track_cam c = { (float)post[p][0], (float)post[p][1], (float)post[p][2], atan2f(-dx, dy),
                           -atan2f(fabsf(dh), flat), 0.0f };
    int32_t move = (int32_t)(dist - NEAR);
    if(zoom && move > 0 && dist > 0.0f) {
        c.x -= dx / dist * (float)move;
        c.y -= dy / dist * (float)move;
        c.h -= fabsf(dh) / dist * (float)move;
    }
    return c;
}

static float wrap(float a)
{
    while(a > PI) a -= 2.0f * PI;
    while(a < -PI) a += 2.0f * PI;
    return a;
}

/* Car k's camera (0x1a920): the post nearest it on the ground (a post at its very place passed
   over); new there, its distance a step before is that from where the car was then. Closing in
   by the post's code: bit 1 always, bit 2 while the car goes away from it, bit 4 while it comes.
   A change of that at the same post flies there (0x19ad8, view-table row 4 asked for as 2): what
   lies between where the camera stood and where it now belongs dies away as when the view
   changes (0x19ef0: the place by damp[90], the angles by 0x19 a step); a new post cuts. */
static void car_cam(int k)
{
    struct cam *c = &cams[k];
    float x, y, h;
    if(!opp_where(k, &x, &y, &h) || !nposts) return;
    int at = -1;
    float best = 0.0f;
    for(int p = 0; p < nposts; p++) {
        float dx = (float)post[p][0] - x, dy = (float)post[p][1] - y, d = sqrtf(dx * dx + dy * dy);
        if(d != 0.0f && (at < 0 || d < best)) {
            best = d;
            at = p;
        }
    }
    if(at < 0) return;
    c->fresh = at != c->post;
    if(c->fresh) {
        float dx = (float)post[at][0] - c->px, dy = (float)post[at][1] - c->py;
        c->prev = sqrtf(dx * dx + dy * dy);
        c->post = at;
    }
    int code = (post[at][2] < 0 ? -post[at][2] : post[at][2]) % 10;
    int zoom = (code & 1) || ((code & 2) && c->prev - best <= 0.0f) || ((code & 4) && c->prev - best >= 0.0f);
    struct track_cam now = place(at, x, y, h, zoom);
    if(c->fresh) c->fly = 0;
    else if(zoom != c->zoom) {
        struct track_cam was = place(at, x, y, h, c->zoom);
        c->d = (struct track_cam){ was.x - now.x, was.y - now.y, was.h - now.h, wrap(was.yaw - now.yaw),
                                   was.pitch - now.pitch, 0.0f };
        c->fly = 1;
    }
    if(c->fly) {
        float kp = drive_damp(90), ka = drive_damp(0x19);
        c->d.x *= kp;
        c->d.y *= kp;
        c->d.h *= kp;
        c->d.yaw *= ka;
        c->d.pitch *= ka;
        c->fly = fabsf(c->d.x) + fabsf(c->d.y) + fabsf(c->d.h) > 4.0f || fabsf(c->d.yaw) + fabsf(c->d.pitch) > 0.01f;
    }
    c->zoom = zoom;
    c->prev = best;
    c->at = now;
    c->px = x;
    c->py = y;
}

/* The credits' block (0x27b64): each line eased from 350 to its place (0 in, -400 out:
   stiffness 0x5a, whole pixels); one out past -360 ends the block's stay. After 4 s the lines go
   out, and once they are out the next block comes; after the last block the state is 3, which
   leaves. The logo (0x279fc): with the first two blocks sprites 0 and 1, eased in from -320 and
   out with the lines (0x50); from the third on sprite 2, in (0x5a), out with the last block,
   block 18, to -340 (0x50). */
static void credits(void)
{
    if(block < 2) logo_x = (logo_x + (float)(out * 320)) * drive_damp(0x50) - (float)(out * 320);
    else if(block == 0x12 && out) logo_x = (logo_x + 340.0f) * drive_damp(0x50) - 340.0f;
    else logo_x *= drive_damp(0x5A);
    int i = 0, k = first, to = out * -400;
    do {
        if(i < BLOCK_LINES) {
            if(xo[i] > -360) xo[i] = (int)((float)(xo[i] - to) * drive_damp(0x5A) + (float)to);
            else clock = STAY;
        }
        i++;
        k++;
    } while(k < nline && line[k][0] != '#');
    if((clock += DT) < STAY) return;
    clock = 0.0f;
    out = !out;
    if(!out) {
        first = k + 1;
        block++;
        for(int j = 0; j < BLOCK_LINES; j++) xo[j] = 350;
    }
    if(first >= nline) state = 3;
}

int intro_step(void)
{
    if(state == 4) return 1;
    if(state != 3) credits();
    if(state & 2) {                             /* 0x27d28: picture and sound out in a second */
        if((level -= DT) <= 0.0f) {
            level = 0.0f;
            state = 4;
        }
    }
    if((change += DT) > CHANGE) {
        change = 0.0f;
        next_car = 1;
    }
    int n = opp_count();
    for(int k = 0; k < n && k < OPP_MAX; k++) car_cam(k);
    if(next_car && n > 0) {                     /* 0x19d82: the next car at another post */
        next_car = 0;
        for(int tries = 0; tries < n; tries++) {
            pending = (pending + 1) % n;
            if(pending == shown || cams[pending].post != cams[shown].post) break;
        }
    }
    if(shown >= n) shown = 0;
    if(cams[shown].fresh && pending != shown && pending < n) shown = pending;   /* 0x1aef3 */
    struct track_cam ear;
    intro_camera(&ear);
    snd_ear(&ear);
    return state == 4;
}

void intro_leave(void)
{
    if(!(state & 2)) state |= 2;
}

int32_t intro_level(void)
{
    return (int32_t)(level * 32768.0f);
}

void intro_camera(struct track_cam *cam)
{
    const struct cam *c = &cams[shown];
    *cam = c->at;
    if(c->fly) {
        cam->x += c->d.x;
        cam->y += c->d.y;
        cam->h += c->d.h;
        cam->yaw += c->d.yaw;
        cam->pitch += c->d.pitch;
    }
}

void intro_text(struct blit_list *l)
{
    if(state == 3 || state == 4 || !hud) return;
    uint32_t s = block < 2 ? (uint32_t)block : 2;
    const struct blit_image *m = &img[s];
    int x = (int)((float)((320 - (int)m->w) >> 1) + logo_x), y = block < 2 ? 24 * block + 4 : 4;
    if(x + m->x + (int)m->w > 0 && x + m->x < 320) blit_copy(l, hud + m->off, m->w, m->h, m->w, x + m->x, y + m->y, BLIT_COPY, 0);
    int i = 0, k = first;
    do {
        if(i < BLOCK_LINES && k < nline && line[k][0]) {
            int w = blit_text(l, hud, img, &font[0], 0, 0, line[k], COLOUR, 0), lx = xo[i] + ((320 - w) >> 1);
            if(lx < 320 && lx + w > 0) blit_text(l, hud, img, &font[0], lx, 20 * i, line[k], COLOUR, 1);
        }
        i++;
        k++;
    } while(k < nline && line[k][0] != '#');
}
