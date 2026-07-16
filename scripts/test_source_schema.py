#!/usr/bin/env python3
"""Executable contract tests and reusable cross-record checks for Aius sources."""

from __future__ import annotations

import copy
import json
import sys
import unittest
from pathlib import Path

from jsonschema_subset import SchemaError, ValidationError, check_schema, validate


AIUS_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_ROOT = AIUS_ROOT / "evidence" / "manifest"


def _load_json(name: str) -> object:
    with (MANIFEST_ROOT / name).open(encoding="utf-8") as handle:
        return json.load(handle)


SOURCE_SCHEMA = _load_json("source-schema.json")
SEED_SOURCES = _load_json("seed-sources.json")
SOURCES = _load_json("sources.json")
SEED_SCHEMA = SOURCE_SCHEMA["$defs"]["seedCollection"]

EXPECTED_SEED_TUPLES = (
    (
        "BOE-GBP-DAILY-FX",
        "dynamic",
        "Bank of England",
        "https://www.bankofengland.co.uk/boeapps/database/Rates.asp?into=GBP",
    ),
    (
        "CLIFF-ORJ5",
        "golden",
        "Cliff Electronic Components",
        "https://www.cliffuk.co.uk/products/optical/FCR684205R.pdf",
    ),
    (
        "CLIFF-OTJ5",
        "golden",
        "Cliff Electronic Components",
        "https://www.cliffuk.co.uk/products/optical/FCR684205T.pdf",
    ),
    (
        "CPC-CLIFF-OTJ5",
        "dynamic",
        "CPC Farnell",
        "https://cpc.farnell.com/cliff-electronic-components/fcr684205t/optical-jack-transmitter-otj-5/dp/CN20715",
    ),
    (
        "DIGIKEY-CLIFF-ORJ5",
        "dynamic",
        "DigiKey UK",
        "https://www.digikey.co.uk/en/products/detail/cliff-electronic-components-ltd/FCR684205R/15283094",
    ),
    (
        "DIGIKEY-TEENSY41",
        "dynamic",
        "DigiKey UK",
        "https://www.digikey.co.uk/en/products/filter/evaluation-boards/embedded-mcu-dsp-evaluation-boards/teensy-4-1/786",
    ),
    (
        "FARNELL-1455N1201BK",
        "dynamic",
        "Farnell UK",
        "https://uk.farnell.com/hammond/1455n1201bk/box-black-aluminium-end-plate/dp/9287876",
    ),
    (
        "HAMMOND-1455N1201BK-DRAWING",
        "golden",
        "Hammond Manufacturing",
        "https://www.hammfg.com/files/parts/pdf/1455N1201BK.pdf",
    ),
    (
        "HAMMOND-1455N1201BK-STEP",
        "golden",
        "Hammond Manufacturing",
        "https://www.hammfg.com/part/1455N1201BK",
    ),
    (
        "PCBWAY-2L-100",
        "dynamic",
        "PCBWay",
        "https://www.pcbway.com/pcb_prototype/2_Layer_pcb/100x100mm.html",
    ),
    (
        "PCBWAY-4L-100",
        "dynamic",
        "PCBWay",
        "https://www.pcbway.com/pcb_prototype/4_Layer_pcb/100x100mm.html",
    ),
    (
        "PJRC-ETHERNET-KIT",
        "golden",
        "PJRC",
        "https://media.digikey.com/pdf/Data%20Sheets/Sparkfun%20PDFs/Ethernet_Kit_for_Teensy_4.1_Web.pdf",
    ),
    (
        "PJRC-IMXRT1060-CEC",
        "golden",
        "PJRC",
        "https://www.pjrc.com/teensy/IMXRT1060CEC_rev4.pdf",
    ),
    (
        "PJRC-IMXRT1060-RM",
        "golden",
        "PJRC",
        "https://www.pjrc.com/teensy/IMXRT1060RM_rev3_annotations.pdf",
    ),
    (
        "PJRC-SPDIF-RX-SOURCE",
        "comparative",
        "PJRC",
        "https://github.com/PaulStoffregen/Audio/blob/master/input_spdif3.cpp",
    ),
    (
        "PJRC-SPDIF-TX-SOURCE",
        "comparative",
        "PJRC",
        "https://github.com/PaulStoffregen/Audio/blob/master/output_spdif3.cpp",
    ),
    (
        "PJRC-TEENSY41-PAGE",
        "golden",
        "PJRC",
        "https://www.pjrc.com/store/teensy41.html",
    ),
    (
        "PJRC-TEENSY41-PINOUT",
        "golden",
        "PJRC",
        "https://www.pjrc.com/teensy/card11b_rev4_web.pdf",
    ),
    (
        "PJRC-TEENSY41-SCHEMATIC",
        "golden",
        "PJRC",
        "https://www.pjrc.com/store/teensy41.html",
    ),
    (
        "PLATFORMIO-TEENSY41",
        "golden",
        "PlatformIO",
        "https://docs.platformio.org/en/latest/boards/teensy/teensy41.html",
    ),
    (
        "VKSDR-TOSLINK-KICAD",
        "comparative",
        "VKSDR",
        "https://vksdr.com/pmod/",
    ),
    (
        "VKSDR-TOSLINK-SCHEMATICS",
        "comparative",
        "VKSDR",
        "https://vksdr.com/pmod/",
    ),
)


