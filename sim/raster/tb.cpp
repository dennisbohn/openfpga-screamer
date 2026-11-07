// Raster testbench: the rasterizer (raster.v, raster_tex.v) against its C model, firmware/raster.c,
// pixel for pixel.
//
// Usage: Vraster_system [triangles, default 4000] [seed, default 1]
//   tools/sim.sh raster 4000 1
// Prints PASS or FAIL (with the first differing pixel) and exits with 0 or 1.
//
// What is drawn
//   Random triangles over random framebuffer content, into both buffers. Their kinds:
//   - flat (routine 0 here), textured (routines 16 and 64), and blended (routine 224), which looks
//     each pixel up in a see-through table; the testbench fills the table with random entries
//     through register 27;
//   - from the CPU's registers, one at a time, as soc.v starts them (the register numbers are
//     those in the header of raster.v), or as the setup unit hands them over (su_*), where the
//     next triangle is taken while the texture pipeline still draws the one before;
//   - among them the fans of convex polygons.
//   Texture lines come from a model of the SDRAM line port with random delays.
//
// Checks
//   - Before the hardware runs, the model alone (fan_check): the fan of a convex polygon blends
//     each pixel at most once and leaves no gap in a row.
//   - After each batch of triangles both buffers are read back and compared with the model's, and
//     the pixel counter with the model's count. At a difference the batch is replayed on the model
//     and its triangles are listed, those that changed the first wrong pixel marked.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#include "Vraster_system.h"
#include "verilated.h"
#include "../../firmware/raster.c"

static Vraster_system *top;
static uint64_t cycles;
static uint32_t rng = 1;

// The random numbers: a xorshift generator, rr(n) is an integer from 0 to n - 1, rf(lo, hi) a
// float from lo up to hi. xy(x, y) packs a coordinate pair the way the registers take it:
// x in bits 15:0, y in bits 31:16.
static uint32_t rnd()
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static int rr(int n) { return (int)(rnd() % (uint32_t)n); }
static float rf(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }
static uint32_t xy(int32_t x, int32_t y) { return (uint32_t)(uint16_t)y << 16 | (uint16_t)x; }

// The SDRAM: the textures (rows of 256 bytes) are anywhere in the first 2 MB, a third of the
// bytes are 0. lines holds the texture line reads the rasterizer has started and not yet got
// the words for: the line's byte address, the clock from which its words may come, and the
// number of words sent so far.
static const uint32_t MEM = 1u << 21;
static std::vector<uint8_t> mem(MEM);
struct line_req { uint32_t addr; uint64_t at; int k; };
static std::deque<line_req> lines;

// One clock of the testbench, with the texture line port of the SDRAM modelled here.
static void tick()
{
    // The line port: mem_ready is high in three clocks of four. The lines asked for are answered
    // in order, each with 8 words. The words of a line may start to come 4 to 15 clocks after the
    // request, and a word comes in four clocks of five.
    top->mem_ready = rr(4) != 0;
    bool ask = top->mem_valid && top->mem_ready;
    uint32_t ask_addr = (uint32_t)top->mem_addr << 5;
    top->mem_rsp_valid = 0;
    if (!lines.empty() && lines.front().at <= cycles && rr(5) != 0) {
        line_req &l = lines.front();
        uint32_t a = (l.addr + 4u * (uint32_t)l.k) & (MEM - 4);
        top->mem_rsp_valid = 1;
        top->mem_rsp_data = mem[a] | mem[a + 1] << 8 | mem[a + 2] << 16 | (uint32_t)mem[a + 3] << 24;
        if (++l.k == 8) lines.pop_front();
    }
    top->clk = 1;
    top->eval();
    if (ask) lines.push_back({ask_addr, cycles + 4 + (uint64_t)rr(12), 0});
    cycles++;
    top->clk = 0;
    top->eval();
}

// Writes register a of the rasterizer (one clock).
static void reg_wr(int a, uint32_t d)
{
    top->wr = 1;
    top->wr_addr = (uint8_t)a;
    top->wr_data = d;
    tick();
    top->wr = 0;
}

static void wait_idle()
{
    while (top->busy) tick();
}

// The see-through table. smg_load fills it with random bytes, writes each entry to the hardware
// through register 27 and points the model (rs_smg) at the copy smg_table.
static uint8_t smg_table[16384];
static void smg_load()
{
    for (int i = 0; i < 16384; i++) {
        smg_table[i] = (uint8_t)rnd();
        reg_wr(27, (uint32_t)i << 8 | smg_table[i]);
    }
    rs_smg = smg_table;
}

