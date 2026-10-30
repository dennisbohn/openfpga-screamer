/*
 * The program (program.bin): code, data and bss in the SDRAM at PROG_BASE (hw.h), run
 * through the caches; the stack at the top of the boot RAM, which the boot loader no
 * longer needs. The Pocket loads program.bin with data slot 2, so .data needs no copy;
 * start_prog.S clears .tbss and .bss. program.bin holds exactly __image_start ..
 * __image_end and starts with the header the boot loader checks.
 */
OUTPUT_ARCH(riscv)
ENTRY(prog_entry)

MEMORY
{
    SDRAM (rwx) : ORIGIN = 0x13000000, LENGTH = 15M     /* above: the saves (save.h) */
}

SECTIONS
{
    .text : {
        __image_start = .;
        KEEP(*(.text.header))       /* start_prog.S: header, entry, trap entry */
        KEEP(*(.text.start))
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
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.srodata.cst16) *(.srodata.cst8) *(.srodata.cst4) *(.srodata.cst2)
        *(.srodata .srodata.*)
        *(.sdata .sdata.*)
        . = ALIGN(8);
    } > SDRAM

    .tdata : ALIGN(8) {
        PROVIDE(__tls_base = .);
        *(.tdata .tdata.*)
        . = ALIGN(4);
        __image_end = .;
    } > SDRAM
    .tbss : ALIGN(8) {
        __bss_start = .;
        *(.tbss .tbss.*)
        *(.tcommon)
    } > SDRAM
    .tbss_space (NOLOAD) : {
        . = ADDR(.tbss) + SIZEOF(.tbss);
    } > SDRAM

    .bss (NOLOAD) : ALIGN(8) {
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > SDRAM

    __stack_top = 0x00008000;       /* top of the 32 KB boot RAM */

    /DISCARD/ : { *(.note.GNU-stack) *(.comment) }
}
