# Changelog

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
