#!/usr/bin/env python3
"""Source-blind native oracle for the Teensy fault checkpoint adapter."""

import os
from pathlib import Path
import shutil
import subprocess


FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
SOURCE = FIRMWARE_ROOT / "src" / "aius_fault_teensy.cpp"
HARNESS = Path(__file__).resolve().with_name("fault_target_harness.cpp")
BUILD_DIR = FIRMWARE_ROOT / ".build" / "native" / "fault-target"
INCLUDE_DIR = FIRMWARE_ROOT / "src"
BOOT_INCLUDE_DIR = FIRMWARE_ROOT / "lib" / "aius_boot" / "include"
SELECTORS = tuple(range(10))
INVALID_SELECTORS = ("-1", "10", "65535", "65536")
STALL_TIMEOUT_SECONDS = 0.5


def compiler():
    requested = os.environ.get("CXX", "g++")
    resolved = shutil.which(requested)
    assert resolved is not None, f"C++ compiler not found: {requested}"
    return resolved


def compile_command(output, selector=None):
    command = [
        compiler(),
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wpedantic",
        "-O1",
        "-g",
        "-fsanitize=address,undefined",
        f"-I{INCLUDE_DIR}",
        f"-I{BOOT_INCLUDE_DIR}",
    ]
    if selector is not None:
        command.append(f"-DAIUS_FAULT_STALL_CHECKPOINT={selector}")
    command.extend((str(SOURCE), str(HARNESS), "-o", str(output)))
    return command


def compile_valid(name, selector=None):
    output = BUILD_DIR / name
    result = subprocess.run(
        compile_command(output, selector),
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, (
        f"compile failed for selector {selector!r}\n"
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )
    return output


def assert_compile_rejected(selector):
    output = BUILD_DIR / f"invalid-{selector.replace('-', 'negative-')}"
    result = subprocess.run(
        compile_command(output, selector),
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode != 0, f"invalid selector {selector} compiled"


def assert_returned(binary, arguments, expected):
    result = subprocess.run(
        [str(binary), *arguments],
        capture_output=True,
        text=True,
        timeout=5,
        check=False,
    )
    assert result.returncode == 0, (
        f"{binary.name} {arguments} returned {result.returncode}\n"
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )
    assert result.stdout == expected
    assert result.stderr == ""


def assert_stalled(binary, checkpoint):
    process = subprocess.Popen(
        [str(binary), str(checkpoint)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        stdout, stderr = process.communicate(timeout=STALL_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        process.kill()
        stdout, stderr = process.communicate(timeout=5)
    else:
        raise AssertionError(
            f"selector {checkpoint} returned instead of stalling: "
            f"rc={process.returncode}, stdout={stdout!r}, stderr={stderr!r}"
        )
    assert process.returncode is not None and process.returncode < 0
    assert stdout == "entered\n"
    assert stderr == ""


def main():
    BUILD_DIR.mkdir(parents=True, exist_ok=True)

    production = compile_valid("production")
    assert_returned(production, ["--all"], "entered\nreturned-all\n")

    for selector in SELECTORS:
        fault = compile_valid(f"selector-{selector}", selector)
        assert_returned(
            fault,
            ["--except", str(selector)],
            "entered\nreturned-except\n",
        )
        assert_stalled(fault, selector)

    for selector in INVALID_SELECTORS:
        assert_compile_rejected(selector)

    print("ok fault target behavior")


if __name__ == "__main__":
    main()
