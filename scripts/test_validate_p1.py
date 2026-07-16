#!/usr/bin/env python3
"""Executable contract tests for the staged Aius P1C validator."""

from __future__ import annotations

import copy
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import validate_p1 as validator_module
from render_p1 import (
    render_components,
    render_functional_topology,
    render_interfaces_and_states,
    render_supplemental_requests,
)


AIUS_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_ROOT = Path(__file__).resolve().parent
VALIDATOR = SCRIPT_ROOT / "validate_p1.py"
SCHEMA_FIXTURES = SCRIPT_ROOT / "fixtures" / "p1c-schema"
MATRIX = SCRIPT_ROOT / "fixtures" / "validate-p1" / "matrix.json"

FUNCTIONAL_BASE = "scripts/fixtures/p1c-schema/functional-topology/base-v1.json"
INTERFACES_BASE = "scripts/fixtures/p1c-schema/interfaces-and-states/base-v1.json"
FUNCTIONAL_CONTRACT = "evidence/design/contracts/functional-topology.json"
INTERFACES_CONTRACT = "evidence/design/contracts/interfaces-and-states.json"
SELECTION_BASE = "scripts/fixtures/p1c-schema/selection-policy/base-v1.json"
SELECTION_CONTRACT = "evidence/design/contracts/selection-policy.json"

SCHEMAS = (
    "evidence/design/functional-topology-schema.json",
    "evidence/design/interfaces-and-states-schema.json",
    "evidence/design/component-schema.json",
    "evidence/design/selection-policy-schema.json",
    "evidence/manifest/source-request-schema.json",
)

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

SELECTED_STATUSES = [
    "candidate-pending-evidence",
    "selected",
    "candidate-evidence-unavailable",
]


def _read_json(path: Path) -> object:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _source(
    identifier: str,
    *,
    source_class: str = "golden",
    artifacts: list[dict[str, object]] | None = None,
    observations: list[dict[str, object]] | None = None,
) -> dict[str, object]:
    return {
        "id": identifier,
        "class": source_class,
        "status": "current",
        "artifacts": [] if artifacts is None else artifacts,
        "observations": [] if observations is None else observations,
    }


def _component(family: str, policy: dict[str, object]) -> dict[str, object]:
    if family in GENERIC_FAMILIES:
        return {
            "block": family,
            "requirements": copy.deepcopy(policy["requirements"]),
            "manufacturer": None,
            "mpn": None,
            "alternatives": [],
            "selection_rationale": None,
            "authority_url": None,
            "manufacturer_request_id": None,
            "availability_request_id": None,
            "generic_policy": {
                "value": "100 nF",
                "package": "0603",
                "tolerance": "10%",
            },
            "status": "generic-allowance",
            "notes": "",
        }
    selected_mpn = {
        "cliff-rx": "ORJ5",
        "cliff-tx": "OTJ5",
    }.get(family, f"MPN-{family.upper()}")
    return {
        "block": family,
        "requirements": copy.deepcopy(policy["requirements"]),
        "manufacturer": "Example Manufacturer",
        "mpn": selected_mpn,
        "alternatives": [],
        "selection_rationale": "Provisional candidate for the declared family.",
        "authority_url": f"https://manufacturer.example/{family}",
        "manufacturer_request_id": policy["manufacturer_request_id"],
        "availability_request_id": policy["availability_request_id"],
        "generic_policy": None,
        "status": "candidate-pending-evidence",
        "notes": "",
    }


def _request(component: dict[str, object], request_kind: str, source_id: str) -> dict[str, object]:
    manufacturer = request_kind == "manufacturer-evidence"
    suffix = "MFR" if manufacturer else "UK"
    family = component["block"]
    return {
        "request_id": f"REQ-{str(family).upper()}-{suffix}",
        "request_kind": request_kind,
        "family": family,
        "requirements": component["requirements"],
        "authority_url": (
            component["authority_url"]
            if manufacturer
            else f"https://uk-distributor.example/{family}"
        ),
        "selected_manufacturer": component["manufacturer"],
        "selected_mpn": component["mpn"],
        "source_id": source_id,
        "artifact_kinds": ["datasheet"] if manufacturer else ["price-observation"],
        "retention": "archive" if manufacturer else "normalized-observation-only",
        "status": "pending",
        "evidence_ids": [],
        "notes": "",
    }


