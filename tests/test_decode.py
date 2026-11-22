#!/usr/bin/env python3
"""tools/lwdecode.py against lw_print: the exports written by test_crash must
decode to exactly the text the C printer produced, and damage must be caught."""
import glob
import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import lwdecode  # noqa: E402

build = sys.argv[1] if len(sys.argv) > 1 else "build"
bins = sorted(glob.glob(os.path.join(build, "export-*.bin")))
if not bins:
    sys.exit("no exports; run test_crash first")

failed = flips = caught = 0
rng = random.Random(7)
for path in bins:
    blob = open(path, "rb").read()
    want = open(path[:-4] + ".txt", "rb").read()
    _, entries = lwdecode.decode(blob)
    got = lwdecode.render(entries)
    if got != want:
        failed += 1
        print(f"{path}: decoded text differs from lw_print")
    for _ in range(200):
        bad = bytearray(blob)
        i = rng.randrange(len(bad))
        bad[i] ^= 1 << rng.randrange(8)
        flips += 1
        try:
            lwdecode.decode(bytes(bad))
        except lwdecode.DecodeError:
            caught += 1
# --crashes on the demo-shaped ring: three distinct faults and one panic.
import io  # noqa: E402
buf = io.StringIO()
lwdecode.crashes([os.path.join(build, "export-demo.bin")], None, None, buf)
rows = buf.getvalue().splitlines()[1:]
if len(rows) != 4 or not any("panic assert free_blocks > 4" in r for r in rows):
    failed += 1
    print("--crashes: unexpected grouping:\n" + buf.getvalue())

print(f"decoder: {len(bins)} exports match lw_print byte for byte" if not failed else f"decoder: {failed} mismatches")
print(f"decoder: {caught} of {flips} single-bit flips rejected")
sys.exit(1 if failed or caught != flips else 0)
