#!/usr/bin/env python3
"""Black-box oracle for selector-specific optical build evidence."""

from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
REPOSITORY_ROOT = FIRMWARE_ROOT.parents[3]
BUILD_SCRIPT = FIRMWARE_ROOT / "scripts" / "build.sh"
PIO_ROOT = FIRMWARE_ROOT / ".pio"
EVIDENCE_ROOT = PIO_ROOT / "evidence"
OPTICAL_LOCK = PIO_ROOT / "optical-build.lock"
FULL_REVISION_A = "0123456789abcdef0123456789abcdef01234567"
FULL_REVISION_B = "fedcba9876543210fedcba9876543210fedcba98"
ARTIFACT_NAMES = {
    "build.log",
    "firmware.elf",
    "firmware.hex",
    "receipt.json",
}
RECEIPT_KEYS = {
    "source_revision",
    "environment",
    "selector",
    "elf_sha256",
    "hex_sha256",
}
MEMORY_OUTPUT = """\
teensy_size: Memory Usage on Teensy 4.1:
teensy_size:   FLASH: code:1000, data:100, headers:100   free for files:8000000
teensy_size:   RAM1: variables:1000, code:1000, padding:100   free for local variables:500000
teensy_size:   RAM2: variables:1000  free for malloc/new:500000
"""


def write_executable(path, contents):
    path.write_text(contents)
    path.chmod(0o755)


class ControlledTools:
    def __init__(self, root):
        self.root = root
        self.bin = root / "bin"
        self.bin.mkdir()
        self.core = root / "platformio-core"
        toolchain_bin = (
            self.core
            / "packages"
            / "toolchain-gccarmnoneeabi-teensy"
            / "bin"
        )
        toolchain_bin.mkdir(parents=True)
        self.calls = root / "calls.jsonl"
        self.git_count = root / "git-count"
        real_sha256sum = shutil.which("sha256sum")
        assert real_sha256sum is not None
        write_executable(
            self.bin / "git",
            """#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

record = Path(os.environ["AIUS_TEST_TOOL_CALLS"])
with record.open("a") as stream:
    stream.write(json.dumps({
        "tool": "git",
        "args": sys.argv[1:],
    }) + "\\n")
counter = Path(os.environ["AIUS_TEST_GIT_COUNT"])
count = int(counter.read_text()) if counter.exists() else 0
counter.write_text(str(count + 1))
revisions = os.environ["AIUS_TEST_GIT_REVISIONS"].split(",")
print(revisions[min(count, len(revisions) - 1)])
""",
        )
        write_executable(
            self.bin / "pio",
            f"""#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

record = Path(os.environ["AIUS_TEST_TOOL_CALLS"])
selector = os.environ.get("AIUS_FAULT_CHECKPOINT")
with record.open("a") as stream:
    stream.write(json.dumps({{
        "tool": "pio",
        "args": sys.argv[1:],
        "selector": selector,
    }}) + "\\n")
if sys.argv[1:] == ["--version"]:
    print("fake PlatformIO")
    raise SystemExit(0)
mode = os.environ.get("AIUS_TEST_PIO_MODE", "success")
if mode == "fail":
    print("injected pio stdout")
    print("injected pio stderr", file=sys.stderr)
    raise SystemExit(23)
environment = sys.argv[sys.argv.index("-e") + 1]
build = Path({str(FIRMWARE_ROOT)!r}) / ".pio" / "build" / environment
build.mkdir(parents=True, exist_ok=True)
identity = selector if selector is not None else "production"
if mode != "missing-elf":
    (build / "firmware.elf").write_bytes(
        ("ELF|" + environment + "|" + identity).encode()
    )
if mode != "missing-hex":
    (build / "firmware.hex").write_bytes(
        ("HEX|" + environment + "|" + identity).encode()
    )
print("pio-stdout-marker:" + identity)
print("pio-stderr-marker:" + identity, file=sys.stderr)
print({MEMORY_OUTPUT!r}, end="")
""",
        )
        write_executable(
            self.bin / "sha256sum",
            f"""#!/bin/sh
if [ "${{AIUS_TEST_HASH_FAILURE-}}" = "1" ]; then
  printf '%s\\n' 'injected sha256 failure' >&2
  exit 86
fi
exec {real_sha256sum} "$@"
""",
        )
        write_executable(
            toolchain_bin / "arm-none-eabi-gcc",
            """#!/bin/sh
printf '%s\n' 'arm-none-eabi-gcc (controlled oracle) 1.0'
""",
        )
        write_executable(
            toolchain_bin / "arm-none-eabi-nm",
            """#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

record = Path(os.environ["AIUS_TEST_TOOL_CALLS"])
with record.open("a") as stream:
    stream.write(json.dumps({
        "tool": "nm",
        "args": sys.argv[1:],
    }) + "\\n")
print("2027ff00 00000080 B g_aius_boot_slots")
""",
        )

    def environment(
        self,
        *,
        mode="success",
        revisions=(FULL_REVISION_A,),
        hash_failure=False,
    ):
        if self.git_count.exists():
            self.git_count.unlink()
        environment = os.environ.copy()
        environment["PATH"] = (
            f"{self.bin}{os.pathsep}{environment['PATH']}"
        )
        environment["PIO"] = str(self.bin / "pio")
        environment["PLATFORMIO_CORE_DIR"] = str(self.core)
        environment["AIUS_TEST_TOOL_CALLS"] = str(self.calls)
        environment["AIUS_TEST_GIT_COUNT"] = str(self.git_count)
        environment["AIUS_TEST_GIT_REVISIONS"] = ",".join(revisions)
        environment["AIUS_TEST_PIO_MODE"] = mode
        if hash_failure:
            environment["AIUS_TEST_HASH_FAILURE"] = "1"
        else:
            environment.pop("AIUS_TEST_HASH_FAILURE", None)
        environment.pop("AIUS_FAULT_CHECKPOINT", None)
        return environment

    def reset_calls(self):
        if self.calls.exists():
            self.calls.unlink()

    def read_calls(self):
        if not self.calls.exists():
            return []
        return [
            json.loads(line)
            for line in self.calls.read_text().splitlines()
        ]


