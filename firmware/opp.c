/*
 * The opponents (opp.h), after OpenScreamer's port/src/main.c (GPL-3.0: race_restart,
 * opp_assign_cars, opponents_step, opp_place, opp_markers), from the game: 0x1ced0 the speed,
 * 0x1d2fc the way along the curve, 0x1d54c + 0x1e7a4 the curve, 0x1cb9c standing on the road,
 * 0x1df24 the turn and lean in bends, 0x1d6ac drifting back to the line, 0x1db84 the markers,
 * 0x20418 the running order, 0x142e8 the cars. Not yet here: contact between cars, overtaking,
 * an opponent's crash.
 *
 * The curve (0x1d54c + 0x1e7a4) is a uniform quadratic B-spline over the nodes' handles, two
 * pieces a node: piece 0 node.in, node.out, next.in; piece 1 node.out, next.in, next.out;
 * point(u) = A (1 - u)^2 / 2 + C u^2 / 2 + B (the rest). The node's own point is not on it,
 * it is only the place on the grid.
 */
#include "opp.h"
#include <math.h>
#include "car.h"

#define PI          3.14159265f

struct opp {
    int node, half;             /* the piece of its curve it is on */
    float u, speed;             /* 0..1 along that piece; units/s */
    float offx, offy, off_age;  /* pushed aside by a collision; drifts back after 3 s */
    float spin, body, prev_yaw; /* its nose turned into a bend, its body leant (0x1df24) */
    float x, y, z, yaw, pitch, roll;
    float wheel;                /* the wheels' turn */
    int mark_at, mk_count, laps;
    uint8_t mk_seen[64];
    float progress;
    int rank, model;
    uint32_t livery;
};
static struct opp opp[OPP_MAX];
static int nopp, player_rank;
static const struct track_spline_node *spl[OPP_MAX];
static int spl_n[OPP_MAX];
static const struct track_opp_row *rows;

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
   no smoothing, no suspension) */
static void place(struct opp *o, int k)
{
    float a[2], b[2], c[2], pos[2], der[2];
    piece(k, o->node, o->half, a, b, c);
    curve(a, b, c, o->u, pos, der);
    o->x = pos[0] + o->offx;
    o->y = pos[1] + o->offy;
    o->yaw = atan2f(der[0], -der[1]);                   /* forward = (sin yaw, -cos yaw) */
    float h;
    if(track_under(o->x, o->y, &h, 0)) {
        o->z = h;
        track_under_tilt(o->yaw, &o->pitch, &o->roll);
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
    int nrows, ns = track_opp_count(&nrows), ncars;
    const struct car_spec *cars = track_cars(&ncars);
    rows = track_opp_rows();
    nopp = ns - 1;                                      /* the last curve is the player's */
    if(nopp > nrows) nopp = nrows;
    if(nopp > OPP_MAX) nopp = OPP_MAX;
    if(nopp < 0) nopp = 0;
    int row = 0;
    for(int k = 0; k < nopp; k++) {
        struct opp *o = &opp[k];
        int start;
        spl[k] = track_opp_spline(k, &start, &spl_n[k]);
        *o = (struct opp){ 0 };
        o->node = start >= 0 && start < spl_n[k] ? start : 0;
        o->z = (float)spl[k][o->node].z;
        o->rank = k + 1;                                /* 0x1491b: the others in grid order */
        /* its car (0x142e8): the next row of the table, the player's skipped */
        if(row >= ncars) row = 0;
        if(row == player_row) row++;
        if(row >= ncars) row = 0;
        o->model = car_model_index(&cars[row]);
        o->livery = (uint32_t)cars[row].livery;
        row++;
        place(o, k);
        o->prev_yaw = o->yaw;
    }
    player_rank = nopp + 1;                             /* 0x12c24: the player starts last */
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
           +-2; the heading's offset eases to v / 2, the body to v x 0.02 of pitch and roll */
        float v = o->speed * sp->sband;
        v = v * v * wrap_pi(o->yaw - o->prev_yaw) * (50.0f / 30.0f);
        if(v > 2.0f) v = 2.0f;
        if(v < -2.0f) v = -2.0f;
        if(!go) v = 0.0f;
        o->prev_yaw = o->yaw;
        o->spin = (o->spin - v * 0.5f) * drive_damp(0x4f) + v * 0.5f;
        o->body = (o->body - v * 0.02f) * drive_damp(0x4c) + v * 0.02f;
        /* 0x1d6ac: three seconds after a collision's push it drifts back to its line */
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
}

int opp_objects(const struct rs_clip *clip, const struct track_cam *cam, struct track_object *objs, int max)
{
    static struct track_block blocks[OPP_MAX][CAR_BLOCKS];
    int n = 0;
    for(int k = 0; k < nopp && n < max; k++) {
        const struct opp *o = &opp[k];
        if(!track_in_view(clip, cam, o->x, o->y, o->z, 1200.0f)) continue;
        /* the object (opp_place): heading with the nose's turn, the slope's tilt negated, the
           body's lean on top; never braking, 0x2000 at 4000 units/s or under */
        struct car_look lk = { o->x, o->y, o->z, o->yaw + o->spin, -o->pitch, -o->roll, o->body, o->body,
                               o->wheel, 0.0f, (o->livery | 0x4000u | (o->speed <= 4000.0f ? 0x2000u : 0)) >> 2 };
        n += car_draw(o->model, &lk, cam, blocks[k], &objs[n]);
    }
    return n;
}

int opp_count(void)
{
    return nopp;
}

int opp_player_rank(void)
{
    return player_rank;
}
