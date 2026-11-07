/*
 * Marks (mark.h).
 */
#include "mark.h"
#include "dlog.h"
#include "hw.h"

void free_cam_step(struct track_cam *c, uint32_t keys)
{
    float f[3], r[3], u[3], fast = (keys & KEY_B) ? FREE_FAST : 1.0f, s = FREE_SPEED * fast;
    track_cam_axes(c, f, r, u);
    if (keys & KEY_UP) {
        c->x += f[0] * s;
        c->y += f[1] * s;
        c->h += f[2] * s;
    }
    if (keys & KEY_DOWN) {
        c->x -= f[0] * s;
        c->y -= f[1] * s;
        c->h -= f[2] * s;
    }
    if (keys & KEY_LEFT) c->yaw += FREE_TURN * (fast > 1.0f ? 2.0f : 1.0f);
    if (keys & KEY_RIGHT) c->yaw -= FREE_TURN * (fast > 1.0f ? 2.0f : 1.0f);
    if (keys & KEY_R) c->h += 0.7f * s;
    if (keys & KEY_L) c->h -= 0.7f * s;
    if (keys & KEY_X) c->pitch += 0.8f * FREE_TURN;
    if (keys & KEY_Y) c->pitch -= 0.8f * FREE_TURN;
    if (c->pitch > FREE_PITCH) c->pitch = FREE_PITCH;
    if (c->pitch < -FREE_PITCH) c->pitch = -FREE_PITCH;
    c->roll = 0.0f;
}

static struct { int32_t v[6]; } ring[MARK_RING];
static uint32_t pushed;                 /* cameras pushed since mark_reset */
static int marks;                       /* marks written since the core started */

void mark_reset(void)
{
    pushed = 0;
}

void mark_push(const struct track_cam *c)
{
    int32_t *v = ring[pushed % MARK_RING].v;
    v[0] = (int32_t)c->x;
    v[1] = (int32_t)c->y;
    v[2] = (int32_t)c->h;
    v[3] = (int32_t)(c->yaw * 1e4f);
    v[4] = (int32_t)(c->pitch * 1e4f);
    v[5] = (int32_t)(c->roll * 1e4f);
    pushed++;
}

int mark_save(int track, int view, uint32_t frame)
{
    if (!pushed) return 0;
    uint32_t room = dlog_kept();                /* the room kept for marks (and the benchmark's result) is theirs to use */
    dlog_keep(0);
    if (dlog_free() < MARK_BYTES) {
        dlog_keep(room);
        return 0;
    }
    marks++;
    dlog_str("mark ");
    dlog_dec(marks);
    dlog_str(" track ");
    dlog_dec(track);
    dlog_str(" view ");
    dlog_dec(view);
    dlog_str(" frame ");
    dlog_dec((int32_t)frame);
    dlog_str(":");
    for (int k = 0; k < MARK_POSES; k++) {
        uint32_t back = (uint32_t)k * MARK_STEP;
        if (back >= pushed || back >= MARK_RING) break;         /* the race is not that old, or the ring is */
        const int32_t *v = ring[(pushed - 1 - back) % MARK_RING].v;
        dlog_str(k ? " |" : "");
        for (int i = 0; i < 6; i++) {
            dlog_char(' ');
            dlog_dec(v[i]);
        }
    }
    dlog_char('\n');
    dlog_keep(room);
    return marks;
}