def remove_path(path):
    if path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


def clear_optical_outputs():
    remove_path(EVIDENCE_ROOT)
    for environment in ("teensy41-optical", "teensy41-optical-fault"):
        remove_path(PIO_ROOT / "build" / environment)
        remove_path(PIO_ROOT / f"build-{environment}.log")


@contextmanager
def controlled_build_area():
    scratch_root = REPOSITORY_ROOT / ".scratch"
    scratch_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="aius-build-evidence-", dir=scratch_root
    ) as temporary:
        clear_optical_outputs()
        try:
            yield ControlledTools(Path(temporary))
        finally:
            clear_optical_outputs()


def invoke(tools, arguments, **environment_options):
    tools.reset_calls()
    return subprocess.run(
        [str(BUILD_SCRIPT), *arguments],
        cwd=FIRMWARE_ROOT,
        env=tools.environment(**environment_options),
        text=True,
        capture_output=True,
        check=False,
    )


def evidence_name(selector):
    return "production" if selector is None else f"fault-cp{selector}"


def build_arguments(selector):
    if selector is None:
        return ["teensy41-optical"]
    return ["teensy41-optical-fault", str(selector)]


def expected_environment(selector):
    if selector is None:
        return "teensy41-optical"
    return "teensy41-optical-fault"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def assert_receipt(selector, revision=FULL_REVISION_A):
    evidence = EVIDENCE_ROOT / evidence_name(selector)
    assert evidence.is_dir(), evidence
    assert {path.name for path in evidence.iterdir()} == ARTIFACT_NAMES
    receipt = json.loads((evidence / "receipt.json").read_text())
    assert set(receipt) == RECEIPT_KEYS
    assert receipt["source_revision"] == revision
    assert receipt["environment"] == expected_environment(selector)
    assert receipt["selector"] == selector
    assert receipt["elf_sha256"] == sha256(evidence / "firmware.elf")
    assert receipt["hex_sha256"] == sha256(evidence / "firmware.hex")
    build_log = (evidence / "build.log").read_text()
    identity = "production" if selector is None else str(selector)
    assert f"pio-stdout-marker:{identity}" in build_log
    assert f"pio-stderr-marker:{identity}" in build_log
    return evidence, receipt


def seed_stale_evidence(selector):
    evidence = EVIDENCE_ROOT / evidence_name(selector)
    evidence.mkdir(parents=True, exist_ok=True)
    elf = evidence / "firmware.elf"
    hex_image = evidence / "firmware.hex"
    elf.write_bytes(b"previous valid elf")
    hex_image.write_bytes(b"previous valid hex")
    (evidence / "build.log").write_text("previous complete build log\n")
    (evidence / "receipt.json").write_text(
        json.dumps(
            {
                "source_revision": FULL_REVISION_A,
                "environment": expected_environment(selector),
                "selector": selector,
                "elf_sha256": sha256(elf),
                "hex_sha256": sha256(hex_image),
            },
            indent=2,
        )
        + "\n"
    )


