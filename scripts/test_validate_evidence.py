#!/usr/bin/env python3
"""Isolated Git-fixture tests for the Aius evidence validator."""

from __future__ import annotations

import copy
import hashlib
import io
import json
import os
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest import mock

import validate_evidence as validator_module
import seed_evidence
from validate_evidence import ControlError, validate_artifact_payload


SCRIPT_ROOT = Path(__file__).resolve().parent
AIUS_ROOT = SCRIPT_ROOT.parent
MANIFEST_ROOT = AIUS_ROOT / "evidence" / "manifest"
VALIDATOR = SCRIPT_ROOT / "validate_evidence.py"
COMPLETE_FIXTURE_ROOT = SCRIPT_ROOT / "fixtures" / "supplemental-assembly" / "valid"

MEDIA_FIXTURES = {
    ".pdf": ("application/pdf", "lfs", b"%PDF-1.7\nfixture\n"),
    ".zip": ("application/zip", "lfs", b"PK\x03\x04fixture"),
    ".png": ("image/png", "lfs", b"\x89PNG\r\n\x1a\nfixture"),
    ".html": ("text/html", "git", b"\xef\xbb\xbf  <!DOCTYPE HTML><html></html>"),
    ".htm": ("text/html", "git", b"<html></html>"),
    ".mhtml": (
        "multipart/related",
        "lfs",
        b"MIME-Version: 1.0\r\nContent-Type: multipart/related; boundary=x\r\n",
    ),
    ".step": (
        "model/step",
        "lfs",
        b"\xef\xbb\xbfISO-10303-21;\nDATA;\nENDSEC;\nEND-ISO-10303-21;\n",
    ),
    ".stp": ("model/step", "lfs", b"ISO-10303-21;END-ISO-10303-21;"),
    ".kicad_sch": ("application/x-kicad-schematic", "git", b"(kicad_sch)\n"),
    ".kicad_pcb": ("application/x-kicad-pcb", "git", b"(kicad_pcb)\n"),
    ".kicad_sym": (
        "application/x-kicad-symbol-library",
        "git",
        b"(kicad_symbol_lib)\n",
    ),
    ".kicad_mod": ("application/x-kicad-footprint", "git", b"(footprint x)\n"),
    ".kicad_pro": ("application/json", "git", b'{"board": {}}\n'),
    ".json": ("application/json", "git", b"[1, 2, 3]\n"),
    ".csv": ("text/csv", "git", b"name,value\nfixture,1\n"),
    ".c": ("text/x-c", "git", b"int fixture(void) { return 0; }\n"),
    ".h": ("text/x-c", "git", b"int fixture(void);\n"),
    ".cpp": ("text/x-c++", "git", b"int fixture() { return 0; }\n"),
    ".cc": ("text/x-c++", "git", b"int fixture() { return 0; }\n"),
    ".cxx": ("text/x-c++", "git", b"int fixture() { return 0; }\n"),
    ".hpp": ("text/x-c++", "git", b"int fixture();\n"),
    ".hh": ("text/x-c++", "git", b"int fixture();\n"),
    ".hxx": ("text/x-c++", "git", b"int fixture();\n"),
    ".rst": ("text/x-rst", "git", "Résumé\n======\n".encode("utf-8")),
    ".txt": ("text/plain", "git", b"fixture\n"),
    ".md": ("text/markdown", "git", b"# Fixture\n"),
}


