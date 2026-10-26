/*
 * The player's car (drive.h), ported from OpenScreamer's port/src/drive.c (GPL-3.0), which
 * ported it from STARTL.EXE. Every frame the game runs on the player record at 0xf918:
 *
 *   0x16f24  integrate: move the car, apply wall shove, wrap the angles
 *   0x15a70  -> 0x18da0 ENGINE + GEARBOX, 0x16094 THROTTLE, 0x16140 BRAKE, 0x1628c STEERING
 *   0x15b9c  -> 0x17d04 SKID / SPIN, 0x188a8 spin recovery (and suspension, sound, crash,
 *               smoke, not here)
 *   0x15aa8  -> lap markers, body-corner collision, wheel contact, 0x16c90 WALL HIT, ride
 *               height (car.c, apart from the wall hit below)
 *
 * The routines in capitals are here as OpenScreamer has them, line for line; only the
 * Pocket's keys are left: the stick's steering and the trigger's brake paths are out.
 *
 * THE DAMPING TABLE. The game smooths values with v = (v - target) * damp[i] + target, where
 * damp[i] = pow(0.5 + i * 0.005, 50 / (fps + 1)): a stiffness from 0.5 (snappy) to 0.995
 * (lazy), defined at 50 frames a second. The steps here are exactly 50 a second, and the
 * table is worked out once (pow is slow on the CPU).
 */
#include "drive.h"
#include <math.h>
#include <string.h>

#define FPS 50.0f

static float damp_tab[100];
static int damp_ready;

static float damp(int i)
{
    if(!damp_ready) {
        for(int k = 0; k < 100; k++) damp_tab[k] = powf(0.5f + (float)k * 0.005f, 50.0f / (FPS + 1.0f));
        damp_ready = 1;
    }
    return damp_tab[i];
}

float drive_damp(int i) { return damp(i); }

static void approach(float *v, float target, int i)        /* 0x22b60 */
{
    *v = (*v - target) * damp(i) + target;
}

/* 0x220fc: the same for an angle -- near the target it eases through a sine */
static void approach_angle(float *v, float target, int i)
{
    if(fabsf(target - *v) < 1.5707963f)
        *v = sinf(*v - target) * damp(i) + target;
    else
        *v = (*v - target) * damp(i) + target;
}

static float sqrt0(float x) { return x < 0.0f ? 0.0f : sqrtf(x); }     /* 0x22bcc */

/* per-gear pull, the table at 0x1d84 in the data segment */
static const float GEAR_PULL[8] = { 0.0f, 5.0f, 3.5f, 2.0f, 0.95f, 0.78f, 0.72f, 0.0f };

/* ------------------------------------------------------------------ set-up */

/* the tail of 0x136c8: turn the table row into the constants the frame uses */
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

