/*
 * Start-up code of the program in the SDRAM (link_prog.ld keeps .text.header first, at
 * PROG_BASE). The header is what the boot loader checks before it jumps to prog_entry with
 * a0 = its results (SYS_SIM bits), a1 = the debug log's position and a2 = the log's current
 * word. prog_entry sets up gp, sp, the FPU and the C runtime, then calls prog_main (main.c).
 */
    .section .text.header, "ax"
    .globl  prog_header
prog_header:
    .word   0x50524353              /* "SCRP" (PROG_MAGIC) */
    .word   __image_end - __image_start     /* bytes in program.bin */
    .word   0                       /* CRC-32 of the bytes after these 16; prog_crc.py fills it in */
    .word   prog_entry              /* address where the boot loader starts the program */

    .globl  prog_entry
    .type   prog_entry, @function
prog_entry:
    .option push
    .option norelax                 /* no gp-relative relaxation: gp is not valid yet */
    la      gp, __global_pointer$
    .option pop
    la      sp, __stack_top         /* top of the boot RAM */

    la      t0, prog_trap           /* the program's own trap entry */
    csrw    mtvec, t0

    li      t0, 3 << 13             /* mstatus.FS = 01 (Initial) turns the FPU on: */
    csrc    mstatus, t0             /* first clear both FS bits, */
    li      t0, 1 << 13
    csrs    mstatus, t0             /* then set the low one */
    csrwi   fcsr, 0

    la      tp, __tls_base          /* picolibc: errno lives in TLS */

    la      t0, __bss_start         /* clear .tbss and .bss; a0..a2 stay for prog_main */
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:
    call    prog_main
3:  j       3b                      /* prog_main does not return */
    .size   prog_entry, . - prog_entry

    .balign 4                       /* mtvec in direct mode: the address must be 4-byte aligned */
    .type   prog_trap, @function
prog_trap:                          /* any trap. Never returns, so nothing is saved. */
    mv      a0, sp                  /* trap_handler(sp, ra) logs the interrupted sp and ra: */
    mv      a1, ra                  /* these are its two arguments */
    .option push
    .option norelax                 /* no gp-relative relaxation: gp is not valid yet */
    la      gp, __global_pointer$
    .option pop
    la      sp, __stack_top
    j       trap_handler
    .size   prog_trap, . - prog_trap
