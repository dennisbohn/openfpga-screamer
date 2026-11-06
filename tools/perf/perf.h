/*
 * Frame statistics for the PC renderer (docs/development.md, "Frame statistics"). Included into the
 * firmware's track.c, geom.c, setup.c and raster.c copies by tools/perf/perf_patch.py (gcc -include);
 * the firmware itself is not changed. Counters of what one frame asks of the CPU and the units, plus
 * two cache models fed with the addresses the C models read:
 *   the rasterizer's texture cache (16 KB, direct mapped, lines of 32 bytes: raster_tex.v)
 *   the geometry unit's read cache (4 KB, direct mapped, lines of 32 bytes: geom.v)
 */
#pragma once
#include <stdint.h>
#include <stdio.h>

#define PERF_FIELDS(X) \
    X(groups) X(grp_skipped) X(grp_out_view) X(tested) X(tested_mesh) X(visible) X(obj_visible) \
    X(obj_blocks) X(multi_blocks) \
    X(gu_pieces) X(verts) X(refl_verts) X(faces_row) X(back) \
    X(polys) X(clip_out) X(clip_any) X(clip_near) X(clip_far) \
    X(tris_flat) X(tris_tex) X(tris_blend) X(walk_tris) X(rows) X(spans) X(px_span) X(px_drawn) \
    X(texel_reads) X(tex_miss) X(tex_lines) X(gu_reads) X(gu_miss) \
    X(lod_s1) X(lod_s2) X(lod_sd) X(lod_c1) X(lod_c2) X(lod_cd) \
    X(vis_hash)

struct perf_counts {
#define X(n) uint32_t n;
    PERF_FIELDS(X)
#undef X
};

extern struct perf_counts perf_c;
void perf_reset(void);                      /* counters and both caches (a cold frame) */
void perf_gu_rd(const void *p, uint32_t len);   /* the geometry unit reads len bytes at p */
void perf_header(FILE *f);                  /* ",groups,grp_skipped,..." */
void perf_row(FILE *f);                     /* ",12,34,..." */
