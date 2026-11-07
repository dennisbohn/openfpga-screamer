/*
 * The player's car physics (drive.h), ported from OpenScreamer's port/src/drive.c (GPL-3.0),
 * which ported it from STARTL.EXE. drive_step() runs the stages of the game's frame on the
 * player record at 0xf918. The addresses are routines of the original (STARTL.EXE):
 *
 *   0x16f24  integrate: move the car, apply the wall shove, wrap the angles
 *   0x15a70  -> 0x18da0 engine and gearbox, 0x16094 throttle, 0x16140 brake, 0x1628c steering
 *   0x15b9c  -> 0x17d04 skid and spin, 0x188a8 spin recovery
 *   0x15aa8  -> lap markers, body-corner collision, wheel contact, 0x16c90 wall hit, ride height
 *
 * The suspension, the sound and the crash are not here. The skid routine only sets the flag for
 * the smoke (skid_lock); car.c makes the smoke. The stages of 0x15aa8 are in car.c, apart from
 * the wall hit (drive_hit_wall).
 *
 * The engine, throttle, brake, steering and skid routines are as OpenScreamer has them, line
 * for line, plus the joystick paths for a docked controller's stick and triggers (the axis
 * flags of struct drive_input).
 *
 * The damping table: the game smooths values with v = (v - target) * damp[i] + target, where
 * damp[i] = pow(0.5 + i * 0.005, 50 / (fps + 1)). The stiffness 0.5 + i * 0.005 runs from 0.5
 * (snappy) to 0.995 (lazy) and is defined for 50 frames a second. The steps here are exactly
 * 50 a second. The table is worked out once because pow is slow on the CPU.
 */
#include "drive.h"
#include <math.h>
#include <string.h>

#define FPS 50.0f

static float damp_tab[100];
static int damp_ready;

/* The smoothing factor for stiffness index i (0 .. 99), from the table, which is filled on the
   first call. */
static float damp(int i)
{
    if(!damp_ready) {
        for(int k = 0; k < 100; k++) damp_tab[k] = powf(0.5f + (float)k * 0.005f, 50.0f / (FPS + 1.0f));
        damp_ready = 1;
    }
    return damp_tab[i];
}

float drive_damp(int i) { return damp(i); }

/* A small cache for sinf (c = 0) and cosf (c = 1). It has MEMO slots for each function. The slot
   is picked by a hash of the angle's bits, and the value is reused only if the bits match
   exactly, so the result is always the library's. */
#define MEMO 8
static float memo(float a, int c)
{
    static uint32_t key[2][MEMO];
    static float val[2][MEMO];
    static uint8_t kept[2][MEMO];
    uint32_t b;
    memcpy(&b, &a, sizeof b);
    int i = (int)((b ^ b >> 11 ^ b >> 19) & (MEMO - 1));
    if(kept[c][i] && key[c][i] == b) return val[c][i];
    float v = c ? cosf(a) : sinf(a);
    key[c][i] = b;
    val[c][i] = v;
    kept[c][i] = 1;
    return v;
}

float drive_sin(float a) { return memo(a, 0); }
float drive_cos(float a) { return memo(a, 1); }

/* Eases *v towards target with the stiffness index i (original: STARTL.EXE 0x22b60). */
static void approach(float *v, float target, int i)
{
    *v = (*v - target) * damp(i) + target;
}

/* The same for an angle (original: STARTL.EXE 0x220fc). Within a quarter turn (pi/2) of the
   target the difference goes through a sine; farther away it is eased as in approach(). */
static void approach_angle(float *v, float target, int i)
{
    if(fabsf(target - *v) < 1.5707963f)
        *v = sinf(*v - target) * damp(i) + target;
    else
        *v = (*v - target) * damp(i) + target;
}

/* Square root that gives 0 for a negative argument (original: STARTL.EXE 0x22bcc). */
static float sqrt0(float x) { return x < 0.0f ? 0.0f : sqrtf(x); }

/* How hard each gear pulls: the table at 0x1d84 in the original's data segment, indexed by
   gear. */
