#!/usr/bin/env python3
"""Executable contract tests for Aius P1C selection and assumption schemas."""

from __future__ import annotations

import copy
import json
import sys
import unittest
from pathlib import Path

from jsonschema_subset import DRAFT_2020_12, ValidationError, check_schema, validate


AIUS_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_ROOT = AIUS_ROOT / "evidence" / "manifest"
DESIGN_ROOT = AIUS_ROOT / "evidence" / "design"
FIXTURE_ROOT = Path(__file__).resolve().parent / "fixtures" / "p1c-schema" / "selection"


def _load_json(path: Path) -> object:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


SOURCE_REQUEST_SCHEMA = _load_json(MANIFEST_ROOT / "source-request-schema.json")
COMPONENT_SCHEMA = _load_json(DESIGN_ROOT / "component-schema.json")
SELECTION_POLICY_SCHEMA = _load_json(DESIGN_ROOT / "selection-policy-schema.json")
ASSUMPTION_SCHEMA = _load_json(MANIFEST_ROOT / "assumption-schema.json")
ASSUMPTIONS = _load_json(MANIFEST_ROOT / "assumptions.json")
VALID_COMPONENTS = _load_json(FIXTURE_ROOT / "valid-components.json")
VALID_REQUESTS = _load_json(FIXTURE_ROOT / "valid-source-requests.json")
VALID_POLICY = _load_json(FIXTURE_ROOT / "valid-selection-policy.json")

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
POLICY_ROOT_KEYS = ("schema_version", "engineering_policies", "requirements", "families")
ENGINEERING_POLICY_KEYS = ("id", "scope", "rule", "validation_gate", "notes")
REQUIREMENT_KEYS = ("id", "statement", "basis_ids", "verification_stage", "notes")
FAMILY_KEYS = (
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
)


def _component(status: str) -> dict[str, object]:
    return copy.deepcopy(next(row for row in VALID_COMPONENTS if row["status"] == status))


def _request(request_kind: str, status: str) -> dict[str, object]:
    return copy.deepcopy(
        next(
            row
            for row in VALID_REQUESTS
            if row["request_kind"] == request_kind and row["status"] == status
        )
    )


def _policy() -> dict[str, object]:
    return copy.deepcopy(VALID_POLICY)


class SelectionSchemaTestCase(unittest.TestCase):
    def assert_invalid(
        self,
        instance: object,
        schema: object,
        *,
        root_schema: object | None = None,
    ) -> None:
        with self.assertRaises(ValidationError):
            validate(instance, schema, root_schema=root_schema)


