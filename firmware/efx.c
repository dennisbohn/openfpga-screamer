/*
 * The effects through MAPn.SMG (see efx.h): loads the table and the meshes, builds the radar's
 * quad and the darkening tables.
 * Entry points: efx_init() at the track's start, efx_radar_piece() for the radar's quad, and
 * the getters efx_shadow(), efx_smoke(), efx_darken() and efx_table().
 */
#include "efx.h"
#include "bytes.h"
#include "dat.h"
#include "hw.h"
#include "raster.h"


#define SMG_SIZE    16384               /* 64 rows of 256 palette indices */
#define SMG_RADAR   63                  /* the row the radar's texels pick */
#define SMG_GLASS   62                  /* routine 192's row; no see-through texture of a track uses 48..62 */
#define MESH_MAX    0x2000

/* Where things lie in mem (byte offsets). */
#define AT_SMG      0x0000              /* MAPn.SMG with the radar's row, kept for the C models */
#define AT_SHADOW   0x4000
#define AT_SMOKE    0x6000
#define AT_RADAR    0x8000              /* the radar's mesh (0xE0 bytes) */
#define AT_TEX      0x8400              /* its texture: 4 x 4 texels 63, rows of 256 bytes */
#define TEX_SIZE    4
#define AT_GLASS    0x8800              /* routine 192's texture: texels 62, rows of 256 bytes */
#define AT_GLASS_REC 0x9000             /* its texture record */
/* The radar's texture record and material are the last 32 bytes of mem. */

/* The radar's mesh is one block that is its own object, so the vertex and texture coordinate
   offsets count from the block. Every face row holds the one face. */
#define R_VERTS     0x80
#define R_UVS       0x98
#define R_ROW       0xB0
#define R_FACE      0xC0
#define R_SIDE      (72 * 16)           /* the window's side in 1/16 pixel: the vertices' unit */

uint32_t efx_cyc_objects, efx_cyc_radar, efx_radar_lines;

static const uint8_t *shadow, *smoke, *radar, *smg_kept;
static int table;

/* Extracts the entry `name` of the container into out (at most max bytes). Returns its size,
   or 0 if it is missing. */
static uint32_t entry(const uint8_t *gfx, uint32_t size, const char *name, uint8_t *out, uint32_t max)
{
    struct dat_entry e;
    return dat_find(gfx, size, name, &e) ? dat_extract(gfx, size, &e, out, max) : 0;
}

/* Extracts the .3DX file `name` into out. Returns out, or 0 if the file is missing or its first
   block does not lie inside it. */
static const uint8_t *mesh(const uint8_t *gfx, uint32_t size, const char *name, uint8_t *out)
{
    uint32_t n = entry(gfx, size, name, out, MESH_MAX);
    if(n < 0x340 || le32s(out + 0x14) < 1) return 0;
    int32_t blk = le32s(out + 0x18);
    return blk > 0 && (uint32_t)blk + 0x80 <= n ? out : 0;
}

/*
 * The radar's darkening table, as the game builds it (original: STARTL.EXE 0x25870). For each
 * colour, out[colour] is the first palette entry that is nearest to the colour at the fraction
 * f of its brightness. The game builds two such tables. The first has the factor 0.75 (original:
 * 0x2587b) and is the radar's; the second has 0.6 and is used for routine 192 (see glass()).
 * The colours are the 6-bit values of MAPn.COL; the track file has them times 4, so pal is
 * shifted right by 2. The distances are squared, in floats, and the search starts at 22100000
 * and goes down (original: 0x259ad), as in the game. The game's clamp at 255 never takes effect
 * on 6-bit values, so it is not coded.
 */
void efx_darken_table(const uint32_t *pal, uint8_t out[256], float f)
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

/*
 * Builds the radar's quad in m (a block that is its own object, see R_VERTS), with its texture
 * in tex and its material and texture record in the last 32 bytes before end. The corners are
 * (0, 0), (R_SIDE, 0), (R_SIDE, -R_SIDE) and (0, -R_SIDE), with x to the right and y up. The
 * normal points towards +z, and the whole texture lies over the quad. The material must be
 * within a face's reach of the MLB (a 16-bit offset). Returns m, or 0 if it is not.
 */
static const uint8_t *radar_mesh(uint8_t *m, uint8_t *tex, uint8_t *end, const uint8_t *mlb)
{
    uint8_t *rec = end - 32, *mat = end - 16;
    int32_t off = (int32_t)(mat - mlb);
    if(off < -32768 || off > 32767) return 0;
    for(int i = 0; i < 0x100; i++) m[i] = 0;
    for(int y = 0; y < TEX_SIZE; y++)
        for(int x = 0; x < TEX_SIZE; x++) tex[256 * y + x] = SMG_RADAR;
    put_le32s(rec, 1);                                  /* frames */
    put_le32s(rec + 4, SMG_RADAR);
    put_le32s(rec + 8, (int32_t)(tex - mlb));           /* frame 0 */
    put_le32s(mat, 224);                                /* the routine */
    put_le32s(mat + 4, (int32_t)(rec - mlb));
    put_le32s(mat + 8, TEX_SIZE);
    put_le32s(mat + 12, TEX_SIZE);

    put_le32s(m, R_SIDE);                               /* the bounding radius */
    put_le32s(m + 0x10, 4);                             /* vertices */
    put_le32s(m + 0x14, 1);                             /* faces */
    put_le32s(m + 0x1C, R_VERTS);
    put_le32s(m + 0x24, R_UVS);
    for(int k = 0; k < 16; k++) put_le32s(m + 0x28 + 4 * k, R_ROW);
    static const int16_t xy[4][2] = { { 0, 0 }, { R_SIDE, 0 }, { R_SIDE, -R_SIDE }, { 0, -R_SIDE } };
    static const int16_t uv[4][2] = { { 0, 0 }, { 255, 0 }, { 255, 255 }, { 0, 255 } };
    for(int i = 0; i < 4; i++) {
        put_le16s(m + R_VERTS + 6 * i, xy[i][0]);
        put_le16s(m + R_VERTS + 6 * i + 2, xy[i][1]);
        put_le16s(m + R_UVS + 6 * i, uv[i][0]);
        put_le16s(m + R_UVS + 6 * i + 2, uv[i][1]);
        put_le16s(m + R_FACE + 8 + 2 * i, 6 * i);       /* the corners' vertex offsets */
    }
    put_le16s(m + R_ROW, R_FACE);
    put_le16s(m + R_ROW + 2, 0xFFFF);
    put_le16s(m + R_FACE + 4, off);                     /* the material */
    put_le16s(m + R_FACE + 6, 4);                       /* corners */
    put_le32s(m + R_FACE + 0x1C, 16383);                /* the normal (0, 0, 1) */
    return m;
}

