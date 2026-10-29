/*
 * The race screen section (tb_hud.h). The files:
 *   GAMEL.SPR  int32 count, size; count x int32 x, y, width, height, offset; the pixels
 *   GAMEL.FNT  u8 fonts - 1; per font int32 height, first character, count; per font count x
 *              int32 width, offset; int32 size; the pixels, two a byte (high half first),
 *              rows of (width + 1) / 2 bytes
 *   .PKC       320 x 200: int32 n < 0 skips -n pixels (see-through), n >= 0 is followed by n
 *              pixels; the picture ends at its last pixel or the file's last whole int32
 * A picture becomes boxes as make_track.py's pkc() finds them: the rows with pixels (a byte
 * not 0) make bands of adjacent rows, top to bottom; a band's columns with pixels make boxes,
 * left to right, split where more than 8 empty columns lie between.
 */
#include "tb_hud.h"
#include <string.h>

#define PKC_W   320             /* a .PKC picture's size */
#define PKC_H   200
#define LINES   26              /* GAME.TXT's lines in the section, 32 bytes each */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    wr16(p, v);
    wr16(p + 2, v >> 16);
}

/* where the section goes: the next image's table entry and pixels */
struct sink { uint8_t *out; uint32_t cap, entry, at; };

/* the table entry of an image w x h at (x, y); its pixels, or NULL if they do not fit or the
   numbers do not fit the entry */
static uint8_t *image(struct sink *s, int32_t w, int32_t h, int32_t x, int32_t y)
{
    if(w < 0 || w > 0xFFFF || h < 0 || h > 0xFFFF || x < -0x8000 || x > 0x7FFF || y < -0x8000 || y > 0x7FFF)
        return NULL;
    uint32_t n = (uint32_t)w * (uint32_t)h;
    if(n > s->cap - s->at) return NULL;
    uint8_t *e = s->out + s->entry, *px = s->out + s->at;
    wr32(e, s->at);
    wr16(e + 4, (uint32_t)w);
    wr16(e + 6, (uint32_t)h);
    wr16(e + 8, (uint32_t)x);
    wr16(e + 10, (uint32_t)y);
    s->entry += 12;
    s->at += n;
    return px;
}

/* A .PKC picture's pixels in pieces of one row, in the order of the file */
struct walk { const uint8_t *d, *src; uint32_t size, p, at, left; };

/* the next piece: 1 with its row, column, length and bytes; 0 at the picture's end; -1 if a
   run passes the file's end. Runs past the picture's last pixel are cut off there. */
static int next(struct walk *k, uint32_t *y, uint32_t *x, uint32_t *n, const uint8_t **src)
{
    while(!k->left) {
        if(k->size - k->p < 4 || k->at >= PKC_W * PKC_H) return 0;
        uint32_t v = rd32(k->d + k->p);
        k->p += 4;
        if(v & 0x80000000u) {
            k->at += 0u - v;
            continue;
        }
        if(v > k->size - k->p) return -1;
        k->src = k->d + k->p;
        k->p += v;
        k->left = v < PKC_W * PKC_H - k->at ? v : PKC_W * PKC_H - k->at;
    }
    *y = k->at / PKC_W;
    *x = k->at % PKC_W;
    *n = PKC_W - *x < k->left ? PKC_W - *x : k->left;
    *src = k->src;
    k->src += *n;
    k->at += *n;
    k->left -= *n;
    return 1;
}

#define SET(b, i)   ((b)[(i) >> 5] |= 1u << ((i) & 31))
#define GET(b, i)   ((b)[(i) >> 5] >> ((i) & 31) & 1)

