/*
 * Data slots the core asks the APF for (deferload in core/data.json): program.bin and the
 * game's files go into the SDRAM only after the boot loader found its clock phase, because
 * the phase sweep may write anywhere while it tries a bad phase.
 */
#pragma once
#include <stdint.h>

/* Read len bytes (a multiple of 4, or SLOT_REST for the rest of the file: the APF sizes
   it, the last word filled up) of data slot id from byte offset into the memory at addr
   (a bridge address, the same as the CPU's for the SDRAM). Waits until the APF is done;
   returns its result, 0 = ok, or -1 after 20 seconds. The data cache is cleared. */
int slot_read(uint32_t id, uint32_t offset, uint32_t addr, uint32_t len);

/* The same read, started only (the music's, music.c): slot_busy() until the APF is done, then
   slot_result() its result. The data cache is not cleared. No other slot call meanwhile. */
void slot_start(uint32_t id, uint32_t offset, uint32_t addr, uint32_t len);
int slot_busy(void);
int slot_result(void);

/* The size of data slot id's file as the APF put it into the slot table when it loaded
   the core (deferload slots too); 0 if the slot has no file or the table no such slot */
uint32_t slot_size(uint32_t id);
