/*
 * Same two measurements as examples/qemu/bench.c, on rv32imac:
 *   -DLW_BENCH_LINK  links lw_init + lw_log + the trap entry, for the flash cost
 *   -DN=<count>      logs a 32-byte line N times, for the instruction count
 */
#include <stdint.h>

#include "lastwords.h"

void lw_reboot(void)
{
    for (;;) {
    }
}

int main(void)
{
#if defined(LW_BENCH_LINK) || defined(N)
    lw_init(0);
#endif
#ifdef N
    for (int i = 0; i < N; i++)
        lw_log(LW_INFO, "battery 3712 mV, temp 41 C, ok!");
#elif defined(LW_BENCH_LINK)
    lw_log(LW_INFO, "x");
#endif
    *(volatile uint32_t *)0x100000u = 0x5555u; /* test finisher: exit */
    for (;;) {
    }
}
