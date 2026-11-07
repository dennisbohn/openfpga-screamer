/*
 * The props and the moving scenery (see scenery.h for the rules). Both are set up at the
 * track's start with their blocks. A step moves the props that are in the air and the moving
 * things. A prop gets its new block in the step; a moving thing gets its block when it is
 * drawn. A frame hands the ones within reach to the drawing.
 * Entry points: scenery_init(), scenery_step(), scenery_objects(), scenery_hits().
 */
#include <math.h>
#include <string.h>
#include "scenery.h"
#include "bytes.h"
#include "car.h"
#include "dat.h"
#include "drive.h"
#include "hw.h"
#include "sound.h"

#define PROPS_MAX   100         /* track 1 has 45, track 5 99 for cone carnage */
#define KINDS       8           /* rows of OSTACLES (track 1 has 3) */
#define MOVING_MAX  8           /* rows of ANIMATIONS (track 1 has 5) */
#define SCRATCH     0x2000      /* MAPnDAT.DTR, then MAPnOST.SPL, at the end of mem */
#define REACH       track_far() /* the far plane (track.c far_z): nothing farther is drawn */
/* Moving scenery moves only this near the player. The game's reach is 52000; the core moves it
   out with the far plane. */
#define NEAR_MOVE   (52000.0f * track_far_scale())
#define PROP_RADIUS 800.0f      /* round a prop's origin (the barrier is 580 wide) */
#define MOVING_RADIUS 12000.0f  /* round a vehicle's origin (the jet) */
#define MOVING_BLOCKS 4           /* blocks of a moving thing's mesh (the helicopter has 2) */
#define FRAMES      (DRIVE_DT * 30.0f)  /* the game's frames in a step; a frame is 1/30 s on a PC */
#define HALF_PI     1.5707964f

struct prop {
    float x, y, h, yaw, pitch, roll;    /* pitch and roll as car_flat_block() takes them */
    float dx, dy, v, vz;                /* thrown: direction, speed, upward speed */
    int flying, gone, key, hit;         /* gone: stopped off the track. key: its class key for
                                           drawing. hit: knocked over once, counted (+0x3c). */
    const uint8_t *mesh;
    struct track_block b;
};
struct moving {
    const uint8_t *spl, *mesh;          /* its spline (count, start, nodes of 56 bytes), its .3DX */
    int node, half, key;                /* the piece it is on: node and half, see moving_place() */
    int sound, voice;                   /* its effect (sound.h) or -1, the handle of its voice */
    float u, speed, x, y, h, yaw;       /* u: 0..1 along the piece; speed in units/s */
    float last;                         /* its distance from the ear in the previous step */
    struct track_block b[MOVING_BLOCKS];    /* its mesh's blocks (the helicopter: body, rotor) */
    int nb;
};
static struct prop props[PROPS_MAX];
static struct moving moving[MOVING_MAX];
static int nprops, nmoving, hits;
static uint8_t *at, *end;               /* mem handed out up to at: the meshes, MAPnANM.SPL */

/* Extracts the entry `name` of the container into out (at most max bytes). Returns its size,
   or 0 if it is missing. */
static uint32_t entry(const uint8_t *file, uint32_t size, const char *name, uint8_t *out, uint32_t max)
{
    struct dat_entry e;
    return dat_find(file, size, name, &e) ? dat_extract(file, size, &e, out, max) : 0;
}

/*
 * Reads the rows of section sec (OSTACLES or ANIMATIONS) of a DTR text t (n bytes). A row has
 * up to four cells. Each cell is stored as written, a string such as "sawtruck", "f16000" or
 * "d0": for a number, a type letter and then the value. A missing cell is "". A line that is
 * empty or starts with '*' is skipped, and the section ends at END_LIST. At most max rows are
 * read into cell[row][column]. Returns the number of rows.
 */
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

/* The number in a cell after its type letter ("f16000", "d2"). */
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

