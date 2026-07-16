#!/usr/bin/env python3
"""Isolated Git-fixture tests for the Aius evidence validator."""

from __future__ import annotations

import copy
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_ROOT = Path(__file__).resolve().parent
AIUS_ROOT = SCRIPT_ROOT.parent
MANIFEST_ROOT = AIUS_ROOT / "evidence" / "manifest"
VALIDATOR = SCRIPT_ROOT / "validate_evidence.py"

REQUIRED_GOLDEN_IDS = (
    "CLIFF-ORJ5",
    "CLIFF-OTJ5",
    "HAMMOND-1455N1201BK-DRAWING",
    "HAMMOND-1455N1201BK-STEP",
    "PJRC-ETHERNET-KIT",
    "PJRC-IMXRT1060-CEC",
    "PJRC-IMXRT1060-RM",
    "PJRC-TEENSY41-PAGE",
    "PJRC-TEENSY41-PINOUT",
    "PJRC-TEENSY41-SCHEMATIC",
    "PLATFORMIO-TEENSY41",
)

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
        self.sources = [
            _source_from_seed(by_id[source_id]) for source_id in REQUIRED_GOLDEN_IDS
        ]
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
        self.assertEqual(completed.returncode, 0, completed.stderr)

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
        missing = str(self.fixture.sources.pop(0)["id"])
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
        unavailable = _nonseed_source(
            "OPTIONAL-DYNAMIC", "dynamic", status="unavailable"
        )
        self.fixture.add_source(unavailable)
        self.fixture.assumptions = [
            _assumption("OPTIONAL-DYNAMIC", unavailable["claim_scope"])
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
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_valid_supersession_chain_is_accepted(self) -> None:
        prior = _nonseed_source("OPTIONAL-DYNAMIC-OLD", "dynamic")
        prior["status"] = "superseded"
        prior_artifact = prior["artifacts"][0]
        assert isinstance(prior_artifact, dict)
        prior_artifact["retrieval_status"] = "superseded"
        current = _nonseed_source("OPTIONAL-DYNAMIC-NEW", "dynamic")
        current["supersedes"] = "OPTIONAL-DYNAMIC-OLD"
        self.fixture.add_source(current)
        self.fixture.add_source(prior)
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
        self.assertEqual(completed.returncode, 0, completed.stderr)

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
        self.assert_diagnostic(completed, "ARTIFACT_PATH", "sources[0].artifacts[0]")

        self.fixture = self._replace_fixture()
        artifact = self.fixture.add_artifact("CLIFF-ORJ5", ".pdf")
        path = self.fixture.root / str(artifact["path"])
        path.unlink()
        path.mkdir()
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "ARTIFACT_PATH", "sources[0].artifacts[0]")

        self.fixture = self._replace_fixture()
        artifact = self.fixture.add_artifact("CLIFF-ORJ5", ".pdf")
        path = self.fixture.root / str(artifact["path"])
        outside = Path(self.fixture._temporary.name) / "outside.pdf"
        outside.write_bytes(MEDIA_FIXTURES[".pdf"][2])
        path.unlink()
        path.symlink_to(outside)
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "ARTIFACT_PATH", "sources[0].artifacts[0]")

        self.fixture = self._replace_fixture()
        artifact = self.fixture.add_artifact("CLIFF-ORJ5", ".pdf")
        old_path = self.fixture.root / str(artifact["path"])
        wrong_relative = "evidence/source-snapshots/CLIFF-ORJ5/artifact.pdf"
        wrong_path = self.fixture.root / wrong_relative
        wrong_path.parent.mkdir(parents=True)
        old_path.replace(wrong_path)
        artifact["path"] = wrong_relative
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "ARTIFACT_PATH", "sources[0].artifacts[0]")

    def test_artifact_storage_bytes_hash_media_signature_attribute_and_pointer(self) -> None:
        artifact = self.fixture.add_artifact(
            "CLIFF-ORJ5", ".pdf", storage="git"
        )
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "ARTIFACT_STORAGE", "sources[0].artifacts[0]")

        self.fixture = self._replace_fixture()
        artifact = self.fixture.add_artifact("CLIFF-ORJ5", ".pdf")
        artifact["bytes"] = int(artifact["bytes"]) + 1
        artifact["sha256"] = "0" * 64
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "ARTIFACT_BYTES", "sources[0].artifacts[0]")
        self.assert_diagnostic(completed, "ARTIFACT_SHA256", "sources[0].artifacts[0]")

        self.fixture = self._replace_fixture()
        self.fixture.add_artifact(
            "CLIFF-ORJ5", ".pdf", media_type="application/octet-stream"
        )
        completed = self.fixture.run()
        self.assert_diagnostic(
            completed, "ARTIFACT_MEDIA_TYPE", "sources[0].artifacts[0]"
        )

        self.fixture = self._replace_fixture()
        self.fixture.add_artifact("CLIFF-ORJ5", ".pdf", payload=b"not a PDF")
        completed = self.fixture.run()
        self.assert_diagnostic(
            completed, "ARTIFACT_SIGNATURE", "sources[0].artifacts[0]"
        )

        self.fixture = self._replace_fixture()
        self.fixture.add_artifact("CLIFF-ORJ5", ".pdf")
        (self.fixture.worktree / ".gitattributes").write_text("", encoding="utf-8")
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "GIT_ATTRIBUTE", "sources[0].artifacts[0]")

        self.fixture = self._replace_fixture()
        pointer = b"version https://git-lfs.github.com/spec/v1\noid sha256:" + b"0" * 64
        self.fixture.add_artifact("CLIFF-ORJ5", ".pdf", payload=pointer)
        completed = self.fixture.run()
        self.assert_diagnostic(completed, "LFS_POINTER", "sources[0].artifacts[0]")

    def test_text_media_rejects_invalid_utf8(self) -> None:
        for index, suffix in enumerate((".txt", ".rst")):
            if index:
                self.fixture = self._replace_fixture()
            with self.subTest(suffix=suffix):
                self.fixture.add_artifact("CLIFF-ORJ5", suffix, payload=b"\xff")
                completed = self.fixture.run()
                self.assert_diagnostic(
                    completed, "ARTIFACT_SIGNATURE", "sources[0].artifacts[0]"
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
                    "sources[0].artifacts[0]",
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
                    "sources[0].artifacts[0]",
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
