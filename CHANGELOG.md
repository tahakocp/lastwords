# Changelog

## 0.4.0 (2026-11-23)

- TrustZone-aware fault capture on ARMv8-M. `lw_fault_entry` takes the frame from `MSP_NS`/`PSP_NS` when a Secure handler catches a Non-secure fault (EXC_RETURN ES=1, S=0), and from `MSP`/`PSP` otherwise. Same Thumb-1 entry on M23.
- Extra state context (DCRS=0) skipped after checking the integrity signature; s16-s31 accounted for when `FPCCR.TS` is set and Secure code was interrupted.
- Secure handler with non-zero SFSR: SFSR and SFAR are appended to the FAULT record after the stack words. `lw_print` and `lwdecode.py` name the SFSR bits and print `sfsr=`/`sfar=`; records without them are byte-for-byte as in 0.3. Older decoders ignore the two extra words.
- Non-secure fault escalated to the Secure HardFault: the Non-secure CFSR bank is merged in, so the record says `UNDEFINSTR` etc. instead of a bare `FORCED`. Printed as `in non-secure code`.
- `examples/armv8m`: Secure image for QEMU `mps3-an547` (Cortex-M55), five boots: SecureFault INVTRAN, Non-secure UNDEFINSTR caught on `MSP_NS`, Secure BusFault, repeat, export check. New CI job.
- Host tests: an ARMv8-M scenario and Secure-style records in the random rings; 54 exports decode byte-identical to `lw_print`, 10,800/10,800 single-bit flips rejected.
- Flash (init + log + fault, `-Os`, `LW_NO_PRINTF`, GCC 15.2): M33 1,888 (was 1,748), M55 1,904 (was 1,760), M3 1,764 (was 1,752).
- QEMU only. The DCRS=0 and `FPCCR.TS` paths and faults on `PSP_NS` were not exercised by the demo.

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