/* Extracts the .3DX file that a cell names into mem at `at` ("sawtruck" gives AWTRUCK.3DX: a
   mesh name starts with 's', which is not part of the file name). Returns the mesh, or 0 if
   it is not there or is not a valid mesh. */
static const uint8_t *mesh(const uint8_t *gfx, uint32_t gfx_size, const char *cell)
{
    char name[16];
    int k = 0;
    if(cell[0] != 's') return 0;
    for(const char *c = cell + 1; *c && k < 11; c++)
        name[k++] = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
    memcpy(name + k, ".3DX", 5);
    uint32_t m = entry(gfx, gfx_size, name, at, (uint32_t)(end - at));
    int32_t blk = m >= 0x340 && le32s(at + 0x14) >= 1 ? le32s(at + 0x18) : 0;
    if(blk <= 0 || (uint32_t)blk + 0x80 > m) return 0;
    const uint8_t *out = at;
    at += (m + 3) & ~3u;
    return out;
}

/* The heading along the path at its point nearest to (x, y): forward = (sin yaw, -cos yaw). */
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

/* Lays the prop on the surface under it: its tilt there, negated as car_flat_block() takes it. */
static void prop_tilt(struct prop *p)
{
    float pitch, roll;
    track_under_tilt(sinf(p->yaw), -cosf(p->yaw), &pitch, &roll);
    p->pitch = -pitch;
    p->roll = -roll;
}

/* Node k of a moving thing's spline. A node is 56 bytes after the 8 byte header: +12 height,
   +16 the next node, +20 and +32 the handles (x, y). */
static const uint8_t *node(const struct moving *m, int k)
{
    return m->spl + 8 + 56 * k;
}

/* The node after node k, or node 0 if the file names none that exists. */
static int next(const struct moving *m, int k)
{
    int nx = le32s(node(m, k) + 16);
    return nx >= 0 && nx < le32s(m->spl) ? nx : 0;
}

/*
 * Sets the moving thing's place from its node, half and u. This is the opponents' piece and
 * curve (opp.c): a piece is half of the stretch from a node to the next. Its three control
 * points are the handles of that half, and the curve is the quadratic B-spline over them. The
 * height runs straight from one node's height to the next. The heading follows the curve.
 * If len is given, it receives the length of the curve's derivative at u (units per unit of u).
 */
static void moving_place(struct moving *m, float *len)
{
    const uint8_t *n = node(m, m->node), *nn = node(m, next(m, m->node));
    const uint8_t *pa = m->half ? n + 32 : n + 20, *pb = m->half ? nn + 20 : n + 32,
                  *pc = m->half ? nn + 32 : nn + 20;
    float u = m->u, w0 = (1.0f - u) * (1.0f - u) * 0.5f, w2 = u * u * 0.5f, w1 = 1.0f - w0 - w2, der[2];
    float p[2];
    for(int j = 0; j < 2; j++) {
        float a = (float)le32s(pa + 4 * j), b = (float)le32s(pb + 4 * j), c = (float)le32s(pc + 4 * j);
        p[j] = a * w0 + b * w1 + c * w2;
        der[j] = (1.0f - u) * (b - a) + u * (c - b);
    }
    float z0 = (float)le32s(n + 12), z1 = (float)le32s(nn + 12);
    m->x = p[0];
    m->y = p[1];
    m->h = z0 + ((float)m->half + u) * (z1 - z0) * 0.5f;
    if(der[0] != 0.0f || der[1] != 0.0f) m->yaw = atan2f(der[0], -der[1]);
    if(len) *len = sqrtf(der[0] * der[0] + der[1] * der[1]);
}

