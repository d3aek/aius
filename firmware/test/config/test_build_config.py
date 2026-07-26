#!/usr/bin/env python3
"""Native validation tests for the single generated optical DSP config."""

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
    assert "AIUS_OPTICAL_SAMPLE_RATE_HZ = 48000u" in header
    assert "AIUS_OPTICAL_INPUT_CHANNEL_COUNT = 6u" in header
    assert "AIUS_OPTICAL_MODEL_MAX_JSON_BYTES = 12288u" in header
    assert "AIUS_OPTICAL_CONFIG PROGMEM = {" in header
    assert "static const AiusOpticalConfig AIUS_OPTICAL_CONFIG PROGMEM" in header
    # The default carries the standard-mix downmix coefficients verbatim:
    # center/surround 1/sqrt(2), lfe 0, headroom 0.5. Headroom was 1.0 until
    # 2026-07-26, which summed to +7.66 dB worst case and demonstrably clipped
    # the committed corpus -- surround51_impulse saturated at SAMPLE_MAX. 0.5 is
    # the smallest exactly-representable power of two that clears the corpus,
    # with 0.79 dB spare. See aius-dsp-config-api.md #audio-quality-roadmap.
    assert "{0.70710678118654746, 0.70710678118654746, 0, 0.5, 0}" in header
    # Six identity input channels (unity trim, no EQ) and no output EQ sections.
    assert "{0, 0, {{0, 0, 0, 0}, {0, 0, 0, 0}}}" in header
    # profiles[]/mixes[] machinery is gone.
    assert "AIUS_OPTICAL_PROFILES" not in header
    assert "AIUS_OPTICAL_MIXES" not in header
    assert "schema_version" not in header


def test_top_level_shape_and_runtime_fields():
    config = copy.deepcopy(fixture)
    config["source"] = "sd"
    expect_invalid(config, "source must be")

    config = copy.deepcopy(fixture)
    config["config_generation"] = 1
    expect_invalid(config, "config_generation must be 0")

    config = copy.deepcopy(fixture)
    config["unexpected"] = True
    expect_invalid(config, "unknown optical config key")

    config = copy.deepcopy(fixture)
    del config["matrix"]
    expect_invalid(config, "optical config is missing key: matrix")

    config = copy.deepcopy(fixture)
    config["sample_rate_hz"] = float("inf")
    expect_invalid(config, "finite number")

    config = copy.deepcopy(fixture)
    config["sample_rate_hz"] = -1
    expect_invalid(config, "positive integer")


def test_features_are_exact_booleans():
    config = copy.deepcopy(fixture)
    config["features"]["extra"] = True
    expect_invalid(config, "unknown features key")

    config = copy.deepcopy(fixture)
    del config["features"]["limiter"]
    expect_invalid(config, "features is missing key: limiter")

    config = copy.deepcopy(fixture)
    config["features"]["matrix"] = 1
    expect_invalid(config, "features matrix must be a boolean")


def test_input_channels_keys_and_trim_bounds():
    config = copy.deepcopy(fixture)
    del config["input_channels"]["fc"]
    expect_invalid(config, "input_channels is missing key: fc")

    config = copy.deepcopy(fixture)
    config["input_channels"]["rear"] = {"gain_db": 0, "eq": []}
    expect_invalid(config, "unknown input_channels key: rear")

    config = copy.deepcopy(fixture)
    config["input_channels"]["fl"]["extra"] = 1
    expect_invalid(config, "unknown input_channel fl key")

    config = copy.deepcopy(fixture)
    config["input_channels"]["fl"]["gain_db"] = -60.01
    expect_invalid(config, "input_channel fl gain_db must be between")

    config = copy.deepcopy(fixture)
    config["input_channels"]["sr"]["gain_db"] = 12.01
    expect_invalid(config, "input_channel sr gain_db must be between")

    config = copy.deepcopy(fixture)
    config["input_channels"]["fl"]["gain_db"] = float("nan")
    expect_invalid(config, "finite number")