class SourceCollectionError(ValueError):
    """One or more deterministic cross-record checks failed."""

    def __init__(self, errors: list[str]) -> None:
        self.errors = tuple(errors)
        super().__init__("\n".join(errors))


def validate_source_collection(sources: object, seeds: object) -> None:
    """Validate sorted IDs, uniqueness, artifact names, and supersession edges."""

    errors: list[str] = []
    source_ids = _validate_collection_ids(sources, "sources", errors)
    _validate_collection_ids(seeds, "seeds", errors)

    if not isinstance(sources, list):
        raise SourceCollectionError(errors)

    by_id: dict[str, dict[str, object]] = {}
    edges: dict[str, str] = {}
    for index, source in enumerate(sources):
        if not isinstance(source, dict):
            continue
        source_id = source.get("id")
        if isinstance(source_id, str) and source_id not in by_id:
            by_id[source_id] = source

        artifacts = source.get("artifacts")
        if isinstance(artifacts, list):
            names: set[str] = set()
            for artifact in artifacts:
                name = artifact.get("name") if isinstance(artifact, dict) else None
                if isinstance(name, str):
                    if name in names:
                        errors.append(
                            f"sources[{index}] {source_id!r}: duplicate artifact name {name!r}"
                        )
                    names.add(name)

        supersedes = source.get("supersedes")
        if isinstance(source_id, str) and isinstance(supersedes, str):
            edges[source_id] = supersedes

    for source_id in source_ids:
        source = by_id.get(source_id)
        if source is None:
            continue
        supersedes = source.get("supersedes")
        if not isinstance(supersedes, str):
            continue
        prior = by_id.get(supersedes)
        if prior is None:
            errors.append(f"source {source_id!r}: supersedes unknown source {supersedes!r}")
        elif prior.get("status") != "superseded":
            errors.append(
                f"source {source_id!r}: superseded source {supersedes!r} is not status 'superseded'"
            )

    _validate_supersession_cycles(edges, errors)
    if errors:
        raise SourceCollectionError(errors)


def assert_seed_oracle(seeds: object) -> None:
    """Require the exact ordered immutable tuples from the normative seed matrix."""

    if not isinstance(seeds, list):
        raise AssertionError("seed source collection is not an array")
    actual = tuple(
        (
            seed.get("id"),
            seed.get("class"),
            seed.get("authority"),
            seed.get("landing_url"),
        )
        if isinstance(seed, dict)
        else (None, None, None, None)
        for seed in seeds
    )
    if actual != EXPECTED_SEED_TUPLES:
        for index in range(max(len(actual), len(EXPECTED_SEED_TUPLES))):
            expected = EXPECTED_SEED_TUPLES[index] if index < len(EXPECTED_SEED_TUPLES) else None
            observed = actual[index] if index < len(actual) else None
            if observed != expected:
                raise AssertionError(
                    f"seed tuple mismatch at index {index}: expected {expected!r}, got {observed!r}"
                )


def _validate_collection_ids(collection: object, label: str, errors: list[str]) -> list[str]:
    if not isinstance(collection, list):
        errors.append(f"{label}: collection must be an array")
        return []

    ids: list[str] = []
    for index, record in enumerate(collection):
        source_id = record.get("id") if isinstance(record, dict) else None
        if not isinstance(source_id, str):
            errors.append(f"{label}[{index}]: record has no string id")
        else:
            ids.append(source_id)

    if len(ids) == len(collection) and ids != sorted(ids):
        errors.append(f"{label}: IDs are not sorted")

    seen: set[str] = set()
    for source_id in ids:
        if source_id in seen:
            errors.append(f"{label}: duplicate ID {source_id!r}")
        seen.add(source_id)
    return ids


