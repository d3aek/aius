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


def main() -> int:
    fixture = AssemblyFixture()
    try:
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
