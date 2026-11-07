/*
 * The player's car on the track (car.h). The logic is after OpenScreamer's port/src/main.c
 * (GPL-3.0), checked against STARTL.EXE where a comment says so. The shadow, the smoke, the
 * trackside camera and the suspension (the body's lean) come from STARTL.EXE itself.
 * The camera does not fly across between views.
 *
 * In file order: the contact with the ground (contact) and with the walls (walls_step), the
 * smoke and the crash, the markers and the race clocks (markers_step), the chase camera,
 * car_init, car_step (one step with everything around the physics), the car's model and wheels
 * (cmodel_build, draw_model), the shadow and the puffs (car_effects), the numbers of the game
 * modes (car_mode_init) and the cameras (trackside_camera, car_camera).
 *
 * Addresses in the form (original: STARTL.EXE 0x...) are places in the original's executable;
 * "ds:0x..." is an offset in its data segment. "+0x..." is a field of the original's player
 * record (or of the wheel record, where it says so).
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

/* The bumper cam's height over the car: camera_hi in TRACK_DATA of MAPnA.DTR. */
#define CAMERA_HI   ((float)track_camera_hi())

/* The game's views (OpenScreamer's g_views). They are the rows of the game's view table
   (original: STARTL.EXE 0x1da0), which has the key, the camera routine, the lines of the 3D view
   and the overlay of each view. The fields:
   - behind, above, down: how far behind the car the camera is, how high, and how much (radians)
     it looks down.
   - incar: 1 if the camera is in the car (camera routine 0).
   - lines: the lines of the 3D view, from the top.
   - overlay (hud.h): 1 the dashboard over the rest, 2 the instruments of the outside views,
     0 neither. */
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

/* The trackside cameras' places (row 3 of the view table): the nodes of the first spline of
   MAPnRPL.SPL, each as x, y and height in whole units. The loader keeps the spline at 0xf90c
   (original: STARTL.EXE 0x14fb4) and the camera routine reads it (original: STARTL.EXE 0x1a620).
   `trackside` says whether the camera is on. It is from the race's end, when the game asks for
   row 3 (original: STARTL.EXE 0x20eb8, 0x20f88). */
#define RPL_MAX     64
static int32_t rpl[RPL_MAX][3];
static int rpl_n;
static int trackside;
static int replaying;                   /* the race runs again from its keys (original: 0x118e68) */

static struct car_state pc;             /* the player's car */
static struct track_model model;        /* the player's car model: wheelbase, axles, track */
static int view;
static int mark_at;                     /* the marker last reached */
static uint32_t wall_hits, crashes;
static struct car_race race;
int car_laps = 3;                       /* the option's default (original: STARTL.EXE 0x1d2ac + 1) */
int car_mode = CAR_NORMAL;
static int carnage;                     /* TRACK_DATA's carnage time (original: STARTL.EXE 0xcf4c) */
static int props_paid;                  /* the props down that the last 1 s was paid for (+0x7bc) */
/* Slalom's gates, from MAPnSLA.MRK. The file has records of 32 bytes: the radius at +0, the
   gate's seconds at +0x10, x at +0x14 and y at +0x1c. gate_done says for each gate whether it
   has been driven through (+0x694), gates_done how many have (+0x7c0). */
#define GATES_MAX   64
static struct { int32_t r, seconds, x, y; } gate[GATES_MAX];
static uint8_t gate_done[GATES_MAX];
static int ngates, gates_done;
/* The markers passed since the last lap (marker 0, the line, is not counted) and how many. */
static uint8_t mark_seen[64];
static int mark_count;
static float needle, needle_speed;      /* the rev counter's and the speed needle, eased */
static float progress;                  /* how far round the race (car_marker_progress) */
static float record;                    /* the lap to beat (original: STARTL.EXE 0x1d268) */
float car_lap_to_beat = 180.0f;         /* the game's default record */
/* The road's heading at the road point nearest to the car. It is set on each step: player
   +0x13c, from the first lines of 0x16c90, which run every frame (original: STARTL.EXE 0x16c90,
   called from 0x15ae4). */
static float road;
static struct track_refl refl;          /* the car object's reflection state (record +0x24 ..) */
static uint32_t refl_phase;             /* its reflection picture (player +0x7b0, record +0x3c) */

/* The player's sounds (sound.h): the engine from the race's set-up (original: STARTL.EXE
   0x128dc), the skid, the wall. snd_over is set once the race is over and they are stopped. */
static int snd_engine = -1, snd_skid = -1, snd_wall = -1, snd_over;

/* The state of a crash: the car is thrown up, tumbles and lands four times (original: STARTL.EXE
   0x16c90 decides on it, 0x18518 runs it). `view` is the view to return to. */
static struct { int on, bounces, view; float height, vy, pitch, roll, pitch_rate, roll_rate; } crash;

/* The body's corners, for the walls (player_car, original: STARTL.EXE 0x136c8). Each is the
   wheel's place moved 250 units ahead of the front wheels or 150 behind the rear ones;
   sideways it is at the wheels. [k][0] is to the right and [k][1] forward. The order of k is
   front-left, rear-left, front-right, rear-right. */
static float corner[4][2];
/* Which side of each wall the track is on: +1 on the side of the wall's normal, else -1. */
static float wall_in[MAX_WALLS];

/* A car model's blocks (mesh.c), made the first time a car needs the model. Each block has:
   - blk: the block in the mesh.
   - t: its place in the model.
   - mid: its middle, by which the drawing order goes.
   - back: how many units further off it counts for that order.
   - wheel, spins, steers: whether it is a wheel, a hub that turns, and a front wheel that
     steers. */
#define MAX_BLOCKS  CAR_BLOCKS
#define MAX_MODELS  8
struct cblock { const uint8_t *blk; float t[3], mid[3]; int back, wheel, spins, steers; };
static struct cmodel {
    const uint8_t *mesh;
    struct cblock cb[MAX_BLOCKS];
    int ncb, ready;
} cmodels[MAX_MODELS];
static int pmodel;                      /* the player's car model (AUTOk) */
static float spin, steer;               /* the wheels' turn and the front ones' angle (wheels_step) */

/* The ground under the four wheels, in the order front-left, rear-left, front-right,
   rear-right. h[] is for the wheels at the car's heading plus the body's slip. hc[] is for the
   heading alone, which the chase camera follows; cam_pitch and cam_roll are the nose-up and
   the lean of that set. `ready` is set once they have been filled. */
static struct { float h[4], hc[4], cam_pitch, cam_roll; int ready; } ct;
/* The chase camera: its heading, lean, pitch and height, eased towards the car's. */
static struct { float yaw, roll, pitch, height; int init; } chase;

/* The shadow's tilt. It is the car object's tilt, but it is kept while the car crashes
   (state 1, original: STARTL.EXE 0x1604c). */
static float shadow_pitch, shadow_roll;

