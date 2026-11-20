/*
 * lastwords: a crash flight recorder for microcontrollers.
 * SPDX-License-Identifier: MIT
 *
 * Ring layout. Each record is written as
 *
 *   [type<<4|level][len][seq lo][seq hi][time x4][payload x len][crc lo][crc hi][len]
 *
 * and the ring only ever grows at head. The trailing copy of len lets
 * lw_init() walk backwards from head without any index: it stops at the
 * first record whose CRC, length or sequence number does not line up, which
 * is exactly where older data was overwritten or where a write was cut off
 * by a reset.
 */
#include "lastwords.h"

#include <string.h>

#ifndef LW_NO_PRINTF
#include <stdarg.h>
#include <stdio.h>
#endif

#if (LW_RING_SIZE & (LW_RING_SIZE - 1)) != 0 || LW_RING_SIZE < 256 || LW_RING_SIZE > 65536
#error "LW_RING_SIZE must be a power of two between 256 and 65536"
#endif

#ifndef LW_SECTION
#define LW_SECTION ".noinit"
#endif

#if defined(__arm__) && defined(__ARM_ARCH_PROFILE) && __ARM_ARCH_PROFILE == 'M'
#define LW_CORTEX_M 1
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_8M_MAIN__) || \
    defined(__ARM_ARCH_8_1M_MAIN__)
#define LW_HAS_CFSR 1
#endif
#endif

#define LW_MAGIC 0x3157574cu /* "LWW1" */
#define HDR_LEN 8u
#define OVERHEAD 11u
#define MASK ((uint32_t)LW_RING_SIZE - 1u)
#define MAX_LEN ((LW_RING_SIZE - OVERHEAD) < LW_MAX_PAYLOAD ? (LW_RING_SIZE - OVERHEAD) : LW_MAX_PAYLOAD)

struct lw_region_s {
    uint32_t magic;
    uint32_t size;
    uint32_t head;
    uint32_t head_inv;
    uint32_t boot;
    uint32_t boot_inv;
    uint8_t ring[LW_RING_SIZE];
};

static struct lw_region_s lw_mem __attribute__((section(LW_SECTION), aligned(4)));

static uint32_t (*lw_clock)(void);
static uint16_t lw_seq;
static volatile uint8_t lw_in_fault;

/* Host tests define this to cut a write short at any store. */
#ifdef LW_TEST_HOOK
void lw_test_store(void);
#define STORE_HOOK() lw_test_store()
#else
#define STORE_HOOK() ((void)0)
#endif

#if defined(LW_LOCK) && defined(LW_UNLOCK)
/* user supplied */
#elif LW_CORTEX_M
static inline uint32_t lw_irq_save(void)
{
    uint32_t primask;
    __asm volatile("mrs %0, primask\n cpsid i" : "=r"(primask)::"memory");
    return primask;
}
static inline void lw_irq_restore(uint32_t primask)
{
    __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
}
#define LW_LOCK() uint32_t lw_key_ = lw_irq_save()
#define LW_UNLOCK() lw_irq_restore(lw_key_)
#else
#define LW_LOCK() ((void)0)
#define LW_UNLOCK() ((void)0)
#endif

/* CRC-16/CCITT-FALSE, nibble table: 32 bytes of flash instead of 512. */
static const uint16_t crc_nib[16] = {0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
                                     0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef};

static uint16_t crc_byte(uint16_t crc, uint8_t b)
{
    crc = (uint16_t)((crc << 4) ^ crc_nib[((crc >> 12) ^ (b >> 4)) & 0xf]);
    crc = (uint16_t)((crc << 4) ^ crc_nib[((crc >> 12) ^ (b & 0xf)) & 0xf]);
    return crc;
}

static uint8_t rd(uint32_t pos)
{
    return lw_mem.ring[pos & MASK];
}

static void put(uint32_t pos, uint8_t b)
{
    STORE_HOOK();
    lw_mem.ring[pos & MASK] = b;
}

