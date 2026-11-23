#!/usr/bin/env python3
"""Check the symbolized RISC-V QEMU transcript: every trap recovered and pinned to the right line."""
import re
import sys

EXPECT = [
    r"^boot 0: lw_init says cold$",
    r"^boot 5: lw_init says crashed$",
    r"#    5 !! trap store access fault",
    r"mtval=0x00200000",
    r"mepc -> config_apply  main\.c:\d+",
    r"#   11 !! trap (load|store) misaligned",
    r"mepc -> count_rx  main\.c:\d+",
    r"#   17 !! panic pc=0x[0-9a-f]{8}: assert free_blocks > 4",
    r"called from main  main\.c:\d+",
    r"#   23 !! trap instruction access fault",
    r"mepc -> bad jump target",
    r"ra -> main  main\.c:\d+",
    r"#   29 !! repeat sig=0x[0-9a-f]{8} count=1",
    r"same crash as #5: config_apply  main\.c:\d+",
    r"0x[0-9a-f]{8}  _start  start\.S:\d+",
    r"#   33 I boot 5, sensor sample 34",
    r"^---- end ----$",
]

failed = 0
for path in sys.argv[1:]:
    text = open(path).read()
    missing = [p for p in EXPECT if not re.search(p, text, re.M)]
    crashed = len(re.findall(r"^boot [1-5]: lw_init says crashed$", text, re.M))
    if crashed != 5:
        missing.append(f"5 crashed boots (got {crashed})")
    sigs = re.findall(r"^   sig=(0x[0-9a-f]{8})$", text, re.M)
    if len(set(sigs)) != 4 or not re.search(r"!! repeat sig=" + (sigs[0] if sigs else "x"), text):
        missing.append("4 distinct signatures, and the repeat carrying the first one")
    print(f"{path}: {'ok' if not missing else 'FAIL'}")
    for m in missing:
        print(f"  missing: {m}")
    failed += bool(missing)
sys.exit(1 if failed or not sys.argv[1:] else 0)
