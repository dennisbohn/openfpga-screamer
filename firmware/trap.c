/*
 * Trap handler. Every trap is fatal: interrupts stay off, and the handler never returns.
 * start.S (or start_prog.S) enters here with a fresh gp and sp, and with the interrupted sp
 * and ra as arguments. The handler writes mcause, mepc and mtval (plus that ra and sp) to the
 * debug log, reports to the simulation through SYS_SIM, shows the same on screen and stops.
 * Look up mepc in build/firmware/firmware.lst (program.lst for a trap in the program).
 */
#include "hw.h"
#include "console.h"
#include "dlog.h"

void trap_handler(uint32_t sp, uint32_t ra) __attribute__((noreturn));

extern uint32_t frames_shown;       /* boot.c and main.c: how many frames have been swapped in */

/* The names of the exception codes 0 to 11 in mcause (RISC-V privileged architecture). Code 10
   is not defined. */
static const char *const causes[] = {
    "insn misaligned", "insn access fault", "illegal instruction", "breakpoint",
    "load misaligned", "load access fault", "store misaligned", "store access fault",
    "ecall from U-mode", "ecall from S-mode", "?", "ecall from M-mode",
};

#define BOX_ROW 18                  /* the report covers text rows 18..24 */

/* Draw the alert box into fb: the lines of text, one per text row from BOX_ROW on */
static void show(volatile uint8_t *fb, char text[][CON_COLS + 1], int lines)
{
    fb_fill(fb, 0, BOX_ROW * 8, FB_W, FB_H - BOX_ROW * 8, COL_ALERT);
    for (int i = 0; i < lines; i++)
        con_text(fb, 0, BOX_ROW + i, text[i], COL_TEXT, COL_ALERT);
}

void trap_handler(uint32_t sp, uint32_t ra)
{
    static int active;
    uint32_t mcause = csr_read(mcause), mepc = csr_read(mepc), mtval = csr_read(mtval);
    /* Bit 31 of mcause marks an interrupt; otherwise the code is an exception. */
    const char *cause = mcause >> 31 ? "interrupt"
                      : mcause < sizeof causes / sizeof causes[0] ? causes[mcause] : "?";
    if (active++)                   /* a trap inside the handler: give up quietly */
        for (;;)
            ;

    /* The log first: the Pocket saves it even when nothing else works. */
    dlog_trap();
    dlog_str("TRAP mcause=");
    dlog_hex(mcause);
    dlog_char(' ');
    dlog_str(cause);
    dlog_str("\n     mepc=");
    dlog_hex(mepc);
    dlog_str(" mtval=");
    dlog_hex(mtval);
    dlog_str(" ra=");
    dlog_hex(ra);
    dlog_str(" sp=");
    dlog_hex(sp);
    dlog_char('\n');
    SYS_SIM = SIM_TRAP | (mcause & 0x3FFFFFFFu);

    /* The text of the report, one line per row of the alert box (40 characters at most) */
    char text[7][CON_COLS + 1], *p;
    fmt_str(text[0], " CPU CRASH (trap)");
    p = fmt_hex(fmt_str(text[1], " mcause "), mcause, 8);
    fmt_str(fmt_str(p, " "), cause);
    p = fmt_hex(fmt_str(text[2], " mepc   "), mepc, 8);
    fmt_hex(fmt_str(p, "  mtval "), mtval, 8);
    p = fmt_hex(fmt_str(text[3], " ra     "), ra, 8);
    fmt_hex(fmt_str(p, "  sp    "), sp, 8);
    fmt_str(text[4], "");
    fmt_str(text[5], " Log: quit the core, then via USB:");
    fmt_str(text[6], "Saves/screamer/Chainy.Screamer/debug.txt");

    /* Draw into both buffers, so that the report shows whichever is on screen now or after a
       pending swap. Before the first frame no buffer has a palette yet: then the back buffer
       gets one and is swapped in. Later the last whole frame stays where it is. */
    uint32_t st = VID_STATUS;
    show(FB(st & VID_FRONT), text, 7);
    show(FB(!(st & VID_FRONT)), text, 7);
    if (!frames_shown) {
        pal_write();
        VID_SWAP = 1;
    }
    for (;;)
        ;
}