class FixtureTree:
    def __init__(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="aius-validate-p1-")
        self.root = Path(self.temporary.name)
        self.functional = copy.deepcopy(
            _read_json(SCHEMA_FIXTURES / "functional-topology" / "valid.json")
        )
        self.interfaces = copy.deepcopy(
            _read_json(SCHEMA_FIXTURES / "interfaces-and-states" / "valid.json")
        )
        self.policy_base = copy.deepcopy(_read_json(AIUS_ROOT / SELECTION_BASE))
        self.policy = copy.deepcopy(_read_json(AIUS_ROOT / SELECTION_CONTRACT))
        self.policy_families = {
            row["family"]: row for row in self.policy["families"]
        }
        self.components = {
            family: _component(family, self.policy_families[family])
            for family in FAMILIES
        }
        self.requests: dict[str, dict[str, object]] = {}
        policy_basis_ids = {
            basis_id
            for requirement in self.policy["requirements"]
            for basis_id in requirement["basis_ids"]
            if not basis_id.startswith("EP-")
        }
        self.sources = [
            _source(identifier)
            for identifier in sorted(
                {
                    evidence_id
                    for collection in (self.functional, self.interfaces)
                    for evidence_id in collection["evidence_ids"]
                }
                | policy_basis_ids
            )
        ]
        self._copy_schemas()
        self._copy_contract_inputs()
        self.write_architecture()
        self.write_policy()
        self.write_fragments()
        self.render_all()

    def close(self) -> None:
        self.temporary.cleanup()

    def __enter__(self) -> FixtureTree:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def path(self, relative: str) -> Path:
        return self.root / relative

    def _copy_schemas(self) -> None:
        for relative in SCHEMAS:
            target = self.path(relative)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(AIUS_ROOT / relative, target)

    def _copy_contract_inputs(self) -> None:
        for relative in (
            FUNCTIONAL_BASE,
            INTERFACES_BASE,
            SELECTION_BASE,
            FUNCTIONAL_CONTRACT,
            INTERFACES_CONTRACT,
            SELECTION_CONTRACT,
        ):
            target = self.path(relative)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(AIUS_ROOT / relative, target)

    def write_architecture(self) -> None:
        _write_json(self.path("evidence/design/functional-topology.json"), self.functional)
        _write_json(self.path("evidence/design/interfaces-and-states.json"), self.interfaces)
        _write_json(self.path("evidence/manifest/sources.json"), self.sources)

    def write_policy(self) -> None:
        _write_json(self.path("evidence/design/selection-policy.json"), self.policy)

    def write_fragments(self) -> None:
        self.requests = {}
        for family, component in self.components.items():
            _write_json(self.path(f"evidence/design/components/{family}.json"), component)
            if component["status"] not in SELECTED_STATUSES:
                continue
            policy = self.policy_families[family]
            for request_kind, source_field in (
                ("manufacturer-evidence", "manufacturer_source_id"),
                ("uk-availability", "availability_source_id"),
            ):
                request = _request(component, request_kind, policy[source_field])
                self.requests[request["request_id"]] = request
                _write_json(
                    self.path(f"evidence/manifest/requests/{request['request_id']}.json"),
                    request,
                )

    def write_component(self, family: str) -> None:
        _write_json(
            self.path(f"evidence/design/components/{family}.json"),
            self.components[family],
        )

    def write_request(self, request_id: str) -> None:
        _write_json(
            self.path(f"evidence/manifest/requests/{request_id}.json"),
            self.requests[request_id],
        )

    def render_architecture(self) -> None:
        functional_output = self.path("docs/reference/functional-topology.md")
        interfaces_output = self.path("docs/reference/interfaces-and-states.md")
        functional_output.parent.mkdir(parents=True, exist_ok=True)
        functional_output.write_bytes(render_functional_topology(self.functional))
        interfaces_output.write_bytes(render_interfaces_and_states(self.interfaces))

    def render_selection(self) -> None:
        output = self.path("evidence/design/provisional-components.csv")
        output.write_bytes(render_components(self.policy, self.components.values()))

    def render_handoff(self) -> None:
        output = self.path("evidence/manifest/supplemental-requests.json")
        output.write_bytes(render_supplemental_requests(self.requests.values()))

    def render_all(self) -> None:
        self.render_architecture()
        self.render_selection()
        self.render_handoff()

    def add_selected_manufacturer_source(
        self,
        family: str,
        *,
        archived: bool = True,
        observed_mpn: str | None = "selected",
    ) -> None:
        source_id = f"AIUS-{family.upper()}-MFR"
        artifact = {
            "storage": "lfs",
            "retrieval_status": "current",
        }
        observations = (
            []
            if observed_mpn is None
            else [
                {
                    "key": "manufacturer_part_number",
                    "value": (
                        self.components[family]["mpn"]
                        if observed_mpn == "selected"
                        else observed_mpn
                    ),
                }
            ]
        )
        self.sources.append(
            _source(
                source_id,
                artifacts=[artifact] if archived else [],
                observations=observations,
            )
        )
        self.sources.sort(key=lambda row: row["id"])
        _write_json(self.path("evidence/manifest/sources.json"), self.sources)

    def add_selected_uk_source(self, family: str, *, matching_mpn: bool = True) -> None:
        source_id = f"AIUS-{family.upper()}-UK"
        mpn = self.components[family]["mpn"] if matching_mpn else "WRONG-MPN"
        self.sources.append(
            _source(
                source_id,
                source_class="dynamic",
                observations=[{"key": "manufacturer_part_number", "value": mpn}],
            )
        )
        self.sources.sort(key=lambda row: row["id"])
        _write_json(self.path("evidence/manifest/sources.json"), self.sources)

    def select_family(self, family: str) -> None:
        component = self.components[family]
        component["status"] = "selected"
        self.write_component(family)
        request_id = f"REQ-{family.upper()}-MFR"
        request = self.requests[request_id]
        request["status"] = "satisfied"
        request["evidence_ids"] = [request["source_id"]]
        self.write_request(request_id)
        self.add_selected_manufacturer_source(family)


def _run(root: Path, stage: str, family: str | None = None) -> subprocess.CompletedProcess[str]:
    command = [
        sys.executable,
        str(VALIDATOR),
        "--root",
        str(root),
        "topology",
        "--stage",
        stage,
    ]
    if family is not None:
        command.extend(("--family", family))
    return subprocess.run(command, check=False, capture_output=True, text=True)


def _codes(result: subprocess.CompletedProcess[str]) -> list[str]:
    return [
        line.split(":", 1)[0].removeprefix("AIUS-P1-")
        for line in result.stderr.splitlines()
        if line.startswith("AIUS-P1-")
    ]


class ValidatorTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture = FixtureTree()

    def tearDown(self) -> None:
        self.fixture.close()

    def assert_ok(self, stage: str, family: str | None = None) -> None:
        result = _run(self.fixture.root, stage, family)
        self.assertEqual(result.returncode, 0, result.stderr)
        suffix = f" {family}" if family is not None else ""
        self.assertEqual(result.stdout, f"OK topology {stage}{suffix}\n")
        self.assertEqual(result.stderr, "")

    def assert_code(self, stage: str, code: str, family: str | None = None) -> subprocess.CompletedProcess[str]:
        result = _run(self.fixture.root, stage, family)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(code, _codes(result), result.stderr)
        return result


class PositiveStageTests(ValidatorTestCase):
    def test_fixture_uses_normative_policy_requirements_and_external_bases(self) -> None:
        expected_external_bases = {
            basis_id
            for requirement in self.fixture.policy["requirements"]
            for basis_id in requirement["basis_ids"]
            if not basis_id.startswith("EP-")
        }
        expected_source_ids = sorted(
            expected_external_bases
            | set(self.fixture.functional["evidence_ids"])
            | set(self.fixture.interfaces["evidence_ids"])
        )
        self.assertEqual(
            [source["id"] for source in self.fixture.sources],
            expected_source_ids,
        )
        self.assertTrue(
            all(source["status"] == "current" for source in self.fixture.sources)
        )
        for family, component in self.fixture.components.items():
            self.assertEqual(
                component["requirements"],
                self.fixture.policy_families[family]["requirements"],
            )
        for request in self.fixture.requests.values():
            self.assertEqual(
                request["requirements"],
                self.fixture.policy_families[request["family"]]["requirements"],
            )

    def test_fixture_matrix_is_finite_and_covers_every_contract_code(self) -> None:
        rows = _read_json(MATRIX)
        self.assertTrue(all(row.get("canonical_positive_fixture") for row in rows))
        self.assertEqual(
            {row["code"] for row in rows},
            {
                "SCHEMA",
                "MISSING",
                "PARSE",
                "INSTANCE",
                "ORDER",
                "IDSET",
                "REFERENCE",
                "CONTRACT",
                "EVIDENCE",
                "POLICY",
                "COMPONENT",
                "REQUEST",
                "FILESET",
                "PARITY",
            },
        )
        mutations = {row["mutation"] for row in rows}
        for family in (
            "mode",
            "block",
            "flow",
            "clock domain",
            "buffer",
            "latency boundary",
            "connector",
            "control",
            "expansion interface",
            "power domain",
            "operating state",
            "fault scenario",
            "transition source",
            "transition target",
            "transition actions",
            "document evidence",
            "record evidence",
        ):
            self.assertIn(f"{family} semantic value differs from normative contract", mutations)

    def test_each_stage_and_scoped_fragment_has_an_explicit_positive_case(self) -> None:
        for stage in (
            "contract",
            "architecture-base",
            "architecture",
            "selection-fragments",
            "selection",
            "handoff",
        ):
            with self.subTest(stage=stage):
                self.assert_ok(stage)
        self.assert_ok("selection-fragments", "usb-c-receptacle")
        self.assert_ok("selection-fragments", "logic-decoupling-capacitor")

    def test_earlier_stages_ignore_permitted_later_paths(self) -> None:
        self.fixture.path("evidence/design/selection-policy.json").write_text("not JSON\n", encoding="utf-8")
        self.fixture.path("evidence/design/components/usb-c-receptacle.json").write_text("not JSON\n", encoding="utf-8")
        self.assert_ok("architecture-base")

    def test_scoped_family_ignores_invalid_sibling_fragments(self) -> None:
        self.fixture.path("evidence/design/components/usb-a-receptacle.json").write_bytes(b"\xff")
        self.fixture.path("evidence/manifest/requests/REQ-USB-A-RECEPTACLE-MFR.json").write_text("{\n", encoding="utf-8")
        self.assert_ok("selection-fragments", "usb-c-receptacle")

    def test_non_json_siblings_are_ignored_by_exact_fileset_checks(self) -> None:
        self.fixture.path("evidence/design/components/notes.txt").write_text("ignored\n", encoding="utf-8")
        self.fixture.path("evidence/manifest/requests/notes.csv").write_text("ignored\n", encoding="utf-8")
        self.assert_ok("selection-fragments")

    def test_noncritical_unresolved_family_passes_scoped_and_exact_unscoped_fileset(self) -> None:
        family = "expansion-spi"
        component = self.fixture.components[family]
        component.update(
            {
                "manufacturer": None,
                "mpn": None,
                "selection_rationale": "No safe candidate is available yet.",
                "authority_url": None,
                "manufacturer_request_id": None,
                "availability_request_id": None,
                "generic_policy": None,
                "status": "unresolved",
            }
        )
        self.fixture.write_component(family)
        for suffix in ("MFR", "UK"):
            request_id = f"REQ-{family.upper()}-{suffix}"
            self.fixture.path(
                f"evidence/manifest/requests/{request_id}.json"
            ).unlink()
            self.fixture.requests.pop(request_id)

        self.assert_ok("selection-fragments", family)
        self.assert_ok("selection-fragments")


