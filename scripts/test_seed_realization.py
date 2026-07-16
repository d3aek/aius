#!/usr/bin/env python3
"""Executable contract tests for the Aius seed-realization oracle."""

from __future__ import annotations

import copy
import json
import unittest
from pathlib import Path

from jsonschema_subset import ValidationError, check_schema, validate


AIUS_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_ROOT = AIUS_ROOT / "evidence" / "manifest"


def _load_json(name: str) -> object:
    with (MANIFEST_ROOT / name).open(encoding="utf-8") as handle:
        return json.load(handle)


SCHEMA = _load_json("seed-realization-schema.json")
REALIZATION = _load_json("seed-realization.json")
SEEDS = _load_json("seed-sources.json")

LOCAL_RETENTION = "personal-noncommercial-no-republication"
DYNAMIC_RETENTION = "normalized-observation-only"

ROW_ORACLE = (
    ("BOE-GBP-DAILY-FX", "dynamic", "observation-date", None, "assumption", DYNAMIC_RETENTION, "boe-gbp-fx"),
    ("CLIFF-ORJ5", "artifact", "fixed", "2.0", "golden-block", LOCAL_RETENTION, None),
    ("CLIFF-OTJ5", "artifact", "fixed", "2.1", "golden-block", LOCAL_RETENTION, None),
    ("CPC-CLIFF-OTJ5", "dynamic", "observation-date", None, "assumption", DYNAMIC_RETENTION, "uk-distributor-price-stock"),
    ("DIGIKEY-CLIFF-ORJ5", "dynamic", "observation-date", None, "assumption", DYNAMIC_RETENTION, "uk-distributor-price-stock"),
    ("DIGIKEY-TEENSY41", "dynamic", "observation-date", None, "assumption", DYNAMIC_RETENTION, "uk-distributor-price-stock"),
    ("FARNELL-1455N1201BK", "dynamic", "observation-date", None, "assumption", DYNAMIC_RETENTION, "uk-distributor-price-stock"),
    ("HAMMOND-1455N1201BK-DRAWING", "artifact", "sha256", None, "golden-block", LOCAL_RETENTION, None),
    ("HAMMOND-1455N1201BK-STEP", "artifact", "fixed", "archive-v1697661986", "golden-block", LOCAL_RETENTION, None),
    ("PCBWAY-2L-100", "dynamic", "observation-date", None, "assumption", DYNAMIC_RETENTION, "pcbway-prototype-quote"),
    ("PCBWAY-4L-100", "dynamic", "observation-date", None, "assumption", DYNAMIC_RETENTION, "pcbway-prototype-quote"),
    ("PJRC-ETHERNET-KIT", "artifact", "sha256", None, "golden-block", LOCAL_RETENTION, None),
    ("PJRC-IMXRT1060-CEC", "artifact", "fixed", "rev4", "golden-block", LOCAL_RETENTION, None),
    ("PJRC-IMXRT1060-RM", "artifact", "fixed", "rev3", "golden-block", LOCAL_RETENTION, None),
    ("PJRC-SPDIF-RX-SOURCE", "snapshot", "commit", "3039be2773e86daf1f381a1e8bdc1e6a55ed11f1", "assumption", LOCAL_RETENTION, None),
    ("PJRC-SPDIF-TX-SOURCE", "snapshot", "commit", "3039be2773e86daf1f381a1e8bdc1e6a55ed11f1", "assumption", LOCAL_RETENTION, None),
    ("PJRC-TEENSY41-PAGE", "snapshot", "sha256", None, "golden-block", LOCAL_RETENTION, None),
    ("PJRC-TEENSY41-PINOUT", "artifact", "fixed", "rev4", "golden-block", LOCAL_RETENTION, None),
    ("PJRC-TEENSY41-SCHEMATIC", "artifact", "sha256", None, "golden-block", LOCAL_RETENTION, None),
    ("PLATFORMIO-TEENSY41", "snapshot", "commit", "54a797fb5440a069d4790d495499552e5a7edb35", "golden-block", LOCAL_RETENTION, None),
    ("VKSDR-TOSLINK-KICAD", "artifact", "sha256", None, "assumption", LOCAL_RETENTION, None),
    ("VKSDR-TOSLINK-SCHEMATICS", "artifact", "sha256", None, "assumption", LOCAL_RETENTION, None),
)

