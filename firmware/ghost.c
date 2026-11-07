/*
 * Time attack's ghost (see ghost.h): the recording of the player's run, each track's best run
 * (ATTACK.DAT), the playback and the ghost's car.
 * Entry points: ghost_init() (set-up), ghost_step() (record and play back), ghost_finish() and
 * ghost_end() (keep a better run), ghost_objects() (draw the ghost).
 */
#include "ghost.h"
#include <math.h>
#include <string.h>
#include "car.h"
#include "save.h"

#define TO_ANGLE    10430.378f          /* 65536 / 2 pi */
#define PROGRESS    64.0f               /* the progress is stored x 64, in bits 0-12 of a pose's info */
_Static_assert(sizeof(struct ghost_pose) == 20 && __builtin_offsetof(struct ghost_file, pose) == 16,
               "save layout");
_Static_assert(sizeof(struct ghost_file) == SAVE_ATTACK_BLOCK, "a track's block of ATTACK.DAT");

int ghost_resume;
static struct ghost_file now;                   /* the run being recorded */
static const struct ghost_file *ghost;          /* the run raced, 0 for none */
static uint32_t at;                             /* steps since the set-up */
static int kept;                                /* the run counted (ghost_finish) */
static struct car_look look;                    /* the ghost's place and look in this step */
static int model;                               /* the ghost's car model and paint job */
static uint32_t livery;
static float progress;                          /* the ghost's progress round the race */
static struct track_refl refl;                  /* the reflection state of the ghost's car object */

/* Each track's kept run lies in the memory of ATTACK.DAT (save.h). It is loaded at boot and
   saved on exit. */
struct ghost_file *ghost_track(int n)
{
    return n >= 1 && n <= GHOST_TRACKS ? (struct ghost_file *)SAVE_ATTACK(n) : 0;
}

/* A run is valid if it has the magic and 1 .. GHOST_MAX poses. */
static int valid(const struct ghost_file *f)
{
    return f && !memcmp(f->magic, "ATK1", 4) && f->n > 0 && f->n <= GHOST_MAX;
}

void ghost_init(int n, int car)
{
    const struct ghost_file *g = ghost_track(n);
    /* The ghost races only in time attack, after RESUME or PLAY AGAIN, and only if a valid run
       is kept (original: STARTL.EXE 0x12e14; the case without a ghost: 0x2a1df). */
    ghost = car_mode == CAR_ATTACK && ghost_resume && valid(g) ? g : 0;
    memcpy(now.magic, "ATK1", 4);
    now.track = (uint8_t)n;
    now.car = (uint8_t)car;
    now.n = 0;
    now.steps = 0;
    now.flags = ghost ? !(ghost->flags & 1) : 0;        /* the other grid place (original: 0x1bb33) */
    at = 0;
    kept = 0;
    progress = 0.0f;
    refl = (struct track_refl){ { 0, 0, 0 }, { 0.0f, 0.0f, 0.0f } };
    if(ghost) {                                         /* its car (original: 0x14d28) */
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

/*
 * A cubic Bezier curve from pose b to pose c, as in the original (STARTL.EXE 0x22724). a and d
 * are the poses before b and after c. The inner control points are c1 = b + (c - a) ab and
 * c2 = c + (b - d) bd. Each lies a third of the way from b to c along the direction a-c or
 * d-b; the caller passes ab and bd as factors that give that length. t runs from 0 at b to 1
 * at c.
 */
static float curve(float a, float b, float c, float d, float ab, float bd, float t)
{
    float c1 = b + (c - a) * ab, c2 = c + (b - d) * bd, s = 1.0f - t;
    return b * s * s * s + 3.0f * c1 * t * s * s + 3.0f * c2 * t * t * s + c * t * t * t;
}

/* The angle at t between the 16-bit angles from and to (65536 = 2 pi), going the short way
   round. Returns radians. */
static float turn(int16_t from, int16_t to, float t)
{
    return ((float)from + (float)(int16_t)(to - from) * t) / TO_ANGLE;
}

/* Works out the ghost's place and look for step `at`. The pose number is
   u = (at - 1) / GHOST_STEP, and the ghost is between poses i and i + 1 (the last pose is
   held). Its wheels turn with the distance it went. */
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
    float bc = sqrtf((float)(c->x - b->x) * (float)(c->x - b->x) +
                     (float)(c->y - b->y) * (float)(c->y - b->y) +
                     (float)(c->z - b->z) * (float)(c->z - b->z)) / 3.0f;
    float ac = sqrtf((float)(c->x - a->x) * (float)(c->x - a->x) +
                     (float)(c->y - a->y) * (float)(c->y - a->y) +
                     (float)(c->z - a->z) * (float)(c->z - a->z));
    float bd = sqrtf((float)(b->x - d->x) * (float)(b->x - d->x) +
                     (float)(b->y - d->y) * (float)(b->y - d->y) +
                     (float)(b->z - d->z) * (float)(b->z - d->z));
    float fa = ac > 0.0f ? bc / ac : 0.0f, fd = bd > 0.0f ? bc / bd : 0.0f;
    float x = curve((float)a->x, (float)b->x, (float)c->x, (float)d->x, fa, fd, t);
    float y = curve((float)a->y, (float)b->y, (float)c->y, (float)d->y, fa, fd, t);
    float moved = sqrtf((x - look.x) * (x - look.x) + (y - look.y) * (y - look.y));
    look.spin = fmodf(look.spin - moved / 108.6f, TWO_PI);
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
    if(at % GHOST_STEP == 0 && now.n < GHOST_MAX) {     /* record a pose (original: 0x231d0) */
        struct car_look lk;
        car_player_look(&lk);
        float pr = car_race()->clock_on ? car_progress() * PROGRESS : 0.0f;    /* 0 before the line */
        struct ghost_pose *q = &now.pose[now.n++];
        q->x = (int32_t)lk.x;
        q->y = (int32_t)lk.y;
        q->z = (int32_t)lk.z;
        q->yaw = (int16_t)(int32_t)lrintf(fmodf(lk.yaw, TWO_PI) * TO_ANGLE);
        q->pitch = (int16_t)(int32_t)lrintf(lk.pitch * TO_ANGLE);
        q->roll = (int16_t)(int32_t)lrintf(lk.roll * TO_ANGLE);
        q->info = (uint16_t)(((lk.phase << 2) & 0x6000u) |
                             (pr < 0.0f ? 0u : pr > 8191.0f ? 0x1FFFu : (uint32_t)pr));
    }
    at++;
    if(ghost) playback();
}

int ghost_finish(float total)
{
    now.steps = (uint32_t)lrintf(total / DRIVE_DT);
    /* A lap as slow as the ghost's is not enough (original: STARTL.EXE 0x23ef0). */
    kept = !ghost || now.steps < ghost->steps;
    return kept;
}

void ghost_end(void)
{
    struct ghost_file *f = ghost_track(now.track);
    if(kept && f && car_mode == CAR_ATTACK) {           /* keep the run (original: 0x1fd64) */
        memcpy(f, &now, 16 + sizeof now.pose[0] * now.n);   /* the header and the poses used */
        ghost_resume = 1;                               /* PLAY AGAIN races this run */
    }
    kept = 0;
}

int ghost_ahead(void)
{
    /* The comparison counts only past the line (original: STARTL.EXE 0x2042e). */
    return ghost && car_race()->clock_on && progress > car_progress();
}

/* Makes the ghost's blocks from its current look, when it comes up in the drawing. */
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
