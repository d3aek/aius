"""Generate the single bounded optical DSP config from the checked-in JSON.

The old select-a-preset model (profiles[]/mixes[]) is gone: there is exactly one
effective config, baked here into a `const` `PROGMEM` (FLASH-resident, read only on
a config change) `AiusOpticalConfig`. `source` and `config_generation` are runtime-reported
fields — the baked default is always `source == "default"`, generation 0 — so they
are validated here but emitted by the firmware serializer, not stored in the struct.
"""

import math

from pathlib import Path


MAX_EQ_SECTIONS = 2
MAX_MODEL_JSON_BYTES = 12288
MAX_MATRIX_GAIN_LINEAR = 4.0
INPUT_TRIM_MIN_DB = -60.0
INPUT_TRIM_MAX_DB = 12.0
EQ_FREQ_MIN_HZ = 10.0
EQ_FREQ_MAX_HZ = 24000.0
EQ_Q_MIN = 0.1
EQ_Q_MAX = 20.0
EQ_GAIN_MIN_DB = -24.0
EQ_GAIN_MAX_DB = 24.0
LIMITER_THRESHOLD_MIN_DBFS = -24.0
LIMITER_THRESHOLD_MAX_DBFS = 0.0
LIMITER_ATTACK_MIN_MS = 0.1
LIMITER_ATTACK_MAX_MS = 100.0
LIMITER_RELEASE_MIN_MS = 1.0
LIMITER_RELEASE_MAX_MS = 1000.0
LIMITER_LOOKAHEAD_MIN_MS = 0.0
LIMITER_LOOKAHEAD_MAX_MS = 5.0

EQ_TYPES = {"bypass": 0, "highpass": 1, "peaking": 2}
INPUT_CHANNELS = ("fl", "fr", "fc", "lfe", "sl", "sr")
FEATURE_KEYS = ("input_eq", "matrix", "output_eq", "limiter", "meters")
MATRIX_KEYS = ("center_gain_linear", "surround_gain_linear", "lfe_gain_linear",
               "headroom_linear", "clipfree")
LIMITER_KEYS = ("enabled", "threshold_dbfs", "attack_ms", "release_ms",
                "lookahead_ms")


def _finite_number(value, field):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f"{field} must be a finite number")
    return value


def _bounded_number(value, field, low, high):
    _finite_number(value, field)
    if value < low or value > high:
        raise ValueError(f"{field} must be between {low} and {high}")
    return value


def _require_bool(value, field):
    if not isinstance(value, bool):
        raise ValueError(f"{field} must be a boolean")
    return value


def _require_exact_keys(obj, keys, field):
    if not isinstance(obj, dict):
        raise ValueError(f"{field} must be an object")
    for key in obj:
        if key not in keys:
            raise ValueError(f"unknown {field} key: {key}")
    for key in keys:
        if key not in obj:
            raise ValueError(f"{field} is missing key: {key}")


def _validate_eq_sections(sections, field):
    if not isinstance(sections, list):
        raise ValueError(f"{field} must be an array")
    if len(sections) > MAX_EQ_SECTIONS:
        raise ValueError(f"{field} exceeds {MAX_EQ_SECTIONS} EQ sections")
    for index, section in enumerate(sections):
        if not isinstance(section, dict):
            raise ValueError(f"{field} EQ {index} must be an object")
        for key in section:
            if key not in ("type", "frequency_hz", "q", "gain_db"):
                raise ValueError(f"unknown {field} EQ key: {key}")
        section_type = section.get("type")
        if section_type not in EQ_TYPES:
            raise ValueError(f"unsupported EQ type: {section_type}")
        for present in ("frequency_hz", "q", "gain_db"):
            if present in section:
                _finite_number(section[present], f"{field} EQ {present}")
        if section_type != "bypass":
            _bounded_number(section.get("frequency_hz", 0), f"{field} EQ frequency_hz",
                            EQ_FREQ_MIN_HZ, EQ_FREQ_MAX_HZ)
            _bounded_number(section.get("q", 0), f"{field} EQ q", EQ_Q_MIN, EQ_Q_MAX)
            _bounded_number(section.get("gain_db", 0), f"{field} EQ gain_db",
                            EQ_GAIN_MIN_DB, EQ_GAIN_MAX_DB)


