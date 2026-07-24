#!/usr/bin/env python3
"""Executable source contract for the sole intentional target stall loop."""

import re
from pathlib import Path


FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
SOURCE_PATH = FIRMWARE_ROOT / "src" / "aius_fault_teensy.cpp"
FUNCTION_MARKER = "aius_fault_checkpoint("


def without_comments_and_literals(text):
    pattern = re.compile(
        r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
        re.DOTALL,
    )
    return pattern.sub(lambda match: " " * len(match.group(0)), text)


def matching_parenthesis(text, opening):
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return index
    raise AssertionError("unterminated parenthesized expression")


def function_bounds(text):
    clean = without_comments_and_literals(text)
    marker = clean.find(FUNCTION_MARKER)
    assert marker >= 0, "missing aius_fault_checkpoint"
    opening = clean.find("{", marker + len(FUNCTION_MARKER))
    assert opening >= 0, "missing aius_fault_checkpoint body"
    depth = 0
    for index in range(opening, len(clean)):
        if clean[index] == "{":
            depth += 1
        elif clean[index] == "}":
            depth -= 1
            if depth == 0:
                return opening, index
    raise AssertionError("unterminated aius_fault_checkpoint body")


def strip_outer_parentheses(expression):
    result = expression.strip()
    while result.startswith("(") and result.endswith(")"):
        if matching_parenthesis(result, 0) != len(result) - 1:
            break
        result = result[1:-1].strip()
    return result


def is_constant_true(expression):
    normalized = strip_outer_parentheses(expression)
    return re.fullmatch(r"(?:true|1[uUlL]*)", normalized) is not None


def for_fields(expression):
    fields = []
    start = 0
    depth = 0
    for index, character in enumerate(expression):
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
        elif character == ";" and depth == 0:
            fields.append(expression[start:index])
            start = index + 1
    fields.append(expression[start:])
    return fields


def loops(text):
    clean = without_comments_and_literals(text)
    found = []
    for match in re.finditer(r"\b(?:while|for)\s*\(", clean):
        keyword = re.match(r"\w+", clean[match.start():]).group(0)
        opening = clean.find("(", match.start())
        closing = matching_parenthesis(clean, opening)
        expression = clean[opening + 1:closing]
        unbounded = False
        if keyword == "while":
            unbounded = is_constant_true(expression)
        else:
            fields = for_fields(expression)
            unbounded = len(fields) == 3 and (
                not fields[1].strip() or is_constant_true(fields[1])
            )
        found.append((match.start(), keyword, unbounded))
    for match in re.finditer(r"\bdo\b", clean):
        found.append((match.start(), "do", False))
    return sorted(found)


def assert_fault_source(source):
    clean = without_comments_and_literals(source)
    opening, closing = function_bounds(source)
    all_loops = loops(source)
    body_loops = loops(source[opening + 1:closing])

    assert len(all_loops) == 1, "fault source must contain exactly one loop"
    assert len(body_loops) == 1, "stall loop must be in aius_fault_checkpoint"
    assert all_loops[0][2], "fault checkpoint loop must be unbounded"
    assert body_loops[0][2], "fault checkpoint loop must be unbounded"
    assert not re.search(r"\bdelay\s*\(", clean), "delay is forbidden"
    assert not re.search(
        r"\bdelayMicroseconds\s*\(", clean
    ), "delayMicroseconds is forbidden"


def insert_in_function(source, statement):
    _, closing = function_bounds(source)
    return source[:closing] + "\n" + statement + "\n" + source[closing:]


def assert_rejected(source, mutation, label):
    try:
        assert_fault_source(mutation(source))
    except AssertionError:
        return
    raise AssertionError(f"source guard accepted {label} mutation")


def remove_loop_keyword(source):
    opening, closing = function_bounds(source)
    body = source[opening + 1:closing]
    found = loops(body)
    assert len(found) == 1
    position, keyword, _ = found[0]
    absolute = opening + 1 + position
    return source[:absolute] + ("x" * len(keyword)) + source[absolute + len(keyword):]


def main():
    source = SOURCE_PATH.read_text()
    assert_fault_source(source)

    mutations = (
        ("bounded for loop", lambda text: insert_in_function(
            text, "for (int i = 0; i < 1; ++i) {}"
        )),
        ("bounded while loop", lambda text: insert_in_function(
            text, "while (false) {}"
        )),
        ("do loop", lambda text: insert_in_function(
            text, "do {} while (false);"
        )),
        ("second stall loop", lambda text: insert_in_function(
            text, "for (;;) {}"
        )),
        ("delay", lambda text: insert_in_function(text, "delay(1);")),
        (
            "delayMicroseconds",
            lambda text: insert_in_function(text, "delayMicroseconds(1);"),
        ),
        ("removed stall loop", remove_loop_keyword),
    )
    for label, mutation in mutations:
        assert_rejected(source, mutation, label)

    commented = insert_in_function(source, "// while (true) { delay(1); }")
    assert_fault_source(commented)
    literal = insert_in_function(source, 'const char *text = "for (;;) {}";')
    assert_fault_source(literal)

    print("ok fault target source guard")


if __name__ == "__main__":
    main()
