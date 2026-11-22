#!/usr/bin/env python3
"""Check the symbolized QEMU transcripts: every crash recovered and pinned to the right line."""
import re
import sys

EXPECT = [
    r"^boot 0: lw_init says cold$",
    r"^boot 5: lw_init says crashed$",
    r"#    5 !! HardFault",
    r"pc -> config_apply  main\.c:\d+",
    r"#   11 !! HardFault",
    r"pc -> (average|read_header)  main\.c:\d+",
    r"#   17 !! panic pc=0x[0-9a-f]{8}: assert free_blocks > 4",
    r"called from main  main\.c:\d+",
    r"#   23 !! HardFault",
    r"pc -> bad jump target",
    r"#   29 !! repeat sig=0x[0-9a-f]{8} count=1",
    r"same crash as #5: config_apply  main\.c:\d+",
    r"#   33 I boot 5, sensor sample 34",
    r"^---- end ----$",
]
EXPECT_V7 = [r"HardFault PRECISERR FORCED", r"bfar=0x30000000", r"HardFault DIVBYZERO FORCED",
             r"HardFault INVSTATE FORCED"]

failed = 0
for path in sys.argv[1:]:
    text = open(path).read()
    want = EXPECT + ([] if "demo-m0" in path or "run-m0" in path else EXPECT_V7)
    missing = [p for p in want if not re.search(p, text, re.M)]
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
