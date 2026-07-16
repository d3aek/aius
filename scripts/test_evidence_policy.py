#!/usr/bin/env python3
"""Executable contract tests for the Aius evidence-handling policy."""

from __future__ import annotations

import copy
import json
import re
import sys
import unittest
from pathlib import Path

from jsonschema_subset import ValidationError, check_schema, validate


AIUS_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_ROOT = AIUS_ROOT / "evidence" / "manifest"


def _load_json(name: str) -> object:
    with (MANIFEST_ROOT / name).open(encoding="utf-8") as handle:
        return json.load(handle)


CLONE_BOUNDARY = _load_json("clone-boundary.json")
SEED_SOURCES = _load_json("seed-sources.json")
README = (MANIFEST_ROOT / "README.md").read_text(encoding="utf-8")

CLONE_BOUNDARY_SCHEMA = {
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://upstream repository.local/aius/clone-boundary.schema.json",
    "$defs": {
        "nonEmptyString": {
            "type": "string",
            "pattern": r"\S",
        },
        "uniqueStringList": {
            "type": "array",
            "items": {"$ref": "#/$defs/nonEmptyString"},
            "uniqueItems": True,
        },
        "row": {
            "type": "object",
            "required": [
                "source_id",
                "classification",
                "reused_material",
                "derived_claims",
                "prohibited_use",
                "notes",
            ],
            "properties": {
                "source_id": {
                    "type": "string",
                    "pattern": r"^[A-Z0-9]+(?:-[A-Z0-9]+)*$",
                },
                "classification": {
                    "enum": [
                        "official-reuse",
                        "independently-re-derived",
                        "comparative-only",
                        "unresolved",
                    ]
                },
                "reused_material": {
                    "$ref": "#/$defs/uniqueStringList",
                },
                "derived_claims": {
                    "$ref": "#/$defs/uniqueStringList",
                    "minItems": 1,
                },
                "prohibited_use": {
                    "$ref": "#/$defs/uniqueStringList",
                    "minItems": 1,
                },
                "notes": {"type": "string"},
            },
            "additionalProperties": False,
            "allOf": [
                {
                    "if": {
                        "properties": {
                            "classification": {"const": "official-reuse"}
                        },
                        "required": ["classification"],
                    },
                    "then": {
                        "properties": {
                            "reused_material": {"minItems": 1}
                        }
                    },
                },
                {
                    "if": {
                        "properties": {
                            "classification": {"const": "comparative-only"}
                        },
                        "required": ["classification"],
                    },
                    "then": {
                        "properties": {
                            "reused_material": {"maxItems": 0}
                        }
                    },
                },
            ],
        },
    },
    "type": "array",
    "items": {"$ref": "#/$defs/row"},
    "minItems": 15,
    "maxItems": 15,
    "uniqueItems": True,
}

CLASSIFICATION_ORACLE = (
    ("CLIFF-ORJ5", "official-reuse"),
    ("CLIFF-OTJ5", "official-reuse"),
    ("HAMMOND-1455N1201BK-DRAWING", "official-reuse"),
    ("HAMMOND-1455N1201BK-STEP", "official-reuse"),
    ("PJRC-ETHERNET-KIT", "official-reuse"),
    ("PJRC-IMXRT1060-CEC", "official-reuse"),
    ("PJRC-IMXRT1060-RM", "official-reuse"),
    ("PJRC-SPDIF-RX-SOURCE", "comparative-only"),
    ("PJRC-SPDIF-TX-SOURCE", "comparative-only"),
    ("PJRC-TEENSY41-PAGE", "official-reuse"),
    ("PJRC-TEENSY41-PINOUT", "official-reuse"),
    ("PJRC-TEENSY41-SCHEMATIC", "official-reuse"),
    ("PLATFORMIO-TEENSY41", "official-reuse"),
    ("VKSDR-TOSLINK-KICAD", "comparative-only"),
    ("VKSDR-TOSLINK-SCHEMATICS", "comparative-only"),
)

EXPECTED_H2_HEADINGS = (
    "Directory boundary",
    "Retrieval workflow",
    "External-reference workflow",
    "Hash and media validation",
    "Git LFS verification",
    "Supersession",
    "Failure recording",
    "Runnable validation",
    "Future validation",
)

