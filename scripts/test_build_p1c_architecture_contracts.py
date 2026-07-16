#!/usr/bin/env python3
"""Tests for reproducible P1C architecture-contract derivation."""

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

import build_p1c_architecture_contracts as builder
from jsonschema_subset import validate


SCRIPT_ROOT = Path(__file__).resolve().parent
AIUS_ROOT = SCRIPT_ROOT.parent
REPO_ROOT = SCRIPT_ROOT.parents[3]
PRETTIER = REPO_ROOT / "node_modules" / ".bin" / "prettier"

FUNCTIONAL_SCHEMA = json.loads(
    (AIUS_ROOT / "evidence/design/functional-topology-schema.json").read_text(
        encoding="utf-8"
    )
)
INTERFACES_SCHEMA = json.loads(
    (AIUS_ROOT / "evidence/design/interfaces-and-states-schema.json").read_text(
        encoding="utf-8"
    )
)


def _load(relative: Path) -> object:
    return json.loads((AIUS_ROOT / relative).read_text(encoding="utf-8"))


FUNCTIONAL_BASE = _load(builder.FUNCTIONAL_BASE)
INTERFACES_BASE = _load(builder.INTERFACES_BASE)
FUNCTIONAL_GENERATED_HASH = (
    "b5e56ef6d6e9c67f7cbb0ed048a8be8d6b2ea89be553c7a5f72e9657ffb1a9a9"
)
OUTPUTS = (
    builder.FUNCTIONAL_CONTRACT,
    builder.INTERFACES_CONTRACT,
    builder.FUNCTIONAL_VALID,
    builder.INTERFACES_VALID,
    builder.FUNCTIONAL_MIRROR,
    builder.INTERFACES_MIRROR,
)


def _row_by_id(document: object, collection: str, row_id: str) -> dict[str, object]:
    assert isinstance(document, dict)
    rows = document[collection]
    assert isinstance(rows, list)
    return next(row for row in rows if row["id"] == row_id)


