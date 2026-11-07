"""Adds the counters of perf.h to COPIES of the firmware's track.c, car.c, geom.c, setup.c and raster.c.

Usage: python -I tools/perf/perf_patch.py <dir with the copies> [--ref]
  tools/perf/framestats.sh build copies the firmware to a directory and calls this script on it.
  --ref  the copies are of another tree (tools/perf/check_cull.sh), whose cull has other text. Only
         the placements tested and kept are counted, in the same words; the level of detail and the
         cells are not counted.
Every edit asserts that the text it replaces occurs exactly once. A changed firmware therefore
fails loudly instead of silently counting something else. The firmware files themselves are never
touched.
"""
import sys


def edit(path, pairs):
    """Replaces in the file each old text of pairs by its new text; stops if an old text is not found once."""
    t = open(path, newline="").read()
    for old, new in pairs:
        n = t.count(old)
        if n != 1:
            raise SystemExit(f"{path}: {old!r} found {n} times")
        t = t.replace(old, new)
    open(path, "w", newline="").write(t)


def main():
    """Applies all edits to the files in the directory given as the first argument."""
    d = sys.argv[1]
    ref = "--ref" in sys.argv
    cells = [
        ("    for(int g = q0; g < q1; g++) {\n",
         "    for(int g = q0; g < q1; g++) {\n"
         "        perf_c.cells_rows++;\n"),
        ("            if(col < col0 || col > col1) continue;\n",
         "            if(col < col0 || col > col1) continue;\n"
         "            perf_c.cells++;\n"),
        ("            const struct cpiece *pc = &cpieces[i];\n            int32_t no_mesh;",
         "            const struct cpiece *pc = &cpieces[i];\n            perf_c.tested++;\n"
         "            int32_t no_mesh;"),
        ("            if(no_mesh < 0) continue;\n",
         "            if(no_mesh < 0) continue;\n"
         "            perf_c.tested_mesh++;\n"),
        ("            vis[nv++] = (KEY(d2) + cpiece_key[i]) * 65536 + i;\n",
         "            vis[nv++] = (KEY(d2) + cpiece_key[i]) * 65536 + i;\n            perf_c.visible++;\n"),
        # The level of detail of the placed blocks: lod_s1 and lod_s2 count the levels taken, lod_sd the
        # blocks left out.
        ("            g.blk = track_lod_block(g.obj, 0, sqrtf(wx * wx + wy * wy + wh * wh), 0);\n"
         "            if(!g.blk) continue;\n",
         "            const uint8_t *b0 = g.blk;\n"
         "            g.blk = track_lod_block(g.obj, 0, sqrtf(wx * wx + wy * wy + wh * wh), 0);\n"
         "            if(!g.blk) { perf_c.lod_sd++; continue; }\n"
         "            if(g.blk != b0) { if(g.blk == g.obj + rd32(g.obj + 0x18 + 0x104)) "
         "perf_c.lod_s1++; else perf_c.lod_s2++; }\n"),
    ]
    # The reference tree (older than the cells: the original's groups of 16, or the groups of eight)
    # counts the placements it tests and keeps in the same words. Its loop is indented by 8 or 12.
    NL = chr(10)
    ref_group = []
    for ind in (8, 12):
        sp = " " * ind
        pairs = [
            (sp + "const struct cpiece *pc = &cpieces[i];" + NL + sp + "int32_t no_mesh;",
             sp + "const struct cpiece *pc = &cpieces[i];" + NL + sp + "perf_c.tested++;" + NL
             + sp + "int32_t no_mesh;"),
            (sp + "if(no_mesh < 0) continue;" + NL,
             sp + "if(no_mesh < 0) continue;" + NL + sp + "perf_c.tested_mesh++;" + NL),
            (sp + "vis[nv++] = (KEY(d2) + cpiece_key[i]) * 65536 + i;" + NL,
             sp + "vis[nv++] = (KEY(d2) + cpiece_key[i]) * 65536 + i;" + NL + sp + "perf_c.visible++;" + NL),
        ]
        if ref and open(f"{d}/track.c", newline="").read().count(pairs[0][0]) == 1:
            ref_group = pairs
    common = [
        ("    uint32_t t_pieces = now();\n",
         "    for(int k = 0; k < nv; k++) "
         "perf_c.vis_hash = (perf_c.vis_hash ^ (uint32_t)vis[k]) * 16777619u;\n"
         "    uint32_t t_pieces = now();\n"),
        ("        st->objects++;\n", "        st->objects++;\n        perf_c.obj_visible++;\n"),
        ("            if(ob->make) ob->make(ob->id, cam, ob);\n",
         "            if(ob->make) ob->make(ob->id, cam, ob);\n"
         "            perf_c.obj_blocks += (uint32_t)ob->n;\n"),
        ("            int nb = multi_pieces(c, f, r, u, pc, gs);\n",
         "            int nb = multi_pieces(c, f, r, u, pc, gs);\n"
         "            perf_c.multi_blocks += (uint32_t)nb;\n"),
    ]
    edit(f"{d}/track.c", (ref_group if ref else cells) + common)
    if not ref:
        edit(f"{d}/car.c", [
            ("        t->blk = track_lod_block(m->mesh, b, dist, 1);\n        if(!t->blk) continue;\n",
             "        t->blk = track_lod_block(m->mesh, b, dist, 1);\n"
             "        if(!t->blk) { perf_c.lod_cd++; continue; }\n"
             "        if(t->blk != q->blk) { "
             "if(t->blk == m->mesh + *(const int32_t *)(m->mesh + 0x18 + 4 * b + 0x104)) "
             "perf_c.lod_c1++; else perf_c.lod_c2++; }\n"),
        ])
    # perf_set_scales sets the far plane's and the level of detail's factors; framestats and racestats
    # call it for FARSCALE and LODSCALE.
    with open(f"{d}/track.c", "a", newline="") as fh:
        fh.write("\nvoid perf_set_scales(float far_s, float lod_s)\n{\n")
        if ref:
            fh.write("    (void)far_s;\n    (void)lod_s;\n")
        else:
            fh.write("    if(far_s > 0.0f) {\n        far_scale = far_s;\n        track_detail(2);\n    }\n")
            fh.write("    if(lod_s >= 0.0f) lod_inv = lod_s > 0.0f ? 1.0f / lod_s : 0.0f;\n")
        fh.write("}\n")
    edit(f"{d}/geom.c", [
        ("    st->vertices += (uint32_t)nvert;\n",
         "    st->vertices += (uint32_t)nvert;\n    perf_c.gu_pieces++;\n"
         "    perf_c.verts += (uint32_t)nvert;\n"
         "    perf_gu_rd(p->blk, 0x30);\n    perf_gu_rd(verts, (uint32_t)nvert * 6);\n"),
        ("        for(int i = 0; i < nvert && i < cnt; i++) reflection(p, vt, nrm + 4, i);\n",
         "        perf_c.refl_verts += (uint32_t)(nvert < cnt ? nvert : cnt);\n"
         "        perf_gu_rd(nrm, 4 + 12 * (uint32_t)cnt);\n"
         "        for(int i = 0; i < nvert && i < cnt; i++) reflection(p, vt, nrm + 4, i);\n"),
        ("        const uint8_t *fc = p->blk + fo;\n",
         "        const uint8_t *fc = p->blk + fo;\n        perf_c.faces_row++;\n        perf_gu_rd(e, 2);\n"
         "        perf_gu_rd(fc, 0x20);\n"),
        ("        const uint8_t *c0 = verts + ru16(fc + 8);\n",
         "        const uint8_t *c0 = verts + ru16(fc + 8);\n"
         "        perf_gu_rd(c0, 6);\n"),
        ("+ bz * rd16(fc + 0x1C) < 0) != (p->mirror != 0)) continue;",
         "+ bz * rd16(fc + 0x1C) < 0) != (p->mirror != 0)) { perf_c.back++; continue; }"),
        ("        const uint8_t *mat = mlb + rd16(fc + 4);\n",
         "        const uint8_t *mat = mlb + rd16(fc + 4);\n"
         "        perf_gu_rd(mat, 16);\n"),
        ("            const uint8_t *trec = mlb + rd32(mat + 4);\n",
         "            const uint8_t *trec = mlb + rd32(mat + 4);\n"
         "            perf_gu_rd(trec, 12);\n"),
        ("            c[j].f = vt[vi % GU_MAX_VERT].f;\n",
         "            c[j].f = vt[vi % GU_MAX_VERT].f;\n"
         "            perf_gu_rd(uvs + vo, 4);\n"),
    ])
    edit(f"{d}/setup.c", [
        ("    if(all) return 0;                       /* wholly outside one plane */\n",
         "    perf_c.polys++;\n    if(all) { perf_c.clip_out++; return 0; }\n    if(any) perf_c.clip_any++;\n"
         "    if(any & 1) perf_c.clip_near++;\n    if(any & 32) perf_c.clip_far++;\n"),
    ])
    edit(f"{d}/raster.c", [
        ("    if(r0 >= r1) return 0;\n",
         "    if(r0 >= r1) return 0;\n"
         "    perf_c.walk_tris++;\n"
         "    perf_c.rows += (uint32_t)(r1 - r0);\n"),
        ("        if(x0 < x1) n += span(fb, r, x0, x1, ctx);\n",
         "        if(x0 < x1) { perf_c.spans++; perf_c.px_span += (uint32_t)(x1 - x0); "
         "n += span(fb, r, x0, x1, ctx); }\n"),
        ("        s.x += s.slope;\n    }\n    return n;\n}\n",
         "        s.x += s.slope;\n    }\n"
         "    perf_c.px_drawn += n;\n    return n;\n}\n"),
        ("    return walk(fb, clip, v, span_flat, &colour, 0);\n",
         "    perf_c.tris_flat++;\n"
         "    return walk(fb, clip, v, span_flat, &colour, 0);\n"),
        ("    struct texfx_ctx c = { s, tex };\n",
         "    struct texfx_ctx c = { s, tex };\n    perf_c.tris_tex++;\n"
         "    perf_c.tris_blend += tex->routine == 224;\n"),
    ])


if __name__ == "__main__":
    main()
