#!/usr/bin/env python3
"""stack_calc.py: worst-case stack per codec2-vhf entry point.

Parses GCC -fstack-usage .su files and reconstructs the call graph from
`arm-none-eabi-objdump -dr`, then reports the deepest call-path frame
sum for each public entry point.  External (newlib/libgcc) callees get
a nominal 128-byte allowance.
"""

import re
import subprocess
import sys
from pathlib import Path

build = Path(sys.argv[1] if len(sys.argv) > 1 else "build-size")

# frame sizes: object.su lines "file:line:col:func\tsize\tqualifier"
frames = {}
for su in build.glob("*.su"):
    for line in su.read_text().splitlines():
        m = re.match(r".*:\d+:\d+:(\S+)\t(\d+)\t", line)
        if m:
            name, size = m.group(1), int(m.group(2))
            frames[name] = max(frames.get(name, 0), size)

# call graph: track the current function from objdump symbol lines,
# then record every direct "bl/bl.w <target>" edge
calls = {}
func_re = re.compile(r"^[0-9a-f]+ <([^>+]+)>:")
bl_re = re.compile(r"\bb(?:l|lx\.w|\.w)\s+[0-9a-f]+ <([^>+]+)>")

for obj in build.glob("*.o"):
    out = subprocess.run(["arm-none-eabi-objdump", "-dr", str(obj)],
                         capture_output=True, text=True).stdout
    cur = None
    for line in out.splitlines():
        if not line.startswith((" ", "\t")):
            m = func_re.match(line)
            cur = m.group(1) if m else None
            continue
        if cur:
            m = bl_re.search(line)
            if m:
                calls.setdefault(cur, set()).add(m.group(1))

ENTRIES = ["codec2_vhf_encoder_init", "codec2_vhf_decoder_init",
           "codec2_vhf_encode_3200", "codec2_vhf_decode_3200",
           "codec2_vhf_encode_1600", "codec2_vhf_decode_1600",
           "codec2_vhf_encode_1300", "codec2_vhf_decode_1300"]

EXTERNAL = 128  # nominal allowance for libm/libc leaf callees


def worst(entry, seen=frozenset()):
    here = frames.get(entry, 0)
    kids = [k for k in calls.get(entry, ())
            if k in frames and k not in seen]
    best = (0, [])
    for k in kids:
        s, path = worst(k, seen | {entry})
        if s > best[0]:
            best = (s, [k] + path)
    ext = 0
    if any(k not in frames for k in calls.get(entry, ())):
        ext = EXTERNAL
    if best[0] >= ext:
        return here + best[0], [entry] + best[1]
    return here + ext, [entry, "<lib>"]


for e in ENTRIES:
    if e not in frames:
        print(f"{e:28s} not found in build")
        continue
    total, path = worst(e)
    print(f"{e:28s} {total:6d} B")
    print(f"{'':28s} via {' -> '.join(path[:8])}"
          + (" ..." if len(path) > 8 else ""))
