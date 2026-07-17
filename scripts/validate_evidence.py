#!/usr/bin/env python3
"""Validate the Aius evidence checkout and its immutable source baseline."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
import re
import stat
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Iterable, Mapping


BASELINE_GIT_PATH = "evidence/manifest/sources.json"
IMMUTABLE_FIELDS = ("id", "class", "authority", "landing_url")
LFS_POINTER_PREFIX = b"version https://git-lfs.github.com/spec/v1\n"


@dataclass(frozen=True, order=True)
class Diagnostic:
    code: str
    subject: str
    message: str


class ControlError(RuntimeError):
    """Invalid invocation, control-file, or Git environment."""


class Diagnostics:
    def __init__(self) -> None:
        self._items: set[Diagnostic] = set()

    def add(self, code: str, subject: str, message: str) -> None:
        self._items.add(Diagnostic(code, subject, message))

    def emit(self) -> bool:
        for item in sorted(self._items):
            print(
                f"ERROR {item.code} {item.subject}: {item.message}",
                file=sys.stderr,
            )
        return bool(self._items)


@dataclass(frozen=True)
class FragmentCollection:
    subject: str
    present: bool
    scannable: bool
    records: dict[str, object]


@dataclass
class GitContext:
    root: Path

    @classmethod
    def discover(cls, aius_root: Path) -> "GitContext":
        completed = _run_git(aius_root, "rev-parse", "--show-toplevel")
        if completed.returncode != 0:
            raise ControlError(
                "cannot find the enclosing Git worktree: "
                + _command_error(completed)
            )
        try:
            worktree = Path(completed.stdout.decode("utf-8").strip()).resolve()
        except UnicodeDecodeError as error:
            raise ControlError("Git returned a non-UTF-8 worktree path") from error
        if not worktree.is_dir():
            raise ControlError(f"Git worktree is not a directory: {worktree}")
        return cls(worktree)

    def baseline(self) -> object:
        head = _run_git(self.root, "rev-parse", "--verify", "HEAD^{commit}")
        if head.returncode != 0:
            raise ControlError("Git HEAD is unusable: " + _command_error(head))

        tree_entry = _run_git(
            self.root, "ls-tree", "-z", "HEAD", "--", BASELINE_GIT_PATH
        )
        if tree_entry.returncode != 0:
            raise ControlError(
                "cannot inspect the Git baseline path: " + _command_error(tree_entry)
            )
        if not tree_entry.stdout:
            return []

        completed = _run_git(self.root, "show", f"HEAD:{BASELINE_GIT_PATH}")
        if completed.returncode != 0:
            raise ControlError("cannot read the Git baseline: " + _command_error(completed))
        return _decode_json_bytes(completed.stdout, "Git baseline")

    def filter_attribute(self, artifact: Path) -> str:
        try:
            relative = artifact.relative_to(self.root).as_posix()
        except ValueError as error:
            raise ControlError(
                f"artifact path is outside the enclosing Git worktree: {artifact}"
            ) from error
        completed = _run_git(
            self.root,
            "check-attr",
            "-z",
            "filter",
            "--",
            relative,
        )
        if completed.returncode != 0:
            raise ControlError("git check-attr failed: " + _command_error(completed))
        fields = completed.stdout.split(b"\0")
        if len(fields) != 4 or fields[-1] != b"":
            raise ControlError("git check-attr returned an unparseable response")
        try:
            observed_path, attribute, value = (
                field.decode("utf-8") for field in fields[:3]
            )
        except UnicodeDecodeError as error:
            raise ControlError("git check-attr returned non-UTF-8 data") from error
        if observed_path != relative or attribute != "filter":
            raise ControlError("git check-attr returned an unexpected path or attribute")
        return value


@dataclass(frozen=True)
class MediaRule:
    media_type: str
    storage: str
    kind: str


MEDIA_RULES = {
    ".pdf": MediaRule("application/pdf", "lfs", "pdf"),
    ".zip": MediaRule("application/zip", "lfs", "zip"),
    ".png": MediaRule("image/png", "lfs", "png"),
    ".html": MediaRule("text/html", "git", "html"),
    ".htm": MediaRule("text/html", "git", "html"),
    ".mhtml": MediaRule("multipart/related", "lfs", "mhtml"),
    ".step": MediaRule("model/step", "lfs", "step"),
    ".stp": MediaRule("model/step", "lfs", "step"),
    ".kicad_sch": MediaRule(
        "application/x-kicad-schematic", "git", "kicad_sch"
    ),
    ".kicad_pcb": MediaRule("application/x-kicad-pcb", "git", "kicad_pcb"),
    ".kicad_sym": MediaRule(
        "application/x-kicad-symbol-library", "git", "kicad_sym"
    ),
    ".kicad_mod": MediaRule(
        "application/x-kicad-footprint", "git", "kicad_mod"
    ),
    ".kicad_pro": MediaRule("application/json", "git", "kicad_pro"),
    ".json": MediaRule("application/json", "git", "json"),
    ".csv": MediaRule("text/csv", "git", "csv"),
    ".c": MediaRule("text/x-c", "git", "text"),
    ".h": MediaRule("text/x-c", "git", "text"),
    ".cpp": MediaRule("text/x-c++", "git", "text"),
    ".cc": MediaRule("text/x-c++", "git", "text"),
    ".cxx": MediaRule("text/x-c++", "git", "text"),
    ".hpp": MediaRule("text/x-c++", "git", "text"),
    ".hh": MediaRule("text/x-c++", "git", "text"),
    ".hxx": MediaRule("text/x-c++", "git", "text"),
    ".rst": MediaRule("text/x-rst", "git", "text"),
    ".txt": MediaRule("text/plain", "git", "text"),
    ".md": MediaRule("text/markdown", "git", "text"),
}


def _run_git(directory: Path, *arguments: str) -> subprocess.CompletedProcess[bytes]:
    try:
        return subprocess.run(
            ["git", "-C", os.fspath(directory), *arguments],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as error:
        raise ControlError(f"cannot execute Git: {error}") from error


def _command_error(completed: subprocess.CompletedProcess[bytes]) -> str:
    try:
        detail = completed.stderr.decode("utf-8").strip()
    except UnicodeDecodeError:
        detail = "non-UTF-8 error output"
    return detail or f"exit status {completed.returncode}"


def _decode_json_bytes(payload: bytes, label: str) -> object:
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ControlError(f"{label} is not UTF-8: {error}") from error
    try:
        return json.loads(text)
    except json.JSONDecodeError as error:
        raise ControlError(f"{label} is not valid JSON: {error}") from error


def _load_control(path: Path, label: str | None = None) -> object:
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise ControlError(f"cannot read {label or path}: {error}") from error
    return _decode_json_bytes(payload, label or os.fspath(path))


def _control_exists(path: Path) -> bool:
    try:
        path.lstat()
    except FileNotFoundError:
        return False
    except OSError as error:
        raise ControlError(f"cannot inspect {path}: {error}") from error
    return True


def _resolve_aius_root(root: Path) -> Path:
    try:
        status = root.lstat()
    except OSError as error:
        raise ControlError(f"Aius root is not a directory: {root}: {error}") from error
    if stat.S_ISLNK(status.st_mode):
        raise ControlError(f"Aius root may not be a symlink: {root}")
    resolved = root.resolve()
    if not resolved.is_dir():
        raise ControlError(f"Aius root is not a directory: {resolved}")
    return resolved


def _relative_subject(root: Path, path: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return os.fspath(path)


def _schema_validate(
    instance: object,
    schema: object,
    subject: str,
    diagnostics: Diagnostics,
    *,
    root_schema: object | None = None,
) -> bool:
    from jsonschema_subset import SchemaError, ValidationError, validate

    try:
        validate(instance, schema, root_schema=root_schema)
    except (SchemaError, ValidationError) as error:
        diagnostics.add("MANIFEST_SCHEMA", subject, str(error))
        return False
    return True


def _check_schema(schema: object, subject: str, diagnostics: Diagnostics) -> bool:
    from jsonschema_subset import SchemaError, check_schema

    try:
        check_schema(schema)
    except SchemaError as error:
        diagnostics.add("MANIFEST_SCHEMA", subject, str(error))
        return False
    return True


def _records(collection: object) -> list[dict[str, object]]:
    if not isinstance(collection, list):
        return []
    return [item for item in collection if isinstance(item, dict)]


def _by_id(collection: object) -> dict[str, dict[str, object]]:
    result: dict[str, dict[str, object]] = {}
    for record in _records(collection):
        source_id = record.get("id")
        if isinstance(source_id, str) and source_id not in result:
            result[source_id] = record
    return result


def _scan_fragment_collection(
    root: Path,
    path: Path,
    *,
    required: bool,
    diagnostics: Diagnostics,
) -> FragmentCollection:
    subject = _relative_subject(root, path)
    try:
        status = path.lstat()
    except FileNotFoundError:
        if required:
            diagnostics.add(
                "FRAGMENT_COLLECTION",
                subject,
                "required fragment directory is missing",
            )
        return FragmentCollection(subject, False, False, {})
    except OSError as error:
        raise ControlError(f"cannot inspect {subject}: {error}") from error

    if stat.S_ISLNK(status.st_mode) or not stat.S_ISDIR(status.st_mode):
        diagnostics.add(
            "FRAGMENT_COLLECTION",
            subject,
            "fragment path is not a regular non-symlink directory",
        )
        return FragmentCollection(subject, True, False, {})

    try:
        entries = list(os.scandir(path))
    except OSError as error:
        raise ControlError(f"cannot scan {subject}: {error}") from error

    records: dict[str, object] = {}
    for entry in entries:
        entry_path = Path(entry.path)
        entry_subject = _relative_subject(root, entry_path)
        try:
            entry_status = entry_path.lstat()
        except OSError as error:
            raise ControlError(f"cannot inspect {entry_subject}: {error}") from error
        match = re.fullmatch(r"([A-Z0-9]+(?:-[A-Z0-9]+)*)\.json", entry.name)
        if (
            stat.S_ISLNK(entry_status.st_mode)
            or not stat.S_ISREG(entry_status.st_mode)
            or match is None
        ):
            diagnostics.add(
                "FRAGMENT_COLLECTION",
                entry_subject,
                "unexpected or unsafe fragment entry",
            )
            continue
        records[match.group(1)] = _load_control(entry_path)
    return FragmentCollection(subject, True, True, records)


def _fragment_ids(
    collection: FragmentCollection,
    expected: set[str],
    diagnostics: Diagnostics,
) -> None:
    if not collection.scannable:
        return
    actual = set(collection.records)
    if actual != expected:
        diagnostics.add(
            "FRAGMENT_COLLECTION",
            collection.subject,
            "fragment IDs differ; "
            f"missing={sorted(expected - actual)!r}; extra={sorted(actual - expected)!r}",
        )


def _source_fragment_records(
    root: Path,
    collection: FragmentCollection,
    diagnostics: Diagnostics,
) -> dict[str, dict[str, object]]:
    result: dict[str, dict[str, object]] = {}
    for filename_id, value in collection.records.items():
        record_id = value.get("id") if isinstance(value, dict) else None
        if record_id != filename_id:
            diagnostics.add(
                "FRAGMENT_COLLECTION",
                f"{collection.subject}/{filename_id}.json",
                f"filename source ID {filename_id!r} does not match record ID {record_id!r}",
            )
        if isinstance(value, dict) and isinstance(record_id, str):
            result[filename_id] = value
    return result


def _assumption_fragment_records(
    collection: FragmentCollection,
    diagnostics: Diagnostics,
) -> dict[str, dict[str, object]]:
    result: dict[str, dict[str, object]] = {}
    for filename_id, value in collection.records.items():
        source_ids = value.get("source_ids") if isinstance(value, dict) else None
        canonical_source = (
            source_ids[0]
            if isinstance(source_ids, list)
            and len(source_ids) == 1
            and isinstance(source_ids[0], str)
            else None
        )
        if canonical_source != filename_id:
            diagnostics.add(
                "FRAGMENT_COLLECTION",
                f"{collection.subject}/{filename_id}.json",
                f"filename source ID {filename_id!r} does not match canonical assumption source {canonical_source!r}",
            )
        record_id = value.get("id") if isinstance(value, dict) else None
        if isinstance(value, dict) and isinstance(record_id, str):
            result[filename_id] = value
    return result


def _check_duplicate_fragment_record_ids(
    collections: Iterable[FragmentCollection],
    diagnostics: Diagnostics,
) -> None:
    counts: dict[str, int] = {}
    for collection in collections:
        for value in collection.records.values():
            record_id = value.get("id") if isinstance(value, dict) else None
            if isinstance(record_id, str):
                counts[record_id] = counts.get(record_id, 0) + 1
    for record_id, count in sorted(counts.items()):
        if count > 1:
            diagnostics.add(
                "FRAGMENT_COLLECTION",
                record_id,
                "record ID appears in more than one fragment",
            )


def _status_assumption_sources(
    sources: Mapping[str, dict[str, object]],
    realizations: Mapping[str, dict[str, object]],
) -> set[str]:
    return {
        source_id
        for source_id, source in sources.items()
        if source.get("status") == "unavailable"
        and realizations.get(source_id, {}).get("failure_policy") == "assumption"
    }


def _sorted_records(records: Iterable[dict[str, object]]) -> list[dict[str, object]]:
    return sorted(records, key=lambda row: str(row["id"]))


def _check_fragment_collisions(
    seed_sources: Mapping[str, dict[str, object]],
    supplemental_sources: Mapping[str, dict[str, object]],
    seed_assumptions: Mapping[str, dict[str, object]],
    supplemental_assumptions: Mapping[str, dict[str, object]],
    seed_source_files: set[str],
    supplemental_source_files: set[str],
    seed_assumption_files: set[str],
    supplemental_assumption_files: set[str],
    diagnostics: Diagnostics,
) -> None:
    collisions = (
        ({str(row["id"]) for row in seed_sources.values()}
         & {str(row["id"]) for row in supplemental_sources.values()})
        | ({str(row["id"]) for row in seed_assumptions.values()}
           & {str(row["id"]) for row in supplemental_assumptions.values()})
        | (seed_source_files & supplemental_source_files)
        | (seed_assumption_files & supplemental_assumption_files)
    )
    for collision in sorted(collisions):
        diagnostics.add(
            "FRAGMENT_COLLECTION",
            collision,
            "ID appears in both seed and supplemental fragments",
        )


def _canonical_unavailable_artifact(
    source: Mapping[str, object], expected_final_url: object
) -> bool:
    artifacts = source.get("artifacts")
    if not isinstance(artifacts, list) or len(artifacts) != 1:
        return False
    artifact = artifacts[0]
    if not isinstance(artifact, dict):
        return False
    note = artifact.get("note")
    expected = {
        "name": "unavailable",
        "final_url": expected_final_url,
        "storage": "external-reference",
        "path": None,
        "sha256": None,
        "bytes": None,
        "media_type": None,
        "retrieval_status": "unavailable",
        "note": note,
    }
    return isinstance(note, str) and bool(note) and artifact == expected


def _ordered_unique(values: Iterable[object]) -> list[object]:
    result: list[object] = []
    for value in values:
        if value not in result:
            result.append(value)
    return result


def _validate_realization_bindings(
    sources: Mapping[str, dict[str, object]],
    realizations: Mapping[str, dict[str, object]],
    requests: Mapping[str, dict[str, object]],
    diagnostics: Diagnostics,
) -> None:
    field_pairs = (
        ("name", "name"),
        ("final_url", "expected_final_url"),
        ("path", "path"),
        ("storage", "storage"),
        ("media_type", "media_type"),
    )
    for source_id, realization in sorted(realizations.items()):
        source = sources.get(source_id)
        if source is None:
            continue
        status = source.get("status")
        manufacturer = realization.get("clone_boundary") is not None
        artifacts = source.get("artifacts")
        request = requests.get(str(realization.get("request_id")), {})
        if status == "unavailable":
            if not _canonical_unavailable_artifact(
                source, request.get("authority_url")
            ):
                diagnostics.add(
                    "REALIZATION_BINDING",
                    source_id,
                    "unavailable source artifact differs from canonical metadata",
                )
            continue
        if status != "current":
            continue
        if not manufacturer:
            if artifacts != []:
                diagnostics.add(
                    "REALIZATION_BINDING",
                    source_id,
                    "current UK source must not contain artifacts",
                )
            continue

        expected_artifacts = realization.get("artifacts")
        actual_artifacts = artifacts if isinstance(artifacts, list) else []
        expected_list = expected_artifacts if isinstance(expected_artifacts, list) else []
        if len(actual_artifacts) != len(expected_list):
            diagnostics.add(
                "REALIZATION_BINDING",
                source_id,
                "current artifact count differs from realization",
            )
        for index, (actual, expected) in enumerate(zip(actual_artifacts, expected_list)):
            if not isinstance(actual, dict) or not isinstance(expected, dict):
                continue
            for source_field, realization_field in field_pairs:
                if actual.get(source_field) != expected.get(realization_field):
                    diagnostics.add(
                        "REALIZATION_BINDING",
                        source_id,
                        f"artifact[{index}] field {source_field!r} differs from realization",
                    )
        kinds = _ordered_unique(
            artifact.get("artifact_kind")
            for artifact in expected_list
            if isinstance(artifact, dict)
        )
        if kinds != request.get("artifact_kinds"):
            diagnostics.add(
                "REALIZATION_BINDING",
                source_id,
                "ordered artifact kinds differ from request",
            )


def _validate_literal_supplemental_boundary(
    supplemental: object | None,
    supplemental_present: bool,
    subject: str,
    sources: Mapping[str, dict[str, object]],
    realizations: Mapping[str, dict[str, object]],
    diagnostics: Diagnostics,
) -> None:
    from jsonschema_subset import ValidationError
    from test_evidence_policy import validate_clone_boundary_rows

    rows = supplemental if isinstance(supplemental, list) else []
    if supplemental_present:
        try:
            validate_clone_boundary_rows(supplemental)
        except (ValidationError, AssertionError) as error:
            diagnostics.add("CLONE_BOUNDARY", subject, str(error))

    manufacturer_ids = {
        source_id
        for source_id, realization in realizations.items()
        if realization.get("clone_boundary") is not None
    }
    uk_ids = set(realizations) - manufacturer_ids
    actual_ids = [
        row.get("source_id")
        for row in rows
        if isinstance(row, dict) and isinstance(row.get("source_id"), str)
    ]
    actual_set = set(actual_ids)
    if actual_set != manufacturer_ids:
        diagnostics.add(
            "CLONE_BOUNDARY",
            subject,
            "supplemental boundary source IDs differ; "
            f"missing={sorted(manufacturer_ids - actual_set)!r}; "
            f"extra={sorted(actual_set - manufacturer_ids)!r}",
        )
    if actual_ids != sorted(actual_ids):
        diagnostics.add(
            "CLONE_BOUNDARY",
            subject,
            "supplemental boundary source IDs are not sorted",
        )
    by_id = {
        str(row["source_id"]): row
        for row in rows
        if isinstance(row, dict) and isinstance(row.get("source_id"), str)
    }
    for source_id in sorted(actual_set & uk_ids):
        diagnostics.add(
            "CLONE_BOUNDARY",
            source_id,
            "UK source must not appear in supplemental clone boundary",
        )
    for source_id in sorted(actual_set & manufacturer_ids):
        source = sources.get(source_id)
        realization = realizations[source_id]
        status = source.get("status") if source is not None else None
        clone_boundary = realization.get("clone_boundary")
        expected = (
            clone_boundary.get(status)
            if isinstance(clone_boundary, dict) and isinstance(status, str)
            else None
        )
        if by_id[source_id] != expected:
            diagnostics.add(
                "CLONE_BOUNDARY",
                source_id,
                f"supplemental boundary row differs from realization outcome {status!r}",
            )


def _immutable_tuple(record: dict[str, object]) -> tuple[object, ...]:
    return tuple(record.get(field) for field in IMMUTABLE_FIELDS)


def _validate_source_and_baseline_collections(
    sources: object,
    seeds: object,
    baseline: object,
    baseline_schema_valid: bool,
    diagnostics: Diagnostics,
) -> None:
    from test_source_schema import SourceCollectionError, validate_source_collection

    try:
        validate_source_collection(sources, seeds)
    except SourceCollectionError as error:
        for message in error.errors:
            diagnostics.add(
                "SOURCE_COLLECTION", "evidence/manifest/sources.json", message
            )

    if not baseline_schema_valid:
        return
    try:
        validate_source_collection(baseline, seeds)
    except SourceCollectionError as error:
        for message in error.errors:
            diagnostics.add("BASELINE", "baseline", message)


def _validate_immutable_identities(
    sources: object,
    seeds: object,
    baseline: object,
    baseline_schema_valid: bool,
    diagnostics: Diagnostics,
) -> None:
    current = _by_id(sources)
    for source_id, seed in sorted(_by_id(seeds).items()):
        source = current.get(source_id)
        if source is not None and _immutable_tuple(source) != _immutable_tuple(seed):
            diagnostics.add(
                "SEED_IDENTITY",
                source_id,
                "source immutable identity differs from seed-sources.json",
            )

        if seed.get("required") is True and seed.get("class") == "golden":
            if source is None:
                diagnostics.add(
                    "REQUIRED_SOURCE", source_id, "required golden source is missing"
                )
            elif source.get("status") != "current":
                diagnostics.add(
                    "REQUIRED_SOURCE",
                    source_id,
                    "required golden source is not current",
                )

    if not baseline_schema_valid:
        return
    for source_id, prior in sorted(_by_id(baseline).items()):
        source = current.get(source_id)
        if source is None:
            diagnostics.add("BASELINE", source_id, "baseline source was deleted")
        elif _immutable_tuple(source) != _immutable_tuple(prior):
            diagnostics.add(
                "BASELINE", source_id, "baseline immutable identity changed"
            )


def _exact_open_assumptions(
    source: dict[str, object], assumptions: object
) -> list[dict[str, object]]:
    source_id = source.get("id")
    claim_scope = source.get("claim_scope")
    return [
        assumption
        for assumption in _records(assumptions)
        if assumption.get("status") == "open"
        and assumption.get("source_ids") == [source_id]
        and assumption.get("claim_scope") == claim_scope
    ]


def _validate_assumption_joins(
    sources: object, assumptions: object, diagnostics: Diagnostics
) -> None:
    sources_by_id = _by_id(sources)
    assumption_records = _records(assumptions)
    if not isinstance(assumptions, list):
        return

    evidence_ids: set[str] = set()
    for assumption_index, assumption in enumerate(assumptions):
        if not isinstance(assumption, dict):
            continue
        for field in ("source_ids", "evidence_ids"):
            identifiers = assumption.get(field)
            if not isinstance(identifiers, list):
                continue
            for identifier_index, source_id in enumerate(identifiers):
                subject = (
                    f"assumptions[{assumption_index}].{field}[{identifier_index}]"
                )
                if not isinstance(source_id, str) or source_id not in sources_by_id:
                    diagnostics.add(
                        "ASSUMPTION_REFERENCE",
                        subject,
                        f"source ID {source_id!r} does not resolve",
                    )
                elif field == "evidence_ids":
                    evidence_ids.add(source_id)
                    if sources_by_id[source_id].get("status") == "unavailable":
                        diagnostics.add(
                            "ASSUMPTION_REFERENCE",
                            subject,
                            f"evidence source {source_id!r} is unavailable",
                        )

    for source_id, source in sorted(sources_by_id.items()):
        if source.get("status") == "unavailable" and source.get("class") in {
            "comparative",
            "dynamic",
        }:
            if len(_exact_open_assumptions(source, assumption_records)) != 1:
                diagnostics.add(
                    "SOURCE_ASSUMPTION",
                    source_id,
                    "unavailable source requires exactly one matching open assumption",
                )
        if (
            source.get("status") == "current"
            and source.get("class") == "independently-derived"
            and source_id not in evidence_ids
        ):
            diagnostics.add(
                "DERIVED_EVIDENCE",
                source_id,
                "current independently-derived source is not cited by an assumption",
            )


def _boundary_pairs(boundary: object) -> dict[str, object]:
    result: dict[str, object] = {}
    if not isinstance(boundary, list):
        return result
    for row in boundary:
        if isinstance(row, dict) and isinstance(row.get("source_id"), str):
            result.setdefault(row["source_id"], row.get("classification"))
    return result


def _validate_boundary_file(
    boundary: object,
    seeds: object,
    subject: str,
    diagnostics: Diagnostics,
) -> None:
    from jsonschema_subset import ValidationError
    from test_evidence_policy import validate_clone_boundary

    try:
        validate_clone_boundary(boundary, seeds)
    except (ValidationError, AssertionError) as error:
        diagnostics.add("CLONE_BOUNDARY", subject, str(error))

    expected = {
        source_id: {
            "golden": "official-reuse",
            "comparative": "comparative-only",
            "independently-derived": "independently-re-derived",
        }.get(seed.get("class"))
        for source_id, seed in _by_id(seeds).items()
        if seed.get("class") != "dynamic"
    }
    actual = _boundary_pairs(boundary)
    for source_id in sorted(set(expected) - set(actual)):
        diagnostics.add(
            "CLONE_BOUNDARY", source_id, "seed source is missing from clone boundary"
        )
    for source_id in sorted(set(actual) - set(expected)):
        diagnostics.add(
            "CLONE_BOUNDARY", source_id, "unexpected source in seed clone boundary"
        )
    for source_id in sorted(set(expected) & set(actual)):
        if actual[source_id] != expected[source_id]:
            diagnostics.add(
                "CLONE_BOUNDARY",
                source_id,
                f"classification must be {expected[source_id]!r}",
            )


def _validate_supplemental_boundary(
    sources: object,
    seeds: object,
    assumptions: object,
    supplemental: object | None,
    supplemental_present: bool,
    subject: str,
    diagnostics: Diagnostics,
    *,
    excluded_ids: set[str] | None = None,
) -> None:
    from jsonschema_subset import ValidationError
    from test_evidence_policy import validate_clone_boundary_rows

    seed_ids = set(_by_id(seeds))
    sources_by_id = _by_id(sources)
    excluded = set() if excluded_ids is None else excluded_ids
    expected = {
        source_id
        for source_id, source in sources_by_id.items()
        if source_id not in seed_ids
        and source_id not in excluded
        and source.get("class") != "dynamic"
    }

    if not supplemental_present:
        if expected:
            diagnostics.add(
                "CLONE_BOUNDARY",
                subject,
                "supplemental clone boundary is required for non-seed sources",
            )
        return

    try:
        validate_clone_boundary_rows(supplemental)
    except (ValidationError, AssertionError) as error:
        diagnostics.add("CLONE_BOUNDARY", subject, str(error))

    actual = {
        source_id: classification
        for source_id, classification in _boundary_pairs(supplemental).items()
        if source_id not in excluded
    }
    for source_id in sorted(set(actual) & seed_ids):
        diagnostics.add(
            "CLONE_BOUNDARY",
            source_id,
            "supplemental clone boundary overlaps a seed source",
        )
    for source_id in sorted(expected - set(actual)):
        diagnostics.add(
            "CLONE_BOUNDARY",
            source_id,
            "supplemental source is missing from clone boundary",
        )
    for source_id in sorted(set(actual) - expected):
        diagnostics.add(
            "CLONE_BOUNDARY",
            source_id,
            "unexpected source in supplemental clone boundary",
        )

    expected_classifications = {
        "golden": "official-reuse",
        "comparative": "comparative-only",
        "independently-derived": "independently-re-derived",
    }
    for source_id in sorted(expected & set(actual)):
        source = sources_by_id[source_id]
        classification = actual[source_id]
        expected_classification = expected_classifications.get(source.get("class"))
        if classification == expected_classification:
            continue
        if classification == "unresolved" and len(
            _exact_open_assumptions(source, assumptions)
        ) == 1:
            continue
        if classification == "unresolved":
            diagnostics.add(
                "SOURCE_ASSUMPTION",
                source_id,
                "unresolved source requires exactly one matching open assumption",
            )
        else:
            diagnostics.add(
                "CLONE_BOUNDARY",
                source_id,
                f"classification must be {expected_classification!r} or 'unresolved'",
            )


def _validate_fragment_collections(
    root: Path,
    manifest_root: Path,
    seeds: object,
    sources: object,
    assumptions: object,
    seed_realizations: object,
    supplemental_realization_document: object,
    requests_document: object,
    supplemental_boundary: object | None,
    supplemental_boundary_present: bool,
    diagnostics: Diagnostics,
) -> tuple[set[str], dict[str, dict[str, object]]]:
    seed_realization_rows = _records(seed_realizations)
    seed_realization_by_source = {
        str(row["source_id"]): row
        for row in seed_realization_rows
        if isinstance(row.get("source_id"), str)
    }
    realization_rows_value = (
        supplemental_realization_document.get("realizations")
        if isinstance(supplemental_realization_document, dict)
        else None
    )
    realization_rows = _records(realization_rows_value)
    realization_by_source = {
        str(row["source_id"]): row
        for row in realization_rows
        if isinstance(row.get("source_id"), str)
    }
    request_rows_value = (
        requests_document.get("requests")
        if isinstance(requests_document, dict)
        else None
    )
    requests_by_id = {
        str(row["request_id"]): row
        for row in _records(request_rows_value)
        if isinstance(row.get("request_id"), str)
    }

    supplemental_source_path = manifest_root / "supplemental-fragments"
    supplemental_assumption_path = manifest_root / "supplemental-assumption-fragments"
    supplemental_source_present = _control_exists(supplemental_source_path)
    supplemental_assumption_present = _control_exists(supplemental_assumption_path)
    aggregate_source_ids = set(_by_id(sources))
    aggregate_assumption_ids = set(_by_id(assumptions))
    realization_assumption_ids = {
        f"ASM-UNAVAILABLE-{source_id}" for source_id in realization_by_source
    }
    boundary_has_row = isinstance(supplemental_boundary, list) and bool(
        supplemental_boundary
    )
    complete = (
        supplemental_source_present
        or supplemental_assumption_present
        or boundary_has_row
        or bool(aggregate_source_ids & set(realization_by_source))
        or bool(aggregate_assumption_ids & realization_assumption_ids)
    )

    seed_source_collection = _scan_fragment_collection(
        root,
        manifest_root / "fragments",
        required=True,
        diagnostics=diagnostics,
    )
    seed_assumption_collection = _scan_fragment_collection(
        root,
        manifest_root / "assumption-fragments",
        required=True,
        diagnostics=diagnostics,
    )
    supplemental_source_collection = _scan_fragment_collection(
        root,
        supplemental_source_path,
        required=complete,
        diagnostics=diagnostics,
    )

    seed_source_records = _source_fragment_records(
        root, seed_source_collection, diagnostics
    )
    supplemental_source_records = _source_fragment_records(
        root, supplemental_source_collection, diagnostics
    )
    expected_seed_assumptions = _status_assumption_sources(
        seed_source_records, seed_realization_by_source
    )
    expected_supplemental_assumptions = (
        _status_assumption_sources(
            {
                source_id: source
                for source_id, source in supplemental_source_records.items()
                if realization_by_source.get(source_id, {}).get("clone_boundary") is None
            },
            realization_by_source,
        )
        if complete
        else set()
    )
    supplemental_assumption_collection = _scan_fragment_collection(
        root,
        supplemental_assumption_path,
        required=complete and bool(expected_supplemental_assumptions),
        diagnostics=diagnostics,
    )
    seed_assumption_records = _assumption_fragment_records(
        seed_assumption_collection, diagnostics
    )
    supplemental_assumption_records = _assumption_fragment_records(
        supplemental_assumption_collection, diagnostics
    )

    _check_duplicate_fragment_record_ids(
        (
            seed_source_collection,
            seed_assumption_collection,
            supplemental_source_collection,
            supplemental_assumption_collection,
        ),
        diagnostics,
    )

    _fragment_ids(seed_source_collection, set(_by_id(seeds)), diagnostics)
    _fragment_ids(
        seed_assumption_collection, expected_seed_assumptions, diagnostics
    )
    _fragment_ids(
        supplemental_source_collection,
        set(realization_by_source) if complete else set(),
        diagnostics,
    )
    _fragment_ids(
        supplemental_assumption_collection,
        expected_supplemental_assumptions,
        diagnostics,
    )

    _check_fragment_collisions(
        seed_source_records,
        supplemental_source_records,
        seed_assumption_records,
        supplemental_assumption_records,
        set(seed_source_collection.records),
        set(supplemental_source_collection.records),
        set(seed_assumption_collection.records),
        set(supplemental_assumption_collection.records),
        diagnostics,
    )

    all_source_records = [
        *seed_source_records.values(),
        *supplemental_source_records.values(),
    ]
    all_assumption_records = [
        *seed_assumption_records.values(),
        *supplemental_assumption_records.values(),
    ]
    source_fragments_well_formed = (
        len(seed_source_records) == len(seed_source_collection.records)
        and len(supplemental_source_records)
        == len(supplemental_source_collection.records)
    )
    assumption_fragments_well_formed = (
        len(seed_assumption_records) == len(seed_assumption_collection.records)
        and len(supplemental_assumption_records)
        == len(supplemental_assumption_collection.records)
    )
    if source_fragments_well_formed and sources != _sorted_records(all_source_records):
        diagnostics.add(
            "FRAGMENT_COLLECTION",
            "evidence/manifest/sources.json",
            "aggregate differs from sorted fragment collection",
        )
    if (
        assumption_fragments_well_formed
        and assumptions != _sorted_records(all_assumption_records)
    ):
        diagnostics.add(
            "FRAGMENT_COLLECTION",
            "evidence/manifest/assumptions.json",
            "aggregate differs from sorted fragment collection",
        )

    if complete:
        _validate_realization_bindings(
            supplemental_source_records,
            realization_by_source,
            requests_by_id,
            diagnostics,
        )
        _validate_literal_supplemental_boundary(
            supplemental_boundary,
            supplemental_boundary_present,
            _relative_subject(
                root, manifest_root / "supplemental-clone-boundary.json"
            ),
            supplemental_source_records,
            realization_by_source,
            diagnostics,
        )
    return set(realization_by_source), supplemental_source_records


def _path_components(path: Path, relative: PurePosixPath) -> Iterable[Path]:
    current = path
    for part in relative.parts:
        current = current / part
        yield current


def _read_artifact(
    root: Path,
    relative_text: str,
    subject: str,
    diagnostics: Diagnostics,
) -> tuple[Path, bytes | None, bool]:
    relative = PurePosixPath(relative_text)
    candidate = root.joinpath(*relative.parts)
    raw_parts = relative_text.split("/")
    drive_path = (
        len(relative_text) >= 3
        and relative_text[0].isalpha()
        and relative_text[1] == ":"
        and relative_text[2] in {"/", "\\"}
    )
    if relative.is_absolute() or drive_path or any(
        part in {".", ".."} for part in raw_parts
    ):
        diagnostics.add(
            "ARTIFACT_PATH", subject, "artifact path is not safely project-relative"
        )
        return candidate, None, False
    try:
        candidate.relative_to(root)
    except ValueError:
        diagnostics.add("ARTIFACT_PATH", subject, "artifact path escapes the Aius root")
        return candidate, None, False

    saw_symlink = False
    for component in _path_components(root, relative):
        try:
            mode = component.lstat().st_mode
        except OSError as error:
            diagnostics.add(
                "ARTIFACT_PATH", subject, f"artifact path is missing or unreadable: {error}"
            )
            return candidate, None, True
        if stat.S_ISLNK(mode):
            saw_symlink = True
            diagnostics.add(
                "ARTIFACT_PATH",
                subject,
                f"artifact path component is a symlink: {_relative_subject(root, component)}",
            )

    try:
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(root)
    except (OSError, ValueError):
        diagnostics.add("ARTIFACT_PATH", subject, "artifact path escapes the Aius root")
        return candidate, None, False

    if saw_symlink:
        return candidate, None, False
    try:
        mode = candidate.lstat().st_mode
    except OSError as error:
        diagnostics.add("ARTIFACT_PATH", subject, f"cannot inspect artifact: {error}")
        return candidate, None, True
    if not stat.S_ISREG(mode):
        diagnostics.add("ARTIFACT_PATH", subject, "artifact path is not a regular file")
        return candidate, None, True
    try:
        return candidate, candidate.read_bytes(), True
    except OSError as error:
        diagnostics.add("ARTIFACT_PATH", subject, f"cannot read artifact: {error}")
        return candidate, None, True


def _decode_artifact_utf8(payload: bytes) -> str:
    return payload.decode("utf-8-sig")


def _signature_failure(rule: MediaRule, payload: bytes) -> str | None:
    try:
        if rule.kind == "pdf":
            return None if payload.startswith(b"%PDF-") else "PDF signature is missing"
        if rule.kind == "zip":
            prefixes = (b"PK\x03\x04", b"PK\x05\x06", b"PK\x07\x08")
            return None if payload.startswith(prefixes) else "ZIP signature is missing"
        if rule.kind == "png":
            signature = b"\x89PNG\r\n\x1a\n"
            return None if payload.startswith(signature) else "PNG signature is missing"
        if rule.kind == "mhtml":
            header = payload[:4096].decode("ascii").lower()
            if "mime-version:" not in header:
                return "MHTML MIME-Version header is missing"
            if "content-type: multipart/related" not in header:
                return "MHTML multipart/related header is missing"
            return None

        text = _decode_artifact_utf8(payload)
        if rule.kind == "html":
            probe = text.lstrip(" \t\r\n\f\v")[:1024].lower()
            if "<!doctype html" not in probe and "<html" not in probe:
                return "HTML document signature is missing"
        elif rule.kind == "step":
            stripped = text.strip()
            if not stripped.startswith("ISO-10303-21;"):
                return "STEP start signature is missing"
            if not stripped.endswith("END-ISO-10303-21;"):
                return "STEP end signature is missing"
        elif rule.kind == "kicad_sch" and not text.strip().startswith("(kicad_sch"):
            return "KiCad schematic signature is missing"
        elif rule.kind == "kicad_pcb" and not text.strip().startswith("(kicad_pcb"):
            return "KiCad PCB signature is missing"
        elif rule.kind == "kicad_sym" and not text.strip().startswith(
            "(kicad_symbol_lib"
        ):
            return "KiCad symbol-library signature is missing"
        elif rule.kind == "kicad_mod" and not text.strip().startswith("(footprint"):
            return "KiCad footprint signature is missing"
        elif rule.kind in {"json", "kicad_pro"}:
            try:
                decoded = json.loads(text)
            except json.JSONDecodeError as error:
                return f"JSON content is invalid: {error}"
            if rule.kind == "kicad_pro" and not isinstance(decoded, dict):
                return "KiCad project JSON must be an object"
        elif rule.kind == "csv":
            try:
                rows = csv.reader(io.StringIO(text, newline=""))
                if not any(row and any(cell != "" for cell in row) for row in rows):
                    return "CSV contains no non-empty row"
            except csv.Error as error:
                return f"CSV content is invalid: {error}"
        elif rule.kind == "text" and "\x00" in text:
            return "text content contains a NUL character"
    except UnicodeDecodeError as error:
        return f"artifact payload is not UTF-8: {error}"
    return None


def validate_artifact_payload(
    root: Path,
    relative_path: str,
    payload: bytes | None,
    media_type: str,
    storage: str,
) -> tuple[Diagnostic, ...]:
    """Validate artifact media and placement, plus readable payload content."""

    resolved_root = _resolve_aius_root(root)

    diagnostics: set[Diagnostic] = set()

    def add(code: str, message: str) -> None:
        diagnostics.add(Diagnostic(code, relative_path, message))

    relative = PurePosixPath(relative_path)
    raw_parts = relative_path.split("/")
    drive_path = (
        len(relative_path) >= 3
        and relative_path[0].isalpha()
        and relative_path[1] == ":"
        and relative_path[2] in {"/", "\\"}
    )
    if (
        relative.is_absolute()
        or drive_path
        or any(part in {"", ".", ".."} for part in raw_parts)
    ):
        add("ARTIFACT_PATH", "artifact path is not safely project-relative")
        return tuple(sorted(diagnostics))

    candidate = resolved_root.joinpath(*relative.parts)
    try:
        candidate.relative_to(resolved_root)
    except ValueError:
        add("ARTIFACT_PATH", "artifact path escapes the Aius root")
        return tuple(sorted(diagnostics))

    suffix = relative.suffix
    rule = MEDIA_RULES.get(suffix)
    if rule is None:
        add(
            "ARTIFACT_MEDIA_TYPE",
            f"unknown case-sensitive artifact suffix {suffix!r}",
        )
    else:
        if media_type != rule.media_type:
            add(
                "ARTIFACT_MEDIA_TYPE",
                f"media type must be {rule.media_type!r} for suffix {suffix!r}",
            )
        if storage != rule.storage:
            add(
                "ARTIFACT_STORAGE",
                f"suffix {suffix!r} requires storage {rule.storage!r}",
            )

    if storage not in {"git", "lfs"}:
        add("ARTIFACT_STORAGE", "payload storage must be 'git' or 'lfs'")
    else:
        git_context = GitContext.discover(resolved_root)
        observed_filter = git_context.filter_attribute(candidate)
        expected_filter = "lfs" if storage == "lfs" else "unspecified"
        if observed_filter != expected_filter:
            add(
                "GIT_ATTRIBUTE",
                f"filter is {observed_filter!r}; expected {expected_filter!r}",
            )

    if (
        payload is not None
        and storage == "lfs"
        and payload.startswith(LFS_POINTER_PREFIX)
    ):
        add("LFS_POINTER", "checked-out artifact is Git LFS pointer text")
    if rule is not None and payload is not None:
        failure = _signature_failure(rule, payload)
        if failure is not None:
            add("ARTIFACT_SIGNATURE", failure)
    return tuple(sorted(diagnostics))


def _validate_artifacts(
    root: Path,
    sources: object,
    diagnostics: Diagnostics,
    git_context: GitContext | None,
) -> GitContext | None:
    if not isinstance(sources, list):
        return git_context
    for source_index, source in enumerate(sources):
        if not isinstance(source, dict):
            continue
        source_id = source.get("id")
        artifacts = source.get("artifacts")
        if not isinstance(artifacts, list):
            continue
        for artifact_index, artifact in enumerate(artifacts):
            if not isinstance(artifact, dict):
                continue
            if artifact.get("storage") not in {"git", "lfs"}:
                continue
            if artifact.get("retrieval_status") != "current":
                continue
            subject = f"sources[{source_index}].artifacts[{artifact_index}]"
            relative_text = artifact.get("path")
            if not isinstance(relative_text, str):
                continue

            storage = artifact.get("storage")
            relative = PurePosixPath(relative_text)
            expected_prefix = PurePosixPath(
                "docs/reference/datasheets.md" if storage == "lfs" else "evidence/source-snapshots"
            ) / str(source_id)
            try:
                relative.relative_to(expected_prefix)
            except ValueError:
                diagnostics.add(
                    "ARTIFACT_PATH",
                    subject,
                    f"{storage} artifact is outside {expected_prefix.as_posix()}/",
                )

            _artifact_path, payload, _safe_for_attribute = _read_artifact(
                root, relative_text, subject, diagnostics
            )
            for item in validate_artifact_payload(
                root,
                relative_text,
                payload,
                str(artifact.get("media_type")),
                str(storage),
            ):
                diagnostics.add(item.code, subject, item.message)
            if payload is None:
                continue
            if artifact.get("bytes") != len(payload):
                diagnostics.add(
                    "ARTIFACT_BYTES",
                    subject,
                    f"recorded {artifact.get('bytes')!r} bytes; found {len(payload)}",
                )
            digest = hashlib.sha256(payload).hexdigest()
            if artifact.get("sha256") != digest:
                diagnostics.add(
                    "ARTIFACT_SHA256",
                    subject,
                    f"recorded {artifact.get('sha256')!r}; found {digest}",
                )
    return git_context


def _explicit_baseline(root: Path, value: str) -> object:
    requested = Path(value)
    if requested.is_absolute():
        path = requested.resolve()
    else:
        path = (root / requested).resolve()
        try:
            path.relative_to(root)
        except ValueError as error:
            raise ControlError("relative baseline must remain beneath --root") from error
    try:
        mode = path.lstat().st_mode
    except OSError as error:
        raise ControlError(f"explicit baseline is not a regular JSON file: {error}") from error
    if not stat.S_ISREG(mode):
        raise ControlError("explicit baseline is not a regular JSON file")
    return _load_control(path, "explicit baseline")


def validate_root(root: Path, baseline_argument: str | None) -> int:
    root = _resolve_aius_root(root)
    manifest_root = root / "evidence" / "manifest"
    source_schema_path = manifest_root / "source-schema.json"
    seed_path = manifest_root / "seed-sources.json"
    sources_path = manifest_root / "sources.json"
    assumption_schema_path = manifest_root / "assumption-schema.json"
    assumptions_path = manifest_root / "assumptions.json"
    boundary_path = manifest_root / "clone-boundary.json"
    supplemental_path = manifest_root / "supplemental-clone-boundary.json"
    seed_realization_path = manifest_root / "seed-realization.json"
    supplemental_realization_path = manifest_root / "supplemental-realization.json"
    requests_path = manifest_root / "supplemental-requests.json"

    source_schema = _load_control(source_schema_path)
    seeds = _load_control(seed_path)
    sources = _load_control(sources_path)
    assumption_schema = _load_control(assumption_schema_path)
    assumptions = _load_control(assumptions_path)
    boundary = _load_control(boundary_path)
    seed_realizations = _load_control(seed_realization_path)
    supplemental_realizations = _load_control(supplemental_realization_path)
    requests = _load_control(requests_path)

    supplemental_present = _control_exists(supplemental_path)
    supplemental = (
        _load_control(supplemental_path) if supplemental_present else None
    )

    git_context: GitContext | None = None
    if baseline_argument is not None:
        baseline = _explicit_baseline(root, baseline_argument)
    else:
        git_context = GitContext.discover(root)
        baseline = git_context.baseline()

    diagnostics = Diagnostics()
    source_schema_subject = _relative_subject(root, source_schema_path)
    assumption_schema_subject = _relative_subject(root, assumption_schema_path)
    source_schema_valid = _check_schema(
        source_schema, source_schema_subject, diagnostics
    )
    assumption_schema_valid = _check_schema(
        assumption_schema, assumption_schema_subject, diagnostics
    )

    if source_schema_valid and isinstance(source_schema, dict):
        seed_schema = source_schema.get("$defs", {}).get("seedCollection")
        if seed_schema is None:
            diagnostics.add(
                "MANIFEST_SCHEMA",
                source_schema_subject,
                "schema has no $defs.seedCollection",
            )
        else:
            _schema_validate(
                seeds,
                seed_schema,
                _relative_subject(root, seed_path),
                diagnostics,
                root_schema=source_schema,
            )
        _schema_validate(
            sources,
            source_schema,
            _relative_subject(root, sources_path),
            diagnostics,
        )
        baseline_schema_valid = _schema_validate(
            baseline, source_schema, "baseline", diagnostics
        )
    else:
        baseline_schema_valid = False

    if assumption_schema_valid:
        _schema_validate(
            assumptions,
            assumption_schema,
            _relative_subject(root, assumptions_path),
            diagnostics,
        )

    _validate_source_and_baseline_collections(
        sources, seeds, baseline, baseline_schema_valid, diagnostics
    )
    _validate_immutable_identities(
        sources, seeds, baseline, baseline_schema_valid, diagnostics
    )
    _validate_assumption_joins(sources, assumptions, diagnostics)
    _validate_boundary_file(
        boundary,
        seeds,
        _relative_subject(root, boundary_path),
        diagnostics,
    )
    realization_ids, _supplemental_sources = _validate_fragment_collections(
        root,
        manifest_root,
        seeds,
        sources,
        assumptions,
        seed_realizations,
        supplemental_realizations,
        requests,
        supplemental,
        supplemental_present,
        diagnostics,
    )
    _validate_supplemental_boundary(
        sources,
        seeds,
        assumptions,
        supplemental,
        supplemental_present,
        _relative_subject(root, supplemental_path),
        diagnostics,
        excluded_ids=realization_ids,
    )
    _validate_artifacts(root, sources, diagnostics, git_context)

    return 1 if diagnostics.emit() else 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", help="Aius project root")
    parser.add_argument("--baseline", help="explicit sources.json baseline")
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run the sibling validator contract test",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _parser()
    arguments = parser.parse_args(argv)
    if arguments.self_test and (arguments.root is not None or arguments.baseline is not None):
        parser.error("--self-test is mutually exclusive with --root and --baseline")
    if arguments.self_test:
        test_script = Path(__file__).resolve().with_name("test_validate_evidence.py")
        try:
            return subprocess.run([sys.executable, os.fspath(test_script)], check=False).returncode
        except OSError as error:
            print(f"ERROR: cannot run self-test: {error}", file=sys.stderr)
            return 2

    root = (
        Path(arguments.root)
        if arguments.root is not None
        else Path(__file__).resolve().parents[1]
    )
    try:
        return validate_root(root, arguments.baseline)
    except ControlError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