def test_eq_sections_types_bounds_and_capacity():
    config = copy.deepcopy(fixture)
    config["output_eq"] = [{"type": "peaking", "frequency_hz": 1000, "q": 1, "gain_db": 3}] * 3
    expect_invalid(config, "exceeds 2 EQ sections")

    config = copy.deepcopy(fixture)
    config["output_eq"] = [{"type": "lowpass", "frequency_hz": 1000, "q": 1, "gain_db": 0}]
    expect_invalid(config, "unsupported EQ type")

    config = copy.deepcopy(fixture)
    config["output_eq"] = [{"type": "highpass", "frequency_hz": 5, "q": 1, "gain_db": 0}]
    expect_invalid(config, "frequency_hz must be between")

    config = copy.deepcopy(fixture)
    config["output_eq"] = [{"type": "peaking", "frequency_hz": 1000, "q": 0.05, "gain_db": 0}]
    expect_invalid(config, "q must be between")

    config = copy.deepcopy(fixture)
    config["output_eq"] = [{"type": "peaking", "frequency_hz": 1000, "q": 1, "gain_db": 30}]
    expect_invalid(config, "gain_db must be between")

    config = copy.deepcopy(fixture)
    config["input_channels"]["fc"]["eq"] = [
        {"type": "highpass", "frequency_hz": 120, "q": 0.7071, "gain_db": 0}
    ]
    # A bounded per-input EQ section is accepted.
    build_config.generate_header(config)

    # A bypass section ignores the frequency/Q/gain bounds.
    config = copy.deepcopy(fixture)
    config["output_eq"] = [{"type": "bypass"}]
    build_config.generate_header(config)


def test_matrix_and_limiter_bounds():
    for field in ("center_gain_linear", "surround_gain_linear", "lfe_gain_linear"):
        config = copy.deepcopy(fixture)
        config["matrix"][field] = float("-inf")
        expect_invalid(config, "finite number")

        config = copy.deepcopy(fixture)
        config["matrix"][field] = -0.01
        expect_invalid(config, "between 0 and")

        config = copy.deepcopy(fixture)
        config["matrix"][field] = 4.01
        expect_invalid(config, "between 0 and")

    config = copy.deepcopy(fixture)
    config["matrix"]["headroom_linear"] = 0
    expect_invalid(config, "greater than 0")

    config = copy.deepcopy(fixture)
    config["matrix"]["headroom_linear"] = 1.01
    expect_invalid(config, "greater than 0")

    config = copy.deepcopy(fixture)
    config["matrix"]["clipfree"] = "no"
    expect_invalid(config, "matrix clipfree must be a boolean")

    config = copy.deepcopy(fixture)
    config["matrix"]["tilt"] = 1
    expect_invalid(config, "unknown matrix key")

    config = copy.deepcopy(fixture)
    config["limiter"]["threshold_dbfs"] = -24.01
    expect_invalid(config, "threshold_dbfs must be between")

    config = copy.deepcopy(fixture)
    config["limiter"]["attack_ms"] = 0.05
    expect_invalid(config, "attack_ms must be between")

    config = copy.deepcopy(fixture)
    config["limiter"]["release_ms"] = 1001
    expect_invalid(config, "release_ms must be between")

    config = copy.deepcopy(fixture)
    config["limiter"]["lookahead_ms"] = 5.01
    expect_invalid(config, "lookahead_ms must be between")

    config = copy.deepcopy(fixture)
    config["limiter"]["enabled"] = 1
    expect_invalid(config, "limiter enabled must be a boolean")

    config = copy.deepcopy(fixture)
    del config["limiter"]["release_ms"]
    expect_invalid(config, "limiter is missing key: release_ms")


def main():
    test_valid_fixture_and_model_bound()
    test_top_level_shape_and_runtime_fields()
    test_features_are_exact_booleans()
    test_input_channels_keys_and_trim_bounds()
    test_eq_sections_types_bounds_and_capacity()
    test_matrix_and_limiter_bounds()
    print("config generator tests: PASS")


if __name__ == "__main__":
    main()