class CliAndInputDiagnosticTests(ValidatorTestCase):
    def test_unknown_family_and_forbidden_family_scope_are_usage_errors(self) -> None:
        unknown = _run(self.fixture.root, "selection-fragments", "unknown-family")
        forbidden = _run(self.fixture.root, "architecture", "usb-c-receptacle")
        self.assertEqual(unknown.returncode, 2)
        self.assertEqual(forbidden.returncode, 2)

    def test_missing_required_input_is_reported_at_every_stage_boundary(self) -> None:
        cases = (
            ("contract", "evidence/manifest/source-request-schema.json", None),
            ("architecture-base", "evidence/design/functional-topology.json", None),
            ("architecture", "evidence/design/selection-policy.json", None),
            ("selection-fragments", "evidence/design/components/usb-c-receptacle.json", "usb-c-receptacle"),
            ("selection", "evidence/design/provisional-components.csv", None),
            ("handoff", "evidence/manifest/supplemental-requests.json", None),
        )
        for stage, relative, family in cases:
            with self.subTest(stage=stage):
                with FixtureTree() as fixture:
                    fixture.path(relative).unlink()
                    result = _run(fixture.root, stage, family)
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("MISSING", _codes(result))

    def test_malformed_and_non_utf8_json_use_parse(self) -> None:
        for payload in (b"{\n", b"\xff"):
            with self.subTest(payload=payload):
                with FixtureTree() as fixture:
                    fixture.path("evidence/design/functional-topology.json").write_bytes(payload)
                    result = _run(fixture.root, "architecture-base")
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("PARSE", _codes(result))

    def test_schema_rejected_instance_uses_instance_and_stops_dependent_parity(self) -> None:
        self.fixture.functional["schema_version"] = "2.0.0"
        self.fixture.write_architecture()
        self.fixture.path("docs/reference/functional-topology.md").write_text("stale\n", encoding="utf-8")
        result = self.assert_code("architecture-base", "INSTANCE")
        self.assertNotIn("PARITY", _codes(result))

    def test_component_parse_and_instance_failures_do_not_continue_request_checks(self) -> None:
        family = "usb-c-receptacle"
        path = self.fixture.path(f"evidence/design/components/{family}.json")
        path.write_text("{\n", encoding="utf-8")
        parsed = self.assert_code("selection-fragments", "PARSE", family)
        self.assertNotIn("REQUEST", _codes(parsed))

        self.fixture.write_component(family)
        self.fixture.components[family]["status"] = "invalid-status"
        self.fixture.write_component(family)
        invalid = self.assert_code("selection-fragments", "INSTANCE", family)
        self.assertNotIn("COMPONENT", _codes(invalid))
        self.assertNotIn("REQUEST", _codes(invalid))

    def test_unscoped_policy_parse_and_instance_failures_stop_dependent_stages(self) -> None:
        for mutation, expected_code in (("parse", "PARSE"), ("instance", "INSTANCE")):
            for stage in ("selection-fragments", "selection", "handoff"):
                with self.subTest(mutation=mutation, stage=stage):
                    with FixtureTree() as fixture:
                        if mutation == "parse":
                            fixture.path("evidence/design/selection-policy.json").write_text(
                                "{\n", encoding="utf-8"
                            )
                        else:
                            fixture.policy["schema_version"] = "2.0.0"
                            fixture.write_policy()
                        result = _run(fixture.root, stage)
                        self.assertEqual(result.returncode, 1)
                        self.assertIn(expected_code, _codes(result))
                        self.assertTrue(
                            {"COMPONENT", "REQUEST", "FILESET", "PARITY"}.isdisjoint(
                                _codes(result)
                            ),
                            result.stderr,
                        )

    def test_diagnostics_are_sorted(self) -> None:
        self.fixture.path("evidence/design/functional-topology.json").unlink()
        self.fixture.path("evidence/design/interfaces-and-states.json").unlink()
        result = self.assert_code("architecture-base", "MISSING")
        lines = [line for line in result.stderr.splitlines() if line.startswith("AIUS-P1-")]
        self.assertEqual(lines, sorted(lines))


