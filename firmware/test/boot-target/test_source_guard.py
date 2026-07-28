#!/usr/bin/env python3
"""Mutation-tested structural guard for the retained boot target adapter."""

from __future__ import annotations

import re
import sys
from pathlib import Path


RETAINED_SECTION = ".aius_retained"

# The retained slots must be declared into the PINNED section, never DMAMEM.
# `.dmabuffers` is packed in object order, so a DMAMEM declaration puts the
# ledger at whatever address the DMA buffers ahead of it happen to leave -- which
# is how the 24-bit output ring moved it into the boot ROM's OCRAM scratch and
# killed retention with no diff to this file at all. link/aius_teensy41.ld pins
# `.aius_retained` to ORIGIN(RAM) and ASSERTs it.
RETAINED_DECLARATION = re.compile(
    r"__attribute__\s*\(\(\s*section\s*\(\s*\"\.aius_retained\"\s*\)\s*,"
    r"[^)]*\bused\b[^)]*\baligned\s*\(\s*32\s*\)[^)]*\)\)\s*"
    r"AiusBootSlot\s+g_aius_boot_slots\s*\[\s*2\s*\]\s*;",
    re.DOTALL,
)
UNBOUNDED_LOOP = re.compile(
    r"\bwhile\s*\(\s*(?:true|1)\s*\)|\bfor\s*\(\s*;\s*;\s*\)"
)
FORBIDDEN_RUNTIME = re.compile(
    r"\b(?:delay|yield|millis|micros)\s*\(|\b(?:Serial|SD|Ethernet)\b"
)

# The adapter reads boot-configuration fuse 0x6E0 (HW_OCOTP_MISC_CONF1) to
# report whether hardware A/B image selection is available. That read must stay
# a read. Blowing an OCOTP fuse is irreversible and per board, and 0x6E0[23:16]
# is where the boot ROM looks for a secondary image -- a stray write there
# permanently changes how the part boots, with no way back and no reflash that
# undoes it. Assignment, read-modify-write and compound forms are all rejected;
# a bare mention is fine, because that is what a read looks like.
OCOTP_WRITE = re.compile(
    r"\b(?:HW_OCOTP_\w+|IMXRT_OCOTP\w*)\b[^;=<>!]*"
    r"(?:\|=|&=|\^=|\+=|-=|>>=|<<=|=(?!=))"
)
# A raw pointer into the OCOTP block would sidestep the check above entirely,
# so the adapter must reach the register through the framework macro rather
# than through an address literal it maintains itself. The 0x401F4xxx addresses
# in this file's derivation comment are stripped before matching, which is the
# point of documenting the derivation in prose rather than in code.
OCOTP_RAW_ADDRESS = re.compile(r"0x401[Ff]4[0-9A-Fa-f]{3}")


COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)


def strip_comments(source: str) -> str:
    """Every check below is about what the adapter DECLARES, not what it explains.

    The file documents why the retained slots are no longer DMAMEM, and a
    structural guard that reads prose would reject that explanation. Replacing
    each comment with a space preserves token boundaries.
    """
    return COMMENT.sub(" ", source)


def violations(text: str) -> list[str]:
    source = strip_comments(text)
    errors: list[str] = []
    retained = re.findall(
        re.escape(RETAINED_SECTION) + r"\"[^;]*;", source, flags=re.DOTALL
    )
    if len(retained) != 1:
        errors.append(
            f"exactly one {RETAINED_SECTION} declaration is required"
        )
    if RETAINED_DECLARATION.search(source) is None:
        errors.append(
            f"retained slots must be section(\"{RETAINED_SECTION}\"), used, "
            "aligned(32), and exactly [2]"
        )
    if re.search(r"\bDMAMEM\b", source):
        errors.append(
            "DMAMEM floats inside .bss.dma; the retained slots need the pinned "
            f"{RETAINED_SECTION} section"
        )
    if UNBOUNDED_LOOP.search(source):
        errors.append("unbounded loop in retained/reset/watchdog adapter")
    if FORBIDDEN_RUNTIME.search(source):
        errors.append("adapter acquired forbidden runtime authority")
    if OCOTP_WRITE.search(source):
        errors.append(
            "OCOTP must be read-only in this adapter: blowing a fuse is "
            "irreversible and 0x6E0 decides where the ROM looks for a "
            "secondary boot image"
        )
    if OCOTP_RAW_ADDRESS.search(source):
        errors.append(
            "reach OCOTP through the framework macro (HW_OCOTP_*), never a "
            "raw 0x401F4xxx address literal"
        )
    if source.count("g_aius_boot_slots") < 1:
        errors.append("retained slot symbol is missing")
    return errors


def assert_mutation_rejected(source: str, mutated: str, label: str) -> None:
    if mutated == source:
        raise AssertionError(f"mutation did not change source: {label}")
    if not violations(mutated):
        raise AssertionError(f"guard accepted mutation: {label}")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_source_guard.py <aius_boot_teensy.cpp>",
              file=sys.stderr)
        return 2
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    errors = violations(source)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    assert_mutation_rejected(
        source,
        source.replace('section(".aius_retained"), used, aligned(32)',
                       "used", 1),
        "drop the pinned section")
    assert_mutation_rejected(
        source,
        source.replace('section(".aius_retained"), used, aligned(32)',
                       'section(".dmabuffers"), used, aligned(32)', 1),
        "revert to a floating .dmabuffers placement")
    assert_mutation_rejected(
        source,
        source.replace('section(".aius_retained"), used, aligned(32)',
                       'section(".aius_retained"), aligned(32)', 1),
        "remove used")
    assert_mutation_rejected(
        source,
        source.replace('section(".aius_retained"), used, aligned(32)',
                       'section(".aius_retained"), used', 1),
        "remove aligned(32)")
    assert_mutation_rejected(
        source,
        source + '\n__attribute__((section(".aius_retained"), used))'
                 " uint32_t second_retained;\n",
        "add retained object")
    assert_mutation_rejected(
        source, source + "\nDMAMEM uint32_t dma_retained;\n",
        "reintroduce DMAMEM")
    assert_mutation_rejected(
        source, source + "\nvoid forbidden(void) { while (true) {} }\n",
        "add unbounded loop")
    assert_mutation_rejected(
        source, source + "\nvoid forbidden(void) { delay(1); }\n",
        "add delay")
    assert_mutation_rejected(
        source, source + "\nvoid forbidden(void) { SD.begin(1); }\n",
        "add SD authority")
    assert_mutation_rejected(
        source,
        source + "\nvoid forbidden(void) { HW_OCOTP_CTRL = 1u; }\n",
        "write an OCOTP control register")
    assert_mutation_rejected(
        source,
        source + "\nvoid forbidden(void) { HW_OCOTP_MISC_CONF1 |= 1u; }\n",
        "read-modify-write the fuse shadow this adapter reads")
    assert_mutation_rejected(
        source,
        source + "\nvoid forbidden(void) { IMXRT_OCOTP.offset000 = 0u; }\n",
        "write through the OCOTP register block")
    assert_mutation_rejected(
        source,
        source + "\nvoid forbidden(void)"
                 " { *(volatile uint32_t *)0x401F46E0 = 0u; }\n",
        "write through a raw OCOTP address literal")
    assert_mutation_rejected(
        source,
        source + "\nuint32_t peek(void)"
                 " { return *(volatile uint32_t *)0x401F46E0; }\n",
        "read through a raw OCOTP address literal")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
