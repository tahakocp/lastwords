# QEMU demo

One firmware image, six boots, run on four emulated cores:

| boot | what goes wrong |
|---|---|
| 0 | store through a pointer to unmapped memory (`config_apply`) |
| 1 | divide by zero with `CCR.DIV_0_TRP` set (`average`); on M0, an unaligned `LDM` (`read_header`) |
| 2 | failed assert, `lw_panic` |
| 3 | call through a function pointer with the Thumb bit clear |
| 4 | the boot 0 bug again: stored as a 17-byte repeat record that points at the boot 0 record |
| 5 | nothing: prints the whole ring, then the `lw_export()` stream as hex, and exits |

The boot number comes from lastwords' own boot counter, which is the first thing the demo proves: it survived five resets.

```sh
make run-m3                       # one core
make run-all                      # all four, then tests/check_qemu.py
```

Each run also feeds its hex export to `tests/check_export.py`, which decodes it with `tools/lwdecode.py` and requires the result to match the `lw_print()` text from the same run byte for byte.

Output goes through semihosting into `build/run-<core>.log`; `build/run-<core>.txt` is the same after `lwsym.py`. QEMU does not clear RAM on a system reset and does not reload `NOLOAD` sections, which is what a real Cortex-M does on a warm reset, so the demo exercises the real mechanism. It does not model RAM decay, bootloaders, or ECC.

Machines: `microbit` (Cortex-M0), `mps2-an385` (M3), `mps2-an386` (M4), `mps2-an500` (M7). The linker script uses 256 KB at 0 and 16 KB at `0x20000000`, which all four have.
