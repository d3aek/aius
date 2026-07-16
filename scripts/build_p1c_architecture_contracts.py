#!/usr/bin/env python3
"""Build the canonical Aius P1C architecture contracts from locked snapshots."""

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


SCRIPT_PATH = Path("scripts/build_p1c_architecture_contracts.py")

FUNCTIONAL_BASE = Path("scripts/fixtures/p1c-schema/functional-topology/base-v1.json")
INTERFACES_BASE = Path(
    "scripts/fixtures/p1c-schema/interfaces-and-states/base-v1.json"
)
FUNCTIONAL_HASH = "17e07765a7ef2de03e0722f3b393f8dafde48ec2d854b381c47d51073b0d3257"
INTERFACES_HASH = "31ea644c922515c0fa6193e67a7432b8b911083af96d3290a1b34959800fcd84"

FUNCTIONAL_CONTRACT = Path("evidence/design/contracts/functional-topology.json")
INTERFACES_CONTRACT = Path("evidence/design/contracts/interfaces-and-states.json")
FUNCTIONAL_VALID = Path("scripts/fixtures/p1c-schema/functional-topology/valid.json")
INTERFACES_VALID = Path(
    "scripts/fixtures/p1c-schema/interfaces-and-states/valid.json"
)

EVIDENCE_BUNDLES = {
    "EV-AUDIO": (
        "PJRC-IMXRT1060-RM",
        "PJRC-SPDIF-RX-SOURCE",
        "PJRC-SPDIF-TX-SOURCE",
    ),
    "EV-CORE": (
        "PJRC-IMXRT1060-RM",
        "PJRC-TEENSY41-PAGE",
        "PJRC-TEENSY41-SCHEMATIC",
    ),
    "EV-ETH": ("PJRC-ETHERNET-KIT", "PJRC-IMXRT1060-RM"),
    "EV-EXP": ("PJRC-IMXRT1060-RM", "PJRC-TEENSY41-PINOUT"),
    "EV-OPTICAL": ("CLIFF-ORJ5", "CLIFF-OTJ5", "VKSDR-TOSLINK-SCHEMATICS"),
    "EV-PIN": ("PJRC-TEENSY41-PINOUT",),
    "EV-RX": (
        "CLIFF-ORJ5",
        "PJRC-SPDIF-RX-SOURCE",
        "VKSDR-TOSLINK-SCHEMATICS",
    ),
    "EV-SYSTEM": ("PJRC-ETHERNET-KIT", "PJRC-TEENSY41-SCHEMATIC"),
    "EV-TX": (
        "CLIFF-OTJ5",
        "PJRC-SPDIF-TX-SOURCE",
        "VKSDR-TOSLINK-SCHEMATICS",
    ),
    "EV-USB": ("PJRC-TEENSY41-SCHEMATIC",),
}
ROOT_EVIDENCE_IDS = tuple(
    sorted({source_id for bundle in EVIDENCE_BUNDLES.values() for source_id in bundle})
)

