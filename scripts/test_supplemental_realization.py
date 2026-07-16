#!/usr/bin/env python3
"""Executable offline contract for the Aius supplemental realization oracle."""

from __future__ import annotations

import copy
import hashlib
import json
import re
import unittest
from pathlib import Path, PurePosixPath

from jsonschema_subset import ValidationError, check_schema, validate


AIUS_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_ROOT = AIUS_ROOT / "evidence" / "manifest"
FIXTURE_ROOT = AIUS_ROOT / "scripts" / "fixtures" / "supplemental-realization"

ORACLE_SHA256 = "ffbb364de79d734c4ba50885095f58319a56687b0bda93669e4753ba689d493d"
OWNED_PATH_ROOT = "src/hardware/aius"
PROFILE_NAME = "uk-distributor-price-stock"
LOCAL_RETENTION = "personal-noncommercial-no-republication"
DYNAMIC_RETENTION = "normalized-observation-only"
LOCAL_REDISTRIBUTION = (
    "Retained for personal noncommercial research; no republication permission is inferred."
)
DYNAMIC_REDISTRIBUTION = "Normalized observation only; no source page is retained."
UNAVAILABLE_REDISTRIBUTION = "No local copy is retained; no republication permission is inferred."

ROOT_KEYS = ("schema_version", "owned_path_root", "observation_profiles", "realizations")
PROFILE_KEYS = ("name", "fields")
FIELD_KEYS = ("key", "value_type", "nullable", "constraint", "unit", "source_role", "basis")
CONSTRAINT_KEYS = ("kind", "value")
ROW_KEYS = (
    "request_id",
    "task_name",
    "owned_paths",
    "source_id",
    "authority",
    "revision_strategy",
    "revision_value",
    "observation_url",
    "authorization",
    "clone_boundary",
    "artifacts",
    "observation_profile",
    "failure_policy",
    "retention",
    "notes",
)
AUTHORIZATION_KEYS = ("status", "authority", "method", "initial_url", "basis")
CLONE_BOUNDARY_KEYS = ("current", "unavailable")
CLONE_OUTCOME_KEYS = (
    "source_id",
    "classification",
    "reused_material",
    "derived_claims",
    "prohibited_use",
    "notes",
)
REALIZATION_ARTIFACT_KEYS = (
    "artifact_kind",
    "name",
    "retrieval_url",
    "expected_final_url",
    "path",
    "storage",
    "media_type",
)
SOURCE_KEYS = (
    "id",
    "class",
    "title",
    "authority",
    "revision",
    "purpose",
    "claim_scope",
    "landing_url",
    "retrieved_at",
    "status",
    "supersedes",
    "redistribution_note",
    "artifacts",
    "observations",
)
SOURCE_ARTIFACT_KEYS = (
    "name",
    "final_url",
    "storage",
    "path",
    "sha256",
    "bytes",
    "media_type",
    "retrieval_status",
    "note",
)
OBSERVATION_KEYS = ("key", "value", "unit", "basis", "observed_at", "source_url")

FIELD_SPECS = (
    ("manufacturer_part_number", "string", False, "pattern", r"^\S(?:.*\S)?$", None, "product"),
    ("supplier_sku", "string", False, "pattern", r"^\S(?:.*\S)?$", None, "product"),
    ("distributor", "string", False, "pattern", r"^\S(?:.*\S)?$", None, "product"),
    ("product_initial_url", "string", False, "pattern", r"^https://\S+$", None, "product"),
    ("product_final_url", "string", False, "pattern", r"^https://\S+$", None, "product"),
    ("authorization_authority", "string", False, "pattern", r"^\S(?:.*\S)?$", None, "authorization"),
    (
        "authorization_method",
        "string",
        False,
        "enum",
        ["manufacturer-page", "manufacturer-locator", "ecia-trustedparts-exception"],
        None,
        "authorization",
    ),
    ("authorization_initial_url", "string", False, "pattern", r"^https://\S+$", None, "authorization"),
    ("authorization_final_url", "string", False, "pattern", r"^https://\S+$", None, "authorization"),
    (
        "stock_state",
        "string",
        False,
        "enum",
        ["in-stock", "out-of-stock", "backorder", "unknown"],
        None,
        "product",
    ),
    ("stock_quantity", "integer", True, "minimum", 0, "pieces", "product"),
    ("price_quantity", "integer", False, "minimum", 1, "pieces", "product"),
    ("unit_price_ex_vat", "number", False, "minimum", 0, "GBP", "product"),
    ("currency", "string", False, "const", "GBP", None, "product"),
    ("minimum_order_quantity", "integer", False, "minimum", 1, "pieces", "product"),
    ("order_multiple", "integer", False, "minimum", 1, "pieces", "product"),
    ("price_includes_vat", "boolean", False, "const", False, None, "product"),
)

CURRENT_CLONE_VALUES = {
    "classification": "official-reuse",
    "reused_material": ["Retained primary manufacturer evidence."],
    "derived_claims": ["Aius requirements are bounded by the retained primary manufacturer evidence."],
    "prohibited_use": ["Do not treat the manufacturer material as a complete Aius design."],
    "notes": "",
}
UNAVAILABLE_CLONE_VALUES = {
    "classification": "unresolved",
    "reused_material": [],
    "derived_claims": ["The manufacturer claim remains unresolved."],
    "prohibited_use": [
        "Do not select or release the component until current manufacturer evidence replaces the unavailable source."
    ],
    "notes": "The fixed manufacturer source is unavailable.",
}


class ContractError(ValueError):
    """The realization or normalized constructor input violates the locked contract."""


def _load_json(path: Path) -> object:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


