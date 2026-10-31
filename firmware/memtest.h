/*
 * Memory test of step 2 (docs/memory.md): the SDRAM with patterns, its clock phase window
 * measured on the Pocket, results in the debug log, on the screen and as SYS_SIM bits.
 */
#pragma once
#include <stdint.h>

/* SYS_SIM result bits 3..12 (bits 0..2 are the CPU self-tests): 1 = passed */
#define MT_BIT_FIRST    3
#define MT_BITS         10
#define MT_ALL          (((1u << MT_BITS) - 1) << MT_BIT_FIRST)

/* Run every test once; the last loads program.bin into the SDRAM and checks it. The patterns
   go over the first and last 2 KB (a tenth of a second with the clock phase's search), which
   leaves the data slots loaded into the SDRAM alone. progress() is called before each test
   and every 64 KB inside one. */
uint32_t memtest_run(void (*progress)(const char *what, uint32_t addr));

/* program.bin's entry if its header and CRC-32 were right, else 0 */
uint32_t memtest_program(void);

/* The results as text rows for the screen (at most MT_ROWS, each up to 40 characters);
   returns the number of rows. Highlight rows whose fail flag is set. */
#define MT_ROWS         9
int memtest_rows(char rows[][41], uint8_t fail[]);
