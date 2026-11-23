/*
 * lastwords: a crash flight recorder for microcontrollers.
 *
 * Log lines and fault records go into a ring buffer in RAM that the C
 * runtime does not zero. After a watchdog, software or fault reset the ring
 * is still there, gets validated, and can be printed or shipped.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LASTWORDS_H
#define LASTWORDS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LW_VERSION "0.3.0"

/* Ring size in bytes. Must be a power of two between 256 and 65536. */
#ifndef LW_RING_SIZE
#define LW_RING_SIZE 1024
#endif

/* Words of the faulting stack copied into a fault record. */
#ifndef LW_FAULT_STACK_WORDS
#define LW_FAULT_STACK_WORDS 16
#endif

/* Longest payload of a single record. lw_logf truncates to this. */
#define LW_MAX_PAYLOAD 255

enum lw_level { LW_DEBUG = 0, LW_INFO = 1, LW_WARN = 2, LW_ERROR = 3, LW_FATAL = 4 };

enum lw_type {
    LW_T_TEXT = 0,  /* payload: UTF-8 text, not NUL terminated */
    LW_T_BOOT = 1,  /* payload: struct lw_boot_info */
    LW_T_FAULT = 2, /* payload: struct lw_fault_info */
    LW_T_PANIC = 3, /* payload: struct lw_panic_info + message */
    LW_T_BLOB = 4,  /* payload: application bytes */
    LW_T_REPEAT = 5, /* payload: struct lw_repeat_info; a crash whose full record is still in the ring */
    LW_T_TRAP = 6    /* payload: struct lw_trap_info (RISC-V machine-mode exception) */
};

/* What lw_init() found in RAM. */
enum lw_start {
    LW_START_COLD = 0,   /* no valid ring: power-on, or RAM was lost */
    LW_START_WARM = 1,   /* ring recovered, previous boot ended normally or by an external reset */
    LW_START_CRASHED = 2 /* ring recovered and the previous boot ended in a fault or panic */
};

struct lw_boot_info {
    uint32_t boot;        /* boot counter, survives warm resets */
    uint32_t reset_cause; /* whatever was passed to lw_init(), usually a raw RCC/RSTC register */
};

/* Exception frame as stacked by the core, plus the fault status registers.
 * On ARMv6-M / ARMv8-M Baseline cfsr..bfar read as zero. */
struct lw_fault_info {
    uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
    uint32_t exc_return;
    uint32_t sp;   /* stack pointer before the exception frame was pushed */
    uint32_t ipsr; /* exception number: 3 HardFault, 4 MemManage, 5 BusFault, 6 UsageFault, 7 SecureFault */
    uint32_t cfsr, hfsr, mmfar, bfar;
    uint32_t nstack;
    uint32_t stack[LW_FAULT_STACK_WORDS];
};

/* RISC-V: the trap CSRs and the registers that point back at the caller.
 * sp and the stack slice are those of the code that trapped. */
struct lw_trap_info {
    uint32_t mcause, mepc, mtval, mstatus;
    uint32_t ra, sp;
    uint32_t a0, a1, a2, a3;
    uint32_t nstack;
    uint32_t stack[LW_FAULT_STACK_WORDS];
};

struct lw_panic_info {
    uint32_t pc; /* address lw_panic() was called from */
};

/* Written instead of a second full fault or panic record when the same crash
 * (same signature) is already in the ring. Keeps a crash loop from pushing
 * the log that led up to it out of the ring. */
struct lw_repeat_info {
    uint32_t sig;
    uint16_t count; /* repeats since the full record, 1 for the first */
};

/* One record handed to lw_foreach callbacks. data is a private copy. */
struct lw_entry {
    uint8_t type;
    uint8_t level;
    uint16_t seq;
    uint32_t time;
    uint32_t len;
    uint8_t data[LW_MAX_PAYLOAD];
};

struct lw_stats {
    uint32_t boot;
    uint32_t records;    /* valid records currently in the ring */
    uint32_t bytes_used; /* bytes those records occupy, overhead included */
};

/* Call once, early in main() and before the first log call.
 * reset_cause is stored verbatim in the BOOT record; pass 0 if unknown. */
enum lw_start lw_init(uint32_t reset_cause);