SCHEMA = _load_json(MANIFEST_ROOT / "supplemental-realization-schema.json")
REALIZATION = _load_json(MANIFEST_ROOT / "supplemental-realization.json")
REQUEST_DOCUMENT = _load_json(MANIFEST_ROOT / "supplemental-requests.json")
SOURCE_SCHEMA = _load_json(MANIFEST_ROOT / "source-schema.json")
REQUESTS = REQUEST_DOCUMENT["requests"]
REQUEST_BY_ID = {request["request_id"]: request for request in REQUESTS}


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def _expected_profile() -> dict[str, object]:
    fields = []
    for key, value_type, nullable, kind, value, unit, source_role in FIELD_SPECS:
        fields.append(
            {
                "key": key,
                "value_type": value_type,
                "nullable": nullable,
                "constraint": {"kind": kind, "value": value},
                "unit": unit,
                "source_role": source_role,
                "basis": (
                    f"P1D {PROFILE_NAME} normalized field {key} from the approved supplemental "
                    "request, realization, and live page."
                ),
            }
        )
    return {"name": PROFILE_NAME, "fields": fields}


EXPECTED_PROFILE = _expected_profile()


def _ordered_unique(values: list[str]) -> list[str]:
    return list(dict.fromkeys(values))


def _path_contains_source_id(path: str, source_id: str) -> bool:
    parts = PurePosixPath(path).parts
    return source_id in parts or PurePosixPath(path).stem == source_id


def _clone_boundary(source_id: str) -> dict[str, object]:
    return {
        "current": {"source_id": source_id, **CURRENT_CLONE_VALUES},
        "unavailable": {"source_id": source_id, **UNAVAILABLE_CLONE_VALUES},
    }


def validate_realization_contract(document: object) -> None:
    validate(document, SCHEMA)
    _require(tuple(document) == ROOT_KEYS, "root property order differs")
    _require(document["observation_profiles"] == [EXPECTED_PROFILE], "observation profile differs")

    pending = [request for request in REQUESTS if request["status"] != "satisfied"]
    pending_ids = [request["request_id"] for request in pending]
    rows = document["realizations"]
    row_ids = [row["request_id"] for row in rows]
    _require(len(pending) == 46, "request aggregate no longer has 46 non-satisfied rows")
    _require(row_ids == sorted(row_ids), "realizations are not sorted by request_id")
    _require(row_ids == sorted(pending_ids), "realizations do not exactly cover non-satisfied requests")

    seen_source_ids: set[str] = set()
    for row in rows:
        request = REQUEST_BY_ID[row["request_id"]]
        source_id = row["source_id"]
        _require(tuple(row) == ROW_KEYS, f"{row['request_id']}: row property order differs")
        _require(row["task_name"] == f"component-evidence-{row['request_id'].lower()}", "task name differs")
        _require(source_id == request["source_id"], f"{row['request_id']}: source ID differs from request")
        _require(source_id not in seen_source_ids, f"{source_id}: duplicate source ID")
        seen_source_ids.add(source_id)

        expected_owned_paths = [
            f"evidence/manifest/supplemental-fragments/{source_id}.json",
            f"evidence/manifest/supplemental-assumption-fragments/{source_id}.json",
            *[artifact["path"] for artifact in row["artifacts"]],
        ]
        _require(row["owned_paths"] == expected_owned_paths, f"{source_id}: owned paths differ")
        for relative in row["owned_paths"]:
            _require(_path_contains_source_id(relative, source_id), f"{source_id}: path omits exact source ID")
            joined = PurePosixPath(document["owned_path_root"]) / PurePosixPath(relative)
            _require(joined.parts[:3] == ("src", "hardware", "aius"), f"{source_id}: path escapes owned root")

        if request["request_kind"] == "manufacturer-evidence":
            _validate_manufacturer_row(request, row)
        else:
            _require(request["request_kind"] == "uk-availability", "unknown request kind")
            _validate_uk_row(request, row)

    profile = document["observation_profiles"][0]
    _require(tuple(profile) == PROFILE_KEYS, "profile property order differs")
    for field in profile["fields"]:
        _require(tuple(field) == FIELD_KEYS, f"{field['key']}: field property order differs")
        _require(tuple(field["constraint"]) == CONSTRAINT_KEYS, f"{field['key']}: constraint order differs")

    uk_rows = [row for row in rows if row["observation_profile"] is not None]
    manufacturer_rows = [row for row in rows if row["observation_profile"] is None]
    _require(len(manufacturer_rows) == 22 and len(uk_rows) == 24, "manufacturer/UK split differs")
    _require(sum(len(row["artifacts"]) for row in rows) == 35, "artifact count differs")
    _require(sum(row["authorization"] is not None for row in rows) == 24, "authorization count differs")

    unresolved = {row["request_id"] for row in uk_rows if row["authorization"]["status"] == "unresolved"}
    _require(
        unresolved == {"REQ-ETHERNET-MAGJACK-UK", "REQ-INTERNAL-USB-CABLE-UK"},
        "unresolved authorization set differs",
    )
    exceptions = [row for row in uk_rows if row["authorization"]["method"] == "ecia-trustedparts-exception"]
    _require(len(exceptions) == 1 and exceptions[0]["request_id"] == "REQ-TEST-FIXTURE-HEADER-UK", "exception differs")
    exception = exceptions[0]["authorization"]
    _require(exception["authority"] == "ECIA TrustedParts", "exception authority differs")
    _require(
        exception["initial_url"] == "https://www.trustedparts.com/en/manufacturers/tag-connect",
        "exception URL differs",
    )