/*
 * Sets up the props and the moving scenery of track n.
 *   1. The track's MAPnDAT.DTR goes into the last SCRATCH bytes of mem. The meshes are put
 *      into the rest of mem, from its start, as far as `at` has handed it out.
 *   2. The first column of the OSTACLES rows names the mesh of each kind of prop.
 *   3. The ANIMATIONS rows give the moving scenery: mesh, speed, spline and sound. MAPnANM.SPL
 *      is kept in mem and cut into its splines (each is a node count, a start node and the
 *      nodes). A row whose spline does not exist or has fewer than 2 nodes is left out.
 *   4. The props' places come from one spline, MAPnOST.SPL: each node is a prop, its height
 *      field the kind. A node that is not on the drivable surface is left out.
 * Returns the number of props in *nprop and of moving things in *nmove.
 */
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

    /* The props: the first column of OSTACLES names each kind's mesh. */
    const uint8_t *kind_mesh[KINDS];
    int nk = rows(scratch, dtr, "OSTACLES", cell, KINDS);
    for(int k = 0; k < nk; k++) kind_mesh[k] = mesh(gfx, gfx_size, cell[k][0]);

    /* The moving scenery: its spline file stays in mem, and each row gives a mesh. The fourth
       column names the row's sound, a row of the track's own SOUND_FX section. Those rows come
       after the ones of SFXSBASE.DTR, so snd_init() must have run first. */
    int na = rows(scratch, dtr, "ANIMATIONS", cell, MOVING_MAX);
    int sounds = snd_add_list(scratch, dtr);
    memcpy(name + 4, "ANM.SPL", 8);
    uint8_t *anm = at;
    uint32_t anm_len = entry(dat, dat_size, name, anm, (uint32_t)(end - at));
    at += (anm_len + 3) & ~3u;
    const uint8_t *spline[MOVING_MAX];
    int ns = 0;
    for(uint32_t p = 0; p + 8 <= anm_len && ns < MOVING_MAX; ) {        /* cut the file into splines */
        int32_t count = le32s(anm + p);
        if(count < 1 || p + 8 + 56 * (uint32_t)count > anm_len) break;
        spline[ns++] = anm + p;
        p += 8 + 56 * (uint32_t)count;
    }
    for(int k = 0; k < na && nmoving < MOVING_MAX; k++) {
        int s = (int)number(cell[k][2]);        /* the third column: the spline's number */
        if(s < 0 || s >= ns || le32s(spline[s]) < 2) continue;
        const uint8_t *ms = mesh(gfx, gfx_size, cell[k][0]);
        if(!ms) continue;
        struct moving *m = &moving[nmoving++];
        memset(m, 0, sizeof *m);
        m->spl = spline[s];
        m->mesh = ms;
        m->node = le32s(m->spl + 4) >= 0 && le32s(m->spl + 4) < le32s(m->spl) ? le32s(m->spl + 4) : 0;
        m->key = track_class_key(ms);
        m->speed = number(cell[k][1]);          /* the second column: the speed */
        m->sound = sounds >= 0 && cell[k][3][0] ? sounds + (int)number(cell[k][3]) : -1;
        m->voice = -1;
        moving_place(m, 0);
    }

    /* The props' places: one spline, each node a prop. The file is MAPnOST.SPL, in cone
       carnage MAPnCNG.SPL and in slalom MAPnSLA.SPL (original: STARTL.EXE 0x14fb4). */
    memcpy(name + 4, car_mode == CAR_CONES ? "CNG.SPL" : car_mode == CAR_SLALOM ? "SLA.SPL" : "OST.SPL", 8);
    uint32_t len = entry(dat, dat_size, name, scratch, SCRATCH);
    int32_t count = len >= 8 ? le32s(scratch) : 0;
    for(int32_t i = 0; i < count && nprops < PROPS_MAX && 8 + 56 * (uint32_t)(i + 1) <= len; i++) {
        const uint8_t *nd = scratch + 8 + 56 * i;
        int32_t kind = le32s(nd + 12);
        float x = (float)le32s(nd + 4), y = (float)le32s(nd + 8), h;
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
    while(!(MEM_STATUS & MEM_WRITES_IDLE))      /* wait until the SDRAM has the meshes */
        ;
    *nprop = nprops;
    *nmove = nmoving;
}

