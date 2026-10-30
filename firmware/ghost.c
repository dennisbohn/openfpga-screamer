/*
 * Time attack's ghost (ghost.h): the recording, each track's best for the session, the
 * playback and the ghost's car.
 */
#include "ghost.h"
#include <math.h>
#include <string.h>
#include "car.h"

#define TO_ANGLE    10430.378f          /* 65536 / 2 pi */
#define PROGRESS    64.0f               /* info's progress, bits 0-12 */
_Static_assert(sizeof(struct ghost_pose) == 20 && __builtin_offsetof(struct ghost_file, pose) == 16, "save layout");

int ghost_resume;
static struct ghost_file best[GHOST_TRACKS];    /* each track's kept run */
static struct ghost_file now;                   /* the run being recorded */
static const struct ghost_file *ghost;          /* the run raced, 0 for none */
static uint32_t at;                             /* steps since the set-up */
static int kept;                                /* the run counted (ghost_finish) */
static struct car_look look;                    /* the ghost's now */
static int model;
static uint32_t livery;
static float progress;
static struct track_refl refl;

struct ghost_file *ghost_track(int n)
{
    return n >= 1 && n <= GHOST_TRACKS ? &best[n - 1] : 0;
}

static int valid(const struct ghost_file *f)
{
    return f && !memcmp(f->magic, "ATK1", 4) && f->n > 0 && f->n <= GHOST_MAX;
}

void ghost_init(int n, int car)
{
    const struct ghost_file *g = ghost_track(n);
    ghost = car_mode == CAR_ATTACK && ghost_resume && valid(g) ? g : 0;    /* 0x12e14; none: 0x2a1df */
    memcpy(now.magic, "ATK1", 4);
    now.track = (uint8_t)n;
    now.car = (uint8_t)car;
    now.n = 0;
    now.steps = 0;
    now.flags = ghost ? !(ghost->flags & 1) : 0;        /* the other grid place (0x1bb33) */
    at = 0;
    kept = 0;
    progress = 0.0f;
    refl = (struct track_refl){ { 0, 0, 0 }, { 0.0f, 0.0f, 0.0f } };
    if(ghost) {                                         /* its car (0x14d28) */
        int nc;
        const struct car_spec *cars = track_cars(&nc);
        const struct car_spec *sp = &cars[ghost->car < nc ? ghost->car : 0];
        model = car_model_index(sp);
        livery = (uint32_t)sp->livery;
        look = (struct car_look){ 0 };
    }
}

int ghost_slot(void)
{
    return (int)(now.flags & 1);
}

float ghost_time(void)
{
    return ghost ? (float)ghost->steps * DRIVE_DT : 0.0f;
}

/* the original's cubic between poses b and c (0x22724): from b to c, the inner points a third of
   the way b-c along a-c and d-b (a, d the poses before and after) */
static float curve(float a, float b, float c, float d, float ab, float bd, float t)
{
    float c1 = b + (c - a) * ab, c2 = c + (b - d) * bd, s = 1.0f - t;
    return b * s * s * s + 3.0f * c1 * t * s * s + 3.0f * c2 * t * t * s + c * t * t * t;
}

static float turn(int16_t from, int16_t to, float t)
{
    return ((float)from + (float)(int16_t)(to - from) * t) / TO_ANGLE;
}

/* the ghost at step at: pose u = (at - 1) / GHOST_STEP, between poses i and i + 1 (the last one
   held), its wheels turning with the way it went */
