/* Host tests for crash signatures, repeat records and the export format.
 * Writes build/export-*.bin plus the matching lw_print text so that
 * tests/test_decode.py can check the Python decoder against the C printer.
 * Build with -DLW_TEST_HOOK. */
#include "lastwords.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);               \
        }                                                                                          \
    } while (0)

void lw_test_store(void) {}
void lw_test_fault(const struct lw_fault_info *f, size_t len);

static jmp_buf reboot_jb;
void lw_reboot(void)
{
    longjmp(reboot_jb, 1);
}

static uint64_t rng = 0x243f6a8885a308d3ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)rng;
}

static uint32_t clk;
static uint32_t clock_fn(void)
{
    return clk;
}

static void wipe(void)
{
    size_t n = 0;
    void *p = (void *)lw_region(&n);
    memset(p, 0, n);
}

static struct lw_fault_info make_fault(uint32_t pc, uint32_t lr, uint32_t cfsr)
{
    struct lw_fault_info f;
    memset(&f, 0, sizeof f);
    f.r0 = rnd();
    f.r1 = rnd() & 0xff;
    f.r3 = 0x30000000u;
    f.pc = pc;
    f.lr = lr;
    f.xpsr = 0x01000000u;
    f.exc_return = 0xfffffff9u;
    f.sp = 0x20003fd8u - (rnd() & 0x3c);
    f.ipsr = 3;
    f.cfsr = cfsr;
    f.hfsr = 0x40000000u;
    f.bfar = cfsr & 0x8000u ? 0x30000000u : 0;
    f.nstack = LW_FAULT_STACK_WORDS;
    for (uint32_t i = 0; i < f.nstack; i++)
        f.stack[i] = rnd() % 3 == 0 ? 0 : (rnd() % 2 ? 0x20003f00u + (rnd() & 0xfc) : (rnd() & 0x3ffe) | 1);
    return f;
}

static void crash_len(const struct lw_fault_info *f, size_t len)
{
    if (!setjmp(reboot_jb))
        lw_test_fault(f, len);
}

static void crash(const struct lw_fault_info *f)
{
    crash_len(f, sizeof *f);
}

static void panic(const char *msg)
{
    if (!setjmp(reboot_jb))
        lw_panic(msg);
}

static int count_type(const struct lw_entry *e, void *ctx)
{
    uint32_t *c = ctx;
    c[e->type & 7]++;
    if (e->type == LW_T_TEXT && e->len >= 7 && memcmp(e->data, "history", 7) == 0)
        c[7]++;
    return 0;
}

/* Signature: stable over everything but fault class, pc and lr. */
static void test_signature(void)
{
    struct lw_fault_info a = make_fault(0x74, 0x103, 0x8200);
    struct lw_fault_info b = make_fault(0x75, 0x102, 0x8200); /* other regs, stack, Thumb bit */
    struct lw_entry ea, eb;
    memset(&ea, 0, sizeof ea);
    memset(&eb, 0, sizeof eb);
    ea.type = eb.type = LW_T_FAULT;
    ea.len = eb.len = sizeof a;
    memcpy(ea.data, &a, sizeof a);
    memcpy(eb.data, &b, sizeof b);
    CHECK(lw_signature(&ea) != 0);
    CHECK(lw_signature(&ea) == lw_signature(&eb));

    b.pc = 0x80;
    memcpy(eb.data, &b, sizeof b);
    CHECK(lw_signature(&ea) != lw_signature(&eb));
    b.pc = 0x74;
    b.cfsr = 0x02000000u;
    memcpy(eb.data, &b, sizeof b);
    CHECK(lw_signature(&ea) != lw_signature(&eb));

    eb.type = LW_T_TEXT;
    CHECK(lw_signature(&eb) == 0);
}

/* A crash loop: same fault every boot. Returns how many boots the log
 * written before the first crash survives. */
static uint32_t crash_loop(uint32_t *repeats, uint32_t *faults)
{
    wipe();
    lw_init(0);
    for (int i = 0; i < 8; i++)
        lw_logf(LW_INFO, "history %d: sensor ok, 3 samples queued", i);
    struct lw_fault_info f = make_fault(0x74, 0x103, 0x8200);
    for (uint32_t boot = 1; boot < 1000; boot++) {
        f.r0 = rnd(); /* registers differ run to run */
        crash(&f);
        CHECK(lw_init(0) == LW_START_CRASHED);
        lw_log(LW_INFO, "up");
        uint32_t c[8] = {0};
        lw_foreach(count_type, c);
        if (c[7] < 8) {
            *repeats = c[LW_T_REPEAT];
            *faults = c[LW_T_FAULT];
            return boot;
        }
    }
    return 1000;
}

