#!/usr/bin/env python3
"""Generate deterministic eligible and blocked P1 lifecycle fixtures."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


SCRIPT_ROOT = Path(__file__).resolve().parents[3]
AIUS_ROOT = SCRIPT_ROOT.parent
OUTPUT_ROOT = Path(__file__).resolve().parent
sys.path.insert(0, os.fspath(SCRIPT_ROOT))

import seed_evidence  # noqa: E402
from test_supplemental_evidence import (  # noqa: E402
    AssemblyFixture,
    UNRESOLVED_REQUESTS,
)


VALIDATOR_INPUTS = (
    "evidence/design/functional-topology-schema.json",
    "evidence/design/interfaces-and-states-schema.json",
    "evidence/design/selection-policy-schema.json",
    "evidence/design/functional-topology.json",
    "evidence/design/interfaces-and-states.json",
    "evidence/design/contracts/functional-topology.json",
    "evidence/design/contracts/interfaces-and-states.json",
    "evidence/design/contracts/selection-policy.json",
    "scripts/fixtures/p1c-schema/functional-topology/base-v1.json",
    "scripts/fixtures/p1c-schema/interfaces-and-states/base-v1.json",
    "scripts/fixtures/p1c-schema/selection-policy/base-v1.json",
    "docs/reference/functional-topology.md",
    "docs/reference/interfaces-and-states.md",
)
SEED_INPUTS = (
    "seed-sources.json",
    "seed-realization.json",
)
SEED_DIRECTORIES = (
    "fragments",
    "assumption-fragments",
)
BLOCKED_MANUFACTURER = "REQ-ETHERNET-MAGJACK-MFR"
SYNTHETIC_PAYLOADS = {
    "application/pdf": b"%PDF-1.7\nfixture\n",
    "application/zip": b"PK\x03\x04fixture",
    "image/png": b"\x89PNG\r\n\x1a\nfixture",
    "text/html": b"<!doctype html><html></html>\n",
    "text/x-rst": b"Fixture reference\n",
    "text/x-c++": b"int fixture() { return 0; }\n",
}


def _copy_file(root: Path, relative: str) -> None:
    source = AIUS_ROOT / relative
    target = root / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)


def _restore_current_seed_inputs(fixture: AssemblyFixture) -> None:
    manifest = AIUS_ROOT / "evidence" / "manifest"
    for name in SEED_INPUTS:
        shutil.copy2(manifest / name, fixture.manifest / name)
    for name in SEED_DIRECTORIES:
        target = fixture.manifest / name
        shutil.rmtree(target)
        shutil.copytree(manifest / name, target)
    realizations = json.loads(
        (fixture.manifest / "seed-realization.json").read_text(encoding="utf-8")
    )
    realizations_by_id = {row["source_id"]: row for row in realizations}
    for fragment_path in sorted((fixture.manifest / "fragments").glob("*.json")):
        source = json.loads(fragment_path.read_text(encoding="utf-8"))
        artifacts = source["artifacts"]
        changed = False
        for artifact in artifacts:
            relative = artifact["path"]
            if relative is None:
                continue
            payload = SYNTHETIC_PAYLOADS[artifact["media_type"]]
            target = fixture.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(payload)
            artifact["sha256"] = hashlib.sha256(payload).hexdigest()
            artifact["bytes"] = len(payload)
            changed = True
        if not changed:
            continue
        source["revision"] = seed_evidence._expected_revision(
            realizations_by_id[source["id"]],
            artifacts,
            source["retrieved_at"],
        )
        fragment_path.write_bytes(
            seed_evidence._render_json(
                fixture.root,
                source,
                seed_evidence._test_formatter,
            )
        )


def _rerender_supplemental_fragments(fixture: AssemblyFixture) -> None:
    for directory in (
        "supplemental-fragments",
        "supplemental-assumption-fragments",
    ):
        for path in sorted((fixture.manifest / directory).glob("*.json")):
            value = json.loads(path.read_text(encoding="utf-8"))
            path.write_bytes(
                seed_evidence._render_json(
                    fixture.root,
                    value,
                    seed_evidence._test_formatter,
                )
            )


def _populate(fixture: AssemblyFixture, *, blocked: bool) -> None:
    manufacturer_ids = {
        str(row["request_id"])
        for row in fixture.rows
        if row["clone_boundary"] is not None
    }
    confirmed_uk_ids = {
        str(row["request_id"])
        for row in fixture.rows
        if row["observation_profile"] is not None
        and row["authorization"]["status"] == "confirmed"
    }
    unresolved_uk_ids = {
        str(row["request_id"])
        for row in fixture.rows
        if row["observation_profile"] is not None
        and row["authorization"]["status"] == "unresolved"
    }
    if len(manufacturer_ids) != 22:
        raise AssertionError("fixture must contain 22 supplemental manufacturers")
    if len(confirmed_uk_ids) != 22:
        raise AssertionError("fixture must contain 22 confirmed UK requests")
    if unresolved_uk_ids != set(UNRESOLVED_REQUESTS):
        raise AssertionError("fixture unresolved UK request set differs")
    if blocked:
        manufacturer_ids.remove(BLOCKED_MANUFACTURER)
    fixture.populate_supplemental(
        current_manufacturer_ids=manufacturer_ids,
        current_uk_ids=confirmed_uk_ids,
    )


def _format_json_tree(root: Path) -> None:
    git_root = seed_evidence._git_root(AIUS_ROOT, subprocess.run)
    executable = git_root / "node_modules" / ".bin" / "prettier"
    config = git_root / ".prettierrc.json"
    seed_evidence._production_formatter(AIUS_ROOT, b"{}")
    json_paths = sorted(root.rglob("*.json"))
    seed_evidence._completed_process(
        subprocess.run,
        [
            os.fspath(executable),
            "--write",
            "--config",
            os.fspath(config),
            *(os.fspath(path) for path in json_paths),
        ],
        cwd=git_root,
        label="fixture JSON formatter",
    )


def _build(*, blocked: bool) -> AssemblyFixture:
    fixture = AssemblyFixture()
    _restore_current_seed_inputs(fixture)
    _populate(fixture, blocked=blocked)
    _rerender_supplemental_fragments(fixture)
    result = fixture.run_assemble("--write")
    if result != (0, "", ""):
        fixture.close()
        raise AssertionError(f"supplemental assembly failed: {result!r}")
    for relative in VALIDATOR_INPUTS:
        _copy_file(fixture.root, relative)
    _format_json_tree(fixture.root)
    return fixture


def _copy_root(source: Path, target: Path) -> None:
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    for child in sorted(source.iterdir(), key=lambda path: path.name):
        if child.name == ".git":
            continue
        destination = target / child.name
        if child.is_dir():
            shutil.copytree(child, destination)
        else:
            shutil.copy2(child, destination)


def _files(root: Path) -> dict[str, bytes]:
    if not root.is_dir():
        return {}
    return {
        path.relative_to(root).as_posix(): path.read_bytes()
        for path in sorted(root.rglob("*"))
        if path.is_file() and ".git" not in path.relative_to(root).parts
    }


def _check_root(expected: Path, actual: Path) -> list[str]:
    expected_files = _files(expected)
    actual_files = _files(actual)
    messages: list[str] = []
    missing = sorted(expected_files.keys() - actual_files.keys())
    extra = sorted(actual_files.keys() - expected_files.keys())
    changed = sorted(
        relative
        for relative in expected_files.keys() & actual_files.keys()
        if expected_files[relative] != actual_files[relative]
    )
    if missing:
        messages.append(f"{actual.name}: missing={missing!r}")
    if extra:
        messages.append(f"{actual.name}: extra={extra!r}")
    if changed:
        messages.append(f"{actual.name}: changed={changed!r}")
    return messages


def run(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    arguments = parser.parse_args(argv)

    fixtures = {
        "eligible": _build(blocked=False),
        "blocked": _build(blocked=True),
    }
    try:
        if arguments.write:
            for name, fixture in fixtures.items():
                _copy_root(fixture.root, OUTPUT_ROOT / name)
            return 0
        messages = [
            message
            for name, fixture in fixtures.items()
            for message in _check_root(fixture.root, OUTPUT_ROOT / name)
        ]
        if messages:
            for message in messages:
                print(message, file=sys.stderr)
            return 1
        return 0
    finally:
        for fixture in fixtures.values():
            fixture.close()


if __name__ == "__main__":
    raise SystemExit(run())