class PureBuilderTests(unittest.TestCase):
    def test_immutable_base_bytes_have_the_locked_hashes(self) -> None:
        for relative, expected in (
            (builder.FUNCTIONAL_BASE, builder.FUNCTIONAL_HASH),
            (builder.INTERFACES_BASE, builder.INTERFACES_HASH),
        ):
            with self.subTest(relative=relative):
                actual = hashlib.sha256((AIUS_ROOT / relative).read_bytes()).hexdigest()
                self.assertEqual(expected, actual)

    def test_pure_builders_are_repeatable_do_not_mutate_and_validate(self) -> None:
        functional = copy.deepcopy(FUNCTIONAL_BASE)
        interfaces = copy.deepcopy(INTERFACES_BASE)
        before = copy.deepcopy((functional, interfaces))

        first = (
            builder.build_functional_contract(functional),
            builder.build_interfaces_contract(interfaces),
        )
        second = (
            builder.build_functional_contract(functional),
            builder.build_interfaces_contract(interfaces),
        )

        self.assertEqual(first, second)
        self.assertEqual(before, (functional, interfaces))
        validate(first[0], FUNCTIONAL_SCHEMA)
        validate(first[1], INTERFACES_SCHEMA)
        self.assertEqual(_load(builder.FUNCTIONAL_CONTRACT), first[0])
        self.assertEqual(_load(builder.INTERFACES_CONTRACT), first[1])
        self.assertEqual(_load(builder.FUNCTIONAL_VALID), first[0])
        self.assertEqual(_load(builder.INTERFACES_VALID), first[1])
        self.assertEqual(_load(builder.FUNCTIONAL_MIRROR), first[0])
        self.assertEqual(_load(builder.INTERFACES_MIRROR), first[1])

    def test_functional_expansion_power_migration_is_exact(self) -> None:
        functional = builder.build_functional_contract(FUNCTIONAL_BASE)

        self.assertEqual(
            {
                "id": "expansion-power",
                "kind": "power",
                "source_id": "teensy",
                "sink_id": "expansion",
                "payload": "expansion-3v3",
                "enabled_modes": [],
                "control_id": None,
                "evidence_ids": ["PJRC-IMXRT1060-RM", "PJRC-TEENSY41-PINOUT"],
                "notes": "",
            },
            _row_by_id(functional, "flows", "expansion-power"),
        )

    def test_functional_migration_rejects_reordered_base_without_mutation(self) -> None:
        base = copy.deepcopy(FUNCTIONAL_BASE)
        base["flows"][0], base["flows"][1] = base["flows"][1], base["flows"][0]
        before = copy.deepcopy(base)

        with self.assertRaisesRegex(
            ValueError,
            "flows do not have the exact locked ID order",
        ):
            builder.build_functional_contract(base)
        self.assertEqual(before, base)

    def test_teensy_micro_b_replacement_is_exact(self) -> None:
        interfaces = builder.build_interfaces_contract(INTERFACES_BASE)

        self.assertEqual(
            list(builder.LOCKED_CONNECTOR_IDS),
            [row["id"] for row in interfaces["connectors"]],
        )
        self.assertEqual(
            {
                "id": "teensy-micro-b",
                "location": "module",
                "role": "power-data",
                "signals": ["USB2-D+", "USB2-D-", "VBUS"],
                "power_domains": ["protected-5v", "digital-ground"],
                "mating_constraint": (
                    "The internal USB cable is the sole Teensy VUSB feed; no VIN "
                    "or second VUSB feed is fitted."
                ),
                "evidence_ids": ["PJRC-TEENSY41-SCHEMATIC"],
                "notes": "Module USB power-and-data entry.",
            },
            _row_by_id(interfaces, "connectors", "teensy-micro-b"),
        )

    def test_protected_5v_consumers_are_exact_and_ordered(self) -> None:
        interfaces = builder.build_interfaces_contract(INTERFACES_BASE)

        self.assertEqual(
            ["teensy", "internal-usb-a", "teensy-micro-b"],
            _row_by_id(interfaces, "power_domains", "protected-5v")["consumers"],
        )

    def test_expansion_power_domains_are_exact_and_ordered(self) -> None:
        interfaces = builder.build_interfaces_contract(INTERFACES_BASE)

        self.assertEqual(
            [
                "optical-rx",
                "raw-bypass-mux",
                "tx-gate",
                "optical-tx",
                "ethernet",
                "expansion-power-protection",
            ],
            _row_by_id(interfaces, "power_domains", "teensy-3v3")["consumers"],
        )
        self.assertEqual(
            {
                "id": "expansion-3v3",
                "source": "expansion-power-protection",
                "consumers": ["expansion"],
                "backfeed_rule": (
                    "Reverse power flow is blocked when either side is unpowered."
                ),
                "evidence_ids": ["PJRC-IMXRT1060-RM", "PJRC-TEENSY41-PINOUT"],
                "notes": "Protected expansion logic rail.",
            },
            _row_by_id(interfaces, "power_domains", "expansion-3v3"),
        )

    def test_remote_rtc_connector_domains_and_evidence_are_exact(self) -> None:
        interfaces = builder.build_interfaces_contract(INTERFACES_BASE)

        self.assertEqual(
            {
                "id": "remote-program-rtc",
                "location": "rear",
                "role": "control",
                "signals": ["PROGRAM", "RTC-BATTERY"],
                "power_domains": [
                    "teensy-3v3",
                    "rtc-backup",
                    "digital-ground",
                ],
                "mating_constraint": (
                    "Service actuation does not require enclosure removal."
                ),
                "evidence_ids": [
                    "PJRC-TEENSY41-PINOUT",
                    "PJRC-TEENSY41-SCHEMATIC",
                ],
                "notes": "Remote program and RTC service interface.",
            },
            _row_by_id(interfaces, "connectors", "remote-program-rtc"),
        )

    def test_rtc_backup_domain_has_exact_position_and_value(self) -> None:
        interfaces = builder.build_interfaces_contract(INTERFACES_BASE)
        rtc_backup = _row_by_id(interfaces, "power_domains", "rtc-backup")

        self.assertEqual(
            [
                "usb-vbus-raw",
                "protected-5v",
                "teensy-3v3",
                "rtc-backup",
                "digital-ground",
                "chassis-shield",
                "expansion-3v3",
                "expansion-usb-host-vbus",
            ],
            [row["id"] for row in interfaces["power_domains"]],
        )
        self.assertEqual(
            (
                "id",
                "source",
                "consumers",
                "backfeed_rule",
                "evidence_ids",
                "notes",
            ),
            tuple(rtc_backup),
        )
        self.assertEqual(
            {
                "id": "rtc-backup",
                "source": "remote-program-rtc",
                "consumers": ["teensy"],
                "backfeed_rule": (
                    "The RTC backup input is isolated from teensy-3v3 and cannot "
                    "power the core."
                ),
                "evidence_ids": [
                    "PJRC-TEENSY41-PINOUT",
                    "PJRC-TEENSY41-SCHEMATIC",
                ],
                "notes": "External RTC backup battery domain.",
            },
            rtc_backup,
        )

    def test_interface_migration_rejects_reordered_base_without_mutation(self) -> None:
        for collection in ("connectors", "power_domains"):
            base = copy.deepcopy(INTERFACES_BASE)
            base[collection][0], base[collection][1] = (
                base[collection][1],
                base[collection][0],
            )
            before = copy.deepcopy(base)

            with self.subTest(collection=collection):
                with self.assertRaisesRegex(
                    ValueError,
                    rf"{collection} do not have the exact locked ID order",
                ):
                    builder.build_interfaces_contract(base)
                self.assertEqual(before, base)

    def test_generated_output_triplets_keep_locked_bytes(self) -> None:
        outputs = (
            AIUS_ROOT / builder.FUNCTIONAL_CONTRACT,
            AIUS_ROOT / builder.FUNCTIONAL_VALID,
            AIUS_ROOT / builder.FUNCTIONAL_MIRROR,
        )

        self.assertEqual(1, len({output.read_bytes() for output in outputs}))
        for output in outputs:
            with self.subTest(output=output):
                self.assertEqual(
                    FUNCTIONAL_GENERATED_HASH,
                    hashlib.sha256(output.read_bytes()).hexdigest(),
                )

        interface_outputs = (
            AIUS_ROOT / builder.INTERFACES_CONTRACT,
            AIUS_ROOT / builder.INTERFACES_VALID,
            AIUS_ROOT / builder.INTERFACES_MIRROR,
        )
        self.assertEqual(
            1,
            len({output.read_bytes() for output in interface_outputs}),
        )

    def test_every_evidence_assignment_is_exhaustive_and_exact(self) -> None:
        functional = builder.build_functional_contract(FUNCTIONAL_BASE)
        interfaces = builder.build_interfaces_contract(INTERFACES_BASE)
        for document, assignments in (
            (functional, builder.FUNCTIONAL_EVIDENCE),
            (interfaces, builder.INTERFACES_EVIDENCE),
        ):
            for collection, rows_to_bundles in assignments.items():
                actual = {row["id"]: row for row in document[collection]}
                self.assertEqual(set(rows_to_bundles), set(actual))
                for row_id, bundle in rows_to_bundles.items():
                    with self.subTest(collection=collection, row_id=row_id):
                        self.assertEqual(
                            list(builder.EVIDENCE_BUNDLES[bundle]),
                            actual[row_id]["evidence_ids"],
                        )
            self.assertEqual(
                list(builder.ROOT_EVIDENCE_IDS),
                document["evidence_ids"],
            )

    def test_no_unlisted_semantic_value_changes(self) -> None:
        functional = builder.build_functional_contract(FUNCTIONAL_BASE)
        normalized_functional = copy.deepcopy(functional)
        normalized_functional["evidence_ids"] = copy.deepcopy(
            FUNCTIONAL_BASE["evidence_ids"]
        )
        for collection in builder.FUNCTIONAL_EVIDENCE:
            base_by_id = {row["id"]: row for row in FUNCTIONAL_BASE[collection]}
            for row in normalized_functional[collection]:
                row["evidence_ids"] = copy.deepcopy(
                    base_by_id[row["id"]]["evidence_ids"]
                )
        _row_by_id(normalized_functional, "flows", "expansion-power")[
            "payload"
        ] = _row_by_id(FUNCTIONAL_BASE, "flows", "expansion-power")["payload"]
        self.assertEqual(FUNCTIONAL_BASE, normalized_functional)

        interfaces = builder.build_interfaces_contract(INTERFACES_BASE)
        normalized_interfaces = copy.deepcopy(interfaces)
        normalized_interfaces["evidence_ids"] = copy.deepcopy(
            INTERFACES_BASE["evidence_ids"]
        )
        for collection in builder.INTERFACES_EVIDENCE:
            base_by_id = {row["id"]: row for row in INTERFACES_BASE[collection]}
            for row in normalized_interfaces[collection]:
                if row["id"] in base_by_id:
                    row["evidence_ids"] = copy.deepcopy(
                        base_by_id[row["id"]]["evidence_ids"]
                    )
        normalized_interfaces["transitions"] = copy.deepcopy(
            INTERFACES_BASE["transitions"]
        )
        connectors_by_id = {
            row["id"]: row for row in normalized_interfaces["connectors"]
        }
        base_connectors_by_id = {
            row["id"]: row for row in INTERFACES_BASE["connectors"]
        }
        for row_id, fields in (
            (
                "teensy-micro-b",
                (
                    "role",
                    "signals",
                    "power_domains",
                    "mating_constraint",
                    "notes",
                ),
            ),
            ("remote-program-rtc", ("power_domains",)),
        ):
            for field in fields:
                connectors_by_id[row_id][field] = copy.deepcopy(
                    base_connectors_by_id[row_id][field]
                )
        power_domains_by_id = {
            row["id"]: row for row in normalized_interfaces["power_domains"]
        }
        power_domains_by_id["protected-5v"]["consumers"] = copy.deepcopy(
            _row_by_id(INTERFACES_BASE, "power_domains", "protected-5v")[
                "consumers"
            ]
        )
        power_domains_by_id["teensy-3v3"]["consumers"] = copy.deepcopy(
            _row_by_id(INTERFACES_BASE, "power_domains", "teensy-3v3")[
                "consumers"
            ]
        )
        power_domains_by_id["expansion-3v3"]["source"] = _row_by_id(
            INTERFACES_BASE,
            "power_domains",
            "expansion-3v3",
        )["source"]
        normalized_interfaces["power_domains"] = [
            row
            for row in normalized_interfaces["power_domains"]
            if row["id"] != "rtc-backup"
        ]
        self.assertEqual(INTERFACES_BASE, normalized_interfaces)

        for base_row, derived_row in zip(
            INTERFACES_BASE["transitions"], interfaces["transitions"]
        ):
            for field in ("id", "triggers", "preconditions", "notes"):
                self.assertEqual(base_row[field], derived_row[field])
            if base_row["id"] != "active-to-carrier-loss":
                self.assertEqual(base_row["actions"], derived_row["actions"])
                self.assertEqual(
                    base_row["postconditions"], derived_row["postconditions"]
                )

    def test_transition_migration_has_exact_keys_endpoints_and_overrides(self) -> None:
        interfaces = builder.build_interfaces_contract(INTERFACES_BASE)
        expected_keys = (
            "id",
            "from_states",
            "to_states",
            "triggers",
            "preconditions",
            "actions",
            "postconditions",
            "notes",
        )
        for transition in interfaces["transitions"]:
            transition_id = transition["id"]
            from_states, to_states = builder.TRANSITION_ENDPOINTS[transition_id]
            with self.subTest(transition=transition_id):
                self.assertEqual(expected_keys, tuple(transition))
                self.assertEqual(from_states, tuple(transition["from_states"]))
                self.assertEqual(to_states, tuple(transition["to_states"]))
                self.assertNotIn("from_state", transition)
                self.assertNotIn("to_state", transition)

        carrier_loss = next(
            row
            for row in interfaces["transitions"]
            if row["id"] == "active-to-carrier-loss"
        )
        self.assertEqual(
            ["disable-tx", "wait-muted", "select-processed", "clear-pipeline"],
            carrier_loss["actions"],
        )
        self.assertEqual(
            ["processed-selected", "tx-disabled"],
            carrier_loss["postconditions"],
        )


class CliTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "aius"
        for relative in (builder.FUNCTIONAL_BASE, builder.INTERFACES_BASE):
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((AIUS_ROOT / relative).read_bytes())

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

    def _run(self, mode: str, **kwargs) -> tuple[int, str, str]:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            result = builder.run(self._arguments(mode), **kwargs)
        return result, stdout.getvalue(), stderr.getvalue()

    def test_write_and_check_have_exact_behavior_and_bytes(self) -> None:
        result, stdout, stderr = self._run("--write")
        self.assertEqual(0, result)
        self.assertEqual("OK build-p1c-architecture-contracts\n", stdout)
        self.assertEqual("", stderr)

        for relative in OUTPUTS:
            self.assertEqual((AIUS_ROOT / relative).read_bytes(), (self.root / relative).read_bytes())

        before = {
            relative: (self.root / relative).stat()
            for relative in OUTPUTS
        }
        result, stdout, stderr = self._run("--check")
        self.assertEqual((0, "OK build-p1c-architecture-contracts\n", ""), (result, stdout, stderr))
        for relative, status in before.items():
            after = (self.root / relative).stat()
            self.assertEqual(status.st_mtime_ns, after.st_mtime_ns)
            self.assertEqual(status.st_ino, after.st_ino)

    def test_check_collects_and_sorts_every_missing_or_stale_output(self) -> None:
        result, stdout, stderr = self._run("--check")
        self.assertEqual((1, ""), (result, stdout))
        lines = stderr.splitlines()
        self.assertEqual(sorted(lines), lines)
        self.assertEqual(6, len(lines))
        self.assertTrue(all("AIUS-P1C-CONTRACT-MISSING" in line for line in lines))

        self._run("--write")
        (self.root / builder.FUNCTIONAL_CONTRACT).write_bytes(b"stale\n")
        (self.root / builder.INTERFACES_VALID).unlink()
        result, stdout, stderr = self._run("--check")
        self.assertEqual((1, ""), (result, stdout))
        lines = stderr.splitlines()
        self.assertEqual(sorted(lines), lines)
        self.assertEqual(2, len(lines))
        self.assertTrue(any("-STALE:" in line for line in lines))
        self.assertTrue(any("-MISSING:" in line for line in lines))

    def test_missing_parse_and_hash_failures_stop_before_outputs(self) -> None:
        functional_path = self.root / builder.FUNCTIONAL_BASE
        functional_path.write_text(
            functional_path.read_text(encoding="utf-8").replace(
                '"schema_version": "1.0.0"', '"schema_version": "1.0.1"'
            ),
            encoding="utf-8",
        )
        result, stdout, stderr = self._run("--write")
        self.assertEqual((1, ""), (result, stdout))
        self.assertIn("AIUS-P1C-CONTRACT-HASH", stderr)
        self.assertFalse((self.root / builder.FUNCTIONAL_CONTRACT).exists())

        functional_path.write_bytes((AIUS_ROOT / builder.FUNCTIONAL_BASE).read_bytes())
        interfaces_path = self.root / builder.INTERFACES_BASE
        interfaces_path.write_bytes(b"\xff")
        result, stdout, stderr = self._run("--write")
        self.assertEqual((1, ""), (result, stdout))
        lines = stderr.splitlines()
        self.assertEqual(sorted(lines), lines)
        self.assertTrue(any("-HASH:" in line for line in lines))
        self.assertTrue(any("-PARSE:" in line for line in lines))

        interfaces_path.write_bytes(b'{"broken":')
        result, stdout, stderr = self._run("--write")
        self.assertEqual((1, ""), (result, stdout))
        self.assertIn("AIUS-P1C-CONTRACT-PARSE", stderr)
        self.assertIn("malformed JSON", stderr)

        interfaces_path.unlink()
        result, stdout, stderr = self._run("--write")
        self.assertEqual((1, ""), (result, stdout))
        self.assertIn("AIUS-P1C-CONTRACT-MISSING", stderr)

    def test_formatter_invocation_and_pinned_bytes_are_exact(self) -> None:
        calls = []

        def recording_runner(command, **kwargs):
            calls.append((command, kwargs["input"]))
            return subprocess.run(command, **kwargs)

        result, stdout, stderr = self._run("--write", runner=recording_runner)
        self.assertEqual((0, "OK build-p1c-architecture-contracts\n", ""), (result, stdout, stderr))
        self.assertEqual(2, len(calls))
        for command, payload in calls:
            self.assertEqual([os.fspath(PRETTIER), "--parser", "json"], command)
            self.assertEqual(json.loads(payload), json.loads(subprocess.run(
                command,
                input=payload,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=True,
            ).stdout))

    def test_formatter_missing_failure_and_non_utf8_are_focused(self) -> None:
        missing = self.root / "missing-prettier"
        stdout = io.StringIO()
        stderr = io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            result = builder.run(self._arguments("--write", prettier=missing))
        self.assertEqual((1, ""), (result, stdout.getvalue()))
        self.assertIn("AIUS-P1C-CONTRACT-FORMATTER", stderr.getvalue())
        self.assertIn(os.fspath(missing), stderr.getvalue())

        for label, completed in (
            ("failure", subprocess.CompletedProcess([], 7, b"", b"failed")),
            ("non-utf8", subprocess.CompletedProcess([], 0, b"\xff", b"")),
        ):
            with self.subTest(label=label):
                runner = mock.Mock(return_value=completed)
                result, stdout_text, stderr_text = self._run(
                    "--write", runner=runner
                )
                self.assertEqual((1, ""), (result, stdout_text))
                self.assertIn("AIUS-P1C-CONTRACT-FORMATTER", stderr_text)
                self.assertIn(os.fspath(PRETTIER), stderr_text)

    def test_atomic_collision_and_replace_failure_preserve_foreign_bytes(self) -> None:
        target = self.root / builder.FUNCTIONAL_CONTRACT
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b"old output\n")
        token = "forced-collision"
        temporary = target.with_name(f".{target.name}.aius-p1c-{token}.tmp")
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
        self.assertEqual([], list(target.parent.glob(f".{target.name}.aius-p1c-*.tmp")))

    def test_each_replace_failure_keeps_earlier_outputs_and_skips_later_outputs(
        self,
    ) -> None:
        result, stdout, stderr = self._run("--write")
        self.assertEqual(
            (0, "OK build-p1c-architecture-contracts\n", ""),
            (result, stdout, stderr),
        )
        expected = {
            relative: (self.root / relative).read_bytes() for relative in OUTPUTS
        }

        for failed_index, failed in enumerate(OUTPUTS):
            old = {
                relative: f"old {failed_index} {relative.as_posix()}\n".encode()
                for relative in OUTPUTS
            }
            for relative, payload in old.items():
                (self.root / relative).write_bytes(payload)
            replaced = []

            def fail_replace(source: str, destination: str) -> None:
                relative = Path(destination).relative_to(self.root)
                replaced.append(relative)
                if relative == failed:
                    raise OSError("injected replacement failure")
                os.replace(source, destination)

            result, stdout, stderr = self._run(
                "--write",
                replace=fail_replace,
            )

            with self.subTest(failed=failed):
                self.assertEqual((1, ""), (result, stdout))
                self.assertEqual(list(OUTPUTS[: failed_index + 1]), replaced)
                self.assertEqual(
                    (
                        "AIUS-P1C-CONTRACT-WRITE: "
                        f"{failed.as_posix()}: injected replacement failure\n"
                    ),
                    stderr,
                )
                for relative in OUTPUTS[:failed_index]:
                    self.assertEqual(
                        expected[relative],
                        (self.root / relative).read_bytes(),
                    )
                for relative in OUTPUTS[failed_index:]:
                    self.assertEqual(
                        old[relative],
                        (self.root / relative).read_bytes(),
                    )
                failed_target = self.root / failed
                self.assertEqual(
                    [],
                    list(
                        failed_target.parent.glob(
                            f".{failed_target.name}.aius-p1c-*.tmp"
                        )
                    ),
                )

            result, stdout, stderr = self._run("--write")
            self.assertEqual(
                (0, "OK build-p1c-architecture-contracts\n", ""),
                (result, stdout, stderr),
            )
            for relative in OUTPUTS:
                self.assertEqual(
                    expected[relative],
                    (self.root / relative).read_bytes(),
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
        fake_script = self.root / "scripts" / "build_p1c_architecture_contracts.py"
        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(builder, "__file__", fake_script):
            with redirect_stdout(stdout), redirect_stderr(stderr):
                result = builder.run(
                    ["--prettier", os.fspath(PRETTIER), "--check"]
                )
        self.assertEqual(0, result)
        self.assertEqual("OK build-p1c-architecture-contracts\n", stdout.getvalue())
        self.assertEqual("", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
