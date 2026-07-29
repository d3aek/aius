#!/usr/bin/env python3
"""Source-blind black-box oracle for Aius build and fault-image identity."""

from contextlib import contextmanager
import os
from pathlib import Path
import re
import runpy
import subprocess
import tempfile


FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
METADATA_SCRIPT = FIRMWARE_ROOT / "scripts" / "build_metadata.py"
BUILD_SCRIPT = FIRMWARE_ROOT / "scripts" / "build.sh"
FIXED_REVISION = "0123456789ab"
PRODUCTION_IDENTITY = f"{FIXED_REVISION}+teensy41-optical"
FAULT_IDENTITIES = [
    f"{FIXED_REVISION}+fault.cp{checkpoint}"
    for checkpoint in range(9)
]
KNOWN_BUILD_IDS = [
    2180687348,
    518221850,
    2919486046,
    3698413212,
    4061990289,
    2250602284,
    3146728504,
    3693711741,
    1770631961,
    3963842019,
]
REQUIRED_DEFINES = {
    "AIUS_BUILD_SOURCE_REV",
    "AIUS_BUILD_TARGET",
    "AIUS_BUILD_FAULT_CHECKPOINT",
    "AIUS_BUILD_GIT_REV",
    "AIUS_BUILD_ID",
    "AIUS_BUILD_UTC",
}
FAULT_DEFINE = "AIUS_FAULT_STALL_CHECKPOINT"
UTC_PATTERN = re.compile(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$")


@contextmanager
def patched_environment(replacements):
    original = os.environ.copy()
    os.environ.clear()
    os.environ.update(original)
    os.environ.update(replacements)
    try:
        yield
    finally:
        os.environ.clear()
        os.environ.update(original)


class FakePlatformioEnvironment:
    def __init__(self, environment):
        self.environment = environment
        self.defines = None
        self.stringified = []

    def subst(self, value):
        if value == "$PROJECT_DIR":
            return str(FIRMWARE_ROOT)
        if value == "$PIOENV":
            return self.environment
        raise AssertionError(f"unexpected PlatformIO substitution: {value!r}")

    def __getitem__(self, key):
        if key == "PIOENV":
            return self.environment
        raise AssertionError(f"unexpected PlatformIO environment key: {key!r}")

    def StringifyMacro(self, value):
        self.stringified.append(value)
        return ("string", value)

    def Append(self, **values):
        assert set(values) == {"CPPDEFINES"}, values
        assert self.defines is None, "metadata appended compiler definitions twice"
        self.defines = values["CPPDEFINES"]


def write_fake_git(directory):
    executable = directory / "git"
    executable.write_text(
        "#!/bin/sh\n"
        "printf '%s\\n' \"${AIUS_TEST_GIT_REV:?missing fixed revision}\"\n"
    )
    executable.chmod(0o755)


def execute_metadata(environment, selector=None, revision=FIXED_REVISION):
    fake = FakePlatformioEnvironment(environment)
    with tempfile.TemporaryDirectory(prefix="aius-build-metadata-") as temporary:
        temporary_path = Path(temporary)
        write_fake_git(temporary_path)
        replacements = {
            "PATH": f"{temporary_path}{os.pathsep}{os.environ['PATH']}",
            "PIOENV": environment,
            "AIUS_TEST_GIT_REV": revision,
        }
        if selector is None:
            original_selector = os.environ.pop("AIUS_FAULT_CHECKPOINT", None)
        else:
            replacements["AIUS_FAULT_CHECKPOINT"] = selector
            original_selector = os.environ.get("AIUS_FAULT_CHECKPOINT")
        try:
            with patched_environment(replacements):
                if selector is None:
                    os.environ.pop("AIUS_FAULT_CHECKPOINT", None)
                runpy.run_path(
                    str(METADATA_SCRIPT),
                    init_globals={"Import": lambda *_names: None, "env": fake},
                )
        finally:
            if original_selector is None:
                os.environ.pop("AIUS_FAULT_CHECKPOINT", None)
            else:
                os.environ["AIUS_FAULT_CHECKPOINT"] = original_selector

    assert fake.defines is not None, "metadata produced no compiler definitions"
    definitions = dict(fake.defines)
    expected_defines = set(REQUIRED_DEFINES)
    if environment == "teensy41-optical-fault":
        expected_defines.add(FAULT_DEFINE)
    assert set(definitions) == expected_defines, definitions
    return definitions


def unstring(value):
    assert isinstance(value, tuple) and value[0] == "string", value
    return value[1]


def assert_metadata(
    definitions,
    *,
    identity,
    target,
    checkpoint,
    build_id,
):
    assert unstring(definitions["AIUS_BUILD_SOURCE_REV"]) == FIXED_REVISION
    assert unstring(definitions["AIUS_BUILD_TARGET"]) == target
    assert definitions["AIUS_BUILD_FAULT_CHECKPOINT"] == checkpoint
    assert unstring(definitions["AIUS_BUILD_GIT_REV"]) == identity
    assert definitions["AIUS_BUILD_ID"] == build_id
    assert UTC_PATTERN.fullmatch(unstring(definitions["AIUS_BUILD_UTC"]))


def test_known_sha256_little_endian_vectors_and_all_selectors():
    production = execute_metadata("teensy41-optical")
    assert FAULT_DEFINE not in production
    assert_metadata(
        production,
        identity=PRODUCTION_IDENTITY,
        target="teensy41-optical",
        checkpoint=-1,
        build_id=KNOWN_BUILD_IDS[0],
    )

    identities = {unstring(production["AIUS_BUILD_GIT_REV"])}
    build_ids = {production["AIUS_BUILD_ID"]}
    for checkpoint in range(9):
        definitions = execute_metadata(
            "teensy41-optical-fault", str(checkpoint)
        )
        assert_metadata(
            definitions,
            identity=FAULT_IDENTITIES[checkpoint],
            target="teensy41-optical-fault",
            checkpoint=checkpoint,
            build_id=KNOWN_BUILD_IDS[checkpoint + 1],
        )
        assert definitions[FAULT_DEFINE] == checkpoint
        identities.add(unstring(definitions["AIUS_BUILD_GIT_REV"]))
        build_ids.add(definitions["AIUS_BUILD_ID"])

    assert len(identities) == 10
    assert len(build_ids) == 10


def test_nonfault_environments_omit_fault_stall_define():
    for environment in (
        "teensy41-optical",
        "teensy41-release",
        "teensy41-instrument",
        "teensy41-capability",
    ):
        definitions = execute_metadata(environment)
        assert FAULT_DEFINE not in definitions


def test_build_id_is_timestamp_independent_and_source_sensitive():
    first = execute_metadata("teensy41-optical-fault", "4")
    second = execute_metadata("teensy41-optical-fault", "4")
    changed_source = execute_metadata(
        "teensy41-optical-fault",
        "4",
        revision="fedcba987654",
    )

    assert first["AIUS_BUILD_ID"] == second["AIUS_BUILD_ID"]
    assert (
        unstring(first["AIUS_BUILD_GIT_REV"])
        == unstring(second["AIUS_BUILD_GIT_REV"])
    )
    assert changed_source["AIUS_BUILD_ID"] != first["AIUS_BUILD_ID"]
    assert (
        unstring(changed_source["AIUS_BUILD_GIT_REV"])
        == "fedcba987654+fault.cp4"
    )


def expect_metadata_error(environment, selector, exact_message):
    try:
        execute_metadata(environment, selector)
    except ValueError as error:
        assert str(error) == exact_message, error
    else:
        raise AssertionError(
            f"expected metadata rejection for {environment!r}, {selector!r}"
        )


def test_metadata_rejects_missing_invalid_and_cross_environment_selectors():
    fault_error = (
        "teensy41-optical-fault requires AIUS_FAULT_CHECKPOINT in 0..8"
    )
    for selector in (None, "", "00", "01", "-1", "+1", " 1", "1 ", "9", "x"):
        expect_metadata_error(
            "teensy41-optical-fault",
            selector,
            fault_error,
        )

    cross_environment_error = (
        "AIUS_FAULT_CHECKPOINT is valid only for teensy41-optical-fault"
    )
    for environment in (
        "teensy41-optical",
        "teensy41-release",
        "teensy41-instrument",
        "teensy41-capability",
    ):
        expect_metadata_error(environment, "0", cross_environment_error)


def write_fake_pio(directory):
    executable = directory / "pio"
    executable.write_text(
        "#!/bin/sh\n"
        "printf '%s|%s\\n' \"${AIUS_FAULT_CHECKPOINT-UNSET}\" \"$*\""
        " >> \"${AIUS_TEST_PIO_RECORD:?missing record}\"\n"
        "if [ \"$1\" = \"--version\" ]; then\n"
        "  printf '%s\\n' 'fake-platformio-identity-oracle'\n"
        "  exit 0\n"
        "fi\n"
        "exit 64\n"
    )
    executable.chmod(0o755)


def invoke_build(arguments, inherited_selector=None):
    with tempfile.TemporaryDirectory(prefix="aius-build-cli-") as temporary:
        temporary_path = Path(temporary)
        record = temporary_path / "pio-record.txt"
        write_fake_pio(temporary_path)
        environment = os.environ.copy()
        environment["PATH"] = (
            f"{temporary_path}{os.pathsep}{environment['PATH']}"
        )
        environment["AIUS_TEST_PIO_RECORD"] = str(record)
        if inherited_selector is None:
            environment.pop("AIUS_FAULT_CHECKPOINT", None)
        else:
            environment["AIUS_FAULT_CHECKPOINT"] = inherited_selector

        result = subprocess.run(
            [str(BUILD_SCRIPT), *arguments],
            cwd=FIRMWARE_ROOT,
            env=environment,
            text=True,
            capture_output=True,
            check=False,
        )
        calls = record.read_text().splitlines() if record.exists() else []
        return result, calls


def expect_cli_usage_error(arguments, exact_message, inherited_selector=None):
    result, calls = invoke_build(arguments, inherited_selector)
    assert result.returncode == 4, (arguments, result)
    assert result.stdout == ""
    assert result.stderr == f"error: {exact_message}\n", (
        arguments,
        inherited_selector,
        result.stderr,
    )
    assert calls == []


def assert_accepted_build(arguments, target, selector):
    result, calls = invoke_build(arguments)
    assert result.returncode == 1, (arguments, result)
    assert calls == [
        "UNSET|--version",
        f"{selector}|run -e {target} -d {FIRMWARE_ROOT}",
    ], calls
    assert f"==> Building {target} for Teensy 4.1" in result.stdout
    assert f"==> Build FAILED for {target}" in result.stderr


def test_build_cli_accepts_all_fault_selectors_without_leaking_to_version_probe():
    for checkpoint in range(9):
        assert_accepted_build(
            ["teensy41-optical-fault", str(checkpoint)],
            "teensy41-optical-fault",
            str(checkpoint),
        )


def test_build_cli_rejects_invalid_missing_extra_and_cross_environment_selectors():
    fault_message = (
        "teensy41-optical-fault requires one checkpoint digit from 0 through 8"
    )
    for arguments in (
        ["teensy41-optical-fault"],
        ["teensy41-optical-fault", ""],
        ["teensy41-optical-fault", "00"],
        ["teensy41-optical-fault", "01"],
        ["teensy41-optical-fault", "-1"],
        ["teensy41-optical-fault", "+1"],
        ["teensy41-optical-fault", "9"],
        ["teensy41-optical-fault", "x"],
        ["teensy41-optical-fault", "0", "extra"],
    ):
        expect_cli_usage_error(arguments, fault_message)

    cross_environment_message = (
        "a checkpoint selector is valid only for teensy41-optical-fault"
    )
    inherited_selector_message = (
        "AIUS_FAULT_CHECKPOINT is build-script controlled; "
        "pass the checkpoint as an argument"
    )
    for environment in (
        "teensy41-optical",
        "teensy41-release",
        "teensy41-instrument",
        "teensy41-capability",
    ):
        expect_cli_usage_error(
            [environment, "0"],
            cross_environment_message,
        )
        expect_cli_usage_error(
            [environment],
            inherited_selector_message,
            inherited_selector="0",
        )

    expect_cli_usage_error(
        ["teensy41-optical-fault"],
        inherited_selector_message,
        inherited_selector="3",
    )


def test_build_cli_preserves_legacy_default_and_single_target_forms():
    assert_accepted_build([], "teensy41-release", "UNSET")
    for target in (
        "teensy41-release",
        "teensy41-instrument",
        "teensy41-capability",
        "teensy41-optical",
    ):
        assert_accepted_build([target], target, "UNSET")


def main():
    test_known_sha256_little_endian_vectors_and_all_selectors()
    test_nonfault_environments_omit_fault_stall_define()
    test_build_id_is_timestamp_independent_and_source_sensitive()
    test_metadata_rejects_missing_invalid_and_cross_environment_selectors()
    test_build_cli_accepts_all_fault_selectors_without_leaking_to_version_probe()
    test_build_cli_rejects_invalid_missing_extra_and_cross_environment_selectors()
    test_build_cli_preserves_legacy_default_and_single_target_forms()
    print("build metadata black-box tests: PASS")


if __name__ == "__main__":
    main()
