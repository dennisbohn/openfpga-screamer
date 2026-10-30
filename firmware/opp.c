/*
 * The opponents (opp.h), after OpenScreamer's port/src/main.c (GPL-3.0: race_restart,
 * opp_assign_cars, opponents_step, opp_place, opp_markers), from the game: 0x1ced0 the speed,
 * 0x1d2fc the way along the curve, 0x1d54c + 0x1e7a4 the curve, 0x1cb9c standing on the road,
 * 0x1df24 the turn and lean in bends, 0x1d6ac drifting back to the line, 0x1db84 the markers,
 * 0x20418 the running order, 0x142e8 the cars; contact between cars: 0x12378 / 0x17818 the
 * player and an opponent, 0x126e4 / 0x1cffc / 0x1d1f0 two opponents, 0x1e404 overtaking; an
 * opponent's crash (0x1cffc, 0x1d824) and the road under it (0x1cb9c); its shadow (0x1c83c,
 * 0x1cd34).
 *
 * The curve (0x1d54c + 0x1e7a4) is a uniform quadratic B-spline over the nodes' handles, two
 * pieces a node: piece 0 node.in, node.out, next.in; piece 1 node.out, next.in, next.out;
 * point(u) = A (1 - u)^2 / 2 + C u^2 / 2 + B (the rest). The node's own point is not on it,
 * it is only the place on the grid.
 */
#include "opp.h"
#include <math.h>
#include "car.h"
#include "ghost.h"
#include "efx.h"
#include "hw.h"
#include "sound.h"

#define PI          3.14159265f

struct opp {
    int node, half;             /* the piece of its curve it is on */
    float u, speed;             /* 0..1 along that piece; units/s */
    float offx, offy, off_age;  /* pushed aside by a collision; drifts back after 3 s */
    float shove, shove_x, shove_y;  /* still being pushed (+0x450, +0x460/468) */
    float prev_speed;
    /* a crash (+0x2f8): 1 thrown up and tumbling, 4 spinning round twice */
    int crash, bounces;
    float vy, up, tip, turn, lean, tip_rate, turn_rate, lean_rate;
    float nose, tail, radius;   /* its two circles: ahead of and behind its origin, their radius */
    float spin, body, dip, prev_yaw;    /* its nose turned into a bend, its body leant and
                                           pitched (0x1df24) */
    float x, y, z, yaw, pitch, roll;
    float sh_pitch, sh_roll;    /* its shadow's tilt: the car object's, kept while it crashes */
    float wheel;                /* the wheels' turn */
    int mark_at, mk_count, laps;
    uint8_t mk_seen[64];
    float progress;
    int rank, model;
    uint32_t livery;
};
static struct opp opp[OPP_MAX];
static struct track_refl refl[OPP_MAX];   /* each car object's reflection state (record +0x24 ..) */
static int nopp, player_rank;
int opp_bullet;
static const struct track_spline_node *spl[OPP_MAX];
static int spl_n[OPP_MAX];
static const struct track_opp_row *rows;
static int engine[OPP_MAX];             /* the engines' voices (sound.h) and their distance from */
static float engine_last[OPP_MAX];      /* the ear a step before */
static int touch;                       /* the player touched one in this step's contact */
static float hitcar;                    /* the player's +0x7c4 (opp_hitcar) */

static float wrap_pi(float a)
{
    while(a > PI) a -= 2.0f * PI;
    while(a < -PI) a += 2.0f * PI;
    return a;
}

/* the three control points of the piece opponent k is on */
static void piece(int k, int node, int half, float a[2], float b[2], float c[2])
{
    const struct track_spline_node *n = &spl[k][node];
    int nx = n->next >= 0 && n->next < spl_n[k] ? n->next : 0;
    const struct track_spline_node *m = &spl[k][nx];
    if(half == 0) {
        a[0] = (float)n->in_x; a[1] = (float)n->in_y;
        b[0] = (float)n->out_x; b[1] = (float)n->out_y;
        c[0] = (float)m->in_x; c[1] = (float)m->in_y;
    } else {
        a[0] = (float)n->out_x; a[1] = (float)n->out_y;
        b[0] = (float)m->in_x; b[1] = (float)m->in_y;
        c[0] = (float)m->out_x; c[1] = (float)m->out_y;
    }
}

