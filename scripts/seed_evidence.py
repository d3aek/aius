#!/usr/bin/env python3
"""Acquire, record, validate, and assemble Aius seed evidence."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import http.client
import json
import os
import re
import secrets
import stat
import sys
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Callable, Iterable, Mapping

from jsonschema_subset import SchemaError, ValidationError, check_schema, validate
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


def _canonical_json(value: object) -> bytes:
    return (
        json.dumps(
            value,
            ensure_ascii=False,
            allow_nan=False,
            sort_keys=True,
            indent=2,
        )
        + "\n"
    ).encode("utf-8")


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
    prior: dict[Path, bytes | None] = {}
    staged: dict[Path, Path] = {}
    published: list[Path] = []
    try:
        for target, payload in outputs.items():
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
            replace(os.fspath(temporary), os.fspath(target))
            published.append(target)
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


def _prepare_output_directories(state: State, realization: Mapping[str, object]) -> None:
    _ensure_directory(state.root, "evidence/manifest/fragments")
    for artifact in realization["artifacts"]:
        assert isinstance(artifact, dict)
        path = PurePosixPath(str(artifact["path"]))
        _ensure_directory(state.root, path.parent.as_posix())


def _acquire(
    state: State,
    source_id: str,
    retrieved_at: str,
    opener: object,
    replace: Callable[[str, str], object],
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
    outputs[_root_path(state.root, fragment_relative)] = _canonical_json(source)
    _publish_new(state.root, outputs, replace)
    return 0


def _record(
    state: State,
    source_id: str,
    values_text: str,
    observed_at: str,
    replace: Callable[[str, str], object],
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
    _publish_new(state.root, {target: _canonical_json(source)}, replace)
    return 0


def _unavailable(
    state: State,
    source_id: str,
    note: str,
    retrieved_at: str,
    replace: Callable[[str, str], object],
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
    outputs = {fragment_target: _canonical_json(source)}
    assumption: dict[str, object] | None = None
    if realization["failure_policy"] == "assumption":
        _ensure_directory(state.root, "evidence/manifest/assumption-fragments")
        assumption_relative = _assumption_fragment_path(source_id)
        assumption_target = _reject_target(state.root, assumption_relative)
        assumption = _expected_assumption(seed)
        outputs[assumption_target] = _canonical_json(assumption)
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
) -> int:
    sources, assumptions = _assemble_data(state)
    rendered = {
        "evidence/manifest/sources.json": _canonical_json(sources),
        "evidence/manifest/assumptions.json": _canonical_json(assumptions),
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
    return parser


def run(
    argv: list[str] | None = None,
    *,
    opener: object | None = None,
    replace: Callable[[str, str], object] = os.replace,
) -> int:
    arguments = _parser().parse_args(argv)
    root = (
        Path(arguments.root)
        if arguments.root is not None
        else Path(__file__).resolve().parents[1]
    )
    try:
        state = _load_state(root)
        if arguments.command == "acquire":
            return _acquire(
                state,
                arguments.source_id,
                arguments.retrieved_at,
                opener if opener is not None else _production_opener(),
                replace,
            )
        if arguments.command == "record":
            return _record(
                state,
                arguments.source_id,
                arguments.values,
                arguments.observed_at,
                replace,
            )
        if arguments.command == "unavailable":
            result = _unavailable(
                state,
                arguments.source_id,
                arguments.note,
                arguments.retrieved_at,
                replace,
            )
            print(
                f"ERROR: seed source {arguments.source_id} was recorded unavailable",
                file=sys.stderr,
            )
            return result
        if arguments.command == "check-fragment":
            return _check_fragments(state, arguments.source_ids)
        if arguments.command == "assemble":
            return _assemble(state, "write" if arguments.write else "check", replace)
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