class SchemaMetaAndFixtureTests(SelectionSchemaTestCase):
    def test_all_committed_schemas_use_draft_2020_12_and_pass_subset_check(self) -> None:
        for schema in (
            SOURCE_REQUEST_SCHEMA,
            COMPONENT_SCHEMA,
            SELECTION_POLICY_SCHEMA,
            ASSUMPTION_SCHEMA,
        ):
            with self.subTest(schema_id=schema["$id"]):
                self.assertEqual(schema["$schema"], DRAFT_2020_12)
                check_schema(schema)

    def test_contract_field_order_is_locked(self) -> None:
        for schema, expected in (
            (COMPONENT_SCHEMA, COMPONENT_KEYS),
            (SOURCE_REQUEST_SCHEMA, REQUEST_KEYS),
            (SELECTION_POLICY_SCHEMA, POLICY_ROOT_KEYS),
        ):
            with self.subTest(schema_id=schema["$id"]):
                self.assertEqual(tuple(schema["required"]), expected)
                self.assertEqual(tuple(schema["properties"]), expected)

        for definition, expected in (
            ("engineeringPolicy", ENGINEERING_POLICY_KEYS),
            ("requirement", REQUIREMENT_KEYS),
            ("family", FAMILY_KEYS),
        ):
            with self.subTest(definition=definition):
                record_schema = SELECTION_POLICY_SCHEMA["$defs"][definition]
                self.assertEqual(tuple(record_schema["required"]), expected)
                self.assertEqual(tuple(record_schema["properties"]), expected)

    def test_all_valid_fixtures_validate(self) -> None:
        for component in VALID_COMPONENTS:
            with self.subTest(component_status=component["status"]):
                validate(component, COMPONENT_SCHEMA)
        for request in VALID_REQUESTS:
            with self.subTest(request_kind=request["request_kind"], status=request["status"]):
                validate(request, SOURCE_REQUEST_SCHEMA)
        validate(VALID_POLICY, SELECTION_POLICY_SCHEMA)

    def test_all_invalid_fixtures_are_rejected(self) -> None:
        fixtures = (
            ("component-duplicate-alternative.json", COMPONENT_SCHEMA),
            ("source-request-kind-mismatch.json", SOURCE_REQUEST_SCHEMA),
            ("selection-policy-critical-unresolved.json", SELECTION_POLICY_SCHEMA),
        )
        for filename, schema in fixtures:
            with self.subTest(filename=filename):
                self.assert_invalid(_load_json(FIXTURE_ROOT / "invalid" / filename), schema)

    def test_current_six_assumptions_validate_and_equal_sorted_fragments(self) -> None:
        validate(ASSUMPTIONS, ASSUMPTION_SCHEMA)
        self.assertEqual(len(ASSUMPTIONS), 6)
        fragments = [
            _load_json(path)
            for path in sorted((MANIFEST_ROOT / "assumption-fragments").glob("*.json"))
        ]
        fragments.sort(key=lambda row: row["id"])
        self.assertEqual(ASSUMPTIONS, fragments)


