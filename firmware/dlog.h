/*
 * Debug log: text in the 4 KB at DLOG_BASE, which the Pocket saves as
 * /Saves/screamer/Chainy.Screamer/debug.txt when the core is quit (docs/soc.md).
 *
 * The first DLOG_HEAD bytes are one line that dlog_head() rewrites in place: the firmware's
 * name and a live frame count, so that a hang shows how far it got. The other functions append
 * after it. Lines end with '\n', and the rest of the 4 KB stays 0. Nothing is ever written
 * past the 4 KB; the last DLOG_RESERVE bytes are kept for the trap handler.
 */
#pragma once
#include <stdint.h>

/* The first line: 127 characters and '\n'. firmware.bin and program.bin must be of the same
   build, so that both use the same value. */
#define DLOG_HEAD       128
#define DLOG_RESERVE    256     /* only dlog_trap() opens these */

void dlog_init(void);           /* clear the 4 KB; call first thing at boot */
void dlog_head(const char *s);  /* (re)write the first line, cut or padded to DLOG_HEAD - 1 */
/* Hand the log over from the boot loader to the program: dlog_state() returns where the next
   byte goes and the bytes already in that word, and dlog_attach() takes them over. */
void dlog_state(uint32_t *pos, uint32_t *cur);
void dlog_attach(uint32_t pos, uint32_t cur);
void dlog_char(char c);
void dlog_str(const char *s);
void dlog_dec(int32_t v);
void dlog_hex(uint32_t v);      /* 8 digits */
void dlog_scale(float v);       /* a factor as fmt_scale() writes it: "1.50", or "off" for 0 */
/* Stop appending `bytes` sooner than usual, to keep room for lines that must still get in
   (for example a benchmark's result). 0 gives the room back. Nothing already written is
   touched. */
void dlog_keep(uint32_t bytes);
uint32_t dlog_kept(void);       /* the room dlog_keep() holds back now */
uint32_t dlog_free(void);       /* the bytes still free before the limit (not counting dlog_keep()'s room) */
/* For the trap handler: initialise the log if that has not happened, open the reserve, and
   finish a line that was cut off. */
void dlog_trap(void);
