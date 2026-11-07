/*
 * perf.c - the counters and cache models declared in tools/perf/perf.h.
 *
 * The SDRAM byte address of a pointer is pointer - SDRAM_BASE: the PC maps the SDRAM at the
 * firmware's address, as tools/host/render.c does.
 */
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "hw.h"
#include "perf.h"

struct perf_counts perf_c;

#define TEX_LINES   512                     /* 16 KB / 32 */
#define GU_LINES    128                     /* 4 KB / 32 */
#define MAX_LINES   (1u << 21)              /* 64 MB of SDRAM in lines of 32 bytes */
static int32_t tex_tag[TEX_LINES], gu_tag[GU_LINES];
static uint8_t seen[MAX_LINES / 8];

void perf_reset(void)
{
    memset(&perf_c, 0, sizeof perf_c);
    for (int i = 0; i < TEX_LINES; i++) tex_tag[i] = -1;
    for (int i = 0; i < GU_LINES; i++) gu_tag[i] = -1;
    memset(seen, 0, sizeof seen);
}

/* Counts one texel read of the rasterizer (called from raster.c when RS_TRACE is defined). It counts a
   miss of the texture cache and the first read of each 32-byte line (tex_lines). */
void rs_trace(const volatile uint8_t *p)
{
    uint32_t a = (uint32_t)((uintptr_t)p - SDRAM_BASE), line = a >> 5;
    perf_c.texel_reads++;
    uint32_t i = line & (TEX_LINES - 1);
    if (tex_tag[i] != (int32_t)(line >> 9)) {
        tex_tag[i] = (int32_t)(line >> 9);
        perf_c.tex_miss++;
    }
    if (line < MAX_LINES && !(seen[line >> 3] >> (line & 7) & 1)) {
        seen[line >> 3] |= (uint8_t)(1u << (line & 7));
        perf_c.tex_lines++;
    }
}

/* Counts the geometry unit's reads of len bytes at p, one per 32-byte line, and the misses of its cache. */
void perf_gu_rd(const void *p, uint32_t len)
{
    if (len == 0) return;
    uint32_t a = (uint32_t)((uintptr_t)p - SDRAM_BASE);
    for (uint32_t line = a >> 5; line <= (a + len - 1) >> 5; line++) {
        uint32_t i = line & (GU_LINES - 1);
        perf_c.gu_reads++;
        if (gu_tag[i] != (int32_t)(line >> 7)) {
            gu_tag[i] = (int32_t)(line >> 7);
            perf_c.gu_miss++;
        }
    }
}

/* Writes the names of the counters as CSV column names, each after a comma. */
void perf_header(FILE *f)
{
#define X(n) fprintf(f, ",%s", #n);
    PERF_FIELDS(X)
#undef X
}

/* Writes the values of the counters as CSV fields, each after a comma. */
void perf_row(FILE *f)
{
#define X(n) fprintf(f, ",%u", perf_c.n);
    PERF_FIELDS(X)
#undef X
}
