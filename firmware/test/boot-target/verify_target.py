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
SLOT_SECTION = ".bss.dma"
SLOT_SIZE = 128
SLOT_ALIGNMENT = 32
CRASH_REPORT_START = 0x2027FF80

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
    symbols: list[LinkedSymbol] = []
    for line in output.splitlines():
        match = SYMBOL_LINE.match(line)
        if not match or match.group(7) != SLOT_SYMBOL:
            continue
        section = match.group(6)
        if not section.isdecimal():
            raise VerificationError(
                f"{SLOT_SYMBOL} is not defined in a numbered section: "
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


def verify_linked_contract(
    sections: Mapping[int, str],
    symbols: list[LinkedSymbol],
) -> LinkedSymbol:
    if len(symbols) != 1:
        raise VerificationError(
            f"expected exactly one defined {SLOT_SYMBOL}, found {len(symbols)}"
        )
    symbol = symbols[0]
    if symbol.symbol_type != "OBJECT":
        raise VerificationError(
            f"{SLOT_SYMBOL} type is {symbol.symbol_type}, expected OBJECT"
        )
    if symbol.size != SLOT_SIZE:
        raise VerificationError(
            f"{SLOT_SYMBOL} size is {symbol.size}, expected {SLOT_SIZE}"
        )
    if symbol.address % SLOT_ALIGNMENT != 0:
        raise VerificationError(
            f"{SLOT_SYMBOL} address 0x{symbol.address:08x} is not "
            f"{SLOT_ALIGNMENT}-byte aligned"
        )
    section = sections.get(symbol.section_index)
    if section != SLOT_SECTION:
        raise VerificationError(
            f"{SLOT_SYMBOL} section is {section!r}, expected {SLOT_SECTION}"
        )
    end = symbol.address + SLOT_SIZE
    if end > CRASH_REPORT_START:
        raise VerificationError(
            f"{SLOT_SYMBOL} ends at 0x{end:08x}, beyond CrashReport boundary "
            f"0x{CRASH_REPORT_START:08x}"
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
        "retained boot slots: PASS "
        f"(0x{symbol.address:08x}.."
        f"0x{symbol.address + symbol.size:08x}, {SLOT_SECTION})"
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

if env is not None and env.subst("$PIOENV") in {
    "teensy41-optical",
    "teensy41-optical-fault",
}:
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", verify_retained_slots)


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv[1:]))
