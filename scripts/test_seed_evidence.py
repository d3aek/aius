#!/usr/bin/env python3
"""Isolated contract tests for reproducible Aius seed evidence orchestration."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import seed_evidence


SCRIPT_ROOT = Path(__file__).resolve().parent
AIUS_ROOT = SCRIPT_ROOT.parent
MANIFEST_ROOT = AIUS_ROOT / "evidence" / "manifest"
TIMESTAMP = "2026-07-16T12:34:56Z"


def _load(path: Path) -> object:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def _payload(media_type: str) -> bytes:
    return {
        "application/pdf": b"%PDF-1.7\nfixture\n",
        "application/zip": b"PK\x03\x04fixture",
        "image/png": b"\x89PNG\r\n\x1a\nfixture",
        "text/html": b"<!doctype html><html></html>\n",
        "text/x-rst": b"Fixture\n=======\n",
        "text/x-c++": b"int fixture() { return 0; }\n",
    }[media_type]


class Response:
    def __init__(
        self,
        status: int,
        url: str,
        payload: bytes = b"",
        headers: dict[str, str] | None = None,
    ) -> None:
        self.status = status
        self._url = url
        self._payload = payload
        self._offset = 0
        self.headers = {} if headers is None else headers

    def geturl(self) -> str:
        return self._url

    def getcode(self) -> int:
        return self.status

    def read(self, size: int = -1) -> bytes:
        if self._offset >= len(self._payload):
            return b""
        if size < 0:
            size = len(self._payload) - self._offset
        result = self._payload[self._offset : self._offset + size]
        self._offset += len(result)
        return result

    def close(self) -> None:
        return None


class ReadFailureResponse(Response):
    def __init__(self, status: int, url: str, error: Exception) -> None:
        super().__init__(status, url)
        self.error = error

    def read(self, size: int = -1) -> bytes:
        raise self.error


class SequenceOpener:
    def __init__(self, responses: list[Response]) -> None:
        self.responses = list(responses)
        self.requests: list[tuple[object, int]] = []

    def open(self, request: object, timeout: int) -> Response:
        self.requests.append((request, timeout))
        if not self.responses:
            raise AssertionError("fake opener has no response")
        return self.responses.pop(0)


class RealizationOpener:
    def __init__(self, realizations: list[dict[str, object]]) -> None:
        self.by_retrieval: dict[str, dict[str, object]] = {}
        self.by_final: dict[str, dict[str, object]] = {}
        for realization in realizations:
            artifacts = realization.get("artifacts")
            if not isinstance(artifacts, list):
                continue
            for artifact in artifacts:
                assert isinstance(artifact, dict)
                self.by_retrieval[str(artifact["retrieval_url"])] = artifact
                self.by_final[str(artifact["expected_final_url"])] = artifact
        self.requests: list[tuple[object, int]] = []
        self.payload_overrides: dict[str, bytes] = {}

    def open(self, request: object, timeout: int) -> Response:
        self.requests.append((request, timeout))
        url = request.full_url
        if url in self.by_retrieval:
            artifact = self.by_retrieval[url]
            final = str(artifact["expected_final_url"])
            if final != url:
                return Response(302, url, headers={"Location": final})
        artifact = self.by_final.get(url) or self.by_retrieval.get(url)
        if artifact is None:
            raise AssertionError(f"unexpected retrieval URL {url}")
        media_type = str(artifact["media_type"])
        payload = self.payload_overrides.get(url, _payload(media_type))
        return Response(200, url, payload, {"Content-Encoding": "identity"})


class Fixture:
    def __init__(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="aius-seed-evidence-")
        self.root = Path(self.temporary.name) / "aius"
        self.manifest = self.root / "evidence" / "manifest"
        self.manifest.mkdir(parents=True)
        for name in (
            "source-schema.json",
            "assumption-schema.json",
            "seed-sources.json",
            "seed-realization-schema.json",
            "seed-realization.json",
            "sources.json",
            "assumptions.json",
        ):
            shutil.copy2(MANIFEST_ROOT / name, self.manifest / name)
        (self.root / ".gitattributes").write_text(
            "evidence/artifacts/** filter=lfs diff=lfs merge=lfs -text\n",
            encoding="utf-8",
        )
        completed = subprocess.run(
            ["git", "init", "-q"],
            cwd=self.root,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if completed.returncode != 0:
            raise AssertionError(completed.stderr.decode())
        self.realizations = _load(self.manifest / "seed-realization.json")
        assert isinstance(self.realizations, list)
        self.opener = RealizationOpener(self.realizations)

    def close(self) -> None:
        self.temporary.cleanup()

    def run(
        self,
        arguments: list[str],
        *,
        opener: object | None = None,
        replace=os.replace,
        root: Path | None = None,
    ) -> tuple[int, str, str]:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = seed_evidence.run(
                ["--root", os.fspath(self.root if root is None else root), *arguments],
                opener=opener,
                replace=replace,
            )
        return result, stdout.getvalue(), stderr.getvalue()

    def realization(self, source_id: str) -> dict[str, object]:
        return next(
            row for row in self.realizations if row.get("source_id") == source_id
        )

    def fragment(self, source_id: str) -> dict[str, object]:
        value = _load(self.manifest / "fragments" / f"{source_id}.json")
        assert isinstance(value, dict)
        return value

    def valid_values(self, source_id: str) -> dict[str, object]:
        profile = self.realization(source_id)["observation_profile"]
        assert isinstance(profile, dict)
        fields = profile["fields"]
        assert isinstance(fields, list)
        result: dict[str, object] = {}
        for field in fields:
            assert isinstance(field, dict)
            key = str(field["key"])
            constraint = field["constraint"]
            assert isinstance(constraint, dict)
            kind = constraint["kind"]
            expected = constraint["value"]
            value_type = field["value_type"]
            if kind == "const":
                value = expected
            elif kind == "enum":
                assert isinstance(expected, list)
                value = expected[0]
            elif kind == "pattern":
                value = "2026-07-16"
            elif kind == "minimum":
                value = expected
            elif kind == "exclusive-minimum":
                assert isinstance(expected, (int, float))
                value = expected + 1
            elif value_type == "boolean":
                value = False
            else:
                value = "fixture"
            result[key] = value
        return result

    def populate(self, *, skip: set[str] | None = None) -> None:
        skipped = set() if skip is None else skip
        for realization in self.realizations:
            source_id = str(realization["source_id"])
            if source_id in skipped:
                continue
            if realization["mode"] == "dynamic":
                values = json.dumps(self.valid_values(source_id), allow_nan=False)
                result, _, error = self.run(
                    [
                        "record",
                        source_id,
                        "--values",
                        values,
                        "--observed-at",
                        TIMESTAMP,
                    ]
                )
            else:
                result, _, error = self.run(
                    ["acquire", source_id, "--retrieved-at", TIMESTAMP],
                    opener=self.opener,
                )
            if result != 0:
                raise AssertionError(f"populate {source_id} failed: {error}")


class SeedEvidenceTests(unittest.TestCase):
    fixture: Fixture

    def setUp(self) -> None:
        self.fixture = Fixture()

    def tearDown(self) -> None:
        self.fixture.close()

    def reset_fixture(self) -> None:
        self.fixture.close()
        self.fixture = Fixture()

    def assert_failure(
        self,
        result: tuple[int, str, str],
        text: str,
        *,
        code: int = 1,
    ) -> None:
        status, stdout, stderr = result
        self.assertEqual(status, code, stderr)
        self.assertEqual(stdout, "")
        self.assertIn(text, stderr)
        self.assertTrue(all(line.startswith("ERROR:") for line in stderr.splitlines()))

    def test_acquire_renders_fixed_hash_commit_and_manual_redirect_sources(self) -> None:
        for source_id in (
            "CLIFF-ORJ5",
            "PJRC-TEENSY41-PAGE",
            "PLATFORMIO-TEENSY41",
            "VKSDR-TOSLINK-SCHEMATICS",
        ):
            result = self.fixture.run(
                ["acquire", source_id, "--retrieved-at", TIMESTAMP],
                opener=self.fixture.opener,
            )
            self.assertEqual(result, (0, "", ""), result[2])
        self.assertEqual(self.fixture.fragment("CLIFF-ORJ5")["revision"], "2.0")
        page = self.fixture.fragment("PJRC-TEENSY41-PAGE")
        expected_hash = hashlib.sha256(_payload("text/html")).hexdigest()
        self.assertEqual(page["revision"], f"sha256:{expected_hash}")
        self.assertEqual(
            self.fixture.fragment("PLATFORMIO-TEENSY41")["revision"],
            "commit:54a797fb5440a069d4790d495499552e5a7edb35",
        )
        self.assertTrue(
            any(
                request.full_url.startswith("https://storage.ghost.io/")
                for request, _ in self.fixture.opener.requests
            )
        )
        for request, timeout in self.fixture.opener.requests:
            self.assertEqual(timeout, 30)
            self.assertEqual(request.get_header("User-agent"), "Aius-evidence/1.0")
            self.assertEqual(request.get_header("Accept-encoding"), "identity")

    def test_acquire_refuses_bad_payload_and_existing_targets(self) -> None:
        source_id = "CLIFF-ORJ5"
        entry = self.fixture.realization(source_id)["artifacts"][0]
        assert isinstance(entry, dict)
        self.fixture.opener.payload_overrides[str(entry["expected_final_url"])] = b"bad"
        result = self.fixture.run(
            ["acquire", source_id, "--retrieved-at", TIMESTAMP],
            opener=self.fixture.opener,
        )
        self.assert_failure(result, "ARTIFACT_SIGNATURE")
        self.assertFalse((self.fixture.manifest / "fragments" / f"{source_id}.json").exists())

        target = self.fixture.root / str(entry["path"])
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(_payload("application/pdf"))
        result = self.fixture.run(
            ["acquire", source_id, "--retrieved-at", TIMESTAMP],
            opener=self.fixture.opener,
        )
        self.assert_failure(result, "refusing to overwrite")

    def test_acquire_rejects_target_and_ancestor_symlinks(self) -> None:
        entry = self.fixture.realization("CLIFF-ORJ5")["artifacts"][0]
        assert isinstance(entry, dict)
        target = self.fixture.root / str(entry["path"])
        target.parent.mkdir(parents=True)
        outside = Path(self.fixture.temporary.name) / "outside.pdf"
        outside.write_bytes(_payload("application/pdf"))
        target.symlink_to(outside)
        result = self.fixture.run(
            ["acquire", "CLIFF-ORJ5", "--retrieved-at", TIMESTAMP],
            opener=self.fixture.opener,
        )
        self.assert_failure(result, "refusing to overwrite")

        self.reset_fixture()
        entry = self.fixture.realization("CLIFF-ORJ5")["artifacts"][0]
        assert isinstance(entry, dict)
        parent = (self.fixture.root / str(entry["path"])).parent
        parent.parent.mkdir(parents=True)
        outside_directory = Path(self.fixture.temporary.name) / "outside-directory"
        outside_directory.mkdir()
        parent.symlink_to(outside_directory, target_is_directory=True)
        result = self.fixture.run(
            ["acquire", "CLIFF-ORJ5", "--retrieved-at", TIMESTAMP],
            opener=self.fixture.opener,
        )
        self.assert_failure(result, "directory path is unsafe")

    def test_record_all_profiles_uses_descriptor_order_and_metadata(self) -> None:
        dynamic = [row for row in self.fixture.realizations if row["mode"] == "dynamic"]
        for row in dynamic:
            source_id = str(row["source_id"])
            result = self.fixture.run(
                [
                    "record",
                    source_id,
                    "--values",
                    json.dumps(self.fixture.valid_values(source_id)),
                    "--observed-at",
                    TIMESTAMP,
                ]
            )
            self.assertEqual(result, (0, "", ""), result[2])
            source = self.fixture.fragment(source_id)
            profile = row["observation_profile"]
            assert isinstance(profile, dict)
            fields = profile["fields"]
            assert isinstance(fields, list)
            observations = source["observations"]
            self.assertEqual(
                [item["key"] for item in observations],
                [item["key"] for item in fields],
            )
            for observation, field in zip(observations, fields):
                self.assertEqual(observation["unit"], field["unit"])
                self.assertEqual(observation["basis"], field["basis"])
                self.assertEqual(observation["observed_at"], TIMESTAMP)
                seed = next(item for item in _load(self.manifest_seed_path()) if item["id"] == source_id)
                self.assertEqual(observation["source_url"], seed["landing_url"])
            self.assertEqual(source["retrieved_at"], TIMESTAMP)
            self.assertEqual(source["revision"], "observed:2026-07-16")

    def manifest_seed_path(self) -> Path:
        return self.fixture.manifest / "seed-sources.json"

    def test_record_rejects_constants_types_keys_pattern_and_nonfinite_json(self) -> None:
        valid = self.fixture.valid_values("BOE-GBP-DAILY-FX")
        cases = (
            ("NaN", "invalid JSON constant"),
            (json.dumps({**valid, "extra": 1}), "observation keys differ"),
            (json.dumps({**valid, "gbp_per_usd": True}), "must be number"),
            (json.dumps({**valid, "rate_date": "2026-99-99"}), "does not match"),
        )
        for index, (values, expected) in enumerate(cases):
            if index:
                self.reset_fixture()
            result = self.fixture.run(
                [
                    "record",
                    "BOE-GBP-DAILY-FX",
                    "--values",
                    values,
                    "--observed-at",
                    TIMESTAMP,
                ]
            )
            self.assert_failure(result, expected)

    def test_record_has_no_profile_branch_and_invalid_descriptor_regex_is_control_error(self) -> None:
        path = self.fixture.manifest / "seed-realization.json"
        realizations = _load(path)
        assert isinstance(realizations, list)
        boe = next(row for row in realizations if row["source_id"] == "BOE-GBP-DAILY-FX")
        field = boe["observation_profile"]["fields"][0]
        field["constraint"] = {"kind": "const", "value": "fixture-date"}
        path.write_text(json.dumps(realizations, indent=2) + "\n", encoding="utf-8")
        values = self.fixture.valid_values("BOE-GBP-DAILY-FX")
        values["rate_date"] = "fixture-date"
        result = self.fixture.run(
            [
                "record",
                "BOE-GBP-DAILY-FX",
                "--values",
                json.dumps(values),
                "--observed-at",
                TIMESTAMP,
            ]
        )
        self.assertEqual(result, (0, "", ""), result[2])

        self.reset_fixture()
        path = self.fixture.manifest / "seed-realization.json"
        realizations = _load(path)
        assert isinstance(realizations, list)
        boe = next(row for row in realizations if row["source_id"] == "BOE-GBP-DAILY-FX")
        boe["observation_profile"]["fields"][0]["constraint"]["value"] = "^($"
        path.write_text(json.dumps(realizations, indent=2) + "\n", encoding="utf-8")
        result = self.fixture.run(
            [
                "record",
                "BOE-GBP-DAILY-FX",
                "--values",
                json.dumps(self.fixture.valid_values("BOE-GBP-DAILY-FX")),
                "--observed-at",
                TIMESTAMP,
            ]
        )
        self.assert_failure(result, "descriptor pattern", code=2)

    def test_unavailable_writes_exact_golden_and_dynamic_results(self) -> None:
        golden = self.fixture.run(
            [
                "unavailable",
                "CLIFF-ORJ5",
                "--note",
                "Network unavailable.",
                "--retrieved-at",
                TIMESTAMP,
            ]
        )
        self.assert_failure(golden, "recorded unavailable", code=3)
        source = self.fixture.fragment("CLIFF-ORJ5")
        self.assertEqual(source["revision"], "unavailable:2026-07-16")
        self.assertFalse((self.fixture.manifest / "assumption-fragments").exists())
        checked = self.fixture.run(["check-fragment", "CLIFF-ORJ5"])
        self.assertEqual(checked, (0, "", ""), checked[2])

        dynamic = self.fixture.run(
            [
                "unavailable",
                "BOE-GBP-DAILY-FX",
                "--note",
                "Page unavailable.",
                "--retrieved-at",
                TIMESTAMP,
            ]
        )
        self.assert_failure(dynamic, "recorded unavailable")
        assumption = _load(
            self.fixture.manifest
            / "assumption-fragments"
            / "BOE-GBP-DAILY-FX.json"
        )
        self.assertEqual(assumption["id"], "ASM-UNAVAILABLE-BOE-GBP-DAILY-FX")
        self.assertEqual(assumption["owner"], "P1B")
        checked = self.fixture.run(["check-fragment", "BOE-GBP-DAILY-FX"])
        self.assertEqual(checked, (0, "", ""), checked[2])

    def test_unavailable_comparative_writes_exact_assumption(self) -> None:
        source_id = "VKSDR-TOSLINK-SCHEMATICS"
        result = self.fixture.run(
            [
                "unavailable",
                source_id,
                "--note",
                "Comparative source unavailable.",
                "--retrieved-at",
                TIMESTAMP,
            ]
        )
        self.assert_failure(result, "recorded unavailable")
        source = self.fixture.fragment(source_id)
        self.assertEqual(source["class"], "comparative")
        assumption = _load(
            self.fixture.manifest / "assumption-fragments" / f"{source_id}.json"
        )
        self.assertEqual(assumption["id"], f"ASM-UNAVAILABLE-{source_id}")
        self.assertEqual(assumption["source_ids"], [source_id])
        self.assertEqual(assumption["claim_scope"], source["claim_scope"])
        checked = self.fixture.run(["check-fragment", source_id])
        self.assertEqual(checked, (0, "", ""), checked[2])

    def test_unavailable_rejects_blank_note_and_golden_blocks_assembly(self) -> None:
        result = self.fixture.run(
            [
                "unavailable",
                "CLIFF-ORJ5",
                "--note",
                "  ",
                "--retrieved-at",
                TIMESTAMP,
            ]
        )
        self.assert_failure(result, "non-whitespace")
        result = self.fixture.run(
            [
                "unavailable",
                "CLIFF-ORJ5",
                "--note",
                "Unavailable.",
                "--retrieved-at",
                TIMESTAMP,
            ]
        )
        self.assertEqual(result[0], 3)
        self.fixture.populate(skip={"CLIFF-ORJ5"})
        self.assertEqual(
            len(list((self.fixture.manifest / "fragments").glob("*.json"))),
            22,
        )
        result = self.fixture.run(["assemble", "--write"])
        self.assert_failure(result, "golden unavailable fragments block assembly")

    def test_timestamp_validation_and_command_mode_errors(self) -> None:
        for value in ("2026-02-30T12:00:00Z", "2026-07-16T12:00:00+00:00"):
            result = self.fixture.run(
                [
                    "unavailable",
                    "CLIFF-ORJ5",
                    "--note",
                    "Unavailable.",
                    "--retrieved-at",
                    value,
                ]
            )
            self.assert_failure(result, "timestamp")
        result = self.fixture.run(
            [
                "record",
                "CLIFF-ORJ5",
                "--values",
                "{}",
                "--observed-at",
                TIMESTAMP,
            ]
        )
        self.assert_failure(result, "must use acquire")

    def test_explicit_root_symlink_is_rejected_before_resolution(self) -> None:
        root_link = Path(self.fixture.temporary.name) / "aius-root-link"
        root_link.symlink_to(self.fixture.root, target_is_directory=True)
        result = self.fixture.run(
            ["check-fragment", "CLIFF-ORJ5"],
            root=root_link,
        )
        self.assert_failure(result, "Aius root may not be a symlink", code=2)

    def test_retrieval_enforces_https_redirect_status_encoding_size_and_final_url(self) -> None:
        cases: list[tuple[str, str, SequenceOpener, str]] = [
            (
                "http://example.test/a",
                "https://example.test/a",
                SequenceOpener([]),
                "must be HTTPS",
            ),
            (
                "https://example.test/a",
                "https://example.test/a",
                SequenceOpener([Response(500, "https://example.test/a")]),
                "terminal HTTP status",
            ),
            (
                "https://example.test/a",
                "https://example.test/a",
                SequenceOpener(
                    [Response(302, "https://example.test/a", headers={"Location": "http://example.test/b"})]
                ),
                "redirect target must be HTTPS",
            ),
            (
                "https://example.test/a",
                "https://example.test/a",
                SequenceOpener(
                    [Response(200, "https://example.test/a", b"x", {"Content-Encoding": "gzip"})]
                ),
                "Content-Encoding",
            ),
            (
                "https://example.test/a",
                "https://example.test/a",
                SequenceOpener(
                    [Response(200, "https://example.test/a", b"x", {"Content-Length": str(seed_evidence.MAX_PAYLOAD_BYTES + 1)})]
                ),
                "payload exceeds",
            ),
            (
                "https://example.test/a",
                "https://example.test/expected",
                SequenceOpener([Response(200, "https://example.test/actual", b"x")]),
                "final URL differs",
            ),
        ]
        for url, final, opener, expected in cases:
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(seed_evidence.DataError, expected):
                    seed_evidence._retrieve(url, final, opener)

        redirects = [
            Response(
                302,
                f"https://example.test/{index}",
                headers={"Location": f"https://example.test/{index + 1}"},
            )
            for index in range(6)
        ]
        with self.assertRaisesRegex(seed_evidence.DataError, "redirect limit"):
            seed_evidence._retrieve(
                "https://example.test/0",
                "https://example.test/6",
                SequenceOpener(redirects),
            )

        for status in sorted(seed_evidence.REDIRECT_STATUSES):
            with self.subTest(redirect_status=status):
                payload = seed_evidence._retrieve(
                    "https://example.test/a",
                    "https://example.test/b",
                    SequenceOpener(
                        [
                            Response(
                                status,
                                "https://example.test/a",
                                headers={"Location": "/b"},
                            ),
                            Response(200, "https://example.test/b", b"fixture"),
                        ]
                    ),
                )
                self.assertEqual(payload, b"fixture")

        old_limit = seed_evidence.MAX_PAYLOAD_BYTES
        seed_evidence.MAX_PAYLOAD_BYTES = 8
        try:
            with self.assertRaisesRegex(seed_evidence.DataError, "payload exceeds"):
                seed_evidence._retrieve(
                    "https://example.test/a",
                    "https://example.test/a",
                    SequenceOpener([Response(200, "https://example.test/a", b"123456789")]),
                )
        finally:
            seed_evidence.MAX_PAYLOAD_BYTES = old_limit

    def test_streamed_read_failures_are_acquisition_exit_one(self) -> None:
        realization = self.fixture.realization("CLIFF-ORJ5")
        artifact = realization["artifacts"][0]
        assert isinstance(artifact, dict)
        url = str(artifact["expected_final_url"])
        for index, error in enumerate(
            (
                OSError("injected socket read failure"),
                seed_evidence.urllib.error.URLError("injected URL read failure"),
            )
        ):
            if index:
                self.reset_fixture()
            result = self.fixture.run(
                ["acquire", "CLIFF-ORJ5", "--retrieved-at", TIMESTAMP],
                opener=SequenceOpener([ReadFailureResponse(200, url, error)]),
            )
            self.assert_failure(result, "streamed read failed")
            self.assertFalse(
                (self.fixture.manifest / "fragments" / "CLIFF-ORJ5.json").exists()
            )

    def test_every_per_source_publish_boundary_rolls_back(self) -> None:
        for failure_call in (1, 2, 3):
            if failure_call > 1:
                self.reset_fixture()
            calls = 0

            def replace(source: str, target: str) -> None:
                nonlocal calls
                calls += 1
                if calls == failure_call:
                    raise OSError("injected replace failure")
                os.replace(source, target)

            result = self.fixture.run(
                ["acquire", "PJRC-TEENSY41-PINOUT", "--retrieved-at", TIMESTAMP],
                opener=self.fixture.opener,
                replace=replace,
            )
            self.assert_failure(result, "publish failed")
            row = self.fixture.realization("PJRC-TEENSY41-PINOUT")
            for artifact in row["artifacts"]:
                self.assertFalse((self.fixture.root / artifact["path"]).exists())
            self.assertFalse(
                (self.fixture.manifest / "fragments" / "PJRC-TEENSY41-PINOUT.json").exists()
            )
            self.assertEqual(list(self.fixture.root.rglob("*.tmp")), [])

    def test_staging_failure_removes_exclusive_temporary_file(self) -> None:
        target = self.fixture.manifest / "staged.json"
        with mock.patch.object(
            seed_evidence.os,
            "fsync",
            side_effect=OSError("injected fsync failure"),
        ):
            with self.assertRaises(seed_evidence.DataError):
                seed_evidence._stage_bytes(target, b"fixture\n")
        self.assertFalse(target.exists())
        self.assertEqual(list(self.fixture.manifest.glob("*.tmp")), [])

    def test_unavailable_pair_publish_boundaries_roll_back(self) -> None:
        for failure_call in (1, 2):
            if failure_call > 1:
                self.reset_fixture()
            calls = 0

            def replace(source: str, target: str) -> None:
                nonlocal calls
                calls += 1
                if calls == failure_call:
                    raise OSError("injected replace failure")
                os.replace(source, target)

            result = self.fixture.run(
                [
                    "unavailable",
                    "BOE-GBP-DAILY-FX",
                    "--note",
                    "Unavailable.",
                    "--retrieved-at",
                    TIMESTAMP,
                ],
                replace=replace,
            )
            self.assert_failure(result, "publish failed")
            self.assertFalse(
                (self.fixture.manifest / "fragments" / "BOE-GBP-DAILY-FX.json").exists()
            )
            self.assertFalse(
                (
                    self.fixture.manifest
                    / "assumption-fragments"
                    / "BOE-GBP-DAILY-FX.json"
                ).exists()
            )
            self.assertEqual(list(self.fixture.root.rglob("*.tmp")), [])

    def test_staged_cleanup_failure_is_reported_as_transaction_failure(self) -> None:
        target = self.fixture.manifest / "cleanup-target.json"

        def replace(source: str, destination: str) -> None:
            raise OSError("injected publish failure")

        with mock.patch.object(
            Path,
            "unlink",
            side_effect=OSError("injected cleanup failure"),
        ):
            with self.assertRaises(seed_evidence.DataError) as raised:
                seed_evidence._publish_new(
                    self.fixture.root,
                    {target: b"fixture\n"},
                    replace,
                )
        self.assertIn("transactional publish failed", str(raised.exception))
        self.assertIn("cleanup failed", str(raised.exception))
        for temporary in self.fixture.manifest.glob("*.tmp"):
            temporary.unlink()

    def test_record_publish_failure_rolls_back(self) -> None:
        def replace(source: str, target: str) -> None:
            raise OSError("injected record failure")

        result = self.fixture.run(
            [
                "record",
                "BOE-GBP-DAILY-FX",
                "--values",
                json.dumps(self.fixture.valid_values("BOE-GBP-DAILY-FX")),
                "--observed-at",
                TIMESTAMP,
            ],
            replace=replace,
        )
        self.assert_failure(result, "publish failed")
        self.assertFalse(
            (self.fixture.manifest / "fragments" / "BOE-GBP-DAILY-FX.json").exists()
        )
        self.assertEqual(list(self.fixture.root.rglob("*.tmp")), [])

    def test_check_fragment_detects_payload_tamper_and_unsafe_paths(self) -> None:
        result = self.fixture.run(
            ["acquire", "CLIFF-ORJ5", "--retrieved-at", TIMESTAMP],
            opener=self.fixture.opener,
        )
        self.assertEqual(result[0], 0, result[2])
        artifact = self.fixture.fragment("CLIFF-ORJ5")["artifacts"][0]
        path = self.fixture.root / artifact["path"]
        path.write_bytes(b"tampered")
        result = self.fixture.run(["check-fragment", "CLIFF-ORJ5"])
        self.assert_failure(result, "hash or size differs")

        self.reset_fixture()
        outside = Path(self.fixture.temporary.name) / "outside"
        outside.mkdir()
        (self.fixture.manifest / "fragments").symlink_to(outside, target_is_directory=True)
        result = self.fixture.run(["check-fragment", "CLIFF-ORJ5"])
        self.assert_failure(result, "directory is unsafe")

    def test_fragment_scan_rejects_missing_extra_temporary_and_nonregular_entries(self) -> None:
        fragments = self.fixture.manifest / "fragments"
        fragments.mkdir()
        (fragments / ".temporary").write_text("x", encoding="utf-8")
        result = self.fixture.run(["assemble", "--write"])
        self.assert_failure(result, "unexpected or unsafe")
        (fragments / ".temporary").unlink()
        (fragments / "NESTED.json").mkdir()
        result = self.fixture.run(["assemble", "--write"])
        self.assert_failure(result, "unexpected or unsafe")

    def test_assembly_reports_isolated_21_of_22_missing_fragment_id(self) -> None:
        missing_id = "BOE-GBP-DAILY-FX"
        self.fixture.populate(skip={missing_id})
        fragments = list((self.fixture.manifest / "fragments").glob("*.json"))
        self.assertEqual(len(fragments), 21)
        result = self.fixture.run(["assemble", "--write"])
        self.assert_failure(
            result,
            f"fragment IDs differ; missing=['{missing_id}'], extra=[]",
        )

    def test_full_assembly_is_canonical_detects_drift_extra_and_duplicates(self) -> None:
        self.fixture.populate()
        result = self.fixture.run(["assemble", "--write"])
        self.assertEqual(result, (0, "", ""), result[2])
        sources_bytes = (self.fixture.manifest / "sources.json").read_bytes()
        assumptions_bytes = (self.fixture.manifest / "assumptions.json").read_bytes()
        self.assertTrue(sources_bytes.endswith(b"\n"))
        sources = json.loads(sources_bytes)
        self.assertEqual(
            [source["id"] for source in sources],
            sorted(source["id"] for source in sources),
        )
        self.assertEqual(assumptions_bytes, b"[]\n")
        result = self.fixture.run(["assemble", "--check"])
        self.assertEqual(result, (0, "", ""), result[2])

        (self.fixture.manifest / "sources.json").write_bytes(sources_bytes + b" ")
        result = self.fixture.run(["assemble", "--check"])
        self.assert_failure(result, "aggregate drift")
        (self.fixture.manifest / "sources.json").write_bytes(sources_bytes)

        fragments = self.fixture.manifest / "fragments"
        paths = sorted(fragments.glob("*.json"))
        first_bytes = paths[0].read_bytes()
        second_bytes = paths[1].read_bytes()
        paths[0].write_bytes(second_bytes)
        paths[1].write_bytes(first_bytes)
        result = self.fixture.run(["assemble", "--write"])
        self.assert_failure(result, "does not match object ID")
        paths[0].write_bytes(first_bytes)
        paths[1].write_bytes(second_bytes)

        (fragments / "EXTRA.json").write_text("{}\n", encoding="utf-8")
        result = self.fixture.run(["assemble", "--write"])
        self.assert_failure(result, "extra=['EXTRA']")
        (fragments / "EXTRA.json").unlink()

        paths = sorted(fragments.glob("*.json"))
        first = _load(paths[0])
        second = _load(paths[1])
        second["id"] = first["id"]
        paths[1].write_text(json.dumps(second, indent=2) + "\n", encoding="utf-8")
        result = self.fixture.run(["assemble", "--write"])
        self.assert_failure(result, "duplicate source record ID")

    def test_nonempty_assumption_aggregation_and_isolated_fragment_failures(self) -> None:
        unavailable_ids = (
            "BOE-GBP-DAILY-FX",
            "VKSDR-TOSLINK-SCHEMATICS",
        )
        self.fixture.populate(skip=set(unavailable_ids))
        for source_id in unavailable_ids:
            result = self.fixture.run(
                [
                    "unavailable",
                    source_id,
                    "--note",
                    f"{source_id} unavailable.",
                    "--retrieved-at",
                    TIMESTAMP,
                ]
            )
            self.assertEqual(result[0], 1, result[2])

        result = self.fixture.run(["assemble", "--write"])
        self.assertEqual(result, (0, "", ""), result[2])
        assumption_directory = self.fixture.manifest / "assumption-fragments"
        expected = [
            _load(assumption_directory / f"{source_id}.json")
            for source_id in unavailable_ids
        ]
        expected.sort(key=lambda row: row["id"])
        aggregate_path = self.fixture.manifest / "assumptions.json"
        self.assertEqual(aggregate_path.read_bytes(), seed_evidence._canonical_json(expected))
        self.assertEqual(
            [row["id"] for row in _load(aggregate_path)],
            sorted(row["id"] for row in expected),
        )
        checked = self.fixture.run(["assemble", "--check"])
        self.assertEqual(checked, (0, "", ""), checked[2])

        missing_path = assumption_directory / "BOE-GBP-DAILY-FX.json"
        missing_bytes = missing_path.read_bytes()
        missing_path.unlink()
        result = self.fixture.run(["assemble", "--write"])
        self.assert_failure(
            result,
            "assumption fragment IDs differ; missing=['BOE-GBP-DAILY-FX'], extra=[]",
        )
        missing_path.write_bytes(missing_bytes)

        extra_path = assumption_directory / "CLIFF-ORJ5.json"
        extra_path.write_bytes(missing_bytes)
        result = self.fixture.run(["assemble", "--write"])
        self.assert_failure(
            result,
            "assumption fragment IDs differ; missing=[], extra=['CLIFF-ORJ5']",
        )

    def test_each_aggregate_publish_boundary_restores_prior_bytes(self) -> None:
        sources_path = self.fixture.manifest / "sources.json"
        assumptions_path = self.fixture.manifest / "assumptions.json"
        for failure_call in (1, 2):
            old_sources = sources_path.read_bytes()
            old_assumptions = assumptions_path.read_bytes()
            calls = 0

            def replace(source: str, target: str) -> None:
                nonlocal calls
                calls += 1
                if calls == failure_call:
                    raise OSError("injected aggregate failure")
                os.replace(source, target)

            with self.assertRaises(seed_evidence.DataError):
                seed_evidence._replace_aggregates(
                    self.fixture.root,
                    {
                        sources_path: b"[1]\n",
                        assumptions_path: b"[2]\n",
                    },
                    replace,
                )
            self.assertEqual(sources_path.read_bytes(), old_sources)
            self.assertEqual(assumptions_path.read_bytes(), old_assumptions)
            self.assertEqual(list(self.fixture.root.rglob("*.tmp")), [])

    def test_actual_aggregate_rollback_operation_failure_is_reported(self) -> None:
        sources_path = self.fixture.manifest / "sources.json"
        assumptions_path = self.fixture.manifest / "assumptions.json"
        calls = 0

        def replace(source: str, target: str) -> None:
            nonlocal calls
            calls += 1
            if calls in {2, 3}:
                raise OSError(f"injected replace failure {calls}")
            os.replace(source, target)

        with self.assertRaises(seed_evidence.DataError) as raised:
            seed_evidence._replace_aggregates(
                self.fixture.root,
                {
                    sources_path: b"[1]\n",
                    assumptions_path: b"[2]\n",
                },
                replace,
            )
        diagnostic = str(raised.exception)
        self.assertIn("aggregate publish failed", diagnostic)
        self.assertIn("rollback failed for", diagnostic)
        self.assertIn("injected replace failure 3", diagnostic)
        self.assertEqual(list(self.fixture.root.rglob("*.tmp")), [])

    def test_aggregate_publish_restores_prior_absence(self) -> None:
        sources_path = self.fixture.manifest / "sources.json"
        assumptions_path = self.fixture.manifest / "assumptions.json"
        sources_path.unlink()
        assumptions_path.unlink()
        calls = 0

        def replace(source: str, target: str) -> None:
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError("injected aggregate failure")
            os.replace(source, target)

        with self.assertRaises(seed_evidence.DataError):
            seed_evidence._replace_aggregates(
                self.fixture.root,
                {sources_path: b"[]\n", assumptions_path: b"[]\n"},
                replace,
            )
        self.assertFalse(sources_path.exists())
        self.assertFalse(assumptions_path.exists())
        self.assertEqual(list(self.fixture.root.rglob("*.tmp")), [])

    def test_aggregate_publish_refuses_symlink_target(self) -> None:
        sources_path = self.fixture.manifest / "sources.json"
        outside = Path(self.fixture.temporary.name) / "outside.json"
        outside.write_bytes(b"outside\n")
        sources_path.unlink()
        sources_path.symlink_to(outside)
        with self.assertRaises(seed_evidence.ControlError):
            seed_evidence._replace_aggregates(
                self.fixture.root,
                {
                    sources_path: b"[]\n",
                    self.fixture.manifest / "assumptions.json": b"[]\n",
                },
            )
        self.assertEqual(outside.read_bytes(), b"outside\n")

    def test_check_fragment_reports_sorted_failures_and_missing_data_as_validation(self) -> None:
        result = self.fixture.run(
            ["check-fragment", "PJRC-TEENSY41-PAGE", "CLIFF-ORJ5"]
        )
        self.assertEqual(result[0], 1)
        lines = result[2].splitlines()
        self.assertEqual(lines, sorted(set(lines)))
        self.assertTrue(all(line.startswith("ERROR:") for line in lines))


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