/* point and direction of travel on a piece */
static void curve(const float a[2], const float b[2], const float c[2], float u, float pos[2], float der[2])
{
    float w0 = (1.0f - u) * (1.0f - u) * 0.5f, w2 = u * u * 0.5f, w1 = 1.0f - w0 - w2;
    for(int j = 0; j < 2; j++) {
        pos[j] = a[j] * w0 + b[j] * w1 + c[j] * w2;
        der[j] = (1.0f - u) * (b[j] - a[j]) + u * (c[j] - b[j]);
    }
}

/* where it is: the curve's point and a collision's offset, heading along the curve; standing
   on ONE point, the drivable surface under its middle, tilted to that face's slope (0x1cb9c:
   no smoothing, no suspension). Pushed off the drivable area at more than 3000 units/s it is
   stopped dead (0x1cb9c), and an opponent that lost more than 5000 units/s since the step
   before crashes (0x1cffc, 0x1d824): thrown up and tumbling like the player, or, by where it
   is, spinning twice round on the road */
static void place(struct opp *o, int k)
{
    float a[2], b[2], c[2], pos[2], der[2];
    piece(k, o->node, o->half, a, b, c);
    curve(a, b, c, o->u, pos, der);
    o->x = pos[0] + o->offx;
    o->y = pos[1] + o->offy;
    o->yaw = atan2f(der[0], -der[1]);                   /* forward = (sin yaw, -cos yaw) */
    float h;
    int on = track_under(o->x, o->y, &h, 0);
    if(on) {
        o->z = h;
        track_under_tilt(o->yaw, &o->pitch, &o->roll);
    }
    /* (a face found with its height is found without: only a miss asks again) */
    if(o->speed > 3000.0f && !on && !track_under(o->x, o->y, 0, 0)) o->speed = 0.0f;
    if(!o->crash && o->prev_speed - o->speed > 5000.0f) {
        if((((int)o->x + (int)o->y) >> 2) & 1) {
            o->crash = 4;
            o->turn = 0.0f;
            o->turn_rate = 24.0f;
        } else {
            o->crash = 1;
            o->bounces = 0;
            o->vy = 3000.0f;
            o->up = o->tip = o->turn = o->lean = 0.0f;
            o->tip_rate = o->turn_rate = 12.0f;
            o->lean_rate = 0.0f;
        }
    }
    o->prev_speed = o->speed;
    if(o->crash == 1) {
        float d = drive_damp(97);
        o->up += o->vy * DRIVE_DT;
        o->vy -= 6000.0f * DRIVE_DT;
        o->tip_rate *= d;
        o->tip += o->tip_rate * DRIVE_DT;
        o->turn_rate *= d;
        o->turn += o->turn_rate * DRIVE_DT;
        o->lean_rate *= d;
        o->lean += o->lean_rate * DRIVE_DT;
        if(o->up <= 0.0f) {                             /* a landing: up again, less, four times */
            o->bounces++;
            o->up = 0.0f;
            o->vy = -o->vy * 0.4f;
            o->tip = o->turn = o->lean = 0.0f;
            o->lean_rate = (o->bounces & 1) ? -0.6f : 0.6f;
            o->tip_rate = -1.0f;
            o->turn_rate = 0.0f;
            if(o->bounces > 3) {
                o->crash = 0;
                o->tip = o->lean = 0.0f;
            }
        }
    } else if(o->crash == 4) {
        o->turn_rate *= drive_damp(97);
        o->turn += o->turn_rate * DRIVE_DT;
        if(o->turn >= 12.566371f || o->turn_rate < 0.05f) {
            o->turn = 0.0f;
            o->crash = 0;
        }
    }
    /* its shadow's tilt (0x1cb14): the car object's unless it crashes (+0x2f8) */
    if(!o->crash) {
        o->sh_pitch = -o->pitch;
        o->sh_roll = -o->roll;
    }
}