def _validate_manufacturer_row(request: dict[str, object], row: dict[str, object]) -> None:
    source_id = row["source_id"]
    _require(row["authority"] == request["selected_manufacturer"], f"{source_id}: manufacturer authority differs")
    _require(row["clone_boundary"] == _clone_boundary(source_id), f"{source_id}: clone boundary differs")
    for outcome in row["clone_boundary"].values():
        _require(tuple(outcome) == CLONE_OUTCOME_KEYS, f"{source_id}: clone outcome order differs")
    _require(tuple(row["clone_boundary"]) == CLONE_BOUNDARY_KEYS, f"{source_id}: clone boundary order differs")

    kinds = [artifact["artifact_kind"] for artifact in row["artifacts"]]
    _require(_ordered_unique(kinds) == request["artifact_kinds"], f"{source_id}: artifact kinds differ")
    _require(
        any(artifact["retrieval_url"] == request["authority_url"] for artifact in row["artifacts"]),
        f"{source_id}: request authority URL is not retained",
    )
    for artifact in row["artifacts"]:
        _require(tuple(artifact) == REALIZATION_ARTIFACT_KEYS, f"{source_id}: artifact property order differs")
        _require(PurePosixPath(artifact["path"]).name == artifact["name"], f"{source_id}: artifact basename differs")
        _require(_path_contains_source_id(artifact["path"], source_id), f"{source_id}: artifact path omits source ID")
        expected_parent = (
            ("evidence", "artifacts", source_id)
            if artifact["storage"] == "lfs"
            else ("evidence", "source-snapshots", source_id)
        )
        _require(
            PurePosixPath(artifact["path"]).parts[:3] == expected_parent,
            f"{source_id}: artifact storage/path mismatch",
        )


def _validate_uk_row(request: dict[str, object], row: dict[str, object]) -> None:
    source_id = row["source_id"]
    _require(row["observation_url"] == request["authority_url"], f"{source_id}: commerce URL differs")
    _require(tuple(row["authorization"]) == AUTHORIZATION_KEYS, f"{source_id}: authorization order differs")


def _row_for(request_id: str) -> dict[str, object]:
    return next(row for row in REALIZATION["realizations"] if row["request_id"] == request_id)


def _source_identity(request: dict[str, object], row: dict[str, object]) -> dict[str, object]:
    manufacturer = request["selected_manufacturer"]
    mpn = request["selected_mpn"]
    if request["request_kind"] == "manufacturer-evidence":
        title = f"{manufacturer} {mpn} primary manufacturer evidence"
        purpose = f"Satisfy {request['request_id']} for {request['family']}."
        source_class = "golden"
    else:
        title = "normalized UK distributor price and stock observation"
        purpose = f"Record a UK authorized-distributor price and stock observation for {manufacturer} {mpn}."
        source_class = "dynamic"
    return {"class": source_class, "title": title, "purpose": purpose}


def _current_manufacturer_source(case: dict[str, object]) -> dict[str, object]:
    request = REQUEST_BY_ID[case["request_id"]]
    row = _row_for(case["request_id"])
    _require(request["request_kind"] == "manufacturer-evidence", "current manufacturer constructor used for UK row")
    results = case["artifact_results"]
    _require(len(results) == len(row["artifacts"]), "artifact result count differs")
    digests = []
    artifacts = []
    for realization_artifact, result in zip(row["artifacts"], results, strict=True):
        digest = result["sha256"]
        _require(isinstance(digest, str) and re.fullmatch(r"[a-f0-9]{64}", digest) is not None, "bad artifact SHA-256")
        _require(type(result["bytes"]) is int and result["bytes"] > 0, "bad artifact byte count")
        digests.append(digest)
        artifacts.append(
            {
                "name": realization_artifact["name"],
                "final_url": realization_artifact["expected_final_url"],
                "storage": realization_artifact["storage"],
                "path": realization_artifact["path"],
                "sha256": digest,
                "bytes": result["bytes"],
                "media_type": realization_artifact["media_type"],
                "retrieval_status": "current",
                "note": "",
            }
        )
    revision_digest = digests[0] if len(digests) == 1 else hashlib.sha256("".join(digests).encode("ascii")).hexdigest()
    identity = _source_identity(request, row)
    return {
        "id": request["source_id"],
        "class": identity["class"],
        "title": identity["title"],
        "authority": row["authority"],
        "revision": f"sha256:{revision_digest}",
        "purpose": identity["purpose"],
        "claim_scope": request["requirements"],
        "landing_url": request["authority_url"],
        "retrieved_at": case["retrieved_at"],
        "status": "current",
        "supersedes": None,
        "redistribution_note": LOCAL_REDISTRIBUTION,
        "artifacts": artifacts,
        "observations": [
            {
                "key": "manufacturer_part_number",
                "value": request["selected_mpn"],
                "unit": None,
                "basis": (
                    "Selected MPN identified by the retained primary manufacturer evidence for "
                    f"{request['request_id']}."
                ),
                "observed_at": case["retrieved_at"],
                "source_url": request["authority_url"],
            }
        ],
    }


def _matches_type(value: object, value_type: str) -> bool:
    if value_type == "string":
        return isinstance(value, str)
    if value_type == "integer":
        return type(value) is int
    if value_type == "number":
        return type(value) in (int, float)
    if value_type == "boolean":
        return type(value) is bool
    return False


