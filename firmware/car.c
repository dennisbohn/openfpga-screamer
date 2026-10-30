/*
 * The player's car (car.h), after OpenScreamer's port/src/main.c (GPL-3.0), checked against
 * STARTL.EXE where it says so; the shadow, the smoke and the trackside camera from STARTL.EXE
 * itself, the suspension (the body's lean) too. Not yet here: the camera's fly-across between
 * views.
 */
#include "car.h"
#include <math.h>
#include "dat.h"
#include "hw.h"
#include "efx.h"
#include "ghost.h"
#include "scenery.h"
#include "sound.h"
#include "speech.h"
#include "tb_spl.h"

#define PI          3.14159265f
/* the bumper cam's height over the car: camera_hi in TRACK_DATA of MAPnA.DTR */
#define CAMERA_HI   ((float)track_camera_hi())

/* the game's views (OpenScreamer's g_views; the table at 0x1da0: key, camera routine, lines of
   the 3D view, overlay): how far behind, how high, looking down how much; from the car (camera
   routine 0); the 3D view's lines from the top; the overlay (hud.h: 1 the dashboard over the
   rest, 2 the outside views' instruments, 0 neither) */
struct view_def { float behind, above, down; int incar, lines, overlay; };
static const struct view_def views[CAR_VIEWS + 2] = {
    {    0.0f,   0.0f, 0.0f,    1, 200, 2 },    /* row 0: bumper cam */
    {    0.0f,   0.0f, 0.0f,    1, 132, 1 },    /* row 1: cockpit */
    { 1300.0f, 300.0f, 0.1885f, 0, 200, 2 },    /* row 2: near chase */
    { 1700.0f, 300.0f, 0.1257f, 0, 200, 2 },    /* row 6: far chase */
    { 1400.0f, 100.0f, 0.1257f, 0, 200, 2 },    /* row 5: the crash's, not the player's to choose */
    {    0.0f,   0.0f, 0.0f,    0, 200, 0 },    /* row 3: trackside (trackside_camera) */
};
#define CRASH_VIEW  CAR_VIEWS
#define TRACKSIDE_VIEW (CAR_VIEWS + 1)

/* the trackside cameras' places (view-table row 3): the nodes of MAPnRPL.SPL's first spline
   (the loader 0x14fb4 keeps it at 0xf90c, the routine 0x1a620 reads it), whole units: x, y,
   height; and whether the camera is on: from the race's end (0x20eb8, 0x20f88 ask for row 3) */
#define RPL_MAX     64
static int32_t rpl[RPL_MAX][3];
static int rpl_n;
static int trackside;
static int replaying;                   /* the race runs again from its keys (0x118e68) */

static struct car_state pc;
static struct track_model model;
static int view;
static int mark_at;                     /* the marker last reached */
static uint32_t wall_hits, crashes;
static struct car_race race;
int car_laps = 3;                       /* the option's default (0x1d2ac + 1) */
int car_mode = CAR_NORMAL;
static int carnage;                     /* TRACK_DATA's carnage time (0xcf4c) */
static int props_paid;                  /* the props down the last 1 s was given for (+0x7bc) */
/* slalom's gates (MAPnSLA.MRK, 32-byte records: radius, .., +0x10 seconds, +0x14 x, +0x1c y),
   whether each has been driven through (+0x694), how many (+0x7c0) */
#define GATES_MAX   64
static struct { int32_t r, seconds, x, y; } gate[GATES_MAX];
static uint8_t gate_done[GATES_MAX];
static int ngates, gates_done;
static uint8_t mark_seen[64];
static int mark_count;
static float needle, needle_speed;
static float progress;                  /* how far round the race (marker_progress) */
static float record;                    /* the lap to beat (0x1d268): the game's default, 180 s */
static float road;                      /* the road's heading at the nearest road point, set
                                           each step (player +0x13c: 0x16c90's first lines, which
                                           run every frame, 0x15ae4) */
static struct track_refl refl;          /* the car object's reflection state (record +0x24 ..) */
static uint32_t refl_phase;             /* its reflection picture (player +0x7b0, record +0x3c) */

/* the player's sounds (sound.h): the engine from the race's set-up (0x128dc), the skid, the wall */
static int snd_engine = -1, snd_skid = -1, snd_wall = -1, snd_over;

/* a crash (0x16c90 decides, 0x18518 runs it): thrown up and tumbling, four landings */
static struct { int on, bounces, view; float height, vy, pitch, roll, pitch_rate, roll_rate; } crash;

/* the body's corners for the walls (player_car, 0x136c8): each wheel's place moved 250 units
   ahead of the front wheels, 150 behind the rear ones; sideways at the wheels. [k][0] is to
   the right, [k][1] forward: front-left, rear-left, front-right, rear-right */
static float corner[4][2];
#define MAX_WALLS   1024
static float wall_in[MAX_WALLS];        /* which side of each wall the track is on */

/* a car model's blocks (mesh.c): place, the middle the drawing order goes by, how much further
   off it counts, whether it is a wheel and what it does; made the first time a car needs it */
#define MAX_BLOCKS  CAR_BLOCKS
#define MAX_MODELS  8
struct cblock { const uint8_t *blk; float t[3], mid[3]; int back, wheel, spins, steers; };
static struct cmodel { const uint8_t *mesh; struct cblock cb[MAX_BLOCKS]; int ncb, ready; } cmodels[MAX_MODELS];
static int pmodel;                      /* the player's */
static float spin, steer;               /* the wheels' turn and the front ones' angle (wheels_step) */

/* the four wheels' ground (front-left, rear-left, front-right, rear-right), and the same
   turned by the heading alone, which the chase camera follows */
static struct { float h[4], hc[4], cam_pitch, cam_roll; int ready; } ct;
static struct { float yaw, roll, pitch, height; int init; } chase;

/* the shadow's tilt: the car object's, but kept while it crashes (state 1, 0x1604c) */
static float shadow_pitch, shadow_roll;

/* the body on its suspension (0x168a4): each wheel's swing (+0x30 its clock, +0x38 sideways,
   +0x40 lengthways, +0x34 the deflection, wheel record +0x194 + 0x54 n; front-left, rear-left,
   front-right, rear-right), and what they make of the four body blocks: pitch +0x4e0, roll
   +0x4e8 (as the ground's +0x4c8 and +0x4d0 from the wheel heights, the deflections taken as
   heights) and the drop +0x4c0 (their average); the time since its last frame and the speed
   then (+0x588) */
static struct swing { float phase, lat, lng; int defl; } susp[4];
static struct { float pitch, roll; int drop; } body;
static float susp_clock, susp_speed;

/* the tyre smoke (0x19818 starts puffs, 0x19970 moves them): 8 slots (+0x308, 0x28 bytes
   each; only the player has them, 0x142e8 gives the opponents none), each an offset from the
   car in the model's frame, its place now and the phase of its 2/3 s */
#define PUFFS       8
static struct puff { int on; int32_t lx, lz, phase; float x, y, h; } puff[PUFFS];
static float puff_clock;                /* +0x304 */

/* the player's whole-number place as the game keeps it (car +0x4b0, +0x4b8, which the smoke
   reads as ds:0xfdc8, 0xfdd0): each move is added to it and the sum cut towards 0 (0x4d7b4:
   frndint with the rounding set to chop, then fistp; x 0x171ae, 0x17229, 0x1732d, z 0x171f8,
   0x1725a), so every step loses its fraction: a move a hair under 0, as off the grid with the
   heading just past pi, takes it a whole unit. Ours moves in floats (drive.c); this follows
   those moves (fx, fy: the float place it last took) */
static struct { int32_t x, y; float fx, fy; } whole;

static float wrap_pi(float a)
{
    while(a > PI) a -= 2.0f * PI;
    while(a < -PI) a += 2.0f * PI;
    return a;
}

/* player_contact (0x11a30, 0x16698): each wheel at its model place (x |cos roll|, z |cos
   pitch|) turned by heading + body slip takes the height of the drivable surface there (a
   wheel over nothing keeps its height); a wheel on grass is jolted by +-((x + y) & 15); the
   car's height is the plain average, pitch and roll from the right-hand wheels and the front
   ones. The model's +x is the car's left, its nose towards -z. */