/* its two circles (0x142e8, as the player's), and its forward (sin yaw, -cos yaw) in f */
static void circles(const struct opp *o, float cx[2], float cy[2], float f[2])
{
    float fx = sinf(o->yaw), fy = -cosf(o->yaw);
    f[0] = fx;
    f[1] = fy;
    cx[0] = o->x + fx * o->nose; cy[0] = o->y + fy * o->nose;
    cx[1] = o->x + fx * o->tail; cy[1] = o->y + fy * o->tail;
}

/* The player touches opponent o (0x17818), (nx, ny) from o's circle to the player's, pen the
   overlap: o is moved out of the way, unless that puts it off the track: then the player is.
   O in front (the player ran into it): shoved on at half the player's speed, knocked round by
   speed / 10000 rad (at most a quarter turn), the player shoved back (car_rammed). O behind
   (it ran into the player): 70 % of its speed. The crash sample opens it, started afresh on
   every frame of contact in the game, here at most 30 times a second. */
static void player_hits(struct opp *o, float nx, float ny, float pen)
{
    static float snd_at = -1.0f;
    const struct car_state *c = car_state();
    float px = nx * pen, py = ny * pen, now = car_race()->clock;
    touch = 1;
    if(now < snd_at || now - snd_at >= 1.0f / 30.0f) {
        snd_play(SND_CRASH);
        snd_at = now;
    }
    if(!track_under(o->x - px, o->y - py, 0, 0)) {
        car_shift(px, py);
        return;
    }
    float fx = sinf(c->yaw), fy = -cosf(c->yaw);
    if((o->x - c->x) * fx + (o->y - c->y) * fy >= 0.0f) {
        float ofx = sinf(o->yaw), ofy = -cosf(o->yaw), orx = -cosf(o->yaw), ory = -sinf(o->yaw);
        float rx = c->x - o->x, ry = c->y - o->y;
        int left = rx * orx + ry * ory < 0.0f, behind = rx * ofx + ry * ofy < 0.0f;
        o->shove = c->speed * 0.5f;
        float turn = c->speed * 0.0001f;
        if(turn > 1.5707964f) turn = 1.5707964f;
        o->spin = (behind ? 1.0f : -1.0f) * (left ? 1.0f : -1.0f) * turn;
        car_rammed(nx, ny, left);
    } else
        o->speed *= 0.7f;
    o->shove_x = -nx;
    o->shove_y = -ny;
    o->offx -= px;
    o->offy -= py;
    o->off_age = 0.0f;
}

/* car against car, after every car's step: the player and each opponent within 1500 (and 600
   in height; 0x12378), overtaking (0x1e404: an opponent looks three radii ahead; if the slower
   player is there, it moves its line over by just enough to clear), and two opponents within
   1500 (0x1cffc, 0x1d1f0: the faster drops to their average speed, the later one is moved) */
