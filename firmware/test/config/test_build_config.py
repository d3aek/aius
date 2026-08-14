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
    assert '#include "aius_dsp_config.h"' in header
    assert "typedef AiusDspModel AiusOpticalConfig;" in header
    # The default carries the standard-mix downmix coefficients verbatim:
    # center/surround 1/sqrt(2), lfe -10 dB through a 120 Hz crossover,
    # headroom 0.5.
    #
    # Headroom was 1.0 until 2026-07-26, which summed to +7.66 dB worst case and
    # demonstrably clipped the committed corpus -- surround51_impulse saturated at
    # SAMPLE_MAX. 0.5 is the smallest exactly-representable power of two that
    # clears the corpus, with 0.79 dB spare.
    #
    # lfe_gain_linear was 0 (the LFE discarded entirely, per BS.775) until roadmap
    # item 2. On a device that downmixes to STEREO that threw away the bottom
    # octave of film content, so it now folds in at -10 dB -- 20 dB below A/52's
    # +10 dB reference fold-in gain -- through the 4th-order Linkwitz-Riley
    # low-pass at lfe_crossover_hz. Re-measured: 0.5 headroom still clears the
    # corpus, the binding vector moving only from -0.791 to -0.790 dBFS.
    # See aius-dsp-config-api.md #audio-quality-roadmap and aius_dsp.h.
    #
    # The trailing 1 is layout_aware, added 2026-07-28: the 0.5 pad is the
    # SUMMING-layout pad, so the downmix resolves 1.0 for a block whose layout
    # sums nothing (mono, and stereo FL|FR pass-through, which was paying 6.02 dB
    # for protection it could not need). It is ON in the factory model; setting it
    # false restores the uniform pad on every layout. The 0 before clipfree is
    # surround_delay_ms (audio-quality item 4), appended to the matrix struct.
    assert (
        "{0.70710678118654746, 0.70710678118654746, 0.31622776601683794, 120, 0.5, 0, 0, 1}"
        in header
    )
    # The generated header reuses the parser's canonical production model
    # instead of carrying a second field-shape definition.
    assert "typedef struct" not in header
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
    config["output_eq"] = [{"type": "lowshelf", "frequency_hz": 1000, "q": 1, "gain_db": 0}]
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

    config = copy.deepcopy(fixture)
    config["output_eq"] = [{"type": "lowpass", "frequency_hz": 12000, "q": 0.7071, "gain_db": 0}]
    # lowpass is a bounded non-bypass type, accepted like highpass.
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

    # layout_aware is a boolean and, in the BAKED model, required like every other
    # matrix key. The PUT validator treats it as optional and defaults it TRUE,
    # deliberately: that parser meets configs written before the field existed,
    # this generator meets one checked-in file that a build error can fix.
    config = copy.deepcopy(fixture)
    config["matrix"]["layout_aware"] = "yes"
    expect_invalid(config, "matrix layout_aware must be a boolean")

    config = copy.deepcopy(fixture)
    del config["matrix"]["layout_aware"]
    expect_invalid(config, "matrix is missing key: layout_aware")

    # And false really does bake a 0, so the diagnostic lever is reachable from
    # the factory model and not only over HTTP.
    config = copy.deepcopy(fixture)
    config["matrix"]["layout_aware"] = False
    assert (
        "{0.70710678118654746, 0.70710678118654746, 0.31622776601683794, 120, 0.5, 0, 0, 0}"
        in build_config.generate_header(config)
    )

    config = copy.deepcopy(fixture)
    config["matrix"]["tilt"] = 1
    expect_invalid(config, "unknown matrix key")

    # The LFE fold-in crossover corner. Required like every other matrix key, and
    # bounded [20, 500] -- a range that deliberately EXCLUDES 0, because the DSP
    # reads a zero corner as "fold the LFE unfiltered" and an unfiltered fold is
    # the mud the crossover exists to prevent. Turning the fold off is
    # lfe_gain_linear = 0, which reproduces the pre-fold-in output bit-exactly.
    config = copy.deepcopy(fixture)
    del config["matrix"]["lfe_crossover_hz"]
    expect_invalid(config, "matrix is missing key: lfe_crossover_hz")

    config = copy.deepcopy(fixture)
    config["matrix"]["lfe_crossover_hz"] = 0
    expect_invalid(config, "lfe_crossover_hz must be between")

    config = copy.deepcopy(fixture)
    config["matrix"]["lfe_crossover_hz"] = 19.99
    expect_invalid(config, "lfe_crossover_hz must be between")

    config = copy.deepcopy(fixture)
    config["matrix"]["lfe_crossover_hz"] = 500.01
    expect_invalid(config, "lfe_crossover_hz must be between")

    config = copy.deepcopy(fixture)
    config["matrix"]["lfe_crossover_hz"] = float("nan")
    expect_invalid(config, "finite number")

    # Both endpoints are accepted, and so is the ubiquitous 80 Hz sub crossover.
    for corner in (20, 80, 500):
        config = copy.deepcopy(fixture)
        config["matrix"]["lfe_crossover_hz"] = corner
        build_config.generate_header(config)

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
