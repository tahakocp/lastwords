#!/usr/bin/env python3
"""The hex export a QEMU run printed must decode to the same text lw_print
printed on the device in the same run."""
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import lwdecode  # noqa: E402

path = sys.argv[1]
text = open(path, encoding="utf-8", errors="replace").read()
printed = re.search(r"---- recovered log ----\n(.*?)---- end ----\n", text, re.S).group(1)
blob = lwdecode.load(path)[0]
_, entries = lwdecode.decode(blob)
decoded = lwdecode.render(entries).decode("utf-8", "replace")
used = sum(len(e["data"]) + 11 for e in entries)
ok = decoded == printed
print(f"{path}: export {len(blob)} bytes for {len(entries)} records "
      f"({used} bytes in the ring): {'matches lw_print' if ok else 'DIFFERS from lw_print'}")
sys.exit(0 if ok else 1)
