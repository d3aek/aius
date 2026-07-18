#!/usr/bin/env python3
"""Generate the compiled-in DTS Core access unit header for the capability firmware.

Extracts the first DTS Core access unit (bounded by consecutive 0x7FFE8001 sync
words) from a .dts vector and emits it as a C `static const uint8_t[]` header. The
firmware embeds this frame so the linker pulls in the entire real decode pipeline,
making the per-bank memory report reflect the true footprint.

Usage:
    embed_frame.py <input.dts> <output.h>

Default (no args): regenerate src/dts_frame_surround51.h from
tools/dts_vectors/vectors/surround51_tones.dts.
"""
import sys
from pathlib import Path

SYNC = b"\x7f\xfe\x80\x01"


def first_frame(data: bytes) -> bytes:
    offsets = []
    i = 0
    while i + 4 <= len(data):
        if data[i:i + 4] == SYNC:
            offsets.append(i)
            i += 4
        else:
            i += 1
    if not offsets:
        raise SystemExit("no DTS Core sync word (0x7FFE8001) found")
    start = offsets[0]
    end = offsets[1] if len(offsets) > 1 else len(data)
    return data[start:end]


def emit(frame: bytes) -> str:
    lines = [
        "/* Generated embedded DTS Core access unit — do not hand-edit.",
        " *",
        " * One 5.1 DTS Core frame (the first access unit of",
        " * tools/dts_vectors/vectors/surround51_tones.dts), embedded so the",
        " * capability firmware forces the linker to pull in the entire real",
        " * decode pipeline. 16-bit big-endian core stream starting at the",
        " * 0x7FFE8001 sync word. Regenerate with tools/memory/embed_frame.py.",
        " */",
        "#ifndef AIUS_DTS_FRAME_SURROUND51_H",
        "#define AIUS_DTS_FRAME_SURROUND51_H",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        f"/* Placed in flash (const). {len(frame)} bytes. */",
        "static const uint8_t aius_dts_frame_surround51[] = {",
    ]
    row = "    "
    for b in frame:
        tok = "0x%02x, " % b
        if len(row) + len(tok) > 96:
            lines.append(row.rstrip())
            row = "    "
        row += tok
    if row.strip():
        lines.append(row.rstrip())
    lines += [
        "};",
        "",
        "static const size_t aius_dts_frame_surround51_len =",
        "    sizeof(aius_dts_frame_surround51);",
        "",
        "#endif /* AIUS_DTS_FRAME_SURROUND51_H */",
        "",
    ]
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    here = Path(__file__).resolve()
    fw_root = here.parents[2]
    if len(argv) == 3:
        src, dst = Path(argv[1]), Path(argv[2])
    elif len(argv) == 1:
        src = fw_root / "tools/dts_vectors/vectors/surround51_tones.dts"
        dst = fw_root / "src/dts_frame_surround51.h"
    else:
        raise SystemExit(__doc__)
    frame = first_frame(src.read_bytes())
    if frame[:4] != SYNC:
        raise SystemExit("extracted frame does not start with the sync word")
    dst.write_text(emit(frame))
    print(f"wrote {dst} ({len(frame)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
