#!/usr/bin/env python3
"""Bucket a libhotblocks report by firmware symbol → ranked flat profile.

Usage: dz_bucket.py <hotblocks_report.txt> <deluge.elf> [top_n]

hotblocks lines look like:  "0x20034abc, 3, 12, 4567"  (pc, tcount, icount, ecount)
Weight = icount * ecount  (total guest instructions executed in that block).
We map each block start PC to the enclosing symbol from `arm-none-eabi-nm` and
sum weights per symbol, printing a gprof-style flat profile.
"""
import bisect
import re
import subprocess
import sys


def load_symbols(elf):
    out = subprocess.check_output(
        ["arm-none-eabi-nm", "-n", "--defined-only", elf],
        text=True, errors="replace")
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) < 3:
            continue
        addr, typ, name = parts
        if typ.lower() not in "tw":  # text / weak text (code)
            continue
        try:
            a = int(addr, 16)
        except ValueError:
            continue
        addrs.append(a)
        names.append(name)
    return addrs, names


def demangle(names):
    try:
        out = subprocess.check_output(
            ["arm-none-eabi-c++filt"], input="\n".join(names),
            text=True, errors="replace")
        return out.splitlines()
    except Exception:
        return names


def main():
    report, elf = sys.argv[1], sys.argv[2]
    top_n = int(sys.argv[3]) if len(sys.argv) > 3 else 40

    addrs, names = load_symbols(elf)
    dnames = demangle(names)

    sym_weight = {}
    total = 0
    line_re = re.compile(r"^\s*(0x[0-9a-fA-F]+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)")
    with open(report) as fh:
        for line in fh:
            m = line_re.match(line)
            if not m:
                continue
            pc = int(m.group(1), 16)
            icount = int(m.group(3))
            ecount = int(m.group(4))
            weight = icount * ecount
            total += weight
            i = bisect.bisect_right(addrs, pc) - 1
            if i < 0:
                sym = "<below-first-symbol>"
            else:
                sym = dnames[i]
            sym_weight[sym] = sym_weight.get(sym, 0) + weight

    if total == 0:
        print("no hotblocks rows parsed", file=sys.stderr)
        sys.exit(1)

    ranked = sorted(sym_weight.items(), key=lambda kv: kv[1], reverse=True)
    print(f"total guest instructions (in counted blocks): {total:,}\n")
    print(f"{'%':>7}  {'cum%':>7}  {'instr':>16}  symbol")
    cum = 0.0
    for sym, w in ranked[:top_n]:
        pct = 100.0 * w / total
        cum += pct
        s = sym if len(sym) <= 90 else sym[:87] + "..."
        print(f"{pct:7.2f}  {cum:7.2f}  {w:16,}  {s}")


if __name__ == "__main__":
    main()