def _validate_uk_values(
    request: dict[str, object],
    row: dict[str, object],
    values: dict[str, object],
    product_final_url: str,
    authorization_final_url: str,
) -> None:
    _require(row["authorization"]["status"] == "confirmed", "unresolved authorization permits only unavailable")
    fields = EXPECTED_PROFILE["fields"]
    _require(list(values) == [field["key"] for field in fields], "normalized value keys/order differ")
    for field in fields:
        value = values[field["key"]]
        if value is None:
            _require(field["nullable"], f"{field['key']}: null is not allowed")
            continue
        _require(_matches_type(value, field["value_type"]), f"{field['key']}: value type differs")
        constraint = field["constraint"]
        if constraint["kind"] == "pattern":
            _require(re.fullmatch(constraint["value"], value) is not None, f"{field['key']}: pattern differs")
        elif constraint["kind"] == "enum":
            _require(value in constraint["value"], f"{field['key']}: enum differs")
        elif constraint["kind"] == "minimum":
            _require(value >= constraint["value"], f"{field['key']}: below minimum")
        elif constraint["kind"] == "exclusive-minimum":
            _require(value > constraint["value"], f"{field['key']}: below exclusive minimum")
        elif constraint["kind"] == "const":
            _require(type(value) is type(constraint["value"]) and value == constraint["value"], f"{field['key']}: const differs")

    authorization = row["authorization"]
    _require(values["manufacturer_part_number"] == request["selected_mpn"], "MPN differs from request")
    _require(values["distributor"] == row["authority"], "distributor differs from realization")
    _require(values["product_initial_url"] == row["observation_url"], "product initial URL differs")
    _require(values["product_final_url"] == product_final_url, "product final URL differs")
    _require(values["authorization_authority"] == authorization["authority"], "authorization authority differs")
    _require(values["authorization_method"] == authorization["method"], "authorization method differs")
    _require(values["authorization_initial_url"] == authorization["initial_url"], "authorization initial URL differs")
    _require(values["authorization_final_url"] == authorization_final_url, "authorization final URL differs")

    stock_state = values["stock_state"]
    stock_quantity = values["stock_quantity"]
    if stock_state == "in-stock":
        _require(type(stock_quantity) is int and stock_quantity >= 1, "in-stock requires quantity >= 1")
    elif stock_state == "out-of-stock":
        _require(type(stock_quantity) is int and stock_quantity == 0, "out-of-stock requires quantity 0")
    else:
        _require(stock_quantity is None, "backorder and unknown require null quantity")
    _require(values["price_quantity"] >= values["minimum_order_quantity"], "price quantity is below MOQ")
    _require(values["price_quantity"] % values["order_multiple"] == 0, "price quantity is not orderable")


def _current_uk_source(case: dict[str, object]) -> dict[str, object]:
    request = REQUEST_BY_ID[case["request_id"]]
    row = _row_for(case["request_id"])
    _require(request["request_kind"] == "uk-availability", "current UK constructor used for manufacturer row")
    values = case["values"]
    _validate_uk_values(request, row, values, case["product_final_url"], case["authorization_final_url"])
    fields = EXPECTED_PROFILE["fields"]
    identity = _source_identity(request, row)
    observations = []
    for field in fields:
        source_url = case["product_final_url"] if field["source_role"] == "product" else case["authorization_final_url"]
        observations.append(
            {
                "key": field["key"],
                "value": values[field["key"]],
                "unit": field["unit"],
                "basis": field["basis"],
                "observed_at": case["observed_at"],
                "source_url": source_url,
            }
        )
    return {
        "id": request["source_id"],
        "class": identity["class"],
        "title": identity["title"],
        "authority": row["authority"],
        "revision": f"observed:{case['observed_at'][:10]}",
        "purpose": identity["purpose"],
        "claim_scope": request["requirements"],
        "landing_url": request["authority_url"],
        "retrieved_at": case["observed_at"],
        "status": "current",
        "supersedes": None,
        "redistribution_note": DYNAMIC_REDISTRIBUTION,
        "artifacts": [],
        "observations": observations,
    }


def _unavailable_source(case: dict[str, object]) -> dict[str, object]:
    request = REQUEST_BY_ID[case["request_id"]]
    row = _row_for(case["request_id"])
    _require(isinstance(case["note"], str) and case["note"].strip(), "unavailable note is empty")
    identity = _source_identity(request, row)
    return {
        "id": request["source_id"],
        "class": identity["class"],
        "title": identity["title"],
        "authority": row["authority"],
        "revision": f"unavailable:{case['retrieved_at'][:10]}",
        "purpose": identity["purpose"],
        "claim_scope": request["requirements"],
        "landing_url": request["authority_url"],
        "retrieved_at": case["retrieved_at"],
        "status": "unavailable",
        "supersedes": None,
        "redistribution_note": UNAVAILABLE_REDISTRIBUTION,
        "artifacts": [
            {
                "name": "unavailable",
                "final_url": request["authority_url"],
                "storage": "external-reference",
                "path": None,
                "sha256": None,
                "bytes": None,
                "media_type": None,
                "retrieval_status": "unavailable",
                "note": case["note"],
            }
        ],
        "observations": [],
    }


class RealizationTestCase(unittest.TestCase):
    def assert_schema_invalid(self, changed: object) -> None:
        with self.assertRaises(ValidationError):
            validate(changed, SCHEMA)

    def assert_contract_invalid(self, changed: object) -> None:
        with self.assertRaises((ValidationError, ContractError)):
            validate_realization_contract(changed)


