/*
 * The player's car (car.h), after OpenScreamer's port/src/main.c (GPL-3.0). Not yet here:
 * the walls (player_walls), the start lights, laps and clocks, the suspension, crashes.
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
    if(!ct.ready)
        for(int n = 0; n < 4; n++) ct.h[n] = ct.hc[n] = pc.z;
    pc.rough = 0;
    for(int n = 0; n < 4; n++) {
        float x = lx[n] * cr, z = lz[n] * cp;
        float wx = pc.x + x * cs - z * sn, wy = pc.y + x * sn + z * cs, h;
        int grass;
        if(track_under(wx, wy, &h, &grass)) {
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
    float cs2 = cosf(pc.yaw), sn2 = sinf(pc.yaw);
    for(int n = 0; n < 4; n++) {
        float x = lx[n] * cr, z = lz[n] * cp, h;
        if(track_under(pc.x + x * cs2 - z * sn2, pc.y + x * sn2 + z * cs2, &h, 0)) ct.hc[n] = h;
    }
    ct.cam_pitch = atanf((ct.hc[2] - ct.hc[3]) / (af + ar));
    ct.cam_roll  = atanf((ct.hc[2] - ct.hc[0]) / (2.0f * tr));
    ct.ready = 1;
    pc.z = 0.25f * (jolted[0] + jolted[1] + jolted[2] + jolted[3]);
    pc.pitch = atanf((jolted[2] - jolted[3]) / (af + ar));
    pc.roll  = atanf((jolted[2] - jolted[0]) / (2.0f * tr));
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
    in.throttle = (keys & (KEY_A | KEY_UP)) ? 1.0f : 0.0f;
    in.brake = (keys & (KEY_B | KEY_DOWN)) ? 1.0f : 0.0f;
    in.steer = ((keys & KEY_LEFT) ? 1.0f : 0.0f) - ((keys & KEY_RIGHT) ? 1.0f : 0.0f);
    in.shift = (pressed & KEY_R) ? 1 : (pressed & KEY_L) ? -1 : 0;
    drive_step(&pc, &in);
    markers_step();
    contact();
    chase_step();
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
