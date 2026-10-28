/*
 * The track file's palette, car models and backdrop from the game files (tb_gfx.h), as
 * tools/make_track.py has them (palette in main(), car_model(), pcx()). The files' words are
 * read a byte at a time: the CPU does no unaligned loads.
 */
#include "tb_gfx.h"
#include <string.h>

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wrf(uint8_t *p, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    wr32(p, v);
}

/* int32 at byte o of a file of size bytes into *v; 0 if it lies outside */
static int rd32(const uint8_t *p, uint32_t size, uint32_t o, int32_t *v)
{
    if(size < 4 || o > size - 4) return 0;
    p += o;
    *v = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
    return 1;
}

uint32_t tb_palette(const uint8_t *col, uint32_t size, uint8_t *out, uint32_t cap)
{
    if(size < 768 || cap < TB_PALETTE_SIZE) return 0;
    for(int i = 0; i < 256; i++) {
        const uint8_t *c = col + 3 * i;     /* values above 63 spill over as in the tool */
        wr32(out + 4 * i, (uint32_t)c[0] * 4 << 16 | (uint32_t)c[1] * 4 << 8 | (uint32_t)c[2] * 4);
    }
    return TB_PALETTE_SIZE;
}

/* A .3DX car (the nose towards local -z; openscreamer mesh.c): header +0x14 the block count,
   +0x18 the blocks' offsets; block +0x0C its frame (<= 0: none), frame +0x10 x, +0x18 z. The
   blocks with a frame off the centre line (x != 0) are the wheels: wheelbase hi - lo, front
   axle -lo, rear axle hi over their z, half track the largest |x|. 1, or 0 if malformed (a
   negative block offset too, which Python would count from the file's end). */
static int car_model(const uint8_t *x3d, uint32_t size, float r[4])
{
    int32_t nblk, off, so, tx, tz;
    int64_t lo = 0, hi = 0, half = 0;
    int seen = 0;
    if(!rd32(x3d, size, 0x14, &nblk)) return 0;
    for(int32_t k = 0; k < nblk; k++) {     /* the reads fail long before 4 k overflows */
        if(!rd32(x3d, size, 0x18 + 4 * (uint32_t)k, &off) || off < 0 ||
           !rd32(x3d, size, (uint32_t)off + 0x0C, &so)) return 0;
        if(so <= 0) continue;
        if(!rd32(x3d, size, (uint32_t)so + 0x10, &tx) || !rd32(x3d, size, (uint32_t)so + 0x18, &tz)) return 0;
        if(tx == 0) continue;
        if(!seen || tz < lo) lo = tz;
        if(!seen || tz > hi) hi = tz;
        seen = 1;
        int64_t a = tx < 0 ? -(int64_t)tx : tx;
        if(a > half) half = a;
    }
    if(!seen) {
        r[0] = 935.0f; r[1] = 445.0f; r[2] = 490.0f; r[3] = 226.0f;
        return 1;
    }
    /* Python goes int -> double -> float: the double is exact below 2^53, one rounding as here */
    r[0] = (float)(hi - lo);
    r[1] = (float)-lo;
    r[2] = (float)hi;
    r[3] = (float)half;
    return 1;
}

uint32_t tb_car_models_at(const uint32_t size[], int n, int k)
{
    uint64_t at = 24 * (uint64_t)n;
    for(int i = 0; i < k; i++)
        at += ((uint64_t)size[i] + 255) & ~(uint64_t)255;
    return at >> 32 ? 0 : (uint32_t)at;
}

uint32_t tb_car_models(const uint8_t *const x3d[], const uint32_t size[], int n,
                       uint8_t *out, uint32_t cap)
{
    uint32_t end = n < 1 ? 0 : tb_car_models_at(size, n, n);
    if(end == 0 || end > cap) return 0;
    uint32_t at = 24 * (uint32_t)n;
    for(int k = 0; k < n; k++) {
        float r[4];
        if(!car_model(x3d[k], size[k], r)) return 0;
        for(int i = 0; i < 4; i++) wrf(out + 24 * k + 4 * i, r[i]);
        wr32(out + 24 * k + 16, at);
        wr32(out + 24 * k + 20, size[k]);
        if(out + at != x3d[k]) memmove(out + at, x3d[k], size[k]);
        uint32_t pad = (uint32_t)(-size[k] & 255);
        memset(out + at + size[k], 0, pad);
        at += size[k] + pad;
    }
    return end;
}

uint32_t tb_backdrop(const uint8_t *bck, uint32_t size, uint8_t *out, uint32_t cap)
{
    if(size < 128 || cap < TB_BACKDROP_SIZE) return 0;
    if(bck[0] != 10 || bck[2] != 1 || bck[3] != 8 || bck[65] != 1) return 0;
    int32_t w = (int32_t)rd16(bck + 8) - (int32_t)rd16(bck + 4) + 1;
    int32_t h = (int32_t)rd16(bck + 10) - (int32_t)rd16(bck + 6) + 1;
    uint32_t line = rd16(bck + 66);         /* bytes per line, the padding after w left out */
    if(w != TB_BACKDROP_W || h != TB_BACKDROP_H || line < TB_BACKDROP_W) return 0;
    uint8_t *row = out, *end = out + TB_BACKDROP_W * TB_BACKDROP_H;
    uint32_t p = 128, x = 0;
    while(row < end) {                      /* a run may go on into the next line */
        if(p >= size) return 0;
        uint32_t c = bck[p++], n = 1;
        if(c >= 0xC0) {
            if(p >= size) return 0;
            n = c & 0x3F;
            c = bck[p++];
        }
        for(; n > 0 && row < end; n--) {
            if(x < TB_BACKDROP_W) row[x] = (uint8_t)c;
            if(++x == line) {
                x = 0;
                row += TB_BACKDROP_W;
            }
        }
    }
    memcpy(end, end - TB_BACKDROP_W, TB_BACKDROP_W);
    return TB_BACKDROP_SIZE;
}
