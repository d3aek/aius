#!/usr/bin/env python3
"""Executable contract tests for Aius selection and assumption schemas."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

from jsonschema_subset import ValidationError, check_schema, validate


AIUS_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_ROOT = AIUS_ROOT / "evidence" / "manifest"
DESIGN_ROOT = AIUS_ROOT / "evidence" / "design"


def _load_json(path: Path) -> object:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


SOURCE_REQUEST_SCHEMA = _load_json(MANIFEST_ROOT / "source-request-schema.json")
COMPONENT_SCHEMA = _load_json(DESIGN_ROOT / "component-schema.json")
ASSUMPTION_SCHEMA = _load_json(MANIFEST_ROOT / "assumption-schema.json")
ASSUMPTIONS = _load_json(MANIFEST_ROOT / "assumptions.json")


def _source_request(status: str = "pending") -> dict[str, object]:
    request: dict[str, object] = {
        "request_id": "REQ-EXAMPLE-PART",
        "family": "example-part",
        "requirements": ["Operates at 3.3 V", "Available in the UK"],
        "authority_url": "https://example.com/manufacturer/example-part",
        "selected_manufacturer": "Example Semiconductor",
        "selected_mpn": "EX1234",
        "source_id": "EXAMPLE-EX1234-DATASHEET",
        "artifact_kinds": ["datasheet", "product-page"],
        "status": status,
        "evidence_ids": [],
        "notes": "",
    }
    if status == "satisfied":
        request["evidence_ids"] = ["EXAMPLE-EX1234-DATASHEET"]
    elif status == "unavailable":
        request["notes"] = "The authority endpoint did not return the selected datasheet."
    return request


def _alternative(suffix: str = "A") -> dict[str, object]:
    return {
        "manufacturer": f"Alternative {suffix}",
        "mpn": f"ALT-{suffix}",
        "selection_rationale": "Meets the same electrical requirements.",
        "authority_url": f"https://example.com/alternatives/{suffix.lower()}",
    }


def _generic_policy() -> dict[str, object]:
    return {
        "value": "100 nF",
        "package": "0603",
        "tolerance": "10%",
    }


def _component(status: str = "selected") -> dict[str, object]:
    component: dict[str, object] = {
        "block": "example-part",
        "requirements": ["Operates at 3.3 V", "Available in the UK"],
        "manufacturer": "Example Semiconductor",
        "mpn": "EX1234",
        "alternatives": [],
        "selection_rationale": "Selected against the recorded requirements.",
        "authority_url": "https://example.com/manufacturer/example-part",
        "source_request_id": "REQ-EXAMPLE-PART",
        "generic_policy": None,
        "status": status,
        "notes": "",
    }
    if status == "generic-allowance":
        component.update(
            {
                "manufacturer": None,
                "mpn": None,
                "authority_url": None,
                "source_request_id": None,
                "generic_policy": _generic_policy(),
            }
        )
    elif status == "unresolved":
        component.update(
            {
                "manufacturer": None,
                "mpn": None,
                "selection_rationale": "The safe voltage rating remains unresolved.",
                "authority_url": None,
                "source_request_id": None,
                "generic_policy": None,
            }
        )
    return component


def _assumption(status: str = "open") -> dict[str, object]:
    assumption: dict[str, object] = {
        "id": "ASM-EXAMPLE-CONSTRAINT",
        "claim": "The example constraint can be satisfied.",
        "consequence": "Failure blocks the component selection.",
        "owner": "hardware",
        "validation_gate": "P1D electrical review",
        "evidence_ids": [],
        "status": status,
        "resolution": None,
    }
    if status == "resolved":
        assumption["evidence_ids"] = ["EXAMPLE-CONSTRAINT-PROOF"]
        assumption["resolution"] = "The authority datasheet confirms the constraint."
    return assumption


class SelectionSchemaTestCase(unittest.TestCase):
    def assert_invalid(self, instance: object, schema: object) -> None:
        with self.assertRaises(ValidationError):
            validate(instance, schema)


class SchemaMetaTests(SelectionSchemaTestCase):
    def test_all_committed_schemas_pass_the_subset_meta_check(self) -> None:
        for schema in (SOURCE_REQUEST_SCHEMA, COMPONENT_SCHEMA, ASSUMPTION_SCHEMA):
            with self.subTest(schema_id=schema["$id"]):
                check_schema(schema)

    def test_committed_empty_assumptions_collection_is_valid(self) -> None:
        self.assertEqual(ASSUMPTIONS, [])
        validate(ASSUMPTIONS, ASSUMPTION_SCHEMA)


class SourceRequestSchemaTests(SelectionSchemaTestCase):
    def test_every_status_has_a_valid_record(self) -> None:
        for status in ("pending", "satisfied", "unavailable"):
            with self.subTest(status=status):
                validate(_source_request(status), SOURCE_REQUEST_SCHEMA)

    def test_all_fields_are_required_and_additional_properties_are_rejected(self) -> None:
        for field in _source_request():
            request = _source_request()
            del request[field]
            with self.subTest(field=field):
                self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

        request = _source_request()
        request["unexpected"] = True
        self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

    def test_identifiers_slug_url_and_selected_strings_are_rejected_when_invalid(self) -> None:
        for field, value in (
            ("request_id", "EXAMPLE-PART"),
            ("request_id", "REQ-bad"),
            ("family", "Example_Part"),
            ("authority_url", "http://example.com/part"),
            ("selected_manufacturer", ""),
            ("selected_mpn", "   "),
            ("source_id", "lowercase-id"),
        ):
            request = _source_request()
            request[field] = value
            with self.subTest(field=field, value=value):
                self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

        request = _source_request("satisfied")
        request["evidence_ids"] = ["bad-evidence-id"]
        self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

    def test_non_empty_string_arrays_reject_empty_items_and_duplicates(self) -> None:
        for field in ("requirements", "artifact_kinds"):
            request = _source_request()
            request[field] = []
            with self.subTest(field=field, case="empty"):
                self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

            request = _source_request()
            request[field] = [request[field][0], request[field][0]]
            with self.subTest(field=field, case="duplicate"):
                self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

        request = _source_request()
        request["requirements"] = [""]
        self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

        request = _source_request()
        request["artifact_kinds"] = ["marketing-copy"]
        self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

        request = _source_request("satisfied")
        request["evidence_ids"] = ["SAME-EVIDENCE", "SAME-EVIDENCE"]
        self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

    def test_status_evidence_and_failure_note_invariants(self) -> None:
        pending = _source_request("pending")
        pending["evidence_ids"] = ["PREMATURE-EVIDENCE"]
        satisfied = _source_request("satisfied")
        satisfied["evidence_ids"] = []
        unavailable_with_evidence = _source_request("unavailable")
        unavailable_with_evidence["evidence_ids"] = ["UNAVAILABLE-EVIDENCE"]
        unavailable_without_note = _source_request("unavailable")
        unavailable_without_note["notes"] = "   "

        for request in (
            pending,
            satisfied,
            unavailable_with_evidence,
            unavailable_without_note,
        ):
            with self.subTest(status=request["status"], request=request):
                self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)

    def test_unknown_status_is_rejected(self) -> None:
        request = _source_request()
        request["status"] = "acquired"
        self.assert_invalid(request, SOURCE_REQUEST_SCHEMA)


class ComponentSchemaTests(SelectionSchemaTestCase):
    def test_every_status_has_a_valid_record(self) -> None:
        for status in ("selected", "generic-allowance", "unresolved"):
            with self.subTest(status=status):
                validate(_component(status), COMPONENT_SCHEMA)

    def test_zero_one_and_two_alternatives_are_valid_but_a_third_is_rejected(self) -> None:
        for count in range(3):
            component = _component()
            component["alternatives"] = [_alternative(str(index)) for index in range(count)]
            with self.subTest(count=count):
                validate(component, COMPONENT_SCHEMA)

        component = _component()
        component["alternatives"] = [_alternative(str(index)) for index in range(3)]
        self.assert_invalid(component, COMPONENT_SCHEMA)

    def test_all_fields_are_required_and_root_additional_properties_are_rejected(self) -> None:
        for field in _component():
            component = _component()
            del component[field]
            with self.subTest(field=field):
                self.assert_invalid(component, COMPONENT_SCHEMA)

        component = _component()
        component["unexpected"] = True
        self.assert_invalid(component, COMPONENT_SCHEMA)

    def test_block_requirements_status_and_nullable_field_types(self) -> None:
        for field, value in (
            ("block", "Example_Part"),
            ("requirements", []),
            ("requirements", [""]),
            ("requirements", ["same", "same"]),
            ("status", "candidate"),
            ("notes", None),
        ):
            component = _component()
            component[field] = value
            with self.subTest(field=field, value=value):
                self.assert_invalid(component, COMPONENT_SCHEMA)

    def test_alternative_fields_values_url_and_additional_properties(self) -> None:
        for field in _alternative():
            component = _component()
            alternative = _alternative()
            del alternative[field]
            component["alternatives"] = [alternative]
            with self.subTest(field=field, case="missing"):
                self.assert_invalid(component, COMPONENT_SCHEMA)

        for field, value in (
            ("manufacturer", ""),
            ("mpn", "   "),
            ("selection_rationale", ""),
            ("authority_url", "http://example.com/alternative"),
        ):
            component = _component()
            alternative = _alternative()
            alternative[field] = value
            component["alternatives"] = [alternative]
            with self.subTest(field=field, case="invalid"):
                self.assert_invalid(component, COMPONENT_SCHEMA)

        component = _component()
        alternative = _alternative()
        alternative["unexpected"] = True
        component["alternatives"] = [alternative]
        self.assert_invalid(component, COMPONENT_SCHEMA)

    def test_selected_requires_every_selection_field_and_forbids_generic_policy(self) -> None:
        for field in (
            "manufacturer",
            "mpn",
            "selection_rationale",
            "authority_url",
            "source_request_id",
        ):
            component = _component("selected")
            component[field] = None
            with self.subTest(field=field):
                self.assert_invalid(component, COMPONENT_SCHEMA)

        component = _component("selected")
        component["authority_url"] = "http://example.com/part"
        self.assert_invalid(component, COMPONENT_SCHEMA)

        component = _component("selected")
        component["source_request_id"] = "EXAMPLE-PART"
        self.assert_invalid(component, COMPONENT_SCHEMA)

        component = _component("selected")
        component["generic_policy"] = _generic_policy()
        self.assert_invalid(component, COMPONENT_SCHEMA)

    def test_generic_allowance_forbids_selected_fields_and_requires_complete_policy(self) -> None:
        forbidden = {
            "manufacturer": "Example Semiconductor",
            "mpn": "EX1234",
            "authority_url": "https://example.com/part",
            "source_request_id": "REQ-EXAMPLE-PART",
        }
        for field, value in forbidden.items():
            component = _component("generic-allowance")
            component[field] = value
            with self.subTest(field=field, case="forbidden"):
                self.assert_invalid(component, COMPONENT_SCHEMA)

        for rationale in (None, ""):
            component = _component("generic-allowance")
            component["selection_rationale"] = rationale
            with self.subTest(rationale=rationale):
                self.assert_invalid(component, COMPONENT_SCHEMA)

        component = _component("generic-allowance")
        component["generic_policy"] = None
        self.assert_invalid(component, COMPONENT_SCHEMA)

        for field in _generic_policy():
            component = _component("generic-allowance")
            policy = _generic_policy()
            del policy[field]
            component["generic_policy"] = policy
            with self.subTest(field=field, case="missing"):
                self.assert_invalid(component, COMPONENT_SCHEMA)

            component = _component("generic-allowance")
            policy = _generic_policy()
            policy[field] = "   "
            component["generic_policy"] = policy
            with self.subTest(field=field, case="empty"):
                self.assert_invalid(component, COMPONENT_SCHEMA)

        component = _component("generic-allowance")
        policy = _generic_policy()
        policy["unexpected"] = "not allowed"
        component["generic_policy"] = policy
        self.assert_invalid(component, COMPONENT_SCHEMA)

    def test_unresolved_forbids_every_selection_field_and_requires_rationale(self) -> None:
        forbidden = {
            "manufacturer": "Example Semiconductor",
            "mpn": "EX1234",
            "authority_url": "https://example.com/part",
            "source_request_id": "REQ-EXAMPLE-PART",
            "generic_policy": _generic_policy(),
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


class AssumptionSchemaTests(SelectionSchemaTestCase):
    def test_every_status_has_a_valid_record(self) -> None:
        for status in ("open", "resolved"):
            with self.subTest(status=status):
                validate([_assumption(status)], ASSUMPTION_SCHEMA)

        open_with_evidence = _assumption("open")
        open_with_evidence["evidence_ids"] = ["PARTIAL-EVIDENCE"]
        validate([open_with_evidence], ASSUMPTION_SCHEMA)

    def test_all_fields_are_required_and_additional_properties_are_rejected(self) -> None:
        for field in _assumption():
            assumption = _assumption()
            del assumption[field]
            with self.subTest(field=field):
                self.assert_invalid([assumption], ASSUMPTION_SCHEMA)

        assumption = _assumption()
        assumption["unexpected"] = True
        self.assert_invalid([assumption], ASSUMPTION_SCHEMA)

    def test_identifier_strings_status_and_evidence_ids_are_constrained(self) -> None:
        for field, value in (
            ("id", "EXAMPLE-CONSTRAINT"),
            ("id", "ASM-bad"),
            ("claim", ""),
            ("consequence", "   "),
            ("owner", ""),
            ("validation_gate", ""),
            ("status", "closed"),
        ):
            assumption = _assumption()
            assumption[field] = value
            with self.subTest(field=field, value=value):
                self.assert_invalid([assumption], ASSUMPTION_SCHEMA)

        assumption = _assumption("resolved")
        assumption["evidence_ids"] = ["bad-evidence-id"]
        self.assert_invalid([assumption], ASSUMPTION_SCHEMA)

        assumption = _assumption("resolved")
        assumption["evidence_ids"] = ["SAME-EVIDENCE", "SAME-EVIDENCE"]
        self.assert_invalid([assumption], ASSUMPTION_SCHEMA)

    def test_open_forbids_non_null_resolution(self) -> None:
        assumption = _assumption("open")
        assumption["resolution"] = "Premature resolution."
        self.assert_invalid([assumption], ASSUMPTION_SCHEMA)

    def test_resolved_requires_evidence_and_non_empty_resolution(self) -> None:
        empty_evidence = _assumption("resolved")
        empty_evidence["evidence_ids"] = []
        null_resolution = _assumption("resolved")
        null_resolution["resolution"] = None
        empty_resolution = _assumption("resolved")
        empty_resolution["resolution"] = "   "

        for assumption in (empty_evidence, null_resolution, empty_resolution):
            with self.subTest(assumption=assumption):
                self.assert_invalid([assumption], ASSUMPTION_SCHEMA)

    def test_collection_must_be_an_array(self) -> None:
        self.assert_invalid(_assumption(), ASSUMPTION_SCHEMA)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