static void put_word(volatile uint32_t *dst, uint32_t v)
{
    STORE_HOOK();
    *dst = v;
}

static uint32_t now(void)
{
    return lw_clock ? lw_clock() : 0;
}

/* Append one record built from two pieces (header struct + text, usually).
 * Caller holds the lock or is the fault handler. */
static void emit(uint8_t type, uint8_t level, const void *a, size_t alen, const void *b, size_t blen)
{
    if (alen > MAX_LEN)
        alen = MAX_LEN;
    if (alen + blen > MAX_LEN)
        blen = MAX_LEN - alen;

    uint8_t len = (uint8_t)(alen + blen);
    uint32_t t = now();
    uint8_t hdr[HDR_LEN] = {(uint8_t)(type << 4 | (level & 0xf)), len, (uint8_t)lw_seq, (uint8_t)(lw_seq >> 8),
                            (uint8_t)t, (uint8_t)(t >> 8), (uint8_t)(t >> 16), (uint8_t)(t >> 24)};

    uint32_t h = lw_mem.head;
    uint16_t crc = 0xffff;
    for (uint32_t i = 0; i < HDR_LEN; i++) {
        crc = crc_byte(crc, hdr[i]);
        put(h++, hdr[i]);
    }
    for (size_t i = 0; i < alen; i++) {
        uint8_t c = ((const uint8_t *)a)[i];
        crc = crc_byte(crc, c);
        put(h++, c);
    }
    for (size_t i = 0; i < blen; i++) {
        uint8_t c = ((const uint8_t *)b)[i];
        crc = crc_byte(crc, c);
        put(h++, c);
    }
    put(h++, (uint8_t)crc);
    put(h++, (uint8_t)(crc >> 8));
    put(h++, len);

    /* The record is complete before head moves, so a reset at any point
     * leaves either the old head or the new one pointing at whole records. */
    put_word(&lw_mem.head, h);
    put_word(&lw_mem.head_inv, ~h);
    lw_seq++;
}

/* Check the record that ends at pos. On success return its total size and
 * its sequence number through *seq; return 0 otherwise. */
static uint32_t check_record(uint32_t pos, uint16_t *seq)
{
    uint32_t len = rd(pos - 1);
    uint32_t total = len + OVERHEAD;
    uint32_t start = pos - total;

    if (len > MAX_LEN || rd(start + 1) != len)
        return 0;

    uint16_t crc = 0xffff;
    for (uint32_t i = 0; i < HDR_LEN + len; i++)
        crc = crc_byte(crc, rd(start + i));
    uint32_t c = start + HDR_LEN + len;
    if (rd(c) != (uint8_t)crc || rd(c + 1) != (uint8_t)(crc >> 8))
        return 0;

    *seq = (uint16_t)(rd(start + 2) | rd(start + 3) << 8);
    return total;
}

struct walk {
    uint32_t oldest;  /* position of the oldest valid record */
    uint32_t count;
    uint32_t used;
    uint16_t last_seq;
    uint8_t last_type;
};

static void walk_back(uint32_t head, struct walk *w)
{
    uint32_t pos = head;
    uint16_t expect = 0;

    memset(w, 0, sizeof *w);
    w->oldest = head;
    while (w->used + OVERHEAD <= LW_RING_SIZE) {
        uint16_t seq;
        uint32_t total = check_record(pos, &seq);
        if (total == 0 || w->used + total > LW_RING_SIZE)
            break;
        if (w->count > 0 && seq != expect)
            break;
        if (w->count == 0) {
            w->last_seq = seq;
            w->last_type = rd(pos - total) >> 4;
        }
        expect = (uint16_t)(seq - 1);
        pos -= total;
        w->used += total;
        w->count++;
    }
    w->oldest = pos;
}

static void cold_start(void)
{
    memset(&lw_mem, 0, sizeof lw_mem);
    lw_mem.size = LW_RING_SIZE;
    lw_mem.head_inv = ~0u;
    lw_mem.boot_inv = ~0u;
    lw_mem.magic = LW_MAGIC;
}

