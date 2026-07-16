#!/usr/bin/env python3
"""Executable contract tests for the Aius functional-topology schema."""

from __future__ import annotations

import copy
import json
import sys
import unittest
from pathlib import Path

from jsonschema_subset import DRAFT_2020_12, ValidationError, check_schema, validate


AIUS_ROOT = Path(__file__).resolve().parents[1]
SCHEMA_PATH = AIUS_ROOT / "evidence" / "design" / "functional-topology-schema.json"
FIXTURE_ROOT = Path(__file__).resolve().parent / "fixtures" / "p1c-schema" / "functional-topology"


def _load_json(path: Path) -> object:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


SCHEMA = _load_json(SCHEMA_PATH)
VALID = _load_json(FIXTURE_ROOT / "valid.json")

ROOT_KEYS = (
    "schema_version",
    "modes",
    "blocks",
    "flows",
    "clock_domains",
    "buffers",
    "latency_boundaries",
    "evidence_ids",
)
ID_ORDERS = {
    "modes": (
        "pcm-processed",
        "dts-processed",
        "diagnostic-raw-bypass",
        "unsupported-muted",
        "fault-muted",
    ),
    "blocks": (
        "power-entry",
        "usb-bridge",
        "teensy",
        "optical-rx",
        "spdif-capture",
        "input-classifier",
        "dts-decoder",
        "pcm-normalizer",
        "downmix-eq",
        "spdif-transmit",
        "raw-bypass-mux",
        "tx-gate",
        "optical-tx",
        "ethernet",
        "expansion",
    ),
    "flows": (
        "optical-to-capture",
        "capture-to-classifier",
        "classifier-to-dts",
        "classifier-to-pcm",
        "dts-to-normalizer",
        "pcm-to-normalizer",
        "normalizer-to-dsp",
        "dsp-to-transmit",
        "transmit-to-mux",
        "optical-to-raw-mux",
        "mux-to-gate",
        "gate-to-optical",
        "path-select-control",
        "tx-enable-control",
        "watchdog-reset-control",
        "usb-vbus-entry",
        "protected-vbus",
        "teensy-power",
        "carrier-3v3",
        "expansion-power",
    ),
    "clock_domains": (
        "spdif-recovered",
        "audio-processing",
        "spdif-transmit-clock",
        "usb-device-clock",
        "ethernet-rmii-clock",
    ),
    "buffers": (
        "spdif-rx-dma",
        "compressed-burst-ring",
        "decoded-pcm-pool",
        "dsp-output-ring",
        "spdif-tx-dma",
    ),
    "latency_boundaries": (
        "capture",
        "burst-assembly",
        "decode",
        "dsp",
        "transmit-queue",
    ),
}
RECORD_DEFS = {
    "modes": "mode",
    "blocks": "block",
    "flows": "flow",
    "clock_domains": "clockDomain",
    "buffers": "buffer",
    "latency_boundaries": "latencyBoundary",
}
RECORD_KEYS = {
    "modes": (
        "id",
        "input_formats",
        "output_format",
        "active_blocks",
        "error_mode",
        "evidence_ids",
        "notes",
    ),
    "blocks": (
        "id",
        "kind",
        "owner",
        "power_domain",
        "clock_domain",
        "evidence_ids",
        "notes",
    ),
    "flows": (
        "id",
        "kind",
        "source_id",
        "sink_id",
        "payload",
        "enabled_modes",
        "control_id",
        "evidence_ids",
        "notes",
    ),
    "clock_domains": (
        "id",
        "source_id",
        "consumer_ids",
        "synchronous_with",
        "frequency_status",
        "evidence_ids",
        "notes",
    ),
    "buffers": (
        "id",
        "owner",
        "producer",
        "consumer",
        "storage_class",
        "overflow_policy",
        "measurement_gate",
        "notes",
    ),
    "latency_boundaries": (
        "id",
        "source",
        "sink",
        "budget_status",
        "measurement_gate",
        "notes",
    ),
}
WRONG_FIELD_TYPES = {
    "modes": {
        "id": 1,
        "input_formats": {},
        "output_format": 1,
        "active_blocks": {},
        "error_mode": 1,
        "evidence_ids": {},
        "notes": None,
    },
    "blocks": {
        "id": 1,
        "kind": 1,
        "owner": 1,
        "power_domain": {},
        "clock_domain": {},
        "evidence_ids": {},
        "notes": None,
    },
    "flows": {
        "id": 1,
        "kind": 1,
        "source_id": 1,
        "sink_id": 1,
        "payload": 1,
        "enabled_modes": {},
        "control_id": {},
        "evidence_ids": {},
        "notes": None,
    },
    "clock_domains": {
        "id": 1,
        "source_id": 1,
        "consumer_ids": {},
        "synchronous_with": {},
        "frequency_status": 1,
        "evidence_ids": {},
        "notes": None,
    },
    "buffers": {
        "id": 1,
        "owner": 1,
        "producer": 1,
        "consumer": 1,
        "storage_class": 1,
        "overflow_policy": 1,
        "measurement_gate": 1,
        "notes": None,
    },
    "latency_boundaries": {
        "id": 1,
        "source": 1,
        "sink": 1,
        "budget_status": 1,
        "measurement_gate": 1,
        "notes": None,
    },
}
ENUM_FIELDS = {
    "modes": ("id", "input_formats", "output_format", "active_blocks", "error_mode"),
    "blocks": ("id", "kind", "owner", "power_domain", "clock_domain"),
    "flows": (
        "id",
        "kind",
        "source_id",
        "sink_id",
        "payload",
        "enabled_modes",
        "control_id",
    ),
    "clock_domains": (
        "id",
        "source_id",
        "consumer_ids",
        "synchronous_with",
        "frequency_status",
    ),
    "buffers": (
        "id",
        "owner",
        "producer",
        "consumer",
        "storage_class",
        "overflow_policy",
        "measurement_gate",
    ),
    "latency_boundaries": (
        "id",
        "source",
        "sink",
        "budget_status",
        "measurement_gate",
    ),
}
ARRAY_ENUM_FIELDS = {
    ("modes", "input_formats"),
    ("modes", "active_blocks"),
    ("flows", "enabled_modes"),
    ("clock_domains", "consumer_ids"),
    ("clock_domains", "synchronous_with"),
}
EVIDENCE_LOCATIONS = ("modes", "blocks", "flows", "clock_domains")