def _validate_supersession_cycles(edges: dict[str, str], errors: list[str]) -> None:
    completed: set[str] = set()
    reported: set[frozenset[str]] = set()
    for start in sorted(edges):
        trail: list[str] = []
        positions: dict[str, int] = {}
        current = start
        while current in edges and current not in completed:
            if current in positions:
                cycle = trail[positions[current] :] + [current]
                identity = frozenset(cycle[:-1])
                if identity not in reported:
                    errors.append(f"supersession cycle: {' -> '.join(cycle)}")
                    reported.add(identity)
                break
            positions[current] = len(trail)
            trail.append(current)
            current = edges[current]
        completed.update(trail)


def _artifact(
    *,
    name: str = "datasheet.pdf",
    storage: str = "lfs",
    retrieval_status: str = "current",
) -> dict[str, object]:
    if retrieval_status == "unavailable":
        return {
            "name": name,
            "final_url": "https://example.com/datasheet.pdf",
            "storage": "external-reference",
            "path": None,
            "sha256": None,
            "bytes": None,
            "media_type": None,
            "retrieval_status": "unavailable",
            "note": "The authority endpoint did not return the required object.",
        }
    if storage == "external-reference":
        return {
            "name": name,
            "final_url": "https://example.com/datasheet.pdf",
            "storage": storage,
            "path": None,
            "sha256": None,
            "bytes": None,
            "media_type": "application/pdf",
            "retrieval_status": retrieval_status,
            "note": "Inspected without redistribution permission.",
        }
    return {
        "name": name,
        "final_url": "https://example.com/datasheet.pdf",
        "storage": storage,
        "path": "docs/reference/datasheets.mdEXAMPLE/datasheet.pdf",
        "sha256": "a" * 64,
        "bytes": 1234,
        "media_type": "application/pdf",
        "retrieval_status": retrieval_status,
        "note": "Redistribution permitted for this fixture.",
    }


def _observation(*, key: str = "price", value: object = 12.34) -> dict[str, object]:
    return {
        "key": key,
        "value": value,
        "unit": "GBP",
        "basis": "Single-unit price excluding delivery and VAT.",
        "observed_at": "2026-07-15T12:34:56Z",
        "source_url": "https://example.com/price",
    }


def _source(
    source_id: str = "EXAMPLE-SOURCE",
    *,
    status: str = "current",
    artifact: dict[str, object] | None = None,
    observations: list[dict[str, object]] | None = None,
    supersedes: str | None = None,
) -> dict[str, object]:
    selected_artifact = _artifact() if artifact is None else artifact
    return {
        "id": source_id,
        "class": "golden",
        "title": "Example source",
        "authority": "Example Authority",
        "revision": "1",
        "purpose": "Exercise the source contract.",
        "claim_scope": ["fixture claim"],
        "landing_url": "https://example.com/source",
        "retrieved_at": "2026-07-15T12:34:56Z",
        "status": status,
        "supersedes": supersedes,
        "redistribution_note": "Fixture content may be redistributed.",
        "artifacts": [selected_artifact] if selected_artifact else [],
        "observations": [] if observations is None else observations,
    }


