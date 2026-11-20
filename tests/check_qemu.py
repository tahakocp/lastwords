#!/usr/bin/env python3
"""Check the symbolized QEMU transcripts: every crash recovered and pinned to the right line."""
import re
import sys

EXPECT = [
    r"^boot 0: lw_init says cold$",
    r"^boot 4: lw_init says crashed$",
    r"#    5 !! HardFault",
    r"pc -> config_apply  main\.c:\d+",
    r"#   11 !! HardFault",
    r"pc -> (average|read_header)  main\.c:\d+",
    r"#   17 !! panic pc=0x[0-9a-f]{8}: assert free_blocks > 4",
    r"called from main  main\.c:\d+",
    r"#   23 !! HardFault",
    r"pc -> bad jump target",
    r"#   27 I boot 4, sensor sample 34",
    r"^---- end ----$",
]
EXPECT_V7 = [r"HardFault PRECISERR FORCED", r"bfar=0x30000000", r"HardFault DIVBYZERO FORCED",
             r"HardFault INVSTATE FORCED"]

failed = 0
for path in sys.argv[1:]:
    text = open(path).read()
    want = EXPECT + ([] if "demo-m0" in path or "run-m0" in path else EXPECT_V7)
    missing = [p for p in want if not re.search(p, text, re.M)]
    crashed = len(re.findall(r"^boot [1-4]: lw_init says crashed$", text, re.M))
    if crashed != 4:
        missing.append(f"4 crashed boots (got {crashed})")
    print(f"{path}: {'ok' if not missing else 'FAIL'}")
    for m in missing:
        print(f"  missing: {m}")
    failed += bool(missing)
sys.exit(1 if failed or not sys.argv[1:] else 0)