class FunctionalTopologySchemaTestCase(unittest.TestCase):
    def topology(self) -> dict[str, object]:
        return copy.deepcopy(VALID)  # type: ignore[return-value]

    def assert_invalid(self, instance: object, schema: object = SCHEMA) -> None:
        with self.assertRaises(ValidationError):
            validate(instance, schema, root_schema=SCHEMA)

    def assert_canonical_order(self, instance: object) -> None:
        self.assertIsInstance(instance, dict)
        document = instance
        self.assertEqual(tuple(document), ROOT_KEYS)
        for collection, expected_ids in ID_ORDERS.items():
            records = document[collection]
            self.assertEqual(tuple(record["id"] for record in records), expected_ids)
            expected_keys = RECORD_KEYS[collection]
            for record in records:
                self.assertEqual(tuple(record), expected_keys)


class SchemaVocabularyTests(FunctionalTopologySchemaTestCase):
    def test_schema_is_supported_draft_2020_12(self) -> None:
        self.assertEqual(SCHEMA["$schema"], DRAFT_2020_12)
        self.assertEqual(
            SCHEMA["$id"],
            "https://example.net/schemas/aius/functional-topology-schema.json",
        )
        check_schema(SCHEMA)

    def test_locked_id_vocabularies_match_the_plan_in_declared_order(self) -> None:
        definition_names = {
            "modes": "modeId",
            "blocks": "blockId",
            "flows": "flowId",
            "clock_domains": "clockDomainId",
            "buffers": "bufferId",
            "latency_boundaries": "latencyBoundaryId",
        }
        for collection, definition_name in definition_names.items():
            with self.subTest(collection=collection):
                self.assertEqual(
                    tuple(SCHEMA["$defs"][definition_name]["enum"]),
                    ID_ORDERS[collection],
                )

        self.assertEqual(
            tuple(SCHEMA["$defs"]["powerDomainId"]["enum"]),
            (
                "usb-vbus-raw",
                "protected-5v",
                "teensy-3v3",
                "digital-ground",
                "chassis-shield",
                "expansion-3v3",
                "expansion-usb-host-vbus",
            ),
        )
        self.assertEqual(
            tuple(SCHEMA["$defs"]["controlId"]["enum"]),
            (
                "spdif-path-select",
                "optical-tx-enable",
                "teensy-reset",
                "teensy-watchdog",
                "program-control",
                "status-led",
            ),
        )

    def test_all_other_locked_enums_and_constants_match_the_plan(self) -> None:
        definitions = SCHEMA["$defs"]
        expected = {
            ("mode", "input_formats"): ("pcm-stereo", "iec61937-dts-core", "none"),
            ("mode", "output_format"): ("pcm-stereo-48k", "raw-spdif", "none"),
            ("mode", "error_mode"): ("unsupported-muted", "fault-muted"),
            ("block", "kind"): ("hardware", "firmware", "software", "boundary"),
            ("block", "owner"): (
                "carrier",
                "teensy-hardware",
                "teensy-firmware",
                "external",
            ),
            ("flow", "kind"): ("data", "control", "power"),
            ("flow", "payload"): (
                "optical-spdif",
                "logic-spdif",
                "iec61937-dts-core",
                "pcm-native",
                "pcm-stereo-48k",
                "path-select",
                "tx-enable",
                "reset",
                "usb-vbus",
                "protected-5v",
                "teensy-3v3",
            ),
            ("clockDomain", "frequency_status"): (
                "evidence-locked",
                "software-derived",
                "deferred",
            ),
            ("buffer", "storage_class"): ("dma-ring", "ring-buffer", "block-pool"),
            ("buffer", "overflow_policy"): (
                "drop-and-mute",
                "reset-pipeline-and-mute",
                "backpressure",
            ),
            ("buffer", "measurement_gate"): ("P3", "P4", "P5", "P6", "P7"),
            ("latencyBoundary", "measurement_gate"): ("P6", "P7"),
        }
        for (definition, field), values in expected.items():
            property_schema = definitions[definition]["properties"][field]
            if definition == "mode" and field == "input_formats":
                property_schema = property_schema["items"]
            with self.subTest(definition=definition, field=field):
                self.assertEqual(tuple(property_schema["enum"]), values)

        self.assertEqual(SCHEMA["properties"]["schema_version"]["const"], "1.0.0")
        self.assertEqual(
            definitions["latencyBoundary"]["properties"]["budget_status"]["const"],
            "deferred",
        )

    def test_schema_property_and_required_order_is_normative(self) -> None:
        self.assertEqual(tuple(SCHEMA["properties"]), ROOT_KEYS)
        self.assertEqual(tuple(SCHEMA["required"]), ROOT_KEYS)
        self.assertEqual(1, SCHEMA["$defs"]["evidenceIds"]["minItems"])
        for collection, definition_name in RECORD_DEFS.items():
            definition = SCHEMA["$defs"][definition_name]
            with self.subTest(collection=collection):
                self.assertEqual(tuple(definition["properties"]), RECORD_KEYS[collection])
                self.assertEqual(tuple(definition["required"]), RECORD_KEYS[collection])


