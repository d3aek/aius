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
# Regression ratchet, not the safety line. The defect-26 guard is the link-time
# ASSERT `_heap_end - _heap_start >= 0x1FFE0` (131,040 B, link/aius_teensy41.ld:271;
# NativeEthernet's measured ~96 KiB boot mallocs plus headroom), lowered from
# 0x20000 on 2026-08-16 to fund the A/B trial ledger with a same-day bench boot
# as proof. This table only trips on static OCRAM growth.
#
# One floor per PIOENV (aius-sample-rate-following.md Phase 4e, R4-B2, E12):
# each is the OCRAM free measured on that env's link of the unchanged tree at
# 4e start (aius d6f4f58, 2026-10-07; plan §Memory, "4e-start baseline"). The
# A/B envs carry the 32 B `.aius_retained_ab` ledger in the content-sized
# `.bss.aius_retained`, so their figure is not the plain env's. An env below
# its own floor fails, and an env with no entry fails. A floor only ever rises
# to a fresh measurement of that env; lowering one needs funding (move
# something to DTCM or flash, or widen the link floor with fresh NativeEthernet
# headroom measurements), so any OCRAM growth in any env fails the gate.
#
# History of the single constant this table replaced (each lowering funded):
# 131424 -> 131392 (2026-08-01, OTA dispatcher seam, 32 B); -> 131328 (same day,
# cache-line pinning of g_runtime_storage and its sd_transfer buffer, 64 B);
# -> 131264 (2026-08-03, retained AiusPollProbe, 64 B); -> 131232 (2026-08-04,
# AiusDownmixConfig +16 B in two OCRAM owners); -> 131136 (2026-08-16, retained
# OTA dark-window stamp, 64 B plus 32 B alignment); -> 131072 (same day, the
# deferred-absorb staging seam, 64 B); -> 131040 (same day, the A/B trial
# ledger, funded by the 0x1FFE0 link floor). The plain image then measured
# 131,776 B (736 B over the link floor), so the old "margin is zero" note was
# stale by the time this table replaced it.
#
# Phase 4i (2026-10-12) retired replay: the 128 KiB ring in g_replay_service
# plus g_replay_job, g_replay_transfer and g_replay_diag left OCRAM, a measured
# gain of 132,992 B per env (131,776 -> 264,768 plain and fault; 131,744 ->
# 264,736 slot-a and slot-b). Every floor below is that link's figure.
OCRAM_LINK_FLOOR_BYTES = 0x1FFE0
OCRAM_FREE_FLOORS = {
    "teensy41-optical": 264768,
    "teensy41-optical-slot-a": 264736,
    "teensy41-optical-slot-b": 264736,
    "teensy41-optical-fault": 264768,
}

# Canonical-image RAM1 budgets (aius-sample-rate-following.md Phase 4g,
# "Starting figures"). They apply to CANONICAL_ENV only, the slot-a image
# the 4e/4f figures quote. Each figure below moves ONLY in a phase commit
# that records the new figure in the plan; never to make a build pass.
CANONICAL_ENV = "teensy41-optical-slot-a"
ITCM_ORIGIN = 0x00000000
ITCM_REGION_BYTES = 512 * 1024
ITCM_BLOCK_BYTES = 32 * 1024
ITCM_SECTIONS = frozenset({".text.itcm", ".ARM.exidx", ".fini"})
# ITCM free = _itcm_block_count * 32 KiB minus the ITCM high-water mark (the
# mem_report.py figure). Set from the Phase 4h link (2026-10-11): 12,708 B
# free (4g's link had 12,900 B; its 1,000 B allowance went to 4h). Reset by
# the Phase 4i link (2026-10-12, replay retired): 13,284 B. The next phase
# records its own allowance.
ITCM_FREE_FLOOR_BYTES = 13284
# DTCM statics end (`_ebss`). Set from the Phase 4h link (2026-10-11):
# 0x20049560 (4g's ceiling was 0x20049A00); Phase 4i link (2026-10-12):
# 0x200494c0 on the slot-a image.
DTCM_EBSS_CEILING = 0x200494c0
# `_VectorsRam` (the 1,024 B-aligned RAM vector table, last in `.data`) sits
# at 0x20007800, set from the Phase 4h link (2026-10-11; the 4g link had it
# at 0x20007c00); `.data` growth past that step moves it to the next 1,024 B
# step and costs 1,024 B of DTCM. New strings and tables go in flash.
VECTORS_RAM_ADDRESS = 0x20007800
VECTORS_RAM_STEP_BYTES = 1024

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


def ocram_floor(environment: str) -> int:
    """The env's committed OCRAM free floor; an env with no entry fails."""
    if environment not in OCRAM_FREE_FLOORS:
        raise VerificationError(
            f"no OCRAM floor for env {environment!r}; add a fresh measurement "
            "of its link to OCRAM_FREE_FLOORS in test/memory/verify_target.py"
        )
    return OCRAM_FREE_FLOORS[environment]


