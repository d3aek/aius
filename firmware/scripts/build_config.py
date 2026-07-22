"""Generate bounded optical DSP tables from the checked-in JSON config."""

import json
from pathlib import Path


Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
config = json.loads((project_dir / "config" / "optical_dsp.json").read_text())
profiles = config["profiles"]
mixes = config["mixes"]
if len(profiles) > 8 or len(mixes) > 8:
    raise ValueError("optical config exceeds bounded table capacity")

eq_types = {"bypass": 0, "highpass": 1, "peaking": 2}
lines = [
    "#pragma once",
    "#include <stdint.h>",
    "typedef struct { uint8_t type; double frequency_hz; double q; double gain_db; } AiusOpticalEqConfig;",
    "typedef struct { const char *name; uint8_t eq_count; AiusOpticalEqConfig eq[2]; } AiusOpticalProfileConfig;",
    "typedef struct { const char *name; double lfe; double headroom; uint8_t clipfree; } AiusOpticalMixConfig;",
    f"static const uint32_t AIUS_OPTICAL_SAMPLE_RATE_HZ = {int(config['sample_rate_hz'])}u;",
    f"static const uint8_t AIUS_OPTICAL_PROFILE_COUNT = {len(profiles)}u;",
    f"static const uint8_t AIUS_OPTICAL_MIX_COUNT = {len(mixes)}u;",
    "static const AiusOpticalProfileConfig AIUS_OPTICAL_PROFILES[] = {",
]
for profile in profiles:
    sections = profile["eq"]
    values = []
    for section in sections:
        section_type = section["type"]
        if section_type not in eq_types:
            raise ValueError(f"unsupported EQ type: {section_type}")
        values.append(
            "{%d, %.17g, %.17g, %.17g}"
            % (eq_types[section_type], section.get("frequency_hz", 0),
               section.get("q", 0), section.get("gain_db", 0))
        )
    values += ["{0, 0, 0, 0}"] * (2 - len(values))
    lines.append('{"%s", %d, {%s, %s}},' %
                 (profile["name"], len(sections), values[0], values[1]))
lines.append("};")
lines.append("static const AiusOpticalMixConfig AIUS_OPTICAL_MIXES[] = {")
for mix in mixes:
    clipfree = 1 if mix.get("headroom") == "clipfree" else 0
    headroom = 1 if clipfree else mix["headroom"]
    lines.append('{"%s", %.17g, %.17g, %d},' %
                 (mix["name"], mix["lfe"], headroom, clipfree))
lines.append("};")

output_dir = Path(env.subst("$BUILD_DIR")) / "generated"
output_dir.mkdir(parents=True, exist_ok=True)
(output_dir / "aius_optical_config.h").write_text("\n".join(lines) + "\n")
env.Append(CPPPATH=[str(output_dir)])
