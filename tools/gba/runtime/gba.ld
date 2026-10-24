/* gba.ld - where everything lives in a Modularity GBA ROM.
 *
 *   ROM    0x08000000  32 MB   code (Thumb), constants, baked scene data
 *   IWRAM  0x03000000  32 KB   1-cycle RAM: ARM hot code, variables, stacks
 *   EWRAM  0x02000000 256 KB   slower RAM for bulk buffers (.ewram)
 *
 * IWRAM's top 256 bytes belong to the BIOS; the stacks grow down from just below them, so
 * data is capped at 28 KB to leave the stacks 3.75 KB.
 */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY {
    rom   (rx)  : ORIGIN = 0x08000000, LENGTH = 32M
    iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 0x7000
    ewram (rwx) : ORIGIN = 0x02000000, LENGTH = 256K
}

__sp_irq = 0x03007FA0;
__sp_usr = 0x03007F00;

SECTIONS {
    .crt0 : { KEEP(*(.crt0)) } > rom

    .text : {
        *(.text .text.*)
        *(.glue_7 .glue_7t .v4_bx)  /* ARM/Thumb interworking veneers */
        . = ALIGN(4);
    } > rom

    .rodata : {
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > rom

    .ARM.exidx : {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > rom

    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
        . = ALIGN(4);
    } > rom

    .iwram : {
        __iwram_start = .;
        *(.iwram .iwram.*)
        *(.data .data.*)
        . = ALIGN(4);
        __iwram_end = .;
    } > iwram AT > rom
    __iwram_lma = LOADADDR(.iwram);

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > iwram

    .ewram : {
        __ewram_start = .;
        *(.ewram .ewram.*)
        . = ALIGN(4);
        __ewram_end = .;
    } > ewram AT > rom
    __ewram_lma = LOADADDR(.ewram);

    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.attributes) }
}
