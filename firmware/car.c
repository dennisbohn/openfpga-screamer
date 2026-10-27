/*
 * The player's car (car.h), after OpenScreamer's port/src/main.c (GPL-3.0), checked against
 * STARTL.EXE where it says so. Not yet here: the start lights, laps and clocks, the
 * suspension (the body's lean), the shadow, smoke, the camera's fly-across between views.
 */
#include "car.h"
#include <math.h>
#include "hw.h"

#define PI          3.14159265f
/* the bumper cam's height over the car: camera_hi in TRACK_DATA of MAPnA.DTR */
#define CAMERA_HI   ((float)track_camera_hi())

/* the game's views (OpenScreamer's g_views): how far behind, how high, looking down how much */
struct view_def { float behind, above, down; int incar; };
static const struct view_def views[CAR_VIEWS + 1] = {
    { 1300.0f, 300.0f, 0.1885f, 0 },    /* row 2: near chase */
    { 1700.0f, 300.0f, 0.1257f, 0 },    /* row 6: far chase */
    {    0.0f,   0.0f, 0.0f,    1 },    /* row 0: bumper cam */
    { 1400.0f, 100.0f, 0.1257f, 0 },    /* row 5: the crash's, not the player's to choose */
};
#define CRASH_VIEW  CAR_VIEWS

static struct car_state pc;
static struct track_model model;
static int view;
static int mark_at;                     /* the marker last reached */
static uint32_t wall_hits, crashes;
static struct car_race race;
static uint8_t mark_seen[64];
static int mark_count;
static float needle;
static float progress;                  /* how far round the race (marker_progress) */

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
    float cr = fabsf(cosf(pc.roll)), cp = fabsf(cosf(pc.pitch));
    float a = pc.yaw + pc.drift, cs = cosf(a), sn = sinf(a);
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
    float cs2 = cosf(pc.yaw), sn2 = sinf(pc.yaw);
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

