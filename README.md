# lastwords

[![ci](https://github.com/tahakocp/lastwords/actions/workflows/ci.yml/badge.svg)](https://github.com/tahakocp/lastwords/actions/workflows/ci.yml)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**The last log lines and the fault that killed your microcontroller, still in RAM after the reset.**

![QEMU demo on Cortex-M3: five boots, then the recovered log with the HardFault pinned to a source line](docs/demo.gif)

<sub>Real run of `make run-m3` in [examples/qemu](examples/qemu), recorded with v0.5.0 (cast file: [docs/demo.cast](docs/demo.cast)). Boot 0 crashed; a later boot printed what boot 0 was doing when it died. The same output from an export, in the browser: [tahakocp.github.io/lastwords](https://tahakocp.github.io/lastwords/).</sub>

lastwords is a crash flight recorder for Cortex-M and 32-bit RISC-V in two C files. Your log calls go into a small ring buffer in a RAM section the startup code never zeroes. When the chip faults, the handler appends the registers and a slice of the stack to the same ring and resets. On the next boot `lw_init()` validates what is left, tells you the previous run crashed, and you print or upload it whenever it suits you.

No flash writes inside the fault handler, no heap, no vendor HAL, no cloud. It works for the device on your desk and for the one in a field that only gets a UART once a month.

New in 0.2: every crash gets a signature, a crash that is already in the ring is stored as a 17-byte repeat record instead of a second full dump (so a crash loop does not wipe the log that led to it), and `lw_export()` streams the ring in a compact format that `tools/lwdecode.py` turns back into the exact `lw_print()` text, or into a table of distinct crashes across many devices.

New in 0.3: RV32 machine-mode trap capture. `lw_trap_entry` goes in `mtvec`, a trap is stored with `mcause`, `mepc`, `mtval`, `ra`, `sp` and a stack slice, and everything above (signatures, repeats, export, decoder, `lwsym.py`) handles it. Tested on QEMU `virt` only, see [RISC-V](#risc-v).

New in 0.4: TrustZone on ARMv8-M. A Secure fault handler reads the frame from `MSP_NS`/`PSP_NS` when the fault came from Non-secure code, skips the extra state context when it is there, and stores SFSR/SFAR with every SecureFault. Tested on a Cortex-M55 in QEMU `mps3-an547` only, see [TrustZone](#trustzone-armv8-m).

New in 0.5: a symbolizer that runs in the browser, at [tahakocp.github.io/lastwords](https://tahakocp.github.io/lastwords/). Drop the ELF and an export; it prints what `lwdecode.py | lwsym.py` would, without a toolchain or Python and without uploading anything. See [In the browser](#in-the-browser).

New in 0.6: a `no_std` Rust crate in [rust/](rust). It compiles the same C file for your target and wraps it, so a Rust firmware gets the same records, export bytes and fault capture, and `lastwords::log!` formats with `core::fmt` instead of `vsnprintf`. See [Rust](#rust).

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
| Flash added to a minimal Cortex-M3 image, `-Os`, `LW_NO_PRINTF`: init, log, fault capture with repeat detection | 1,764 bytes with GCC 15.2, 1,844 with Ubuntu's GCC 14.2 in CI (1,216 with GCC 15.2 and `LW_NO_DEDUP`). Cortex-M33: 1,896, Cortex-M55: 1,912 (GCC 15.2, TrustZone handling included) |
| Whole object incl. `lw_print`, `lw_export` and the fault-bit names | 4.8 KB (4.3 KB with `LW_NO_FAULT_NAMES`; the RISC-V trap printer is not compiled for Cortex-M); `lw_export` is 0.7 KB of that and is dropped by `--gc-sections` if unused |
| RAM | ring size + 24 bytes header (`.noinit`) + 7 bytes `.bss` |
| `lw_log()` of a 32-byte line, Cortex-M3 | 1,242 instructions with GCC 15.2, 1,298 with 14.2 (QEMU instruction count; real cycles depend on flash wait states) |
| Record overhead | 11 bytes (8 header, CRC-16, trailing length) |
| Resets injected mid-write by the host tests | 14,576, one at every single store of a `lw_log()` call, at 300 ring offsets: 0 corrupt records recovered |
| Fully random RAM at boot (1,000 fills) | always detected as a cold start |
| Valid header, random ring contents (1,000,000 trials) | 1 bogus record accepted at 1 KB ring, 0 at 256 B |
| Crash scenarios run in QEMU on Cortex-M0, M3, M4, M7 | 4 of 4 recovered and pinned to the right source line on every core; the fifth crash, a repeat of the first, stored as a repeat record and traced back to it |
| Crash loop on a 1 KB ring (same fault every boot, one log line per boot) | the log from before the first crash survives 11 boots, 4 with `LW_NO_DEDUP` |
| Export size, QEMU demo ring (34 records, 1,291 bytes of records) | 963 bytes on M3, 917 on M0 with GCC 15.2 (25-30% smaller); no buffer needed, it streams |
| Exports decoded by `lwdecode.py` | 53 host scenarios (traps included) and 5 QEMU runs (4 Cortex-M, 1 RV32), all byte-identical to `lw_print`; 10,600 of 10,600 single-bit flips rejected by the CRC |
| Flash added to a minimal rv32imac image, `-Os`, `LW_NO_PRINTF`: init, log, trap entry and capture with repeat detection | 2,128 bytes with xPack GCC 14.2 |
| RAM on RV32 | ring size + 24 bytes header (`.noinit`) + 512 bytes trap stack (`LW_TRAP_STACK`) + 16 bytes `.bss` |
| `lw_log()` of a 32-byte line, rv32imac | about 1,560 instructions (QEMU instruction count) |
| Trap scenarios run in QEMU `virt` (rv32) | 4 of 4 recovered and pinned to the right source line, the repeat traced back to the first trap; export 941 bytes for 1,252 bytes of records |

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
void lw_reboot(void);                             /* weak; default is AIRCR.SYSRESETREQ, spins on RISC-V */
void lw_trap_install(void);                       /* RISC-V: mtvec = lw_trap_entry */
```

Compile-time options: `LW_RING_SIZE` (power of two, 256 to 65536, default 1024), `LW_FAULT_STACK_WORDS` (default 16), `LW_SECTION`, `LW_NO_PRINTF` (drops `lw_logf` and the vsnprintf dependency), `LW_NO_FAULT_NAMES`, `LW_NO_FAULT_HANDLERS`, `LW_NO_DEDUP` (always write full crash records), `LW_STACK_TOP`, `LW_TRAP_STACK` (RISC-V, default 512), and `LW_LOCK()`/`LW_UNLOCK()` if masking interrupts with PRIMASK (RISC-V: `mstatus.MIE`) is not what you want.

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

Sequence numbers are implicit (the ring only keeps consecutive ones). Fault and trap payloads go out as a word count, a bitmap of non-zero words and those words; text, panic and blob payloads are raw. On the host:

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

## In the browser

[tahakocp.github.io/lastwords](https://tahakocp.github.io/lastwords/) takes the firmware ELF plus one of: an export `.bin`, a UART log with the hex between the export markers, or plain `lw_print()` text. It decodes the export, then resolves `pc`/`lr` (`mepc`/`ra` on RISC-V), the panic caller and the return addresses on the saved stack, the same way `lwsym.py` does. The "load the Cortex-M3 QEMU demo" button pulls in the ELF and log that the Pages build just produced.

It is one plain JavaScript file, [web/lwcore.js](web/lwcore.js) (no build step, no packages): the export decoder, an ELF32 reader, a DWARF `.debug_info`/`.debug_line` reader (versions 2 to 5, `DW_AT_ranges`, inlined functions) and the binutils rules for falling back to the symbol table. The same module runs under Node, so CI holds it to the Python tools and to addr2line:

```sh
node tests/check_web.mjs examples/qemu/build/demo-m3.elf    # after make qemu
```

```text
ok   m3: 4278/4278 addresses match addr2line (parse 3.4 ms)
ok   m3: 963 byte export decodes like lwdecode.py
ok   m3: annotated log matches lwsym.py
ok   m3: annotated export matches lwsym.py
```

Every halfword of `.text` is looked up in both, so inlined functions, padding between functions and newlib code without debug info are covered. CI runs it on all six demo ELFs (M0, M3, M4, M7, M55 TrustZone, RV32), about 5,000 addresses each. It has been checked against two Arm toolchains (xPack GCC 15.2 with binutils 2.45, Debian/Ubuntu GCC 14.2 whose newlib carries debug info) and xPack RISC-V GCC 14.2 with binutils 2.43. Getting there took three binutils rules that are not in the DWARF spec: an address outside every loaded section is never looked up (functions removed by `--gc-sections` keep debug info at 0), a unit also owns its functions' ranges, and the symbol-table fallback ignores symbol sizes.

File paths are shown relative to the build directory recorded in the ELF, which is what `lwsym.py` prints when run from there; change it in the box above the output.

## RISC-V

RV32 in machine mode, bare metal. Install the entry once, before anything can trap:

```c
lw_init(0);
lw_trap_install();          /* csrw mtvec, lw_trap_entry (direct mode) */
```

With a vectored `mtvec`, put `j lw_trap_entry` in slot 0 instead: exceptions go there and interrupts keep their own slots. In direct mode an interrupt would land in `lw_trap_entry` too and be recorded as a crash, so use direct mode only while interrupts are off, or call `lw_trap_capture()` from the exception branch of your own handler.

The entry parks `sp` in `mscratch`, switches to its own 512-byte stack (the trap may be a stack overflow), saves `ra`, `a0`-`a3` and the old `sp`, and calls the C half. That reads `mcause`, `mepc`, `mtval` and `mstatus`, copies up to 16 words of the old stack (bounded by `__StackTop`/`_estack` or `LW_STACK_TOP`), writes a TRAP record, and calls `lw_reboot()`. There is no standard way to reset a RISC-V core, so the default `lw_reboot()` only spins: define your own that pokes your SoC's reset register or lets the watchdog bite. The demo writes `0x7777` to QEMU's SiFive test device.

The signature covers `mcause`, `mepc` and `ra`, not `mtval`, which holds the data address on an access fault. Boot 3 of [examples/riscv](examples/riscv) calls a function pointer that points at nothing:

```text
[       3] #   22 I dispatching message 2
[       3] #   23 !! trap instruction access fault
   sig=0x53116f07
   mepc=0x00200000 ra=0x800001fe sp=0x8000ffd0 mtval=0x00200000
   mepc -> bad jump target (instruction access fault), the caller is at ra
   ra -> main  main.c:152
   mcause=0x00000001 mstatus=0x00001800
   a0=0x00000081 a1=0x00000002 a2=0x80002354 a3=0x80002f17
   stack: 0x00000033 0x00000003 0x00000013 0x000002d0
          0x00000000 0x00000000 0x00000000 0x80002000
          0x00000011 0x00000002 0x00000000 0x8000002a
   backtrace guess (return addresses found on the stack):
     0x8000002a  _start  start.S:18
```

`lwsym.py` sees from the ELF header that this is RISC-V, uses `riscv-none-elf-addr2line`, and accepts a stack word as a return address only if the instruction before it is a `jal`/`jalr` into `ra` or a `c.jal`/`c.jalr`.

The other boots: a store through a pointer to unmapped memory (store access fault, `mtval` = the address), an atomic add on a counter one byte into a frame buffer (misaligned AMO; QEMU 9.2 reports it as load misaligned, cause 4, where the spec and most cores say cause 6 or an access fault), a failed assert, and boot 4 repeating boot 0.

QEMU only. I have not run this on RISC-V silicon. What QEMU does not tell you about your chip: whether its boot ROM or reset logic clears SRAM, what reset register to use in `lw_reboot()`, and whether your SDK's startup already owns `mtvec` and `mscratch`.

## TrustZone (ARMv8-M)

On a core with the Security Extension the fault handlers usually run Secure, while the code that crashes may be Non-secure. `lw_fault_entry` reads EXC_RETURN and picks the stack the core actually used:

| EXC_RETURN | meaning | what lastwords does |
|---|---|---|
| ES=1, S=0 | Secure handler, Non-secure code faulted | frame from `MSP_NS`, or `PSP_NS` if a Non-secure thread had `CONTROL_NS.SPSEL` set; NS CFSR bank merged in; printed as `in non-secure code` |
| ES=1, S=1 | Secure handler, Secure code faulted | frame from `MSP`/`PSP` as before |
| DCRS=0 | r4-r11 and the integrity signature were stacked first | skipped (10 words) after checking the signature `0xFEFA125A/B` |
| FType=0, S=1, `FPCCR.TS` | s16-s31 were stacked too | sp skips the extra 16 words |
| ES=0 | handler is Non-secure (or no TrustZone) | as on ARMv7-M; SFSR is not read |

When the handler runs Secure and SFSR is non-zero, SFSR and SFAR go into the record after the stack words, and `lw_print`/`lwdecode.py` name the bits (`INVEP`, `INVIS`, `INVER`, `AUVIOL`, `INVTRAN`, `LSPERR`, `LSERR`). Records without them are unchanged, so 0.3 exports still decode. The signature does not cover SFSR; the exception number already tells a SecureFault apart.

[examples/armv8m](examples/armv8m) is a Secure image for QEMU's `mps3-an547` (Cortex-M55). It opens two SAU regions and enables SecureFault. Six boots: three ways Non-secure and Secure code can die, a Secure BusFault, a repeat, and the readout. Boot 0 calls into Non-secure memory with a plain `BLX`, which the core refuses:

```text
[       0] #    2 I calling plugin
[       0] #    3 !! SecureFault INVTRAN
   sig=0x5a78f34f
   pc=0x000000a0 lr=0x10000195 sp=0x30003fd8 xpsr=0x21000000
   pc -> not in this ELF (jump to a bad address?)
   lr -> main  main.c:148
   r0=0x000000a1 r1=0x0000006e r2=0xffffffb2 r3=0x30000052 r12=0x1000209c
   sfsr=0x00000010
   stack: 0x30000030 0x00000000 0x00000001 0x00000013
          0x00000000 0x00000000 0x00000000 0x10000297
          0x00000000 0xffffffff
   backtrace guess (return addresses found on the stack):
     0x10000297  Reset_Handler  startup.c:11
```

Boot 1 enters Non-secure code properly with `BLXNS`, and that code hits an undefined instruction. The Non-secure UsageFault is not enabled, so it escalates to the Secure HardFault, which finds the frame on the Non-secure main stack (`sp` is the top of the `MSP_NS` area, `lr` is the FNC_RETURN value):

```text
[       0] #    7 !! HardFault UNDEFINSTR FORCED in non-secure code
   sig=0xfcf5586b
   pc=0x000000a0 lr=0xfeffffff sp=0x20010000 xpsr=0x21000000
   pc -> not in this ELF (jump to a bad address?)
   r0=0x000000a0 r1=0x00000065 r2=0xfffffee0 r3=0x20010000 r12=0x1000209c
   cfsr=0x00010000 hfsr=0x40000000
   stack: 0x00000000 0x00000000 0x00000000 0x00000000
          0x00000000 0x00000000 0x00000000 0x00000000
          0x00000000 0x00000000 0x00000000 0x00000000
          0x00000000 0x00000000 0x00000000 0x00000000
```

Boot 2 runs a Non-secure function that moves itself onto a process stack (`msr psp`, `CONTROL.SPSEL=1`) before it hits `udf`; the record shows `pc=0x000000b8` and `sp=0x2000c000`, the `PSP_NS` it set. Getting that right took one detour: in QEMU the EXC_RETURN.SPSEL bit a Secure handler sees for a Non-secure fault follows the Secure CONTROL, and said `MSP_NS`. The entry now reads `CONTROL_NS.SPSEL` for that case instead, which is correct whichever way a core fills the bit.

`pc` is the Non-secure alias of the function (`0x100000a0` in the Secure image). In a real system the Non-secure side is a separate ELF; point `lwsym.py` at that one for these records.

QEMU only. Not tested: the DCRS=0 and `FPCCR.TS` paths (QEMU did not produce them in this demo; the code follows the Armv8-M ARM), Non-secure handlers with `AIRCR.BFHFNMINS=1`, and any real M33/M55 silicon.

## Rust

[rust/](rust) is a binding, not a rewrite. `build.rs` compiles `src/lastwords.c` with the C compiler that matches the Rust target (`arm-none-eabi-gcc` for `thumbv6m`/`v7m`/`v7em`/`v8m`, `riscv-none-elf-gcc` for `riscv32*`, `cc` on the host) and links it as a static library. No dependencies, not even the `cc` crate. On bare metal it builds with `LW_NO_PRINTF`; formatting is done in Rust.

```rust
use lastwords::{log, Level, Start};

if lastwords::init(rcc_csr) == Start::Crashed {
    lastwords::export(|chunk| uart.write(chunk));   // or lastwords::print(|line| ...)
}
lastwords::set_clock(Some(millis));
log!(Level::Warn, "rail {} mV, retry {}", mv, n);   // 255-byte line on the stack
if free_blocks <= 4 {
    lastwords::fatal!("pool_take: free_blocks <= 4"); // PANIC record with this pc, then reset
}
```

The fault handlers come from the C object (`HardFault_Handler` and friends), so put them in your vector table, as [rust/qemu](rust/qemu/src/main.rs) does. `cortex-m-rt` owns the vector table and its own `HardFault` symbol; wiring lastwords into it is not done or tested yet. `LASTWORDS_RING_SIZE` sets the ring size, `LASTWORDS_CFLAGS` adds defines and `LASTWORDS_CC` picks the compiler.

`fatal!` is a macro on purpose. The C side records its own return address; from an `#[inline(always)]` function that address is right, but addr2line (without `-i`) then names the inlined helper instead of your function. Rust's own `panic!` is not recorded: a panic handler has the file and line but no address to sign the crash with, so it would give every panic the same signature.

How it is checked:

- `make rust`: [rust/tests/host.rs](rust/tests/host.rs) runs a scenario through the Rust API (two boots, a fault, a repeat, a 300-byte line cut to 255, a wrapped ring) and [rust/tests/twin.c](rust/tests/twin.c) runs the same one through the C API. The 659-byte exports must be identical byte for byte, the `lw_print` text too, and `tools/lwdecode.py` must turn the Rust export into the Rust-side `print` text. Changing the twin's loop from 40 to 39 iterations makes it fail, so the comparison is live.
- `make rust-qemu`: [rust/qemu](rust/qemu) is the QEMU demo's story with no runtime crates (own vector table and reset handler) on `mps2-an385`. Five boots: a store to unmapped memory (PRECISERR, `bfar=0x30000000`), `fatal!`, a jump without the Thumb bit (INVSTATE), the first bug again (repeat record), then print and export. `tests/check_rust_qemu.py` wants each crash pinned to the right Rust function by `lwsym.py`; `tests/check_export.py` wants the 820-byte export to decode to what the device printed.
- CI builds the crate for `thumbv6m-none-eabi` and `thumbv7m-none-eabi` and runs both checks.

```text
[       2] #    5 !! HardFault PRECISERR FORCED
   pc -> lastwords_qemu_demo::config_apply  rust/qemu/src/main.rs:109
   lr -> lastwords_qemu_demo::main  rust/qemu/src/main.rs:147
[       2] #   11 !! panic pc=0x000003d3: pool_take: free_blocks <= 4
   called from lastwords_qemu_demo::pool_take  rust/qemu/src/main.rs:115
[       2] #   23 !! repeat sig=0xfbdcb044 count=1
   same crash as #5: lastwords_qemu_demo::config_apply  rust/qemu/src/main.rs:109
```

The demo image is 16.2 KB of `.text`, about 6 KB of it `core::fmt` (the demo prints `Debug` names and the whole ring); the C part is the same object the C numbers above measure.

Not done yet: the crate is not on crates.io (it reads `../src`; a `csrc/` copy is picked up first when present), no `cortex-m-rt` example, no Embassy/RTIC lock integration (on Cortex-M the C side masks interrupts around each write), RISC-V and ARMv8-M Rust targets compile-checked only through the C side. The browser symbolizer does not demangle Rust names yet, so use `lwsym.py` for Rust ELFs.

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
make riscv        # needs riscv-none-elf-gcc and qemu-system-riscv32 (xPack builds in CI)
make armv8m       # TrustZone Cortex-M55 on qemu-system-arm mps3-an547
make footprint    # flash cost on Cortex-M3
make rust         # Rust binding vs the C API, byte for byte (needs cargo)
make rust-qemu    # Rust demo on mps2-an385 (thumbv7m-none-eabi target, arm-none-eabi-gcc, qemu-system-arm)
node tests/check_web.mjs examples/*/build/demo-*.elf   # browser symbolizer vs addr2line and the Python tools
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
- RISC-V: RV32 machine mode only, no RV64, no S-mode/U-mode stacks, no PMP awareness. Tested on QEMU `virt` only. The trap stack is 512 bytes; the deepest path measured at `-Os` is about 370 bytes, so `-O0` builds may need `LW_TRAP_STACK=1024`. `mscratch` is overwritten on the way to a reset; if your RTOS keeps something there, call `lw_trap_capture()` from its handler instead.
- TrustZone: the Secure side records everything; a Non-secure-only build cannot see SFSR or Secure state. Non-secure addresses in a Secure record are not symbolized against the Secure ELF.
- Browser symbolizer: ELF32 little-endian and 32-bit DWARF only, no split DWARF (`.dwo`), no compressed debug sections, no C++ demangling. Checked only against GCC-built QEMU demo ELFs; Clang or IAR output may give different names or lines than addr2line.
- Rust: a binding over the C file, tested on the host and in QEMU on a Cortex-M3 only. Rust `panic!` is not recorded (no address to sign it with); use `fatal!`. The browser page shows mangled Rust names.
- Records are capped at 255 bytes; `lw_logf` truncates.
- 16-bit sequence numbers and CRC, see the numbers table for what that costs.
- Two different bugs with the same exception, fault bits, pc and lr share a signature. Signatures are per build: rebuild and the addresses move, which is what `--elf` grouping is for.
- Repeat detection walks the ring inside the fault handler, the same CRC-checked walk `lw_init()` does at boot, so the handler takes longer on a big, full ring. `LW_NO_DEDUP` turns it off.
- `lw_export` does not lock the ring while it streams. Lines logged from interrupts meanwhile can overwrite the oldest records being sent; the host then reports a CRC mismatch, never wrong data.

## FAQ

**Why not write the crash to flash right away?** Erasing and programming flash from a fault handler means running the flash driver with a broken system state, often with interrupts and clocks in an unknown state, and it takes milliseconds to seconds. RAM survives a warm reset anyway. Copy the region to flash later, from normal code, if you need it to survive power loss: `lw_region()` gives you the bytes.

**Does it work with FreeRTOS / Zephyr / bare metal?** Bare metal is what the demo runs. FreeRTOS should work, since tasks run on PSP and the entry stub picks the right stack, but I have not tested it under FreeRTOS yet. Zephyr has its own fault handling and coredump; use the logging part with `LW_NO_FAULT_HANDLERS` if you want the ring there.

**RISC-V, ESP32?** RV32 machine-mode traps are captured since 0.3, see [RISC-V](#risc-v); QEMU only so far. The ring, `lw_log` and `lw_print` are plain C and run anywhere (the host tests run them on x86-64). On ESP-IDF (Xtensa or RISC-V) the ring works, but IDF owns the trap vector, so getting the crash into the ring would mean hooking its panic handler, which this repo does not do.

**Can I parse the ring on the host instead of printing on the device?** Yes: send `lw_export()` output and run `tools/lwdecode.py` on it, see [Shipping the ring](#shipping-the-ring).

## License

MIT, see [LICENSE](LICENSE).
