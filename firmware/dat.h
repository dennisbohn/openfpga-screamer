/*
 * The game's .DAT containers, read as the GOG folder has them (tools/datfile.py is the same
 * reader in Python). From openscreamer docs/NOTES.md: [data][N x 32-byte records][u32
 * directory size]; a record is the name (16 bytes XOR 0x3A, NUL-padded), then u32 offset,
 * stored size, size, method. Method 0 is stored, 1-3 are LZW: codes of 9 to 12 bits, LSB
 * first; 256 clears the table, 257 ends the stream, new codes from 258 on, one bit wider
 * once the next free code reaches 512, 1024, 2048.
 *
 * Every read of the file is bounds-checked; nothing is written past the output's size.
 * dat_lzw() keeps a static table of 15 KB (.bss) and is not reentrant. Its cost on the CPU:
 * 11 instructions per output byte (counted in QEMU over the LZW entries track 1 needs),
 * about 18 cycles with the D-cache misses (0.15 per byte), so about 1 s for 4 MB.
 */
#pragma once
#include <stdint.h>

struct dat_entry { char name[17]; uint32_t offset, stored, size, method; };

/* Records in the directory, up to the first one with an empty name; 0 if malformed */
uint32_t dat_count(const uint8_t *file, uint32_t file_size);
/* Record i (0 .. dat_count() - 1): 1 if it exists */
int dat_entry_at(const uint8_t *file, uint32_t file_size, uint32_t i, struct dat_entry *e);
/* The first record named name (ASCII letters in either case): 1 if found */
int dat_find(const uint8_t *file, uint32_t file_size, const char *name, struct dat_entry *e);
/* Entry e into out: returns e->size, or 0 if out_max is smaller, the data lies outside the
   file, the method is not 0-3 or the stream is corrupt (an empty entry gives 0 as well) */
uint32_t dat_extract(const uint8_t *file, uint32_t file_size, const struct dat_entry *e, uint8_t *out,
                     uint32_t out_max);
/* One LZW stream of src_size bytes: exactly size bytes into out, or 0 (stream too short,
   ended early or a code not yet defined). out is read back as well: a code's string is
   copied from where it was written before. */
uint32_t dat_lzw(const uint8_t *src, uint32_t src_size, uint8_t *out, uint32_t size);
