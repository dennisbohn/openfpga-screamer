/*
 * The blitter's commands and its C model (see blit.h). The model draws the same pixels as
 * core/src/fpga/core/blit.v.
 *
 * A command is 4 words (docs/soc.md, BL_PUSH). Signed coordinates have 10 bits.
 *   word 0  copy: the SDRAM address of the first source pixel (0 here, the pixels are
 *           found through blit_list.src); fill: 0; line: the end point, x in bits 9..0 and
 *           y in bits 25..16
 *   word 1  x in bits 9..0 and y in 25..16 of the first pixel (line: of the start), bits 29..28
 *           the kind (BLIT_*), bit 30 the buffer, bit 31 the opaque flag of a copy
 *   word 2  width in bits 8..0, height in 24..16 (line: the clip's left in 8..0, top in 24..16)
 *   word 3  copy: bytes from row to row in bits 9..0; line: the clip's right in 8..0 and bottom
 *           in 24..16, both exclusive; bits 31..24 the colour
 */
#include "blit.h"

void blit_start(struct blit_list *l, int buffer)
{
    l->n = 0;
    l->buffer = buffer;
}

/* The x and y of a command word: x in bits 9..0, y in bits 25..16. */
static uint32_t xy(int x, int y)
{
    return ((uint32_t)x & 0x3FF) | ((uint32_t)y & 0x3FF) << 16;
}

void blit_copy(struct blit_list *l, const uint8_t *src, int w, int h, int stride, int x, int y, int kind,
               int colour)
{
    if(l->n >= BLIT_MAX || w < 1 || h < 1 || w > 511 || h > 511) return;
    uint32_t *c = l->c[l->n];
    l->src[l->n++] = src;
    c[0] = 0;
    c[1] = xy(x, y) | (uint32_t)(kind & 3) << 28 | (uint32_t)l->buffer << 30 |
           (kind & BLIT_OPAQUE ? 1u << 31 : 0);
    c[2] = (uint32_t)w | (uint32_t)h << 16;
    c[3] = ((uint32_t)stride & 0x3FF) | (uint32_t)colour << 24;
}

void blit_fill(struct blit_list *l, int x, int y, int w, int h, int colour)
{
    if(l->n >= BLIT_MAX || w < 1 || h < 1 || w > 511 || h > 511) return;
    uint32_t *c = l->c[l->n];
    l->src[l->n++] = 0;
    c[0] = 0;
    c[1] = xy(x, y) | (uint32_t)BLIT_FILL << 28 | (uint32_t)l->buffer << 30;
    c[2] = (uint32_t)w | (uint32_t)h << 16;
    c[3] = (uint32_t)colour << 24;
}

void blit_line(struct blit_list *l, int x0, int y0, int x1, int y1, int cx0, int cy0, int cx1, int cy1,
               int colour)
{
    if(l->n >= BLIT_MAX) return;
    uint32_t *c = l->c[l->n];
    l->src[l->n++] = 0;
    c[0] = xy(x1, y1);
    c[1] = xy(x0, y0) | (uint32_t)BLIT_LINE << 28 | (uint32_t)l->buffer << 30;
    c[2] = ((uint32_t)cx0 & 0x1FF) | ((uint32_t)cy0 & 0x1FF) << 16;
    c[3] = ((uint32_t)cx1 & 0x1FF) | ((uint32_t)cy1 & 0x1FF) << 16 | (uint32_t)colour << 24;
}

int blit_text(struct blit_list *l, const uint8_t *base, const struct blit_image *img,
              const struct blit_font *f, int x, int y, const char *s, int colour, int draw)
{
    int cell = (int)f->widest + 2, fixed = 0, gap = 1, x0 = x;
    for(const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int ch = *p;
        if(ch < (int)f->first || ch >= (int)(f->first + f->count)) {
            if(ch == 1) fixed = 1;              /* control bytes, see blit.h */
            else if(ch == 2) fixed = 0;
            else if(ch == 3) gap = 0;
            else if(ch == 4) gap = 1;
            continue;
        }
        const struct blit_image *g = &img[f->first_image + (uint32_t)(ch - (int)f->first)];
        int w = g->w;
        if(draw) blit_copy(l, base + g->off, w, g->h, w, x + gap + (fixed ? (cell - (w + 1)) / 2 : 0), y,
                           BLIT_FONT, colour);
        x += fixed ? cell : w + 1 + gap;
    }
    return x - x0;
}

static void put(volatile uint8_t *fb, int x, int y, int v)
{
    fb[y * 320 + x] = (uint8_t)v;
}

/* The low 10 bits of v as a signed number. */
static int sx10(uint32_t v) { return (int)(v & 0x3FF) - (v & 0x200 ? 0x400 : 0); }

void blit_model(volatile uint8_t *fb, const struct blit_list *l)
{
    for(int i = 0; i < l->n; i++) {
        const uint32_t *c = l->c[i];
        int kind = (int)(c[1] >> 28 & 3), colour = (int)(c[3] >> 24);
        int x0 = sx10(c[1]), y0 = sx10(c[1] >> 16);
        if(kind == BLIT_LINE) {
            /* Bresenham's line from (x0, y0) to (x1, y1); only the pixels in the clip are drawn. */
            int x1 = sx10(c[0]), y1 = sx10(c[0] >> 16);
            int cx0 = (int)(c[2] & 0x1FF), cy0 = (int)(c[2] >> 16 & 0x1FF);
            int cx1 = (int)(c[3] & 0x1FF), cy1 = (int)(c[3] >> 16 & 0x1FF);
            int dx = x1 >= x0 ? x1 - x0 : x0 - x1, dy = y1 >= y0 ? y0 - y1 : y1 - y0;
            int sxp = x1 >= x0, syp = y1 >= y0, err = dx + dy;
            for(;;) {
                if(x0 >= cx0 && x0 < cx1 && y0 >= cy0 && y0 < cy1) put(fb, x0, y0, colour);
                if(x0 == x1 && y0 == y1) break;
                int e2 = 2 * err, ex = e2 >= dy, ey = e2 <= dx;
                if(ex) { x0 += sxp ? 1 : -1; err += dy; }
                if(ey) { y0 += syp ? 1 : -1; err += dx; }
            }
            continue;
        }
        int w = (int)(c[2] & 0x1FF), h = (int)(c[2] >> 16 & 0x1FF), stride = (int)(c[3] & 0x3FF);
        int opaque = (int)(c[1] >> 31);
        if(kind == BLIT_FILL) {                 /* clipped to the screen */
            for(int py = y0 < 0 ? 0 : y0; py < y0 + h && py < 200; py++)
                for(int px = x0 < 0 ? 0 : x0; px < x0 + w && px < 320; px++) put(fb, px, py, colour);
            continue;
        }
        /* BLIT_COPY and BLIT_FONT, clipped to the screen. A font pixel is a shade 1..15 of the
           colour (colour * 16 + shade); shade 0 is see-through. */
        const uint8_t *row = l->src[i];
        for(int r = 0; r < h; r++, row += stride)
            for(int k = 0; k < w; k++) {
                int px = x0 + k, py = y0 + r;
                if(px < 0 || px >= 320 || py < 0 || py >= 200) continue;
                int v = row[k];
                if(kind == BLIT_FONT) {
                    if(v & 15) put(fb, px, py, (colour & 15) << 4 | (v & 15));
                } else if(v || opaque) put(fb, px, py, v);
            }
    }
}
