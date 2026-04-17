/* Linker script for generated bare-metal tests.
 *
 * The auto-generated ALU test ELF is too large for IRAM (8 KB).  Place .text
 * in SRAM (256 KB) so it has room to grow and leaves stack space at the top
 * of IRAM where crt0 places it.
 */

MEMORY {
    SRAM (rwx) : ORIGIN = 0x100000, LENGTH = 0x40000
}

SECTIONS {
    . = 0x100000;

    .text : {
        KEEP(*(.text._start))
        *(.text .text.*)
        . = ALIGN(4);
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > SRAM

    .data : ALIGN(4) {
        *(.data .data.*)
        . = ALIGN(4);
    } > SRAM

    __bss_start = .;
    .bss (NOLOAD) : ALIGN(4) {
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
    } > SRAM
    __bss_end = .;

    /* Stack stays at top of IRAM (matches crt0 setup) */
    __stack_top = 0x002000;

    /DISCARD/ : {
        *(.note .note.*)
        *(.comment)
        *(.eh_frame .eh_frame_hdr)
    }
}