class ContractStageDiagnosticTests(ValidatorTestCase):
    def test_each_historical_base_is_required(self) -> None:
        for relative in (FUNCTIONAL_BASE, INTERFACES_BASE, SELECTION_BASE):
            with self.subTest(relative=relative):
                with FixtureTree() as fixture:
                    fixture.path(relative).unlink()
                    result = _run(fixture.root, "contract")
                    self.assertEqual(result.returncode, 1)
                    self.assertEqual(_codes(result), ["MISSING"])

    def test_each_malformed_historical_base_reports_contract_and_parse(self) -> None:
        for relative in (FUNCTIONAL_BASE, INTERFACES_BASE, SELECTION_BASE):
            with self.subTest(relative=relative):
                with FixtureTree() as fixture:
                    fixture.path(relative).write_bytes(b"{\n")
                    result = _run(fixture.root, "contract")
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("CONTRACT", _codes(result))
                    self.assertIn("PARSE", _codes(result))

    def test_non_utf8_historical_base_reports_contract_and_parse(self) -> None:
        for relative in (FUNCTIONAL_BASE, INTERFACES_BASE, SELECTION_BASE):
            with self.subTest(relative=relative), FixtureTree() as fixture:
                fixture.path(relative).write_bytes(b"\xff")
                result = _run(fixture.root, "contract")
                self.assertEqual(result.returncode, 1)
                self.assertIn("CONTRACT", _codes(result))
                self.assertIn("PARSE", _codes(result))
                self.assertIn(relative, result.stderr)

    def test_each_parseable_base_raw_hash_mismatch_uses_contract(self) -> None:
        for relative in (FUNCTIONAL_BASE, INTERFACES_BASE, SELECTION_BASE):
            with self.subTest(relative=relative):
                with FixtureTree() as fixture:
                    path = fixture.path(relative)
                    path.write_bytes(path.read_bytes() + b" ")
                    result = _run(fixture.root, "contract")
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("CONTRACT", _codes(result))
                    self.assertNotIn("PARSE", _codes(result))

    def test_joint_base_and_normative_edit_cannot_redefine_the_contract(self) -> None:
        cases = (
            (FUNCTIONAL_BASE, FUNCTIONAL_CONTRACT, "modes"),
            (INTERFACES_BASE, INTERFACES_CONTRACT, "connectors"),
            (SELECTION_BASE, SELECTION_CONTRACT, "engineering_policies"),
        )
        for base_relative, contract_relative, collection in cases:
            with self.subTest(base=base_relative):
                with FixtureTree() as fixture:
                    base = _read_json(fixture.path(base_relative))
                    contract = _read_json(fixture.path(contract_relative))
                    base[collection][0]["notes"] += " Joint edit."
                    contract[collection][0]["notes"] += " Joint edit."
                    _write_json(fixture.path(base_relative), base)
                    _write_json(fixture.path(contract_relative), contract)
                    result = _run(fixture.root, "contract")
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("CONTRACT", _codes(result))
                    self.assertIn(base_relative, result.stderr)

    def test_normative_contracts_are_validated_by_the_migrated_schemas(self) -> None:
        cases = (
            (FUNCTIONAL_CONTRACT, "modes"),
            (INTERFACES_CONTRACT, "connectors"),
        )
        for relative, collection in cases:
            with self.subTest(relative=relative):
                with FixtureTree() as fixture:
                    contract = _read_json(fixture.path(relative))
                    contract[collection][0]["evidence_ids"] = []
                    _write_json(fixture.path(relative), contract)
                    result = _run(fixture.root, "contract")
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("INSTANCE", _codes(result))
                    self.assertNotIn("CONTRACT", _codes(result))

    def test_normative_semantic_drift_from_pure_builder_uses_contract(self) -> None:
        contract = _read_json(self.fixture.path(FUNCTIONAL_CONTRACT))
        contract["buffers"][0]["notes"] += " Drift."
        _write_json(self.fixture.path(FUNCTIONAL_CONTRACT), contract)
        result = self.assert_code("contract", "CONTRACT")
        self.assertIn(FUNCTIONAL_CONTRACT, result.stderr)

    def test_normative_record_key_order_uses_order(self) -> None:
        contract = _read_json(self.fixture.path(FUNCTIONAL_CONTRACT))
        contract["modes"][0] = {
            key: contract["modes"][0][key]
            for key in reversed(tuple(contract["modes"][0]))
        }
        _write_json(self.fixture.path(FUNCTIONAL_CONTRACT), contract)
        self.assert_code("contract", "ORDER")

    def test_selection_normative_semantic_drift_from_pure_builder_uses_contract(self) -> None:
        contract = _read_json(self.fixture.path(SELECTION_CONTRACT))
        contract["engineering_policies"][0]["notes"] += " Drift."
        _write_json(self.fixture.path(SELECTION_CONTRACT), contract)
        result = self.assert_code("contract", "CONTRACT")
        self.assertIn(SELECTION_CONTRACT, result.stderr)

    def test_selection_base_hash_mismatch_stops_derivation(self) -> None:
        path = self.fixture.path(SELECTION_BASE)
        path.write_bytes(path.read_bytes() + b" ")
        contract = _read_json(self.fixture.path(SELECTION_CONTRACT))
        contract["engineering_policies"][0]["notes"] += " Drift ignored."
        _write_json(self.fixture.path(SELECTION_CONTRACT), contract)
        result = self.assert_code("contract", "CONTRACT")
        self.assertIn(SELECTION_BASE, result.stderr)
        self.assertNotIn(SELECTION_CONTRACT, result.stderr)

    def test_selection_missing_normative_contract_uses_missing(self) -> None:
        self.fixture.path(SELECTION_CONTRACT).unlink()
        result = self.assert_code("contract", "MISSING")
        self.assertIn(SELECTION_CONTRACT, result.stderr)

    def test_selection_malformed_and_non_utf8_normative_contract_use_parse(self) -> None:
        for payload in (b"{\n", b"\xff"):
            with self.subTest(payload=payload), FixtureTree() as fixture:
                fixture.path(SELECTION_CONTRACT).write_bytes(payload)
                result = _run(fixture.root, "contract")
                self.assertEqual(result.returncode, 1)
                self.assertEqual(_codes(result), ["PARSE"])
                self.assertIn(SELECTION_CONTRACT, result.stderr)

    def test_selection_schema_invalid_normative_contract_stops_equality(self) -> None:
        contract = _read_json(self.fixture.path(SELECTION_CONTRACT))
        contract["schema_version"] = "2.0.0"
        _write_json(self.fixture.path(SELECTION_CONTRACT), contract)
        result = self.assert_code("contract", "INSTANCE")
        self.assertNotIn("CONTRACT", _codes(result))

    def test_selection_pure_builder_rejection_uses_locked_base_contract(self) -> None:
        validator = validator_module.Validator(self.fixture.root)
        validator.load_schemas()
        with patch.object(
            validator_module,
            "build_selection_contract",
            side_effect=ValueError("forced rejection"),
        ):
            validator.check_contracts()
        rendered = [diagnostic.render() for diagnostic in sorted(validator.diagnostics)]
        self.assertTrue(
            any(
                "AIUS-P1-CONTRACT: " + SELECTION_BASE
                in line
                and "pure builder rejected locked base: forced rejection" in line
                for line in rendered
            ),
            rendered,
        )

    def test_selection_unreadable_base_uses_parse_without_hash_or_derivation(self) -> None:
        validator = validator_module.Validator(self.fixture.root)
        target = self.fixture.path(SELECTION_BASE)
        original = Path.read_bytes

        def read_bytes(path: Path) -> bytes:
            if path == target:
                raise OSError("forced unreadable base")
            return original(path)

        with patch.object(Path, "read_bytes", read_bytes):
            value = validator._load_historical_base(
                validator_module.SELECTION_BASE,
                validator_module.SELECTION_BASE_HASH,
            )
        self.assertIsNone(value)
        diagnostics = sorted(validator.diagnostics)
        self.assertEqual([diagnostic.code for diagnostic in diagnostics], ["PARSE"])
        self.assertEqual(diagnostics[0].path, SELECTION_BASE)

    def test_selection_unreadable_normative_contract_uses_parse(self) -> None:
        validator = validator_module.Validator(self.fixture.root)
        validator.load_schemas()
        target = self.fixture.path(SELECTION_CONTRACT)
        original = Path.read_text

        def read_text(path: Path, *args: object, **kwargs: object) -> str:
            if path == target:
                raise OSError("forced unreadable contract")
            return original(path, *args, **kwargs)

        with patch.object(Path, "read_text", read_text):
            validator.check_contracts()
        diagnostics = [
            diagnostic
            for diagnostic in sorted(validator.diagnostics)
            if diagnostic.path == SELECTION_CONTRACT
        ]
        self.assertEqual([diagnostic.code for diagnostic in diagnostics], ["PARSE"])