/* The body on its suspension (original: STARTL.EXE 0x168a4). susp[n] is the swing of wheel n,
   in the order front-left, rear-left, front-right, rear-right (wheel record +0x194 + 0x54 n):
   its clock (+0x30), sideways swing (+0x38), lengthways swing (+0x40) and deflection (+0x34).
   `body` is what the four swings make of the four body blocks: pitch (+0x4e0), roll (+0x4e8)
   and drop (+0x4c0, the average of the deflections). Pitch and roll are worked out like the
   ground's (+0x4c8 and +0x4d0) from the wheel heights, with the deflections taken as heights.
   susp_clock is the time since the last frame of the suspension and susp_speed the speed
   then (+0x588). */
static struct swing { float phase, lat, lng; int defl; } susp[4];
static struct { float pitch, roll; int drop; } body;
static float susp_clock, susp_speed;

/* The tyre smoke. The routine at 0x19818 starts puffs and 0x19970 moves them
   (original: STARTL.EXE). There are 8 slots (+0x308, 0x28 bytes each). Only the player has
   them: 0x142e8 gives the opponents none. A puff has an offset from the car in the model's
   frame (lx, lz), its place now (x, y, h) and its phase, which runs through 2/3 s. */
#define PUFFS       8
static struct puff { int on; int32_t lx, lz, phase; float x, y, h; } puff[PUFFS];
static float puff_clock;                /* +0x304 */

/* The player's whole-number place as the game keeps it (car +0x4b0 and +0x4b8; the smoke reads
   them as ds:0xfdc8 and ds:0xfdd0). The game adds each move to it and cuts the sum towards 0
   (original: STARTL.EXE 0x4d7b4, frndint with the rounding set to chop, then fistp; for x at
   0x171ae, 0x17229 and 0x1732d, for z at 0x171f8 and 0x1725a). So every step loses its
   fraction. A move a hair under 0 takes a whole unit: this happens off the grid with the
   heading slightly past pi. Our car moves in floats (drive.c); this place follows those moves.
   fx and fy are the float place that it took last. */
static struct { int32_t x, y; float fx, fy; } whole;

/* Stands the car on the drivable surface (OpenScreamer's player_contact;
   original: STARTL.EXE 0x11a30, 0x16698). One call changes pc.z, pc.pitch, pc.roll, pc.rough,
   pc.grass[] and the ground set ct.
   - Each wheel is put at its place in the model (x times |cos roll|, z times |cos pitch|),
     turned by the heading plus the body's slip. It takes the height of the drivable surface
     there. A wheel over nothing keeps its last height.
   - A wheel on grass is jolted by +-((x + y) & 15).
   - The car's height is the plain average of the four wheels. Its pitch comes from the
     right-hand wheels and its roll from the front ones.
   The model's +x is the car's left, and its nose points towards -z. */
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
            pc.grass[n] = grass;                /* the wheel's own flag; kept while over nothing */
            if(grass && !(n & 1)) pc.rough = 1;
        }
        jolted[n] = ct.h[n];
        if(pc.grass[n]) {
            int spot = (int)wx + (int)wy;
            jolted[n] += (float)((spot & 2) ? -(spot & 0xf) : (spot & 0xf));
        }
    }
    /* The same wheels turned by the heading alone. While the body does not slip these are the
       same places, so the heights found above are reused. */
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

/* The road points sorted by x, for nearest_dir(). dirs_prepare() makes the table at each race's
   start. A point has its place as floats (the places are whole numbers, so this is exact) and
   its index in the file. nsdirs is -1 while there is no table. */
#define MAX_DIRS    1024
static struct sdir { float x, y; int i; } sdirs[MAX_DIRS];
static int nsdirs = -1;

/* Sorts the track's road points by x into sdirs (insertion sort). If there are more than
   MAX_DIRS, there is no table and nearest_dir() searches them all. */
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

/* Returns the road point nearest to (x, y); of equally near points the first in the file.
   It goes out from x in both directions through the points sorted by x. A direction ends
   where the distance in x alone (with a margin over the rounding) is past the nearest distance
   found. The distances are worked out as a search through all points would work them out, so
   the result is the same point. */