static const float GEAR_PULL[8] = { 0.0f, 5.0f, 3.5f, 2.0f, 0.95f, 0.78f, 0.72f, 0.0f };

/* ------------------------------------------------------------------ set-up */

/* Turns the car's row of CARS.DTR into the constants that each frame uses
   (original: STARTL.EXE 0x136c8, its last part). */
void drive_init(struct car_state *c, const struct car_spec *spec, float wheelbase)
{
    memset(c, 0, sizeof *c);
    c->spec = *spec;
    c->wheelbase = wheelbase;
    c->surface = 1.0f;
    c->k_rpm          = spec->maxrpm / spec->maxspd * 0.02223f;
    c->k_engine_brake = spec->maxspd * 0.03f;
    c->k_brake        = spec->maxspd * 0.3f;
    c->k_wall         = spec->maxspd * 0.3f;
    c->grip_slide     = spec->grip * 16000.0f;
    c->grip_spin      = spec->grip * 26000.0f;
    c->grip_decel     = spec->grip * 16000.0f;
    c->grip_lock      = spec->grip * 11500.0f;
}

float drive_kmh(const struct car_state *c) { return c->speed * 340.0f / 21000.0f; }

/* ------------------------------------------------------------------ 0x18da0 */

/* Engine and gearbox (original: STARTL.EXE 0x18da0). One call picks the gear (by the shift keys,
   or automatically), works out the revs from the speed and the throttle, and then the new speed
   from the revs. It changes gear, launch, rpm, speed and lock; throttle and the other inputs
   are read only. */
static void engine(struct car_state *c, const struct drive_input *in)
{
    const struct car_spec *s = &c->spec;
    const float M = s->maxrpm, dt = DRIVE_DT;

    /* Until the green light neither the gearbox nor the drive runs. The engine follows the
       pedal, as in neutral. */
    if(c->waiting) {
        approach(&c->rpm, c->throttle * M, 0x4b);
        if(c->speed < dt * 500.0f) c->gear = 0;
        return;
    }

    if(!s->automatic) {
        if(in->shift > 0 && c->gear < s->nshift) c->gear++;
        if(in->shift < 0 && c->gear > 0)         c->gear--;
    } else {
        /* Automatic box: change up past 8500 rpm and down below 4000. Each limit is moved by
           the gear number (up: 8500 + gear, down: 4000 - gear). */
        if(c->rpm > (float)(c->gear + 8500)) {
            if(c->gear < s->nshift) c->gear++;
        } else if(c->rpm < (float)(4000 - c->gear) && c->gear > 1) {
            c->gear--;
        }
        if(c->launch) c->gear = 1;
    }

    if(c->gear == 0) {
        /* Neutral: the car coasts down (500 units/s each second) and the engine follows the
           pedal. */
        c->speed -= dt * 500.0f;
        if(c->speed < 0.0f) c->speed = 0.0f;
        approach(&c->rpm, c->throttle * M, 0x4b);
    } else {
        float ratio = s->ratio[c->gear];
        float top = s->maxspd;                          /* +0x510, the speed cap */

        /* Normally the engine is locked to the wheels. When the car pulls away with high revs
           and almost no speed, the wheels spin (state 5): the revs fall by 500 and the speed
           rises by a flat 1000 units/s, both each second, until the two meet. */
        if(c->rpm <= s->grip * 6000.0f || c->speed >= 2000.0f) {
            c->rpm = ratio * c->speed * c->k_rpm;
            c->launch = 0;
        } else if(in->throttle > 200.0f / 1023.0f) {
            c->launch = 1;
            c->rpm -= dt * 500.0f;
        } else {
            c->rpm = ratio * c->speed * c->k_rpm;
            c->launch = 0;
        }

        /* How hard this gear pulls: strongest in the middle of its range. */
        float vgear = M / (c->k_rpm * ratio);           /* speed at max rpm */
        float pull, curve;
        if(s->nshift == 1) {
            pull  = s->accel;
            curve = sqrt0(1.0f - c->speed / vgear);     /* only the one-gear "Raz car" */
        } else {
            float a = (2.0f - (vgear - c->speed) / (vgear * 0.5f)) - 1.0f;
            if(c->gear == 1 && a < 0.0f) a = 0.2f;
            curve = (1.0f + sinf(a * HALF_PI)) * 0.5f;
            pull  = s->accel * GEAR_PULL[c->gear];
        }
        if(c->rpm < c->throttle * M)
            c->rpm += c->throttle * pull * curve * dt;

        if(c->rpm <= M) {
            if(c->throttle * M < c->rpm) {              /* engine braking */
                c->rpm -= sqrt0(c->rpm / M) * c->k_engine_brake * dt;
                if(c->rpm < M * c->throttle) c->rpm = M * c->throttle;
            }
        } else {                                        /* over-revving */
            c->rpm -= (c->rpm - M) * 2.0f * dt;
            if(c->rpm < M * c->throttle) c->rpm = M * c->throttle;
        }

        float before = c->speed;
        c->speed = c->launch ? c->speed + dt * 1000.0f : c->rpm / (c->k_rpm * ratio);
        if(c->speed > top) c->speed = top;
        /* Slowing down faster than the tyres can manage locks the wheels. */
        if(c->grip_decel * dt < before - c->speed) c->lock = 1.0f;
    }

    /* Stopped: back to neutral. The automatic box revs up there and takes first again past
       8500 rpm, which is the launch. */
    if(c->speed < dt * 500.0f) c->gear = 0;
}