class OracleContractTests(RealizationTestCase):
    def test_schema_is_supported_and_oracle_is_valid(self) -> None:
        check_schema(SCHEMA)
        validate_realization_contract(REALIZATION)

    def test_precommitted_oracle_bytes_are_hash_locked(self) -> None:
        digest = hashlib.sha256((MANIFEST_ROOT / "supplemental-realization.json").read_bytes()).hexdigest()
        self.assertEqual(digest, ORACLE_SHA256)

    def test_every_property_order_is_exact(self) -> None:
        validate_realization_contract(REALIZATION)

    def test_cross_bindings_reject_each_independent_identity_mutation(self) -> None:
        cases = (
            (0, "request_id", "REQ-NOT-A-REQUEST"),
            (0, "task_name", "component-evidence-req-wrong"),
            (0, "source_id", "AIUS-WRONG-SOURCE"),
            (0, "observation_url", "https://example.com/wrong"),
            (2, "authority", "Wrong Manufacturer"),
        )
        for index, field, value in cases:
            changed = copy.deepcopy(REALIZATION)
            changed["realizations"][index][field] = value
            with self.subTest(index=index, field=field):
                self.assert_contract_invalid(changed)

    def test_sorted_exact_membership_rejects_reorder_and_duplicate(self) -> None:
        changed = copy.deepcopy(REALIZATION)
        changed["realizations"][0], changed["realizations"][1] = changed["realizations"][1], changed["realizations"][0]
        self.assert_contract_invalid(changed)
        changed = copy.deepcopy(REALIZATION)
        changed["realizations"][1] = copy.deepcopy(changed["realizations"][0])
        self.assert_contract_invalid(changed)

    def test_profile_is_an_independent_exact_oracle(self) -> None:
        for field_name in FIELD_KEYS:
            changed = copy.deepcopy(REALIZATION)
            field = changed["observation_profiles"][0]["fields"][0]
            if field_name == "constraint":
                field[field_name] = {"kind": "pattern", "value": "^wrong$"}
            elif field_name == "nullable":
                field[field_name] = True
            elif field_name == "unit":
                field[field_name] = "pieces"
            else:
                field[field_name] = "wrong"
            with self.subTest(field=field_name):
                self.assert_contract_invalid(changed)

    def test_owned_paths_are_exact_confined_and_source_containing(self) -> None:
        cases = (
            "../supplemental-fragments/AIUS-CLIFF-RX-UK.json",
            "/evidence/manifest/supplemental-fragments/AIUS-CLIFF-RX-UK.json",
            "evidence/manifest/supplemental-fragments/${source_id}.json",
            "evidence/manifest/supplemental-fragments/%SOURCE_ID%.json",
            "evidence/manifest/supplemental-fragments/AIUS-WRONG.json",
        )
        for value in cases:
            changed = copy.deepcopy(REALIZATION)
            changed["realizations"][0]["owned_paths"][0] = value
            with self.subTest(value=value):
                self.assert_contract_invalid(changed)

    def test_clone_boundaries_reject_prose_source_and_outcome_mutations(self) -> None:
        manufacturer_index = next(
            index for index, row in enumerate(REALIZATION["realizations"]) if row["clone_boundary"] is not None
        )
        for outcome, field, value in (
            ("current", "source_id", "AIUS-WRONG"),
            ("current", "reused_material", ["Paraphrased."]),
            ("current", "notes", "extra"),
            ("unavailable", "classification", "official-reuse"),
            ("unavailable", "derived_claims", ["Paraphrased."]),
            ("unavailable", "prohibited_use", ["Paraphrased."]),
        ):
            changed = copy.deepcopy(REALIZATION)
            changed["realizations"][manufacturer_index]["clone_boundary"][outcome][field] = value
            with self.subTest(outcome=outcome, field=field):
                self.assert_contract_invalid(changed)

    def test_artifact_kind_path_storage_and_authority_retention_are_cross_bound(self) -> None:
        manufacturer_index = next(
            index for index, row in enumerate(REALIZATION["realizations"]) if row["artifacts"]
        )
        for field, value in (
            ("name", "../unsafe.pdf"),
            ("path", "docs/reference/datasheets.mdAIUS-WRONG/file.pdf"),
            ("storage", "git"),
            ("artifact_kind", "product-page"),
            ("retrieval_url", "https://example.com/wrong.pdf"),
        ):
            changed = copy.deepcopy(REALIZATION)
            changed["realizations"][manufacturer_index]["artifacts"][0][field] = value
            with self.subTest(field=field):
                self.assert_contract_invalid(changed)

    def test_artifact_parent_segment_is_exact_when_all_path_fields_are_synchronized(self) -> None:
        cases = []
        for row_index, row in enumerate(REALIZATION["realizations"]):
            for artifact_index, artifact in enumerate(row["artifacts"]):
                if artifact["storage"] not in {storage for storage, _, _ in cases}:
                    cases.append((artifact["storage"], row_index, artifact_index))
        self.assertEqual({storage for storage, _, _ in cases}, {"lfs", "git"})

        for storage, row_index, artifact_index in cases:
            changed = copy.deepcopy(REALIZATION)
            row = changed["realizations"][row_index]
            source_id = row["source_id"]
            artifact = row["artifacts"][artifact_index]
            suffix = PurePosixPath(artifact["name"]).suffix
            artifact["name"] = f"{source_id}{suffix}"
            root = "docs/reference/datasheets.md" if storage == "lfs" else "evidence/source-snapshots"
            artifact["path"] = f"{root}/AIUS-WRONG/{artifact['name']}"
            row["owned_paths"][2 + artifact_index] = artifact["path"]

            with self.subTest(storage=storage):
                validate(changed, SCHEMA)
                self.assert_contract_invalid(changed)

    def test_authorization_status_and_exception_are_exact(self) -> None:
        uk_index = next(
            index for index, row in enumerate(REALIZATION["realizations"]) if row["authorization"] is not None
        )
        for field, value in (
            ("status", "unresolved"),
            ("method", "ecia-trustedparts-exception"),
            ("initial_url", "http://example.com/not-https"),
        ):
            changed = copy.deepcopy(REALIZATION)
            changed["realizations"][uk_index]["authorization"][field] = value
            with self.subTest(field=field):
                self.assert_contract_invalid(changed)


