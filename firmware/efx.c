/*
 * The effects through MAPn.SMG (efx.h): the table, the meshes, the radar's quad.
 */
#include "efx.h"
#include "dat.h"
#include "hw.h"
#include "raster.h"


#define SMG_SIZE    16384               /* 64 rows of 256 palette indices */
#define SMG_RADAR   63                  /* the row the radar's texels pick */
#define SMG_GLASS   62                  /* routine 192's: no see-through texture of a track uses 48..62 */
#define MESH_MAX    0x2000

/* where things lie in mem */
#define AT_SMG      0x0000              /* MAPn.SMG with the radar's row, kept for the C models */
#define AT_SHADOW   0x4000
#define AT_SMOKE    0x6000
#define AT_RADAR    0x8000              /* the radar's mesh (0xE0 bytes) */
#define AT_TEX      0x8400              /* its texture: 4 x 4 texels 63, rows of 256 bytes */
#define TEX_SIZE    4
#define AT_GLASS    0x8800              /* routine 192's texture: texels 62, rows of 256 bytes */
#define AT_GLASS_REC 0x9000             /* its texture record */
/* its texture record and material: the last 32 bytes of mem */

/* the radar's mesh: one block that is its own object (vertex and texture coordinate offsets
   from it as from an object), every face row the one face */
#define R_VERTS     0x80
#define R_UVS       0x98
#define R_ROW       0xB0
#define R_FACE      0xC0
#define R_SIDE      (72 * 16)           /* the window's side in 1/16 pixel: the vertices' unit */

uint32_t efx_cyc_objects, efx_cyc_radar, efx_radar_lines;

static const uint8_t *shadow, *smoke, *radar, *smg_kept;
static int table;

static void wr16(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, int32_t v)
{
    wr16(p, v);
    wr16(p + 2, v >> 16);
}

