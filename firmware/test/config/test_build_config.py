#!/usr/bin/env python3
"""Native validation tests for the generated optical DSP model."""

import copy
import importlib.util
import json
from pathlib import Path


firmware_root = Path(__file__).resolve().parents[2]
module_spec = importlib.util.spec_from_file_location(
    "aius_build_config", firmware_root / "scripts" / "build_config.py"
)
build_config = importlib.util.module_from_spec(module_spec)
module_spec.loader.exec_module(build_config)

fixture = json.loads((firmware_root / "config" / "optical_dsp.json").read_text())


def expect_invalid(config, text):
    try:
        build_config.generate_header(config)
    except ValueError as error:
        assert text in str(error), (text, error)
    else:
        raise AssertionError(f"expected ValueError containing {text!r}")


def test_valid_fixture_and_model_bound():
    header = build_config.generate_header(fixture)
    assert "AIUS_OPTICAL_PROFILE_COUNT = 3u" in header
    assert "AIUS_OPTICAL_MIX_COUNT = 3u" in header
    assert "AIUS_OPTICAL_MODEL_MAX_JSON_BYTES = 6144u" in header


def test_table_and_eq_bounds():
    config = copy.deepcopy(fixture)
    config["profiles"] *= 3
    expect_invalid(config, "bounded profile capacity")

    config = copy.deepcopy(fixture)
    config["mixes"] *= 3
    expect_invalid(config, "bounded mix capacity")

    config = copy.deepcopy(fixture)
    config["profiles"][0]["eq"] *= 3
    expect_invalid(config, "exceeds 2 EQ sections")


def test_ids_are_bounded_ascii_and_unique_per_table():
    config = copy.deepcopy(fixture)
    config["profiles"][1]["name"] = config["profiles"][0]["name"]
    expect_invalid(config, "duplicate profile ID")

    config = copy.deepcopy(fixture)
    config["mixes"][1]["name"] = config["mixes"][0]["name"]
    expect_invalid(config, "duplicate mix ID")

    config = copy.deepcopy(fixture)
    config["profiles"][0]["name"] = "a" * 25
    expect_invalid(config, "ASCII")

    config = copy.deepcopy(fixture)
    config["profiles"][0]["name"] = "speech café"
    expect_invalid(config, "ASCII")

    config = copy.deepcopy(fixture)
    config["profiles"][0]["name"] = "has space"
    expect_invalid(config, "ASCII")


def test_eq_types_and_all_numeric_fields_are_finite():
    config = copy.deepcopy(fixture)
    config["profiles"][0]["eq"][0]["type"] = "lowpass"
    expect_invalid(config, "unsupported EQ type")

    for field in ("frequency_hz", "q", "gain_db"):
        config = copy.deepcopy(fixture)
        config["profiles"][0]["eq"][0][field] = float("nan")
        expect_invalid(config, "finite number")

    config = copy.deepcopy(fixture)
    config["sample_rate_hz"] = float("inf")
    expect_invalid(config, "finite number")

    config = copy.deepcopy(fixture)
    config["mixes"][0]["lfe"] = float("-inf")
    expect_invalid(config, "finite number")

    config = copy.deepcopy(fixture)
    config["mixes"][0]["headroom"] = float("nan")
    expect_invalid(config, "finite number")


def main():
    test_valid_fixture_and_model_bound()
    test_table_and_eq_bounds()
    test_ids_are_bounded_ascii_and_unique_per_table()
    test_eq_types_and_all_numeric_fields_are_finite()
    print("config generator tests: PASS")


if __name__ == "__main__":
    main()
