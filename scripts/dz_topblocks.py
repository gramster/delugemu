#!/usr/bin/env python3
"""Map the hottest hotblocks (by icount*ecount) to source lines via addr2line."""
import subprocess
import sys

report = sys.argv[1]
elf = sys.argv[2]
topn = int(sys.argv[3]) if len(sys.argv) > 3 else 12
a2l = "arm-none-eabi-addr2line"

rows = []
with open(report) as f:
    for line in f:
        line = line.strip()
        if not line.startswith("0x"):
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 4:
            continue
        pc = parts[0]
        ic = int(parts[2])
        ec = int(parts[3])
        rows.append((ic * ec, pc, ic, ec))

rows.sort(reverse=True)
total = sum(r[0] for r in rows) or 1
for weight, pc, ic, ec in rows[:topn]:
    loc = subprocess.run(
        [a2l, "-e", elf, "-f", pc], capture_output=True, text=True
    ).stdout.replace("\n", " ").strip()
    print(f"{100*weight/total:6.2f}%  pc={pc} ic={ic:>3} ec={ec:>12,}  {loc}")
