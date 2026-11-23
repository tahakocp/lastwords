/* MPS3 AN547 (SSE-300, Cortex-M55), Secure aliases: ITCM at 0x10000000,
 * DTCM at 0x30000000. The Non-secure side of the demo runs from the same
 * TCMs through their Non-secure aliases at 0x00000000 and 0x20000000. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 256K
    RAM   (rwx) : ORIGIN = 0x30000000, LENGTH = 16K
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

    .noinit (NOLOAD) : {
        . = ALIGN(4);
        *(.noinit*)
    } > RAM

    end = ALIGN(8);

    __StackTop = ORIGIN(RAM) + LENGTH(RAM);
}
