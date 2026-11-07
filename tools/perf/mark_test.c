/* The marks (firmware/mark.c): the line a mark writes, the cameras 15, 30 and 45 frames earlier, a race that is younger than that,
   a log with no room. Stubs for the log. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static char logbuf[4096];
static uint32_t room = 1000;
void dlog_str(const char *s) { strcat(logbuf, s); }
void dlog_char(char c) { char b[2] = { c, 0 }; strcat(logbuf, b); }
void dlog_dec(int32_t v) { char b[16]; sprintf(b, "%d", v); strcat(logbuf, b); }
uint32_t dlog_free(void) { return room; }
static uint32_t kept_room;
uint32_t dlog_kept(void) { return kept_room; }
void dlog_keep(uint32_t b) { kept_room = b; }
#include "../../firmware/hw.h"
#include "../../firmware/track.h"
/* the camera's axes as track.c has them (forward, right, up in the game's x, y, height), for a level roll */
void track_cam_axes(const struct track_cam *c, float f[3], float r[3], float u[3])
{
    float sy = sinf(c->yaw), cy = cosf(c->yaw), sp = sinf(c->pitch), cp = cosf(c->pitch);
    f[0] = sy * cp; f[1] = -cy * cp; f[2] = sp;
    r[0] = -cy; r[1] = -sy; r[2] = 0.0f;
    u[0] = 0.0f; u[1] = 0.0f; u[2] = 1.0f;
}
#include "../../firmware/mark.c"

static int fails;

static void same(const char *what, const char *got, const char *want)
{
    int ok = strcmp(got, want) == 0;
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) printf("  got  \"%s\"\n  want \"%s\"\n", got, want);
    fails += !ok;
}

static void push(int i)
{
    struct track_cam c = { (float)i, (float)(-i), (float)(i + 1000), 1.5f, -0.25f, 0.0f };
    mark_push(&c);
}

int main(void)
{
    mark_reset();
    same("no mark before a camera", mark_save(4, 2, 0) == 0 ? "0" : "n", "0");
    for (int i = 0; i < 100; i++) push(i);
    int m = mark_save(4, 2, 812);
    char want[400];
    snprintf(want, sizeof want, "mark 1 track 4 view 2 frame 812: 99 -99 1099 15000 -2500 0 | 84 -84 1084 15000 -2500 0 | 69 -69 1069 15000 -2500 0 | 54 -54 1054 15000 -2500 0\n");
    same("a mark with four cameras", logbuf, want);
    same("its number", m == 1 ? "1" : "other", "1");
    printf("the line is %zu bytes (MARK_BYTES %d)\n", strlen(logbuf), MARK_BYTES);
    fails += strlen(logbuf) > MARK_BYTES;
    mark_reset();
    for (int i = 0; i < 20; i++) push(i);
    logbuf[0] = 0;
    m = mark_save(1, 0, 20);
    same("a young race has two cameras", logbuf, "mark 2 track 1 view 0 frame 20: 19 -19 1019 15000 -2500 0 | 4 -4 1004 15000 -2500 0\n");
    same("the second mark has number 2", m == 2 ? "2" : "other", "2");
    room = MARK_BYTES - 1;
    logbuf[0] = 0;
    same("no room: nothing is written", mark_save(1, 0, 20) == 0 && logbuf[0] == 0 ? "nothing" : "written", "nothing");
    kept_room = 1600;
    room = MARK_BYTES + 100;                     /* (dlog_free: what is there with the kept room given back) */
    logbuf[0] = 0;
    same("a mark takes the kept room and puts it back", mark_save(1, 0, 20) != 0 && kept_room == 1600 ? "ok" : "wrong", "ok");
    /* the free camera */
    struct track_cam c = { 0 };
    free_cam_step(&c, KEY_UP);
    same("up moves along the view", c.y < -159.9f && c.y > -160.1f && fabsf(c.x) < 0.01f && c.h == 0.0f ? "ok" : "wrong", "ok");
    c = (struct track_cam){ 0 };
    free_cam_step(&c, KEY_DOWN | KEY_B);
    same("down with B is six times back", c.y > 959.9f && c.y < 960.1f ? "ok" : "wrong", "ok");
    c = (struct track_cam){ 0 };
    free_cam_step(&c, KEY_LEFT);
    same("left raises the yaw", c.yaw > 0.0249f && c.yaw < 0.0251f ? "ok" : "wrong", "ok");
    free_cam_step(&c, KEY_RIGHT | KEY_RIGHT);
    same("right lowers it", c.yaw > -0.0001f && c.yaw < 0.0001f ? "ok" : "wrong", "ok");
    c = (struct track_cam){ 0 };
    free_cam_step(&c, KEY_R);
    free_cam_step(&c, KEY_R | KEY_L);
    same("R raises, L lowers", c.h > 111.9f && c.h < 112.1f ? "ok" : "wrong", "ok");
    c = (struct track_cam){ 0 };
    for (int i = 0; i < 200; i++) free_cam_step(&c, KEY_X);
    same("the tilt up stops at the limit", c.pitch == FREE_PITCH ? "ok" : "wrong", "ok");
    c.roll = 0.5f;
    c.x = c.y = c.h = 0.0f;
    free_cam_step(&c, KEY_UP);
    same("looking up, forward climbs; the roll is level", c.h > 100.0f && c.roll == 0.0f ? "ok" : "wrong", "ok");
    c = (struct track_cam){ 0 };
    free_cam_step(&c, 0);
    same("nothing held, nothing moves", c.x == 0.0f && c.y == 0.0f && c.h == 0.0f && c.yaw == 0.0f ? "ok" : "wrong", "ok");
    return fails ? 1 : 0;
}