enum lw_start lw_init(uint32_t reset_cause)
{
    enum lw_start start = LW_START_COLD;
    struct walk w;

    if (lw_mem.magic == LW_MAGIC && lw_mem.size == LW_RING_SIZE) {
        uint32_t head = lw_mem.head;
        if (lw_mem.head_inv != ~head) {
            /* Reset hit between the two head stores. The new head is only
             * written after its record, so prefer it if it checks out. */
            walk_back(head, &w);
            if (w.count == 0)
                head = ~lw_mem.head_inv;
        }
        walk_back(head, &w);
        lw_mem.head = head;
        lw_mem.head_inv = ~head;
        lw_seq = (uint16_t)(w.count ? w.last_seq + 1 : 0);
        start = (w.count && (w.last_type == LW_T_FAULT || w.last_type == LW_T_PANIC)) ? LW_START_CRASHED
                                                                                      : LW_START_WARM;
        uint32_t boot = lw_mem.boot;
        if (lw_mem.boot_inv != ~boot && ~lw_mem.boot_inv > boot)
            boot = ~lw_mem.boot_inv;
        lw_mem.boot = boot + 1;
        lw_mem.boot_inv = ~(boot + 1);
    } else {
        cold_start();
        lw_seq = 0;
    }
    lw_in_fault = 0;

    struct lw_boot_info bi = {lw_mem.boot, reset_cause};
    emit(LW_T_BOOT, LW_INFO, &bi, sizeof bi, NULL, 0);
    return start;
}

#ifdef LW_TEST_HOOK
void lw_test_resync(void)
{
    struct walk w;
    walk_back(lw_mem.head, &w);
    lw_seq = (uint16_t)(w.count ? w.last_seq + 1 : 0);
}
#endif

void lw_set_clock(uint32_t (*fn)(void))
{
    lw_clock = fn;
}

void lw_write(enum lw_level level, enum lw_type type, const void *data, size_t len)
{
    LW_LOCK();
    emit((uint8_t)type, (uint8_t)level, data, len, NULL, 0);
    LW_UNLOCK();
}

void lw_log(enum lw_level level, const char *text)
{
    lw_write(level, LW_T_TEXT, text, strlen(text));
}

#ifndef LW_NO_PRINTF
void lw_logf(enum lw_level level, const char *fmt, ...)
{
    char buf[LW_MAX_PAYLOAD + 1];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n > LW_MAX_PAYLOAD)
        n = LW_MAX_PAYLOAD;
    lw_write(level, LW_T_TEXT, buf, (size_t)n);
}
#endif

void lw_panic(const char *msg)
{
    struct lw_panic_info pi;
    pi.pc = (uint32_t)(uintptr_t)__builtin_return_address(0);
    LW_LOCK();
    emit(LW_T_PANIC, LW_FATAL, &pi, sizeof pi, msg, msg ? strlen(msg) : 0);
    LW_UNLOCK();
    lw_reboot();
}

static void read_entry(uint32_t pos, struct lw_entry *e)
{
    e->type = rd(pos) >> 4;
    e->level = rd(pos) & 0xf;
    e->len = rd(pos + 1);
    e->seq = (uint16_t)(rd(pos + 2) | rd(pos + 3) << 8);
    e->time = (uint32_t)rd(pos + 4) | (uint32_t)rd(pos + 5) << 8 | (uint32_t)rd(pos + 6) << 16 |
              (uint32_t)rd(pos + 7) << 24;
    for (uint32_t i = 0; i < e->len; i++)
        e->data[i] = rd(pos + HDR_LEN + i);
}

uint32_t lw_foreach(lw_visit_fn fn, void *ctx)
{
    struct walk w;
    struct lw_entry e;
    uint32_t n = 0;

    walk_back(lw_mem.head, &w);
    uint32_t pos = w.oldest;
    while (n < w.count) {
        read_entry(pos, &e);
        n++;
        if (fn(&e, ctx))
            break;
        pos += e.len + OVERHEAD;
    }
    return n;
}

