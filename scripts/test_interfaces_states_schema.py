#!/usr/bin/env python3
"""Executable contract tests for the Aius interface/state schema."""

from __future__ import annotations

import copy
import json
import unittest
from pathlib import Path

from jsonschema_subset import ValidationError, check_schema, validate


AIUS_ROOT = Path(__file__).resolve().parents[1]
SCHEMA_PATH = AIUS_ROOT / "evidence" / "design" / "interfaces-and-states-schema.json"
FIXTURE_ROOT = Path(__file__).resolve().parent / "fixtures" / "p1c-schema" / "interfaces-and-states"


def _load_json(path: Path) -> object:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


SCHEMA = _load_json(SCHEMA_PATH)
VALID = _load_json(FIXTURE_ROOT / "valid.json")

ROOT_FIELDS = (
    "schema_version",
    "connectors",
    "controls",
    "expansion_interfaces",
    "power_domains",
    "operating_states",
    "fault_scenarios",
    "transitions",
    "evidence_ids",
)

COLLECTIONS = {
    "connectors": (
        "connector",
        (
            "id",
            "location",
            "role",
            "signals",
            "power_domains",
            "mating_constraint",
            "evidence_ids",
            "notes",
        ),
    ),
    "controls": (
        "control",
        (
            "id",
            "owner",
            "active_level",
            "reset_default",
            "safe_value",
            "evidence_ids",
            "notes",
        ),
    ),
    "expansion_interfaces": (
        "expansionInterface",
        (
            "id",
            "direction",
            "signals",
            "power_domains",
            "powered_behavior",
            "unpowered_behavior",
            "protection_method",
            "backfeed_rule",
            "evidence_ids",
            "notes",
        ),
    ),
    "power_domains": (
        "powerDomain",
        ("id", "source", "consumers", "backfeed_rule", "evidence_ids", "notes"),
    ),
    "operating_states": (
        "operatingState",
        (
            "id",
            "entry_condition",
            "path_select",
            "tx_state",
            "output_payload",
            "expansion_state",
            "exit_condition",
            "notes",
        ),
    ),
    "fault_scenarios": (
        "faultScenario",
        (
            "id",
            "trigger",
            "path_select",
            "tx_state",
            "output_payload",
            "expansion_state",
            "hardware_pull_required",
            "watchdog_required",
            "debounce_measurement_gate",
            "residual_interval_measurement_gate",
            "actions",
            "recovery_state",
            "notes",
        ),
    ),
    "transitions": (
        "transition",
        (
            "id",
            "from_state",
            "to_state",
            "triggers",
            "preconditions",
            "actions",
            "postconditions",
            "notes",
        ),
    ),
}

EXPECTED_IDS = {
    "connectors": (
        "rear-usb-c",
        "internal-usb-a",
        "teensy-micro-b",
        "toslink-rx",
        "toslink-tx",
        "ethernet-rj45",
        "microsd-access",
        "remote-program-rtc",
        "test-fixture",
    ),
    "controls": (
        "spdif-path-select",
        "optical-tx-enable",
        "teensy-reset",
        "teensy-watchdog",
        "program-control",
        "status-led",
    ),
    "expansion_interfaces": (
        "expansion-i2c-control",
        "expansion-spi",
        "expansion-uart-debug",
        "expansion-i2s-tdm",
        "expansion-usb-host",
        "expansion-gpio-adc",
    ),
    "power_domains": (
        "usb-vbus-raw",
        "protected-5v",
        "teensy-3v3",
        "digital-ground",
        "chassis-shield",
        "expansion-3v3",
        "expansion-usb-host-vbus",
    ),
    "operating_states": (
        "power-off",
        "powered-reset",
        "boot-muted",
        "waiting-for-carrier",
        "pcm-active",
        "dts-active",
        "unsupported-muted",
        "diagnostic-bypass",
        "fault-muted",
    ),
    "fault_scenarios": (
        "teensy-absent",
        "boot",
        "reset",
        "brownout",
        "firmware-crash",
        "optical-carrier-loss",
        "unsupported-burst",
        "malformed-input",
        "usb-unplug",
        "raw-bypass-request",
        "unpowered-expansion",
    ),
    "transitions": (
        "power-on-to-reset",
        "reset-to-boot",
        "boot-to-waiting",
        "waiting-to-pcm",
        "waiting-to-dts",
        "active-to-carrier-loss",
        "active-to-unsupported",
        "unsupported-to-active",
        "active-to-diagnostic-bypass",
        "diagnostic-bypass-to-waiting",
        "watchdog-to-reset",
        "usb-unplug-to-power-off",
    ),
}

