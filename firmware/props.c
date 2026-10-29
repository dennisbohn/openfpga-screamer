/*
 * The props by the road (props.h): placed once at the track's start, each with its block made
 * then, so a frame only hands the ones in reach to the drawing.
 */
#include <math.h>
#include <string.h>
#include "props.h"
#include "car.h"
#include "dat.h"
#include "hw.h"

#define PROPS_MAX   64          /* track 1 has 45 */
#define KINDS       6           /* rows of OSTACLES read (track 1 has 3) */
#define MESH_MAX    0x2000      /* a prop's .3DX file (track 1: up to 2576 bytes) */
#define SCRATCH     0x2000      /* MAPnDAT.DTR, then MAPnOST.SPL, at the end of mem */
#define REACH       43808.0f    /* the far plane (track.c far_z): nothing farther is drawn */
#define RADIUS      800.0f      /* round a prop's origin (the barrier is 580 wide) */

static struct prop { float x, y, h; int key; struct track_block b; } props[PROPS_MAX];
static int nprops;

static int32_t rd32(const uint8_t *p)
{
    return (int32_t)(p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

/* entry name of the container into out (at most max bytes): its size, 0 if it is missing */
static uint32_t entry(const uint8_t *file, uint32_t size, const char *name, uint8_t *out, uint32_t max)
{
    struct dat_entry e;
    return dat_find(file, size, name, &e) ? dat_extract(file, size, &e, out, max) : 0;
}

/* The mesh names of the kinds, the first column of OSTACLES' rows ("sazcono" -> "AZCONO.3DX":
   a string's type letter, then the name); how many rows there are */
static int kinds(const uint8_t *t, uint32_t n, char names[KINDS][16])
{
    static const char key[] = "OSTACLES";       /* (sic) */
    uint32_t p = 0;
    while(p + 8 <= n && memcmp(t + p, key, 8)) p++;
    if(p + 8 > n) return 0;
    int rows = 0;
    while(p < n && rows < KINDS) {
        while(p < n && t[p] != '\n') p++;       /* the next line */
        p++;
        while(p < n && (t[p] == ' ' || t[p] == '\t')) p++;
        if(p >= n || t[p] == '*' || t[p] == '\r' || t[p] == '\n') continue;
        if(t[p] == 'E') break;                  /* END_LIST */
        if(t[p] == 's') p++;
        int k = 0;
        while(p < n && k < 11 && t[p] > ' ') {
            uint8_t c = t[p++];
            names[rows][k++] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
        }
        memcpy(names[rows] + k, ".3DX", 5);
        rows++;
    }
    return rows;
}

/* the heading along the path at its point nearest to (x, y): forward = (sin yaw, -cos yaw) */
static float path_heading(float x, float y)
{
    uint32_t n = track_path_length(), best = 0;
    float bd = 3.4e38f, px, py, ph;
    for(uint32_t i = 0; i < n; i++) {
        track_path_point(i, &px, &py, &ph);
        float d = (px - x) * (px - x) + (py - y) * (py - y);
        if(d < bd) {
            bd = d;
            best = i;
        }
    }
    float ax, ay, bx, by;
    track_path_point(best + n - 1, &ax, &ay, &ph);
    track_path_point(best + 1, &bx, &by, &ph);
    return atan2f(bx - ax, -(by - ay));
}

int props_init(const uint8_t *dat, uint32_t dat_size, const uint8_t *gfx, uint32_t gfx_size, int n,
               uint8_t *mem, uint32_t cap)
{
    nprops = 0;
    if(!dat || !gfx || cap < KINDS * MESH_MAX + SCRATCH || !track_path_length()) return 0;
    uint8_t *scratch = mem + cap - SCRATCH;
    char name[16] = "MAPnDAT.DTR", names[KINDS][16];
    name[3] = (char)('0' + n);
    uint32_t len = entry(dat, dat_size, name, scratch, SCRATCH);
    int nk = kinds(scratch, len, names);
    const uint8_t *mesh[KINDS];
    for(int k = 0; k < nk; k++) {
        uint8_t *out = mem + k * MESH_MAX;
        uint32_t m = entry(gfx, gfx_size, names[k], out, MESH_MAX);
        int32_t blk = m >= 0x340 && rd32(out + 0x14) >= 1 ? rd32(out + 0x18) : 0;
        mesh[k] = blk > 0 && (uint32_t)blk + 0x80 <= m ? out : 0;
    }
    memcpy(name + 4, "OST.SPL", 8);
    len = entry(dat, dat_size, name, scratch, SCRATCH);
    if(len < 8) return 0;
    int32_t count = rd32(scratch);              /* its one spline: count, start, nodes of 56 bytes */
    for(int32_t i = 0; i < count && nprops < PROPS_MAX && 8 + 56 * (uint32_t)(i + 1) <= len; i++) {
        const uint8_t *nd = scratch + 8 + 56 * i;
        int32_t kind = rd32(nd + 12);
        float x = (float)rd32(nd + 4), y = (float)rd32(nd + 8), h;
        int grass;
        if(kind < 0 || kind >= nk || !mesh[kind] || !track_under(x, y, &h, &grass)) continue;
        struct prop *p = &props[nprops++];
        p->x = x;
        p->y = y;
        p->h = h;
        p->key = track_class_key(mesh[kind]);
        car_flat_block(mesh[kind], x, y, h, path_heading(x, y), 0.0f, 0.0f, &p->b);
    }
    while(!(MEM_STATUS & MEM_WRITES_IDLE))      /* the meshes in the SDRAM for the geometry unit */
        ;
    return nprops;
}

int props_objects(const struct track_cam *cam, struct track_object *objs, int max)
{
    int n = 0;
    for(int k = 0; k < nprops && n < max; k++) {
        const struct prop *p = &props[k];
        float dx = p->x - cam->x, dy = p->y - cam->y;
        if(dx * dx + dy * dy > REACH * REACH) continue;
        objs[n++] = (struct track_object){ p->x, p->y, p->h, RADIUS, 1, &p->b, 0, k, p->key };
    }
    return n;
}