// The framebuffers, 320 x 200 pixels of one byte each. The hardware's is reached through the
// testbench's port, the model's is fbm. fb_fill puts the same random content into buffer b of
// both, after the rasterizer is idle.
static uint8_t fbm[2][64000];
static void fb_fill(int b)
{
    wait_idle();
    top->tb_buf = b;
    top->tb_wr = 1;
    for (int w = 0; w < 16000; w++) {
        uint32_t d = rnd();
        for (int k = 0; k < 4; k++) fbm[b][4 * w + k] = (uint8_t)(d >> (24 - 8 * k));
        top->tb_addr = w;
        top->tb_data = d;
        tick();
    }
    top->tb_wr = 0;
}

// Reads buffer b back and compares it with the model's. Returns the number of pixels that differ,
// prints the first one, and remembers it in first_bad (pixel index) and first_buf.
static int first_bad, first_buf;
static uint32_t fb_check(int b)
{
    wait_idle();
    top->tb_wr = 0;
    top->tb_buf = b;
    uint32_t differ = 0;
    int first = 0, fh = 0, fm = 0;
    for (int w = 0; w < 16000; w++) {
        top->tb_addr = w;
        tick();
        uint32_t q = top->fb_q;
        for (int k = 0; k < 4; k++) {
            int hw = (int)(q >> (24 - 8 * k) & 255), m = fbm[b][4 * w + k];
            if (hw != m && differ++ == 0) {
                first = 4 * w + k;
                fh = hw;
                fm = m;
            }
        }
    }
    if (differ) {
        first_bad = first;
        first_buf = b;
    }
    if (differ)
        printf("buffer %d: %u pixels differ, first at x %d y %d: %02x, model %02x\n", b, differ,
               first % 320, first / 320, fh, fm);
    return differ;
}

// A triangle to draw. routine 0 is flat, 16, 64 and 224 are textured; su is 1 if the setup unit
// hands it over, 0 if the CPU's registers do. tex is the texture's address in mem, tw and th its
// size, s the texture setup (the reference pixel and the Q, U, V values).
struct tri {
    int buf, routine, su;
    rs_vertex v[3];
    rs_clip clip;
    uint8_t colour;
    uint32_t tex;
    int tw, th;
    struct rs_tex_setup s;
};

// model_pixels is the model's pixel count; n_tri counts the triangles drawn per kind (flat,
// routine 16, 64, 224); n_su those from the setup unit. done holds the triangles of the batch in
// progress, and before the two buffers as they were at the start of the batch (to replay it).
static uint32_t model_pixels, n_tri[4], n_su;
static std::vector<tri> done;
static uint8_t before[2][64000];

// The texture size register (8) and the draw register (5) for a triangle; the bits are described
// in the header of raster.v.
static uint32_t size_word(const tri &t)
{
    return (uint32_t)t.tw | (uint32_t)t.th << 16 | (t.routine == 224 ? 1u << 30 : 0) |
           (t.routine == 64 || t.routine == 224 ? 1u << 31 : 0);
}

static uint32_t draw_word(const tri &t)
{
    return t.colour | (uint32_t)t.buf << 8 | (t.routine ? 1u << 9 : 0);
}