static void collide(void)
{
    const struct car_state *c = car_state();
    float pcx[2], pcy[2], pr = car_circles(pcx, pcy);
    hitcar = (hitcar - (float)touch) * drive_damp(0x32) + (float)touch;     /* 0x175aa, 0x177f7 */
    touch = 0;
    /* each one's circles and forward, once: nothing here moves an opponent or turns it */
    float occ[OPP_MAX][2][2], of[OPP_MAX][2];
    for(int k = 0; k < nopp; k++) circles(&opp[k], occ[k][0], occ[k][1], of[k]);
    for(int k = 0; k < nopp; k++) {
        struct opp *o = &opp[k];
        const float *ocx = occ[k][0], *ocy = occ[k][1];
        if(fabsf(c->x - o->x) < 1500.0f && fabsf(c->y - o->y) < 1500.0f && fabsf(c->z - o->z) < 600.0f)
            for(int a = 0; a < 2; a++)
                for(int b = 0; b < 2; b++) {
                    float dx = pcx[a] - ocx[b], dy = pcy[a] - ocy[b];
                    float dist = sqrtf(dx * dx + dy * dy);
                    if(dist < 1.0f) dist = 1.0f;
                    if(dist < pr + o->radius) {
                        player_hits(o, dx / dist, dy / dist, pr + o->radius - dist);
                        pr = car_circles(pcx, pcy);
                        break;
                    }
                }
        float fx = of[k][0], fy = of[k][1];
        float qx = o->x + fx * o->radius * 3.0f, qy = o->y + fy * o->radius * 3.0f;
        if(fabsf(qx - c->x) < 5000.0f && fabsf(qy - c->y) < 5000.0f && c->speed < o->speed &&
           fabsf(c->z - o->z) < 600.0f) {
            float thr = o->radius * 2.0f + pr;
            for(int a = 0; a < 2; a++) {
                float dx = qx - pcx[a], dy = qy - pcy[a];
                float dist = sqrtf(dx * dx + dy * dy);
                if(dist < thr) {
                    dist += 1.0f;
                    o->offx += thr * dx / dist - dx;
                    o->offy += thr * dy / dist - dy;
                    o->off_age = 0.0f;
                }
            }
        }
        for(int j = k + 1; j < nopp; j++) {
            struct opp *q = &opp[j];
            if(fabsf(o->x - q->x) >= 1500.0f || fabsf(o->y - q->y) >= 1500.0f) continue;
            const float *qcx = occ[j][0], *qcy = occ[j][1];
            for(int a = 0; a < 2; a++)
                for(int b = 0; b < 2; b++) {
                    float dx = ocx[a] - qcx[b], dy = ocy[a] - qcy[b];
                    float dist = sqrtf(dx * dx + dy * dy);
                    if(dist < 1.0f) dist = 1.0f;
                    if(dist >= o->radius + q->radius) continue;
                    float avg = (o->speed + q->speed) * 0.5f;
                    if(q->speed <= o->speed) o->speed = avg;
                    else q->speed = avg;
                    float pen = o->radius + q->radius - dist;
                    q->offx -= dx / dist * pen;
                    q->offy -= dy / dist * pen;
                    q->off_age = 0.0f;
                }
        }
    }
}

/* the lap's markers (0x1db84): like the player it looks only at the marker it last reached and
   that one's two neighbours; a marker counts when the one before it has been passed and it has
   not; the line with every marker passed is a lap */
static void markers(struct opp *o)
{
    int n;
    const struct track_marker *mk = track_markers(&n);
    if(n == 0) return;
    for(int d = -1; d <= 1; d++) {
        int i = ((o->mark_at + d) % n + n) % n;
        const struct track_marker *m = &mk[i];
        float dx = o->x - (float)m->x, dy = o->y - (float)m->y, rad = (float)m->radius;
        if(fabsf(dx) >= rad || fabsf(dy) >= rad) continue;
        if(sqrtf(dx * dx + dy * dy) > rad) continue;
        o->mark_at = i;
        if(i >= 64) continue;
        if((i != 0 && !o->mk_seen[i - 1]) || o->mk_seen[i]) {
            if(i == 0 && o->mk_count == n) {
                for(int j = 1; j < 64; j++) o->mk_seen[j] = 0;
                o->mk_count = 1;
                o->laps++;
            }
        } else {
            o->mk_seen[i] = 1;
            o->mk_count++;
        }
    }
    o->progress = car_marker_progress(o->mark_at, o->laps, o->x, o->y);
}