FUNCTIONAL_EVIDENCE = {
    "modes": {
        "pcm-processed": "EV-AUDIO",
        "dts-processed": "EV-AUDIO",
        "diagnostic-raw-bypass": "EV-OPTICAL",
        "unsupported-muted": "EV-AUDIO",
        "fault-muted": "EV-CORE",
    },
    "blocks": {
        "power-entry": "EV-USB",
        "usb-bridge": "EV-USB",
        "teensy": "EV-CORE",
        "optical-rx": "EV-RX",
        "spdif-capture": "EV-RX",
        "input-classifier": "EV-AUDIO",
        "dts-decoder": "EV-AUDIO",
        "pcm-normalizer": "EV-AUDIO",
        "downmix-eq": "EV-AUDIO",
        "spdif-transmit": "EV-TX",
        "raw-bypass-mux": "EV-OPTICAL",
        "tx-gate": "EV-OPTICAL",
        "optical-tx": "EV-TX",
        "ethernet": "EV-ETH",
        "expansion": "EV-EXP",
    },
    "flows": {
        "optical-to-capture": "EV-RX",
        "capture-to-classifier": "EV-RX",
        "classifier-to-dts": "EV-AUDIO",
        "classifier-to-pcm": "EV-AUDIO",
        "dts-to-normalizer": "EV-AUDIO",
        "pcm-to-normalizer": "EV-AUDIO",
        "normalizer-to-dsp": "EV-AUDIO",
        "dsp-to-transmit": "EV-AUDIO",
        "transmit-to-mux": "EV-TX",
        "optical-to-raw-mux": "EV-OPTICAL",
        "mux-to-gate": "EV-OPTICAL",
        "gate-to-optical": "EV-TX",
        "path-select-control": "EV-OPTICAL",
        "tx-enable-control": "EV-OPTICAL",
        "watchdog-reset-control": "EV-CORE",
        "usb-vbus-entry": "EV-USB",
        "protected-vbus": "EV-USB",
        "teensy-power": "EV-CORE",
        "carrier-3v3": "EV-CORE",
        "expansion-power": "EV-EXP",
    },
    "clock_domains": {
        "spdif-recovered": "EV-RX",
        "audio-processing": "EV-AUDIO",
        "spdif-transmit-clock": "EV-TX",
        "usb-device-clock": "EV-USB",
        "ethernet-rmii-clock": "EV-ETH",
    },
}

INTERFACES_EVIDENCE = {
    "connectors": {
        "rear-usb-c": "EV-USB",
        "internal-usb-a": "EV-USB",
        "teensy-micro-b": "EV-USB",
        "toslink-rx": "EV-RX",
        "toslink-tx": "EV-TX",
        "ethernet-rj45": "EV-ETH",
        "microsd-access": "EV-PIN",
        "remote-program-rtc": "EV-PIN",
        "test-fixture": "EV-EXP",
    },
    "controls": {
        "spdif-path-select": "EV-OPTICAL",
        "optical-tx-enable": "EV-OPTICAL",
        "teensy-reset": "EV-CORE",
        "teensy-watchdog": "EV-CORE",
        "program-control": "EV-PIN",
        "status-led": "EV-CORE",
    },
    "expansion_interfaces": {
        "expansion-i2c-control": "EV-EXP",
        "expansion-spi": "EV-EXP",
        "expansion-uart-debug": "EV-EXP",
        "expansion-i2s-tdm": "EV-EXP",
        "expansion-usb-host": "EV-EXP",
        "expansion-gpio-adc": "EV-EXP",
    },
    "power_domains": {
        "usb-vbus-raw": "EV-USB",
        "protected-5v": "EV-USB",
        "teensy-3v3": "EV-CORE",
        "digital-ground": "EV-CORE",
        "chassis-shield": "EV-SYSTEM",
        "expansion-3v3": "EV-EXP",
        "expansion-usb-host-vbus": "EV-EXP",
    },
}

TRANSITION_ENDPOINTS = {
    "power-on-to-reset": (("power-off",), ("powered-reset",)),
    "reset-to-boot": (("powered-reset",), ("boot-muted",)),
    "boot-to-waiting": (("boot-muted",), ("waiting-for-carrier",)),
    "waiting-to-pcm": (("waiting-for-carrier",), ("pcm-active",)),
    "waiting-to-dts": (("waiting-for-carrier",), ("dts-active",)),
    "active-to-carrier-loss": (
        ("pcm-active", "dts-active", "unsupported-muted", "diagnostic-bypass"),
        ("waiting-for-carrier",),
    ),
    "active-to-unsupported": (
        ("pcm-active", "dts-active"),
        ("unsupported-muted",),
    ),
    "unsupported-to-active": (
        ("unsupported-muted",),
        ("pcm-active", "dts-active"),
    ),
    "active-to-diagnostic-bypass": (
        ("pcm-active", "dts-active", "unsupported-muted"),
        ("diagnostic-bypass",),
    ),
    "diagnostic-bypass-to-waiting": (
        ("diagnostic-bypass",),
        ("waiting-for-carrier",),
    ),
    "watchdog-to-reset": (
        (
            "boot-muted",
            "waiting-for-carrier",
            "pcm-active",
            "dts-active",
            "unsupported-muted",
            "diagnostic-bypass",
            "fault-muted",
        ),
        ("powered-reset",),
    ),
    "usb-unplug-to-power-off": (
        (
            "powered-reset",
            "boot-muted",
            "waiting-for-carrier",
            "pcm-active",
            "dts-active",
            "unsupported-muted",
            "diagnostic-bypass",
            "fault-muted",
        ),
        ("power-off",),
    ),
}

