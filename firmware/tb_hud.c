/*
 * The race screen section of the track image (tb_hud.h; the layout of the section is described
 * there). It is made from five files of the game. The caller (track_build.c) has unpacked them
 * from GFX/GAME.DAT and DATA/LOCAL.DAT. Entry points: tb_hud_build and tb_hud_picture.
 * The files:
 *   GAMEL.SPR  The sprites: int32 count, size; then count records of int32 x, y, width, height,
 *              offset; then the pixels.
 *   GAMEL.FNT  The fonts: u8 (number of fonts - 1); per font int32 height, first character, count;
 *              per font count records of int32 width, offset; int32 size; then the pixels, two
 *              a byte (high half first), in rows of (width + 1) / 2 bytes.
 *   IMPLET_L.PKC, IMPRES_L.PKC
 *              Two pictures of 320 x 200. A file is a series of int32 n. If n < 0, the next -n
 *              pixels are skipped (see-through). If n >= 0, n pixels follow. The picture ends at
 *              its last pixel or at the file's last whole int32.
 *   GAME.TXT   The game's texts, one per line.
 * The section holds the sprites, the fonts' characters and the pictures as images, with a table of
 * them. A picture becomes boxes as make_track.py's pkc() finds them:
 * 1. The rows with pixels (a byte that is not 0) make bands of adjacent rows, top to bottom.
 * 2. The columns with pixels in a band make boxes, left to right. A box is split where more than
 *    8 empty columns lie between.
 */
#include "tb_hud.h"
#include "bytes.h"
#include <string.h>

#define PKC_W   320             /* a .PKC picture's size */
#define PKC_H   200
#define LINES   26              /* GAME.TXT's lines in the section, 32 bytes each */

/* Where the section goes. out is the section (cap bytes), entry is the offset of the next image's
   table entry, and at is the offset where the next image's pixels go. */
struct sink { uint8_t *out; uint32_t cap, entry, at; };

/* Writes the table entry of an image w x h at (x, y) and returns where its pixels go. Returns
   NULL if the pixels do not fit, or if the numbers do not fit the entry. */
static uint8_t *image(struct sink *s, int32_t w, int32_t h, int32_t x, int32_t y)
{
    if(w < 0 || w > 0xFFFF || h < 0 || h > 0xFFFF || x < -0x8000 || x > 0x7FFF || y < -0x8000 || y > 0x7FFF)
        return NULL;
    uint32_t n = (uint32_t)w * (uint32_t)h;
    if(n > s->cap - s->at) return NULL;
    uint8_t *e = s->out + s->entry, *px = s->out + s->at;
    put_le32(e, s->at);
    put_le16(e + 4, (uint32_t)w);
    put_le16(e + 6, (uint32_t)h);
    put_le16(e + 8, (uint32_t)x);
    put_le16(e + 10, (uint32_t)y);
    s->entry += 12;
    s->at += n;
    return px;
}

/* A walk over a .PKC picture's pixels, in pieces of one row, in the order of the file. d is the
   file and size its size. p is the read position in the file, and at the pixel position in the
   picture (row x 320 + column). left is the number of pixels left in the current run, and src
   where they are. */
struct walk { const uint8_t *d, *src; uint32_t size, p, at, left; };

/* Gets the next piece of the walk k. Returns 1 with its row, column, length and bytes (*y, *x,
   *n, *src). Returns 0 at the picture's end, and -1 if a run passes the file's end. Runs past the
   picture's last pixel are cut off there. */
