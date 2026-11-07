/*
 * CRC-32 as zlib computes it. The boot loader uses it to name its own build and to check
 * program.bin; the program uses it to check the track file.
 */
#pragma once
#include <stdint.h>

/* The CRC-32 of the bytes from p up to, but not including, end */
uint32_t crc32(const uint8_t *p, const uint8_t *end);