/* ------------------------------------------------------- 0x16094 / 0x16140 */

/* Throttle and brake (original: STARTL.EXE 0x16094 and 0x16140). One call takes the pedals from
   the input and, when the car is not drifting, slows it by the brake. It changes throttle,
   brake, speed, and lock or spin when the brake is too hard for the tyres. */
static void pedals(struct car_state *c, const struct drive_input *in)
{
    /* No drive while sliding, locked or spinning. */
    if(c->throttle >= 0.0f && c->slide_want == 0.0f && c->lock == 0.0f && !c->spin)
        c->throttle = in->throttle;
    else
        c->throttle = 0.0f;

    /* A joystick axis is the brake itself (the word / 1023, original: STARTL.EXE 0x16174).
       A key brings the brake up to full by easing. */
    if(in->brake <= 0.0f) c->brake = 0.0f;
    else if(in->brake_axis) c->brake = in->brake;
    else approach(&c->brake, 1.0f, 0x52);

    if(fabsf(c->drift) < 0.1f) {
        c->speed -= c->k_brake * DRIVE_DT * c->brake;
        if(c->speed < 0.0f) c->speed = 0.0f;
        if(c->grip_lock < c->brake * c->speed && !c->spin) {
            /* Braking that hard with a front wheel on the grass spins the car
               (original: STARTL.EXE 0x16259). On tarmac the wheels lock. */
            if(c->rough) c->spin = 1;
            else         c->lock = c->brake;
        }
    }
}

/* ------------------------------------------------------------------ 0x1628c */

/* Steering (original: STARTL.EXE 0x1628c). One call sets the front-wheel angle from the input,
   limited by the speed, and turns the heading by it: it changes steer_max, steer, radius, turn
   and yaw. */