void lw_get_stats(struct lw_stats *st)
{
    struct walk w;
    walk_back(lw_mem.head, &w);
    st->boot = lw_mem.boot;
    st->records = w.count;
    st->bytes_used = w.used;
}

void lw_clear(void)
{
    LW_LOCK();
    uint32_t boot = lw_mem.boot;
    cold_start();
    lw_mem.boot = boot;
    lw_mem.boot_inv = ~boot;
    lw_seq = 0;
    LW_UNLOCK();
}

const void *lw_region(size_t *size)
{
    if (size)
        *size = sizeof lw_mem;
    return &lw_mem;
}

/* lw_print formats by hand so it works with LW_NO_PRINTF and from a
 * context where newlib's printf would be a bad idea. */
struct line {
    char buf[96];
    uint32_t n;
    lw_out_fn out;
    void *ctx;
};

static void l_flush(struct line *l)
{
    l->buf[l->n++] = '\n';
    l->buf[l->n] = 0;
    l->out(l->buf, l->ctx);
    l->n = 0;
}

static void l_ch(struct line *l, char c)
{
    if (l->n >= sizeof l->buf - 2) {
        l_flush(l);
        l_ch(l, ' ');
        l_ch(l, ' ');
    }
    l->buf[l->n++] = c;
}

static void l_str(struct line *l, const char *s)
{
    while (*s)
        l_ch(l, *s++);
}

static void l_hex(struct line *l, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    l_str(l, "0x");
    for (int i = 28; i >= 0; i -= 4)
        l_ch(l, digits[(v >> i) & 0xf]);
}

static void l_dec(struct line *l, uint32_t v, int width)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (width-- > n)
        l_ch(l, ' ');
    while (n)
        l_ch(l, tmp[--n]);
}

