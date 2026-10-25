/*
 * The boot loader: code, data, bss and stack all in the 32 KB boot RAM at 0x00000000
 * (docs/soc.md). The Pocket writes firmware.bin into the boot RAM before the CPU leaves
 * reset, so .data needs no copy (VMA == LMA); start.S only clears .tbss and .bss.
 * firmware.bin holds exactly __image_start .. __image_end (the Makefile checks this).
 * Everything stays below 24 KB: above it is the program's stack (link_prog.ld).
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    BOOTRAM (rwx) : ORIGIN = 0x00000000, LENGTH = 32K
}

__stack_min = 2K;                   /* the link fails if less is left for the stack */

SECTIONS
{
    .text : {
        __image_start = .;
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
        PROVIDE(__global_pointer$ = . + 0x800);    /* gp reaches the small data below */
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
        __image_end = .;            /* last byte of firmware.bin + 1 */
    } > BOOTRAM
    .tbss : ALIGN(8) {
        __bss_start = .;
        *(.tbss .tbss.*)
        *(.tcommon)
    } > BOOTRAM
    .tbss_space (NOLOAD) : {        /* ld does not advance . for .tbss */
        . = ADDR(.tbss) + SIZEOF(.tbss);
    } > BOOTRAM

    .bss (NOLOAD) : ALIGN(8) {
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > BOOTRAM

    __stack_top = ORIGIN(BOOTRAM) + LENGTH(BOOTRAM);
    ASSERT(__bss_end + __stack_min <= __stack_top,
           "firmware and a 2 KB stack do not fit into the 32 KB boot RAM")
    ASSERT(__bss_end <= 0x6000, "the boot loader reaches into the program's stack above 24 KB")

    /DISCARD/ : { *(.note.GNU-stack) *(.comment) }
}