class SchemaMutationTests(RealizationTestCase):
    def test_root_fields_are_required_closed_and_cardinality_is_exact(self) -> None:
        for field in ROOT_KEYS:
            changed = copy.deepcopy(REALIZATION)
            del changed[field]
            with self.subTest(field=field):
                self.assert_schema_invalid(changed)
        changed = copy.deepcopy(REALIZATION)
        changed["unexpected"] = True
        self.assert_schema_invalid(changed)
        for rows in (REALIZATION["realizations"][:-1], [*REALIZATION["realizations"], REALIZATION["realizations"][-1]]):
            changed = copy.deepcopy(REALIZATION)
            changed["realizations"] = copy.deepcopy(rows)
            self.assert_schema_invalid(changed)

    def test_every_row_field_is_required_and_rows_are_closed(self) -> None:
        for field in ROW_KEYS:
            changed = copy.deepcopy(REALIZATION)
            del changed["realizations"][0][field]
            with self.subTest(field=field):
                self.assert_schema_invalid(changed)
        changed = copy.deepcopy(REALIZATION)
        changed["realizations"][0]["unexpected"] = True
        self.assert_schema_invalid(changed)

    def test_manufacturer_and_uk_shapes_are_mutually_exclusive(self) -> None:
        manufacturer_index = next(
            index for index, row in enumerate(REALIZATION["realizations"]) if row["revision_strategy"] == "sha256"
        )
        uk_index = next(
            index for index, row in enumerate(REALIZATION["realizations"]) if row["revision_strategy"] == "observation-date"
        )
        manufacturer_cases = (
            ("observation_url", REALIZATION["realizations"][uk_index]["observation_url"]),
            ("authorization", copy.deepcopy(REALIZATION["realizations"][uk_index]["authorization"])),
            ("clone_boundary", None),
            ("artifacts", []),
            ("observation_profile", PROFILE_NAME),
            ("failure_policy", "assumption"),
            ("retention", DYNAMIC_RETENTION),
        )
        uk_cases = (
            ("observation_url", None),
            ("authorization", None),
            ("clone_boundary", copy.deepcopy(REALIZATION["realizations"][manufacturer_index]["clone_boundary"])),
            ("artifacts", copy.deepcopy(REALIZATION["realizations"][manufacturer_index]["artifacts"])),
            ("observation_profile", None),
            ("failure_policy", "golden-block"),
            ("retention", LOCAL_RETENTION),
        )
        for index, cases in ((manufacturer_index, manufacturer_cases), (uk_index, uk_cases)):
            for field, value in cases:
                changed = copy.deepcopy(REALIZATION)
                changed["realizations"][index][field] = value
                with self.subTest(index=index, field=field):
                    self.assert_schema_invalid(changed)

    def test_nested_objects_are_complete_and_closed(self) -> None:
        manufacturer_index = next(index for index, row in enumerate(REALIZATION["realizations"]) if row["artifacts"])
        uk_index = next(index for index, row in enumerate(REALIZATION["realizations"]) if row["authorization"])
        targets = (
            (manufacturer_index, ("artifacts", 0), REALIZATION_ARTIFACT_KEYS),
            (uk_index, ("authorization",), AUTHORIZATION_KEYS),
        )
        for index, path, keys in targets:
            for field in keys:
                changed = copy.deepcopy(REALIZATION)
                target = changed["realizations"][index]
                for part in path:
                    target = target[part]
                del target[field]
                with self.subTest(path=path, field=field):
                    self.assert_schema_invalid(changed)
            changed = copy.deepcopy(REALIZATION)
            target = changed["realizations"][index]
            for part in path:
                target = target[part]
            target["unexpected"] = True
            self.assert_schema_invalid(changed)

    def test_profile_field_and_constraint_objects_are_complete_and_closed(self) -> None:
        profile = REALIZATION["observation_profiles"][0]
        for field in PROFILE_KEYS:
            changed = copy.deepcopy(REALIZATION)
            del changed["observation_profiles"][0][field]
            with self.subTest(level="profile", field=field):
                self.assert_schema_invalid(changed)
        for field in FIELD_KEYS:
            changed = copy.deepcopy(REALIZATION)
            del changed["observation_profiles"][0]["fields"][0][field]
            with self.subTest(level="field", field=field):
                self.assert_schema_invalid(changed)
        for field in CONSTRAINT_KEYS:
            changed = copy.deepcopy(REALIZATION)
            del changed["observation_profiles"][0]["fields"][0]["constraint"][field]
            with self.subTest(level="constraint", field=field):
                self.assert_schema_invalid(changed)
        for level in ("profile", "field", "constraint"):
            changed = copy.deepcopy(REALIZATION)
            if level == "profile":
                target = changed["observation_profiles"][0]
            elif level == "field":
                target = changed["observation_profiles"][0]["fields"][0]
            else:
                target = changed["observation_profiles"][0]["fields"][0]["constraint"]
            target["unexpected"] = True
            with self.subTest(level=level):
                self.assert_schema_invalid(changed)
        self.assertEqual(len(profile["fields"]), 17)

    def test_clone_boundary_objects_are_complete_and_closed(self) -> None:
        manufacturer_index = next(
            index for index, row in enumerate(REALIZATION["realizations"]) if row["clone_boundary"] is not None
        )
        for field in CLONE_BOUNDARY_KEYS:
            changed = copy.deepcopy(REALIZATION)
            del changed["realizations"][manufacturer_index]["clone_boundary"][field]
            with self.subTest(level="boundary", field=field):
                self.assert_schema_invalid(changed)
        for outcome in CLONE_BOUNDARY_KEYS:
            for field in CLONE_OUTCOME_KEYS:
                changed = copy.deepcopy(REALIZATION)
                del changed["realizations"][manufacturer_index]["clone_boundary"][outcome][field]
                with self.subTest(outcome=outcome, field=field):
                    self.assert_schema_invalid(changed)
        for outcome in (None, *CLONE_BOUNDARY_KEYS):
            changed = copy.deepcopy(REALIZATION)
            target = changed["realizations"][manufacturer_index]["clone_boundary"]
            if outcome is not None:
                target = target[outcome]
            target["unexpected"] = True
            with self.subTest(outcome=outcome):
                self.assert_schema_invalid(changed)

    def test_artifact_url_basename_path_storage_and_media_boundaries(self) -> None:
        manufacturer_index = next(index for index, row in enumerate(REALIZATION["realizations"]) if row["artifacts"])
        cases = (
            ("name", "."),
            ("name", "nested/file.pdf"),
            ("retrieval_url", "http://example.com/file.pdf"),
            ("expected_final_url", "https://example.com/bad path.pdf"),
            ("path", "/absolute/file.pdf"),
            ("path", "docs/reference/datasheets.md${source_id}/file.pdf"),
            ("storage", "external-reference"),
            ("media_type", ""),
        )
        for field, value in cases:
            changed = copy.deepcopy(REALIZATION)
            changed["realizations"][manufacturer_index]["artifacts"][0][field] = value
            with self.subTest(field=field, value=value):
                self.assert_schema_invalid(changed)

    def test_template_and_substitution_tokens_are_rejected_in_paths_and_basenames(self) -> None:
        names = (
            "${SOURCE_ID}.pdf",
            "$(source-id).pdf",
            "%SOURCE_ID%.pdf",
            "<SOURCE_ID>.pdf",
            "`source-id`.pdf",
        )
        project_relative_schema = SCHEMA["$defs"]["projectRelativePath"]
        basename_schema = SCHEMA["$defs"]["safeBasename"]
        for name in names:
            path = f"docs/reference/datasheets.mdAIUS-ETHERNET-MAGJACK-MFR/{name}"
            with self.subTest(name=name, target="project-relative"):
                with self.assertRaises(ValidationError):
                    validate(path, project_relative_schema, root_schema=SCHEMA)
            with self.subTest(name=name, target="basename"):
                with self.assertRaises(ValidationError):
                    validate(name, basename_schema, root_schema=SCHEMA)

    def test_synchronized_artifact_templates_cannot_bypass_whole_document_schema(self) -> None:
        manufacturer_index = next(index for index, row in enumerate(REALIZATION["realizations"]) if row["artifacts"])
        for name in ("${SOURCE_ID}.pdf", "$(source-id).pdf", "%SOURCE_ID%.pdf", "`source-id`.pdf"):
            changed = copy.deepcopy(REALIZATION)
            row = changed["realizations"][manufacturer_index]
            artifact = row["artifacts"][0]
            artifact["name"] = name
            artifact["path"] = f"docs/reference/datasheets.md{row['source_id']}/{name}"
            row["owned_paths"][2] = artifact["path"]
            with self.subTest(name=name):
                self.assert_schema_invalid(changed)


