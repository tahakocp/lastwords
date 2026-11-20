/* Host tests: reset simulation, torn writes, garbage RAM. Build with -DLW_TEST_HOOK. */
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

/* --- hooks the library calls ------------------------------------------- */

static jmp_buf cut;
static long store_budget = -1; /* -1: unlimited */
static long stores;

void lw_test_resync(void);

void lw_test_store(void)
{
    stores++;
    if (store_budget >= 0 && store_budget-- == 0)
        longjmp(cut, 1);
}

static jmp_buf reboot_jb;
void lw_reboot(void)
{
    longjmp(reboot_jb, 1);
}

/* --- helpers ------------------------------------------------------------ */

static uint8_t *mem(void)
{
    size_t n;
    return (uint8_t *)lw_region(&n);
}

static size_t mem_size(void)
{
    size_t n;
    lw_region(&n);
    return n;
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)rng;
}

#define MAX_SEEN 4096
struct seen {
    int n;
    uint8_t type[MAX_SEEN];
    uint16_t seq[MAX_SEEN];
    char text[MAX_SEEN][LW_MAX_PAYLOAD + 1];
};

static int collect(const struct lw_entry *e, void *ctx)
{
    struct seen *s = ctx;
    if (s->n < MAX_SEEN) {
        s->type[s->n] = e->type;
        s->seq[s->n] = e->seq;
        memcpy(s->text[s->n], e->data, e->len);
        s->text[s->n][e->len] = 0;
        s->n++;
    }
    return 0;
}

static void snapshot(struct seen *s)
{
    memset(s, 0, sizeof *s);
    lw_foreach(collect, s);
}

/* Index of the last text record, or -1. */
static int last_text(const struct seen *s)
{
    for (int i = s->n - 1; i >= 0; i--)
        if (s->type[i] == LW_T_TEXT)
            return i;
    return -1;
}

static void message(char *buf, size_t cap, unsigned i)
{
    /* Variable length so records straddle the wrap point at every offset. */
    unsigned len = 5 + (i * 7u) % 60;
    int n = snprintf(buf, cap, "msg %u ", i);
    for (; (unsigned)n < len && (size_t)n < cap - 1; n++)
        buf[n] = (char)('a' + (i + (unsigned)n) % 26);
    buf[n] = 0;
}

/* --- tests ---------------------------------------------------------------- */

static void test_cold_then_warm(void)
{
    memset(mem(), 0, mem_size());
    CHECK(lw_init(0) == LW_START_COLD);
    lw_log(LW_INFO, "hello");
    lw_logf(LW_WARN, "value=%d", 42);

    CHECK(lw_init(4) == LW_START_WARM);
    struct seen s;
    snapshot(&s);
    CHECK(s.n == 4); /* boot, hello, value, boot */
    CHECK(s.type[0] == LW_T_BOOT && s.type[3] == LW_T_BOOT);
    CHECK(strcmp(s.text[1], "hello") == 0);
    CHECK(strcmp(s.text[2], "value=42") == 0);
    for (int i = 1; i < s.n; i++)
        CHECK(s.seq[i] == (uint16_t)(s.seq[i - 1] + 1));

    struct lw_stats st;
    lw_get_stats(&st);
    CHECK(st.boot == 1);
    CHECK(st.records == 4);
}

static void test_garbage_is_cold(void)
{
    for (int i = 0; i < 1000; i++) {
        uint8_t *m = mem();
        for (size_t j = 0; j < mem_size(); j++)
            m[j] = (uint8_t)rnd();
        CHECK(lw_init(0) == LW_START_COLD);
        struct seen s;
        snapshot(&s);
        CHECK(s.n == 1 && s.type[0] == LW_T_BOOT);
    }
}

static void test_wrap_keeps_suffix(void)
{
    char buf[128];
    memset(mem(), 0, mem_size());
    lw_init(0);
    for (unsigned i = 0; i < 5000; i++) {
        message(buf, sizeof buf, i);
        lw_log(LW_INFO, buf);
    }
    CHECK(lw_init(0) == LW_START_WARM);

    struct seen s;
    snapshot(&s);
    CHECK(s.n > LW_RING_SIZE / 80);
    /* All but the trailing boot record are the newest messages, in order. */
    int last = last_text(&s);
    CHECK(last == s.n - 2);
    for (int k = 0; k <= last; k++) {
        message(buf, sizeof buf, 5000u - 1 - (unsigned)(last - k));
        CHECK(strcmp(s.text[k], buf) == 0);
    }
    struct lw_stats st;
    lw_get_stats(&st);
    CHECK(st.bytes_used <= LW_RING_SIZE);
    CHECK(st.bytes_used > LW_RING_SIZE - 80); /* a full ring wastes less than one record */
}

/* Reset in the middle of every store of a log call, at many ring offsets.
 * After recovery the ring must hold the previous history, plus the new
 * record if and only if it was completely written. Never anything else. */
