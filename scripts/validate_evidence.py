#!/usr/bin/env python3
"""Validate the Aius evidence checkout and its immutable source baseline."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
import stat
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Iterable


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
) -> None:
    from jsonschema_subset import ValidationError
    from test_evidence_policy import validate_clone_boundary_rows

    seed_ids = set(_by_id(seeds))
    sources_by_id = _by_id(sources)
    expected = {
        source_id
        for source_id, source in sources_by_id.items()
        if source_id not in seed_ids and source.get("class") != "dynamic"
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

    actual = _boundary_pairs(supplemental)
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

            artifact_path, payload, safe_for_attribute = _read_artifact(
                root, relative_text, subject, diagnostics
            )

            suffix = relative.suffix
            rule = MEDIA_RULES.get(suffix)
            if rule is None:
                diagnostics.add(
                    "ARTIFACT_MEDIA_TYPE",
                    subject,
                    f"unknown case-sensitive artifact suffix {suffix!r}",
                )
            else:
                if artifact.get("media_type") != rule.media_type:
                    diagnostics.add(
                        "ARTIFACT_MEDIA_TYPE",
                        subject,
                        f"media type must be {rule.media_type!r} for suffix {suffix!r}",
                    )
                if storage != rule.storage:
                    diagnostics.add(
                        "ARTIFACT_STORAGE",
                        subject,
                        f"suffix {suffix!r} requires storage {rule.storage!r}",
                    )

            if safe_for_attribute:
                if git_context is None:
                    git_context = GitContext.discover(root)
                observed_filter = git_context.filter_attribute(artifact_path)
                expected_filter = "lfs" if storage == "lfs" else "unspecified"
                if observed_filter != expected_filter:
                    diagnostics.add(
                        "GIT_ATTRIBUTE",
                        subject,
                        f"filter is {observed_filter!r}; expected {expected_filter!r}",
                    )

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
            if storage == "lfs" and payload.startswith(LFS_POINTER_PREFIX):
                diagnostics.add(
                    "LFS_POINTER", subject, "checked-out artifact is Git LFS pointer text"
                )
            if rule is not None:
                failure = _signature_failure(rule, payload)
                if failure is not None:
                    diagnostics.add("ARTIFACT_SIGNATURE", subject, failure)
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
    root = root.resolve()
    manifest_root = root / "evidence" / "manifest"
    source_schema_path = manifest_root / "source-schema.json"
    seed_path = manifest_root / "seed-sources.json"
    sources_path = manifest_root / "sources.json"
    assumption_schema_path = manifest_root / "assumption-schema.json"
    assumptions_path = manifest_root / "assumptions.json"
    boundary_path = manifest_root / "clone-boundary.json"
    supplemental_path = manifest_root / "supplemental-clone-boundary.json"

    source_schema = _load_control(source_schema_path)
    seeds = _load_control(seed_path)
    sources = _load_control(sources_path)
    assumption_schema = _load_control(assumption_schema_path)
    assumptions = _load_control(assumptions_path)
    boundary = _load_control(boundary_path)

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
    _validate_supplemental_boundary(
        sources,
        seeds,
        assumptions,
        supplemental,
        supplemental_present,
        _relative_subject(root, supplemental_path),
        diagnostics,
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
