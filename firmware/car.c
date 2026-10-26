/*
 * The player's car (car.h), after OpenScreamer's port/src/main.c (GPL-3.0). Not yet here:
 * the start lights, laps and clocks, the suspension (the body's lean), crashes, the shadow.
 */
#include "car.h"
#include <math.h>
#include "hw.h"

#define PI          3.14159265f
/* the bumper cam's height over the car: camera_hi in TRACK_DATA of MAP1A.DTR (track 1) */
#define CAMERA_HI   300.0f

/* the game's views (OpenScreamer's g_views): how far behind, how high, looking down how much */
struct view_def { float behind, above, down; int incar; };
static const struct view_def views[CAR_VIEWS] = {
    { 1300.0f, 300.0f, 0.1885f, 0 },    /* row 2: near chase */
    { 1700.0f, 300.0f, 0.1257f, 0 },    /* row 6: far chase */
    {    0.0f,   0.0f, 0.0f,    1 },    /* row 0: bumper cam */
};

static struct car_state pc;
static struct track_model model;
static int view;
static int mark_at;                     /* the marker last reached */
static uint32_t wall_hits;

/* the body's corners for the walls (player_car, 0x136c8): each wheel's place moved 250 units
   ahead of the front wheels, 150 behind the rear ones; sideways at the wheels. [k][0] is to
   the right, [k][1] forward: front-left, rear-left, front-right, rear-right */
static float corner[4][2];
#define MAX_WALLS   1024
static float wall_in[MAX_WALLS];        /* which side of each wall the track is on */

/* the car's blocks (mesh.c): place, the middle the drawing order goes by, how much further
   off it counts, whether it is a wheel and what it does */
#define MAX_BLOCKS  16
struct cblock { const uint8_t *blk; float t[3], mid[3]; int back, wheel, spins, steers; };
static const uint8_t *mesh;
static struct cblock cb[MAX_BLOCKS];
static int ncb;
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
    wall_hits++;
    drive_hit_wall(&pc, mx, my, (float)(out[2] - out[0]), wall_angle);
}

/* markers_step (0x1820c): the marker last reached and its two neighbours; inside one's
   circle it becomes the last reached and its slip the surface factor of the cornering force */
