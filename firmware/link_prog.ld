/*
 * Linker script of the program (program.bin): code, data and bss lie in the SDRAM at PROG_BASE
 * (hw.h) and run through the caches. The stack is at the top of the boot RAM, which the boot
 * loader no longer needs. The Pocket loads program.bin with data slot 2, so .data needs no copy;
 * start_prog.S clears .tbss and .bss. program.bin holds exactly __image_start .. __image_end
 * and starts with the header that the boot loader checks.
 */
OUTPUT_ARCH(riscv)
ENTRY(prog_entry)

MEMORY
{
    SDRAM (rwx) : ORIGIN = 0x13000000, LENGTH = 15M     /* the saved games start above it (save.h) */
}

SECTIONS
{
    .text : {
        __image_start = .;          /* first byte of program.bin */
        KEEP(*(.text.header))       /* start_prog.S: header, entry, trap entry */
        KEEP(*(.text.start))
        /* The code that runs for every piece of a frame is placed together here. The
           instruction cache is 8 KB and direct mapped: two hot pieces of code that lie a
           multiple of 8 KB apart (mod 8 KB) throw each other out on every piece. For example,
           hw_piece and the loop in draw can collide this way when they lie apart. Inside one
           block of under 8 KB they cannot collide (the ASSERT below checks the size). */
        __hot_start = .;
        *(.text.tl_mark) *(.text.hw_piece) *(.text.face_eye) *(.text.block_piece) *(.text.draw)
        __hot_end = .;
        *(.text.unlikely .text.unlikely.*)
        *(.text.startup .text.startup.*)
        *(.text .text.*)
    } > SDRAM

    .rodata : {
        *(.rodata .rodata.*)
    } > SDRAM

    .data : ALIGN(8) {
        *(.data .data.*)
        . = ALIGN(8);
        /* gp points 2 KB into the small data that follows, so that gp-relative addressing
           (+-2 KB) reaches all of it */
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.srodata.cst16) *(.srodata.cst8) *(.srodata.cst4) *(.srodata.cst2)
        *(.srodata .srodata.*)
        *(.sdata .sdata.*)
        . = ALIGN(8);
    } > SDRAM

    /* picolibc keeps errno in thread-local storage: tp points at __tls_base, and the one
       TLS block is used in place (.tdata as loaded, then the cleared .tbss). */
    .tdata : ALIGN(8) {
        PROVIDE(__tls_base = .);
        *(.tdata .tdata.*)
        . = ALIGN(4);
        __image_end = .;            /* one byte past the last byte of program.bin */
    } > SDRAM
    .tbss : ALIGN(8) {
        __bss_start = .;            /* start_prog.S clears from here to __bss_end */
        *(.tbss .tbss.*)
        *(.tcommon)
    } > SDRAM
    .tbss_space (NOLOAD) : {        /* ld does not advance the location counter for .tbss: this does it */
        . = ADDR(.tbss) + SIZEOF(.tbss);
    } > SDRAM

    .bss (NOLOAD) : ALIGN(8) {
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > SDRAM

    /* The hot code must fit into the 8 KB instruction cache (tl_mark is only in a debug build) */
    ASSERT(__hot_end - __hot_start <= 8192,
           "the hot code (hw_piece, face_eye, block_piece, draw, and tl_mark in a debug build) is over 8 KB: it would collide with itself in the I-cache")

    __stack_top = 0x00008000;       /* top of the 32 KB boot RAM; the stack grows down from it */

    /DISCARD/ : { *(.note.GNU-stack) *(.comment) }
}