def validate_config(config):
    """Validate the single JSON model before it is converted into a C table."""
    if not isinstance(config, dict):
        raise ValueError("optical config must be an object")
    _require_exact_keys(
        config,
        ("source", "config_generation", "sample_rate_hz", "features",
         "input_channels", "output_eq", "matrix", "limiter"),
        "optical config",
    )

    if config["source"] != "default":
        raise ValueError("baked optical config source must be \"default\"")
    if isinstance(config["config_generation"], bool) or \
            not isinstance(config["config_generation"], int) or config["config_generation"] != 0:
        raise ValueError("baked optical config config_generation must be 0")

    _finite_number(config["sample_rate_hz"], "sample_rate_hz")
    if not float(config["sample_rate_hz"]).is_integer() or config["sample_rate_hz"] <= 0:
        raise ValueError("sample_rate_hz must be a positive integer")

    _require_exact_keys(config["features"], FEATURE_KEYS, "features")
    for key in FEATURE_KEYS:
        _require_bool(config["features"][key], f"features {key}")

    _require_exact_keys(config["input_channels"], INPUT_CHANNELS, "input_channels")
    for channel in INPUT_CHANNELS:
        entry = config["input_channels"][channel]
        _require_exact_keys(entry, ("gain_db", "eq"), f"input_channel {channel}")
        _bounded_number(entry["gain_db"], f"input_channel {channel} gain_db",
                        INPUT_TRIM_MIN_DB, INPUT_TRIM_MAX_DB)
        _validate_eq_sections(entry["eq"], f"input_channel {channel}")

    _validate_eq_sections(config["output_eq"], "output_eq")

    matrix = config["matrix"]
    _require_exact_keys(matrix, MATRIX_KEYS, "matrix")
    for field in ("center_gain_linear", "surround_gain_linear", "lfe_gain_linear"):
        _bounded_number(matrix[field], f"matrix {field}", 0, MAX_MATRIX_GAIN_LINEAR)
    _finite_number(matrix["headroom_linear"], "matrix headroom_linear")
    if matrix["headroom_linear"] <= 0 or matrix["headroom_linear"] > 1:
        raise ValueError("matrix headroom_linear must be greater than 0 and at most 1")
    _require_bool(matrix["clipfree"], "matrix clipfree")

    limiter = config["limiter"]
    _require_exact_keys(limiter, LIMITER_KEYS, "limiter")
    _require_bool(limiter["enabled"], "limiter enabled")
    _bounded_number(limiter["threshold_dbfs"], "limiter threshold_dbfs",
                    LIMITER_THRESHOLD_MIN_DBFS, LIMITER_THRESHOLD_MAX_DBFS)
    _bounded_number(limiter["attack_ms"], "limiter attack_ms",
                    LIMITER_ATTACK_MIN_MS, LIMITER_ATTACK_MAX_MS)
    _bounded_number(limiter["release_ms"], "limiter release_ms",
                    LIMITER_RELEASE_MIN_MS, LIMITER_RELEASE_MAX_MS)
    _bounded_number(limiter["lookahead_ms"], "limiter lookahead_ms",
                    LIMITER_LOOKAHEAD_MIN_MS, LIMITER_LOOKAHEAD_MAX_MS)


def _eq_initializer(sections):
    values = []
    for section in sections:
        values.append(
            "{%d, %.17g, %.17g, %.17g}"
            % (
                EQ_TYPES[section["type"]],
                section.get("frequency_hz", 0),
                section.get("q", 0),
                section.get("gain_db", 0),
            )
        )
    values += ["{0, 0, 0, 0}"] * (MAX_EQ_SECTIONS - len(values))
    return "{%s, %s}" % (values[0], values[1])