POLICY_STATEMENTS = (
    "**Binary evidence:** Store redistributable binary evidence under "
    "`docs/reference/datasheets.md<source-id>/`; Git LFS must report `filter=lfs`.",
    "**Reviewable source snapshots:** Store source snapshots under "
    "`evidence/source-snapshots/<source-id>/` as normal Git text.",
    "**External references:** Do not commit non-redistributable bytes; record "
    "`storage: external-reference` with a null path.",
    "**Text contracts:** Keep schemas, manifests, observations, scripts, "
    "Markdown, CSV, JSON, KiCad sources, and patches in normal Git.",
)

RUNNABLE_COMMANDS = "\n".join(
    (
        "python3 scripts/test_source_schema.py",
        "python3 scripts/test_selection_schemas.py",
        "python3 scripts/test_evidence_policy.py",
    )
)
FUTURE_COMMAND = "python3 scripts/validate_evidence.py"
EXPECTED_RUNNABLE_SECTION = (
    "Run all policy contract tests from the repository root:\n\n"
    f"```bash\n{RUNNABLE_COMMANDS}\n```"
)
EXPECTED_FUTURE_SECTION = (
    "The following command is unavailable until P1A4:\n\n"
    f"```bash\n{FUTURE_COMMAND}\n```"
)


def _classification_pairs(boundary: list[object]) -> tuple[tuple[object, object], ...]:
    return tuple(
        (row.get("source_id"), row.get("classification"))
        if isinstance(row, dict)
        else (None, None)
        for row in boundary
    )


def _expected_pairs_from_seeds(seeds: list[object]) -> tuple[tuple[object, str], ...]:
    mapping = {
        "golden": "official-reuse",
        "comparative": "comparative-only",
        "independently-derived": "independently-re-derived",
    }
    pairs: list[tuple[object, str]] = []
    for seed in seeds:
        if not isinstance(seed, dict) or seed.get("class") == "dynamic":
            continue
        seed_class = seed.get("class")
        if seed_class not in mapping:
            raise AssertionError(f"non-dynamic seed has unsupported class {seed_class!r}")
        pairs.append((seed.get("id"), mapping[seed_class]))
    return tuple(pairs)


def validate_clone_boundary(boundary: object, seeds: object) -> None:
    """Validate the embedded schema, exact oracle, ordering, and seed agreement."""

    validate(boundary, CLONE_BOUNDARY_SCHEMA)
    if not isinstance(boundary, list) or not isinstance(seeds, list):
        raise AssertionError("clone boundary and seed sources must be arrays")

    pairs = _classification_pairs(boundary)
    source_ids = tuple(pair[0] for pair in pairs)
    if source_ids != tuple(sorted(source_ids)):
        raise AssertionError("clone-boundary source IDs are not sorted")
    if len(source_ids) != len(set(source_ids)):
        raise AssertionError("clone-boundary source IDs are not unique")
    if pairs != CLASSIFICATION_ORACLE:
        raise AssertionError(
            f"clone-boundary oracle mismatch: expected {CLASSIFICATION_ORACLE!r}, got {pairs!r}"
        )

    seed_pairs = _expected_pairs_from_seeds(seeds)
    if pairs != seed_pairs:
        raise AssertionError(
            f"non-dynamic seed coverage mismatch: expected {seed_pairs!r}, got {pairs!r}"
        )


def _readme_sections(markdown: str) -> tuple[tuple[str, ...], dict[str, str]]:
    matches = list(re.finditer(r"^## (.+)$", markdown, flags=re.MULTILINE))
    headings = tuple(match.group(1) for match in matches)
    sections: dict[str, str] = {}
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(markdown)
        sections[match.group(1)] = markdown[match.end() : end].strip()
    return headings, sections


