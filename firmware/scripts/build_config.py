"""Generate bounded optical DSP tables from the checked-in JSON config."""

import math
import re
from pathlib import Path


MAX_PROFILES = 8
MAX_MIXES = 8
MAX_EQ_SECTIONS = 2
MAX_ID_LENGTH = 24
MAX_MODEL_JSON_BYTES = 6144
MAX_MIX_GAIN_LINEAR = 4.0
EQ_TYPES = {"bypass": 0, "highpass": 1, "peaking": 2}
ID_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,23}")


def _finite_number(value, field):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f"{field} must be a finite number")
    return value


def _validate_id(value, field):
    if not isinstance(value, str):
        raise ValueError(f"{field} must be an ASCII ID")
    if not 1 <= len(value) <= MAX_ID_LENGTH or not value.isascii() or not ID_PATTERN.fullmatch(value):
        raise ValueError(
            f"{field} must be 1-{MAX_ID_LENGTH} ASCII letters, digits, '.', '_' or '-'"
        )


def _bounded_mix_gain(value, field):
    _finite_number(value, field)
    if value < 0 or value > MAX_MIX_GAIN_LINEAR:
        raise ValueError(f"{field} must be between 0 and {MAX_MIX_GAIN_LINEAR}")


def _bounded_headroom(value, field):
    _finite_number(value, field)
    if value <= 0 or value > 1:
        raise ValueError(f"{field} must be greater than 0 and at most 1")


def validate_config(config):
    """Validate the JSON model before it is converted into C tables."""
    _finite_number(config.get("sample_rate_hz"), "sample_rate_hz")
    if not float(config["sample_rate_hz"]).is_integer() or config["sample_rate_hz"] <= 0:
        raise ValueError("sample_rate_hz must be a positive integer")

    profiles = config.get("profiles")
    mixes = config.get("mixes")
    if not isinstance(profiles, list) or not isinstance(mixes, list):
        raise ValueError("optical config profiles and mixes must be arrays")
    if len(profiles) > MAX_PROFILES:
        raise ValueError("optical config exceeds bounded profile capacity")
    if len(mixes) > MAX_MIXES:
        raise ValueError("optical config exceeds bounded mix capacity")

    profile_ids = set()
    for profile_index, profile in enumerate(profiles):
        if not isinstance(profile, dict):
            raise ValueError(f"profile {profile_index} must be an object")
        profile_id = profile.get("name")
        _validate_id(profile_id, f"profile {profile_index} name")
        if profile_id in profile_ids:
            raise ValueError(f"duplicate profile ID: {profile_id}")
        profile_ids.add(profile_id)
        sections = profile.get("eq")
        if not isinstance(sections, list):
            raise ValueError(f"profile {profile_id} eq must be an array")
        if len(sections) > MAX_EQ_SECTIONS:
            raise ValueError(f"profile {profile_id} exceeds {MAX_EQ_SECTIONS} EQ sections")
        for section_index, section in enumerate(sections):
            if not isinstance(section, dict):
                raise ValueError(f"profile {profile_id} EQ {section_index} must be an object")
            section_type = section.get("type")
            if section_type not in EQ_TYPES:
                raise ValueError(f"unsupported EQ type: {section_type}")
            for field in ("frequency_hz", "q", "gain_db"):
                _finite_number(section.get(field, 0), f"profile {profile_id} EQ {field}")

    mix_ids = set()
    for mix_index, mix in enumerate(mixes):
        if not isinstance(mix, dict):
            raise ValueError(f"mix {mix_index} must be an object")
        mix_id = mix.get("name")
        _validate_id(mix_id, f"mix {mix_index} name")
        if mix_id in mix_ids:
            raise ValueError(f"duplicate mix ID: {mix_id}")
        mix_ids.add(mix_id)
        for field in ("center", "surround", "lfe"):
            _bounded_mix_gain(mix.get(field), f"mix {mix_id} {field}")
        headroom = mix.get("headroom")
        if headroom != "clipfree":
            _bounded_headroom(headroom, f"mix {mix_id} headroom")


def generate_header(config):
    """Return the generated C header for a validated optical DSP config."""
    validate_config(config)
    profiles = config["profiles"]
    mixes = config["mixes"]
    lines = [
        "#pragma once",
        "#include <stdint.h>",
        "typedef struct { uint8_t type; double frequency_hz; double q; double gain_db; } AiusOpticalEqConfig;",
        "typedef struct { const char *name; uint8_t eq_count; AiusOpticalEqConfig eq[2]; } AiusOpticalProfileConfig;",
        "typedef struct { const char *name; double center; double surround; double lfe; double headroom; uint8_t clipfree; } AiusOpticalMixConfig;",
        f"static const uint32_t AIUS_OPTICAL_SAMPLE_RATE_HZ = {int(config['sample_rate_hz'])}u;",
        f"static const uint8_t AIUS_OPTICAL_PROFILE_COUNT = {len(profiles)}u;",
        f"static const uint8_t AIUS_OPTICAL_MIX_COUNT = {len(mixes)}u;",
        f"static const uint32_t AIUS_OPTICAL_MODEL_MAX_JSON_BYTES = {MAX_MODEL_JSON_BYTES}u;",
        "static const AiusOpticalProfileConfig AIUS_OPTICAL_PROFILES[] = {",
    ]
    for profile in profiles:
        sections = profile["eq"]
        values = []
        for section in sections:
            section_type = section["type"]
            values.append(
                "{%d, %.17g, %.17g, %.17g}"
                % (
                    EQ_TYPES[section_type],
                    section.get("frequency_hz", 0),
                    section.get("q", 0),
                    section.get("gain_db", 0),
                )
            )
        values += ["{0, 0, 0, 0}"] * (MAX_EQ_SECTIONS - len(values))
        lines.append(
            '{"%s", %d, {%s, %s}},'
            % (profile["name"], len(sections), values[0], values[1])
        )
    lines.append("};")
    lines.append("static const AiusOpticalMixConfig AIUS_OPTICAL_MIXES[] = {")
    for mix in mixes:
        clipfree = 1 if mix.get("headroom") == "clipfree" else 0
        headroom = 1 if clipfree else mix["headroom"]
        lines.append('{"%s", %.17g, %.17g, %.17g, %.17g, %d},' % (mix["name"], mix["center"], mix["surround"], mix["lfe"], headroom, clipfree))
    lines.append("};")
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
