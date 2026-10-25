/*
 * CRC-32 as zlib computes it: names the build, checks program.bin and the track file.
 */
#pragma once
#include <stdint.h>

uint32_t crc32(const uint8_t *p, const uint8_t *end);
