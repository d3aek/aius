#!/usr/bin/env python3
"""Regenerate the committed valid P1D0c supplemental-assembly fixture."""

from __future__ import annotations

import shutil
import sys
from pathlib import Path


SCRIPT_ROOT = Path(__file__).resolve().parents[2]
AIUS_ROOT = SCRIPT_ROOT.parent
TARGET = Path(__file__).resolve().parent / "valid"
sys.path.insert(0, str(SCRIPT_ROOT))

import seed_evidence  # noqa: E402
from test_supplemental_evidence import AssemblyFixture  # noqa: E402


def _formatter(root: Path, payload: bytes) -> bytes:
    del root
    return seed_evidence._production_formatter(AIUS_ROOT, payload)


def _format_json(path: Path, *, preserve_object_order: bool) -> None:
    value = seed_evidence._decode_json(path.read_bytes(), str(path))
    path.write_bytes(
        seed_evidence._render_json(
            AIUS_ROOT,
            value,
            seed_evidence._production_formatter,
            preserve_object_order=preserve_object_order,
        )
    )


def _adapt_validator_fixture(root: Path) -> None:
    production_manifest = AIUS_ROOT / "evidence" / "manifest"
    fixture_manifest = root / "evidence" / "manifest"

    production_boundary = production_manifest / "clone-boundary.json"
    (fixture_manifest / "clone-boundary.json").write_bytes(
        production_boundary.read_bytes()
    )

    production_seeds = production_manifest / "seed-sources.json"
    seed_payload = production_seeds.read_bytes()
    seed_rows = seed_evidence._decode_json(seed_payload, str(production_seeds))
    if not isinstance(seed_rows, list):
        raise RuntimeError("production seed sources are not an array")
    required_ids = {
        str(row["id"])
        for row in seed_rows
        if isinstance(row, dict) and row.get("required") is True
    }
    required_golden_ids = {
        str(row["id"])
        for row in seed_rows
        if isinstance(row, dict)
        and row.get("class") == "golden"
        and row.get("required") is True
    }
    if required_ids != required_golden_ids or len(required_golden_ids) != 11:
        raise RuntimeError(
            "production seed controls do not contain exactly 11 required golden rows"
        )
    marker = b'"required": true'
    if seed_payload.count(marker) != 11:
        raise RuntimeError(
            "production seed bytes do not contain exactly 11 required markers"
        )
    (fixture_manifest / "seed-sources.json").write_bytes(
        seed_payload.replace(marker, b'"required": false')
    )


def main() -> int:
    fixture = AssemblyFixture()
    try:
        _adapt_validator_fixture(fixture.root)
        for relative in (
            "evidence/manifest/fragments",
            "evidence/manifest/assumption-fragments",
            "evidence/manifest/supplemental-fragments",
            "evidence/manifest/supplemental-assumption-fragments",
        ):
            for path in sorted((fixture.root / relative).glob("*.json")):
                _format_json(path, preserve_object_order=False)
        for relative in (
            "evidence/manifest/requests",
            "evidence/design/components",
        ):
            for path in sorted((fixture.root / relative).glob("*.json")):
                _format_json(path, preserve_object_order=True)
        _format_json(
            fixture.manifest / "seed-realization.json",
            preserve_object_order=True,
        )
        result, _, error = fixture.run_assemble("--write", formatter=_formatter)
        if result != 0:
            raise RuntimeError(error)
        if TARGET.exists():
            shutil.rmtree(TARGET)
        shutil.copytree(
            fixture.root,
            TARGET,
            ignore=shutil.ignore_patterns(".git"),
        )
    finally:
        fixture.close()
    return seed_evidence.run(
        [
            "--root",
            str(TARGET),
            "supplemental",
            "assemble",
            "--check",
        ]
    )


if __name__ == "__main__":
    raise SystemExit(main())