TRANSITION_TRIGGERS = {
    "power-on-to-reset": ("power-applied",),
    "reset-to-boot": ("reset-released",),
    "boot-to-waiting": ("pipeline-ready",),
    "waiting-to-pcm": ("pcm-detected",),
    "waiting-to-dts": ("dts-detected",),
    "active-to-carrier-loss": ("optical-carrier-loss",),
    "active-to-unsupported": ("unsupported-burst", "malformed-input"),
    "unsupported-to-active": ("supported-format-detected",),
    "active-to-diagnostic-bypass": ("raw-bypass-request",),
    "diagnostic-bypass-to-waiting": ("raw-bypass-release",),
    "watchdog-to-reset": ("watchdog-expired",),
    "usb-unplug-to-power-off": ("usb-unplug",),
}

FAULT_OUTCOMES = {
    "teensy-absent": (
        ("processed",),
        ("disabled",),
        ("none",),
        ("high-impedance", "current-limited"),
    ),
    "boot": (
        ("processed",),
        ("disabled",),
        ("none",),
        ("high-impedance", "current-limited"),
    ),
    "reset": (
        ("processed",),
        ("disabled",),
        ("none",),
        ("high-impedance", "current-limited"),
    ),
    "brownout": (
        ("processed",),
        ("disabled",),
        ("none",),
        ("high-impedance", "current-limited"),
    ),
    "firmware-crash": (
        ("processed",),
        ("disabled",),
        ("none",),
        ("high-impedance", "current-limited"),
    ),
    "optical-carrier-loss": (
        ("processed",),
        ("disabled",),
        ("none",),
        ("unchanged",),
    ),
    "unsupported-burst": (
        ("processed",),
        ("enabled-pcm",),
        ("pcm-silence-48k",),
        ("unchanged",),
    ),
    "malformed-input": (
        ("processed",),
        ("enabled-pcm",),
        ("pcm-silence-48k",),
        ("unchanged",),
    ),
    "usb-unplug": (
        ("processed",),
        ("disabled",),
        ("none",),
        ("high-impedance", "current-limited"),
    ),
    "raw-bypass-request": (
        ("raw",),
        ("enabled-raw",),
        ("raw-spdif",),
        ("unchanged",),
    ),
    "unpowered-expansion": (
        ("processed",),
        ("unchanged",),
        ("unchanged",),
        ("high-impedance", "current-limited"),
    ),
}