class SchemaEvaluatorTests(unittest.TestCase):
    def test_committed_schema_meta_check(self) -> None:
        check_schema(SOURCE_SCHEMA)

    def test_unknown_keywords_are_rejected_at_all_schema_levels(self) -> None:
        for schema in (
            {"unknown": True},
            {"properties": {"value": {"description": "not in the subset"}}},
            {"$defs": {"value": {"minLength": 1}}},
        ):
            with self.subTest(schema=schema), self.assertRaisesRegex(
                SchemaError, "unrecognized schema keyword"
            ):
                check_schema(schema)

    def test_contains_minimum_and_maximum(self) -> None:
        schema = {
            "type": "array",
            "contains": {"type": "integer", "minimum": 2},
            "minContains": 1,
            "maxContains": 2,
        }
        validate([1, 2, 3], schema)
        for invalid in ([1], [2, 3, 4]):
            with self.subTest(invalid=invalid), self.assertRaises(ValidationError):
                validate(invalid, schema)

    def test_unsatisfiable_bounds_are_schema_valid_and_instance_invalid(self) -> None:
        schemas = (
            {"type": "array", "minItems": 2, "maxItems": 1},
            {
                "type": "array",
                "contains": {"const": "match"},
                "minContains": 2,
                "maxContains": 1,
            },
        )
        for schema in schemas:
            with self.subTest(schema=schema):
                check_schema(schema)
                with self.assertRaises(ValidationError):
                    validate(["match"], schema)

    def test_malformed_uris_are_validation_errors(self) -> None:
        schema = {"type": "string", "format": "uri"}
        for uri in ("https://[malformed", "https://example.com/%zz"):
            with self.subTest(uri=uri), self.assertRaises(ValidationError):
                validate(uri, schema)

    def test_arbitrarily_large_integers_remain_json_numbers(self) -> None:
        huge = 10**10000
        validate(huge, {"type": "integer"})
        validate(huge, {"type": "number", "minimum": huge - 1})
        with self.assertRaises(ValidationError):
            validate(huge, {"type": "integer", "minimum": huge + 1})

    def test_if_then_else_valid_and_invalid_paths(self) -> None:
        schema = {
            "type": "object",
            "required": ["kind", "value"],
            "properties": {
                "kind": {"enum": ["number", "text"]},
                "value": {},
            },
            "additionalProperties": False,
            "if": {
                "properties": {"kind": {"const": "number"}},
                "required": ["kind"],
            },
            "then": {"properties": {"value": {"type": "integer"}}},
            "else": {"properties": {"value": {"type": "string"}}},
        }
        validate({"kind": "number", "value": 3}, schema)
        validate({"kind": "text", "value": "three"}, schema)
        for invalid in (
            {"kind": "number", "value": "three"},
            {"kind": "text", "value": 3},
        ):
            with self.subTest(invalid=invalid), self.assertRaises(ValidationError):
                validate(invalid, schema)

    def test_boolean_schema_and_schema_additional_properties(self) -> None:
        validate("anything", True)
        with self.assertRaises(ValidationError):
            validate("anything", False)
        additional_schema = {
            "properties": {"known": {"type": "integer"}},
            "additionalProperties": {"type": "string"},
        }
        validate({"known": 1, "extra": "text"}, additional_schema)
        with self.assertRaises(ValidationError):
            validate({"known": 1, "extra": 2}, additional_schema)


