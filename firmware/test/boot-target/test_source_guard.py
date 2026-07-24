#!/usr/bin/env python3
"""Mutation-tested structural guard for the retained boot target adapter."""

from __future__ import annotations

import re
import sys
from pathlib import Path


RETAINED_DECLARATION = re.compile(
    r"\bDMAMEM\b[^;\n]*\bg_aius_boot_slots\s*\[\s*2\s*\][^;]*"
    r"__attribute__\s*\(\(\s*used\s*\)\)",
    re.DOTALL,
)
UNBOUNDED_LOOP = re.compile(
    r"\bwhile\s*\(\s*(?:true|1)\s*\)|\bfor\s*\(\s*;\s*;\s*\)"
)
FORBIDDEN_RUNTIME = re.compile(
    r"\b(?:delay|yield|millis|micros)\s*\(|\b(?:Serial|SD|Ethernet)\b"
)


def violations(source: str) -> list[str]:
    errors: list[str] = []
    retained = re.findall(r"\bDMAMEM\b[^;]*;", source, flags=re.DOTALL)
    if len(retained) != 1:
        errors.append("exactly one DMAMEM retained declaration is required")
    if RETAINED_DECLARATION.search(source) is None:
        errors.append("retained slots must be DMAMEM, used, and exactly [2]")
    if UNBOUNDED_LOOP.search(source):
        errors.append("unbounded loop in retained/reset/watchdog adapter")
    if FORBIDDEN_RUNTIME.search(source):
        errors.append("adapter acquired forbidden runtime authority")
    if source.count("g_aius_boot_slots") < 1:
        errors.append("retained slot symbol is missing")
    return errors


def assert_mutation_rejected(source: str, mutated: str, label: str) -> None:
    if mutated == source:
        raise AssertionError(f"mutation did not change source: {label}")
    if not violations(mutated):
        raise AssertionError(f"guard accepted mutation: {label}")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_source_guard.py <aius_boot_teensy.cpp>",
              file=sys.stderr)
        return 2
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    errors = violations(source)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    assert_mutation_rejected(
        source, source.replace("DMAMEM", "", 1), "remove DMAMEM")
    assert_mutation_rejected(
        source, source.replace("__attribute__((used))", "", 1),
        "remove used")
    assert_mutation_rejected(
        source, source + "\nDMAMEM uint32_t second_retained;\n",
        "add retained object")
    assert_mutation_rejected(
        source, source + "\nvoid forbidden(void) { while (true) {} }\n",
        "add unbounded loop")
    assert_mutation_rejected(
        source, source + "\nvoid forbidden(void) { delay(1); }\n",
        "add delay")
    assert_mutation_rejected(
        source, source + "\nvoid forbidden(void) { SD.begin(1); }\n",
        "add SD authority")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
