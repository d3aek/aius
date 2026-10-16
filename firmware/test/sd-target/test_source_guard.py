#!/usr/bin/env python3
"""Executable one-shot source contract for the Teensy SD adapter."""

import re
from pathlib import Path


FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
SOURCE_PATH = FIRMWARE_ROOT / "src" / "aius_sd_teensy.cpp"
FUNCTION_MARKER = "AiusSdEvent aius_sd_teensy_execute("

ELIGIBLE = {
    "BEGIN": r"\bSD\s*\.\s*begin\s*\(",
    "MEDIA_PRESENT": r"\bSD\s*\.\s*mediaPresent\s*\(",
    "MKDIR": r"\bSD\s*\.\s*mkdir\s*\(",
    "OPEN_READ": r"\bSD\s*\.\s*open\s*\(",
    "OPEN_WRITE_BEGIN": r"\bSD\s*\.\s*open\s*\(",
    "OPEN_APPEND": r"\bSD\s*\.\s*open\s*\(",
    "TRUNCATE": r"\.\s*truncate\s*\(",
    "SEEK": r"\.\s*seek\s*\(",
    "READ": r"\.\s*read\s*\(",
    "WRITE": r"\.\s*write\s*\(",
    "FILE_SIZE": r"\.\s*size\s*\(",
    "EXISTS": r"\bSD\s*\.\s*exists\s*\(",
    "RENAME": r"\bSD\s*\.\s*rename\s*\(",
    "REMOVE": r"\bSD\s*\.\s*remove\s*\(",
    "CLOSE": r"\.\s*close\s*\(",
    "TOTAL_SIZE": r"\bSD\s*\.\s*totalSize\s*\(",
    # One chunk of the bounded free-cluster scan (Phase 4zd), reached through
    # the adapter's helper; SD.usedSize() is the unbounded walk it replaces.
    "USED_SIZE": r"\bsd_used_size_step\s*\(",
    # The read-back's cache drop (Phase 4h H1, CR4-B2): one call to the
    # adapter's partition-cache helper, whose route to cacheClear() is the
    # owner's open decision (FsVolume keeps its partitions private).
    "CACHE_DROP": r"\bsd_cache_clear\s*\(",
}

INERT = (
    "NONE",
    "NEED_INPUT",
    "OUTPUT",
    "PUBLISH_CONFIG",
    "COMPLETE",
    "FAIL",
)

PRIMITIVE = re.compile("|".join(f"(?:{pattern})" for pattern in ELIGIBLE.values()))

# The mounted volume's cached type. Not a primitive -- it starts no card
# transaction -- but it is a vendor reach, so it is pinned to the one action that
# may make it: the probe's media check, which already holds the mounted volume.
# Anywhere else it would be a read against a volume another job could be
# mid-mount on, which is the defect this contract exists to prevent.
VOLUME_TYPE = re.compile(r"\bSD\s*\.\s*sdfs\b")
VOLUME_TYPE_ACTION = "MEDIA_PRESENT"


def without_comments_and_literals(text):
    pattern = re.compile(
        r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
        re.DOTALL,
    )
    return pattern.sub(lambda match: " " * len(match.group(0)), text)


def function_body(text, marker=FUNCTION_MARKER):
    start = text.find(marker)
    assert start >= 0, "missing adapter execute function"
    opening = text.find("{", start + len(marker))
    assert opening >= 0, "missing adapter execute body"
    clean = without_comments_and_literals(text)
    depth = 0
    for index in range(opening, len(clean)):
        if clean[index] == "{":
            depth += 1
        elif clean[index] == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1:index]
    raise AssertionError("unterminated adapter execute body")


def case_fragments(body, action):
    clean = without_comments_and_literals(body)
    marker = re.compile(
        rf"\bcase\s+AIUS_SD_ACTION_{re.escape(action)}\s*:"
    )
    boundaries = list(
        re.finditer(r"\b(?:case\s+AIUS_SD_ACTION_[A-Z_]+\s*:|default\s*:)", clean)
    )
    fragments = []
    for match in marker.finditer(clean):
        end = len(clean)
        for boundary in boundaries:
            if boundary.start() > match.end():
                end = boundary.start()
                break
        fragments.append(clean[match.end():end])
    return fragments