class ContractAndArchitectureDiagnosticTests(ValidatorTestCase):
    def test_invalid_schema_version_constant_uses_schema(self) -> None:
        path = self.fixture.path("evidence/design/functional-topology-schema.json")
        schema = _read_json(path)
        schema["properties"]["schema_version"]["const"] = "2.0.0"
        _write_json(path, schema)
        self.assert_code("contract", "SCHEMA")

    def test_schema_and_instance_key_order_use_order(self) -> None:
        schema_path = self.fixture.path("evidence/design/component-schema.json")
        schema = _read_json(schema_path)
        schema["required"] = list(reversed(schema["required"]))
        _write_json(schema_path, schema)
        self.assert_code("contract", "ORDER")

        with FixtureTree() as fixture:
            functional = fixture.functional
            functional["modes"][0] = {
                key: functional["modes"][0][key]
                for key in reversed(tuple(functional["modes"][0]))
            }
            fixture.write_architecture()
            fixture.render_architecture()
            result = _run(fixture.root, "architecture-base")
            self.assertEqual(result.returncode, 1)
            self.assertIn("ORDER", _codes(result))

    def test_locked_id_order_uses_idset(self) -> None:
        self.fixture.functional["modes"][0], self.fixture.functional["modes"][1] = (
            self.fixture.functional["modes"][1],
            self.fixture.functional["modes"][0],
        )
        self.fixture.write_architecture()
        self.fixture.render_architecture()
        self.assert_code("architecture-base", "IDSET")

    def test_migrated_power_domain_id_order_is_locked(self) -> None:
        domains = self.fixture.interfaces["power_domains"]
        domains[2], domains[3] = domains[3], domains[2]
        self.fixture.write_architecture()
        self.fixture.render_architecture()
        self.assert_code("architecture-base", "IDSET")

    def test_old_data_only_teensy_usb_contract_is_rejected(self) -> None:
        connector = next(
            row
            for row in self.fixture.interfaces["connectors"]
            if row["id"] == "teensy-micro-b"
        )
        connector.update(
            {
                "role": "data",
                "signals": ["USB2-D+", "USB2-D-"],
                "power_domains": ["digital-ground"],
                "mating_constraint": "No second VIN or VUSB power feed is fitted.",
                "notes": "Module USB data access.",
            }
        )
        self.fixture.write_architecture()
        self.fixture.render_architecture()
        result = self.assert_code("architecture-base", "CONTRACT")
        self.assertIn(
            "teensy-micro-b differs from the locked USB power-data contract",
            result.stderr,
        )

    def test_protected_5v_consumers_are_exact_and_ordered(self) -> None:
        mutations = (
            ["teensy", "internal-usb-a"],
            ["internal-usb-a", "teensy", "teensy-micro-b"],
        )
        for consumers in mutations:
            with self.subTest(consumers=consumers), FixtureTree() as fixture:
                domain = next(
                    row
                    for row in fixture.interfaces["power_domains"]
                    if row["id"] == "protected-5v"
                )
                domain["consumers"] = consumers
                fixture.write_architecture()
                fixture.render_architecture()
                result = _run(fixture.root, "architecture-base")
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("CONTRACT", _codes(result), result.stderr)
                self.assertIn(
                    "protected-5v consumers differ from the locked sole-feed contract",
                    result.stderr,
                )

    def test_remote_program_rtc_domains_and_evidence_are_exact(self) -> None:
        mutations = (
            ("power_domains", ["teensy-3v3", "digital-ground"]),
            ("evidence_ids", ["PJRC-TEENSY41-PINOUT"]),
        )
        for field, value in mutations:
            with self.subTest(field=field), FixtureTree() as fixture:
                connector = next(
                    row
                    for row in fixture.interfaces["connectors"]
                    if row["id"] == "remote-program-rtc"
                )
                connector[field] = value
                fixture.write_architecture()
                fixture.render_architecture()
                result = _run(fixture.root, "architecture-base")
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("CONTRACT", _codes(result), result.stderr)
                self.assertIn(
                    f"remote-program-rtc {field} differs from the locked RTC service contract",
                    result.stderr,
                )

    def test_rtc_backup_position_and_semantics_are_locked(self) -> None:
        with FixtureTree() as fixture:
            domains = fixture.interfaces["power_domains"]
            domains[3], domains[4] = domains[4], domains[3]
            fixture.write_architecture()
            fixture.render_architecture()
            result = _run(fixture.root, "architecture-base")
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("IDSET", _codes(result), result.stderr)

        mutations = (
            ("source", "power-entry"),
            ("consumers", ["optical-rx"]),
            ("backfeed_rule", "RTC backup may sustain the core."),
            ("evidence_ids", ["PJRC-TEENSY41-PINOUT"]),
        )
        for field, value in mutations:
            with self.subTest(field=field), FixtureTree() as fixture:
                domain = next(
                    row
                    for row in fixture.interfaces["power_domains"]
                    if row["id"] == "rtc-backup"
                )
                domain[field] = value
                fixture.write_architecture()
                fixture.render_architecture()
                result = _run(fixture.root, "architecture-base")
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("CONTRACT", _codes(result), result.stderr)
                self.assertIn(
                    f"rtc-backup {field} differs from the locked isolated backup-power contract",
                    result.stderr,
                )

    def test_connector_power_domain_reference_is_checked(self) -> None:
        schema_path = self.fixture.path(
            "evidence/design/interfaces-and-states-schema.json"
        )
        schema = _read_json(schema_path)
        schema["$defs"]["powerDomainIdentifier"]["enum"].append("missing-domain")
        _write_json(schema_path, schema)
        connector = next(
            row
            for row in self.fixture.interfaces["connectors"]
            if row["id"] == "remote-program-rtc"
        )
        connector["power_domains"][1] = "missing-domain"
        self.fixture.write_architecture()
        self.fixture.render_architecture()
        result = self.assert_code("architecture-base", "REFERENCE")
        self.assertIn("connectors[7].power_domains", result.stderr)

    def test_rtc_backup_cannot_source_a_core_power_domain(self) -> None:
        domain = next(
            row
            for row in self.fixture.interfaces["power_domains"]
            if row["id"] == "teensy-3v3"
        )
        domain["source"] = "rtc-backup"
        self.fixture.write_architecture()
        self.fixture.render_architecture()
        result = self.assert_code("architecture-base", "CONTRACT")
        self.assertIn("rtc-backup cannot source a core power domain", result.stderr)

    def test_unresolved_control_owner_uses_reference(self) -> None:
        self.fixture.interfaces["controls"][0]["owner"] = "missing-owner"
        self.fixture.write_architecture()
        self.fixture.render_architecture()
        self.assert_code("architecture-base", "REFERENCE")

    def test_every_plural_transition_endpoint_member_is_reference_checked(self) -> None:
        schema_path = self.fixture.path(
            "evidence/design/interfaces-and-states-schema.json"
        )
        schema = _read_json(schema_path)
        schema["$defs"]["operatingStateIdentifier"]["enum"].append("missing-state")
        constraints = schema["$defs"]["transition"]["allOf"]
        schema["$defs"]["transition"]["allOf"] = [
            constraint
            for constraint in constraints
            if constraint["if"]["properties"]["id"]["const"]
            != "active-to-carrier-loss"
        ]
        _write_json(schema_path, schema)
        self.fixture.interfaces["transitions"][5]["from_states"][1] = (
            "missing-state"
        )
        self.fixture.write_architecture()
        self.fixture.render_architecture()
        result = self.assert_code("architecture-base", "REFERENCE")
        self.assertIn("transitions[5].from_states", result.stderr)

    def test_each_architecture_record_family_has_exact_semantic_equality(self) -> None:
        cases = (
            ("functional", "modes"),
            ("functional", "blocks"),
            ("functional", "flows"),
            ("functional", "clock_domains"),
            ("functional", "buffers"),
            ("functional", "latency_boundaries"),
            ("interfaces", "connectors"),
            ("interfaces", "controls"),
            ("interfaces", "expansion_interfaces"),
            ("interfaces", "power_domains"),
            ("interfaces", "operating_states"),
            ("interfaces", "fault_scenarios"),
        )
        for document_name, collection in cases:
            with self.subTest(document=document_name, collection=collection):
                with FixtureTree() as fixture:
                    document = getattr(fixture, document_name)
                    document[collection][0]["notes"] += " Semantic drift."
                    fixture.write_architecture()
                    fixture.render_architecture()
                    result = _run(fixture.root, "architecture-base")
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("CONTRACT", _codes(result), result.stderr)

    def test_transition_sources_targets_and_actions_have_ordered_semantic_equality(self) -> None:
        cases = (
            ("active-to-carrier-loss", "from_states"),
            ("unsupported-to-active", "to_states"),
            ("active-to-carrier-loss", "actions"),
        )
        for transition_id, field in cases:
            with self.subTest(transition=transition_id, field=field):
                with FixtureTree() as fixture:
                    if field in {"from_states", "to_states"}:
                        schema_path = fixture.path(
                            "evidence/design/interfaces-and-states-schema.json"
                        )
                        schema = _read_json(schema_path)
                        constraints = schema["$defs"]["transition"]["allOf"]
                        schema["$defs"]["transition"]["allOf"] = [
                            constraint
                            for constraint in constraints
                            if constraint["if"]["properties"]["id"]["const"]
                            != transition_id
                        ]
                        _write_json(schema_path, schema)
                    transition = next(
                        row
                        for row in fixture.interfaces["transitions"]
                        if row["id"] == transition_id
                    )
                    transition[field] = list(reversed(transition[field]))
                    fixture.write_architecture()
                    fixture.render_architecture()
                    result = _run(fixture.root, "architecture-base")
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("CONTRACT", _codes(result), result.stderr)

    def test_document_and_record_evidence_order_have_semantic_equality(self) -> None:
        for scope in ("document", "record"):
            with self.subTest(scope=scope):
                with FixtureTree() as fixture:
                    if scope == "document":
                        fixture.functional["evidence_ids"].reverse()
                    else:
                        fixture.functional["modes"][0]["evidence_ids"].reverse()
                    fixture.write_architecture()
                    fixture.render_architecture()
                    result = _run(fixture.root, "architecture-base")
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("CONTRACT", _codes(result), result.stderr)

    def test_record_evidence_is_resolved_independently_of_root_evidence(self) -> None:
        self.fixture.functional["modes"][0]["evidence_ids"].append(
            "MISSING-ROW-EVIDENCE"
        )
        self.fixture.write_architecture()
        self.fixture.render_architecture()
        result = self.assert_code("architecture-base", "EVIDENCE")
        self.assertIn("modes[0].evidence_ids", result.stderr)

    def test_wrong_locked_flow_kind_uses_contract(self) -> None:
        self.fixture.functional["flows"][0]["kind"] = "control"
        self.fixture.write_architecture()
        self.fixture.render_architecture()
        self.assert_code("architecture-base", "CONTRACT")

    def test_wrong_fault_safe_outcome_uses_contract(self) -> None:
        schema_path = self.fixture.path(
            "evidence/design/interfaces-and-states-schema.json"
        )
        schema = _read_json(schema_path)
        constraints = schema["$defs"]["faultScenario"]["allOf"]
        schema["$defs"]["faultScenario"]["allOf"] = [
            constraint
            for constraint in constraints
            if constraint["if"]["properties"]["id"]["const"] != "teensy-absent"
        ]
        _write_json(schema_path, schema)
        self.fixture.interfaces["fault_scenarios"][0]["path_select"] = "raw"
        self.fixture.write_architecture()
        self.fixture.render_architecture()
        self.assert_code("architecture-base", "CONTRACT")

    def test_wrong_transition_trigger_mapping_uses_contract(self) -> None:
        schema_path = self.fixture.path(
            "evidence/design/interfaces-and-states-schema.json"
        )
        schema = _read_json(schema_path)
        constraints = schema["$defs"]["transition"]["allOf"]
        schema["$defs"]["transition"]["allOf"] = [
            constraint
            for constraint in constraints
            if constraint["if"]["properties"]["id"]["const"]
            != "power-on-to-reset"
        ]
        _write_json(schema_path, schema)
        self.fixture.interfaces["transitions"][0]["triggers"] = [
            "reset-released"
        ]
        self.fixture.write_architecture()
        self.fixture.render_architecture()
        self.assert_code("architecture-base", "CONTRACT")

    def test_absent_and_noncurrent_evidence_use_evidence(self) -> None:
        self.fixture.interfaces["evidence_ids"].append("MISSING-EVIDENCE")
        self.fixture.write_architecture()
        self.fixture.render_architecture()
        self.assert_code("architecture-base", "EVIDENCE")

        with FixtureTree() as fixture:
            fixture.sources[0]["status"] = "superseded"
            _write_json(fixture.path("evidence/manifest/sources.json"), fixture.sources)
            result = _run(fixture.root, "architecture-base")
            self.assertEqual(result.returncode, 1)
            self.assertIn("EVIDENCE", _codes(result))

    def test_markdown_byte_drift_uses_parity(self) -> None:
        path = self.fixture.path("docs/reference/functional-topology.md")
        path.write_bytes(path.read_bytes() + b"stale\n")
        self.assert_code("architecture-base", "PARITY")


