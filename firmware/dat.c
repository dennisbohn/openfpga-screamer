/*
 * The reader of the game's .DAT containers (dat.h; the container format is described there).
 * It finds an entry by its name and unpacks it, stored or LZW coded. The track builder
 * (track_build.c) and the other loaders of game files use it. Entry points:
 *   dat_count    the number of entries
 *   dat_find     the directory record of the entry with a name
 *   dat_extract  the entry's bytes, stored or unpacked
 *   dat_lzw      one LZW stream
 */
#include "dat.h"
#include <string.h>

#define REC         32          /* bytes per directory record */
#define FIRST       258         /* the first code of the table */
#define CODES       4096        /* 12 bits */

/* The CPU does no unaligned loads, so words are read a byte at a time. */
static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Gets the directory, which ends the file in front of the last u32 (its size). Returns the
   first record, and *records is the number of whole records. Returns 0 if the file is malformed. */
static const uint8_t *directory(const uint8_t *file, uint32_t file_size, uint32_t *records)
{
    if (file_size < 4) return 0;
    uint32_t dir_size = rd32(file + file_size - 4);
    if (dir_size > file_size - 4) return 0;
    *records = dir_size / REC;
    return file + file_size - 4 - dir_size;
}

uint32_t dat_count(const uint8_t *file, uint32_t file_size)
{
    uint32_t records, n = 0;
    const uint8_t *r = directory(file, file_size, &records);
    if (!r) return 0;
    while (n < records && (r[n * REC] ^ 0x3A) != 0)
        n++;
    return n;
}

/* Decodes the directory record r into e: the name (each byte XOR 0x3A, up to 16 bytes, ended by
   a NUL), then offset, stored size, size and method as u32 numbers. */
static void record(const uint8_t *r, struct dat_entry *e)
{
    int i;
    for (i = 0; i < 16 && (r[i] ^ 0x3A) != 0; i++)
        e->name[i] = (char)(r[i] ^ 0x3A);
    e->name[i] = 0;
    e->offset = rd32(r + 16);
    e->stored = rd32(r + 20);
    e->size = rd32(r + 24);
    e->method = rd32(r + 28);
}

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

/* Compares the names ignoring ASCII case. A name of 16 characters has no NUL in its record. */
int dat_find(const uint8_t *file, uint32_t file_size, const char *name, struct dat_entry *e)
{
    uint32_t records, n = dat_count(file, file_size);
    const uint8_t *r = directory(file, file_size, &records);
    for (uint32_t k = 0; k < n; k++, r += REC) {
        int i = 0;
        while (i < 16 && name[i] && upper(r[i] ^ 0x3A) == upper((uint8_t)name[i]))
            i++;
        if (!name[i] && (i == 16 || (r[i] ^ 0x3A) == 0)) {
            record(r, e);
            return 1;
        }
    }
    return 0;
}

/* Unpacks entry e into out. Method 0 is a copy of the stored bytes, methods 1 to 3 are one LZW
   stream. The entry's data must lie inside the file. Returns e->size on success. */
uint32_t dat_extract(const uint8_t *file, uint32_t file_size, const struct dat_entry *e, uint8_t *out,
                     uint32_t out_max)
{
    if (e->offset > file_size || e->stored > file_size - e->offset || e->size > out_max) return 0;
    const uint8_t *src = file + e->offset;
    if (e->method == 0) {
        if (e->stored < e->size) return 0;
        memcpy(out, src, e->size);
        return e->size;
    }
    if (e->method > 3) return 0;
    return dat_lzw(src, e->stored, out, e->size);
}

/*
 * How the LZW decoder keeps its table:
 * 1. The table holds no strings. It holds only where the strings are in the output, and a code
 *    is copied from there, as an LZ77 decoder does. This saves walking a chain of prefixes.
 * 2. Every code after a clear writes one string. String j (counted from 0) begins at start[j].
 * 3. Code 258 + j is string j plus the first byte of string j + 1: out[start[j] .. start[j + 1]],
 *    both ends included. So a code is defined once the string after its own has begun.
 * 4. The code that a string completes may be that very string. Its last byte is then its first
 *    byte, which only a byte copy in order gets right.
 * 5. Every other code lies wholly before the output position.
 */
static uint32_t start[CODES - FIRST + 1];

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* Decodes one LZW stream (see dat.h). The loop reads one code at a time. The bits are collected
   in `bits` two bytes at a time. A code below 256 is a byte, 256 clears the table, and any other
   code that is defined is copied from the output written so far. A code that is not defined yet,
   or the end code 257, stops the loop. The result is size, or 0 if the stream does not give
   exactly size bytes. */
uint32_t dat_lzw(const uint8_t *src, uint32_t src_size, uint8_t *out, uint32_t size)
{
    const uint8_t *in = src, *const in_end = src + src_size;
    uint8_t *d = out, *const out_end = out + size;
    uint32_t bits = 0, nbits = 0, width = 9, mask = 511;
    uint32_t strings = 0;               /* the entries in start[] since the last clear */
    uint32_t grow = 511 - 257;          /* the entry made at this count widens the codes */

    while (d != out_end) {
        if (nbits < width) {            /* two bytes at a time: this gives at most 27 bits */
            if (LIKELY(in_end - in >= 2)) {
                bits |= (in[0] | (uint32_t)in[1] << 8) << nbits;
                in += 2;
                nbits += 16;
            } else {
                if (in != in_end) {
                    bits |= (uint32_t)*in++ << nbits;
                    nbits += 8;
                }
                if (nbits < width) return 0;
            }
        }
        uint32_t code = bits & mask;
        bits >>= width;
        nbits -= width;
        uint32_t k = code - FIRST;
        if (UNLIKELY(k >= strings) && code >= 256) {
            if (code != 256) break;     /* the end, or a code not defined yet */
            width = 9;
            mask = 511;
            grow = 511 - 257;
            strings = 0;
            continue;
        }
        /* This string begins at the current output position. Its first byte completes the code
           FIRST + strings - 1. When the table gets the entry at `grow`, the codes widen by a bit,
           up to 12 bits. */
        if (LIKELY(strings < grow)) {
            start[strings++] = (uint32_t)(d - out);
        } else if (strings < CODES - FIRST + 1) {
            start[strings++] = (uint32_t)(d - out);
            if (width < 12) {
                width++;
                mask = mask * 2 + 1;
                grow = mask - 257;
            }
        }
        if (code < 256) {
            *d++ = (uint8_t)code;
            continue;
        }
        const uint32_t *t = start + k;
        const uint8_t *s = out + t[0];
        uint32_t n = t[1] - t[0] + 1, room = (uint32_t)(out_end - d);
        if (LIKELY(k + 2 != strings && n + 3 <= room)) {
            /* Copy four bytes at a time. Up to three bytes too many are written; the next string
               overwrites them. */
            uint8_t *end = d + n;
            do {
                uint8_t b0 = s[0], b1 = s[1], b2 = s[2], b3 = s[3];
                d[0] = b0;
                d[1] = b1;
                d[2] = b2;
                d[3] = b3;
                s += 4;
                d += 4;
            } while (d < end);
            d = end;
        } else {                        /* byte by byte: the code this string completes, or the end */
            uint8_t *end = n < room ? d + n : out_end;
            do *d++ = *s++;
            while (d != end);
        }
    }
    return d == out_end ? size : 0;
}