// Draws t on the model at once and on the hardware: through the CPU's registers (the call
// returns when the start is written) or by offering it to the setup unit's port (the call
// returns when the rasterizer has taken it).
static void draw(const tri &t)
{
    done.push_back(t);
    if (t.routine == 0) {
        model_pixels += rs_triangle(fbm[t.buf], &t.clip, t.v, t.colour);
    } else {
        rs_texture tx = { &mem[t.tex], t.tw, t.th, t.routine };
        model_pixels += rs_textured_fx(fbm[t.buf], &t.clip, t.v, &t.s, &tx);
    }
    n_tri[t.routine == 0 ? 0 : t.routine == 16 ? 1 : t.routine == 64 ? 2 : 3]++;
    // The setup unit's port has no clip, so the clip rectangle comes from the registers in both
    // ways; it is taken with the triangle.
    reg_wr(3, xy(t.clip.x0, t.clip.y0));
    reg_wr(4, xy(t.clip.x1, t.clip.y1));
    if (!t.su) {                            // the CPU's registers (7..18 may change while one is drawn)
        for (int i = 0; i < 3; i++) reg_wr(i, xy(t.v[i].x, t.v[i].y));
        if (t.routine) {
            reg_wr(7, t.tex);
            reg_wr(8, size_word(t));
            reg_wr(9, xy(t.s.xr, t.s.yr));
            for (int k = 0; k < 3; k++) {
                reg_wr(10 + k, (uint32_t)t.s.a[k]);
                reg_wr(13 + k, (uint32_t)t.s.dx[k]);
                reg_wr(16 + k, (uint32_t)t.s.dy[k]);
            }
        }
        wait_idle();                        // here no soc.v holds the start back while busy
        reg_wr(5, draw_word(t));
        return;
    }
    n_su++;                                 // the setup unit's port: offered until taken
    top->su_v0 = xy(t.v[0].x, t.v[0].y);
    top->su_v1 = xy(t.v[1].x, t.v[1].y);
    top->su_v2 = xy(t.v[2].x, t.v[2].y);
    top->su_ref = xy(t.s.xr, t.s.yr);
    top->su_tex = t.tex;
    top->su_size = size_word(t);
    top->su_draw = (uint16_t)draw_word(t);
    top->su_a0 = (uint32_t)t.s.a[0];
    top->su_a1 = (uint32_t)t.s.a[1];
    top->su_a2 = (uint32_t)t.s.a[2];
    top->su_dx0 = (uint32_t)t.s.dx[0];
    top->su_dx1 = (uint32_t)t.s.dx[1];
    top->su_dx2 = (uint32_t)t.s.dx[2];
    top->su_dy0 = (uint32_t)t.s.dy[0];
    top->su_dy1 = (uint32_t)t.s.dy[1];
    top->su_dy2 = (uint32_t)t.s.dy[2];
    top->su_go = 1;
    top->eval();
    while (!top->su_accept) tick();
    tick();
    top->su_go = 0;
}

// A corner at distance rad and angle ang from (cx, cy), in 1/16 pixel. q is 1/z, a random value;
// uq and vq are the texel position (u, v) times q, for a texture of tw x th texels. The texel
// position runs from -0.2 to 1.2 times the texture's size, so it goes past the edges.
static rs_tvertex corner(float cx, float cy, float rad, float ang, int tw, int th)
{
    rs_tvertex c;
    c.x = (int32_t)floorf((cx + rad * cosf(ang)) * 16.0f);
    c.y = (int32_t)floorf((cy + rad * sinf(ang)) * 16.0f);
    c.q = rf(0.02f, 1.0f);
    c.uq = rf(-0.2f, 1.2f) * (float)tw * c.q;
    c.vq = rf(-0.2f, 1.2f) * (float)th * c.q;
    return c;
}

// Sets t's vertices to the corners a, b, c and computes its texture setup. The other values of t
// stay. Returns false if a textured triangle has no area (no setup exists for it).
static bool make(tri &t, const rs_tvertex &a, const rs_tvertex &b, const rs_tvertex &c)
{
    const rs_tvertex tv[3] = { a, b, c };
    for (int i = 0; i < 3; i++) {
        t.v[i].x = tv[i].x;
        t.v[i].y = tv[i].y;
    }
    memset(&t.s, 0, sizeof t.s);
    return t.routine == 0 || rs_tex_setup(&t.s, tv);
}