ARTIFACT_ORACLE = {
    "CLIFF-ORJ5": (
        (
            "FCR684205R.pdf",
            "https://www.cliffuk.co.uk/products/optical/FCR684205R.pdf",
            "https://www.cliffuk.co.uk/products/optical/FCR684205R.pdf",
            "https://www.cliffuk.co.uk/products/optical/FCR684205R.pdf",
            "lfs",
            "application/pdf",
        ),
    ),
    "CLIFF-OTJ5": (
        (
            "FCR684205T.pdf",
            "https://www.cliffuk.co.uk/products/optical/FCR684205T.pdf",
            "https://www.cliffuk.co.uk/products/optical/FCR684205T.pdf",
            "https://www.cliffuk.co.uk/products/optical/FCR684205T.pdf",
            "lfs",
            "application/pdf",
        ),
    ),
    "HAMMOND-1455N1201BK-DRAWING": (
        (
            "1455N1201BK.pdf",
            "https://www.hammfg.com/files/parts/pdf/1455N1201BK.pdf",
            "https://www.hammfg.com/files/parts/pdf/1455N1201BK.pdf",
            "https://www.hammfg.com/part/1455N1201BK",
            "lfs",
            "application/pdf",
        ),
    ),
    "HAMMOND-1455N1201BK-STEP": (
        (
            "1455N1201BK.zip",
            "https://www.hammfg.com/files/parts/stp/1455N1201BK.zip?v=1697661986",
            "https://www.hammfg.com/files/parts/stp/1455N1201BK.zip?v=1697661986",
            "https://www.hammfg.com/part/1455N1201BK",
            "lfs",
            "application/zip",
        ),
    ),
    "PJRC-ETHERNET-KIT": (
        (
            "Ethernet_Kit_for_Teensy_4.1_Web.pdf",
            "https://media.digikey.com/pdf/Data%20Sheets/Sparkfun%20PDFs/Ethernet_Kit_for_Teensy_4.1_Web.pdf",
            "https://media.digikey.com/pdf/Data%20Sheets/Sparkfun%20PDFs/Ethernet_Kit_for_Teensy_4.1_Web.pdf",
            "https://www.pjrc.com/store/ethernet_kit.html",
            "lfs",
            "application/pdf",
        ),
    ),
    "PJRC-IMXRT1060-CEC": (
        (
            "IMXRT1060CEC_rev4.pdf",
            "https://www.pjrc.com/teensy/IMXRT1060CEC_rev4.pdf",
            "https://www.pjrc.com/teensy/IMXRT1060CEC_rev4.pdf",
            "https://www.pjrc.com/teensy/IMXRT1060CEC_rev4.pdf",
            "lfs",
            "application/pdf",
        ),
    ),
    "PJRC-IMXRT1060-RM": (
        (
            "IMXRT1060RM_rev3_annotations.pdf",
            "https://www.pjrc.com/teensy/IMXRT1060RM_rev3_annotations.pdf",
            "https://www.pjrc.com/teensy/IMXRT1060RM_rev3_annotations.pdf",
            "https://www.pjrc.com/teensy/IMXRT1060RM_rev3_annotations.pdf",
            "lfs",
            "application/pdf",
        ),
    ),
    "PJRC-SPDIF-RX-SOURCE": (
        (
            "input_spdif3.cpp",
            "https://raw.githubusercontent.com/PaulStoffregen/Audio/3039be2773e86daf1f381a1e8bdc1e6a55ed11f1/input_spdif3.cpp",
            "https://raw.githubusercontent.com/PaulStoffregen/Audio/3039be2773e86daf1f381a1e8bdc1e6a55ed11f1/input_spdif3.cpp",
            "evidence/source-snapshots/PJRC-SPDIF-RX-SOURCE/input_spdif3.cpp",
            "git",
            "text/x-c++",
        ),
    ),
    "PJRC-SPDIF-TX-SOURCE": (
        (
            "output_spdif3.cpp",
            "https://raw.githubusercontent.com/PaulStoffregen/Audio/3039be2773e86daf1f381a1e8bdc1e6a55ed11f1/output_spdif3.cpp",
            "https://raw.githubusercontent.com/PaulStoffregen/Audio/3039be2773e86daf1f381a1e8bdc1e6a55ed11f1/output_spdif3.cpp",
            "evidence/source-snapshots/PJRC-SPDIF-TX-SOURCE/output_spdif3.cpp",
            "git",
            "text/x-c++",
        ),
    ),
    "PJRC-TEENSY41-PAGE": (
        (
            "teensy41.html",
            "https://www.pjrc.com/store/teensy41.html",
            "https://www.pjrc.com/store/teensy41.html",
            "evidence/source-snapshots/PJRC-TEENSY41-PAGE/teensy41.html",
            "git",
            "text/html",
        ),
    ),
    "PJRC-TEENSY41-PINOUT": (
        (
            "card11a_rev4_web.pdf",
            "https://www.pjrc.com/teensy/card11a_rev4_web.pdf",
            "https://www.pjrc.com/teensy/card11a_rev4_web.pdf",
            "https://www.pjrc.com/teensy/card11a_rev4_web.pdf",
            "lfs",
            "application/pdf",
        ),
        (
            "card11b_rev4_web.pdf",
            "https://www.pjrc.com/teensy/card11b_rev4_web.pdf",
            "https://www.pjrc.com/teensy/card11b_rev4_web.pdf",
            "https://www.pjrc.com/teensy/card11b_rev4_web.pdf",
            "lfs",
            "application/pdf",
        ),
    ),
    "PJRC-TEENSY41-SCHEMATIC": (
        (
            "schematic41.png",
            "https://www.pjrc.com/teensy/schematic41.png",
            "https://www.pjrc.com/teensy/schematic41.png",
            "https://www.pjrc.com/teensy/schematic.html",
            "lfs",
            "image/png",
        ),
    ),
    "PLATFORMIO-TEENSY41": (
        (
            "teensy41.rst",
            "https://raw.githubusercontent.com/platformio/platformio-docs/54a797fb5440a069d4790d495499552e5a7edb35/boards/teensy/teensy41.rst",
            "https://raw.githubusercontent.com/platformio/platformio-docs/54a797fb5440a069d4790d495499552e5a7edb35/boards/teensy/teensy41.rst",
            "evidence/source-snapshots/PLATFORMIO-TEENSY41/teensy41.rst",
            "git",
            "text/x-rst",
        ),
    ),
    "VKSDR-TOSLINK-KICAD": (
        (
            "toslink_kicad.zip",
            "https://vksdr.com/content/files/2022/11/toslink_kicad.zip",
            "https://storage.ghost.io/c/e5/f8/e5f82b1b-0848-44d9-95b4-b4a61ecb3633/content/files/2022/11/toslink_kicad.zip",
            "docs/reference/datasheets.mdVKSDR-TOSLINK-KICAD/toslink_kicad.zip",
            "lfs",
            "application/zip",
        ),
    ),
    "VKSDR-TOSLINK-SCHEMATICS": (
        (
            "toslink_schematics.zip",
            "https://vksdr.com/content/files/2022/11/toslink_schematics.zip",
            "https://storage.ghost.io/c/e5/f8/e5f82b1b-0848-44d9-95b4-b4a61ecb3633/content/files/2022/11/toslink_schematics.zip",
            "docs/reference/datasheets.mdVKSDR-TOSLINK-SCHEMATICS/toslink_schematics.zip",
            "lfs",
            "application/zip",
        ),
    ),
}


