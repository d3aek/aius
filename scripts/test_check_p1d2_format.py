#!/usr/bin/env python3
"""Executable contract for the closed P1D2 formatting input set."""

from __future__ import annotations

import copy
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from validate_p1 import CRITICAL_FAMILIES, FAMILIES, GENERIC_FAMILIES, NONCRITICAL_FAMILIES


AIUS_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_ROOT = AIUS_ROOT / "scripts"
CHECKER_SOURCE = SCRIPT_ROOT / "check_p1d2_format.py"
REALIZATION_SOURCE = AIUS_ROOT / "evidence" / "manifest" / "supplemental-realization.json"

STATIC_INPUTS = (
    "evidence/manifest/sources.json",
    "evidence/manifest/assumptions.json",
    "evidence/manifest/supplemental-clone-boundary.json",
    "evidence/manifest/supplemental-requests.json",
)
REQUEST_DIRECTORY = "evidence/manifest/requests"
COMPONENT_DIRECTORY = "evidence/design/components"

with REALIZATION_SOURCE.open(encoding="utf-8") as handle:
    BASE_REALIZATION = json.load(handle)

REALIZATION_IDS = tuple(row["request_id"] for row in BASE_REALIZATION["realizations"])
LIFECYCLE_IDS = tuple(
    request_id
    for family in (*CRITICAL_FAMILIES, *NONCRITICAL_FAMILIES)
    for request_id in (f"REQ-{family.upper()}-MFR", f"REQ-{family.upper()}-UK")
)


class CheckerFixture:
    def __init__(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="aius-p1d2-format-")
        self.base = Path(self.temporary.name)
        self.repo_root = self.base / "repository with spaces & shell"
        self.aius_root = self.repo_root / "src" / "hardware" / "aius"
        self.script_root = self.aius_root / "scripts"
        self.manifest_root = self.aius_root / "evidence" / "manifest"
        self.request_root = self.manifest_root / "requests"
        self.component_root = self.aius_root / "evidence" / "design" / "components"
        self.checker = self.script_root / CHECKER_SOURCE.name
        self.realization = self.manifest_root / "supplemental-realization.json"
        self.prettier = self.repo_root / "node_modules" / ".bin" / "prettier"
        self.call_log = self.base / "prettier-calls.jsonl"
        self.caller_root = self.base / "unrelated-caller"
        self._create()

    def cleanup(self) -> None:
        self.temporary.cleanup()

    def _create(self) -> None:
        self.script_root.mkdir(parents=True)
        self.request_root.mkdir(parents=True)
        self.component_root.mkdir(parents=True)
        self.prettier.parent.mkdir(parents=True)
        self.caller_root.mkdir()
        shutil.copy2(CHECKER_SOURCE, self.checker)
        self.write_validator(FAMILIES)
        self.write_realization(copy.deepcopy(BASE_REALIZATION))
        for relative in STATIC_INPUTS:
            self.write_json(relative)
        for request_id in LIFECYCLE_IDS:
            self.write_json(f"{REQUEST_DIRECTORY}/{request_id}.json")
        for family in FAMILIES:
            self.write_json(f"{COMPONENT_DIRECTORY}/{family}.json")
        self.prettier.write_text(
            """#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

record = {"argv": sys.argv, "cwd": os.getcwd()}
with Path(os.environ["AIUS_P1D2_CALL_LOG"]).open("a", encoding="utf-8") as handle:
    handle.write(json.dumps(record) + "\\n")
raise SystemExit(int(os.environ.get("AIUS_P1D2_FORMAT_EXIT", "0")))
""",
            encoding="utf-8",
        )
        self.prettier.chmod(0o755)

    def write_validator(self, families: tuple[object, ...]) -> None:
        (self.script_root / "validate_p1.py").write_text(
            "\n".join(
                (
                    f"CRITICAL_FAMILIES = {CRITICAL_FAMILIES!r}",
                    f"NONCRITICAL_FAMILIES = {NONCRITICAL_FAMILIES!r}",
                    f"GENERIC_FAMILIES = {GENERIC_FAMILIES!r}",
                    f"FAMILIES = {families!r}",
                    "",
                )
            ),
            encoding="utf-8",
        )

    def path(self, relative: str) -> Path:
        return self.aius_root / relative

    def write_json(self, relative: str) -> None:
        path = self.path(relative)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("{}\n", encoding="utf-8")

    def write_realization(self, document: object) -> None:
        self.realization.write_text(json.dumps(document) + "\n", encoding="utf-8")

    def run(self, *arguments: str, formatter_exit: int = 0) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        environment["AIUS_P1D2_CALL_LOG"] = os.fspath(self.call_log)
        environment["AIUS_P1D2_FORMAT_EXIT"] = str(formatter_exit)
        return subprocess.run(
            [sys.executable, os.fspath(self.checker), *arguments],
            cwd=self.caller_root,
            env=environment,
            text=True,
            capture_output=True,
            check=False,
        )

    def calls(self) -> list[dict[str, object]]:
        if not self.call_log.exists():
            return []
        return [json.loads(line) for line in self.call_log.read_text(encoding="utf-8").splitlines()]


