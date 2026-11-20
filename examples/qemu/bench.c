/*
 * Two measurements, no output of its own:
 *   -DLW_BENCH_LINK  only links lw_init + lw_log, for the flash cost
 *   -DN=<count>      logs a 32-byte line N times, for the instruction count
 */
#include <stdint.h>

#include "lastwords.h"

static void sh_exit(void)
{
    register uint32_t r0 __asm("r0") = 0x18;
    register uint32_t r1 __asm("r1") = 0x20026;
    __asm volatile("bkpt 0xab" ::"r"(r0), "r"(r1));
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
    sh_exit();
    return 0;
}