def generate_header(config):
    """Return the generated C header for a validated single optical DSP config."""
    validate_config(config)
    features = config["features"]
    matrix = config["matrix"]
    limiter = config["limiter"]

    input_initializers = []
    for channel in INPUT_CHANNELS:
        entry = config["input_channels"][channel]
        input_initializers.append(
            "{%.17g, %d, %s}"
            % (entry["gain_db"], len(entry["eq"]), _eq_initializer(entry["eq"]))
        )

    lines = [
        "#pragma once",
        "#include <stdint.h>",
        "#ifndef FLASHMEM",
        "#define FLASHMEM",
        "#endif",
        "#ifndef PROGMEM",
        "#define PROGMEM",
        "#endif",
        "typedef struct { uint8_t type; double frequency_hz; double q; double gain_db; } AiusOpticalEqConfig;",
        "typedef struct { double gain_db; uint8_t eq_count; AiusOpticalEqConfig eq[2]; } AiusOpticalInputChannelConfig;",
        "typedef struct { uint8_t input_eq; uint8_t matrix; uint8_t output_eq; uint8_t limiter; uint8_t meters; } AiusOpticalFeaturesConfig;",
        "typedef struct { double center_gain_linear; double surround_gain_linear; double lfe_gain_linear; double headroom_linear; uint8_t clipfree; } AiusOpticalMatrixConfig;",
        "typedef struct { uint8_t enabled; double threshold_dbfs; double attack_ms; double release_ms; double lookahead_ms; } AiusOpticalLimiterConfig;",
        "typedef struct {",
        "\tuint32_t sample_rate_hz;",
        "\tAiusOpticalFeaturesConfig features;",
        "\tAiusOpticalInputChannelConfig input_channels[6];",
        "\tuint8_t output_eq_count;",
        "\tAiusOpticalEqConfig output_eq[2];",
        "\tAiusOpticalMatrixConfig matrix;",
        "\tAiusOpticalLimiterConfig limiter;",
        "} AiusOpticalConfig;",
        f"static const uint32_t AIUS_OPTICAL_SAMPLE_RATE_HZ = {int(config['sample_rate_hz'])}u;",
        f"static const uint8_t AIUS_OPTICAL_INPUT_CHANNEL_COUNT = {len(INPUT_CHANNELS)}u;",
        f"static const uint32_t AIUS_OPTICAL_MODEL_MAX_JSON_BYTES = {MAX_MODEL_JSON_BYTES}u;",
        "static const AiusOpticalConfig AIUS_OPTICAL_CONFIG PROGMEM = {",
        f"\t{int(config['sample_rate_hz'])}u,",
        "\t{%d, %d, %d, %d, %d},"
        % (
            features["input_eq"],
            features["matrix"],
            features["output_eq"],
            features["limiter"],
            features["meters"],
        ),
        "\t{%s}," % ", ".join(input_initializers),
        f"\t{len(config['output_eq'])},",
        "\t%s," % _eq_initializer(config["output_eq"]),
        "\t{%.17g, %.17g, %.17g, %.17g, %d},"
        % (
            matrix["center_gain_linear"],
            matrix["surround_gain_linear"],
            matrix["lfe_gain_linear"],
            matrix["headroom_linear"],
            1 if matrix["clipfree"] else 0,
        ),
        "\t{%d, %.17g, %.17g, %.17g, %.17g},"
        % (
            1 if limiter["enabled"] else 0,
            limiter["threshold_dbfs"],
            limiter["attack_ms"],
            limiter["release_ms"],
            limiter["lookahead_ms"],
        ),
        "};",
    ]
    return "\n".join(lines) + "\n"


def _run_platformio():
    try:
        Import("env")
    except NameError:
        return
    import json

    project_dir = Path(env.subst("$PROJECT_DIR"))
    config = json.loads((project_dir / "config" / "optical_dsp.json").read_text())
    output_dir = Path(env.subst("$BUILD_DIR")) / "generated"
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "aius_optical_config.h").write_text(generate_header(config))
    env.Append(CPPPATH=[str(output_dir)])


_run_platformio()