static void steering(struct car_state *c, const struct drive_input *in)
{
    int stiff = c->launch ? 0x58
              : (c->lock == 0.0f && c->slide_want == 0.0f && !c->spin) ? 0x50 : 0x55;

    /* Steering lock: 18 degrees at a standstill, falling to 1 degree at top speed. */
    c->steer_max = (1.0f - sqrt0(c->speed / c->spec.maxspd)) * 0.296706f + 0.0174533f;

    /* A stick's wheel is where the stick is (original: STARTL.EXE 0x163bb), unless the car is
       sliding or leaving the line. Then it is eased like the keys' wheel. */
    if(in->steer_axis && stiff == 0x50) c->steer = in->steer * c->steer_max;
    else approach(&c->steer, in->steer * c->steer_max, stiff);
    if(fabsf(c->steer) < 1e-5f) c->steer = 0.0f;
    /* On the grass the wheel is tugged about (original: STARTL.EXE 0x16458). The tug is less
       the faster the car goes, goes one way or the other from one spot to the next, and is held
       to the lock. */
    if(c->rough) {
        int spot = (int)c->x + (int)c->y;
        float tug = (float)(spot & 0x3f) * 0.0008f * ((spot & 1) ? -1.0f : 1.0f);
        c->steer += sqrt0(1.0f - c->speed / c->spec.maxspd) * tug;
        if(fabsf(c->steer) > c->steer_max) c->steer = c->steer < 0.0f ? -c->steer_max : c->steer_max;
    }

    if(c->steer != 0.0f) {
        /* The car turns about a point at the distance wheelbase / tan(steer) to its side. */
        c->radius = c->wheelbase / tanf(c->steer);
        c->turn   = DRIVE_DT * c->speed / c->radius;
        c->yaw   += c->turn;
    } else {
        c->radius = 0.0f;
        c->turn   = 0.0f;
    }
}

/* ------------------------------------------------------- 0x17d04 / 0x188a8 */

/* Skid and spin (original: STARTL.EXE 0x17d04 and 0x188a8). One call compares the cornering
   force with the grip. Below the limit the car grips. Above it the body swings out and the car
   runs wide (slip, slide), and far above it the car spins. Locked wheels slide straight on.
   It changes lock, slip, slide_want, slide, drift, spin, skid_lock, yaw and the slide direction. */
static void skid(struct car_state *c)
{
    const float dt = DRIVE_DT;

    c->skid_lock = 0;
    if(!c->spin) {
        if(c->lock == 0.0f) {
            /* Cornering force: speed squared times how fast the heading turns, and more for
               every wheel on the grass, 1000 a front wheel and 600 a rear one
               (original: STARTL.EXE 0x17e5c). */
            float f = c->speed * c->speed * c->turn * 0.0001f / dt * c->surface
                    + (float)((c->grass[0] + c->grass[2]) * 1000 + (c->grass[1] + c->grass[3]) * 600)
                      * (c->turn < 0.0f ? -1.0f : 1.0f);
            if(fabsf(f) <= c->grip_slide) {
                c->slide_want = 0.0f;
                c->slip = 0.0f;
            } else if((c->brake + 0.5f) * f <= c->grip_spin) {
                /* Past the limit the body swings out (slip) and the car runs wide at
                   surface * 0.2 * speed^2 / radius. The game is lop-sided here: f and the
                   radius carry the turn's sign, and only a growing heading runs wide or
                   spins. OpenScreamer follows the game, and so does this code. */
                f += (f <= 0.0f) ? c->grip_slide : -c->grip_slide;
                c->slip = f * 0.00015f;
                if(fabsf(c->slip) > 1.53938f)
                    c->slip = (f < 0.0f ? -1.0f : 1.0f) * 1.53938f;
                c->slide_want = c->radius != 0.0f
                              ? c->surface * 0.2f * (c->speed * c->speed / c->radius) : 0.0f;
                c->slide_dx = sinf(c->yaw - HALF_PI);
                c->slide_dy = -cosf(c->yaw - HALF_PI);
            } else {
                c->spin = 1;                            /* too much, especially braking */
            }
        } else {
            /* Wheels locked: the car slides straight on until the speed comes down. */
            c->skid_lock = 1;
            c->lock *= damp(98);
            if(c->lock * c->speed < 2000.0f) c->lock = 0.0f;
            approach(&c->slip, 0.0f, 7);
            c->yaw += c->slip * 0.2f;
        }
        approach_angle(&c->drift, sqrt0(fabsf(drive_cos(c->roll)) * 2.0f) * c->slip, 0x5a);
    }
    approach(&c->slide, c->slide_want, 0x50);
    if(c->slide < 10.0f) c->slide = 0.0f;

    /* Spin recovery (original: STARTL.EXE 0x188a8): a spin throws the tail round once and then
       settles. */
    if(c->spin == 1) {
        float dir = (c->turn < 0.0f) ? 1.0f : -1.0f;
        c->spin = dir > 0.0f ? 4 : 2;
        c->drift += dir * TWO_PI;
    } else if(c->spin > 1) {
        float dir = (float)(c->spin - 3);
        float rate = (fabsf(c->drift) + 0.2f) / PI * 3.0f * dt;
        int done = 0;
        if(dir > 0.0f) { c->drift -= rate; if(c->drift < 0.0f) done = 1; }
        if(dir < 0.0f) { c->drift += rate; if(c->drift > 0.0f) done = 1; }
        if(done) { c->drift = 0.0f; c->spin = 0; }
    }
}