static int next(struct walk *k, uint32_t *y, uint32_t *x, uint32_t *n, const uint8_t **src)
{
    while(!k->left) {
        if(k->size - k->p < 4 || k->at >= PKC_W * PKC_H) return 0;
        uint32_t v = le32u(k->d + k->p);
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

/* SET and GET set and read bit i of the bitmap b (an array of u32). */
#define SET(b, i)   ((b)[(i) >> 5] |= 1u << ((i) & 31))
#define GET(b, i)   ((b)[(i) >> 5] >> ((i) & 31) & 1)

/* Writes the box x0 .. x1 of the band y0 .. y1 (x1 and y1 excluded): its table entry, and the
   picture's pixels in it. Returns 1, or 0 if the box does not fit. */
static int box(const uint8_t *d, uint32_t size, struct sink *s, uint32_t x0, uint32_t x1, uint32_t y0,
               uint32_t y1)
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

/* Finds the boxes of a .PKC picture (the rule is in the header of this file). With s, it also
   writes their entries and pixels; with s NULL it only counts them. Returns their number, or -1
   if the file is malformed or they do not fit. The first walk marks the rows with pixels. For
   each band of rows, a second walk marks the columns with pixels. */
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
        int32_t x0 = -1, prev = -1;             /* the open box: its first column, the last one seen */
        for(int32_t c = 0; c <= PKC_W; c++) {
            if(c < PKC_W && !GET(cols, c)) continue;
            if(x0 >= 0 && (c == PKC_W || c - prev > 9)) {       /* more than 8 empty columns: close the box */
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

/* One .PKC picture alone as boxes (see tb_hud.h). The first walk counts the boxes, which sizes the
   table; the second writes them. */
uint32_t tb_hud_picture(const uint8_t *d, uint32_t size, uint8_t *out, uint32_t cap)
{
    int32_t n = pkc(d, size, NULL);
    if(n < 0 || cap < 4 || 4 + 12 * (uint64_t)n > cap) return 0;
    struct sink s = { out, cap, 4, 4 + 12 * (uint32_t)n };
    if(pkc(d, size, &s) != n) return 0;
    put_le32(out, (uint32_t)n);
    return s.at;
}

/* Python's str.splitlines() breaks in latin-1 */
static int line_break(uint8_t c)
{
    return (c >= 0x0A && c <= 0x0D) || (c >= 0x1C && c <= 0x1E) || c == 0x85;
}

/* Makes the whole section (see tb_hud.h). The steps:
   1. The counts of sprites, characters and boxes, from which the size of the tables follows. The
      pixels go behind the tables.
   2. The sprites, as images.
   3. The fonts' characters, as images, and the font table.
   4. The two pictures, as boxes, and the table of their box ranges.
   5. The text lines. */
uint32_t tb_hud_build(const uint8_t *spr, uint32_t spr_size, const uint8_t *fnt, uint32_t fnt_size,
                      const uint8_t *implet, uint32_t implet_size, const uint8_t *impres,
                      uint32_t impres_size, const uint8_t *txt, uint32_t txt_size, uint8_t *out,
                      uint32_t cap)
{
    /* The counts come first: the size of the tables places the pixels. */
    if(spr_size < 8 || fnt_size < 1) return 0;
    uint32_t nspr = le32u(spr);
    if(nspr > (spr_size - 8) / 20) return 0;
    uint32_t nf = fnt[0] + 1u, glyphs = 1 + 12 * nf, pixels = glyphs, nglyph = 0;
    if(glyphs > fnt_size) return 0;
    for(uint32_t f = 0; f < nf; f++) {
        const uint8_t *h = fnt + 1 + 12 * f;
        uint32_t count = le32u(h + 8);
        if((le32u(h) | le32u(h + 4) | count) & 0x80000000u || count > (fnt_size - pixels) / 8) return 0;
        pixels += 8 * count;
        nglyph += count;
    }
    pixels += 4;                                        /* skips the int32 that holds the pixels' size */
    int32_t nlet = pkc(implet, implet_size, NULL), nres = pkc(impres, impres_size, NULL);
    if(nlet < 0 || nres < 0) return 0;
    uint32_t nimg = nspr + nglyph + (uint32_t)nlet + (uint32_t)nres;
    uint64_t head = 8 + 12 * (uint64_t)nimg + 20 * nf + 16 + 32 * LINES;
    if(head > cap) return 0;
    struct sink s = { out, cap, 8, (uint32_t)head };
    put_le32(out, nimg);
    put_le32(out + 4, nf);

    /* The sprites. The record's x and y go into the table as the image's place. */
    const uint8_t *base = spr + 8 + 20 * nspr;
    uint32_t left = spr_size - 8 - 20 * nspr;
    for(uint32_t i = 0; i < nspr; i++) {
        const uint8_t *r = spr + 8 + 20 * i;
        int32_t w = (int32_t)le32u(r + 8), h = (int32_t)le32u(r + 12);
        uint32_t at = le32u(r + 16);
        uint8_t *px = image(&s, w, h, (int32_t)le32u(r), (int32_t)le32u(r + 4));
        if(!px) return 0;
        uint32_t n = (uint32_t)w * (uint32_t)h;
        if(!n) continue;
        if(at > left || n > left - at) return 0;
        memcpy(px, base + at, n);
    }

    /* The fonts' characters. The file packs two pixels in a byte; here each pixel becomes a byte,
       its shade 0 .. 15. */
    uint8_t *ft = out + 8 + 12 * nimg;
    for(uint32_t f = 0, first = nspr; f < nf; f++) {
        const uint8_t *hd = fnt + 1 + 12 * f;
        int32_t h = (int32_t)le32u(hd);
        uint32_t count = le32u(hd + 8), widest = 1;
        for(uint32_t i = 0; i < count; i++, glyphs += 8) {
            int32_t w = (int32_t)le32u(fnt + glyphs), at = (int32_t)le32u(fnt + glyphs + 4);
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
        put_le32(ft, (uint32_t)h);
        put_le32(ft + 4, le32u(hd + 4));
        put_le32(ft + 8, count);
        put_le32(ft + 12, widest);
        put_le32(ft + 16, first);
        ft += 20;
        first += count;
    }

    /* The two pictures, as boxes at their places. Then the table of their ranges: for each
       picture the number of its first box and the box count. */
    if(pkc(implet, implet_size, &s) != nlet || pkc(impres, impres_size, &s) != nres) return 0;
    put_le32(ft, nspr + nglyph);
    put_le32(ft + 4, (uint32_t)nlet);
    put_le32(ft + 8, nspr + nglyph + (uint32_t)nlet);
    put_le32(ft + 12, (uint32_t)nres);
    ft += 16;

    /* GAME.TXT's first LINES lines, cut as Python's splitlines() cuts them. A line is stored in 32
       bytes: at most 31 bytes of text, and the rest zero. */
    memset(ft, 0, 32 * LINES);
    for(uint32_t l = 0, i = 0; l < LINES && i < txt_size; l++) {
        for(uint32_t n = 0; i < txt_size && !line_break(txt[i]); i++)
            if(n < 31) ft[32 * l + n++] = txt[i];
        if(i < txt_size) i += txt[i] == '\r' && i + 1 < txt_size && txt[i + 1] == '\n' ? 2 : 1;
    }
    return s.at;
}