void opp_init(int player_row)
{
    int nrows, ns = track_opp_count(&nrows), ncars, nm;
    const struct car_spec *cars = track_cars(&ncars);
    const struct track_model *models = track_models(&nm);
    rows = track_opp_rows();
    nopp = ns - 1;                                      /* the last curve is the player's */
    if(nopp > nrows) nopp = nrows;
    if(nopp > OPP_MAX) nopp = OPP_MAX;
    if(nopp < 0 || (car_mode & CAR_ALONE)) nopp = 0;    /* the special modes: alone (0x1311c, ...) */
    int row = 0;
    for(int k = 0; k < nopp; k++) {
        struct opp *o = &opp[k];
        int start;
        refl[k] = (struct track_refl){ { 0, 0, 0 }, { 0.0f, 0.0f, 0.0f } };
        spl[k] = track_opp_spline(k, &start, &spl_n[k]);
        *o = (struct opp){ 0 };
        o->node = start >= 0 && start < spl_n[k] ? start : 0;
        o->z = (float)spl[k][o->node].z;
        o->rank = k + 1;                                /* 0x1491b: the others in grid order */
        /* its car (0x142e8): the next row of the table, the player's skipped */
        if(row >= ncars) row = 0;
        if(row == player_row) row++;
        if(row >= ncars) row = 0;
        o->model = car_model_index(&cars[opp_bullet && ncars > 12 ? 12 : row]);
        o->livery = (uint32_t)cars[row].livery;
        row++;
        const struct track_model *md = &models[o->model >= 0 && o->model < nm ? o->model : 0];
        float nose = md->axle_front + 250.0f, tail = md->axle_rear + 150.0f;
        o->radius = (nose + tail) * 0.25f;
        o->nose = nose - o->radius;
        o->tail = -(tail - o->radius);
        place(o, k);
        o->prev_yaw = o->yaw;
    }
    player_rank = nopp + 1;                             /* 0x12c24: the player starts last */
    for(int k = 0; k < OPP_MAX; k++) engine[k] = snd_stop(engine[k]);
    touch = 0;
    hitcar = 0.0f;
}

int opp_lapped(void)
{
    int laps = car_race()->laps;
    float p = car_progress() - (float)(laps * 100);
    for(int k = 0; k < nopp; k++) {
        const struct opp *o = &opp[k];
        if((o->laps < laps && o->progress - (float)(o->laps * 100) < p) || laps - o->laps > 1) return 1;
    }
    return 0;
}

float opp_hitcar(void)
{
    return hitcar;
}

/* 0x1dffc: the engines, heard from the camera (car_step set the ear). Only cars within 35000
   and only the three nearest, a nearer car taking a farther one's voice; ((35000 - d) / 35000)^2
   x 23552, panned; pitch speed x 2.4 + 13568 + the Doppler shift. On the grid they rev
   (0x20e63): the speed for the note is sin(car number x 0.5 x the clock) x 15000 until the
   green light (below the lowest pitch held there, snd_pitch). Silent once the race is over. */
static void engines(const struct car_race *rc)
{
    int near[3] = { -1, -1, -1 }, over = rc->finished || rc->game_over;
    float d[OPP_MAX];
    for(int k = 0; k < nopp; k++) {
        d[k] = snd_dist(opp[k].x, opp[k].y);
        if(over || d[k] >= 35000.0f) continue;
        for(int j = 0; j < 3; j++)
            if(near[j] < 0 || d[k] < d[near[j]]) {
                for(int m = 2; m > j; m--) near[m] = near[m - 1];
                near[j] = k;
                break;
            }
    }
    for(int k = 0; k < nopp; k++) {
        if(k != near[0] && k != near[1] && k != near[2]) {
            engine[k] = snd_stop(engine[k]);
            engine_last[k] = d[k];
            continue;
        }
        float v = rc->on ? opp[k].speed : sinf((float)k * 0.5f * rc->clock) * 15000.0f;
        engine[k] = snd_at(engine[k], SND_OPP_ENGINE, opp[k].x, opp[k].y, 35000.0f, 23552.0f, v * 2.4f + 13568.0f,
                           &engine_last[k]);
    }
}

