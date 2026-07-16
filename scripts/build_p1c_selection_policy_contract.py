#!/usr/bin/env python3
"""Build the canonical Aius P1C selection-policy contract from its locked base."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import secrets
import subprocess
import sys
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path


SCRIPT_PATH = Path("scripts/build_p1c_selection_policy_contract.py")
SELECTION_BASE = Path(
    "scripts/fixtures/p1c-schema/selection-policy/base-v1.json"
)
SELECTION_HASH = "73b8b769e9137c29068120cebc4f13d419e1eb65484b4dac9a19e5403a7a7bc2"
SELECTION_CONTRACT = Path("evidence/design/contracts/selection-policy.json")

ROOT_KEYS = (
    "schema_version",
    "engineering_policies",
    "requirements",
    "families",
)
POLICY_KEYS = ("id", "scope", "rule", "validation_gate", "notes")
REQUIREMENT_KEYS = (
    "id",
    "statement",
    "basis_ids",
    "verification_stage",
    "notes",
)
MINIMAL_FAMILY_KEYS = ("family", "requirements", "dependent_feature", "notes")
DERIVED_FAMILY_KEYS = (
    "family",
    "component_file",
    "critical",
    "generic_eligible",
    "allowed_statuses",
    "requirements",
    "manufacturer_request_id",
    "availability_request_id",
    "manufacturer_source_id",
    "availability_source_id",
    "dependent_feature",
    "notes",
)

LOCKED_POLICY_IDS = (
    "EP-MANUFACTURABILITY-SMALL-RUN",
    "EP-PRODUCT-SINGLE-POWER-ENTRY",
    "EP-PROVISIONAL-GENERIC-PASSIVES",
    "EP-PROVISIONAL-TWO-LAYER-BOARD",
    "EP-SAFE-EXPANSION-UNPOWERED",
    "EP-SAFE-OPTICAL-DEFAULTS",
    "EP-SOURCING-MANUFACTURER-AUTHORITY",
    "EP-SOURCING-UK-AVAILABILITY",
)

LOCKED_REQUIREMENT_IDS = (
    "REQT-COMMON-ENCLOSURE-CLEARANCE",
    "REQT-COMMON-MANUFACTURABILITY",
    "REQT-COMMON-MANUFACTURER-EVIDENCE",
    "REQT-COMMON-UK-AVAILABILITY",
    "REQT-ETHERNET-CENTER-TAP-CAPACITOR",
    "REQT-ETHERNET-MAGJACK",
    "REQT-EXPANSION-GPIO-ADC-CONNECTOR",
    "REQT-EXPANSION-I2C-CONTROL-CONNECTOR",
    "REQT-EXPANSION-I2C-ISOLATION",
    "REQT-EXPANSION-I2S-TDM-CONNECTOR",
    "REQT-EXPANSION-IO-SERIES-RESISTOR",
    "REQT-EXPANSION-POWER-PROTECTION",
    "REQT-EXPANSION-SPI-CONNECTOR",
    "REQT-EXPANSION-UART-DEBUG-CONNECTOR",
    "REQT-EXPANSION-USB-HOST-CONNECTOR",
    "REQT-LOGIC-CONTROL-PULLDOWN",
    "REQT-LOGIC-DECOUPLING",
    "REQT-MECHANICAL-CABLE-RETAINER",
    "REQT-OPTICAL-BANDWIDTH",
    "REQT-OPTICAL-LOGIC-COMPATIBILITY",
    "REQT-OPTICAL-PROCESSED-DEFAULT",
    "REQT-REMOTE-PROGRAM-RTC",
    "REQT-TEENSY-ETHERNET-SOCKET",
    "REQT-TEENSY-SIDE-SOCKET",
    "REQT-TEENSY-USB-HOST-SOCKET",
    "REQT-TEST-FIXTURE-HEADER",
    "REQT-USB-A-RECEPTACLE",
    "REQT-USB-C-CC-PULLDOWN",
    "REQT-USB-C-RECEPTACLE",
    "REQT-USB-ESD",
    "REQT-USB-INTERNAL-CABLE",
    "REQT-USB-POWER-PROTECTION",
)

CRITICAL_FAMILIES = (
    "usb-c-receptacle",
    "usb-a-receptacle",
    "internal-usb-cable",
    "usb-esd",
    "usb-power-protection",
    "cliff-rx",
    "cliff-tx",
    "spdif-mux",
    "spdif-tx-gate",
    "ethernet-magjack",
    "teensy-side-socket",
    "teensy-ethernet-socket",
    "teensy-usb-host-socket",
    "expansion-power-protection",
    "expansion-i2c-isolator",
)
NONCRITICAL_FAMILIES = (
    "expansion-i2c-control",
    "expansion-spi",
    "expansion-uart-debug",
    "expansion-i2s-tdm",
    "expansion-usb-host",
    "expansion-gpio-adc",
    "remote-program-rtc",
    "test-fixture-header",
    "mechanical-cable-retainer",
)
GENERIC_FAMILIES = (
    "usb-c-cc-pulldown",
    "logic-decoupling-capacitor",
    "logic-control-pulldown",
    "ethernet-center-tap-capacitor",
    "expansion-io-series-resistor",
)
LOCKED_FAMILY_IDS = CRITICAL_FAMILIES + NONCRITICAL_FAMILIES + GENERIC_FAMILIES

CRITICAL_STATUSES = (
    "candidate-pending-evidence",
    "selected",
    "candidate-evidence-unavailable",
)
NONCRITICAL_STATUSES = CRITICAL_STATUSES + ("unresolved",)
GENERIC_STATUSES = ("generic-allowance",)

CLIFF_MANUFACTURER_SOURCES = {
    "cliff-rx": "CLIFF-ORJ5",
    "cliff-tx": "CLIFF-OTJ5",
}


@dataclass(frozen=True)
class Diagnostic:
    code: str
    path: Path
    detail: str

    def render(self) -> str:
        return (
            f"AIUS-P1C-CONTRACT-{self.code}: {self.path.as_posix()}: "
            f"{self.detail}"
        )


class WriteFailure(RuntimeError):
    """The generated output could not be atomically replaced."""


ProcessRunner = Callable[..., subprocess.CompletedProcess[bytes]]
Replace = Callable[[str, str], object]


def _locked_rows(
    document: dict[str, object],
    collection: str,
    row_keys: tuple[str, ...],
    identifier_key: str,
    expected_ids: tuple[str, ...],
    label: str,
) -> list[dict[str, object]]:
    rows = document.get(collection)
    if not isinstance(rows, list) or not all(isinstance(row, dict) for row in rows):
        raise ValueError(f"{collection} must be an array of objects")
    for row in rows:
        if tuple(row) != row_keys:
            raise ValueError(f"{label} {row.get(identifier_key)!r} has unexpected keys")
    if tuple(row.get(identifier_key) for row in rows) != expected_ids:
        raise ValueError(f"{label} ID order does not match the locked registry")
    return rows


def _validate_base(document: dict[str, object]) -> list[dict[str, object]]:
    if tuple(document) != ROOT_KEYS:
        raise ValueError("root keys do not match the locked order")
    if document["schema_version"] != "1.0.0":
        raise ValueError("schema version must be 1.0.0")
    _locked_rows(
        document,
        "engineering_policies",
        POLICY_KEYS,
        "id",
        LOCKED_POLICY_IDS,
        "policy",
    )
    _locked_rows(
        document,
        "requirements",
        REQUIREMENT_KEYS,
        "id",
        LOCKED_REQUIREMENT_IDS,
        "requirement",
    )
    families = _locked_rows(
        document,
        "families",
        MINIMAL_FAMILY_KEYS,
        "family",
        LOCKED_FAMILY_IDS,
        "family",
    )
    requirement_ids = set(LOCKED_REQUIREMENT_IDS)
    for row in families:
        requirements = row["requirements"]
        if (
            not isinstance(requirements, list)
            or not requirements
            or not all(isinstance(item, str) for item in requirements)
            or requirements != sorted(set(requirements))
            or not set(requirements) <= requirement_ids
        ):
            raise ValueError(
                f"family {row['family']!r} requirements are not the locked sorted IDs"
            )
    return families


def build_selection_contract(base: object) -> object:
    """Return a deep-copied normative contract derived from the immutable base."""

    if not isinstance(base, dict):
        raise ValueError("selection-policy base must be an object")
    document = copy.deepcopy(base)
    families = _validate_base(document)
    critical = set(CRITICAL_FAMILIES)
    noncritical = set(NONCRITICAL_FAMILIES)
    generic = set(GENERIC_FAMILIES)
    if (
        critical & noncritical
        or critical & generic
        or noncritical & generic
        or critical | noncritical | generic != set(LOCKED_FAMILY_IDS)
    ):
        raise ValueError("family groups do not exactly partition the locked families")

    derived = []
    for row in families:
        family = row["family"]
        upper = family.upper()
        is_critical = family in critical
        is_generic = family in generic
        if is_critical:
            statuses = CRITICAL_STATUSES
        elif family in noncritical:
            statuses = NONCRITICAL_STATUSES
        else:
            statuses = GENERIC_STATUSES
        if is_generic:
            manufacturer_request_id = None
            availability_request_id = None
            manufacturer_source_id = None
            availability_source_id = None
        else:
            manufacturer_request_id = f"REQ-{upper}-MFR"
            availability_request_id = f"REQ-{upper}-UK"
            manufacturer_source_id = CLIFF_MANUFACTURER_SOURCES.get(
                family, f"AIUS-{upper}-MFR"
            )
            availability_source_id = f"AIUS-{upper}-UK"
        derived.append(
            {
                "family": family,
                "component_file": f"components/{family}.json",
                "critical": is_critical,
                "generic_eligible": is_generic,
                "allowed_statuses": list(statuses),
                "requirements": copy.deepcopy(row["requirements"]),
                "manufacturer_request_id": manufacturer_request_id,
                "availability_request_id": availability_request_id,
                "manufacturer_source_id": manufacturer_source_id,
                "availability_source_id": availability_source_id,
                "dependent_feature": row["dependent_feature"],
                "notes": row["notes"],
            }
        )
    document["families"] = derived
    return document


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", help="Aius project root")
    parser.add_argument("--prettier", help="Prettier executable")
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--write", action="store_true")
    modes.add_argument("--check", action="store_true")
    return parser


def _default_prettier() -> Path:
    script_directory = Path(__file__).resolve().parent
    candidates = [
        directory / "node_modules" / ".bin" / "prettier"
        for directory in (script_directory, *script_directory.parents)
    ]
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return candidates[0]


def _load_base(root: Path) -> tuple[object | None, list[Diagnostic]]:
    path = root / SELECTION_BASE
    try:
        raw = path.read_bytes()
    except FileNotFoundError:
        return None, [Diagnostic("MISSING", SELECTION_BASE, "missing immutable base")]
    except OSError as error:
        return None, [
            Diagnostic("PARSE", SELECTION_BASE, f"cannot read base: {error}")
        ]

    diagnostics = []
    actual_hash = hashlib.sha256(raw).hexdigest()
    if actual_hash != SELECTION_HASH:
        diagnostics.append(
            Diagnostic(
                "HASH",
                SELECTION_BASE,
                f"SHA-256 {actual_hash} does not match locked {SELECTION_HASH}",
            )
        )
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        diagnostics.append(
            Diagnostic("PARSE", SELECTION_BASE, f"invalid UTF-8: {error}")
        )
        return None, diagnostics
    try:
        document = json.loads(text)
    except json.JSONDecodeError as error:
        diagnostics.append(
            Diagnostic("PARSE", SELECTION_BASE, f"malformed JSON: {error}")
        )
        return None, diagnostics
    if diagnostics:
        return None, diagnostics
    return document, []


def _same_ordered_json(left: object, right: object) -> bool:
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return list(left) == list(right) and all(
            _same_ordered_json(left[key], right[key]) for key in left
        )
    if isinstance(left, list):
        return len(left) == len(right) and all(
            _same_ordered_json(left_item, right_item)
            for left_item, right_item in zip(left, right)
        )
    return left == right


def _format_document(
    document: object,
    prettier: Path,
    runner: ProcessRunner,
) -> tuple[bytes | None, Diagnostic | None]:
    executable = os.fspath(prettier)
    if not prettier.is_file() or not os.access(prettier, os.X_OK):
        return None, Diagnostic(
            "FORMATTER",
            SCRIPT_PATH,
            f"formatter is missing or not executable: {executable}",
        )
    payload = json.dumps(document, ensure_ascii=False).encode("utf-8")
    try:
        completed = runner(
            [executable, "--parser", "json"],
            input=payload,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as error:
        return None, Diagnostic(
            "FORMATTER", SCRIPT_PATH, f"cannot run formatter {executable}: {error}"
        )
    if completed.returncode != 0:
        try:
            detail = completed.stderr.decode("utf-8").strip()
        except UnicodeDecodeError:
            detail = "non-UTF-8 stderr"
        suffix = f": {detail}" if detail else ""
        return None, Diagnostic(
            "FORMATTER",
            SCRIPT_PATH,
            f"formatter {executable} exited {completed.returncode}{suffix}",
        )
    try:
        text = completed.stdout.decode("utf-8")
    except UnicodeDecodeError as error:
        return None, Diagnostic(
            "FORMATTER",
            SCRIPT_PATH,
            f"formatter {executable} returned non-UTF-8 output: {error}",
        )
    if text.startswith("\ufeff"):
        detail = "returned a UTF-8 BOM"
    elif "\r" in text:
        detail = "returned non-LF line endings"
    else:
        try:
            rendered_value, end = json.JSONDecoder().raw_decode(text)
        except json.JSONDecodeError as error:
            detail = f"returned malformed JSON: {error}"
        else:
            if text[end:] != "\n":
                detail = "must return JSON followed by exactly one final LF"
            elif not _same_ordered_json(document, rendered_value):
                detail = "changed the JSON value or object-key order"
            else:
                return completed.stdout, None
    return None, Diagnostic(
        "FORMATTER", SCRIPT_PATH, f"formatter {executable} {detail}"
    )


def _derive_and_format(
    base: object,
    prettier: Path,
    runner: ProcessRunner,
) -> tuple[bytes | None, list[Diagnostic]]:
    try:
        document = build_selection_contract(base)
    except (KeyError, TypeError, ValueError) as error:
        return None, [
            Diagnostic("PARSE", SCRIPT_PATH, f"invalid base structure: {error}")
        ]
    payload, error = _format_document(document, prettier, runner)
    if error is not None:
        return None, [error]
    return payload, []


def _check_output(root: Path, expected: bytes) -> list[Diagnostic]:
    try:
        actual = (root / SELECTION_CONTRACT).read_bytes()
    except FileNotFoundError:
        return [
            Diagnostic("MISSING", SELECTION_CONTRACT, "missing generated output")
        ]
    except OSError as error:
        return [Diagnostic("STALE", SELECTION_CONTRACT, f"cannot read output: {error}")]
    if actual != expected:
        return [Diagnostic("STALE", SELECTION_CONTRACT, "generated bytes do not match")]
    return []


def _atomic_write(
    root: Path,
    relative: Path,
    payload: bytes,
    replace: Replace,
) -> None:
    target = root / relative
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        raise WriteFailure(str(error)) from error
    temporary = target.with_name(
        f".{target.name}.aius-p1c-{secrets.token_hex(8)}.tmp"
    )
    descriptor: int | None = None
    owns_temporary = False
    try:
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        owns_temporary = True
        with os.fdopen(descriptor, "wb", closefd=True) as handle:
            descriptor = None
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        replace(os.fspath(temporary), os.fspath(target))
    except OSError as error:
        if descriptor is not None:
            try:
                os.close(descriptor)
            except OSError:
                pass
        raise WriteFailure(str(error)) from error
    finally:
        if owns_temporary:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass


def _emit(diagnostics: list[Diagnostic]) -> None:
    for message in sorted(diagnostic.render() for diagnostic in diagnostics):
        print(message, file=sys.stderr)


def run(
    argv: list[str] | None = None,
    *,
    runner: ProcessRunner = subprocess.run,
    replace: Replace = os.replace,
) -> int:
    arguments = _parser().parse_args(argv)
    root = (
        Path(arguments.root)
        if arguments.root is not None
        else Path(__file__).resolve().parents[1]
    )
    prettier = (
        Path(arguments.prettier)
        if arguments.prettier is not None
        else _default_prettier()
    )

    base, diagnostics = _load_base(root)
    if diagnostics:
        _emit(diagnostics)
        return 1
    output, diagnostics = _derive_and_format(base, prettier, runner)
    if diagnostics:
        _emit(diagnostics)
        return 1
    assert output is not None

    if arguments.check:
        diagnostics = _check_output(root, output)
        if diagnostics:
            _emit(diagnostics)
            return 1
    else:
        try:
            _atomic_write(root, SELECTION_CONTRACT, output, replace)
        except WriteFailure as error:
            _emit([Diagnostic("WRITE", SELECTION_CONTRACT, str(error))])
            return 1

    print("OK build-p1c-selection-policy-contract")
    return 0


def main(argv: list[str] | None = None) -> int:
    return run(argv)


if __name__ == "__main__":
    raise SystemExit(main())