class FixtureTests(FunctionalTopologySchemaTestCase):
    def test_canonical_fixture_is_valid_and_ordered(self) -> None:
        validate(VALID, SCHEMA)
        self.assert_canonical_order(VALID)

    def test_focused_invalid_record_fixtures_are_rejected(self) -> None:
        cases = {
            "mode-unknown-format.json": "mode",
            "block-null-kind.json": "block",
            "flow-additional-property.json": "flow",
            "clock-domain-empty-consumers.json": "clockDomain",
            "buffer-unknown-gate.json": "buffer",
            "latency-boundary-numeric-budget.json": "latencyBoundary",
            "evidence-empty.json": "mode",
        }
        for filename, definition in cases.items():
            with self.subTest(filename=filename):
                self.assert_invalid(
                    _load_json(FIXTURE_ROOT / "invalid" / filename),
                    SCHEMA["$defs"][definition],
                )

        self.assert_invalid(_load_json(FIXTURE_ROOT / "invalid" / "root-array.json"))


class RequiredAndTypeTests(FunctionalTopologySchemaTestCase):
    def test_every_root_and_record_field_is_required(self) -> None:
        for field in ROOT_KEYS:
            topology = self.topology()
            del topology[field]
            with self.subTest(scope="root", field=field):
                self.assert_invalid(topology)

        for collection, fields in RECORD_KEYS.items():
            for field in fields:
                topology = self.topology()
                del topology[collection][0][field]
                with self.subTest(collection=collection, field=field):
                    self.assert_invalid(topology)

    def test_every_object_rejects_additional_properties(self) -> None:
        topology = self.topology()
        topology["unexpected"] = True
        self.assert_invalid(topology)

        for collection in RECORD_KEYS:
            topology = self.topology()
            topology[collection][0]["unexpected"] = True
            with self.subTest(collection=collection):
                self.assert_invalid(topology)

    def test_every_root_and_record_field_rejects_a_wrong_json_type(self) -> None:
        root_values = {
            "schema_version": 1,
            "modes": {},
            "blocks": {},
            "flows": {},
            "clock_domains": {},
            "buffers": {},
            "latency_boundaries": {},
            "evidence_ids": {},
        }
        for field, value in root_values.items():
            topology = self.topology()
            topology[field] = value
            with self.subTest(scope="root", field=field):
                self.assert_invalid(topology)

        for collection, fields in WRONG_FIELD_TYPES.items():
            for field, value in fields.items():
                topology = self.topology()
                topology[collection][0][field] = value
                with self.subTest(collection=collection, field=field):
                    self.assert_invalid(topology)

    def test_array_item_types_and_evidence_identifier_pattern_are_enforced(self) -> None:
        item_fields = {
            ("modes", "input_formats"),
            ("modes", "active_blocks"),
            ("flows", "enabled_modes"),
            ("clock_domains", "consumer_ids"),
            ("clock_domains", "synchronous_with"),
        }
        for collection, field in item_fields:
            topology = self.topology()
            topology[collection][0][field] = [1]
            with self.subTest(collection=collection, field=field):
                self.assert_invalid(topology)

        for collection in EVIDENCE_LOCATIONS:
            topology = self.topology()
            topology[collection][0]["evidence_ids"] = ["bad-evidence-id"]
            with self.subTest(collection=collection):
                self.assert_invalid(topology)

        topology = self.topology()
        topology["evidence_ids"] = ["bad-evidence-id"]
        self.assert_invalid(topology)