/* ------------------------------------------------------------------ 0x16f24 */

/* Moves the car (original: STARTL.EXE 0x16f24): forward along the heading, then by the shove
   from a wall and by the sideways run of a skid. It also turns the heading by the wall's yaw
   rate and wraps it into 0 .. 2 pi. It changes x, y, yaw, yaw_rate and shove. */
static void integrate(struct car_state *c)
{
    const float dt = DRIVE_DT;
    float cp = drive_cos(c->pitch);

    /* Forward. */
    c->x += sinf(c->yaw) * cp * c->speed * dt;
    c->y -= cosf(c->yaw) * cp * c->speed * dt;

    /* Shoved off a wall. */
    c->x += c->shove_x * c->shove * dt;
    c->y += c->shove_y * c->shove * dt;
    c->yaw_rate *= damp(80);
    c->yaw += c->yaw_rate * dt;
    c->shove *= damp(91);
    if(fabsf(c->shove) < 250.0f) c->shove = 0.0f;

    /* Running wide in a skid (original: STARTL.EXE 0x172d8): along (sin, -cos) of the heading
       - 90 degrees, which is the direction set when the slide starts
       (original: STARTL.EXE 0x17f4d). */
    float k = sqrt0(fabsf(drive_cos(c->roll)));
    c->x += k * dt * c->slide * c->slide_dx;
    c->y += k * dt * c->slide * c->slide_dy;

    while(c->yaw < 0.0f)        c->yaw += TWO_PI;
    while(c->yaw > TWO_PI)  c->yaw -= TWO_PI;
}

/* The wall response (original: STARTL.EXE 0x16c90): glancing a wall scrubs speed by how
   squarely it was hit. side is (front-right corner out) - (front-left corner out); the game
   turns the nose with exactly that. The shove goes along (push_x, push_y). A push of almost
   no length is ignored. */
void drive_hit_wall(struct car_state *c, float push_x, float push_y, float side, float wall_angle)
{
    float len = sqrtf(push_x * push_x + push_y * push_y);
    if(len < 1e-3f) return;
    /* How squarely the wall was hit: |sin(heading + wall angle)| (original: STARTL.EXE 0x16cec). */
    float square = fabsf(sinf(c->yaw + wall_angle));
    c->shove_x = push_x / len;
    c->shove_y = push_y / len;
    /* The shove speed is square^4 * speed * 2 + 2000, at most 5000 (original: STARTL.EXE 0x16df4). */
    c->shove = square * square * square * square * c->speed * 2.0f + 2000.0f;
    if(c->shove > 5000.0f) c->shove = 5000.0f;
    c->speed -= c->speed * 0.9f * square;
    if(c->speed < 0.0f) c->speed = 0.0f;
    c->yaw_rate = side * 5.0f * square;                 /* turns the nose along the wall */
    c->lock = 0.0f;
    c->slip = 0.0f;
    c->slide_want = 0.0f;
    if(!c->spin) c->drift = 0.0f;
}

/* One physics step of 1/50 s: the car moves first, then engine, pedals, steering and skid
   work out the state for the next step. */
void drive_step(struct car_state *c, const struct drive_input *in)
{
    integrate(c);
    engine(c, in);
    pedals(c, in);
    steering(c, in);
    skid(c);
}
