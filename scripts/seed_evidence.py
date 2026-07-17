#!/usr/bin/env python3
"""Acquire, record, validate, and assemble Aius seed evidence."""

from __future__ import annotations

import argparse
import copy
import datetime as dt
import hashlib
import http.client
import json
import os
import re
import secrets
import stat
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Callable, Iterable, Mapping

import render_p1
from jsonschema_subset import SchemaError, ValidationError, check_schema, validate
from test_evidence_policy import validate_clone_boundary_rows
from test_source_schema import SourceCollectionError, validate_source_collection
from validate_evidence import ControlError, validate_artifact_payload


MAX_PAYLOAD_BYTES = 256 * 1024 * 1024
TIMEOUT_SECONDS = 30
MAX_REDIRECTS = 5
REDIRECT_STATUSES = {301, 302, 303, 307, 308}
USER_AGENT = "Aius-evidence/1.0"
ARTIFACT_NOTE = (
    "Retained for personal noncommercial research; "
    "no republication permission is inferred."
)
DYNAMIC_NOTE = "Normalized observation only; no source page is retained."
UNAVAILABLE_NOTE = (
    "No local copy is retained; no republication permission is inferred."
)
UNAVAILABLE_CONSEQUENCE = (
    "Dependent P1 claims remain provisional until the source is replaced."
)
PRETTIER_VERSION = "3.8.3"
FORMATTER_TIMEOUT_SECONDS = 30
SUPPLEMENTAL_REALIZATION_SHA256 = (
    "ffbb364de79d734c4ba50885095f58319a56687b0bda93669e4753ba689d493d"
)
SUPPLEMENTAL_OWNED_PATH_ROOT = "src/hardware/aius"
SUPPLEMENTAL_RETENTION = "personal-noncommercial-no-republication"
SUPPLEMENTAL_SOURCE_KEYS = (
    "id",
    "class",
    "title",
    "authority",
    "revision",
    "purpose",
    "claim_scope",
    "landing_url",
    "retrieved_at",
    "status",
    "supersedes",
    "redistribution_note",
    "artifacts",
    "observations",
)
SUPPLEMENTAL_ARTIFACT_KEYS = (
    "name",
    "final_url",
    "storage",
    "path",
    "sha256",
    "bytes",
    "media_type",
    "retrieval_status",
    "note",
)
SUPPLEMENTAL_OBSERVATION_KEYS = (
    "key",
    "value",
    "unit",
    "basis",
    "observed_at",
    "source_url",
)
SUPPLEMENTAL_ASSUMPTION_KEYS = (
    "id",
    "claim",
    "consequence",
    "owner",
    "validation_gate",
    "evidence_ids",
    "source_ids",
    "claim_scope",
    "status",
    "resolution",
)
SUPPLEMENTAL_PROFILE_KEYS = ("name", "fields")
SUPPLEMENTAL_FIELD_KEYS = (
    "key",
    "value_type",
    "nullable",
    "constraint",
    "unit",
    "source_role",
    "basis",
)
UK_OBSERVATION_PROFILE_NAME = "uk-distributor-price-stock"
PRESERVED_REQUEST_IDS = (
    "REQ-CLIFF-RX-MFR",
    "REQ-CLIFF-TX-MFR",
)
UK_OBSERVATION_FIELD_SPECS = (
    (
        "manufacturer_part_number",
        "string",
        False,
        "pattern",
        r"^\S(?:.*\S)?$",
        None,
        "product",
    ),
    ("supplier_sku", "string", False, "pattern", r"^\S(?:.*\S)?$", None, "product"),
    ("distributor", "string", False, "pattern", r"^\S(?:.*\S)?$", None, "product"),
    ("product_initial_url", "string", False, "pattern", r"^https://\S+$", None, "product"),
    ("product_final_url", "string", False, "pattern", r"^https://\S+$", None, "product"),
    (
        "authorization_authority",
        "string",
        False,
        "pattern",
        r"^\S(?:.*\S)?$",
        None,
        "authorization",
    ),
    (
        "authorization_method",
        "string",
        False,
        "enum",
        (
            "manufacturer-page",
            "manufacturer-locator",
            "ecia-trustedparts-exception",
        ),
        None,
        "authorization",
    ),
    (
        "authorization_initial_url",
        "string",
        False,
        "pattern",
        r"^https://\S+$",
        None,
        "authorization",
    ),
    (
        "authorization_final_url",
        "string",
        False,
        "pattern",
        r"^https://\S+$",
        None,
        "authorization",
    ),
    (
        "stock_state",
        "string",
        False,
        "enum",
        ("in-stock", "out-of-stock", "backorder", "unknown"),
        None,
        "product",
    ),
    ("stock_quantity", "integer", True, "minimum", 0, "pieces", "product"),
    ("price_quantity", "integer", False, "minimum", 1, "pieces", "product"),
    ("unit_price_ex_vat", "number", False, "minimum", 0, "GBP", "product"),
    ("currency", "string", False, "const", "GBP", None, "product"),
    (
        "minimum_order_quantity",
        "integer",
        False,
        "minimum",
        1,
        "pieces",
        "product",
    ),
    ("order_multiple", "integer", False, "minimum", 1, "pieces", "product"),
    ("price_includes_vat", "boolean", False, "const", False, None, "product"),
)

Formatter = Callable[[Path, bytes], bytes]
ProcessRunner = Callable[..., subprocess.CompletedProcess[bytes]]


class DataError(RuntimeError):
    """One or more deterministic evidence-data checks failed."""

    def __init__(self, messages: str | Iterable[str]) -> None:
        if isinstance(messages, str):
            values = (messages,)
        else:
            values = tuple(sorted(set(messages)))
        self.messages = values
        super().__init__("\n".join(values))


class _Parser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        print(f"ERROR: {message}", file=sys.stderr)
        raise SystemExit(2)


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file_pointer, code, message, headers, url):
        return None


@dataclass(frozen=True)
class State:
    root: Path
    seeds: tuple[dict[str, object], ...]
    realizations: tuple[dict[str, object], ...]
    source_schema: dict[str, object]
    assumption_schema: dict[str, object]

    @property
    def seeds_by_id(self) -> dict[str, dict[str, object]]:
        return {str(row["id"]): row for row in self.seeds}

    @property
    def realizations_by_id(self) -> dict[str, dict[str, object]]:
        return {str(row["source_id"]): row for row in self.realizations}


@dataclass(frozen=True)
class SupplementalState:
    root: Path
    requests: tuple[dict[str, object], ...]
    realizations: tuple[dict[str, object], ...]
    observation_profiles: tuple[dict[str, object], ...]
    source_schema: dict[str, object]
    assumption_schema: dict[str, object]
    request_schema: dict[str, object]

    @property
    def requests_by_id(self) -> dict[str, dict[str, object]]:
        return {str(row["request_id"]): row for row in self.requests}

    @property
    def realizations_by_id(self) -> dict[str, dict[str, object]]:
        return {str(row["request_id"]): row for row in self.realizations}

    @property
    def observation_profiles_by_name(self) -> dict[str, dict[str, object]]:
        return {str(row["name"]): row for row in self.observation_profiles}