static void contact(void)
{
    float tr = model.half_track, af = model.axle_front, ar = model.axle_rear;
    const float lx[4] = { tr, tr, -tr, -tr }, lz[4] = { -af, ar, -af, ar };
    float cr = fabsf(drive_cos(pc.roll)), cp = fabsf(drive_cos(pc.pitch));
    float a = pc.yaw + pc.drift, cs = drive_cos(a), sn = drive_sin(a);
    float jolted[4];
    int found[4];
    if(!ct.ready)
        for(int n = 0; n < 4; n++) ct.h[n] = ct.hc[n] = pc.z;
    pc.rough = 0;
    for(int n = 0; n < 4; n++) {
        float x = lx[n] * cr, z = lz[n] * cp;
        float wx = pc.x + x * cs - z * sn, wy = pc.y + x * sn + z * cs, h;
        int grass;
        found[n] = track_under(wx, wy, &h, &grass);
        if(found[n]) {
            ct.h[n] = h;
            pc.grass[n] = grass;                /* the wheel's own flag, kept over nothing */
            if(grass && !(n & 1)) pc.rough = 1;
        }
        jolted[n] = ct.h[n];
        if(pc.grass[n]) {
            int spot = (int)wx + (int)wy;
            jolted[n] += (float)((spot & 2) ? -(spot & 0xf) : (spot & 0xf));
        }
    }
    /* the same turned by the heading alone: the same places while the body does not slip */
    float cs2 = drive_cos(pc.yaw), sn2 = drive_sin(pc.yaw);
    for(int n = 0; n < 4; n++) {
        float x = lx[n] * cr, z = lz[n] * cp, h;
        if(pc.drift == 0.0f) {
            if(found[n]) ct.hc[n] = ct.h[n];
        } else if(track_under(pc.x + x * cs2 - z * sn2, pc.y + x * sn2 + z * cs2, &h, 0)) ct.hc[n] = h;
    }
    ct.cam_pitch = atanf((ct.hc[2] - ct.hc[3]) / (af + ar));
    ct.cam_roll  = atanf((ct.hc[2] - ct.hc[0]) / (2.0f * tr));
    ct.ready = 1;
    pc.z = 0.25f * (jolted[0] + jolted[1] + jolted[2] + jolted[3]);
    pc.pitch = atanf((jolted[2] - jolted[3]) / (af + ar));
    pc.roll  = atanf((jolted[2] - jolted[0]) / (2.0f * tr));
}

/* the road points by x for nearest_dir (dirs_prepare, at each race's start): their places as
   floats (whole numbers: exact) and their index in the file */
#define MAX_DIRS    1024
static struct sdir { float x, y; int i; } sdirs[MAX_DIRS];
static int nsdirs = -1;

static void dirs_prepare(void)
{
    int n;
    const struct track_dir *d = track_dirs(&n);
    nsdirs = n <= MAX_DIRS ? n : -1;
    for(int i = 0; i < nsdirs; i++) {
        struct sdir s = { (float)d[i].x, (float)d[i].y, i };
        int j = i;
        for(; j > 0 && sdirs[j - 1].x > s.x; j--) sdirs[j] = sdirs[j - 1];
        sdirs[j] = s;
    }
}

/* the road point nearest (x, y), of equally near ones the first in the file: out from x both
   ways through the points by x, each way ending where the distance in x alone (with a margin
   over the rounding) is past the nearest found; the distances as the search through all of
   them had them, so the same point */
static const struct track_dir *nearest_dir(float x, float y)
{
    int n;
    const struct track_dir *d = track_dirs(&n), *best = 0;
    float bd = 1e30f;
    if(nsdirs < 0 || nsdirs != n) {                     /* not in the table: all of them */
        for(int i = 0; i < n; i++) {
            float e = ((float)d[i].x - x) * ((float)d[i].x - x) + ((float)d[i].y - y) * ((float)d[i].y - y);
            if(e < bd) { bd = e; best = &d[i]; }
        }
        return best;
    }
    int lo = 0, hi = nsdirs, bi = -1;                   /* the first point with x >= the place's */
    while(lo < hi) {
        int m = (lo + hi) / 2;
        if(sdirs[m].x < x) lo = m + 1;
        else hi = m;
    }
    float reach = 1e30f;
    for(int way = 1; way >= -1; way -= 2)
        for(int k = way > 0 ? lo : lo - 1; k >= 0 && k < nsdirs; k += way) {
            const struct sdir *s = &sdirs[k];
            if(fabsf(s->x - x) > reach) break;
            float e = (s->x - x) * (s->x - x) + (s->y - y) * (s->y - y);
            if(e < bd || (e == bd && bi >= 0 && s->i < bi)) {
                bd = e;
                bi = s->i;
                reach = sqrtf(bd) * 1.001f + 1.0f;
            }
        }
    return bi >= 0 ? &d[bi] : 0;
}

static int nearest_wall(float x, float y)
{
    int n, best = -1;
    const struct track_wall *w = track_walls(&n);
    float bd = 1e30f;
    for(int i = 0; i < n && i < MAX_WALLS; i++) {
        float e = ((float)w[i].x - x) * ((float)w[i].x - x) + ((float)w[i].y - y) * ((float)w[i].y - y);
        if(e < bd) { bd = e; best = i; }
    }
    return best;
}

/* walls_prepare: which side of each wall is the track on? The side that has track 200 units
   from the wall's middle; if that does not decide it, the side the nearest road point is on
   (the files have walls authored both ways round) */
static void walls_prepare(void)
{
    int n;
    const struct track_wall *w = track_walls(&n);
    dirs_prepare();
    for(int i = 0; i < n && i < MAX_WALLS; i++) {
        float nx = cosf(w[i].angle), ny = -sinf(w[i].angle);
        int neg = track_under((float)w[i].x - nx * 200.0f, (float)w[i].y - ny * 200.0f, 0, 0);
        int pos = track_under((float)w[i].x + nx * 200.0f, (float)w[i].y + ny * 200.0f, 0, 0);
        if(neg != pos) { wall_in[i] = pos ? 1.0f : -1.0f; continue; }
        const struct track_dir *d = nearest_dir((float)w[i].x, (float)w[i].y);
        float side = d ? ((float)d->x - (float)w[i].x) * nx + ((float)d->y - (float)w[i].y) * ny : -1.0f;
        wall_in[i] = side > 0.0f ? 1.0f : -1.0f;
    }
}

/* player_walls (0x11e74, 0x11f98, 0x16b88, 0x16c90): each of the body's corners off the
   drivable surface is put back along the nearest wall's normal to 30 units inside its line
   (OpenScreamer's one departure from the game, which puts it 30 units on the other side of
   the line: on the inside of a bend the straight wall chords stand off the surface, and that
   threw the car out through the wall); the car moves by the largest push in x and in y, and
   the wall response scrubs speed, swings the nose and shoves */
static void walls_step(void)
{
    int nw;
    const struct track_wall *w = track_walls(&nw);
    float fx = drive_sin(pc.yaw), fy = -drive_cos(pc.yaw), rx = -drive_cos(pc.yaw), ry = -drive_sin(pc.yaw);
    float cr = fabsf(drive_cos(pc.roll)), cp = fabsf(drive_cos(pc.pitch));
    int out[4], any = 0;
    float px[4], py[4], wall_angle = 0.0f;
    const struct track_dir *d = nearest_dir(pc.x, pc.y);   /* 0x16cbb, before any wall */
    if(d) road = d->angle;
    for(int k = 0; k < 4; k++) {
        float lx = corner[k][0] * cr, lz = corner[k][1] * cp;
        px[k] = pc.x + rx * lx + fx * lz;
        py[k] = pc.y + ry * lx + fy * lz;
        out[k] = !track_under(px[k], py[k], 0, 0);
        any |= out[k];
    }
    if(!any) return;
    float mx = 0.0f, my = 0.0f;
    for(int k = 0; k < 4; k++) {
        if(!out[k]) continue;
        int i = nearest_wall(px[k], py[k]);
        if(i < 0) continue;
        float nx = cosf(w[i].angle), ny = -sinf(w[i].angle);
        float across = (px[k] - (float)w[i].x) * nx + (py[k] - (float)w[i].y) * ny;
        float depth = across * wall_in[i];              /* > 0: inside the line */
        if(depth >= 30.0f) continue;
        float push = (30.0f - depth) * wall_in[i], qx = push * nx, qy = push * ny;
        if(fabsf(qx) > fabsf(mx)) { mx = qx; wall_angle = w[i].angle; }
        if(fabsf(qy) > fabsf(my)) { my = qy; wall_angle = w[i].angle; }
    }
    if(mx == 0.0f && my == 0.0f) return;                /* off the surface but not at a wall */
    pc.x += mx;
    pc.y += my;
    if(crash.on) return;                                /* already in the air: no wall response */
    wall_hits++;
    /* 0x16c90: a crash when speed x (angle off the road's heading / 90 degrees) > 0.3 maxspd,
       and only out in the open (+0x7b0, the last marker's kind, is 0: not in a tunnel; the
       game checks that, OpenScreamer does not); the wall response comes all the same (the
       crash only stops the wall's sound there) */
    float off = d ? fabsf(pc.yaw - road) : 0.0f;          /* 0x16d17: against +0x13c */
    while(off > PI) off -= PI;
    if(off > PI / 2.0f) off = PI - off;
    int nm;
    const struct track_marker *mk = track_markers(&nm);
    int open = nm == 0 || mk[mark_at].kind == 0;
    int crashing = pc.speed * (off / (PI / 2.0f)) > pc.k_wall && open;
    drive_hit_wall(&pc, mx, my, (float)(out[2] - out[0]), wall_angle);
    if(!crashing) snd_wall = snd_play_once(SND_WALL, snd_wall);    /* unless it still sounds (0x16f0b) */
    if(crashing) {
        snd_play(SND_DENT);                             /* 0x18540 */
        speech_say(SAY_CRASH, 3, 0.5f);
        /* the crash's start (0x18518 state 2): speed x 0.6, heading the road's, thrown up at
           3000 units/s, turning 12 rad/s in pitch and in roll, the crash view */
        crash.on = 1;
        crash.bounces = 0;
        crash.view = view;
        crash.height = crash.pitch = crash.roll = 0.0f;
        crash.vy = 3000.0f;
        crash.pitch_rate = crash.roll_rate = 12.0f;
        pc.speed *= 0.6f;
        if(d) pc.yaw = d->angle;
        pc.drift = 0.0f;
        pc.spin = 0;
        crashes++;
        view = CRASH_VIEW;
        chase.init = 0;
    }
}

