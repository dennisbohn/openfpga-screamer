/*
 * Start-up code of the program in the SDRAM (link_prog.ld keeps .text.header first, at
 * PROG_BASE). The header is what the boot loader checks before it jumps to prog_entry with
 * a0 = its results (SYS_SIM bits), a1, a2 = the debug log's position and current word.
 */
    .section .text.header, "ax"
    .globl  prog_header
prog_header:
    .word   0x50524353              /* "SCRP" (PROG_MAGIC) */
    .word   __image_end - __image_start     /* bytes in program.bin */
    .word   0                       /* CRC-32 of the bytes after these 16, set by the Makefile */
    .word   prog_entry

    .globl  prog_entry
    .type   prog_entry, @function
prog_entry:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    la      sp, __stack_top         /* top of the boot RAM */

    la      t0, prog_trap           /* the program's own trap entry from now on */
    csrw    mtvec, t0

    li      t0, 3 << 13             /* mstatus.FS = 01 (Initial): FPU on */
    csrc    mstatus, t0
    li      t0, 1 << 13
    csrs    mstatus, t0
    csrwi   fcsr, 0

    la      tp, __tls_base          /* picolibc: errno lives in TLS */

    la      t0, __bss_start         /* clear .tbss and .bss; a0..a2 stay for main */
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:
    call    prog_main
3:  j       3b                      /* main does not return */
    .size   prog_entry, . - prog_entry

    .balign 4
    .type   prog_trap, @function
prog_trap:                          /* any trap. Never returns, so nothing is saved. */
    mv      a0, sp                  /* trap_handler(sp, ra) logs the interrupted sp and ra */
    mv      a1, ra
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    la      sp, __stack_top
    j       trap_handler
    .size   prog_trap, . - prog_trap