static void test_repeat(void)
{
    uint32_t reps = 0, faults = 0;
    uint32_t boots = crash_loop(&reps, &faults);
#ifdef LW_NO_DEDUP
    printf("crash loop, LW_NO_DEDUP: log before the first crash lost after %u boots\n", boots);
#else
    printf("crash loop, dedup on: log before the first crash lost after %u boots\n", boots);
    CHECK(faults <= 1);
    CHECK(reps >= 1);

    /* Count goes up, and a different crash still gets a full record. */
    wipe();
    lw_init(0);
    struct lw_fault_info f = make_fault(0x74, 0x103, 0x8200);
    crash(&f);
    lw_init(0);
    crash(&f);
    lw_init(0);
    crash(&f);
    lw_init(0);
    panic("assert x");
    lw_init(0);
    panic("assert x");
    lw_init(0);
    struct lw_fault_info g = make_fault(0x90, 0x113, 0x02000000u);
    crash(&g);
    lw_init(0);
    uint32_t c[8] = {0};
    lw_foreach(count_type, c);
    CHECK(c[LW_T_FAULT] == 2);
    CHECK(c[LW_T_PANIC] == 1);
    CHECK(c[LW_T_REPEAT] == 3);
#endif
}

static void sink_file(const void *data, size_t len, void *ctx)
{
    CHECK(len > 0 && len <= 32);
    fwrite(data, 1, len, ctx);
}

static void print_file(const char *line, void *ctx)
{
    fputs(line, ctx);
}

static uint32_t used_bytes(void)
{
    struct lw_stats st;
    lw_get_stats(&st);
    return st.bytes_used;
}

static void dump(const char *name, uint32_t *xbytes)
{
    char path[256];
    snprintf(path, sizeof path, "build/export-%s.bin", name);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f)
        return;
    *xbytes = lw_export(sink_file, f);
    fclose(f);
    snprintf(path, sizeof path, "build/export-%s.txt", name);
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f)
        return;
    lw_print(print_file, f);
    fclose(f);
}

/* Scenarios for the decoder; also the size numbers in the README. */
static void test_export(void)
{
    uint32_t x;

    wipe();
    dump("empty", &x);

    /* The QEMU demo shape: a few lines per boot, then a crash. */
    wipe();
    clk = 0;
    lw_set_clock(clock_fn);
    for (int boot = 0; boot < 4; boot++) {
        lw_init(0x0c000000u >> boot);
        for (clk = 0; clk < 3; clk++)
            lw_logf(LW_INFO, "boot %d, sensor sample %u", boot, clk * 17);
        lw_log(LW_INFO, "applying DMA config");
        if (boot == 2) {
            panic("assert free_blocks > 4");
        } else {
            struct lw_fault_info f = make_fault(0x74 + 0x10 * (uint32_t)boot, 0x103, boot == 1 ? 0x02000000u : 0x8200u);
            crash(&f);
        }
    }
    lw_init(0);
    dump("demo", &x);
    printf("demo-shaped ring: %u bytes of records, export %u bytes\n", used_bytes(), x);

    /* A full ring of random lines, levels, blobs, crashes and clock jumps. */
    for (int round = 0; round < 50; round++) {
        wipe();
        lw_init(rnd());
        for (int i = 0; i < 200; i++) {
            uint32_t r = rnd() % 20;
            clk += rnd() % 4 == 0 ? rnd() : rnd() % 1000;
            if (r == 0) {
                struct lw_fault_info f = make_fault(rnd() & 0xfffe, rnd() | 1, rnd() & 0x030f8200u);
                f.nstack = rnd() % (LW_FAULT_STACK_WORDS + 1);
                crash_len(&f, sizeof f - (LW_FAULT_STACK_WORDS - f.nstack) * 4);
                lw_init(rnd());
            } else if (r == 1) {
                panic(rnd() % 2 ? "assert len < 64" : "oom");
                lw_init(rnd());
            } else if (r == 2) {
                uint8_t blob[40];
                for (size_t k = 0; k < sizeof blob; k++)
                    blob[k] = (uint8_t)rnd();
                lw_write(LW_INFO, LW_T_BLOB, blob, 1 + rnd() % sizeof blob);
            } else {
                char line[300];
                size_t n = rnd() % 120;
                for (size_t k = 0; k < n; k++)
                    line[k] = (char)(rnd() % 3 ? 'a' + rnd() % 26 : rnd() % 256);
                line[n] = 0;
                lw_log((enum lw_level)(rnd() % 5), line[0] ? line : "x");
            }
        }
        char name[32];
        snprintf(name, sizeof name, "random%02d", round);
        dump(name, &x);
    }
    lw_set_clock(NULL);
}

int main(void)
{
    test_signature();
    test_repeat();
    test_export();
    printf("crash/export tests: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
