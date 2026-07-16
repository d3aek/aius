#!/usr/bin/env python3
"""Tests for deterministic Aius P1 review-artifact rendering."""

from __future__ import annotations

import copy
import io
import json
import os
import shutil
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

import render_p1


SCRIPT_ROOT = Path(__file__).resolve().parent
FIXTURE_ROOT = SCRIPT_ROOT / "fixtures" / "render-p1" / "escaping"
INPUT_ROOT = FIXTURE_ROOT / "inputs"
GOLDEN_ROOT = FIXTURE_ROOT / "golden"

COMMANDS = {
    "functional-topology": (
        Path("docs/reference/functional-topology.md"),
        GOLDEN_ROOT / "functional-topology.md",
    ),
    "interfaces-and-states": (
        Path("docs/reference/interfaces-and-states.md"),
        GOLDEN_ROOT / "interfaces-and-states.md",
    ),
    "components": (
        Path("evidence/design/provisional-components.csv"),
        GOLDEN_ROOT / "provisional-components.csv",
    ),
    "supplemental-requests": (
        Path("evidence/manifest/supplemental-requests.json"),
        GOLDEN_ROOT / "supplemental-requests.golden",
    ),
}


def _json(relative: str) -> object:
    return json.loads((INPUT_ROOT / relative).read_text(encoding="utf-8"))


def _components() -> list[object]:
    directory = INPUT_ROOT / "evidence" / "design" / "components"
    return [json.loads(path.read_text(encoding="utf-8")) for path in directory.glob("*.json")]


def _requests() -> list[object]:
    directory = INPUT_ROOT / "evidence" / "manifest" / "requests"
    return [json.loads(path.read_text(encoding="utf-8")) for path in directory.glob("*.json")]