static int32_t whole_add(int32_t w, float d)
{
    return w + (int32_t)((float)w + d >= 0.0f ? floorf(d) : ceilf(d));
}

/* after a step's move: added to the whole-number place, the sum cut towards 0 */
static void whole_step(void)
{
    whole.x = whole_add(whole.x, pc.x - whole.fx);
    whole.y = whole_add(whole.y, pc.y - whole.fy);
    whole.fx = pc.x;
    whole.fy = pc.y;
}

/* the game's tick counter T (obj1:0x7fd, counted up by its 100 Hz timer's callback 0x1020d;
   the fixups at 0x198b8 and 0x10215 both name object 1, not the data object, whose 0x7fd,
   the middle of a constant, OpenScreamer took for a fixed 51), from the cycle counter: it
   runs on whatever the frame rate */
static uint32_t ticks(void)
{
    static uint32_t last, rest, t;
    uint32_t now = rdcycle();
    rest += now - last;
    last = now;
    t += rest / (CPU_HZ / 100);
    rest %= CPU_HZ / 100;
    return t;
}

/* 0x19818, called on every step that smokes: the clock (+0x304) gains the step's time while
   it is under 0.05 s (ds:0x8cb); else it starts again and each rear wheel (records 1 and 3,
   rear left and rear right) takes the first free slot: its place on the model pushed sideways
   by (x & 7) (T & 63) and back by (z & 7) (T & 63), x and z the player's whole-number place
   as the game keeps it (ds:0xfdc8, 0xfdd0: the car record 0x10f918's +0x4b0, +0x4b8; whole,
   not the float place cut: off the grid that sits a hair under 2600, x & 7 = 7, every push
   the largest); the right one's push turned round when T has bit 1 (0x198bf: T and the
   wheel's index 2). The pushes go out to the left and either way on the right, up to 441
   units. At 50 steps a second a pair every 4 steps. */
static void smoke_spawn(void)
{
    if(puff_clock < 0.05f) {
        puff_clock += DRIVE_DT;
        return;
    }
    puff_clock = 0.0f;
    uint32_t t = ticks();
    int32_t tt = (int32_t)(t & 63), wx = (int32_t)model.half_track, wz = (int32_t)model.axle_rear;
    int k = 0;
    for(int w = 0; w < 4; w += 2) {
        while(k < PUFFS && puff[k].on) k++;
        if(k == PUFFS) return;
        struct puff *p = &puff[k];
        p->on = 1;
        p->phase = 0;
        p->lx = (w ? -wx : wx) + (whole.x & 7) * tt * ((t & (uint32_t)w) ? -1 : 1);
        p->lz = wz + (whole.y & 7) * tt;
    }
}

/* 0x19970, every step after the skid: each puff where the car is now, its offset turned by
   heading + slip (0x2216c) with no height, then about the world's x axis by the car's pitch
   (0x221dc), in whole units from the car's whole-number place: it goes with the car. Its phase
   gains 1.5 x 65535 a second (ds:0x8d3, 0x118e8: 1966 a step) and it ends at 0x10000
   (0x11930), after 34 steps. */
static void smoke_step(void)
{
    int any = 0;
    for(int k = 0; k < PUFFS; k++) any |= puff[k].on;
    if(!any) return;
    float a = pc.yaw + pc.drift, ca = drive_cos(a), sa = drive_sin(a), cp = drive_cos(pc.pitch), sp = drive_sin(pc.pitch);
    int32_t cx = (int32_t)pc.x, cy = (int32_t)pc.y, ch = (int32_t)pc.z;
    for(int k = 0; k < PUFFS; k++) {
        struct puff *p = &puff[k];
        if(!p->on) continue;
        float lx = (float)p->lx, lz = (float)p->lz;
        int32_t dx = (int32_t)(lx * ca - lz * sa), dy = (int32_t)(lx * sa + lz * ca);
        int32_t dh = (int32_t)((float)dy * sp);
        dy = (int32_t)((float)dy * cp);
        p->x = (float)(cx + dx);
        p->y = (float)(cy + dy);
        p->h = (float)(ch + dh);
        p->phase = (int32_t)((float)p->phase + DRIVE_DT * 1.5f * 65535.0f);
        if(p->phase >= 0x10000) p->on = 0;
    }
}

/* a crash's step: gravity 6000, the turning dies away (damp[97]); each landing throws it up
   again at 0.4 times the fall, level, with a new small turn, and smokes (0x18727); the fourth
   ends it */
static void crash_step(void)
{
    if(!crash.on) return;
    crash.height += crash.vy * DRIVE_DT;
    crash.vy -= DRIVE_DT * 6000.0f;
    crash.pitch_rate *= drive_damp(97);
    crash.pitch += crash.pitch_rate * DRIVE_DT;
    crash.roll_rate *= drive_damp(97);
    crash.roll += crash.roll_rate * DRIVE_DT;
    if(crash.height > 0.0f) return;
    smoke_spawn();
    crash.bounces++;
    crash.height = 0.0f;
    crash.vy = -crash.vy * 0.4f;
    crash.pitch = crash.roll = 0.0f;
    crash.roll_rate = (crash.bounces & 1) ? -0.6f : 0.6f;
    crash.pitch_rate = -1.0f;
    if(crash.bounces > 3) {
        crash.on = 0;
        view = crash.view;
        chase.init = 0;
    }
}

/* markers_step (0x1820c, 0x19490): the clocks run; the marker last reached and its two
   neighbours: inside one's circle it becomes the last reached and its slip the surface factor
   of the cornering force; the line starts the clocks, and once every other marker has been
   passed it counts a lap; the line and a checkpoint (flag 2) give the bonus */
