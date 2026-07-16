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
LOADED_VALID = _load_json(FIXTURE_ROOT / "valid.json")

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
            "from_states",
            "to_states",
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
        "rtc-backup",
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

POWER_DOMAIN_IDENTIFIER_ENUM = (
    "usb-vbus-raw",
    "protected-5v",
    "teensy-3v3",
    "rtc-backup",
    "digital-ground",
    "chassis-shield",
    "expansion-3v3",
    "expansion-usb-host-vbus",
)

PRE_BUILDER_POWER_DOMAIN_IDS = (
    "usb-vbus-raw",
    "protected-5v",
    "teensy-3v3",
    "digital-ground",
    "chassis-shield",
    "expansion-3v3",
    "expansion-usb-host-vbus",
)

PRE_BUILDER_MICRO_B = {
    "id": "teensy-micro-b",
    "location": "module",
    "role": "data",
    "signals": ["USB2-D+", "USB2-D-"],
    "power_domains": ["digital-ground"],
    "mating_constraint": "No second VIN or VUSB power feed is fitted.",
    "evidence_ids": ["PJRC-TEENSY41-SCHEMATIC"],
    "notes": "Module USB data access.",
}

POST_BUILDER_MICRO_B = {
    "id": "teensy-micro-b",
    "location": "module",
    "role": "power-data",
    "signals": ["USB2-D+", "USB2-D-", "VBUS"],
    "power_domains": ["protected-5v", "digital-ground"],
    "mating_constraint": (
        "The internal USB cable is the sole Teensy VUSB feed; no VIN or second "
        "VUSB feed is fitted."
    ),
    "evidence_ids": ["PJRC-TEENSY41-SCHEMATIC"],
    "notes": "Module USB power-and-data entry.",
}

PRE_BUILDER_PROTECTED_5V = {
    "id": "protected-5v",
    "source": "power-entry",
    "consumers": ["teensy", "internal-usb-a"],
    "backfeed_rule": "Reverse current to raw VBUS is blocked.",
    "evidence_ids": ["PJRC-TEENSY41-SCHEMATIC"],
    "notes": "Protected internal 5 V rail.",
}

POST_BUILDER_PROTECTED_5V = {
    "id": "protected-5v",
    "source": "power-entry",
    "consumers": ["teensy", "internal-usb-a", "teensy-micro-b"],
    "backfeed_rule": "Reverse current to raw VBUS is blocked.",
    "evidence_ids": ["PJRC-TEENSY41-SCHEMATIC"],
    "notes": "Protected internal 5 V rail.",
}

PRE_BUILDER_REMOTE_RTC = {
    "id": "remote-program-rtc",
    "location": "rear",
    "role": "control",
    "signals": ["PROGRAM", "RTC-BATTERY"],
    "power_domains": ["teensy-3v3", "digital-ground"],
    "mating_constraint": "Service actuation does not require enclosure removal.",
    "evidence_ids": ["PJRC-TEENSY41-PINOUT"],
    "notes": "Remote program and RTC service interface.",
}

POST_BUILDER_REMOTE_RTC = {
    "id": "remote-program-rtc",
    "location": "rear",
    "role": "control",
    "signals": ["PROGRAM", "RTC-BATTERY"],
    "power_domains": ["teensy-3v3", "rtc-backup", "digital-ground"],
    "mating_constraint": "Service actuation does not require enclosure removal.",
    "evidence_ids": [
        "PJRC-TEENSY41-PINOUT",
        "PJRC-TEENSY41-SCHEMATIC",
    ],
    "notes": "Remote program and RTC service interface.",
}

POST_BUILDER_RTC_BACKUP = {
    "id": "rtc-backup",
    "source": "remote-program-rtc",
    "consumers": ["teensy"],
    "backfeed_rule": (
        "The RTC backup input is isolated from teensy-3v3 and cannot power the core."
    ),
    "evidence_ids": [
        "PJRC-TEENSY41-PINOUT",
        "PJRC-TEENSY41-SCHEMATIC",
    ],
    "notes": "External RTC backup battery domain.",
}


def _locked_row_index(
    rows: list[dict[str, object]],
    row_id: str,
    expected: dict[str, object],
) -> int:
    matches = [index for index, row in enumerate(rows) if row.get("id") == row_id]
    if len(matches) != 1:
        raise ValueError(f"expected exactly one {row_id!r} row")
    index = matches[0]
    row = rows[index]
    if tuple(row) != tuple(expected) or row != expected:
        raise ValueError(f"{row_id!r} does not match its locked handoff value")
    return index