class PureRendererTests(unittest.TestCase):
    def test_all_renderers_match_adversarial_golden_bytes(self) -> None:
        policy = _json("evidence/design/selection-policy.json")
        actual = {
            "functional-topology.md": render_p1.render_functional_topology(
                _json("evidence/design/functional-topology.json")
            ),
            "interfaces-and-states.md": render_p1.render_interfaces_and_states(
                _json("evidence/design/interfaces-and-states.json")
            ),
            "provisional-components.csv": render_p1.render_components(
                policy, reversed(_components())
            ),
            "supplemental-requests.golden": render_p1.render_supplemental_requests(
                reversed(_requests())
            ),
        }
        for name, rendered in actual.items():
            with self.subTest(name=name):
                self.assertEqual((GOLDEN_ROOT / name).read_bytes(), rendered)
                self.assertFalse(rendered.startswith(b"\xef\xbb\xbf"))
                self.assertNotIn(b"\r", rendered)
                self.assertTrue(rendered.endswith(b"\n"))
                self.assertFalse(rendered.endswith(b"\n\n"))

    def test_pure_functions_are_repeatable_do_not_mutate_inputs_or_open_files(self) -> None:
        functional = _json("evidence/design/functional-topology.json")
        interfaces = _json("evidence/design/interfaces-and-states.json")
        policy = _json("evidence/design/selection-policy.json")
        components = _components()
        requests = _requests()
        before = copy.deepcopy((functional, interfaces, policy, components, requests))

        with mock.patch("builtins.open", side_effect=AssertionError("filesystem access")):
            first = (
                render_p1.render_functional_topology(functional),
                render_p1.render_interfaces_and_states(interfaces),
                render_p1.render_components(policy, components),
                render_p1.render_supplemental_requests(requests),
            )
            second = (
                render_p1.render_functional_topology(functional),
                render_p1.render_interfaces_and_states(interfaces),
                render_p1.render_components(policy, components),
                render_p1.render_supplemental_requests(requests),
            )

        self.assertEqual(first, second)
        self.assertEqual(
            before,
            (functional, interfaces, policy, components, requests),
        )

    def test_markdown_retains_rows_and_arrays_in_input_order(self) -> None:
        document = _json("evidence/design/functional-topology.json")
        document["evidence_ids"] = ["z-last", "a-first"]
        rendered = render_p1.render_functional_topology(document).decode("utf-8")
        self.assertLess(rendered.index("| z-last |"), rendered.index("| a-first |"))
        self.assertIn("block-a<br>block-b", rendered)
        self.assertIn("pcm\\|stereo<br>iec\\\\61937", rendered)
        self.assertIn('{"a":"slash\\\\\\\\","z":"pipe\\|"}', rendered)
        self.assertIn("line 1<br>line 2<br>line 3<br>line 4", rendered)

    def test_flow_sections_filter_kind_without_reordering_rows(self) -> None:
        document = _json("evidence/design/functional-topology.json")
        duplicate = copy.deepcopy(document["flows"][0])
        duplicate["id"] = "flow-data-second"
        document["flows"].insert(1, duplicate)
        rendered = render_p1.render_functional_topology(document).decode("utf-8")
        data_start = rendered.index("## Data flows")
        control_start = rendered.index("## Control flows")
        data_section = rendered[data_start:control_start]
        self.assertLess(data_section.index("flow-data |"), data_section.index("flow-data-second |"))
        self.assertNotIn("flow-control", data_section)
        self.assertNotIn("flow-power", data_section)

    def test_csv_sorts_blocks_and_compacts_array_object_cells(self) -> None:
        rendered = render_p1.render_components(
            _json("evidence/design/selection-policy.json"),
            reversed(_components()),
        ).decode("utf-8")
        self.assertLess(rendered.index("\nalpha,"), rendered.index("\nzeta,"))
        self.assertIn('"[""REQT-QUOTE"",""REQT-Ω""]"', rendered)
        self.assertIn(
            '"[{""authority_url"":""https://example.test/a"",""manufacturer"":""Beta""',
            rendered,
        )
        self.assertIn('"Acme, Inc."', rendered)
        self.assertIn('"A""1"', rendered)
        self.assertIn('"comma, quote "" and\nnewline"', rendered)

    def test_supplemental_requests_sorts_records_and_reorders_every_key(self) -> None:
        rendered = render_p1.render_supplemental_requests(reversed(_requests()))
        parsed = json.loads(rendered)
        self.assertEqual(["REQ-A", "REQ-Z"], [row["request_id"] for row in parsed["requests"]])
        self.assertEqual(
            list(render_p1.REQUEST_KEYS),
            list(parsed["requests"][0].keys()),
        )
        self.assertEqual(["schema_version", "requests"], list(parsed.keys()))


class CliTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "aius"
        shutil.copytree(INPUT_ROOT, self.root)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _run(
        self,
        command: str,
        mode: str,
        *,
        replace=None,
    ) -> tuple[int, str, str]:
        stdout = io.StringIO()
        stderr = io.StringIO()
        arguments = ["--root", os.fspath(self.root), command, mode]
        with redirect_stdout(stdout), redirect_stderr(stderr):
            result = render_p1.run(arguments, replace=replace)
        return result, stdout.getvalue(), stderr.getvalue()

    def _write_golden(self, command: str) -> Path:
        relative, golden = COMMANDS[command]
        target = self.root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(golden.read_bytes())
        return target

    def test_check_success_is_exact_and_never_writes(self) -> None:
        for command, (relative, _) in COMMANDS.items():
            with self.subTest(command=command):
                target = self._write_golden(command)
                before = target.stat()
                result, stdout, stderr = self._run(command, "--check")
                after = target.stat()
                self.assertEqual(0, result)
                self.assertEqual(f"OK render {command} {relative.as_posix()}\n", stdout)
                self.assertEqual("", stderr)
                self.assertEqual(before.st_mtime_ns, after.st_mtime_ns)
                self.assertEqual(before.st_ino, after.st_ino)

    def test_write_atomically_replaces_only_the_mapped_output(self) -> None:
        for command, (relative, golden) in COMMANDS.items():
            with self.subTest(command=command):
                target = self.root / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(b"stale\n")
                sentinel = target.parent / "unrelated.keep"
                sentinel.write_bytes(b"untouched")
                replacements: list[tuple[Path, Path]] = []

                def replace(source: str, destination: str) -> None:
                    replacements.append((Path(source), Path(destination)))
                    self.assertEqual(b"stale\n", target.read_bytes())
                    os.replace(source, destination)

                result, stdout, stderr = self._run(command, "--write", replace=replace)
                self.assertEqual(0, result)
                self.assertEqual(f"OK render {command} {relative.as_posix()}\n", stdout)
                self.assertEqual("", stderr)
                self.assertEqual(golden.read_bytes(), target.read_bytes())
                self.assertEqual(b"untouched", sentinel.read_bytes())
                self.assertEqual(1, len(replacements))
                temporary, destination = replacements[0]
                self.assertEqual(target, destination)
                self.assertEqual(target.parent, temporary.parent)
                self.assertTrue(temporary.name.startswith(f".{target.name}.aius-"))
                self.assertFalse(temporary.exists())

    def test_missing_output_has_exact_diagnostic_and_check_does_not_create_it(self) -> None:
        relative, _ = COMMANDS["functional-topology"]
        result, stdout, stderr = self._run("functional-topology", "--check")
        self.assertEqual(1, result)
        self.assertEqual("", stdout)
        self.assertEqual(
            f"AIUS-P1-RENDER-MISSING: {relative.as_posix()}\n",
            stderr,
        )
        self.assertFalse((self.root / relative).exists())

    def test_stale_output_has_exact_diagnostic_and_check_does_not_change_it(self) -> None:
        relative, _ = COMMANDS["interfaces-and-states"]
        target = self.root / relative
        target.parent.mkdir(parents=True)
        target.write_bytes(b"stale\r\n")
        before = target.stat()
        result, stdout, stderr = self._run("interfaces-and-states", "--check")
        self.assertEqual(1, result)
        self.assertEqual("", stdout)
        self.assertEqual(f"AIUS-P1-RENDER-STALE: {relative.as_posix()}\n", stderr)
        self.assertEqual(b"stale\r\n", target.read_bytes())
        self.assertEqual(before.st_mtime_ns, target.stat().st_mtime_ns)

    def test_missing_and_malformed_single_inputs_are_input_errors(self) -> None:
        relative = Path("evidence/design/functional-topology.json")
        path = self.root / relative
        path.unlink()
        result, stdout, stderr = self._run("functional-topology", "--check")
        self.assertEqual((1, ""), (result, stdout))
        self.assertEqual(f"AIUS-P1-INPUT: {relative.as_posix()}: missing input\n", stderr)

        path.write_bytes(b'{"broken":')
        result, stdout, stderr = self._run("functional-topology", "--check")
        self.assertEqual((1, ""), (result, stdout))
        self.assertTrue(stderr.startswith(f"AIUS-P1-INPUT: {relative.as_posix()}: "))
        self.assertFalse((self.root / COMMANDS["functional-topology"][0]).exists())

    def test_invalid_utf8_and_malformed_structure_are_input_errors(self) -> None:
        relative = Path("evidence/design/interfaces-and-states.json")
        path = self.root / relative
        path.write_bytes(b"\xff")
        result, stdout, stderr = self._run("interfaces-and-states", "--write")
        self.assertEqual((1, ""), (result, stdout))
        self.assertTrue(stderr.startswith(f"AIUS-P1-INPUT: {relative.as_posix()}: "))

        path.write_text("{}", encoding="utf-8")
        result, stdout, stderr = self._run("interfaces-and-states", "--write")
        self.assertEqual((1, ""), (result, stdout))
        self.assertTrue(stderr.startswith(f"AIUS-P1-INPUT: {relative.as_posix()}: "))
        self.assertIn("malformed input structure", stderr)

    def test_missing_or_empty_fragment_directories_are_input_errors(self) -> None:
        cases = (
            ("components", Path("evidence/design/components")),
            ("supplemental-requests", Path("evidence/manifest/requests")),
        )
        for command, relative in cases:
            with self.subTest(command=command, shape="missing"):
                directory = self.root / relative
                shutil.rmtree(directory)
                result, stdout, stderr = self._run(command, "--check")
                self.assertEqual((1, ""), (result, stdout))
                self.assertEqual(
                    f"AIUS-P1-INPUT: {relative.as_posix()}: missing input directory\n",
                    stderr,
                )
                directory.mkdir(parents=True)
            with self.subTest(command=command, shape="empty"):
                result, stdout, stderr = self._run(command, "--check")
                self.assertEqual((1, ""), (result, stdout))
                self.assertEqual(
                    f"AIUS-P1-INPUT: {relative.as_posix()}: contains no JSON inputs\n",
                    stderr,
                )

    def test_failed_atomic_replace_preserves_old_output_and_cleans_temporary(self) -> None:
        command = "components"
        relative, _ = COMMANDS[command]
        target = self.root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b"old bytes\n")

        def fail_replace(source: str, destination: str) -> None:
            raise OSError("injected replace failure")

        result, stdout, stderr = self._run(command, "--write", replace=fail_replace)
        self.assertEqual(1, result)
        self.assertEqual("", stdout)
        self.assertEqual(b"old bytes\n", target.read_bytes())
        self.assertEqual([], list(target.parent.glob(f".{target.name}.aius-*.tmp")))
        self.assertEqual(
            f"AIUS-P1-RENDER-WRITE: {relative.as_posix()}: injected replace failure\n",
            stderr,
        )

    def test_atomic_temporary_collision_never_unlinks_foreign_file(self) -> None:
        command = "functional-topology"
        relative, _ = COMMANDS[command]
        target = self.root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        sentinel = target.parent / "unrelated.keep"
        sentinel.write_bytes(b"unrelated bytes")

        for output_exists in (True, False):
            with self.subTest(output_exists=output_exists):
                token = f"forced-collision-{output_exists}"
                temporary = target.with_name(f".{target.name}.aius-{token}.tmp")
                foreign_bytes = f"foreign temporary {output_exists}".encode("ascii")
                temporary.write_bytes(foreign_bytes)
                if output_exists:
                    target.write_bytes(b"existing output")
                else:
                    target.unlink(missing_ok=True)

                with mock.patch.object(render_p1.secrets, "token_hex", return_value=token):
                    result, stdout, stderr = self._run(command, "--write")

                self.assertEqual(1, result)
                self.assertEqual("", stdout)
                self.assertTrue(
                    stderr.startswith(
                        f"AIUS-P1-RENDER-WRITE: {relative.as_posix()}: "
                    )
                )
                self.assertIn("File exists", stderr)
                self.assertEqual(foreign_bytes, temporary.read_bytes())
                if output_exists:
                    self.assertEqual(b"existing output", target.read_bytes())
                else:
                    self.assertFalse(target.exists())
                self.assertEqual(b"unrelated bytes", sentinel.read_bytes())

    def test_exactly_one_mode_and_a_command_are_required_by_argparse(self) -> None:
        cases = (
            ["--root", os.fspath(self.root)],
            ["--root", os.fspath(self.root), "functional-topology"],
            [
                "--root",
                os.fspath(self.root),
                "functional-topology",
                "--write",
                "--check",
            ],
        )
        for arguments in cases:
            with self.subTest(arguments=arguments):
                stdout = io.StringIO()
                stderr = io.StringIO()
                with redirect_stdout(stdout), redirect_stderr(stderr):
                    with self.assertRaises(SystemExit) as raised:
                        render_p1.run(arguments)
                self.assertEqual(2, raised.exception.code)
                self.assertEqual("", stdout.getvalue())
                self.assertIn("error:", stderr.getvalue())

    def test_root_defaults_to_parent_of_scripts(self) -> None:
        with mock.patch.object(render_p1, "__file__", self.root / "scripts" / "render_p1.py"):
            relative, golden = COMMANDS["functional-topology"]
            target = self.root / relative
            target.parent.mkdir(parents=True)
            target.write_bytes(golden.read_bytes())
            stdout = io.StringIO()
            stderr = io.StringIO()
            with redirect_stdout(stdout), redirect_stderr(stderr):
                result = render_p1.run(["functional-topology", "--check"])
        self.assertEqual(0, result)
        self.assertEqual(
            f"OK render functional-topology {relative.as_posix()}\n",
            stdout.getvalue(),
        )
        self.assertEqual("", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
