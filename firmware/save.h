/*
 * What the game keeps between runs, as nonvolatile data slots (core/data.json) in the SDRAM:
 * CHOICE.DAT (slot 23, the game's CONFIG\CHOICE.DAT: options, championship, records,
 * unlocks; champ.h) and ATTACK.DAT (slot 24, the time attack's kept runs, ghost.h's
 * struct ghost_file for tracks 1..6 in turn, in place of the game's REPLAYS\MAPn.ATK; ghost.c
 * reads and writes them in place). save_load() reads them at boot
 * (deferload); from then on the CPU's writes there are the save: the APF reads both back over
 * the bridge when the core exits or the Pocket is switched off, as it does debug.txt.
 */
#pragma once
#include <stdint.h>

#define SAVE_CHOICE_SLOT    23
#define SAVE_CHOICE_ADDR    0x13F00000u     /* CHOICE_SIZE bytes (champ.h) */
#define SAVE_ATTACK_SLOT    24
#define SAVE_ATTACK_ADDR    0x13F10000u     /* past the program's 15 MB (link_prog.ld) */
#define SAVE_ATTACK_BLOCK   60016u          /* a track's: sizeof(struct ghost_file) */
#define SAVE_ATTACK_SIZE    (6u * SAVE_ATTACK_BLOCK)

/* track n's block of ATTACK.DAT (1..6, ghost_track); all zeros until the time attack keeps a run */
#define SAVE_ATTACK(n)      ((uint8_t *)(uintptr_t)(SAVE_ATTACK_ADDR + ((n) - 1) * SAVE_ATTACK_BLOCK))

/* Both slots from the SD card into the SDRAM; a missing file, a failed read or a CHOICE.DAT
   that does not look like the game's is replaced by the game's defaults (zeros for
   ATTACK.DAT). Then each slot's full size into the APF's slot table, so the APF saves it on
   exit (it keeps a size only for a file it found). Early: until then, an exit would save
   what lies in the SDRAM there. */
void save_load(void);