class EnumAndNullabilityTests(FunctionalTopologySchemaTestCase):
    def test_every_enum_or_constant_field_rejects_an_unknown_string(self) -> None:
        for collection, fields in ENUM_FIELDS.items():
            for field in fields:
                topology = self.topology()
                value = ["not-a-locked-token"] if (collection, field) in ARRAY_ENUM_FIELDS else "not-a-locked-token"
                topology[collection][0][field] = value
                with self.subTest(collection=collection, field=field):
                    self.assert_invalid(topology)

        topology = self.topology()
        topology["schema_version"] = "1.0.1"
        self.assert_invalid(topology)

    def test_only_power_clock_and_control_references_are_nullable(self) -> None:
        nullable_cases = (
            ("blocks", "power_domain", "teensy-3v3"),
            ("blocks", "clock_domain", "audio-processing"),
            ("flows", "control_id", "spdif-path-select"),
        )
        for collection, field, locked_value in nullable_cases:
            for value in (None, locked_value):
                topology = self.topology()
                topology[collection][0][field] = value
                with self.subTest(collection=collection, field=field, value=value):
                    validate(topology, SCHEMA)

        nullable_fields = {(collection, field) for collection, field, _ in nullable_cases}
        for collection, fields in RECORD_KEYS.items():
            for field in fields:
                if (collection, field) in nullable_fields:
                    continue
                topology = self.topology()
                topology[collection][0][field] = None
                with self.subTest(collection=collection, field=field):
                    self.assert_invalid(topology)


