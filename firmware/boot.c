/*
 * The boot loader (firmware.bin, data slot 0, in the boot RAM; docs/soc.md). It tests the CPU
 * and the SDRAM, then starts the program in the SDRAM (program.bin, data slot 2). main() runs
 * these steps in order:
 *   1. The CPU self-tests (selftest.c): integer, floating point and CFU.
 *   2. The short memory test of the SDRAM (memtest.c): first its clock phase, then windows of
 *      2 KB, about a tenth of a second in all. It also loads program.bin and checks its
 *      header and CRC-32. Its progress goes only into the debug log's head line. The screen
 *      stays black until the program draws its first picture.
 *   3. If everything passed, a jump to program.bin with the results and the debug log's
 *      position. Otherwise the results go on screen and stay there, and after the third frame
 *      they go to SYS_SIM as well.
 */
#include "hw.h"
#include "console.h"
#include "crc32.h"
#include "dlog.h"
#include "selftest.h"
#include "memtest.h"
#include "screen.h"

uint32_t frames_shown;             /* read by the trap handler */

extern const uint8_t __image_start[], __image_end[];   /* link.ld: what firmware.bin holds */

#define ALL_PASSED  (7u | MT_ALL)    /* result bits 0..2 (self-tests) and the memory test's bits */

static int ok[3];               /* self-tests: integer, float, CFU; 1 = passed */
static uint32_t results;        /* result bits: 0..2 the self-tests, 3..12 the memory test (memtest.h) */
static int (*const tests[3])(void) = { selftest_int, selftest_float, selftest_cfu };
static const char *const test_names[3] = { "Ganzzahl", "Fliesskomma", "CFU" };     /* on the screen */
static const char *const log_names[3] = { "integer", "float", "cfu" };              /* in the log */

/* Rewrite the debug log's head line as "Screamer Pocket boot, <what><n>" */
static void log_head(const char *what, uint32_t n)
{
    char buf[DLOG_HEAD + 16];
    fmt_udec(fmt_str(fmt_str(buf, "Screamer Pocket boot, "), what), n);
    dlog_head(buf);
}

/* Draw the common part of the results screen into fb: palette, test band, background, the
   title bar and the self-test results */
static void draw_frame(volatile uint8_t *fb, const char *title)
{
    pal_write();
    draw_band(fb);
    fb_fill(fb, 0, BAND_H, FB_W, FB_H - BAND_H, COL_BG);
    fb_fill(fb, 0, 2 * 8 - 3, FB_W, 8 + 6, COL_TEXT);  /* title bar: text row 2 and 3 pixels around it */
    con_text(fb, 1, 2, title, COL_BG, COL_TEXT);
    int col = con_text(fb, 1, 4, "CPU", COL_DIM, COL_BG) + 1;
    for (int i = 0; i < 3; i++) {
        col = con_text(fb, col, 4, test_names[i], COL_TEXT, COL_BG) + 1;
        col = con_text(fb, col, 4, ok[i] ? "ok" : "XX", COL_TEXT, ok[i] ? COL_BG : COL_ALERT) + 1;
    }
}

/* memtest_run() calls this before each test and every 64 KB inside one. It writes the test
   and the address into the log's head line, so that a hang shows where. */
static void progress(const char *what, uint32_t addr)
{
    char buf[DLOG_HEAD + 16];
    fmt_hex(fmt_str(fmt_str(fmt_str(buf, "Screamer Pocket boot, "), what), " @"), addr, 8);
    dlog_head(buf);
}

/* Draw the memory test rows (memtest_rows()) and the note below them */
static void draw_results(volatile uint8_t *fb, const char *note)
{
    char rows[MT_ROWS][41];
    uint8_t fail[MT_ROWS];
    int n = memtest_rows(rows, fail);
    con_text(fb, 1, 6, "Memory", COL_DIM, COL_BG);
    for (int i = 0; i < n; i++)
        con_text(fb, 1, 7 + i, rows[i], COL_TEXT, fail[i] ? COL_ALERT : COL_BG);
    con_text(fb, 1, 18, note, COL_TEXT, COL_BG);
}

int main(void)
{
    uint32_t crc = crc32(__image_start, __image_end);  /* first, before .data changes */

    dlog_init();
    log_head("frames drawn: ", 0);
    dlog_str("firmware.bin: ");     /* the same line as tools/fw.sh prints */
    dlog_dec((int32_t)(__image_end - __image_start));
    dlog_str(" bytes, crc32 ");
    dlog_hex(crc);
    uint32_t id = SYS_ID;
    dlog_str("\nSYS_ID=");
    dlog_hex(id);
    dlog_str(id == SYS_ID_VALUE ? " ok\n" : " wrong\n");

    for (int i = 0; i < 3; i++) {   /* one line each, so a trap shows which one ran */
        ok[i] = tests[i]();
        results |= (uint32_t)ok[i] << i;
        dlog_str("selftest ");
        dlog_str(log_names[i]);
        dlog_str(ok[i] ? ": ok\n" : ": FAIL\n");
    }

#ifdef FAULT_IFETCH
    /* Fault test for sim/soc (make FAULT=IFETCH): jump to code past the end of the boot RAM.
       The trap handler has to run from lines that the cache loads after the bad one
       (docs/soc.md). */
    dlog_str("fault test: jump to 00008000\n");
    ((void (*)(void))0x00008000u)();
#endif

    /* Keep the screen black until the program's first picture: show a swapped-in buffer with an
       all-black palette. Without this, after a reset the last run's picture would stay
       (docs/soc.md). */
    wait_swap_done();
    for (int i = 0; i < 256; i++) VID_PAL[i] = 0;
    VID_SWAP = 1;
    results |= memtest_run(progress);

    uint32_t entry = memtest_program();
    if (entry && (results & ALL_PASSED) == ALL_PASSED) {
        uint32_t pos, cur;
        dlog_str("boot: starting program.bin\n");
        dlog_state(&pos, &cur);
        /* The bridge wrote program.bin behind the data cache, and the instruction cache has
           not seen it. The program's start-up code receives results, pos and cur in a0..a2. */
        dcache_invalidate();
        icache_sync();
        ((void (*)(uint32_t, uint32_t, uint32_t))(uintptr_t)entry)(results, pos, cur);
    }

    /* A test failed or there is no program to start: the results stay on the screen. Every
       loop pass waits for the last swap, draws the screen into the back buffer and swaps. */
    const char *note = entry ? "Test failed: not starting" : "program.bin missing or damaged";
    dlog_str(entry ? "boot: a test failed, the results stay on screen\n" : "boot: no program to start\n");
    uint32_t drawn = 0;
    for (;;) {
        uint32_t st = wait_swap_done();
        frames_shown = drawn;
        if (drawn == 3) {           /* the simulation reads the results after the third frame */
            SYS_SIM = SIM_DONE | results;
            dlog_str("SYS_SIM=");
            dlog_hex(SIM_DONE | results);
            dlog_char('\n');
        }
        volatile uint8_t *fb = FB(!(st & VID_FRONT));
        draw_frame(fb, "SCREAMER POCKET - SPEICHERTEST");
        draw_results(fb, note);
        VID_SWAP = 1;
        log_head("frames drawn: ", ++drawn);
    }
}