def assert_one_shot_body(body):
    clean = without_comments_and_literals(body)
    forbidden = {
        "for": r"\bfor\s*\(",
        "while": r"\bwhile\s*\(",
        "do": r"\bdo\b",
        "recursion": r"\baius_sd_teensy_execute\s*\(",
        "delay": r"\bdelay\s*\(",
        "delayMicroseconds": r"\bdelayMicroseconds\s*\(",
    }
    for label, pattern in forbidden.items():
        assert not re.search(pattern, clean), f"forbidden {label}"

    primitive_sites = 0
    for action, expected in ELIGIBLE.items():
        fragments = case_fragments(clean, action)
        assert fragments, f"missing eligible case {action}"
        matching = [
            fragment for fragment in fragments
            if re.search(expected, fragment)
        ]
        assert len(matching) == 1, f"wrong primitive site count for {action}"
        assert len(PRIMITIVE.findall(matching[0])) == 1, (
            f"multiple primitives in {action}"
        )
        for fragment in fragments:
            primitive_sites += len(PRIMITIVE.findall(fragment))
    assert primitive_sites == 18, "eligible primitive catalog is not exact"

    # The mount mode is the adapter's stored mode, never a literal.
    for fragment in case_fragments(clean, "BEGIN"):
        assert re.search(r"\bSD\s*\.\s*begin\s*\([^)]*adapter\s*->\s*dma\s*\)", fragment), (
            "BEGIN does not pass the stored mode"
        )

    # clearError() runs before the primitive in every eligible case, so the
    # fault split reads only that primitive's error code.
    for action, expected in ELIGIBLE.items():
        for fragment in case_fragments(clean, action):
            match = re.search(expected, fragment)
            if not match:
                continue
            clear = re.search(r"\bSD\s*\.\s*clearError\s*\(", fragment)
            assert clear and clear.start() < match.start(), (
                f"clearError() does not precede the primitive in {action}"
            )

    volume_reads = len(VOLUME_TYPE.findall(clean))
    assert volume_reads == 1, "volume type is not read exactly once"
    probe_reads = sum(
        len(VOLUME_TYPE.findall(fragment))
        for fragment in case_fragments(clean, VOLUME_TYPE_ACTION)
    )
    assert probe_reads == 1, f"volume type is read outside {VOLUME_TYPE_ACTION}"

    for action in INERT:
        for fragment in case_fragments(clean, action):
            assert not PRIMITIVE.search(fragment), f"primitive in inert {action}"


def assert_rejected(body, mutation, label):
    mutated = mutation(body)
    try:
        assert_one_shot_body(mutated)
    except AssertionError:
        return
    raise AssertionError(f"source guard accepted {label} mutation")


def append(text):
    return lambda body: body + "\n" + text


def test_source_and_mutation_sensitivity():
    body = function_body(SOURCE_PATH.read_text())
    assert_one_shot_body(body)

    mutations = (
        ("for", append("for (;;) {}")),
        ("while", append("while (true) {}")),
        ("do", append("do {} while (false);")),
        ("recursion", append("aius_sd_teensy_execute(nullptr, nullptr);")),
        ("delay", append("delay(1);")),
        ("delayMicroseconds", append("delayMicroseconds(1);")),
        (
            "duplicate primitive",
            lambda source: source.replace(
                "SD.begin(BUILTIN_SDCARD, adapter->dma)",
                "SD.begin(BUILTIN_SDCARD, adapter->dma), SD.begin(BUILTIN_SDCARD, adapter->dma)",
                1,
            ),
        ),
        (
            "missing primitive",
            lambda source: source.replace(
                "SD.begin(BUILTIN_SDCARD, adapter->dma)", "true", 1
            ),
        ),
        (
            "mode ignored",
            lambda source: source.replace(
                "SD.begin(BUILTIN_SDCARD, adapter->dma)",
                "SD.begin(BUILTIN_SDCARD, false)",
                1,
            ),
        ),
        (
            "clearError removed",
            lambda source: source.replace("SD.clearError();", "", 1),
        ),
        (
            "clearError after the primitive",
            lambda source: source.replace(
                "SD.clearError();\n\t\tconst uint32_t started = micros();\n\t\tconst bool success = SD.mkdir(action->path);",
                "const uint32_t started = micros();\n\t\tconst bool success = SD.mkdir(action->path);\n\t\tSD.clearError();",
                1,
            ),
        ),
        ("volume type outside the probe", append("SD.sdfs.vol()->fatType();")),
        (
            "volume type removed",
            lambda source: source.replace(
                "SD.sdfs.vol()->fatType()", "0u", 1
            ),
        ),
    )
    for label, mutation in mutations:
        assert_rejected(body, mutation, label)


if __name__ == "__main__":
    test_source_and_mutation_sensitivity()
    print("ok sd target source guard")
