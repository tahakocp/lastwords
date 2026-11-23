# RISC-V demo

The Cortex-M demo's six boots on QEMU `virt`, rv32imac, machine mode, `-bios none`:

| boot | what goes wrong |
|---|---|
| 0 | store through a pointer to unmapped memory (`config_apply`): store access fault, `mtval` = the address |
| 1 | atomic add on a counter one byte into a frame buffer (`count_rx`): misaligned AMO |
| 2 | failed assert, `lw_panic` |
| 3 | call through a function pointer to unmapped memory: instruction access fault, `ra` points at the caller |
| 4 | the boot 0 bug again: stored as a repeat record |
| 5 | prints the ring and the `lw_export()` stream as hex, then exits |

Output goes through semihosting. Reset and exit go through the SiFive test device at `0x100000` (`0x7777` resets, `0x5555` exits).

```sh
make run          # build, run, symbolize, check
make footprint    # flash and RAM cost on rv32imac
make count        # instructions per lw_log(), from QEMU's execution trace
```

Two things in [link.ld](link.ld) that matter on QEMU and may matter on your board: `.noinit` sits in no program header (`:NONE`), because QEMU zero-fills the rest of every loadable segment on reset; and `__StackTop` bounds the stack copy in the trap handler.

QEMU 9.2 reports the misaligned `amoadd.w` of boot 1 as load misaligned (cause 4). The privileged spec says store/AMO misaligned (6) or an access fault, so expect a different cause on silicon. Nothing here has run on real RISC-V hardware yet.