/*
 * One step of the props. First it tests the player against each prop that is not flying: if
 * one of the player's two circles touches it, the prop is thrown and the hit sound plays
 * (original: STARTL.EXE 0x124c0). Then it moves each prop that is in the air (original:
 * 0x205b4) and makes its block for the new place.
 */
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
                hits += !p->hit;                /* each prop counts once (original: STARTL.EXE 0x126a4) */
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
        if(off) {                               /* it left the drivable area: it goes back to the place
                                                   it came from and turns round; the ground is taken
                                                   at its own height */
            ground = p->h;
            p->x = ox;
            p->y = oy;
            p->dx = -p->dx;
            p->dy = -p->dy;
            p->v *= 0.3f;
        }
        p->vz -= DRIVE_DT * 500.0f;
        p->h += p->vz * FRAMES;
        if(p->h <= ground) {                    /* it hit the ground: bounce, keeping 0.8 of its fall speed */
            p->h = ground;
            p->vz = -p->vz * 0.8f;
            if(fabsf(p->vz) < 50.0f) p->vz = 0.0f;
            if(p->v == 0.0f && p->vz == 0.0f) {
                p->flying = 0;
                if(off) p->gone = 1;
                else {                          /* on its side along the slope (original: 0x20880) */
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

/*
 * One step of the moving scenery (original: STARTL.EXE 0x20944, 0x20b0c). A thing that is near
 * the player moves its speed x DRIVE_DT along its curve. A step may cross several pieces
 * (at most 8). Each piece is half of the stretch between two nodes.
 */
static void moving_step(void)
{
    const struct car_state *c = car_state();
    for(int k = 0; k < nmoving; k++) {
        struct moving *m = &moving[k];
        if(fabsf(c->x - m->x) >= NEAR_MOVE || fabsf(c->y - m->y) >= NEAR_MOVE) continue;
        float left = m->speed * DRIVE_DT;       /* the distance still to move in this step */
        for(int guard = 0; guard < 8 && left > 0.0f; guard++) {
            float len;
            moving_place(m, &len);
            if(len < 1.0f) len = 1.0f;
            float du = left / len;
            if(m->u + du < 1.0f) {              /* it stays in this piece */
                m->u += du;
                break;
            }
            left -= (1.0f - m->u) * len;        /* it reaches the end of the piece: go on in the next */
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

/*
 * Plays the moving scenery's sounds as heard from the camera (original: STARTL.EXE 0x21b80;
 * car_step sets the ear). A sound is heard within 40000 units. Its volume is
 * ((40000 - d) / 40000)^2 for the distance d. It is panned, and its pitch is 65536 plus the
 * Doppler shift.
 */
static void moving_sound(void)
{
    for(int k = 0; k < nmoving; k++) {
        struct moving *m = &moving[k];
        if(m->sound >= 0)
            m->voice = snd_at(m->voice, m->sound, m->x, m->y, 40000.0f, 32767.0f, 65536.0f, &m->last);
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

/*
 * Makes moving thing k's blocks from its place in this step, when it comes up in the drawing. (The CPU's
 * time before the first piece goes to the geometry unit counts towards the frame; once the
 * pieces go, the geometry unit's work does.) Its textures play by the clock (original:
 * STARTL.EXE 0x153e3: the record's phase is -1); the rotor has 20 frames.
 */
static int moving_make(int k, const struct track_cam *cam, struct track_object *obj)
{
    struct moving *m = &moving[k];
    m->nb = car_mesh_blocks(m->mesh, m->x, m->y, m->h, m->yaw, 0.0f, 0.0f, cam, m->b, MOVING_BLOCKS);
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
        objs[n++] = (struct track_object){ m->x, m->y, m->h, MOVING_RADIUS, 0, 0, moving_make, k, m->key,
                                           0, 0 };
    }
    for(int k = 0; k < nprops && n < max; k++) {
        const struct prop *p = &props[k];
        float dx = p->x - cam->x, dy = p->y - cam->y;
        if(p->gone || dx * dx + dy * dy > REACH * REACH) continue;
        objs[n++] = (struct track_object){ p->x, p->y, p->h, PROP_RADIUS, 1, &p->b, 0, k, p->key, 0, 0 };
    }
    return n;
}