def canonical_budgets(
    sections: dict[int, tuple[str, int, int]], symbols: list[Symbol]
) -> tuple[int, int, int]:
    """(ITCM free, `_ebss`, `_VectorsRam`) of a linked image."""
    block_count = _one_symbol(symbols, "_itcm_block_count").value
    high_water = max(
        (
            addr + size
            for name, addr, size in sections.values()
            if name in ITCM_SECTIONS and addr < ITCM_REGION_BYTES
        ),
        default=ITCM_ORIGIN,
    )
    itcm_free = block_count * ITCM_BLOCK_BYTES - (high_water - ITCM_ORIGIN)
    return (
        itcm_free,
        _one_symbol(symbols, "_ebss").value,
        _one_symbol(symbols, "_VectorsRam").value,
    )


def check_canonical_budgets(
    sections: dict[int, tuple[str, int, int]], symbols: list[Symbol]
) -> tuple[int, int, int]:
    itcm_free, ebss, vectors = canonical_budgets(sections, symbols)
    if itcm_free < ITCM_FREE_FLOOR_BYTES:
        raise VerificationError(
            f"ITCM free is {itcm_free} B, below its committed "
            f"{ITCM_FREE_FLOOR_BYTES} B floor"
        )
    if ebss > DTCM_EBSS_CEILING:
        raise VerificationError(
            f"DTCM _ebss is {ebss:#x}, above its committed "
            f"{DTCM_EBSS_CEILING:#x} ceiling"
        )
    if vectors != VECTORS_RAM_ADDRESS:
        raise VerificationError(
            f"_VectorsRam moved to {vectors:#x} from {VECTORS_RAM_ADDRESS:#x} "
            f"(.data crossed a {VECTORS_RAM_STEP_BYTES} B step)"
        )
    return itcm_free, ebss, vectors


def verify_snapshot(
    sections: dict[int, tuple[str, int, int]],
    symbols: list[Symbol],
    environment: str,
) -> tuple[int, int, int]:
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

    floor = ocram_floor(environment)
    heap_start = _one_symbol(symbols, "_heap_start")
    heap_end = _one_symbol(symbols, "_heap_end")
    if not (
        OCRAM_START <= heap_start.value <= heap_end.value <= OCRAM_END
    ):
        raise VerificationError("OCRAM heap symbols lie outside OCRAM")
    ocram_free = heap_end.value - heap_start.value
    if ocram_free < floor:
        raise VerificationError(
            f"OCRAM free is {ocram_free} B on {environment}, below its "
            f"committed {floor} B floor"
        )
    if environment == CANONICAL_ENV:
        check_canonical_budgets(sections, symbols)
    return ocram_free, staging_bytes, floor


def verify(elf: Path, readelf: str, environment: str) -> tuple[int, int, int]:
    return verify_snapshot(
        _sections(readelf, elf), _symbols(readelf, elf), environment
    )


def _budget_line(elf: Path, readelf: str, environment: str) -> str:
    if environment != CANONICAL_ENV:
        return ""
    itcm_free, ebss, vectors = canonical_budgets(
        _sections(readelf, elf), _symbols(readelf, elf)
    )
    return (
        f"\ncanonical budgets: ITCM free={itcm_free} B "
        f"(floor {ITCM_FREE_FLOOR_BYTES}), _ebss={ebss:#x} "
        f"(ceiling {DTCM_EBSS_CEILING:#x}), _VectorsRam={vectors:#x}"
    )


def _summary(ocram_free: int, staging_bytes: int, floor: int) -> str:
    delta = ocram_free - floor
    return (
        "audio target memory: PASS "
        f"(ingress={INGRESS_BYTES} B OCRAM, "
        f"decoder arena={DECODER_ARENA_BYTES} B DTCM, "
        f"recorder OCRAM staging={staging_bytes} B, "
        f"OCRAM free={ocram_free} B, "
        f"floor={floor} B, delta={delta:+d} B)"
    )


def verify_target(source, target, env) -> None:
    del source
    elf = Path(str(target[0])).resolve()
    readelf = env.WhereIs("arm-none-eabi-readelf")
    if not readelf:
        raise VerificationError("arm-none-eabi-readelf is unavailable")
    pioenv = env.subst("$PIOENV")
    print(_summary(*verify(elf, str(readelf), pioenv))
          + _budget_line(elf, str(readelf), pioenv))


def _main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    parser.add_argument("--readelf", default="arm-none-eabi-readelf")
    parser.add_argument(
        "--env",
        required=True,
        help="PlatformIO environment whose OCRAM floor applies",
    )
    args = parser.parse_args(argv)
    try:
        elf = args.elf.resolve()
        print(_summary(*verify(elf, args.readelf, args.env))
              + _budget_line(elf, args.readelf, args.env))
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