static const struct track_dir *nearest_dir(float x, float y)
{
    int n;
    const struct track_dir *d = track_dirs(&n), *best = 0;
    float bd = 1e30f;
    for(int i = 0; i < n; i++) {
        float e = ((float)d[i].x - x) * ((float)d[i].x - x) + ((float)d[i].y - y) * ((float)d[i].y - y);
        if(e < bd) { bd = e; best = &d[i]; }
    }
    return best;
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
    float fx = sinf(pc.yaw), fy = -cosf(pc.yaw), rx = -cosf(pc.yaw), ry = -sinf(pc.yaw);
    float cr = fabsf(cosf(pc.roll)), cp = fabsf(cosf(pc.pitch));
    int out[4], any = 0;
    float px[4], py[4], wall_angle = 0.0f;
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
    const struct track_dir *d = nearest_dir(pc.x, pc.y);
    float off = d ? fabsf(pc.yaw - d->angle) : 0.0f;
    while(off > PI) off -= PI;
    if(off > PI / 2.0f) off = PI - off;
    int nm;
    const struct track_marker *mk = track_markers(&nm);
    int open = nm == 0 || mk[mark_at].kind == 0;
    int crashing = pc.speed * (off / (PI / 2.0f)) > pc.k_wall && open;
    drive_hit_wall(&pc, mx, my, (float)(out[2] - out[0]), wall_angle);
    if(crashing) {
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

/* a crash's step: gravity 6000, the turning dies away (damp[97]); each landing throws it up
   again at 0.4 times the fall, level, with a new small turn; the fourth ends it */
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
    float bonus = (float)track_bonus_time();
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
        } else if(i == 0 && mark_count >= n - 1) {        /* across the line */
            if(race.laps + 1 >= race.total_laps) race.finished = 1;
            else if(!race.finished) race.timer = race.timer / 5.0f + bonus;
            race.laps++;
            race.last = race.lap;
            if(race.best == 0.0f || race.last < race.best) race.best = race.last;
            for(int j = 0; j < n && j < 64; j++) mark_seen[j] = 0;
            mark_count = 0;
            race.lap = 0.0f;
        }
        if(i != 0 && i < 64 && !mark_seen[i]) {
            mark_seen[i] = 1;
            mark_count++;
            if((m->flags & 2) && race.clock_on && !race.finished) race.timer = race.timer / 5.0f + bonus;
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

void car_init(int which)
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
    wall_hits = crashes = 0;
    crash.on = 0;
    race = (struct car_race){ 0 };
    race.timer = 15.0f;
    race.total_laps = 3;                                /* the option's default (0x1d2ac) */
    for(int j = 0; j < 64; j++) mark_seen[j] = 0;
    mark_count = 0;
    needle = 0.0f;
    pc.waiting = 1;

    /* player_reset: on the road point by the third path point, facing along the path, at
       the path's height until the wheels find the ground; no skidding until a marker */
    float px, py, ph;
    track_start(&pc.x, &pc.y, &pc.yaw);
    track_path_point(2, &px, &py, &ph);
    pc.z = ph;
    pc.surface = 0.0f;
    mark_at = 0;
    progress = 0.0f;
    ct.ready = 0;
    contact();
    chase.init = 0;
    chase_step();
}

void car_step(uint32_t keys, uint32_t keys_before)
{
    uint32_t pressed = keys & ~keys_before;
    struct drive_input in;
    /* gas and brake on the buttons only: on the d-pad a thumb steering into a diagonal
       would brake or lift off by accident */
    in.throttle = (keys & KEY_A) ? 1.0f : 0.0f;
    in.brake = (keys & KEY_B) ? 1.0f : 0.0f;
    in.steer = ((keys & KEY_LEFT) ? 1.0f : 0.0f) - ((keys & KEY_RIGHT) ? 1.0f : 0.0f);
    in.shift = (pressed & KEY_R) ? 1 : (pressed & KEY_L) ? -1 : 0;
    /* the start lights: green at 6 s; until then the pedal only revs the engine */
    race.clock += DRIVE_DT;
    race.on = race.clock >= 6.0f;
    pc.waiting = !race.on;
    if(race.time_out) {                                 /* 0x18a04: out of time and stopped: over */
        in.throttle = 0.0f;
        if(pc.speed < 1.0f) race.game_over = 1;
    }
    if(race.finished || race.game_over) { in.throttle = 0.0f; in.brake = 0.4f; }
    if(crash.on) in.throttle = in.steer = 0.0f;          /* no drive in the air */
    crash_step();
    drive_step(&pc, &in);
    walls_step();
    markers_step();
    contact();
    chase_step();

    /* wheels_step: the wheels turn with the speed; the front ones steer by the share of the
       lock, the sign as OpenScreamer has it since its fix (the model's +x is the car's left) */
    spin = fmodf(spin - pc.speed * DRIVE_DT / 108.6f, 6.2831853f);
    steer = -(pc.steer_max > 0.0f ? pc.steer / pc.steer_max : 0.0f) / 1.5f;

    /* dash_step: the rev counter's needle eases to rpm / maxrpm x pi */
    float want = pc.rpm / pc.spec.maxrpm * PI;
    if(want > PI) want = PI;
    needle = want + (needle - want) * drive_damp(0x5a);
}

const struct car_race *car_race(void)
{
    return &race;
}

float car_needle(void)
{
    return needle;
}

/* model k (AUTOk) */
static const struct cmodel *cmodel(int k)
{
    int nm;
    track_models(&nm);
    if(k < 0 || k >= nm || k >= MAX_MODELS) k = 0;
    struct cmodel *m = &cmodels[k];
    if(m->ready) return m;
    /* the blocks: a name of three letters ending in i (a tyre, the arches' insides) counts
       4000 further off, one ending in x (a hub) 1000, unless it starts with A */
    m->mesh = track_model_mesh(k);
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
    return m;
}

int car_model_index(const struct car_spec *sp)
{
    return (sp->name[4] - '0') * 10 + (sp->name[5] - '0');           /* "AUTO03" -> 3 */
}

/* a car as it is drawn: the cos and sin of the wheels' turn and the front ones' angle, of the
   object's pitch, roll and heading, of the body's own pitch and roll, and its place */
struct pose { float sc[4], car[6], body[4]; int lean; float x, y, z; };

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
    if(ps->lean && !q->wheel) {                         /* the body's lean (an opponent in a bend) */
        float yb = y * ps->body[0] - z * ps->body[1], zb = z * ps->body[0] + y * ps->body[1];
        float xb = x * ps->body[2] - yb * ps->body[3];
        y = yb * ps->body[2] + x * ps->body[3];
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

int car_draw(int model, const struct car_look *lk, const struct track_cam *cam, struct track_block *out,
             struct track_object *obj)
{
    static struct track_block tb[MAX_BLOCKS];
    const struct cmodel *m = cmodel(model);
    if(!m->ncb) return 0;
    struct pose ps = { { cosf(lk->spin), sinf(lk->spin), cosf(lk->steer), sinf(lk->steer) },
                       { cosf(lk->pitch), sinf(lk->pitch), cosf(lk->roll), sinf(lk->roll), cosf(lk->yaw), sinf(lk->yaw) },
                       { cosf(lk->body_pitch), sinf(lk->body_pitch), cosf(lk->body_roll), sinf(lk->body_roll) },
                       lk->body_pitch != 0.0f || lk->body_roll != 0.0f, lk->x, lk->y, lk->z };
    int ord[MAX_BLOCKS];
    float key[MAX_BLOCKS];
    for(int b = 0; b < m->ncb; b++) {
        static const float zero[3] = { 0.0f, 0.0f, 0.0f }, ex[3] = { 1.0f, 0.0f, 0.0f };
        static const float ey[3] = { 0.0f, 1.0f, 0.0f }, ez[3] = { 0.0f, 0.0f, 1.0f };
        const struct cblock *q = &m->cb[b];
        struct track_block *t = &tb[b];
        float o[3], px[3], py[3], pz[3], mid[3], mw[3];
        block_point(q, &ps, zero, o);
        block_point(q, &ps, ex, px);
        block_point(q, &ps, ey, py);
        block_point(q, &ps, ez, pz);
        for(int j = 0; j < 3; j++) {
            t->o[j] = o[j];
            t->m[3 * j] = px[j] - o[j];
            t->m[3 * j + 1] = py[j] - o[j];
            t->m[3 * j + 2] = pz[j] - o[j];
        }
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
    obj->x = lk->x;
    obj->y = lk->y;
    obj->h = lk->z;
    obj->radius = 1200.0f;
    obj->n = m->ncb;
    obj->b = out;
    return 1;
}

int car_object(const struct track_cam *cam, struct track_object *obj)
{
    static struct track_block blocks[MAX_BLOCKS];
    if(views[view].incar) return 0;
    /* the object's angles (player_sync): the car's, negated, and a crash's tumbling on top;
       the texture frame (0x173a9): paint job | 0x4000 not braking | 0x2000 at 4000 units/s or
       under, of which the phase is a quarter (record +0x20 >> 2) */
    struct car_look lk = { pc.x, pc.y, pc.z + crash.height, pc.yaw + pc.drift, -pc.pitch + crash.pitch,
                           -pc.roll + crash.roll, 0.0f, 0.0f, spin, steer,
                           ((uint32_t)pc.spec.livery | (pc.brake == 0.0f ? 0x4000u : 0) |
                            (pc.speed <= 4000.0f ? 0x2000u : 0)) >> 2 };
    return car_draw(pmodel, &lk, cam, blocks, obj);
}

void car_view(int v)
{
    if(crash.on) return;                                /* the crash view until it is over */
    view = (v % CAR_VIEWS + CAR_VIEWS) % CAR_VIEWS;
    chase.init = 0;
    chase_step();
}

/* view_pose */
void car_camera(struct track_cam *cam)
{
    const struct view_def *v = &views[view];
    if(v->incar) {
        cam->x = pc.x;
        cam->y = pc.y;
        cam->h = pc.z + CAMERA_HI;
        cam->yaw = pc.yaw + pc.drift;
        cam->pitch = pc.pitch;
        cam->roll = pc.roll;
        return;
    }
    float cp = cosf(chase.pitch);
    cam->x = pc.x - cp * v->behind * sinf(chase.yaw);
    cam->y = pc.y + cp * v->behind * cosf(chase.yaw);
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