static void markers_step(void)
{
    if(race.on && !race.finished) {
        if(race.clock_on) {
            race.lap += DRIVE_DT;
            race.total += DRIVE_DT;
        }
        race.timer -= DRIVE_DT;                         /* 0x18ad0 */
        race.time_out = race.timer < 0.0f;
        if(race.time_out) race.timer = 0.0f;
    }
    int n;
    const struct track_marker *mk = track_markers(&n);
    if(n == 0) return;
    float bonus = (float)(car_mode & (CAR_CONES | CAR_SLALOM) ? carnage : track_bonus_time());  /* 0x195fc */
    if(car_mode == CAR_CONES) {                         /* 0x18b50: more props down than paid for */
        int down = scenery_hits(0);
        if(down > props_paid) {
            race.timer += 1.0f;
            props_paid = down;
        }
    }
    if(car_mode == CAR_SLALOM) {                        /* 0x18b98: the first gate the car is in */
        int32_t x = (int32_t)pc.x, y = (int32_t)pc.y;
        for(int i = 0; i < ngates; i++) {
            int32_t dx = x - gate[i].x, dy = y - gate[i].y, r = gate[i].r;
            if(dx >= r || -dx >= r || dy >= r || -dy >= r || sqrtf((float)(dx * dx + dy * dy)) > (float)r) continue;
            if(!gate_done[i]) {                         /* the first time through: its seconds */
                gate_done[i] = 1;
                gates_done++;
                race.timer += (float)gate[i].seconds;
                snd_play(SND_DOOR);
            }
            break;
        }
    }
    for(int k = -1; k <= 1; k++) {
        int i = ((mark_at + k) % n + n) % n;
        const struct track_marker *m = &mk[i];
        float dx = pc.x - (float)m->x, dy = pc.y - (float)m->y, rad = (float)m->radius;
        if(fabsf(dx) >= rad || fabsf(dy) >= rad) continue;
        if(sqrtf(dx * dx + dy * dy) > rad) continue;
        mark_at = i;
        pc.surface = m->slip;
        if(i == 0 && !race.clock_on && race.on) {        /* the start: clocks on */
            race.clock_on = 1;
            race.lap = 0.0f;
            race.timer = race.timer / 5.0f + bonus;
            speech_rearm(SAY_HURRY);
        } else if(i == 0 && mark_count >= n - 1) {        /* across the line */
            if(race.laps + 1 >= race.total_laps) {
                race.finished = 1;
                int all, down = scenery_hits(&all);     /* PERFECT! (flag 0x8000): */
                if(car_mode == CAR_CONES) race.perfect = down == all;   /* every prop down (0x23fbc) */
                if(car_mode == CAR_SLALOM) race.perfect = gates_done == ngates && down == 0;    /* every
                                                           gate, no prop touched (0x23f68) */
                race.made_it = car_mode != CAR_ATTACK || ghost_finish(race.total);      /* 0x23ef0 */
            } else if(!race.finished) {
                race.timer = race.timer / 5.0f + bonus;
                speech_rearm(SAY_HURRY);
            }
            race.laps++;
            race.last = race.lap;
            /* 0x196ea: a lap under the record is the new one, "new lap record"; 0x19668: the lap
               call, recording 2 into the last lap, 1 after the race's best lap, else 0 */
            int best = race.best == 0.0f || race.last < race.best;
            if(race.last < record) {
                record = race.last;
                speech_say(SAY_NEWLAP, 3, 0.2f);
            }
            if(!race.finished) speech_say(SAY_LAP, race.laps + 1 == race.total_laps ? 2 : best, 0.0f);
            if(best) race.best = race.last;
            for(int j = 0; j < n && j < 64; j++) mark_seen[j] = 0;
            mark_count = 0;
            race.lap = 0.0f;
        }
        if(i != 0 && i < 64 && !mark_seen[i]) {
            mark_seen[i] = 1;
            mark_count++;
            if((m->flags & 2) && race.clock_on && !race.finished) {
                /* 0x19490: the checkpoint call by the time left: under 3 s recording 2, up to
                   15 s recording 0, more recording 1 */
                speech_say(SAY_CHECK, race.timer < 3.0f ? 2 : race.timer <= 15.0f ? 0 : 1, 0.0f);
                race.timer = race.timer / 5.0f + bonus;
                speech_rearm(SAY_HURRY);
            }
        }
    }
    progress = car_marker_progress(mark_at, race.laps, pc.x, pc.y);
}

/* how far a car has got (0x1820c's last lines for the player, 0x1db84's for an opponent):
   laps x 100 + the marker last reached + its distance from that marker over the distance from
   that marker to the next, both cut to whole twentieths of a unit */
float car_marker_progress(int at, int laps, float x, float y)
{
    int n;
    const struct track_marker *mk = track_markers(&n);
    if(n == 0) return 0.0f;
    const struct track_marker *a = &mk[at], *b = &mk[(at + 1) % n];
    float sx = (float)(b->x - a->x), sy = (float)(b->y - a->y), gx = x - (float)a->x, gy = y - (float)a->y;
    int span = (int)(sqrtf(sx * sx + sy * sy) * 0.05f), gone = (int)(sqrtf(gx * gx + gy * gy) * 0.05f);
    return (span ? (float)gone / (float)span : 0.0f) + (float)at + (float)(laps * 100);
}

float car_progress(void)
{
    return progress;
}

int car_backwards(void)
{
    return fabsf(wrap_pi(pc.yaw - road - PI)) < PI / 4.0f;
}

float car_circles(float cx[2], float cy[2])
{
    float nose = model.axle_front + 250.0f, tail = model.axle_rear + 150.0f, q = (nose + tail) * 0.25f;
    float fx = drive_sin(pc.yaw), fy = -drive_cos(pc.yaw), zf = nose - q, zr = -(tail - q);
    cx[0] = pc.x + fx * zf; cy[0] = pc.y + fy * zf;
    cx[1] = pc.x + fx * zr; cy[1] = pc.y + fy * zr;
    return q;
}

void car_shift(float dx, float dy)
{
    pc.x += dx;
    pc.y += dy;
}

void car_rammed(float nx, float ny, int left)
{
    pc.shove = pc.speed * 0.21f;
    pc.shove_x = nx;
    pc.shove_y = ny;
    pc.yaw_rate = pc.speed * (left ? 0.0002f : -0.0002f);
    pc.speed *= drive_damp(90);
    wall_hits++;
}

/* chase_step (0x1a53d): the camera eases to the car's heading (without its skid angle) and
   to the lean and nose-up of the heading-only wheel set; its height to the car's + above -
   behind sin(pitch) */
static void chase_step(void)
{
    const struct view_def *v = &views[view];
    float want_pitch = ct.cam_pitch - v->down;
    if(!chase.init) {
        chase.yaw = pc.yaw;
        chase.roll = ct.cam_roll;
        chase.pitch = want_pitch;
        chase.height = pc.z + v->above - v->behind * sinf(chase.pitch);
        chase.init = 1;
        return;
    }
    float k = drive_damp(0x51);
    chase.yaw   = pc.yaw      + wrap_pi(chase.yaw   - pc.yaw)      * k;
    chase.roll  = ct.cam_roll + wrap_pi(chase.roll  - ct.cam_roll) * k;
    chase.pitch = want_pitch  + wrap_pi(chase.pitch - want_pitch)  * k;
    float want_h = pc.z + v->above - v->behind * sinf(chase.pitch);
    chase.height = want_h + (chase.height - want_h) * drive_damp(0);
}

void car_init(int which, int replay)
{
    int n, nm;
    const struct car_spec *cars = track_cars(&n);
    const struct track_model *models = track_models(&nm);
    const struct car_spec *sp = &cars[(which % n + n) % n];
    int k = car_model_index(sp);
    model = models[k >= 0 && k < nm ? k : 0];
    drive_init(&pc, sp, model.wheelbase > 0.0f ? model.wheelbase : 935.0f);
    float zf = model.axle_front + 250.0f, zr = model.axle_rear + 150.0f;
    corner[0][0] = -model.half_track; corner[0][1] =  zf;
    corner[1][0] = -model.half_track; corner[1][1] = -zr;
    corner[2][0] =  model.half_track; corner[2][1] =  zf;
    corner[3][0] =  model.half_track; corner[3][1] = -zr;
    walls_prepare();
    pmodel = k;
    spin = steer = 0.0f;
    for(int j = 0; j < 4; j++) susp[j] = (struct swing){ 0 };
    body.pitch = body.roll = 0.0f;                      /* (0x13f28: none at the start) */
    body.drop = 0;
    susp_clock = susp_speed = 0.0f;
    wall_hits = crashes = 0;
    crash.on = 0;
    replaying = replay;
    trackside = 0;
    if(replay) car_trackside();                         /* row 3 asked for (0x12afe) */
    snd_engine = snd_stop(snd_engine);
    snd_skid = snd_stop(snd_skid);
    snd_wall = -1;
    snd_over = 0;
    snd_engine = snd_play(SND_ENGINE);
    speech_reset();
    if(!replay) record = car_mode == CAR_ATTACK ? ghost_time() : car_mode & CAR_ALONE ? 0.0f : 180.0f;
                                                        /* (0x1ba3c); a lap the race beat it with stays */
    race = (struct car_race){ 0 };
    race.timer = 15.0f;
    refl = (struct track_refl){ { 0, 0, 0 }, { 0.0f, 0.0f, 0.0f } };
    refl_phase = 0;
    track_reflection_rate(0.008f);
    race.total_laps = car_mode & CAR_ALONE ? 1 : car_laps;
    props_paid = gates_done = 0;
    for(int j = 0; j < GATES_MAX; j++) gate_done[j] = 0;
    for(int j = 0; j < 64; j++) mark_seen[j] = 0;
    mark_count = 0;
    needle = needle_speed = 0.0f;
    pc.waiting = 1;

    /* player_reset: on the grid, facing as the nearest road point (0x13e07, the image's
       start pose), at the third path point's height until the wheels find the ground; no
       skidding until a marker */
    float px, py, ph;
    track_start(&pc.x, &pc.y, &pc.yaw);
    if(car_mode & CAR_ALONE) {          /* 0x13ded: the last opponent's place, in time attack */
        int rows, ns = track_opp_count(&rows), start = 0, nn = 0;       /* the ghost's other one */
        int k = (ns - 1 < rows ? ns - 1 : rows) - 1 + (car_mode == CAR_ATTACK ? ghost_slot() : 0);
        if(k > ns - 1) k = ns - 1;
        const struct track_spline_node *s = k >= 0 ? track_opp_spline(k, &start, &nn) : 0;
        const struct track_dir *d = s && start >= 0 && start < nn ? nearest_dir((float)s[start].x, (float)s[start].y) : 0;
        if(d) {                                         /* the road's heading there, as the grid's */
            pc.x = (float)s[start].x;
            pc.y = (float)s[start].y;
            pc.yaw = d->angle;
        }
    }
    road = pc.yaw;
    track_path_point(2, &px, &py, &ph);
    pc.z = ph;
    pc.surface = 0.0f;
    mark_at = 0;
    progress = 0.0f;
    ct.ready = 0;
    for(int k = 0; k < MAX_MODELS; k++)         /* each track's image has its own car models */
        cmodels[k].ready = 0;
    contact();
    chase.init = 0;
    chase_step();
    shadow_pitch = -pc.pitch;
    shadow_roll = -pc.roll;
    for(int j = 0; j < PUFFS; j++) puff[j].on = 0;
    puff_clock = 0.0f;
    whole.x = (int32_t)pc.x;
    whole.y = (int32_t)pc.y;
    whole.fx = pc.x;
    whole.fy = pc.y;
}

