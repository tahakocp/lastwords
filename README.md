# lastwords

[![ci](https://github.com/tahakocp/lastwords/actions/workflows/ci.yml/badge.svg)](https://github.com/tahakocp/lastwords/actions/workflows/ci.yml)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**The last log lines and the fault that killed your microcontroller, still in RAM after the reset.**

![QEMU demo on Cortex-M3: five boots, then the recovered log with the HardFault pinned to a source line](docs/demo.gif)

<sub>Real run of `make run-m3` in [examples/qemu](examples/qemu), recorded with v0.1.0 (cast file: [docs/demo.cast](docs/demo.cast)). Boot 0 crashed; a later boot printed what boot 0 was doing when it died.</sub>

lastwords is a crash flight recorder for Cortex-M in two C files. Your log calls go into a small ring buffer in a RAM section the startup code never zeroes. When the chip faults, the handler appends the registers and a slice of the stack to the same ring and resets. On the next boot `lw_init()` validates what is left, tells you the previous run crashed, and you print or upload it whenever it suits you.

No flash writes inside the fault handler, no heap, no vendor HAL, no cloud. It works for the device on your desk and for the one in a field that only gets a UART once a month.

New in 0.2: every crash gets a signature, a crash that is already in the ring is stored as a 17-byte repeat record instead of a second full dump (so a crash loop does not wipe the log that led to it), and `lw_export()` streams the ring in a compact format that `tools/lwdecode.py` turns back into the exact `lw_print()` text, or into a table of distinct crashes across many devices.

This is what the last boot of the [QEMU demo](examples/qemu) prints about boot 0, after `tools/lwsym.py` added the source lines:

```text
[       3] #    4 I applying DMA config
[       3] #    5 !! HardFault PRECISERR FORCED
   sig=0x4403a4f5
   pc=0x000000c0 lr=0x00000151 sp=0x20003fd8 xpsr=0x01000000
   pc -> config_apply  main.c:72
   lr -> main  main.c:125
   r0=0x000000e2 r1=0x00000067 r2=0x00000008 r3=0x30000000 r12=0x00001f0e
   cfsr=0x00008200 hfsr=0x40000000 bfar=0x30000000
   stack: 0x00000000 0x00000030 0x00000000 0x00000000
          0x00000001 0x00000013 0x00000000 0x00000283
          0x00000000 0xffffffff
   backtrace guess (return addresses found on the stack):
     0x00000283  Reset_Handler  startup.c:11
[       0] #    6 -- boot 1 reset_cause=0x00000000
```

`main.c:72` is `*c->dma_ctrl = 1u << c->channel;` with a pointer to unmapped memory, and `bfar` holds that address. The log line before the fault says what the firmware was doing. A register dump tells you where it died; the log tells you why it was there.

## Numbers

All measured; the commands are in [Running the tests](#running-the-tests).

| | |
|---|---|
| Flash added to a minimal Cortex-M3 image, `-Os`, `LW_NO_PRINTF`: init, log, fault capture with repeat detection | 1,740 bytes with GCC 15.2, 1,824 with Ubuntu's GCC 14.2 in CI (1,212 with GCC 15.2 and `LW_NO_DEDUP`) |
| Whole object incl. `lw_print`, `lw_export` and the fault-bit names | 4.7 KB (4.3 KB with `LW_NO_FAULT_NAMES`); `lw_export` is 0.7 KB of that and is dropped by `--gc-sections` if unused |
| RAM | ring size + 24 bytes header (`.noinit`) + 7 bytes `.bss` |
| `lw_log()` of a 32-byte line, Cortex-M3 | 1,242 instructions with GCC 15.2, 1,298 with 14.2 (QEMU instruction count; real cycles depend on flash wait states) |
| Record overhead | 11 bytes (8 header, CRC-16, trailing length) |
| Resets injected mid-write by the host tests | 14,576, one at every single store of a `lw_log()` call, at 300 ring offsets: 0 corrupt records recovered |
| Fully random RAM at boot (1,000 fills) | always detected as a cold start |
| Valid header, random ring contents (1,000,000 trials) | 1 bogus record accepted at 1 KB ring, 0 at 256 B |
| Crash scenarios run in QEMU on Cortex-M0, M3, M4, M7 | 4 of 4 recovered and pinned to the right source line on every core; the fifth crash, a repeat of the first, stored as a repeat record and traced back to it |
| Crash loop on a 1 KB ring (same fault every boot, one log line per boot) | the log from before the first crash survives 11 boots, 4 with `LW_NO_DEDUP` |
| Export size, QEMU demo ring (34 records, 1,291 bytes of records) | 963 bytes on M3, 917 on M0 with GCC 15.2 (25-30% smaller); no buffer needed, it streams |
| Exports decoded by `lwdecode.py` | 52 host scenarios and 4 QEMU runs, all byte-identical to `lw_print`; 10,400 of 10,400 single-bit flips rejected by the CRC |

That one-in-a-million line is the price of a 16-bit CRC: if RAM decays just enough to keep the header but scramble the ring, roughly one boot in a million can show a line of noise. A cold power-on randomises the header too, and is caught by the magic and size check.

## Quick start

1. Copy `src/lastwords.c` and `src/lastwords.h` into your project (or use CMake / PlatformIO, see below).

2. Make sure your linker script has a section that is neither loaded nor zeroed. Many already do (`.noinit` in AVR Arduino, Zephyr, ESP-IDF; `.uninitialized_data` in the Pico SDK, pass `-DLW_SECTION=\".uninitialized_data\"`). STM32CubeIDE scripts do not, so add this after `.bss`:

   ```ld
   .noinit (NOLOAD) :
   {
     . = ALIGN(4);
     *(.noinit*)
   } >RAM
   ```

3. Call `lw_init()` first thing in `main()`, then log as usual:

   ```c
   #include "lastwords.h"

   int main(void)
   {
       enum lw_start s = lw_init(RCC->CSR);   /* any raw reset-cause value, or 0 */
       clocks_init();
       uart_init();
       lw_set_clock(HAL_GetTick);

       if (s == LW_START_CRASHED)
           lw_print(uart_write_line, NULL);   /* or queue the ring for upload */

       LW_I("fw %s up, vbat %u mV", FW_VERSION, vbat_mv());
       ...
   }
   ```

4. That's it for faults: `lastwords.c` defines `HardFault_Handler`, `MemManage_Handler`, `BusFault_Handler`, `UsageFault_Handler` (and `SecureFault_Handler` on ARMv8-M Mainline). CMSIS startup files declare these weak, so ours win. If you already have a HardFault handler, build with `-DLW_NO_FAULT_HANDLERS` and put `lw_fault_entry` in your vector table, or branch to it (not call: it reads EXC_RETURN from `lr`). For asserts there is `lw_panic("msg")`, which records the caller's address and resets.

5. On the host, feed the printed log and the matching ELF to the symbolizer:

   ```sh
   python3 tools/lwsym.py build/firmware.elf uart.log
   ```

   It needs only Python 3 and `arm-none-eabi-addr2line` (or `llvm-addr2line`) on `PATH`.

CMake: `add_subdirectory(lastwords)` and `target_link_libraries(app PRIVATE lastwords)`. PlatformIO: `lib_deps = https://github.com/tahakocp/lastwords.git`.

## API

```c
enum lw_start lw_init(uint32_t reset_cause);     /* COLD, WARM or CRASHED */
void lw_set_clock(uint32_t (*now)(void));         /* timestamp source, optional */

void lw_log(enum lw_level, const char *text);
void lw_logf(enum lw_level, const char *fmt, ...); /* LW_D / LW_I / LW_W / LW_E macros */
void lw_write(enum lw_level, enum lw_type, const void *data, size_t len); /* binary blobs */
void lw_panic(const char *msg);                   /* noreturn */

uint32_t lw_foreach(lw_visit_fn fn, void *ctx);   /* oldest to newest */
uint32_t lw_signature(const struct lw_entry *e);  /* crash signature, 0 if not a crash record */
uint32_t lw_export(lw_sink_fn sink, void *ctx);   /* compact stream, 32-byte chunks */
void lw_print(lw_out_fn out, void *ctx);          /* human-readable, one line per call */
void lw_get_stats(struct lw_stats *st);
void lw_clear(void);
const void *lw_region(size_t *size);              /* raw bytes, to copy to flash or ship */
void lw_reboot(void);                             /* weak; default is AIRCR.SYSRESETREQ */
```

Compile-time options: `LW_RING_SIZE` (power of two, 256 to 65536, default 1024), `LW_FAULT_STACK_WORDS` (default 16), `LW_SECTION`, `LW_NO_PRINTF` (drops `lw_logf` and the vsnprintf dependency), `LW_NO_FAULT_NAMES`, `LW_NO_FAULT_HANDLERS`, `LW_NO_DEDUP` (always write full crash records), `LW_STACK_TOP`, and `LW_LOCK()`/`LW_UNLOCK()` if masking interrupts with PRIMASK is not what you want.

Logging is safe from interrupts: each record is written with interrupts masked. A 32-byte line is about 1,200 instructions, so tens of microseconds at common clock speeds. If that is too long for your fastest ISR, log less from it or supply your own lock.

## How a half-written record is survived

Every record is `[type|level][len][seq x2][time x4][payload][crc16 x2][len]`. The write order is: all record bytes first, then `head`, then `~head` as a check word. So at any instant `head` points at the end of complete records only.

At boot `lw_init()` starts at `head` and walks backwards, using the trailing length byte to find each record's start. It stops at the first record whose CRC fails, whose two length bytes disagree, or whose sequence number is not exactly one less than the newer record's. That is where older data was overwritten by the ring, or where a reset cut a write short. Nothing past that point is ever shown.

If the reset lands between the two head stores, `head` and `~head` disagree; the new head is tried first (its record is complete by construction), then the old one.

The host test `test_torn_writes` checks this by aborting a `lw_log()` call at every single store, at 300 different ring positions, then rebooting the simulated device: the recovered history always ends with either the previous line or the new one, never with a fragment.

## Same crash, every boot

A device that crashes in a loop is the one you most want the story from, and a plain ring makes it the one you lose: each boot appends a boot record and another ~140-byte fault dump, and after a few rounds the lines that led up to the first crash are gone.

Each fault and panic gets a 32-bit signature: FNV-1a over the record type, exception number, CFSR, HFSR.FORCED/VECTTBL, pc and lr (the caller address for a panic). Registers, sp and the stack slice are left out, so the same bug on the same build gives the same signature on every device and every run. Before the fault handler writes a record it walks the ring; if a full record with that signature is still there, it writes a 6-byte repeat payload instead (signature plus count):

```text
[       3] #   29 !! repeat sig=0x4403a4f5 count=1
   same crash as #5: config_apply  main.c:72
```

The second line is `lwsym.py` pointing back at the full record. If the full record has been overwritten by the time the crash comes back, a new full record is written.

## Shipping the ring

`lw_export(sink, ctx)` streams the valid records through your callback in chunks of up to 32 bytes, so it needs no buffer: hand it a UART write, a BLE notify queue, or a function that appends to a flash page. The format:

```text
"LWX1"  varint boot  varint count  varint first_seq
per record:  [type<<4|level]  varint zigzag(time delta)  varint len  payload
crc16 (CCITT-FALSE, little-endian) over everything before it
```

Sequence numbers are implicit (the ring only keeps consecutive ones). Fault payloads go out as a word count, a bitmap of non-zero words and those words; text, panic and blob payloads are raw. On the host:

```sh
python3 tools/lwdecode.py dump.bin           # the same text lw_print() gives, then pipe into lwsym.py
python3 tools/lwdecode.py uart.log           # hex between "---- export ----" markers works too
python3 tools/lwdecode.py --crashes devices/*.bin --elf fw.elf
```

```text
count  devices  signature   crash
    2        1  4403a4f5  HardFault  config_apply <- main
    1        1  97a05ca1  HardFault  average <- main
    1        1  91bdf9a5  panic assert free_blocks > 4  main
    1        1  a4913da1  HardFault  (bad jump) <- main
```

That is the M3 QEMU run: the boot 0 crash and its boot 4 repeat are one row.

`--crashes` counts each crash once per occurrence, repeat records included, and with `--elf` also merges signatures whose pc and lr resolve to the same functions, so a crash keeps its row when a rebuild moves the addresses. It is Python 3 only, no packages.

## Compared with

| | what it keeps across reset | size / scope | license |
|---|---|---|---|
| **lastwords** | log ring + fault registers + stack slice, in one ordered history; repeated crashes collapse on the device | 2 C files, Cortex-M fault capture, any MCU for logging, offline decoder | MIT |
| [adamgreen/CrashCatcher](https://github.com/adamgreen/CrashCatcher) | full register + RAM dump, written out at fault time through your dump callbacks | Cortex-M, GDB-based host side | Apache-2.0 |
| [armink/CmBacktrace](https://github.com/armink/CmBacktrace) | nothing by itself; prints the diagnosis at fault time (persisting means writing flash from the handler with EasyFlash) | on-device call stack decoding, RTOS ports | MIT |
| [memfault/memfault-firmware-sdk](https://github.com/memfault/memfault-firmware-sdk) | coredumps, logs, metrics, reboot reasons | full SDK | source available, licensed only for use with the Memfault service |
| [laststate/latch](https://github.com/laststate/latch) | snapshots, breadcrumbs, metrics, delivery spool | larger framework, several architectures | Apache-2.0 |
| [korken89/defmt-persist](https://github.com/korken89/defmt-persist) | defmt log ring | Rust / defmt only | Apache-2.0 |
| Zephyr coredump + retention, ESP-IDF core dump | full dumps | tied to that RTOS / SDK | Apache-2.0 |

Memfault groups crashes into issues on its server; lastwords does the grouping on the device (repeat records) and offline (`lwdecode.py --crashes`), with no service in between. If you already run Memfault, use Memfault. If you want an on-device call-stack printer, CmBacktrace is mature. lastwords is for the case in between: a bare-metal or FreeRTOS product where you want the story before the crash, without adopting a framework.

## Running the tests

```sh
make test         # host tests at ring sizes 256, 1024, 4096; signatures, repeats, export + decoder
make test-asan    # same under AddressSanitizer + UBSan
make qemu         # needs arm-none-eabi-gcc and qemu-system-arm
make cross        # compile-only for M0+, M23, M33, M55 and friends
make footprint    # flash cost on Cortex-M3
make count        # instructions per lw_log(), from a QEMU execution trace
./build/test_1024 1000000   # the random-ring trial from the table
```

`make qemu` builds [examples/qemu](examples/qemu) for Cortex-M0 (micro:bit model), M3 (MPS2 AN385), M4 (AN386) and M7 (AN500), and runs each through six boots. Boot 0 writes through a bad pointer, boot 1 divides by zero (an unaligned `LDM` on M0, which has no divide trap), boot 2 fails an assert, boot 3 calls a function pointer without the Thumb bit, boot 4 hits the boot 0 bug again, and boot 5 prints everything and then the export as hex. `tests/check_qemu.py` checks that each crash was recovered and symbolized to the right function and that boot 4 became a repeat record; `tests/check_export.py` checks that the export decodes to exactly what the device printed. CI runs all of this on every push.

## Limitations

- Verified in simulation only: QEMU and host tests. I have not yet run it on silicon; the mechanism (SRAM retained over a warm reset) is standard on Cortex-M parts, but check yours: some bootloaders and ROM boot code use the top or bottom of RAM, some parts with ECC RAM need it initialised after reset, and a power cycle or brown-out loses everything by design.
- If the main stack itself overflowed, the fault handler has no stack to run on and the core locks up instead of recording. Put the stack at the bottom of RAM (so overflow faults immediately) or use the MPU or ARMv8-M stack limit registers.
- The fault handler trusts the stacked frame pointer and only checks alignment and the top of the stack. A wild PSP in an RTOS task can cause a second fault, which on ARMv7-M means lockup and a reset without a record.
- Faults are recorded as HardFault unless you enable the separate MemManage/BusFault/UsageFault exceptions in `SCB->SHCSR`. The CFSR bits still say what happened.
- ARMv6-M (M0/M0+) and ARMv8-M Baseline (M23) have no fault status registers, so the record has the frame and stack only. `lwsym.py` still flags a cleared Thumb bit in xPSR as a bad jump.
- The backtrace is a heuristic: odd stack words that point right after a `BL`/`BLX`. Stale return addresses from earlier calls can show up, and frames whose return address lives in a callee-saved register will not.
- Records are capped at 255 bytes; `lw_logf` truncates.
- 16-bit sequence numbers and CRC, see the numbers table for what that costs.
- Two different bugs with the same exception, fault bits, pc and lr share a signature. Signatures are per build: rebuild and the addresses move, which is what `--elf` grouping is for.
- Repeat detection walks the ring inside the fault handler, the same CRC-checked walk `lw_init()` does at boot, so the handler takes longer on a big, full ring. `LW_NO_DEDUP` turns it off.
- `lw_export` does not lock the ring while it streams. Lines logged from interrupts meanwhile can overwrite the oldest records being sent; the host then reports a CRC mismatch, never wrong data.

## FAQ

**Why not write the crash to flash right away?** Erasing and programming flash from a fault handler means running the flash driver with a broken system state, often with interrupts and clocks in an unknown state, and it takes milliseconds to seconds. RAM survives a warm reset anyway. Copy the region to flash later, from normal code, if you need it to survive power loss: `lw_region()` gives you the bytes.

**Does it work with FreeRTOS / Zephyr / bare metal?** Bare metal is what the demo runs. FreeRTOS should work, since tasks run on PSP and the entry stub picks the right stack, but I have not tested it under FreeRTOS yet. Zephyr has its own fault handling and coredump; use the logging part with `LW_NO_FAULT_HANDLERS` if you want the ring there.

**RISC-V, ESP32?** The ring, `lw_log` and `lw_print` are plain C and run anywhere (the host tests run them on x86-64). Fault capture is Cortex-M only for now. On ESP-IDF the ring works, but getting the crash into it would mean hooking IDF's own panic handler, which this repo does not do.

**Can I parse the ring on the host instead of printing on the device?** Yes: send `lw_export()` output and run `tools/lwdecode.py` on it, see [Shipping the ring](#shipping-the-ring).

## License

MIT, see [LICENSE](LICENSE).
