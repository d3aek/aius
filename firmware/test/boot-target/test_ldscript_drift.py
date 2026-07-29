#!/usr/bin/env python3
"""Prove link/aius_teensy41.ld is the framework script plus only the Aius block.

`board_build.ldscript` points the optical envs at our own copy of
`imxrt1062_t41.ld`, because the retained region has to be carved out immediately
before `.bss.dma` and neither an INSERT augmentation nor a link-flag script can
do that order-independently (the reasoning is in the script's header). A copy of
a framework file is a drift hazard, so this test removes our additions and
requires the remainder to be byte-identical to the framework's script.

  test_ldscript_drift.py <link/aius_teensy41.ld> [--framework <imxrt1062_t41.ld>]

Exit: 0 identical; 1 drifted or an addition is missing; 3 the framework script
could not be located.
"""
from __future__ import annotations

import argparse
import difflib
import os
from pathlib import Path
import re
import sys


DEFAULT_FRAMEWORK = Path(
    os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio")
) / "packages/framework-arduinoteensy/cores/teensy4/imxrt1062_t41.ld"

# Each addition is delimited by its own comment marker, so a drifting copy cannot
# hide an edit by moving it: the marker text is part of what must match.
HEADER_END = " * contract post-link against the full measured map. */\n\n"

SECTION_BLOCK = """\t/* AIUS ADDITION — retained across reset, pinned to ORIGIN(RAM). See the
\t * header. Must stay immediately before .bss.dma: that is what fixes the
\t * address, and .bss.dma then simply starts after it. */
\t.bss.aius_retained (NOLOAD) : {
\t\t__aius_retained_start = .;
\t\tKEEP(*(.aius_retained))
\t\tKEEP(*(.aius_retained_latch))
\t\t. = ALIGN(32);
\t\t__aius_retained_end = .;
\t} > RAM

"""

STAGING_BLOCK = """\t/* AIUS ADDITION — flight-recorder staging, deliberately AFTER every
\t * .bss.dma input so adding or resizing it can never move a pre-existing
\t * DMAMEM object. 2026-07-27: declaring the 64 KiB staging ring DMAMEM
\t * displaced every later .bss.dma object and made the image unbootable
\t * (aius-network-hardening.md defect 26); this section is the placement
\t * fix, and the heap start moves past it in the footer below. */
\t.bss.aius_staging (NOLOAD) : {
\t\t__aius_staging_start = .;
\t\tKEEP(*(.aius_recorder_staging))
\t\t. = ALIGN(32);
\t\t__aius_staging_end = .;
\t} > RAM

"""

FOOTER_START = "\n/* AIUS ADDITION — the enforcement."

REQUIRED_ASSERTS = (
    "ADDR(.bss.aius_retained) == ORIGIN(RAM)",
    "SIZEOF(.bss.aius_retained) <= 0xC60",
    "ADDR(.bss.dma) == ADDR(.bss.aius_retained) + SIZEOF(.bss.aius_retained)",
    "ADDR(.bss.aius_staging) >= ADDR(.bss.dma) + SIZEOF(.bss.dma)",
    "_heap_start >= ADDR(.bss.dma) + SIZEOF(.bss.dma)",
    "_heap_end - _heap_start >= 0x20000",
    "_itcm_block_count <= 6",
)

# The heap must be re-based past the staging carve-out, or malloc grows into
# the recorder's ring the moment the vendor-derived _heap_start is the one
# that sticks.
REQUIRED_FOOTER_LINES = (
    "_heap_start = ADDR(.bss.aius_staging) + SIZEOF(.bss.aius_staging);",
)


class DriftError(RuntimeError):
    """The copy is no longer the framework script plus exactly our block."""


def strip_additions(text: str) -> str:
    if not text.startswith("/* Aius Teensy 4.1 link script"):
        raise DriftError("the Aius header comment is missing or moved")
    header_end = text.find(HEADER_END)
    if header_end < 0:
        raise DriftError("the Aius header comment does not end as expected")
    body = text[header_end + len(HEADER_END):]

    if body.count(SECTION_BLOCK) != 1:
        raise DriftError(
            "the .bss.aius_retained output section block is missing or edited"
        )
    # It must be immediately before .bss.dma: that adjacency is what pins the
    # address, and a reordering would link cleanly while breaking retention.
    if SECTION_BLOCK + "\t.bss.dma (NOLOAD) : {\n" not in body:
        raise DriftError(
            "the retained section must sit immediately before .bss.dma"
        )
    body = body.replace(SECTION_BLOCK, "", 1)

    if body.count(STAGING_BLOCK) != 1:
        raise DriftError(
            "the .bss.aius_staging output section block is missing or edited"
        )
    # It must be immediately before .bss.extram, i.e. after every .bss.dma
    # input: a reordering before .bss.dma would re-create the displacement
    # incident this section exists to prevent, and would link cleanly.
    if STAGING_BLOCK + "\t.bss.extram (NOLOAD) : {\n" not in body:
        raise DriftError(
            "the staging section must sit between .bss.dma and .bss.extram"
        )
    body = body.replace(STAGING_BLOCK, "", 1)

    footer_at = body.find(FOOTER_START)
    if footer_at < 0:
        raise DriftError("the Aius ASSERT block is missing")
    footer = body[footer_at:]
    for expression in REQUIRED_ASSERTS:
        if expression not in footer:
            raise DriftError(f"missing link-time ASSERT on: {expression}")
    for line in REQUIRED_FOOTER_LINES:
        if line not in footer:
            raise DriftError(f"missing footer line: {line}")
    return body[:footer_at]