def _field(
    profile: str,
    key: str,
    value_type: str,
    nullable: bool,
    constraint_kind: str,
    constraint_value: object,
    unit: str | None,
) -> dict[str, object]:
    return {
        "key": key,
        "value_type": value_type,
        "nullable": nullable,
        "constraint": {"kind": constraint_kind, "value": constraint_value},
        "unit": unit,
        "basis": f"P1B {profile} normalized field {key} from the fixed seed landing page.",
    }


def _distributor_fields() -> list[dict[str, object]]:
    profile = "uk-distributor-price-stock"
    return [
        _field(profile, "manufacturer_part_number", "string", False, "non-empty", None, None),
        _field(profile, "supplier_sku", "string", False, "non-empty", None, None),
        _field(profile, "stock_state", "string", False, "enum", ["in-stock", "out-of-stock", "backorder", "unknown"], None),
        _field(profile, "stock_quantity", "integer", True, "minimum", 0, "pieces"),
        _field(profile, "price_quantity", "integer", False, "exclusive-minimum", 0, "pieces"),
        _field(profile, "unit_price_ex_vat", "number", False, "minimum", 0, "GBP"),
        _field(profile, "minimum_order_quantity", "integer", False, "exclusive-minimum", 0, "pieces"),
        _field(profile, "order_multiple", "integer", False, "exclusive-minimum", 0, "pieces"),
        _field(profile, "price_includes_vat", "boolean", False, "none", None, None),
    ]


