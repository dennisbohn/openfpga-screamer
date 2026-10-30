/*
 * What the game keeps between runs, as nonvolatile data slots (core/data.json) in the SDRAM:
 * CHOICE.DAT (slot 23, the game's CONFIG\CHOICE.DAT: options, championship, records,
 * unlocks; champ.h) and ATTACK.DAT (slot 24, the time attack's kept runs, ghost.h's
 * struct ghost_file for tracks 1..6 in turn, in place of the game's REPLAYS\MAPn.ATK; ghost.c
 * reads and writes them in place). save_load() reads them at boot
 * (deferload); from then on the CPU's writes there are the save: the APF reads both back over
 * the bridge when the core exits or the Pocket is switched off, as it does debug.txt.
 * The Pocket menu resets both (core/interact.json): its check box "Allow Save Reset" and its
 * action "Reset Save Data" each write a word over the bridge into the SDRAM (SAVE_ARM_ADDR,
 * SAVE_RESET_ADDR), which save_reset() looks at.
 */
#pragma once
#include <stdint.h>

#define SAVE_CHOICE_SLOT    23
#define SAVE_CHOICE_ADDR    0x13F00000u     /* CHOICE_SIZE bytes (champ.h) */
#define SAVE_ATTACK_SLOT    24
#define SAVE_ATTACK_ADDR    0x13F10000u     /* past the program's 15 MB (link_prog.ld) */
#define SAVE_ATTACK_BLOCK   60016u          /* a track's: sizeof(struct ghost_file) */
#define SAVE_ATTACK_SIZE    (6u * SAVE_ATTACK_BLOCK)
#define SAVE_ARM_ADDR       0x13FF0000u     /* the Pocket menu's words: above ATTACK.DAT, below the */
#define SAVE_RESET_ADDR     0x13FF0004u     /* memory test's last window (memtest.c) */

/* track n's block of ATTACK.DAT (1..6, ghost_track); all zeros until the time attack keeps a run */
#define SAVE_ATTACK(n)      ((uint8_t *)(uintptr_t)(SAVE_ATTACK_ADDR + ((n) - 1) * SAVE_ATTACK_BLOCK))

/* Both slots from the SD card into the SDRAM; a missing file, a failed read or a CHOICE.DAT
   that does not look like the game's is replaced by the game's defaults (zeros for
   ATTACK.DAT). Then each slot's full size into the APF's slot table, so the APF saves it on
   exit (it keeps a size only for a file it found). Early: until then, an exit would save
   what lies in the SDRAM there. */
void save_load(void);

/* The Pocket menu's "Reset Save Data", looked at once a frame: 1 if it was chosen with "Allow
   Save Reset" ticked; then both saves are the game's defaults in the SDRAM, as for missing
   files (the APF saves them on exit; the files on the SD card stay as they are until then), and
   both words are 0 again (the box unticked). Chosen without the box: ignored, 0. */
int save_reset(void);