LEGACY_TRANSITION_KEYS = (
    "id",
    "from_state",
    "to_state",
    "triggers",
    "preconditions",
    "actions",
    "postconditions",
    "notes",
)


@dataclass(frozen=True)
class Diagnostic:
    code: str
    path: Path
    detail: str

    def render(self) -> str:
        return (
            f"AIUS-P1C-CONTRACT-{self.code}: {self.path.as_posix()}: {self.detail}"
        )


class WriteFailure(RuntimeError):
    """A generated output could not be atomically replaced."""


ProcessRunner = Callable[..., subprocess.CompletedProcess[bytes]]
Replace = Callable[[str, str], object]


def _copy_document(base: object, label: str) -> dict[str, object]:
    if not isinstance(base, dict):
        raise ValueError(f"{label} base must be an object")
    return copy.deepcopy(base)


def _assign_evidence(
    document: dict[str, object],
    assignments: dict[str, dict[str, str]],
) -> None:
    for collection, rows_to_bundles in assignments.items():
        rows = document.get(collection)
        if not isinstance(rows, list) or not all(isinstance(row, dict) for row in rows):
            raise ValueError(f"{collection} must be an array of objects")
        actual_ids = [row.get("id") for row in rows]
        if set(actual_ids) != set(rows_to_bundles) or len(actual_ids) != len(
            rows_to_bundles
        ):
            raise ValueError(f"{collection} does not contain the exact locked row IDs")
        for row in rows:
            row_id = row["id"]
            bundle = rows_to_bundles[row_id]
            row["evidence_ids"] = list(EVIDENCE_BUNDLES[bundle])
    document["evidence_ids"] = list(ROOT_EVIDENCE_IDS)


def build_functional_contract(base: object) -> object:
    """Return the functional contract derived from the immutable P1C0a base."""

    document = _copy_document(base, "functional-topology")
    _assign_evidence(document, FUNCTIONAL_EVIDENCE)
    return document


def build_interfaces_contract(base: object) -> object:
    """Return the interface/state contract derived from the immutable P1C0a base."""

    document = _copy_document(base, "interfaces-and-states")
    _assign_evidence(document, INTERFACES_EVIDENCE)
    transitions = document.get("transitions")
    if not isinstance(transitions, list) or not all(
        isinstance(row, dict) for row in transitions
    ):
        raise ValueError("transitions must be an array of objects")
    actual_ids = [row.get("id") for row in transitions]
    if tuple(actual_ids) != tuple(TRANSITION_ENDPOINTS):
        raise ValueError("transitions do not have the exact locked ID order")

    migrated = []
    for row in transitions:
        if tuple(row) != LEGACY_TRANSITION_KEYS:
            raise ValueError(f"transition {row.get('id')!r} has unexpected base keys")
        row_id = row["id"]
        from_states, to_states = TRANSITION_ENDPOINTS[row_id]
        migrated_row = {
            "id": row_id,
            "from_states": list(from_states),
            "to_states": list(to_states),
            "triggers": copy.deepcopy(row["triggers"]),
            "preconditions": copy.deepcopy(row["preconditions"]),
            "actions": copy.deepcopy(row["actions"]),
            "postconditions": copy.deepcopy(row["postconditions"]),
            "notes": row["notes"],
        }
        if row_id == "active-to-carrier-loss":
            migrated_row["actions"] = [
                "disable-tx",
                "wait-muted",
                "select-processed",
                "clear-pipeline",
            ]
            migrated_row["postconditions"] = ["processed-selected", "tx-disabled"]
        migrated.append(migrated_row)
    document["transitions"] = migrated
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


