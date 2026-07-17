#!/usr/bin/env python3
"""Check formatting for the exact closed P1D2 lifecycle input set."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

from validate_p1 import CRITICAL_FAMILIES, FAMILIES, NONCRITICAL_FAMILIES


SCRIPT_PATH = Path(__file__).resolve()
AIUS_ROOT = SCRIPT_PATH.parents[1]
REPOSITORY_ROOT = SCRIPT_PATH.parents[4]

REALIZATION = AIUS_ROOT / "evidence" / "manifest" / "supplemental-realization.json"
REQUEST_DIRECTORY = AIUS_ROOT / "evidence" / "manifest" / "requests"
COMPONENT_DIRECTORY = AIUS_ROOT / "evidence" / "design" / "components"
STATIC_INPUTS = (
    AIUS_ROOT / "evidence" / "manifest" / "sources.json",
    AIUS_ROOT / "evidence" / "manifest" / "assumptions.json",
    AIUS_ROOT / "evidence" / "manifest" / "supplemental-clone-boundary.json",
    AIUS_ROOT / "evidence" / "manifest" / "supplemental-requests.json",
)

REALIZATION_COUNT = 46
REQUEST_MEMBER_COUNT = 48
COMPONENT_COUNT = 29
OPERAND_COUNT = 79


class CheckError(ValueError):
    """The closed P1D2 formatting input contract is not satisfied."""


def _load_realization_ids() -> tuple[str, ...]:
    try:
        with REALIZATION.open(encoding="utf-8") as handle:
            document = json.load(handle)
    except (OSError, json.JSONDecodeError) as error:
        raise CheckError(f"cannot load supplemental realization: {error}") from error
    if not isinstance(document, dict) or not isinstance(document.get("realizations"), list):
        raise CheckError("supplemental realization must contain a realizations array")
    rows = document["realizations"]
    if len(rows) != REALIZATION_COUNT:
        raise CheckError(f"supplemental realization must contain exactly {REALIZATION_COUNT} rows")
    request_ids: list[str] = []
    for index, row in enumerate(rows):
        if not isinstance(row, dict) or not isinstance(row.get("request_id"), str):
            raise CheckError(f"supplemental realization row {index} has no string request_id")
        request_ids.append(row["request_id"])
    if len(set(request_ids)) != REALIZATION_COUNT:
        raise CheckError("supplemental realization request IDs must be unique")
    return tuple(request_ids)


def _direct_child(directory: Path, filename: str, label: str) -> Path:
    candidate = directory / filename
    if candidate.parent != directory:
        raise CheckError(f"{label} is not a lexical direct child of {directory}")
    return candidate


def _expected_request_paths() -> tuple[Path, ...]:
    lifecycle_families = (*CRITICAL_FAMILIES, *NONCRITICAL_FAMILIES)
    if not all(isinstance(family, str) for family in lifecycle_families):
        raise CheckError("validator lifecycle family names must be strings")
    request_ids = tuple(
        request_id
        for family in lifecycle_families
        for request_id in (f"REQ-{family.upper()}-MFR", f"REQ-{family.upper()}-UK")
    )
    if len(request_ids) != REQUEST_MEMBER_COUNT or len(set(request_ids)) != REQUEST_MEMBER_COUNT:
        raise CheckError(f"validator lifecycle families must define exactly {REQUEST_MEMBER_COUNT} request IDs")
    return tuple(
        _direct_child(REQUEST_DIRECTORY, f"{request_id}.json", f"request {request_id}")
        for request_id in request_ids
    )


def _expected_component_paths() -> tuple[Path, ...]:
    if not all(isinstance(family, str) for family in FAMILIES):
        raise CheckError("validator component family names must be strings")
    if len(FAMILIES) != COMPONENT_COUNT or len(set(FAMILIES)) != COMPONENT_COUNT:
        raise CheckError(f"validator FAMILIES must contain exactly {COMPONENT_COUNT} unique families")
    return tuple(
        _direct_child(COMPONENT_DIRECTORY, f"{family}.json", f"component {family}")
        for family in FAMILIES
    )


def _json_members(directory: Path, label: str) -> set[str]:
    try:
        return {candidate.name for candidate in directory.iterdir() if candidate.name.endswith(".json")}
    except OSError as error:
        raise CheckError(f"cannot inspect {label} directory: {error}") from error


def _require_exact_members(directory: Path, expected: tuple[Path, ...], label: str) -> None:
    expected_names = {path.name for path in expected}
    actual_names = _json_members(directory, label)
    if actual_names != expected_names:
        missing = sorted(expected_names - actual_names)
        extra = sorted(actual_names - expected_names)
        raise CheckError(f"{label} JSON membership differs; missing={missing!r}, extra={extra!r}")


def _resolved_directory(directory: Path, label: str) -> Path:
    try:
        resolved = directory.resolve(strict=True)
    except OSError as error:
        raise CheckError(f"cannot resolve {label} directory: {error}") from error
    if not resolved.is_dir():
        raise CheckError(f"{label} directory is not a directory")
    return resolved


def _require_beneath(path: Path, root: Path, label: str) -> None:
    try:
        path.relative_to(root)
    except ValueError as error:
        raise CheckError(f"{label} resolves outside {root}") from error


def _validated_operand(path: Path, aius_root: Path, containment_root: Path | None = None) -> str:
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise CheckError(f"formatter operand is missing or cannot resolve: {path}: {error}") from error
    if not resolved.is_file():
        raise CheckError(f"formatter operand is not a file: {path}")
    _require_beneath(resolved, aius_root, os.fspath(path))
    if containment_root is not None:
        _require_beneath(resolved, containment_root, os.fspath(path))
    try:
        return path.relative_to(REPOSITORY_ROOT).as_posix()
    except ValueError as error:
        raise CheckError(f"formatter operand is not beneath the repository root: {path}") from error


def _formatter_operands() -> list[str]:
    realization_ids = _load_realization_ids()
    realization_requests = tuple(
        _direct_child(REQUEST_DIRECTORY, f"{request_id}.json", f"request {request_id}")
        for request_id in realization_ids
    )
    request_members = _expected_request_paths()
    component_members = _expected_component_paths()
    _require_exact_members(REQUEST_DIRECTORY, request_members, "request")
    _require_exact_members(COMPONENT_DIRECTORY, component_members, "component")

    aius_root = _resolved_directory(AIUS_ROOT, "Aius root")
    request_root = _resolved_directory(REQUEST_DIRECTORY, "request")
    component_root = _resolved_directory(COMPONENT_DIRECTORY, "component")
    validated_requests = {
        path: _validated_operand(path, aius_root, request_root) for path in request_members
    }
    unknown_realization_requests = set(realization_requests) - set(request_members)
    if unknown_realization_requests:
        raise CheckError(
            "supplemental realization contains request IDs outside the closed lifecycle set"
        )
    operands = [
        *(_validated_operand(path, aius_root) for path in STATIC_INPUTS),
        *(validated_requests[path] for path in realization_requests),
        *(_validated_operand(path, aius_root, component_root) for path in component_members),
    ]
    if len(operands) != OPERAND_COUNT or len(set(operands)) != OPERAND_COUNT:
        raise CheckError(f"formatter must receive exactly {OPERAND_COUNT} unique operands")
    return sorted(operands)


def main() -> int:
    if len(sys.argv) != 1:
        print("check_p1d2_format.py accepts no arguments", file=sys.stderr)
        return 2
    try:
        operands = _formatter_operands()
    except CheckError as error:
        print(f"P1D2 format preflight failed: {error}", file=sys.stderr)
        return 2

    executable = REPOSITORY_ROOT / "node_modules" / ".bin" / "prettier"
    try:
        completed = subprocess.run(
            [os.fspath(executable), "--check", *operands],
            cwd=REPOSITORY_ROOT,
            check=False,
            shell=False,
        )
    except OSError as error:
        print(f"cannot run pinned Prettier: {error}", file=sys.stderr)
        return 2
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