/* 0x1813c, while the car drives (not in a crash): the reflection picture by the kind of the
   marker last reached, 0 the sky, 1 (a tunnel) 0x8000, 2 0xff00 (frame n x phase >> 16 of the
   window material's texture: the tunnel's ceiling), and how fast reflections slide, 0.002 in
   the open, 0.03 under a roof (the program's 0.008 until then); other kinds leave both */
static void refl_step(void)
{
    int nm;
    const struct track_marker *mk = track_markers(&nm);
    if(crash.on || nm == 0) return;
    int kind = mk[mark_at].kind;
    if(kind < 0 || kind > 2) return;
    refl_phase = kind == 0 ? 0 : kind == 1 ? 0x8000 : 0xFF00;
    track_reflection_rate(kind == 0 ? 0.002f : 0.03f);
}

/* the engine's note every step (0x19408): pitch rpm x 14 + 32768, automoto, autogall in a tunnel
   (the last marker's kind 1); the skid's sample while the skid flag of 0x17d04 is up (0x180d0,
   the same flag as the smoke's) or the wheels spin off the line in a gear (0x1883d); both stop
   when the race is over */
static void sound_step(int skid)
{
    if(race.finished || race.game_over) {
        if(!snd_over) {
            snd_over = 1;
            snd_engine = snd_stop(snd_engine);
            snd_skid = snd_stop(snd_skid);
        }
        return;
    }
    int nm;
    const struct track_marker *mk = track_markers(&nm);
    snd_swap(snd_engine, nm && mk[mark_at].kind == 1 ? SND_ENGINE_TUNNEL : SND_ENGINE);
    snd_pitch(snd_engine, (int32_t)(pc.rpm * 14.0f + 32768.0f));
    if(skid) {
        if(!snd_playing(snd_skid)) snd_skid = snd_play(SND_SKID);
    } else
        snd_skid = snd_stop(snd_skid);
}

uint32_t car_keys(uint32_t keys, uint32_t keys_before)
{
    const uint32_t shift = KEY_L | KEY_R;
    return (keys & (KEY_B | KEY_Y | KEY_LEFT | KEY_RIGHT)) | (keys & ~keys_before & shift);
}

int car_replay(void)
{
    return replaying;
}

/* 0x168a4, each frame after the drive (0x15b9c): the change of speed since the frame before
   (+0x50c - +0x588, 0x15bec) x 0.00175 kicks the front wheels one way and the rear ones three
   times as hard the other, the heading's turn a second x speed^2 x 1.5e-9 the left ones
   against the right; both over the frame's time, a kick bigger than the swing restarts it (the
   lengthways one eased in, stiffness 0x3d). A wheel swings as cos(4 pi t + pi) x (both), at
   most 18 units; the swings die away (stiffness 61 sideways, 71 lengthways), under 2 gone.
   The sideways kick, over the frame's time twice, and the cut at 2 go with the frame rate:
   this runs on the game's frames of 1/30 s (as the opponents' lean, opp.c), its stiffnesses
   pow(0.5 + i x 0.005, 50 / 31) as the game's table has them at 30 frames a second */
#define GAME_DT     (1.0f / 30.0f)
static void suspension_step(void)
{
    susp_clock += DRIVE_DT;
    if(susp_clock < GAME_DT) return;
    susp_clock -= GAME_DT;
    const float damp61 = 0.70478698f, damp71 = 0.77672615f;
    float a = (pc.speed - susp_speed) * 0.00175f;
    float b = pc.speed * pc.speed * 1.5e-9f * (pc.turn / DRIVE_DT);
    susp_speed = pc.speed;
    for(int n = 0; n < 4; n++) {
        struct swing *w = &susp[n];
        float lng = ((n & 1) ? a * 3.0f : -a) / GAME_DT, lat = ((n & 2) ? b : -b) / GAME_DT;
        if(fabsf(w->lat) < fabsf(lat)) {
            w->phase = 0.0f;
            w->lat = lat;
        }
        if(fabsf(w->lng) < fabsf(lng)) {
            w->phase = 0.0f;
            w->lng = (w->lng - lng) * damp61 + lng;
        }
        int d = (int)(cosf(w->phase * 4.0f * PI + PI) * (w->lat + w->lng));
        w->defl = d > 18 ? 18 : d < -18 ? -18 : d;
        w->lat *= damp61;
        if(fabsf(w->lat) < 2.0f) w->lat = 0.0f;
        w->lng *= damp71;
        if(fabsf(w->lng) < 2.0f) w->lng = 0.0f;
        w->phase += GAME_DT;
    }
    /* roll -atan((front-left - front-right) / track), pitch atan((front-right - rear-right) /
       wheelbase); the game's track (+0x2e4) is -(x front-left - x front-right), the left at +x:
       negative, so the outer side goes down in a bend and the nose under braking */
    body.drop = (susp[0].defl + susp[1].defl + susp[2].defl + susp[3].defl) / 4;
    body.roll = atanf((float)(susp[0].defl - susp[2].defl) / (2.0f * model.half_track));
    body.pitch = atanf((float)(susp[2].defl - susp[3].defl) / pc.wheelbase);
}