class P1D2FormatCheckerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture = CheckerFixture()

    def tearDown(self) -> None:
        self.fixture.cleanup()

    def assert_preflight_rejected(self, result: subprocess.CompletedProcess[str]) -> None:
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.fixture.calls(), [])
        self.assertTrue(result.stderr.strip())

    def test_invokes_pinned_prettier_once_with_exact_sorted_argv_from_unrelated_cwd(self) -> None:
        result = self.fixture.run()

        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.fixture.calls()
        self.assertEqual(len(calls), 1)
        expected_operands = sorted(
            (
                *(f"{relative}" for relative in STATIC_INPUTS),
                *(
                    f"{REQUEST_DIRECTORY}/{request_id}.json"
                    for request_id in REALIZATION_IDS
                ),
                *(
                    f"{COMPONENT_DIRECTORY}/{family}.json"
                    for family in FAMILIES
                ),
            )
        )
        expected_argv = [os.fspath(self.fixture.prettier), "--check", *expected_operands]
        self.assertEqual(len(expected_operands), 79)
        self.assertEqual(len(expected_argv), 81)
        self.assertEqual(calls[0]["argv"], expected_argv)
        self.assertEqual(calls[0]["cwd"], os.fspath(self.fixture.repo_root))

    def test_propagates_formatter_nonzero_exit_after_one_call(self) -> None:
        result = self.fixture.run(formatter_exit=23)

        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertEqual(len(self.fixture.calls()), 1)

    def test_rejects_extra_cli_arguments_before_formatting(self) -> None:
        self.assert_preflight_rejected(self.fixture.run("unexpected"))

    def test_rejects_duplicate_realization_request_ids_before_formatting(self) -> None:
        document = copy.deepcopy(BASE_REALIZATION)
        document["realizations"][-1]["request_id"] = document["realizations"][0]["request_id"]
        self.fixture.write_realization(document)

        self.assert_preflight_rejected(self.fixture.run())

    def test_rejects_45_realization_rows_before_formatting(self) -> None:
        document = copy.deepcopy(BASE_REALIZATION)
        document["realizations"].pop()
        self.fixture.write_realization(document)

        self.assert_preflight_rejected(self.fixture.run())

    def test_rejects_47_realization_rows_before_formatting(self) -> None:
        document = copy.deepcopy(BASE_REALIZATION)
        document["realizations"].append({"request_id": "REQ-UNEXPECTED-MFR"})
        self.fixture.write_realization(document)

        self.assert_preflight_rejected(self.fixture.run())

    def test_rejects_non_lifecycle_realization_request_before_formatting(self) -> None:
        document = copy.deepcopy(BASE_REALIZATION)
        document["realizations"][-1]["request_id"] = "REQ-UNEXPECTED-MFR"
        self.fixture.write_realization(document)

        self.assert_preflight_rejected(self.fixture.run())

    def test_rejects_each_missing_static_input_before_formatting(self) -> None:
        for relative in STATIC_INPUTS:
            with self.subTest(relative=relative):
                path = self.fixture.path(relative)
                path.unlink()
                self.assert_preflight_rejected(self.fixture.run())
                self.fixture.write_json(relative)

    def test_rejects_missing_request_input_before_formatting(self) -> None:
        membership_only_id = next(request_id for request_id in LIFECYCLE_IDS if request_id not in REALIZATION_IDS)
        for request_id in (REALIZATION_IDS[0], membership_only_id):
            with self.subTest(request_id=request_id):
                request = self.fixture.request_root / f"{request_id}.json"
                request.unlink()
                self.assert_preflight_rejected(self.fixture.run())
                self.fixture.write_json(f"{REQUEST_DIRECTORY}/{request_id}.json")

    def test_rejects_missing_component_input_before_formatting(self) -> None:
        (self.fixture.component_root / f"{FAMILIES[0]}.json").unlink()

        self.assert_preflight_rejected(self.fixture.run())

    def test_rejects_extra_request_json_member_before_formatting(self) -> None:
        self.fixture.write_json(f"{REQUEST_DIRECTORY}/REQ-EXTRA-MFR.json")

        self.assert_preflight_rejected(self.fixture.run())

    def test_rejects_extra_component_json_member_before_formatting(self) -> None:
        self.fixture.write_json(f"{COMPONENT_DIRECTORY}/extra-family.json")

        self.assert_preflight_rejected(self.fixture.run())

    def test_rejects_lexical_request_traversal_before_formatting(self) -> None:
        document = copy.deepcopy(BASE_REALIZATION)
        document["realizations"][-1]["request_id"] = "../escape"
        self.fixture.write_realization(document)
        self.fixture.write_json("evidence/manifest/escape.json")

        self.assert_preflight_rejected(self.fixture.run())

    def test_rejects_lexical_component_traversal_before_formatting(self) -> None:
        self.fixture.write_validator(("../escape", *FAMILIES[1:]))
        self.fixture.write_json("evidence/design/escape.json")

        self.assert_preflight_rejected(self.fixture.run())

    def test_rejects_request_symlink_escape_from_request_directory(self) -> None:
        membership_only_id = next(request_id for request_id in LIFECYCLE_IDS if request_id not in REALIZATION_IDS)
        request = self.fixture.request_root / f"{membership_only_id}.json"
        request.unlink()
        request.symlink_to(self.fixture.path(STATIC_INPUTS[0]))

        self.assert_preflight_rejected(self.fixture.run())

    def test_rejects_component_symlink_escape_from_component_directory(self) -> None:
        outside_component_root = self.fixture.aius_root / "evidence" / "design" / "outside.json"
        outside_component_root.write_text("{}\n", encoding="utf-8")
        component = self.fixture.component_root / f"{FAMILIES[0]}.json"
        component.unlink()
        component.symlink_to(outside_component_root)

        self.assert_preflight_rejected(self.fixture.run())

    def test_rejects_static_symlink_escape_from_aius_root(self) -> None:
        outside = self.fixture.base / "outside.json"
        outside.write_text("{}\n", encoding="utf-8")
        static = self.fixture.path(STATIC_INPUTS[0])
        static.unlink()
        static.symlink_to(outside)

        self.assert_preflight_rejected(self.fixture.run())

    def test_rejects_malformed_realization_json_before_formatting(self) -> None:
        self.fixture.realization.write_text("{\n", encoding="utf-8")

        self.assert_preflight_rejected(self.fixture.run())

    def test_rejects_malformed_realization_shape_before_formatting(self) -> None:
        self.fixture.write_realization({"realizations": [None] * 46})

        self.assert_preflight_rejected(self.fixture.run())

    def test_rejects_non_file_operand_before_formatting(self) -> None:
        static = self.fixture.path(STATIC_INPUTS[0])
        static.unlink()
        static.mkdir()

        self.assert_preflight_rejected(self.fixture.run())

    def test_ignores_non_json_directory_members(self) -> None:
        (self.fixture.request_root / "README.txt").write_text("not an operand\n", encoding="utf-8")
        (self.fixture.component_root / "README.txt").write_text("not an operand\n", encoding="utf-8")

        result = self.fixture.run()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.fixture.calls()), 1)


if __name__ == "__main__":
    unittest.main()
