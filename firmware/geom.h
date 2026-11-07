/*
 * The geometry unit (GU): a C model of core/src/fpga/core/geom.v. It does bit for bit what the
 * hardware does with one piece of the track, that is one placement of a mesh.
 *
 * Inputs: the CPU picks the pieces, sorts them far to near and hands each over as a gu_piece
 * with its place in view space. The unit reads the mesh itself, as it lies in the game files
 * (track.c has the layout): the UNR block, its vertices, the face row, the faces, the texture
 * coordinates and the MLB materials.
 * Outputs: the unit takes the vertices into view space, leaves out the faces that are turned
 * away and hands the others to the setup unit (setup.h) as polygons in fixed point.
 *
 * Number formats. "n fraction bits" means a value times 2^n. Every >> of a signed value
 * rounds down.
 *   - Mesh vertex: x, y, z are int16. The matrix m has 16 fraction bits. The piece's origin t
 *     and the eye e have 8.
 *   - Vertex in view space: r = t_r + ((x m_rx + y m_ry + z m_rz) >> 8). u and f are
 *     computed in the same way. The result has 8 fraction bits.
 *   - Facing test: a face is seen when ((c0 << 8) - e) . n >= 0, where c0 is its first corner,
 *     e the eye in the piece's frame (8 fraction bits) and n the face's int16 normal.
 *   - Texel position, 8 fraction bits: the raw u times the material's width and the raw v times
 *     its height (texel = raw * size / 256).
 *
 * Routine 160 (a car's windows) takes its texel from a reflection and not from the material
 * size: the reflection's u, v of the corner's vertex, in whole texels, wrapped at 256
 * (raster.h; original: STARTL.EXE 0xdb310). A block with flag 0x2000 (block +0x18) has a normal
 * for each vertex (block +0x20: a count, then float x, y, z for each vertex). For each vertex
 * i below the count the unit works out the reflection r = 2 (e.n) n - e, as the original does
 * on the x87 (original: STARTL.EXE 0xc2230, docs/original_geometry.md 8.5). Here e is the
 * direction from the camera to the vertex (the camera is EYE_BACK in front of the eye) and n
 * is the vertex's normal in view space. The unit keeps
 *     u = round(127 r.x) + 127 - offx        v = round(127 r.z) + 127 - offz
 * where x and z are the original's view axes. They are left and back, which are -r and -f
 * in this unit's view space.
 *
 * The hardware computes this in integers. Qn below means n fraction bits.
 *   q    each normal component from its float bits, rounded to Q14
 *   n    (m q) >> 14: the normal in view space, Q16
 *   p    the vertex less EYE_BACK in f (8 fraction bits), shifted left or right until the
 *        largest |component| (one's complement) has 16 bits; 17 bits signed
 *   S    (p.n) >> 16;  Q = p.p;  B = ((p << 16) - 2 S n) >> 16 for r and f. B is the
 *        reflection above with its sign reversed, so B_r is its x and B_f is its z.
 *   y    (127/128) 2^32 / sqrt(Q'), from a table of 128 entries and one Newton step. Q' is Q,
 *        or 4 Q when Q is below 2^32 (h = 1). With x = Q' >> 17:
 *        t1 = (x y0) >> 16, t2 = (t1 y0) >> 15, t3 = (3 2^16 - t2) >> 1,
 *        y = (y0 (t3 - (t3 >> 7))) >> 16, where y0 is the table entry.
 *   u    ((B_r y << h) + 2^24) >> 25 + 127 - offx;  v the same with B_f and offz
 * The result differs from the float form (GU_REFL_FLOAT, which computes it as the original
 * does) by one in about 1 % of the values.
 */
#pragma once
#include <stdint.h>
#include "raster.h"
#include "setup.h"

#define GU_MAX_VERT     512             /* vertices of a mesh; with more, the piece is left out */
#define GU_MAX_FACES    1024            /* faces of a row; the rest of the row is left out */

struct gu_piece {
    int32_t m[9];                       /* local to view: rows r, u, f of columns x, y, z; 16 fraction bits */
    int32_t t[3];                       /* the piece's origin in view space, 8 fraction bits */
    int32_t e[3];                       /* the eye in the piece's frame, 8 fraction bits */
    int row;                            /* the face row for the side the eye is on, 0..15 (track.c) */
    int mirror;                         /* A moving part's block: its frames turn the model's z up into
                                           y up, which is a mirror, so the faces turned away are the
                                           other ones (original: STARTL.EXE 0x8acf0, jge instead of jl). */
    uint32_t phase;                     /* texture frame of n: n x phase >> 14 (14 bits) */
    uint32_t refl;                      /* Routine 160. Bits 15..0: the reflection picture's phase
                                           (frame n x phase >> 16; the object's record +0x3c,
                                           original: STARTL.EXE 0x84288). Bits 23..16 and 31..24:
                                           the reflection's offsets of u and v (original:
                                           STARTL.EXE 0xc1fc0; track.c). */
    const uint8_t *obj, *blk;           /* the mesh: the object and its block */
};

struct gu_stats { uint32_t faces, triangles, see_through, vertices; };

/* Sends one piece through the models of the geometry unit, the setup unit and the rasterizer
   into fb. mlb is the track's material file. Adds to *st and returns the pixels drawn. */
uint32_t gu_draw(volatile uint8_t *fb, const struct rs_clip *clip, const struct su_view *v,
                 const uint8_t *mlb, const struct gu_piece *p, struct gu_stats *st);