void car_step(uint32_t keys)
{
    struct drive_input in;
    /* gas and brake on the buttons only (on the d-pad a thumb steering into a diagonal
       would brake or lift off by accident), as the classic racing games lay them out: gas
       on the bottom button (B), the brake on the left one (Y); L and R shift on the step they
       went down (car_keys) */
    in.throttle = (keys & KEY_B) ? 1.0f : 0.0f;
    in.brake = (keys & KEY_Y) ? 1.0f : 0.0f;
    in.steer = ((keys & KEY_LEFT) ? 1.0f : 0.0f) - ((keys & KEY_RIGHT) ? 1.0f : 0.0f);
    in.shift = (keys & KEY_R) ? 1 : (keys & KEY_L) ? -1 : 0;
    /* the start lights: green at 6 s; until then the pedal only revs the engine */
    race.clock += DRIVE_DT;
    race.on = race.clock >= 6.0f;
    pc.waiting = !race.on;
    if(race.time_out) {                                 /* 0x18a04: out of time and stopped: over */
        in.throttle = 0.0f;
        if(pc.speed < 1.0f) race.game_over = 1;
    }
    if(race.finished || race.game_over) { in.throttle = 0.0f; in.brake = 0.4f; }
    if(race.finished || race.game_over) car_trackside();    /* row 3 asked for */
    if(crash.on) in.throttle = in.steer = 0.0f;          /* no drive in the air */
    int crashed = crash.on;
    crash_step();
    drive_step(&pc, &in);
    suspension_step();
    whole_step();
    /* 0x17d04's smoke: its branch for locked wheels, taken only out of a crash and while the
       engine pulls (flag 0x40 at 0x18e19, time over), or the body more than 0.23 rad off its
       heading (ds:0x810, 0x180be); and state 5, the wheels spinning off the line, smokes in a
       gear (0x18879) and starts the skid's sample too (0x1883d) */
    uint32_t t0 = rdcycle();
    int skid = (pc.skid_lock && !crash.on && !race.time_out) || fabsf(pc.drift) > 0.23f;
    int wheelspin = pc.launch && pc.gear != 0;
    if(skid) smoke_spawn();
    if(wheelspin) smoke_spawn();
    efx_cyc_objects += rdcycle() - t0;
    walls_step();
    markers_step();
    refl_step();
    contact();
    chase_step();
    t0 = rdcycle();
    smoke_step();
    /* the shadow's tilt (0x1604c): the car object's, but not while it crashes */
    if(!crashed || !crash.on) {
        shadow_pitch = -pc.pitch + crash.pitch;
        shadow_roll = -pc.roll + crash.roll;
    }
    efx_cyc_objects += rdcycle() - t0;
    sound_step(skid || wheelspin);
    speech_step();
    struct track_cam ear;                              /* the sounds with a place are heard from */
    car_camera(&ear);                                   /* the camera (0x1dffc, 0x21b80) */
    snd_ear(&ear);

    /* wheels_step: the wheels turn with the speed; the front ones steer by the share of the
       lock, the sign as OpenScreamer has it since its fix (the model's +x is the car's left) */
    spin = fmodf(spin - pc.speed * DRIVE_DT / 108.6f, 6.2831853f);
    steer = -(pc.steer_max > 0.0f ? pc.steer / pc.steer_max : 0.0f) / 1.5f;

    /* dash_step (the race screen's 0x2367c eases them each frame, stiffness 0x5a): the rev
       counter's needle to rpm / maxrpm x pi on the outside views' instrument, x 3.613 on the
       dashboard (0x23739), no further; the dashboard's speed needle, only while it is shown, to
       speed / maxspd x 4.084 (0x236ec), unbounded */
    int dash = views[view].overlay == 1;
    float full = dash ? 3.6128317f : PI, want = pc.rpm / pc.spec.maxrpm * full;
    if(want > full) want = full;
    needle = want + (needle - want) * drive_damp(0x5a);
    ghost_step();                                       /* time attack (0x231d0, 0x23018) */
    if(dash) {
        want = pc.speed / pc.spec.maxspd * 4.0840706f;
        needle_speed = want + (needle_speed - want) * drive_damp(0x5a);
    }
}

const struct car_race *car_race(void)
{
    return &race;
}

float car_needle(void)
{
    return needle;
}

float car_speed_needle(void)
{
    return needle_speed;
}

/* a car's mesh as car_draw takes it */
static void cmodel_build(struct cmodel *m, const uint8_t *mesh)
{
    /* the blocks: a name of three letters ending in i (a tyre, the arches' insides) counts
       4000 further off, one ending in x (a hub) 1000, unless it starts with A */
    m->mesh = mesh;
    m->ncb = 0;
    for(int b = 0; b < *(const int32_t *)(m->mesh + 0x14) && m->ncb < MAX_BLOCKS; b++) {
        struct cblock *q = &m->cb[m->ncb++];
        q->blk = m->mesh + *(const int32_t *)(m->mesh + 0x18 + 4 * b);
        int32_t so = *(const int32_t *)(q->blk + 0x0C);
        for(int j = 0; j < 3; j++) q->t[j] = so > 0 ? (float)*(const int32_t *)(m->mesh + so + 0x10 + 4 * j) : 0.0f;
        for(int j = 0; j < 3; j++) q->mid[j] = (float)*(const int16_t *)(q->blk + 4 + 2 * j) + q->t[j];
        const uint8_t *nmz = q->blk + 0x68;
        int named = nmz[0] != 'A' && nmz[0] != 'a' && nmz[3] == 0;
        q->back = named && nmz[2] == 'i' ? 4000 : named && nmz[2] == 'x' ? 1000 : 0;
        /* a wheel sits off-centre; only its hub disc (flags 0x800) turns, the tyre block is
           the lower half of the tread and stays; the front ones (local -z) steer */
        q->wheel = q->t[0] != 0.0f;
        q->spins = q->wheel && (*(const int32_t *)(q->blk + 0x18) & 0x800);
        q->steers = q->wheel && q->t[2] < 0.0f;
    }
    m->ready = 1;
}

/* model k (AUTOk) */
static const struct cmodel *cmodel(int k)
{
    int nm;
    track_models(&nm);
    if(k < 0 || k >= nm || k >= MAX_MODELS) k = 0;
    struct cmodel *m = &cmodels[k];
    if(!m->ready) cmodel_build(m, track_model_mesh(k));
    return m;
}

int car_model_index(const struct car_spec *sp)
{
    return (sp->name[4] - '0') * 10 + (sp->name[5] - '0');           /* "AUTO03" -> 3 */
}

/* a car as it is drawn: the cos and sin of the wheels' turn and the front ones' angle, of the
   object's pitch, roll and heading, of the body's own pitch and roll, and its place; how far
   the body is dropped */
struct pose { float sc[4], car[6], body[4]; int lean; float x, y, z, drop; };

/* A block's point p (as stored) in the world (block_to_view, tilt, place): a wheel turned
   about its axle and steered, moved to its place on the car, a body block leant on its own,
   the object's pitch and roll, its heading, its place */
static void block_point(const struct cblock *q, const struct pose *ps, const float p[3], float w[3])
{
    const float *sc = ps->sc, *car = ps->car;
    float x = p[0], y = p[1], z = p[2];
    if(q->wheel) {
        float wcs = q->spins ? sc[0] : 1.0f, wsn = q->spins ? sc[1] : 0.0f;
        float scs = q->steers ? sc[2] : 1.0f, ssn = q->steers ? sc[3] : 0.0f;
        float y2 = y * wcs - z * wsn, z2 = z * wcs + y * wsn;
        float x2 = x * scs + z2 * ssn;
        z = z2 * scs - x * ssn;
        x = x2;
        y = y2;
    }
    x += q->t[0]; y += q->t[1]; z += q->t[2];
    if(ps->lean && !q->wheel) {     /* the body's lean (the suspension, an opponent in a bend) */
        float yb = y * ps->body[0] - z * ps->body[1], zb = z * ps->body[0] + y * ps->body[1];
        float xb = x * ps->body[2] - yb * ps->body[3];
        y = yb * ps->body[2] + x * ps->body[3] - ps->drop;
        x = xb;
        z = zb;
    }
    /* tilt: the object's pitch and roll are the car's, negated (player_sync) */
    float y3 = y * car[0] + z * car[1], z3 = z * car[0] - y * car[1];
    float x3 = x * car[2] - y3 * car[3], y4 = y3 * car[2] + x * car[3];
    w[0] = ps->x + x3 * car[4] - z3 * car[5];
    w[1] = ps->y + x3 * car[5] + z3 * car[4];
    w[2] = ps->z + y4;
}

/* the block's place in the world: its origin and the images of its three axes */
static void place_block(const struct cblock *q, const struct pose *ps, struct track_block *t)
{
    static const float zero[3] = { 0.0f, 0.0f, 0.0f }, ex[3] = { 1.0f, 0.0f, 0.0f };
    static const float ey[3] = { 0.0f, 1.0f, 0.0f }, ez[3] = { 0.0f, 0.0f, 1.0f };
    float o[3], px[3], py[3], pz[3];
    block_point(q, ps, zero, o);
    block_point(q, ps, ex, px);
    block_point(q, ps, ey, py);
    block_point(q, ps, ez, pz);
    for(int j = 0; j < 3; j++) {
        t->o[j] = o[j];
        t->m[3 * j] = px[j] - o[j];
        t->m[3 * j + 1] = py[j] - o[j];
        t->m[3 * j + 2] = pz[j] - o[j];
    }
}

/* m's blocks placed as lk, the body dropped by drop, into out in the game's order for the
   camera: how many */
