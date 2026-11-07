/*
 * The opponents (see opp.h). This follows OpenScreamer's port/src/main.c (GPL-3.0: race_restart,
 * opp_assign_cars, opponents_step, opp_place, opp_markers), which took it from the game. The
 * addresses are in STARTL.EXE:
 *   0x1ced0 the speed, 0x1d2fc the way along the curve, 0x1d54c + 0x1e7a4 the curve,
 *   0x1cb9c standing on the road, 0x1df24 the turn and lean in bends, 0x1d6ac drifting back to
 *   the line, 0x1db84 the markers, 0x20418 the running order, 0x142e8 the cars.
 *   Contact between cars: 0x12378 and 0x17818 the player and an opponent, 0x126e4, 0x1cffc and
 *   0x1d1f0 two opponents, 0x1e404 overtaking. An opponent's crash: 0x1cffc and 0x1d824, and
 *   the road under it 0x1cb9c. Its shadow: 0x1c83c and 0x1cd34.
 * Entry points: opp_init() (the grid), opp_step() (one step of all opponents), opp_objects()
 * (for drawing), and the getters of opp.h.
 *
 * The curve (0x1d54c + 0x1e7a4) is a uniform quadratic B-spline over the nodes' handles, two
 * pieces for each node. Piece 0 runs over node.in, node.out, next.in. Piece 1 runs over
 * node.out, next.in, next.out. With the piece's control points A, B and C,
 * point(u) = A (1 - u)^2 / 2 + C u^2 / 2 + B (1 - (1 - u)^2 / 2 - u^2 / 2).
 * The node's own point is not on the curve. It is only the place on the grid.
 */
#include "opp.h"
#include <math.h>
#include "car.h"
#include "ghost.h"
#include "efx.h"
#include "hw.h"
#include "sound.h"

/* Scheduling aids for the CPU's FPU. The answer to a comparison comes back late, and a use
   right after the question waits for it. If all comparisons are asked for before any answer is
   used, the answers come back side by side and the CPU waits only once (as track.c's INT_NOW).
   An ASKED macro is an empty assembler statement that reads and writes its operands. The
   compiler therefore has to compute all of them before it and cannot move a use of one of
   them in between. */
#define ASKED2(a, b) __asm__ ("" : "+r"(a), "+r"(b))
#define ASKED3(a, b, c) __asm__ ("" : "+r"(a), "+r"(b), "+r"(c))
#define ASKED4(a, b, c, d) __asm__ ("" : "+r"(a), "+r"(b), "+r"(c), "+r"(d))

struct opp {
    int node, half;             /* the piece of its curve it is on */
    float u, speed;             /* 0..1 along that piece; units/s */
    float offx, offy, off_age;  /* pushed aside by a collision; drifts back after 3 s */
    float shove, shove_x, shove_y;  /* still being pushed (+0x450, +0x460/468) */
    float prev_speed;           /* its speed in the previous step (a loss of more than 5000 crashes it) */
    /* a crash (+0x2f8): 1 thrown up and tumbling, 4 spinning round twice */
    int crash, bounces;
    /* vy: upward speed. up: height above the road. tip, turn, lean: the extra pitch, heading
       and roll of a crash. tip_rate, turn_rate, lean_rate: their rates. */
    float vy, up, tip, turn, lean, tip_rate, turn_rate, lean_rate;
    float nose, tail, radius;   /* its two circles: ahead of and behind its origin, their radius */
    float spin, body, dip, prev_yaw;    /* the nose turned into a bend, the body leant (roll) and
                                           pitched (0x1df24); the heading in the previous step */
    float x, y, z, yaw, pitch, roll;
    float fwd[2];               /* the direction it faces: (sin yaw, -cos yaw) */
    float sh_pitch, sh_roll;    /* its shadow's tilt: the car object's, kept while it crashes */
    float wheel;                /* the wheels' turn */
    int mark_at, mk_count, laps;    /* the marker it last reached, markers passed in this lap, laps */
    uint8_t mk_seen[64];        /* markers passed in this lap */
    float progress;             /* its progress round the race */
    int rank, model;            /* its place in the running order; its car model */
    uint32_t livery;            /* its paint job */
};
static struct opp opp[OPP_MAX];
static struct track_refl refl[OPP_MAX];   /* each car object's reflection state (record +0x24 ..) */
static int nopp, player_rank;
int opp_bullet;
static const struct track_spline_node *spl[OPP_MAX];
static int spl_n[OPP_MAX];
static const struct track_opp_row *rows;
static int engine[OPP_MAX];             /* the engines' voices (sound.h) and their distance from */
static float engine_last[OPP_MAX];      /* the ear in the previous step */
static int touch;                       /* the player touched one in this step's contact */
static float hitcar;                    /* the player's +0x7c4 (opp_hitcar) */