class InterfacesAndStatesSchemaTests(unittest.TestCase):
    def assert_document_invalid(self, document: object) -> None:
        with self.assertRaises(ValidationError):
            validate(document, SCHEMA)

    def assert_definition_invalid(self, instance: object, definition: str) -> None:
        with self.assertRaises(ValidationError):
            validate(instance, SCHEMA["$defs"][definition], root_schema=SCHEMA)

    def record_index(self, collection: str, record_id: str) -> int:
        return next(
            index
            for index, record in enumerate(VALID[collection])
            if record["id"] == record_id
        )

    def test_committed_schema_and_canonical_fixture(self) -> None:
        check_schema(SCHEMA)
        validate(VALID, SCHEMA)
        self.assertEqual(VALID["schema_version"], "1.0.0")

    def test_schema_and_fixture_preserve_locked_property_and_row_order(self) -> None:
        self.assertEqual(tuple(SCHEMA["required"]), ROOT_FIELDS)
        self.assertEqual(tuple(SCHEMA["properties"]), ROOT_FIELDS)
        self.assertEqual(tuple(VALID), ROOT_FIELDS)
        for collection, (definition, fields) in COLLECTIONS.items():
            with self.subTest(collection=collection):
                self.assertEqual(tuple(SCHEMA["$defs"][definition]["required"]), fields)
                self.assertEqual(tuple(SCHEMA["$defs"][definition]["properties"]), fields)
                for record in VALID[collection]:
                    self.assertEqual(tuple(record), fields)
                self.assertEqual(
                    tuple(record["id"] for record in VALID[collection]),
                    EXPECTED_IDS[collection],
                )

    def test_focused_invalid_fixtures(self) -> None:
        fixtures = {
            "invalid-connector-control-id.json": "connector",
            "invalid-expansion-connector-id.json": "expansionInterface",
            "invalid-fault-outcome.json": "faultScenario",
            "invalid-transition-trigger.json": "transition",
            "invalid-additional-property.json": "operatingState",
            "invalid-missing-required.json": "control",
            "invalid-nullability.json": "faultScenario",
            "invalid-array-duplicate.json": "connector",
        }
        for name, definition in fixtures.items():
            with self.subTest(name=name):
                self.assert_definition_invalid(_load_json(FIXTURE_ROOT / name), definition)

    def test_root_required_types_version_and_additional_properties(self) -> None:
        for field in ROOT_FIELDS:
            mutated = copy.deepcopy(VALID)
            del mutated[field]
            with self.subTest(missing=field):
                self.assert_document_invalid(mutated)

        extra = copy.deepcopy(VALID)
        extra["unexpected"] = True
        self.assert_document_invalid(extra)

        invalid_values = {
            "schema_version": ("1.0", None),
            "connectors": ({}, None),
            "controls": ({}, None),
            "expansion_interfaces": ({}, None),
            "power_domains": ({}, None),
            "operating_states": ({}, None),
            "fault_scenarios": ({}, None),
            "transitions": ({}, None),
            "evidence_ids": ({}, None),
        }
        for field, values in invalid_values.items():
            for value in values:
                mutated = copy.deepcopy(VALID)
                mutated[field] = value
                with self.subTest(field=field, value=value):
                    self.assert_document_invalid(mutated)

    def test_every_member_is_required_and_every_object_is_closed(self) -> None:
        for collection, (_, fields) in COLLECTIONS.items():
            for field in fields:
                mutated = copy.deepcopy(VALID)
                del mutated[collection][0][field]
                with self.subTest(collection=collection, missing=field):
                    self.assert_document_invalid(mutated)

            mutated = copy.deepcopy(VALID)
            mutated[collection][0]["unexpected"] = True
            with self.subTest(collection=collection, additional=True):
                self.assert_document_invalid(mutated)

    def test_every_member_field_rejects_wrong_type_or_domain(self) -> None:
        cases = (
            ("connectors", 0, "id", "program-control"),
            ("connectors", 0, "location", "front"),
            ("connectors", 0, "role", "power"),
            ("connectors", 0, "signals", "VBUS"),
            ("connectors", 0, "power_domains", "usb-vbus-raw"),
            ("connectors", 0, "mating_constraint", 7),
            ("connectors", 0, "evidence_ids", "SOURCE"),
            ("connectors", 0, "notes", None),
            ("controls", 0, "id", "rear-usb-c"),
            ("controls", 0, "owner", 7),
            ("controls", 0, "active_level", "active-high"),
            ("controls", 0, "reset_default", "safe"),
            ("controls", 0, "safe_value", "safe"),
            ("controls", 0, "evidence_ids", "SOURCE"),
            ("controls", 0, "notes", None),
            ("expansion_interfaces", 0, "id", "rear-usb-c"),
            ("expansion_interfaces", 0, "direction", "inout"),
            ("expansion_interfaces", 0, "signals", "I2C-SCL"),
            ("expansion_interfaces", 0, "power_domains", "expansion-3v3"),
            ("expansion_interfaces", 0, "powered_behavior", 7),
            ("expansion_interfaces", 0, "unpowered_behavior", 7),
            ("expansion_interfaces", 0, "protection_method", 7),
            ("expansion_interfaces", 0, "backfeed_rule", 7),
            ("expansion_interfaces", 0, "evidence_ids", "SOURCE"),
            ("expansion_interfaces", 0, "notes", None),
            ("power_domains", 0, "id", "unknown-rail"),
            ("power_domains", 0, "source", 7),
            ("power_domains", 0, "consumers", "power-entry"),
            ("power_domains", 0, "backfeed_rule", 7),
            ("power_domains", 0, "evidence_ids", "SOURCE"),
            ("power_domains", 0, "notes", None),
            ("operating_states", 0, "id", "unknown-state"),
            ("operating_states", 0, "entry_condition", 7),
            ("operating_states", 0, "path_select", "bypass"),
            ("operating_states", 0, "tx_state", "enabled"),
            ("operating_states", 0, "output_payload", "silence"),
            ("operating_states", 0, "expansion_state", "safe"),
            ("operating_states", 0, "exit_condition", 7),
            ("operating_states", 0, "notes", None),
            ("fault_scenarios", 0, "id", "unknown-fault"),
            ("fault_scenarios", 0, "trigger", 7),
            ("fault_scenarios", 0, "path_select", "bypass"),
            ("fault_scenarios", 0, "tx_state", "enabled"),
            ("fault_scenarios", 0, "output_payload", "silence"),
            ("fault_scenarios", 0, "expansion_state", "safe"),
            ("fault_scenarios", 0, "hardware_pull_required", 1),
            ("fault_scenarios", 0, "watchdog_required", 0),
            ("fault_scenarios", 0, "debounce_measurement_gate", "P5"),
            ("fault_scenarios", 0, "residual_interval_measurement_gate", "P6"),
            ("fault_scenarios", 0, "actions", "disable-tx"),
            ("fault_scenarios", 0, "recovery_state", "recover"),
            ("fault_scenarios", 0, "notes", None),
            ("transitions", 0, "id", "unknown-transition"),
            ("transitions", 0, "from_state", "unknown-state"),
            ("transitions", 0, "to_state", "unknown-state"),
            ("transitions", 0, "triggers", "power-applied"),
            ("transitions", 0, "preconditions", "rails-valid"),
            ("transitions", 0, "actions", "assert-reset"),
            ("transitions", 0, "postconditions", "tx-disabled"),
            ("transitions", 0, "notes", None),
        )
        for collection, index, field, value in cases:
            mutated = copy.deepcopy(VALID)
            mutated[collection][index][field] = value
            with self.subTest(collection=collection, field=field, value=value):
                self.assert_document_invalid(mutated)

    def test_non_empty_strings_and_array_item_domains(self) -> None:
        empty_string_cases = (
            ("connectors", 0, "signals", [" "]),
            ("connectors", 0, "mating_constraint", " "),
            ("controls", 0, "owner", ""),
            ("expansion_interfaces", 0, "powered_behavior", ""),
            ("expansion_interfaces", 0, "unpowered_behavior", " "),
            ("expansion_interfaces", 0, "protection_method", ""),
            ("expansion_interfaces", 0, "backfeed_rule", " "),
            ("power_domains", 0, "source", ""),
            ("power_domains", 0, "consumers", [""]),
            ("power_domains", 0, "backfeed_rule", " "),
            ("operating_states", 0, "entry_condition", ""),
            ("operating_states", 0, "exit_condition", " "),
            ("fault_scenarios", 0, "trigger", ""),
        )
        item_cases = (
            ("connectors", 0, "signals", [7]),
            ("connectors", 0, "power_domains", ["unknown-rail"]),
            ("connectors", 0, "evidence_ids", ["lowercase-id"]),
            ("expansion_interfaces", 0, "signals", [7]),
            ("expansion_interfaces", 0, "power_domains", ["unknown-rail"]),
            ("power_domains", 0, "consumers", [7]),
            ("fault_scenarios", 0, "actions", ["mute"]),
            ("transitions", 0, "preconditions", ["power-valid"]),
            ("transitions", 0, "actions", ["mute"]),
            ("transitions", 0, "postconditions", ["safe"]),
        )
        for collection, index, field, value in empty_string_cases + item_cases:
            mutated = copy.deepcopy(VALID)
            mutated[collection][index][field] = value
            with self.subTest(collection=collection, field=field, value=value):
                self.assert_document_invalid(mutated)

    def test_unique_and_non_empty_array_boundaries(self) -> None:
        duplicate_cases = (
            ("connectors", 0, "signals", "VBUS"),
            ("connectors", 0, "power_domains", "usb-vbus-raw"),
            ("connectors", 3, "evidence_ids", "CLIFF-ORJ5"),
            ("controls", 4, "evidence_ids", "PJRC-TEENSY41-PINOUT"),
            ("expansion_interfaces", 0, "signals", "I2C-SCL"),
            ("expansion_interfaces", 0, "power_domains", "expansion-3v3"),
            ("expansion_interfaces", 0, "evidence_ids", "CLIFF-ORJ5"),
            ("power_domains", 1, "consumers", "teensy"),
            ("power_domains", 2, "evidence_ids", "PJRC-TEENSY41-SCHEMATIC"),
            ("fault_scenarios", 1, "actions", "disable-tx"),
            ("transitions", 0, "triggers", "power-applied"),
            ("transitions", 0, "preconditions", "rails-valid"),
            ("transitions", 0, "postconditions", "tx-disabled"),
        )
        for collection, index, field, value in duplicate_cases:
            mutated = copy.deepcopy(VALID)
            mutated[collection][index][field] = [value, value]
            with self.subTest(collection=collection, field=field):
                self.assert_document_invalid(mutated)

        root_duplicate = copy.deepcopy(VALID)
        root_duplicate["evidence_ids"] = ["CLIFF-ORJ5", "CLIFF-ORJ5"]
        self.assert_document_invalid(root_duplicate)

        for collection, field in (
            ("connectors", "signals"),
            ("expansion_interfaces", "signals"),
            ("transitions", "triggers"),
            ("transitions", "actions"),
            ("transitions", "postconditions"),
        ):
            mutated = copy.deepcopy(VALID)
            mutated[collection][0][field] = []
            with self.subTest(collection=collection, empty=field):
                self.assert_document_invalid(mutated)

        permitted_empty_arrays = (
            ("connectors", 0, "power_domains"),
            ("connectors", 0, "evidence_ids"),
            ("controls", 0, "evidence_ids"),
            ("expansion_interfaces", 0, "power_domains"),
            ("expansion_interfaces", 0, "evidence_ids"),
            ("power_domains", 0, "consumers"),
            ("power_domains", 0, "evidence_ids"),
            ("fault_scenarios", 0, "actions"),
            ("transitions", 0, "preconditions"),
        )
        for collection, index, field in permitted_empty_arrays:
            mutated = copy.deepcopy(VALID)
            mutated[collection][index][field] = []
            with self.subTest(collection=collection, permitted_empty=field):
                validate(mutated, SCHEMA)

        repeated_ordered_action = copy.deepcopy(VALID)
        repeated_ordered_action["transitions"][0]["actions"] = [
            "assert-reset",
            "assert-reset",
        ]
        validate(repeated_ordered_action, SCHEMA)

    def test_nullable_measurement_gates_accept_only_locked_values(self) -> None:
        for debounce, residual in ((None, None), ("P6", "P7"), ("P7", "P7")):
            mutated = copy.deepcopy(VALID)
            index = self.record_index("fault_scenarios", "brownout")
            mutated["fault_scenarios"][index]["debounce_measurement_gate"] = debounce
            mutated["fault_scenarios"][index][
                "residual_interval_measurement_gate"
            ] = residual
            with self.subTest(debounce=debounce, residual=residual):
                validate(mutated, SCHEMA)

        for field in (
            "path_select",
            "tx_state",
            "output_payload",
            "expansion_state",
            "hardware_pull_required",
            "watchdog_required",
            "actions",
            "recovery_state",
        ):
            mutated = copy.deepcopy(VALID)
            mutated["fault_scenarios"][0][field] = None
            with self.subTest(non_nullable=field):
                self.assert_document_invalid(mutated)

    def test_exact_id_sets_reject_missing_extra_and_duplicate_members(self) -> None:
        for collection in EXPECTED_IDS:
            missing = copy.deepcopy(VALID)
            missing[collection].pop()
            extra = copy.deepcopy(VALID)
            extra[collection].append(copy.deepcopy(extra[collection][-1]))
            duplicate = copy.deepcopy(VALID)
            duplicate[collection][-1] = copy.deepcopy(duplicate[collection][0])
            for label, mutated in (
                ("missing", missing),
                ("extra", extra),
                ("duplicate", duplicate),
            ):
                with self.subTest(collection=collection, mutation=label):
                    self.assert_document_invalid(mutated)

    def test_connector_control_and_expansion_id_partitions_are_strict(self) -> None:
        substitutions = (
            ("connectors", "program-control"),
            ("connectors", "expansion-i2c-control"),
            ("controls", "rear-usb-c"),
            ("controls", "expansion-spi"),
            ("expansion_interfaces", "rear-usb-c"),
            ("expansion_interfaces", "status-led"),
        )
        for collection, foreign_id in substitutions:
            mutated = copy.deepcopy(VALID)
            mutated[collection][0]["id"] = foreign_id
            with self.subTest(collection=collection, foreign_id=foreign_id):
                self.assert_document_invalid(mutated)

    def test_every_fault_has_exact_locked_outcomes(self) -> None:
        fields = ("path_select", "tx_state", "output_payload", "expansion_state")
        domains = (
            ("processed", "raw"),
            ("disabled", "enabled-pcm", "enabled-raw", "unchanged"),
            ("none", "pcm-silence-48k", "processed-pcm-48k", "raw-spdif", "unchanged"),
            ("high-impedance", "current-limited", "active", "unchanged"),
        )
        for fault_id, allowed_by_field in FAULT_OUTCOMES.items():
            index = self.record_index("fault_scenarios", fault_id)
            for field, domain, allowed in zip(fields, domains, allowed_by_field):
                for value in allowed:
                    mutated = copy.deepcopy(VALID)
                    mutated["fault_scenarios"][index][field] = value
                    with self.subTest(fault=fault_id, field=field, allowed=value):
                        validate(mutated, SCHEMA)
                rejected = next(value for value in domain if value not in allowed)
                mutated = copy.deepcopy(VALID)
                mutated["fault_scenarios"][index][field] = rejected
                with self.subTest(fault=fault_id, field=field, rejected=rejected):
                    self.assert_document_invalid(mutated)

    def test_fault_hardware_watchdog_and_measurement_contracts(self) -> None:
        cases = (
            ("teensy-absent", "hardware_pull_required", False),
            ("reset", "hardware_pull_required", False),
            ("boot", "watchdog_required", False),
            ("firmware-crash", "watchdog_required", False),
            ("firmware-crash", "residual_interval_measurement_gate", None),
            ("optical-carrier-loss", "debounce_measurement_gate", None),
        )
        for fault_id, field, value in cases:
            mutated = copy.deepcopy(VALID)
            index = self.record_index("fault_scenarios", fault_id)
            mutated["fault_scenarios"][index][field] = value
            with self.subTest(fault=fault_id, field=field):
                self.assert_document_invalid(mutated)

    def test_every_transition_has_exact_ordered_trigger_mapping(self) -> None:
        trigger_sets = tuple(TRANSITION_TRIGGERS.values())
        for index, (transition_id, expected) in enumerate(TRANSITION_TRIGGERS.items()):
            actual = tuple(VALID["transitions"][index]["triggers"])
            self.assertEqual(actual, expected)
            wrong = next(candidate for candidate in trigger_sets if candidate != expected)
            mutated = copy.deepcopy(VALID)
            mutated["transitions"][index]["triggers"] = list(wrong)
            with self.subTest(transition=transition_id, triggers=wrong):
                self.assert_document_invalid(mutated)

        reordered = copy.deepcopy(VALID)
        index = self.record_index("transitions", "active-to-unsupported")
        reordered["transitions"][index]["triggers"].reverse()
        self.assert_document_invalid(reordered)

        unknown = copy.deepcopy(VALID)
        unknown["transitions"][0]["triggers"] = ["unknown-trigger"]
        self.assert_document_invalid(unknown)

    def test_locked_safe_action_precondition_and_postcondition_tokens(self) -> None:
        safe_values = (
            "processed",
            "raw",
            "disabled",
            "enabled-pcm",
            "enabled-raw",
            "unchanged",
            "none",
            "pcm-silence-48k",
            "processed-pcm-48k",
            "raw-spdif",
            "high-impedance",
            "current-limited",
            "active",
            "asserted",
            "deasserted",
            "low",
            "high",
        )
        actions = (
            "disable-tx",
            "wait-muted",
            "select-processed",
            "select-raw",
            "clear-pipeline",
            "prime-pcm-silence",
            "enable-tx-pcm",
            "enable-tx-raw",
            "assert-reset",
            "hold-expansion-safe",
        )
        preconditions = (
            "rails-valid",
            "watchdog-armed",
            "pipeline-primed",
            "carrier-present",
            "format-supported",
            "output-muted",
            "tx-disabled",
            "diagnostic-authorized",
        )
        postconditions = (
            "processed-selected",
            "raw-selected",
            "tx-disabled",
            "tx-pcm-enabled",
            "tx-raw-enabled",
            "expansion-safe",
        )
        for token in safe_values:
            validate(token, SCHEMA["$defs"]["safeValue"], root_schema=SCHEMA)
        for token in actions:
            validate(token, SCHEMA["$defs"]["action"], root_schema=SCHEMA)
        for token in preconditions:
            validate(token, SCHEMA["$defs"]["precondition"], root_schema=SCHEMA)
        for token in postconditions:
            validate(token, SCHEMA["$defs"]["postcondition"], root_schema=SCHEMA)
        for definition in ("safeValue", "action", "precondition", "postcondition"):
            with self.subTest(definition=definition):
                self.assert_definition_invalid("not-a-token", definition)


if __name__ == "__main__":
    unittest.main()