static int draw_model(const struct cmodel *m, const struct car_look *lk, float drop, const struct track_cam *cam,
                      struct track_block *out)
{
    static struct track_block tb[MAX_BLOCKS];
    struct pose ps = { { cosf(lk->spin), sinf(lk->spin), cosf(lk->steer), sinf(lk->steer) },
                       { cosf(lk->pitch), sinf(lk->pitch), cosf(lk->roll), sinf(lk->roll), cosf(lk->yaw), sinf(lk->yaw) },
                       { cosf(lk->body_pitch), sinf(lk->body_pitch), cosf(lk->body_roll), sinf(lk->body_roll) },
                       lk->body_pitch != 0.0f || lk->body_roll != 0.0f || drop != 0.0f, lk->x, lk->y, lk->z, drop };
    int ord[MAX_BLOCKS];
    float key[MAX_BLOCKS];
    for(int b = 0; b < m->ncb; b++) {
        const struct cblock *q = &m->cb[b];
        struct track_block *t = &tb[b];
        float mid[3], mw[3];
        place_block(q, &ps, t);
        t->file = m->mesh;
        t->blk = q->blk;
        t->phase = lk->phase;
        /* the drawing order (0x8ac58, 0x8af24): the furthest middle first, the tyres and
           hubs counted further off; on a tie the lower block first */
        for(int j = 0; j < 3; j++) mid[j] = q->mid[j] - q->t[j];
        block_point(q, &ps, mid, mw);
        float dx = mw[0] - cam->x, dy = mw[1] - cam->y, dh = mw[2] - cam->h;
        key[b] = -rintf(sqrtf(dx * dx + dy * dy + dh * dh)) - (float)q->back;
        int i = b;
        while(i > 0 && key[ord[i - 1]] > key[b]) { ord[i] = ord[i - 1]; i--; }
        ord[i] = b;
    }
    for(int b = 0; b < m->ncb; b++) out[b] = tb[ord[b]];
    return m->ncb;
}

static int draw_car(int model, const struct car_look *lk, float drop, const struct track_cam *cam,
                    struct track_block *out, struct track_object *obj)
{
    const struct cmodel *m = cmodel(model);
    if(!m->ncb) return 0;
    draw_model(m, lk, drop, cam, out);
    obj->x = lk->x;
    obj->y = lk->y;
    obj->h = lk->z;
    obj->radius = 1200.0f;
    obj->n = m->ncb;
    obj->b = out;
    return 1;
}

int car_draw(int model, const struct car_look *lk, const struct track_cam *cam, struct track_block *out,
             struct track_object *obj)
{
    return draw_car(model, lk, 0.0f, cam, out, obj);
}

int car_mesh_draw(const uint8_t *mesh, const struct car_look *lk, const struct track_cam *cam,
                  struct track_block *out)
{
    static struct cmodel m;
    cmodel_build(&m, mesh);
    return draw_model(&m, lk, 0.0f, cam, out);
}

/* the player's car's blocks, when it comes up in the drawing */
static int car_make(int id, const struct track_cam *cam, struct track_object *obj)
{
    static struct track_block blocks[MAX_BLOCKS];
    struct car_look lk;
    (void)id;
    car_player_look(&lk);
    return draw_car(pmodel, &lk, (float)body.drop, cam, blocks, obj);
}

/* the object's angles (player_sync): the car's, negated, and a crash's tumbling on top; the four
   body blocks (0x15c0c): pitch +0x4e0 x 2, roll +0x4e8 x 4 (the height less +0x4c0: draw_car);
   the texture frame (0x173a9): paint job | 0x4000 not braking | 0x2000 at 4000 units/s or under,
   of which the phase is a quarter (record +0x20 >> 2) */
void car_player_look(struct car_look *lk)
{
    *lk = (struct car_look){ pc.x, pc.y, pc.z + crash.height, pc.yaw + pc.drift, -pc.pitch + crash.pitch,
                             -pc.roll + crash.roll, body.pitch * 2.0f, body.roll * 4.0f, spin, steer,
                             ((uint32_t)pc.spec.livery | (pc.brake == 0.0f ? 0x4000u : 0) |
                              (pc.speed <= 4000.0f ? 0x2000u : 0)) >> 2 };
}

int car_object(const struct track_cam *cam, struct track_object *obj)
{
    (void)cam;
    if(views[view].incar && !trackside) return 0;
    *obj = (struct track_object){ pc.x, pc.y, pc.z + crash.height, 1200.0f, 0, 0, car_make, 0, 0, &refl, refl_phase };
    return 1;
}

void car_reflection(const float acc[3], uint32_t phase)
{
    refl = (struct track_refl){ { (int32_t)pc.x, (int32_t)pc.y, (int32_t)(pc.z + crash.height) },
                                { acc[0], acc[1], acc[2] } };
    refl_phase = phase;
}

/* sine and cosine; a tilt (under 0.5 rad, nearly always) by their series, to float's precision,
   without the library's calls */
static void sin_cos(float a, float *s, float *c)
{
    if(fabsf(a) >= 0.5f) {
        *s = sinf(a);
        *c = cosf(a);
        return;
    }
    float a2 = a * a;
    *s = a * (1.0f - a2 * (1.0f / 6.0f - a2 * (1.0f / 120.0f - a2 * (1.0f / 5040.0f))));
    *c = 1.0f - a2 * (0.5f - a2 * (1.0f / 24.0f - a2 * (1.0f / 720.0f - a2 * (1.0f / 40320.0f))));
}

void car_flat_block(const uint8_t *file, float x, float y, float z, float yaw, float pitch, float roll,
                    struct track_block *out)
{
    const struct cblock q = { 0 };
    struct pose ps = { { 1.0f, 0.0f, 1.0f, 0.0f }, { 0 }, { 1.0f, 0.0f, 1.0f, 0.0f }, 0, x, y, z, 0.0f };
    sin_cos(pitch, &ps.car[1], &ps.car[0]);
    sin_cos(roll, &ps.car[3], &ps.car[2]);
    ps.car[4] = cosf(yaw);
    ps.car[5] = sinf(yaw);
    place_block(&q, &ps, out);
    out->file = file;
    out->blk = file + *(const int32_t *)(file + 0x18);
    out->phase = 0;
}

int car_mesh_blocks(const uint8_t *file, float x, float y, float z, float yaw, float pitch, float roll,
                    struct track_block *out, int max)
{
    struct pose ps = { { 1.0f, 0.0f, 1.0f, 0.0f }, { 0 }, { 1.0f, 0.0f, 1.0f, 0.0f }, 0, x, y, z, 0.0f };
    sin_cos(pitch, &ps.car[1], &ps.car[0]);
    sin_cos(roll, &ps.car[3], &ps.car[2]);
    ps.car[4] = cosf(yaw);
    ps.car[5] = sinf(yaw);
    int nb = *(const int32_t *)(file + 0x14), n = 0;
    for(int b = 0; b < nb && n < max; b++, n++) {
        struct cblock q = { 0 };
        q.blk = file + *(const int32_t *)(file + 0x18 + 4 * b);
        int32_t so = *(const int32_t *)(q.blk + 0x0C);         /* its place in the mesh, as cmodel() */
        for(int j = 0; j < 3; j++) q.t[j] = so > 0 ? (float)*(const int32_t *)(file + so + 0x10 + 4 * j) : 0.0f;
        place_block(&q, &ps, &out[n]);
        out[n].file = file;
        out[n].blk = q.blk;
        out[n].phase = 0;
    }
    return n;
}

static struct { float x, y, h; int on; } shadow;

/* the player's shadow block (0x15c0c), made when it is drawn (the CPU's time before the first
   piece is the frame's; once the pieces go, the geometry unit's work is): heading the car
   object's, the tilt kept in a crash */
static int shadow_make(int id, const struct track_cam *cam, struct track_object *obj)
{
    static struct track_block shadow_block;
    (void)id;
    (void)cam;
    car_flat_block(efx_shadow(), obj->x, obj->y, obj->h, pc.yaw + pc.drift, shadow_pitch, shadow_roll, &shadow_block);
    obj->n = 1;
    obj->b = &shadow_block;
    return 1;
}

/* The player's shadow and puffs, the puffs' blocks made at once, all in a row (the code stays
   in the cache). A puff faces the screen (flag 0x8000 of its object, 0x118a4: the engine puts
   the camera's angles in for its own, 0x8abc7 -> 0x4bca0): its block's axes are the camera's
   right, up and forward; the texture's frame 6 x (phase >> 2) >> 14 (0xcd906). */
