/*
 * Memory test of step 2 (docs/speicher.md): SDRAM, SRAM and PSRAM cram0 with patterns,
 * the SDRAM clock phase window measured on the Pocket, results in the debug log, on the
 * screen and as SYS_SIM bits.
 */
#pragma once
#include <stdint.h>

/* SYS_SIM result bits 3..20 (bits 0..2 are the CPU self-tests): 1 = passed */
#define MT_BIT_FIRST    3
#define MT_BITS         18
#define MT_ALL          (((1u << MT_BITS) - 1) << MT_BIT_FIRST)

/* Run every test once. boot_crc: CRC-32 of the boot image, for the copy the APF loaded
   into the SDRAM. progress() is called before each test and every 64 KB inside one. */
uint32_t memtest_run(uint32_t boot_crc, void (*progress)(const char *what, uint32_t addr));

/* The results as text rows for the screen (at most MT_ROWS, each up to 40 characters);
   returns the number of rows. Highlight rows whose fail flag is set. */
#define MT_ROWS         9
int memtest_rows(char rows[][41], uint8_t fail[]);

/* CRC-32 as zlib computes it (main.c) */
uint32_t crc32(const uint8_t *p, const uint8_t *end);
