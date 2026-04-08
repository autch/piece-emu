/* Bare-metal linker script for piece-emu integration tests.
 *
 * Code runs entirely in IRAM (0x000000–0x001FFF, 8 KB).
 * The ELF entry point is _start in crt0.s; piece_emu sets PC there directly.
 *
 * Memory map (S1C33209 as emulated):
 *   IRAM:  0x000000–0x001FFF   8 KB internal RAM (0-wait)
 *   SRAM:  0x100000–0x13FFFF 256 KB external SRAM
 */

MEMORY {
    IRAM (rwx) : ORIGIN = 0x000000, LENGTH = 0x2000
}

SECTIONS {
    . = 0x000000;

    .text : {
        KEEP(*(.text._start))
        *(.text .text.*)
        . = ALIGN(4);
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > IRAM

    .data : ALIGN(4) {
        *(.data .data.*)
        . = ALIGN(4);
    } > IRAM

    __bss_start = .;
    .bss (NOLOAD) : ALIGN(4) {
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
    } > IRAM
    __bss_end = .;

    /* stack grows down from top of IRAM */
    __stack_top = 0x002000;

    /DISCARD/ : {
        *(.note .note.*)
        *(.comment)
        *(.eh_frame .eh_frame_hdr)
    }
}
