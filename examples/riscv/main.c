/*
 * The Cortex-M demo's six boots, on a RISC-V core: QEMU virt, rv32imac,
 * machine mode. Boots 0-3 each die in a different way, boot 4 repeats the
 * boot 0 crash, boot 5 prints what the ring remembers and the compact
 * export. Output goes through semihosting; resets and the final exit go
 * through the SiFive test finisher at 0x100000.
 */
#include <stdint.h>

#include "lastwords.h"

#define FINISHER ((volatile uint32_t *)0x100000u)
#define BAD_ADDR 0x00200000u /* nothing mapped here on virt */

/* Semihosting: a0 = operation, a1 = argument. The ebreak has to sit
 * between these two exact uncompressed instructions. */
__attribute__((noinline)) static void sh_call(uint32_t op, const void *arg)
{
    register uint32_t a0 __asm("a0") = op;
    register const void *a1 __asm("a1") = arg;
    __asm volatile(".option push\n"
                   ".option norvc\n"
                   ".balign 16\n"
                   "slli x0, x0, 0x1f\n"
                   "ebreak\n"
                   "srai x0, x0, 7\n"
                   ".option pop\n"
                   : "+r"(a0)
                   : "r"(a1)
                   : "memory");
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

static void say_hex(const void *data, size_t len, void *ctx)
{
    static const char d[] = "0123456789abcdef";
    char buf[66];
    const uint8_t *p = data;
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        buf[2 * i] = d[p[i] >> 4];
        buf[2 * i + 1] = d[p[i] & 0xf];
    }
    buf[2 * len] = '\n';
    buf[2 * len + 1] = 0;
    say(buf);
}

static __attribute__((noreturn)) void finish(int ok)
{
    *FINISHER = ok ? 0x5555u : (1u << 16) | 0x3333u;
    for (;;) {
    }
}

/* lastwords calls this after recording a trap or panic. */
void lw_reboot(void)
{
    *FINISHER = 0x7777u;
    for (;;) {
    }
}

static volatile uint32_t ticks;
static uint32_t uptime(void)
{
    return ticks;
}

struct config {
    volatile uint32_t *dma_ctrl;
    uint32_t channel;
};

__attribute__((noinline)) static void config_apply(const struct config *c)
{
    *c->dma_ctrl = 1u << c->channel;
}

/* Counters kept inside a received frame, one byte in. A plain add would be
 * fine there; the atomic add needs natural alignment. */
static uint8_t frame[16] __attribute__((aligned(4)));

__attribute__((noinline)) static void count_rx(volatile uint32_t *counter)
{
    __atomic_fetch_add(counter, 1, __ATOMIC_RELAXED);
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
    char n[2] = {(char)('0' + st.boot % 10), 0};
    say("boot ");
    say(n);
    say(": lw_init says ");
    say(names[start]);
    say("\n");

    for (ticks = 0; ticks < 3; ticks++)
        lw_logf(LW_INFO, "boot %lu, sensor sample %lu", (unsigned long)st.boot, (unsigned long)(ticks * 17));

    switch (st.boot) {
    case 0:
    case 4: {
        static const struct config cfg = {(volatile uint32_t *)BAD_ADDR, 3};
        lw_log(LW_INFO, "applying DMA config");
        config_apply(&cfg);
        break;
    }
    case 1:
        lw_log(LW_INFO, "packet in, counting");
        count_rx((volatile uint32_t *)(void *)(frame + 1));
        break;
    case 2: {
        uint32_t free_blocks = 1;
        lw_logf(LW_ERROR, "pool low: %lu blocks", (unsigned long)free_blocks);
        ASSERT(free_blocks > 4);
        break;
    }
    case 3:
        lw_log(LW_INFO, "dispatching message 2");
        handlers[2] = (handler_fn)BAD_ADDR;
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
        finish(st.boot == 5);
    }

    say("expected a trap, got none\n");
    finish(0);
}
