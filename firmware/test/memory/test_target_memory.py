#!/usr/bin/env python3
"""Native tests for the optical post-link audio-memory verifier."""

from copy import deepcopy
import importlib.util
from pathlib import Path
import sys


VERIFY_PATH = Path(__file__).resolve().parent / "verify_target.py"
SPEC = importlib.util.spec_from_file_location("target_memory_verify", VERIFY_PATH)
assert SPEC is not None and SPEC.loader is not None
verify = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = verify
SPEC.loader.exec_module(verify)


ENV = "teensy41-optical-slot-a"


def fixture(environment=ENV):
    """A snapshot sitting exactly on `environment`'s floor."""
    floor = verify.OCRAM_FREE_FLOORS[environment]
    sections = {
        9: (".bss", 0x20020000, 0x22000),
        11: (".bss.dma", 0x202000A0, 0x5FDE0),
        12: (".bss.aius_staging", 0x2025FE80, 0),
        # ITCM high-water 183,324 B of 6 x 32 KiB: 13,284 B free (post-4i).
        13: (".text.itcm", 0x0, 183320),
        14: (".fini", 0x2CC18, 4),
    }
    symbols = [
        verify.Symbol(
            0x2022B870, verify.INGRESS_BYTES, 11,
            "_ZL21g_audio_ingress_words",
        ),
        verify.Symbol(
            0x20021720, verify.DECODER_ARENA_BYTES, 9,
            "_ZN18AiusAudioProcessor14g_arena_memoryE",
        ),
        verify.Symbol(verify.OCRAM_END - floor, 0, 12, "_heap_start"),
        verify.Symbol(verify.OCRAM_END, 0, None, "_heap_end"),
        verify.Symbol(6, 0, None, "_itcm_block_count"),
        verify.Symbol(0x200494c0, 0, 18, "_ebss"),
        verify.Symbol(0x20007800, 704, 14, "_VectorsRam"),
    ]
    return sections, symbols


def expect_failure(sections, symbols, needle, environment=ENV):
    try:
        verify.verify_snapshot(sections, symbols, environment)
    except verify.VerificationError as error:
        assert needle in str(error), f"wrong failure: {error}"
        return
    raise AssertionError(f"expected verifier failure containing {needle!r}")


