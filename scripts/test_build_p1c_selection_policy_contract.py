#!/usr/bin/env python3
"""Tests for reproducible P1C selection-policy contract derivation."""

from __future__ import annotations

import copy
import hashlib
import io
import json
import os
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

import build_p1c_selection_policy_contract as builder
from jsonschema_subset import validate


SCRIPT_ROOT = Path(__file__).resolve().parent
AIUS_ROOT = SCRIPT_ROOT.parent
REPO_ROOT = SCRIPT_ROOT.parents[3]
PRETTIER = REPO_ROOT / "node_modules" / ".bin" / "prettier"

SCHEMA = json.loads(
    (AIUS_ROOT / "evidence/design/selection-policy-schema.json").read_text(
        encoding="utf-8"
    )
)


def _load(relative: Path) -> object:
    return json.loads((AIUS_ROOT / relative).read_text(encoding="utf-8"))


def _family(document: object, family: str) -> dict[str, object]:
    assert isinstance(document, dict)
    rows = document["families"]
    assert isinstance(rows, list)
    return next(row for row in rows if row["family"] == family)


class PureBuilderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.base = _load(builder.SELECTION_BASE)

    def test_immutable_base_bytes_have_the_locked_hash(self) -> None:
        actual = hashlib.sha256(
            (AIUS_ROOT / builder.SELECTION_BASE).read_bytes()
        ).hexdigest()
        self.assertEqual(builder.SELECTION_HASH, actual)

    def test_base_has_exact_counts_orders_and_minimal_family_shape(self) -> None:
        self.assertEqual(builder.ROOT_KEYS, tuple(self.base))
        self.assertEqual("1.0.0", self.base["schema_version"])
        self.assertEqual(8, len(self.base["engineering_policies"]))
        self.assertEqual(32, len(self.base["requirements"]))
        self.assertEqual(29, len(self.base["families"]))
        self.assertEqual(
            builder.LOCKED_POLICY_IDS,
            tuple(row["id"] for row in self.base["engineering_policies"]),
        )
        self.assertEqual(
            builder.LOCKED_REQUIREMENT_IDS,
            tuple(row["id"] for row in self.base["requirements"]),
        )
        self.assertEqual(
            builder.LOCKED_FAMILY_IDS,
            tuple(row["family"] for row in self.base["families"]),
        )
        for row in self.base["engineering_policies"]:
            self.assertEqual(builder.POLICY_KEYS, tuple(row))
            self.assertEqual("", row["notes"])
        for row in self.base["requirements"]:
            self.assertEqual(builder.REQUIREMENT_KEYS, tuple(row))
            self.assertEqual("", row["notes"])
        for row in self.base["families"]:
            self.assertEqual(builder.MINIMAL_FAMILY_KEYS, tuple(row))
            self.assertEqual(sorted(row["requirements"]), row["requirements"])
            self.assertEqual("", row["notes"])

    def test_builder_is_repeatable_immutable_schema_valid_and_matches_contract(self) -> None:
        base = copy.deepcopy(self.base)
        before = copy.deepcopy(base)

        first = builder.build_selection_contract(base)
        second = builder.build_selection_contract(base)

        self.assertEqual(first, second)
        self.assertEqual(before, base)
        validate(first, SCHEMA)
        self.assertEqual(_load(builder.SELECTION_CONTRACT), first)
        self.assertEqual(builder.ROOT_KEYS, tuple(first))
        for row in first["families"]:
            self.assertEqual(builder.DERIVED_FAMILY_KEYS, tuple(row))

    def test_usb_c_receptacle_acceptance_example_is_exact(self) -> None:
        document = builder.build_selection_contract(self.base)
        self.assertEqual(
            {
                "family": "usb-c-receptacle",
                "component_file": "components/usb-c-receptacle.json",
                "critical": True,
                "generic_eligible": False,
                "allowed_statuses": [
                    "candidate-pending-evidence",
                    "selected",
                    "candidate-evidence-unavailable",
                ],
                "requirements": [
                    "REQT-COMMON-ENCLOSURE-CLEARANCE",
                    "REQT-COMMON-MANUFACTURABILITY",
                    "REQT-COMMON-MANUFACTURER-EVIDENCE",
                    "REQT-COMMON-UK-AVAILABILITY",
                    "REQT-USB-C-RECEPTACLE",
                ],
                "manufacturer_request_id": "REQ-USB-C-RECEPTACLE-MFR",
                "availability_request_id": "REQ-USB-C-RECEPTACLE-UK",
                "manufacturer_source_id": "AIUS-USB-C-RECEPTACLE-MFR",
                "availability_source_id": "AIUS-USB-C-RECEPTACLE-UK",
                "dependent_feature": "rear-usb-device-entry",
                "notes": "",
            },
            _family(document, "usb-c-receptacle"),
        )

    def test_logic_decoupling_acceptance_example_is_exact(self) -> None:
        document = builder.build_selection_contract(self.base)
        self.assertEqual(
            {
                "family": "logic-decoupling-capacitor",
                "component_file": "components/logic-decoupling-capacitor.json",
                "critical": False,
                "generic_eligible": True,
                "allowed_statuses": ["generic-allowance"],
                "requirements": ["REQT-LOGIC-DECOUPLING"],
                "manufacturer_request_id": None,
                "availability_request_id": None,
                "manufacturer_source_id": None,
                "availability_source_id": None,
                "dependent_feature": "local-logic-decoupling",
                "notes": "",
            },
            _family(document, "logic-decoupling-capacitor"),
        )

    def test_every_group_derivation_and_identifier_is_exact(self) -> None:
        document = builder.build_selection_contract(self.base)
        groups = (
            (
                builder.CRITICAL_FAMILIES,
                True,
                False,
                builder.CRITICAL_STATUSES,
            ),
            (
                builder.NONCRITICAL_FAMILIES,
                False,
                False,
                builder.NONCRITICAL_STATUSES,
            ),
            (builder.GENERIC_FAMILIES, False, True, builder.GENERIC_STATUSES),
        )
        seen: set[str] = set()
        for families, critical, generic, statuses in groups:
            for family in families:
                seen.add(family)
                row = _family(document, family)
                upper = family.upper()
                with self.subTest(family=family):
                    self.assertEqual(critical, row["critical"])
                    self.assertEqual(generic, row["generic_eligible"])
                    self.assertEqual(list(statuses), row["allowed_statuses"])
                    self.assertEqual(f"components/{family}.json", row["component_file"])
                    if generic:
                        self.assertEqual(
                            [None, None, None, None],
                            [
                                row["manufacturer_request_id"],
                                row["availability_request_id"],
                                row["manufacturer_source_id"],
                                row["availability_source_id"],
                            ],
                        )
                    else:
                        self.assertEqual(
                            f"REQ-{upper}-MFR", row["manufacturer_request_id"]
                        )
                        self.assertEqual(
                            f"REQ-{upper}-UK", row["availability_request_id"]
                        )
                        self.assertEqual(
                            f"AIUS-{upper}-UK", row["availability_source_id"]
                        )
                        expected_source = {
                            "cliff-rx": "CLIFF-ORJ5",
                            "cliff-tx": "CLIFF-OTJ5",
                        }.get(family, f"AIUS-{upper}-MFR")
                        self.assertEqual(expected_source, row["manufacturer_source_id"])
        self.assertEqual(set(builder.LOCKED_FAMILY_IDS), seen)

    def test_wrong_root_registry_row_and_family_order_are_rejected_immutably(self) -> None:
        mutations = []

        wrong_root = copy.deepcopy(self.base)
        wrong_root["extra"] = None
        mutations.append(("root", wrong_root, "root keys"))

        reordered_root = {
            key: copy.deepcopy(self.base[key])
            for key in reversed(builder.ROOT_KEYS)
        }
        mutations.append(("root-order", reordered_root, "root keys"))

        reordered_policy = copy.deepcopy(self.base)
        reordered_policy["engineering_policies"][0], reordered_policy[
            "engineering_policies"
        ][1] = (
            reordered_policy["engineering_policies"][1],
            reordered_policy["engineering_policies"][0],
        )
        mutations.append(("policy-order", reordered_policy, "policy ID order"))

        wrong_policy_member = copy.deepcopy(self.base)
        wrong_policy_member["engineering_policies"][0]["id"] = "EP-UNKNOWN"
        mutations.append(
            ("policy-membership", wrong_policy_member, "policy ID order")
        )

        reordered_requirement = copy.deepcopy(self.base)
        reordered_requirement["requirements"][0], reordered_requirement[
            "requirements"
        ][1] = (
            reordered_requirement["requirements"][1],
            reordered_requirement["requirements"][0],
        )
        mutations.append(
            ("requirement-order", reordered_requirement, "requirement ID order")
        )

        wrong_requirement_member = copy.deepcopy(self.base)
        wrong_requirement_member["requirements"][0]["id"] = "REQT-UNKNOWN"
        mutations.append(
            (
                "requirement-membership",
                wrong_requirement_member,
                "requirement ID order",
            )
        )

        reordered_family = copy.deepcopy(self.base)
        reordered_family["families"][0], reordered_family["families"][1] = (
            reordered_family["families"][1],
            reordered_family["families"][0],
        )
        mutations.append(("family-order", reordered_family, "family ID order"))

        for collection, keys, label in (
            ("engineering_policies", builder.POLICY_KEYS, "has unexpected keys"),
            ("requirements", builder.REQUIREMENT_KEYS, "has unexpected keys"),
            ("families", builder.MINIMAL_FAMILY_KEYS, "has unexpected keys"),
        ):
            wrong_keys = copy.deepcopy(self.base)
            row = wrong_keys[collection][0]
            row["unexpected"] = row.pop(keys[-1])
            mutations.append((f"{collection}-keys", wrong_keys, label))

        missing_family = copy.deepcopy(self.base)
        missing_family["families"].pop()
        mutations.append(
            ("family-membership", missing_family, "family ID order")
        )

        for label, base, message in mutations:
            before = copy.deepcopy(base)
            with self.subTest(label=label):
                with self.assertRaisesRegex(ValueError, message):
                    builder.build_selection_contract(base)
                self.assertEqual(before, base)

        with self.assertRaisesRegex(ValueError, "base must be an object"):
            builder.build_selection_contract([])


class CliTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "aius"
        self.base = _load(builder.SELECTION_BASE)
        target = self.root / builder.SELECTION_BASE
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((AIUS_ROOT / builder.SELECTION_BASE).read_bytes())

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _arguments(self, mode: str, *, prettier: Path = PRETTIER) -> list[str]:
        return [
            "--root",
            os.fspath(self.root),
            "--prettier",
            os.fspath(prettier),
            mode,
        ]

    def _run(
        self,
        mode: str,
        *,
        prettier: Path = PRETTIER,
        **kwargs,
    ) -> tuple[int, str, str]:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            result = builder.run(
                self._arguments(mode, prettier=prettier), **kwargs
            )
        return result, stdout.getvalue(), stderr.getvalue()

    def test_write_creates_output_parent_then_check_is_exact_and_nonmutating(self) -> None:
        target = self.root / builder.SELECTION_CONTRACT
        self.assertFalse(target.parent.exists())

        result = self._run("--write")
        self.assertEqual(
            (0, "OK build-p1c-selection-policy-contract\n", ""), result
        )
        self.assertEqual(
            (AIUS_ROOT / builder.SELECTION_CONTRACT).read_bytes(),
            target.read_bytes(),
        )
        before = target.stat()

        result = self._run("--check")
        self.assertEqual(
            (0, "OK build-p1c-selection-policy-contract\n", ""), result
        )
        after = target.stat()
        self.assertEqual(before.st_mtime_ns, after.st_mtime_ns)
        self.assertEqual(before.st_ino, after.st_ino)

    def test_check_reports_missing_and_stale_output(self) -> None:
        result, stdout, stderr = self._run("--check")
        self.assertEqual((1, ""), (result, stdout))
        self.assertEqual(
            [
                "AIUS-P1C-CONTRACT-MISSING: "
                "evidence/design/contracts/selection-policy.json: "
                "missing generated output"
            ],
            stderr.splitlines(),
        )

        self._run("--write")
        (self.root / builder.SELECTION_CONTRACT).write_bytes(b"stale\n")
        result, stdout, stderr = self._run("--check")
        self.assertEqual((1, ""), (result, stdout))
        self.assertIn("AIUS-P1C-CONTRACT-STALE", stderr)

    def test_missing_malformed_and_hash_mismatched_base_are_focused(self) -> None:
        path = self.root / builder.SELECTION_BASE
        original = path.read_bytes()
        path.write_bytes(original + b"\n")
        result, stdout, stderr = self._run("--write")
        self.assertEqual((1, ""), (result, stdout))
        self.assertIn("AIUS-P1C-CONTRACT-HASH", stderr)
        self.assertNotIn("AIUS-P1C-CONTRACT-PARSE", stderr)

        path.write_text('{"broken":', encoding="utf-8")
        result, stdout, stderr = self._run("--write")
        self.assertEqual((1, ""), (result, stdout))
        lines = stderr.splitlines()
        self.assertEqual(sorted(lines), lines)
        self.assertTrue(any("-HASH:" in line for line in lines))
        self.assertTrue(any("-PARSE:" in line for line in lines))
        self.assertFalse((self.root / builder.SELECTION_CONTRACT).exists())

        path.write_bytes(b"\xff")
        result, stdout, stderr = self._run("--write")
        self.assertEqual((1, ""), (result, stdout))
        self.assertIn("invalid UTF-8", stderr)

        path.unlink()
        result, stdout, stderr = self._run("--write")
        self.assertEqual((1, ""), (result, stdout))
        self.assertIn("AIUS-P1C-CONTRACT-MISSING", stderr)

    def test_formatter_invocation_and_drift_failures_are_exact(self) -> None:
        calls = []

        def recording_runner(command, **kwargs):
            calls.append((command, kwargs["input"]))
            return subprocess.run(command, **kwargs)

        result = self._run("--write", runner=recording_runner)
        self.assertEqual(
            (0, "OK build-p1c-selection-policy-contract\n", ""), result
        )
        self.assertEqual(1, len(calls))
        self.assertEqual([os.fspath(PRETTIER), "--parser", "json"], calls[0][0])

        reversed_document = builder.build_selection_contract(self.base)
        reversed_document = {
            key: reversed_document[key] for key in reversed(tuple(reversed_document))
        }
        drifts = (
            ("bom", b"\xef\xbb\xbf{}\n"),
            ("crlf", b"{}\r\n"),
            ("malformed", b"{\n"),
            ("extra-final-lf", b"{}\n\n"),
            ("changed-value", b"{}\n"),
            (
                "changed-order",
                (json.dumps(reversed_document) + "\n").encode("utf-8"),
            ),
            ("non-utf8", b"\xff"),
        )
        for label, payload in drifts:
            runner = mock.Mock(
                return_value=subprocess.CompletedProcess([], 0, payload, b"")
            )
            with self.subTest(label=label):
                result, stdout, stderr = self._run("--write", runner=runner)
                self.assertEqual((1, ""), (result, stdout))
                self.assertIn("AIUS-P1C-CONTRACT-FORMATTER", stderr)

    def test_formatter_missing_and_process_failure_are_focused(self) -> None:
        missing = self.root / "missing-prettier"
        result, stdout, stderr = self._run("--write", prettier=missing)
        self.assertEqual((1, ""), (result, stdout))
        self.assertIn("AIUS-P1C-CONTRACT-FORMATTER", stderr)
        self.assertIn(os.fspath(missing), stderr)

        runner = mock.Mock(
            return_value=subprocess.CompletedProcess([], 7, b"", b"failed")
        )
        result, stdout, stderr = self._run("--write", runner=runner)
        self.assertEqual((1, ""), (result, stdout))
        self.assertIn("exited 7: failed", stderr)

    def test_atomic_collision_and_replace_failure_preserve_foreign_bytes(self) -> None:
        target = self.root / builder.SELECTION_CONTRACT
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b"old output\n")
        token = "forced-collision"
        temporary = target.with_name(
            f".{target.name}.aius-p1c-{token}.tmp"
        )
        temporary.write_bytes(b"foreign temporary")

        with mock.patch.object(builder.secrets, "token_hex", return_value=token):
            result, stdout, stderr = self._run("--write")
        self.assertEqual((1, ""), (result, stdout))
        self.assertIn("AIUS-P1C-CONTRACT-WRITE", stderr)
        self.assertEqual(b"foreign temporary", temporary.read_bytes())
        self.assertEqual(b"old output\n", target.read_bytes())

        temporary.unlink()

        def fail_replace(source: str, destination: str) -> None:
            raise OSError("injected replacement failure")

        result, stdout, stderr = self._run("--write", replace=fail_replace)
        self.assertEqual((1, ""), (result, stdout))
        self.assertIn("injected replacement failure", stderr)
        self.assertEqual(b"old output\n", target.read_bytes())
        self.assertEqual(
            [], list(target.parent.glob(f".{target.name}.aius-p1c-*.tmp"))
        )

    def test_exactly_one_mode_is_required_by_argparse(self) -> None:
        for modes in ((), ("--write", "--check")):
            arguments = [
                "--root",
                os.fspath(self.root),
                "--prettier",
                os.fspath(PRETTIER),
                *modes,
            ]
            with self.subTest(modes=modes):
                with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as raised:
                        builder.run(arguments)
                self.assertEqual(2, raised.exception.code)

    def test_root_defaults_to_parent_of_scripts(self) -> None:
        self._run("--write")
        fake_script = self.root / "scripts/build_p1c_selection_policy_contract.py"
        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(builder, "__file__", fake_script):
            with redirect_stdout(stdout), redirect_stderr(stderr):
                result = builder.run(
                    ["--prettier", os.fspath(PRETTIER), "--check"]
                )
        self.assertEqual(0, result)
        self.assertEqual(
            "OK build-p1c-selection-policy-contract\n", stdout.getvalue()
        )
        self.assertEqual("", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
