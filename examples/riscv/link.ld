/* QEMU virt, rv32: 128 MB of RAM at 0x80000000, the demo uses the first 64 KB.
 * Everything runs from RAM. On reset QEMU copies the image back in and
 * zero-fills the rest of each loadable segment, so .noinit must not be in
 * one: it goes in no segment at all (:NONE) and QEMU never touches it. */
MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 64K
}

ENTRY(_start)

PHDRS
{
    image PT_LOAD;
}

SECTIONS
{
    .text : {
        KEEP(*(.text.start))
        *(.text*)
        *(.rodata*)
        *(.srodata*)
        . = ALIGN(4);
    } > RAM :image

    .data : {
        __global_pointer$ = . + 0x800;
        *(.sdata*)
        *(.data*)
        . = ALIGN(4);
    } > RAM

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.sbss*)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM

    .noinit (NOLOAD) : {
        . = ALIGN(4);
        *(.noinit*)
    } > RAM :NONE

    end = ALIGN(16);
    __StackTop = ORIGIN(RAM) + LENGTH(RAM);
}