class SourceConstructorTests(unittest.TestCase):
    def _fixture(self, name: str) -> dict[str, object]:
        return _load_json(FIXTURE_ROOT / name)

    def _assert_exact_source(self, actual: dict[str, object], expected: dict[str, object]) -> None:
        self.assertEqual(actual, expected)
        self.assertEqual(tuple(actual), SOURCE_KEYS)
        for artifact in actual["artifacts"]:
            self.assertEqual(tuple(artifact), SOURCE_ARTIFACT_KEYS)
        for observation in actual["observations"]:
            self.assertEqual(tuple(observation), OBSERVATION_KEYS)
        validate(actual, SOURCE_SCHEMA["$defs"]["sourceRecord"], root_schema=SOURCE_SCHEMA)

    def test_current_manufacturer_constructor_matches_multi_artifact_fixture(self) -> None:
        case = self._fixture("manufacturer-current.json")
        self._assert_exact_source(_current_manufacturer_source(case), case["expected_source"])

    def test_current_manufacturer_single_artifact_revision_is_artifact_digest(self) -> None:
        request_id = next(
            row["request_id"]
            for row in REALIZATION["realizations"]
            if row["observation_profile"] is None and len(row["artifacts"]) == 1
        )
        digest = "c" * 64
        source = _current_manufacturer_source(
            {"request_id": request_id, "retrieved_at": "2026-07-16T12:34:56Z", "artifact_results": [{"sha256": digest, "bytes": 1}]}
        )
        self.assertEqual(source["revision"], f"sha256:{digest}")

    def test_current_uk_constructor_matches_ordered_observation_fixture(self) -> None:
        case = self._fixture("uk-current.json")
        self._assert_exact_source(_current_uk_source(case), case["expected_source"])
        expected_keys = [field[0] for field in FIELD_SPECS]
        self.assertEqual([observation["key"] for observation in case["expected_source"]["observations"]], expected_keys)

    def test_unavailable_constructors_match_both_class_fixtures(self) -> None:
        for name in ("manufacturer-unavailable.json", "uk-unavailable.json"):
            case = self._fixture(name)
            source = _unavailable_source(case)
            with self.subTest(name=name):
                self._assert_exact_source(source, case["expected_source"])
                self.assertEqual(source["status"], "unavailable")
                self.assertEqual(source["observations"], [])
                self.assertEqual(len(source["artifacts"]), 1)

    def test_unavailable_manufacturer_and_uk_differ_only_in_locked_identity(self) -> None:
        manufacturer = _unavailable_source(self._fixture("manufacturer-unavailable.json"))
        uk = _unavailable_source(self._fixture("uk-unavailable.json"))
        common_keys = set(SOURCE_KEYS) - {"id", "class", "title", "authority", "purpose", "claim_scope", "landing_url"}
        for key in common_keys:
            with self.subTest(key=key):
                if key == "artifacts":
                    left = {**manufacturer[key][0], "final_url": None, "note": None}
                    right = {**uk[key][0], "final_url": None, "note": None}
                    self.assertEqual(left, right)
                else:
                    self.assertEqual(manufacturer[key], uk[key])

    def test_unavailable_rejects_empty_note(self) -> None:
        case = self._fixture("uk-unavailable.json")
        for note in ("", "   "):
            changed = copy.deepcopy(case)
            changed["note"] = note
            with self.subTest(note=note):
                with self.assertRaises(ContractError):
                    _unavailable_source(changed)