static void playback(void)
{
    const struct ghost_pose *p = ghost->pose;
    int last = ghost->n - 1;
    float u = (float)(at - 1) / (float)GHOST_STEP;
    int i = (int)u;
    float t = u - (float)i;
    if(i >= last) {
        i = last;
        t = 0.0f;
    }
    const struct ghost_pose *a = &p[i > 0 ? i - 1 : 0], *b = &p[i], *c = &p[i < last ? i + 1 : last];
    const struct ghost_pose *d = &p[i + 2 <= last ? i + 2 : last];
    float bc = sqrtf((float)(c->x - b->x) * (float)(c->x - b->x) + (float)(c->y - b->y) * (float)(c->y - b->y) +
                     (float)(c->z - b->z) * (float)(c->z - b->z)) / 3.0f;
    float ac = sqrtf((float)(c->x - a->x) * (float)(c->x - a->x) + (float)(c->y - a->y) * (float)(c->y - a->y) +
                     (float)(c->z - a->z) * (float)(c->z - a->z));
    float bd = sqrtf((float)(b->x - d->x) * (float)(b->x - d->x) + (float)(b->y - d->y) * (float)(b->y - d->y) +
                     (float)(b->z - d->z) * (float)(b->z - d->z));
    float fa = ac > 0.0f ? bc / ac : 0.0f, fd = bd > 0.0f ? bc / bd : 0.0f;
    float x = curve((float)a->x, (float)b->x, (float)c->x, (float)d->x, fa, fd, t);
    float y = curve((float)a->y, (float)b->y, (float)c->y, (float)d->y, fa, fd, t);
    float moved = sqrtf((x - look.x) * (x - look.x) + (y - look.y) * (y - look.y));
    look.spin = fmodf(look.spin - moved / 108.6f, 6.2831853f);
    look.x = x;
    look.y = y;
    look.z = curve((float)a->z, (float)b->z, (float)c->z, (float)d->z, fa, fd, t);
    look.yaw = turn(b->yaw, c->yaw, t);
    look.pitch = turn(b->pitch, c->pitch, t);
    look.roll = turn(b->roll, c->roll, t);
    look.phase = (livery | (b->info & 0x6000u)) >> 2;
    progress = (float)(b->info & 0x1FFF) / PROGRESS;
}

void ghost_step(void)
{
    if(car_mode != CAR_ATTACK) return;
    if(at % GHOST_STEP == 0 && now.n < GHOST_MAX) {     /* 0x231d0 */
        struct car_look lk;
        car_player_look(&lk);
        float pr = car_race()->clock_on ? car_progress() * PROGRESS : 0.0f;    /* 0 before the line */
        struct ghost_pose *q = &now.pose[now.n++];
        q->x = (int32_t)lk.x;
        q->y = (int32_t)lk.y;
        q->z = (int32_t)lk.z;
        q->yaw = (int16_t)(int32_t)lrintf(fmodf(lk.yaw, 6.2831853f) * TO_ANGLE);
        q->pitch = (int16_t)(int32_t)lrintf(lk.pitch * TO_ANGLE);
        q->roll = (int16_t)(int32_t)lrintf(lk.roll * TO_ANGLE);
        q->info = (uint16_t)(((lk.phase << 2) & 0x6000u) | (pr < 0.0f ? 0u : pr > 8191.0f ? 0x1FFFu : (uint32_t)pr));
    }
    at++;
    if(ghost) playback();
}

int ghost_finish(float total)
{
    now.steps = (uint32_t)lrintf(total / DRIVE_DT);
    kept = !ghost || now.steps < ghost->steps;          /* 0x23ef0: as slow is not enough */
    return kept;
}

void ghost_end(void)
{
    struct ghost_file *f = ghost_track(now.track);
    if(kept && f && car_mode == CAR_ATTACK) {           /* 0x1fd64: kept, raced from now on */
        memcpy(f, &now, 16 + sizeof now.pose[0] * now.n);
        ghost_resume = 1;
    }
    kept = 0;
}

int ghost_ahead(void)
{
    return ghost && car_race()->clock_on && progress > car_progress();    /* past the line (0x2042e) */
}

static int ghost_make(int id, const struct track_cam *cam, struct track_object *obj)
{
    static struct track_block blocks[CAR_BLOCKS];
    (void)id;
    return car_draw(model, &look, cam, blocks, obj);
}

int ghost_objects(struct track_object *objs, int max)
{
    if(!ghost || !at || max < 1) return 0;
    objs[0] = (struct track_object){ look.x, look.y, look.z, 1200.0f, 0, 0, ghost_make, 0, 0, &refl, 0 };
    return 1;
}