// The model alone, without the hardware. Random convex polygons with whole-pixel corners are
// drawn as fans of blended triangles (routine 224) over a buffer of zeros. The texture is all
// ones and the see-through table is set so that row 1 adds one to the pixel. So a pixel above 1
// was blended twice, and a zero between drawn pixels of a row is a gap. Returns the number of
// such pixels.
static int fan_check()
{
    static uint8_t count[16384], ones[65536], f[64000];
    for (int p = 0; p < 256; p++) count[256 + p] = (uint8_t)(p + 1);
    memset(ones, 1, sizeof ones);
    const uint8_t *keep = rs_smg;
    rs_smg = count;
    const rs_clip full = { 0, 0, 320, 200 };
    const rs_texture tx = { ones, 256, 256, 224 };
    int bad = 0, polys = 0;
    while (polys < 4000) {
        int n = 3 + rr(5);
        float cx = rf(-20, 340), cy = rf(-20, 220), rad = rr(4) ? rf(1, 30) : rf(30, 300), ang[8];
        for (int i = 0; i < n; i++) ang[i] = rf(0, 6.2831853f);
        for (int i = 1; i < n; i++)
            for (int j = i; j > 0 && ang[j] < ang[j - 1]; j--) std::swap(ang[j], ang[j - 1]);
        rs_tvertex c[8];
        for (int i = 0; i < n; i++) {
            float d = rad * rf(0.5f, 1.0f);
            c[i].x = 16 * (int32_t)lrintf(cx + d * cosf(ang[i]));
            c[i].y = 16 * (int32_t)lrintf(cy + d * sinf(ang[i]));
            c[i].q = 1.0f;
            c[i].uq = c[i].vq = 0.0f;
        }
        bool convex = true;                 // strictly, after the rounding: every other corner is
        for (int i = 0; i < n && convex; i++)   // left of each edge
            for (int k = 0; k < n && convex; k++) {
                const rs_tvertex &p = c[i], &q = c[(i + 1) % n], &r = c[k];
                if (k == i || k == (i + 1) % n) continue;
                convex = (int64_t)(q.x - p.x) * (r.y - p.y) - (int64_t)(q.y - p.y) * (r.x - p.x) > 0;
            }
        if (!convex) continue;
        polys++;
        memset(f, 0, sizeof f);
        for (int i = 1; i + 1 < n; i++) {
            struct rs_tex_setup s;
            const rs_tvertex tv[3] = { c[0], c[i], c[i + 1] };
            rs_vertex pos[3] = { { c[0].x, c[0].y }, { c[i].x, c[i].y }, { c[i + 1].x, c[i + 1].y } };
            if (rs_tex_setup(&s, tv)) rs_textured_fx(f, &full, pos, &s, &tx);
        }
        for (int y = 0; y < 200; y++) {
            int x0 = -1, x1 = -1;
            for (int x = 0; x < 320; x++) {
                if (f[y * 320 + x] > 1) bad++;
                if (f[y * 320 + x]) {
                    if (x0 < 0) x0 = x;
                    x1 = x;
                }
            }
            for (int x = x0; x >= 0 && x <= x1; x++)
                if (!f[y * 320 + x]) bad++;
        }
    }
    rs_smg = keep;
    return bad;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    int total = argc > 1 ? atoi(argv[1]) : 4000;
    rng = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 0) : 1u;
    if (!rng) rng = 1;

    // First the model alone.
    int bad = fan_check();
    printf("model: fans of 4000 convex polygons, %d pixels blended twice or left out in a row\n", bad);
    if (bad) {
        printf("FAIL\n");
        return 1;
    }

    // Then the hardware: random texture memory (a third of the bytes are 0, the transparent
    // texel), a reset, the see-through table and random content in both buffers.
    for (uint32_t i = 0; i < MEM; i++) mem[i] = rr(3) ? (uint8_t)(1 + rr(255)) : 0;
    top = new Vraster_system;
    top->clk = 0;
    top->reset = 1;
    top->eval();
    for (int i = 0; i < 4; i++) tick();
    top->reset = 0;
    smg_load();
    fb_fill(0);
    fb_fill(1);

    // Each round draws a batch of 1 to 40 random triangles, then compares the buffers. A triangle
    // gets a random buffer (changed in one case of ten), kind (20 % flat, 20 % each routine 16
    // and 64, 40 % routine 224), source (70 % the setup unit), texture size (one in four is at
    // most 8 texels wide or high), clip rectangle (4 of 5 are the whole screen), size and place
    // (partly off the screen).
    int sent = 0, buf = 0, batches = 0;
    uint32_t differ = 0;
    while (sent < total && !differ) {
        int batch = 1 + rr(40);
        memcpy(before, fbm, sizeof before);
        done.clear();
        for (int j = 0; j < batch && sent < total; ) {
            tri t;
            if (rr(10) == 0) buf ^= 1;
            t.buf = buf;
            int k = rr(10);
            t.routine = k < 2 ? 0 : k < 4 ? 16 : k < 6 ? 64 : 224;
            t.su = rr(10) >= 3;
            t.colour = (uint8_t)rnd();
            t.tw = rr(4) ? 1 + rr(256) : 1 + rr(8);
            t.th = rr(4) ? 1 + rr(256) : 1 + rr(8);
            t.tex = (uint32_t)rr((int)MEM - 65536 - 512);
            if (rr(5)) {
                t.clip = { 0, 0, 320, 200 };
            } else {
                int x0 = rr(321), y0 = rr(201);
                t.clip = { x0, y0, x0 + rr(321 - x0), y0 + rr(201 - y0) };
            }
            int size = rr(20);
            float rad = size < 10 ? rf(0.5f, 12) : size < 17 ? rf(12, 80) : rf(80, 500);
            float cx = rf(-30, 350), cy = rf(-30, 230);
            // What this round draws: a fan of a convex polygon (2 in 10), a large triangle
            // followed by small ones (2 in 10), or a single triangle (6 in 10).
            int shape = rr(10);
            if (shape < 2) {                // the fan of a convex polygon, back to back
                int n = 4 + rr(3);
                float ang[6];
                for (int i = 0; i < n; i++) ang[i] = rf(0, 6.2831853f);
                for (int i = 1; i < n; i++)
                    for (int m = i; m > 0 && ang[m] < ang[m - 1]; m--) std::swap(ang[m], ang[m - 1]);
                rs_tvertex c[6];
                for (int i = 0; i < n; i++) c[i] = corner(cx, cy, rad * rf(0.6f, 1.0f), ang[i], t.tw, t.th);
                for (int i = 1; i + 1 < n; i++)
                    if (make(t, c[0], c[i], c[i + 1])) {
                        draw(t);
                        sent++, j++;
                    }
            } else if (shape < 4) {         // a large one that fills the queue, then a small one a few times
                t.su = 1;                   // all from the setup unit's port
                t.routine = rr(4) ? 224 : 16;
                rs_tvertex a = corner(cx, cy, 120, rf(0, 6.2831853f), t.tw, t.th);
                rs_tvertex b = corner(cx, cy, 120, rf(0, 6.2831853f), t.tw, t.th);
                rs_tvertex c = corner(cx, cy, 120, rf(0, 6.2831853f), t.tw, t.th);
                if (make(t, a, b, c)) {
                    draw(t);
                    sent++, j++;
                }
                float sx = rf(10, 310), sy = rf(10, 190), sr = rf(0.6f, 2.5f);
                a = corner(sx, sy, sr, rf(0, 6.2831853f), t.tw, t.th);
                b = corner(sx, sy, sr, rf(0, 6.2831853f), t.tw, t.th);
                c = corner(sx, sy, sr, rf(0, 6.2831853f), t.tw, t.th);
                for (int n = 2 + rr(4); n > 0; n--) {
                    int m = rr(5);
                    t.routine = m == 0 ? 16 : m == 1 ? 64 : 224;
                    if (make(t, a, b, c)) {
                        draw(t);
                        sent++, j++;
                    }
                }
            } else {                        // one triangle
                rs_tvertex a = corner(cx, cy, rad, rf(0, 6.2831853f), t.tw, t.th);
                rs_tvertex b = corner(cx, cy, rad, rf(0, 6.2831853f), t.tw, t.th);
                rs_tvertex c = corner(cx, cy, rad, rf(0, 6.2831853f), t.tw, t.th);
                if (make(t, a, b, c)) {
                    draw(t);
                    sent++, j++;
                }
            }
        }
        batches++;
        wait_idle();
        differ = fb_check(0) + fb_check(1);
        if (top->pixels != model_pixels) {
            printf("pixel counter %u, model %u\n", top->pixels, model_pixels);
            differ++;
        }
        if (differ) {
            // Replay the batch on the model from the buffers as they were before it, and list its
            // triangles. A triangle that changed the first wrong pixel is marked with the old and
            // the new value.
            static uint8_t f[2][64000];
            memcpy(f, before, sizeof f);
            for (size_t i = 0; i < done.size(); i++) {
                const tri &t = done[i];
                int p = first_bad;
                uint8_t old = f[t.buf][p];
                uint32_t n;
                if (t.routine == 0) n = rs_triangle(f[t.buf], &t.clip, t.v, t.colour);
                else {
                    rs_texture tx = { &mem[t.tex], t.tw, t.th, t.routine };
                    n = rs_textured_fx(f[t.buf], &t.clip, t.v, &t.s, &tx);
                }
                printf(
                    "  %2zu: buf %d routine %3d %s, %5u pixels, (%d,%d) (%d,%d) (%d,%d) clip %d %d %d %d%s",
                    i, t.buf, t.routine, t.su ? "su " : "cpu", n, t.v[0].x >> 4, t.v[0].y >> 4,
                    t.v[1].x >> 4, t.v[1].y >> 4, t.v[2].x >> 4, t.v[2].y >> 4,
                    t.clip.x0, t.clip.y0, t.clip.x1, t.clip.y1, "");
                if (t.buf == first_buf && f[t.buf][p] != old) printf("  <- %02x to %02x", old, f[t.buf][p]);
                printf("\n");
            }
        }
        // From time to time new random content for one buffer, or a new see-through table.
        if (!differ && rr(20) == 0) fb_fill(rr(2));
        if (!differ && rr(40) == 0) smg_load();
    }
    printf(
        "%d triangles (flat %u, routine 16 %u, 64 %u, 224 %u; %u from the setup unit's side) in %d batches, "
        "%u pixels, %llu clocks\n",
        sent, n_tri[0], n_tri[1], n_tri[2], n_tri[3], n_su, batches,
        model_pixels, (unsigned long long)cycles);
    printf(differ ? "FAIL\n" : "PASS\n");
    top->final();
    delete top;
    return differ ? 1 : 0;
}
