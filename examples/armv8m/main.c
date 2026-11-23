/*
 * lastwords on a TrustZone Cortex-M55 (QEMU mps3-an547), linked as the
 * Secure image. Six boots in one QEMU run:
 *   0  Secure code jumps into Non-secure memory without BLXNS: SecureFault INVTRAN
 *   1  Non-secure code hits an undefined instruction; the Secure HardFault
 *      handler records it from the Non-secure main stack (MSP_NS)
 *   2  the same from a Non-secure thread that switched to its process stack (PSP_NS)
 *   3  Secure store to an unmapped address: BusFault
 *   4  the boot 0 bug again, stored as a repeat
 *   5  prints the ring and the export, exits
 */
#include <stdint.h>
#include <string.h>

#include "lastwords.h"

#define REG(a) (*(volatile uint32_t *)(a))
#define SHCSR REG(0xE000ED24u)
#define SAU_CTRL REG(0xE000EDD0u)
#define SAU_RNR REG(0xE000EDD8u)
#define SAU_RBAR REG(0xE000EDDCu)
#define SAU_RLAR REG(0xE000EDE0u)

#define NS_ALIAS 0x10000000u /* Secure TCM address minus this is its Non-secure alias */

static void sh_call(uint32_t op, const void *arg)
{
    register uint32_t r0 __asm("r0") = op;
    register const void *r1 __asm("r1") = arg;
    __asm volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
}

static void say(const char *s)
{
    sh_call(0x04, s);
}

static void say_line(const char *line, void *ctx)
{
    (void)ctx;
    say(line);
}

static void say_hex(const void *data, size_t len, void *ctx)
{
    static const char d[] = "0123456789abcdef";
    char buf[65];
    const uint8_t *p = data;
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        buf[2 * i] = d[p[i] >> 4];
        buf[2 * i + 1] = d[p[i] & 0xf];
    }
    buf[2 * len] = 0;
    say(buf);
    say("\n");
}

static void sh_exit(int ok)
{
    sh_call(0x18, (const void *)(uintptr_t)(ok ? 0x20026u : 0x20024u));
    for (;;) {
    }
}

static void sau_region(uint32_t n, uint32_t base, uint32_t limit)
{
    SAU_RNR = n;
    SAU_RBAR = base & ~0x1fu;
    SAU_RLAR = (limit & ~0x1fu) | 1u;
}

/* Lower ITCM and the top of DTCM become Non-secure. Both are only used
 * through their Non-secure aliases by the code below. */
static void trustzone_setup(void)
{
    sau_region(0, 0x00000000u, 0x0003ffffu);
    sau_region(1, 0x20008000u, 0x2000ffffu);
    SAU_CTRL = 1u;
    SHCSR |= (1u << 19) | (1u << 18) | (1u << 17) | (1u << 16); /* SecureFault, UsageFault, BusFault, MemManage */
    __asm volatile("dsb\n isb" ::: "memory");
}

/* Runs in Non-secure state. Naked: no stack use, nothing Secure touched. */
__attribute__((naked, noinline)) void ns_driver_poll(void)
{
    __asm volatile("udf #7\n");
}

/* Non-secure too: moves itself onto a process stack, like an RTOS task. */
__attribute__((naked, noinline)) void ns_task_run(void)
{
    __asm volatile("movw r0, #0xc000\n"
                   "movt r0, #0x2000\n"
                   "msr psp, r0\n"
                   "movs r0, #2\n"
                   "msr control, r0\n"
                   "isb\n"
                   "udf #8\n");
}

__attribute__((noinline)) static void plugin_call(uint32_t entry)
{
    void (*fn)(void) = (void (*)(void))entry;
    fn(); /* a plain BLX into Non-secure memory: the core refuses it */
}

__attribute__((noinline)) static void ns_start(uint32_t entry)
{
    uint32_t ns_sp = 0x20010000u;
    __asm volatile("msr msp_ns, %0\n"
                   "blxns %1\n" ::"r"(ns_sp),
                   "r"(entry & ~1u)
                   : "memory");
}

struct config {
    volatile uint32_t *dma_ctrl;
    uint32_t channel;
};

__attribute__((noinline)) static void config_apply(const struct config *c)
{
    *c->dma_ctrl = 1u << c->channel;
}

int main(void)
{
    static const char *names[] = {"cold", "warm", "crashed"};
    enum lw_start start = lw_init(0);
    trustzone_setup();

    struct lw_stats st;
    lw_get_stats(&st);
    say("boot ");
    char n[2] = {(char)('0' + st.boot % 10), 0};
    say(n);
    say(": lw_init says ");
    say(names[start]);
    say("\n");
    lw_logf(LW_INFO, "boot %lu, secure side up", (unsigned long)st.boot);

    uint32_t plugin = (uint32_t)(uintptr_t)ns_driver_poll - NS_ALIAS;
    switch (st.boot) {
    case 0:
    case 4:
        lw_log(LW_INFO, "calling plugin");
        plugin_call(plugin);
        break;
    case 1:
        lw_log(LW_INFO, "starting non-secure side");
        ns_start(plugin);
        break;
    case 2:
        lw_log(LW_INFO, "starting non-secure task");
        ns_start((uint32_t)(uintptr_t)ns_task_run - NS_ALIAS);
        break;
    case 3: {
        static const struct config cfg = {(volatile uint32_t *)0x0f000000u, 3};
        lw_log(LW_INFO, "applying DMA config");
        config_apply(&cfg);
        break;
    }
    default:
        say("---- recovered log ----\n");
        lw_print(say_line, 0);
        lw_get_stats(&st);
        say(st.records > 0 ? "---- end ----\n" : "empty\n");
        say("---- export ----\n");
        lw_export(say_hex, 0);
        say("---- end export ----\n");
        sh_exit(st.boot == 5);
    }

    say("expected a fault, got none\n");
    sh_exit(0);
    return 0;
}