static void test_torn_writes(long *cuts_out)
{
    char buf[128], prev[128];
    static uint8_t saved[65536 + 64];
    volatile long cuts = 0;

    for (unsigned round = 0; round < 300; round++) {
        memset(mem(), 0, mem_size());
        lw_init(0);
        unsigned pre = 20 + round; /* moves the write position around the ring */
        for (unsigned i = 0; i < pre; i++) {
            message(buf, sizeof buf, i);
            lw_log(LW_INFO, buf);
        }
        message(prev, sizeof prev, pre - 1);
        message(buf, sizeof buf, pre);

        memcpy(saved, mem(), mem_size());

        stores = 0;
        lw_log(LW_INFO, buf);
        long total = stores;

        for (volatile long k = 0; k <= total; k++) {
            memcpy(mem(), saved, mem_size());
            lw_test_resync();
            store_budget = k;
            if (setjmp(cut) == 0)
                lw_log(LW_INFO, buf);
            store_budget = -1;
            cuts++;

            CHECK(lw_init(0) != LW_START_COLD);
            struct seen s;
            snapshot(&s);
            int last = last_text(&s);
            CHECK(last >= 0);
            if (last < 0)
                continue;
            int complete = k >= total - 1; /* the record counts once head is stored */
            CHECK(strcmp(s.text[last], complete ? buf : prev) == 0);
            for (int i = 1; i < s.n; i++)
                CHECK(s.seq[i] == (uint16_t)(s.seq[i - 1] + 1));
        }
    }
    *cuts_out = cuts;
}

static void test_panic_marks_crash(void)
{
    char out[4096] = "";
    memset(mem(), 0, mem_size());
    lw_init(0);
    lw_log(LW_INFO, "about to fail");
    if (setjmp(reboot_jb) == 0)
        lw_panic("queue overflow");
    CHECK(lw_init(0x10) == LW_START_CRASHED);
    CHECK(lw_init(0) == LW_START_WARM); /* the next clean reboot is warm again */

    struct seen s;
    snapshot(&s);
    CHECK(s.n == 5);
    CHECK(s.type[2] == LW_T_PANIC);
    CHECK(strcmp(s.text[2] + 4, "queue overflow") == 0);
    (void)out;
}

static void print_line(const char *line, void *ctx)
{
    strncat((char *)ctx, line, 4095 - strlen((char *)ctx));
}

static uint32_t fake_clock(void)
{
    return 1234;
}

static void test_print(void)
{
    char out[4096] = "";
    memset(mem(), 0, mem_size());
    lw_set_clock(fake_clock);
    lw_init(0x0c000000);
    lw_log(LW_ERROR, "sensor timeout");
    uint8_t blob[3] = {0xde, 0xad, 0x01};
    lw_write(LW_INFO, LW_T_BLOB, blob, sizeof blob);
    lw_set_clock(NULL);
    lw_print(print_line, out);
    CHECK(strstr(out, "[    1234] #    0 -- boot 0 reset_cause=0x0c000000\n") != NULL);
    CHECK(strstr(out, "[    1234] #    1 E sensor timeout\n") != NULL);
    CHECK(strstr(out, "blob type=4 len=3: de ad 01\n") != NULL);
}

static void test_truncation_and_clear(void)
{
    char big[600];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    memset(mem(), 0, mem_size());
    lw_init(0);
    lw_log(LW_INFO, big);
    lw_logf(LW_INFO, "%s", big);
    struct seen s;
    snapshot(&s);
    unsigned max = LW_RING_SIZE - 11 < LW_MAX_PAYLOAD ? LW_RING_SIZE - 11 : LW_MAX_PAYLOAD;
    CHECK(s.n == (LW_RING_SIZE >= 1024 ? 3 : 1)); /* a 256-byte ring holds one full-size record */
    CHECK(strlen(s.text[s.n - 1]) == max);
    if (s.n == 3)
        CHECK(strlen(s.text[1]) == max);

    lw_clear();
    snapshot(&s);
    CHECK(s.n == 0);
    CHECK(lw_init(0) == LW_START_WARM);
}

static void test_seq_wraps(void)
{
    memset(mem(), 0, mem_size());
    lw_init(0);
    for (unsigned i = 0; i < 70000; i++)
        lw_log(LW_DEBUG, "tick");
    CHECK(lw_init(0) == LW_START_WARM);
    struct seen s;
    snapshot(&s);
    CHECK(s.n > 2);
    for (int i = 1; i < s.n; i++)
        CHECK(s.seq[i] == (uint16_t)(s.seq[i - 1] + 1));
}

/* Valid header, random ring: how often does noise pass as a record? */
static void test_noise_records(long trials, long *bogus_out)
{
    long bogus = 0;
    for (long t = 0; t < trials; t++) {
        memset(mem(), 0, mem_size());
        lw_init(0);
        uint8_t *m = mem();
        for (size_t j = 6 * sizeof(uint32_t); j < mem_size(); j++)
            m[j] = (uint8_t)rnd();
        struct lw_stats st;
        lw_get_stats(&st);
        bogus += st.records;
    }
    *bogus_out = bogus;
}

int main(int argc, char **argv)
{
    long noise_trials = argc > 1 ? atol(argv[1]) : 20000;
    long cuts = 0, bogus = 0;

    test_cold_then_warm();
    test_garbage_is_cold();
    test_wrap_keeps_suffix();
    test_torn_writes(&cuts);
    test_panic_marks_crash();
    test_print();
    test_truncation_and_clear();
    test_seq_wraps();
    test_noise_records(noise_trials, &bogus);

    printf("ring %d bytes: %d checks, %d failed; %ld torn-write cuts; "
           "%ld bogus records accepted from %ld random rings\n",
           LW_RING_SIZE, checks, failures, cuts, bogus, noise_trials);
    return failures != 0;
}
