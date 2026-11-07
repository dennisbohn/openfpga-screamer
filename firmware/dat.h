/*
 * The game's .DAT containers, read as the GOG folder has them (tools/datfile.py is the same
 * reader in Python). The format is from openscreamer docs/NOTES.md:
 *   file     [data][N x 32-byte records][u32 directory size]
 *   record   the name (16 bytes, each XOR 0x3A, NUL-padded), then u32 offset, u32 stored size,
 *            u32 size, u32 method
 *   method   0 is stored. 1 to 3 are LZW: codes of 9 to 12 bits, LSB first. Code 256 clears
 *            the table, 257 ends the stream, and new codes start at 258. The codes get one bit
 *            wider once the next free code reaches 512, 1024 and 2048.
 *
 * Every read of the file is bounds-checked, and nothing is written past the output's size.
 * dat_lzw() keeps a static table of 15 KB (.bss) and is not reentrant.
 * Its cost on the CPU is 11 instructions per output byte (counted in QEMU over the LZW entries
 * that track 1 needs). With the D-cache misses (0.15 per byte) it is about 18 cycles a byte, so
 * about 1 s for 4 MB.
 */
#pragma once
#include <stdint.h>

struct dat_entry { char name[17]; uint32_t offset, stored, size, method; };

/* The number of records in the directory, up to the first one with an empty name. Returns 0 if
   the file is malformed. */
uint32_t dat_count(const uint8_t *file, uint32_t file_size);
/* Finds the first record named name (ASCII letters in either case). Returns 1 if it is found. */
int dat_find(const uint8_t *file, uint32_t file_size, const char *name, struct dat_entry *e);
/* Unpacks entry e into out. Returns e->size. Returns 0 if out_max is smaller than the entry, if the
   data lies outside the file, if the method is not 0 to 3, or if the stream is corrupt. An empty
   entry gives 0 as well. */
uint32_t dat_extract(const uint8_t *file, uint32_t file_size, const struct dat_entry *e, uint8_t *out,
                     uint32_t out_max);
/* Decodes one LZW stream of src_size bytes into exactly size bytes of out. Returns size, or 0 if
   the stream is too short, ends early, or uses a code that is not defined yet. out is read back
   as well: a code's string is copied from where it was written earlier. */
uint32_t dat_lzw(const uint8_t *src, uint32_t src_size, uint8_t *out, uint32_t size);
