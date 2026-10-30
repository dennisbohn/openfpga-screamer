/*
 * The props and the moving scenery (scenery.h): placed at the track's start, each with its
 * block; a step moves the ones that move and makes their blocks again, a frame hands those in
 * reach to the drawing.
 */
#include <math.h>
#include <string.h>
#include "scenery.h"
#include "car.h"
#include "dat.h"
#include "drive.h"
#include "hw.h"
#include "sound.h"

#define PROPS_MAX   100         /* track 1 has 45, track 5 99 for cone carnage */
#define KINDS       8           /* rows of OSTACLES (track 1 has 3) */
#define MOVING_MAX  8           /* rows of ANIMATIONS (track 1 has 5) */
#define SCRATCH     0x2000      /* MAPnDAT.DTR, then MAPnOST.SPL, at the end of mem */
#define REACH       43808.0f    /* the far plane (track.c far_z): nothing farther is drawn */
#define NEAR_MOVE   52000.0f    /* moving scenery moves only this near the player */
#define PROP_RADIUS 800.0f      /* round a prop's origin (the barrier is 580 wide) */
#define MOVING_RADIUS 12000.0f  /* round a vehicle's (the jet) */
#define MOVING_BLOCKS 4           /* blocks of a moving thing's mesh (the helicopter has 2) */
#define FRAMES      (DRIVE_DT * 30.0f)  /* the game's frames in a step, on a PC of 30 a second */
#define HALF_PI     1.5707964f

struct prop {
    float x, y, h, yaw, pitch, roll;    /* pitch and roll as car_flat_block() takes them */
    float dx, dy, v, vz;                /* thrown: direction, speed, upward speed */
    int flying, gone, key, hit;         /* hit: knocked over once, counted (+0x3c) */
    const uint8_t *mesh;
    struct track_block b;
};
struct moving {
    const uint8_t *spl, *mesh;          /* its spline (count, start, nodes of 56 bytes), its .3DX */
    int node, half, key;
    int sound, voice;                   /* its effect (sound.h) or -1, the handle of its voice */
    float u, speed, x, y, h, yaw;
    float last;                         /* its distance from the ear a step before */
    struct track_block b[MOVING_BLOCKS];    /* its mesh's blocks (the helicopter: body, rotor) */
    int nb;
};
static struct prop props[PROPS_MAX];
static struct moving moving[MOVING_MAX];
static int nprops, nmoving, hits;
static uint8_t *at, *end;               /* mem handed out up to at: the meshes, MAPnANM.SPL */