def _compact_json(value: object, *, preserve_object_order: bool = False) -> bytes:
    try:
        return json.dumps(
            value,
            ensure_ascii=False,
            allow_nan=False,
            sort_keys=not preserve_object_order,
            separators=(",", ":"),
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        raise ControlError(f"cannot serialize strict JSON: {error}") from error


def _strict_rendered_json(payload: bytes) -> object:
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ControlError(f"formatter output is not UTF-8: {error}") from error

    def invalid_constant(value: str) -> object:
        raise ValueError(f"invalid JSON constant {value}")

    def pairs(values: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in values:
            if key in result:
                raise ValueError(f"duplicate JSON key {key!r}")
            result[key] = value
        return result

    decoder = json.JSONDecoder(
        parse_constant=invalid_constant,
        object_pairs_hook=pairs,
    )
    try:
        value, end = decoder.raw_decode(text)
    except (json.JSONDecodeError, ValueError) as error:
        raise ControlError(f"formatter output is not strict JSON: {error}") from error
    if text[end:] != "\n":
        raise ControlError("formatter output must have exactly one terminal LF")
    return value


def _same_json_value(left: object, right: object) -> bool:
    if isinstance(left, bool) or isinstance(right, bool):
        return type(left) is type(right) and left == right
    if isinstance(left, (int, float)) and isinstance(right, (int, float)):
        return left == right
    if isinstance(left, dict) and isinstance(right, dict):
        return set(left) == set(right) and all(
            _same_json_value(left[key], right[key]) for key in left
        )
    if isinstance(left, list) and isinstance(right, list):
        return len(left) == len(right) and all(
            _same_json_value(left_item, right_item)
            for left_item, right_item in zip(left, right)
        )
    return type(left) is type(right) and left == right


def _object_keys_are_sorted(value: object) -> bool:
    if isinstance(value, dict):
        return list(value) == sorted(value) and all(
            _object_keys_are_sorted(item) for item in value.values()
        )
    if isinstance(value, list):
        return all(_object_keys_are_sorted(item) for item in value)
    return True


def _object_key_order_matches(left: object, right: object) -> bool:
    if isinstance(left, dict) and isinstance(right, dict):
        return list(left) == list(right) and all(
            _object_key_order_matches(left[key], right[key]) for key in left
        )
    if isinstance(left, list) and isinstance(right, list):
        return len(left) == len(right) and all(
            _object_key_order_matches(left_item, right_item)
            for left_item, right_item in zip(left, right)
        )
    return True


def _render_json(
    root: Path,
    value: object,
    formatter: Formatter,
    *,
    preserve_object_order: bool = False,
) -> bytes:
    compact = _compact_json(value, preserve_object_order=preserve_object_order)
    expected = _decode_json(compact, "compact renderer input")
    try:
        rendered = formatter(root, compact)
    except ControlError:
        raise
    except Exception as error:
        raise ControlError(f"formatter failed: {error}") from error
    if not isinstance(rendered, bytes):
        raise ControlError("formatter output must be bytes")
    observed = _strict_rendered_json(rendered)
    if not _same_json_value(expected, observed):
        raise ControlError("formatter output changed the JSON value")
    if preserve_object_order and not _object_key_order_matches(expected, observed):
        raise ControlError("formatter output changed object-key order")
    if not preserve_object_order and not _object_keys_are_sorted(observed):
        raise ControlError("formatter output changed sorted object-key order")
    return rendered


def _test_formatter(root: Path, payload: bytes) -> bytes:
    """Deterministic in-process formatter for isolated orchestration tests."""

    del root
    value = _decode_json(payload, "test formatter input")
    return (
        json.dumps(
            value,
            ensure_ascii=False,
            allow_nan=False,
            sort_keys=False,
            indent=2,
        )
        + "\n"
    ).encode("utf-8")


def _completed_process(
    runner: ProcessRunner,
    command: list[str],
    *,
    cwd: Path | None = None,
    payload: bytes | None = None,
    label: str,
) -> subprocess.CompletedProcess[bytes]:
    try:
        completed = runner(
            command,
            cwd=None if cwd is None else os.fspath(cwd),
            input=payload,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=FORMATTER_TIMEOUT_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise ControlError(f"{label} timed out after 30 seconds") from error
    except OSError as error:
        raise ControlError(f"cannot run {label}: {error}") from error
    if not isinstance(completed.stdout, bytes) or not isinstance(
        completed.stderr, bytes
    ):
        raise ControlError(f"{label} returned non-byte process output")
    if completed.returncode != 0:
        try:
            detail = completed.stderr.decode("utf-8").strip()
        except UnicodeDecodeError:
            detail = "non-UTF-8 stderr"
        suffix = f": {detail}" if detail else ""
        raise ControlError(f"{label} exited {completed.returncode}{suffix}")
    return completed


def _git_root(root: Path, runner: ProcessRunner) -> Path:
    completed = _completed_process(
        runner,
        ["git", "-C", os.fspath(root), "rev-parse", "--show-toplevel"],
        label="Git root discovery",
    )
    try:
        output = completed.stdout.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ControlError("Git root discovery returned non-UTF-8 output") from error
    if not output.endswith("\n") or output.count("\n") != 1 or "\r" in output:
        raise ControlError("Git root discovery must return exactly one absolute path")
    candidate = Path(output[:-1])
    if not candidate.is_absolute():
        raise ControlError("Git root discovery did not return an absolute path")
    try:
        resolved = candidate.resolve(strict=True)
    except OSError as error:
        raise ControlError(f"Git root discovery returned an unusable path: {error}") from error
    if not resolved.is_dir():
        raise ControlError("Git root discovery path is not a directory")
    try:
        root.relative_to(resolved)
    except ValueError as error:
        raise ControlError("Git root discovery returned the wrong worktree") from error
    return resolved


def _regular_control(path: Path, label: str, *, executable: bool = False) -> None:
    try:
        status = path.lstat()
    except OSError as error:
        raise ControlError(f"{label} is missing or unusable: {error}") from error
    if stat.S_ISLNK(status.st_mode) or not stat.S_ISREG(status.st_mode):
        raise ControlError(f"{label} must be a regular non-symlink file: {path}")
    if executable and not os.access(path, os.X_OK):
        raise ControlError(f"{label} is not executable: {path}")


def _production_formatter(
    root: Path,
    payload: bytes,
    *,
    runner: ProcessRunner = subprocess.run,
) -> bytes:
    git_root = _git_root(root, runner)
    executable = git_root / "node_modules" / ".bin" / "prettier"
    config = git_root / ".prettierrc.json"
    _regular_control(executable, "pinned Prettier", executable=True)
    _regular_control(config, "Prettier config")

    version = _completed_process(
        runner,
        [os.fspath(executable), "--version"],
        cwd=git_root,
        label="Prettier version check",
    )
    try:
        version_text = version.stdout.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ControlError("Prettier version output is not UTF-8") from error
    if version_text not in {PRETTIER_VERSION, PRETTIER_VERSION + "\n"}:
        raise ControlError(
            f"Prettier version must be exactly {PRETTIER_VERSION}; got {version_text!r}"
        )

    rendered = _completed_process(
        runner,
        [
            os.fspath(executable),
            "--parser",
            "json",
            "--config",
            os.fspath(config),
        ],
        cwd=git_root,
        payload=payload,
        label="Prettier formatter",
    )
    return rendered.stdout


def _safe_relative(relative_text: str) -> PurePosixPath:
    relative = PurePosixPath(relative_text)
    parts = relative_text.split("/")
    drive_path = (
        len(relative_text) >= 3
        and relative_text[0].isalpha()
        and relative_text[1] == ":"
        and relative_text[2] in {"/", "\\"}
    )
    if (
        not relative_text
        or relative.is_absolute()
        or drive_path
        or any(part in {"", ".", ".."} for part in parts)
    ):
        raise ControlError(f"unsafe project-relative path: {relative_text!r}")
    return relative


def _root_path(root: Path, relative_text: str) -> Path:
    relative = _safe_relative(relative_text)
    candidate = root.joinpath(*relative.parts)
    try:
        candidate.relative_to(root)
    except ValueError as error:
        raise ControlError(f"path escapes the Aius root: {relative_text}") from error
    return candidate


def _lstat(path: Path) -> os.stat_result | None:
    try:
        return path.lstat()
    except FileNotFoundError:
        return None
    except OSError as error:
        raise ControlError(f"cannot inspect {path}: {error}") from error


def _check_parent_chain(root: Path, target: Path) -> None:
    try:
        relative = target.relative_to(root)
    except ValueError as error:
        raise ControlError(f"path escapes the Aius root: {target}") from error
    current = root
    root_status = _lstat(root)
    if root_status is None or not stat.S_ISDIR(root_status.st_mode):
        raise ControlError(f"Aius root is not a directory: {root}")
    if stat.S_ISLNK(root_status.st_mode):
        raise ControlError(f"Aius root may not be a symlink: {root}")
    for part in relative.parts[:-1]:
        current = current / part
        status = _lstat(current)
        if status is None:
            raise ControlError(f"path parent is missing: {current}")
        if stat.S_ISLNK(status.st_mode):
            raise ControlError(f"path component is a symlink: {current}")
        if not stat.S_ISDIR(status.st_mode):
            raise ControlError(f"path component is not a directory: {current}")


def _ensure_directory(root: Path, relative_text: str) -> Path:
    relative = _safe_relative(relative_text)
    current = root
    for part in relative.parts:
        current = current / part
        status = _lstat(current)
        if status is None:
            try:
                current.mkdir()
            except FileExistsError:
                pass
            except OSError as error:
                raise DataError(f"cannot create directory {current}: {error}") from error
            status = _lstat(current)
        if status is None or stat.S_ISLNK(status.st_mode):
            raise DataError(f"directory path is unsafe: {current}")
        if not stat.S_ISDIR(status.st_mode):
            raise DataError(f"directory path is not a directory: {current}")
    return current


def _read_regular(root: Path, relative_text: str, *, required: bool = True) -> bytes | None:
    path = _root_path(root, relative_text)
    _check_parent_chain(root, path)
    status = _lstat(path)
    if status is None:
        if required:
            raise ControlError(f"required regular file is missing: {relative_text}")
        return None
    if stat.S_ISLNK(status.st_mode) or not stat.S_ISREG(status.st_mode):
        raise ControlError(f"path is not a regular non-symlink file: {relative_text}")
    try:
        return path.read_bytes()
    except OSError as error:
        raise ControlError(f"cannot read {relative_text}: {error}") from error


def _reject_target(root: Path, relative_text: str) -> Path:
    path = _root_path(root, relative_text)
    try:
        _check_parent_chain(root, path)
    except ControlError as error:
        raise DataError(str(error)) from error
    if _lstat(path) is not None:
        raise DataError(f"refusing to overwrite existing path: {relative_text}")
    return path


def _decode_json(payload: bytes, label: str) -> object:
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ControlError(f"{label} is not UTF-8: {error}") from error

    def invalid_constant(value: str) -> object:
        raise ValueError(f"invalid JSON constant {value}")

    try:
        return json.loads(text, parse_constant=invalid_constant)
    except (json.JSONDecodeError, ValueError) as error:
        raise ControlError(f"{label} is not valid strict JSON: {error}") from error


def _load_json(root: Path, relative_text: str) -> object:
    payload = _read_regular(root, relative_text)
    assert payload is not None
    return _decode_json(payload, relative_text)


def _schema(instance: object, schema: object, root_schema: object, label: str) -> None:
    try:
        validate(instance, schema, root_schema=root_schema)
    except (SchemaError, ValidationError) as error:
        raise DataError(f"{label}: {error}") from error


def _load_state(root_argument: Path) -> State:
    argument_status = _lstat(root_argument)
    if argument_status is not None and stat.S_ISLNK(argument_status.st_mode):
        raise ControlError(f"Aius root may not be a symlink: {root_argument}")
    root = root_argument.resolve()
    root_status = _lstat(root)
    if root_status is None or stat.S_ISLNK(root_status.st_mode) or not stat.S_ISDIR(
        root_status.st_mode
    ):
        raise ControlError(f"Aius root is not a regular directory: {root}")

    source_schema = _load_json(root, "evidence/manifest/source-schema.json")
    assumption_schema = _load_json(root, "evidence/manifest/assumption-schema.json")
    realization_schema = _load_json(
        root, "evidence/manifest/seed-realization-schema.json"
    )
    seeds = _load_json(root, "evidence/manifest/seed-sources.json")
    realizations = _load_json(root, "evidence/manifest/seed-realization.json")
    for label, schema in (
        ("source schema", source_schema),
        ("assumption schema", assumption_schema),
        ("seed realization schema", realization_schema),
    ):
        try:
            check_schema(schema)
        except SchemaError as error:
            raise ControlError(f"{label} is invalid: {error}") from error
    if not isinstance(source_schema, dict) or not isinstance(assumption_schema, dict):
        raise ControlError("source and assumption schemas must be objects")
    if not isinstance(realization_schema, dict):
        raise ControlError("seed realization schema must be an object")
    try:
        seed_schema = source_schema["$defs"]["seedCollection"]
        validate(seeds, seed_schema, root_schema=source_schema)
        validate(realizations, realization_schema)
    except (KeyError, TypeError, SchemaError, ValidationError) as error:
        raise ControlError(f"seed control data is invalid: {error}") from error
    if not isinstance(seeds, list) or not all(isinstance(row, dict) for row in seeds):
        raise ControlError("seed source control must be an object array")
    if not isinstance(realizations, list) or not all(
        isinstance(row, dict) for row in realizations
    ):
        raise ControlError("seed realization control must be an object array")
    seed_ids = [str(row["id"]) for row in seeds]
    realization_ids = [str(row["source_id"]) for row in realizations]
    if seed_ids != sorted(seed_ids) or realization_ids != seed_ids:
        raise ControlError("seed and realization IDs are not the same sorted set")
    return State(
        root,
        tuple(seeds),
        tuple(realizations),
        source_schema,
        assumption_schema,
    )


def _expected_uk_observation_profile() -> dict[str, object]:
    fields: list[dict[str, object]] = []
    for key, value_type, nullable, kind, value, unit, source_role in (
        UK_OBSERVATION_FIELD_SPECS
    ):
        fields.append(
            {
                "key": key,
                "value_type": value_type,
                "nullable": nullable,
                "constraint": {
                    "kind": kind,
                    "value": list(value) if isinstance(value, tuple) else value,
                },
                "unit": unit,
                "source_role": source_role,
                "basis": (
                    f"P1D {UK_OBSERVATION_PROFILE_NAME} normalized field {key} "
                    "from the approved supplemental request, realization, and live page."
                ),
            }
        )
    return {"name": UK_OBSERVATION_PROFILE_NAME, "fields": fields}


def _load_supplemental_state(root_argument: Path) -> SupplementalState:
    argument_status = _lstat(root_argument)
    if argument_status is not None and stat.S_ISLNK(argument_status.st_mode):
        raise ControlError(f"Aius root may not be a symlink: {root_argument}")
    root = root_argument.resolve()
    root_status = _lstat(root)
    if root_status is None or stat.S_ISLNK(root_status.st_mode) or not stat.S_ISDIR(
        root_status.st_mode
    ):
        raise ControlError(f"Aius root is not a regular directory: {root}")

    source_schema = _load_json(root, "evidence/manifest/source-schema.json")
    assumption_schema = _load_json(root, "evidence/manifest/assumption-schema.json")
    request_schema = _load_json(root, "evidence/manifest/source-request-schema.json")
    realization_schema = _load_json(
        root, "evidence/manifest/supplemental-realization-schema.json"
    )
    request_document = _load_json(
        root, "evidence/manifest/supplemental-requests.json"
    )
    realization_payload = _read_regular(
        root, "evidence/manifest/supplemental-realization.json"
    )
    assert realization_payload is not None
    realization_document = _decode_json(
        realization_payload,
        "evidence/manifest/supplemental-realization.json",
    )
    for label, schema in (
        ("source schema", source_schema),
        ("assumption schema", assumption_schema),
        ("source request schema", request_schema),
        ("supplemental realization schema", realization_schema),
    ):
        try:
            check_schema(schema)
        except SchemaError as error:
            raise ControlError(f"{label} is invalid: {error}") from error
    if (
        not isinstance(source_schema, dict)
        or not isinstance(assumption_schema, dict)
        or not isinstance(request_schema, dict)
    ):
        raise ControlError(
            "supplemental source, assumption, and request schemas must be objects"
        )
    if not isinstance(realization_schema, dict):
        raise ControlError("supplemental realization schema must be an object")
    if not isinstance(request_document, dict) or tuple(request_document) != (
        "schema_version",
        "requests",
    ):
        raise ControlError("supplemental request aggregate has invalid root properties")
    if request_document.get("schema_version") != "1.0.0":
        raise ControlError("supplemental request aggregate schema version differs")
    requests = request_document.get("requests")
    if not isinstance(requests, list) or not all(
        isinstance(row, dict) for row in requests
    ):
        raise ControlError("supplemental requests must be an object array")
    try:
        for request in requests:
            validate(request, request_schema)
        validate(realization_document, realization_schema)
    except (SchemaError, ValidationError) as error:
        raise ControlError(f"supplemental control data is invalid: {error}") from error
    if hashlib.sha256(realization_payload).hexdigest() != SUPPLEMENTAL_REALIZATION_SHA256:
        raise ControlError("supplemental realization differs from the immutable P1D0a oracle")
    if not isinstance(realization_document, dict):
        raise ControlError("supplemental realization control must be an object")
    if tuple(realization_document) != (
        "schema_version",
        "owned_path_root",
        "observation_profiles",
        "realizations",
    ):
        raise ControlError("supplemental realization root property order differs")
    if realization_document.get("owned_path_root") != SUPPLEMENTAL_OWNED_PATH_ROOT:
        raise ControlError("supplemental realization owned path root differs")
    observation_profiles = realization_document.get("observation_profiles")
    if (
        not isinstance(observation_profiles, list)
        or len(observation_profiles) != 1
        or not isinstance(observation_profiles[0], dict)
    ):
        raise ControlError("supplemental observation profile set differs")
    profile = observation_profiles[0]
    fields = profile.get("fields")
    if (
        tuple(profile) != SUPPLEMENTAL_PROFILE_KEYS
        or not isinstance(fields, list)
        or any(
            not isinstance(field, dict)
            or tuple(field) != SUPPLEMENTAL_FIELD_KEYS
            or not isinstance(field.get("constraint"), dict)
            or tuple(field["constraint"]) != ("kind", "value")
            for field in fields
        )
        or profile != _expected_uk_observation_profile()
    ):
        raise ControlError("supplemental UK observation profile differs")
    realizations = realization_document.get("realizations")
    if not isinstance(realizations, list) or not all(
        isinstance(row, dict) for row in realizations
    ):
        raise ControlError("supplemental realizations must be an object array")

    request_ids = [str(row["request_id"]) for row in requests]
    if request_ids != sorted(request_ids) or len(request_ids) != len(set(request_ids)):
        raise ControlError("supplemental request IDs must be unique and sorted")
    realization_ids = [str(row["request_id"]) for row in realizations]
    expected_realization_ids = sorted(set(request_ids) - set(PRESERVED_REQUEST_IDS))
    if len(realizations) != 46 or realization_ids != expected_realization_ids:
        raise ControlError(
            "supplemental realizations do not exactly cover the 46 non-preserved requests"
        )
    request_by_id = {str(row["request_id"]): row for row in requests}
    for request_id in PRESERVED_REQUEST_IDS:
        request = request_by_id.get(request_id)
        if (
            request is None
            or request.get("status") != "satisfied"
            or request.get("evidence_ids") != [request.get("source_id")]
            or request.get("notes") != ""
        ):
            raise ControlError(f"{request_id}: preserved request outcome differs")
    source_ids: set[str] = set()
    manufacturer_count = 0
    uk_count = 0
    for row in realizations:
        request_id = str(row["request_id"])
        request = request_by_id[request_id]
        source_id = str(row["source_id"])
        if source_id != request["source_id"] or source_id in source_ids:
            raise ControlError(f"{request_id}: supplemental source identity differs")
        source_ids.add(source_id)
        artifacts = row["artifacts"]
        assert isinstance(artifacts, list)
        expected_owned_paths = [
            f"evidence/manifest/supplemental-fragments/{source_id}.json",
            f"evidence/manifest/supplemental-assumption-fragments/{source_id}.json",
            *[str(artifact["path"]) for artifact in artifacts],
        ]
        if row["owned_paths"] != expected_owned_paths:
            raise ControlError(f"{request_id}: supplemental owned paths differ")
        for relative_text in expected_owned_paths:
            relative = _safe_relative(relative_text)
            if source_id not in relative.parts and relative.stem != source_id:
                raise ControlError(
                    f"{request_id}: supplemental owned path omits its source ID"
                )
        if request["request_kind"] == "manufacturer-evidence":
            manufacturer_count += 1
            kinds = [str(artifact["artifact_kind"]) for artifact in artifacts]
            if (
                row["authority"] != request["selected_manufacturer"]
                or row["revision_strategy"] != "sha256"
                or row["revision_value"] is not None
                or row["observation_url"] is not None
                or row["authorization"] is not None
                or row["observation_profile"] is not None
                or row["failure_policy"] != "golden-block"
                or row["retention"] != SUPPLEMENTAL_RETENTION
                or list(dict.fromkeys(kinds)) != request["artifact_kinds"]
                or not artifacts
            ):
                raise ControlError(f"{request_id}: manufacturer realization differs")
            if not any(
                artifact["retrieval_url"] == request["authority_url"]
                for artifact in artifacts
            ):
                raise ControlError(
                    f"{request_id}: manufacturer authority URL is not retained"
                )
        elif request["request_kind"] == "uk-availability":
            uk_count += 1
            authorization = row["authorization"]
            if (
                artifacts
                or row["revision_strategy"] != "observation-date"
                or row["revision_value"] is not None
                or row["observation_url"] != request["authority_url"]
                or not isinstance(authorization, dict)
                or row["clone_boundary"] is not None
                or row["observation_profile"] != UK_OBSERVATION_PROFILE_NAME
                or row["failure_policy"] != "assumption"
                or row["retention"] != "normalized-observation-only"
            ):
                raise ControlError(f"{request_id}: UK realization differs")
            is_tag_connect = request["selected_manufacturer"] == "Tag-Connect"
            is_exception = (
                authorization.get("method") == "ecia-trustedparts-exception"
            )
            if is_tag_connect != is_exception:
                raise ControlError(f"{request_id}: authorization exception differs")
        else:
            raise ControlError(f"{request_id}: unknown supplemental request kind")
    if manufacturer_count != 22 or uk_count != 24:
        raise ControlError("supplemental manufacturer/UK realization split differs")
    return SupplementalState(
        root,
        tuple(requests),
        tuple(realizations),
        tuple(observation_profiles),
        source_schema,
        assumption_schema,
        request_schema,
    )


def _timestamp(value: str) -> str:
    if re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z", value) is None:
        raise DataError(f"timestamp must be exact UTC YYYY-MM-DDTHH:MM:SSZ: {value!r}")
    try:
        dt.datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ")
    except ValueError as error:
        raise DataError(f"timestamp is not a real calendar time: {value!r}") from error
    return value


def _select(state: State, source_id: str) -> tuple[dict[str, object], dict[str, object]]:
    seed = state.seeds_by_id.get(source_id)
    realization = state.realizations_by_id.get(source_id)
    if seed is None or realization is None:
        raise DataError(f"unknown seed source ID: {source_id}")
    return seed, realization


def _source_base(
    seed: Mapping[str, object],
    *,
    revision: str,
    retrieved_at: str,
    status: str,
    redistribution_note: str,
    artifacts: list[dict[str, object]],
    observations: list[dict[str, object]],
) -> dict[str, object]:
    return {
        "id": seed["id"],
        "class": seed["class"],
        "title": seed["required_object"],
        "authority": seed["authority"],
        "revision": revision,
        "purpose": seed["purpose"],
        "claim_scope": seed["claim_scope"],
        "landing_url": seed["landing_url"],
        "retrieved_at": retrieved_at,
        "status": status,
        "supersedes": None,
        "redistribution_note": redistribution_note,
        "artifacts": artifacts,
        "observations": observations,
    }


def _json_equal(left: object, right: object) -> bool:
    if isinstance(left, bool) or isinstance(right, bool):
        return type(left) is type(right) and left == right
    if isinstance(left, (int, float)) and isinstance(right, (int, float)):
        return left == right
    return type(left) is type(right) and left == right


def _type_matches(value: object, value_type: str) -> bool:
    if value_type == "string":
        return isinstance(value, str)
    if value_type == "boolean":
        return isinstance(value, bool)
    if value_type == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if value_type == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    raise ControlError(f"unknown descriptor value type: {value_type!r}")


def _validate_field_value(field: Mapping[str, object], value: object) -> None:
    key = str(field["key"])
    if value is None:
        if field["nullable"] is True:
            return
        raise DataError(f"observation value {key!r} may not be null")
    value_type = str(field["value_type"])
    if not _type_matches(value, value_type):
        raise DataError(f"observation value {key!r} must be {value_type}")
    constraint = field["constraint"]
    if not isinstance(constraint, dict):
        raise ControlError(f"descriptor constraint for {key!r} is not an object")
    kind = constraint.get("kind")
    expected = constraint.get("value")
    if kind == "none":
        return
    if kind == "non-empty":
        if not isinstance(value, str) or not value.strip():
            raise DataError(f"observation value {key!r} must be non-empty")
        return
    if kind == "enum":
        if not isinstance(expected, list) or not any(
            _json_equal(value, item) for item in expected
        ):
            raise DataError(f"observation value {key!r} is outside its enum")
        return
    if kind == "minimum":
        if not isinstance(expected, (int, float)) or isinstance(expected, bool):
            raise ControlError(f"descriptor minimum for {key!r} is invalid")
        if value < expected:
            raise DataError(f"observation value {key!r} is below its minimum")
        return
    if kind == "exclusive-minimum":
        if not isinstance(expected, (int, float)) or isinstance(expected, bool):
            raise ControlError(f"descriptor exclusive minimum for {key!r} is invalid")
        if value <= expected:
            raise DataError(
                f"observation value {key!r} is not above its exclusive minimum"
            )
        return
    if kind == "const":
        if not _json_equal(value, expected):
            raise DataError(f"observation value {key!r} does not equal its constant")
        return
    if kind == "pattern":
        if not isinstance(expected, str):
            raise ControlError(f"descriptor pattern for {key!r} is invalid")
        try:
            matches = re.fullmatch(expected, str(value)) is not None
        except re.error as error:
            raise ControlError(f"descriptor pattern for {key!r} is invalid: {error}") from error
        if not matches:
            raise DataError(f"observation value {key!r} does not match its pattern")
        return
    raise ControlError(f"unknown descriptor constraint kind: {kind!r}")


def _parse_values(text: str) -> dict[str, object]:
    def invalid_constant(value: str) -> object:
        raise ValueError(f"invalid JSON constant {value}")

    def pairs(values: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in values:
            if key in result:
                raise ValueError(f"duplicate JSON key {key!r}")
            result[key] = value
        return result

    try:
        value = json.loads(
            text,
            parse_constant=invalid_constant,
            object_pairs_hook=pairs,
        )
    except (json.JSONDecodeError, ValueError) as error:
        raise DataError(f"--values is not strict JSON: {error}") from error
    if not isinstance(value, dict):
        raise DataError("--values must be a JSON object")
    if any(isinstance(item, (dict, list)) for item in value.values()):
        raise DataError("--values must contain only JSON scalars")
    return value


def _observations(
    seed: Mapping[str, object],
    realization: Mapping[str, object],
    values: Mapping[str, object],
    observed_at: str,
) -> list[dict[str, object]]:
    profile = realization.get("observation_profile")
    if not isinstance(profile, dict) or not isinstance(profile.get("fields"), list):
        raise ControlError("dynamic realization has no usable observation descriptor")
    fields = profile["fields"]
    keys = [str(field["key"]) for field in fields if isinstance(field, dict)]
    if len(keys) != len(fields):
        raise ControlError("observation descriptor contains a non-object field")
    if set(values) != set(keys) or len(values) != len(keys):
        missing = sorted(set(keys) - set(values))
        extra = sorted(set(values) - set(keys))
        raise DataError(f"observation keys differ; missing={missing!r}, extra={extra!r}")
    result: list[dict[str, object]] = []
    for field in fields:
        assert isinstance(field, dict)
        key = str(field["key"])
        value = values[key]
        _validate_field_value(field, value)
        result.append(
            {
                "key": key,
                "value": value,
                "unit": field["unit"],
                "basis": field["basis"],
                "observed_at": observed_at,
                "source_url": seed["landing_url"],
            }
        )
    return result


def _expected_revision(
    realization: Mapping[str, object],
    artifacts: Iterable[Mapping[str, object]],
    retrieved_at: str,
) -> str:
    strategy = realization["revision_strategy"]
    if strategy == "fixed":
        return str(realization["revision_value"])
    if strategy == "commit":
        return f"commit:{str(realization['revision_value']).lower()}"
    if strategy == "observation-date":
        return f"observed:{retrieved_at[:10]}"
    if strategy == "sha256":
        digests = [str(artifact["sha256"]).lower() for artifact in artifacts]
        if len(digests) == 1:
            return f"sha256:{digests[0]}"
        combined = hashlib.sha256("".join(digests).encode("ascii")).hexdigest()
        return f"sha256:{combined}"
    raise ControlError(f"unknown revision strategy: {strategy!r}")


def _select_supplemental(
    state: SupplementalState,
    request_id: str,
) -> tuple[dict[str, object], dict[str, object]]:
    request = state.requests_by_id.get(request_id)
    if request is None:
        raise DataError(f"unknown supplemental request ID: {request_id}")
    realization = state.realizations_by_id.get(request_id)
    if realization is None:
        raise DataError(
            f"supplemental request {request_id} has no acquisition realization"
        )
    return request, realization


def _supplemental_source_identity(
    request: Mapping[str, object],
) -> tuple[str, str, str]:
    request_id = str(request["request_id"])
    if request["request_kind"] == "manufacturer-evidence":
        return (
            "golden",
            (
                f"{request['selected_manufacturer']} {request['selected_mpn']} "
                "primary manufacturer evidence"
            ),
            f"Satisfy {request_id} for {request['family']}.",
        )
    if request["request_kind"] == "uk-availability":
        return (
            "dynamic",
            "normalized UK distributor price and stock observation",
            (
                "Record a UK authorized-distributor price and stock observation for "
                f"{request['selected_manufacturer']} {request['selected_mpn']}."
            ),
        )
    raise ControlError(f"{request_id}: unknown supplemental request kind")


def _supplemental_current_manufacturer_source(
    request: Mapping[str, object],
    realization: Mapping[str, object],
    artifacts: list[dict[str, object]],
    retrieved_at: str,
) -> dict[str, object]:
    request_id = str(request["request_id"])
    if request["request_kind"] != "manufacturer-evidence":
        raise DataError(
            f"UK supplemental request {request_id} does not support acquire"
        )
    if len(artifacts) != len(realization["artifacts"]):
        raise DataError(f"{request_id}: supplemental artifact result count differs")
    source_class, title, purpose = _supplemental_source_identity(request)
    source = {
        "id": request["source_id"],
        "class": source_class,
        "title": title,
        "authority": realization["authority"],
        "revision": _expected_revision(realization, artifacts, retrieved_at),
        "purpose": purpose,
        "claim_scope": request["requirements"],
        "landing_url": request["authority_url"],
        "retrieved_at": retrieved_at,
        "status": "current",
        "supersedes": None,
        "redistribution_note": ARTIFACT_NOTE,
        "artifacts": artifacts,
        "observations": [
            {
                "key": "manufacturer_part_number",
                "value": request["selected_mpn"],
                "unit": None,
                "basis": (
                    "Selected MPN identified by the retained primary manufacturer "
                    f"evidence for {request_id}."
                ),
                "observed_at": retrieved_at,
                "source_url": request["authority_url"],
            }
        ],
    }
    if tuple(source) != SUPPLEMENTAL_SOURCE_KEYS:
        raise DataError("supplemental source constructor property order differs")
    if any(tuple(artifact) != SUPPLEMENTAL_ARTIFACT_KEYS for artifact in artifacts):
        raise DataError("supplemental source artifact property order differs")
    if tuple(source["observations"][0]) != SUPPLEMENTAL_OBSERVATION_KEYS:
        raise DataError("supplemental source observation property order differs")
    return source


def _supplemental_observation_profile(
    state: SupplementalState,
    realization: Mapping[str, object],
) -> dict[str, object]:
    profile_name = realization.get("observation_profile")
    if not isinstance(profile_name, str):
        raise ControlError("UK realization has no observation profile")
    profile = state.observation_profiles_by_name.get(profile_name)
    if profile is None:
        raise ControlError(f"unknown supplemental observation profile: {profile_name!r}")
    return profile


def _validate_uk_value_bindings(
    request: Mapping[str, object],
    realization: Mapping[str, object],
    values: Mapping[str, object],
    product_final_url: str,
    authorization_final_url: str,
) -> None:
    authorization = realization.get("authorization")
    if not isinstance(authorization, dict):
        raise ControlError("UK realization authorization is invalid")
    expected = {
        "manufacturer_part_number": request["selected_mpn"],
        "distributor": realization["authority"],
        "product_initial_url": realization["observation_url"],
        "product_final_url": product_final_url,
        "authorization_authority": authorization["authority"],
        "authorization_method": authorization["method"],
        "authorization_initial_url": authorization["initial_url"],
        "authorization_final_url": authorization_final_url,
    }
    for key, expected_value in expected.items():
        if values.get(key) != expected_value:
            raise DataError(
                f"observation value {key!r} differs from its request or realization binding"
            )


def _validate_uk_cross_fields(values: Mapping[str, object]) -> None:
    state = values["stock_state"]
    quantity = values["stock_quantity"]
    if state == "in-stock" and (
        not isinstance(quantity, int) or isinstance(quantity, bool) or quantity < 1
    ):
        raise DataError("stock_state 'in-stock' requires stock_quantity >= 1")
    if state == "out-of-stock" and quantity != 0:
        raise DataError("stock_state 'out-of-stock' requires stock_quantity 0")
    if state in {"backorder", "unknown"} and quantity is not None:
        raise DataError(f"stock_state {state!r} requires null stock_quantity")

    price_quantity = values["price_quantity"]
    minimum_order_quantity = values["minimum_order_quantity"]
    order_multiple = values["order_multiple"]
    assert isinstance(price_quantity, int) and not isinstance(price_quantity, bool)
    assert isinstance(minimum_order_quantity, int) and not isinstance(
        minimum_order_quantity, bool
    )
    assert isinstance(order_multiple, int) and not isinstance(order_multiple, bool)
    if price_quantity < minimum_order_quantity:
        raise DataError(
            "price_quantity must be greater than or equal to minimum_order_quantity"
        )
    if price_quantity % order_multiple != 0:
        raise DataError("price_quantity must be divisible by order_multiple")


def _supplemental_uk_observations(
    profile: Mapping[str, object],
    values: Mapping[str, object],
    product_final_url: str,
    authorization_final_url: str,
    observed_at: str,
) -> list[dict[str, object]]:
    fields = profile.get("fields")
    if not isinstance(fields, list):
        raise ControlError("supplemental observation profile has no fields")
    keys = [str(field["key"]) for field in fields if isinstance(field, dict)]
    if len(keys) != len(fields):
        raise ControlError("supplemental observation profile contains a non-object field")
    if set(values) != set(keys) or len(values) != len(keys):
        missing = sorted(set(keys) - set(values))
        extra = sorted(set(values) - set(keys))
        raise DataError(f"observation keys differ; missing={missing!r}, extra={extra!r}")

    observations: list[dict[str, object]] = []
    source_urls = {
        "product": product_final_url,
        "authorization": authorization_final_url,
    }
    for field in fields:
        assert isinstance(field, dict)
        key = str(field["key"])
        value = values[key]
        _validate_field_value(field, value)
        source_role = field.get("source_role")
        if source_role not in source_urls:
            raise ControlError(
                f"observation field {key!r} has unknown source role {source_role!r}"
            )
        observations.append(
            {
                "key": key,
                "value": value,
                "unit": field["unit"],
                "basis": field["basis"],
                "observed_at": observed_at,
                "source_url": source_urls[str(source_role)],
            }
        )
    _validate_uk_cross_fields(values)
    return observations


def _supplemental_current_uk_source(
    request: Mapping[str, object],
    realization: Mapping[str, object],
    profile: Mapping[str, object],
    values: Mapping[str, object],
    product_final_url: str,
    authorization_final_url: str,
    observed_at: str,
) -> dict[str, object]:
    request_id = str(request["request_id"])
    if request["request_kind"] != "uk-availability":
        raise DataError(
            f"manufacturer supplemental request {request_id} does not support record"
        )
    authorization = realization.get("authorization")
    if not isinstance(authorization, dict):
        raise ControlError(f"{request_id}: UK authorization tuple is invalid")
    if authorization.get("status") != "confirmed":
        raise DataError(
            f"{request_id}: authorization is unresolved; use supplemental unavailable"
        )
    product_final_url = _https_url(product_final_url, "product final URL")
    authorization_final_url = _https_url(
        authorization_final_url,
        "authorization final URL",
    )
    observations = _supplemental_uk_observations(
        profile,
        values,
        product_final_url,
        authorization_final_url,
        observed_at,
    )
    _validate_uk_value_bindings(
        request,
        realization,
        values,
        product_final_url,
        authorization_final_url,
    )
    source_class, title, purpose = _supplemental_source_identity(request)
    source = {
        "id": request["source_id"],
        "class": source_class,
        "title": title,
        "authority": realization["authority"],
        "revision": _expected_revision(realization, [], observed_at),
        "purpose": purpose,
        "claim_scope": request["requirements"],
        "landing_url": request["authority_url"],
        "retrieved_at": observed_at,
        "status": "current",
        "supersedes": None,
        "redistribution_note": DYNAMIC_NOTE,
        "artifacts": [],
        "observations": observations,
    }
    if tuple(source) != SUPPLEMENTAL_SOURCE_KEYS:
        raise DataError("supplemental source constructor property order differs")
    if any(tuple(row) != SUPPLEMENTAL_OBSERVATION_KEYS for row in observations):
        raise DataError("supplemental source observation property order differs")
    return source


def _supplemental_unavailable_source(
    request: Mapping[str, object],
    realization: Mapping[str, object],
    note: str,
    retrieved_at: str,
) -> dict[str, object]:
    source_class, title, purpose = _supplemental_source_identity(request)
    source = {
        "id": request["source_id"],
        "class": source_class,
        "title": title,
        "authority": realization["authority"],
        "revision": f"unavailable:{retrieved_at[:10]}",
        "purpose": purpose,
        "claim_scope": request["requirements"],
        "landing_url": request["authority_url"],
        "retrieved_at": retrieved_at,
        "status": "unavailable",
        "supersedes": None,
        "redistribution_note": UNAVAILABLE_NOTE,
        "artifacts": [
            {
                "name": "unavailable",
                "final_url": request["authority_url"],
                "storage": "external-reference",
                "path": None,
                "sha256": None,
                "bytes": None,
                "media_type": None,
                "retrieval_status": "unavailable",
                "note": note,
            }
        ],
        "observations": [],
    }
    if tuple(source) != SUPPLEMENTAL_SOURCE_KEYS:
        raise DataError("supplemental source constructor property order differs")
    if tuple(source["artifacts"][0]) != SUPPLEMENTAL_ARTIFACT_KEYS:
        raise DataError("supplemental source artifact property order differs")
    return source


def _expected_supplemental_assumption(
    request: Mapping[str, object],
) -> dict[str, object]:
    request_id = str(request["request_id"])
    source_id = str(request["source_id"])
    requirements = request["requirements"]
    assert isinstance(requirements, list)
    assumption = {
        "id": f"ASM-UNAVAILABLE-{source_id}",
        "claim": (
            f"Supplemental request {request_id} source {source_id} is unavailable "
            f"for: {'; '.join(str(value) for value in requirements)}"
        ),
        "consequence": (
            f"The selected component remains provisional until {request_id} is "
            "replaced with current evidence."
        ),
        "owner": "P1D",
        "validation_gate": "Replace unavailable supplemental evidence",
        "evidence_ids": [],
        "source_ids": [source_id],
        "claim_scope": requirements,
        "status": "open",
        "resolution": None,
    }
    if tuple(assumption) != SUPPLEMENTAL_ASSUMPTION_KEYS:
        raise DataError("supplemental assumption constructor property order differs")
    return assumption


def _expected_assumption(seed: Mapping[str, object]) -> dict[str, object]:
    source_id = str(seed["id"])
    claim_scope = seed["claim_scope"]
    assert isinstance(claim_scope, list)
    return {
        "id": f"ASM-UNAVAILABLE-{source_id}",
        "claim": f"Seed source {source_id} is unavailable for: {'; '.join(str(value) for value in claim_scope)}",
        "consequence": UNAVAILABLE_CONSEQUENCE,
        "owner": "P1B",
        "validation_gate": "Replace unavailable seed evidence",
        "evidence_ids": [],
        "source_ids": [source_id],
        "claim_scope": claim_scope,
        "status": "open",
        "resolution": None,
    }


def _validate_assumption(
    state: State,
    seed: Mapping[str, object],
    source: Mapping[str, object],
    assumption: object | None,
) -> None:
    realization = state.realizations_by_id[str(seed["id"])]
    required = (
        source.get("status") == "unavailable"
        and realization["failure_policy"] == "assumption"
    )
    if not required:
        if assumption is not None:
            raise DataError(f"source {seed['id']} has an unexpected assumption fragment")
        return
    if assumption is None:
        raise DataError(f"source {seed['id']} requires an unavailable assumption")
    assumption_record_schema = state.assumption_schema["$defs"]["assumptionRecord"]
    _schema(
        assumption,
        assumption_record_schema,
        state.assumption_schema,
        "assumption",
    )
    if assumption != _expected_assumption(seed):
        raise DataError(f"source {seed['id']} has a non-canonical unavailable assumption")


def _validate_source(
    state: State,
    source_id: str,
    source: object,
    assumption: object | None,
    *,
    payloads: Mapping[str, bytes] | None = None,
) -> None:
    seed, realization = _select(state, source_id)
    source_record_schema = state.source_schema["$defs"]["sourceRecord"]
    _schema(source, source_record_schema, state.source_schema, f"source {source_id}")
    assert isinstance(source, dict)
    try:
        validate_source_collection([source], [seed])
    except SourceCollectionError as error:
        raise DataError(error.errors) from error
    expected_common = {
        "id": seed["id"],
        "class": seed["class"],
        "title": seed["required_object"],
        "authority": seed["authority"],
        "purpose": seed["purpose"],
        "claim_scope": seed["claim_scope"],
        "landing_url": seed["landing_url"],
        "supersedes": None,
    }
    for key, expected in expected_common.items():
        if source.get(key) != expected:
            raise DataError(f"source {source_id} field {key!r} differs from its seed")
    retrieved_at = source.get("retrieved_at")
    if not isinstance(retrieved_at, str):
        raise DataError(f"source {source_id} has no timestamp")
    _timestamp(retrieved_at)

    if source.get("status") == "unavailable":
        expected = _source_base(
            seed,
            revision=f"unavailable:{retrieved_at[:10]}",
            retrieved_at=retrieved_at,
            status="unavailable",
            redistribution_note=UNAVAILABLE_NOTE,
            artifacts=[
                {
                    "name": "unavailable",
                    "final_url": seed["landing_url"],
                    "storage": "external-reference",
                    "path": None,
                    "sha256": None,
                    "bytes": None,
                    "media_type": None,
                    "retrieval_status": "unavailable",
                    "note": source["artifacts"][0]["note"]
                    if isinstance(source.get("artifacts"), list)
                    and len(source["artifacts"]) == 1
                    and isinstance(source["artifacts"][0], dict)
                    else "",
                }
            ],
            observations=[],
        )
        if source != expected or not str(expected["artifacts"][0]["note"]).strip():
            raise DataError(f"source {source_id} is not a canonical unavailable fragment")
        _validate_assumption(state, seed, source, assumption)
        return

    if source.get("status") != "current":
        raise DataError(f"source {source_id} must be current or unavailable")
    _validate_assumption(state, seed, source, assumption)
    if realization["mode"] == "dynamic":
        if source.get("artifacts") != []:
            raise DataError(f"dynamic source {source_id} may not contain artifacts")
        observations = source.get("observations")
        if not isinstance(observations, list):
            raise DataError(f"dynamic source {source_id} has invalid observations")
        profile = realization["observation_profile"]
        assert isinstance(profile, dict)
        fields = profile["fields"]
        assert isinstance(fields, list)
        if len(observations) != len(fields):
            raise DataError(f"dynamic source {source_id} observation count differs")
        values: dict[str, object] = {}
        for observation in observations:
            if isinstance(observation, dict) and isinstance(observation.get("key"), str):
                values[observation["key"]] = observation.get("value")
        expected_observations = _observations(seed, realization, values, retrieved_at)
        if observations != expected_observations:
            raise DataError(f"dynamic source {source_id} observations differ from descriptor")
        expected_revision = _expected_revision(realization, [], retrieved_at)
        if source.get("revision") != expected_revision:
            raise DataError(f"dynamic source {source_id} revision differs")
        if source.get("redistribution_note") != DYNAMIC_NOTE:
            raise DataError(f"dynamic source {source_id} redistribution note differs")
        return

    actual_artifacts = source.get("artifacts")
    expected_artifacts = realization["artifacts"]
    if not isinstance(actual_artifacts, list) or not isinstance(expected_artifacts, list):
        raise DataError(f"source {source_id} artifact realization is invalid")
    if len(actual_artifacts) != len(expected_artifacts) or source.get("observations") != []:
        raise DataError(f"source {source_id} artifact count or observations differ")
    for index, (actual, expected) in enumerate(zip(actual_artifacts, expected_artifacts)):
        if not isinstance(actual, dict) or not isinstance(expected, dict):
            raise DataError(f"source {source_id} artifact {index} is invalid")
        exact = {
            "name": expected["name"],
            "final_url": expected["expected_final_url"],
            "storage": expected["storage"],
            "path": expected["path"],
            "media_type": expected["media_type"],
            "retrieval_status": "current",
            "note": ARTIFACT_NOTE,
        }
        for key, value in exact.items():
            if actual.get(key) != value:
                raise DataError(
                    f"source {source_id} artifact {index} field {key!r} differs"
                )
        relative_path = str(expected["path"])
        if payloads is not None and relative_path in payloads:
            payload = payloads[relative_path]
        else:
            try:
                payload = _read_regular(state.root, relative_path)
            except ControlError as error:
                raise DataError(str(error)) from error
            assert payload is not None
        digest = hashlib.sha256(payload).hexdigest()
        if actual.get("bytes") != len(payload) or actual.get("sha256") != digest:
            raise DataError(f"source {source_id} artifact {index} hash or size differs")
        diagnostics = validate_artifact_payload(
            state.root,
            relative_path,
            payload,
            str(expected["media_type"]),
            str(expected["storage"]),
        )
        if diagnostics:
            raise DataError(
                f"{item.code} {item.subject}: {item.message}" for item in diagnostics
            )
    expected_revision = _expected_revision(realization, actual_artifacts, retrieved_at)
    if source.get("revision") != expected_revision:
        raise DataError(f"source {source_id} revision differs")
    if source.get("redistribution_note") != ARTIFACT_NOTE:
        raise DataError(f"source {source_id} redistribution note differs")


def _fsync_directory(path: Path) -> None:
    try:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
    except OSError as error:
        raise DataError(f"cannot fsync directory {path}: {error}") from error


def _stage_bytes(target: Path, payload: bytes) -> Path:
    temporary = target.with_name(f".{target.name}.aius-{secrets.token_hex(8)}.tmp")
    descriptor: int | None = None
    try:
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "wb", closefd=True) as handle:
            descriptor = None
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
    except Exception as error:
        if descriptor is not None:
            try:
                os.close(descriptor)
            except OSError:
                pass
        messages = [f"cannot stage {target}: {error}"]
        messages.extend(_cleanup([temporary]))
        raise DataError(messages) from error
    return temporary


def _cleanup(paths: Iterable[Path]) -> tuple[str, ...]:
    errors: list[str] = []
    for path in paths:
        try:
            path.unlink()
        except FileNotFoundError:
            pass
        except OSError as error:
            errors.append(f"cleanup failed for {path}: {error}")
    return tuple(errors)


def _publish_new(
    root: Path,
    outputs: Mapping[Path, bytes],
    replace: Callable[[str, str], object] = os.replace,
) -> None:
    staged: dict[Path, Path] = {}
    published: list[Path] = []
    try:
        for target, payload in outputs.items():
            try:
                _check_parent_chain(root, target)
            except ControlError as error:
                raise DataError(str(error)) from error
            if _lstat(target) is not None:
                raise DataError(f"refusing to overwrite existing path: {target.relative_to(root)}")
            staged[target] = _stage_bytes(target, payload)
        for target, temporary in staged.items():
            try:
                _check_parent_chain(root, target)
            except ControlError as error:
                raise DataError(str(error)) from error
            if _lstat(target) is not None:
                raise DataError(f"refusing to overwrite existing path: {target.relative_to(root)}")
            replace(os.fspath(temporary), os.fspath(target))
            published.append(target)
            _fsync_directory(target.parent)
    except Exception as error:
        rollback_errors: list[str] = []
        for target in reversed(published):
            try:
                target.unlink(missing_ok=True)
                _fsync_directory(target.parent)
            except Exception as rollback_error:
                rollback_errors.append(
                    f"rollback failed for {target}: {rollback_error}"
                )
        rollback_errors.extend(_cleanup(staged.values()))
        if rollback_errors:
            raise DataError(
                [f"transactional publish failed: {error}", *rollback_errors]
            ) from error
        if isinstance(error, (ControlError, DataError)):
            raise
        raise DataError(f"transactional publish failed: {error}") from error


def _replace_aggregates(
    root: Path,
    outputs: Mapping[Path, bytes],
    replace: Callable[[str, str], object] = os.replace,
) -> None:
    ordered_outputs = dict(
        sorted(
            outputs.items(),
            key=lambda item: item[0].relative_to(root).as_posix(),
        )
    )
    prior: dict[Path, bytes | None] = {}
    staged: dict[Path, Path] = {}
    published: list[Path] = []
    try:
        for target, payload in ordered_outputs.items():
            _check_parent_chain(root, target)
            status = _lstat(target)
            if status is not None and (
                stat.S_ISLNK(status.st_mode) or not stat.S_ISREG(status.st_mode)
            ):
                raise ControlError(
                    f"aggregate target is not a regular non-symlink file: {target.relative_to(root)}"
                )
            prior[target] = target.read_bytes() if status is not None else None
            staged[target] = _stage_bytes(target, payload)
        for target, temporary in staged.items():
            _check_parent_chain(root, target)
            status = _lstat(target)
            if status is not None and (
                stat.S_ISLNK(status.st_mode) or not stat.S_ISREG(status.st_mode)
            ):
                raise ControlError(f"aggregate target became unsafe: {target.relative_to(root)}")
            published.append(target)
            replace(os.fspath(temporary), os.fspath(target))
            _fsync_directory(target.parent)
    except Exception as error:
        rollback_errors: list[str] = []
        for target in reversed(published):
            try:
                old_payload = prior[target]
                if old_payload is None:
                    target.unlink(missing_ok=True)
                else:
                    restore = _stage_bytes(target, old_payload)
                    try:
                        replace(os.fspath(restore), os.fspath(target))
                    finally:
                        rollback_errors.extend(_cleanup([restore]))
                _fsync_directory(target.parent)
            except Exception as rollback_error:
                rollback_errors.append(f"rollback failed for {target}: {rollback_error}")
        rollback_errors.extend(_cleanup(staged.values()))
        if rollback_errors:
            raise DataError(
                [f"aggregate publish failed: {error}", *rollback_errors]
            ) from error
        if isinstance(error, (ControlError, DataError)):
            raise
        raise DataError(f"aggregate publish failed: {error}") from error


def _https_url(value: str, label: str) -> str:
    parsed = urllib.parse.urlsplit(value)
    if parsed.scheme != "https" or not parsed.netloc:
        raise DataError(f"{label} must be HTTPS: {value!r}")
    return value


def _status(response: object) -> int:
    value = getattr(response, "status", None)
    if value is None:
        value = response.getcode()
    return int(value)


def _retrieve(
    url: str,
    expected_final_url: str,
    opener: object,
) -> bytes:
    current = _https_url(url, "retrieval URL")
    _https_url(expected_final_url, "expected final URL")
    redirects = 0
    while True:
        request = urllib.request.Request(
            current,
            headers={"User-Agent": USER_AGENT, "Accept-Encoding": "identity"},
            method="GET",
        )
        try:
            response = opener.open(request, timeout=TIMEOUT_SECONDS)
        except urllib.error.HTTPError as error:
            response = error
        except (OSError, urllib.error.URLError) as error:
            raise DataError(f"retrieval failed for {current}: {error}") from error
        try:
            response_url = str(response.geturl())
            _https_url(response_url, "response final URL")
            status_code = _status(response)
            headers = response.headers
            if status_code in REDIRECT_STATUSES:
                if redirects >= MAX_REDIRECTS:
                    raise DataError(f"redirect limit exceeded for {url}")
                location = headers.get("Location")
                if not location:
                    raise DataError(f"redirect response has no Location for {current}")
                current = _https_url(
                    urllib.parse.urljoin(current, location), "redirect target"
                )
                redirects += 1
                continue
            if status_code != 200:
                raise DataError(f"terminal HTTP status is {status_code} for {current}")
            if response_url != expected_final_url:
                raise DataError(
                    f"final URL differs: expected {expected_final_url!r}, got {response_url!r}"
                )
            encoding = headers.get("Content-Encoding")
            if encoding is not None and encoding.strip().lower() != "identity":
                raise DataError(f"unsupported Content-Encoding {encoding!r}")
            content_length = headers.get("Content-Length")
            if content_length is not None:
                try:
                    declared = int(content_length)
                except ValueError as error:
                    raise DataError(f"invalid Content-Length {content_length!r}") from error
                if declared < 0 or declared > MAX_PAYLOAD_BYTES:
                    raise DataError(f"payload exceeds {MAX_PAYLOAD_BYTES} bytes")
            payload = bytearray()
            try:
                while True:
                    chunk = response.read(64 * 1024)
                    if not chunk:
                        break
                    payload.extend(chunk)
                    if len(payload) > MAX_PAYLOAD_BYTES:
                        raise DataError(f"payload exceeds {MAX_PAYLOAD_BYTES} bytes")
            except DataError:
                raise
            except (
                OSError,
                urllib.error.URLError,
                http.client.HTTPException,
                EOFError,
            ) as error:
                raise DataError(f"streamed read failed for {current}: {error}") from error
            return bytes(payload)
        finally:
            try:
                response.close()
            except Exception:
                pass


def _production_opener() -> object:
    return urllib.request.build_opener(urllib.request.ProxyHandler({}), _NoRedirect())


def _fragment_path(source_id: str) -> str:
    return f"evidence/manifest/fragments/{source_id}.json"


def _assumption_fragment_path(source_id: str) -> str:
    return f"evidence/manifest/assumption-fragments/{source_id}.json"


def _supplemental_fragment_path(source_id: str) -> str:
    return f"evidence/manifest/supplemental-fragments/{source_id}.json"


def _supplemental_assumption_fragment_path(source_id: str) -> str:
    return (
        "evidence/manifest/supplemental-assumption-fragments/"
        f"{source_id}.json"
    )


def _prepare_output_directories(state: State, realization: Mapping[str, object]) -> None:
    _ensure_directory(state.root, "evidence/manifest/fragments")
    for artifact in realization["artifacts"]:
        assert isinstance(artifact, dict)
        path = PurePosixPath(str(artifact["path"]))
        _ensure_directory(state.root, path.parent.as_posix())


def _prepare_supplemental_output_directories(
    state: SupplementalState,
    realization: Mapping[str, object],
) -> None:
    _ensure_directory(state.root, "evidence/manifest/supplemental-fragments")
    artifacts = realization["artifacts"]
    assert isinstance(artifacts, list)
    for artifact in artifacts:
        assert isinstance(artifact, dict)
        path = PurePosixPath(str(artifact["path"]))
        _ensure_directory(state.root, path.parent.as_posix())


def _supplemental_property_order(
    value: Mapping[str, object],
    constructor_keys: tuple[str, ...],
    label: str,
    *,
    published: bool,
) -> None:
    expected = tuple(sorted(constructor_keys)) if published else constructor_keys
    if tuple(value) != expected:
        raise DataError(f"{label} property order differs")


def _validate_supplemental_manufacturer_source(
    state: SupplementalState,
    request: Mapping[str, object],
    realization: Mapping[str, object],
    source: object,
    payloads: Mapping[str, bytes],
    *,
    published: bool = False,
) -> None:
    source_record_schema = state.source_schema["$defs"]["sourceRecord"]
    _schema(
        source,
        source_record_schema,
        state.source_schema,
        f"supplemental source {request['source_id']}",
    )
    if not isinstance(source, dict):
        raise DataError("supplemental manufacturer source must be an object")
    _supplemental_property_order(
        source,
        SUPPLEMENTAL_SOURCE_KEYS,
        "supplemental manufacturer source",
        published=published,
    )
    retrieved_at = source.get("retrieved_at")
    if not isinstance(retrieved_at, str):
        raise DataError("supplemental manufacturer source has no timestamp")
    _timestamp(retrieved_at)
    actual_artifacts = source.get("artifacts")
    expected_artifacts = realization["artifacts"]
    if not isinstance(actual_artifacts, list) or not isinstance(
        expected_artifacts, list
    ):
        raise DataError("supplemental manufacturer artifacts are invalid")
    if len(actual_artifacts) != len(expected_artifacts):
        raise DataError("supplemental manufacturer artifact count differs")
    for index, (actual, expected) in enumerate(
        zip(actual_artifacts, expected_artifacts, strict=True)
    ):
        if not isinstance(actual, dict) or not isinstance(expected, dict):
            raise DataError(f"supplemental artifact {index} is invalid")
        _supplemental_property_order(
            actual,
            SUPPLEMENTAL_ARTIFACT_KEYS,
            f"supplemental artifact {index}",
            published=published,
        )
        exact = {
            "name": expected["name"],
            "final_url": expected["expected_final_url"],
            "storage": expected["storage"],
            "path": expected["path"],
            "media_type": expected["media_type"],
            "retrieval_status": "current",
            "note": "",
        }
        for key, value in exact.items():
            if actual.get(key) != value:
                raise DataError(
                    f"supplemental artifact {index} field {key!r} differs"
                )
        relative_path = str(expected["path"])
        payload = payloads.get(relative_path)
        if payload is None:
            raise DataError(f"supplemental artifact payload is missing: {relative_path}")
        digest = hashlib.sha256(payload).hexdigest()
        if actual.get("sha256") != digest or actual.get("bytes") != len(payload):
            raise DataError(f"supplemental artifact {index} hash or size differs")
        diagnostics = validate_artifact_payload(
            state.root,
            relative_path,
            payload,
            str(expected["media_type"]),
            str(expected["storage"]),
        )
        if diagnostics:
            raise DataError(
                f"{item.code} {item.subject}: {item.message}" for item in diagnostics
            )
    constructor_artifacts = [
        {key: artifact[key] for key in SUPPLEMENTAL_ARTIFACT_KEYS}
        for artifact in actual_artifacts
    ]
    expected_source = _supplemental_current_manufacturer_source(
        request,
        realization,
        constructor_artifacts,
        retrieved_at,
    )
    if source != expected_source:
        raise DataError("supplemental manufacturer source differs from its constructor")


def _validate_supplemental_uk_source(
    state: SupplementalState,
    request: Mapping[str, object],
    realization: Mapping[str, object],
    source: object,
    *,
    published: bool = False,
) -> None:
    source_record_schema = state.source_schema["$defs"]["sourceRecord"]
    _schema(
        source,
        source_record_schema,
        state.source_schema,
        f"supplemental source {request['source_id']}",
    )
    if not isinstance(source, dict):
        raise DataError("supplemental UK source must be an object")
    _supplemental_property_order(
        source,
        SUPPLEMENTAL_SOURCE_KEYS,
        "supplemental UK source",
        published=published,
    )
    observed_at = source.get("retrieved_at")
    if not isinstance(observed_at, str):
        raise DataError("supplemental UK source has no timestamp")
    _timestamp(observed_at)
    profile = _supplemental_observation_profile(state, realization)
    fields = profile["fields"]
    observations = source.get("observations")
    if not isinstance(fields, list) or not isinstance(observations, list):
        raise DataError("supplemental UK source observations are invalid")
    if len(observations) != len(fields):
        raise DataError("supplemental UK source observation count differs")
    values: dict[str, object] = {}
    for index, (field, observation) in enumerate(
        zip(fields, observations, strict=True)
    ):
        if not isinstance(field, dict) or not isinstance(observation, dict):
            raise DataError(f"supplemental UK observation {index} is invalid")
        _supplemental_property_order(
            observation,
            SUPPLEMENTAL_OBSERVATION_KEYS,
            f"supplemental UK observation {index}",
            published=published,
        )
        key = str(field["key"])
        if observation.get("key") != key:
            raise DataError("supplemental UK observation order differs from profile")
        values[key] = observation.get("value")
    product_final_url = values.get("product_final_url")
    authorization_final_url = values.get("authorization_final_url")
    if not isinstance(product_final_url, str) or not isinstance(
        authorization_final_url, str
    ):
        raise DataError("supplemental UK final URLs are invalid")
    expected_source = _supplemental_current_uk_source(
        request,
        realization,
        profile,
        values,
        product_final_url,
        authorization_final_url,
        observed_at,
    )
    if source != expected_source:
        raise DataError("supplemental UK source differs from its constructor")


def _validate_supplemental_assumption(
    state: SupplementalState,
    request: Mapping[str, object],
    realization: Mapping[str, object],
    source: Mapping[str, object],
    assumption: object | None,
    *,
    published: bool,
) -> None:
    required = (
        source.get("status") == "unavailable"
        and realization["failure_policy"] == "assumption"
    )
    source_id = str(request["source_id"])
    if not required:
        if assumption is not None:
            raise DataError(
                f"supplemental source {source_id} has an unexpected assumption fragment"
            )
        return
    if assumption is None:
        raise DataError(
            f"supplemental source {source_id} requires an unavailable assumption"
        )
    assumption_record_schema = state.assumption_schema["$defs"]["assumptionRecord"]
    _schema(
        assumption,
        assumption_record_schema,
        state.assumption_schema,
        f"supplemental assumption {source_id}",
    )
    assert isinstance(assumption, dict)
    _supplemental_property_order(
        assumption,
        SUPPLEMENTAL_ASSUMPTION_KEYS,
        "supplemental assumption",
        published=published,
    )
    if assumption != _expected_supplemental_assumption(request):
        raise DataError(
            f"supplemental source {source_id} has a non-canonical unavailable assumption"
        )


def _validate_supplemental_unavailable_source(
    state: SupplementalState,
    request: Mapping[str, object],
    realization: Mapping[str, object],
    source: object,
    assumption: object | None,
    *,
    published: bool = False,
) -> None:
    source_id = str(request["source_id"])
    source_record_schema = state.source_schema["$defs"]["sourceRecord"]
    _schema(
        source,
        source_record_schema,
        state.source_schema,
        f"supplemental source {source_id}",
    )
    if not isinstance(source, dict):
        raise DataError("supplemental unavailable source must be an object")
    _supplemental_property_order(
        source,
        SUPPLEMENTAL_SOURCE_KEYS,
        "supplemental unavailable source",
        published=published,
    )
    retrieved_at = source.get("retrieved_at")
    if not isinstance(retrieved_at, str):
        raise DataError("supplemental unavailable source has no timestamp")
    _timestamp(retrieved_at)
    artifacts = source.get("artifacts")
    if (
        not isinstance(artifacts, list)
        or len(artifacts) != 1
        or not isinstance(artifacts[0], dict)
    ):
        raise DataError("supplemental unavailable source metadata artifact is invalid")
    artifact = artifacts[0]
    _supplemental_property_order(
        artifact,
        SUPPLEMENTAL_ARTIFACT_KEYS,
        "supplemental unavailable artifact",
        published=published,
    )
    note = artifact.get("note")
    if not isinstance(note, str) or not note.strip():
        raise DataError("supplemental unavailable artifact note must be non-empty")
    expected = _supplemental_unavailable_source(
        request,
        realization,
        note,
        retrieved_at,
    )
    if source != expected:
        raise DataError(
            f"supplemental unavailable source {source_id} differs from its constructor"
        )
    _validate_supplemental_assumption(
        state,
        request,
        realization,
        source,
        assumption,
        published=published,
    )


def _supplemental_acquire(
    state: SupplementalState,
    request_id: str,
    retrieved_at: str,
    opener: object,
    replace: Callable[[str, str], object],
    formatter: Formatter,
) -> int:
    request, realization = _select_supplemental(state, request_id)
    if request["request_kind"] != "manufacturer-evidence":
        raise DataError(f"UK supplemental request {request_id} does not support acquire")
    retrieved_at = _timestamp(retrieved_at)
    _prepare_supplemental_output_directories(state, realization)
    source_id = str(request["source_id"])
    fragment_relative = _supplemental_fragment_path(source_id)
    _reject_target(state.root, fragment_relative)
    payloads: dict[str, bytes] = {}
    artifacts: list[dict[str, object]] = []
    realization_artifacts = realization["artifacts"]
    assert isinstance(realization_artifacts, list)
    for entry in realization_artifacts:
        assert isinstance(entry, dict)
        relative_path = str(entry["path"])
        _reject_target(state.root, relative_path)
        payload = _retrieve(
            str(entry["retrieval_url"]),
            str(entry["expected_final_url"]),
            opener,
        )
        diagnostics = validate_artifact_payload(
            state.root,
            relative_path,
            payload,
            str(entry["media_type"]),
            str(entry["storage"]),
        )
        if diagnostics:
            raise DataError(
                f"{item.code} {item.subject}: {item.message}" for item in diagnostics
            )
        payloads[relative_path] = payload
        artifacts.append(
            {
                "name": entry["name"],
                "final_url": entry["expected_final_url"],
                "storage": entry["storage"],
                "path": relative_path,
                "sha256": hashlib.sha256(payload).hexdigest(),
                "bytes": len(payload),
                "media_type": entry["media_type"],
                "retrieval_status": "current",
                "note": "",
            }
        )
    source = _supplemental_current_manufacturer_source(
        request,
        realization,
        artifacts,
        retrieved_at,
    )
    _validate_supplemental_manufacturer_source(
        state,
        request,
        realization,
        source,
        payloads,
    )
    outputs = {
        _root_path(state.root, path): payload for path, payload in payloads.items()
    }
    outputs[_root_path(state.root, fragment_relative)] = _render_json(
        state.root,
        source,
        formatter,
    )
    _publish_new(state.root, outputs, replace)
    return 0


def _supplemental_record(
    state: SupplementalState,
    request_id: str,
    values_text: str,
    product_final_url: str,
    authorization_final_url: str,
    observed_at: str,
    replace: Callable[[str, str], object],
    formatter: Formatter,
) -> int:
    request, realization = _select_supplemental(state, request_id)
    if request["request_kind"] != "uk-availability":
        raise DataError(
            f"manufacturer supplemental request {request_id} does not support record"
        )
    authorization = realization.get("authorization")
    if not isinstance(authorization, dict):
        raise ControlError(f"{request_id}: UK authorization tuple is invalid")
    if authorization.get("status") != "confirmed":
        raise DataError(
            f"{request_id}: authorization is unresolved; use supplemental unavailable"
        )
    observed_at = _timestamp(observed_at)
    values = _parse_values(values_text)
    profile = _supplemental_observation_profile(state, realization)
    source = _supplemental_current_uk_source(
        request,
        realization,
        profile,
        values,
        product_final_url,
        authorization_final_url,
        observed_at,
    )
    _validate_supplemental_uk_source(state, request, realization, source)
    _ensure_directory(state.root, "evidence/manifest/supplemental-fragments")
    fragment_relative = _supplemental_fragment_path(str(request["source_id"]))
    target = _reject_target(state.root, fragment_relative)
    _publish_new(
        state.root,
        {target: _render_json(state.root, source, formatter)},
        replace,
    )
    return 0


def _supplemental_unavailable(
    state: SupplementalState,
    request_id: str,
    note: str,
    retrieved_at: str,
    replace: Callable[[str, str], object],
    formatter: Formatter,
) -> int:
    request, realization = _select_supplemental(state, request_id)
    if not note.strip():
        raise DataError("--note must contain non-whitespace text")
    retrieved_at = _timestamp(retrieved_at)
    _ensure_directory(state.root, "evidence/manifest/supplemental-fragments")
    source_id = str(request["source_id"])
    fragment_relative = _supplemental_fragment_path(source_id)
    fragment_target = _reject_target(state.root, fragment_relative)

    source = _supplemental_unavailable_source(
        request,
        realization,
        note,
        retrieved_at,
    )
    assumption: dict[str, object] | None = None
    assumption_target: Path | None = None
    if realization["failure_policy"] == "assumption":
        _ensure_directory(
            state.root,
            "evidence/manifest/supplemental-assumption-fragments",
        )
        assumption_relative = _supplemental_assumption_fragment_path(source_id)
        assumption_target = _reject_target(state.root, assumption_relative)
        assumption = _expected_supplemental_assumption(request)

    _validate_supplemental_unavailable_source(
        state,
        request,
        realization,
        source,
        assumption,
    )
    outputs = {
        fragment_target: _render_json(state.root, source, formatter),
    }
    if assumption_target is not None:
        assert assumption is not None
        outputs[assumption_target] = _render_json(
            state.root,
            assumption,
            formatter,
        )
    _publish_new(state.root, outputs, replace)
    return 3 if request["request_kind"] == "manufacturer-evidence" else 0


def _load_supplemental_published_json(
    state: SupplementalState,
    relative_text: str,
    formatter: Formatter,
    *,
    required: bool = True,
) -> object | None:
    try:
        payload = _read_regular(state.root, relative_text, required=required)
        if payload is None:
            return None
        value = _strict_rendered_json(payload)
    except ControlError as error:
        raise DataError(str(error)) from error
    if not _object_keys_are_sorted(value):
        raise DataError(f"published JSON object-key order differs: {relative_text}")
    expected = _render_json(state.root, value, formatter)
    if payload != expected:
        raise DataError(f"published JSON renderer bytes differ: {relative_text}")
    return value


def _load_optional_supplemental_assumption(
    state: SupplementalState,
    source_id: str,
    formatter: Formatter,
) -> object | None:
    directory = _root_path(
        state.root,
        "evidence/manifest/supplemental-assumption-fragments",
    )
    status = _lstat(directory)
    if status is None:
        return None
    if stat.S_ISLNK(status.st_mode) or not stat.S_ISDIR(status.st_mode):
        raise DataError("supplemental assumption fragment directory is unsafe")
    return _load_supplemental_published_json(
        state,
        _supplemental_assumption_fragment_path(source_id),
        formatter,
        required=False,
    )


def _supplemental_check_fragment(
    state: SupplementalState,
    request_id: str,
    formatter: Formatter,
) -> int:
    request, realization = _select_supplemental(state, request_id)
    source_id = str(request["source_id"])
    source = _load_supplemental_published_json(
        state,
        _supplemental_fragment_path(source_id),
        formatter,
    )
    if not isinstance(source, dict):
        raise DataError(f"supplemental source {source_id} must be an object")
    assumption = _load_optional_supplemental_assumption(
        state,
        source_id,
        formatter,
    )
    status = source.get("status")
    if status == "unavailable":
        _validate_supplemental_unavailable_source(
            state,
            request,
            realization,
            source,
            assumption,
            published=True,
        )
        return 0
    if status != "current":
        raise DataError(
            f"supplemental source {source_id} must be current or unavailable"
        )
    _validate_supplemental_assumption(
        state,
        request,
        realization,
        source,
        assumption,
        published=True,
    )
    if request["request_kind"] == "manufacturer-evidence":
        payloads: dict[str, bytes] = {}
        realization_artifacts = realization["artifacts"]
        assert isinstance(realization_artifacts, list)
        for artifact in realization_artifacts:
            assert isinstance(artifact, dict)
            relative_path = str(artifact["path"])
            try:
                payload = _read_regular(state.root, relative_path)
            except ControlError as error:
                raise DataError(str(error)) from error
            assert payload is not None
            payloads[relative_path] = payload
        _validate_supplemental_manufacturer_source(
            state,
            request,
            realization,
            source,
            payloads,
            published=True,
        )
    else:
        _validate_supplemental_uk_source(
            state,
            request,
            realization,
            source,
            published=True,
        )
    return 0


def _acquire(
    state: State,
    source_id: str,
    retrieved_at: str,
    opener: object,
    replace: Callable[[str, str], object],
    formatter: Formatter,
) -> int:
    seed, realization = _select(state, source_id)
    if realization["mode"] == "dynamic":
        raise DataError(f"dynamic source {source_id} must use record")
    retrieved_at = _timestamp(retrieved_at)
    _prepare_output_directories(state, realization)
    fragment_relative = _fragment_path(source_id)
    _reject_target(state.root, fragment_relative)
    payloads: dict[str, bytes] = {}
    artifacts: list[dict[str, object]] = []
    for entry in realization["artifacts"]:
        assert isinstance(entry, dict)
        relative_path = str(entry["path"])
        _reject_target(state.root, relative_path)
        payload = _retrieve(
            str(entry["retrieval_url"]),
            str(entry["expected_final_url"]),
            opener,
        )
        diagnostics = validate_artifact_payload(
            state.root,
            relative_path,
            payload,
            str(entry["media_type"]),
            str(entry["storage"]),
        )
        if diagnostics:
            raise DataError(
                f"{item.code} {item.subject}: {item.message}" for item in diagnostics
            )
        digest = hashlib.sha256(payload).hexdigest()
        payloads[relative_path] = payload
        artifacts.append(
            {
                "name": entry["name"],
                "final_url": entry["expected_final_url"],
                "storage": entry["storage"],
                "path": relative_path,
                "sha256": digest,
                "bytes": len(payload),
                "media_type": entry["media_type"],
                "retrieval_status": "current",
                "note": ARTIFACT_NOTE,
            }
        )
    source = _source_base(
        seed,
        revision=_expected_revision(realization, artifacts, retrieved_at),
        retrieved_at=retrieved_at,
        status="current",
        redistribution_note=ARTIFACT_NOTE,
        artifacts=artifacts,
        observations=[],
    )
    _validate_source(state, source_id, source, None, payloads=payloads)
    outputs = {
        _root_path(state.root, path): payload for path, payload in payloads.items()
    }
    outputs[_root_path(state.root, fragment_relative)] = _render_json(
        state.root, source, formatter
    )
    _publish_new(state.root, outputs, replace)
    return 0


def _record(
    state: State,
    source_id: str,
    values_text: str,
    observed_at: str,
    replace: Callable[[str, str], object],
    formatter: Formatter,
) -> int:
    seed, realization = _select(state, source_id)
    if realization["mode"] != "dynamic":
        raise DataError(f"non-dynamic source {source_id} must use acquire")
    observed_at = _timestamp(observed_at)
    values = _parse_values(values_text)
    observations = _observations(seed, realization, values, observed_at)
    source = _source_base(
        seed,
        revision=_expected_revision(realization, [], observed_at),
        retrieved_at=observed_at,
        status="current",
        redistribution_note=DYNAMIC_NOTE,
        artifacts=[],
        observations=observations,
    )
    _validate_source(state, source_id, source, None)
    _ensure_directory(state.root, "evidence/manifest/fragments")
    fragment_relative = _fragment_path(source_id)
    target = _reject_target(state.root, fragment_relative)
    _publish_new(
        state.root,
        {target: _render_json(state.root, source, formatter)},
        replace,
    )
    return 0


def _unavailable(
    state: State,
    source_id: str,
    note: str,
    retrieved_at: str,
    replace: Callable[[str, str], object],
    formatter: Formatter,
) -> int:
    seed, realization = _select(state, source_id)
    if not note.strip():
        raise DataError("--note must contain non-whitespace text")
    retrieved_at = _timestamp(retrieved_at)
    _ensure_directory(state.root, "evidence/manifest/fragments")
    fragment_relative = _fragment_path(source_id)
    fragment_target = _reject_target(state.root, fragment_relative)
    source = _source_base(
        seed,
        revision=f"unavailable:{retrieved_at[:10]}",
        retrieved_at=retrieved_at,
        status="unavailable",
        redistribution_note=UNAVAILABLE_NOTE,
        artifacts=[
            {
                "name": "unavailable",
                "final_url": seed["landing_url"],
                "storage": "external-reference",
                "path": None,
                "sha256": None,
                "bytes": None,
                "media_type": None,
                "retrieval_status": "unavailable",
                "note": note,
            }
        ],
        observations=[],
    )
    outputs = {fragment_target: _render_json(state.root, source, formatter)}
    assumption: dict[str, object] | None = None
    if realization["failure_policy"] == "assumption":
        _ensure_directory(state.root, "evidence/manifest/assumption-fragments")
        assumption_relative = _assumption_fragment_path(source_id)
        assumption_target = _reject_target(state.root, assumption_relative)
        assumption = _expected_assumption(seed)
        outputs[assumption_target] = _render_json(
            state.root, assumption, formatter
        )
    _validate_source(state, source_id, source, assumption)
    _publish_new(state.root, outputs, replace)
    return 3 if realization["failure_policy"] == "golden-block" else 1


def _load_fragment(state: State, source_id: str) -> object:
    directory = _root_path(state.root, "evidence/manifest/fragments")
    status = _lstat(directory)
    if status is None:
        raise DataError("source fragment directory is missing")
    if stat.S_ISLNK(status.st_mode) or not stat.S_ISDIR(status.st_mode):
        raise DataError("source fragment directory is unsafe")
    try:
        return _load_json(state.root, _fragment_path(source_id))
    except ControlError as error:
        raise DataError(str(error)) from error


def _optional_assumption(state: State, source_id: str) -> object | None:
    directory = _root_path(state.root, "evidence/manifest/assumption-fragments")
    status = _lstat(directory)
    if status is None:
        return None
    if stat.S_ISLNK(status.st_mode) or not stat.S_ISDIR(status.st_mode):
        raise DataError("assumption fragment directory is unsafe")
    try:
        payload = _read_regular(
            state.root,
            _assumption_fragment_path(source_id),
            required=False,
        )
        if payload is None:
            return None
        return _decode_json(payload, _assumption_fragment_path(source_id))
    except ControlError as error:
        raise DataError(str(error)) from error


def _check_fragments(state: State, source_ids: list[str]) -> int:
    errors: list[str] = []
    for source_id in source_ids:
        try:
            _select(state, source_id)
            source = _load_fragment(state, source_id)
            assumption = _optional_assumption(state, source_id)
            _validate_source(state, source_id, source, assumption)
        except DataError as error:
            errors.extend(error.messages)
    if errors:
        raise DataError(errors)
    return 0


def _scan_json_directory(
    state: State,
    relative_text: str,
    *,
    required: bool,
) -> dict[str, object]:
    path = _root_path(state.root, relative_text)
    _check_parent_chain(state.root, path)
    status = _lstat(path)
    if status is None:
        if required:
            raise DataError(f"required fragment directory is missing: {relative_text}")
        return {}
    if stat.S_ISLNK(status.st_mode) or not stat.S_ISDIR(status.st_mode):
        raise DataError(f"fragment path is not a regular directory: {relative_text}")
    result: dict[str, object] = {}
    errors: list[str] = []
    try:
        entries = list(os.scandir(path))
    except OSError as error:
        raise ControlError(f"cannot scan {relative_text}: {error}") from error
    for entry in entries:
        entry_path = Path(entry.path)
        entry_status = _lstat(entry_path)
        match = re.fullmatch(r"([A-Z0-9]+(?:-[A-Z0-9]+)*)\.json", entry.name)
        if (
            entry_status is None
            or stat.S_ISLNK(entry_status.st_mode)
            or not stat.S_ISREG(entry_status.st_mode)
            or match is None
        ):
            errors.append(f"unexpected or unsafe fragment entry: {relative_text}/{entry.name}")
            continue
        source_id = match.group(1)
        try:
            payload = _read_regular(state.root, f"{relative_text}/{entry.name}")
            assert payload is not None
            result[source_id] = _decode_json(
                payload, f"{relative_text}/{entry.name}"
            )
        except ControlError as error:
            errors.append(str(error))
    if errors:
        raise DataError(errors)
    return result


def _assemble_data(state: State) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    fragment_rows = _scan_json_directory(
        state, "evidence/manifest/fragments", required=True
    )
    expected_ids = set(state.seeds_by_id)
    actual_ids = set(fragment_rows)
    if actual_ids != expected_ids:
        raise DataError(
            f"fragment IDs differ; missing={sorted(expected_ids - actual_ids)!r}, "
            f"extra={sorted(actual_ids - expected_ids)!r}"
        )
    fragment_sources: list[tuple[str, str, dict[str, object]]] = []
    seen_record_ids: set[str] = set()
    duplicate_record_ids: set[str] = set()
    sources: list[dict[str, object]] = []
    for filename_id, source in fragment_rows.items():
        if not isinstance(source, dict):
            raise DataError(f"fragment {filename_id} is not a source object")
        record_id = source.get("id")
        if not isinstance(record_id, str):
            raise DataError(f"fragment {filename_id} has no source ID")
        if record_id in seen_record_ids:
            duplicate_record_ids.add(record_id)
        seen_record_ids.add(record_id)
        fragment_sources.append((filename_id, record_id, source))
    if duplicate_record_ids:
        raise DataError(
            f"duplicate source record ID: {sorted(duplicate_record_ids)[0]}"
        )
    for filename_id, record_id, source in fragment_sources:
        if record_id != filename_id:
            raise DataError(
                f"fragment filename ID {filename_id!r} does not match object ID {record_id!r}"
            )
        sources.append(source)
    unavailable_golden = sorted(
        str(source.get("id"))
        for source in sources
        if source.get("status") == "unavailable"
        and state.realizations_by_id.get(str(source.get("id")), {}).get(
            "failure_policy"
        )
        == "golden-block"
    )
    if unavailable_golden:
        raise DataError(
            f"golden unavailable fragments block assembly: {unavailable_golden!r}"
        )

    assumption_rows = _scan_json_directory(
        state, "evidence/manifest/assumption-fragments", required=False
    )
    expected_assumption_ids = {
        str(source["id"])
        for source in sources
        if source.get("status") == "unavailable"
        and state.realizations_by_id.get(str(source.get("id")), {}).get(
            "failure_policy"
        )
        == "assumption"
    }
    if set(assumption_rows) != expected_assumption_ids:
        raise DataError(
            "assumption fragment IDs differ; "
            f"missing={sorted(expected_assumption_ids - set(assumption_rows))!r}, "
            f"extra={sorted(set(assumption_rows) - expected_assumption_ids)!r}"
        )
    assumption_ids: set[str] = set()
    assumptions: list[dict[str, object]] = []
    for filename_id, assumption in assumption_rows.items():
        if not isinstance(assumption, dict):
            raise DataError(f"assumption fragment {filename_id} is not an object")
        record_id = assumption.get("id")
        if not isinstance(record_id, str):
            raise DataError(f"assumption fragment {filename_id} has no ID")
        if record_id in assumption_ids:
            raise DataError(f"duplicate assumption record ID: {record_id}")
        assumption_ids.add(record_id)
        assumptions.append(assumption)

    for source in sources:
        source_id = str(source.get("id"))
        _validate_source(state, source_id, source, assumption_rows.get(source_id))
    sources.sort(key=lambda row: str(row["id"]))
    assumptions.sort(key=lambda row: str(row["id"]))
    _schema(sources, state.source_schema, state.source_schema, "assembled sources")
    _schema(
        assumptions,
        state.assumption_schema,
        state.assumption_schema,
        "assembled assumptions",
    )
    try:
        validate_source_collection(sources, list(state.seeds))
    except SourceCollectionError as error:
        raise DataError(error.errors) from error
    return sources, assumptions


def _assemble(
    state: State,
    mode: str,
    replace: Callable[[str, str], object],
    formatter: Formatter,
) -> int:
    sources, assumptions = _assemble_data(state)
    rendered = {
        "evidence/manifest/sources.json": _render_json(
            state.root, sources, formatter
        ),
        "evidence/manifest/assumptions.json": _render_json(
            state.root, assumptions, formatter
        ),
    }
    if mode == "check":
        errors: list[str] = []
        for relative, expected in rendered.items():
            actual = _read_regular(state.root, relative)
            if actual != expected:
                errors.append(f"aggregate drift: {relative}")
        if errors:
            raise DataError(errors)
        return 0
    outputs = {
        _root_path(state.root, relative): payload
        for relative, payload in rendered.items()
    }
    _replace_aggregates(state.root, outputs, replace)
    return 0


@dataclass(frozen=True)
class SupplementalAssembly:
    sources: tuple[dict[str, object], ...]
    assumptions: tuple[dict[str, object], ...]
    clone_boundaries: tuple[dict[str, object], ...]
    requests: tuple[dict[str, object], ...]
    affected_request_ids: tuple[str, ...]
    components: tuple[dict[str, object], ...]
    affected_component_ids: tuple[str, ...]
    selection_policy: dict[str, object]


def _scan_named_json_directory(
    root: Path,
    relative_text: str,
    name_pattern: str,
) -> dict[str, object]:
    path = _root_path(root, relative_text)
    _check_parent_chain(root, path)
    status = _lstat(path)
    if status is None:
        raise DataError(f"required fragment directory is missing: {relative_text}")
    if stat.S_ISLNK(status.st_mode) or not stat.S_ISDIR(status.st_mode):
        raise DataError(f"fragment path is not a regular directory: {relative_text}")
    result: dict[str, object] = {}
    errors: list[str] = []
    try:
        entries = list(os.scandir(path))
    except OSError as error:
        raise ControlError(f"cannot scan {relative_text}: {error}") from error
    for entry in entries:
        entry_path = Path(entry.path)
        entry_status = _lstat(entry_path)
        match = re.fullmatch(name_pattern, entry.name)
        if (
            entry_status is None
            or stat.S_ISLNK(entry_status.st_mode)
            or not stat.S_ISREG(entry_status.st_mode)
            or match is None
        ):
            errors.append(
                f"unexpected or unsafe fragment entry: {relative_text}/{entry.name}"
            )
            continue
        filename_id = match.group(1)
        try:
            payload = _read_regular(root, f"{relative_text}/{entry.name}")
            assert payload is not None
            result[filename_id] = _decode_json(
                payload,
                f"{relative_text}/{entry.name}",
            )
        except ControlError as error:
            errors.append(str(error))
    if errors:
        raise DataError(errors)
    return result


def _require_exact_ids(
    rows: Mapping[str, object],
    expected_ids: set[str],
    label: str,
) -> None:
    actual_ids = set(rows)
    if actual_ids != expected_ids:
        raise DataError(
            f"{label} IDs differ; missing={sorted(expected_ids - actual_ids)!r}, "
            f"extra={sorted(actual_ids - expected_ids)!r}"
        )


def _require_record_ids(
    rows: Mapping[str, object],
    record_key: str,
    label: str,
) -> dict[str, dict[str, object]]:
    by_record_id: dict[str, dict[str, object]] = {}
    duplicate_ids: set[str] = set()
    for filename_id, value in rows.items():
        if not isinstance(value, dict):
            raise DataError(f"{label} {filename_id} is not an object")
        record_id = value.get(record_key)
        if not isinstance(record_id, str):
            raise DataError(f"{label} {filename_id} has no {record_key}")
        if record_id != filename_id:
            raise DataError(
                f"{label} filename ID {filename_id!r} does not match object ID "
                f"{record_id!r}"
            )
        if record_id in by_record_id:
            duplicate_ids.add(record_id)
        by_record_id[record_id] = value
    if duplicate_ids:
        raise DataError(f"duplicate {label} record IDs: {sorted(duplicate_ids)!r}")
    return by_record_id


def _require_unchanged_except(
    before: Mapping[str, object],
    after: Mapping[str, object],
    allowed_keys: set[str],
    label: str,
) -> None:
    if tuple(before) != tuple(after):
        raise DataError(f"{label} property order changed during projection")
    for key in before:
        if key not in allowed_keys and not _same_json_value(before[key], after[key]):
            raise DataError(f"{label} field {key!r} changed during projection")
        if key not in allowed_keys and not _object_key_order_matches(
            before[key], after[key]
        ):
            raise DataError(f"{label} field {key!r} key order changed during projection")


def _supplemental_assembly_data(
    seed_state: State,
    state: SupplementalState,
    formatter: Formatter,
) -> SupplementalAssembly:
    seed_sources, seed_assumptions = _assemble_data(seed_state)
    expected_source_ids = {
        str(realization["source_id"]) for realization in state.realizations
    }
    seed_source_ids = {str(source["id"]) for source in seed_sources}
    collisions = seed_source_ids & expected_source_ids
    if collisions:
        raise DataError(f"seed and supplemental source IDs collide: {sorted(collisions)!r}")

    supplemental_rows = _scan_json_directory(
        seed_state,
        "evidence/manifest/supplemental-fragments",
        required=True,
    )
    _require_exact_ids(
        supplemental_rows,
        expected_source_ids,
        "supplemental fragment",
    )
    supplemental_sources = _require_record_ids(
        supplemental_rows,
        "id",
        "supplemental fragment",
    )

    supplemental_assumption_rows = _scan_json_directory(
        seed_state,
        "evidence/manifest/supplemental-assumption-fragments",
        required=False,
    )
    expected_supplemental_assumption_ids = {
        str(request["source_id"])
        for request in state.requests
        if str(request["request_id"]) not in PRESERVED_REQUEST_IDS
        and supplemental_sources[str(request["source_id"])].get("status")
        == "unavailable"
        and state.realizations_by_id[str(request["request_id"])]["failure_policy"]
        == "assumption"
    }
    _require_exact_ids(
        supplemental_assumption_rows,
        expected_supplemental_assumption_ids,
        "supplemental assumption fragment",
    )

    supplemental_assumptions: list[dict[str, object]] = []
    supplemental_assumption_record_ids: set[str] = set()
    for filename_id, value in supplemental_assumption_rows.items():
        if not isinstance(value, dict):
            raise DataError(
                f"supplemental assumption fragment {filename_id} is not an object"
            )
        record_id = value.get("id")
        if not isinstance(record_id, str):
            raise DataError(
                f"supplemental assumption fragment {filename_id} has no ID"
            )
        if record_id in supplemental_assumption_record_ids:
            raise DataError(f"duplicate supplemental assumption record ID: {record_id}")
        supplemental_assumption_record_ids.add(record_id)
        supplemental_assumptions.append(value)

    clone_boundaries: list[dict[str, object]] = []
    for realization in state.realizations:
        request_id = str(realization["request_id"])
        request = state.requests_by_id[request_id]
        source_id = str(request["source_id"])
        source = supplemental_sources[source_id]
        assumption = supplemental_assumption_rows.get(source_id)
        published_source = _load_supplemental_published_json(
            state,
            _supplemental_fragment_path(source_id),
            formatter,
        )
        if not _same_json_value(source, published_source):
            raise DataError(f"supplemental source scan differs: {source_id}")
        status = source.get("status")
        if status == "unavailable":
            _validate_supplemental_unavailable_source(
                state,
                request,
                realization,
                source,
                assumption,
                published=True,
            )
        elif status == "current":
            _validate_supplemental_assumption(
                state,
                request,
                realization,
                source,
                assumption,
                published=True,
            )
            if request["request_kind"] == "manufacturer-evidence":
                payloads: dict[str, bytes] = {}
                for artifact in realization["artifacts"]:
                    assert isinstance(artifact, dict)
                    relative = str(artifact["path"])
                    payload = _read_regular(state.root, relative)
                    assert payload is not None
                    payloads[relative] = payload
                _validate_supplemental_manufacturer_source(
                    state,
                    request,
                    realization,
                    source,
                    payloads,
                    published=True,
                )
            else:
                _validate_supplemental_uk_source(
                    state,
                    request,
                    realization,
                    source,
                    published=True,
                )
        else:
            raise DataError(
                f"supplemental source {source_id} must be current or unavailable"
            )
        if assumption is not None:
            published_assumption = _load_supplemental_published_json(
                state,
                _supplemental_assumption_fragment_path(source_id),
                formatter,
            )
            if not _same_json_value(assumption, published_assumption):
                raise DataError(f"supplemental assumption scan differs: {source_id}")
        boundary = realization.get("clone_boundary")
        if request["request_kind"] == "manufacturer-evidence":
            if not isinstance(boundary, dict):
                raise DataError(f"{request_id}: manufacturer clone boundary is invalid")
            selected = boundary.get(str(status))
            if not isinstance(selected, dict) or selected.get("source_id") != source_id:
                raise DataError(f"{request_id}: selected clone boundary differs")
            clone_boundaries.append(copy.deepcopy(selected))
        elif boundary is not None:
            raise DataError(f"{request_id}: UK clone boundary must be null")

    all_sources = sorted(
        [*seed_sources, *supplemental_sources.values()],
        key=lambda row: str(row["id"]),
    )
    all_assumptions = sorted(
        [*seed_assumptions, *supplemental_assumptions],
        key=lambda row: str(row["id"]),
    )
    source_record_ids = [str(row["id"]) for row in all_sources]
    if len(source_record_ids) != len(set(source_record_ids)):
        raise DataError("combined source record IDs are not unique")
    assumption_record_ids = [str(row["id"]) for row in all_assumptions]
    if len(assumption_record_ids) != len(set(assumption_record_ids)):
        raise DataError("combined assumption record IDs are not unique")
    _schema(all_sources, state.source_schema, state.source_schema, "assembled sources")
    _schema(
        all_assumptions,
        state.assumption_schema,
        state.assumption_schema,
        "assembled assumptions",
    )
    expected_source_rows = sorted(
        [
            *({"id": str(seed["id"])} for seed in seed_state.seeds),
            *({"id": source_id} for source_id in expected_source_ids),
        ],
        key=lambda row: row["id"],
    )
    try:
        validate_source_collection(all_sources, expected_source_rows)
    except SourceCollectionError as error:
        raise DataError(error.errors) from error

    clone_boundaries.sort(key=lambda row: str(row["source_id"]))
    try:
        validate_clone_boundary_rows(clone_boundaries)
    except (AssertionError, SchemaError, ValidationError) as error:
        raise DataError(f"supplemental clone boundary is invalid: {error}") from error

    request_rows = _scan_json_directory(
        seed_state,
        "evidence/manifest/requests",
        required=True,
    )
    expected_request_ids = set(state.requests_by_id)
    _require_exact_ids(request_rows, expected_request_ids, "request fragment")
    input_requests = _require_record_ids(
        request_rows,
        "request_id",
        "request fragment",
    )
    projected_requests: list[dict[str, object]] = []
    for request_id in sorted(expected_request_ids):
        request = input_requests[request_id]
        control = state.requests_by_id[request_id]
        if not _same_json_value(request, control) or not _object_key_order_matches(
            request, control
        ):
            raise DataError(f"request fragment {request_id} differs from its aggregate")
        _schema(request, state.request_schema, state.request_schema, request_id)
        projected = copy.deepcopy(request)
        if request_id not in PRESERVED_REQUEST_IDS:
            source_id = str(request["source_id"])
            source = supplemental_sources[source_id]
            if source["status"] == "current":
                projected["status"] = "satisfied"
                projected["evidence_ids"] = [source_id]
                projected["notes"] = ""
            else:
                artifact = source["artifacts"][0]
                assert isinstance(artifact, dict)
                projected["status"] = "unavailable"
                projected["evidence_ids"] = []
                projected["notes"] = artifact["note"]
            _require_unchanged_except(
                request,
                projected,
                {"status", "evidence_ids", "notes"},
                f"request {request_id}",
            )
        _schema(
            projected,
            state.request_schema,
            state.request_schema,
            f"projected request {request_id}",
        )
        projected_requests.append(projected)

    component_schema = _load_json(state.root, "evidence/design/component-schema.json")
    selection_policy = _load_json(
        state.root,
        "evidence/design/selection-policy.json",
    )
    try:
        check_schema(component_schema)
    except SchemaError as error:
        raise ControlError(f"component schema is invalid: {error}") from error
    if not isinstance(component_schema, dict) or not isinstance(selection_policy, dict):
        raise ControlError("component schema and selection policy must be objects")
    families = selection_policy.get("families")
    if not isinstance(families, list) or not all(
        isinstance(family, dict) and isinstance(family.get("family"), str)
        for family in families
    ):
        raise ControlError("selection policy families are invalid")
    expected_component_ids = {str(family["family"]) for family in families}
    if len(expected_component_ids) != len(families):
        raise ControlError("selection policy family IDs are not unique")
    component_rows = _scan_named_json_directory(
        state.root,
        "evidence/design/components",
        r"([a-z0-9]+(?:-[a-z0-9]+)*)\.json",
    )
    _require_exact_ids(component_rows, expected_component_ids, "component fragment")
    components_by_id = _require_record_ids(
        component_rows,
        "block",
        "component fragment",
    )
    for component_id, component in components_by_id.items():
        _schema(
            component,
            component_schema,
            component_schema,
            f"component {component_id}",
        )

    requests_by_family: dict[str, dict[str, dict[str, object]]] = {}
    for request in projected_requests:
        family = str(request["family"])
        kind = str(request["request_kind"])
        family_requests = requests_by_family.setdefault(family, {})
        if kind in family_requests:
            raise DataError(f"component family {family} has duplicate {kind} requests")
        family_requests[kind] = request
    if len(requests_by_family) != 24:
        raise DataError("supplemental requests do not cover exactly 24 component families")

    affected_component_ids: list[str] = []
    projected_components = {
        component_id: copy.deepcopy(component)
        for component_id, component in components_by_id.items()
    }
    for family, family_requests in requests_by_family.items():
        component = projected_components.get(family)
        manufacturer_request = family_requests.get("manufacturer-evidence")
        uk_request = family_requests.get("uk-availability")
        if component is None or manufacturer_request is None or uk_request is None:
            raise DataError(f"component family {family} request join is incomplete")
        exact_fields = (
            ("manufacturer_request_id", manufacturer_request["request_id"]),
            ("availability_request_id", uk_request["request_id"]),
            ("manufacturer", manufacturer_request["selected_manufacturer"]),
            ("mpn", manufacturer_request["selected_mpn"]),
            ("authority_url", manufacturer_request["authority_url"]),
            ("requirements", manufacturer_request["requirements"]),
        )
        for key, expected in exact_fields:
            if not _same_json_value(component.get(key), expected):
                raise DataError(f"component family {family} field {key!r} join differs")
        for key in ("selected_manufacturer", "selected_mpn"):
            if uk_request[key] != manufacturer_request[key]:
                raise DataError(f"component family {family} UK {key} join differs")
        request_id = str(manufacturer_request["request_id"])
        if request_id in PRESERVED_REQUEST_IDS:
            if component.get("status") != "selected":
                raise DataError(f"preserved component {family} status differs")
            continue
        source = supplemental_sources[str(manufacturer_request["source_id"])]
        before = copy.deepcopy(component)
        component["status"] = (
            "selected"
            if source["status"] == "current"
            else "candidate-evidence-unavailable"
        )
        _require_unchanged_except(
            before,
            component,
            {"status"},
            f"component {family}",
        )
        _schema(
            component,
            component_schema,
            component_schema,
            f"projected component {family}",
        )
        affected_component_ids.append(family)
    if len(affected_component_ids) != 22:
        raise DataError("supplemental lifecycle does not affect exactly 22 components")

    return SupplementalAssembly(
        tuple(all_sources),
        tuple(all_assumptions),
        tuple(clone_boundaries),
        tuple(projected_requests),
        tuple(sorted(set(expected_request_ids) - set(PRESERVED_REQUEST_IDS))),
        tuple(
            projected_components[component_id]
            for component_id in sorted(projected_components)
        ),
        tuple(sorted(affected_component_ids)),
        selection_policy,
    )


def _supplemental_assemble(
    seed_state: State,
    state: SupplementalState,
    mode: str,
    replace: Callable[[str, str], object],
    formatter: Formatter,
) -> int:
    assembly = _supplemental_assembly_data(seed_state, state, formatter)
    requests_by_id = {
        str(request["request_id"]): request for request in assembly.requests
    }
    components_by_id = {
        str(component["block"]): component for component in assembly.components
    }
    rendered: dict[str, bytes] = {
        "evidence/manifest/assumptions.json": _render_json(
            state.root,
            list(assembly.assumptions),
            formatter,
        ),
        "evidence/manifest/sources.json": _render_json(
            state.root,
            list(assembly.sources),
            formatter,
        ),
        "evidence/manifest/supplemental-clone-boundary.json": _render_json(
            state.root,
            list(assembly.clone_boundaries),
            formatter,
            preserve_object_order=True,
        ),
    }
    for request_id in assembly.affected_request_ids:
        rendered[f"evidence/manifest/requests/{request_id}.json"] = _render_json(
            state.root,
            requests_by_id[request_id],
            formatter,
            preserve_object_order=True,
        )
    for component_id in assembly.affected_component_ids:
        rendered[f"evidence/design/components/{component_id}.json"] = _render_json(
            state.root,
            components_by_id[component_id],
            formatter,
            preserve_object_order=True,
        )
    request_aggregate = render_p1.render_supplemental_requests(assembly.requests)
    observed_request_aggregate = _strict_rendered_json(request_aggregate)
    expected_request_aggregate = {
        "schema_version": "1.0.0",
        "requests": list(assembly.requests),
    }
    if not _same_json_value(expected_request_aggregate, observed_request_aggregate):
        raise DataError("supplemental request aggregate renderer changed its input")
    rendered["evidence/manifest/supplemental-requests.json"] = request_aggregate
    rendered["evidence/design/provisional-components.csv"] = render_p1.render_components(
        assembly.selection_policy,
        assembly.components,
    )
    if len(rendered) != 73:
        raise DataError(f"supplemental assembler output count differs: {len(rendered)}")
    rendered = dict(sorted(rendered.items()))
    if mode == "check":
        errors: list[str] = []
        for relative, expected in rendered.items():
            actual = _read_regular(state.root, relative, required=False)
            if actual != expected:
                errors.append(f"aggregate drift: {relative}")
        if errors:
            raise DataError(errors)
        return 0
    outputs = {
        _root_path(state.root, relative): payload
        for relative, payload in rendered.items()
    }
    _replace_aggregates(state.root, outputs, replace)
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = _Parser(description=__doc__)
    parser.add_argument("--root", help="Aius project root")
    subparsers = parser.add_subparsers(dest="command", required=True)

    acquire = subparsers.add_parser("acquire")
    acquire.add_argument("source_id")
    acquire.add_argument("--retrieved-at", required=True)

    record = subparsers.add_parser("record")
    record.add_argument("source_id")
    record.add_argument("--values", required=True)
    record.add_argument("--observed-at", required=True)

    unavailable = subparsers.add_parser("unavailable")
    unavailable.add_argument("source_id")
    unavailable.add_argument("--note", required=True)
    unavailable.add_argument("--retrieved-at", required=True)

    check = subparsers.add_parser("check-fragment")
    check.add_argument("source_ids", nargs="+")

    assemble = subparsers.add_parser("assemble")
    modes = assemble.add_mutually_exclusive_group(required=True)
    modes.add_argument("--write", action="store_true")
    modes.add_argument("--check", action="store_true")

    supplemental = subparsers.add_parser("supplemental")
    supplemental_commands = supplemental.add_subparsers(
        dest="supplemental_command",
        required=True,
    )

    supplemental_acquire = supplemental_commands.add_parser("acquire")
    supplemental_acquire.add_argument("request_id")
    supplemental_acquire.add_argument("--retrieved-at", required=True)

    supplemental_record = supplemental_commands.add_parser("record")
    supplemental_record.add_argument("request_id")
    supplemental_record.add_argument("--values", required=True)
    supplemental_record.add_argument("--product-final-url", required=True)
    supplemental_record.add_argument("--authorization-final-url", required=True)
    supplemental_record.add_argument("--observed-at", required=True)

    supplemental_unavailable = supplemental_commands.add_parser("unavailable")
    supplemental_unavailable.add_argument("request_id")
    supplemental_unavailable.add_argument("--note", required=True)
    supplemental_unavailable.add_argument("--retrieved-at", required=True)

    supplemental_check = supplemental_commands.add_parser("check-fragment")
    supplemental_check.add_argument("request_id")

    supplemental_assemble = supplemental_commands.add_parser("assemble")
    supplemental_modes = supplemental_assemble.add_mutually_exclusive_group(
        required=True
    )
    supplemental_modes.add_argument("--write", action="store_true")
    supplemental_modes.add_argument("--check", action="store_true")
    return parser


def run(
    argv: list[str] | None = None,
    *,
    opener: object | None = None,
    replace: Callable[[str, str], object] = os.replace,
    formatter: Formatter | None = None,
) -> int:
    arguments = _parser().parse_args(argv)
    root = (
        Path(arguments.root)
        if arguments.root is not None
        else Path(__file__).resolve().parents[1]
    )
    try:
        selected_formatter = _production_formatter if formatter is None else formatter
        if arguments.command == "supplemental":
            supplemental_state = _load_supplemental_state(root)
            if arguments.supplemental_command == "acquire":
                return _supplemental_acquire(
                    supplemental_state,
                    arguments.request_id,
                    arguments.retrieved_at,
                    opener if opener is not None else _production_opener(),
                    replace,
                    selected_formatter,
                )
            if arguments.supplemental_command == "record":
                return _supplemental_record(
                    supplemental_state,
                    arguments.request_id,
                    arguments.values,
                    arguments.product_final_url,
                    arguments.authorization_final_url,
                    arguments.observed_at,
                    replace,
                    selected_formatter,
                )
            if arguments.supplemental_command == "unavailable":
                result = _supplemental_unavailable(
                    supplemental_state,
                    arguments.request_id,
                    arguments.note,
                    arguments.retrieved_at,
                    replace,
                    selected_formatter,
                )
                if result == 3:
                    print(
                        "ERROR: supplemental manufacturer request "
                        f"{arguments.request_id} was recorded unavailable",
                        file=sys.stderr,
                    )
                return result
            if arguments.supplemental_command == "check-fragment":
                return _supplemental_check_fragment(
                    supplemental_state,
                    arguments.request_id,
                    selected_formatter,
                )
            if arguments.supplemental_command == "assemble":
                return _supplemental_assemble(
                    _load_state(root),
                    supplemental_state,
                    "write" if arguments.write else "check",
                    replace,
                    selected_formatter,
                )
            raise ControlError(
                f"unknown supplemental command: {arguments.supplemental_command}"
            )
        state = _load_state(root)
        if arguments.command == "acquire":
            return _acquire(
                state,
                arguments.source_id,
                arguments.retrieved_at,
                opener if opener is not None else _production_opener(),
                replace,
                selected_formatter,
            )
        if arguments.command == "record":
            return _record(
                state,
                arguments.source_id,
                arguments.values,
                arguments.observed_at,
                replace,
                selected_formatter,
            )
        if arguments.command == "unavailable":
            result = _unavailable(
                state,
                arguments.source_id,
                arguments.note,
                arguments.retrieved_at,
                replace,
                selected_formatter,
            )
            print(
                f"ERROR: seed source {arguments.source_id} was recorded unavailable",
                file=sys.stderr,
            )
            return result
        if arguments.command == "check-fragment":
            return _check_fragments(state, arguments.source_ids)
        if arguments.command == "assemble":
            return _assemble(
                state,
                "write" if arguments.write else "check",
                replace,
                selected_formatter,
            )
        raise ControlError(f"unknown command: {arguments.command}")
    except DataError as error:
        for message in sorted(set(error.messages)):
            print(f"ERROR: {message}", file=sys.stderr)
        return 1
    except ControlError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2
    except Exception as error:
        print(f"ERROR: unexpected internal exception: {error}", file=sys.stderr)
        return 2


def main(argv: list[str] | None = None) -> int:
    return run(argv)


if __name__ == "__main__":
    raise SystemExit(main())