class SourceSchemaTests(unittest.TestCase):
    def assert_source_valid(self, source: dict[str, object]) -> None:
        validate([source], SOURCE_SCHEMA)

    def assert_source_invalid(self, source: dict[str, object]) -> None:
        with self.assertRaises(ValidationError):
            validate([source], SOURCE_SCHEMA)

    def test_empty_committed_sources_collection(self) -> None:
        self.assertEqual(SOURCES, [])
        validate(SOURCES, SOURCE_SCHEMA)
        validate_source_collection(SOURCES, SEED_SOURCES)

    def test_valid_local_external_observation_superseded_and_unavailable_records(self) -> None:
        external_without_digest = _artifact(storage="external-reference")
        external_with_digest = copy.deepcopy(external_without_digest)
        external_with_digest["sha256"] = "b" * 64
        external_with_digest["bytes"] = 42
        observation_only = _source(artifact={}, observations=[_observation()])
        observation_only["artifacts"] = []
        superseded = _source(
            status="superseded",
            artifact=_artifact(retrieval_status="superseded"),
        )
        unavailable = _source(
            status="unavailable",
            artifact=_artifact(retrieval_status="unavailable"),
        )

        for source in (
            _source(),
            _source(artifact=_artifact(storage="git")),
            _source(artifact=external_without_digest),
            _source(artifact=external_with_digest),
            observation_only,
            superseded,
            unavailable,
        ):
            with self.subTest(source=source):
                self.assert_source_valid(source)

    def test_required_fields_enums_strings_arrays_urls_and_timestamps(self) -> None:
        mutations = []
        missing = _source()
        del missing["title"]
        mutations.append(missing)
        extra = _source()
        extra["unexpected"] = True
        mutations.append(extra)
        for field, value in (
            ("id", "lowercase-id"),
            ("class", "blog"),
            ("status", "pending"),
            ("title", ""),
            ("authority", "   "),
            ("revision", ""),
            ("purpose", ""),
            ("redistribution_note", ""),
            ("claim_scope", []),
            ("claim_scope", ["same", "same"]),
            ("claim_scope", [""]),
            ("landing_url", "http://example.com/source"),
            ("retrieved_at", "2026-07-15T12:34Z"),
            ("retrieved_at", "2026-02-30T12:34:56Z"),
        ):
            mutated = _source()
            mutated[field] = value
            mutations.append(mutated)
        no_evidence = _source(artifact={})
        no_evidence["artifacts"] = []
        mutations.append(no_evidence)
        wrong_supersedes = _source(status="superseded", supersedes="OLD-SOURCE")
        wrong_supersedes["artifacts"][0]["retrieval_status"] = "superseded"
        mutations.append(wrong_supersedes)

        for source in mutations:
            with self.subTest(source=source):
                self.assert_source_invalid(source)

    def test_artifact_local_storage_rules(self) -> None:
        mutations = []
        extra = _artifact()
        extra["unexpected"] = True
        mutations.append(extra)
        for field, value in (
            ("name", ""),
            ("final_url", None),
            ("final_url", "http://example.com/file.pdf"),
            ("path", None),
            ("path", "/absolute/file.pdf"),
            ("path", "../outside.pdf"),
            ("sha256", None),
            ("sha256", "A" * 64),
            ("bytes", None),
            ("bytes", 0),
            ("media_type", None),
            ("media_type", ""),
        ):
            artifact = _artifact()
            artifact[field] = value
            mutations.append(artifact)

        for artifact in mutations:
            with self.subTest(artifact=artifact):
                self.assert_source_invalid(_source(artifact=artifact))

    def test_external_reference_digest_pair_and_path_rules(self) -> None:
        mutations = []
        for field, value in (
            ("path", "docs/reference/datasheets.mdEXAMPLE/file.pdf"),
            ("sha256", "a" * 64),
            ("bytes", 10),
        ):
            artifact = _artifact(storage="external-reference")
            artifact[field] = value
            mutations.append(artifact)
        for artifact in mutations:
            with self.subTest(artifact=artifact):
                self.assert_source_invalid(_source(artifact=artifact))

    def test_unavailable_artifact_and_source_rules(self) -> None:
        mutations = []
        for field, value in (
            ("storage", "git"),
            ("path", "evidence/source-snapshots/EXAMPLE/file.txt"),
            ("sha256", "a" * 64),
            ("bytes", 12),
            ("media_type", "text/plain"),
            ("note", ""),
        ):
            artifact = _artifact(retrieval_status="unavailable")
            artifact[field] = value
            mutations.append(_source(status="unavailable", artifact=artifact))

        no_unavailable_artifact = _source(
            status="unavailable",
            artifact=_artifact(storage="external-reference"),
        )
        mutations.append(no_unavailable_artifact)
        has_observation = _source(
            status="unavailable",
            artifact=_artifact(retrieval_status="unavailable"),
            observations=[_observation()],
        )
        mutations.append(has_observation)
        superseded_with_current = _source(status="superseded")
        mutations.append(superseded_with_current)

        for source in mutations:
            with self.subTest(source=source):
                self.assert_source_invalid(source)

    def test_observation_scalar_and_field_rules(self) -> None:
        for value in ("in stock", 12.5, 4, True, None):
            source = _source(artifact={}, observations=[_observation(value=value)])
            source["artifacts"] = []
            with self.subTest(value=value):
                self.assert_source_valid(source)

        mutations = []
        for field, value in (
            ("key", ""),
            ("value", {"not": "scalar"}),
            ("unit", ""),
            ("basis", ""),
            ("observed_at", "2026-07-15T12:34:56+00:00"),
            ("source_url", "http://example.com/price"),
        ):
            observation = _observation()
            observation[field] = value
            mutations.append(observation)
        extra = _observation()
        extra["unexpected"] = True
        mutations.append(extra)

        for observation in mutations:
            source = _source(artifact={}, observations=[observation])
            source["artifacts"] = []
            with self.subTest(observation=observation):
                self.assert_source_invalid(source)