def _migrate_usb_rtc_fixture(document: object) -> dict[str, object]:
    if not isinstance(document, dict):
        raise ValueError("interface fixture must be an object")
    migrated = copy.deepcopy(document)
    connectors = migrated.get("connectors")
    power_domains = migrated.get("power_domains")
    if not isinstance(connectors, list) or not all(
        isinstance(row, dict) for row in connectors
    ):
        raise ValueError("connectors must be an array of objects")
    if not isinstance(power_domains, list) or not all(
        isinstance(row, dict) for row in power_domains
    ):
        raise ValueError("power_domains must be an array of objects")
    if tuple(row.get("id") for row in connectors) != EXPECTED_IDS["connectors"]:
        raise ValueError("connectors do not have the exact locked ID order")

    power_domain_ids = tuple(row.get("id") for row in power_domains)
    if power_domain_ids == PRE_BUILDER_POWER_DOMAIN_IDS:
        micro_b_index = _locked_row_index(
            connectors, "teensy-micro-b", PRE_BUILDER_MICRO_B
        )
        remote_rtc_index = _locked_row_index(
            connectors, "remote-program-rtc", PRE_BUILDER_REMOTE_RTC
        )
        protected_5v_index = _locked_row_index(
            power_domains, "protected-5v", PRE_BUILDER_PROTECTED_5V
        )
        teensy_3v3_index = next(
            index
            for index, row in enumerate(power_domains)
            if row["id"] == "teensy-3v3"
        )
        connectors[micro_b_index] = copy.deepcopy(POST_BUILDER_MICRO_B)
        connectors[remote_rtc_index] = copy.deepcopy(POST_BUILDER_REMOTE_RTC)
        power_domains[protected_5v_index] = copy.deepcopy(POST_BUILDER_PROTECTED_5V)
        power_domains.insert(
            teensy_3v3_index + 1,
            copy.deepcopy(POST_BUILDER_RTC_BACKUP),
        )
    elif power_domain_ids != POWER_DOMAIN_IDENTIFIER_ENUM:
        raise ValueError("power_domains do not match a locked handoff ID order")

    _locked_row_index(connectors, "teensy-micro-b", POST_BUILDER_MICRO_B)
    _locked_row_index(connectors, "remote-program-rtc", POST_BUILDER_REMOTE_RTC)
    _locked_row_index(power_domains, "protected-5v", POST_BUILDER_PROTECTED_5V)
    _locked_row_index(power_domains, "rtc-backup", POST_BUILDER_RTC_BACKUP)
    return migrated


VALID = _migrate_usb_rtc_fixture(LOADED_VALID)

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