class CloneBoundaryContractTests(unittest.TestCase):
    def assert_schema_invalid(self, boundary: object) -> None:
        with self.assertRaises(ValidationError):
            validate(boundary, CLONE_BOUNDARY_SCHEMA)

    def test_embedded_schema_passes_the_subset_meta_check(self) -> None:
        check_schema(CLONE_BOUNDARY_SCHEMA)

    def test_committed_boundary_matches_schema_oracle_and_seed_classes(self) -> None:
        validate_clone_boundary(CLONE_BOUNDARY, SEED_SOURCES)

    def test_every_row_field_is_required_and_additional_fields_are_rejected(self) -> None:
        self.assertIsInstance(CLONE_BOUNDARY, list)
        row = CLONE_BOUNDARY[0]
        self.assertIsInstance(row, dict)
        for field in row:
            boundary = copy.deepcopy(CLONE_BOUNDARY)
            del boundary[0][field]
            with self.subTest(field=field, case="missing"):
                self.assert_schema_invalid(boundary)

        boundary = copy.deepcopy(CLONE_BOUNDARY)
        boundary[0]["unexpected"] = True
        self.assert_schema_invalid(boundary)

    def test_list_fields_reject_wrong_types_empty_items_and_duplicates(self) -> None:
        for field in ("reused_material", "derived_claims", "prohibited_use"):
            for value in ("not-an-array", [""], ["same", "same"]):
                boundary = copy.deepcopy(CLONE_BOUNDARY)
                boundary[0][field] = value
                with self.subTest(field=field, value=value):
                    self.assert_schema_invalid(boundary)

        for field in ("derived_claims", "prohibited_use"):
            boundary = copy.deepcopy(CLONE_BOUNDARY)
            boundary[0][field] = []
            with self.subTest(field=field, case="required-non-empty"):
                self.assert_schema_invalid(boundary)

    def test_classification_invariants_and_enum_are_enforced(self) -> None:
        official = copy.deepcopy(CLONE_BOUNDARY)
        official[0]["reused_material"] = []
        self.assert_schema_invalid(official)

        comparative_index = next(
            index
            for index, row in enumerate(CLONE_BOUNDARY)
            if row["classification"] == "comparative-only"
        )
        comparative = copy.deepcopy(CLONE_BOUNDARY)
        comparative[comparative_index]["reused_material"] = ["Forbidden reuse"]
        self.assert_schema_invalid(comparative)

        unknown = copy.deepcopy(CLONE_BOUNDARY)
        unknown[0]["classification"] = "copied"
        self.assert_schema_invalid(unknown)

    def test_oracle_rejects_reserved_classifications_reordering_and_duplicate_ids(self) -> None:
        reserved = copy.deepcopy(CLONE_BOUNDARY)
        reserved[0]["classification"] = "independently-re-derived"
        reordered = copy.deepcopy(CLONE_BOUNDARY)
        reordered[0], reordered[1] = reordered[1], reordered[0]
        duplicate_id = copy.deepcopy(CLONE_BOUNDARY)
        duplicate_id[1]["source_id"] = duplicate_id[0]["source_id"]

        for boundary in (reserved, reordered, duplicate_id):
            with self.subTest(boundary=boundary), self.assertRaises(AssertionError):
                validate_clone_boundary(boundary, SEED_SOURCES)

    def test_seed_agreement_rejects_class_and_coverage_drift(self) -> None:
        changed_class = copy.deepcopy(SEED_SOURCES)
        next(seed for seed in changed_class if seed["id"] == "CLIFF-ORJ5")[
            "class"
        ] = "comparative"
        changed_dynamic = copy.deepcopy(SEED_SOURCES)
        next(seed for seed in changed_dynamic if seed["id"] == "BOE-GBP-DAILY-FX")[
            "class"
        ] = "golden"

        for seeds in (changed_class, changed_dynamic):
            with self.subTest(seeds=seeds), self.assertRaises(AssertionError):
                validate_clone_boundary(CLONE_BOUNDARY, seeds)


class ReadmeContractTests(unittest.TestCase):
    def test_h2_headings_are_exact_and_ordered(self) -> None:
        headings, _ = _readme_sections(README)
        self.assertEqual(headings, EXPECTED_H2_HEADINGS)

    def test_directory_policy_statements_are_exact(self) -> None:
        _, sections = _readme_sections(README)
        statements = tuple(
            line for line in sections["Directory boundary"].splitlines() if line.startswith("**")
        )
        self.assertEqual(statements, POLICY_STATEMENTS)
        for statement in POLICY_STATEMENTS:
            self.assertEqual(README.count(statement), 1)

    def test_runnable_validation_section_is_exact(self) -> None:
        _, sections = _readme_sections(README)
        self.assertEqual(sections["Runnable validation"], EXPECTED_RUNNABLE_SECTION)

    def test_future_validation_section_is_exact_and_unavailable_until_p1a4(self) -> None:
        _, sections = _readme_sections(README)
        self.assertEqual(sections["Future validation"], EXPECTED_FUTURE_SECTION)

    def test_only_the_contract_command_blocks_are_present(self) -> None:
        command_blocks = tuple(
            match.group(1)
            for match in re.finditer(r"```bash\n(.*?)\n```", README, flags=re.DOTALL)
        )
        self.assertEqual(command_blocks, (RUNNABLE_COMMANDS, FUTURE_COMMAND))


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