def _load_bases(root: Path) -> tuple[dict[str, object], list[Diagnostic]]:
    documents: dict[str, object] = {}
    diagnostics: list[Diagnostic] = []
    for name, relative, expected_hash in (
        ("functional", FUNCTIONAL_BASE, FUNCTIONAL_HASH),
        ("interfaces", INTERFACES_BASE, INTERFACES_HASH),
    ):
        path = root / relative
        try:
            raw = path.read_bytes()
        except FileNotFoundError:
            diagnostics.append(Diagnostic("MISSING", relative, "missing immutable base"))
            continue
        except OSError as error:
            diagnostics.append(Diagnostic("PARSE", relative, f"cannot read base: {error}"))
            continue

        actual_hash = hashlib.sha256(raw).hexdigest()
        if actual_hash != expected_hash:
            diagnostics.append(
                Diagnostic(
                    "HASH",
                    relative,
                    f"SHA-256 {actual_hash} does not match locked {expected_hash}",
                )
            )
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError as error:
            diagnostics.append(Diagnostic("PARSE", relative, f"invalid UTF-8: {error}"))
            continue
        try:
            documents[name] = json.loads(text)
        except json.JSONDecodeError as error:
            diagnostics.append(Diagnostic("PARSE", relative, f"malformed JSON: {error}"))

    if diagnostics:
        return {}, diagnostics
    return documents, []


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
    bases: dict[str, object],
    prettier: Path,
    runner: ProcessRunner,
) -> tuple[dict[Path, bytes], list[Diagnostic]]:
    try:
        functional = build_functional_contract(bases["functional"])
        interfaces = build_interfaces_contract(bases["interfaces"])
    except (KeyError, TypeError, ValueError) as error:
        return {}, [Diagnostic("PARSE", SCRIPT_PATH, f"invalid base structure: {error}")]

    functional_bytes, error = _format_document(functional, prettier, runner)
    if error is not None:
        return {}, [error]
    interfaces_bytes, error = _format_document(interfaces, prettier, runner)
    if error is not None:
        return {}, [error]
    assert functional_bytes is not None
    assert interfaces_bytes is not None
    return {
        FUNCTIONAL_CONTRACT: functional_bytes,
        INTERFACES_CONTRACT: interfaces_bytes,
        FUNCTIONAL_VALID: functional_bytes,
        INTERFACES_VALID: interfaces_bytes,
    }, []


def _check_outputs(root: Path, outputs: dict[Path, bytes]) -> list[Diagnostic]:
    diagnostics = []
    for relative, expected in outputs.items():
        try:
            actual = (root / relative).read_bytes()
        except FileNotFoundError:
            diagnostics.append(Diagnostic("MISSING", relative, "missing generated output"))
        except OSError as error:
            diagnostics.append(Diagnostic("STALE", relative, f"cannot read output: {error}"))
        else:
            if actual != expected:
                diagnostics.append(
                    Diagnostic("STALE", relative, "generated bytes do not match")
                )
    return diagnostics


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

    bases, diagnostics = _load_bases(root)
    if diagnostics:
        _emit(diagnostics)
        return 1
    outputs, diagnostics = _derive_and_format(bases, prettier, runner)
    if diagnostics:
        _emit(diagnostics)
        return 1

    if arguments.check:
        diagnostics = _check_outputs(root, outputs)
        if diagnostics:
            _emit(diagnostics)
            return 1
    else:
        for relative, payload in outputs.items():
            try:
                _atomic_write(root, relative, payload, replace)
            except WriteFailure as error:
                _emit([Diagnostic("WRITE", relative, str(error))])
                return 1

    print("OK build-p1c-architecture-contracts")
    return 0


def main(argv: list[str] | None = None) -> int:
    return run(argv)


if __name__ == "__main__":
    raise SystemExit(main())
