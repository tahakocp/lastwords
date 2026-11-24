/* The scenario of tests/host.rs, written against the C API. The Rust test
 * builds this, runs it, and wants the same export and lw_print text. */
#include <stdio.h>
#include <string.h>

#include "lastwords.h"

void lw_test_store(void) {}
void lw_test_fault(const struct lw_fault_info *f, size_t len);

static uint32_t t;
static uint32_t clock_fn(void) { return t += 7; }

static void fault(void)
{
    struct lw_fault_info f;
    memset(&f, 0, sizeof f);
    f.lr = 0x08000a3du;
    f.pc = 0x08000c42u;
    f.xpsr = 0x01000003u;
    f.exc_return = 0xfffffff9u;
    f.sp = 0x20003f80u;
    f.ipsr = 3;
    f.cfsr = 1u << 25; /* DIVBYZERO */
    f.hfsr = 1u << 30; /* FORCED */
    f.nstack = 2;
    f.stack[0] = 0xdeadbeefu;
    f.stack[1] = 0x08000a3du;
    lw_test_fault(&f, offsetof(struct lw_fault_info, stack) + 2 * 4);
}

static void out_txt(const char *line, void *ctx) { fputs(line, ctx); }
static void out_bin(const void *d, size_t n, void *ctx) { fwrite(d, 1, n, ctx); }

int main(int argc, char **argv)
{
    char x[301];
    if (argc < 3)
        return 2;
    if (lw_init(0x55) != LW_START_COLD)
        return 1;
    lw_set_clock(clock_fn);
    lw_log(LW_INFO, "sensor ok");
    lw_logf(LW_WARN, "temp %d C, rail %.2f V", 41, 3.3);
    static const uint8_t blob[8] = {1, 2, 3, 4, 0xfe, 0xff, 0, 9};
    lw_write(LW_DEBUG, LW_T_BLOB, blob, sizeof blob);
    memset(x, 'x', 300);
    x[300] = 0;
    lw_logf(LW_ERROR, "%s", x);
    fault();
    if (lw_init(0) != LW_START_CRASHED)
        return 1;
    fault();
    for (int i = 0; i < 40; i++)
        lw_logf(LW_INFO, "tick %d of %d", i, 40);
    FILE *b = fopen(argv[1], "wb"), *p = fopen(argv[2], "wb");
    lw_export(out_bin, b);
    lw_print(out_txt, p);
    fclose(b);
    fclose(p);
    return 0;
}
