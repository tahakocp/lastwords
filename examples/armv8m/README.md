# TrustZone demo (Cortex-M55, QEMU mps3-an547)

A Secure image that also runs a few instructions in Non-secure state. Six boots in one QEMU run:

| boot | what goes wrong |
|---|---|
| 0 | Secure code calls a Non-secure address with a plain `BLX`: SecureFault, `SFSR.INVTRAN` |
| 1 | `BLXNS` into Non-secure code that hits `udf`: Secure HardFault, frame taken from `MSP_NS`, `UNDEFINSTR` from the Non-secure CFSR bank |
| 2 | Non-secure code switches to its process stack and hits `udf`: frame taken from `PSP_NS` |
| 3 | Secure store to unmapped memory: BusFault `PRECISERR`, `bfar=0x0f000000` |
| 4 | the boot 0 bug again, stored as a repeat record |
| 5 | prints the ring and the `lw_export()` stream, exits |

```sh
make run      # needs arm-none-eabi-gcc (cortex-m55, -mcmse) and qemu-system-arm with mps3-an547
```

`main.c` sets two SAU regions: `0x00000000-0x0003ffff` (Non-secure alias of the ITCM the image runs from) and `0x20008000-0x2000ffff` (Non-secure alias of the upper DTCM, used as the Non-secure main stack). The Non-secure code is `ns_driver_poll` and `ns_task_run` from the same image, called through its Non-secure alias, so records show `pc=0x000000a0` for what the Secure ELF calls `0x100000a0`. `sp=0x2000c000` in the boot 2 record is the process stack `ns_task_run` set for itself.

The run is checked by `tests/check_export.py` (export decodes to the same text the device printed) and `tests/check_armv8m.py` (every fault recognised, four distinct signatures plus the repeat).

Simulation only. QEMU models the SAU, the banked fault status registers and the EXC_RETURN bits well enough for this, but it is not silicon: which faults are precise, what the boot ROM does with the TCMs on reset and how a vendor's IDAU splits memory can differ, and nothing here has run on a real M33 or M55.