def _pcbway_fields(layer_count: int) -> list[dict[str, object]]:
    profile = "pcbway-prototype-quote"
    return [
        _field(profile, "layer_count", "integer", False, "const", layer_count, "layers"),
        _field(profile, "board_width_mm", "number", False, "const", 100, "mm"),
        _field(profile, "board_height_mm", "number", False, "const", 100, "mm"),
        _field(profile, "quantity", "integer", False, "const", 5, "pieces"),
        _field(profile, "material", "string", False, "const", "FR-4", None),
        _field(profile, "finished_thickness_mm", "number", False, "const", 1.6, "mm"),
        _field(profile, "copper_weight_oz", "number", False, "const", 1, "oz/ft^2"),
        _field(profile, "solder_mask", "string", False, "const", "green", None),
        _field(profile, "silkscreen", "string", False, "const", "white", None),
        _field(profile, "surface_finish", "string", False, "const", "lead-free HASL", None),
        _field(profile, "fabrication_total", "number", False, "minimum", 0, "USD"),
        _field(profile, "shipping_included", "boolean", False, "none", None, None),
        _field(profile, "tax_included", "boolean", False, "none", None, None),
        _field(profile, "lead_time_days", "integer", True, "exclusive-minimum", 0, "days"),
    ]


def _boe_fields() -> list[dict[str, object]]:
    profile = "boe-gbp-fx"
    return [
        _field(
            profile,
            "rate_date",
            "string",
            False,
            "pattern",
            "^[0-9]{4}-(?:0[1-9]|1[0-2])-(?:0[1-9]|[12][0-9]|3[01])$",
            None,
        ),
        _field(profile, "gbp_per_usd", "number", False, "exclusive-minimum", 0, "GBP/USD"),
        _field(profile, "gbp_per_eur", "number", False, "exclusive-minimum", 0, "GBP/EUR"),
    ]


PROFILE_ORACLE = {
    "BOE-GBP-DAILY-FX": ("boe-gbp-fx", _boe_fields()),
    "CPC-CLIFF-OTJ5": ("uk-distributor-price-stock", _distributor_fields()),
    "DIGIKEY-CLIFF-ORJ5": ("uk-distributor-price-stock", _distributor_fields()),
    "DIGIKEY-TEENSY41": ("uk-distributor-price-stock", _distributor_fields()),
    "FARNELL-1455N1201BK": ("uk-distributor-price-stock", _distributor_fields()),
    "PCBWAY-2L-100": ("pcbway-prototype-quote", _pcbway_fields(2)),
    "PCBWAY-4L-100": ("pcbway-prototype-quote", _pcbway_fields(4)),
}


def _rows_by_id() -> dict[str, dict[str, object]]:
    assert isinstance(REALIZATION, list)
    return {row["source_id"]: row for row in REALIZATION}


class RealizationTestCase(unittest.TestCase):
    def assert_invalid(self, instance: object) -> None:
        with self.assertRaises(ValidationError):
            validate(instance, SCHEMA)


class CommittedOracleTests(RealizationTestCase):
    def test_schema_passes_subset_check_and_realization_validates(self) -> None:
        check_schema(SCHEMA)
        validate(REALIZATION, SCHEMA)

    def test_rows_match_the_exact_ordered_oracle(self) -> None:
        self.assertIsInstance(REALIZATION, list)
        actual = tuple(
            (
                row["source_id"],
                row["mode"],
                row["revision_strategy"],
                row["revision_value"],
                row["failure_policy"],
                row["retention"],
                None if row["observation_profile"] is None else row["observation_profile"]["name"],
            )
            for row in REALIZATION
        )
        self.assertEqual(actual, ROW_ORACLE)

    def test_realization_ids_exactly_cover_the_seed_matrix(self) -> None:
        self.assertIsInstance(SEEDS, list)
        self.assertEqual(
            [row[0] for row in ROW_ORACLE],
            [seed["id"] for seed in SEEDS],
        )

    def test_artifacts_match_every_exact_tuple_and_basename(self) -> None:
        rows = _rows_by_id()
        self.assertEqual(
            set(ARTIFACT_ORACLE),
            {source_id for source_id, row in rows.items() if row["artifacts"]},
        )
        for source_id, expected in ARTIFACT_ORACLE.items():
            actual = tuple(
                (
                    artifact["name"],
                    artifact["retrieval_url"],
                    artifact["expected_final_url"],
                    artifact["path"],
                    artifact["storage"],
                    artifact["media_type"],
                )
                for artifact in rows[source_id]["artifacts"]
            )
            with self.subTest(source_id=source_id):
                self.assertEqual(actual, expected)
                self.assertTrue(
                    all(artifact["name"] == Path(artifact["path"]).name for artifact in rows[source_id]["artifacts"])
                )

    def test_profiles_match_every_exact_ordered_field_rule(self) -> None:
        rows = _rows_by_id()
        self.assertEqual(
            set(PROFILE_ORACLE),
            {source_id for source_id, row in rows.items() if row["observation_profile"] is not None},
        )
        for source_id, (name, fields) in PROFILE_ORACLE.items():
            profile = rows[source_id]["observation_profile"]
            with self.subTest(source_id=source_id):
                self.assertEqual(profile, {"name": name, "fields": fields})
                keys = [field["key"] for field in profile["fields"]]
                self.assertEqual(len(keys), len(set(keys)))