TRANSITION_ENDPOINTS = {
    "power-on-to-reset": (("power-off",), ("powered-reset",)),
    "reset-to-boot": (("powered-reset",), ("boot-muted",)),
    "boot-to-waiting": (("boot-muted",), ("waiting-for-carrier",)),
    "waiting-to-pcm": (("waiting-for-carrier",), ("pcm-active",)),
    "waiting-to-dts": (("waiting-for-carrier",), ("dts-active",)),
    "active-to-carrier-loss": (
        ("pcm-active", "dts-active", "unsupported-muted", "diagnostic-bypass"),
        ("waiting-for-carrier",),
    ),
    "active-to-unsupported": (
        ("pcm-active", "dts-active"),
        ("unsupported-muted",),
    ),
    "unsupported-to-active": (
        ("unsupported-muted",),
        ("pcm-active", "dts-active"),
    ),
    "active-to-diagnostic-bypass": (
        ("pcm-active", "dts-active", "unsupported-muted"),
        ("diagnostic-bypass",),
    ),
    "diagnostic-bypass-to-waiting": (
        ("diagnostic-bypass",),
        ("waiting-for-carrier",),
    ),
    "watchdog-to-reset": (
        (
            "boot-muted",
            "waiting-for-carrier",
            "pcm-active",
            "dts-active",
            "unsupported-muted",
            "diagnostic-bypass",
            "fault-muted",
        ),
        ("powered-reset",),
    ),
    "usb-unplug-to-power-off": (
        (
            "powered-reset",
            "boot-muted",
            "waiting-for-carrier",
            "pcm-active",
            "dts-active",
            "unsupported-muted",
            "diagnostic-bypass",
            "fault-muted",
        ),
        ("power-off",),
    ),
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

    def assert_power_domain_schema_contract(self, schema: object) -> None:
        definition = schema["$defs"]["powerDomains"]
        self.assertEqual(8, definition["minItems"])
        self.assertEqual(8, definition["maxItems"])
        contains_ids = tuple(
            clause["contains"]["properties"]["id"]["const"]
            for clause in definition["allOf"]
        )
        self.assertEqual(contains_ids, POWER_DOMAIN_IDENTIFIER_ENUM)
        self.assertEqual(
            tuple(schema["$defs"]["powerDomainIdentifier"]["enum"]),
            POWER_DOMAIN_IDENTIFIER_ENUM,
        )

    def test_committed_schema_and_canonical_fixture(self) -> None:
        check_schema(SCHEMA)
        validate(VALID, SCHEMA)
        self.assertEqual(VALID["schema_version"], "1.0.0")
        self.assertEqual(1, SCHEMA["$defs"]["evidenceIdentifiers"]["minItems"])

    def test_fixture_helper_migrates_pre_builder_and_noops_post_builder(self) -> None:
        loaded_before = copy.deepcopy(LOADED_VALID)
        canonical_post = _migrate_usb_rtc_fixture(LOADED_VALID)
        self.assertEqual(LOADED_VALID, loaded_before)
        self.assertEqual(VALID, canonical_post)

        post_builder = copy.deepcopy(canonical_post)
        post_builder_before = copy.deepcopy(post_builder)
        post_result = _migrate_usb_rtc_fixture(post_builder)
        self.assertEqual(post_builder, post_builder_before)
        self.assertEqual(post_result, post_builder_before)
        self.assertIsNot(post_result, post_builder)

        pre_builder = copy.deepcopy(canonical_post)
        connector_index = {
            row["id"]: index for index, row in enumerate(pre_builder["connectors"])
        }
        pre_builder["connectors"][connector_index["teensy-micro-b"]] = (
            copy.deepcopy(PRE_BUILDER_MICRO_B)
        )
        pre_builder["connectors"][connector_index["remote-program-rtc"]] = (
            copy.deepcopy(PRE_BUILDER_REMOTE_RTC)
        )
        pre_builder["power_domains"] = [
            row for row in pre_builder["power_domains"] if row["id"] != "rtc-backup"
        ]
        protected_index = next(
            index
            for index, row in enumerate(pre_builder["power_domains"])
            if row["id"] == "protected-5v"
        )
        pre_builder["power_domains"][protected_index] = copy.deepcopy(
            PRE_BUILDER_PROTECTED_5V
        )
        pre_builder_before = copy.deepcopy(pre_builder)
        migrated = _migrate_usb_rtc_fixture(pre_builder)
        self.assertEqual(pre_builder, pre_builder_before)
        self.assertEqual(migrated, canonical_post)
        validate(migrated, SCHEMA)

        seven_row_loaded_valid_equivalent = copy.deepcopy(pre_builder_before)
        seven_row_before = copy.deepcopy(seven_row_loaded_valid_equivalent)
        module_valid_from_seven_rows = _migrate_usb_rtc_fixture(
            seven_row_loaded_valid_equivalent
        )
        self.assertEqual(seven_row_loaded_valid_equivalent, seven_row_before)
        self.assertEqual(module_valid_from_seven_rows, canonical_post)

        wrong_ids = copy.deepcopy(pre_builder)
        wrong_ids["power_domains"][3]["id"] = "unexpected-domain"
        with self.assertRaises(ValueError):
            _migrate_usb_rtc_fixture(wrong_ids)

        wrong_pre_value = copy.deepcopy(pre_builder)
        wrong_pre_value["connectors"][connector_index["teensy-micro-b"]][
            "notes"
        ] = "Drifted pre-builder value."
        with self.assertRaises(ValueError):
            _migrate_usb_rtc_fixture(wrong_pre_value)

        wrong_post_value = copy.deepcopy(canonical_post)
        rtc_index = next(
            index
            for index, row in enumerate(wrong_post_value["power_domains"])
            if row["id"] == "rtc-backup"
        )
        wrong_post_value["power_domains"][rtc_index][
            "backfeed_rule"
        ] = "Drifted post-builder value."
        with self.assertRaises(ValueError):
            _migrate_usb_rtc_fixture(wrong_post_value)

    def test_rtc_backup_power_domain_forward_migration_contract(self) -> None:
        migrated = copy.deepcopy(VALID)
        self.assert_power_domain_schema_contract(SCHEMA)
        validate(migrated, SCHEMA)

        seven_rows = copy.deepcopy(migrated)
        seven_rows["power_domains"].pop(
            self.record_index("power_domains", "rtc-backup")
        )
        self.assert_document_invalid(seven_rows)
        nine_rows = copy.deepcopy(migrated)
        nine_rows["power_domains"].append(
            copy.deepcopy(nine_rows["power_domains"][-1])
        )
        self.assert_document_invalid(nine_rows)

        contract_mutations = []
        omitted_enum = copy.deepcopy(SCHEMA)
        omitted_enum["$defs"]["powerDomainIdentifier"]["enum"].remove("rtc-backup")
        contract_mutations.append(omitted_enum)
        reordered_enum = copy.deepcopy(SCHEMA)
        enum = reordered_enum["$defs"]["powerDomainIdentifier"]["enum"]
        enum[3], enum[4] = enum[4], enum[3]
        contract_mutations.append(reordered_enum)
        for bound in ("minItems", "maxItems"):
            for value in (7, 9):
                wrong_bound = copy.deepcopy(SCHEMA)
                wrong_bound["$defs"]["powerDomains"][bound] = value
                contract_mutations.append(wrong_bound)
        missing_contains = copy.deepcopy(SCHEMA)
        missing_contains["$defs"]["powerDomains"]["allOf"].pop(3)
        contract_mutations.append(missing_contains)
        reordered_contains = copy.deepcopy(SCHEMA)
        contains = reordered_contains["$defs"]["powerDomains"]["allOf"]
        contains[3], contains[4] = contains[4], contains[3]
        contract_mutations.append(reordered_contains)
        for mutated in contract_mutations:
            with self.assertRaises(AssertionError):
                self.assert_power_domain_schema_contract(mutated)

        connector = next(
            row
            for row in migrated["connectors"]
            if row["id"] == "remote-program-rtc"
        )
        unknown_connector = copy.deepcopy(connector)
        unknown_connector["power_domains"][1] = "unknown-rail"
        self.assert_definition_invalid(unknown_connector, "connector")
        rtc_domain = next(
            row for row in migrated["power_domains"] if row["id"] == "rtc-backup"
        )
        unknown_domain = copy.deepcopy(rtc_domain)
        unknown_domain["id"] = "unknown-rail"
        self.assert_definition_invalid(unknown_domain, "powerDomain")

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
            "invalid-transition-legacy-endpoints.json": "transition",
            "invalid-transition-empty-endpoints.json": "transition",
            "invalid-transition-duplicate-endpoints.json": "transition",
            "invalid-evidence-empty.json": "connector",
            "invalid-additional-property.json": "operatingState",
            "invalid-missing-required.json": "control",
            "invalid-nullability.json": "faultScenario",
            "invalid-array-duplicate.json": "connector",
        }
        for name, definition in fixtures.items():
            with self.subTest(name=name):
                self.assert_definition_invalid(_load_json(FIXTURE_ROOT / name), definition)

        wrong_order = _load_json(FIXTURE_ROOT / "invalid-transition-wrong-key-order.json")
        validate(wrong_order, SCHEMA["$defs"]["transition"], root_schema=SCHEMA)
        self.assertNotEqual(
            tuple(wrong_order),
            COLLECTIONS["transitions"][1],
        )

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
            ("transitions", 0, "from_states", ["unknown-state"]),
            ("transitions", 0, "to_states", ["unknown-state"]),
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
            ("transitions", 5, "from_states", "pcm-active"),
            ("transitions", 7, "to_states", "pcm-active"),
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
            ("transitions", "from_states"),
            ("transitions", "to_states"),
            ("transitions", "actions"),
            ("transitions", "postconditions"),
        ):
            mutated = copy.deepcopy(VALID)
            mutated[collection][0][field] = []
            with self.subTest(collection=collection, empty=field):
                self.assert_document_invalid(mutated)

        permitted_empty_arrays = (
            ("connectors", 0, "power_domains"),
            ("expansion_interfaces", 0, "power_domains"),
            ("power_domains", 0, "consumers"),
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

        for collection in (
            "connectors",
            "controls",
            "expansion_interfaces",
            "power_domains",
        ):
            mutated = copy.deepcopy(VALID)
            mutated[collection][0]["evidence_ids"] = []
            with self.subTest(collection=collection, empty="evidence_ids"):
                self.assert_document_invalid(mutated)

        root_evidence = copy.deepcopy(VALID)
        root_evidence["evidence_ids"] = []
        self.assert_document_invalid(root_evidence)

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

    def test_every_transition_has_exact_ordered_endpoint_sets(self) -> None:
        for transition_id, (from_states, to_states) in TRANSITION_ENDPOINTS.items():
            index = self.record_index("transitions", transition_id)
            transition = VALID["transitions"][index]
            self.assertEqual(from_states, tuple(transition["from_states"]))
            self.assertEqual(to_states, tuple(transition["to_states"]))

            for field, expected in (
                ("from_states", from_states),
                ("to_states", to_states),
            ):
                mutated = copy.deepcopy(VALID)
                replacement = ["power-off"]
                if tuple(replacement) == expected:
                    replacement = ["fault-muted"]
                mutated["transitions"][index][field] = replacement
                with self.subTest(transition=transition_id, field=field):
                    self.assert_document_invalid(mutated)

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