class SeedSchemaTests(unittest.TestCase):
    def assert_seeds_invalid(self, seeds: list[object]) -> None:
        with self.assertRaises(ValidationError):
            validate(seeds, SEED_SCHEMA, root_schema=SOURCE_SCHEMA)

    def test_committed_seed_schema_and_exact_oracle(self) -> None:
        validate(SEED_SOURCES, SEED_SCHEMA, root_schema=SOURCE_SCHEMA)
        assert_seed_oracle(SEED_SOURCES)

    def test_seed_count_uniqueness_and_fields(self) -> None:
        self.assert_seeds_invalid(copy.deepcopy(SEED_SOURCES[:-1]))
        extra = copy.deepcopy(SEED_SOURCES)
        extra.append(copy.deepcopy(extra[-1]))
        self.assert_seeds_invalid(extra)
        duplicate = copy.deepcopy(SEED_SOURCES)
        duplicate[-1] = copy.deepcopy(duplicate[0])
        self.assert_seeds_invalid(duplicate)

        mutations = []
        for field, value in (
            ("id", "bad-id"),
            ("class", "unknown"),
            ("authority", ""),
            ("landing_url", "http://example.com"),
            ("required_object", ""),
            ("purpose", ""),
            ("claim_scope", []),
            ("claim_scope", ["same", "same"]),
            ("required", "yes"),
        ):
            seeds = copy.deepcopy(SEED_SOURCES)
            seeds[0][field] = value
            mutations.append(seeds)
        extra_property = copy.deepcopy(SEED_SOURCES)
        extra_property[0]["unexpected"] = True
        mutations.append(extra_property)
        missing_property = copy.deepcopy(SEED_SOURCES)
        del missing_property[0]["required_object"]
        mutations.append(missing_property)

        for seeds in mutations:
            with self.subTest(seed=seeds[0]):
                self.assert_seeds_invalid(seeds)

    def test_oracle_rejects_missing_extra_reordered_and_changed_tuples(self) -> None:
        missing = copy.deepcopy(SEED_SOURCES[:-1])
        extra = copy.deepcopy(SEED_SOURCES)
        extra.append(copy.deepcopy(extra[-1]))
        reordered = copy.deepcopy(SEED_SOURCES)
        reordered[0], reordered[1] = reordered[1], reordered[0]
        changed = copy.deepcopy(SEED_SOURCES)
        changed[0]["authority"] = "A different authority"
        for seeds in (missing, extra, reordered, changed):
            with self.subTest(seeds=seeds), self.assertRaises(AssertionError):
                assert_seed_oracle(seeds)


class SourceCollectionTests(unittest.TestCase):
    def assert_collection_error(
        self, sources: object, seeds: object, diagnostic: str
    ) -> None:
        with self.assertRaises(SourceCollectionError) as caught:
            validate_source_collection(sources, seeds)
        self.assertIn(diagnostic, str(caught.exception))

    def test_valid_empty_and_supersession_chain(self) -> None:
        validate_source_collection([], SEED_SOURCES)
        prior = _source(
            "A-OLD",
            status="superseded",
            artifact=_artifact(retrieval_status="superseded"),
        )
        current = _source("B-NEW", supersedes="A-OLD")
        validate_source_collection([prior, current], SEED_SOURCES)

    def test_source_and_seed_sortedness_and_id_uniqueness(self) -> None:
        self.assert_collection_error(
            [_source("B-SOURCE"), _source("A-SOURCE")],
            SEED_SOURCES,
            "sources: IDs are not sorted",
        )
        self.assert_collection_error(
            [_source("A-SOURCE"), _source("A-SOURCE")],
            SEED_SOURCES,
            "sources: duplicate ID 'A-SOURCE'",
        )
        reversed_seeds = list(reversed(copy.deepcopy(SEED_SOURCES)))
        self.assert_collection_error([], reversed_seeds, "seeds: IDs are not sorted")
        duplicate_seeds = copy.deepcopy(SEED_SOURCES)
        duplicate_seeds[1]["id"] = duplicate_seeds[0]["id"]
        self.assert_collection_error(
            [], duplicate_seeds, "seeds: duplicate ID 'BOE-GBP-DAILY-FX'"
        )

    def test_artifact_name_uniqueness(self) -> None:
        source = _source("A-SOURCE")
        source["artifacts"].append(copy.deepcopy(source["artifacts"][0]))
        self.assert_collection_error(
            [source], SEED_SOURCES, "duplicate artifact name 'datasheet.pdf'"
        )

    def test_supersession_reference_and_status(self) -> None:
        self.assert_collection_error(
            [_source("A-SOURCE", supersedes="MISSING-SOURCE")],
            SEED_SOURCES,
            "supersedes unknown source 'MISSING-SOURCE'",
        )
        current_prior = _source("A-PRIOR")
        replacement = _source("B-REPLACEMENT", supersedes="A-PRIOR")
        self.assert_collection_error(
            [current_prior, replacement],
            SEED_SOURCES,
            "is not status 'superseded'",
        )

    def test_supersession_cycle_detection(self) -> None:
        left = _source("A-SOURCE", supersedes="B-SOURCE")
        right = _source("B-SOURCE", supersedes="A-SOURCE")
        self.assert_collection_error(
            [left, right], SEED_SOURCES, "supersession cycle: A-SOURCE -> B-SOURCE -> A-SOURCE"
        )


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