/* box x0..x1 of the band y0..y1 (exclusive): its entry and the picture's bytes in it */
static int box(const uint8_t *d, uint32_t size, struct sink *s, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1)
{
    uint32_t w = x1 - x0;
    uint8_t *px = image(s, (int32_t)w, (int32_t)(y1 - y0), (int32_t)x0, (int32_t)y0);
    if(!px) return 0;
    memset(px, 0, w * (y1 - y0));
    struct walk k = { d, NULL, size, 0, 0, 0 };
    uint32_t y, x, n;
    const uint8_t *src;
    while(next(&k, &y, &x, &n, &src) > 0 && y < y1) {
        uint32_t a = x > x0 ? x : x0, b = x + n < x1 ? x + n : x1;
        if(y >= y0 && a < b) memcpy(px + (y - y0) * w + a - x0, src + a - x, b - a);
    }
    return 1;
}

/* the boxes of a .PKC picture; with s, their entries and pixels. Returns their number, -1 if
   the file is malformed or they do not fit. */
static int32_t pkc(const uint8_t *d, uint32_t size, struct sink *s)
{
    uint32_t rows[(PKC_H + 31) / 32] = {0};
    struct walk k = { d, NULL, size, 0, 0, 0 };
    uint32_t y, x, n;
    const uint8_t *src;
    int e;
    while((e = next(&k, &y, &x, &n, &src)) > 0)
        for(uint32_t i = 0; i < n; i++)
            if(src[i]) {
                SET(rows, y);
                break;
            }
    if(e < 0) return -1;
    int32_t count = 0;
    for(uint32_t y0 = 0, y1; y0 < PKC_H; y0 = y1) {
        for(y1 = y0; y1 < PKC_H && GET(rows, y1); y1++) ;
        if(y1 == y0) {
            y1++;
            continue;
        }
        uint32_t cols[PKC_W / 32] = {0};
        k = (struct walk){ d, NULL, size, 0, 0, 0 };
        while(next(&k, &y, &x, &n, &src) > 0 && y < y1)
            if(y >= y0)
                for(uint32_t i = 0; i < n; i++)
                    if(src[i]) SET(cols, x + i);
        int32_t x0 = -1, prev = -1;
        for(int32_t c = 0; c <= PKC_W; c++) {
            if(c < PKC_W && !GET(cols, c)) continue;
            if(x0 >= 0 && (c == PKC_W || c - prev > 9)) {
                if(s && !box(d, size, s, (uint32_t)x0, (uint32_t)prev + 1, y0, y1)) return -1;
                count++;
                x0 = -1;
            }
            if(x0 < 0) x0 = c;
            prev = c;
        }
    }
    return count;
}

uint32_t tb_hud_picture(const uint8_t *d, uint32_t size, uint8_t *out, uint32_t cap)
{
    int32_t n = pkc(d, size, NULL);
    if(n < 0 || cap < 4 || 4 + 12 * (uint64_t)n > cap) return 0;
    struct sink s = { out, cap, 4, 4 + 12 * (uint32_t)n };
    if(pkc(d, size, &s) != n) return 0;
    wr32(out, (uint32_t)n);
    return s.at;
}

/* Python's str.splitlines() breaks in latin-1 */
static int line_break(uint8_t c)
{
    return (c >= 0x0A && c <= 0x0D) || (c >= 0x1C && c <= 0x1E) || c == 0x85;
}

