# Contributing

Bug reports from real hardware are the most useful thing right now. The library has only run in QEMU and on the host, so "works on an STM32G0 with the stock CubeIDE script" or "the RP2040 bootrom clobbers the first 4 KB of SRAM" is exactly what I want to hear.

When something goes wrong, please include:

- core and part number, toolchain and version
- the relevant part of your linker script (where `.noinit` ends up)
- the `lw_print` output and, if you can, the `lwsym.py` output with the ELF that produced it

## Changing the code

```sh
make test test-asan      # host, 3 ring sizes, ASan/UBSan
make qemu                # needs arm-none-eabi-gcc and qemu-system-arm
```

Both run in CI. A change to the record format or the write order needs a host test that cuts the new write sequence at every store; `test_torn_writes` shows how.

Keep the library in two files, C99, no heap, no dependency beyond `string.h` (and `stdio.h` for `lw_logf`). New platform support (RISC-V trap capture, for example) is welcome behind its own `#if`, with a QEMU run in CI if QEMU can model it.
