#!/usr/bin/env python3
"""Check the symbolized AN547 (Cortex-M55, TrustZone) transcript."""
import re
import sys

EXPECT = [
    r"^boot 0: lw_init says cold$",
    r"^boot 4: lw_init says crashed$",
    r"#    3 !! SecureFault INVTRAN$",
    r"sfsr=0x00000010",
    r"lr -> main  main\.c:\d+",
    r"#    7 !! HardFault UNDEFINSTR FORCED in non-secure code",
    r"lr=0xfeffffff sp=0x20010000",
    r"#   11 !! BusFault PRECISERR",
    r"bfar=0x0f000000",
    r"pc -> config_apply  main\.c:\d+",
    r"#   15 !! repeat sig=0x[0-9a-f]{8} count=1",
    r"#   17 I boot 4, secure side up",
    r"^---- end ----$",
]

text = open(sys.argv[1]).read()
missing = [p for p in EXPECT if not re.search(p, text, re.M)]
crashed = len(re.findall(r"^boot [1-4]: lw_init says crashed$", text, re.M))
if crashed != 4:
    missing.append(f"4 crashed boots (got {crashed})")
sigs = re.findall(r"^   sig=(0x[0-9a-f]{8})$", text, re.M)
if len(set(sigs)) != 3 or not re.search(r"!! repeat sig=" + (sigs[0] if sigs else "x"), text):
    missing.append("3 distinct signatures, and the repeat carrying the first one")
print(f"{sys.argv[1]}: {'ok' if not missing else 'FAIL'}")
for m in missing:
    print(f"  missing: {m}")
sys.exit(1 if missing else 0)
