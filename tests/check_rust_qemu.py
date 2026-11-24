#!/usr/bin/env python3
"""Check the symbolized transcript of the Rust QEMU demo (rust/qemu)."""
import re
import sys

EXPECT = [
    r"^boot 0: lastwords::init says Cold$",
    r"^boot 4: lastwords::init says Crashed$",
    r"#    5 !! HardFault PRECISERR FORCED",
    r"bfar=0x30000000",
    r"pc -> lastwords_qemu_demo::config_apply  \S*main\.rs:\d+",
    r"#   11 !! panic pc=0x[0-9a-f]{8}: pool_take: free_blocks <= 4",
    r"called from lastwords_qemu_demo::pool_take  \S*main\.rs:\d+",
    r"#   17 !! HardFault INVSTATE FORCED",
    r"lr -> lastwords_qemu_demo::main  \S*main\.rs:\d+",
    r"#   23 !! repeat sig=0x[0-9a-f]{8} count=1",
    r"same crash as #5: lastwords_qemu_demo::config_apply",
    r"#   27 I boot 4, sensor sample 34",
    r"^---- end ----$",
]

text = open(sys.argv[1]).read()
missing = [p for p in EXPECT if not re.search(p, text, re.M)]
crashed = len(re.findall(r"^boot [1-4]: lastwords::init says Crashed$", text, re.M))
if crashed != 4:
    missing.append(f"4 crashed boots (got {crashed})")
sigs = re.findall(r"^   sig=(0x[0-9a-f]{8})$", text, re.M)
if len(set(sigs)) != 3 or not re.search(r"!! repeat sig=" + (sigs[0] if sigs else "x"), text):
    missing.append("3 distinct signatures, and the repeat carrying the first one")
print(f"{sys.argv[1]}: {'ok' if not missing else 'FAIL'}")
for m in missing:
    print(f"  missing: {m}")
sys.exit(1 if missing else 0)
