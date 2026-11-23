# Changelog

## 0.3.0 (2026-11-23)

- RISC-V RV32 machine-mode trap capture: `lw_trap_entry` (for `mtvec`), `lw_trap_install()`, `lw_trap_capture()` for ports with their own handler. Runs on its own 512-byte stack (`LW_TRAP_STACK`).
- New record type `LW_T_TRAP` (6): `mcause`, `mepc`, `mtval`, `mstatus`, `ra`, `sp`, `a0`-`a3`, up to 16 stack words. Printed by `lw_print`, signed by `lw_signature` (mcause, mepc, ra), deduplicated into repeat records, word-packed by `lw_export`.
- `lwdecode.py` decodes TRAP records byte-identically to `lw_print`. Older decoders do not know type 6 and will reject or misread exports that contain one.
- `lwsym.py` reads RV32 ELFs: mepc/ra lookup, riscv addr2line, return-address check against `jal`/`jalr`/`c.jal`/`c.jalr`.
- `examples/riscv`: QEMU `virt` demo, six boots, four different traps and a repeat, semihosting output, SiFive test device for reset. New CI job with xPack riscv-none-elf-gcc 14.2 and qemu-system-riscv32 9.2.
- rv32imac: 2,128 bytes of flash for init + log + trap capture, about 1,560 instructions per `lw_log()`.
- Cortex-M builds do not compile the trap code, printer or trap signature: +12 bytes over 0.2.0 in the M3 footprint (1,752 with GCC 15.2).
- QEMU only, no RISC-V silicon tested.

## 0.2.0 (2026-11-22)

- Crash signatures: `lw_signature()`, FNV-1a over exception, fault bits, pc and lr. Printed as `sig=` under every fault and panic.
- Repeat records (`LW_T_REPEAT`): a crash whose full record is still in the ring is stored as 17 bytes instead of a second dump, so crash loops keep the log from before the first crash (11 boots instead of 4 on a 1 KB ring in the host test). `LW_NO_DEDUP` restores the old behaviour.
- `lw_export()`: compact, CRC-protected stream of the ring for upload, no buffer needed. 25-30% smaller than the raw records in the QEMU demo.
- `tools/lwdecode.py`: decodes exports to the exact `lw_print()` text; `--crashes` groups crashes across files by signature, or by function names with `--elf`.
- `lwsym.py` points repeat records back at the full record.
- QEMU demo: six boots, the fifth repeats the first crash; the export is decoded and compared with the device's own output on M0/M3/M4/M7.
- Verified in QEMU and host tests only, as before.

## 0.1.0 (2026-11-20)

First release.

- Ring buffer in `.noinit` with CRC-16 records, recovered after warm resets, cut-off writes discarded.
- Cortex-M fault capture: stacked frame, CFSR/HFSR/MMFAR/BFAR, 16 stack words, then reset. Run in QEMU on M0/M3/M4/M7; compile-checked for M0+/M23/M33/M55.
- `lw_panic` for asserts, `lw_print` without printf, `lwsym.py` host symbolizer.
- Host tests at three ring sizes, ASan/UBSan, QEMU demo on four cores in CI.
