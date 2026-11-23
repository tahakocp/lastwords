/* Reset entry for QEMU virt with -bios none: QEMU's reset stub jumps here
 * in machine mode, on every boot including after a test-finisher reset. */
    .section .text.start, "ax", @progbits
    .globl _start
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    la sp, __StackTop
    la t0, __bss_start
    la t1, __bss_end
1:  bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:  call lw_trap_install
    call main
3:  j 3b
