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


def fixture():
    sections = {
        9: (".bss", 0x20020000, 0x22000),
        11: (".bss.dma", 0x202000A0, 0x5FDE0),
        12: (".bss.aius_staging", 0x2025FE80, 0),
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
        verify.Symbol(0x2025FE80, 0, 12, "_heap_start"),
        verify.Symbol(verify.OCRAM_END, 0, None, "_heap_end"),
    ]
    return sections, symbols


def expect_failure(sections, symbols, needle):
    try:
        verify.verify_snapshot(sections, symbols)
    except verify.VerificationError as error:
        assert needle in str(error), f"wrong failure: {error}"
        return
    raise AssertionError(f"expected verifier failure containing {needle!r}")


def main():
    sections, symbols = fixture()
    assert verify.verify_snapshot(sections, symbols) == (131456, 0)

    no_empty_output = deepcopy(sections)
    del no_empty_output[12]
    assert verify.verify_snapshot(no_empty_output, symbols) == (131456, 0)

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
    expect_failure(sections, small_arena, "expected 131072 B")

    low_headroom = deepcopy(symbols)
    low_headroom[2] = verify.Symbol(
        verify.OCRAM_END - verify.COMMITTED_OCRAM_FREE_BYTES + 1,
        0, 12, "_heap_start",
    )
    expect_failure(sections, low_headroom, "below committed")

    parsed = verify.parse_symbols(
        "  1: 20021720 0x20000 OBJECT GLOBAL DEFAULT 9 "
        "_ZN18AiusAudioProcessor14g_arena_memoryE\n"
    )
    assert parsed[0].size == verify.DECODER_ARENA_BYTES
    print("optical target memory verifier tests: PASS")


if __name__ == "__main__":
    main()
