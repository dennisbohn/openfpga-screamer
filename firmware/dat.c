#include "dat.h"
#include <string.h>

#define REC         32          /* bytes per directory record */
#define FIRST       258         /* the first code of the table */
#define CODES       4096        /* 12 bits */

/* the CPU does no unaligned loads */
static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* the first record of the directory and the number of whole records, 0 if malformed */
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

int dat_entry_at(const uint8_t *file, uint32_t file_size, uint32_t i, struct dat_entry *e)
{
    if (i >= dat_count(file, file_size)) return 0;
    uint32_t records;
    record(directory(file, file_size, &records) + i * REC, e);
    return 1;
}

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

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
 * The table holds no strings, only where they are in the output, and a code is copied from
 * there (as an LZ77 decoder does) instead of walking a chain of prefixes. Every code after
 * a clear writes one string; string j (from 0) begins at start[j]. Code 258 + j is string j
 * plus the first byte of string j + 1: out[start[j] .. start[j + 1]], both ends included.
 * So a code is defined once the string after its own has begun. The code a string
 * completes may be that very string; its last byte is then its first, which only a byte
 * copy in order gets right. Every other code lies wholly before the output position.
 */
static uint32_t start[CODES - FIRST + 1];

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

uint32_t dat_lzw(const uint8_t *src, uint32_t src_size, uint8_t *out, uint32_t size)
{
    const uint8_t *in = src, *const in_end = src + src_size;
    uint8_t *d = out, *const out_end = out + size;
    uint32_t bits = 0, nbits = 0, width = 9, mask = 511;
    uint32_t strings = 0;               /* since the clear: entries in start[] */
    uint32_t grow = 511 - 257;          /* the entry made at this count widens the codes */

    while (d != out_end) {
        if (nbits < width) {            /* two bytes at a time: at most 27 bits */
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
        /* this string's start completes code FIRST + strings - 1 */
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
            /* four bytes at a time; up to three too many, which the next string overwrites */
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
        } else {                        /* the code this string completes, or the output's end */
            uint8_t *end = n < room ? d + n : out_end;
            do *d++ = *s++;
            while (d != end);
        }
    }
    return d == out_end ? size : 0;
}