static int32_t rd32(const uint8_t *p)
{
    return (int32_t)(p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

/* entry name of the container into out (at most max bytes): its size, 0 if it is missing */
static uint32_t entry(const uint8_t *file, uint32_t size, const char *name, uint8_t *out, uint32_t max)
{
    struct dat_entry e;
    return dat_find(file, size, name, &e) ? dat_extract(file, size, &e, out, max) : 0;
}

/* Up to max rows of section sec (OSTACLES, ANIMATIONS) of a DTR text, their first four cells
   as written ("sawtruck", "f16000", "d0": a type letter, then the value; "" if missing); how many */
static int rows(const uint8_t *t, uint32_t n, const char *sec, char cell[][4][16], int max)
{
    uint32_t len = (uint32_t)strlen(sec), p = 0;
    while(p + len <= n && memcmp(t + p, sec, len)) p++;
    if(p + len > n) return 0;
    int r = 0;
    while(p < n && r < max) {
        while(p < n && t[p] != '\n') p++;       /* the next line */
        p++;
        while(p < n && (t[p] == ' ' || t[p] == '\t')) p++;
        if(p >= n || t[p] == '*' || t[p] == '\r' || t[p] == '\n') continue;
        if(t[p] == 'E') break;                  /* END_LIST */
        for(int c = 0; c < 4; c++) {
            int k = 0;
            while(p < n && k < 15 && t[p] > ' ') cell[r][c][k++] = (char)t[p++];
            cell[r][c][k] = 0;
            while(p < n && (t[p] == ' ' || t[p] == '\t')) p++;
        }
        r++;
    }
    return r;
}

/* a cell's number after its type letter ("f16000", "d2") */
static float number(const char *cell)
{
    float v = 0.0f, scale = 0.0f;
    for(const char *c = cell + 1; *c; c++) {
        if(*c == '.') scale = 1.0f;
        else if(*c >= '0' && *c <= '9') {
            v = v * 10.0f + (float)(*c - '0');
            scale *= 10.0f;
        }
    }
    return scale > 0.0f ? v / scale : v;
}

/* the .3DX file a cell names ("sawtruck" -> AWTRUCK.3DX) into mem; 0 if it is not there */
static const uint8_t *mesh(const uint8_t *gfx, uint32_t gfx_size, const char *cell)
{
    char name[16];
    int k = 0;
    if(cell[0] != 's') return 0;
    for(const char *c = cell + 1; *c && k < 11; c++) name[k++] = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
    memcpy(name + k, ".3DX", 5);
    uint32_t m = entry(gfx, gfx_size, name, at, (uint32_t)(end - at));
    int32_t blk = m >= 0x340 && rd32(at + 0x14) >= 1 ? rd32(at + 0x18) : 0;
    if(blk <= 0 || (uint32_t)blk + 0x80 > m) return 0;
    const uint8_t *out = at;
    at += (m + 3) & ~3u;
    return out;
}

/* the heading along the path at its point nearest to (x, y): forward = (sin yaw, -cos yaw) */
static float path_heading(float x, float y)
{
    uint32_t n = track_path_length(), best = 0;
    float bd = 3.4e38f, px, py, ph;
    for(uint32_t i = 0; i < n; i++) {
        track_path_point(i, &px, &py, &ph);
        float d = (px - x) * (px - x) + (py - y) * (py - y);
        if(d < bd) {
            bd = d;
            best = i;
        }
    }
    float ax, ay, bx, by;
    track_path_point(best + n - 1, &ax, &ay, &ph);
    track_path_point(best + 1, &bx, &by, &ph);
    return atan2f(bx - ax, -(by - ay));
}

static void prop_block(struct prop *p)
{
    car_flat_block(p->mesh, p->x, p->y, p->h, p->yaw, p->pitch, p->roll, &p->b);
}

/* lying on the surface under it: its tilt there (negated, as car_flat_block takes it) */
static void prop_tilt(struct prop *p)
{
    float pitch, roll;
    track_under_tilt(p->yaw, &pitch, &roll);
    p->pitch = -pitch;
    p->roll = -roll;
}

/* node k of a moving thing's spline, and the one after it */
static const uint8_t *node(const struct moving *m, int k)
{
    return m->spl + 8 + 56 * k;
}

static int next(const struct moving *m, int k)
{
    int nx = rd32(node(m, k) + 16);
    return nx >= 0 && nx < rd32(m->spl) ? nx : 0;
}

/* its place on the curve (opp.c's piece and curve: the three handles of its half of the piece
   from its node to the next, the quadratic B-spline over them), its height the nodes' run
   straight between them, its heading along the curve */
static void moving_place(struct moving *m, float *len)
{
    const uint8_t *n = node(m, m->node), *nn = node(m, next(m, m->node));
    const uint8_t *pa = m->half ? n + 32 : n + 20, *pb = m->half ? nn + 20 : n + 32, *pc = m->half ? nn + 32 : nn + 20;
    float u = m->u, w0 = (1.0f - u) * (1.0f - u) * 0.5f, w2 = u * u * 0.5f, w1 = 1.0f - w0 - w2, der[2];
    float p[2];
    for(int j = 0; j < 2; j++) {
        float a = (float)rd32(pa + 4 * j), b = (float)rd32(pb + 4 * j), c = (float)rd32(pc + 4 * j);
        p[j] = a * w0 + b * w1 + c * w2;
        der[j] = (1.0f - u) * (b - a) + u * (c - b);
    }
    float z0 = (float)rd32(n + 12), z1 = (float)rd32(nn + 12);
    m->x = p[0];
    m->y = p[1];
    m->h = z0 + ((float)m->half + u) * (z1 - z0) * 0.5f;
    if(der[0] != 0.0f || der[1] != 0.0f) m->yaw = atan2f(der[0], -der[1]);
    if(len) *len = sqrtf(der[0] * der[0] + der[1] * der[1]);
}

void scenery_init(const uint8_t *dat, uint32_t dat_size, const uint8_t *gfx, uint32_t gfx_size, int n,
                  uint8_t *mem, uint32_t cap, int *nprop, int *nmove)
{
    static char cell[KINDS > MOVING_MAX ? KINDS : MOVING_MAX][4][16];
    nprops = nmoving = hits = 0;
    *nprop = *nmove = 0;
    if(!dat || !gfx || cap < 2 * SCRATCH || !track_path_length()) return;
    uint8_t *scratch = mem + cap - SCRATCH;
    at = mem;
    end = scratch;
    char name[16] = "MAPnDAT.DTR";
    name[3] = (char)('0' + n);
    uint32_t dtr = entry(dat, dat_size, name, scratch, SCRATCH);

    /* the props: the first column of OSTACLES names each kind's mesh */
    const uint8_t *kind_mesh[KINDS];
    int nk = rows(scratch, dtr, "OSTACLES", cell, KINDS);
    for(int k = 0; k < nk; k++) kind_mesh[k] = mesh(gfx, gfx_size, cell[k][0]);

    /* the moving scenery: its spline file kept in mem, each row's mesh; the fourth column names
       its sound, a row of the track's own SOUND_FX (after SFXSBASE.DTR's: snd_init first) */
    int na = rows(scratch, dtr, "ANIMATIONS", cell, MOVING_MAX);
    int sounds = snd_add_list(scratch, dtr);
    memcpy(name + 4, "ANM.SPL", 8);
    uint8_t *anm = at;
    uint32_t anm_len = entry(dat, dat_size, name, anm, (uint32_t)(end - at));
    at += (anm_len + 3) & ~3u;
    const uint8_t *spline[MOVING_MAX];
    int ns = 0;
    for(uint32_t p = 0; p + 8 <= anm_len && ns < MOVING_MAX; ) {
        int32_t count = rd32(anm + p);
        if(count < 1 || p + 8 + 56 * (uint32_t)count > anm_len) break;
        spline[ns++] = anm + p;
        p += 8 + 56 * (uint32_t)count;
    }
    for(int k = 0; k < na && nmoving < MOVING_MAX; k++) {
        int s = (int)number(cell[k][2]);
        if(s < 0 || s >= ns || rd32(spline[s]) < 2) continue;
        const uint8_t *ms = mesh(gfx, gfx_size, cell[k][0]);
        if(!ms) continue;
        struct moving *m = &moving[nmoving++];
        memset(m, 0, sizeof *m);
        m->spl = spline[s];
        m->mesh = ms;
        m->node = rd32(m->spl + 4) >= 0 && rd32(m->spl + 4) < rd32(m->spl) ? rd32(m->spl + 4) : 0;
        m->key = track_class_key(ms);
        m->speed = number(cell[k][1]);
        m->sound = sounds >= 0 && cell[k][3][0] ? sounds + (int)number(cell[k][3]) : -1;
        m->voice = -1;
        moving_place(m, 0);
    }

    /* the props' places: one spline, each node a prop; MAPnOST.SPL, in cone carnage MAPnCNG.SPL,
       in slalom MAPnSLA.SPL (0x14fb4) */
    memcpy(name + 4, car_mode == CAR_CONES ? "CNG.SPL" : car_mode == CAR_SLALOM ? "SLA.SPL" : "OST.SPL", 8);
    uint32_t len = entry(dat, dat_size, name, scratch, SCRATCH);
    int32_t count = len >= 8 ? rd32(scratch) : 0;
    for(int32_t i = 0; i < count && nprops < PROPS_MAX && 8 + 56 * (uint32_t)(i + 1) <= len; i++) {
        const uint8_t *nd = scratch + 8 + 56 * i;
        int32_t kind = rd32(nd + 12);
        float x = (float)rd32(nd + 4), y = (float)rd32(nd + 8), h;
        int grass;
        if(kind < 0 || kind >= nk || !kind_mesh[kind] || !track_under(x, y, &h, &grass)) continue;
        struct prop *p = &props[nprops++];
        memset(p, 0, sizeof *p);
        p->x = x;
        p->y = y;
        p->h = h;
        p->yaw = path_heading(x, y);
        p->mesh = kind_mesh[kind];
        p->key = track_class_key(p->mesh);
        prop_tilt(p);
        prop_block(p);
    }
    while(!(MEM_STATUS & MEM_WRITES_IDLE))      /* the meshes in the SDRAM for the geometry unit */
        ;
    *nprop = nprops;
    *nmove = nmoving;
}

/* the player against a prop, knocked flying with the hit's sample, and a prop in the air
   (0x124c0, 0x205b4) */
static void props_step(void)
{
    const struct car_state *c = car_state();
    float cx[2], cy[2], rad = car_circles(cx, cy);
    for(int i = 0; i < nprops; i++) {
        struct prop *p = &props[i];
        if(p->gone) continue;
        if(!p->flying && fabsf(c->x - p->x) < 1500.0f && fabsf(c->y - p->y) < 1500.0f &&
           fabsf(c->z - p->h) < 1500.0f)
            for(int k = 0; k < 2 && !p->flying; k++) {
                float dx = cx[k] - p->x, dy = cy[k] - p->y;
                if(sqrtf(dx * dx + dy * dy) - (rad + 100.0f) >= 100.0f) continue;
                p->dx = sinf(c->yaw);
                p->dy = -cosf(c->yaw);
                p->vz = c->speed * 0.02f;
                p->v = c->speed * 1.4f;
                p->flying = 1;
                hits += !p->hit;                /* the first time only (0x126a4) */
                p->hit = 1;
                snd_play(SND_HIT);
            }
        if(!p->flying) continue;
        float ox = p->x, oy = p->y, ground;
        p->x += p->v * p->dx * DRIVE_DT;
        p->y += p->v * p->dy * DRIVE_DT;
        p->pitch -= p->v * p->dx * DRIVE_DT * 0.008f;
        p->roll -= p->v * p->dy * DRIVE_DT * 0.0015f;
        p->v -= DRIVE_DT * 2000.0f;
        if(p->v < 0.0f) p->v = 0.0f;
        int grass, off = !track_under(p->x, p->y, &ground, &grass);
        if(off) {                               /* turned back; the ground where it is */
            ground = p->h;
            p->x = ox;
            p->y = oy;
            p->dx = -p->dx;
            p->dy = -p->dy;
            p->v *= 0.3f;
        }
        p->vz -= DRIVE_DT * 500.0f;
        p->h += p->vz * FRAMES;
        if(p->h <= ground) {
            p->h = ground;
            p->vz = -p->vz * 0.8f;
            if(fabsf(p->vz) < 50.0f) p->vz = 0.0f;
            if(p->v == 0.0f && p->vz == 0.0f) {
                p->flying = 0;
                if(off) p->gone = 1;
                else {                          /* on its side along the slope (0x20880) */
                    float roll = p->roll;
                    track_under(p->x, p->y, &ground, &grass);
                    prop_tilt(p);
                    p->pitch += HALF_PI;
                    p->roll = roll;
                }
            }
        }
        prop_block(p);
    }
}

/* the moving scenery near the player along its spline (0x20944, 0x20b0c) */
static void moving_step(void)
{
    const struct car_state *c = car_state();
    for(int k = 0; k < nmoving; k++) {
        struct moving *m = &moving[k];
        if(fabsf(c->x - m->x) >= NEAR_MOVE || fabsf(c->y - m->y) >= NEAR_MOVE) continue;
        float left = m->speed * DRIVE_DT;
        for(int guard = 0; guard < 8 && left > 0.0f; guard++) {
            float len;
            moving_place(m, &len);
            if(len < 1.0f) len = 1.0f;
            float du = left / len;
            if(m->u + du < 1.0f) {
                m->u += du;
                break;
            }
            left -= (1.0f - m->u) * len;
            m->u = 0.0f;
            if(m->half == 0) m->half = 1;
            else {
                m->half = 0;
                m->node = next(m, m->node);
            }
        }
        moving_place(m, 0);
    }
}

/* 0x21b80: the moving scenery's sounds, heard from the camera (car_step set the ear): within
   40000 at ((40000 - d) / 40000)^2, panned, pitch 65536 + the Doppler shift */
static void moving_sound(void)
{
    for(int k = 0; k < nmoving; k++) {
        struct moving *m = &moving[k];
        if(m->sound >= 0) m->voice = snd_at(m->voice, m->sound, m->x, m->y, 40000.0f, 32767.0f, 65536.0f, &m->last);
    }
}

int scenery_hits(int *total)
{
    if(total) *total = nprops;
    return hits;
}

void scenery_step(void)
{
    props_step();
    moving_step();
    moving_sound();
}

/* moving thing k's blocks, made when it is drawn, from its place now (the CPU's time before
   the first piece is the frame's; once the pieces go, the geometry unit's work is); its
   textures play by the clock (0x153e3: the record's phase -1), the rotor's 20 frames */
static int moving_make(int k, const struct track_cam *cam, struct track_object *obj)
{
    struct moving *m = &moving[k];
    (void)cam;
    m->nb = car_mesh_blocks(m->mesh, m->x, m->y, m->h, m->yaw, 0.0f, 0.0f, m->b, MOVING_BLOCKS);
    for(int b = 0; b < m->nb; b++) m->b[b].phase = track_phase();
    obj->n = m->nb;
    obj->b = m->b;
    return 1;
}

int scenery_objects(const struct track_cam *cam, struct track_object *objs, int max)
{
    int n = 0;
    for(int k = 0; k < nmoving && n < max; k++) {
        struct moving *m = &moving[k];
        float dx = m->x - cam->x, dy = m->y - cam->y;
        if(dx * dx + dy * dy > (REACH + MOVING_RADIUS) * (REACH + MOVING_RADIUS)) continue;
        objs[n++] = (struct track_object){ m->x, m->y, m->h, MOVING_RADIUS, 0, 0, moving_make, k, m->key, 0, 0 };
    }
    for(int k = 0; k < nprops && n < max; k++) {
        const struct prop *p = &props[k];
        float dx = p->x - cam->x, dy = p->y - cam->y;
        if(p->gone || dx * dx + dy * dy > REACH * REACH) continue;
        objs[n++] = (struct track_object){ p->x, p->y, p->h, PROP_RADIUS, 1, &p->b, 0, k, p->key, 0, 0 };
    }
    return n;
}