uint32_t tb_hud_build(const uint8_t *spr, uint32_t spr_size, const uint8_t *fnt, uint32_t fnt_size,
                      const uint8_t *implet, uint32_t implet_size, const uint8_t *impres, uint32_t impres_size,
                      const uint8_t *txt, uint32_t txt_size, uint8_t *out, uint32_t cap)
{
    /* the counts first: the table's size places the pixels */
    if(spr_size < 8 || fnt_size < 1) return 0;
    uint32_t nspr = rd32(spr);
    if(nspr > (spr_size - 8) / 20) return 0;
    uint32_t nf = fnt[0] + 1u, glyphs = 1 + 12 * nf, pixels = glyphs, nglyph = 0;
    if(glyphs > fnt_size) return 0;
    for(uint32_t f = 0; f < nf; f++) {
        const uint8_t *h = fnt + 1 + 12 * f;
        uint32_t count = rd32(h + 8);
        if((rd32(h) | rd32(h + 4) | count) & 0x80000000u || count > (fnt_size - pixels) / 8) return 0;
        pixels += 8 * count;
        nglyph += count;
    }
    pixels += 4;                                        /* the pixels' size */
    int32_t nlet = pkc(implet, implet_size, NULL), nres = pkc(impres, impres_size, NULL);
    if(nlet < 0 || nres < 0) return 0;
    uint32_t nimg = nspr + nglyph + (uint32_t)nlet + (uint32_t)nres;
    uint64_t head = 8 + 12 * (uint64_t)nimg + 20 * nf + 16 + 32 * LINES;
    if(head > cap) return 0;
    struct sink s = { out, cap, 8, (uint32_t)head };
    wr32(out, nimg);
    wr32(out + 4, nf);

    /* the sprites, their offsets as the place */
    const uint8_t *base = spr + 8 + 20 * nspr;
    uint32_t left = spr_size - 8 - 20 * nspr;
    for(uint32_t i = 0; i < nspr; i++) {
        const uint8_t *r = spr + 8 + 20 * i;
        int32_t w = (int32_t)rd32(r + 8), h = (int32_t)rd32(r + 12);
        uint32_t at = rd32(r + 16);
        uint8_t *px = image(&s, w, h, (int32_t)rd32(r), (int32_t)rd32(r + 4));
        if(!px) return 0;
        uint32_t n = (uint32_t)w * (uint32_t)h;
        if(!n) continue;
        if(at > left || n > left - at) return 0;
        memcpy(px, base + at, n);
    }

    /* the fonts' characters, each pixel its shade */
    uint8_t *ft = out + 8 + 12 * nimg;
    for(uint32_t f = 0, first = nspr; f < nf; f++) {
        const uint8_t *hd = fnt + 1 + 12 * f;
        int32_t h = (int32_t)rd32(hd);
        uint32_t count = rd32(hd + 8), widest = 1;
        for(uint32_t i = 0; i < count; i++, glyphs += 8) {
            int32_t w = (int32_t)rd32(fnt + glyphs), at = (int32_t)rd32(fnt + glyphs + 4);
            uint8_t *px = image(&s, w, h, 0, 0);
            if(!px) return 0;
            if((uint32_t)w > widest) widest = (uint32_t)w;
            if(!w || !h) continue;
            uint32_t bw = ((uint32_t)w + 1) / 2;
            int64_t lo = (int64_t)pixels + at, hi = lo + (int64_t)(h - 1) * bw + (w - 1) / 2;
            if(lo < 0 || hi >= fnt_size) return 0;
            const uint8_t *row = fnt + lo;
            for(int32_t r = 0; r < h; r++, row += bw)
                for(int32_t c = 0; c < w; c++) *px++ = (uint8_t)(row[c >> 1] >> (c & 1 ? 0 : 4) & 15);
        }
        wr32(ft, (uint32_t)h);
        wr32(ft + 4, rd32(hd + 4));
        wr32(ft + 8, count);
        wr32(ft + 12, widest);
        wr32(ft + 16, first);
        ft += 20;
        first += count;
    }

    /* the two pictures as boxes at their places */
    if(pkc(implet, implet_size, &s) != nlet || pkc(impres, impres_size, &s) != nres) return 0;
    wr32(ft, nspr + nglyph);
    wr32(ft + 4, (uint32_t)nlet);
    wr32(ft + 8, nspr + nglyph + (uint32_t)nlet);
    wr32(ft + 12, (uint32_t)nres);
    ft += 16;

    /* GAME.TXT's lines as Python's splitlines() cuts them, 31 bytes at most */
    memset(ft, 0, 32 * LINES);
    for(uint32_t l = 0, i = 0; l < LINES && i < txt_size; l++) {
        for(uint32_t n = 0; i < txt_size && !line_break(txt[i]); i++)
            if(n < 31) ft[32 * l + n++] = txt[i];
        if(i < txt_size) i += txt[i] == '\r' && i + 1 < txt_size && txt[i + 1] == '\n' ? 2 : 1;
    }
    return s.at;
}