class PolicyDiagnosticTests(ValidatorTestCase):
    def test_policy_semantic_drift_always_uses_contract(self) -> None:
        current = self.fixture.policy["engineering_policies"][0]["scope"]
        self.fixture.policy["engineering_policies"][0]["scope"] = (
            "sourcing" if current != "sourcing" else "safe-default"
        )
        self.fixture.write_policy()
        result = self.assert_code("architecture", "CONTRACT")
        self.assertIn("evidence/design/selection-policy.json", result.stderr)

    def test_engineering_policy_mutation_matrix_preserves_independent_checks(self) -> None:
        with FixtureTree() as fixture:
            rows = fixture.policy["engineering_policies"]
            rows[0], rows[1] = rows[1], rows[0]
            fixture.write_policy()
            result = _run(fixture.root, "architecture")
            self.assertIn("CONTRACT", _codes(result), result.stderr)
            self.assertIn("ORDER", _codes(result), result.stderr)

        for field in ("scope", "rule", "validation_gate", "notes"):
            with self.subTest(field=field), FixtureTree() as fixture:
                row = fixture.policy["engineering_policies"][0]
                if field == "scope":
                    row[field] = "sourcing"
                else:
                    row[field] += " Drift."
                fixture.write_policy()
                result = _run(fixture.root, "architecture")
                self.assertIn("CONTRACT", _codes(result), result.stderr)

    def test_requirement_mutation_matrix_preserves_independent_checks(self) -> None:
        with FixtureTree() as fixture:
            rows = fixture.policy["requirements"]
            rows[0], rows[1] = rows[1], rows[0]
            fixture.write_policy()
            result = _run(fixture.root, "architecture")
            self.assertIn("CONTRACT", _codes(result), result.stderr)
            self.assertIn("ORDER", _codes(result), result.stderr)

        fields = {
            "statement": lambda fixture, row: row["statement"] + " Drift.",
            "basis_ids": lambda fixture, row: copy.deepcopy(
                fixture.policy["requirements"][1]["basis_ids"]
            ),
            "verification_stage": lambda fixture, row: (
                "P1E-sourcing"
                if row["verification_stage"] != "P1E-sourcing"
                else "P1D-electrical"
            ),
            "notes": lambda fixture, row: row["notes"] + " Drift.",
        }
        for field, mutate in fields.items():
            with self.subTest(field=field), FixtureTree() as fixture:
                row = fixture.policy["requirements"][0]
                row[field] = mutate(fixture, row)
                fixture.write_policy()
                result = _run(fixture.root, "architecture")
                self.assertIn("CONTRACT", _codes(result), result.stderr)

    def test_family_authored_field_mutation_matrix_preserves_independent_checks(self) -> None:
        with FixtureTree() as fixture:
            rows = fixture.policy["families"]
            rows[0], rows[1] = rows[1], rows[0]
            fixture.write_policy()
            result = _run(fixture.root, "architecture")
            self.assertIn("CONTRACT", _codes(result), result.stderr)
            self.assertIn("POLICY", _codes(result), result.stderr)

        fields = {
            "requirements": lambda fixture, row: copy.deepcopy(
                fixture.policy["families"][1]["requirements"]
            ),
            "dependent_feature": lambda fixture, row: row["dependent_feature"] + "-drift",
            "notes": lambda fixture, row: row["notes"] + " Drift.",
        }
        for field, mutate in fields.items():
            with self.subTest(field=field), FixtureTree() as fixture:
                row = fixture.policy["families"][0]
                row[field] = mutate(fixture, row)
                fixture.write_policy()
                result = _run(fixture.root, "architecture")
                self.assertIn("CONTRACT", _codes(result), result.stderr)

    def test_each_derived_family_field_mutation_uses_contract(self) -> None:
        mutations = {
            "component_file": "components/usb-a-receptacle.json",
            "critical": False,
            "generic_eligible": True,
            "allowed_statuses": ["selected"],
            "manufacturer_request_id": "REQ-USB-A-RECEPTACLE-MFR",
            "availability_request_id": "REQ-USB-A-RECEPTACLE-UK",
            "manufacturer_source_id": "AIUS-USB-A-RECEPTACLE-MFR",
            "availability_source_id": "AIUS-USB-A-RECEPTACLE-UK",
        }
        for field, value in mutations.items():
            with self.subTest(field=field), FixtureTree() as fixture:
                fixture.policy["families"][0][field] = copy.deepcopy(value)
                fixture.write_policy()
                result = _run(fixture.root, "architecture")
                codes = _codes(result)
                self.assertIn("CONTRACT", codes, result.stderr)
                self.assertTrue(
                    {"POLICY", "INSTANCE"} & set(codes),
                    result.stderr,
                )

    def test_family_order_basis_and_dependent_feature_contracts_use_policy(self) -> None:
        self.fixture.policy["families"][0], self.fixture.policy["families"][1] = (
            self.fixture.policy["families"][1],
            self.fixture.policy["families"][0],
        )
        self.fixture.write_policy()
        self.assert_code("architecture", "POLICY")

        with FixtureTree() as fixture:
            fixture.policy["requirements"][0]["basis_ids"] = ["MISSING-BASIS"]
            fixture.write_policy()
            result = _run(fixture.root, "architecture")
            self.assertEqual(result.returncode, 1)
            self.assertIn("POLICY", _codes(result))

        with FixtureTree() as fixture:
            duplicate = fixture.policy["families"][0]["dependent_feature"]
            fixture.policy["families"][1]["dependent_feature"] = duplicate
            fixture.write_policy()
            result = _run(fixture.root, "architecture")
            self.assertEqual(result.returncode, 1)
            self.assertIn("POLICY", _codes(result))

    def test_source_and_engineering_policy_namespace_overlap_uses_policy(self) -> None:
        policy_id = self.fixture.policy["engineering_policies"][0]["id"]
        self.fixture.sources.append(_source(policy_id))
        self.fixture.sources.sort(key=lambda row: row["id"])
        _write_json(self.fixture.path("evidence/manifest/sources.json"), self.fixture.sources)
        self.assert_code("architecture", "POLICY")


