#!/usr/bin/env python3
"""Validate the staged Aius P1C topology and selection contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from jsonschema_subset import DRAFT_2020_12, SchemaError, ValidationError, check_schema, validate
from build_p1c_architecture_contracts import (
    build_functional_contract,
    build_interfaces_contract,
)
from render_p1 import (
    render_components,
    render_functional_topology,
    render_interfaces_and_states,
    render_supplemental_requests,
)


STAGES = (
    "contract",
    "architecture-base",
    "architecture",
    "selection-fragments",
    "selection",
    "handoff",
)

FUNCTIONAL_SCHEMA = Path("evidence/design/functional-topology-schema.json")
INTERFACES_SCHEMA = Path("evidence/design/interfaces-and-states-schema.json")
COMPONENT_SCHEMA = Path("evidence/design/component-schema.json")
POLICY_SCHEMA = Path("evidence/design/selection-policy-schema.json")
REQUEST_SCHEMA = Path("evidence/manifest/source-request-schema.json")

FUNCTIONAL = Path("evidence/design/functional-topology.json")
INTERFACES = Path("evidence/design/interfaces-and-states.json")
POLICY = Path("evidence/design/selection-policy.json")
SOURCES = Path("evidence/manifest/sources.json")
COMPONENT_DIRECTORY = Path("evidence/design/components")
REQUEST_DIRECTORY = Path("evidence/manifest/requests")
FUNCTIONAL_MARKDOWN = Path("docs/reference/functional-topology.md")
INTERFACES_MARKDOWN = Path("docs/reference/interfaces-and-states.md")
COMPONENT_CSV = Path("evidence/design/provisional-components.csv")
REQUEST_AGGREGATE = Path("evidence/manifest/supplemental-requests.json")

FUNCTIONAL_CONTRACT = Path("evidence/design/contracts/functional-topology.json")
INTERFACES_CONTRACT = Path("evidence/design/contracts/interfaces-and-states.json")
FUNCTIONAL_BASE = Path("scripts/fixtures/p1c-schema/functional-topology/base-v1.json")
INTERFACES_BASE = Path("scripts/fixtures/p1c-schema/interfaces-and-states/base-v1.json")
FUNCTIONAL_BASE_HASH = "17e07765a7ef2de03e0722f3b393f8dafde48ec2d854b381c47d51073b0d3257"
INTERFACES_BASE_HASH = "31ea644c922515c0fa6193e67a7432b8b911083af96d3290a1b34959800fcd84"

FUNCTIONAL_ROOT_KEYS = (
    "schema_version",
    "modes",
    "blocks",
    "flows",
    "clock_domains",
    "buffers",
    "latency_boundaries",
    "evidence_ids",
)
FUNCTIONAL_RECORDS = {
    "modes": (
        "mode",
        ("id", "input_formats", "output_format", "active_blocks", "error_mode", "evidence_ids", "notes"),
    ),
    "blocks": (
        "block",
        ("id", "kind", "owner", "power_domain", "clock_domain", "evidence_ids", "notes"),
    ),
    "flows": (
        "flow",
        ("id", "kind", "source_id", "sink_id", "payload", "enabled_modes", "control_id", "evidence_ids", "notes"),
    ),
    "clock_domains": (
        "clockDomain",
        ("id", "source_id", "consumer_ids", "synchronous_with", "frequency_status", "evidence_ids", "notes"),
    ),
    "buffers": (
        "buffer",
        ("id", "owner", "producer", "consumer", "storage_class", "overflow_policy", "measurement_gate", "notes"),
    ),
    "latency_boundaries": (
        "latencyBoundary",
        ("id", "source", "sink", "budget_status", "measurement_gate", "notes"),
    ),
}

INTERFACES_ROOT_KEYS = (
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
INTERFACES_RECORDS = {
    "connectors": (
        "connector",
        ("id", "location", "role", "signals", "power_domains", "mating_constraint", "evidence_ids", "notes"),
    ),
    "controls": (
        "control",
        ("id", "owner", "active_level", "reset_default", "safe_value", "evidence_ids", "notes"),
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
        ("id", "entry_condition", "path_select", "tx_state", "output_payload", "expansion_state", "exit_condition", "notes"),
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
        ("id", "from_states", "to_states", "triggers", "preconditions", "actions", "postconditions", "notes"),
    ),
}

POLICY_ROOT_KEYS = ("schema_version", "engineering_policies", "requirements", "families")
POLICY_RECORDS = {
    "engineering_policies": (
        "engineeringPolicy",
        ("id", "scope", "rule", "validation_gate", "notes"),
    ),
    "requirements": (
        "requirement",
        ("id", "statement", "basis_ids", "verification_stage", "notes"),
    ),
    "families": (
        "family",
        (
            "family",
            "component_file",
            "critical",
            "generic_eligible",
            "allowed_statuses",
            "requirements",
            "manufacturer_request_id",
            "availability_request_id",
            "manufacturer_source_id",
            "availability_source_id",
            "dependent_feature",
            "notes",
        ),
    ),
}

COMPONENT_KEYS = (
    "block",
    "requirements",
    "manufacturer",
    "mpn",
    "alternatives",
    "selection_rationale",
    "authority_url",
    "manufacturer_request_id",
    "availability_request_id",
    "generic_policy",
    "status",
    "notes",
)
ALTERNATIVE_KEYS = ("manufacturer", "mpn", "selection_rationale", "authority_url")
GENERIC_POLICY_KEYS = ("value", "package", "tolerance")
REQUEST_KEYS = (
    "request_id",
    "request_kind",
    "family",
    "requirements",
    "authority_url",
    "selected_manufacturer",
    "selected_mpn",
    "source_id",
    "artifact_kinds",
    "retention",
    "status",
    "evidence_ids",
    "notes",
)

SCHEMA_SPECS = {
    FUNCTIONAL_SCHEMA: (FUNCTIONAL_ROOT_KEYS, FUNCTIONAL_RECORDS, True),
    INTERFACES_SCHEMA: (INTERFACES_ROOT_KEYS, INTERFACES_RECORDS, True),
    COMPONENT_SCHEMA: (
        COMPONENT_KEYS,
        {
            "alternatives": ("alternative", ALTERNATIVE_KEYS),
            "generic_policy": ("genericPolicy", GENERIC_POLICY_KEYS),
        },
        False,
    ),
    POLICY_SCHEMA: (POLICY_ROOT_KEYS, POLICY_RECORDS, True),
    REQUEST_SCHEMA: (REQUEST_KEYS, {}, False),
}

FUNCTIONAL_IDS = {
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
    "latency_boundaries": ("capture", "burst-assembly", "decode", "dsp", "transmit-queue"),
}

INTERFACES_IDS = {
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

TEENSY_USB_CONTRACT = {
    "location": "module",
    "role": "power-data",
    "signals": ("USB2-D+", "USB2-D-", "VBUS"),
    "power_domains": ("protected-5v", "digital-ground"),
    "mating_constraint": "The internal USB cable is the sole Teensy VUSB feed; no VIN or second VUSB feed is fitted.",
    "evidence_ids": ("PJRC-TEENSY41-SCHEMATIC",),
    "notes": "Module USB power-and-data entry.",
}
PROTECTED_5V_CONSUMERS = ("teensy", "internal-usb-a", "teensy-micro-b")
REMOTE_PROGRAM_RTC_CONTRACT = {
    "power_domains": ("teensy-3v3", "rtc-backup", "digital-ground"),
    "evidence_ids": ("PJRC-TEENSY41-PINOUT", "PJRC-TEENSY41-SCHEMATIC"),
}
RTC_BACKUP_CONTRACT = {
    "source": "remote-program-rtc",
    "consumers": ("teensy",),
    "backfeed_rule": "The RTC backup input is isolated from teensy-3v3 and cannot power the core.",
    "evidence_ids": ("PJRC-TEENSY41-PINOUT", "PJRC-TEENSY41-SCHEMATIC"),
    "notes": "External RTC backup battery domain.",
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
    "teensy-absent": ("processed", "disabled", "none", ("high-impedance", "current-limited")),
    "boot": ("processed", "disabled", "none", ("high-impedance", "current-limited")),
    "reset": ("processed", "disabled", "none", ("high-impedance", "current-limited")),
    "brownout": ("processed", "disabled", "none", ("high-impedance", "current-limited")),
    "firmware-crash": ("processed", "disabled", "none", ("high-impedance", "current-limited")),
    "optical-carrier-loss": ("processed", "disabled", "none", ("unchanged",)),
    "unsupported-burst": ("processed", "enabled-pcm", "pcm-silence-48k", ("unchanged",)),
    "malformed-input": ("processed", "enabled-pcm", "pcm-silence-48k", ("unchanged",)),
    "usb-unplug": ("processed", "disabled", "none", ("high-impedance", "current-limited")),
    "raw-bypass-request": ("raw", "enabled-raw", "raw-spdif", ("unchanged",)),
    "unpowered-expansion": ("processed", "unchanged", "unchanged", ("high-impedance", "current-limited")),
}

FAULT_REQUIRED_VALUES = {
    "teensy-absent": {"hardware_pull_required": True},
    "boot": {"watchdog_required": True},
    "reset": {"hardware_pull_required": True},
    "firmware-crash": {"watchdog_required": True, "residual_interval_measurement_gate": "P7"},
}

CRITICAL_FAMILIES = (
    "usb-c-receptacle",
    "usb-a-receptacle",
    "internal-usb-cable",
    "usb-esd",
    "usb-power-protection",
    "cliff-rx",
    "cliff-tx",
    "spdif-mux",
    "spdif-tx-gate",
    "ethernet-magjack",
    "teensy-side-socket",
    "teensy-ethernet-socket",
    "teensy-usb-host-socket",
    "expansion-power-protection",
    "expansion-i2c-isolator",
)
NONCRITICAL_FAMILIES = (
    "expansion-i2c-control",
    "expansion-spi",
    "expansion-uart-debug",
    "expansion-i2s-tdm",
    "expansion-usb-host",
    "expansion-gpio-adc",
    "remote-program-rtc",
    "test-fixture-header",
    "mechanical-cable-retainer",
)
GENERIC_FAMILIES = (
    "usb-c-cc-pulldown",
    "logic-decoupling-capacitor",
    "logic-control-pulldown",
    "ethernet-center-tap-capacitor",
    "expansion-io-series-resistor",
)
FAMILIES = CRITICAL_FAMILIES + NONCRITICAL_FAMILIES + GENERIC_FAMILIES

SELECTED_STATUSES = (
    "candidate-pending-evidence",
    "selected",
    "candidate-evidence-unavailable",
)
CRITICAL_ALLOWED = SELECTED_STATUSES
NONCRITICAL_ALLOWED = SELECTED_STATUSES + ("unresolved",)
GENERIC_ALLOWED = ("generic-allowance",)

FLOW_KINDS = {
    **{identifier: "data" for identifier in FUNCTIONAL_IDS["flows"][:12]},
    **{identifier: "control" for identifier in FUNCTIONAL_IDS["flows"][12:15]},
    **{identifier: "power" for identifier in FUNCTIONAL_IDS["flows"][15:]},
}


@dataclass(frozen=True, order=True)
class Diagnostic:
    code: str
    path: str
    detail: str

    def render(self) -> str:
        return f"AIUS-P1-{self.code}: {self.path}: {self.detail}"


@dataclass
class JsonInput:
    value: object | None
    parsed: bool


class Validator:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.diagnostics: set[Diagnostic] = set()
        self.json_inputs: dict[Path, JsonInput] = {}
        self.schemas: dict[Path, object] = {}
        self.schema_usable: dict[Path, bool] = {}
        self.instance_valid: dict[Path, bool] = {}
        self.normative_contracts: dict[Path, object] = {}
        self.sources_by_id: dict[str, Mapping[str, object]] | None = None
        self.policy_families: dict[str, Mapping[str, object]] | None = None
        self.components: dict[str, Mapping[str, object]] = {}
        self.requests: dict[str, Mapping[str, object]] = {}
        self.selection_fragments_ready = False

    def add(self, code: str, path: Path, detail: str) -> None:
        self.diagnostics.add(Diagnostic(code, path.as_posix(), detail))

    def load_json(self, path: Path) -> JsonInput:
        if path in self.json_inputs:
            return self.json_inputs[path]
        absolute = self.root / path
        try:
            value = json.loads(absolute.read_text(encoding="utf-8"))
        except FileNotFoundError:
            result = JsonInput(None, False)
            self.add("MISSING", path, "required input is missing")
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            result = JsonInput(None, False)
            self.add("PARSE", path, str(error))
        else:
            result = JsonInput(value, True)
        self.json_inputs[path] = result
        return result

    def load_schemas(self) -> None:
        for path, (root_keys, records, has_version) in SCHEMA_SPECS.items():
            loaded = self.load_json(path)
            usable = False
            if loaded.parsed:
                schema = loaded.value
                try:
                    check_schema(schema)
                except SchemaError as error:
                    self.add("SCHEMA", path, str(error))
                else:
                    usable = True
                    self.schemas[path] = schema
                    if not isinstance(schema, dict) or schema.get("$schema") != DRAFT_2020_12:
                        self.add("SCHEMA", path, "schema must declare Draft 2020-12")
                        usable = False
                    if has_version and not self._has_locked_version(schema):
                        self.add("SCHEMA", path, "schema_version must be the constant 1.0.0")
                        usable = False
                    self._check_schema_order(path, schema, root_keys, records)
            self.schema_usable[path] = usable

    @staticmethod
    def _has_locked_version(schema: object) -> bool:
        return (
            isinstance(schema, dict)
            and isinstance(schema.get("properties"), dict)
            and schema["properties"].get("schema_version") == {"const": "1.0.0"}
        )

    def _check_schema_order(
        self,
        path: Path,
        schema: object,
        root_keys: Sequence[str],
        records: Mapping[str, tuple[str, Sequence[str]]],
    ) -> None:
        if not isinstance(schema, dict):
            return
        self._check_declared_keys(path, "root", schema, root_keys)
        definitions = schema.get("$defs")
        if not isinstance(definitions, dict):
            return
        for _, (definition, keys) in records.items():
            record_schema = definitions.get(definition)
            if isinstance(record_schema, dict):
                self._check_declared_keys(path, f"$defs.{definition}", record_schema, keys)

    def _check_declared_keys(
        self, path: Path, label: str, schema: Mapping[str, object], expected: Sequence[str]
    ) -> None:
        required = schema.get("required")
        properties = schema.get("properties")
        actual_required = tuple(required) if isinstance(required, list) else ()
        actual_properties = tuple(properties) if isinstance(properties, dict) else ()
        if actual_required != tuple(expected) or actual_properties != tuple(expected):
            self.add("ORDER", path, f"{label} required/property order differs from the locked contract")

    def validate_instance(self, path: Path, schema_path: Path) -> object | None:
        loaded = self.load_json(path)
        if not loaded.parsed or not self.schema_usable.get(schema_path, False):
            self.instance_valid[path] = False
            return None
        try:
            validate(loaded.value, self.schemas[schema_path])
        except ValidationError as error:
            self.add("INSTANCE", path, str(error))
            self.instance_valid[path] = False
            return None
        except SchemaError as error:
            self.add("SCHEMA", schema_path, str(error))
            self.instance_valid[path] = False
            return None
        self.instance_valid[path] = True
        return loaded.value

    def check_contracts(self) -> None:
        specifications = (
            (
                FUNCTIONAL_CONTRACT,
                FUNCTIONAL_SCHEMA,
                FUNCTIONAL_ROOT_KEYS,
                FUNCTIONAL_RECORDS,
                FUNCTIONAL_BASE,
                FUNCTIONAL_BASE_HASH,
                build_functional_contract,
            ),
            (
                INTERFACES_CONTRACT,
                INTERFACES_SCHEMA,
                INTERFACES_ROOT_KEYS,
                INTERFACES_RECORDS,
                INTERFACES_BASE,
                INTERFACES_BASE_HASH,
                build_interfaces_contract,
            ),
        )
        for contract_path, schema_path, root_keys, records, base_path, base_hash, builder in specifications:
            contract = self.validate_instance(contract_path, schema_path)
            if contract is not None:
                self.check_document_order(contract_path, contract, root_keys, records)
                self.normative_contracts[contract_path] = contract

            base = self._load_historical_base(base_path, base_hash)
            if base is None or contract is None:
                continue
            try:
                derived = builder(base)
            except (KeyError, TypeError, ValueError) as error:
                self.add("CONTRACT", base_path, f"pure builder rejected locked base: {error}")
                continue
            if derived != contract:
                self.add("CONTRACT", contract_path, "normative value differs from pure builder derivation")

    def _load_historical_base(self, path: Path, expected_hash: str) -> object | None:
        try:
            raw = (self.root / path).read_bytes()
        except FileNotFoundError:
            self.add("MISSING", path, "required immutable base is missing")
            return None
        except OSError as error:
            self.add("PARSE", path, str(error))
            return None

        actual_hash = hashlib.sha256(raw).hexdigest()
        hash_valid = actual_hash == expected_hash
        if not hash_valid:
            self.add(
                "CONTRACT",
                path,
                f"raw SHA-256 {actual_hash} does not match locked {expected_hash}",
            )
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError as error:
            self.add("PARSE", path, f"invalid UTF-8: {error}")
            return None
        try:
            parsed = json.loads(text)
        except json.JSONDecodeError as error:
            self.add("PARSE", path, f"malformed historical JSON: {error}")
            return None
        return parsed if hash_valid else None

    def check_document_order(
        self,
        path: Path,
        document: object,
        root_keys: Sequence[str],
        records: Mapping[str, tuple[str, Sequence[str]]],
    ) -> None:
        if not isinstance(document, dict):
            return
        if tuple(document) != tuple(root_keys):
            self.add("ORDER", path, "root key order differs from the locked contract")
        for collection, (_, keys) in records.items():
            rows = document.get(collection)
            if not isinstance(rows, list):
                continue
            for index, row in enumerate(rows):
                if isinstance(row, dict) and tuple(row) != tuple(keys):
                    self.add("ORDER", path, f"{collection}[{index}] key order differs from the locked contract")

    def check_ids(self, path: Path, document: object, expected: Mapping[str, Sequence[str]]) -> None:
        if not isinstance(document, dict):
            return
        for collection, identifiers in expected.items():
            rows = document.get(collection)
            if not isinstance(rows, list):
                continue
            actual = tuple(row.get("id") for row in rows if isinstance(row, dict))
            if actual != tuple(identifiers):
                self.add("IDSET", path, f"{collection} ID membership/order differs from the locked contract")

    def check_architecture(self) -> tuple[object | None, object | None]:
        functional = self.validate_instance(FUNCTIONAL, FUNCTIONAL_SCHEMA)
        interfaces = self.validate_instance(INTERFACES, INTERFACES_SCHEMA)
        sources_input = self.load_json(SOURCES)

        if functional is not None:
            self.check_document_order(FUNCTIONAL, functional, FUNCTIONAL_ROOT_KEYS, FUNCTIONAL_RECORDS)
            self.check_ids(FUNCTIONAL, functional, FUNCTIONAL_IDS)
            self._check_normative_contract(FUNCTIONAL, functional, FUNCTIONAL_CONTRACT)
            self._check_flow_contract(functional)
            self._check_parity(
                FUNCTIONAL_MARKDOWN,
                lambda: render_functional_topology(functional),
            )
        else:
            self._require_file(FUNCTIONAL_MARKDOWN)

        if interfaces is not None:
            self.check_document_order(INTERFACES, interfaces, INTERFACES_ROOT_KEYS, INTERFACES_RECORDS)
            self.check_ids(INTERFACES, interfaces, INTERFACES_IDS)
            self._check_normative_contract(INTERFACES, interfaces, INTERFACES_CONTRACT)
            self._check_usb_rtc_contract(interfaces)
            self._check_fault_and_transition_contract(interfaces)
            self._check_parity(
                INTERFACES_MARKDOWN,
                lambda: render_interfaces_and_states(interfaces),
            )
        else:
            self._require_file(INTERFACES_MARKDOWN)

        if functional is not None and interfaces is not None:
            self._check_references(functional, interfaces)
        if sources_input.parsed:
            self.sources_by_id = self._index_sources(sources_input.value)
            if self.sources_by_id is not None:
                if functional is not None:
                    self._check_document_evidence(FUNCTIONAL, functional)
                if interfaces is not None:
                    self._check_document_evidence(INTERFACES, interfaces)
        return functional, interfaces

    def _check_normative_contract(self, path: Path, document: object, contract_path: Path) -> None:
        contract = self.normative_contracts.get(contract_path)
        if contract is not None and document != contract:
            self.add("CONTRACT", path, f"ordered semantic value differs from {contract_path.as_posix()}")

    def _require_file(self, path: Path) -> bool:
        try:
            (self.root / path).read_bytes()
        except FileNotFoundError:
            self.add("MISSING", path, "required input is missing")
            return False
        except OSError as error:
            self.add("PARITY", path, str(error))
            return False
        return True

    def _check_parity(self, path: Path, renderer: object) -> None:
        try:
            actual = (self.root / path).read_bytes()
        except FileNotFoundError:
            self.add("MISSING", path, "required generated output is missing")
            return
        except OSError as error:
            self.add("PARITY", path, str(error))
            return
        try:
            expected = renderer()  # type: ignore[operator]
        except (KeyError, TypeError, ValueError) as error:
            self.add("PARITY", path, f"renderer rejected validated input: {error}")
            return
        if actual != expected:
            self.add("PARITY", path, "generated bytes differ from scripts/render_p1.py")

    def _check_flow_contract(self, functional: object) -> None:
        if not isinstance(functional, dict):
            return
        rows = functional.get("flows")
        if not isinstance(rows, list):
            return
        for row in rows:
            if not isinstance(row, dict):
                continue
            identifier = row.get("id")
            if identifier in FLOW_KINDS and row.get("kind") != FLOW_KINDS[identifier]:
                self.add("CONTRACT", FUNCTIONAL, f"flow {identifier!r} has the wrong locked kind")

    def _check_usb_rtc_contract(self, interfaces: object) -> None:
        if not isinstance(interfaces, dict):
            return
        connectors = {
            row.get("id"): row
            for row in interfaces.get("connectors", ())
            if isinstance(row, dict)
        }
        domains = {
            row.get("id"): row
            for row in interfaces.get("power_domains", ())
            if isinstance(row, dict)
        }

        teensy_usb = connectors.get("teensy-micro-b")
        if isinstance(teensy_usb, dict) and any(
            self._contract_value(teensy_usb.get(field)) != expected
            for field, expected in TEENSY_USB_CONTRACT.items()
        ):
            self.add(
                "CONTRACT",
                INTERFACES,
                "teensy-micro-b differs from the locked USB power-data contract",
            )

        protected_5v = domains.get("protected-5v")
        if isinstance(protected_5v, dict) and self._contract_value(
            protected_5v.get("consumers")
        ) != PROTECTED_5V_CONSUMERS:
            self.add(
                "CONTRACT",
                INTERFACES,
                "protected-5v consumers differ from the locked sole-feed contract",
            )

        remote_rtc = connectors.get("remote-program-rtc")
        if isinstance(remote_rtc, dict):
            for field, expected in REMOTE_PROGRAM_RTC_CONTRACT.items():
                if self._contract_value(remote_rtc.get(field)) != expected:
                    self.add(
                        "CONTRACT",
                        INTERFACES,
                        f"remote-program-rtc {field} differs from the locked RTC service contract",
                    )

        rtc_backup = domains.get("rtc-backup")
        if isinstance(rtc_backup, dict):
            for field, expected in RTC_BACKUP_CONTRACT.items():
                if self._contract_value(rtc_backup.get(field)) != expected:
                    self.add(
                        "CONTRACT",
                        INTERFACES,
                        f"rtc-backup {field} differs from the locked isolated backup-power contract",
                    )

        teensy_3v3 = domains.get("teensy-3v3")
        if isinstance(teensy_3v3, dict) and teensy_3v3.get("source") == "rtc-backup":
            self.add("CONTRACT", INTERFACES, "rtc-backup cannot source a core power domain")

    @staticmethod
    def _contract_value(value: object) -> object:
        return tuple(value) if isinstance(value, list) else value

    def _check_fault_and_transition_contract(self, interfaces: object) -> None:
        if not isinstance(interfaces, dict):
            return
        faults = interfaces.get("fault_scenarios")
        if isinstance(faults, list):
            for row in faults:
                if not isinstance(row, dict) or row.get("id") not in FAULT_OUTCOMES:
                    continue
                identifier = row["id"]
                expected = FAULT_OUTCOMES[identifier]
                actual = (row.get("path_select"), row.get("tx_state"), row.get("output_payload"))
                if actual != expected[:3] or row.get("expansion_state") not in expected[3]:
                    self.add("CONTRACT", INTERFACES, f"fault {identifier!r} safe outcome differs from the locked matrix")
                for field, value in FAULT_REQUIRED_VALUES.get(identifier, {}).items():
                    if row.get(field) != value:
                        self.add("CONTRACT", INTERFACES, f"fault {identifier!r} {field} differs from the locked matrix")
            optical_loss = next(
                (row for row in faults if isinstance(row, dict) and row.get("id") == "optical-carrier-loss"),
                None,
            )
            if isinstance(optical_loss, dict) and optical_loss.get("debounce_measurement_gate") not in {"P6", "P7"}:
                self.add("CONTRACT", INTERFACES, "optical-carrier-loss must retain a P6/P7 debounce measurement gate")

        transitions = interfaces.get("transitions")
        if isinstance(transitions, list):
            for row in transitions:
                if not isinstance(row, dict) or row.get("id") not in TRANSITION_TRIGGERS:
                    continue
                identifier = row["id"]
                if tuple(row.get("triggers", ())) != TRANSITION_TRIGGERS[identifier]:
                    self.add("CONTRACT", INTERFACES, f"transition {identifier!r} trigger mapping differs from the locked contract")

    def _check_references(self, functional: object, interfaces: object) -> None:
        if not isinstance(functional, dict) or not isinstance(interfaces, dict):
            return
        blocks = set(FUNCTIONAL_IDS["blocks"])
        modes = set(FUNCTIONAL_IDS["modes"])
        clocks = set(FUNCTIONAL_IDS["clock_domains"])
        controls = set(INTERFACES_IDS["controls"])
        power_domains = set(INTERFACES_IDS["power_domains"])
        connectors = set(INTERFACES_IDS["connectors"])
        expansions = set(INTERFACES_IDS["expansion_interfaces"])
        states = set(INTERFACES_IDS["operating_states"])

        for index, row in enumerate(functional["modes"]):
            self._check_many_refs(FUNCTIONAL, f"modes[{index}].active_blocks", row["active_blocks"], blocks)
            self._check_ref(FUNCTIONAL, f"modes[{index}].error_mode", row["error_mode"], modes)
        for index, row in enumerate(functional["blocks"]):
            self._check_ref(FUNCTIONAL, f"blocks[{index}].power_domain", row["power_domain"], power_domains, nullable=True)
            self._check_ref(FUNCTIONAL, f"blocks[{index}].clock_domain", row["clock_domain"], clocks, nullable=True)
        for index, row in enumerate(functional["flows"]):
            self._check_ref(FUNCTIONAL, f"flows[{index}].source_id", row["source_id"], blocks)
            self._check_ref(FUNCTIONAL, f"flows[{index}].sink_id", row["sink_id"], blocks)
            self._check_many_refs(FUNCTIONAL, f"flows[{index}].enabled_modes", row["enabled_modes"], modes)
            self._check_ref(FUNCTIONAL, f"flows[{index}].control_id", row["control_id"], controls, nullable=True)
        for index, row in enumerate(functional["clock_domains"]):
            self._check_ref(FUNCTIONAL, f"clock_domains[{index}].source_id", row["source_id"], blocks)
            self._check_many_refs(FUNCTIONAL, f"clock_domains[{index}].consumer_ids", row["consumer_ids"], blocks)
            self._check_many_refs(FUNCTIONAL, f"clock_domains[{index}].synchronous_with", row["synchronous_with"], clocks)
        for collection in ("buffers", "latency_boundaries"):
            fields = ("owner", "producer", "consumer") if collection == "buffers" else ("source", "sink")
            for index, row in enumerate(functional[collection]):
                for field in fields:
                    self._check_ref(FUNCTIONAL, f"{collection}[{index}].{field}", row[field], blocks)

        owner_targets = blocks | connectors | expansions
        power_targets = blocks | connectors | expansions | power_domains | {"carrier", "enclosure"}
        for index, row in enumerate(interfaces["connectors"]):
            self._check_many_refs(INTERFACES, f"connectors[{index}].power_domains", row["power_domains"], power_domains)
        for index, row in enumerate(interfaces["controls"]):
            self._check_ref(INTERFACES, f"controls[{index}].owner", row["owner"], owner_targets)
        for index, row in enumerate(interfaces["expansion_interfaces"]):
            self._check_many_refs(INTERFACES, f"expansion_interfaces[{index}].power_domains", row["power_domains"], power_domains)
        for index, row in enumerate(interfaces["power_domains"]):
            self._check_ref(INTERFACES, f"power_domains[{index}].source", row["source"], power_targets)
            self._check_many_refs(INTERFACES, f"power_domains[{index}].consumers", row["consumers"], power_targets)
        for index, row in enumerate(interfaces["fault_scenarios"]):
            self._check_ref(INTERFACES, f"fault_scenarios[{index}].recovery_state", row["recovery_state"], states)
        for index, row in enumerate(interfaces["transitions"]):
            self._check_many_refs(
                INTERFACES,
                f"transitions[{index}].from_states",
                row["from_states"],
                states,
            )
            self._check_many_refs(
                INTERFACES,
                f"transitions[{index}].to_states",
                row["to_states"],
                states,
            )

    def _check_ref(
        self,
        path: Path,
        field: str,
        value: object,
        allowed: set[str],
        *,
        nullable: bool = False,
    ) -> None:
        if value is None and nullable:
            return
        if value not in allowed:
            self.add("REFERENCE", path, f"{field} does not resolve: {value!r}")

    def _check_many_refs(self, path: Path, field: str, values: object, allowed: set[str]) -> None:
        if not isinstance(values, list):
            return
        for value in values:
            self._check_ref(path, field, value, allowed)

    def _index_sources(self, sources: object) -> dict[str, Mapping[str, object]] | None:
        if not isinstance(sources, list):
            self.add("INSTANCE", SOURCES, "source collection must be an array")
            return None
        result: dict[str, Mapping[str, object]] = {}
        for index, row in enumerate(sources):
            if not isinstance(row, dict) or not isinstance(row.get("id"), str) or not isinstance(row.get("status"), str):
                self.add("INSTANCE", SOURCES, f"sources[{index}] lacks the required source-record shape")
                return None
            identifier = row["id"]
            if identifier in result:
                self.add("EVIDENCE", SOURCES, f"duplicate source ID {identifier!r}")
            else:
                result[identifier] = row
        identifiers = tuple(result)
        if identifiers != tuple(sorted(identifiers)):
            self.add("ORDER", SOURCES, "source records are not sorted by ID")
        return result

    def _check_document_evidence(self, path: Path, document: object) -> None:
        if not isinstance(document, dict) or self.sources_by_id is None:
            return
        evidence: list[tuple[str, object]] = [("evidence_ids", document.get("evidence_ids"))]
        for collection, (_, keys) in (
            FUNCTIONAL_RECORDS.items() if path == FUNCTIONAL else INTERFACES_RECORDS.items()
        ):
            if "evidence_ids" not in keys:
                continue
            for index, row in enumerate(document.get(collection, [])):
                if isinstance(row, dict):
                    evidence.append((f"{collection}[{index}].evidence_ids", row.get("evidence_ids")))
        for field, identifiers in evidence:
            if not isinstance(identifiers, list):
                continue
            for identifier in identifiers:
                source = self.sources_by_id.get(identifier)
                if source is None:
                    self.add("EVIDENCE", path, f"{field} references absent source {identifier!r}")
                elif source.get("status") != "current":
                    self.add("EVIDENCE", path, f"{field} references non-current source {identifier!r}")

    def check_policy(self) -> object | None:
        policy = self.validate_instance(POLICY, POLICY_SCHEMA)
        if policy is None or not isinstance(policy, dict):
            return None
        self.check_document_order(POLICY, policy, POLICY_ROOT_KEYS, POLICY_RECORDS)
        self._check_registry_order_and_uniqueness(policy)
        families = policy.get("families")
        if not isinstance(families, list):
            return None
        actual_families = tuple(row.get("family") for row in families if isinstance(row, dict))
        if actual_families != FAMILIES:
            self.add("POLICY", POLICY, "family membership/order differs from the locked P1C2 family set")
        self.policy_families = {
            row["family"]: row for row in families if isinstance(row, dict) and isinstance(row.get("family"), str)
        }
        self._check_family_contract(policy)
        self._check_requirement_bases(policy)
        return policy

    def _check_registry_order_and_uniqueness(self, policy: Mapping[str, object]) -> None:
        for collection, field in (
            ("engineering_policies", "id"),
            ("requirements", "id"),
            ("families", "family"),
        ):
            rows = policy.get(collection)
            if not isinstance(rows, list):
                continue
            identifiers = [row.get(field) for row in rows if isinstance(row, dict)]
            if len(identifiers) != len(set(identifiers)):
                self.add("POLICY", POLICY, f"{collection} contains duplicate identifiers")
            if collection != "families" and identifiers != sorted(identifiers):
                self.add("ORDER", POLICY, f"{collection} rows are not sorted by ID")

    def _check_family_contract(self, policy: Mapping[str, object]) -> None:
        if self.policy_families is None:
            return
        dependent_features: list[object] = []
        for family in FAMILIES:
            row = self.policy_families.get(family)
            if row is None:
                continue
            expected_critical = family in CRITICAL_FAMILIES
            expected_generic = family in GENERIC_FAMILIES
            expected_statuses = (
                GENERIC_ALLOWED
                if expected_generic
                else CRITICAL_ALLOWED
                if expected_critical
                else NONCRITICAL_ALLOWED
            )
            expected_mfr = None if expected_generic else f"REQ-{family.upper()}-MFR"
            expected_uk = None if expected_generic else f"REQ-{family.upper()}-UK"
            expected_mfr_source = None if expected_generic else f"AIUS-{family.upper()}-MFR"
            if family == "cliff-rx":
                expected_mfr_source = "CLIFF-ORJ5"
            elif family == "cliff-tx":
                expected_mfr_source = "CLIFF-OTJ5"
            expected_uk_source = None if expected_generic else f"AIUS-{family.upper()}-UK"
            expected_values = {
                "component_file": f"components/{family}.json",
                "critical": expected_critical,
                "generic_eligible": expected_generic,
                "allowed_statuses": list(expected_statuses),
                "manufacturer_request_id": expected_mfr,
                "availability_request_id": expected_uk,
                "manufacturer_source_id": expected_mfr_source,
                "availability_source_id": expected_uk_source,
            }
            for field, expected in expected_values.items():
                if row.get(field) != expected:
                    self.add("POLICY", POLICY, f"family {family!r} has invalid {field}")
            requirements = row.get("requirements")
            if isinstance(requirements, list) and requirements != sorted(requirements):
                self.add("ORDER", POLICY, f"family {family!r} requirements are not sorted")
            dependent_features.append(row.get("dependent_feature"))
        if len(dependent_features) != len(set(dependent_features)):
            self.add("POLICY", POLICY, "dependent_feature values must be unique")

        requirements = policy.get("requirements")
        if isinstance(requirements, list):
            declared = {row.get("id") for row in requirements if isinstance(row, dict)}
            used: set[object] = set()
            for row in self.policy_families.values():
                family_requirements = row.get("requirements")
                if isinstance(family_requirements, list):
                    used.update(family_requirements)
            if declared != used:
                self.add("POLICY", POLICY, "declared and family requirement ID sets differ")

    def _check_requirement_bases(self, policy: Mapping[str, object]) -> None:
        engineering = policy.get("engineering_policies")
        requirements = policy.get("requirements")
        if not isinstance(engineering, list) or not isinstance(requirements, list):
            return
        policy_ids = {row.get("id") for row in engineering if isinstance(row, dict)}
        if self.sources_by_id is not None and policy_ids & set(self.sources_by_id):
            self.add("POLICY", POLICY, "engineering-policy and source namespaces overlap")
        for row in requirements:
            if not isinstance(row, dict):
                continue
            for basis in row.get("basis_ids", []):
                if basis in policy_ids:
                    continue
                if self.sources_by_id is None:
                    continue
                source = self.sources_by_id.get(basis)
                if source is None or source.get("status") != "current":
                    self.add("POLICY", POLICY, f"requirement {row.get('id')!r} has unresolved basis {basis!r}")

    def check_selection_fragments(self, family: str | None) -> None:
        if self.policy_families is None:
            return
        if family is not None:
            self._check_family_fragment(family, fileset_mode=False)
            return

        component_names = self._json_basenames(COMPONENT_DIRECTORY)
        expected_component_names = {f"{name}.json" for name in FAMILIES}
        if component_names is not None and component_names != expected_component_names:
            self.add("FILESET", COMPONENT_DIRECTORY, self._set_difference_detail(expected_component_names, component_names))

        for name in FAMILIES:
            self._check_family_fragment(name, fileset_mode=True)

        if len(self.components) != len(FAMILIES):
            return

        expected_request_names: set[str] = set()
        for name, component in self.components.items():
            if component.get("status") in SELECTED_STATUSES:
                family_policy = self.policy_families.get(name)
                if family_policy is not None:
                    for field in ("manufacturer_request_id", "availability_request_id"):
                        request_id = family_policy.get(field)
                        if isinstance(request_id, str):
                            expected_request_names.add(f"{request_id}.json")
        request_names = self._json_basenames(REQUEST_DIRECTORY)
        if request_names is not None and request_names != expected_request_names:
            self.add("FILESET", REQUEST_DIRECTORY, self._set_difference_detail(expected_request_names, request_names))
        self.selection_fragments_ready = not self.diagnostics

    @staticmethod
    def _set_difference_detail(expected: set[str], actual: set[str]) -> str:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        return f"JSON basename set differs; missing={missing!r}; extra={extra!r}"

    def _json_basenames(self, directory: Path) -> set[str] | None:
        absolute = self.root / directory
        try:
            return {path.name for path in absolute.iterdir() if path.is_file() and path.suffix == ".json"}
        except FileNotFoundError:
            self.add("MISSING", directory, "required input directory is missing")
        except OSError as error:
            self.add("PARSE", directory, str(error))
        return None

    def _check_family_fragment(self, family: str, *, fileset_mode: bool) -> None:
        family_policy = self.policy_families.get(family) if self.policy_families is not None else None
        path = COMPONENT_DIRECTORY / f"{family}.json"
        component = self.validate_instance(path, COMPONENT_SCHEMA)
        if component is None or not isinstance(component, dict):
            return
        self.check_document_order(path, component, COMPONENT_KEYS, {})
        self._check_component_nested_order(path, component)
        self.components[family] = component
        if component.get("block") != family:
            self.add("COMPONENT", path, f"block must equal filename family {family!r}")
        self._check_component_matrix(path, component)
        self._check_alternatives(path, component)
        if family_policy is None:
            return
        if component.get("requirements") != family_policy.get("requirements"):
            self.add("COMPONENT", path, "requirements differ from selection policy")
        status = component.get("status")
        if status in SELECTED_STATUSES:
            for field in ("manufacturer_request_id", "availability_request_id"):
                if component.get(field) != family_policy.get(field):
                    self.add("COMPONENT", path, f"{field} differs from selection policy")
        if component.get("status") not in family_policy.get("allowed_statuses", []):
            self.add("COMPONENT", path, "status is not allowed by selection policy")
        if fileset_mode and family_policy.get("critical") is True and status in {
            "unresolved",
            "candidate-evidence-unavailable",
        }:
            self.add("COMPONENT", path, f"critical family is blocked by status {status!r}")
        if status not in SELECTED_STATUSES:
            return
        self._check_request_fragment(path, component, family_policy, "manufacturer-evidence")
        self._check_request_fragment(path, component, family_policy, "uk-availability")

    def _check_component_nested_order(self, path: Path, component: Mapping[str, object]) -> None:
        alternatives = component.get("alternatives")
        if isinstance(alternatives, list):
            for index, row in enumerate(alternatives):
                if isinstance(row, dict) and tuple(row) != ALTERNATIVE_KEYS:
                    self.add("ORDER", path, f"alternatives[{index}] key order differs from the locked contract")
        generic = component.get("generic_policy")
        if isinstance(generic, dict) and tuple(generic) != GENERIC_POLICY_KEYS:
            self.add("ORDER", path, "generic_policy key order differs from the locked contract")

    def _check_component_matrix(self, path: Path, component: Mapping[str, object]) -> None:
        status = component.get("status")
        selected_fields = (
            "manufacturer",
            "mpn",
            "selection_rationale",
            "authority_url",
            "manufacturer_request_id",
            "availability_request_id",
        )
        if status in SELECTED_STATUSES:
            if any(component.get(field) is None for field in selected_fields) or component.get("generic_policy") is not None:
                self.add("COMPONENT", path, f"status {status!r} violates the selected-field matrix")
        elif status == "generic-allowance":
            if any(component.get(field) is not None for field in selected_fields) or not isinstance(component.get("generic_policy"), dict):
                self.add("COMPONENT", path, "generic-allowance violates the null/generic matrix")
        elif status == "unresolved":
            nullable = ("manufacturer", "mpn", "authority_url", "manufacturer_request_id", "availability_request_id", "generic_policy")
            rationale = component.get("selection_rationale")
            if any(component.get(field) is not None for field in nullable) or not isinstance(rationale, str) or not rationale.strip():
                self.add("COMPONENT", path, "unresolved violates the rationale/null matrix")

    def _check_alternatives(self, path: Path, component: Mapping[str, object]) -> None:
        alternatives = component.get("alternatives")
        if not isinstance(alternatives, list):
            return
        pairs = [
            (row.get("manufacturer"), row.get("mpn"))
            for row in alternatives
            if isinstance(row, dict)
        ]
        if len(pairs) != len(set(pairs)):
            self.add("COMPONENT", path, "alternatives repeat a manufacturer/MPN pair")
        selected_pair = (component.get("manufacturer"), component.get("mpn"))
        if selected_pair != (None, None) and selected_pair in pairs:
            self.add("COMPONENT", path, "an alternative repeats the selected manufacturer/MPN")

    def _check_request_fragment(
        self,
        component_path: Path,
        component: Mapping[str, object],
        family_policy: Mapping[str, object],
        request_kind: str,
    ) -> None:
        id_field = "manufacturer_request_id" if request_kind == "manufacturer-evidence" else "availability_request_id"
        source_field = "manufacturer_source_id" if request_kind == "manufacturer-evidence" else "availability_source_id"
        request_id = family_policy.get(id_field)
        if not isinstance(request_id, str):
            self.add("REQUEST", component_path, f"policy lacks {id_field}")
            return
        path = REQUEST_DIRECTORY / f"{request_id}.json"
        request = self.validate_instance(path, REQUEST_SCHEMA)
        if request is None or not isinstance(request, dict):
            return
        self.check_document_order(path, request, REQUEST_KEYS, {})
        self.requests[request_id] = request
        expected = {
            "request_id": request_id,
            "request_kind": request_kind,
            "family": component.get("block"),
            "requirements": component.get("requirements"),
            "selected_manufacturer": component.get("manufacturer"),
            "selected_mpn": component.get("mpn"),
            "source_id": family_policy.get(source_field),
        }
        if request_kind == "manufacturer-evidence":
            expected["authority_url"] = component.get("authority_url")
        for field, value in expected.items():
            if request.get(field) != value:
                self.add("REQUEST", path, f"{field} differs from component/policy linkage")
        if request_kind == "manufacturer-evidence":
            expected_status = {
                "candidate-pending-evidence": "pending",
                "selected": "satisfied",
                "candidate-evidence-unavailable": "unavailable",
            }.get(component.get("status"))
            if request.get("status") != expected_status:
                self.add("REQUEST", path, "manufacturer request status differs from component status")
        self._check_request_source(path, request)

    def _check_request_source(self, path: Path, request: Mapping[str, object]) -> None:
        if request.get("status") != "satisfied" or self.sources_by_id is None:
            return
        source_id = request.get("source_id")
        evidence_ids = request.get("evidence_ids")
        if not isinstance(source_id, str) or not isinstance(evidence_ids, list):
            return
        if source_id not in evidence_ids:
            self.add("REQUEST", path, "satisfied request evidence_ids must include source_id")
        for evidence_id in evidence_ids:
            source = self.sources_by_id.get(evidence_id)
            if source is None or source.get("status") != "current":
                self.add("REQUEST", path, f"satisfied request references absent/non-current evidence {evidence_id!r}")
        source = self.sources_by_id.get(source_id)
        if source is None or source.get("status") != "current":
            self.add("REQUEST", path, "satisfied request source_id is absent or non-current")
            return
        if request.get("request_kind") == "manufacturer-evidence":
            artifacts = source.get("artifacts")
            archived = isinstance(artifacts, list) and any(
                isinstance(artifact, dict)
                and artifact.get("storage") in {"lfs", "git"}
                and artifact.get("retrieval_status") == "current"
                for artifact in artifacts
            )
            observations = source.get("observations")
            selected_mpn = request.get("selected_mpn")
            matches_mpn = isinstance(observations, list) and any(
                isinstance(observation, dict)
                and observation.get("key") == "manufacturer_part_number"
                and observation.get("value") == selected_mpn
                for observation in observations
            )
            seed_pair = (source_id, selected_mpn) in {
                ("CLIFF-ORJ5", "ORJ5"),
                ("CLIFF-OTJ5", "OTJ5"),
            }
            if source.get("class") not in {"golden", "comparative", "supplemental"} or not archived:
                self.add("REQUEST", path, "satisfied manufacturer request lacks a current archived artifact")
            if not (matches_mpn or seed_pair):
                self.add("REQUEST", path, "satisfied manufacturer source is not bound to selected_mpn")
        else:
            observations = source.get("observations")
            mpn = request.get("selected_mpn")
            matches_mpn = isinstance(observations, list) and any(
                isinstance(observation, dict)
                and observation.get("key") == "manufacturer_part_number"
                and observation.get("value") == mpn
                for observation in observations
            )
            if source.get("class") != "dynamic" or not matches_mpn:
                self.add("REQUEST", path, "satisfied UK request lacks a current dynamic matching-MPN observation")

    def check_selection(self, policy: object | None) -> None:
        if (
            not self.selection_fragments_ready
            or not isinstance(policy, dict)
            or len(self.components) != len(FAMILIES)
        ):
            self._require_file(COMPONENT_CSV)
            return
        self._check_parity(
            COMPONENT_CSV,
            lambda: render_components(policy, self.components.values()),
        )

    def check_handoff(self) -> None:
        aggregate = self.load_json(REQUEST_AGGREGATE)
        if not aggregate.parsed or not self.selection_fragments_ready:
            return
        expected_request_ids = {
            request_id
            for family in FAMILIES
            for request_id in self._expected_request_ids_for_component(family)
        }
        if set(self.requests) != expected_request_ids:
            self.add("PARITY", REQUEST_AGGREGATE, "loaded request fragment membership is incomplete")
            return
        self._check_parity(
            REQUEST_AGGREGATE,
            lambda: render_supplemental_requests(self.requests.values()),
        )

    def _expected_request_ids_for_component(self, family: str) -> tuple[str, ...]:
        component = self.components.get(family)
        family_policy = None if self.policy_families is None else self.policy_families.get(family)
        if component is None or family_policy is None or component.get("status") not in SELECTED_STATUSES:
            return ()
        return tuple(
            value
            for value in (
                family_policy.get("manufacturer_request_id"),
                family_policy.get("availability_request_id"),
            )
            if isinstance(value, str)
        )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", help="Aius project root")
    subparsers = parser.add_subparsers(dest="command", required=True)
    topology = subparsers.add_parser("topology")
    topology.add_argument("--stage", choices=STAGES, required=True)
    topology.add_argument("--family")
    return parser


def run(argv: list[str] | None = None) -> int:
    parser = _parser()
    arguments = parser.parse_args(argv)
    if arguments.family is not None and arguments.stage != "selection-fragments":
        parser.error("--family is permitted only with topology --stage selection-fragments")
    if arguments.family is not None and arguments.family not in FAMILIES:
        parser.error(f"unknown P1C family: {arguments.family}")

    root = Path(arguments.root) if arguments.root is not None else Path(__file__).resolve().parents[1]
    validator = Validator(root)
    validator.load_schemas()
    validator.check_contracts()
    stage_index = STAGES.index(arguments.stage)
    policy: object | None = None
    if stage_index >= STAGES.index("architecture-base"):
        validator.check_architecture()
    if stage_index >= STAGES.index("architecture"):
        policy = validator.check_policy()
    if stage_index >= STAGES.index("selection-fragments"):
        validator.check_selection_fragments(arguments.family)
    if stage_index >= STAGES.index("selection"):
        validator.check_selection(policy)
    if stage_index >= STAGES.index("handoff"):
        validator.check_handoff()

    if validator.diagnostics:
        for diagnostic in sorted(validator.diagnostics):
            print(diagnostic.render(), file=sys.stderr)
        return 1
    suffix = f" {arguments.family}" if arguments.family is not None else ""
    print(f"OK topology {arguments.stage}{suffix}")
    return 0


def main(argv: list[str] | None = None) -> int:
    return run(argv)


if __name__ == "__main__":
    raise SystemExit(main())
