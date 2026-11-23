/* Startup for bench.c: no trap entry, so the base image has none either. */
    .section .text.start, "ax", @progbits
    .globl _start
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    la sp, __StackTop
#ifdef LW_BENCH_LINK
    call lw_trap_install
#endif
    call main
1:  j 1b