class SelectionFragmentDiagnosticTests(ValidatorTestCase):
    def test_component_linkage_and_alternative_pair_use_component(self) -> None:
        family = "usb-c-receptacle"
        component = self.fixture.components[family]
        component["requirements"] = ["REQT-OTHER"]
        self.fixture.write_component(family)
        self.assert_code("selection-fragments", "COMPONENT", family)

        with FixtureTree() as fixture:
            component = fixture.components[family]
            component["alternatives"] = [
                {
                    "manufacturer": component["manufacturer"],
                    "mpn": component["mpn"],
                    "selection_rationale": "Duplicate selected pair.",
                    "authority_url": component["authority_url"],
                }
            ]
            fixture.write_component(family)
            result = _run(fixture.root, "selection-fragments", family)
            self.assertEqual(result.returncode, 1)
            self.assertIn("COMPONENT", _codes(result))

    def test_both_component_request_ids_must_equal_policy(self) -> None:
        family = "usb-c-receptacle"
        cases = (
            ("manufacturer_request_id", "REQ-USB-A-RECEPTACLE-MFR"),
            ("availability_request_id", "REQ-USB-A-RECEPTACLE-UK"),
        )
        for field, value in cases:
            with self.subTest(field=field):
                with FixtureTree() as fixture:
                    fixture.components[family][field] = value
                    fixture.write_component(family)
                    result = _run(fixture.root, "selection-fragments", family)
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("COMPONENT", _codes(result))

    def test_request_component_and_status_linkage_use_request(self) -> None:
        family = "usb-c-receptacle"
        request_id = f"REQ-{family.upper()}-MFR"
        self.fixture.requests[request_id]["selected_mpn"] = "OTHER-MPN"
        self.fixture.write_request(request_id)
        self.assert_code("selection-fragments", "REQUEST", family)

        with FixtureTree() as fixture:
            fixture.components[family]["status"] = "selected"
            fixture.write_component(family)
            result = _run(fixture.root, "selection-fragments", family)
            self.assertEqual(result.returncode, 1)
            self.assertIn("REQUEST", _codes(result))

    def test_exact_component_and_request_basename_sets_use_fileset(self) -> None:
        _write_json(
            self.fixture.path("evidence/design/components/extra.json"),
            self.fixture.components["usb-c-receptacle"],
        )
        self.assert_code("selection-fragments", "FILESET")

        with FixtureTree() as fixture:
            fixture.path("evidence/manifest/requests/REQ-USB-C-RECEPTACLE-UK.json").unlink()
            result = _run(fixture.root, "selection-fragments")
            self.assertEqual(result.returncode, 1)
            self.assertIn("FILESET", _codes(result))

    def test_unscoped_invalid_component_does_not_derive_request_fileset(self) -> None:
        family = "usb-c-receptacle"
        for mutation, expected_code in (("parse", "PARSE"), ("instance", "INSTANCE")):
            with self.subTest(mutation=mutation):
                with FixtureTree() as fixture:
                    path = fixture.path(f"evidence/design/components/{family}.json")
                    if mutation == "parse":
                        path.write_text("{\n", encoding="utf-8")
                    else:
                        fixture.components[family]["status"] = "invalid-status"
                        fixture.write_component(family)
                    result = _run(fixture.root, "selection-fragments")
                    self.assertEqual(result.returncode, 1)
                    self.assertIn(expected_code, _codes(result))
                    self.assertNotIn("FILESET", _codes(result), result.stderr)

    def test_critical_unavailable_blocks_only_unscoped_completion(self) -> None:
        family = "usb-c-receptacle"
        self.fixture.components[family]["status"] = "candidate-evidence-unavailable"
        self.fixture.write_component(family)
        request_id = f"REQ-{family.upper()}-MFR"
        self.fixture.requests[request_id]["status"] = "unavailable"
        self.fixture.requests[request_id]["notes"] = "Manufacturer evidence unavailable."
        self.fixture.write_request(request_id)
        self.assert_ok("selection-fragments", family)
        self.assert_code("selection-fragments", "COMPONENT")

    def test_satisfied_manufacturer_request_requires_current_archived_source(self) -> None:
        family = "usb-c-receptacle"
        self.fixture.select_family(family)
        self.assert_ok("selection-fragments", family)

        with FixtureTree() as fixture:
            fixture.components[family]["status"] = "selected"
            fixture.write_component(family)
            request_id = f"REQ-{family.upper()}-MFR"
            request = fixture.requests[request_id]
            request["status"] = "satisfied"
            request["evidence_ids"] = [request["source_id"]]
            fixture.write_request(request_id)
            fixture.add_selected_manufacturer_source(family, archived=False)
            result = _run(fixture.root, "selection-fragments", family)
            self.assertEqual(result.returncode, 1)
            self.assertIn("REQUEST", _codes(result))

    def test_satisfied_manufacturer_source_requires_matching_mpn_observation(self) -> None:
        family = "usb-c-receptacle"
        for observed_mpn in ("WRONG-MPN", None):
            with self.subTest(observed_mpn=observed_mpn):
                with FixtureTree() as fixture:
                    fixture.components[family]["status"] = "selected"
                    fixture.write_component(family)
                    request_id = f"REQ-{family.upper()}-MFR"
                    request = fixture.requests[request_id]
                    request["status"] = "satisfied"
                    request["evidence_ids"] = [request["source_id"]]
                    fixture.write_request(request_id)
                    fixture.add_selected_manufacturer_source(
                        family, observed_mpn=observed_mpn
                    )
                    result = _run(fixture.root, "selection-fragments", family)
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("REQUEST", _codes(result))

    def test_locked_cliff_seed_source_mpn_pairs_need_no_observation(self) -> None:
        for family, source_id, mpn in (
            ("cliff-rx", "CLIFF-ORJ5", "ORJ5"),
            ("cliff-tx", "CLIFF-OTJ5", "OTJ5"),
        ):
            with self.subTest(family=family):
                with FixtureTree() as fixture:
                    fixture.components[family]["status"] = "selected"
                    fixture.write_component(family)
                    request_id = f"REQ-{family.upper()}-MFR"
                    request = fixture.requests[request_id]
                    self.assertEqual(request["source_id"], source_id)
                    self.assertEqual(request["selected_mpn"], mpn)
                    request["status"] = "satisfied"
                    request["evidence_ids"] = [source_id]
                    fixture.write_request(request_id)
                    source = next(
                        row for row in fixture.sources if row["id"] == source_id
                    )
                    source["artifacts"] = [
                        {"storage": "lfs", "retrieval_status": "current"}
                    ]
                    _write_json(
                        fixture.path("evidence/manifest/sources.json"),
                        fixture.sources,
                    )
                    result = _run(
                        fixture.root, "selection-fragments", family
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)

        with FixtureTree() as fixture:
            family = "cliff-rx"
            fixture.components[family]["status"] = "selected"
            fixture.components[family]["mpn"] = "WRONG-MPN"
            fixture.write_component(family)
            request_id = f"REQ-{family.upper()}-MFR"
            request = fixture.requests[request_id]
            request["selected_mpn"] = "WRONG-MPN"
            request["status"] = "satisfied"
            request["evidence_ids"] = ["CLIFF-ORJ5"]
            fixture.write_request(request_id)
            source = next(
                row for row in fixture.sources if row["id"] == "CLIFF-ORJ5"
            )
            source["artifacts"] = [
                {"storage": "lfs", "retrieval_status": "current"}
            ]
            _write_json(
                fixture.path("evidence/manifest/sources.json"), fixture.sources
            )
            result = _run(fixture.root, "selection-fragments", family)
            self.assertEqual(result.returncode, 1)
            self.assertIn("REQUEST", _codes(result))

    def test_satisfied_uk_request_requires_dynamic_matching_mpn_observation(self) -> None:
        family = "usb-c-receptacle"
        request_id = f"REQ-{family.upper()}-UK"
        request = self.fixture.requests[request_id]
        request["status"] = "satisfied"
        request["evidence_ids"] = [request["source_id"]]
        self.fixture.write_request(request_id)
        self.fixture.add_selected_uk_source(family)
        self.assert_ok("selection-fragments", family)

        with FixtureTree() as fixture:
            request = fixture.requests[request_id]
            request["status"] = "satisfied"
            request["evidence_ids"] = [request["source_id"]]
            fixture.write_request(request_id)
            fixture.add_selected_uk_source(family, matching_mpn=False)
            result = _run(fixture.root, "selection-fragments", family)
            self.assertEqual(result.returncode, 1)
            self.assertIn("REQUEST", _codes(result))


