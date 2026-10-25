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

/* Run every test once; the first checks program.bin in the SDRAM. full: patterns over the
   whole memories (about 45 s on the Pocket; program.bin is gone afterwards), else over the
   first and last 2 KB of each die, as always in the simulation (under a second), which
   leaves program.bin and the textures alone. progress() is called before each test and
   every 64 KB inside one. */
uint32_t memtest_run(int full, void (*progress)(const char *what, uint32_t addr));

/* program.bin's entry if its header and CRC-32 were right, else 0 */
uint32_t memtest_program(void);

/* The results as text rows for the screen (at most MT_ROWS, each up to 40 characters);
   returns the number of rows. Highlight rows whose fail flag is set. */
#define MT_ROWS         9
int memtest_rows(char rows[][41], uint8_t fail[]);
