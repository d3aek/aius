#!/usr/bin/env python3
"""Post-link proof for the Slice 2b3 target memory contract."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import subprocess
import sys


OCRAM_START = 0x20200000
OCRAM_END = 0x20280000
INGRESS_BYTES = 16384
# instance bound (5,632) + DSP double-staging carve (32,768) + codec slot
# (180,224, the E-AC-3 residency need) + allocation-header slack (64) --
# the same compile-time sum optical_main.cpp static_asserts (E-AC-3 plan
# phase 4; was 131072 before the slot grew past the DTS need).
DECODER_ARENA_BYTES = 218688
# Regression ratchet, not the safety line. The real defect-26 guard is the
# link-time ASSERT `_heap_end - _heap_start >= 0x20000` (128 KiB = NativeEthernet's
# measured ~96 KiB boot mallocs plus a third headroom) in link/aius_teensy41.ld;
# it retains 256 B of margin at this value. This ratchet only trips on a static
# OCRAM regression below the committed baseline. Lowered from 131424 to 131392 on
# 2026-08-01: the OTA app-dispatcher seam (`AiusOpticalAppOta ota;` in the DMAMEM
# AiusOpticalAppDispatcher, added with the OTA route layer) costs 32 B of OCRAM,
# consistent with the always-present recorder/replay/measure seams beside it. The
# OTA-on image was hardware-verified to boot NativeEthernet (DHCP + HTTP) at this
# free value, which directly proves the boot mallocs still succeed. Lowered again
# to 131328 on 2026-08-01 (+64 B of deliberate alignment padding): the
# SD-corruption regression fix pins g_runtime_storage and its interior
# sd_transfer DMA buffer to 32-byte cache lines (aius_runtime_storage.h), so the
# driver's cache maintenance can never straddle a neighbouring field again --
# 64 B of padding is the price of making that placement robust instead of lucky.
# Lowered to 131264 on 2026-08-03: the retained fnet_poll probe (AiusPollProbe,
# 64 B in `.aius_retained_probe` inside the ROM-safe ORIGIN(RAM) window) is the
# diagnostic vector for the periodic DHCP-T1 watchdog wedge (open-work
# register); still 192 B above the defect-26 128 KiB heap-floor link ASSERT.
# Lowered to 131232 on 2026-08-04: AiusDownmixConfig grew 16 B (the surround
# decorrelation delay's value + state pointer, audio-quality roadmap item 4)
# and two OCRAM-resident owners embed a config copy; the 4.6 KiB delay ring
# itself is DTCM (inside AiusDspRuntimeConfig). Still 160 B above the
# defect-26 floor.
# Lowered to 131136 on 2026-08-16: the retained OTA dark-window forensic
# stamp (64 B in `.aius_retained_ota`, last in the ROM-safe ORIGIN(RAM)
# window, plus 32 B of section alignment measured post-link) -- the record a
# bricked window carries into its WDOG reset (aius-ota-resilience.md
# Phase 1). Still 64 B above the defect-26 floor.
# Lowered to 131072 on 2026-08-16, later: the deferred-absorb staging seam
# (aius-ota-resilience.md Phase 4a) -- the dispatcher's pending-chunk stash
# and the transaction's settle-hook latch, 64 B after cache-line alignment.
# This value IS the defect-26 128 KiB link-time floor: OCRAM margin is now
# ZERO, and the next OCRAM byte must be funded (move something to DTCM/flash
# or widen the floor with fresh NativeEthernet headroom measurements), not
# ratcheted past.
# Lowered to 131040 on 2026-08-16, latest: the A/B trial ledger (32 B in
# `.aius_retained_ab`, the chooser/application contract at 0x20200120,
# aius-ota-resilience.md Phase 3). Funded by lowering the defect-26 link
# floor to 0x1FFE0 with the same-day bench boot as proof (~32 KiB of real
# headroom over the ~96 KiB measured NativeEthernet demand remains).
COMMITTED_OCRAM_FREE_BYTES = 131040

SECTION_RE = re.compile(
    r"^\s*\[\s*(\d+)\]\s+(\S+)\s+\S+\s+"
    r"([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+"
)


class VerificationError(RuntimeError):
    """The linked image violates the target memory contract."""


@dataclass(frozen=True)
class Symbol:
    value: int
    size: int
    section_index: int | None
    name: str


def _output(command: list[str]) -> str:
    return subprocess.run(
        command, check=True, capture_output=True, text=True
    ).stdout


def parse_sections(output: str) -> dict[int, tuple[str, int, int]]:
    sections: dict[int, tuple[str, int, int]] = {}
    for line in output.splitlines():
        match = SECTION_RE.match(line)
        if match:
            sections[int(match.group(1))] = (
                match.group(2),
                int(match.group(3), 16),
                int(match.group(4), 16),
            )
    return sections


def parse_symbols(output: str) -> list[Symbol]:
    symbols = []
    for line in output.splitlines():
        parts = line.split()
        if len(parts) < 8 or not parts[0].rstrip(":").isdigit():
            continue
        try:
            section_index = int(parts[6])
        except ValueError:
            section_index = None
        symbols.append(
            Symbol(
                value=int(parts[1], 16),
                size=int(parts[2], 0),
                section_index=section_index,
                name=parts[7],
            )
        )
    return symbols


def _sections(readelf: str, elf: Path) -> dict[int, tuple[str, int, int]]:
    return parse_sections(_output([readelf, "-SW", str(elf)]))


def _symbols(readelf: str, elf: Path) -> list[Symbol]:
    return parse_symbols(_output([readelf, "-sW", str(elf)]))


def _one_symbol(symbols: list[Symbol], suffix: str) -> Symbol:
    matches = [symbol for symbol in symbols if symbol.name.endswith(suffix)]
    if len(matches) != 1:
        raise VerificationError(
            f"expected one symbol ending {suffix!r}, found "
            f"{[symbol.name for symbol in matches]}"
        )
    return matches[0]


def _section_for(
    symbol: Symbol, sections: dict[int, tuple[str, int, int]]
) -> tuple[str, int, int]:
    if symbol.section_index is None or symbol.section_index not in sections:
        raise VerificationError(f"{symbol.name} has no allocated section")
    return sections[symbol.section_index]


def verify_snapshot(
    sections: dict[int, tuple[str, int, int]], symbols: list[Symbol]
) -> tuple[int, int]:
    ingress = _one_symbol(symbols, "g_audio_ingress_words")
    ingress_section = _section_for(ingress, sections)
    if ingress.size != INGRESS_BYTES:
        raise VerificationError(
            f"g_audio_ingress_words is {ingress.size} B, expected "
            f"{INGRESS_BYTES} B"
        )
    if ingress_section[0] != ".bss.dma" or not (
        OCRAM_START <= ingress.value < OCRAM_END
        and ingress.value + ingress.size <= OCRAM_END
    ):
        raise VerificationError(
            "g_audio_ingress_words is not wholly in OCRAM .bss.dma"
        )

    arena = _one_symbol(symbols, "g_arena_memoryE")
    arena_section = _section_for(arena, sections)
    if arena.size != DECODER_ARENA_BYTES:
        raise VerificationError(
            f"decoder arena is {arena.size} B, expected "
            f"{DECODER_ARENA_BYTES} B"
        )
    if arena_section[0] != ".bss":
        raise VerificationError(
            f"decoder arena moved from DTCM .bss to {arena_section[0]}"
        )

    staging_symbols = [
        symbol.name
        for symbol in symbols
        if symbol.name.endswith("g_recorder_staging")
    ]
    if staging_symbols:
        raise VerificationError(
            "obsolete recorder sample-staging symbol remains: "
            + ", ".join(staging_symbols)
        )
    staging_sections = [
        section for section in sections.values()
        if section[0] == ".bss.aius_staging"
    ]
    if len(staging_sections) > 1:
        raise VerificationError(
            "expected at most one .bss.aius_staging output section"
        )
    staging_bytes = staging_sections[0][2] if staging_sections else 0
    if staging_bytes != 0:
        raise VerificationError(
            f".bss.aius_staging still consumes {staging_bytes} OCRAM bytes"
        )

    heap_start = _one_symbol(symbols, "_heap_start")
    heap_end = _one_symbol(symbols, "_heap_end")
    if not (
        OCRAM_START <= heap_start.value <= heap_end.value <= OCRAM_END
    ):
        raise VerificationError("OCRAM heap symbols lie outside OCRAM")
    ocram_free = heap_end.value - heap_start.value
    if ocram_free < COMMITTED_OCRAM_FREE_BYTES:
        raise VerificationError(
            f"OCRAM free is {ocram_free} B, below committed "
            f"{COMMITTED_OCRAM_FREE_BYTES} B floor"
        )
    return ocram_free, staging_bytes


def verify(elf: Path, readelf: str) -> tuple[int, int]:
    return verify_snapshot(_sections(readelf, elf), _symbols(readelf, elf))


def _summary(ocram_free: int, staging_bytes: int) -> str:
    delta = ocram_free - COMMITTED_OCRAM_FREE_BYTES
    return (
        "audio target memory: PASS "
        f"(ingress={INGRESS_BYTES} B OCRAM, "
        f"decoder arena={DECODER_ARENA_BYTES} B DTCM, "
        f"recorder OCRAM staging={staging_bytes} B, "
        f"OCRAM free={ocram_free} B, "
        f"baseline={COMMITTED_OCRAM_FREE_BYTES} B, delta={delta:+d} B)"
    )


def verify_target(source, target, env) -> None:
    del source
    elf = Path(str(target[0])).resolve()
    readelf = env.WhereIs("arm-none-eabi-readelf")
    if not readelf:
        raise VerificationError("arm-none-eabi-readelf is unavailable")
    print(_summary(*verify(elf, str(readelf))))


def _main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    parser.add_argument("--readelf", default="arm-none-eabi-readelf")
    args = parser.parse_args(argv)
    try:
        print(_summary(*verify(args.elf.resolve(), args.readelf)))
    except (OSError, subprocess.CalledProcessError, VerificationError) as error:
        print(f"audio target memory: FAIL: {error}", file=sys.stderr)
        return 1
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
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", verify_target)


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv[1:]))