static const struct track_dir *nearest_dir(float x, float y)
{
    int n;
    const struct track_dir *d = track_dirs(&n), *best = 0;
    float bd = 1e30f;
    if(nsdirs < 0 || nsdirs != n) {                     /* not in the table: all of them */
        for(int i = 0; i < n; i++) {
            float e = ((float)d[i].x - x) * ((float)d[i].x - x)
                    + ((float)d[i].y - y) * ((float)d[i].y - y);
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

/* Returns the index of the wall whose middle is nearest to (x, y), or -1 if there is none. */
static int nearest_wall(float x, float y)
{
    int n, best = -1;
    const struct track_wall *w = track_walls(&n);
    float bd = 1e30f;
    for(int i = 0; i < n && i < MAX_WALLS; i++) {
        float e = ((float)w[i].x - x) * ((float)w[i].x - x)
                + ((float)w[i].y - y) * ((float)w[i].y - y);
        if(e < bd) { bd = e; best = i; }
    }
    return best;
}

/* Works out wall_in[] (walls_prepare): which side of each wall is the track on? It is the side
   that has track 200 units from the wall's middle. If that does not decide it, it is the side
   that the nearest road point is on. The files have walls authored both ways round. Also
   prepares the road-point table. */
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
        float side = d ? ((float)d->x - (float)w[i].x) * nx + ((float)d->y - (float)w[i].y) * ny
                       : -1.0f;
        wall_in[i] = side > 0.0f ? 1.0f : -1.0f;
    }
}

/* Keeps the car inside the walls (OpenScreamer's player_walls;
   original: STARTL.EXE 0x11e74, 0x11f98, 0x16b88, 0x16c90). One call changes pc.x and pc.y, and
   with a hit also the speed, the heading and the shove, the counters, the sounds and the view.
   1. It takes the heading of the road point nearest to the car as the road's heading.
   2. It tests the body's four corners against the drivable surface. If all are on it, the
      call ends.
   3. Each corner that is off the surface is put back along the normal of the nearest wall, to
      30 units inside the wall's line. The game puts it 30 units on the other side of the line;
      this is OpenScreamer's one departure from the game. On the inside of a bend the straight
      wall chords stand off the surface, and the game's way threw the car out through the wall.
   4. The car moves by the largest push in x and in y.
   5. The wall response (drive_hit_wall) scrubs speed, swings the nose and shoves. A hard hit
      is a crash. */
static void walls_step(void)
{
    int nw;
    const struct track_wall *w = track_walls(&nw);
    float fx = drive_sin(pc.yaw), fy = -drive_cos(pc.yaw), rx = -drive_cos(pc.yaw), ry = -drive_sin(pc.yaw);
    float cr = fabsf(drive_cos(pc.roll)), cp = fabsf(drive_cos(pc.pitch));
    int out[4], any = 0;
    float px[4], py[4], wall_angle = 0.0f;
    const struct track_dir *d = nearest_dir(pc.x, pc.y);   /* original: 0x16cbb; before any wall */
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
        float depth = across * wall_in[i];              /* > 0 means inside the line */
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
    /* A hit is a crash when speed x (angle off the road's heading / 90 degrees) is more than
       0.3 maxspd (original: STARTL.EXE 0x16c90). It must also be out in the open: the last
       marker's kind (+0x7b0) is 0, so not in a tunnel. The game checks that; OpenScreamer does
       not, and this code does. The wall response comes in either case. A crash only keeps the
       wall's sound from playing. */
    float off = d ? fabsf(pc.yaw - road) : 0.0f;          /* original: 0x16d17; against +0x13c */
    while(off > PI) off -= PI;
    if(off > PI / 2.0f) off = PI - off;
    int nm;
    const struct track_marker *mk = track_markers(&nm);
    int open = nm == 0 || mk[mark_at].kind == 0;
    int crashing = pc.speed * (off / (PI / 2.0f)) > pc.k_wall && open;
    drive_hit_wall(&pc, mx, my, (float)(out[2] - out[0]), wall_angle);
    /* The wall's sound plays unless it still sounds (original: STARTL.EXE 0x16f0b). */
    if(!crashing) snd_wall = snd_play_once(SND_WALL, snd_wall);
    if(crashing) {
        snd_play(SND_DENT);                             /* original: STARTL.EXE 0x18540 */
        speech_say(SAY_CRASH, 3, 0.5f);
        /* The start of the crash (state 2 of original: STARTL.EXE 0x18518): the speed falls to
           0.6 of its value, the heading becomes the road's, and the car is thrown up at
           3000 units/s, turning at 12 rad/s in pitch and in roll. The view changes to the
           crash view. */
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

/* Adds the move d to the whole number w and cuts the sum towards 0. */
static int32_t whole_add(int32_t w, float d)
{
    return w + (int32_t)((float)w + d >= 0.0f ? floorf(d) : ceilf(d));
}

/* Called after a step's move: adds the move to the whole-number place, cutting each sum
   towards 0. */
static void whole_step(void)
{
    whole.x = whole_add(whole.x, pc.x - whole.fx);
    whole.y = whole_add(whole.y, pc.y - whole.fy);
    whole.fx = pc.x;
    whole.fy = pc.y;
}

/* Returns the game's tick counter T, worked out from the CPU's cycle counter. It counts 100
   ticks a second, whatever the frame rate. In the original, T is at obj1:0x7fd and the
   callback of its 100 Hz timer counts it up (original: STARTL.EXE 0x1020d). The fixups at
   0x198b8 and 0x10215 both name object 1, not the data object. OpenScreamer read the data
   object's 0x7fd, which is the middle of a constant, as a fixed 51. */
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

/* Starts puffs of tyre smoke (original: STARTL.EXE 0x19818). It is called on every step that
   smokes.
   1. The clock (+0x304) gains the step's time while it is under 0.05 s (ds:0x8cb). Then the
      call ends.
   2. Otherwise the clock starts again, and each rear wheel (records 1 and 3: rear left and
      rear right) takes the first free slot.
   3. The puff's place is the wheel's place on the model, pushed sideways by (x & 7) * (T & 63)
      and back by (z & 7) * (T & 63). x and z are the player's whole-number place as the game
      keeps it (ds:0xfdc8 and ds:0xfdd0: +0x4b0 and +0x4b8 of the car record 0x10f918). This
      is `whole`, not the float place cut: off the grid the float place sits a hair under 2600,
      so x & 7 = 7 and every push is the largest.
   4. The push of the right-hand puff is turned round when T has bit 1 set
      (original: STARTL.EXE 0x198bf: T and the wheel's index 2).
   The pushes go out to the left, and either way on the right, up to 441 units. At 50 steps a
   second this gives a pair of puffs every 4 steps. */
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

/* Moves the puffs (original: STARTL.EXE 0x19970). It runs on every step after the skid.
   - Each puff is put where the car is now. Its offset is turned by heading + slip
     (original: STARTL.EXE 0x2216c) with no height, then about the world's x axis by the car's
     pitch (original: STARTL.EXE 0x221dc). The sums are in whole units from the car's
     whole-number place, so the puff goes with the car.
   - Its phase gains 1.5 x 65535 a second (ds:0x8d3, 0x118e8: 1966 a step). The puff ends at
     phase 0x10000 (original: STARTL.EXE 0x11930), after 34 steps. */
static void smoke_step(void)
{
    int any = 0;
    for(int k = 0; k < PUFFS; k++) any |= puff[k].on;
    if(!any) return;
    float a = pc.yaw + pc.drift, ca = drive_cos(a), sa = drive_sin(a), cp = drive_cos(pc.pitch),
          sp = drive_sin(pc.pitch);
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

/* One step of a crash, if there is one. Gravity is 6000 units/s^2, and the turning dies away
   (damp[97]). Each landing throws the car up again at 0.4 times the fall speed, level and with
   a new small turn, and makes smoke (original: STARTL.EXE 0x18727). The fourth landing ends
   the crash and brings back the view from before. */
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

/* The markers and the race clocks (OpenScreamer's markers_step;
   original: STARTL.EXE 0x1820c, 0x19490). One call does these steps:
   1. It runs the clocks: the lap and total time once the line was crossed, and the countdown.
   2. It gives the special modes their extra time: a second for each new prop down in cone
      carnage, the gate's seconds in slalom.
   3. It looks at the marker last reached and its two neighbours. If the car is inside one's
      circle, that marker becomes the last reached, and its slip becomes the surface factor
      of the cornering force.
   4. The line (marker 0) starts the clocks. Once every other marker has been passed, it
      counts a lap.
   5. The line and each checkpoint (flag 2) give the bonus time.
   6. It works out how far round the race the car is (progress). */
static void markers_step(void)
{
    if(race.on && !race.finished) {
        if(race.clock_on) {
            race.lap += DRIVE_DT;
            race.total += DRIVE_DT;
        }
        race.timer -= DRIVE_DT;                         /* original: STARTL.EXE 0x18ad0 */
        race.time_out = race.timer < 0.0f;
        if(race.time_out) race.timer = 0.0f;
    }
    int n;
    const struct track_marker *mk = track_markers(&n);
    if(n == 0) return;
    /* The bonus time of the line and the checkpoints: the carnage time in cone carnage and
       slalom (original: STARTL.EXE 0x195fc), else TRACK_DATA's bonus. */
    float bonus = (float)(car_mode & (CAR_CONES | CAR_SLALOM) ? carnage : track_bonus_time());
    if(car_mode == CAR_CONES) {
        /* More props down than paid for: 1 s (original: STARTL.EXE 0x18b50). */
        int down = scenery_hits(0);
        if(down > props_paid) {
            race.timer += 1.0f;
            props_paid = down;
        }
    }
    if(car_mode == CAR_SLALOM) {
        /* The first gate that the car is in (original: STARTL.EXE 0x18b98). The car is in a
           gate if it is inside the gate's circle, which is tested after its bounding square. */
        int32_t x = (int32_t)pc.x, y = (int32_t)pc.y;
        for(int i = 0; i < ngates; i++) {
            int32_t dx = x - gate[i].x, dy = y - gate[i].y, r = gate[i].r;
            if(dx >= r || -dx >= r || dy >= r || -dy >= r
               || sqrtf((float)(dx * dx + dy * dy)) > (float)r) continue;
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
        } else if(i == 0 && mark_count >= n - 1) {        /* across the line after a full lap */
            int finish = race.laps + 1 >= race.total_laps && !race.finished;
            if(race.laps + 1 >= race.total_laps) {
                race.finished = 1;
                /* PERFECT! (flag 0x8000): in cone carnage every prop is down (original:
                   STARTL.EXE 0x23fbc); in slalom every gate was driven through and no prop was
                   touched (original: STARTL.EXE 0x23f68). */
                int all, down = scenery_hits(&all);
                if(car_mode == CAR_CONES) race.perfect = down == all;
                if(car_mode == CAR_SLALOM) race.perfect = gates_done == ngates && down == 0;
                /* Whether the finish counts: always, except in time attack, where the race must be
                   faster than the ghost's (original: STARTL.EXE 0x23ef0). */
                race.made_it = car_mode != CAR_ATTACK || ghost_finish(race.total);
            } else if(!race.finished) {
                race.timer = race.timer / 5.0f + bonus;
                speech_rearm(SAY_HURRY);
            }
            race.laps++;
            race.last = race.lap;
            /* A lap under the record is the new record and calls "new lap record"
               (original: STARTL.EXE 0x196ea). The lap call (original: STARTL.EXE 0x19668) uses
               recording 2 for the last lap, 1 for a lap that is the race's best so far, else 0. */
            int best = race.best == 0.0f || race.last < race.best;
            if(race.last < record) {
                record = race.last;
                speech_say(SAY_NEWLAP, 3, 0.2f);
            }
            if(!race.finished) speech_say(SAY_LAP, race.laps + 1 == race.total_laps ? 2 : best, 0.0f);
            if(best) race.best = race.last;
            /* At the finish the race's best lap goes to the record (original: STARTL.EXE 0x20eb8,
               the word at 0x1d268). */
            if(finish) record = race.best;
            for(int j = 0; j < n && j < 64; j++) mark_seen[j] = 0;
            mark_count = 0;
            race.lap = 0.0f;
        }
        if(i != 0 && i < 64 && !mark_seen[i]) {
            mark_seen[i] = 1;
            mark_count++;
            if((m->flags & 2) && race.clock_on && !race.finished) {
                /* The checkpoint call depends on the time left (original: STARTL.EXE 0x19490):
                   recording 2 under 3 s, recording 0 up to 15 s, recording 1 for more. */
                speech_say(SAY_CHECK, race.timer < 3.0f ? 2 : race.timer <= 15.0f ? 0 : 1, 0.0f);
                race.timer = race.timer / 5.0f + bonus;
                speech_rearm(SAY_HURRY);
            }
        }
    }
    progress = car_marker_progress(mark_at, race.laps, pc.x, pc.y);
}

/* How far a car has got: the last lines of original: STARTL.EXE 0x1820c for the player and of
   0x1db84 for an opponent. The result is laps x 100 + the marker last reached (at) + the
   car's distance from that marker over the distance from that marker to the next. Both
   distances are first multiplied by 0.05 and cut to a whole number. */
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

/* 1 if the heading is within 45 degrees of the road's heading reversed (car.h). */
int car_backwards(void)
{
    return fabsf(wrap_pi(pc.yaw - road - PI)) < PI / 4.0f;
}

/* The functions car_circles(), car_shift() and car_rammed() are the contact with the opponents
   (opp.c); car.h describes them. */
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

/* Moves the chase camera one step (chase_step, original: STARTL.EXE 0x1a53d). The camera eases
   to the car's heading (without its skid angle) and to the lean and nose-up of the
   heading-only wheel set (ct.hc). Its height eases to the car's + above - behind * sin(pitch).
   The first call after chase.init was cleared puts the camera there at once. */
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

/* Sets up the player's car and the race for a new start (car.h): the car's spec and model, the
   corners for the walls, the suspension, the crash, the sounds, the race clocks and the
   starting pose on the grid. `which` is the row of CARS.DTR; `replay` is 1 for a race run
   again from its keys. */
void car_init(int which, int replay)
{
    int n, nm;
    const struct car_spec *cars = track_cars(&n);
    const struct track_model *models = track_models(&nm);
    const struct car_spec *sp = &cars[(which % n + n) % n];
    int k = car_model_index(sp);
    model = models[k >= 0 && k < nm ? k : 0];
    drive_init(&pc, sp, model.wheelbase > 0.0f ? model.wheelbase : 935.0f);
    /* The body's corners for the walls (see corner). */
    float zf = model.axle_front + 250.0f, zr = model.axle_rear + 150.0f;
    corner[0][0] = -model.half_track; corner[0][1] =  zf;
    corner[1][0] = -model.half_track; corner[1][1] = -zr;
    corner[2][0] =  model.half_track; corner[2][1] =  zf;
    corner[3][0] =  model.half_track; corner[3][1] = -zr;
    walls_prepare();
    pmodel = k;
    /* The wheels, the suspension and the crash start from rest. */
    spin = steer = 0.0f;
    for(int j = 0; j < 4; j++) susp[j] = (struct swing){ 0 };
    body.pitch = body.roll = 0.0f;                      /* no lean at the start (original: 0x13f28) */
    body.drop = 0;
    susp_clock = susp_speed = 0.0f;
    wall_hits = crashes = 0;
    if(crash.on) view = crash.view;                     /* a race left in a crash: back to its view */
    crash.on = crash.bounces = 0;                       /* the crash view is gone, too */
    crash.height = crash.vy = crash.pitch = crash.roll = crash.pitch_rate = crash.roll_rate = 0.0f;
    replaying = replay;
    trackside = 0;
    if(replay) car_trackside();                         /* the replay asks for row 3 (original: 0x12afe) */
    /* The sounds. */
    snd_engine = snd_stop(snd_engine);
    snd_skid = snd_stop(snd_skid);
    snd_wall = -1;
    snd_over = 0;
    snd_engine = snd_play(SND_ENGINE);
    speech_reset();
    /* The lap to beat (original: STARTL.EXE 0x1ba3c): the ghost's in time attack, none in the
       other special modes, else car_lap_to_beat. A replay keeps the record as it is, because a
       lap that the race beat it with stays. */
    if(!replay)
        record = car_mode == CAR_ATTACK ? ghost_time() : car_mode & CAR_ALONE ? 0.0f : car_lap_to_beat;
    /* The race: no clocks run yet, 15 s on the countdown. */
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

    /* player_reset (OpenScreamer): the car stands on the grid, facing the way of the nearest
       road point (original: STARTL.EXE 0x13e07; this is the start pose of the track's image).
       Its height is the third path point's until the wheels find the ground. It does not skid
       until the first marker (surface 0). */
    float px, py, ph;
    track_start(&pc.x, &pc.y, &pc.yaw);
    if(car_mode & CAR_ALONE) {
        /* The place of the last opponent on the grid (original: STARTL.EXE 0x13ded). In time
           attack ghost_slot() is added to the opponent index, too. */
        int rows, ns = track_opp_count(&rows), start = 0, nn = 0;
        int last = (ns - 1 < rows ? ns - 1 : rows) - 1 + (car_mode == CAR_ATTACK ? ghost_slot() : 0);
        if(last > ns - 1) last = ns - 1;
        const struct track_spline_node *s = last >= 0 ? track_opp_spline(last, &start, &nn) : 0;
        const struct track_dir *d = s && start >= 0 && start < nn
                                    ? nearest_dir((float)s[start].x, (float)s[start].y) : 0;
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
    for(int i = 0; i < MAX_MODELS; i++)         /* each track's image has its own car models */
        cmodels[i].ready = 0;
    /* Stand the car on the ground, and put the chase camera and the shadow where they belong. */
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

/* Sets the car's reflection while the car drives, not in a crash (original: STARTL.EXE
   0x1813c). The kind of the marker last reached decides:
   - Reflection picture: kind 0 the sky (phase 0), kind 1 (a tunnel) phase 0x8000, kind 2 phase
     0xff00. The phase picks frame n x phase >> 16 of the window material's texture, which is
     the tunnel's ceiling.
   - How fast the reflections slide: 0.002 in the open, 0.03 under a roof. Until the first
     such marker the rate is the program's 0.008 (set in car_init).
   Other kinds leave both as they are. */
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

/* Sets the player's sounds on every step. The engine's note (original: STARTL.EXE 0x19408) has
   the pitch rpm x 14 + 32768; its sample is automoto, or autogall in a tunnel (the last
   marker's kind 1). The skid's sample plays while `skid` is set. That is the skid flag of
   0x17d04 (original: STARTL.EXE 0x180d0, the same flag as the smoke's), or the wheels
   spinning off the line in a gear (original: STARTL.EXE 0x1883d). Both sounds stop when the
   race is over. */
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

/* Makes the controls of a step from the keys, the stick and the triggers (car.h). Held keys
   count while they are held; L and R (the gear keys) count only on the step they went down. */
struct car_input car_keys(uint32_t keys, uint32_t keys_before, uint32_t joy, uint32_t trig)
{
    const uint32_t held = KEY_B | KEY_Y | KEY_LEFT | KEY_RIGHT, shift = KEY_L | KEY_R;
    struct car_input ci = { (uint16_t)((keys & held) | (keys & ~keys_before & shift)), 0x80, 0, 0 };
    if(IN_ANALOG(keys, joy)) {
        ci.stick = (uint8_t)joy;
        ci.brake = (uint8_t)trig;
        ci.gas = (uint8_t)(trig >> 8);
    }
    return ci;
}

/* A joystick axis as the game makes its control word (original: STARTL.EXE 0x24e1c). The value
   v out of `full` is scaled to +-0x3ff and held there. A word under 0x66 is 0 (dead zone;
   original: STARTL.EXE 0x24edc). */
static int axis_word(int v, int full)
{
    int w = v * 0x3ff / full;
    if(w > 0x3ff) w = 0x3ff;
    if(w < -0x3ff) w = -0x3ff;
    return w < 0x66 && w > -0x66 ? 0 : w;
}

int car_replay(void)
{
    return replaying;
}

/* The suspension (original: STARTL.EXE 0x168a4). The game runs it each frame after the drive
   (original: STARTL.EXE 0x15b9c). The sideways kick (divided by the frame's time twice) and
   the cut at 2 depend on the frame rate. So this runs on the game's frames of 1/30 s, as the
   opponents' lean does (opp.c): every 1/30 s of the 1/50 s steps. Its stiffnesses are
   pow(0.5 + i x 0.005, 50 / 31), as the game's table has them at 30 frames a second.
   Each frame:
   - The change of speed since the frame before (+0x50c - +0x588, original: STARTL.EXE 0x15bec)
     x 0.00175 kicks the front wheels one way and the rear ones three times as hard the other.
   - The heading's turn a second x speed^2 x 1.5e-9 kicks the left wheels against the right.
   - Both kicks are divided by the frame's time. A kick bigger than the swing restarts the
     swing. The lengthways kick is eased in (stiffness 0x3d).
   - A wheel swings as cos(4 pi t + pi) x (both kicks), at most 18 units.
   - The swings die away (stiffness 61 sideways, 71 lengthways), and a swing under 2 is gone. */
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
    /* The body's lean from the deflections: roll -atan((front-left - front-right) / track) and
       pitch atan((front-right - rear-right) / wheelbase). The game's track (+0x2e4) is
       -(x front-left - x front-right), and the left wheel is at +x, so the track is negative.
       So the outer side goes down in a bend and the nose goes down under braking. */
    body.drop = (susp[0].defl + susp[1].defl + susp[2].defl + susp[3].defl) / 4;
    body.roll = atanf((float)(susp[0].defl - susp[2].defl) / (2.0f * model.half_track));
    body.pitch = atanf((float)(susp[2].defl - susp[3].defl) / pc.wheelbase);
}

/* One step of 1/50 s (car.h). It does these things in order:
   1. It turns the controls into the physics input.
   2. It runs the start lights and the end of the race (time out, finish).
   3. It runs the crash and the physics (drive_step), then the suspension.
   4. It makes smoke, keeps the car in the walls, runs the markers and the reflections, stands
      the car on the ground and moves the chase camera and the smoke.
   5. It sets the sounds and the speech, and the camera that the sounds are heard from.
   6. It turns the wheels, eases the needles of the instruments and steps the time attack
      (ghost_step). */
void car_step(const struct car_input *ci)
{
    struct drive_input in;
    uint32_t keys = ci->keys;
    /* Gas and brake are on the buttons only. On the d-pad a thumb steering into a diagonal
       would brake or lift off by accident. The layout is that of the classic racing games: gas
       on the bottom button (B), the brake on the left one (Y). L and R shift on the step they
       went down (car_keys). */
    in.throttle = (keys & KEY_B) ? 1.0f : 0.0f;
    in.brake = (keys & KEY_Y) ? 1.0f : 0.0f;
    in.steer = ((keys & KEY_LEFT) ? 1.0f : 0.0f) - ((keys & KEY_RIGHT) ? 1.0f : 0.0f);
    in.shift = (keys & KEY_R) ? 1 : (keys & KEY_L) ? -1 : 0;
    /* A docked controller's triggers and left stick count when they are out of the dead zone.
       The game takes its joystick axes like this:
       - the pedal is |word| / 1023 (original: STARTL.EXE 0x1611c);
       - the brake is the same, taken as it is (original: STARTL.EXE 0x16174);
       - the wheel is -word / 1024 of the lock (original: STARTL.EXE 0x1633b); the stick's left
         is negative. */
    int gas = axis_word(ci->gas, 0xFF), brake = axis_word(ci->brake, 0xFF);
    int stick = axis_word(ci->stick - 0x80, 0x7F);
    in.brake_axis = brake != 0;
    in.steer_axis = stick != 0;
    if(gas) in.throttle = (float)gas / 1023.0f;
    if(brake) in.brake = (float)brake / 1023.0f;
    if(stick) in.steer = (float)-stick * 0.0009765625f;
    /* The start lights are green at 6 s. Until then the pedal only revs the engine. */
    race.clock += DRIVE_DT;
    race.on = race.clock >= 6.0f;
    pc.waiting = !race.on;
    if(race.time_out) {                                 /* out of time (original: 0x18a04) */
        in.throttle = 0.0f;
        if(pc.speed < 1.0f) race.game_over = 1;         /* and stopped: the race is over */
    }
    /* After the finish or the end of the time the car brakes by itself and the trackside
       camera takes over (it asks for row 3). */
    if(race.finished || race.game_over) { in.throttle = 0.0f; in.brake = 0.4f; in.brake_axis = 0; }
    if(race.finished || race.game_over) car_trackside();
    if(crash.on) { in.throttle = in.steer = 0.0f; in.steer_axis = 0; }  /* no drive in the air */
    int crashed = crash.on;
    crash_step();
    drive_step(&pc, &in);
    suspension_step();
    whole_step();
    /* The smoke of the skid routine (original: STARTL.EXE 0x17d04) comes from two conditions.
       The first is its branch for locked wheels, taken only out of a crash and while the
       engine pulls (flag 0x40 at 0x18e19, time over). The second is the body more than
       0.23 rad off its heading (ds:0x810, original: STARTL.EXE 0x180be). State 5, the wheels
       spinning off the line, smokes too, in a gear (original: STARTL.EXE 0x18879) and starts
       the skid's sample (original: STARTL.EXE 0x1883d). */
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
    /* The shadow's tilt (original: STARTL.EXE 0x1604c) is the car object's, but not while the
       car crashes. */
    if(!crashed || !crash.on) {
        shadow_pitch = -pc.pitch + crash.pitch;
        shadow_roll = -pc.roll + crash.roll;
    }
    efx_cyc_objects += rdcycle() - t0;
    sound_step(skid || wheelspin);
    speech_step();
    /* The sounds that have a place are heard from the camera (original: STARTL.EXE 0x1dffc,
       0x21b80). */
    struct track_cam ear;
    car_camera(&ear);
    snd_ear(&ear);

    /* wheels_step: the wheels turn with the speed (108.6 units of travel per radian). The front
       ones steer by the share of the lock; the sign follows OpenScreamer, because the model's
       +x is the car's left. */
    spin = fmodf(spin - pc.speed * DRIVE_DT / 108.6f, 6.2831853f);
    steer = -(pc.steer_max > 0.0f ? pc.steer / pc.steer_max : 0.0f) / 1.5f;

    /* dash_step: the race screen eases the needles each frame with stiffness 0x5a
       (original: STARTL.EXE 0x2367c).
       - The rev counter's needle goes to rpm / maxrpm x pi on the instrument of the outside
         views and x 3.613 on the dashboard (original: STARTL.EXE 0x23739), and no further.
       - The dashboard's speed needle, only while the dashboard is shown, goes to
         speed / maxspd x 4.084 (original: STARTL.EXE 0x236ec), with no upper limit. */
    int dash = views[view].overlay == 1;
    float full = dash ? 3.6128317f : PI, want = pc.rpm / pc.spec.maxrpm * full;
    if(want > full) want = full;
    needle = want + (needle - want) * drive_damp(0x5a);
    ghost_step();                           /* time attack (original: STARTL.EXE 0x231d0, 0x23018) */
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

/* Finds block b of the mesh: sets q->blk, and q->t to the block's place in the mesh (x, y and z of
   the record that the block's +0x0C points to; 0 for a block without one). */
static void cblock_find(struct cblock *q, const uint8_t *mesh, int b)
{
    q->blk = mesh + *(const int32_t *)(mesh + 0x18 + 4 * b);
    int32_t so = *(const int32_t *)(q->blk + 0x0C);
    for(int j = 0; j < 3; j++)
        q->t[j] = so > 0 ? (float)*(const int32_t *)(mesh + so + 0x10 + 4 * j) : 0.0f;
}

/* Makes the model (struct cmodel) of a car's mesh from its blocks, as car_draw() takes it. */
static void cmodel_build(struct cmodel *m, const uint8_t *mesh)
{
    /* For each block, the drawing order counts it further off by its name. A name of three
       letters ending in i (a tyre, the arches' insides) counts 4000 further off, one ending in
       x (a hub) 1000, unless the name starts with A. */
    m->mesh = mesh;
    m->ncb = 0;
    for(int b = 0; b < *(const int32_t *)(m->mesh + 0x14) && m->ncb < MAX_BLOCKS; b++) {
        struct cblock *q = &m->cb[m->ncb++];
        cblock_find(q, m->mesh, b);
        for(int j = 0; j < 3; j++) q->mid[j] = (float)*(const int16_t *)(q->blk + 4 + 2 * j) + q->t[j];
        const uint8_t *nmz = q->blk + 0x68;
        int named = nmz[0] != 'A' && nmz[0] != 'a' && nmz[3] == 0;
        q->back = named && nmz[2] == 'i' ? 4000 : named && nmz[2] == 'x' ? 1000 : 0;
        /* A wheel sits off-centre. Only its hub disc (flags 0x800) turns. The tyre block is the
           lower half of the tread and stays. The front wheels (local -z) steer. */
        q->wheel = q->t[0] != 0.0f;
        q->spins = q->wheel && (*(const int32_t *)(q->blk + 0x18) & 0x800);
        q->steers = q->wheel && q->t[2] < 0.0f;
    }
    m->ready = 1;
}

/* Returns the model of car k (AUTOk), made the first time it is asked for. A k out of range
   gives model 0. */
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

/* A car as it is drawn. The arrays hold cosine and sine pairs:
   - sc: the wheels' turn and the front ones' angle (cos, sin of each);
   - car: the object's pitch, roll and heading;
   - body: the body's own pitch and roll.
   `lean` says whether the body is leant or dropped. x, y, z is the car's place and drop how
   far the body is dropped. */
struct pose { float sc[4], car[6], body[4]; int lean; float x, y, z, drop; };

/* Puts a block's point p (as stored in the mesh) into the world as w (block_to_view, tilt,
   place). The steps, in order: a wheel is turned about its axle and steered; the point is
   moved to the block's place on the car; a body block is leant on its own; then come the
   object's pitch and roll, its heading, and its place. */
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
    if(ps->lean && !q->wheel) {     /* the body's lean: the suspension, or an opponent in a bend */
        float yb = y * ps->body[0] - z * ps->body[1], zb = z * ps->body[0] + y * ps->body[1];
        float xb = x * ps->body[2] - yb * ps->body[3];
        y = yb * ps->body[2] + x * ps->body[3] - ps->drop;
        x = xb;
        z = zb;
    }
    /* Tilt: the object's pitch and roll are the car's, negated (player_sync). */
    float y3 = y * car[0] + z * car[1], z3 = z * car[0] - y * car[1];
    float x3 = x * car[2] - y3 * car[3], y4 = y3 * car[2] + x * car[3];
    w[0] = ps->x + x3 * car[4] - z3 * car[5];
    w[1] = ps->y + x3 * car[5] + z3 * car[4];
    w[2] = ps->z + y4;
}

/* Gives the block's place in the world as t: its origin o and, in m, the images of its three
   axes (the columns of the block's matrix). */
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

/* Places the blocks of model m as lk says, with the body dropped by `drop`, and puts them into
   out in the game's order for the camera. Each block has its level of detail (track_lod_block:
   chosen by the distance of its middle from the camera, by the rule for moving things). The
   matrices stay those of level 0. A block with no level is left out. Returns how many blocks
   went into out. */
static int draw_model(const struct cmodel *m, const struct car_look *lk, float drop,
                      const struct track_cam *cam, struct track_block *out)
{
    static struct track_block tb[MAX_BLOCKS];
    struct pose ps = { { cosf(lk->spin), sinf(lk->spin), cosf(lk->steer), sinf(lk->steer) },
                       { cosf(lk->pitch), sinf(lk->pitch), cosf(lk->roll), sinf(lk->roll),
                         cosf(lk->yaw), sinf(lk->yaw) },
                       { cosf(lk->body_pitch), sinf(lk->body_pitch), cosf(lk->body_roll),
                         sinf(lk->body_roll) },
                       lk->body_pitch != 0.0f || lk->body_roll != 0.0f || drop != 0.0f,
                       lk->x, lk->y, lk->z, drop };
    int ord[MAX_BLOCKS], n = 0;
    float key[MAX_BLOCKS];
    for(int b = 0; b < m->ncb; b++) {
        const struct cblock *q = &m->cb[b];
        struct track_block *t = &tb[b];
        float mid[3], mw[3];
        place_block(q, &ps, t);
        t->file = m->mesh;
        t->phase = lk->phase;
        /* The drawing order (original: STARTL.EXE 0x8ac58, 0x8af24): the block with the furthest
           middle comes first, and the tyres and hubs count further off (back). On a tie the
           lower block comes first. */
        for(int j = 0; j < 3; j++) mid[j] = q->mid[j] - q->t[j];
        block_point(q, &ps, mid, mw);
        float dx = mw[0] - cam->x, dy = mw[1] - cam->y, dh = mw[2] - cam->h,
              dist = sqrtf(dx * dx + dy * dy + dh * dh);
        key[b] = -rintf(dist) - (float)q->back;
        t->blk = track_lod_block(m->mesh, b, dist, 1);
        if(!t->blk) continue;
        int i = n;
        while(i > 0 && key[ord[i - 1]] > key[b]) { ord[i] = ord[i - 1]; i--; }
        ord[i] = b;
        n++;
    }
    for(int b = 0; b < n; b++) out[b] = tb[ord[b]];
    return n;
}

/* Fills obj and out for car model k as lk says (draw_model), with the body dropped by
   `drop`. Returns 0 if the model has no blocks. */
static int draw_car(int k, const struct car_look *lk, float drop, const struct track_cam *cam,
                    struct track_block *out, struct track_object *obj)
{
    const struct cmodel *m = cmodel(k);
    if(!m->ncb) return 0;
    obj->n = draw_model(m, lk, drop, cam, out);
    obj->x = lk->x;
    obj->y = lk->y;
    obj->h = lk->z;
    obj->radius = 1200.0f;
    obj->b = out;
    return 1;
}

int car_draw(int k, const struct car_look *lk, const struct track_cam *cam, struct track_block *out,
             struct track_object *obj)
{
    return draw_car(k, lk, 0.0f, cam, out, obj);
}

int car_mesh_draw(const uint8_t *mesh, const struct car_look *lk, const struct track_cam *cam,
                  struct track_block *out)
{
    static struct cmodel m;
    cmodel_build(&m, mesh);
    return draw_model(&m, lk, 0.0f, cam, out);
}

/* Makes the player's car's blocks when the car comes up in the drawing (the callback of the
   car object). */
static int car_make(int id, const struct track_cam *cam, struct track_object *obj)
{
    static struct track_block blocks[MAX_BLOCKS];
    struct car_look lk;
    (void)id;
    car_player_look(&lk);
    return draw_car(pmodel, &lk, (float)body.drop, cam, blocks, obj);
}

/* Gives the look of the player's car (car.h).
   - The object's angles are the car's, negated (player_sync), with a crash's tumbling on top.
   - The four body blocks lean by pitch +0x4e0 x 2 and roll +0x4e8 x 4 (original: STARTL.EXE
     0x15c0c). The drop, the height less +0x4c0, is applied in draw_car().
   - The texture frame (original: STARTL.EXE 0x173a9) is the paint job | 0x4000 when not
     braking | 0x2000 at 4000 units/s or under. The phase is a quarter of it (record +0x20 >> 2). */
void car_player_look(struct car_look *lk)
{
    *lk = (struct car_look){ pc.x, pc.y, pc.z + crash.height, pc.yaw + pc.drift,
                             -pc.pitch + crash.pitch, -pc.roll + crash.roll,
                             body.pitch * 2.0f, body.roll * 4.0f, spin, steer,
                             ((uint32_t)pc.spec.livery | (pc.brake == 0.0f ? 0x4000u : 0) |
                              (pc.speed <= 4000.0f ? 0x2000u : 0)) >> 2 };
}

int car_object(const struct track_cam *cam, struct track_object *obj)
{
    (void)cam;
    if(views[view].incar && !trackside) return 0;
    *obj = (struct track_object){ pc.x, pc.y, pc.z + crash.height, 1200.0f, 0, 0, car_make, 0, 0,
                                  &refl, refl_phase };
    return 1;
}

void car_reflection(const float acc[3], uint32_t phase)
{
    refl = (struct track_refl){ { (int32_t)pc.x, (int32_t)pc.y, (int32_t)(pc.z + crash.height) },
                                { acc[0], acc[1], acc[2] } };
    refl_phase = phase;
}

/* Sine and cosine of a. A tilt (under 0.5 rad, which is nearly always) is worked out by the
   series, to float's precision, without the library's calls. */
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

/* Sets ps to the pose of a mesh placed as one body at (x, y, z) with the heading yaw, the pitch and the
   roll: no wheels turned and no lean. */
static inline void flat_pose(struct pose *ps, float x, float y, float z, float yaw, float pitch, float roll)
{
    *ps = (struct pose){ { 1.0f, 0.0f, 1.0f, 0.0f }, { 0 }, { 1.0f, 0.0f, 1.0f, 0.0f }, 0, x, y, z, 0.0f };
    sin_cos(pitch, &ps->car[1], &ps->car[0]);
    sin_cos(roll, &ps->car[3], &ps->car[2]);
    ps->car[4] = cosf(yaw);
    ps->car[5] = sinf(yaw);
}

/* Makes the first block of the flat mesh `file` and places it as a car's body is placed
   (car.h). The pose has no wheels turned and no lean. */
void car_flat_block(const uint8_t *file, float x, float y, float z, float yaw, float pitch, float roll,
                    struct track_block *out)
{
    const struct cblock q = { 0 };
    struct pose ps;
    flat_pose(&ps, x, y, z, yaw, pitch, roll);
    place_block(&q, &ps, out);
    out->file = file;
    out->blk = file + *(const int32_t *)(file + 0x18);
    out->phase = 0;
}

/* Places every block of a mesh as one body (car.h). Each block is put at its own place in the
   mesh. With a camera the blocks are then sorted into the game's drawing order. */
int car_mesh_blocks(const uint8_t *file, float x, float y, float z, float yaw, float pitch, float roll,
                    const struct track_cam *cam, struct track_block *out, int max)
{
    struct pose ps;
    flat_pose(&ps, x, y, z, yaw, pitch, roll);
    int nb = *(const int32_t *)(file + 0x14), n = 0;
    float key[MAX_BLOCKS];
    for(int b = 0; b < nb && n < max && n < MAX_BLOCKS; b++, n++) {
        struct cblock q = { 0 };
        cblock_find(&q, file, b);
        place_block(&q, &ps, &out[n]);
        out[n].file = file;
        out[n].blk = q.blk;
        out[n].phase = 0;
        if(cam) {
            /* The block's middle from the camera, as draw_model() has it. block_point() adds
               the block's place. */
            float mid[3], mw[3];
            for(int j = 0; j < 3; j++) mid[j] = (float)*(const int16_t *)(q.blk + 4 + 2 * j);
            block_point(&q, &ps, mid, mw);
            float dx = mw[0] - cam->x, dy = mw[1] - cam->y, dh = mw[2] - cam->h;
            key[n] = -rintf(sqrtf(dx * dx + dy * dy + dh * dh));
        }
    }
    if(cam)             /* furthest middle first; on a tie the lower block first (stable insertion sort) */
        for(int i = 1; i < n; i++) {
            struct track_block t = out[i];
            float k = key[i];
            int j = i;
            while(j > 0 && key[j - 1] > k) { out[j] = out[j - 1]; key[j] = key[j - 1]; j--; }
            out[j] = t;
            key[j] = k;
        }
    return n;
}

/* The shadow's place now, and whether there is one (car_effects). */
static struct { float x, y, h; int on; } shadow;

/* Makes the player's shadow block (original: STARTL.EXE 0x15c0c) when it is drawn. The CPU
   time spent before the first piece goes out counts for the frame; once the pieces go, the
   geometry unit's work counts. The heading is the car object's, and the tilt is kept in a
   crash. */
static int shadow_make(int id, const struct track_cam *cam, struct track_object *obj)
{
    static struct track_block shadow_block;
    (void)id;
    (void)cam;
    car_flat_block(efx_shadow(), obj->x, obj->y, obj->h, pc.yaw + pc.drift, shadow_pitch, shadow_roll,
                   &shadow_block);
    obj->n = 1;
    obj->b = &shadow_block;
    return 1;
}

/* Makes the objects of the player's shadow and of the puffs (car.h). The puffs' blocks are
   made at once, all in a row, so that the code stays in the cache.
   - A puff faces the screen (flag 0x8000 of its object, original: STARTL.EXE 0x118a4): the
     engine puts the camera's angles in for its own (original: STARTL.EXE 0x8abc7 -> 0x4bca0).
     So its block's axes are the camera's right, up and forward.
   - The texture's frame is 6 x (phase >> 2) >> 14 (original: STARTL.EXE 0xcd906). */
int car_effects(const struct track_cam *cam, struct track_object *objs, int max)
{
    static struct track_block puff_block[PUFFS];
    uint32_t t0 = rdcycle();
    int n = 0;
    const uint8_t *sh = efx_shadow(), *sm = efx_smoke();
    shadow.on = 0;
    if(sh && (!views[view].incar || trackside) && n < max) {
        /* The shadow's place. Along the ground it is the car's whole-number x and z moved 20
           units (ds:0x854) away from the camera (original: STARTL.EXE 0x18cbc). Its height is
           2 under the ground below the wheels, without the grass's jolts (original: STARTL.EXE
           0x16043); that ground is +0x4ec, the sum of the wheel heights / 4 (original:
           STARTL.EXE 0x1675f). */
        int32_t cx = (int32_t)pc.x, cy = (int32_t)pc.y,
                ex = (int32_t)cam->x - cx, ey = (int32_t)cam->y - cy;
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

/* Counts the puffs that are on (car.h). */
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

/* Reads the numbers of the game mode for track n (car.h). TRACK_DATA of MAPn?.DTR gives the
   carnage time (its fourth number, at +12). MAPnSLA.MRK gives slalom's gates. A missing or
   malformed file leaves the carnage time or the gate count at 0. */
void car_mode_init(const uint8_t *dat, uint32_t dat_size, int n, int difficulty)
{
    static uint8_t dtr[0x800];
    char name[16] = "MAPnA.DTR";                        /* the difficulty's letter (original: 0x13251) */
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

/* Sets up the intro's race (car.h): the race is on, and the car is moved 1e7 units away, out
   of the world, with its engine stopped. */
void car_intro(void)
{
    race.on = 1;
    pc.x += 1.0e7f;
    snd_engine = snd_stop(snd_engine);
}

/* Reads the nodes of MAPnRPL.SPL into rpl[] (car.h). The file has a count, a start and the
   nodes of 14 words each; the camera uses the words 1 to 3 of a node. */
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

/* Gives the trackside camera in *cam (original: STARTL.EXE 0x1a620).
   1. It picks the node nearest to the car on the ground. The car's place is in whole units
      (+0x4b0, +0x4b8: where the game draws it). Ours is the float place cut, not `whole`,
      which loses up to a unit a step and is thousands off after a race. The node's distance
      from the car is cut to a whole number. A node at the car's very place is passed over,
      and of equally near nodes the first is kept.
   2. From the node the camera looks straight at the car. The heading is the way to the car
      (forward = sin yaw, -cos yaw). The pitch is 2 pi - acos(flat / distance): down by the
      angle that the height between the two gives, whichever is the higher. There is no lean.
   3. More than 1500 units from the car, the camera moves on along its heading and pitch until
      it is 1500 away, each coordinate cut to a whole unit. */
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

/* Chooses the view (car.h). The crash view stays until the crash is over. */
void car_view(int v)
{
    if(crash.on) return;
    view = (v % CAR_VIEWS + CAR_VIEWS) % CAR_VIEWS;
    chase.init = 0;
    chase_step();
}

/* Turns the trackside camera on, if the track has any places for it. */
void car_trackside(void)
{
    trackside = rpl_n > 0;
}

/* Gives the camera of the view as it is now in *cam (view_pose). The trackside camera, the
   cameras in the car (bumper cam, cockpit) and the chase cameras each have their own pose. */
void car_camera(struct track_cam *cam)
{
    const struct view_def *v = &views[view];
    if(trackside) {
        trackside_camera(cam);
        return;
    }
    if(v->incar) {
        /* With the suspension (original: STARTL.EXE 0x1a2ac) the pitch gets the body's +0x4e0,
           the roll gets 3 x its +0x4e8 (the camera's roll is the other way round from the
           game's), and the height gets its drop. */
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
