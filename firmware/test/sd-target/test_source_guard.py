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
    "USED_SIZE": r"\bSD\s*\.\s*usedSize\s*\(",
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
    assert primitive_sites == 17, "eligible primitive catalog is not exact"

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
                "SD.begin(BUILTIN_SDCARD)",
                "SD.begin(BUILTIN_SDCARD), SD.begin(BUILTIN_SDCARD)",
                1,
            ),
        ),
        (
            "missing primitive",
            lambda source: source.replace(
                "SD.begin(BUILTIN_SDCARD)", "true", 1
            ),
        ),
    )
    for label, mutation in mutations:
        assert_rejected(body, mutation, label)


if __name__ == "__main__":
    test_source_and_mutation_sensitivity()
    print("ok sd target source guard")
