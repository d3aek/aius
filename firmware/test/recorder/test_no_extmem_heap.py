#!/usr/bin/env python3
"""The extmem-heap ban, as a build gate (mutation-tested).

The flight-recorder contract (aius-optical-prototype.md §PSRAM flight
recorder, Memory and placement) takes the whole PSRAM as a raw ring at
0x70000000 sized by `external_psram_size`. With no EXTMEM data in the image
the core's smalloc pool spans that same storage, so ONE call into the extmem
heap anywhere in this firmware silently hands out ring bytes. The ban is
firmware-wide and this guard is its enforcement: no application or library
source may reference the extmem allocator family, and no source may declare
`EXTMEM` data (which would also break 8 MB boards -- ERAM is declared 32 MB
and `.bss.extram` is NOLOAD, so an oversized array links without error).

Vendored code is out of scope: the guard walks `src/` and `lib/` only, and
comments are stripped first so prose explaining the ban does not trip it.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


FORBIDDEN = re.compile(
    r"\b(?:extmem_malloc|extmem_calloc|extmem_realloc|extmem_free|"
    r"sm_alloc|sm_zalloc|extmem_smalloc_pool)\b|^\s*EXTMEM\b",
    re.MULTILINE,
)

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)

SOURCE_SUFFIXES = {".c", ".cpp", ".h", ".hpp"}


def violations(root: Path) -> list[str]:
    errors: list[str] = []
    for area in ("src", "lib"):
        for path in sorted((root / area).rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            source = COMMENT.sub(" ", path.read_text(encoding="utf-8"))
            match = FORBIDDEN.search(source)
            if match is not None:
                errors.append(
                    f"{path.relative_to(root)}: forbidden extmem reference "
                    f"'{match.group(0).strip()}'"
                )
    return errors


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_no_extmem_heap.py <firmware-root>", file=sys.stderr)
        return 2
    root = Path(sys.argv[1])
    if not (root / "src").is_dir() or not (root / "lib").is_dir():
        print(f"not a firmware root: {root}", file=sys.stderr)
        return 2
    errors = violations(root)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    # Mutation checks: the guard must actually reject what it claims to.
    import tempfile

    with tempfile.TemporaryDirectory() as scratch:
        fake = Path(scratch)
        (fake / "src").mkdir()
        (fake / "lib").mkdir()
        clean = fake / "src" / "clean.c"
        clean.write_text("/* extmem_malloc in prose is fine */ int x;\n",
                         encoding="utf-8")
        if violations(fake):
            raise AssertionError("guard rejected a comment-only reference")
        clean.write_text("void *p; void f(void) { p = extmem_malloc(4); }\n",
                         encoding="utf-8")
        if not violations(fake):
            raise AssertionError("guard accepted extmem_malloc")
        clean.write_text("EXTMEM static int ring[4];\n", encoding="utf-8")
        if not violations(fake):
            raise AssertionError("guard accepted EXTMEM data")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