/*
 * Routine 192 is tinted glass (original: STARTL.EXE 0xcae84; the station of track 5). A face of
 * this routine has one colour that is not drawn. Instead each pixel under the face is taken
 * through the second table that 0x25870 builds, which has 0.6 of the brightness (original:
 * [0xbcb1c] = 0x15cbc + 0x100, the column loop at 0xccad0).
 * The core draws it as a see-through face (routine 224) whose texels all pick the table's
 * row SMG_GLASS. This function writes that texture (tex) and its record (rec), and turns each
 * routine 192 material of the track's MLB into a routine 224 material over them. The MLB
 * starts with its table of materials, which ends where the first texture record begins.
 */
static void glass(uint8_t *tex, uint8_t *rec, const uint8_t *mlb)
{
    for(int y = 0; y < 2 * TEX_SIZE; y++)
        for(int x = 0; x < 256; x++) tex[256 * y + x] = SMG_GLASS;
    put_le32s(rec, 1);                                  /* frames */
    put_le32s(rec + 4, SMG_GLASS);
    put_le32s(rec + 8, (int32_t)(tex - mlb));           /* frame 0 */
    uint8_t *m = (uint8_t *)mlb;
    int32_t first = 0x7FFFFFFF;
    for(int32_t k = 0; 16 * (k + 1) <= first; k++) {
        int32_t routine = le32s(m + 16 * k), ro = le32s(m + 16 * k + 4);
        if(routine == 192) {
            put_le32s(m + 16 * k, 224);
            put_le32s(m + 16 * k + 4, (int32_t)(rec - mlb));
            put_le32s(m + 16 * k + 8, TEX_SIZE);
            put_le32s(m + 16 * k + 12, TEX_SIZE);
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
        efx_darken_table(pal, smg + SMG_RADAR * 256, 0.75f);
        efx_darken_table(pal, smg + SMG_GLASS * 256, 0.6f);
        glass(mem + AT_GLASS, mem + AT_GLASS_REC, mlb);
        for(uint32_t a = 0; a < SMG_SIZE; a++)          /* the table into the rasterizer, one entry a write */
            RS_SMG = a << 8 | smg[a];
        rs_smg = smg;                                   /* the C models' copy (raster.h) */
        radar = radar_mesh(mem + AT_RADAR, mem + AT_TEX, mem + cap, mlb);
    }
    shadow = mesh(gfx, gfx_size, "SHADOW.3DX", mem + AT_SHADOW);
    smoke = mesh(gfx, gfx_size, "SMOKE.3DX", mem + AT_SMOKE);
    while(!(MEM_STATUS & MEM_WRITES_IDLE))              /* wait until the SDRAM has it all */
        ;
    return ok && radar && shadow && smoke;
}

int efx_table(void)
{
    return table;
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

/*
 * Makes the radar's quad a piece for the geometry unit. It lies at depth 256, where 1/16 pixel
 * is 256 / scale view units. The corners are a quarter pixel away from the window's edge lines
 * (x 3.75 .. 75.75, y 124.25 .. 196.25). Then a pixel whose centre is inside, and the game's
 * rule for routine 224 (the point below and right of the pixel's corner, original: STARTL.EXE
 * 0xcd97a), cover the same pixels, whichever the blend draws with.
 */
int efx_radar_piece(const struct su_view *v, struct gu_piece *g)
{
    if(!radar) return 0;
    const int32_t f = 256, x16 = EFX_RADAR_X0 * 16 - 4, y16 = EFX_RADAR_Y0 * 16 + 4;
    /* view units per 1/16 pixel, 16 fraction bits */
    int32_t per = (int32_t)(((int64_t)f << 16) / v->scale);
    for(int k = 0; k < 9; k++) g->m[k] = 0;
    g->m[0] = per;
    g->m[4] = per;
    g->m[8] = 65536;
    g->t[0] = (int32_t)(((int64_t)(x16 - v->cx) * f << 8) / v->scale);
    g->t[1] = (int32_t)(((int64_t)(v->cy - y16) * f << 8) / v->scale);
    g->t[2] = f << 8;
    g->e[0] = g->e[1] = 0;
    g->e[2] = -(f << 8);                                /* the eye on the -z side, so the face is seen */
    g->row = 0;
    g->mirror = 0;
    g->phase = 0;
    g->refl = 0;
    g->obj = g->blk = radar;
    return 1;
}
