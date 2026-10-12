#!/usr/bin/env python3
"""Mutation tests for the generated AC-3 fixture-stream validator."""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
from types import ModuleType


def load_validator(path: Path) -> ModuleType:
    spec = importlib.util.spec_from_file_location("iec_fixtures_validate", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load validator: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def expect_rejected(
    validator: ModuleType,
    scratch: Path,
    name: str,
    raw: bytes,
    iec: bytes,
    expected: str,
) -> None:
    raw_path = scratch / f"{name}.ac3"
    iec_path = scratch / f"{name}_ac3.iec"
    raw_path.write_bytes(raw)
    iec_path.write_bytes(iec)
    try:
        validator.validate_pair(raw_path, iec_path)
    except validator.ValidationError as error:
        if expected not in str(error):
            raise AssertionError(
                f"{name}: expected {expected!r} rejection, got {error!s}"
            ) from error
        return
    raise AssertionError(f"{name}: invalid stream was accepted")


def replace_raw_byte(raw: bytearray, iec: bytearray, index: int, value: int) -> None:
    raw[index] = value
    iec[8 + (index ^ 1)] = value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("validator", type=Path)
    parser.add_argument("generated", type=Path)
    parser.add_argument("scratch", type=Path)
    args = parser.parse_args()

    validator = load_validator(args.validator)
    args.scratch.mkdir(parents=True, exist_ok=True)
    raw = (args.generated / "channel_id_51.ac3").read_bytes()
    iec = (args.generated / "channel_id_51_ac3.iec").read_bytes()

    mutations: list[tuple[str, bytes, bytes, str]] = []
    for name, offset in (("wrong_pa", 0), ("wrong_pc", 4), ("wrong_pd", 6)):
        changed = bytearray(iec)
        changed[offset] ^= 1
        mutations.append((name, raw, bytes(changed), "preamble"))

    changed = bytearray(iec)
    changed[validator.PERIOD_BYTES - 1] = 1
    mutations.append(("nonzero_padding", raw, bytes(changed), "non-zero"))

    changed = bytearray(iec)
    changed[8 + 100] ^= 1
    mutations.append(("payload_mismatch", raw, bytes(changed), "differs from raw"))

    changed_raw = bytearray(raw)
    changed_iec = bytearray(iec)
    replace_raw_byte(
        changed_raw, changed_iec, 4, (changed_raw[4] & 0x3F) | 0x40
    )
    mutations.append(
        ("wrong_sample_rate", bytes(changed_raw), bytes(changed_iec), "fscod")
    )

    changed_raw = bytearray(raw)
    changed_iec = bytearray(iec)
    replace_raw_byte(
        changed_raw, changed_iec, 6, (changed_raw[6] & 0x1F) | (2 << 5)
    )
    mutations.append(
        ("wrong_channel_mode", bytes(changed_raw), bytes(changed_iec), "acmod/lfeon")
    )

    for name, candidate_raw, candidate_iec, expected in mutations:
        expect_rejected(
            validator,
            args.scratch,
            name,
            candidate_raw,
            candidate_iec,
            expected,
        )

    print(f"validator mutation test: PASS ({len(mutations)} rejects)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
