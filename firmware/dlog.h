/*
 * Debug log: text in the 4 KB at DLOG_BASE, which the Pocket saves as
 * /Saves/screamer/Chainy.Screamer/debug.txt when the core is quit (docs/soc.md).
 *
 * The first DLOG_HEAD bytes are one line that dlog_head() rewrites in place (firmware
 * name and a live frame count, so a hang shows how far it got); the other functions append
 * after it. Lines end with '\n', the rest of the 4 KB stays 0. Nothing is ever written
 * past the 4 KB; the last DLOG_RESERVE bytes are kept for the trap handler.
 */
#pragma once
#include <stdint.h>

#define DLOG_HEAD       128     /* first line: 127 characters and '\n' (firmware.bin and program.bin must be of the same build) */
#define DLOG_RESERVE    256     /* only dlog_trap() opens these */

void dlog_init(void);           /* clear the 4 KB; call first thing at boot */
void dlog_head(const char *s);  /* (re)write the first line, cut or padded to DLOG_HEAD - 1 */
/* hand the log over from the boot loader to the program: where the next byte goes and the
   bytes already in that word */
void dlog_state(uint32_t *pos, uint32_t *cur);
void dlog_attach(uint32_t pos, uint32_t cur);
void dlog_char(char c);
void dlog_str(const char *s);
void dlog_dec(int32_t v);
void dlog_hex(uint32_t v);      /* 8 digits */
/* Stop the appends bytes sooner than the end, to keep room for lines that must get in (a benchmark's result): 0 gives
   the room back. Nothing already written is touched */
void dlog_keep(uint32_t bytes);
uint32_t dlog_kept(void);       /* the room dlog_keep holds back now */
uint32_t dlog_free(void);       /* the bytes still free before the limit (dlog_keep's room is not counted) */
void dlog_trap(void);           /* for the trap handler: init if needed, open the reserve,
                                   finish a cut-off line */