/* Gives the three control points a, b and c (x, y) of the piece that opponent k is on: node
   and half of its curve (see the top of this file). */
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

/* Gives the point pos and the direction of travel der (the derivative) of the piece a, b, c
   at u (0..1). */
static void curve(const float a[2], const float b[2], const float c[2], float u, float pos[2], float der[2])
{
    float w0 = (1.0f - u) * (1.0f - u) * 0.5f, w2 = u * u * 0.5f, w1 = 1.0f - w0 - w2;
    for(int j = 0; j < 2; j++) {
        pos[j] = a[j] * w0 + b[j] * w1 + c[j] * w2;
        der[j] = (1.0f - u) * (b[j] - a[j]) + u * (c[j] - b[j]);
    }
}

/*
 * Works out where opponent o (number k) is, and how it crashes.
 *   - The place is the curve's point plus a collision's offset. The heading is along the curve.
 *   - It stands on ONE point, the drivable surface under its middle, and is tilted to that
 *     face's slope (original: STARTL.EXE 0x1cb9c; no smoothing, no suspension).
 *   - Pushed off the drivable area at more than 3000 units/s, it is stopped dead (0x1cb9c).
 *   - An opponent that lost more than 5000 units/s since the previous step crashes (0x1cffc,
 *     0x1d824). Depending on where it is, it is either thrown up and tumbles like the player,
 *     or it spins twice round on the road.
 *   - A crash in progress is advanced by one step.
 */
