/*
 * Data slots that the core asks the APF for (deferload in core/data.json). program.bin and the
 * game's files go into the SDRAM only after the boot loader has found its clock phase, because
 * the phase sweep may write anywhere while it tries a bad phase.
 */
#pragma once
#include <stdint.h>

/* Read len bytes of data slot id, starting at byte offset of the file, into the memory at addr.
   len is a multiple of 4, or SLOT_REST for the rest of the file: the APF sizes it and the last
   word is filled up. addr is a bridge address; for the SDRAM it equals the CPU's address.
   Waits until the APF is done and returns its result: 0 = ok, or -1 after 20 seconds.
   The data cache is invalidated, so the CPU sees the new data. */
int slot_read(uint32_t id, uint32_t offset, uint32_t addr, uint32_t len);

/* The same read, but only started (the music uses it, music.c). slot_busy() is true until the
   APF is done; then slot_result() gives its result. The data cache is not invalidated. Make
   no other slot call in the meantime. */
void slot_start(uint32_t id, uint32_t offset, uint32_t addr, uint32_t len);
int slot_busy(void);
int slot_result(void);

/* The size of the file of data slot id, as the APF put it into the slot table when it loaded
   the core (deferload slots too). 0 if the slot has no file or the table has no such slot. */
uint32_t slot_size(uint32_t id);

/* Enter size as the file size of data slot id into the APF's slot table. A nonvolatile slot is
   written back on exit with this size (the saves, save.h). */
void slot_set_size(uint32_t id, uint32_t size);
