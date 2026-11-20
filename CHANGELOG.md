# Changelog

## 0.1.0 (2026-11-20)

First release.

- Ring buffer in `.noinit` with CRC-16 records, recovered after warm resets, cut-off writes discarded.
- Cortex-M fault capture (M0/M0+/M3/M4/M7/M33): stacked frame, CFSR/HFSR/MMFAR/BFAR, 16 stack words, then reset.
- `lw_panic` for asserts, `lw_print` without printf, `lwsym.py` host symbolizer.
- Host tests at three ring sizes, ASan/UBSan, QEMU demo on four cores in CI.
