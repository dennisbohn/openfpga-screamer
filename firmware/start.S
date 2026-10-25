/*
 * Start-up code, first in the boot RAM (link.ld keeps .text.start at 0x00000000).
 * The CPU starts at 0x00000000 and takes every trap at 0x00000020 until mtvec is written
 * (docs/soc.md); reset points mtvec at that same entry, so all traps take one path.
 */
    .section .text.start, "ax"
    .globl  _start
    .type   _start, @function
_start:
    j       reset                   /* 0x00: reset vector */
    .size   _start, . - _start

    .org    0x20
    .globl  trap_entry
    .type   trap_entry, @function
trap_entry:                         /* 0x20: any trap. Never returns, so nothing is saved. */
    mv      a0, sp                  /* trap_handler(sp, ra) logs the interrupted sp and ra */
    mv      a1, ra
    .option push
    .option norelax                 /* gp must not be relaxed against itself */
    la      gp, __global_pointer$   /* gp and sp may be anything when the trap hits */
    .option pop
    la      sp, __stack_top
    j       trap_handler
    .size   trap_entry, . - trap_entry

    .type   reset, @function
reset:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    la      sp, __stack_top         /* 0x8000, top of the 32 KB; the stack grows down */

    la      t0, trap_entry          /* direct mode: the entry is 4-byte aligned */
    csrw    mtvec, t0

    li      t0, 3 << 13             /* mstatus.FS = 01 (Initial): FPU on */
    csrc    mstatus, t0
    li      t0, 1 << 13
    csrs    mstatus, t0
    csrwi   fcsr, 0                 /* round to nearest, ties to even; flags cleared */

    la      tp, __tls_base          /* picolibc: errno lives in TLS */

    la      a0, __bss_start         /* clear .tbss and .bss (word aligned in link.ld) */
    la      a1, __bss_end
1:  bgeu    a0, a1, 2f
    sw      zero, 0(a0)
    addi    a0, a0, 4
    j       1b
2:
    call    main
3:  j       3b                      /* main does not return */
    .size   reset, . - reset