static void engine(struct car_state *c, const struct drive_input *in)
{
    const struct car_spec *s = &c->spec;
    const float M = s->maxrpm, dt = DRIVE_DT;

    /* until the green light none of the gearbox or the drive is run: the engine follows the
       pedal, as in neutral */
    if(c->waiting) {
        approach(&c->rpm, c->throttle * M, 0x4b);
        if(c->speed < dt * 500.0f) c->gear = 0;
        return;
    }

    if(!s->automatic) {
        if(in->shift > 0 && c->gear < s->nshift) c->gear++;
        if(in->shift < 0 && c->gear > 0)         c->gear--;
    } else {
        /* change up past 8500 rpm, down below 4000 (each nudged by the gear) */
        if(c->rpm > (float)(c->gear + 8500)) {
            if(c->gear < s->nshift) c->gear++;
        } else if(c->rpm < (float)(4000 - c->gear) && c->gear > 1) {
            c->gear--;
        }
        if(c->launch) c->gear = 1;
    }

    if(c->gear == 0) {
        /* neutral: the car coasts down and the engine follows the pedal */
        c->speed -= dt * 500.0f;
        if(c->speed < 0.0f) c->speed = 0.0f;
        approach(&c->rpm, c->throttle * M, 0x4b);
    } else {
        float ratio = s->ratio[c->gear];
        float top = s->maxspd;                          /* +0x510, the speed cap */

        /* normally the engine is locked to the wheels; pulling away with high revs and
           almost no speed the wheels spin (state 5): the revs fall and the car gains a flat
           1000 units/s each second until the two meet */
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

        /* how hard this gear pulls: strongest in the middle of its range */
        float vgear = M / (c->k_rpm * ratio);           /* speed at max rpm */
        float pull, curve;
        if(s->nshift == 1) {
            pull  = s->accel;
            curve = sqrt0(1.0f - c->speed / vgear);     /* only the one-gear "Raz car" */
        } else {
            float a = (2.0f - (vgear - c->speed) / (vgear * 0.5f)) - 1.0f;
            if(c->gear == 1 && a < 0.0f) a = 0.2f;
            curve = (1.0f + sinf(a * 1.5707964f)) * 0.5f;
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
        /* slowing faster than the tyres can manage locks them */
        if(c->grip_decel * dt < before - c->speed) c->lock = 1.0f;
    }

    /* stopped: back to neutral (the automatic box revs up there and takes first again past
       8500 rpm, which is the launch) */
    if(c->speed < dt * 500.0f) c->gear = 0;
}

/* ------------------------------------------------------- 0x16094 / 0x16140 */

static void pedals(struct car_state *c, const struct drive_input *in)
{
    /* no drive while sliding, locked or spinning */
    if(c->throttle >= 0.0f && c->slide_want == 0.0f && c->lock == 0.0f && !c->spin)
        c->throttle = in->throttle;
    else
        c->throttle = 0.0f;

    /* 0x16174: a key brings the brake up to full */
    if(in->brake <= 0.0f) c->brake = 0.0f;
    else approach(&c->brake, 1.0f, 0x52);

    if(fabsf(c->drift) < 0.1f) {
        c->speed -= c->k_brake * DRIVE_DT * c->brake;
        if(c->speed < 0.0f) c->speed = 0.0f;
        if(c->grip_lock < c->brake * c->speed && !c->spin) {
            /* that hard with a front wheel on the grass spins the car (0x16259) */
            if(c->rough) c->spin = 1;
            else         c->lock = c->brake;
        }
    }
}

/* ------------------------------------------------------------------ 0x1628c */

static void steering(struct car_state *c, const struct drive_input *in)
{
    int stiff = c->launch ? 0x58
              : (c->lock == 0.0f && c->slide_want == 0.0f && !c->spin) ? 0x50 : 0x55;

    /* 18 degrees of lock at a standstill, falling to 1 degree at top speed */
    c->steer_max = (1.0f - sqrt0(c->speed / c->spec.maxspd)) * 0.296706f + 0.0174533f;

    approach(&c->steer, in->steer * c->steer_max, stiff);
    if(fabsf(c->steer) < 1e-5f) c->steer = 0.0f;
    /* 0x16458: on the grass the wheel is tugged about, less the faster the car goes, one way
       or the other from one spot to the next, and held to the lock */
    if(c->rough) {
        int spot = (int)c->x + (int)c->y;
        float tug = (float)(spot & 0x3f) * 0.0008f * ((spot & 1) ? -1.0f : 1.0f);
        c->steer += sqrt0(1.0f - c->speed / c->spec.maxspd) * tug;
        if(fabsf(c->steer) > c->steer_max) c->steer = c->steer < 0.0f ? -c->steer_max : c->steer_max;
    }

    if(c->steer != 0.0f) {
        /* the car turns about a point one wheelbase / tan(lock) to its side */
        c->radius = c->wheelbase / tanf(c->steer);
        c->turn   = DRIVE_DT * c->speed / c->radius;
        c->yaw   += c->turn;
    } else {
        c->radius = 0.0f;
        c->turn   = 0.0f;
    }
}

/* ------------------------------------------------------- 0x17d04 / 0x188a8 */

static void skid(struct car_state *c)
{
    const float dt = DRIVE_DT;

    if(!c->spin) {
        if(c->lock == 0.0f) {
            /* cornering force: speed squared times how fast the heading turns, and more for
               every wheel on the grass, 1000 a front wheel, 600 a rear one (0x17e5c) */
            float f = c->speed * c->speed * c->turn * 0.0001f / dt * c->surface
                    + (float)((c->grass[0] + c->grass[2]) * 1000 + (c->grass[1] + c->grass[3]) * 600)
                      * (c->turn < 0.0f ? -1.0f : 1.0f);
            if(fabsf(f) <= c->grip_slide) {
                c->slide_want = 0.0f;
                c->slip = 0.0f;
            } else if((c->brake + 0.5f) * f <= c->grip_spin) {
                /* past the limit: the body swings out (slip) and the car runs wide at
                   surface * 0.2 * speed^2 / radius. The game is lop-sided here (f and the
                   radius carry the turn's sign, and only a growing heading runs wide or
                   spins); OpenScreamer follows it, and so does this. */
                f += (f <= 0.0f) ? c->grip_slide : -c->grip_slide;
                c->slip = f * 0.00015f;
                if(fabsf(c->slip) > 1.53938f)
                    c->slip = (f < 0.0f ? -1.0f : 1.0f) * 1.53938f;
                c->slide_want = c->radius != 0.0f
                              ? c->surface * 0.2f * (c->speed * c->speed / c->radius) : 0.0f;
                c->slide_dx = sinf(c->yaw - 1.5707964f);
                c->slide_dy = -cosf(c->yaw - 1.5707964f);
            } else {
                c->spin = 1;                            /* too much, especially braking */
            }
        } else {
            /* wheels locked: slide straight on until the speed comes down */
            c->lock *= damp(98);
            if(c->lock * c->speed < 2000.0f) c->lock = 0.0f;
            approach(&c->slip, 0.0f, 7);
            c->yaw += c->slip * 0.2f;
        }
        approach_angle(&c->drift, sqrt0(fabsf(cosf(c->roll)) * 2.0f) * c->slip, 0x5a);
    }
    approach(&c->slide, c->slide_want, 0x50);
    if(c->slide < 10.0f) c->slide = 0.0f;

    /* 0x188a8: a spin throws the tail round once and then settles */
    if(c->spin == 1) {
        float dir = (c->turn < 0.0f) ? 1.0f : -1.0f;
        c->spin = dir > 0.0f ? 4 : 2;
        c->drift += dir * 6.2831855f;
    } else if(c->spin > 1) {
        float dir = (float)(c->spin - 3);
        float rate = (fabsf(c->drift) + 0.2f) / 3.1415927f * 3.0f * dt;
        int done = 0;
        if(dir > 0.0f) { c->drift -= rate; if(c->drift < 0.0f) done = 1; }
        if(dir < 0.0f) { c->drift += rate; if(c->drift > 0.0f) done = 1; }
        if(done) { c->drift = 0.0f; c->spin = 0; }
    }
}

/* ------------------------------------------------------------------ 0x16f24 */

static void integrate(struct car_state *c)
{
    const float dt = DRIVE_DT;
    float cp = cosf(c->pitch);

    /* forward */
    c->x += sinf(c->yaw) * cp * c->speed * dt;
    c->y -= cosf(c->yaw) * cp * c->speed * dt;

    /* shoved off a wall */
    c->x += c->shove_x * c->shove * dt;
    c->y += c->shove_y * c->shove * dt;
    c->yaw_rate *= damp(80);
    c->yaw += c->yaw_rate * dt;
    c->shove *= damp(91);
    if(fabsf(c->shove) < 250.0f) c->shove = 0.0f;

    /* running wide in a skid (0x172d8): along (sin, -cos) of heading - 90 degrees, the
       direction set when the slide starts (0x17f4d) */
    float k = sqrt0(fabsf(cosf(c->roll)));
    c->x += k * dt * c->slide * c->slide_dx;
    c->y += k * dt * c->slide * c->slide_dy;

    while(c->yaw < 0.0f)        c->yaw += 6.2831855f;
    while(c->yaw > 6.2831855f)  c->yaw -= 6.2831855f;
}

/* 0x16c90: glancing a wall scrubs speed by how squarely it was hit. side is (front-right
   corner out) - (front-left corner out): the game turns the nose with exactly that */
void drive_hit_wall(struct car_state *c, float push_x, float push_y, float side, float wall_angle)
{
    float len = sqrtf(push_x * push_x + push_y * push_y);
    if(len < 1e-3f) return;
    /* how squarely the wall was hit: |sin(heading + wall angle)| (0x16cec) */
    float square = fabsf(sinf(c->yaw + wall_angle));
    c->shove_x = push_x / len;
    c->shove_y = push_y / len;
    /* 0x16df4: square^4 * speed * 2 + 2000, at most 5000 */
    c->shove = square * square * square * square * c->speed * 2.0f + 2000.0f;
    if(c->shove > 5000.0f) c->shove = 5000.0f;
    c->speed -= c->speed * 0.9f * square;
    if(c->speed < 0.0f) c->speed = 0.0f;
    c->yaw_rate = side * 5.0f * square;                 /* the nose is turned along the wall */
    c->lock = 0.0f;
    c->slip = 0.0f;
    c->slide_want = 0.0f;
    if(!c->spin) c->drift = 0.0f;
}

void drive_step(struct car_state *c, const struct drive_input *in)
{
    integrate(c);
    engine(c, in);
    pedals(c, in);
    steering(c, in);
    skid(c);
}
