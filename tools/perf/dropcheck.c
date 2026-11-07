/*
 * dropcheck - why is a placement drawn from one camera and not from the next?
 *
 * track.c is included, so that its static data are in reach. The program runs the cull of draw()
 * for each of the cameras it is given, placement by placement, and tells for each placement why it
 * was left out. The states are:
 *   W  its cell is outside the game's window (cell_window, with the camera cell's box)
 *   R  out of reach (d2 >= reach2)
 *   1  behind the near plane         2  beyond the far plane
 *   3  right of the view             4  left of the view
 *   5  above the view                6  below the view
 *   V  drawn
 * A block that has no mesh at the level of detail of its distance still shows as V: the program
 * does not look at the level of detail.
 * Not part of the firmware. tools/perf/framestats.sh builds it in the counting copy, like walkcheck
 * (the build command), and runs it (the drop command).
 *
 * Usage: dropcheck <track image> <lines of the screen's height> [radius] < cameras
 *   The cameras are lines "x y h yaw pitch" on the standard input: the mark lines of debug.txt.
 *   Yaw and pitch are in radians; x, y and h in units.
 * Output: for each placement within the radius (default 30000 units) of any camera whose states
 *   differ between the cameras: "P index object x y h rad | state camera 1, camera 2, ..." and the
 *   distances of the placement from the cameras.
 * Environment: DROP_ALL lists every placement near a camera, also those whose states do not differ.
 *   DROP_ROWS and DROP_DRAW add the two checks described below.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "hw.h"
#include "track.c"

#define TEX_FILE    ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000))
#define MAXCAM      16

int main(int argc, char **argv)
{
    int fd = memfd_create("sdram", 0);
    if (argc < 3 || fd < 0 || ftruncate(fd, SDRAM_SIZE)
        || mmap((void *)(uintptr_t)SDRAM_BASE, SDRAM_SIZE, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED
        || mmap((void *)(uintptr_t)SDRAM_UBASE, SDRAM_SIZE, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
        perror("map");
        return 1;
    }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 1; }
    fread(TEX_FILE, 1, 0x700000, fp);
    fclose(fp);
    if (!track_open(TEX_FILE) || !track_init()) { fprintf(stderr, "track image not usable\n"); return 1; }
    const int lines = atoi(argv[2]);
    const float radius = argc > 3 ? (float)atof(argv[3]) : 30000.0f;
    struct track_cam cams[MAXCAM];
    int nc = 0;
    while (nc < MAXCAM && scanf("%f %f %f %f %f", &cams[nc].x, &cams[nc].y, &cams[nc].h,
                                &cams[nc].yaw, &cams[nc].pitch) == 5) {
        cams[nc].roll = 0.0f;
        nc++;
    }
    /* Each placement's cell, and for each camera the window and the cell's state. */
    static int in_win[MAXCAM][MAX_PIECES];
    static int cell_of[MAX_PIECES];
    for (int g = 0; g < nvcells; g++)
        for (int i = vcells[g].first; i < vcells[g].first + vcells[g].count; i++) cell_of[i] = g;
    const struct rs_clip clip = { 0, 0, FB_W, lines };
    const float far_eye = far_z + EYE_BACK;
    const float half_w = (float)(clip.x1 - clip.x0) / 2.0f / FOCAL;
    const int eye = EYE_LINE(clip.y1 - clip.y0);
    const float up = (float)eye / FOCAL;
    const float down = (float)(clip.y1 - clip.y0 - eye) / FOCAL;
    for (int k = 0; k < nc; k++) {
        int row0, row1, col0, col1;
        cell_window(cams[k].x, cams[k].y, cams[k].yaw, &row0, &row1, &col0, &col1);
        printf("camera %d: %.0f %.0f %.0f yaw %.4f pitch %.4f: window rows %d..%d columns %d..%d (far %.0f reach %.0f)\n",
               k + 1, cams[k].x, cams[k].y, cams[k].h, cams[k].yaw, cams[k].pitch,
               row0, row1, col0, col1, far_z, sqrtf(reach2));
        for (int i = 0; i < npieces; i++) {
            int row = vcells[cell_of[i]].key >> 8, col = vcells[cell_of[i]].key & 255;
            in_win[k][i] = row >= row0 && row <= row1 && col >= col0 && col <= col1;
        }
    }
    int count_v[MAXCAM] = { 0 }, count_near[MAXCAM] = { 0 };
    for (int i = 0; i < npieces; i++) {
        const struct cpiece *pc = &cpieces[i];
        int32_t no_mesh;
        memcpy(&no_mesh, &pc->rad, sizeof no_mesh);
        if (no_mesh < 0) continue;
        char st[MAXCAM][3];
        int near_any = 0, differ = 0;
        float dist[MAXCAM], depth[MAXCAM], side[MAXCAM];
        for (int k = 0; k < nc; k++) {
            float f[3], r[3], u[3];
            cam_frame(&cams[k], f, r, u);
            const float cx = cams[k].x, cy = cams[k].y, ch = cams[k].h;
            float rad = pc->rad, dx = pc->x - cx, dy = pc->y - cy, dh = pc->h - ch;
            float d2 = dx * dx + dy * dy + dh * dh;
            dist[k] = sqrtf(d2);
            depth[k] = dx * f[0] + dy * f[1] + dh * f[2];
            side[k] = dx * r[0] + dy * r[1] + dh * r[2];
            if (dist[k] < radius) near_any = 1;
            float vf = dx * f[0] + dy * f[1] + dh * f[2] + EYE_BACK, vr = dx * r[0] + dy * r[1] + dh * r[2];
            float vu = dx * u[0] + dy * u[1] + dh * u[2];
            const char *s = "V";
            if (!in_win[k][i]) s = "W";
            else if (d2 >= reach2) s = "R";
            else if (vf + rad < NEAR) s = "1";
            else if (vf - rad > far_eye) s = "2";
            else if (vr - rad > half_w * (vf + rad)) s = "3";
            else if (vr + rad < -half_w * (vf + rad)) s = "4";
            else if (vu - rad > up * (vf + rad)) s = "5";
            else if (vu + rad < -down * (vf + rad)) s = "6";
            else {
                const uint8_t *blk = track_lod_block(unr_blob + unr_off[pieces[i].obj], 0, dist[k], 0);
                (void)blk;
                s = "V";
            }
            strcpy(st[k], s);
            count_v[k] += s[0] == 'V';
            count_near[k] += s[0] == 'V' && dist[k] < 12000.0f;
            if (k && strcmp(st[k], st[0])) differ = 1;
        }
        if (!near_any || (!differ && !getenv("DROP_ALL"))) continue;
        printf("P %d obj %d at %d %d %d rad %.0f |",
               i, pieces[i].obj, pieces[i].x, pieces[i].y, pieces[i].h, pc->rad);
        for (int k = 0; k < nc; k++) printf(" %s", st[k]);
        printf(" | dist");
        for (int k = 0; k < nc; k++) printf(" %.0f", dist[k]);
        printf(" | ahead, right");
        for (int k = 0; k < nc; k++) printf(" %.0f,%.0f", depth[k], side[k]);
        printf("\n");
    }

    for (int k = 0; k < nc; k++)
        printf("camera %d: %d placements pass the cull, %d of them within 12000 units\n",
               k + 1, count_v[k], count_near[k]);
    /* DROP_ROWS: the camera moves from the first to the last of the cameras in many steps. For each
       placement that passes the cull, the check finds the face row that the view picks (face_eye) and
       whether that row draws anything: gu_draw leaves a piece out when the row's offset is 0 or the
       block has more than GU_MAX_VERT vertices. A run of steps where that happens shows as a building
       that is gone. */
    if (getenv("DROP_ROWS") && nc >= 2) {
        const int steps = 600;
        for (int i = 0; i < npieces; i++) {
            const struct cpiece *pc = &cpieces[i];
            int32_t no_mesh;
            memcpy(&no_mesh, &pc->rad, sizeof no_mesh);
            if (no_mesh < 0) continue;
            int bad_from = -1, last_bad = -1, nbad = 0, near_steps = 0;
            int rows_seen = 0;
            char what[64] = "";
            for (int st = 0; st <= steps; st++) {
                float t = (float)st / (float)steps;
                struct track_cam c = cams[0];
                const struct track_cam *a = &cams[0], *b = &cams[nc - 1];
                c.x = a->x + (b->x - a->x) * t;
                c.y = a->y + (b->y - a->y) * t;
                c.h = a->h + (b->h - a->h) * t;
                float dx = pc->x - c.x, dy = pc->y - c.y, dh = pc->h - c.h;
                if (sqrtf(dx * dx + dy * dy + dh * dh) > 12000.0f) continue;
                near_steps++;
                const uint8_t *obj = unr_blob + unr_off[pieces[i].obj];
                const uint8_t *blk = obj + ld32s(obj + 0x18);
                float ca = sin2048((uint32_t)pieces[i].t + 512), sa = sin2048((uint32_t)pieces[i].t);
                struct gu_piece g;
                memset(&g, 0, sizeof g);
                face_eye(blk, -dx * ca - dy * sa, -dh, dx * sa - dy * ca, &g);
                int32_t ro = ld32s(blk + 0x28 + 4 * g.row), nvert = ld32s(blk + 0x10);
                rows_seen |= 1 << g.row;
                int bad = ro <= 0 || nvert > GU_MAX_VERT || nvert < 0;
                if (bad) {
                    if (bad_from < 0) {
                        bad_from = st;
                        snprintf(what, sizeof what, "row %d offset %d vertices %d",
                                 g.row, (int)ro, (int)nvert);
                    }
                    last_bad = st;
                    nbad++;
                }
            }
            if (nbad && near_steps)
                printf("ROWS placement %d obj %d at %d %d %d rad %.0f: %d of %d near steps draw nothing (steps %d..%d of %d, %s), rows seen 0x%04x\n",
                       i, pieces[i].obj, pieces[i].x, pieces[i].y, pieces[i].h, pc->rad, nbad, near_steps,
                       bad_from, last_bad, steps, what, rows_seen);
        }
    }

    /* DROP_DRAW: for each placement that passes the cull within 15000 units of a camera, the check
       finds what draw() would hand on and what gu_draw (the C model) then draws from it: faces,
       vertices and pixels, summed over the placement's blocks. A piece that draws less (or nothing)
       from one camera than from the next shows as gone. */
    if (getenv("DROP_DRAW")) {
        static uint8_t scratch[FB_W * 200];
        for (int i = 0; i < npieces; i++) {
            const struct cpiece *pc = &cpieces[i];
            int32_t no_mesh;
            memcpy(&no_mesh, &pc->rad, sizeof no_mesh);
            if (no_mesh < 0) continue;
            uint32_t faces[MAXCAM], verts[MAXCAM], pixels[MAXCAM];
            int pass[MAXCAM], any = 0;
            for (int k = 0; k < nc; k++) {
                faces[k] = verts[k] = pixels[k] = 0;
                pass[k] = 0;
                float f[3], r[3], u[3];
                cam_frame(&cams[k], f, r, u);
                const float cx = cams[k].x, cy = cams[k].y, ch = cams[k].h;
                float rad = pc->rad, dx = pc->x - cx, dy = pc->y - cy, dh = pc->h - ch;
                float d2 = dx * dx + dy * dy + dh * dh;
                if (d2 > 15000.0f * 15000.0f) continue;
                float vf = dx * f[0] + dy * f[1] + dh * f[2] + EYE_BACK,
                      vr = dx * r[0] + dy * r[1] + dh * r[2];
                float vu = dx * u[0] + dy * u[1] + dh * u[2];
                if (!in_win[k][i] || d2 >= reach2 || vf + rad < NEAR || vf - rad > far_eye ||
                    vr - rad > half_w * (vf + rad) || vr + rad < -half_w * (vf + rad) ||
                    vu - rad > up * (vf + rad) || vu + rad < -down * (vf + rad)) continue;
                pass[k] = 1;
                any = 1;
                struct su_view view;
                track_view(&clip, &view);
                struct gu_stats gst = { 0, 0, 0, 0 };
                struct gu_piece g, gs[MAX_BLOCKS];
                int ng = 0;
                float ca = sin2048((uint32_t)pieces[i].t + 512), sa = sin2048((uint32_t)pieces[i].t);
                const float c3[3] = { cx, cy, ch };
                if (multi[pc ? pieces[i].obj : 0]) {
                    ng = multi_pieces(c3, f, r, u, &pieces[i], gs);
                } else {
                    memset(&g, 0, sizeof g);
                    g.phase = piece_phase;
                    g.blk = block_of(pieces[i].obj, &g.obj);
                    int drop = 0;
                    if (lod_inv != 0.0f) {
                        float lx = ld16s(g.blk + 4), ly = ld16s(g.blk + 6), lz = ld16s(g.blk + 8);
                        float wx = dx + lx * ca - lz * sa, wy = dy + lx * sa + lz * ca, wh = dh + ly;
                        g.blk = track_lod_block(g.obj, 0, sqrtf(wx * wx + wy * wy + wh * wh), 0);
                        drop = !g.blk;
                    }
                    if (!drop) {
                        const float m[9] = { r[0] * ca + r[1] * sa, r[2], r[1] * ca - r[0] * sa,
                                             u[0] * ca + u[1] * sa, u[2], u[1] * ca - u[0] * sa,
                                             f[0] * ca + f[1] * sa, f[2], f[1] * ca - f[0] * sa };
                        const float o[3] = { dx * r[0] + dy * r[1] + dh * r[2],
                                             dx * u[0] + dy * u[1] + dh * u[2],
                                             dx * f[0] + dy * f[1] + dh * f[2] + EYE_BACK };
                        for (int q = 0; q < 9; q++) g.m[q] = (int32_t)(m[q] * 65536.0f);
                        for (int q = 0; q < 3; q++) g.t[q] = (int32_t)(o[q] * 256.0f);
                        face_eye(g.blk, -dx * ca - dy * sa, -dh, dx * sa - dy * ca, &g);
                        gs[0] = g;
                        ng = 1;
                    }
                }
                for (int b = 0; b < ng; b++)
                    pixels[k] += gu_draw((volatile uint8_t *)scratch, &clip, &view, mlb, &gs[b], &gst);
                faces[k] = gst.faces;
                verts[k] = gst.vertices;
            }
            int differ = 0;
            for (int k = 1; k < nc; k++) differ |= (faces[k] != faces[0]) || (pass[k] != pass[0]);
            if (!any || !differ) continue;
            printf("DRAW placement %d obj %d at %d %d %d rad %.0f |",
                   i, pieces[i].obj, pieces[i].x, pieces[i].y, pieces[i].h, pc->rad);
            for (int k = 0; k < nc; k++)
                printf(" [%s faces %u verts %u px %u]", pass[k] ? "cull ok" : "culled", faces[k], verts[k],
                       pixels[k]);
            printf("\n");
        }
    }
    return 0;
}