/* Optional millisecond (or any unit) clock stored with each record. */
void lw_set_clock(uint32_t (*now)(void));

void lw_log(enum lw_level level, const char *text);
void lw_write(enum lw_level level, enum lw_type type, const void *data, size_t len);

#ifndef LW_NO_PRINTF
void lw_logf(enum lw_level level, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
#endif

/* Record a panic with the caller's address and reset via lw_reboot(). */
void lw_panic(const char *msg)
#if defined(__GNUC__)
    __attribute__((noreturn))
#endif
    ;

/* Walk the ring from oldest to newest. Return nonzero from fn to stop.
 * Returns the number of records visited. */
typedef int (*lw_visit_fn)(const struct lw_entry *e, void *ctx);
uint32_t lw_foreach(lw_visit_fn fn, void *ctx);

/* Human-readable dump, one line at a time (each line ends in '\n'). */
typedef void (*lw_out_fn)(const char *line, void *ctx);
void lw_print(lw_out_fn out, void *ctx);

void lw_get_stats(struct lw_stats *st);

/* Crash signature of a FAULT, TRAP, PANIC or REPEAT record, 0 for anything
 * else. FNV-1a over the record type, exception number, CFSR,
 * HFSR.FORCED/VECTTBL, pc and lr (RISC-V: mcause, mepc and ra; panic: the
 * caller address). Registers, sp and the stack slice
 * are left out, so the same bug hits the same signature on every device
 * running the same build. tools/lwdecode.py computes the same value. */
uint32_t lw_signature(const struct lw_entry *e);

/* Stream the ring in the compact export format (see README) to sink, in
 * chunks of up to 32 bytes. Returns the number of bytes produced. Call it
 * from normal context; records logged from interrupts while it runs can
 * overwrite the oldest ones being sent, which the host sees as a CRC error. */
typedef void (*lw_sink_fn)(const void *data, size_t len, void *ctx);
uint32_t lw_export(lw_sink_fn sink, void *ctx);

/* Forget everything, e.g. after the log was uploaded. */
void lw_clear(void);

/* Raw region, for copying to flash or sending upstream from normal context. */
const void *lw_region(size_t *size);

/* Called after a fault or panic was recorded. The default resets the core
 * through AIRCR.SYSRESETREQ on Cortex-M and spins elsewhere (RISC-V has no
 * standard reset); it is weak, so define your own to
 * flush a UART first or to halt under a debugger. */
void lw_reboot(void)
#if defined(__GNUC__)
    __attribute__((noreturn))
#endif
    ;

/* Cortex-M only: the naked fault entry. lastwords aliases the CMSIS fault
 * handler names to it unless LW_NO_FAULT_HANDLERS is defined. Branch to it
 * or put it in the vector table; do not call it, it needs EXC_RETURN in lr. */
void lw_fault_entry(void);

/* Cortex-M only: C half of the fault handler. Exposed for RTOS ports that
 * install their own HardFault entry. */
void lw_fault_capture(uint32_t exc_return, uint32_t *frame)
#if defined(__GNUC__)
    __attribute__((noreturn))
#endif
    ;

/* RISC-V only: machine-mode trap entry. Point mtvec at it (direct mode), or
 * jump to it from slot 0 of a vectored table so interrupts keep their own
 * handlers. It switches to a private stack of LW_TRAP_STACK bytes, records a
 * TRAP and calls lw_reboot(). In direct mode an interrupt also lands here
 * and is recorded as a crash, so only use direct mode without interrupts. */
void lw_trap_entry(void);
void lw_trap_install(void);

/* RISC-V only: C half of the trap entry, for ports with their own mtvec
 * handler. saved holds ra, a0, a1, a2, a3 and sp as they were at the trap. */
void lw_trap_capture(const uint32_t saved[6])
#if defined(__GNUC__)
    __attribute__((noreturn))
#endif
    ;

#define LW_D(...) lw_logf(LW_DEBUG, __VA_ARGS__)
#define LW_I(...) lw_logf(LW_INFO, __VA_ARGS__)
#define LW_W(...) lw_logf(LW_WARN, __VA_ARGS__)
#define LW_E(...) lw_logf(LW_ERROR, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