class UkNormalizedValueTests(unittest.TestCase):
    def setUp(self) -> None:
        self.case = _load_json(FIXTURE_ROOT / "uk-current.json")
        self.request = REQUEST_BY_ID[self.case["request_id"]]
        self.row = _row_for(self.case["request_id"])

    def assert_values_invalid(self, values: dict[str, object], *, row: dict[str, object] | None = None) -> None:
        with self.assertRaises(ContractError):
            _validate_uk_values(
                self.request,
                self.row if row is None else row,
                values,
                self.case["product_final_url"],
                self.case["authorization_final_url"],
            )

    def test_all_four_state_quantity_pairs_are_accepted(self) -> None:
        for state, quantity in (("in-stock", 1), ("out-of-stock", 0), ("backorder", None), ("unknown", None)):
            values = copy.deepcopy(self.case["values"])
            values["stock_state"] = state
            values["stock_quantity"] = quantity
            with self.subTest(state=state):
                _validate_uk_values(
                    self.request,
                    self.row,
                    values,
                    self.case["product_final_url"],
                    self.case["authorization_final_url"],
                )

    def test_every_invalid_state_quantity_pair_is_rejected(self) -> None:
        cases = (
            ("in-stock", 0),
            ("in-stock", None),
            ("out-of-stock", 1),
            ("out-of-stock", None),
            ("backorder", 0),
            ("backorder", 1),
            ("unknown", 0),
            ("unknown", 1),
        )
        for state, quantity in cases:
            values = copy.deepcopy(self.case["values"])
            values["stock_state"] = state
            values["stock_quantity"] = quantity
            with self.subTest(state=state, quantity=quantity):
                self.assert_values_invalid(values)

    def test_orderability_cross_fields_are_enforced(self) -> None:
        for price_quantity, moq, multiple in ((1, 2, 1), (10, 1, 3)):
            values = copy.deepcopy(self.case["values"])
            values["price_quantity"] = price_quantity
            values["minimum_order_quantity"] = moq
            values["order_multiple"] = multiple
            with self.subTest(price_quantity=price_quantity, moq=moq, multiple=multiple):
                self.assert_values_invalid(values)

    def test_every_request_realization_binding_is_enforced(self) -> None:
        cases = (
            ("manufacturer_part_number", "WRONG"),
            ("distributor", "Wrong Distributor"),
            ("product_initial_url", "https://example.com/product"),
            ("product_final_url", "https://example.com/product"),
            ("authorization_authority", "Wrong Authority"),
            ("authorization_method", "manufacturer-locator"),
            ("authorization_initial_url", "https://example.com/authorization"),
            ("authorization_final_url", "https://example.com/authorization"),
        )
        for field, value in cases:
            values = copy.deepcopy(self.case["values"])
            values[field] = value
            with self.subTest(field=field):
                self.assert_values_invalid(values)

    def test_profile_types_constraints_and_constants_are_enforced(self) -> None:
        cases = (
            ("supplier_sku", ""),
            ("product_final_url", "http://example.com/product"),
            ("stock_state", "available"),
            ("stock_quantity", -1),
            ("price_quantity", 0),
            ("unit_price_ex_vat", -0.01),
            ("currency", "USD"),
            ("minimum_order_quantity", 0),
            ("order_multiple", 0),
            ("price_includes_vat", True),
            ("price_quantity", 1.5),
            ("unit_price_ex_vat", False),
        )
        for field, value in cases:
            values = copy.deepcopy(self.case["values"])
            values[field] = value
            with self.subTest(field=field, value=value):
                self.assert_values_invalid(values)

    def test_value_key_membership_and_order_are_exact(self) -> None:
        values = copy.deepcopy(self.case["values"])
        values["unexpected"] = True
        self.assert_values_invalid(values)
        values = copy.deepcopy(self.case["values"])
        del values["supplier_sku"]
        self.assert_values_invalid(values)
        values = copy.deepcopy(self.case["values"])
        first = values.pop("manufacturer_part_number")
        values["manufacturer_part_number"] = first
        self.assert_values_invalid(values)

    def test_unresolved_authorization_rejects_current_constructor(self) -> None:
        unresolved = next(
            row for row in REALIZATION["realizations"] if row["authorization"] and row["authorization"]["status"] == "unresolved"
        )
        with self.assertRaises(ContractError):
            _current_uk_source(
                {
                    **self.case,
                    "request_id": unresolved["request_id"],
                }
            )


if __name__ == "__main__":
    unittest.main()