static void l_kv(struct line *l, const char *k, uint32_t v)
{
    l_ch(l, ' ');
    l_str(l, k);
    l_ch(l, '=');
    l_hex(l, v);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#ifndef LW_NO_FAULT_NAMES
static const struct {
    uint8_t bit;
    const char *name;
} cfsr_names[] = {
    {0, "IACCVIOL"},    {1, "DACCVIOL"},  {3, "MUNSTKERR"},  {4, "MSTKERR"},      {5, "MLSPERR"},
    {8, "IBUSERR"},     {9, "PRECISERR"}, {10, "IMPRECISERR"}, {11, "UNSTKERR"},  {12, "STKERR"},
    {13, "LSPERR"},     {16, "UNDEFINSTR"}, {17, "INVSTATE"}, {18, "INVPC"},      {19, "NOCP"},
    {20, "STKOF"},      {24, "UNALIGNED"}, {25, "DIVBYZERO"},
};
#endif

static const char *exc_name(uint32_t ipsr)
{
    switch (ipsr) {
    case 3: return "HardFault";
    case 4: return "MemManage";
    case 5: return "BusFault";
    case 6: return "UsageFault";
    case 7: return "SecureFault";
    default: return "exception";
    }
}

static void print_fault(struct line *l, const struct lw_entry *e)
{
    struct lw_fault_info f;
    memset(&f, 0, sizeof f);
    memcpy(&f, e->data, e->len < sizeof f ? e->len : sizeof f);
    if (f.nstack > LW_FAULT_STACK_WORDS)
        f.nstack = LW_FAULT_STACK_WORDS;

    l_str(l, "!! ");
    l_str(l, exc_name(f.ipsr));
#ifndef LW_NO_FAULT_NAMES
    for (size_t i = 0; i < sizeof cfsr_names / sizeof cfsr_names[0]; i++) {
        if (f.cfsr & (1u << cfsr_names[i].bit)) {
            l_ch(l, ' ');
            l_str(l, cfsr_names[i].name);
        }
    }
    if (f.hfsr & (1u << 30))
        l_str(l, " FORCED");
    if (f.hfsr & (1u << 1))
        l_str(l, " VECTTBL");
#endif
    l_flush(l);
    l_str(l, "  ");
    l_kv(l, "pc", f.pc);
    l_kv(l, "lr", f.lr);
    l_kv(l, "sp", f.sp);
    l_kv(l, "xpsr", f.xpsr);
    l_flush(l);
    l_str(l, "  ");
    l_kv(l, "r0", f.r0);
    l_kv(l, "r1", f.r1);
    l_kv(l, "r2", f.r2);
    l_kv(l, "r3", f.r3);
    l_kv(l, "r12", f.r12);
    l_flush(l);
    if (f.cfsr | f.hfsr) {
        l_str(l, "  ");
        l_kv(l, "cfsr", f.cfsr);
        l_kv(l, "hfsr", f.hfsr);
        if (f.cfsr & (1u << 7))
            l_kv(l, "mmfar", f.mmfar);
        if (f.cfsr & (1u << 15))
            l_kv(l, "bfar", f.bfar);
        l_flush(l);
    }
    for (uint32_t i = 0; i < f.nstack; i++) {
        if (i % 4 == 0)
            l_str(l, i == 0 ? "   stack:" : "         ");
        l_ch(l, ' ');
        l_hex(l, f.stack[i]);
        if (i % 4 == 3 || i + 1 == f.nstack)
            l_flush(l);
    }
}

static int print_one(const struct lw_entry *e, void *ctx)
{
    static const char lv[] = "DIWEF";
    struct line *l = ctx;

    l_ch(l, '[');
    l_dec(l, e->time, 8);
    l_str(l, "] #");
    l_dec(l, e->seq, 5);
    l_ch(l, ' ');

    switch (e->type) {
    case LW_T_TEXT:
        l_ch(l, e->level < 5 ? lv[e->level] : '?');
        l_ch(l, ' ');
        for (uint32_t i = 0; i < e->len; i++)
            l_ch(l, (e->data[i] >= 0x20 && e->data[i] < 0x7f) || e->data[i] >= 0x80 ? (char)e->data[i] : '.');
        l_flush(l);
        break;
    case LW_T_BOOT:
        l_str(l, "-- boot ");
        l_dec(l, e->len >= 4 ? le32(e->data) : 0, 0);
        if (e->len >= 8)
            l_kv(l, "reset_cause", le32(e->data + 4));
        l_flush(l);
        break;
    case LW_T_FAULT:
        print_fault(l, e);
        break;
    case LW_T_PANIC:
        l_str(l, "!! panic");
        if (e->len >= 4)
            l_kv(l, "pc", le32(e->data));
        l_str(l, ": ");
        for (uint32_t i = 4; i < e->len; i++)
            l_ch(l, (char)e->data[i]);
        l_flush(l);
        break;
    default:
        l_str(l, "blob type=");
        l_dec(l, e->type, 0);
        l_str(l, " len=");
        l_dec(l, e->len, 0);
        l_ch(l, ':');
        for (uint32_t i = 0; i < e->len && i < 16; i++) {
            static const char d[] = "0123456789abcdef";
            l_ch(l, ' ');
            l_ch(l, d[e->data[i] >> 4]);
            l_ch(l, d[e->data[i] & 0xf]);
        }
        if (e->len > 16)
            l_str(l, " ...");
        l_flush(l);
        break;
    }
    return 0;
}

void lw_print(lw_out_fn out, void *ctx)
{
    struct line l;
    l.n = 0;
    l.out = out;
    l.ctx = ctx;
    lw_foreach(print_one, &l);
}

#if defined(__GNUC__)
__attribute__((weak))
#endif
void lw_reboot(void)
{
#if LW_CORTEX_M
    __asm volatile("dsb 0xf" ::: "memory");
    *(volatile uint32_t *)0xE000ED0Cu = 0x05FA0004u; /* AIRCR: VECTKEY | SYSRESETREQ */
    __asm volatile("dsb 0xf" ::: "memory");
#endif
    for (;;) {
    }
}

#if LW_CORTEX_M

/* Linker symbols that mark the top of the main stack in the common GCC
 * startup files. Weak, so whichever exists bounds the stack copy and the
 * handler never reads past the end of RAM. */
extern uint32_t __StackTop __attribute__((weak));
extern uint32_t _estack __attribute__((weak));

static uint32_t stack_limit(void)
{
#ifdef LW_STACK_TOP
    return (uint32_t)(LW_STACK_TOP);
#else
    if (&__StackTop)
        return (uint32_t)(uintptr_t)&__StackTop;
    if (&_estack)
        return (uint32_t)(uintptr_t)&_estack;
    return 0;
#endif
}

void lw_fault_capture(uint32_t exc_return, uint32_t *frame)
{
    struct lw_fault_info f;
    uint32_t ipsr;

    if (lw_in_fault)
        lw_reboot(); /* faulted while recording a fault */
    lw_in_fault = 1;

    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    memset(&f, 0, sizeof f);
    f.exc_return = exc_return;
    f.ipsr = ipsr & 0x1ff;

    if (((uintptr_t)frame & 3u) == 0) {
        f.r0 = frame[0];
        f.r1 = frame[1];
        f.r2 = frame[2];
        f.r3 = frame[3];
        f.r12 = frame[4];
        f.lr = frame[5];
        f.pc = frame[6];
        f.xpsr = frame[7];

        uint32_t words = 8;
        if ((exc_return & 0x10u) == 0)
            words += 18; /* extended frame: s0-s15, fpscr, reserved */
        if (f.xpsr & (1u << 9))
            words += 1; /* the core padded the frame to 8-byte alignment */
        uint32_t *sp = frame + words;
        f.sp = (uint32_t)(uintptr_t)sp;

        uint32_t limit = stack_limit();
        uint32_t n = 0;
        while (n < LW_FAULT_STACK_WORDS && (limit == 0 || (uintptr_t)(sp + n) < limit)) {
            f.stack[n] = sp[n];
            n++;
        }
        f.nstack = n;
    }

#if LW_HAS_CFSR
    f.cfsr = *(volatile uint32_t *)0xE000ED28u;
    f.hfsr = *(volatile uint32_t *)0xE000ED2Cu;
    f.mmfar = *(volatile uint32_t *)0xE000ED34u;
    f.bfar = *(volatile uint32_t *)0xE000ED38u;
#endif

    size_t len = sizeof f - (LW_FAULT_STACK_WORDS - f.nstack) * sizeof(uint32_t);
    emit(LW_T_FAULT, LW_FATAL, &f, len, NULL, 0);
    lw_reboot();
}

__attribute__((naked)) void lw_fault_entry(void)
{
    /* Thumb-1 only, so the same entry works on ARMv6-M. */
    __asm volatile("mov r0, lr\n"
                   "movs r2, #4\n"
                   "tst r0, r2\n"
                   "bne 1f\n"
                   "mrs r1, msp\n"
                   "b 2f\n"
                   "1:\n"
                   "mrs r1, psp\n"
                   "2:\n"
                   "ldr r2, =lw_fault_capture\n"
                   "bx r2\n"
                   ".ltorg\n");
}

#ifndef LW_NO_FAULT_HANDLERS
void HardFault_Handler(void) __attribute__((alias("lw_fault_entry")));
#if LW_HAS_CFSR
void MemManage_Handler(void) __attribute__((alias("lw_fault_entry")));
void BusFault_Handler(void) __attribute__((alias("lw_fault_entry")));
void UsageFault_Handler(void) __attribute__((alias("lw_fault_entry")));
#endif
#if defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8_1M_MAIN__)
void SecureFault_Handler(void) __attribute__((alias("lw_fault_entry")));
#endif
#endif

#endif /* LW_CORTEX_M */