def main():
    # One floor per optical env, each the 4e-start measurement; every env
    # in OPTICAL_IMAGE_ENVS has one, and each passes exactly on it.
    assert set(verify.OCRAM_FREE_FLOORS) == set(verify.OPTICAL_IMAGE_ENVS)
    assert verify.OCRAM_FREE_FLOORS == {
        "teensy41-optical": 264768,
        "teensy41-optical-slot-a": 264736,
        "teensy41-optical-slot-b": 264736,
        "teensy41-optical-fault": 264768,
    }
    for environment, floor in verify.OCRAM_FREE_FLOORS.items():
        assert floor >= verify.OCRAM_LINK_FLOOR_BYTES
        env_sections, env_symbols = fixture(environment)
        assert verify.verify_snapshot(
            env_sections, env_symbols, environment
        ) == (floor, 0, floor)
        # Synthetic snapshot 1 B under this env's floor fails.
        under = deepcopy(env_symbols)
        under[2] = verify.Symbol(
            verify.OCRAM_END - floor + 1, 0, 12, "_heap_start"
        )
        expect_failure(env_sections, under, "below its committed", environment)

    sections, symbols = fixture()
    floor = verify.OCRAM_FREE_FLOORS[ENV]
    no_empty_output = deepcopy(sections)
    del no_empty_output[12]
    assert verify.verify_snapshot(no_empty_output, symbols, ENV) == (
        floor, 0, floor
    )

    # The plain env's floor is not the A/B envs': a slot-a link at the plain
    # figure passes, the plain env at the slot-a figure fails.
    plain_floor = verify.OCRAM_FREE_FLOORS["teensy41-optical"]
    assert plain_floor > floor
    expect_failure(sections, symbols, "below its committed", "teensy41-optical")

    # An env with no entry fails, even with ample OCRAM.
    expect_failure(sections, symbols, "no OCRAM floor", "teensy41-release")

    wrong_ingress = deepcopy(symbols)
    wrong_ingress[0] = verify.Symbol(
        wrong_ingress[0].value, 8192, 11, wrong_ingress[0].name
    )
    expect_failure(sections, wrong_ingress, "expected 16384 B")

    recorder_staging = deepcopy(symbols)
    recorder_staging.append(
        verify.Symbol(0x2025BE80, 16384, 12, "_ZL18g_recorder_staging")
    )
    expect_failure(sections, recorder_staging, "sample-staging symbol")

    staging_bytes = deepcopy(sections)
    staging_bytes[12] = (".bss.aius_staging", 0x2025BE80, 16384)
    expect_failure(staging_bytes, symbols, "consumes 16384 OCRAM bytes")

    small_arena = deepcopy(symbols)
    small_arena[1] = verify.Symbol(
        small_arena[1].value, 65536, 9, small_arena[1].name
    )
    expect_failure(sections, small_arena, "expected 218688 B")

    low_headroom = deepcopy(symbols)
    low_headroom[2] = verify.Symbol(
        verify.OCRAM_END - floor + 1, 0, 12, "_heap_start",
    )
    expect_failure(sections, low_headroom, "below its committed")

    # Canonical RAM1 budgets: the post-4i link (slot-a, 2026-10-12) sits
    # exactly on its own figures.
    assert verify.canonical_budgets(sections, symbols) == (
        13284, 0x200494c0, 0x20007800
    )
    assert verify.ITCM_FREE_FLOOR_BYTES == 13284
    assert verify.DTCM_EBSS_CEILING == 0x200494c0
    assert verify.VECTORS_RAM_ADDRESS == 0x20007800

    def with_symbol(name, value):
        out = [s for s in symbols if s.name != name]
        old = next(s for s in symbols if s.name == name)
        out.append(verify.Symbol(value, old.size, old.section_index, name))
        return out

    # ITCM free 1 B under the floor (high-water grown) fails.
    itcm_full = deepcopy(sections)
    grown = 6 * 32768 - verify.ITCM_FREE_FLOOR_BYTES + 1
    itcm_full[13] = (".text.itcm", 0x0, grown)
    assert verify.canonical_budgets(itcm_full, symbols)[0] == (
        verify.ITCM_FREE_FLOOR_BYTES - 1
    )
    expect_failure(itcm_full, symbols, "ITCM free")
    # Exactly on the floor passes.
    itcm_edge = deepcopy(sections)
    itcm_edge[13] = (".text.itcm", 0x0, grown - 1)
    verify.verify_snapshot(itcm_edge, symbols, ENV)

    # DTCM statics 1 B over the ceiling fails; on it passes.
    expect_failure(
        sections, with_symbol("_ebss", verify.DTCM_EBSS_CEILING + 1),
        "_ebss",
    )
    verify.verify_snapshot(
        sections, with_symbol("_ebss", verify.DTCM_EBSS_CEILING), ENV
    )

    # `.data` growth that moves _VectorsRam to the next 1,024 B step fails.
    expect_failure(
        sections,
        with_symbol(
            "_VectorsRam",
            verify.VECTORS_RAM_ADDRESS + verify.VECTORS_RAM_STEP_BYTES,
        ),
        "_VectorsRam moved",
    )

    # Other envs are not held to the canonical budgets.
    other = with_symbol("_ebss", verify.DTCM_EBSS_CEILING + 4096)
    plain_sections, plain_symbols = fixture("teensy41-optical")
    plain_symbols = [s for s in plain_symbols if s.name != "_ebss"]
    plain_symbols.append(other[-1])
    verify.verify_snapshot(plain_sections, plain_symbols, "teensy41-optical")

    parsed = verify.parse_symbols(
        "  1: 20021720 0x35640 OBJECT GLOBAL DEFAULT 9 "
        "_ZN18AiusAudioProcessor14g_arena_memoryE\n"
    )
    assert parsed[0].size == verify.DECODER_ARENA_BYTES
    print("optical target memory verifier tests: PASS")


if __name__ == "__main__":
    main()
