#!/usr/bin/env python3
"""Fixture-proven P1D0b1 supplemental manufacturer acquisition tests."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import shutil
import subprocess
import tempfile
import unittest
import urllib.error
from pathlib import Path

import seed_evidence


SCRIPT_ROOT = Path(__file__).resolve().parent
AIUS_ROOT = SCRIPT_ROOT.parent
MANIFEST_ROOT = AIUS_ROOT / "evidence" / "manifest"
ORACLE_FIXTURE_ROOT = SCRIPT_ROOT / "fixtures" / "supplemental-realization"
TIMESTAMP = "2026-07-16T12:34:56Z"
MULTI_REQUEST = "REQ-TEENSY-ETHERNET-SOCKET-MFR"
SINGLE_REQUEST = "REQ-ETHERNET-MAGJACK-MFR"
UK_REQUEST = "REQ-CLIFF-RX-UK"


def _load(path: Path) -> object:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def _payload(artifact: dict[str, object]) -> bytes:
    marker = str(artifact["expected_final_url"]).encode("utf-8")
    if artifact["media_type"] == "application/pdf":
        return b"%PDF-1.7\n" + marker + b"\n"
    if artifact["media_type"] == "text/html":
        return b"<!doctype html><title>fixture</title>\n" + marker + b"\n"
    raise AssertionError(f"unexpected media type {artifact['media_type']!r}")


class Response:
    def __init__(
        self,
        status: int,
        url: str,
        payload: bytes = b"",
        headers: dict[str, str] | None = None,
    ) -> None:
        self.status = status
        self.url = url
        self.payload = payload
        self.offset = 0
        self.headers = {} if headers is None else headers

    def geturl(self) -> str:
        return self.url

    def getcode(self) -> int:
        return self.status

    def read(self, size: int = -1) -> bytes:
        if self.offset >= len(self.payload):
            return b""
        if size < 0:
            size = len(self.payload) - self.offset
        result = self.payload[self.offset : self.offset + size]
        self.offset += len(result)
        return result

    def close(self) -> None:
        return None


class ReadFailureResponse(Response):
    def read(self, size: int = -1) -> bytes:
        del size
        raise OSError("injected streamed read failure")


class SequenceOpener:
    def __init__(self, responses: list[Response | Exception]) -> None:
        self.responses = list(responses)
        self.requests: list[tuple[object, int]] = []

    def open(self, request: object, timeout: int) -> Response:
        self.requests.append((request, timeout))
        if not self.responses:
            raise AssertionError("fake opener has no response")
        response = self.responses.pop(0)
        if isinstance(response, Exception):
            raise response
        return response


class OracleOpener:
    def __init__(self, rows: list[dict[str, object]]) -> None:
        self.artifacts = {
            str(artifact["retrieval_url"]): artifact
            for row in rows
            for artifact in row["artifacts"]
        }
        self.requests: list[tuple[object, int]] = []

    def open(self, request: object, timeout: int) -> Response:
        self.requests.append((request, timeout))
        artifact = self.artifacts.get(request.full_url)
        if artifact is None:
            raise AssertionError(f"unexpected retrieval URL {request.full_url}")
        payload = _payload(artifact)
        return Response(
            200,
            str(artifact["expected_final_url"]),
            payload,
            {
                "Content-Encoding": "identity",
                "Content-Length": str(len(payload)),
            },
        )


class Fixture:
    def __init__(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(
            prefix="aius-supplemental-evidence-"
        )
        self.root = Path(self.temporary.name) / "aius"
        self.manifest = self.root / "evidence" / "manifest"
        self.manifest.mkdir(parents=True)
        for name in (
            "source-schema.json",
            "source-request-schema.json",
            "supplemental-realization-schema.json",
            "supplemental-realization.json",
            "supplemental-requests.json",
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
        realization = _load(self.manifest / "supplemental-realization.json")
        requests = _load(self.manifest / "supplemental-requests.json")
        assert isinstance(realization, dict) and isinstance(requests, dict)
        self.rows = realization["realizations"]
        self.requests = requests["requests"]
        assert isinstance(self.rows, list) and isinstance(self.requests, list)
        self.opener = OracleOpener(self.rows)

    def close(self) -> None:
        self.temporary.cleanup()

    def run(
        self,
        arguments: list[str],
        *,
        opener: object | None = None,
        replace=os.replace,
        formatter: seed_evidence.Formatter = seed_evidence._test_formatter,
    ) -> tuple[int, str, str]:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = seed_evidence.run(
                ["--root", os.fspath(self.root), "supplemental", *arguments],
                opener=self.opener if opener is None else opener,
                replace=replace,
                formatter=formatter,
            )
        return result, stdout.getvalue(), stderr.getvalue()

    def row(self, request_id: str) -> dict[str, object]:
        return next(row for row in self.rows if row["request_id"] == request_id)

    def request(self, request_id: str) -> dict[str, object]:
        return next(
            request
            for request in self.requests
            if request["request_id"] == request_id
        )

    def fragment_path(self, request_id: str) -> Path:
        source_id = self.request(request_id)["source_id"]
        return self.manifest / "supplemental-fragments" / f"{source_id}.json"

    def output_paths(self, request_id: str) -> list[Path]:
        row = self.row(request_id)
        return [
            *[self.root / artifact["path"] for artifact in row["artifacts"]],
            self.fragment_path(request_id),
        ]


class SupplementalEvidenceTests(unittest.TestCase):
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

    def assert_no_outputs(self, request_id: str) -> None:
        for path in self.fixture.output_paths(request_id):
            self.assertFalse(path.exists(), path)
        self.assertEqual(list(self.fixture.root.rglob("*.tmp")), [])

    def test_constructor_matches_committed_p1d0a_multi_artifact_oracle(self) -> None:
        case = _load(ORACLE_FIXTURE_ROOT / "manufacturer-current.json")
        assert isinstance(case, dict)
        request = self.fixture.request(str(case["request_id"]))
        row = self.fixture.row(str(case["request_id"]))
        artifacts = []
        for realization_artifact, result in zip(
            row["artifacts"], case["artifact_results"], strict=True
        ):
            artifacts.append(
                {
                    "name": realization_artifact["name"],
                    "final_url": realization_artifact["expected_final_url"],
                    "storage": realization_artifact["storage"],
                    "path": realization_artifact["path"],
                    "sha256": result["sha256"],
                    "bytes": result["bytes"],
                    "media_type": realization_artifact["media_type"],
                    "retrieval_status": "current",
                    "note": "",
                }
            )
        source = seed_evidence._supplemental_current_manufacturer_source(
            request,
            row,
            artifacts,
            str(case["retrieved_at"]),
        )
        self.assertEqual(source, case["expected_source"])
        self.assertEqual(tuple(source), seed_evidence.SUPPLEMENTAL_SOURCE_KEYS)
        self.assertTrue(
            all(
                tuple(artifact) == seed_evidence.SUPPLEMENTAL_ARTIFACT_KEYS
                for artifact in source["artifacts"]
            )
        )
        self.assertEqual(
            tuple(source["observations"][0]),
            seed_evidence.SUPPLEMENTAL_OBSERVATION_KEYS,
        )

    def test_all_22_manufacturer_realizations_publish_exact_current_sources(self) -> None:
        manufacturer_rows = [row for row in self.fixture.rows if row["artifacts"]]
        self.assertEqual(len(manufacturer_rows), 22)
        for row in manufacturer_rows:
            request_id = str(row["request_id"])
            result = self.fixture.run(
                ["acquire", request_id, "--retrieved-at", TIMESTAMP]
            )
            with self.subTest(request_id=request_id):
                self.assertEqual(result, (0, "", ""))
                source = _load(self.fixture.fragment_path(request_id))
                self.assertIsInstance(source, dict)
                artifacts = []
                for artifact in row["artifacts"]:
                    payload = _payload(artifact)
                    path = self.fixture.root / artifact["path"]
                    self.assertEqual(path.read_bytes(), payload)
                    artifacts.append(
                        {
                            "name": artifact["name"],
                            "final_url": artifact["expected_final_url"],
                            "storage": artifact["storage"],
                            "path": artifact["path"],
                            "sha256": hashlib.sha256(payload).hexdigest(),
                            "bytes": len(payload),
                            "media_type": artifact["media_type"],
                            "retrieval_status": "current",
                            "note": "",
                        }
                    )
                expected = seed_evidence._supplemental_current_manufacturer_source(
                    self.fixture.request(request_id),
                    row,
                    artifacts,
                    TIMESTAMP,
                )
                self.assertEqual(source, expected)
                self.assertTrue(seed_evidence._object_keys_are_sorted(source))
        self.assertEqual(
            [timeout for _, timeout in self.fixture.opener.requests],
            [seed_evidence.TIMEOUT_SECONDS] * 35,
        )
        for request, _ in self.fixture.opener.requests:
            self.assertEqual(request.headers["User-agent"], seed_evidence.USER_AGENT)
            self.assertEqual(request.headers["Accept-encoding"], "identity")

    def test_ordered_repeated_artifacts_define_revision_digest(self) -> None:
        row = self.fixture.row(MULTI_REQUEST)
        self.assertEqual(
            [artifact["artifact_kind"] for artifact in row["artifacts"]],
            ["datasheet", "datasheet", "drawing", "drawing", "product-page"],
        )
        result = self.fixture.run(
            ["acquire", MULTI_REQUEST, "--retrieved-at", TIMESTAMP]
        )
        self.assertEqual(result, (0, "", ""))
        source = _load(self.fixture.fragment_path(MULTI_REQUEST))
        digests = [
            hashlib.sha256(_payload(artifact)).hexdigest()
            for artifact in row["artifacts"]
        ]
        expected = hashlib.sha256("".join(digests).encode("ascii")).hexdigest()
        self.assertEqual(source["revision"], f"sha256:{expected}")
        self.assertNotEqual(
            expected,
            hashlib.sha256("".join(reversed(digests)).encode("ascii")).hexdigest(),
        )

    def test_single_artifact_revision_is_its_lowercase_digest(self) -> None:
        row = self.fixture.row(SINGLE_REQUEST)
        result = self.fixture.run(
            ["acquire", SINGLE_REQUEST, "--retrieved-at", TIMESTAMP]
        )
        self.assertEqual(result, (0, "", ""))
        source = _load(self.fixture.fragment_path(SINGLE_REQUEST))
        digest = hashlib.sha256(_payload(row["artifacts"][0])).hexdigest()
        self.assertEqual(source["revision"], f"sha256:{digest}")

    def test_uk_satisfied_unknown_and_bad_timestamp_reject_without_publication(self) -> None:
        cases = (
            (UK_REQUEST, TIMESTAMP, "does not support acquire"),
            ("REQ-CLIFF-RX-MFR", TIMESTAMP, "has no acquisition realization"),
            ("REQ-NOT-REAL-MFR", TIMESTAMP, "unknown supplemental request ID"),
            (SINGLE_REQUEST, "2026-02-30T12:34:56Z", "timestamp"),
            (SINGLE_REQUEST, "2026-07-16T12:34:56+00:00", "timestamp"),
        )
        for index, (request_id, timestamp, message) in enumerate(cases):
            if index:
                self.reset_fixture()
            result = self.fixture.run(
                ["acquire", request_id, "--retrieved-at", timestamp]
            )
            with self.subTest(request_id=request_id, timestamp=timestamp):
                self.assert_failure(result, message)
                self.assertEqual(self.fixture.opener.requests, [])

    def test_later_serial_commands_are_deterministic_nonfunctional_stubs(self) -> None:
        commands = (
            [
                "record",
                UK_REQUEST,
                "--values",
                "{}",
                "--product-final-url",
                "https://example.com/product",
                "--authorization-final-url",
                "https://example.com/authorization",
                "--observed-at",
                TIMESTAMP,
            ],
            [
                "unavailable",
                SINGLE_REQUEST,
                "--note",
                "Unavailable.",
                "--retrieved-at",
                TIMESTAMP,
            ],
            ["check-fragment", SINGLE_REQUEST],
        )
        for command in commands:
            result = self.fixture.run(command)
            with self.subTest(command=command[0]):
                self.assert_failure(result, "later P1D0b checkpoint")
                self.assertEqual(self.fixture.opener.requests, [])

    def test_supplemental_cli_usage_errors_are_exit_two(self) -> None:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            with self.assertRaises(SystemExit) as raised:
                seed_evidence.run(
                    [
                        "--root",
                        os.fspath(self.fixture.root),
                        "supplemental",
                        "acquire",
                        SINGLE_REQUEST,
                    ]
                )
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("--retrieved-at", stderr.getvalue())

    def test_control_schema_join_and_immutable_oracle_fail_as_exit_two(self) -> None:
        cases = (
            ("realization", "immutable P1D0a oracle"),
            ("request-join", "manufacturer realization differs"),
            ("schema", "source schema is invalid"),
        )
        for index, (kind, message) in enumerate(cases):
            if index:
                self.reset_fixture()
            if kind == "realization":
                path = self.fixture.manifest / "supplemental-realization.json"
                document = _load(path)
                document["realizations"][0]["notes"] = "tampered"
                path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
            elif kind == "request-join":
                path = self.fixture.manifest / "supplemental-requests.json"
                document = _load(path)
                request = next(
                    row
                    for row in document["requests"]
                    if row["request_id"] == SINGLE_REQUEST
                )
                request["selected_manufacturer"] = "Wrong Manufacturer"
                path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
            else:
                path = self.fixture.manifest / "source-schema.json"
                document = _load(path)
                document["type"] = "unknown-schema-type"
                path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
            result = self.fixture.run(
                ["acquire", SINGLE_REQUEST, "--retrieved-at", TIMESTAMP]
            )
            with self.subTest(kind=kind):
                self.assert_failure(result, message, code=2)
                self.assertEqual(self.fixture.opener.requests, [])

    def test_shared_retrieval_rejections_publish_nothing(self) -> None:
        row = self.fixture.row(SINGLE_REQUEST)
        artifact = row["artifacts"][0]
        url = str(artifact["retrieval_url"])
        expected = str(artifact["expected_final_url"])
        valid_payload = _payload(artifact)
        cases: list[tuple[str, SequenceOpener, str]] = [
            (
                "insecure redirect",
                SequenceOpener(
                    [Response(302, url, headers={"Location": "http://example.com"})]
                ),
                "redirect target must be HTTPS",
            ),
            (
                "redirect limit",
                SequenceOpener(
                    [Response(302, url, headers={"Location": url}) for _ in range(6)]
                ),
                "redirect limit exceeded",
            ),
            ("status", SequenceOpener([Response(404, url)]), "terminal HTTP status"),
            (
                "final URL",
                SequenceOpener(
                    [Response(200, "https://example.com/wrong", valid_payload)]
                ),
                "final URL differs",
            ),
            (
                "encoding",
                SequenceOpener(
                    [
                        Response(
                            200,
                            expected,
                            valid_payload,
                            {"Content-Encoding": "gzip"},
                        )
                    ]
                ),
                "unsupported Content-Encoding",
            ),
            (
                "declared size",
                SequenceOpener(
                    [
                        Response(
                            200,
                            expected,
                            valid_payload,
                            {"Content-Length": str(seed_evidence.MAX_PAYLOAD_BYTES + 1)},
                        )
                    ]
                ),
                "payload exceeds",
            ),
            (
                "invalid length",
                SequenceOpener(
                    [
                        Response(
                            200,
                            expected,
                            valid_payload,
                            {"Content-Length": "not-an-integer"},
                        )
                    ]
                ),
                "invalid Content-Length",
            ),
            (
                "stream failure",
                SequenceOpener([ReadFailureResponse(200, expected)]),
                "streamed read failed",
            ),
            (
                "transport",
                SequenceOpener([urllib.error.URLError("injected")]),
                "retrieval failed",
            ),
            (
                "signature",
                SequenceOpener([Response(200, expected, b"not a PDF")]),
                "ARTIFACT_SIGNATURE",
            ),
        ]
        for index, (name, opener, message) in enumerate(cases):
            if index:
                self.reset_fixture()
            result = self.fixture.run(
                ["acquire", SINGLE_REQUEST, "--retrieved-at", TIMESTAMP],
                opener=opener,
            )
            with self.subTest(case=name):
                self.assert_failure(result, message)
                self.assert_no_outputs(SINGLE_REQUEST)

    def test_existing_targets_and_symlink_boundaries_reject_before_publish(self) -> None:
        row = self.fixture.row(SINGLE_REQUEST)
        cases = ("fragment", "artifact", "artifact-parent", "fragment-directory")
        for index, kind in enumerate(cases):
            if index:
                self.reset_fixture()
                row = self.fixture.row(SINGLE_REQUEST)
            outside = Path(self.fixture.temporary.name) / f"outside-{kind}"
            if kind == "fragment":
                target = self.fixture.fragment_path(SINGLE_REQUEST)
                target.parent.mkdir(parents=True)
                target.write_text("foreign\n", encoding="utf-8")
            elif kind == "artifact":
                target = self.fixture.root / row["artifacts"][0]["path"]
                target.parent.mkdir(parents=True)
                target.write_bytes(b"foreign\n")
            elif kind == "artifact-parent":
                outside.mkdir()
                target = (self.fixture.root / row["artifacts"][0]["path"]).parent
                target.parent.mkdir(parents=True)
                target.symlink_to(outside, target_is_directory=True)
            else:
                outside.mkdir()
                target = self.fixture.manifest / "supplemental-fragments"
                target.symlink_to(outside, target_is_directory=True)
            result = self.fixture.run(
                ["acquire", SINGLE_REQUEST, "--retrieved-at", TIMESTAMP]
            )
            with self.subTest(kind=kind):
                self.assert_failure(result, "unsafe" if "parent" in kind or "directory" in kind else "overwrite")

    def test_every_multi_artifact_publication_boundary_rolls_back_and_cleans(self) -> None:
        output_count = len(self.fixture.row(MULTI_REQUEST)["artifacts"]) + 1
        self.assertEqual(output_count, 6)
        for failure_call in range(1, output_count + 1):
            if failure_call > 1:
                self.reset_fixture()
            calls = 0

            def replace(source: str, target: str) -> None:
                nonlocal calls
                calls += 1
                if calls == failure_call:
                    raise OSError("injected supplemental publish failure")
                os.replace(source, target)

            result = self.fixture.run(
                ["acquire", MULTI_REQUEST, "--retrieved-at", TIMESTAMP],
                replace=replace,
            )
            with self.subTest(failure_call=failure_call):
                self.assert_failure(result, "publish failed")
                self.assert_no_outputs(MULTI_REQUEST)

    def test_renderer_failure_occurs_before_transaction_and_cleans_no_outputs(self) -> None:
        def formatter(root: Path, payload: bytes) -> bytes:
            del root, payload
            raise seed_evidence.ControlError("injected renderer failure")

        result = self.fixture.run(
            ["acquire", SINGLE_REQUEST, "--retrieved-at", TIMESTAMP],
            formatter=formatter,
        )
        self.assert_failure(result, "injected renderer failure", code=2)
        self.assert_no_outputs(SINGLE_REQUEST)

    def test_published_fragment_matches_real_pinned_prettier_bytes(self) -> None:
        def real_formatter(root: Path, payload: bytes) -> bytes:
            del root
            return seed_evidence._production_formatter(AIUS_ROOT, payload)

        result = self.fixture.run(
            ["acquire", SINGLE_REQUEST, "--retrieved-at", TIMESTAMP],
            formatter=real_formatter,
        )
        self.assertEqual(result, (0, "", ""))
        path = self.fixture.fragment_path(SINGLE_REQUEST)
        source = _load(path)
        self.assertTrue(seed_evidence._object_keys_are_sorted(source))
        expected = seed_evidence._render_json(
            AIUS_ROOT,
            source,
            seed_evidence._production_formatter,
        )
        self.assertEqual(path.read_bytes(), expected)


if __name__ == "__main__":
    unittest.main(verbosity=2)