def test_successful_production_and_all_fault_evidence(tools):
    archived_contents = set()
    for selector in (None, *range(9)):
        result = invoke(tools, build_arguments(selector))
        assert result.returncode == 0, (selector, result)
        pio_calls = [
            call for call in tools.read_calls() if call["tool"] == "pio"
        ]
        nm_calls = [
            call for call in tools.read_calls() if call["tool"] == "nm"
        ]
        assert pio_calls == [
            {"tool": "pio", "args": ["--version"], "selector": None},
            {
                "tool": "pio",
                "args": [
                    "run",
                    "-e",
                    expected_environment(selector),
                    "-d",
                    str(FIRMWARE_ROOT),
                ],
                "selector": None if selector is None else str(selector),
            },
        ]
        assert len(nm_calls) == 1, nm_calls
        assert nm_calls[0]["args"][-1] == str(
            PIO_ROOT
            / "build"
            / expected_environment(selector)
            / "firmware.elf"
        )
        evidence, _receipt = assert_receipt(selector)
        archived_contents.add((evidence / "firmware.elf").read_bytes())
        archived_contents.add((evidence / "firmware.hex").read_bytes())

    assert {path.name for path in EVIDENCE_ROOT.iterdir()} == {
        "production",
        *(f"fault-cp{selector}" for selector in range(9)),
    }
    assert len(archived_contents) == 20
    for selector in (None, *range(9)):
        assert_receipt(selector)


def test_one_shared_nonblocking_lock_precedes_mutation(tools):
    PIO_ROOT.mkdir(parents=True, exist_ok=True)
    OPTICAL_LOCK.touch(exist_ok=True)
    seed_stale_evidence(None)
    seed_stale_evidence(0)
    before = {
        path: path.read_bytes()
        for path in EVIDENCE_ROOT.rglob("*")
        if path.is_file()
    }
    with OPTICAL_LOCK.open("r+") as lock_stream:
        fcntl.flock(lock_stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        for selector in (None, 0):
            result = invoke(tools, build_arguments(selector))
            assert result.returncode == 75, (selector, result)
            assert not [
                call
                for call in tools.read_calls()
                if call["tool"] == "pio"
            ]
            after = {
                path: path.read_bytes()
                for path in EVIDENCE_ROOT.rglob("*")
                if path.is_file()
            }
            assert after == before


def test_failure_paths_publish_no_partial_or_stale_receipt(tools):
    cases = (
        ("fail", False, (FULL_REVISION_A,), None),
        ("missing-elf", False, (FULL_REVISION_A,), None),
        ("missing-hex", False, (FULL_REVISION_A,), None),
        ("success", True, (FULL_REVISION_A,), None),
        ("success", False, (FULL_REVISION_A, FULL_REVISION_B), None),
        ("fail", False, (FULL_REVISION_A,), 4),
    )
    retained_receipts = []
    for mode, hash_failure, revisions, selector in cases:
        clear_optical_outputs()
        seed_stale_evidence(selector)
        result = invoke(
            tools,
            build_arguments(selector),
            mode=mode,
            revisions=revisions,
            hash_failure=hash_failure,
        )
        assert result.returncode != 0, (
            mode,
            hash_failure,
            revisions,
            selector,
            result,
        )
        receipt = (
            EVIDENCE_ROOT
            / evidence_name(selector)
            / "receipt.json"
        )
        if receipt.exists():
            retained_receipts.append(
                (
                    mode,
                    hash_failure,
                    revisions,
                    selector,
                    result.returncode,
                    receipt.read_text(),
                )
            )
    assert not retained_receipts, retained_receipts


def test_selector_archives_remain_isolated(tools):
    first = invoke(tools, build_arguments(3))
    assert first.returncode == 0, first
    cp3, _first_receipt = assert_receipt(3)
    cp3_elf = (cp3 / "firmware.elf").read_bytes()

    second = invoke(tools, build_arguments(7))
    assert second.returncode == 0, second
    cp7, _second_receipt = assert_receipt(7)
    assert (cp3 / "firmware.elf").read_bytes() == cp3_elf
    assert (cp7 / "firmware.elf").read_bytes() != cp3_elf


def main():
    with controlled_build_area() as tools:
        test_successful_production_and_all_fault_evidence(tools)
        clear_optical_outputs()
        test_one_shared_nonblocking_lock_precedes_mutation(tools)
        clear_optical_outputs()
        test_selector_archives_remain_isolated(tools)
        clear_optical_outputs()
        test_failure_paths_publish_no_partial_or_stale_receipt(tools)
    print("build evidence black-box tests: PASS")


if __name__ == "__main__":
    main()