class SourceRequestSchemaTests(SelectionSchemaTestCase):
    def test_both_request_kinds_cover_every_status(self) -> None:
        self.assertEqual(
            {(row["request_kind"], row["status"]) for row in VALID_REQUESTS},
            {
                (request_kind, status)
                for request_kind in ("manufacturer-evidence", "uk-availability")
                for status in ("pending", "satisfied", "unavailable")
            },
        )

    def test_all_fields_are_required_and_additional_properties_are_rejected(self) -> None:
        for field in REQUEST_KEYS:
            request = _request("manufacturer-evidence", "pending")
            del request[field]
            with self.subTest(field=field):
                self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

        request = _request("manufacturer-evidence", "pending")
        request["unexpected"] = True
        self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

    def test_identifier_slug_requirement_url_and_selected_string_patterns(self) -> None:
        invalid_values = (
            ("request_id", "REQ-bad-MFR"),
            ("family", "Power_Protection"),
            ("requirements", ["REQ-POWER"]),
            ("requirements", ["REQT-lowercase"]),
            ("authority_url", "http://manufacturer.example/ex1234"),
            ("selected_manufacturer", ""),
            ("selected_mpn", "   "),
            ("source_id", "lowercase-source"),
        )
        for field, value in invalid_values:
            request = _request("manufacturer-evidence", "pending")
            request[field] = value
            with self.subTest(field=field, value=value):
                self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

        request = _request("manufacturer-evidence", "satisfied")
        request["evidence_ids"] = ["bad-evidence-id"]
        self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

    def test_manufacturer_kind_locks_request_source_artifacts_and_retention(self) -> None:
        allowed_artifacts = ("datasheet", "drawing", "cad", "product-page", "reference-design")
        for artifact_kind in allowed_artifacts:
            request = _request("manufacturer-evidence", "pending")
            request["artifact_kinds"] = [artifact_kind]
            with self.subTest(artifact_kind=artifact_kind):
                validate(request, SOURCE_REQUEST_SCHEMA)

        invalid_values = (
            ("request_id", "REQ-POWER-PROTECTION-UK"),
            ("source_id", "AIUS-POWER-PROTECTION-UK"),
            ("source_id", "OTHER-SOURCE"),
            ("artifact_kinds", ["price-observation"]),
            ("retention", "normalized-observation-only"),
        )
        for field, value in invalid_values:
            request = _request("manufacturer-evidence", "pending")
            request[field] = value
            with self.subTest(field=field, value=value):
                self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

    def test_only_two_cliff_manufacturer_source_exceptions_are_valid(self) -> None:
        for source_id in ("CLIFF-ORJ5", "CLIFF-OTJ5"):
            request = _request("manufacturer-evidence", "pending")
            request["source_id"] = source_id
            with self.subTest(source_id=source_id):
                validate(request, SOURCE_REQUEST_SCHEMA)

        request = _request("manufacturer-evidence", "pending")
        request["source_id"] = "CLIFF-OTHER"
        self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

    def test_uk_kind_locks_request_source_artifact_and_retention(self) -> None:
        invalid_values = (
            ("request_id", "REQ-POWER-PROTECTION-MFR"),
            ("source_id", "AIUS-POWER-PROTECTION-MFR"),
            ("source_id", "CLIFF-ORJ5"),
            ("artifact_kinds", ["datasheet"]),
            ("artifact_kinds", ["price-observation", "product-page"]),
            ("retention", "archive"),
        )
        for field, value in invalid_values:
            request = _request("uk-availability", "pending")
            request[field] = value
            with self.subTest(field=field, value=value):
                self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

    def test_array_emptiness_uniqueness_and_vocabulary_are_enforced(self) -> None:
        invalid_values = (
            ("requirements", []),
            ("requirements", ["REQT-SAME", "REQT-SAME"]),
            ("artifact_kinds", []),
            ("artifact_kinds", ["datasheet", "datasheet"]),
            ("artifact_kinds", ["source-snapshot"]),
        )
        for field, value in invalid_values:
            request = _request("manufacturer-evidence", "pending")
            request[field] = value
            with self.subTest(field=field, value=value):
                self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

        request = _request("manufacturer-evidence", "satisfied")
        request["evidence_ids"] = ["SAME-EVIDENCE", "SAME-EVIDENCE"]
        self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

    def test_status_evidence_and_failure_note_invariants_hold_for_both_kinds(self) -> None:
        for request_kind in ("manufacturer-evidence", "uk-availability"):
            pending = _request(request_kind, "pending")
            pending["evidence_ids"] = [pending["source_id"]]
            satisfied = _request(request_kind, "satisfied")
            satisfied["evidence_ids"] = []
            unavailable_with_evidence = _request(request_kind, "unavailable")
            unavailable_with_evidence["evidence_ids"] = [unavailable_with_evidence["source_id"]]
            unavailable_without_note = _request(request_kind, "unavailable")
            unavailable_without_note["notes"] = "   "

            for request in (
                pending,
                satisfied,
                unavailable_with_evidence,
                unavailable_without_note,
            ):
                with self.subTest(request_kind=request_kind, status=request["status"]):
                    self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

    def test_unknown_kind_status_and_retention_are_rejected(self) -> None:
        for field, value in (
            ("request_kind", "distributor-page"),
            ("status", "acquired"),
            ("retention", "snapshot"),
        ):
            request = _request("manufacturer-evidence", "pending")
            request[field] = value
            with self.subTest(field=field):
                self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)


