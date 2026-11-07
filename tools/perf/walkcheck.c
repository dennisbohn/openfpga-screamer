/*
 * The firmware's cell window and cell table against the original's walk (tools/perf/walkcheck.py): track.c itself
 * (included, so that its statics are in reach) gives, for a camera, the rows and columns of the cells it looks at
 * (cell_window) and, from the image's cell table, the placements of those cells whose place is within the reach
 * with the game's own integer test (0x83164: dist^2 >> 2 < 676000000, 64 bits), in the order the game lists them.
 * Not part of the firmware. Built by tools/perf/walkcheck.py in the counting copy of the firmware.
 *
 * Usage: walkcheck <track image> < lines "x height z yaw" (the view's position, the camera's heading in radians)
 * Output per line: "W row0 row1 col0 col1" and one "P object x height z" for each placement in reach.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "hw.h"
#include "track.c"

#define TEX_FILE    ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000))

int main(int argc, char **argv)
{
    int fd = memfd_create("sdram", 0);
    if (argc < 2 || fd < 0 || ftruncate(fd, SDRAM_SIZE)
        || mmap((void *)(uintptr_t)SDRAM_BASE, SDRAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED
        || mmap((void *)(uintptr_t)SDRAM_UBASE, SDRAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
        perror("map");
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fread(TEX_FILE, 1, 0x700000, f);
    fclose(f);
    if (!track_open(TEX_FILE) || !track_init()) { fprintf(stderr, "track image not usable\n"); return 1; }
    int x, h, z;
    float yaw;
    while (scanf("%d %d %d %f", &x, &h, &z, &yaw) == 4) {
        int row0, row1, col0, col1;
        cell_window((float)x, (float)z, yaw, &row0, &row1, &col0, &col1);
        printf("W %d %d %d %d\n", row0, row1, col0, col1);
        for (int g = 0; g < nvcells; g++) {
            int row = vcells[g].key >> 8, col = vcells[g].key & 255;
            if (row < row0 || row > row1 || col < col0 || col > col1) continue;
            for (int i = vcells[g].first; i < vcells[g].first + vcells[g].count; i++) {
                int64_t dx = pieces[i].x - x, dy = pieces[i].y - z, dh = pieces[i].h - h;
                if ((int32_t)(uint32_t)((dx * dx + dy * dy + dh * dh) >> 2) < 676000000)       /* (the low 32 bits, signed: jge) */
                    printf("P %d %d %d %d\n", pieces[i].obj, pieces[i].x, pieces[i].h, pieces[i].y);
            }
        }
    }
    return 0;
}