void opp_step(void)
{
    const struct car_race *rc = car_race();
    const float dt = DRIVE_DT;
    int go = rc->on, nm;
    const struct track_marker *mk = track_markers(&nm);
    for(int k = 0; k < nopp; k++) {
        struct opp *o = &opp[k];
        const struct track_opp_row *sp = &rows[k];
        /* 0x1ced0: the speed, max_spd x the PACE of the marker it last reached, with skill on
           top while the player is ahead of it in the running order (the catch-up) */
        if(go) {
            markers(o);
            float pace = nm ? mk[o->mark_at].pace : 1.0f;
            float target = (sp->max_spd + (player_rank < o->rank ? sp->skill : 0.0f)) * pace;
            if(o->speed < target) {
                o->speed += sp->accel * dt;
                if(o->speed > target) o->speed = target;
            } else if(o->speed > target) {
                o->speed -= sp->brakes * dt;
                if(o->speed < target) o->speed = target;
            }
        }
        /* 0x1d2fc: that far along the curve */
        float left = o->speed * dt;
        for(int guard = 0; guard < 8 && left > 0.0f; guard++) {
            float a[2], b[2], c[2], pos[2], der[2];
            piece(k, o->node, o->half, a, b, c);
            curve(a, b, c, o->u, pos, der);
            float len = sqrtf(der[0] * der[0] + der[1] * der[1]);
            if(len < 1.0f) len = 1.0f;
            float du = left / len;
            if(o->u + du < 1.0f) {
                o->u += du;
                break;
            }
            left -= (1.0f - o->u) * len;
            o->u = 0.0f;
            if(o->half == 0) o->half = 1;
            else {
                o->half = 0;
                int nx = spl[k][o->node].next;
                o->node = nx >= 0 && nx < spl_n[k] ? nx : 0;
            }
        }
        /* 0x1df24: in a bend the nose turns in and the body leans a little: v = (speed x sband)^2
           x the heading's turn over the step before (a frame of the game's 1/30 s), held to
           +-2; the heading's offset eases to v / 2 (+0x4c8), the body's roll to v x 0.02
           (+0x4b8) and its pitch to -|v x 0.02| (+0x4b0): nose down whichever way it turns.
           (0x1c9c0 gives the first body block the roll as its pitch instead; not copied) */
        float v = o->speed * sp->sband;
        v = v * v * wrap_pi(o->yaw - o->prev_yaw) * (50.0f / 30.0f);
        if(v > 2.0f) v = 2.0f;
        if(v < -2.0f) v = -2.0f;
        if(!go) v = 0.0f;
        o->prev_yaw = o->yaw;
        o->spin = (o->spin - v * 0.5f) * drive_damp(0x4f) + v * 0.5f;
        o->body = (o->body - v * 0.02f) * drive_damp(0x4c) + v * 0.02f;
        o->dip = (o->dip + fabsf(v) * 0.02f) * drive_damp(0x4c) - fabsf(v) * 0.02f;
        /* 0x1d6ac: a collision's push carries on for a moment and fades; three seconds after
           the last one it drifts back to its line */
        if(o->shove > 0.0f) {
            o->offx += o->shove_x * o->shove * dt;
            o->offy += o->shove_y * o->shove * dt;
            o->shove *= drive_damp(79);
            if(o->shove < 10.0f) o->shove = 0.0f;
            o->off_age = 0.0f;
        }
        if(o->offx != 0.0f || o->offy != 0.0f) {
            o->off_age += dt;
            if(o->off_age >= 3.0f) {
                float d = drive_damp(0x60);
                o->offx *= d;
                o->offy *= d;
                if(fabsf(o->offx) < 1.0f && fabsf(o->offy) < 1.0f) o->offx = o->offy = 0.0f;
            }
        }
        o->wheel = fmodf(o->wheel - o->speed * dt / 108.6f, 2.0f * PI);
        place(o, k);
    }
    collide();
    /* the running order (0x20418): once the race is on and the player has crossed the line,
       the first opponent found on the wrong side of the player -- further on with a worse
       place, or the other way round -- changes places with it; one a step, and opponents never
       change places among themselves */
    if(go && rc->clock_on && !rc->finished) {         /* (the place at the line stays) */
        float prog = car_progress();
        for(int k = 0; k < nopp; k++)
            if((prog - opp[k].progress) * (float)(opp[k].rank - player_rank) < 0.0f) {
                int t = opp[k].rank;
                opp[k].rank = player_rank;
                player_rank = t;
                break;
            }
    }
    engines(rc);
}