class SchemaBoundaryTests(RealizationTestCase):
    def test_exact_collection_size_is_required(self) -> None:
        self.assert_invalid(copy.deepcopy(REALIZATION[:-1]))
        expanded = copy.deepcopy(REALIZATION)
        expanded.append(copy.deepcopy(REALIZATION[-1]))
        self.assert_invalid(expanded)

    def test_every_row_field_is_required_and_extra_fields_are_rejected(self) -> None:
        for field in REALIZATION[0]:
            changed = copy.deepcopy(REALIZATION)
            del changed[0][field]
            with self.subTest(field=field):
                self.assert_invalid(changed)

        changed = copy.deepcopy(REALIZATION)
        changed[0]["unexpected"] = True
        self.assert_invalid(changed)

    def test_revision_strategy_controls_revision_value(self) -> None:
        rows = _rows_by_id()
        for source_id, value in (
            ("CLIFF-ORJ5", None),
            ("PJRC-SPDIF-RX-SOURCE", ""),
            ("PJRC-TEENSY41-PAGE", "unexpected"),
            ("BOE-GBP-DAILY-FX", "2026-07-16"),
        ):
            changed = copy.deepcopy(REALIZATION)
            changed[[row["source_id"] for row in changed].index(source_id)]["revision_value"] = value
            with self.subTest(source_id=source_id, value=value):
                self.assert_invalid(changed)

    def test_mode_controls_artifacts_profile_retention_and_storage(self) -> None:
        cases = (
            ("BOE-GBP-DAILY-FX", "artifacts", copy.deepcopy(_rows_by_id()["CLIFF-ORJ5"]["artifacts"])),
            ("BOE-GBP-DAILY-FX", "observation_profile", None),
            ("BOE-GBP-DAILY-FX", "retention", LOCAL_RETENTION),
            ("BOE-GBP-DAILY-FX", "failure_policy", "golden-block"),
            ("CLIFF-ORJ5", "artifacts", []),
            ("CLIFF-ORJ5", "observation_profile", copy.deepcopy(_rows_by_id()["BOE-GBP-DAILY-FX"]["observation_profile"])),
            ("CLIFF-ORJ5", "retention", DYNAMIC_RETENTION),
        )
        for source_id, field, value in cases:
            changed = copy.deepcopy(REALIZATION)
            changed[[row["source_id"] for row in changed].index(source_id)][field] = value
            with self.subTest(source_id=source_id, field=field):
                self.assert_invalid(changed)

        changed = copy.deepcopy(REALIZATION)
        changed[1]["artifacts"][0]["storage"] = "git"
        self.assert_invalid(changed)
        changed = copy.deepcopy(REALIZATION)
        snapshot_index = [row["source_id"] for row in changed].index("PJRC-TEENSY41-PAGE")
        changed[snapshot_index]["artifacts"][0]["storage"] = "lfs"
        self.assert_invalid(changed)

    def test_artifact_structure_url_path_and_storage_boundaries(self) -> None:
        artifact_index = [row["source_id"] for row in REALIZATION].index("CLIFF-ORJ5")
        for field in REALIZATION[artifact_index]["artifacts"][0]:
            changed = copy.deepcopy(REALIZATION)
            del changed[artifact_index]["artifacts"][0][field]
            with self.subTest(field=field, case="missing"):
                self.assert_invalid(changed)

        for field, value in (
            ("retrieval_url", "http://example.com/file.pdf"),
            ("expected_final_url", "https://example.com/bad path.pdf"),
            ("path", "/absolute/file.pdf"),
            ("storage", "external-reference"),
            ("media_type", ""),
        ):
            changed = copy.deepcopy(REALIZATION)
            changed[artifact_index]["artifacts"][0][field] = value
            with self.subTest(field=field, case="invalid"):
                self.assert_invalid(changed)

        changed = copy.deepcopy(REALIZATION)
        changed[artifact_index]["artifacts"][0]["unexpected"] = True
        self.assert_invalid(changed)

    def test_profile_and_field_structures_are_closed_and_complete(self) -> None:
        dynamic_index = 0
        profile = REALIZATION[dynamic_index]["observation_profile"]
        for field in profile:
            changed = copy.deepcopy(REALIZATION)
            del changed[dynamic_index]["observation_profile"][field]
            with self.subTest(level="profile", field=field):
                self.assert_invalid(changed)

        for field in profile["fields"][0]:
            changed = copy.deepcopy(REALIZATION)
            del changed[dynamic_index]["observation_profile"]["fields"][0][field]
            with self.subTest(level="field", field=field):
                self.assert_invalid(changed)

        for level in ("profile", "field", "constraint"):
            changed = copy.deepcopy(REALIZATION)
            if level == "profile":
                target = changed[dynamic_index]["observation_profile"]
            elif level == "field":
                target = changed[dynamic_index]["observation_profile"]["fields"][0]
            else:
                target = changed[dynamic_index]["observation_profile"]["fields"][0]["constraint"]
            target["unexpected"] = True
            with self.subTest(level=level):
                self.assert_invalid(changed)

    def test_constraint_value_shapes_and_type_relationships_are_enforced(self) -> None:
        field_schema = SCHEMA["$defs"]["field"]
        root = SCHEMA
        valid_fields = (
            _field("test", "value", "boolean", False, "none", None, None),
            _field("test", "value", "string", False, "non-empty", None, None),
            _field("test", "value", "string", False, "enum", [True, 1, "one"], None),
            _field("test", "value", "integer", False, "minimum", 0, None),
            _field("test", "value", "number", False, "exclusive-minimum", 0, None),
            _field("test", "value", "string", False, "const", "fixed", None),
            _field("test", "value", "string", False, "pattern", "^[0-9]+$", None),
        )
        for field in valid_fields:
            with self.subTest(kind=field["constraint"]["kind"], case="valid"):
                validate(field, field_schema, root_schema=root)

        invalid_fields = (
            _field("test", "value", "boolean", False, "none", 0, None),
            _field("test", "value", "string", False, "non-empty", "x", None),
            _field("test", "value", "string", False, "enum", [], None),
            _field("test", "value", "string", False, "enum", ["same", "same"], None),
            _field("test", "value", "number", False, "minimum", "0", None),
            _field("test", "value", "integer", False, "exclusive-minimum", True, None),
            _field("test", "value", "string", False, "const", None, None),
            _field("test", "value", "string", False, "pattern", "[0-9]+", None),
            _field("test", "value", "integer", False, "non-empty", None, None),
            _field("test", "value", "string", False, "minimum", 0, None),
        )
        for field in invalid_fields:
            with self.subTest(kind=field["constraint"]["kind"], case="invalid"):
                with self.assertRaises(ValidationError):
                    validate(field, field_schema, root_schema=root)

    def test_field_key_type_unit_profile_name_and_empty_fields_boundaries(self) -> None:
        changed = copy.deepcopy(REALIZATION)
        changed[0]["observation_profile"]["name"] = "unknown-profile"
        self.assert_invalid(changed)

        cases = (
            ("key", "Bad-Key"),
            ("value_type", "object"),
            ("nullable", 0),
            ("unit", ""),
            ("basis", "   "),
        )
        for field, value in cases:
            changed = copy.deepcopy(REALIZATION)
            changed[0]["observation_profile"]["fields"][0][field] = value
            with self.subTest(field=field):
                self.assert_invalid(changed)

        changed = copy.deepcopy(REALIZATION)
        changed[0]["observation_profile"]["fields"] = []
        self.assert_invalid(changed)


if __name__ == "__main__":
    unittest.main()
