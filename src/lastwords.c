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
#if defined(__ARM_ARCH_8M_BASE__) || defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8_1M_MAIN__)
#define LW_V8M 1 /* EXC_RETURN carries ES, S, DCRS; TrustZone may be present */
#endif
#endif

#if defined(__riscv) && __riscv_xlen == 32
#define LW_RISCV 1
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
#elif LW_RISCV
static inline uint32_t lw_irq_save(void)
{
    uint32_t mstatus;
    __asm volatile("csrrci %0, mstatus, 8" : "=r"(mstatus)::"memory"); /* clear MIE */
    return mstatus;
}
static inline void lw_irq_restore(uint32_t mstatus)
{
    if (mstatus & 8u)
        __asm volatile("csrsi mstatus, 8" ::: "memory");
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

/* --- crash signatures and repeat records -------------------------------- */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t fnv_word(uint32_t h, uint32_t w)
{
    for (int i = 0; i < 4; i++) {
        h ^= (uint8_t)(w >> (8 * i));
        h *= 16777619u;
    }
    return h;
}

/* Offsets into struct lw_fault_info / lw_panic_info. */
#define F_LR 20u
#define F_PC 24u
#define F_IPSR 40u
#define F_CFSR 44u
#define F_HFSR 48u
#define F_SIG_LEN 52u
/* struct lw_trap_info */
#define T_MCAUSE 0u
#define T_MEPC 4u
#define T_RA 16u
#define T_SIG_LEN 20u

/* p holds the first min(len, F_SIG_LEN) payload bytes. */
static uint32_t sig_from(uint32_t type, uint32_t len, const uint8_t *p)
{
    uint32_t h = 2166136261u;
    if (type == LW_T_REPEAT)
        return len >= 4 ? le32(p) : 0;
    if (type == LW_T_FAULT && len >= F_SIG_LEN) {
        h = fnv_word(h, type);
        h = fnv_word(h, le32(p + F_IPSR));
        h = fnv_word(h, le32(p + F_CFSR));
        h = fnv_word(h, le32(p + F_HFSR) & 0x40000002u);
        h = fnv_word(h, le32(p + F_LR) & ~1u);
        h = fnv_word(h, le32(p + F_PC) & ~1u);
#if !LW_CORTEX_M /* a Cortex-M build never writes TRAP records; saves 60 bytes there */
    } else if (type == LW_T_TRAP && len >= T_SIG_LEN) {
        /* mtval is left out: for access faults it is the data address. */
        h = fnv_word(h, type);
        h = fnv_word(h, le32(p + T_MCAUSE));
        h = fnv_word(h, le32(p + T_RA) & ~1u);
        h = fnv_word(h, le32(p + T_MEPC) & ~1u);
#endif
    } else if (type == LW_T_PANIC && len >= 4) {
        h = fnv_word(h, type);
        h = fnv_word(h, le32(p) & ~1u);
    } else {
        return 0;
    }
    return h ? h : 1;
}

uint32_t lw_signature(const struct lw_entry *e)
{
    return sig_from(e->type, e->len, e->data);
}

#ifndef LW_NO_DEDUP
static uint32_t ring_sig(uint32_t pos)
{
    uint8_t p[F_SIG_LEN];
    uint32_t len = rd(pos + 1);
    for (uint32_t i = 0; i < F_SIG_LEN && i < len; i++)
        p[i] = rd(pos + HDR_LEN + i);
    return sig_from(rd(pos) >> 4, len, p);
}
#endif

/* Append a fault or panic record, or a short REPEAT record if the same
 * crash is still in the ring. Caller holds the lock or is the fault handler. */
static void record_crash(uint8_t type, const void *a, size_t alen, const void *b, size_t blen)
{
#ifndef LW_NO_DEDUP
    uint32_t sig = sig_from(type, (uint32_t)alen, a);
    struct walk w;
    uint32_t found = 0, count = 0;

    walk_back(lw_mem.head, &w);
    uint32_t pos = w.oldest;
    for (uint32_t i = 0; i < w.count; i++) {
        uint32_t t = rd(pos) >> 4, len = rd(pos + 1);
        if ((t == LW_T_FAULT || t == LW_T_TRAP || t == LW_T_PANIC) && ring_sig(pos) == sig) {
            found = 1;
            count = 0;
        } else if (found && t == LW_T_REPEAT && len >= 6 && ring_sig(pos) == sig) {
            count = rd(pos + HDR_LEN + 4) | (uint32_t)rd(pos + HDR_LEN + 5) << 8;
        }
        pos += len + OVERHEAD;
    }
    if (found && sig) {
        if (count < 0xffffu)
            count++;
        uint8_t rep[6] = {(uint8_t)sig, (uint8_t)(sig >> 8), (uint8_t)(sig >> 16), (uint8_t)(sig >> 24),
                          (uint8_t)count, (uint8_t)(count >> 8)};
        emit(LW_T_REPEAT, LW_FATAL, rep, sizeof rep, NULL, 0);
        return;
    }
#endif
    emit(type, LW_FATAL, a, alen, b, blen);
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
        start = (w.count && (w.last_type == LW_T_FAULT || w.last_type == LW_T_TRAP || w.last_type == LW_T_PANIC ||
                                 w.last_type == LW_T_REPEAT)) ? LW_START_CRASHED
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
/* Host tests: the fault handler's recording path without a fault. */
void lw_test_fault(const struct lw_fault_info *f, size_t len)
{
    record_crash(LW_T_FAULT, f, len, NULL, 0);
}

void lw_test_trap(const struct lw_trap_info *t, size_t len)
{
    record_crash(LW_T_TRAP, t, len, NULL, 0);
}

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
    record_crash(LW_T_PANIC, &pi, sizeof pi, msg, msg ? strlen(msg) : 0);
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

/* --- compact export ------------------------------------------------------ */

struct xo {
    lw_sink_fn sink;
    void *ctx;
    uint32_t total;
    uint16_t crc;
    uint8_t n;
    uint8_t buf[32];
};

static void x_flush(struct xo *x)
{
    if (x->n)
        x->sink(x->buf, x->n, x->ctx);
    x->n = 0;
}

static void x_raw(struct xo *x, uint8_t b)
{
    x->total++;
    x->buf[x->n++] = b;
    if (x->n == sizeof x->buf)
        x_flush(x);
}

static void x_byte(struct xo *x, uint8_t b)
{
    x->crc = crc_byte(x->crc, b);
    x_raw(x, b);
}

static void x_var(struct xo *x, uint32_t v)
{
    while (v >= 0x80) {
        x_byte(x, (uint8_t)(v | 0x80));
        v >>= 7;
    }
    x_byte(x, (uint8_t)v);
}

static uint32_t ring_le32(uint32_t pos)
{
    return (uint32_t)rd(pos) | (uint32_t)rd(pos + 1) << 8 | (uint32_t)rd(pos + 2) << 16 |
           (uint32_t)rd(pos + 3) << 24;
}

/* Fault and trap payloads are mostly 32-bit words, many of them zero (fault address
 * registers, r12, padding on the stack). They go out as a word count, a
 * bitmap of the non-zero words, and those words. Everything else is raw. */
static void x_payload(struct xo *x, uint32_t type, uint32_t pos, uint32_t len)
{
    uint32_t p = pos + HDR_LEN;
    uint32_t words = type == LW_T_FAULT || type == LW_T_TRAP;
    if (words && len % 4 == 0 && len > 0) {
        uint32_t nw = len / 4, nz = 0;
        for (uint32_t i = 0; i < nw; i++)
            nz += ring_le32(p + 4 * i) != 0;
        x_var(x, 1 + (nw + 7) / 8 + 4 * nz);
        x_byte(x, (uint8_t)nw);
        for (uint32_t i = 0; i < nw; i += 8) {
            uint8_t m = 0;
            for (uint32_t j = 0; j < 8 && i + j < nw; j++)
                m |= (uint8_t)((ring_le32(p + 4 * (i + j)) != 0) << j);
            x_byte(x, m);
        }
        for (uint32_t i = 0; i < nw; i++) {
            uint32_t v = ring_le32(p + 4 * i);
            if (v)
                for (int k = 0; k < 4; k++)
                    x_byte(x, (uint8_t)(v >> (8 * k)));
        }
        return;
    }
    x_var(x, len + words);
    if (words)
        x_byte(x, 0); /* unpacked */
    for (uint32_t i = 0; i < len; i++)
        x_byte(x, rd(p + i));
}

uint32_t lw_export(lw_sink_fn sink, void *ctx)
{
    struct xo x;
    struct walk w;

    x.sink = sink;
    x.ctx = ctx;
    x.total = 0;
    x.crc = 0xffff;
    x.n = 0;

    walk_back(lw_mem.head, &w);
    uint32_t pos = w.oldest;
    x_byte(&x, 'L');
    x_byte(&x, 'W');
    x_byte(&x, 'X');
    x_byte(&x, '1');
    x_var(&x, lw_mem.boot);
    x_var(&x, w.count);
    x_var(&x, w.count ? (uint32_t)(rd(pos + 2) | rd(pos + 3) << 8) : 0);

    uint32_t prev = 0;
    for (uint32_t i = 0; i < w.count; i++) {
        uint32_t type = rd(pos) >> 4, len = rd(pos + 1);
        uint32_t t = ring_le32(pos + 4);
        uint32_t d = t - prev;
        prev = t;
        x_byte(&x, rd(pos));
        x_var(&x, (d << 1) ^ (0u - (d >> 31))); /* zigzag: clocks restart at boot */
        x_payload(&x, type, pos, len);
        pos += len + OVERHEAD;
    }
    uint16_t crc = x.crc;
    x_raw(&x, (uint8_t)crc);
    x_raw(&x, (uint8_t)(crc >> 8));
    x_flush(&x);
    return x.total;
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

#ifndef LW_NO_FAULT_NAMES
static const char *const sfsr_names[] = {"INVEP", "INVIS", "INVER", "AUVIOL", "INVTRAN", "LSPERR", NULL, "LSERR"};

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
    uint32_t sfsr = 0, sfar = 0, tail = 64 + 4 * f.nstack;
    if (e->len >= tail + 8) {
        memcpy(&sfsr, e->data + tail, 4);
        memcpy(&sfar, e->data + tail + 4, 4);
    }

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
    for (uint32_t i = 0; i < 8; i++) {
        if (sfsr & (1u << i) && sfsr_names[i]) {
            l_ch(l, ' ');
            l_str(l, sfsr_names[i]);
        }
    }
#endif
    /* EXC_RETURN.ES=1, S=0: a Secure handler caught a Non-secure fault. */
    if ((f.exc_return >> 24) == 0xffu && (f.exc_return & 0x41u) == 0x01u)
        l_str(l, " in non-secure code");
    l_flush(l);
    l_str(l, "  ");
    l_kv(l, "sig", lw_signature(e));
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
    if (sfsr) {
        l_str(l, "  ");
        l_kv(l, "sfsr", sfsr);
        if (sfsr & (1u << 6))
            l_kv(l, "sfar", sfar);
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

/* RISC-V traps. Cortex-M builds never write them and skip the 0.8 KB. */
#if !LW_CORTEX_M
#ifndef LW_NO_FAULT_NAMES
static const char *const trap_names[] = {
    "instruction misaligned", "instruction access fault", "illegal instruction", "breakpoint",
    "load misaligned", "load access fault", "store misaligned", "store access fault",
    "ecall from U", "ecall from S", NULL, "ecall from M",
    "instruction page fault", "load page fault", NULL, "store page fault",
};
#endif

static void print_trap(struct line *l, const struct lw_entry *e)
{
    struct lw_trap_info t;
    memset(&t, 0, sizeof t);
    memcpy(&t, e->data, e->len < sizeof t ? e->len : sizeof t);
    if (t.nstack > LW_FAULT_STACK_WORDS)
        t.nstack = LW_FAULT_STACK_WORDS;

    l_str(l, "!! trap");
    if (t.mcause >> 31) {
        l_str(l, " interrupt ");
        l_dec(l, t.mcause & 0x7fffffffu, 0);
    }
#ifndef LW_NO_FAULT_NAMES
    else if (t.mcause < sizeof trap_names / sizeof trap_names[0] && trap_names[t.mcause]) {
        l_ch(l, ' ');
        l_str(l, trap_names[t.mcause]);
    }
#endif
    else {
        l_str(l, " cause ");
        l_dec(l, t.mcause, 0);
    }
    l_flush(l);
    l_str(l, "  ");
    l_kv(l, "sig", lw_signature(e));
    l_flush(l);
    l_str(l, "  ");
    l_kv(l, "mepc", t.mepc);
    l_kv(l, "ra", t.ra);
    l_kv(l, "sp", t.sp);
    l_kv(l, "mtval", t.mtval);
    l_flush(l);
    l_str(l, "  ");
    l_kv(l, "mcause", t.mcause);
    l_kv(l, "mstatus", t.mstatus);
    l_flush(l);
    l_str(l, "  ");
    l_kv(l, "a0", t.a0);
    l_kv(l, "a1", t.a1);
    l_kv(l, "a2", t.a2);
    l_kv(l, "a3", t.a3);
    l_flush(l);
    for (uint32_t i = 0; i < t.nstack; i++) {
        if (i % 4 == 0)
            l_str(l, i == 0 ? "   stack:" : "         ");
        l_ch(l, ' ');
        l_hex(l, t.stack[i]);
        if (i % 4 == 3 || i + 1 == t.nstack)
            l_flush(l);
    }
}
#endif

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
#if !LW_CORTEX_M
    case LW_T_TRAP:
        print_trap(l, e);
        break;
#endif
    case LW_T_PANIC:
        l_str(l, "!! panic");
        if (e->len >= 4)
            l_kv(l, "pc", le32(e->data));
        l_str(l, ": ");
        for (uint32_t i = 4; i < e->len; i++)
            l_ch(l, (char)e->data[i]);
        l_flush(l);
        l_str(l, "  ");
        l_kv(l, "sig", lw_signature(e));
        l_flush(l);
        break;
    case LW_T_REPEAT:
        l_str(l, "!! repeat");
        l_kv(l, "sig", lw_signature(e));
        l_str(l, " count=");
        l_dec(l, e->len >= 6 ? (uint32_t)(e->data[4] | e->data[5] << 8) : 0, 0);
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

#if LW_CORTEX_M || LW_RISCV

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
#endif

#if LW_RISCV

#ifndef LW_TRAP_STACK
#define LW_TRAP_STACK 512
#endif
#define LW_STR_(x) #x
#define LW_STR(x) LW_STR_(x)

/* The trap may come from a blown stack, so the handler runs on its own. */
static uint32_t lw_trap_stack[LW_TRAP_STACK / 4] __attribute__((used, aligned(16)));

void lw_trap_capture(const uint32_t saved[6])
{
    struct lw_trap_info t;

    if (lw_in_fault)
        lw_reboot(); /* trapped while recording a trap */
    lw_in_fault = 1;

    memset(&t, 0, sizeof t);
    __asm volatile("csrr %0, mcause" : "=r"(t.mcause));
    __asm volatile("csrr %0, mepc" : "=r"(t.mepc));
    __asm volatile("csrr %0, mtval" : "=r"(t.mtval));
    __asm volatile("csrr %0, mstatus" : "=r"(t.mstatus));
    t.ra = saved[0];
    t.a0 = saved[1];
    t.a1 = saved[2];
    t.a2 = saved[3];
    t.a3 = saved[4];
    t.sp = saved[5];

    if ((t.sp & 3u) == 0) {
        const uint32_t *sp = (const uint32_t *)(uintptr_t)t.sp;
        uint32_t limit = stack_limit();
        uint32_t n = 0;
        while (n < LW_FAULT_STACK_WORDS && (limit == 0 || (uintptr_t)(sp + n) < limit)) {
            t.stack[n] = sp[n];
            n++;
        }
        t.nstack = n;
    }

    size_t len = sizeof t - (LW_FAULT_STACK_WORDS - t.nstack) * sizeof(uint32_t);
    record_crash(LW_T_TRAP, &t, len, NULL, 0);
    lw_reboot();
}

/* sp goes to mscratch so no register is touched before it is saved. la is
 * kept from being relaxed against gp, which the crashing code may have
 * clobbered. */
__asm(".section .text.lw_trap_entry,\"ax\",@progbits\n"
      ".globl lw_trap_entry\n"
      ".type lw_trap_entry, @function\n"
      ".balign 4\n"
      "lw_trap_entry:\n"
      "  csrw mscratch, sp\n"
      "  .option push\n"
      "  .option norelax\n"
      "  la sp, lw_trap_stack + " LW_STR(LW_TRAP_STACK) "\n"
      "  .option pop\n"
      "  addi sp, sp, -32\n"
      "  sw ra, 0(sp)\n"
      "  sw a0, 4(sp)\n"
      "  sw a1, 8(sp)\n"
      "  sw a2, 12(sp)\n"
      "  sw a3, 16(sp)\n"
      "  csrr a0, mscratch\n"
      "  sw a0, 20(sp)\n"
      "  mv a0, sp\n"
      "  tail lw_trap_capture\n"
      ".size lw_trap_entry, . - lw_trap_entry\n"
      ".text\n");

void lw_trap_install(void)
{
    __asm volatile("csrw mtvec, %0" ::"r"((uintptr_t)lw_trap_entry) : "memory");
}

#endif /* LW_RISCV */

#if LW_CORTEX_M

void lw_fault_capture(uint32_t exc_return, uint32_t *frame)
{
    struct {
        struct lw_fault_info f;
        uint32_t room[2]; /* SFSR and SFAR land right after the stack words in use */
    } r;
    struct lw_fault_info *f = &r.f;
    uint32_t ipsr;

    if (lw_in_fault)
        lw_reboot(); /* faulted while recording a fault */
    lw_in_fault = 1;

    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    memset(&r, 0, sizeof r);
    f->exc_return = exc_return;
    f->ipsr = ipsr & 0x1ff;

#if LW_V8M
    /* DCRS=0: r4-r11 and the integrity signature sit below the basic frame.
     * Only a Secure handler sees this, after it interrupted Secure code. */
    if ((exc_return & 0x20u) == 0 && ((uintptr_t)frame & 3u) == 0 && (frame[0] | 1u) == 0xFEFA125Bu)
        frame += 10;
#endif

    if (((uintptr_t)frame & 3u) == 0) {
        f->r0 = frame[0];
        f->r1 = frame[1];
        f->r2 = frame[2];
        f->r3 = frame[3];
        f->r12 = frame[4];
        f->lr = frame[5];
        f->pc = frame[6];
        f->xpsr = frame[7];

        uint32_t words = 8;
        if ((exc_return & 0x10u) == 0) {
            words += 18; /* extended frame: s0-s15, fpscr, reserved */
#if LW_V8M && LW_HAS_CFSR
            /* FPCCR.TS with Secure code interrupted: s16-s31 were stacked too. */
            if ((exc_return & 0x40u) && (*(volatile uint32_t *)0xE000EF34u & (1u << 26)))
                words += 16;
#endif
        }
        if (f->xpsr & (1u << 9))
            words += 1; /* the core padded the frame to 8-byte alignment */
        uint32_t *sp = frame + words;
        f->sp = (uint32_t)(uintptr_t)sp;

        /* The Secure stack limit means nothing for a Non-secure stack. */
        uint32_t limit = (exc_return & 0x41u) == 0x01u ? 0 : stack_limit();
        uint32_t n = 0;
        while (n < LW_FAULT_STACK_WORDS && (limit == 0 || (uintptr_t)(sp + n) < limit)) {
            f->stack[n] = sp[n];
            n++;
        }
        f->nstack = n;
    }

#if LW_HAS_CFSR
    f->cfsr = *(volatile uint32_t *)0xE000ED28u;
    f->hfsr = *(volatile uint32_t *)0xE000ED2Cu;
    f->mmfar = *(volatile uint32_t *)0xE000ED34u;
    f->bfar = *(volatile uint32_t *)0xE000ED38u;
#endif

    size_t len = sizeof *f - (LW_FAULT_STACK_WORDS - f->nstack) * sizeof(uint32_t);
#if LW_V8M
    /* SFSR is RAZ from Non-secure state and absent without TrustZone, so it
     * is only read when the handler runs Secure (ES=1). */
    if (exc_return & 1u) {
        uint32_t sec[2] = {*(volatile uint32_t *)0xE000EDE4u, *(volatile uint32_t *)0xE000EDE8u};
        if (sec[0]) {
            memcpy((uint8_t *)f + len, sec, sizeof sec);
            len += sizeof sec;
        }
    }
#endif
    record_crash(LW_T_FAULT, f, len, NULL, 0);
    lw_reboot();
}

__attribute__((naked)) void lw_fault_entry(void)
{
    /* Thumb-1 only, so the same entry works on ARMv6-M and ARMv8-M Baseline. */
    /* GCC hands Thumb-1 targets' inline asm over in divided syntax. */
    __asm volatile(".syntax unified\n"
                   "mov r0, lr\n"
#if LW_V8M
                   /* ES=1, S=0: Secure handler, frame on a Non-secure stack. */
                   "lsls r2, r0, #31\n" /* ES into Z */
                   "beq 3f\n"
                   "lsls r2, r0, #25\n" /* S into N */
                   "bmi 3f\n"
                   "movs r2, #4\n"
                   "tst r0, r2\n"
                   "bne 4f\n"
                   "mrs r1, msp_ns\n"
                   "b 2f\n"
                   "4:\n"
                   "mrs r1, psp_ns\n"
                   "b 2f\n"
                   "3:\n"
#endif
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