class ArrayAndCoverageTests(FunctionalTopologySchemaTestCase):
    def test_unique_reference_and_evidence_arrays_reject_duplicates(self) -> None:
        cases = (
            ("modes", "input_formats", "pcm-stereo"),
            ("modes", "active_blocks", "teensy"),
            ("flows", "enabled_modes", "pcm-processed"),
            ("clock_domains", "consumer_ids", "teensy"),
            ("clock_domains", "synchronous_with", "audio-processing"),
        )
        for collection, field, value in cases:
            topology = self.topology()
            topology[collection][0][field] = [value, value]
            with self.subTest(collection=collection, field=field):
                self.assert_invalid(topology)

        for collection in EVIDENCE_LOCATIONS:
            topology = self.topology()
            topology[collection][0]["evidence_ids"] = ["EVIDENCE-A", "EVIDENCE-A"]
            with self.subTest(collection=collection):
                self.assert_invalid(topology)

        topology = self.topology()
        topology["evidence_ids"] = ["EVIDENCE-A", "EVIDENCE-A"]
        self.assert_invalid(topology)

    def test_required_non_empty_arrays_reject_empty_values(self) -> None:
        for collection, field in (
            ("modes", "input_formats"),
            ("clock_domains", "consumer_ids"),
        ):
            topology = self.topology()
            topology[collection][0][field] = []
            with self.subTest(collection=collection, field=field):
                self.assert_invalid(topology)

        for collection in EVIDENCE_LOCATIONS:
            topology = self.topology()
            topology[collection][0]["evidence_ids"] = []
            with self.subTest(collection=collection, field="evidence_ids"):
                self.assert_invalid(topology)

        topology = self.topology()
        topology["evidence_ids"] = []
        self.assert_invalid(topology)

    def test_only_non_evidence_optional_arrays_may_be_empty(self) -> None:
        topology = self.topology()
        topology["modes"][0]["active_blocks"] = []
        topology["flows"][0]["enabled_modes"] = []
        topology["clock_domains"][0]["synchronous_with"] = []
        validate(topology, SCHEMA)

    def test_every_locked_id_collection_requires_the_exact_set_once(self) -> None:
        for collection in ID_ORDERS:
            topology = self.topology()
            del topology[collection][-1]
            with self.subTest(collection=collection, case="missing"):
                self.assert_invalid(topology)

            topology = self.topology()
            topology[collection].append(copy.deepcopy(topology[collection][0]))
            with self.subTest(collection=collection, case="extra"):
                self.assert_invalid(topology)

            topology = self.topology()
            topology[collection][1]["id"] = topology[collection][0]["id"]
            with self.subTest(collection=collection, case="duplicate-id"):
                self.assert_invalid(topology)

            topology = self.topology()
            topology[collection][0]["id"] = "unknown-id"
            with self.subTest(collection=collection, case="unknown-id"):
                self.assert_invalid(topology)

    def test_schema_accepts_reordering_but_the_canonical_order_oracle_rejects_it(self) -> None:
        for collection in ID_ORDERS:
            topology = self.topology()
            topology[collection][0], topology[collection][1] = (
                topology[collection][1],
                topology[collection][0],
            )
            with self.subTest(collection=collection):
                validate(topology, SCHEMA)
                with self.assertRaises(AssertionError):
                    self.assert_canonical_order(topology)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