int car_effects(const struct track_cam *cam, struct track_object *objs, int max)
{
    static struct track_block puff_block[PUFFS];
    uint32_t t0 = rdcycle();
    int n = 0;
    const uint8_t *sh = efx_shadow(), *sm = efx_smoke();
    shadow.on = 0;
    if(sh && (!views[view].incar || trackside) && n < max) {
        /* 0x18cbc: the car's whole-number x and z moved 20 units (ds:0x854) away from the
           camera along the ground; 0x16043: 2 under the ground below the wheels, without the
           grass's jolts (+0x4ec, 0x1675f: their sum / 4) */
        int32_t cx = (int32_t)pc.x, cy = (int32_t)pc.y, ex = (int32_t)cam->x - cx, ey = (int32_t)cam->y - cy;
        float d = sqrtf((float)(ex * ex + ey * ey));
        int32_t ground = ((int32_t)ct.h[0] + (int32_t)ct.h[1] + (int32_t)ct.h[2] + (int32_t)ct.h[3]) / 4;
        if(d > 0.0f) {
            shadow.x = (float)(int32_t)((float)cx - (float)ex / d * 20.0f);
            shadow.y = (float)(int32_t)((float)cy - (float)ey / d * 20.0f);
            shadow.h = (float)(ground - 2);
            shadow.on = 1;
            objs[n++] = (struct track_object){ shadow.x, shadow.y, shadow.h, 900.0f, 0, 0, shadow_make, 0,
                                               track_class_key(sh), 0, 0 };
        }
    }
    float f[3], r[3], u[3];
    int axes = 0;
    for(int k = 0; k < PUFFS && sm && n < max; k++) {
        if(!puff[k].on) continue;
        if(!axes) {
            track_cam_axes(cam, f, r, u);
            axes = 1;
        }
        struct track_block *t = &puff_block[k];
        for(int j = 0; j < 3; j++) {
            t->m[3 * j] = r[j];
            t->m[3 * j + 1] = u[j];
            t->m[3 * j + 2] = f[j];
        }
        t->o[0] = puff[k].x;
        t->o[1] = puff[k].y;
        t->o[2] = puff[k].h;
        t->file = sm;
        t->blk = sm + *(const int32_t *)(sm + 0x18);
        t->phase = (uint32_t)puff[k].phase >> 2;
        objs[n++] = (struct track_object){ puff[k].x, puff[k].y, puff[k].h, 420.0f, 1, t, 0, k,
                                           track_class_key(sm), 0, 0 };
    }
    efx_cyc_objects += rdcycle() - t0;
    return n;
}

int car_puffs(void)
{
    int n = 0;
    for(int k = 0; k < PUFFS; k++) n += puff[k].on;
    return n;
}

int car_shadow_at(float *x, float *y, float *h)
{
    *x = shadow.x;
    *y = shadow.y;
    *h = shadow.h;
    return shadow.on;
}

int car_view_lines(void)
{
    return views[trackside ? TRACKSIDE_VIEW : view].lines;
}

int car_overlay(void)
{
    return views[trackside ? TRACKSIDE_VIEW : view].overlay;
}

int car_cockpit(void)
{
    return car_overlay() == 1;
}

void car_mode_init(const uint8_t *dat, uint32_t dat_size, int n, int difficulty)
{
    static uint8_t dtr[0x800];
    char name[16] = "MAPnA.DTR";                        /* the difficulty's letter (0x13251) */
    struct dat_entry e;
    uint8_t td[16];
    name[3] = (char)('0' + n);
    name[4] = (char)('A' + difficulty);
    carnage = 0;
    uint32_t len = dat && dat_find(dat, dat_size, name, &e) && e.size <= sizeof dtr ?
                   dat_extract(dat, dat_size, &e, dtr, sizeof dtr) : 0;
    if(len && tb_track_data(dtr, len, td, sizeof td) == sizeof td)
        carnage = (int)(td[12] | (uint32_t)td[13] << 8 | (uint32_t)td[14] << 16 | (uint32_t)td[15] << 24);
    static int32_t mrk[1 + 8 * GATES_MAX];              /* the gates: count, records of 8 words */
    char gates_name[16] = "MAPnSLA.MRK";
    gates_name[3] = name[3];
    len = dat && dat_find(dat, dat_size, gates_name, &e) && e.size <= sizeof mrk ?
          dat_extract(dat, dat_size, &e, (uint8_t *)mrk, sizeof mrk) : 0;
    ngates = 0;
    for(int32_t k = 0; len >= 4 && k < mrk[0] && k < GATES_MAX && 4 + 32 * (uint32_t)(k + 1) <= len; k++) {
        const int32_t *g = mrk + 1 + 8 * k;
        gate[k].r = g[0];
        gate[k].seconds = g[4];
        gate[k].x = g[5];
        gate[k].y = g[7];
        ngates = k + 1;
    }
}

int car_gates(int *total)
{
    if(total) *total = ngates;
    return gates_done;
}

float car_lap_record(void)
{
    return record;
}

void car_trackside_init(const uint8_t *dat, uint32_t dat_size, int n)
{
    static int32_t spl[2 + 14 * RPL_MAX];       /* count, start, nodes of 14 words */
    char name[16] = "MAPnRPL.SPL";
    struct dat_entry e;
    name[3] = (char)('0' + n);
    rpl_n = 0;
    uint32_t len = dat && dat_find(dat, dat_size, name, &e) && e.size <= sizeof spl ?
                   dat_extract(dat, dat_size, &e, (uint8_t *)spl, sizeof spl) : 0;
    int32_t count = len >= 8 ? spl[0] : 0;
    for(int32_t k = 0; k < count && k < RPL_MAX && 8 + 56 * (uint32_t)(k + 1) <= len; k++) {
        const int32_t *nd = spl + 2 + 14 * k;  /* ?, x, y, height, ... */
        rpl[k][0] = nd[1];
        rpl[k][1] = nd[2];
        rpl[k][2] = nd[3];
        rpl_n = k + 1;
    }
}

/* The trackside camera (0x1a620): the node nearest the car on the ground, the car's place in
   whole units (+0x4b0, +0x4b8: where the game draws it; ours is the float place cut, not
   `whole`, which loses up to a unit a step and is thousands off after a race) against the
   node's, the distance cut (a node at the car's very place is passed over, the first of
   equals kept); from there straight at the car,
   heading the way (forward = sin yaw, -cos yaw), pitch 2 pi - acos(flat / distance): down by
   the angle the height between them gives, whichever is the higher; no lean. More than 1500
   units from the car it moves on along its heading and pitch until it is 1500 away, each
   coordinate cut to a whole unit. */
static void trackside_camera(struct track_cam *cam)
{
    int32_t cx = (int32_t)pc.x, cy = (int32_t)pc.y, ch = (int32_t)pc.z, near = 0x7FFFFFFF;
    int at = 0;
    for(int k = 0; k < rpl_n; k++) {
        float dx = (float)(rpl[k][0] - cx), dy = (float)(rpl[k][1] - cy), d = sqrtf(dx * dx + dy * dy);
        if(d != 0.0f && (float)near > d) {
            near = (int32_t)d;
            at = k;
        }
    }
    float dx = (float)(rpl[at][0] - cx), dy = (float)(rpl[at][1] - cy), dh = (float)(rpl[at][2] - ch);
    float flat = sqrtf(dx * dx + dy * dy), dist = sqrtf(dx * dx + dy * dy + dh * dh);
    cam->x = (float)rpl[at][0];
    cam->y = (float)rpl[at][1];
    cam->h = (float)rpl[at][2];
    cam->yaw = atan2f(-dx, dy);
    cam->pitch = -atan2f(fabsf(dh), flat);
    cam->roll = 0.0f;
    int32_t move = (int32_t)(dist - 1500.0f);
    if(move > 0 && dist > 0.0f) {
        cam->x = (float)(int32_t)(cam->x - dx / dist * (float)move);
        cam->y = (float)(int32_t)(cam->y - dy / dist * (float)move);
        cam->h = (float)(int32_t)(cam->h - fabsf(dh) / dist * (float)move);
    }
}

void car_view(int v)
{
    if(crash.on) return;                                /* the crash view until it is over */
    view = (v % CAR_VIEWS + CAR_VIEWS) % CAR_VIEWS;
    chase.init = 0;
    chase_step();
}

void car_trackside(void)
{
    trackside = rpl_n > 0;
}

/* view_pose */
void car_camera(struct track_cam *cam)
{
    const struct view_def *v = &views[view];
    if(trackside) {
        trackside_camera(cam);
        return;
    }
    if(v->incar) {
        /* 0x1a2ac: with the suspension, pitch + the body's +0x4e0, roll + 3 x its +0x4e8 (the
           camera's roll is the other way round from the game's), height + its drop */
        cam->x = pc.x;
        cam->y = pc.y;
        cam->h = pc.z + CAMERA_HI + (float)body.drop;
        cam->yaw = pc.yaw + pc.drift;
        cam->pitch = pc.pitch + body.pitch;
        cam->roll = pc.roll - 3.0f * body.roll;
        return;
    }
    float cp = drive_cos(chase.pitch);
    cam->x = pc.x - cp * v->behind * drive_sin(chase.yaw);
    cam->y = pc.y + cp * v->behind * drive_cos(chase.yaw);
    cam->h = chase.height;
    cam->yaw = chase.yaw;
    cam->pitch = chase.pitch;
    cam->roll = chase.roll;
}

const struct car_state *car_state(void)
{
    return &pc;
}

uint32_t car_wall_hits(void)
{
    return wall_hits;
}

uint32_t car_crashes(void)
{
    return crashes;
}

int car_marker(void)
{
    return mark_at;
}