class AggregateParityTests(ValidatorTestCase):
    def test_component_csv_byte_drift_uses_parity(self) -> None:
        path = self.fixture.path("evidence/design/provisional-components.csv")
        path.write_bytes(path.read_bytes() + b"stale\n")
        self.assert_code("selection", "PARITY")

    def test_supplemental_aggregate_drift_and_malformed_json_use_parity_and_parse(self) -> None:
        aggregate = _read_json(
            self.fixture.path("evidence/manifest/supplemental-requests.json")
        )
        aggregate["requests"].pop()
        _write_json(
            self.fixture.path("evidence/manifest/supplemental-requests.json"),
            aggregate,
        )
        self.assert_code("handoff", "PARITY")

        with FixtureTree() as fixture:
            fixture.path("evidence/manifest/supplemental-requests.json").write_text("{\n", encoding="utf-8")
            result = _run(fixture.root, "handoff")
            self.assertEqual(result.returncode, 1)
            self.assertIn("PARSE", _codes(result))

    def test_handoff_membership_includes_satisfied_requests(self) -> None:
        family = "usb-c-receptacle"
        self.fixture.select_family(family)
        self.fixture.render_selection()
        self.fixture.render_handoff()
        self.assert_ok("handoff")


if __name__ == "__main__":
    unittest.main(verbosity=2)