class ComponentSchemaTests(SelectionSchemaTestCase):
    def test_all_five_statuses_have_valid_fixtures(self) -> None:
        self.assertEqual(
            {row["status"] for row in VALID_COMPONENTS},
            {
                "candidate-pending-evidence",
                "selected",
                "candidate-evidence-unavailable",
                "generic-allowance",
                "unresolved",
            },
        )

    def test_all_fields_are_required_and_additional_properties_are_rejected(self) -> None:
        for field in COMPONENT_KEYS:
            component = _component("selected")
            del component[field]
            with self.subTest(field=field):
                self.assert_invalid(component, COMPONENT_SCHEMA)

        component = _component("selected")
        component["unexpected"] = True
        self.assert_invalid(component, COMPONENT_SCHEMA)

    def test_block_requirement_and_request_identifier_patterns(self) -> None:
        invalid_values = (
            ("block", "Optical_RX"),
            ("requirements", []),
            ("requirements", ["REQ-OPTICAL-RX"]),
            ("requirements", ["REQT-SAME", "REQT-SAME"]),
            ("manufacturer_request_id", "REQ-OPTICAL-RX-UK"),
            ("availability_request_id", "REQ-OPTICAL-RX-MFR"),
            ("notes", None),
        )
        for field, value in invalid_values:
            component = _component("selected")
            component[field] = value
            with self.subTest(field=field, value=value):
                self.assert_invalid(component, COMPONENT_SCHEMA)

    def test_alternative_shape_count_uniqueness_and_https_url(self) -> None:
        base_alternative = copy.deepcopy(_component("selected")["alternatives"][0])
        for count in range(3):
            component = _component("selected")
            component["alternatives"] = []
            for index in range(count):
                alternative = copy.deepcopy(base_alternative)
                alternative["mpn"] = f"ALT-{index}"
                component["alternatives"].append(alternative)
            with self.subTest(count=count):
                validate(component, COMPONENT_SCHEMA)

        component = _component("selected")
        component["alternatives"] = [base_alternative, base_alternative]
        self.assert_invalid(component, COMPONENT_SCHEMA)

        component = _component("selected")
        component["alternatives"] = [base_alternative, copy.deepcopy(base_alternative), copy.deepcopy(base_alternative)]
        component["alternatives"][1]["mpn"] = "ALT-1"
        component["alternatives"][2]["mpn"] = "ALT-2"
        self.assert_invalid(component, COMPONENT_SCHEMA)

        for field in tuple(base_alternative):
            component = _component("selected")
            alternative = copy.deepcopy(base_alternative)
            del alternative[field]
            component["alternatives"] = [alternative]
            with self.subTest(field=field, case="missing"):
                self.assert_invalid(component, COMPONENT_SCHEMA)

        for field, value in (
            ("manufacturer", ""),
            ("mpn", "   "),
            ("selection_rationale", ""),
            ("authority_url", "http://manufacturer.example/alt"),
        ):
            component = _component("selected")
            alternative = copy.deepcopy(base_alternative)
            alternative[field] = value
            component["alternatives"] = [alternative]
            with self.subTest(field=field, case="invalid"):
                self.assert_invalid(component, COMPONENT_SCHEMA)

        component = _component("selected")
        alternative = copy.deepcopy(base_alternative)
        alternative["unexpected"] = True
        component["alternatives"] = [alternative]
        self.assert_invalid(component, COMPONENT_SCHEMA)

    def test_each_selected_candidate_status_requires_every_selected_field(self) -> None:
        statuses = ("candidate-pending-evidence", "selected", "candidate-evidence-unavailable")
        for status in statuses:
            for field in (
                "manufacturer",
                "mpn",
                "selection_rationale",
                "authority_url",
                "manufacturer_request_id",
                "availability_request_id",
            ):
                component = _component(status)
                component[field] = None
                with self.subTest(status=status, field=field):
                    self.assert_invalid(component, COMPONENT_SCHEMA)

            component = _component(status)
            component["generic_policy"] = {"value": "1", "package": "0603", "tolerance": "1%"}
            with self.subTest(status=status, field="generic_policy"):
                self.assert_invalid(component, COMPONENT_SCHEMA)

    def test_generic_allowance_requires_null_selection_and_complete_policy(self) -> None:
        forbidden = {
            "manufacturer": "Example Semiconductor",
            "mpn": "EX1234",
            "selection_rationale": "Part-specific rationale",
            "authority_url": "https://manufacturer.example/ex1234",
            "manufacturer_request_id": "REQ-EXAMPLE-MFR",
            "availability_request_id": "REQ-EXAMPLE-UK",
        }
        for field, value in forbidden.items():
            component = _component("generic-allowance")
            component[field] = value
            with self.subTest(field=field):
                self.assert_invalid(component, COMPONENT_SCHEMA)

        component = _component("generic-allowance")
        component["generic_policy"] = None
        self.assert_invalid(component, COMPONENT_SCHEMA)

        for field in ("value", "package", "tolerance"):
            component = _component("generic-allowance")
            del component["generic_policy"][field]
            with self.subTest(field=field, case="missing"):
                self.assert_invalid(component, COMPONENT_SCHEMA)

            component = _component("generic-allowance")
            component["generic_policy"][field] = "   "
            with self.subTest(field=field, case="empty"):
                self.assert_invalid(component, COMPONENT_SCHEMA)

        component = _component("generic-allowance")
        component["generic_policy"]["unexpected"] = "forbidden"
        self.assert_invalid(component, COMPONENT_SCHEMA)

    def test_unresolved_requires_rationale_and_forbids_selection_and_generic_policy(self) -> None:
        forbidden = {
            "manufacturer": "Example Semiconductor",
            "mpn": "EX1234",
            "authority_url": "https://manufacturer.example/ex1234",
            "manufacturer_request_id": "REQ-EXAMPLE-MFR",
            "availability_request_id": "REQ-EXAMPLE-UK",
            "generic_policy": {"value": "1", "package": "0603", "tolerance": "1%"},
        }
        for field, value in forbidden.items():
            component = _component("unresolved")
            component[field] = value
            with self.subTest(field=field):
                self.assert_invalid(component, COMPONENT_SCHEMA)

        for rationale in (None, "   "):
            component = _component("unresolved")
            component["selection_rationale"] = rationale
            with self.subTest(rationale=rationale):
                self.assert_invalid(component, COMPONENT_SCHEMA)

    def test_unknown_status_is_rejected(self) -> None:
        component = _component("selected")
        component["status"] = "candidate"
        self.assert_invalid(component, COMPONENT_SCHEMA)