static void place(struct opp *o, int k)
{
    float a[2], b[2], c[2], pos[2], der[2];
    piece(k, o->node, o->half, a, b, c);
    curve(a, b, c, o->u, pos, der);
    o->x = pos[0] + o->offx;
    o->y = pos[1] + o->offy;
    o->yaw = atan2f(der[0], -der[1]);                   /* forward = (sin yaw, -cos yaw) */
    o->fwd[0] = sinf(o->yaw);
    o->fwd[1] = -cosf(o->yaw);
    float h;
    int on = track_under(o->x, o->y, &h, 0);
    if(on) {
        o->z = h;
        track_under_tilt(o->fwd[0], o->fwd[1], &o->pitch, &o->roll);
    }
    /* A face that is found with its height is found without it as well, so only a miss makes
       a second call. */
    if(o->speed > 3000.0f && !on && !track_under(o->x, o->y, 0, 0)) o->speed = 0.0f;
    if(!o->crash && o->prev_speed - o->speed > 5000.0f) {
        if((((int)o->x + (int)o->y) >> 2) & 1) {        /* which crash depends on where it is */
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
        if(o->up <= 0.0f) {                             /* a landing: it bounces up again, less, four times */
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
    /* Its shadow's tilt (original: 0x1cb14) is the car object's, unless it crashes (+0x2f8). */
    if(!o->crash) {
        o->sh_pitch = -o->pitch;
        o->sh_roll = -o->roll;
    }
}

/* Gives the centres of opponent o's two circles (nose and tail), as for the player (original:
   STARTL.EXE 0x142e8). */
static void circles(const struct opp *o, float cx[2], float cy[2])
{
    float fx = o->fwd[0], fy = o->fwd[1];
    cx[0] = o->x + fx * o->nose; cy[0] = o->y + fy * o->nose;
    cx[1] = o->x + fx * o->tail; cy[1] = o->y + fy * o->tail;
}

/*
 * The player touches opponent o (original: STARTL.EXE 0x17818). (nx, ny) points from o's
 * circle to the player's, and pen is the overlap.
 *   - o is moved out of the way, unless that puts it off the track. Then the player is moved.
 *   - If o is in front (the player ran into it), o is shoved on at half the player's speed and
 *     knocked round by speed / 10000 rad (at most a quarter turn). The player is shoved back
 *     (car_rammed).
 *   - If o is behind (it ran into the player), o keeps 70 % of its speed.
 *   - The crash sound plays with the contact. The game starts it afresh on every frame of
 *     contact; here it starts at most 30 times a second.
 */
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
        if(turn > HALF_PI) turn = HALF_PI;
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

/*
 * Car against car, after every car's step.
 *   - The player and each opponent within 1500 units (and 600 in height) touch (original:
 *     STARTL.EXE 0x12378).
 *   - Overtaking (0x1e404): an opponent looks three radii ahead. If the slower player is there,
 *     the opponent moves its line over by just enough to clear the player.
 *   - Two opponents within 1500 units touch (0x1cffc, 0x1d1f0). The faster drops to their
 *     average speed, and the later one is moved.
 */
static void collide(void)
{
    const struct car_state *c = car_state();
    float pcx[2], pcy[2], pr = car_circles(pcx, pcy);
    hitcar = (hitcar - (float)touch) * drive_damp(0x32) + (float)touch;     /* 0x175aa, 0x177f7 */
    touch = 0;
    /* Each opponent's circles are computed once: nothing here moves an opponent or turns it. */
    float occ[OPP_MAX][2][2];
    for(int k = 0; k < nopp; k++) circles(&opp[k], occ[k][0], occ[k][1]);
    for(int k = 0; k < nopp; k++) {
        struct opp *o = &opp[k];
        const float *ocx = occ[k][0], *ocy = occ[k][1];
        int nx = fabsf(c->x - o->x) < 1500.0f, ny = fabsf(c->y - o->y) < 1500.0f,
            nz = fabsf(c->z - o->z) < 600.0f;
        ASKED3(nx, ny, nz);
        if(nx & ny & nz)
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
        float fx = o->fwd[0], fy = o->fwd[1];
        float qx = o->x + fx * o->radius * 3.0f, qy = o->y + fy * o->radius * 3.0f;
        int ax = fabsf(qx - c->x) < 5000.0f, ay = fabsf(qy - c->y) < 5000.0f, slower = c->speed < o->speed;
        int az = fabsf(c->z - o->z) < 600.0f;
        ASKED4(ax, ay, slower, az);
        if(ax & ay & slower & az) {
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
            int fx2 = fabsf(o->x - q->x) >= 1500.0f, fy2 = fabsf(o->y - q->y) >= 1500.0f;
            ASKED2(fx2, fy2);
            if(fx2 | fy2) continue;
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

/*
 * Follows the lap's markers (original: STARTL.EXE 0x1db84). Like the player, the opponent
 * looks only at the marker it last reached and at that marker's two neighbours. A marker
 * counts when the marker before it has been passed and it has not been passed yet. When the
 * start line (marker 0) is reached with every marker passed, a lap is done.
 */
static void markers(struct opp *o)
{
    int n;
    const struct track_marker *mk = track_markers(&n);
    if(n == 0) return;
    for(int d = -1; d <= 1; d++) {
        int i = o->mark_at + d;                         /* wrap round at both ends of the list */
        i = i < 0 ? i + n : i >= n ? i - n : i;
        const struct track_marker *m = &mk[i];
        float dx = o->x - (float)m->x, dy = o->y - (float)m->y, rad = (float)m->radius;
        int wx = fabsf(dx) >= rad, wy = fabsf(dy) >= rad;
        ASKED2(wx, wy);
        if(wx | wy) continue;
        if(sqrtf(dx * dx + dy * dy) > rad) continue;
        o->mark_at = i;
        if(i >= 64) continue;
        if((i != 0 && !o->mk_seen[i - 1]) || o->mk_seen[i]) {
            if(i == 0 && o->mk_count == n) {            /* the line with every marker passed: a lap */
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
    /* The special modes race alone (original: STARTL.EXE 0x1311c, ...). */
    if(nopp < 0 || (car_mode & CAR_ALONE)) nopp = 0;
    int row = 0;
    for(int k = 0; k < nopp; k++) {
        struct opp *o = &opp[k];
        int start;
        refl[k] = (struct track_refl){ { 0, 0, 0 }, { 0.0f, 0.0f, 0.0f } };
        spl[k] = track_opp_spline(k, &start, &spl_n[k]);
        *o = (struct opp){ 0 };
        o->node = start >= 0 && start < spl_n[k] ? start : 0;
        o->z = (float)spl[k][o->node].z;
        o->rank = k + 1;                                /* the others in grid order (original: 0x1491b) */
        /* Its car (original: 0x142e8): the next row of the table, with the player's row skipped. */
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
    player_rank = nopp + 1;                             /* the player starts last (original: 0x12c24) */
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

/*
 * Plays the opponents' engines as heard from the camera (original: STARTL.EXE 0x1dffc;
 * car_step sets the ear).
 *   - Only cars within 35000 units are heard, and only the three nearest. A nearer car takes a
 *     farther one's voice.
 *   - The volume is ((35000 - d) / 35000)^2 x 23552 for the distance d, panned. The pitch is
 *     speed x 2.4 + 13568 plus the Doppler shift.
 *   - On the grid the engines rev (original: 0x20e63). Until the green light the speed for the
 *     note is sin(car number x 0.5 x the clock) x 15000. A speed that gives a pitch below the
 *     lowest one is held at the lowest (snd_pitch).
 *   - Once the race is over the engines are silent.
 */
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
        engine[k] = snd_at(engine[k], SND_OPP_ENGINE, opp[k].x, opp[k].y, 35000.0f, 23552.0f,
                           v * 2.4f + 13568.0f, &engine_last[k]);
    }
}

/*
 * One step of 1/50 s for all opponents, after the player's car_step(). For each opponent:
 *   1. The speed moves towards the target of the marker it last reached.
 *   2. It moves along its curve by speed x dt.
 *   3. In a bend its nose turns in and its body leans.
 *   4. A collision's push carries on and fades, and the offset it left drifts back to the line.
 * Then, for all opponents together: where each stands (place), contact between the cars
 * (collide), the running order and the engines' sound.
 */
void opp_step(void)
{
    const struct car_race *rc = car_race();
    const float dt = DRIVE_DT;
    int go = rc->on, nm;
    const struct track_marker *mk = track_markers(&nm);
    for(int k = 0; k < nopp; k++) {
        struct opp *o = &opp[k];
        const struct track_opp_row *sp = &rows[k];
        /* The speed (original: STARTL.EXE 0x1ced0). The target is max_spd x the PACE of the
           marker it last reached. While the player is ahead of it in the running order, skill is
           added to max_spd (the catch-up). The speed moves towards the target at accel when it
           speeds up and at brakes when it slows down. Before the green light it is held. */
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
        /* The way along the curve (original: 0x1d2fc). left is the distance still to move in
           this step. It may cross several pieces (at most 8). len, the length of the
           derivative, converts the distance to a step in u. */
        float left = o->speed * dt;
        for(int guard = 0; guard < 8 && left > 0.0f; guard++) {
            float a[2], b[2], c[2], pos[2], der[2];
            piece(k, o->node, o->half, a, b, c);
            curve(a, b, c, o->u, pos, der);
            float len = sqrtf(der[0] * der[0] + der[1] * der[1]);
            if(len < 1.0f) len = 1.0f;
            float du = left / len;
            if(o->u + du < 1.0f) {                      /* it stays in this piece */
                o->u += du;
                break;
            }
            left -= (1.0f - o->u) * len;                /* it reaches the end of the piece */
            o->u = 0.0f;
            if(o->half == 0) o->half = 1;
            else {
                o->half = 0;
                int nx = spl[k][o->node].next;
                o->node = nx >= 0 && nx < spl_n[k] ? nx : 0;
            }
        }
        /* In a bend the nose turns in and the body leans a little (original: 0x1df24). With
           v = (speed x sband)^2 x the heading's turn over the previous step (a frame of the game's
           1/30 s), held to +-2: the heading's offset eases to v / 2 (+0x4c8), the body's roll
           to v x 0.02 (+0x4b8) and its pitch to -|v x 0.02| (+0x4b0). So the nose is down
           whichever way it turns. The game (0x1c9c0) gives the first body block the roll as its
           pitch; the core does not copy that. */
        float v = o->speed * sp->sband;
        v = v * v * wrap_pi(o->yaw - o->prev_yaw) * (50.0f / 30.0f);
        if(v > 2.0f) v = 2.0f;
        if(v < -2.0f) v = -2.0f;
        if(!go) v = 0.0f;
        o->prev_yaw = o->yaw;
        o->spin = (o->spin - v * 0.5f) * drive_damp(0x4f) + v * 0.5f;
        o->body = (o->body - v * 0.02f) * drive_damp(0x4c) + v * 0.02f;
        o->dip = (o->dip + fabsf(v) * 0.02f) * drive_damp(0x4c) - fabsf(v) * 0.02f;
        /* A collision's push carries on for a moment and fades (original: 0x1d6ac). Three
           seconds after the last push the offset drifts back to the line. */
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
    }
    /* Where each opponent stands is worked out in a loop of its own. The opponents do not
       touch each other until collide(). The separate loops keep the code of each loop in the
       cache. */
    for(int k = 0; k < nopp; k++) place(&opp[k], k);
    collide();
    /* The running order (original: 0x20418). Once the race is on and the player has crossed
       the line, the first opponent found on the wrong side of the player changes places with
       the player. Wrong side means further on with a worse place, or the other way round. One
       change a step; opponents never change places among themselves. After the finish the
       order stays. */
    if(go && rc->clock_on && !rc->finished) {
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

/* Makes opponent k's blocks (opp_place in the port). The heading includes the nose's turn, the
   slope's tilt is negated, and the body's lean is added on top. The texture phase has bit
   0x4000 set (never braking) and bit 0x2000 (slow) at 4000 units/s or under. */
static int opp_make(int k, const struct track_cam *cam, struct track_object *obj)
{
    static struct track_block blocks[OPP_MAX][CAR_BLOCKS];
    const struct opp *o = &opp[k];
    struct car_look lk = { o->x, o->y, o->z + (o->crash == 1 ? o->up : 0.0f),
                           o->yaw + o->spin + (o->crash ? o->turn : 0.0f),
                           -o->pitch + (o->crash == 1 ? o->tip : 0.0f),
                           -o->roll + (o->crash == 1 ? o->lean : 0.0f),
                           o->dip, o->body, o->wheel, 0.0f,
                           (o->livery | 0x4000u | (o->speed <= 4000.0f ? 0x2000u : 0)) >> 2 };
    return car_draw(o->model, &lk, cam, blocks[k], obj);
}

/* Makes opponent k's shadow (original: STARTL.EXE 0x1c83c) when it is drawn. (The CPU's time
   before the first piece goes to the geometry unit counts towards the frame; once the pieces
   go, the geometry unit's work does.) The heading is the car object's, and the tilt is the
   car's unless it crashes. */
static int shadow_make(int k, const struct track_cam *cam, struct track_object *obj)
{
    static struct track_block shadows[OPP_MAX];
    const struct opp *o = &opp[k];
    (void)cam;
    car_flat_block(efx_shadow(), obj->x, obj->y, obj->h, o->yaw + o->spin + (o->crash ? o->turn : 0.0f),
                   o->sh_pitch, o->sh_roll, &shadows[k]);
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
        /* Its shadow: the car's place cut to whole numbers, without the player's nudge, and 2
           under the ground height (original: 0x1cd34: +0x238, which the car stands on unless it
           is thrown up). */
        float x = (float)(int32_t)o->x, y = (float)(int32_t)o->y, h = (float)((int32_t)o->z - 2);
        objs[n++] = (struct track_object){ x, y, h, 900.0f, 0, 0, shadow_make, k, track_class_key(sh), 0, 0 };
    }
    efx_cyc_objects += rdcycle() - t0;
    return n;
}

int opp_where(int k, float *x, float *y, float *z)
{
    if(k < 0 || k >= nopp) return 0;
    *x = opp[k].x;
    *y = opp[k].y;
    if(z) *z = opp[k].z;
    return 1;
}

int opp_count(void)
{
    return nopp;
}

int opp_player_rank(void)
{
    /* In time attack the player is second while the ghost is ahead (original: STARTL.EXE 0x20418). */
    return player_rank + ghost_ahead();
}
