/* mps2-an385 (Cortex-M3): 4 MB at 0, 4 MB SRAM at 0x20000000; 16 KB used. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
}

ENTRY(Reset_Handler)

SECTIONS
{
    .text : {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .data : {
        __data_start = .;
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT > FLASH
    __data_load = LOADADDR(.data);

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM

    /* Not zeroed, not in the image: survives the reset. */
    .noinit (NOLOAD) : {
        . = ALIGN(4);
        *(.noinit*)
    } > RAM

    __StackTop = ORIGIN(RAM) + LENGTH(RAM);

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}
