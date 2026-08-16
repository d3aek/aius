#!/usr/bin/env python3
"""Merge disjoint Intel HEX images into one file (chooser + slot A → the
HalfKay recovery image, aius-ota-resilience.md Phase 3).

  merge_hex.py OUTPUT INPUT [INPUT...]

Data and extended-address records are concatenated in argument order; each
input's EOF record is dropped and a single EOF is emitted. The inputs must
not overlap — this tool verifies byte-range disjointness and fails loudly,
because an overlap in a recovery image is a corrupted board waiting to
happen.
"""
from __future__ import annotations

import sys
from pathlib import Path


def parse_ranges(lines):
    """Yield (start, end) absolute byte ranges the hex's data records cover."""
    base = 0
    for line in lines:
        line = line.strip()
        if not line.startswith(":"):
            continue
        count = int(line[1:3], 16)
        address = int(line[3:7], 16)
        kind = int(line[7:9], 16)
        if kind == 0x04:
            base = int(line[9:13], 16) << 16
        elif kind == 0x00:
            start = base + address
            yield (start, start + count)


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    output = Path(sys.argv[1])
    inputs = [Path(p) for p in sys.argv[2:]]

    covered = []
    merged = []
    for source in inputs:
        lines = source.read_text(encoding="ascii").splitlines()
        for start, end in parse_ranges(lines):
            for other_start, other_end in covered:
                if start < other_end and other_start < end:
                    print(f"merge_hex: {source} overlaps "
                          f"[0x{start:08X},0x{end:08X}) with an earlier "
                          "input", file=sys.stderr)
                    return 1
            covered.append((start, end))
        for line in lines:
            line = line.strip()
            if not line.startswith(":"):
                continue
            if int(line[7:9], 16) == 0x01:  # EOF: emit one at the end
                continue
            merged.append(line)
    merged.append(":00000001FF")
    output.write_text("\n".join(merged) + "\n", encoding="ascii")
    total = sum(end - start for start, end in covered)
    print(f"merge_hex: {output} <- {len(inputs)} images, {total} data bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