static int32_t rd32(const uint8_t *p)
{
    return (int32_t)(p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

/* entry name of the container into out (at most max bytes): its size, 0 if it is missing */
static uint32_t entry(const uint8_t *gfx, uint32_t size, const char *name, uint8_t *out, uint32_t max)
{
    struct dat_entry e;
    return dat_find(gfx, size, name, &e) ? dat_extract(gfx, size, &e, out, max) : 0;
}

/* a .3DX file whose first block lies inside it */
static const uint8_t *mesh(const uint8_t *gfx, uint32_t size, const char *name, uint8_t *out)
{
    uint32_t n = entry(gfx, size, name, out, MESH_MAX);
    if(n < 0x340 || rd32(out + 0x14) < 1) return 0;
    int32_t blk = rd32(out + 0x18);
    return blk > 0 && (uint32_t)blk + 0x80 <= n ? out : 0;
}

/* The radar's darkening (0x25870, the first of its two tables: factor 0.75 at 0x2587b; the
   second, 0.6, is not the radar's): for each colour the first palette entry nearest to it at
   0.75 of its brightness, in the 6-bit values of MAPn.COL (the track file has them times 4),
   squared distances in floats from 22100000 down (0x259ad) as the game; its clamp at 255
   never bites on 6-bit values */
static void darken(const uint32_t *pal, uint8_t out[256], float f)
{
    static float v[256][3];
    for(int i = 0; i < 256; i++)
        for(int k = 0; k < 3; k++) v[i][k] = (float)((pal[i] >> (16 - 8 * k) & 255) >> 2);
    for(int c = 0; c < 256; c++) {
        float tr = v[c][0] * f, tg = v[c][1] * f, tb = v[c][2] * f, best = 22100000.0f;
        for(int i = 0; i < 256; i++) {
            float er = v[i][0] - tr, eg = v[i][1] - tg, eb = v[i][2] - tb, d = er * er + eg * eg + eb * eb;
            if(d < best) {
                best = d;
                out[c] = (uint8_t)i;
            }
        }
    }
}

/* The radar's quad: corners (0, 0), (R_SIDE, 0), (R_SIDE, -R_SIDE), (0, -R_SIDE) (x right, y
   up), the normal towards +z, the whole texture over it; material and texture record in the
   last 32 bytes of mem, which must lie within a face's reach of the MLB */
static const uint8_t *radar_mesh(uint8_t *m, uint8_t *tex, uint8_t *end, const uint8_t *mlb)
{
    uint8_t *rec = end - 32, *mat = end - 16;
    int32_t off = (int32_t)(mat - mlb);
    if(off < -32768 || off > 32767) return 0;
    for(int i = 0; i < 0x100; i++) m[i] = 0;
    for(int y = 0; y < TEX_SIZE; y++)
        for(int x = 0; x < TEX_SIZE; x++) tex[256 * y + x] = SMG_RADAR;
    wr32(rec, 1);                                       /* frames */
    wr32(rec + 4, SMG_RADAR);
    wr32(rec + 8, (int32_t)(tex - mlb));                /* frame 0 */
    wr32(mat, 224);                                     /* the routine */
    wr32(mat + 4, (int32_t)(rec - mlb));
    wr32(mat + 8, TEX_SIZE);
    wr32(mat + 12, TEX_SIZE);

    wr32(m, R_SIDE);                                    /* (the bounding radius) */
    wr32(m + 0x10, 4);                                  /* vertices */
    wr32(m + 0x14, 1);                                  /* faces */
    wr32(m + 0x1C, R_VERTS);
    wr32(m + 0x24, R_UVS);
    for(int k = 0; k < 16; k++) wr32(m + 0x28 + 4 * k, R_ROW);
    static const int16_t xy[4][2] = { { 0, 0 }, { R_SIDE, 0 }, { R_SIDE, -R_SIDE }, { 0, -R_SIDE } };
    static const int16_t uv[4][2] = { { 0, 0 }, { 255, 0 }, { 255, 255 }, { 0, 255 } };
    for(int i = 0; i < 4; i++) {
        wr16(m + R_VERTS + 6 * i, xy[i][0]);
        wr16(m + R_VERTS + 6 * i + 2, xy[i][1]);
        wr16(m + R_UVS + 6 * i, uv[i][0]);
        wr16(m + R_UVS + 6 * i + 2, uv[i][1]);
        wr16(m + R_FACE + 8 + 2 * i, 6 * i);            /* the corners' vertex offsets */
    }
    wr16(m + R_ROW, R_FACE);
    wr16(m + R_ROW + 2, 0xFFFF);
    wr16(m + R_FACE + 4, off);                          /* the material */
    wr16(m + R_FACE + 6, 4);                            /* corners */
    wr32(m + R_FACE + 0x1C, 16383);                     /* the normal (0, 0, 1) */
    return m;
}

/* Routine 192 (0xcae84; track 5's station): one colour that is not drawn, each pixel under the
   face taken through the second table 0x25870 builds, 0.6 of its brightness ([0xbcb1c] =
   0x15cbc + 0x100, the column loop 0xccad0) -- tinted glass. Drawn as a see-through face
   (routine 224) whose texels all pick the table's row SMG_GLASS: each such material of the
   track's MLB (the table up to its first texture record) is turned into one over the
   texture here */
static void glass(uint8_t *tex, uint8_t *rec, const uint8_t *mlb)
{
    for(int y = 0; y < 2 * TEX_SIZE; y++)
        for(int x = 0; x < 256; x++) tex[256 * y + x] = SMG_GLASS;
    wr32(rec, 1);                                       /* frames */
    wr32(rec + 4, SMG_GLASS);
    wr32(rec + 8, (int32_t)(tex - mlb));                /* frame 0 */
    uint8_t *m = (uint8_t *)mlb;
    int32_t first = 0x7FFFFFFF;
    for(int32_t k = 0; 16 * (k + 1) <= first; k++) {
        int32_t routine = rd32(m + 16 * k), ro = rd32(m + 16 * k + 4);
        if(routine == 192) {
            wr32(m + 16 * k, 224);
            wr32(m + 16 * k + 4, (int32_t)(rec - mlb));
            wr32(m + 16 * k + 8, TEX_SIZE);
            wr32(m + 16 * k + 12, TEX_SIZE);
        } else if(ro > 0 && ro < first)
            first = ro;
    }
}

int efx_init(const uint8_t *gfx, uint32_t gfx_size, int n, const uint8_t *mlb, const uint32_t *pal,
             uint8_t *mem, uint32_t cap)
{
    char name[16] = "MAPn.SMG";
    name[3] = (char)('0' + n);
    shadow = smoke = radar = 0;
    table = 0;
    if(cap < AT_TEX + 256 * TEX_SIZE + 32) return 0;
    uint8_t *smg = mem + AT_SMG;
    smg_kept = smg;
    int ok = table = entry(gfx, gfx_size, name, smg, SMG_SIZE) == SMG_SIZE;
    if(ok) {
        darken(pal, smg + SMG_RADAR * 256, 0.75f);
        darken(pal, smg + SMG_GLASS * 256, 0.6f);
        glass(mem + AT_GLASS, mem + AT_GLASS_REC, mlb);
        for(uint32_t a = 0; a < SMG_SIZE; a++)          /* nothing blended is drawn now */
            RS_SMG = a << 8 | smg[a];
        rs_smg = smg;                                   /* the C models' copy (raster.h) */
        radar = radar_mesh(mem + AT_RADAR, mem + AT_TEX, mem + cap, mlb);
    }
    shadow = mesh(gfx, gfx_size, "SHADOW.3DX", mem + AT_SHADOW);
    smoke = mesh(gfx, gfx_size, "SMOKE.3DX", mem + AT_SMOKE);
    while(!(MEM_STATUS & MEM_WRITES_IDLE))              /* all of it in the SDRAM for the 3D units */
        ;
    return ok && radar && shadow && smoke;
}

int efx_table(void)
{
    return table;
}

const uint8_t *efx_smg(void)
{
    return table ? smg_kept : 0;
}

const uint8_t *efx_darken(void)
{
    return table ? smg_kept + SMG_RADAR * 256 : 0;
}

const uint8_t *efx_shadow(void)
{
    return shadow;
}

const uint8_t *efx_smoke(void)
{
    return smoke;
}

/* The window's corners at a quarter pixel in from its edges' lines (3.75 .. 75.75, 124.25 ..
   196.25): the same pixels with the rasterizer's rule (a pixel whose centre is inside) and
   with the game's for routine 224 (the point below and right of the pixel's corner, 0xcd97a),
   whichever the blend draws with. Depth 256: 1/16 pixel there is 256 / scale view units. */
int efx_radar_piece(const struct su_view *v, struct gu_piece *g)
{
    if(!radar) return 0;
    const int32_t f = 256, x16 = EFX_RADAR_X0 * 16 - 4, y16 = EFX_RADAR_Y0 * 16 + 4;
    int32_t per = (int32_t)(((int64_t)f << 16) / v->scale);    /* view units per 1/16 pixel, 16 fraction bits */
    for(int k = 0; k < 9; k++) g->m[k] = 0;
    g->m[0] = per;
    g->m[4] = per;
    g->m[8] = 65536;
    g->t[0] = (int32_t)(((int64_t)(x16 - v->cx) * f << 8) / v->scale);
    g->t[1] = (int32_t)(((int64_t)(v->cy - y16) * f << 8) / v->scale);
    g->t[2] = f << 8;
    g->e[0] = g->e[1] = 0;
    g->e[2] = -(f << 8);                                /* the eye on the -z side: the face is seen */
    g->row = 0;
    g->mirror = 0;
    g->phase = 0;
    g->obj = g->blk = radar;
    return 1;
}
