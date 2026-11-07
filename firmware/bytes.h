/*
 * Reads and writes of 16 and 32 bit numbers in the game's files and in the track image.
 * The CPU does no unaligned loads, so a number at an address that is not a multiple of its width is read
 * and written a byte at a time. The names say how a helper works:
 *   le16u, le32u, le32s   read little-endian a byte at a time, so any address is fine; u is unsigned, s signed
 *   put_le16, put_le32    write little-endian a byte at a time
 *   ld16u, ld16s, ld32s   read in one access through a pointer cast, so the address must be aligned
 */
#pragma once
#include <stdint.h>

/* The unsigned 16 bit number at p, little-endian, at any address. The result is 32 bits wide. */
static inline uint32_t le16u(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

/* The unsigned 32 bit number at p, little-endian, at any address */
static inline uint32_t le32u(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The signed 32 bit number at p, little-endian, at any address */
static inline int32_t le32s(const uint8_t *p)
{
    return (int32_t)le32u(p);
}

/* Writes the low 16 bits of v at p, little-endian, at any address */
static inline void put_le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* Writes v at p, little-endian, at any address */
static inline void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* The unsigned 16 bit number at p, in one access: p must be a multiple of 2 */
static inline uint16_t ld16u(const uint8_t *p)
{
    return *(const uint16_t *)p;
}

/* The signed 16 bit number at p, in one access: p must be a multiple of 2 */
static inline int16_t ld16s(const uint8_t *p)
{
    return *(const int16_t *)p;
}

/* The signed 32 bit number at p, in one access: p must be a multiple of 4 */
static inline int32_t ld32s(const uint8_t *p)
{
    return *(const int32_t *)p;
}
