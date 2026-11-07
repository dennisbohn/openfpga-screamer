/*
 * Linker script of the boot loader (firmware.bin): code, data, bss and stack all lie in the
 * 32 KB boot RAM at 0x00000000 (docs/soc.md). The Pocket writes firmware.bin into the boot RAM
 * before the CPU leaves reset, so .data needs no copy (VMA == LMA); start.S only clears .tbss
 * and .bss. firmware.bin holds exactly __image_start .. __image_end (the Makefile checks this).
 * Everything stays below 24 KB: above it is the program's stack (link_prog.ld).
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    BOOTRAM (rwx) : ORIGIN = 0x00000000, LENGTH = 32K
}

__stack_min = 2K;                   /* the link fails if less than this is left for the stack */

SECTIONS
{
    .text : {
        __image_start = .;          /* first byte of firmware.bin */
        KEEP(*(.text.start))        /* start.S: jump at 0x0, trap entry at 0x20 */
        *(.text.unlikely .text.unlikely.*)
        *(.text.startup .text.startup.*)
        *(.text .text.*)
    } > BOOTRAM

    .rodata : {
        *(.rodata .rodata.*)
    } > BOOTRAM

    .data : ALIGN(8) {
        *(.data .data.*)
        . = ALIGN(8);
        /* gp points 2 KB into the small data that follows, so that gp-relative addressing
           (+-2 KB) reaches all of it */
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.srodata.cst16) *(.srodata.cst8) *(.srodata.cst4) *(.srodata.cst2)
        *(.srodata .srodata.*)
        *(.sdata .sdata.*)
        . = ALIGN(8);               /* .tdata follows without a gap */
    } > BOOTRAM

    /* picolibc keeps errno in thread-local storage: tp points at __tls_base, and the one
       TLS block is used in place (.tdata as loaded, then the cleared .tbss). */
    .tdata : ALIGN(8) {
        PROVIDE(__tls_base = .);
        *(.tdata .tdata.*)
        . = ALIGN(4);
        __image_end = .;            /* one byte past the last byte of firmware.bin */
    } > BOOTRAM
    .tbss : ALIGN(8) {
        __bss_start = .;            /* start.S clears from here to __bss_end */
        *(.tbss .tbss.*)
        *(.tcommon)
    } > BOOTRAM
    .tbss_space (NOLOAD) : {        /* ld does not advance the location counter for .tbss: this does it */
        . = ADDR(.tbss) + SIZEOF(.tbss);
    } > BOOTRAM

    .bss (NOLOAD) : ALIGN(8) {
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > BOOTRAM

    __stack_top = ORIGIN(BOOTRAM) + LENGTH(BOOTRAM);    /* the stack grows down from the end of the RAM */
    ASSERT(__bss_end + __stack_min <= __stack_top,
           "firmware and a 2 KB stack do not fit into the 32 KB boot RAM")
    ASSERT(__bss_end <= 0x6000, "the boot loader reaches into the program's stack above 24 KB")

    /DISCARD/ : { *(.note.GNU-stack) *(.comment) }
}