/* opponent k's blocks (opp_place): heading with the nose's turn, the slope's tilt negated, the
   body's lean on top; never braking, 0x2000 at 4000 units/s or under */
static int opp_make(int k, const struct track_cam *cam, struct track_object *obj)
{
    static struct track_block blocks[OPP_MAX][CAR_BLOCKS];
    const struct opp *o = &opp[k];
    struct car_look lk = { o->x, o->y, o->z + (o->crash == 1 ? o->up : 0.0f),
                           o->yaw + o->spin + (o->crash ? o->turn : 0.0f),
                           -o->pitch + (o->crash == 1 ? o->tip : 0.0f), -o->roll + (o->crash == 1 ? o->lean : 0.0f),
                           o->dip, o->body, o->wheel, 0.0f,
                           (o->livery | 0x4000u | (o->speed <= 4000.0f ? 0x2000u : 0)) >> 2 };
    return car_draw(o->model, &lk, cam, blocks[k], obj);
}

/* opponent k's shadow (0x1c83c), made when it is drawn (the CPU's time before the first piece
   is the frame's; once the pieces go, the geometry unit's work is): heading the car object's,
   tilted as its car unless that crashes */
static int shadow_make(int k, const struct track_cam *cam, struct track_object *obj)
{
    static struct track_block shadows[OPP_MAX];
    const struct opp *o = &opp[k];
    (void)cam;
    car_flat_block(efx_shadow(), obj->x, obj->y, obj->h, o->yaw + o->spin + (o->crash ? o->turn : 0.0f), o->sh_pitch,
                   o->sh_roll, &shadows[k]);
    obj->n = 1;
    obj->b = &shadows[k];
    return 1;
}

int opp_objects(struct track_object *objs, int max)
{
    uint32_t t0 = rdcycle();
    int n = 0;
    const uint8_t *sh = efx_shadow();
    for(int k = 0; k < nopp && n < max; k++) {
        const struct opp *o = &opp[k];
        objs[n++] = (struct track_object){ o->x, o->y, o->z, 1200.0f, 0, 0, opp_make, k, 0, &refl[k], 0 };
        if(!sh || n >= max) continue;
        /* its shadow: its whole-number place without the player's nudge, 2 under the ground under
           it (0x1cd34: +0x238, which it stands on unless thrown up) */
        float x = (float)(int32_t)o->x, y = (float)(int32_t)o->y, h = (float)((int32_t)o->z - 2);
        objs[n++] = (struct track_object){ x, y, h, 900.0f, 0, 0, shadow_make, k, track_class_key(sh), 0, 0 };
    }
    efx_cyc_objects += rdcycle() - t0;
    return n;
}

int opp_where(int k, float *x, float *y)
{
    if(k < 0 || k >= nopp) return 0;
    *x = opp[k].x;
    *y = opp[k].y;
    return 1;
}

int opp_count(void)
{
    return nopp;
}

int opp_player_rank(void)
{
    return player_rank + ghost_ahead();                 /* time attack: second behind the ghost (0x20418) */
}
