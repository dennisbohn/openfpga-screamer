/*
 * Data slots the core asks the APF for (deferload in core/data.json): program.bin and the
 * track go into the SDRAM only after the boot loader found its clock phase, because the
 * phase sweep may write anywhere while it tries a bad phase.
 */
#pragma once
#include <stdint.h>

/* Read len bytes (a multiple of 4) of data slot id from byte offset into the memory at
   addr (a bridge address, the same as the CPU's for the SDRAM). Waits until the APF is
   done; returns its result, 0 = ok, or -1 after 20 seconds. The data cache is cleared. */
int slot_read(uint32_t id, uint32_t offset, uint32_t addr, uint32_t len);