def _load_json(path: Path) -> object:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def _run(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def _external_artifact(
    *,
    unavailable: bool = False,
    with_digest: bool = False,
) -> dict[str, object]:
    if unavailable:
        return {
            "name": "unavailable.pdf",
            "final_url": "https://example.com/unavailable.pdf",
            "storage": "external-reference",
            "path": None,
            "sha256": None,
            "bytes": None,
            "media_type": None,
            "retrieval_status": "unavailable",
            "note": "The required object was unavailable.",
        }
    return {
        "name": "external.pdf",
        "final_url": "https://example.com/external.pdf",
        "storage": "external-reference",
        "path": None,
        "sha256": "a" * 64 if with_digest else None,
        "bytes": 123 if with_digest else None,
        "media_type": "application/pdf",
        "retrieval_status": "current",
        "note": "Inspected without redistribution permission.",
    }


def _source_from_seed(seed: dict[str, object]) -> dict[str, object]:
    return {
        "id": seed["id"],
        "class": seed["class"],
        "title": f"Fixture for {seed['id']}",
        "authority": seed["authority"],
        "revision": "fixture-1",
        "purpose": seed["purpose"],
        "claim_scope": copy.deepcopy(seed["claim_scope"]),
        "landing_url": seed["landing_url"],
        "retrieved_at": "2026-07-16T12:00:00Z",
        "status": "current",
        "supersedes": None,
        "redistribution_note": "Fixture metadata only.",
        "artifacts": [_external_artifact()],
        "observations": [],
    }


def _nonseed_source(
    source_id: str,
    source_class: str,
    *,
    status: str = "current",
) -> dict[str, object]:
    artifact = _external_artifact(unavailable=status == "unavailable")
    return {
        "id": source_id,
        "class": source_class,
        "title": f"Fixture {source_id}",
        "authority": "Fixture Authority",
        "revision": "fixture-1",
        "purpose": "Exercise supplemental evidence validation.",
        "claim_scope": [f"claim for {source_id}"],
        "landing_url": f"https://example.com/{source_id.lower()}",
        "retrieved_at": "2026-07-16T12:00:00Z",
        "status": status,
        "supersedes": None,
        "redistribution_note": "Fixture metadata only.",
        "artifacts": [artifact],
        "observations": [],
    }


def _assumption(
    source_id: str,
    claim_scope: list[object],
    *,
    evidence_ids: list[str] | None = None,
    status: str = "open",
) -> dict[str, object]:
    return {
        "id": f"ASM-{source_id}",
        "claim": "The fixture claim remains provisional.",
        "consequence": "The dependent decision remains provisional.",
        "owner": "P1 fixture",
        "validation_gate": "Replace the provisional evidence.",
        "evidence_ids": [] if evidence_ids is None else evidence_ids,
        "source_ids": [source_id],
        "claim_scope": copy.deepcopy(claim_scope),
        "status": status,
        "resolution": None if status == "open" else "Resolved by fixture evidence.",
    }


def _boundary_row(source_id: str, classification: str) -> dict[str, object]:
    return {
        "source_id": source_id,
        "classification": classification,
        "reused_material": ["Official fixture material"]
        if classification == "official-reuse"
        else [],
        "derived_claims": ["A fixture claim is bounded."],
        "prohibited_use": ["Do not extend the fixture claim."],
        "notes": "",
    }


class Fixture:
    def __init__(self) -> None:
        self._temporary = tempfile.TemporaryDirectory(prefix="aius-evidence-")
        self.worktree = Path(self._temporary.name) / "repo"
        self.root = self.worktree / "src" / "hardware" / "aius"
        self.manifest = self.root / "evidence" / "manifest"
        self.scripts = self.root / "scripts"
        self.manifest.mkdir(parents=True)
        self.scripts.mkdir(parents=True)

        for name in (
            "source-schema.json",
            "seed-sources.json",
            "assumption-schema.json",
            "clone-boundary.json",
            "seed-realization.json",
            "supplemental-realization.json",
            "supplemental-requests.json",
            "README.md",
        ):
            shutil.copy2(MANIFEST_ROOT / name, self.manifest / name)
        for name in (
            "jsonschema_subset.py",
            "test_source_schema.py",
            "test_evidence_policy.py",
            "validate_evidence.py",
        ):
            shutil.copy2(SCRIPT_ROOT / name, self.scripts / name)

        self.seeds = _load_json(self.manifest / "seed-sources.json")
        assert isinstance(self.seeds, list)
        by_id = {
            seed["id"]: seed for seed in self.seeds if isinstance(seed, dict)
        }
        self.seed_ids = set(by_id)
        self.sources = [_source_from_seed(seed) for seed in self.seeds]
        self.sources.sort(key=lambda source: str(source["id"]))
        self.assumptions: list[dict[str, object]] = []
        self.supplemental: list[dict[str, object]] | None = None
        self.baseline: object = []
        self.flush()

        self.worktree.mkdir(exist_ok=True)
        attributes = (
            "/evidence/artifacts/** filter=lfs diff=lfs merge=lfs\n"
        )
        (self.worktree / ".gitattributes").write_text(attributes, encoding="utf-8")
        self.git("init", "-q")
        self.git("config", "user.name", "Aius Fixture")
        self.git("config", "user.email", "aius-fixture@example.invalid")
        self.git("add", ".gitattributes")
        self.git("commit", "-q", "-m", "fixture root")

    def close(self) -> None:
        self._temporary.cleanup()

    def git(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        completed = _run(["git", *arguments], self.worktree)
        if completed.returncode != 0:
            raise AssertionError(
                f"git {' '.join(arguments)} failed: {completed.stderr}"
            )
        return completed

    def flush(self) -> None:
        (self.manifest / "sources.json").write_text(
            json.dumps(self.sources, indent=2) + "\n", encoding="utf-8"
        )
        (self.manifest / "assumptions.json").write_text(
            json.dumps(self.assumptions, indent=2) + "\n", encoding="utf-8"
        )
        (self.root / "baseline.json").write_text(
            json.dumps(self.baseline, indent=2) + "\n", encoding="utf-8"
        )
        supplemental_path = self.manifest / "supplemental-clone-boundary.json"
        if self.supplemental is not None:
            supplemental_path.write_text(
                json.dumps(self.supplemental, indent=2) + "\n", encoding="utf-8"
            )
        elif supplemental_path.exists():
            supplemental_path.unlink()

        fragment_root = self.manifest / "fragments"
        assumption_fragment_root = self.manifest / "assumption-fragments"
        fragment_root.mkdir(exist_ok=True)
        assumption_fragment_root.mkdir(exist_ok=True)
        for path in fragment_root.glob("*.json"):
            path.unlink()
        for path in assumption_fragment_root.glob("*.json"):
            path.unlink()
        for source in self.sources:
            source_id = source.get("id")
            if isinstance(source_id, str) and source_id in self.seed_ids:
                (fragment_root / f"{source_id}.json").write_text(
                    json.dumps(source, indent=2) + "\n", encoding="utf-8"
                )
        for assumption in self.assumptions:
            source_ids = assumption.get("source_ids")
            if (
                isinstance(source_ids, list)
                and len(source_ids) == 1
                and source_ids[0] in self.seed_ids
            ):
                (assumption_fragment_root / f"{source_ids[0]}.json").write_text(
                    json.dumps(assumption, indent=2) + "\n", encoding="utf-8"
                )

    def source(self, source_id: str) -> dict[str, object]:
        return next(source for source in self.sources if source["id"] == source_id)

    def add_source(self, source: dict[str, object]) -> None:
        self.sources.append(source)
        self.sources.sort(key=lambda record: str(record["id"]))

    def add_artifact(
        self,
        source_id: str,
        suffix: str,
        *,
        payload: bytes | None = None,
        media_type: str | None = None,
        storage: str | None = None,
        append: bool = False,
        stem: str = "artifact",
    ) -> dict[str, object]:
        default_media, default_storage, default_payload = MEDIA_FIXTURES[suffix]
        selected_storage = default_storage if storage is None else storage
        selected_payload = default_payload if payload is None else payload
        directory = (
            "docs/reference/datasheets.md"
            if selected_storage == "lfs"
            else "evidence/source-snapshots"
        )
        relative = f"{directory}/{source_id}/{stem}{suffix}"
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(selected_payload)
        artifact = {
            "name": f"{stem}{suffix}",
            "final_url": f"https://example.com/{stem}{suffix}",
            "storage": selected_storage,
            "path": relative,
            "sha256": hashlib.sha256(selected_payload).hexdigest(),
            "bytes": len(selected_payload),
            "media_type": default_media if media_type is None else media_type,
            "retrieval_status": "current",
            "note": "Redistributable fixture payload.",
        }
        source = self.source(source_id)
        artifacts = source["artifacts"]
        assert isinstance(artifacts, list)
        if append:
            artifacts.append(artifact)
        else:
            source["artifacts"] = [artifact]
        return artifact

    def run(
        self,
        *,
        explicit_baseline: bool = True,
        validator: Path | None = None,
        arguments: list[str] | None = None,
        flush: bool = True,
    ) -> subprocess.CompletedProcess[str]:
        if flush:
            self.flush()
        command = [sys.executable, "-B", os.fspath(validator or VALIDATOR)]
        if arguments is None:
            command.extend(["--root", os.fspath(self.root)])
            if explicit_baseline:
                command.extend(["--baseline", "baseline.json"])
        else:
            command.extend(arguments)
        return _run(command, self.worktree)


class CompleteFixture:
    def __init__(self) -> None:
        self._temporary = tempfile.TemporaryDirectory(prefix="aius-complete-")
        self.worktree = Path(self._temporary.name) / "repo"
        self.root = self.worktree / "src" / "hardware" / "aius"
        shutil.copytree(COMPLETE_FIXTURE_ROOT, self.root)
        self.manifest = self.root / "evidence" / "manifest"
        (self.root / "baseline.json").write_text("[]\n", encoding="utf-8")
        (self.worktree / ".gitattributes").write_text(
            "/evidence/artifacts/** filter=lfs diff=lfs merge=lfs\n",
            encoding="utf-8",
        )
        _run(["git", "init", "-q"], self.worktree)

    def close(self) -> None:
        self._temporary.cleanup()

    def run(self) -> subprocess.CompletedProcess[str]:
        return _run(
            [
                sys.executable,
                "-B",
                os.fspath(VALIDATOR),
                "--root",
                os.fspath(self.root),
                "--baseline",
                "baseline.json",
            ],
            self.worktree,
        )

    def json(self, relative: str) -> object:
        return _load_json(self.root / relative)

    def write_json(self, relative: str, value: object) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")

    def source(self, source_id: str) -> dict[str, object]:
        value = self.json(
            f"evidence/manifest/supplemental-fragments/{source_id}.json"
        )
        assert isinstance(value, dict)
        return value

    def write_source(self, source: dict[str, object]) -> None:
        source_id = str(source["id"])
        self.write_json(
            f"evidence/manifest/supplemental-fragments/{source_id}.json",
            source,
        )
        sources = self.json("evidence/manifest/sources.json")
        assert isinstance(sources, list)
        replacement = [
            source if row.get("id") == source_id else row
            for row in sources
            if isinstance(row, dict)
        ]
        replacement.sort(key=lambda row: str(row["id"]))
        self.write_json("evidence/manifest/sources.json", replacement)

    def realization(self, source_id: str) -> dict[str, object]:
        document = self.json("evidence/manifest/supplemental-realization.json")
        assert isinstance(document, dict)
        rows = document["realizations"]
        assert isinstance(rows, list)
        return next(row for row in rows if row["source_id"] == source_id)

    def make_current_manufacturer(self, source_id: str) -> dict[str, object]:
        source = self.source(source_id)
        realization = self.realization(source_id)
        realization_artifacts = realization["artifacts"]
        assert isinstance(realization_artifacts, list)
        artifacts: list[dict[str, object]] = []
        digests: list[str] = []
        for index, row in enumerate(realization_artifacts):
            assert isinstance(row, dict)
            suffix = Path(str(row["path"])).suffix
            _media, _storage, payload = MEDIA_FIXTURES[suffix]
            target = self.root / str(row["path"])
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(payload)
            digest = hashlib.sha256(payload).hexdigest()
            digests.append(digest)
            artifacts.append(
                {
                    "name": row["name"],
                    "final_url": row["expected_final_url"],
                    "storage": row["storage"],
                    "path": row["path"],
                    "sha256": digest,
                    "bytes": len(payload),
                    "media_type": row["media_type"],
                    "retrieval_status": "current",
                    "note": "",
                }
            )
        source["revision"] = "sha256:" + hashlib.sha256(
            "".join(digests).encode("ascii")
        ).hexdigest()
        source["status"] = "current"
        source["redistribution_note"] = (
            "Retained for personal noncommercial research; no republication permission is inferred."
        )
        source["artifacts"] = artifacts
        source["observations"] = []
        self.write_source(source)
        boundary = self.json("evidence/manifest/supplemental-clone-boundary.json")
        assert isinstance(boundary, list)
        current_row = realization["clone_boundary"]["current"]
        boundary = [
            current_row if row.get("source_id") == source_id else row
            for row in boundary
        ]
        self.write_json(
            "evidence/manifest/supplemental-clone-boundary.json", boundary
        )
        return source


class ValidatorFixtureTests(unittest.TestCase):
    fixture: Fixture

    def setUp(self) -> None:
        self.fixture = Fixture()

    def tearDown(self) -> None:
        self.fixture.close()

    def assert_diagnostic(
        self,
        completed: subprocess.CompletedProcess[str],
        code: str,
        subject: str,
        *,
        exit_code: int = 1,
    ) -> None:
        self.assertEqual(completed.returncode, exit_code, completed.stderr)
        prefix = f"ERROR {code} {subject}:"
        self.assertTrue(
            any(line.startswith(prefix) for line in completed.stderr.splitlines()),
            f"missing {prefix!r} in:\n{completed.stderr}",
        )

    def make_passing_multi_artifact_fixture(self) -> None:
        self.fixture.add_artifact("CLIFF-ORJ5", ".pdf", stem="datasheet")
        self.fixture.add_artifact(
            "CLIFF-ORJ5", ".json", stem="metadata", append=True
        )

    def test_passing_multi_artifact_collection_and_external_references(self) -> None:
        self.make_passing_multi_artifact_fixture()
        self.fixture.source("CLIFF-OTJ5")["artifacts"] = [
            _external_artifact(with_digest=True)
        ]
        completed = self.fixture.run()
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stderr, "")

    def test_every_media_suffix_accepts_its_exact_signature_and_storage(self) -> None:
        for index, suffix in enumerate(MEDIA_FIXTURES):
            self.fixture.add_artifact(
                "CLIFF-ORJ5", suffix, append=index != 0, stem=f"artifact-{index}"
            )
        completed = self.fixture.run()
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_wrong_top_level_source_and_baseline_types_are_validation_exit_one(self) -> None:
        (self.fixture.manifest / "sources.json").write_text("{}\n", encoding="utf-8")
        completed = self.fixture.run(arguments=[
            "--root",
            os.fspath(self.fixture.root),
            "--baseline",
            "baseline.json",
        ], flush=False)
        self.assert_diagnostic(
            completed,
            "MANIFEST_SCHEMA",
            "evidence/manifest/sources.json",
        )

        self.fixture.flush()
        self.fixture.baseline = {}
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "MANIFEST_SCHEMA", "baseline")

    def test_control_file_io_utf8_and_json_failures_are_exit_two(self) -> None:
        for name, payload in (
            ("sources.json", b"\xff"),
            ("assumptions.json", b"{"),
        ):
            with self.subTest(name=name):
                self.fixture.flush()
                (self.fixture.manifest / name).write_bytes(payload)
                completed = self.fixture.run(arguments=[
                    "--root",
                    os.fspath(self.fixture.root),
                    "--baseline",
                    "baseline.json",
                ], flush=False)
                self.assertEqual(completed.returncode, 2, completed.stderr)
                self.assertNotIn("ERROR MANIFEST_SCHEMA", completed.stderr)

    def test_explicit_baseline_and_supplemental_control_syntax_failures_are_exit_two(
        self,
    ) -> None:
        self.fixture.flush()
        (self.fixture.root / "baseline.json").write_bytes(b"\xff")
        completed = self.fixture.run(arguments=[
            "--root",
            os.fspath(self.fixture.root),
            "--baseline",
            "baseline.json",
        ], flush=False)
        self.assertEqual(completed.returncode, 2, completed.stderr)

        self.fixture = self._replace_fixture()
        source = _nonseed_source("SUPPLEMENTAL-GOLDEN", "golden")
        self.fixture.add_source(source)
        self.fixture.flush()
        (self.fixture.manifest / "supplemental-clone-boundary.json").write_text(
            "{", encoding="utf-8"
        )
        completed = self.fixture.run(arguments=[
            "--root",
            os.fspath(self.fixture.root),
            "--baseline",
            "baseline.json",
        ], flush=False)
        self.assertEqual(completed.returncode, 2, completed.stderr)

    def test_missing_required_control_file_is_exit_two(self) -> None:
        self.fixture.flush()
        (self.fixture.manifest / "assumptions.json").unlink()
        completed = self.fixture.run(arguments=[
            "--root",
            os.fspath(self.fixture.root),
            "--baseline",
            "baseline.json",
        ], flush=False)
        self.assertEqual(completed.returncode, 2, completed.stderr)

    def test_invalid_invocation_and_nonregular_baseline_are_exit_two(self) -> None:
        completed = self.fixture.run(arguments=[
            "--self-test",
            "--root",
            os.fspath(self.fixture.root),
        ])
        self.assertEqual(completed.returncode, 2, completed.stderr)

        baseline_directory = self.fixture.root / "baseline-dir"
        baseline_directory.mkdir()
        completed = self.fixture.run(arguments=[
            "--root",
            os.fspath(self.fixture.root),
            "--baseline",
            "baseline-dir",
        ])
        self.assertEqual(completed.returncode, 2, completed.stderr)

    def test_source_collection_import_preserves_code_subject_and_message(self) -> None:
        self.fixture.sources[0], self.fixture.sources[1] = (
            self.fixture.sources[1],
            self.fixture.sources[0],
        )
        completed = self.fixture.run()
        self.assert_diagnostic(
            completed,
            "SOURCE_COLLECTION",
            "evidence/manifest/sources.json",
        )
        self.assertIn("sources: IDs are not sorted", completed.stderr)

    def test_baseline_deleted_changed_new_and_external_absolute_cases(self) -> None:
        self.fixture.baseline = [copy.deepcopy(self.fixture.sources[0])]
        self.fixture.sources.pop(0)
        completed = self.fixture.run()
        deleted_id = self.fixture.baseline[0]["id"]
        self.assert_diagnostic(completed, "BASELINE", str(deleted_id))

        self.fixture = self._replace_fixture()
        self.fixture.baseline = [copy.deepcopy(self.fixture.sources[0])]
        self.fixture.sources[0]["authority"] = "Changed Authority"
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "BASELINE", str(self.fixture.sources[0]["id"]))

        self.fixture = self._replace_fixture()
        new_dynamic = _nonseed_source("NEW-DYNAMIC", "dynamic")
        self.fixture.add_source(new_dynamic)
        completed = self.fixture.run()
        self.assert_diagnostic(
            completed,
            "FRAGMENT_COLLECTION",
            "evidence/manifest/sources.json",
        )

        self.fixture = self._replace_fixture()
        external = Path(self.fixture._temporary.name) / "external-baseline.json"
        external.write_text("[]\n", encoding="utf-8")
        completed = self.fixture.run(arguments=[
            "--root",
            os.fspath(self.fixture.root),
            "--baseline",
            os.fspath(external),
        ])
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def _replace_fixture(self) -> Fixture:
        self.fixture.close()
        return Fixture()

    def test_seed_identity_and_required_golden_diagnostics(self) -> None:
        source_id = str(self.fixture.sources[0]["id"])
        self.fixture.sources[0]["landing_url"] = "https://example.com/changed"
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "SEED_IDENTITY", source_id)

        self.fixture = self._replace_fixture()
        missing = "CLIFF-ORJ5"
        self.fixture.sources.remove(self.fixture.source(missing))
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "REQUIRED_SOURCE", missing)

    def test_assumption_reference_source_assumption_and_derived_join(self) -> None:
        self.fixture.assumptions = [
            _assumption("MISSING-SOURCE", ["missing claim"])
        ]
        completed = self.fixture.run()
        self.assert_diagnostic(
            completed,
            "ASSUMPTION_REFERENCE",
            "assumptions[0].source_ids[0]",
        )

        self.fixture = self._replace_fixture()
        unavailable = _nonseed_source(
            "OPTIONAL-COMPARATIVE", "comparative", status="unavailable"
        )
        self.fixture.add_source(unavailable)
        self.fixture.supplemental = [
            _boundary_row("OPTIONAL-COMPARATIVE", "comparative-only")
        ]
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "SOURCE_ASSUMPTION", "OPTIONAL-COMPARATIVE")

        self.fixture = self._replace_fixture()
        derived = _nonseed_source("SUPPLEMENTAL-DERIVED", "independently-derived")
        self.fixture.add_source(derived)
        self.fixture.supplemental = [
            _boundary_row("SUPPLEMENTAL-DERIVED", "independently-re-derived")
        ]
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "DERIVED_EVIDENCE", "SUPPLEMENTAL-DERIVED")

    def test_unavailable_evidence_reference_has_exact_index_subject(self) -> None:
        unavailable = _nonseed_source(
            "OPTIONAL-DYNAMIC", "dynamic", status="unavailable"
        )
        self.fixture.add_source(unavailable)
        assumption = _assumption(
            "OPTIONAL-DYNAMIC",
            unavailable["claim_scope"],
            evidence_ids=["OPTIONAL-DYNAMIC"],
        )
        self.fixture.assumptions = [assumption]
        completed = self.fixture.run()
        self.assert_diagnostic(
            completed,
            "ASSUMPTION_REFERENCE",
            "assumptions[0].evidence_ids[0]",
        )

    def test_unavailable_artifact_and_exact_open_assumption_are_valid(self) -> None:
        unavailable = self.fixture.source("BOE-GBP-DAILY-FX")
        unavailable["status"] = "unavailable"
        unavailable["artifacts"] = [_external_artifact(unavailable=True)]
        self.fixture.assumptions = [
            _assumption("BOE-GBP-DAILY-FX", unavailable["claim_scope"])
        ]
        completed = self.fixture.run()
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_current_derived_source_cited_by_assumption_is_valid(self) -> None:
        derived = _nonseed_source("SUPPLEMENTAL-DERIVED", "independently-derived")
        self.fixture.add_source(derived)
        self.fixture.assumptions = [
            _assumption(
                "SUPPLEMENTAL-DERIVED",
                derived["claim_scope"],
                evidence_ids=["SUPPLEMENTAL-DERIVED"],
                status="resolved",
            )
        ]
        self.fixture.supplemental = [
            _boundary_row("SUPPLEMENTAL-DERIVED", "independently-re-derived")
        ]
        completed = self.fixture.run()
        self.assertEqual(completed.returncode, 1, completed.stderr)
        self.assertNotIn("DERIVED_EVIDENCE", completed.stderr)

    def test_valid_supersession_chain_is_accepted(self) -> None:
        prior = self.fixture.source("BOE-GBP-DAILY-FX")
        prior["status"] = "superseded"
        prior_artifact = prior["artifacts"][0]
        assert isinstance(prior_artifact, dict)
        prior_artifact["retrieval_status"] = "superseded"
        current = self.fixture.source("CPC-CLIFF-OTJ5")
        current["supersedes"] = "BOE-GBP-DAILY-FX"
        completed = self.fixture.run()
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_clone_boundary_seed_and_supplemental_contracts(self) -> None:
        boundary_path = self.fixture.manifest / "clone-boundary.json"
        boundary = _load_json(boundary_path)
        assert isinstance(boundary, list)
        removed_id = str(boundary.pop(0)["source_id"])
        boundary_path.write_text(json.dumps(boundary), encoding="utf-8")
        completed = self.fixture.run(arguments=[
            "--root",
            os.fspath(self.fixture.root),
            "--baseline",
            "baseline.json",
        ])
        self.assert_diagnostic(completed, "CLONE_BOUNDARY", removed_id)

        self.fixture = self._replace_fixture()
        supplemental = _nonseed_source("SUPPLEMENTAL-GOLDEN", "golden")
        self.fixture.add_source(supplemental)
        completed = self.fixture.run()
        self.assert_diagnostic(
            completed,
            "CLONE_BOUNDARY",
            "evidence/manifest/supplemental-clone-boundary.json",
        )

        self.fixture.supplemental = [
            _boundary_row("SUPPLEMENTAL-GOLDEN", "comparative-only")
        ]
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "CLONE_BOUNDARY", "SUPPLEMENTAL-GOLDEN")

    def test_valid_unresolved_supplemental_boundary_uses_exact_open_assumption(self) -> None:
        source = _nonseed_source("SUPPLEMENTAL-UNRESOLVED", "golden")
        self.fixture.add_source(source)
        self.fixture.assumptions = [
            _assumption("SUPPLEMENTAL-UNRESOLVED", source["claim_scope"])
        ]
        self.fixture.supplemental = [
            _boundary_row("SUPPLEMENTAL-UNRESOLVED", "unresolved")
        ]
        completed = self.fixture.run()
        self.assertEqual(completed.returncode, 1, completed.stderr)
        self.assertNotIn("SOURCE_ASSUMPTION", completed.stderr)

    def test_supplemental_seed_overlap_and_dynamic_presence_are_rejected(self) -> None:
        self.fixture.supplemental = [
            _boundary_row("CLIFF-ORJ5", "official-reuse")
        ]
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "CLONE_BOUNDARY", "CLIFF-ORJ5")

        self.fixture = self._replace_fixture()
        dynamic = _nonseed_source("OPTIONAL-DYNAMIC", "dynamic")
        self.fixture.add_source(dynamic)
        self.fixture.supplemental = [
            _boundary_row("OPTIONAL-DYNAMIC", "comparative-only")
        ]
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "CLONE_BOUNDARY", "OPTIONAL-DYNAMIC")

    def test_artifact_path_missing_nonregular_symlink_escape_and_directory(self) -> None:
        artifact = self.fixture.add_artifact("CLIFF-ORJ5", ".pdf")
        path = self.fixture.root / str(artifact["path"])
        path.unlink()
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "ARTIFACT_PATH", "sources[1].artifacts[0]")

        self.fixture = self._replace_fixture()
        artifact = self.fixture.add_artifact("CLIFF-ORJ5", ".pdf")
        path = self.fixture.root / str(artifact["path"])
        path.unlink()
        path.mkdir()
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "ARTIFACT_PATH", "sources[1].artifacts[0]")

        self.fixture = self._replace_fixture()
        artifact = self.fixture.add_artifact("CLIFF-ORJ5", ".pdf")
        path = self.fixture.root / str(artifact["path"])
        outside = Path(self.fixture._temporary.name) / "outside.pdf"
        outside.write_bytes(MEDIA_FIXTURES[".pdf"][2])
        path.unlink()
        path.symlink_to(outside)
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "ARTIFACT_PATH", "sources[1].artifacts[0]")

        self.fixture = self._replace_fixture()
        artifact = self.fixture.add_artifact("CLIFF-ORJ5", ".pdf")
        old_path = self.fixture.root / str(artifact["path"])
        wrong_relative = "evidence/source-snapshots/CLIFF-ORJ5/artifact.pdf"
        wrong_path = self.fixture.root / wrong_relative
        wrong_path.parent.mkdir(parents=True)
        old_path.replace(wrong_path)
        artifact["path"] = wrong_relative
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "ARTIFACT_PATH", "sources[1].artifacts[0]")

    def test_artifact_storage_bytes_hash_media_signature_attribute_and_pointer(self) -> None:
        artifact = self.fixture.add_artifact(
            "CLIFF-ORJ5", ".pdf", storage="git"
        )
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "ARTIFACT_STORAGE", "sources[1].artifacts[0]")

        self.fixture = self._replace_fixture()
        artifact = self.fixture.add_artifact("CLIFF-ORJ5", ".pdf")
        artifact["bytes"] = int(artifact["bytes"]) + 1
        artifact["sha256"] = "0" * 64
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "ARTIFACT_BYTES", "sources[1].artifacts[0]")
        self.assert_diagnostic(completed, "ARTIFACT_SHA256", "sources[1].artifacts[0]")

        self.fixture = self._replace_fixture()
        self.fixture.add_artifact(
            "CLIFF-ORJ5", ".pdf", media_type="application/octet-stream"
        )
        completed = self.fixture.run()
        self.assert_diagnostic(
            completed, "ARTIFACT_MEDIA_TYPE", "sources[1].artifacts[0]"
        )

        self.fixture = self._replace_fixture()
        self.fixture.add_artifact("CLIFF-ORJ5", ".pdf", payload=b"not a PDF")
        completed = self.fixture.run()
        self.assert_diagnostic(
            completed, "ARTIFACT_SIGNATURE", "sources[1].artifacts[0]"
        )

        self.fixture = self._replace_fixture()
        self.fixture.add_artifact("CLIFF-ORJ5", ".pdf")
        (self.fixture.worktree / ".gitattributes").write_text("", encoding="utf-8")
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "GIT_ATTRIBUTE", "sources[1].artifacts[0]")

        self.fixture = self._replace_fixture()
        pointer = b"version https://git-lfs.github.com/spec/v1\noid sha256:" + b"0" * 64
        self.fixture.add_artifact("CLIFF-ORJ5", ".pdf", payload=pointer)
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "LFS_POINTER", "sources[1].artifacts[0]")

    def test_public_payload_seam_matches_aggregate_media_diagnostics(self) -> None:
        artifact = self.fixture.add_artifact(
            "CLIFF-ORJ5",
            ".pdf",
            payload=b"not a PDF",
            media_type="application/octet-stream",
        )
        direct = validate_artifact_payload(
            self.fixture.root,
            str(artifact["path"]),
            b"not a PDF",
            "application/octet-stream",
            "lfs",
        )
        self.assertEqual(
            [(item.code, item.message) for item in direct],
            [
                (
                    "ARTIFACT_MEDIA_TYPE",
                    "media type must be 'application/pdf' for suffix '.pdf'",
                ),
                ("ARTIFACT_SIGNATURE", "PDF signature is missing"),
            ],
        )
        completed = self.fixture.run()
        aggregate = {
            (line.split(" ", 3)[1], line.split(": ", 1)[1])
            for line in completed.stderr.splitlines()
            if line.startswith("ERROR ARTIFACT_")
        }
        self.assertEqual(aggregate, set((item.code, item.message) for item in direct))

    def test_public_payload_seam_requires_git_placement_without_aggregate(self) -> None:
        relative = "docs/reference/datasheets.mdCLIFF-ORJ5/direct.pdf"
        (self.fixture.worktree / ".gitattributes").write_text("", encoding="utf-8")
        diagnostics = validate_artifact_payload(
            self.fixture.root,
            relative,
            MEDIA_FIXTURES[".pdf"][2],
            "application/pdf",
            "lfs",
        )
        self.assertEqual(
            [(item.code, item.subject) for item in diagnostics],
            [("GIT_ATTRIBUTE", relative)],
        )

    def test_public_null_payload_owns_unreadable_aggregate_media_parity(self) -> None:
        artifact = self.fixture.add_artifact(
            "CLIFF-ORJ5",
            ".pdf",
            media_type="application/octet-stream",
        )
        relative = str(artifact["path"])
        (self.fixture.root / relative).unlink()
        (self.fixture.worktree / ".gitattributes").write_text("", encoding="utf-8")
        direct = validate_artifact_payload(
            self.fixture.root,
            relative,
            None,
            "application/octet-stream",
            "lfs",
        )
        self.assertEqual(
            [(item.code, item.message) for item in direct],
            [
                (
                    "ARTIFACT_MEDIA_TYPE",
                    "media type must be 'application/pdf' for suffix '.pdf'",
                ),
                ("GIT_ATTRIBUTE", "filter is 'unspecified'; expected 'lfs'"),
            ],
        )
        completed = self.fixture.run()
        self.assertEqual(completed.returncode, 1, completed.stderr)
        aggregate = {
            (line.split(" ", 3)[1], line.split(": ", 1)[1])
            for line in completed.stderr.splitlines()
            if line.startswith("ERROR ARTIFACT_MEDIA_TYPE ")
            or line.startswith("ERROR GIT_ATTRIBUTE ")
        }
        self.assertEqual(aggregate, set((item.code, item.message) for item in direct))
        self.assertNotIn("ARTIFACT_SIGNATURE", completed.stderr)

    def test_text_media_rejects_invalid_utf8(self) -> None:
        for index, suffix in enumerate((".txt", ".rst")):
            if index:
                self.fixture = self._replace_fixture()
            with self.subTest(suffix=suffix):
                self.fixture.add_artifact("CLIFF-ORJ5", suffix, payload=b"\xff")
                completed = self.fixture.run()
                self.assert_diagnostic(
                    completed, "ARTIFACT_SIGNATURE", "sources[1].artifacts[0]"
                )

    def test_each_content_check_rejects_an_invalid_payload(self) -> None:
        invalid_payloads = {
            ".pdf": b"not-pdf",
            ".zip": b"not-zip",
            ".png": b"\x89PNG\r\n\x1a",
            ".html": b"plain text",
            ".mhtml": b"MIME-Version: 1.0\r\n",
            ".step": b"ISO-10303-21;\nDATA;",
            ".kicad_sch": b"(wrong)",
            ".kicad_pcb": b"(wrong)",
            ".kicad_sym": b"(wrong)",
            ".kicad_mod": b"(wrong)",
            ".kicad_pro": b"[]",
            ".json": b"{invalid",
            ".csv": b"\n",
            ".c": b"text\x00with-nul",
            ".rst": b"text\x00with-nul",
        }
        for index, (suffix, payload) in enumerate(invalid_payloads.items()):
            if index:
                self.fixture = self._replace_fixture()
            with self.subTest(suffix=suffix):
                self.fixture.add_artifact("CLIFF-ORJ5", suffix, payload=payload)
                completed = self.fixture.run()
                self.assert_diagnostic(
                    completed,
                    "ARTIFACT_SIGNATURE",
                    "sources[1].artifacts[0]",
                )

    def test_unknown_and_uppercase_suffixes_are_rejected(self) -> None:
        for index, suffix in enumerate((".bin", ".PDF")):
            if index:
                self.fixture = self._replace_fixture()
            artifact = self.fixture.add_artifact("CLIFF-ORJ5", ".txt")
            old_path = self.fixture.root / str(artifact["path"])
            new_relative = str(artifact["path"])[: -len(".txt")] + suffix
            new_path = self.fixture.root / new_relative
            old_path.replace(new_path)
            artifact["name"] = f"artifact{suffix}"
            artifact["path"] = new_relative
            with self.subTest(suffix=suffix):
                completed = self.fixture.run()
                self.assert_diagnostic(
                    completed,
                    "ARTIFACT_MEDIA_TYPE",
                    "sources[1].artifacts[0]",
                )

    def test_diagnostics_are_exactly_deduplicated_and_sorted(self) -> None:
        artifact = self.fixture.add_artifact("CLIFF-ORJ5", ".pdf", payload=b"bad")
        artifact["bytes"] = 99
        artifact["sha256"] = "0" * 64
        completed = self.fixture.run()
        self.assertEqual(completed.returncode, 1, completed.stderr)
        lines = completed.stderr.splitlines()
        self.assertEqual(lines, sorted(set(lines)))

    def test_default_root_and_missing_head_path_baseline(self) -> None:
        self.make_passing_multi_artifact_fixture()
        completed = self.fixture.run(
            validator=self.fixture.scripts / "validate_evidence.py",
            arguments=["--baseline", "baseline.json"],
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)

        completed = self.fixture.run(
            explicit_baseline=False,
            validator=self.fixture.scripts / "validate_evidence.py",
            arguments=[],
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_unusable_git_environment_is_exit_two(self) -> None:
        (self.fixture.worktree / ".git").rename(self.fixture.worktree / "git-disabled")
        completed = self.fixture.run(
            explicit_baseline=False,
            arguments=["--root", os.fspath(self.fixture.root)],
        )
        self.assertEqual(completed.returncode, 2, completed.stderr)

    def test_explicit_root_symlink_is_rejected_before_resolution(self) -> None:
        root_link = Path(self.fixture._temporary.name) / "aius-root-link"
        root_link.symlink_to(self.fixture.root, target_is_directory=True)
        completed = self.fixture.run(
            arguments=[
                "--root",
                os.fspath(root_link),
                "--baseline",
                "baseline.json",
            ]
        )
        self.assertEqual(completed.returncode, 2, completed.stderr)
        self.assertIn("Aius root may not be a symlink", completed.stderr)
        with self.assertRaisesRegex(ControlError, "may not be a symlink"):
            validate_artifact_payload(
                root_link,
                "docs/reference/datasheets.mdCLIFF-ORJ5/direct.pdf",
                MEDIA_FIXTURES[".pdf"][2],
                "application/pdf",
                "lfs",
            )

    def test_schema_meta_failure_and_baseline_collection_invariant_subjects(self) -> None:
        schema_path = self.fixture.manifest / "source-schema.json"
        schema = _load_json(schema_path)
        assert isinstance(schema, dict)
        schema["unknown-keyword"] = True
        schema_path.write_text(json.dumps(schema), encoding="utf-8")
        completed = self.fixture.run()
        self.assert_diagnostic(
            completed,
            "MANIFEST_SCHEMA",
            "evidence/manifest/source-schema.json",
        )

        self.fixture = self._replace_fixture()
        self.fixture.baseline = copy.deepcopy(self.fixture.sources[:2])
        assert isinstance(self.fixture.baseline, list)
        self.fixture.baseline.reverse()
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "BASELINE", "baseline")

    def test_relative_baseline_may_not_escape_root(self) -> None:
        external = Path(self.fixture._temporary.name) / "outside.json"
        external.write_text("[]\n", encoding="utf-8")
        completed = self.fixture.run(arguments=[
            "--root",
            os.fspath(self.fixture.root),
            "--baseline",
            "../../../outside.json",
        ])
        self.assertEqual(completed.returncode, 2, completed.stderr)

    def test_explicit_baseline_takes_precedence_over_git_head(self) -> None:
        changed = copy.deepcopy(self.fixture.sources)
        changed[0]["authority"] = "Old Git Authority"
        (self.manifest_git_path()).write_text(
            json.dumps(changed, indent=2) + "\n", encoding="utf-8"
        )
        self.fixture.git("add", "evidence/manifest/sources.json")
        self.fixture.git("commit", "-q", "-m", "add stale baseline")
        completed = self.fixture.run()
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def manifest_git_path(self) -> Path:
        return self.fixture.manifest / "sources.json"


