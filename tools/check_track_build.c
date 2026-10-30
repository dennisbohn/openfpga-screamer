/*
 * Host test of the track build on the Pocket (firmware/track_build.c), run by
 * tools/check_track_build.sh: the image from the five original containers, built as the
 * firmware builds it, compared byte for byte with the image tools/make_track.py made from
 * the same files. The output buffer starts out filled with junk, as the SDRAM is.
 *
 * Usage: check_track_build <game folder> <reference image> [n]     n: the track, default 1
 */
#include "../firmware/track_build.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAP         (7u << 20)      /* room for the image, as main.c gives it */
#define SCRATCH     0x2A0000u       /* and the scratch: BUILD_SCRATCH up to the program */

static uint8_t *slurp(const char *dir, const char *name, uint32_t *size)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if(!f) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)n + 1);
    if(!p || fread(p, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
    fclose(f);
    *size = (uint32_t)n;
    printf("%s: %u bytes\n", name, *size);
    return p;
}

int main(int argc, char **argv)
{
    if(argc != 3 && argc != 4) { fprintf(stderr, "usage: %s <game folder> <reference image> [n]\n", argv[0]); return 2; }
    int track = argc == 4 ? atoi(argv[3]) : 1;
    char gfx[32], data[32];
    snprintf(gfx, sizeof gfx, "GFX/MAP%dH.DAT", track);
    snprintf(data, sizeof data, "DATA/MAP%d.DAT", track);
    struct track_files f;
    f.gfx = slurp(argv[1], gfx, &f.gfx_size);
    f.game = slurp(argv[1], "GFX/GAME.DAT", &f.game_size);
    f.data = slurp(argv[1], data, &f.data_size);
    f.gen = slurp(argv[1], "DATA/GEN.DAT", &f.gen_size);
    f.local = slurp(argv[1], "DATA/LOCAL.DAT", &f.local_size);
    uint32_t ref_size;
    uint8_t *ref = slurp(".", argv[2], &ref_size);

    uint8_t *out = malloc(CAP), *scratch = malloc(SCRATCH);
    for(uint32_t k = 0; k < CAP; k++) out[k] = (uint8_t)(0xA5 ^ k ^ (k >> 11));
    memset(scratch, 0x5A, SCRATCH);
    struct track_build_info info;
    uint32_t size = track_build(&f, track, out, CAP, scratch, SCRATCH, &info);
    if(!size) { printf("FAIL: track_build stopped at \"%s\"\n", info.fail); return 1; }
    printf("built: %u bytes, reference: %u bytes\n", size, ref_size);

    /* the first difference, named by the reference's section that holds it */
    static const char *const names[] = { "MLB", "UNR", "palette", "placements", "camera path",
        "ground index", "markers", "cars", "car models", "road points", "walls", "race screen",
        "backdrop", "opponents" };
    static const uint32_t at[] = { 0x08, 0x10, 0x18, 0x1C, 0x24, 0x2C, 0x34, 0x3C, 0x44, 0x4C, 0x54,
        0x68, 0x78, 0x80 };
    uint32_t n = size < ref_size ? size : ref_size, differ = 0, first = 0;
    for(uint32_t k = 0; k < n; k++)
        if(out[k] != ref[k] && !differ++) first = k;
    if(differ) {
        const char *in = "header";
        for(int s = 0; s < 14; s++) {
            uint32_t o = ref[at[s]] | ref[at[s] + 1] << 8 | ref[at[s] + 2] << 16 | (uint32_t)ref[at[s] + 3] << 24;
            if(first >= o) in = names[s];
        }
        printf("%u bytes differ, the first at 0x%x (%s): 0x%02x, expected 0x%02x\n", differ, first, in,
               out[first], ref[first]);
    }
    if(differ || size != ref_size) { printf("FAIL\n"); return 1; }
    printf("PASS: byte-identical with the reference\n");
    return 0;
}