def compare(ours: Path, framework: Path) -> None:
    if not framework.is_file():
        raise FileNotFoundError(framework)
    stripped = strip_additions(ours.read_text(encoding="utf-8"))
    vendor = framework.read_text(encoding="utf-8")
    if stripped == vendor:
        return
    diff = "".join(
        difflib.unified_diff(
            vendor.splitlines(keepends=True),
            stripped.splitlines(keepends=True),
            fromfile=str(framework),
            tofile=f"{ours} (Aius additions removed)",
        )
    )
    raise DriftError(
        "the vendored copy has drifted from the framework script:\n" + diff
    )


def self_test(ours: Path) -> None:
    """Mutations the guard must reject, so a green run means something."""
    text = ours.read_text(encoding="utf-8")
    mutations = {
        "drop the retained section": text.replace(SECTION_BLOCK, "", 1),
        "move the retained section after .bss.dma": text.replace(
            SECTION_BLOCK, "", 1
        ).replace(
            "\t.bss.extram (NOLOAD) : {\n", SECTION_BLOCK + "\t.bss.extram (NOLOAD) : {\n", 1
        ),
        "drop the staging section": text.replace(STAGING_BLOCK, "", 1),
        "move the staging section before .bss.dma": text.replace(
            STAGING_BLOCK, "", 1
        ).replace(
            "\t.bss.dma (NOLOAD) : {\n",
            STAGING_BLOCK + "\t.bss.dma (NOLOAD) : {\n",
            1,
        ),
        "drop the heap re-base": text.replace(
            "_heap_start = ADDR(.bss.aius_staging) + "
            "SIZEOF(.bss.aius_staging);\n",
            "",
            1,
        ),
        "weaken the origin ASSERT": text.replace(
            "ADDR(.bss.aius_retained) == ORIGIN(RAM)",
            "ADDR(.bss.aius_retained) >= ORIGIN(RAM)",
            1,
        ),
        "drop the headroom ASSERT": re.sub(
            r"ASSERT\(SIZEOF\(\.bss\.aius_retained\)[^\n]*\n[^\n]*\n", "", text
        ),
        "drop the ITCM block ceiling": re.sub(
            r'ASSERT\(_itcm_block_count <= 6,\n\t"[^"]*"\)\n?', "", text
        ),
        "weaken the ITCM block ceiling": text.replace(
            "_itcm_block_count <= 6",
            "_itcm_block_count <= 7",
            1,
        ),
        "edit the vendor body": text.replace(
            "RAM (rw):   ORIGIN = 0x20200000, LENGTH = 512K",
            "RAM (rw):   ORIGIN = 0x20200000, LENGTH = 256K",
            1,
        ),
    }
    for label, mutated in mutations.items():
        if mutated == text:
            raise DriftError(f"self-test mutation did not change the script: {label}")
        try:
            stripped = strip_additions(mutated)
        except DriftError:
            continue
        if stripped != DEFAULT_FRAMEWORK.read_text(encoding="utf-8"):
            continue
        raise DriftError(f"self-test: the guard accepted a mutation: {label}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("script", type=Path)
    parser.add_argument("--framework", type=Path, default=DEFAULT_FRAMEWORK)
    parser.add_argument("--no-self-test", action="store_true")
    args = parser.parse_args(argv)

    try:
        compare(args.script, args.framework)
        if not args.no_self_test:
            self_test(args.script)
    except FileNotFoundError as error:
        print(f"link script drift: SKIP: framework script absent ({error})",
              file=sys.stderr)
        return 3
    except DriftError as error:
        print(f"link script drift: FAIL: {error}", file=sys.stderr)
        return 1
    print("link script drift: PASS (framework body byte-identical, "
          "Aius additions intact)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