class SupplementalCollectionTests(unittest.TestCase):
    fixture: CompleteFixture

    def setUp(self) -> None:
        self.fixture = CompleteFixture()

    def tearDown(self) -> None:
        self.fixture.close()

    def assert_exact(
        self,
        completed: subprocess.CompletedProcess[str],
        code: str,
        subject: str,
        message: str,
        *,
        exit_code: int = 1,
    ) -> None:
        self.assertEqual(completed.returncode, exit_code, completed.stderr)
        self.assertIn(
            f"ERROR {code} {subject}: {message}",
            completed.stderr.splitlines(),
            completed.stderr,
        )

    def replace_complete_fixture(self) -> CompleteFixture:
        self.fixture.close()
        return CompleteFixture()

    def test_seed_only_and_complete_states_are_accepted(self) -> None:
        seed = Fixture()
        try:
            seed.supplemental = []
            completed = seed.run()
            self.assertEqual(completed.returncode, 0, completed.stderr)
        finally:
            seed.close()
        completed = self.fixture.run()
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_complete_fixture_synthetic_seed_boundary_is_explicit(self) -> None:
        seeds = _load_json(
            COMPLETE_FIXTURE_ROOT / "evidence/manifest/seed-sources.json"
        )
        assert isinstance(seeds, list)
        required_golden = [
            row["id"]
            for row in seeds
            if row.get("class") == "golden" and row.get("required") is not False
        ]
        self.assertEqual(required_golden, [])
        production = _load_json(MANIFEST_ROOT / "seed-sources.json")
        assert isinstance(production, list)
        self.assertEqual(
            sum(
                row.get("class") == "golden" and row.get("required") is True
                for row in production
            ),
            11,
        )
        generator = runpy.run_path(
            os.fspath(
                SCRIPT_ROOT
                / "fixtures"
                / "supplemental-assembly"
                / "generate.py"
            ),
            run_name="aius_fixture_generator_test",
        )
        adapt = generator["_adapt_validator_fixture"]
        with tempfile.TemporaryDirectory(prefix="aius-adapted-fixture-") as temporary:
            adapted_root = Path(temporary) / "aius"
            adapted_manifest = adapted_root / "evidence" / "manifest"
            adapted_manifest.mkdir(parents=True)
            adapt(adapted_root)
            self.assertEqual(
                (adapted_manifest / "clone-boundary.json").read_bytes(),
                (MANIFEST_ROOT / "clone-boundary.json").read_bytes(),
            )
            self.assertEqual(
                (adapted_manifest / "clone-boundary.json").read_bytes(),
                (
                    COMPLETE_FIXTURE_ROOT
                    / "evidence/manifest/clone-boundary.json"
                ).read_bytes(),
            )
            self.assertEqual(
                (adapted_manifest / "seed-sources.json").read_bytes(),
                (
                    COMPLETE_FIXTURE_ROOT
                    / "evidence/manifest/seed-sources.json"
                ).read_bytes(),
            )

    def test_every_complete_activation_signal_requires_complete_members(self) -> None:
        source_id = "AIUS-ETHERNET-MAGJACK-MFR"
        for signal in (
            "supplemental-source-directory",
            "supplemental-assumption-directory",
            "boundary-row",
            "aggregate-source",
            "aggregate-assumption",
        ):
            seed = Fixture()
            try:
                if signal == "supplemental-source-directory":
                    (seed.manifest / "supplemental-fragments").mkdir()
                elif signal == "supplemental-assumption-directory":
                    (seed.manifest / "supplemental-assumption-fragments").mkdir()
                elif signal == "boundary-row":
                    seed.supplemental = [_boundary_row(source_id, "unresolved")]
                elif signal == "aggregate-source":
                    seed.add_source(_nonseed_source(source_id, "golden"))
                else:
                    assumption = _assumption(source_id, ["activation"])
                    assumption["id"] = f"ASM-UNAVAILABLE-{source_id}"
                    seed.assumptions = [assumption]
                completed = seed.run()
                with self.subTest(signal=signal):
                    if signal == "supplemental-source-directory":
                        self.assert_exact(
                            completed,
                            "FRAGMENT_COLLECTION",
                            "evidence/manifest/supplemental-fragments",
                            "fragment IDs differ; missing="
                            + repr(
                                sorted(
                                    row["source_id"]
                                    for row in _load_json(
                                        seed.manifest
                                        / "supplemental-realization.json"
                                    )["realizations"]
                                )
                            )
                            + "; extra=[]",
                        )
                    else:
                        self.assert_exact(
                            completed,
                            "FRAGMENT_COLLECTION",
                            "evidence/manifest/supplemental-fragments",
                            "required fragment directory is missing",
                        )
            finally:
                seed.close()

    def test_four_directory_missing_nonregular_unsafe_and_malformed_matrix(self) -> None:
        directories = (
            "evidence/manifest/fragments",
            "evidence/manifest/assumption-fragments",
            "evidence/manifest/supplemental-fragments",
            "evidence/manifest/supplemental-assumption-fragments",
        )
        for relative in directories:
            for mutation in (
                "missing",
                "non-directory",
                "symlink",
                "unsafe",
                "non-utf8",
                "malformed",
            ):
                self.fixture = self.replace_complete_fixture()
                path = self.fixture.root / relative
                entries = sorted(path.glob("*.json"))
                if mutation == "missing":
                    shutil.rmtree(path)
                elif mutation == "non-directory":
                    shutil.rmtree(path)
                    path.write_text("not a directory\n", encoding="utf-8")
                elif mutation == "symlink":
                    shutil.rmtree(path)
                    target = self.fixture.root / "symlink-target"
                    target.mkdir(exist_ok=True)
                    path.symlink_to(target, target_is_directory=True)
                elif mutation == "unsafe":
                    (path / "unsafe.txt").write_text("{}\n", encoding="utf-8")
                elif mutation == "non-utf8":
                    entries[0].write_bytes(b"\xff")
                else:
                    entries[0].write_text("{", encoding="utf-8")
                completed = self.fixture.run()
                with self.subTest(directory=relative, mutation=mutation):
                    if mutation == "missing":
                        self.assert_exact(
                            completed,
                            "FRAGMENT_COLLECTION",
                            relative,
                            "required fragment directory is missing",
                        )
                    elif mutation in {"non-directory", "symlink"}:
                        self.assert_exact(
                            completed,
                            "FRAGMENT_COLLECTION",
                            relative,
                            "fragment path is not a regular non-symlink directory",
                        )
                    elif mutation == "unsafe":
                        self.assert_exact(
                            completed,
                            "FRAGMENT_COLLECTION",
                            f"{relative}/unsafe.txt",
                            "unexpected or unsafe fragment entry",
                        )
                    elif mutation in {"non-utf8", "malformed"}:
                        self.assertEqual(completed.returncode, 2, completed.stderr)
                        self.assertIn(
                            "is not UTF-8"
                            if mutation == "non-utf8"
                            else "is not valid JSON",
                            completed.stderr,
                        )

    def test_unreadable_fragment_control_bytes_are_exit_two(self) -> None:
        original = validator_module._load_control

        def reject_fragment(path: Path, label: str | None = None) -> object:
            if "/supplemental-fragments/" in path.as_posix():
                raise ControlError(f"cannot read {path}: permission denied")
            return original(path, label)

        stderr = io.StringIO()
        with mock.patch.object(
            validator_module, "_load_control", side_effect=reject_fragment
        ), redirect_stderr(stderr):
            status = validator_module.main(
                [
                    "--root",
                    os.fspath(self.fixture.root),
                    "--baseline",
                    "baseline.json",
                ]
            )
        self.assertEqual(status, 2)
        self.assertIn("permission denied", stderr.getvalue())

    def test_four_directory_membership_and_identity_matrix(self) -> None:
        cases = (
            ("evidence/manifest/fragments", "source"),
            ("evidence/manifest/assumption-fragments", "assumption"),
            ("evidence/manifest/supplemental-fragments", "source"),
            ("evidence/manifest/supplemental-assumption-fragments", "assumption"),
        )
        for relative, kind in cases:
            for mutation in ("missing-entry", "extra-entry", "identity", "non-object"):
                self.fixture = self.replace_complete_fixture()
                directory = self.fixture.root / relative
                first = sorted(directory.glob("*.json"))[0]
                filename_id = first.stem
                if mutation == "missing-entry":
                    first.unlink()
                elif mutation == "extra-entry":
                    shutil.copy2(first, directory / "EXTRA.json")
                elif mutation == "identity":
                    value = _load_json(first)
                    assert isinstance(value, dict)
                    if kind == "source":
                        value["id"] = "MISMATCH"
                    else:
                        value["source_ids"] = ["MISMATCH"]
                    first.write_text(
                        json.dumps(value, indent=2) + "\n", encoding="utf-8"
                    )
                else:
                    first.write_text("[]\n", encoding="utf-8")
                completed = self.fixture.run()
                with self.subTest(directory=relative, mutation=mutation):
                    if mutation == "missing-entry":
                        self.assertIn(
                            f"ERROR FRAGMENT_COLLECTION {relative}: fragment IDs differ; missing=['{filename_id}']; extra=[]",
                            completed.stderr.splitlines(),
                        )
                    elif mutation == "extra-entry":
                        self.assertIn(
                            f"ERROR FRAGMENT_COLLECTION {relative}: fragment IDs differ; missing=[]; extra=['EXTRA']",
                            completed.stderr.splitlines(),
                        )
                    elif kind == "source":
                        self.assert_exact(
                            completed,
                            "FRAGMENT_COLLECTION",
                            f"{relative}/{filename_id}.json",
                            f"filename source ID {filename_id!r} does not match record ID "
                            + ("'MISMATCH'" if mutation == "identity" else "None"),
                        )
                    else:
                        self.assert_exact(
                            completed,
                            "FRAGMENT_COLLECTION",
                            f"{relative}/{filename_id}.json",
                            f"filename source ID {filename_id!r} does not match canonical assumption source "
                            + ("'MISMATCH'" if mutation == "identity" else "None"),
                        )

    def test_fragment_collision_and_both_aggregate_parity_diagnostics(self) -> None:
        seed_source = self.fixture.root / "evidence/manifest/fragments/BOE-GBP-DAILY-FX.json"
        supplemental = self.fixture.root / "evidence/manifest/supplemental-fragments"
        shutil.copy2(seed_source, supplemental / seed_source.name)
        completed = self.fixture.run()
        self.assert_exact(
            completed,
            "FRAGMENT_COLLECTION",
            "BOE-GBP-DAILY-FX",
            "ID appears in both seed and supplemental fragments",
        )

        self.fixture = self.replace_complete_fixture()
        seed_assumption = sorted(
            (
                self.fixture.root
                / "evidence/manifest/assumption-fragments"
            ).glob("*.json")
        )[0]
        supplemental_assumption = sorted(
            (
                self.fixture.root
                / "evidence/manifest/supplemental-assumption-fragments"
            ).glob("*.json")
        )[0]
        seed_value = _load_json(seed_assumption)
        supplemental_value = _load_json(supplemental_assumption)
        assert isinstance(seed_value, dict) and isinstance(supplemental_value, dict)
        supplemental_value["id"] = seed_value["id"]
        supplemental_assumption.write_text(
            json.dumps(supplemental_value, indent=2) + "\n", encoding="utf-8"
        )
        completed = self.fixture.run()
        self.assert_exact(
            completed,
            "FRAGMENT_COLLECTION",
            str(seed_value["id"]),
            "ID appears in both seed and supplemental fragments",
        )

        self.fixture = self.replace_complete_fixture()
        sources = self.fixture.json("evidence/manifest/sources.json")
        assert isinstance(sources, list) and isinstance(sources[0], dict)
        sources[0]["title"] = "aggregate-only mutation"
        self.fixture.write_json("evidence/manifest/sources.json", sources)
        completed = self.fixture.run()
        self.assert_exact(
            completed,
            "FRAGMENT_COLLECTION",
            "evidence/manifest/sources.json",
            "aggregate differs from sorted fragment collection",
        )

        self.fixture = self.replace_complete_fixture()
        assumptions = self.fixture.json("evidence/manifest/assumptions.json")
        assert isinstance(assumptions, list) and isinstance(assumptions[0], dict)
        assumptions[0]["claim"] = "aggregate-only mutation"
        self.fixture.write_json("evidence/manifest/assumptions.json", assumptions)
        completed = self.fixture.run()
        self.assert_exact(
            completed,
            "FRAGMENT_COLLECTION",
            "evidence/manifest/assumptions.json",
            "aggregate differs from sorted fragment collection",
        )

    def test_duplicate_assumption_record_ids_cannot_be_masked_by_aggregate(self) -> None:
        for relative in (
            "evidence/manifest/assumption-fragments",
            "evidence/manifest/supplemental-assumption-fragments",
        ):
            self.fixture = self.replace_complete_fixture()
            directory = self.fixture.root / relative
            first, second = sorted(directory.glob("*.json"))[:2]
            first_value = _load_json(first)
            second_value = _load_json(second)
            assert isinstance(first_value, dict) and isinstance(second_value, dict)
            duplicate_id = str(first_value["id"])
            second_value["id"] = duplicate_id
            second.write_text(
                json.dumps(second_value, indent=2) + "\n", encoding="utf-8"
            )

            all_assumptions: list[dict[str, object]] = []
            for fragment_relative in (
                "evidence/manifest/assumption-fragments",
                "evidence/manifest/supplemental-assumption-fragments",
            ):
                for path in sorted(
                    (self.fixture.root / fragment_relative).glob("*.json")
                ):
                    value = _load_json(path)
                    assert isinstance(value, dict)
                    all_assumptions.append(value)
            all_assumptions.sort(key=lambda row: str(row["id"]))
            self.fixture.write_json(
                "evidence/manifest/assumptions.json", all_assumptions
            )

            completed = self.fixture.run()
            with self.subTest(directory=relative):
                self.assert_exact(
                    completed,
                    "FRAGMENT_COLLECTION",
                    duplicate_id,
                    "record ID appears in more than one fragment",
                )

    def test_current_manufacturer_artifact_binding_matrix(self) -> None:
        source_id = "AIUS-TEENSY-ETHERNET-SOCKET-MFR"
        source = self.fixture.make_current_manufacturer(source_id)
        completed = self.fixture.run()
        self.assertEqual(completed.returncode, 0, completed.stderr)
        artifacts = source["artifacts"]
        assert isinstance(artifacts, list)
        artifact_count = len(artifacts)
        self.assertEqual(artifact_count, 5)
        for index in range(artifact_count):
            for field in ("name", "final_url", "path", "storage", "media_type"):
                self.fixture = self.replace_complete_fixture()
                source = self.fixture.make_current_manufacturer(source_id)
                artifacts = source["artifacts"]
                assert isinstance(artifacts, list) and isinstance(artifacts[index], dict)
                artifacts[index][field] = "wrong"
                self.fixture.write_source(source)
                completed = self.fixture.run()
                with self.subTest(index=index, field=field):
                    self.assert_exact(
                        completed,
                        "REALIZATION_BINDING",
                        source_id,
                        f"artifact[{index}] field {field!r} differs from realization",
                    )

        self.fixture = self.replace_complete_fixture()
        source = self.fixture.make_current_manufacturer(source_id)
        source["artifacts"] = []
        self.fixture.write_source(source)
        completed = self.fixture.run()
        self.assert_exact(
            completed,
            "REALIZATION_BINDING",
            source_id,
            "current artifact count differs from realization",
        )

        self.fixture = self.replace_complete_fixture()
        self.fixture.make_current_manufacturer(source_id)
        document = self.fixture.json("evidence/manifest/supplemental-realization.json")
        assert isinstance(document, dict)
        row = next(
            item for item in document["realizations"] if item["source_id"] == source_id
        )
        row["artifacts"][0]["artifact_kind"] = "wrong-kind"
        self.fixture.write_json(
            "evidence/manifest/supplemental-realization.json", document
        )
        completed = self.fixture.run()
        self.assert_exact(
            completed,
            "REALIZATION_BINDING",
            source_id,
            "ordered artifact kinds differ from request",
        )

    def test_unavailable_binding_exemption_and_payload_hash_size(self) -> None:
        source_id = "AIUS-ETHERNET-MAGJACK-MFR"
        document = self.fixture.json("evidence/manifest/supplemental-realization.json")
        assert isinstance(document, dict)
        row = next(
            item for item in document["realizations"] if item["source_id"] == source_id
        )
        row["artifacts"][0]["name"] = "ignored-while-unavailable"
        row["artifacts"][0]["artifact_kind"] = "ignored-kind"
        self.fixture.write_json(
            "evidence/manifest/supplemental-realization.json", document
        )
        completed = self.fixture.run()
        self.assertEqual(completed.returncode, 0, completed.stderr)

        for field, value in (("bytes", 999), ("sha256", "0" * 64)):
            self.fixture = self.replace_complete_fixture()
            source = self.fixture.make_current_manufacturer(source_id)
            artifacts = source["artifacts"]
            assert isinstance(artifacts, list) and isinstance(artifacts[0], dict)
            artifacts[0][field] = value
            self.fixture.write_source(source)
            completed = self.fixture.run()
            with self.subTest(field=field):
                self.assertEqual(completed.returncode, 1, completed.stderr)
                self.assertIn(
                    "ARTIFACT_BYTES" if field == "bytes" else "ARTIFACT_SHA256",
                    completed.stderr,
                )

    def test_unavailable_and_current_uk_artifact_binding(self) -> None:
        source_id = "AIUS-ETHERNET-MAGJACK-MFR"
        for field in (
            "name",
            "final_url",
            "storage",
            "path",
            "sha256",
            "bytes",
            "media_type",
            "retrieval_status",
            "note",
        ):
            self.fixture = self.replace_complete_fixture()
            source = self.fixture.source(source_id)
            artifacts = source["artifacts"]
            assert isinstance(artifacts, list) and isinstance(artifacts[0], dict)
            artifacts[0][field] = "wrong" if field != "note" else ""
            self.fixture.write_source(source)
            completed = self.fixture.run()
            with self.subTest(field=field):
                self.assert_exact(
                    completed,
                    "REALIZATION_BINDING",
                    source_id,
                    "unavailable source artifact differs from canonical metadata",
                )

        for artifacts_value in ([], [
            _external_artifact(unavailable=True),
            _external_artifact(unavailable=True),
        ]):
            self.fixture = self.replace_complete_fixture()
            source = self.fixture.source(source_id)
            source["artifacts"] = artifacts_value
            self.fixture.write_source(source)
            completed = self.fixture.run()
            with self.subTest(artifact_count=len(artifacts_value)):
                self.assert_exact(
                    completed,
                    "REALIZATION_BINDING",
                    source_id,
                    "unavailable source artifact differs from canonical metadata",
                )

        self.fixture = self.replace_complete_fixture()
        source = self.fixture.source(source_id)
        source["landing_url"] = "https://example.com/wrong-authority"
        artifacts = source["artifacts"]
        assert isinstance(artifacts, list) and isinstance(artifacts[0], dict)
        artifacts[0]["final_url"] = "https://example.com/wrong-authority"
        self.fixture.write_source(source)
        completed = self.fixture.run()
        self.assert_exact(
            completed,
            "REALIZATION_BINDING",
            source_id,
            "unavailable source artifact differs from canonical metadata",
        )

        self.fixture = self.replace_complete_fixture()
        uk_id = "AIUS-CLIFF-RX-UK"
        source = self.fixture.source(uk_id)
        source["status"] = "current"
        self.fixture.write_source(source)
        completed = self.fixture.run()
        self.assert_exact(
            completed,
            "REALIZATION_BINDING",
            uk_id,
            "current UK source must not contain artifacts",
        )
        self.assertIn(
            "ERROR FRAGMENT_COLLECTION evidence/manifest/supplemental-assumption-fragments: "
            f"fragment IDs differ; missing=[]; extra=['{uk_id}']",
            completed.stderr.splitlines(),
        )

    def test_current_uk_without_artifacts_removes_status_derived_assumption(
        self,
    ) -> None:
        source_id = "AIUS-CLIFF-RX-UK"
        realization = self.fixture.realization(source_id)
        requests_document = self.fixture.json(
            "evidence/manifest/supplemental-requests.json"
        )
        realization_document = self.fixture.json(
            "evidence/manifest/supplemental-realization.json"
        )
        assert isinstance(requests_document, dict)
        assert isinstance(realization_document, dict)
        request = next(
            row
            for row in requests_document["requests"]
            if row["source_id"] == source_id
        )
        profile = next(
            row
            for row in realization_document["observation_profiles"]
            if row["name"] == realization["observation_profile"]
        )
        authorization = realization["authorization"]
        assert isinstance(authorization, dict)
        values = {
            "manufacturer_part_number": request["selected_mpn"],
            "supplier_sku": "SKU-REQ-CLIFF-RX-UK",
            "distributor": realization["authority"],
            "product_initial_url": realization["observation_url"],
            "product_final_url": "https://example.com/commerce/final",
            "authorization_authority": authorization["authority"],
            "authorization_method": authorization["method"],
            "authorization_initial_url": authorization["initial_url"],
            "authorization_final_url": "https://example.com/authorization/final",
            "stock_state": "in-stock",
            "stock_quantity": 12,
            "price_quantity": 12,
            "unit_price_ex_vat": 1.25,
            "currency": "GBP",
            "minimum_order_quantity": 1,
            "order_multiple": 1,
            "price_includes_vat": False,
        }
        source = seed_evidence._supplemental_current_uk_source(
            request,
            realization,
            profile,
            values,
            "https://example.com/commerce/final",
            "https://example.com/authorization/final",
            "2026-07-16T12:34:56Z",
        )
        self.assertEqual(source["artifacts"], [])
        self.fixture.write_source(source)

        assumption_path = (
            self.fixture.manifest
            / "supplemental-assumption-fragments"
            / f"{source_id}.json"
        )
        assumption_path.unlink()
        assumptions = self.fixture.json("evidence/manifest/assumptions.json")
        assert isinstance(assumptions, list)
        assumptions = [
            row
            for row in assumptions
            if row.get("id") != f"ASM-UNAVAILABLE-{source_id}"
        ]
        self.fixture.write_json("evidence/manifest/assumptions.json", assumptions)

        completed = self.fixture.run()
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_literal_supplemental_boundary_membership_order_outcome_and_uk(self) -> None:
        relative = "evidence/manifest/supplemental-clone-boundary.json"
        boundary = self.fixture.json(relative)
        assert isinstance(boundary, list)
        removed = str(boundary.pop(0)["source_id"])
        self.fixture.write_json(relative, boundary)
        completed = self.fixture.run()
        self.assertIn(
            f"ERROR CLONE_BOUNDARY {relative}: supplemental boundary source IDs differ; missing=['{removed}']; extra=[]",
            completed.stderr.splitlines(),
        )

        self.fixture = self.replace_complete_fixture()
        boundary = self.fixture.json(relative)
        assert isinstance(boundary, list)
        boundary[0], boundary[1] = boundary[1], boundary[0]
        self.fixture.write_json(relative, boundary)
        completed = self.fixture.run()
        self.assert_exact(
            completed,
            "CLONE_BOUNDARY",
            relative,
            "supplemental boundary source IDs are not sorted",
        )

        self.fixture = self.replace_complete_fixture()
        boundary = self.fixture.json(relative)
        assert isinstance(boundary, list)
        source_id = str(boundary[0]["source_id"])
        boundary[0]["notes"] = "changed"
        self.fixture.write_json(relative, boundary)
        completed = self.fixture.run()
        self.assert_exact(
            completed,
            "CLONE_BOUNDARY",
            source_id,
            "supplemental boundary row differs from realization outcome 'unavailable'",
        )

        self.fixture = self.replace_complete_fixture()
        boundary = self.fixture.json(relative)
        assert isinstance(boundary, list)
        uk_id = "AIUS-CLIFF-RX-UK"
        boundary.append(_boundary_row(uk_id, "unresolved"))
        self.fixture.write_json(relative, boundary)
        completed = self.fixture.run()
        self.assert_exact(
            completed,
            "CLONE_BOUNDARY",
            uk_id,
            "UK source must not appear in supplemental clone boundary",
        )


class SelfTestDispatchTests(unittest.TestCase):
    def test_self_test_dispatches_to_sibling_with_active_python_status(self) -> None:
        with tempfile.TemporaryDirectory(prefix="aius-self-test-") as temporary:
            scripts = Path(temporary) / "scripts"
            scripts.mkdir()
            shutil.copy2(VALIDATOR, scripts / "validate_evidence.py")
            marker = Path(temporary) / "marker.txt"
            test_script = scripts / "test_validate_evidence.py"
            test_script.write_text(
                "from pathlib import Path\n"
                "import sys\n"
                f"Path({os.fspath(marker)!r}).write_text(sys.executable, encoding='utf-8')\n"
                "raise SystemExit(7)\n",
                encoding="utf-8",
            )
            completed = _run(
                [sys.executable, "-B", os.fspath(scripts / "validate_evidence.py"), "--self-test"],
                Path(temporary),
            )
            self.assertEqual(completed.returncode, 7, completed.stderr)
            self.assertEqual(marker.read_text(encoding="utf-8"), sys.executable)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
