/*
 * The boot loader (firmware.bin, data slot 0, in the boot RAM; docs/soc.md): the CPU tests
 * itself and the memories, then starts the program in the SDRAM (program.bin, data slot 2).
 * CPU self-tests, then the memory test of SDRAM, SRAM and PSRAM cram0 (quick, or over the
 * whole memories while Select is held), its progress on screen and in the debug log's head
 * line; the memory test also checks program.bin's header and CRC-32. If that passed (and
 * the full test did not overwrite it), a jump to program.bin with the results and the debug
 * log's position. Otherwise the results stay on screen and go to SYS_SIM after the third
 * frame.
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

static int ok[3];               /* self-tests: integer, float, CFU */
static uint32_t results;        /* SYS_SIM bits 20..0 */
static int (*const tests[3])(void) = { selftest_int, selftest_float, selftest_cfu };
static const char *const test_names[3] = { "Ganzzahl", "Fliesskomma", "CFU" };
static const char *const log_names[3] = { "integer", "float", "cfu" };
static volatile uint8_t *progress_fb;

static void log_head(const char *what, uint32_t n)
{
    char buf[DLOG_HEAD + 16];
    fmt_udec(fmt_str(fmt_str(buf, "Screamer Pocket boot, "), what), n);
    dlog_head(buf);
}

/* band, background, title, self-test results */
static void draw_frame(volatile uint8_t *fb, const char *title)
{
    pal_write();
    draw_band(fb);
    fb_fill(fb, 0, BAND_H, FB_W, FB_H - BAND_H, COL_BG);
    fb_fill(fb, 0, 2 * 8 - 3, FB_W, 8 + 6, COL_TEXT);  /* title bar, 3 pixels around the text */
    con_text(fb, 1, 2, title, COL_BG, COL_TEXT);
    int col = con_text(fb, 1, 4, "CPU", COL_DIM, COL_BG) + 1;
    for (int i = 0; i < 3; i++) {
        col = con_text(fb, col, 4, test_names[i], COL_TEXT, COL_BG) + 1;
        col = con_text(fb, col, 4, ok[i] ? "ok" : "XX", COL_TEXT, ok[i] ? COL_BG : COL_ALERT) + 1;
    }
}

/* memtest_run() calls this before each test and every 64 KB: on screen and in the log */
static void progress(const char *what, uint32_t addr)
{
    char buf[DLOG_HEAD + 16];
    fmt_hex(fmt_str(fmt_str(fmt_str(buf, "teste "), what), " bei "), addr, 8);
    fb_fill(progress_fb, 0, 8 * 8, FB_W, 8, COL_BG);
    con_text(progress_fb, 1, 8, buf, COL_TEXT, COL_BG);
    fmt_hex(fmt_str(fmt_str(fmt_str(buf, "Screamer Pocket boot, "), what), " @"), addr, 8);
    dlog_head(buf);
}

static void draw_results(volatile uint8_t *fb, const char *note)
{
    char rows[MT_ROWS][41];
    uint8_t fail[MT_ROWS];
    int n = memtest_rows(rows, fail);
    con_text(fb, 1, 6, "Speicher", COL_DIM, COL_BG);
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
    /* fault test for sim/soc (make FAULT=IFETCH): code past the end of the boot RAM. The
       trap handler has to run from lines the cache loads after the bad one (docs/soc.md). */
    dlog_str("fault test: jump to 00008000\n");
    ((void (*)(void))0x00008000u)();
#endif

    /* the memory test, its progress drawn into the buffer on screen */
    uint32_t st = wait_swap_done();
    int full = (IN_KEY & KEY_SELECT) != 0;
    progress_fb = FB(!(st & VID_FRONT));
    draw_frame(progress_fb, "SCREAMER POCKET - SPEICHERTEST");
    VID_SWAP = 1;
    wait_swap_done();
    results |= memtest_run(full, progress);

    uint32_t entry = memtest_program();
    if (entry && !full) {
        uint32_t pos, cur;
        dlog_str("boot: starting program.bin\n");
        dlog_state(&pos, &cur);
        dcache_invalidate();
        icache_sync();
        ((void (*)(uint32_t, uint32_t, uint32_t))(uintptr_t)entry)(results, pos, cur);
    }

    /* no program to start: the results stay on screen */
    const char *note = full ? "Voller Test: Neustart fuer das Programm" : "program.bin fehlt oder ist defekt";
    dlog_str(full ? "boot: full memory test, its results stay on screen: restart the core for the program\n"
                  : "boot: no program to start\n");
    uint32_t drawn = 0;
    for (;;) {
        st = wait_swap_done();
        frames_shown = drawn;
        if (drawn == 3) {
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
