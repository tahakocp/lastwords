/* Fits every machine the demo runs on: MPS2 AN385/AN386/AN500 and the
 * nRF51 micro:bit (256 KB flash at 0, 16 KB RAM at 0x20000000). */
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

    /* Not zeroed by startup and not part of the image, so it keeps its
     * contents across a reset. This is the only part lastwords needs. */
    .noinit (NOLOAD) : {
        . = ALIGN(4);
        *(.noinit*)
    } > RAM

    /* newlib's sbrk starts the heap here, after .noinit, never on top of it. */
    end = ALIGN(8);

    __StackTop = ORIGIN(RAM) + LENGTH(RAM);
}
