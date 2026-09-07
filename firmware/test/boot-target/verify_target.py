#!/usr/bin/env python3
"""Post-link proof for the retained Teensy boot slots."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import subprocess
import sys
from typing import Mapping


SLOT_SYMBOL = "g_aius_boot_slots"
SLOT_SECTION = ".bss.aius_retained"
SLOT_SIZE = 128
SLOT_ALIGNMENT = 32
# The defect-25 abort latch packs directly after the slots (definition order in
# aius_boot_teensy.cpp); pinning its address here is what makes that ordering a
# build contract rather than a toolchain habit.
LATCH_SYMBOL = "g_aius_abort_latch"
LATCH_SIZE = 8
CRASH_REPORT_START = 0x2027FF80

# ORIGIN(RAM) in the framework's imxrt1062_t41.ld: the start of OCRAM, and the
# address link/aius_teensy41.ld pins the retained region to. Fixed by
# construction rather than by luck -- see that script's header.
RETAINED_ORIGIN = 0x20200000

# THE BOOT ROM'S OCRAM FOOTPRINT, measured on the deployed board 2026-07-26 by
# filling 0x20200000..0x2027F800 with a self-describing pattern, resetting, and
# scanning it from startup_early_hook before any other code runs. Identical for a
# SYSRESETREQ (SRC_SRSR=0x02) and a WDOG1 timeout (SRC_SRSR=0x10). Retained state
# placed inside any of these does not survive a reset; that is exactly what
# happened when the 24-bit output ring pushed the slots to 0x20205140, and the
# previous version of this file passed anyway because it checked placement and the
# CrashReport bound and never asked whether the address retains.
#
# Re-measure before trusting these on a different chip or bootloader. The probe is
# recorded in docs/reference/firmware-io.md §Retained OCRAM.
BOOT_ROM_CLOBBERED = (
    (0x20200C60, 0x20201100),
    (0x20201B80, 0x20201C40),
    (0x20203A60, 0x20207F60),
)

SECTION_LINE = re.compile(r"^\s*\[\s*(\d+)\]\s+(\S+)(?:\s|$)")
SYMBOL_LINE = re.compile(
    r"^\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+(\S+)\s+"
    r"(\S+)\s+(\S+)\s+(\S+)\s+(.+?)\s*$"
)


class VerificationError(RuntimeError):
    """The linked retained-slot contract is not satisfied."""


@dataclass(frozen=True)
class LinkedSymbol:
    address: int
    size: int
    symbol_type: str
    section_index: int
    name: str


def parse_sections(output: str) -> dict[int, str]:
    sections: dict[int, str] = {}
    for line in output.splitlines():
        match = SECTION_LINE.match(line)
        if match:
            sections[int(match.group(1))] = match.group(2)
    return sections


def parse_symbols(output: str) -> list[LinkedSymbol]:
    watched = (SLOT_SYMBOL, LATCH_SYMBOL)
    symbols: list[LinkedSymbol] = []
    for line in output.splitlines():
        match = SYMBOL_LINE.match(line)
        if not match or match.group(7) not in watched:
            continue
        section = match.group(6)
        if not section.isdecimal():
            raise VerificationError(
                f"{match.group(7)} is not defined in a numbered section: "
                f"{section}"
            )
        symbols.append(
            LinkedSymbol(
                address=int(match.group(1), 16),
                size=int(match.group(2), 10),
                symbol_type=match.group(3),
                section_index=int(section, 10),
                name=match.group(7),
            )
        )
    return symbols


def verify_one_symbol(
    sections: Mapping[int, str],
    symbols: list[LinkedSymbol],
    name: str,
    expected_address: int,
    expected_size: int,
) -> LinkedSymbol:
    found = [symbol for symbol in symbols if symbol.name == name]
    if len(found) != 1:
        raise VerificationError(
            f"expected exactly one defined {name}, found {len(found)}"
        )
    symbol = found[0]
    if symbol.symbol_type != "OBJECT":
        raise VerificationError(
            f"{name} type is {symbol.symbol_type}, expected OBJECT"
        )
    if symbol.size != expected_size:
        raise VerificationError(
            f"{name} size is {symbol.size}, expected {expected_size}"
        )
    if symbol.address % SLOT_ALIGNMENT != 0:
        raise VerificationError(
            f"{name} address 0x{symbol.address:08x} is not "
            f"{SLOT_ALIGNMENT}-byte aligned"
        )
    section = sections.get(symbol.section_index)
    if section != SLOT_SECTION:
        raise VerificationError(
            f"{name} section is {section!r}, expected {SLOT_SECTION}"
        )
    end = symbol.address + expected_size
    if end > CRASH_REPORT_START:
        raise VerificationError(
            f"{name} ends at 0x{end:08x}, beyond CrashReport boundary "
            f"0x{CRASH_REPORT_START:08x}"
        )
    if symbol.address != expected_address:
        raise VerificationError(
            f"{name} is at 0x{symbol.address:08x}, expected the pinned "
            f"retained address 0x{expected_address:08x}; a floating address "
            "is what broke retention once already"
        )
    for start, stop in BOOT_ROM_CLOBBERED:
        if symbol.address < stop and start < end:
            raise VerificationError(
                f"{name} 0x{symbol.address:08x}..0x{end:08x} intersects "
                f"the boot ROM's OCRAM scratch 0x{start:08x}..0x{stop:08x}, so "
                "it cannot survive a reset"
            )
    return symbol


def verify_linked_contract(
    sections: Mapping[int, str],
    symbols: list[LinkedSymbol],
) -> LinkedSymbol:
    # Slots first, so a whole-section drift is reported as the slots leaving
    # their origin rather than as latch collateral. The latch is expected
    # DIRECTLY after the slots: `.aius_retained_latch` is placed after
    # `.aius_retained` by the ldscript, and the slot array's 128 bytes (a
    # multiple of the 32-byte alignment) leave no pad between them.
    symbol = verify_one_symbol(
        sections, symbols, SLOT_SYMBOL, RETAINED_ORIGIN, SLOT_SIZE,
    )
    verify_one_symbol(
        sections, symbols, LATCH_SYMBOL,
        RETAINED_ORIGIN + SLOT_SIZE, LATCH_SIZE,
    )
    return symbol


def inspect_elf(elf: Path, readelf: str) -> LinkedSymbol:
    section_result = subprocess.run(
        [readelf, "-SW", str(elf)],
        check=True,
        text=True,
        capture_output=True,
    )
    symbol_result = subprocess.run(
        [readelf, "-sW", str(elf)],
        check=True,
        text=True,
        capture_output=True,
    )
    return verify_linked_contract(
        parse_sections(section_result.stdout),
        parse_symbols(symbol_result.stdout),
    )


def _resolve_readelf(env) -> str:
    readelf = env.WhereIs("arm-none-eabi-readelf")
    if not readelf:
        raise VerificationError("arm-none-eabi-readelf is unavailable")
    return str(readelf)


def verify_retained_slots(source, target, env) -> None:
    del source
    elf = Path(str(target[0])).resolve()
    symbol = inspect_elf(elf, _resolve_readelf(env))
    print(
        "retained boot slots + abort latch: PASS "
        f"(0x{symbol.address:08x}.."
        f"0x{symbol.address + symbol.size + LATCH_SIZE:08x}, {SLOT_SECTION}, "
        f"clear of the {len(BOOT_ROM_CLOBBERED)} measured boot-ROM OCRAM runs)"
    )


def _main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    parser.add_argument(
        "--readelf",
        default="arm-none-eabi-readelf",
        help="readelf-compatible executable",
    )
    args = parser.parse_args(argv)
    try:
        inspect_elf(args.elf, args.readelf)
    except (OSError, subprocess.CalledProcessError, VerificationError) as error:
        print(f"retained boot slots: FAIL: {error}", file=sys.stderr)
        return 1
    print("retained boot slots: PASS")
    return 0


try:
    Import("env")  # type: ignore[name-defined]  # PlatformIO/SCons injection
except NameError:
    env = None

# Every env that links the optical image: the A/B slot envs extend
# teensy41-optical and are the deployed image since the slot-agnostic chooser,
# so a guard keyed on the bench env alone leaves the canonical image unguarded
# (register, 2026-09-07).
OPTICAL_IMAGE_ENVS = frozenset({
    "teensy41-optical",
    "teensy41-optical-slot-a",
    "teensy41-optical-slot-b",
    "teensy41-optical-fault",
})

if env is not None and env.subst("$PIOENV") in OPTICAL_IMAGE_ENVS:
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", verify_retained_slots)


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv[1:]))
