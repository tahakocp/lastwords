/*
 * Six boots in one QEMU run. Boots 0-3 each die in a different way, boot 4
 * repeats the boot 0 crash, boot 5 prints what the ring remembers, then the
 * compact export as hex, and exits. Output goes through semihosting
 * so the same binary works on every emulated board.
 */
#include <stdint.h>
#include <string.h>

#include "lastwords.h"

#ifndef BAD_ADDR
#define BAD_ADDR 0x30000000u /* unmapped on all four boards */
#endif

static void sh_call(uint32_t op, const void *arg)
{
    register uint32_t r0 __asm("r0") = op;
    register const void *r1 __asm("r1") = arg;
    __asm volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
}

static void say(const char *s)
{
    sh_call(0x04, s); /* SYS_WRITE0 */
}

static void say_line(const char *line, void *ctx)
{
    (void)ctx;
    say(line);
}

/* Hex for the compact export, 32 bytes per line. */
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

static volatile uint32_t ticks;
static uint32_t uptime(void)
{
    return ticks;
}

/* Things that go wrong in real firmware, one per boot. */

struct config {
    volatile uint32_t *dma_ctrl;
    uint32_t channel;
};

__attribute__((noinline)) static void config_apply(const struct config *c)
{
    *c->dma_ctrl = 1u << c->channel;
}

__attribute__((noinline)) static uint32_t average(uint32_t sum, uint32_t count)
{
    return sum / count;
}

__attribute__((noinline)) static uint32_t read_header(const uint8_t *packet)
{
    /* LDM on an unaligned address faults on every Cortex-M. */
    uint32_t a, b;
    __asm volatile("ldm %2!, {%0, %1}" : "=r"(a), "=r"(b), "+r"(packet)::"memory");
    return a ^ b;
}

typedef void (*handler_fn)(void);
static handler_fn handlers[4];

__attribute__((noinline)) static void dispatch(uint32_t id)
{
    handlers[id]();
}

#define ASSERT(c)                                                                                  \
    do {                                                                                           \
        if (!(c))                                                                                  \
            lw_panic("assert " #c);                                                                \
    } while (0)

int main(void)
{
    static const char *names[] = {"cold", "warm", "crashed"};
    enum lw_start start = lw_init(0);
    lw_set_clock(uptime);

    struct lw_stats st;
    lw_get_stats(&st);
    say("boot ");
    char n[2] = {(char)('0' + st.boot % 10), 0};
    say(n);
    say(": lw_init says ");
    say(names[start]);
    say("\n");

    for (ticks = 0; ticks < 3; ticks++)
        lw_logf(LW_INFO, "boot %lu, sensor sample %lu", (unsigned long)st.boot, (unsigned long)(ticks * 17));

    switch (st.boot) {
    case 0:
    case 4: { /* boot 4 hits the boot 0 bug again: recorded as a repeat */
        static const struct config cfg = {(volatile uint32_t *)BAD_ADDR, 3};
        lw_log(LW_INFO, "applying DMA config");
        config_apply(&cfg);
        break;
    }
    case 1: {
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
        *(volatile uint32_t *)0xE000ED14u |= 1u << 4; /* CCR.DIV_0_TRP */
        volatile uint32_t samples = 0;
        lw_log(LW_WARN, "no samples this window");
        lw_logf(LW_INFO, "avg %lu", (unsigned long)average(1000, samples));
#else
        static uint8_t packet[12];
        lw_log(LW_WARN, "parsing packet at odd offset");
        lw_logf(LW_INFO, "hdr %lu", (unsigned long)read_header(packet + 1));
#endif
        break;
    }
    case 2: {
        uint32_t free_blocks = 1;
        lw_logf(LW_ERROR, "pool low: %lu blocks", (unsigned long)free_blocks);
        ASSERT(free_blocks > 4);
        break;
    }
    case 3:
        lw_log(LW_INFO, "dispatching message 2");
        handlers[2] = (handler_fn)0x00001000u; /* even address: Thumb bit missing */
        dispatch(2);
        break;
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
    (void)average;
    (void)read_header;
    return 0;
}