class SelectionPolicySchemaTests(SelectionSchemaTestCase):
    def test_root_fields_are_required_and_additional_properties_are_rejected(self) -> None:
        for field in POLICY_ROOT_KEYS:
            policy = _policy()
            del policy[field]
            with self.subTest(field=field):
                self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)

        for field, value in (("schema_version", "2.0.0"), ("unexpected", True)):
            policy = _policy()
            policy[field] = value
            with self.subTest(field=field):
                self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)

    def test_engineering_policy_fields_patterns_scopes_and_additional_properties(self) -> None:
        scopes = {
            "product-topology",
            "safe-default",
            "manufacturability",
            "sourcing",
            "provisional-design-choice",
        }
        self.assertEqual({row["scope"] for row in VALID_POLICY["engineering_policies"]}, scopes)

        for field in ENGINEERING_POLICY_KEYS:
            policy = _policy()
            del policy["engineering_policies"][0][field]
            with self.subTest(field=field):
                self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)

        for field, value in (
            ("id", "POLICY-SAFE-DEFAULT"),
            ("id", "EP-lowercase"),
            ("scope", "component-capability"),
            ("rule", "   "),
            ("validation_gate", ""),
            ("notes", None),
        ):
            policy = _policy()
            policy["engineering_policies"][0][field] = value
            with self.subTest(field=field, value=value):
                self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)

        policy = _policy()
        policy["engineering_policies"][0]["unexpected"] = True
        self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)

    def test_requirement_fields_patterns_basis_and_verification_stages(self) -> None:
        stages = {"P1D-electrical", "P1D-mechanical", "P1E-sourcing", "H1-schematic", "H2-placement"}
        self.assertEqual({row["verification_stage"] for row in VALID_POLICY["requirements"]}, stages)

        for field in REQUIREMENT_KEYS:
            policy = _policy()
            del policy["requirements"][0][field]
            with self.subTest(field=field):
                self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)

        for field, value in (
            ("id", "REQ-POWER"),
            ("id", "REQT-lowercase"),
            ("statement", "   "),
            ("basis_ids", []),
            ("basis_ids", ["bad-basis"]),
            ("basis_ids", ["EP-SAFE-DEFAULT", "EP-SAFE-DEFAULT"]),
            ("verification_stage", "P1C-selection"),
            ("notes", None),
        ):
            policy = _policy()
            policy["requirements"][0][field] = value
            with self.subTest(field=field, value=value):
                self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)

        policy = _policy()
        policy["requirements"][0]["unexpected"] = True
        self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)

    def test_family_fields_patterns_arrays_and_additional_properties(self) -> None:
        for field in FAMILY_KEYS:
            policy = _policy()
            del policy["families"][0][field]
            with self.subTest(field=field):
                self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)

        invalid_values = (
            ("family", "Power_Protection"),
            ("component_file", "power-protection.json"),
            ("component_file", "components/Power_Protection.json"),
            ("critical", "yes"),
            ("generic_eligible", 0),
            ("allowed_statuses", []),
            ("allowed_statuses", ["selected", "selected"]),
            ("allowed_statuses", ["acquired"]),
            ("requirements", []),
            ("requirements", ["REQ-POWER"]),
            ("requirements", ["REQT-SAME", "REQT-SAME"]),
            ("manufacturer_request_id", "REQ-POWER-PROTECTION-UK"),
            ("availability_request_id", "REQ-POWER-PROTECTION-MFR"),
            ("manufacturer_source_id", "OTHER-SOURCE"),
            ("availability_source_id", "AIUS-POWER-PROTECTION-MFR"),
            ("dependent_feature", "Protected_Power"),
            ("notes", None),
        )
        for field, value in invalid_values:
            policy = _policy()
            policy["families"][0][field] = value
            with self.subTest(field=field, value=value):
                self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)

        policy = _policy()
        policy["families"][0]["unexpected"] = True
        self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)

    def test_generic_family_has_exact_status_and_no_request_or_source_ids(self) -> None:
        for field, value in {
            "manufacturer_request_id": "REQ-LOGIC-DECOUPLING-CAPACITOR-MFR",
            "availability_request_id": "REQ-LOGIC-DECOUPLING-CAPACITOR-UK",
            "manufacturer_source_id": "AIUS-LOGIC-DECOUPLING-CAPACITOR-MFR",
            "availability_source_id": "AIUS-LOGIC-DECOUPLING-CAPACITOR-UK",
        }.items():
            policy = _policy()
            policy["families"][2][field] = value
            with self.subTest(field=field):
                self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)

        for statuses in (["selected"], ["generic-allowance", "selected"]):
            policy = _policy()
            policy["families"][2]["allowed_statuses"] = statuses
            with self.subTest(statuses=statuses):
                self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)

    def test_non_generic_family_requires_all_request_and_source_ids(self) -> None:
        for field in (
            "manufacturer_request_id",
            "availability_request_id",
            "manufacturer_source_id",
            "availability_source_id",
        ):
            policy = _policy()
            policy["families"][0][field] = None
            with self.subTest(field=field):
                self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)

        policy = _policy()
        policy["families"][0]["allowed_statuses"] = ["generic-allowance"]
        self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)

    def test_critical_family_excludes_unresolved_but_noncritical_family_allows_it(self) -> None:
        policy = _policy()
        policy["families"][0]["allowed_statuses"] = ["selected", "unresolved"]
        self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)

        policy = _policy()
        policy["families"][1]["allowed_statuses"] = ["unresolved"]
        validate(policy, SELECTION_POLICY_SCHEMA)

    def test_family_manufacturer_source_allows_only_aius_or_two_cliff_exceptions(self) -> None:
        for source_id in ("AIUS-OPTICAL-RX-MFR", "CLIFF-ORJ5", "CLIFF-OTJ5"):
            policy = _policy()
            policy["families"][1]["manufacturer_source_id"] = source_id
            with self.subTest(source_id=source_id):
                validate(policy, SELECTION_POLICY_SCHEMA)

        policy = _policy()
        policy["families"][1]["manufacturer_source_id"] = "CLIFF-OTHER"
        self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)

    def test_exact_duplicate_registry_rows_are_rejected(self) -> None:
        for collection in ("engineering_policies", "requirements", "families"):
            policy = _policy()
            policy[collection].append(copy.deepcopy(policy[collection][0]))
            with self.subTest(collection=collection):
                self.assert_invalid(policy, SELECTION_POLICY_SCHEMA)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