static void markers_step(void)
{
    int n;
    const struct track_marker *mk = track_markers(&n);
    if(n == 0) return;
    for(int k = -1; k <= 1; k++) {
        int i = ((mark_at + k) % n + n) % n;
        const struct track_marker *m = &mk[i];
        float dx = pc.x - (float)m->x, dy = pc.y - (float)m->y, rad = (float)m->radius;
        if(fabsf(dx) >= rad || fabsf(dy) >= rad) continue;
        if(sqrtf(dx * dx + dy * dy) > rad) continue;
        mark_at = i;
        pc.surface = m->slip;
    }
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
    int k = (sp->name[4] - '0') * 10 + (sp->name[5] - '0');            /* "AUTO03" -> 3 */
    model = models[k >= 0 && k < nm ? k : 0];
    drive_init(&pc, sp, model.wheelbase > 0.0f ? model.wheelbase : 935.0f);
    float zf = model.axle_front + 250.0f, zr = model.axle_rear + 150.0f;
    corner[0][0] = -model.half_track; corner[0][1] =  zf;
    corner[1][0] = -model.half_track; corner[1][1] = -zr;
    corner[2][0] =  model.half_track; corner[2][1] =  zf;
    corner[3][0] =  model.half_track; corner[3][1] = -zr;
    walls_prepare();

    /* the blocks: a name of three letters ending in i (a tyre, the arches' insides) counts
       4000 further off, one ending in x (a hub) 1000, unless it starts with A */
    mesh = track_model_mesh(k < nm ? k : 0);
    ncb = 0;
    for(int b = 0; b < *(const int32_t *)(mesh + 0x14) && ncb < MAX_BLOCKS; b++) {
        struct cblock *q = &cb[ncb++];
        q->blk = mesh + *(const int32_t *)(mesh + 0x18 + 4 * b);
        int32_t so = *(const int32_t *)(q->blk + 0x0C);
        for(int j = 0; j < 3; j++) q->t[j] = so > 0 ? (float)*(const int32_t *)(mesh + so + 0x10 + 4 * j) : 0.0f;
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
    spin = steer = 0.0f;
    wall_hits = 0;

    /* player_reset: on the road point by the third path point, facing along the path, at
       the path's height until the wheels find the ground; no skidding until a marker */
    float px, py, ph;
    track_start(&pc.x, &pc.y, &pc.yaw);
    track_path_point(2, &px, &py, &ph);
    pc.z = ph;
    pc.surface = 0.0f;
    mark_at = 0;
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
    drive_step(&pc, &in);
    walls_step();
    markers_step();
    contact();
    chase_step();

    /* wheels_step: the wheels turn with the speed; the front ones steer by the share of the
       lock, the sign as OpenScreamer has it since its fix (the model's +x is the car's left) */
    spin = fmodf(spin - pc.speed * DRIVE_DT / 108.6f, 6.2831853f);
    steer = -(pc.steer_max > 0.0f ? pc.steer / pc.steer_max : 0.0f) / 1.5f;
}

/* A block's point p (as stored) in the world (block_to_view, tilt, place): a wheel turned
   about its axle and steered, moved to its place on the car, the car's pitch and roll, its
   heading with the skid angle, its place */
static void block_point(const struct cblock *q, const float sc[4], const float car[6], const float p[3], float w[3])
{
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
    /* tilt: the object's pitch and roll are the car's, negated (player_sync) */
    float y3 = y * car[0] + z * car[1], z3 = z * car[0] - y * car[1];
    float x3 = x * car[2] - y3 * car[3], y4 = y3 * car[2] + x * car[3];
    w[0] = pc.x + x3 * car[4] - z3 * car[5];
    w[1] = pc.y + x3 * car[5] + z3 * car[4];
    w[2] = pc.z + y4;
}

int car_object(const struct track_cam *cam, struct track_object *obj)
{
    static struct track_block tb[MAX_BLOCKS];
    if(views[view].incar || !ncb) return 0;
    const float sc[4] = { cosf(spin), sinf(spin), cosf(steer), sinf(steer) };
    float a = pc.yaw + pc.drift;
    const float car[6] = { cosf(-pc.pitch), sinf(-pc.pitch), cosf(-pc.roll), sinf(-pc.roll), cosf(a), sinf(a) };
    /* the texture frame (0x173a9): paint job | 0x4000 not braking | 0x2000 at 4000 units/s or
       under, of which the phase is a quarter (record +0x20 >> 2) */
    uint32_t phase = ((uint32_t)pc.spec.livery | (pc.brake == 0.0f ? 0x4000u : 0) |
                      (pc.speed <= 4000.0f ? 0x2000u : 0)) >> 2;
    int ord[MAX_BLOCKS];
    float key[MAX_BLOCKS];
    for(int b = 0; b < ncb; b++) {
        static const float zero[3] = { 0.0f, 0.0f, 0.0f }, ex[3] = { 1.0f, 0.0f, 0.0f };
        static const float ey[3] = { 0.0f, 1.0f, 0.0f }, ez[3] = { 0.0f, 0.0f, 1.0f };
        const struct cblock *q = &cb[b];
        struct track_block *t = &tb[b];
        float o[3], px[3], py[3], pz[3], mid[3], mw[3];
        block_point(q, sc, car, zero, o);
        block_point(q, sc, car, ex, px);
        block_point(q, sc, car, ey, py);
        block_point(q, sc, car, ez, pz);
        for(int j = 0; j < 3; j++) {
            t->o[j] = o[j];
            t->m[3 * j] = px[j] - o[j];
            t->m[3 * j + 1] = py[j] - o[j];
            t->m[3 * j + 2] = pz[j] - o[j];
        }
        t->file = mesh;
        t->blk = q->blk;
        t->phase = phase;
        /* the drawing order (0x8ac58, 0x8af24): the furthest middle first, the tyres and
           hubs counted further off; on a tie the lower block first */
        for(int j = 0; j < 3; j++) mid[j] = q->mid[j] - q->t[j];
        block_point(q, sc, car, mid, mw);
        float dx = mw[0] - cam->x, dy = mw[1] - cam->y, dh = mw[2] - cam->h;
        key[b] = -rintf(sqrtf(dx * dx + dy * dy + dh * dh)) - (float)q->back;
        int i = b;
        while(i > 0 && key[ord[i - 1]] > key[b]) { ord[i] = ord[i - 1]; i--; }
        ord[i] = b;
    }
    static struct track_block sorted[MAX_BLOCKS];
    for(int b = 0; b < ncb; b++) sorted[b] = tb[ord[b]];
    obj->x = pc.x;
    obj->y = pc.y;
    obj->h = pc.z;
    obj->radius = 1200.0f;
    obj->n = ncb;
    obj->b = sorted;
    return 1;
}

void car_view(int v)
{
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
