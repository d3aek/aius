#!/usr/bin/env python3
"""Fixture-proven P1D0b supplemental evidence tests."""

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
UNRESOLVED_REQUESTS = (
    "REQ-ETHERNET-MAGJACK-UK",
    "REQ-INTERNAL-USB-CABLE-UK",
)
PRODUCT_FINAL_URL = "https://example.com/commerce/final"
AUTHORIZATION_FINAL_URL = "https://example.com/authorization/final"


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
            "assumption-schema.json",
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
        self.profiles = realization["observation_profiles"]
        self.requests = requests["requests"]
        assert (
            isinstance(self.rows, list)
            and isinstance(self.profiles, list)
            and isinstance(self.requests, list)
        )
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

    def fragment(self, request_id: str) -> dict[str, object]:
        value = _load(self.fragment_path(request_id))
        assert isinstance(value, dict)
        return value

    def assumption_path(self, request_id: str) -> Path:
        source_id = self.request(request_id)["source_id"]
        return (
            self.manifest
            / "supplemental-assumption-fragments"
            / f"{source_id}.json"
        )

    def assumption(self, request_id: str) -> dict[str, object]:
        value = _load(self.assumption_path(request_id))
        assert isinstance(value, dict)
        return value

    def profile(self) -> dict[str, object]:
        self.assert_profile_count()
        profile = self.profiles[0]
        assert isinstance(profile, dict)
        return profile

    def assert_profile_count(self) -> None:
        if len(self.profiles) != 1:
            raise AssertionError("fixture must contain exactly one observation profile")

    def valid_values(
        self,
        request_id: str,
        *,
        stock_state: str = "in-stock",
        stock_quantity: int | None = 12,
        price_quantity: int = 12,
        minimum_order_quantity: int = 1,
        order_multiple: int = 1,
    ) -> dict[str, object]:
        request = self.request(request_id)
        row = self.row(request_id)
        authorization = row["authorization"]
        assert isinstance(authorization, dict)
        return {
            "manufacturer_part_number": request["selected_mpn"],
            "supplier_sku": f"SKU-{request_id}",
            "distributor": row["authority"],
            "product_initial_url": row["observation_url"],
            "product_final_url": PRODUCT_FINAL_URL,
            "authorization_authority": authorization["authority"],
            "authorization_method": authorization["method"],
            "authorization_initial_url": authorization["initial_url"],
            "authorization_final_url": AUTHORIZATION_FINAL_URL,
            "stock_state": stock_state,
            "stock_quantity": stock_quantity,
            "price_quantity": price_quantity,
            "unit_price_ex_vat": 1.25,
            "currency": "GBP",
            "minimum_order_quantity": minimum_order_quantity,
            "order_multiple": order_multiple,
            "price_includes_vat": False,
        }

    def record_arguments(
        self,
        request_id: str,
        values: dict[str, object] | None = None,
        *,
        product_final_url: str = PRODUCT_FINAL_URL,
        authorization_final_url: str = AUTHORIZATION_FINAL_URL,
        observed_at: str = TIMESTAMP,
    ) -> list[str]:
        return [
            "record",
            request_id,
            "--values",
            json.dumps(
                self.valid_values(request_id) if values is None else values,
                allow_nan=False,
            ),
            "--product-final-url",
            product_final_url,
            "--authorization-final-url",
            authorization_final_url,
            "--observed-at",
            observed_at,
        ]

    def unavailable_arguments(
        self,
        request_id: str,
        *,
        note: str = "Unavailable.",
        retrieved_at: str = TIMESTAMP,
    ) -> list[str]:
        return [
            "unavailable",
            request_id,
            "--note",
            note,
            "--retrieved-at",
            retrieved_at,
        ]

    def output_paths(self, request_id: str) -> list[Path]:
        row = self.row(request_id)
        return [
            *[self.root / artifact["path"] for artifact in row["artifacts"]],
            self.fragment_path(request_id),
            self.assumption_path(request_id),
        ]

    def write_rendered(self, path: Path, value: object) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(
            seed_evidence._render_json(
                self.root,
                value,
                seed_evidence._test_formatter,
            )
        )


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

    def test_unavailable_constructors_match_both_committed_p1d0a_oracles(self) -> None:
        for fixture_name in ("manufacturer-unavailable.json", "uk-unavailable.json"):
            case = _load(ORACLE_FIXTURE_ROOT / fixture_name)
            assert isinstance(case, dict)
            request_id = str(case["request_id"])
            source = seed_evidence._supplemental_unavailable_source(
                self.fixture.request(request_id),
                self.fixture.row(request_id),
                str(case["note"]),
                str(case["retrieved_at"]),
            )
            with self.subTest(fixture=fixture_name):
                self.assertEqual(source, case["expected_source"])
                self.assertEqual(tuple(source), seed_evidence.SUPPLEMENTAL_SOURCE_KEYS)
                self.assertEqual(
                    tuple(source["artifacts"][0]),
                    seed_evidence.SUPPLEMENTAL_ARTIFACT_KEYS,
                )
        request = self.fixture.request(UK_REQUEST)
        requirements = request["requirements"]
        source_id = request["source_id"]
        self.assertEqual(
            seed_evidence._expected_supplemental_assumption(request),
            {
                "id": f"ASM-UNAVAILABLE-{source_id}",
                "claim": (
                    f"Supplemental request {UK_REQUEST} source {source_id} is "
                    f"unavailable for: {'; '.join(requirements)}"
                ),
                "consequence": (
                    f"The selected component remains provisional until {UK_REQUEST} "
                    "is replaced with current evidence."
                ),
                "owner": "P1D",
                "validation_gate": "Replace unavailable supplemental evidence",
                "evidence_ids": [],
                "source_ids": [source_id],
                "claim_scope": requirements,
                "status": "open",
                "resolution": None,
            },
        )

    def test_all_46_rows_publish_validate_and_exit_by_exact_unavailable_class(self) -> None:
        manufacturer_ids: list[str] = []
        uk_ids: list[str] = []
        note = "  Fixed realization unavailable.  "
        for row in self.fixture.rows:
            request_id = str(row["request_id"])
            request = self.fixture.request(request_id)
            result = self.fixture.run(
                self.fixture.unavailable_arguments(request_id, note=note)
            )
            source = self.fixture.fragment(request_id)
            expected_source = seed_evidence._supplemental_unavailable_source(
                request,
                row,
                note,
                TIMESTAMP,
            )
            with self.subTest(request_id=request_id):
                self.assertEqual(source, expected_source)
                self.assertTrue(seed_evidence._object_keys_are_sorted(source))
                self.assertEqual(source["artifacts"][0]["note"], note)
                if request["request_kind"] == "manufacturer-evidence":
                    manufacturer_ids.append(request_id)
                    self.assertEqual(result[0], 3, result[2])
                    self.assertEqual(result[1], "")
                    self.assertIn("recorded unavailable", result[2])
                    self.assertFalse(self.fixture.assumption_path(request_id).exists())
                else:
                    uk_ids.append(request_id)
                    self.assertEqual(result, (0, "", ""))
                    assumption = self.fixture.assumption(request_id)
                    self.assertEqual(
                        assumption,
                        seed_evidence._expected_supplemental_assumption(request),
                    )
                    self.assertTrue(
                        seed_evidence._object_keys_are_sorted(assumption)
                    )
                for artifact in row["artifacts"]:
                    self.assertFalse((self.fixture.root / artifact["path"]).exists())
                self.assertEqual(
                    self.fixture.run(["check-fragment", request_id]),
                    (0, "", ""),
                )
        self.assertEqual(len(manufacturer_ids), 22)
        self.assertEqual(len(uk_ids), 24)
        self.assertEqual(self.fixture.opener.requests, [])

    def test_unavailable_rejects_identity_note_and_timestamp_without_publication(self) -> None:
        cases = (
            (SINGLE_REQUEST, "   ", TIMESTAMP, "non-whitespace"),
            (SINGLE_REQUEST, "Unavailable.", "2026-07-16", "timestamp"),
            ("REQ-NOT-REAL-UK", "Unavailable.", TIMESTAMP, "unknown"),
            (
                "REQ-CLIFF-RX-MFR",
                "Unavailable.",
                TIMESTAMP,
                "no acquisition realization",
            ),
        )
        for index, (request_id, note, retrieved_at, message) in enumerate(cases):
            if index:
                self.reset_fixture()
            result = self.fixture.run(
                [
                    "unavailable",
                    request_id,
                    "--note",
                    note,
                    "--retrieved-at",
                    retrieved_at,
                ]
            )
            with self.subTest(request_id=request_id, note=note):
                self.assert_failure(result, message)
                fragments = self.fixture.manifest / "supplemental-fragments"
                assumptions = (
                    self.fixture.manifest
                    / "supplemental-assumption-fragments"
                )
                self.assertEqual(
                    list(fragments.glob("*.json")) if fragments.exists() else [],
                    [],
                )
                self.assertEqual(
                    list(assumptions.glob("*.json")) if assumptions.exists() else [],
                    [],
                )
                self.assertEqual(list(self.fixture.root.rglob("*.tmp")), [])
                self.assertEqual(self.fixture.opener.requests, [])

    def test_every_unavailable_publication_boundary_is_atomic_exit_one(self) -> None:
        cases = ((SINGLE_REQUEST, 1), (UK_REQUEST, 1), (UK_REQUEST, 2))
        for index, (request_id, failure_call) in enumerate(cases):
            if index:
                self.reset_fixture()
            calls = 0

            def replace(source: str, target: str) -> None:
                nonlocal calls
                calls += 1
                if calls == failure_call:
                    raise OSError("injected supplemental unavailable publish failure")
                os.replace(source, target)

            result = self.fixture.run(
                self.fixture.unavailable_arguments(request_id),
                replace=replace,
            )
            with self.subTest(request_id=request_id, failure_call=failure_call):
                self.assert_failure(result, "publish failed", code=1)
                self.assert_no_outputs(request_id)
                self.assertEqual(self.fixture.opener.requests, [])

    def test_unavailable_refuses_existing_and_symlink_output_boundaries(self) -> None:
        cases = (
            ("source-existing", SINGLE_REQUEST),
            ("source-symlink", SINGLE_REQUEST),
            ("source-parent-symlink", SINGLE_REQUEST),
            ("assumption-existing", UK_REQUEST),
            ("assumption-symlink", UK_REQUEST),
            ("assumption-parent-symlink", UK_REQUEST),
        )
        for index, (kind, request_id) in enumerate(cases):
            if index:
                self.reset_fixture()
            outside = Path(self.fixture.temporary.name) / f"outside-{kind}"
            if kind == "source-existing":
                target = self.fixture.fragment_path(request_id)
                target.parent.mkdir(parents=True)
                target.write_text("foreign\n", encoding="utf-8")
            elif kind == "source-symlink":
                target = self.fixture.fragment_path(request_id)
                target.parent.mkdir(parents=True)
                outside.write_text("outside\n", encoding="utf-8")
                target.symlink_to(outside)
            elif kind == "source-parent-symlink":
                target = self.fixture.manifest / "supplemental-fragments"
                outside.mkdir()
                target.symlink_to(outside, target_is_directory=True)
            elif kind == "assumption-existing":
                target = self.fixture.assumption_path(request_id)
                target.parent.mkdir(parents=True)
                target.write_text("foreign\n", encoding="utf-8")
            elif kind == "assumption-symlink":
                target = self.fixture.assumption_path(request_id)
                target.parent.mkdir(parents=True)
                outside.write_text("outside\n", encoding="utf-8")
                target.symlink_to(outside)
            else:
                target = (
                    self.fixture.manifest
                    / "supplemental-assumption-fragments"
                )
                outside.mkdir()
                target.symlink_to(outside, target_is_directory=True)
            result = self.fixture.run(
                self.fixture.unavailable_arguments(request_id)
            )
            with self.subTest(kind=kind):
                self.assert_failure(
                    result,
                    "unsafe" if "parent" in kind else "overwrite",
                )
                if kind.startswith("assumption"):
                    self.assertFalse(self.fixture.fragment_path(request_id).exists())
                if kind == "source-existing":
                    self.assertEqual(target.read_text(encoding="utf-8"), "foreign\n")
                if kind in {"source-symlink", "assumption-symlink"}:
                    self.assertTrue(target.is_symlink())
                    self.assertEqual(outside.read_text(encoding="utf-8"), "outside\n")
                if "parent" in kind:
                    self.assertEqual(list(outside.iterdir()), [])
                self.assertEqual(list(self.fixture.root.rglob("*.tmp")), [])

    def test_unavailable_renderer_failures_precede_atomic_publication(self) -> None:
        for failure_call in (1, 2):
            if failure_call > 1:
                self.reset_fixture()
            calls = 0

            def formatter(root: Path, payload: bytes) -> bytes:
                nonlocal calls
                calls += 1
                if calls == failure_call:
                    raise seed_evidence.ControlError(
                        "injected unavailable renderer failure"
                    )
                return seed_evidence._test_formatter(root, payload)

            result = self.fixture.run(
                self.fixture.unavailable_arguments(UK_REQUEST),
                formatter=formatter,
            )
            with self.subTest(failure_call=failure_call):
                self.assert_failure(
                    result,
                    "injected unavailable renderer failure",
                    code=2,
                )
                self.assert_no_outputs(UK_REQUEST)

    def test_unavailable_pair_matches_real_pinned_prettier_and_checks(self) -> None:
        def real_formatter(root: Path, payload: bytes) -> bytes:
            del root
            return seed_evidence._production_formatter(AIUS_ROOT, payload)

        result = self.fixture.run(
            self.fixture.unavailable_arguments(UK_REQUEST),
            formatter=real_formatter,
        )
        self.assertEqual(result, (0, "", ""), result[2])
        for path, value in (
            (self.fixture.fragment_path(UK_REQUEST), self.fixture.fragment(UK_REQUEST)),
            (
                self.fixture.assumption_path(UK_REQUEST),
                self.fixture.assumption(UK_REQUEST),
            ),
        ):
            expected = seed_evidence._render_json(
                AIUS_ROOT,
                value,
                seed_evidence._production_formatter,
            )
            self.assertEqual(path.read_bytes(), expected)
            self.assertTrue(seed_evidence._object_keys_are_sorted(value))
        self.assertEqual(
            self.fixture.run(
                ["check-fragment", UK_REQUEST],
                formatter=real_formatter,
            ),
            (0, "", ""),
        )

    def test_check_fragment_accepts_each_outcome_without_requiring_siblings(self) -> None:
        cases = (
            (
                SINGLE_REQUEST,
                ["acquire", SINGLE_REQUEST, "--retrieved-at", TIMESTAMP],
                0,
            ),
            (UK_REQUEST, self.fixture.record_arguments(UK_REQUEST), 0),
            (UK_REQUEST, self.fixture.unavailable_arguments(UK_REQUEST), 0),
            (
                SINGLE_REQUEST,
                self.fixture.unavailable_arguments(SINGLE_REQUEST),
                3,
            ),
        )
        for index, (request_id, command, expected_status) in enumerate(cases):
            if index:
                self.reset_fixture()
                if command[0] == "record":
                    command = self.fixture.record_arguments(request_id)
                elif command[0] == "unavailable":
                    command = self.fixture.unavailable_arguments(request_id)
            published = self.fixture.run(command)
            with self.subTest(command=command[0], request_id=request_id):
                self.assertEqual(published[0], expected_status, published[2])
                sibling = self.fixture.fragment_path("REQ-CLIFF-TX-UK")
                sibling.parent.mkdir(parents=True, exist_ok=True)
                sibling.write_bytes(b"not JSON\n")
                sibling_assumption = self.fixture.assumption_path("REQ-CLIFF-TX-UK")
                sibling_assumption.parent.mkdir(parents=True, exist_ok=True)
                sibling_assumption.write_bytes(b"not JSON\n")
                self.assertEqual(
                    self.fixture.run(["check-fragment", request_id]),
                    (0, "", ""),
                )

    def test_check_fragment_rejects_payload_and_metadata_tampering(self) -> None:
        cases = (
            "missing-source",
            "source-constructor",
            "source-rendering",
            "source-order",
            "source-duplicate-key",
            "missing-assumption",
            "assumption-constructor",
            "unexpected-assumption",
            "artifact-payload",
        )
        for index, kind in enumerate(cases):
            if index:
                self.reset_fixture()
            request_id = UK_REQUEST
            if kind == "missing-source":
                message = "missing"
            elif kind in {
                "source-constructor",
                "source-rendering",
                "source-order",
                "source-duplicate-key",
                "missing-assumption",
                "assumption-constructor",
            }:
                self.assertEqual(
                    self.fixture.run(
                        self.fixture.unavailable_arguments(UK_REQUEST)
                    ),
                    (0, "", ""),
                )
                if kind == "source-constructor":
                    source = self.fixture.fragment(UK_REQUEST)
                    source["title"] = "tampered"
                    self.fixture.write_rendered(
                        self.fixture.fragment_path(UK_REQUEST),
                        source,
                    )
                    message = "constructor"
                elif kind == "source-rendering":
                    source = self.fixture.fragment(UK_REQUEST)
                    self.fixture.fragment_path(UK_REQUEST).write_text(
                        json.dumps(source, separators=(",", ":")) + "\n",
                        encoding="utf-8",
                    )
                    message = "renderer bytes"
                elif kind == "source-order":
                    source = self.fixture.fragment(UK_REQUEST)
                    reversed_source = dict(reversed(list(source.items())))
                    self.fixture.fragment_path(UK_REQUEST).write_text(
                        json.dumps(reversed_source, indent=2) + "\n",
                        encoding="utf-8",
                    )
                    message = "object-key order"
                elif kind == "source-duplicate-key":
                    path = self.fixture.fragment_path(UK_REQUEST)
                    payload = path.read_text(encoding="utf-8")
                    path.write_text(
                        '{"status":"unavailable",' + payload[1:],
                        encoding="utf-8",
                    )
                    message = "duplicate JSON key"
                elif kind == "missing-assumption":
                    self.fixture.assumption_path(UK_REQUEST).unlink()
                    message = "requires an unavailable assumption"
                else:
                    assumption = self.fixture.assumption(UK_REQUEST)
                    assumption["owner"] = "wrong"
                    self.fixture.write_rendered(
                        self.fixture.assumption_path(UK_REQUEST),
                        assumption,
                    )
                    message = "non-canonical"
            elif kind == "unexpected-assumption":
                self.assertEqual(
                    self.fixture.run(self.fixture.record_arguments(UK_REQUEST)),
                    (0, "", ""),
                )
                assumption = seed_evidence._expected_supplemental_assumption(
                    self.fixture.request(UK_REQUEST)
                )
                self.fixture.write_rendered(
                    self.fixture.assumption_path(UK_REQUEST),
                    assumption,
                )
                message = "unexpected assumption"
            else:
                request_id = SINGLE_REQUEST
                self.assertEqual(
                    self.fixture.run(
                        ["acquire", SINGLE_REQUEST, "--retrieved-at", TIMESTAMP]
                    ),
                    (0, "", ""),
                )
                artifact = self.fixture.row(SINGLE_REQUEST)["artifacts"][0]
                (self.fixture.root / artifact["path"]).write_bytes(b"tampered")
                message = "hash or size"
            result = self.fixture.run(["check-fragment", request_id])
            with self.subTest(kind=kind):
                self.assert_failure(result, message)

    def test_check_fragment_rejects_source_assumption_and_artifact_symlinks(self) -> None:
        cases = ("source", "assumption", "artifact")
        for index, kind in enumerate(cases):
            if index:
                self.reset_fixture()
            request_id = UK_REQUEST if kind != "artifact" else SINGLE_REQUEST
            if kind == "artifact":
                self.assertEqual(
                    self.fixture.run(
                        ["acquire", request_id, "--retrieved-at", TIMESTAMP]
                    ),
                    (0, "", ""),
                )
                artifact = self.fixture.row(request_id)["artifacts"][0]
                target = self.fixture.root / artifact["path"]
            else:
                self.assertEqual(
                    self.fixture.run(
                        self.fixture.unavailable_arguments(request_id)
                    ),
                    (0, "", ""),
                )
                target = (
                    self.fixture.fragment_path(request_id)
                    if kind == "source"
                    else self.fixture.assumption_path(request_id)
                )
            outside = Path(self.fixture.temporary.name) / f"outside-check-{kind}"
            target.unlink()
            outside.write_text("outside\n", encoding="utf-8")
            target.symlink_to(outside)
            result = self.fixture.run(["check-fragment", request_id])
            with self.subTest(kind=kind):
                self.assert_failure(result, "regular non-symlink")
                self.assertEqual(outside.read_text(encoding="utf-8"), "outside\n")

    def test_exact_profile_matches_locked_17_field_contract(self) -> None:
        profile = self.fixture.profile()
        expected = seed_evidence._expected_uk_observation_profile()
        self.assertEqual(profile, expected)
        self.assertEqual(tuple(profile), seed_evidence.SUPPLEMENTAL_PROFILE_KEYS)
        self.assertEqual(len(profile["fields"]), 17)
        self.assertEqual(
            [field["key"] for field in profile["fields"]],
            [spec[0] for spec in seed_evidence.UK_OBSERVATION_FIELD_SPECS],
        )
        for field in profile["fields"]:
            self.assertEqual(tuple(field), seed_evidence.SUPPLEMENTAL_FIELD_KEYS)
            self.assertEqual(tuple(field["constraint"]), ("kind", "value"))

    def test_all_24_uk_rows_record_or_reject_by_locked_authorization(self) -> None:
        uk_rows = [row for row in self.fixture.rows if row["observation_profile"]]
        self.assertEqual(len(uk_rows), 24)
        unresolved: list[str] = []
        for row in uk_rows:
            request_id = str(row["request_id"])
            authorization = row["authorization"]
            assert isinstance(authorization, dict)
            result = self.fixture.run(self.fixture.record_arguments(request_id))
            with self.subTest(request_id=request_id):
                if authorization["status"] == "unresolved":
                    unresolved.append(request_id)
                    self.assert_failure(result, "authorization is unresolved")
                    self.assertFalse(self.fixture.fragment_path(request_id).exists())
                    continue
                self.assertEqual(result, (0, "", ""), result[2])
                source = self.fixture.fragment(request_id)
                expected = seed_evidence._supplemental_current_uk_source(
                    self.fixture.request(request_id),
                    row,
                    self.fixture.profile(),
                    self.fixture.valid_values(request_id),
                    PRODUCT_FINAL_URL,
                    AUTHORIZATION_FINAL_URL,
                    TIMESTAMP,
                )
                self.assertEqual(source, expected)
                self.assertTrue(seed_evidence._object_keys_are_sorted(source))
        self.assertEqual(tuple(unresolved), UNRESOLVED_REQUESTS)
        self.assertEqual(self.fixture.opener.requests, [])

    def test_dynamic_constructor_order_metadata_and_role_source_urls_are_exact(self) -> None:
        row = self.fixture.row(UK_REQUEST)
        request = self.fixture.request(UK_REQUEST)
        values = self.fixture.valid_values(UK_REQUEST)
        source = seed_evidence._supplemental_current_uk_source(
            request,
            row,
            self.fixture.profile(),
            values,
            PRODUCT_FINAL_URL,
            AUTHORIZATION_FINAL_URL,
            TIMESTAMP,
        )
        self.assertEqual(tuple(source), seed_evidence.SUPPLEMENTAL_SOURCE_KEYS)
        self.assertEqual(source["class"], "dynamic")
        self.assertEqual(source["revision"], "observed:2026-07-16")
        self.assertEqual(source["landing_url"], request["authority_url"])
        self.assertEqual(source["authority"], row["authority"])
        self.assertEqual(source["artifacts"], [])
        self.assertEqual(source["redistribution_note"], seed_evidence.DYNAMIC_NOTE)
        observations = source["observations"]
        self.assertEqual(
            [observation["key"] for observation in observations],
            [field["key"] for field in self.fixture.profile()["fields"]],
        )
        for field, observation in zip(
            self.fixture.profile()["fields"], observations, strict=True
        ):
            self.assertEqual(
                tuple(observation), seed_evidence.SUPPLEMENTAL_OBSERVATION_KEYS
            )
            self.assertEqual(observation["unit"], field["unit"])
            self.assertEqual(observation["basis"], field["basis"])
            self.assertEqual(observation["observed_at"], TIMESTAMP)
            expected_url = (
                PRODUCT_FINAL_URL
                if field["source_role"] == "product"
                else AUTHORIZATION_FINAL_URL
            )
            self.assertEqual(observation["source_url"], expected_url)

    def test_record_accepts_every_stock_state_at_its_exact_quantity_boundary(self) -> None:
        cases = (
            ("in-stock", 1),
            ("out-of-stock", 0),
            ("backorder", None),
            ("unknown", None),
        )
        for index, (stock_state, stock_quantity) in enumerate(cases):
            if index:
                self.reset_fixture()
            values = self.fixture.valid_values(
                UK_REQUEST,
                stock_state=stock_state,
                stock_quantity=stock_quantity,
            )
            result = self.fixture.run(
                self.fixture.record_arguments(UK_REQUEST, values)
            )
            with self.subTest(stock_state=stock_state):
                self.assertEqual(result, (0, "", ""), result[2])

    def test_record_accepts_selected_quantity_at_moq_and_multiple_boundaries(self) -> None:
        values = self.fixture.valid_values(
            UK_REQUEST,
            price_quantity=6,
            minimum_order_quantity=5,
            order_multiple=3,
        )
        result = self.fixture.run(self.fixture.record_arguments(UK_REQUEST, values))
        self.assertEqual(result, (0, "", ""), result[2])

    def test_record_rejects_strict_json_shape_and_every_field_type(self) -> None:
        valid = self.fixture.valid_values(UK_REQUEST)
        wrong_types = {
            "manufacturer_part_number": 1,
            "supplier_sku": 1,
            "distributor": 1,
            "product_initial_url": 1,
            "product_final_url": 1,
            "authorization_authority": 1,
            "authorization_method": 1,
            "authorization_initial_url": 1,
            "authorization_final_url": 1,
            "stock_state": 1,
            "stock_quantity": "1",
            "price_quantity": True,
            "unit_price_ex_vat": True,
            "currency": 1,
            "minimum_order_quantity": True,
            "order_multiple": True,
            "price_includes_vat": "false",
        }
        cases: list[tuple[str, str, str]] = [
            ("nonfinite", "NaN", "invalid JSON constant"),
            ("duplicate", '{"x":1,"x":2}', "duplicate JSON key"),
            ("array", "[]", "must be a JSON object"),
            ("nested", '{"x":{}}', "only JSON scalars"),
            (
                "missing",
                json.dumps({key: value for key, value in valid.items() if key != "supplier_sku"}),
                "observation keys differ",
            ),
            (
                "extra",
                json.dumps({**valid, "extra": 1}),
                "observation keys differ",
            ),
        ]
        cases.extend(
            (
                key,
                json.dumps({**valid, key: wrong_value}),
                f"{key!r} must be",
            )
            for key, wrong_value in wrong_types.items()
        )
        for index, (name, values_text, message) in enumerate(cases):
            if index:
                self.reset_fixture()
            arguments = self.fixture.record_arguments(UK_REQUEST)
            arguments[3] = values_text
            result = self.fixture.run(arguments)
            with self.subTest(case=name):
                self.assert_failure(result, message)
                self.assert_no_outputs(UK_REQUEST)

    def test_record_identity_timestamp_and_exit_contracts(self) -> None:
        cases = (
            (SINGLE_REQUEST, TIMESTAMP, "does not support record"),
            ("REQ-CLIFF-RX-MFR", TIMESTAMP, "has no acquisition realization"),
            ("REQ-NOT-REAL-UK", TIMESTAMP, "unknown supplemental request ID"),
            (UK_REQUEST, "2026-02-30T12:34:56Z", "timestamp"),
            (UK_REQUEST, "2026-07-16T12:34:56+00:00", "timestamp"),
        )
        for index, (request_id, observed_at, message) in enumerate(cases):
            if index:
                self.reset_fixture()
            values = (
                self.fixture.valid_values(UK_REQUEST)
                if request_id != UK_REQUEST
                else self.fixture.valid_values(request_id)
            )
            result = self.fixture.run(
                self.fixture.record_arguments(
                    request_id,
                    values,
                    observed_at=observed_at,
                )
            )
            with self.subTest(request_id=request_id, observed_at=observed_at):
                self.assert_failure(result, message, code=1)
                self.assertEqual(self.fixture.opener.requests, [])

    def test_record_existing_target_and_symlink_boundaries_reject(self) -> None:
        for index, kind in enumerate(
            ("fragment", "fragment-symlink", "fragment-directory")
        ):
            if index:
                self.reset_fixture()
            outside = Path(self.fixture.temporary.name) / f"outside-record-{kind}"
            target = self.fixture.fragment_path(UK_REQUEST)
            if kind == "fragment":
                target.parent.mkdir(parents=True)
                target.write_text("foreign\n", encoding="utf-8")
            elif kind == "fragment-symlink":
                target.parent.mkdir(parents=True)
                outside.write_text("outside\n", encoding="utf-8")
                target.symlink_to(outside)
            else:
                outside.mkdir()
                target.parent.symlink_to(outside, target_is_directory=True)
            result = self.fixture.run(self.fixture.record_arguments(UK_REQUEST))
            with self.subTest(kind=kind):
                self.assert_failure(
                    result,
                    "unsafe" if kind == "fragment-directory" else "overwrite",
                )
                if kind == "fragment":
                    self.assertEqual(target.read_text(encoding="utf-8"), "foreign\n")
                elif kind == "fragment-symlink":
                    self.assertTrue(target.is_symlink())
                    self.assertEqual(outside.read_text(encoding="utf-8"), "outside\n")
                else:
                    self.assertEqual(list(outside.iterdir()), [])
                self.assertEqual(list(self.fixture.root.rglob("*.tmp")), [])

    def test_record_publication_failure_rolls_back_and_cleans_temporary(self) -> None:
        def replace(source: str, target: str) -> None:
            del source, target
            raise OSError("injected supplemental record publish failure")

        result = self.fixture.run(
            self.fixture.record_arguments(UK_REQUEST),
            replace=replace,
        )
        self.assert_failure(result, "publish failed")
        self.assert_no_outputs(UK_REQUEST)

    def test_record_renderer_failure_precedes_publication(self) -> None:
        def formatter(root: Path, payload: bytes) -> bytes:
            del root, payload
            raise seed_evidence.ControlError("injected record renderer failure")

        result = self.fixture.run(
            self.fixture.record_arguments(UK_REQUEST),
            formatter=formatter,
        )
        self.assert_failure(result, "injected record renderer failure", code=2)
        self.assert_no_outputs(UK_REQUEST)

    def test_record_fragment_matches_real_pinned_prettier_bytes(self) -> None:
        def real_formatter(root: Path, payload: bytes) -> bytes:
            del root
            return seed_evidence._production_formatter(AIUS_ROOT, payload)

        result = self.fixture.run(
            self.fixture.record_arguments(UK_REQUEST),
            formatter=real_formatter,
        )
        self.assertEqual(result, (0, "", ""), result[2])
        path = self.fixture.fragment_path(UK_REQUEST)
        source = self.fixture.fragment(UK_REQUEST)
        self.assertTrue(seed_evidence._object_keys_are_sorted(source))
        expected = seed_evidence._render_json(
            AIUS_ROOT,
            source,
            seed_evidence._production_formatter,
        )
        self.assertEqual(path.read_bytes(), expected)

    def test_record_rejects_every_constraint_and_cross_field_failure(self) -> None:
        valid = self.fixture.valid_values(UK_REQUEST)
        mutations: tuple[tuple[str, dict[str, object], str], ...] = (
            ("empty-sku", {"supplier_sku": " "}, "does not match its pattern"),
            ("stock-enum", {"stock_state": "available"}, "outside its enum"),
            ("stock-minimum", {"stock_quantity": -1}, "below its minimum"),
            ("price-minimum", {"price_quantity": 0}, "below its minimum"),
            ("unit-price-minimum", {"unit_price_ex_vat": -0.01}, "below its minimum"),
            ("currency", {"currency": "USD"}, "does not equal its constant"),
            ("moq-minimum", {"minimum_order_quantity": 0}, "below its minimum"),
            ("multiple-minimum", {"order_multiple": 0}, "below its minimum"),
            ("vat", {"price_includes_vat": True}, "does not equal its constant"),
            (
                "in-stock-null",
                {"stock_state": "in-stock", "stock_quantity": None},
                "requires stock_quantity >= 1",
            ),
            (
                "in-stock-zero",
                {"stock_state": "in-stock", "stock_quantity": 0},
                "requires stock_quantity >= 1",
            ),
            (
                "out-of-stock-positive",
                {"stock_state": "out-of-stock", "stock_quantity": 1},
                "requires stock_quantity 0",
            ),
            (
                "out-of-stock-null",
                {"stock_state": "out-of-stock", "stock_quantity": None},
                "requires stock_quantity 0",
            ),
            (
                "backorder-quantity",
                {"stock_state": "backorder", "stock_quantity": 0},
                "requires null stock_quantity",
            ),
            (
                "unknown-quantity",
                {"stock_state": "unknown", "stock_quantity": 0},
                "requires null stock_quantity",
            ),
            (
                "below-moq",
                {"price_quantity": 2, "minimum_order_quantity": 3},
                "greater than or equal",
            ),
            (
                "order-multiple",
                {"price_quantity": 5, "order_multiple": 2},
                "divisible by order_multiple",
            ),
        )
        for index, (name, changes, message) in enumerate(mutations):
            if index:
                self.reset_fixture()
            result = self.fixture.run(
                self.fixture.record_arguments(UK_REQUEST, {**valid, **changes})
            )
            with self.subTest(case=name):
                self.assert_failure(result, message)
                self.assert_no_outputs(UK_REQUEST)

    def test_record_rejects_every_request_realization_and_final_url_mismatch(self) -> None:
        valid = self.fixture.valid_values(UK_REQUEST)
        bound_fields = (
            "manufacturer_part_number",
            "distributor",
            "product_initial_url",
            "product_final_url",
            "authorization_authority",
            "authorization_method",
            "authorization_initial_url",
            "authorization_final_url",
        )
        for index, key in enumerate(bound_fields):
            if index:
                self.reset_fixture()
            if key.endswith("url"):
                wrong_value = "https://example.com/wrong"
            elif key == "authorization_method":
                wrong_value = "manufacturer-locator"
            else:
                wrong_value = "wrong"
            values = {**valid, key: wrong_value}
            result = self.fixture.run(
                self.fixture.record_arguments(UK_REQUEST, values)
            )
            with self.subTest(key=key):
                self.assert_failure(result, "binding")
                self.assert_no_outputs(UK_REQUEST)

        for name, option, message in (
            ("product-http", "product", "product final URL must be HTTPS"),
            (
                "authorization-http",
                "authorization",
                "authorization final URL must be HTTPS",
            ),
        ):
            self.reset_fixture()
            arguments = self.fixture.record_arguments(
                UK_REQUEST,
                {
                    **valid,
                    "product_final_url": "http://example.com/product"
                    if option == "product"
                    else PRODUCT_FINAL_URL,
                    "authorization_final_url": "http://example.com/authorization"
                    if option == "authorization"
                    else AUTHORIZATION_FINAL_URL,
                },
                product_final_url=(
                    "http://example.com/product"
                    if option == "product"
                    else PRODUCT_FINAL_URL
                ),
                authorization_final_url=(
                    "http://example.com/authorization"
                    if option == "authorization"
                    else AUTHORIZATION_FINAL_URL
                ),
            )
            result = self.fixture.run(arguments)
            with self.subTest(case=name):
                self.assert_failure(result, message)
                self.assert_no_outputs(UK_REQUEST)

    def test_supplemental_cli_usage_errors_are_exit_two(self) -> None:
        cases = (
            (["acquire", SINGLE_REQUEST], "--retrieved-at"),
            (
                [
                    "record",
                    UK_REQUEST,
                    "--values",
                    "{}",
                    "--product-final-url",
                    PRODUCT_FINAL_URL,
                    "--observed-at",
                    TIMESTAMP,
                ],
                "--authorization-final-url",
            ),
            (
                ["unavailable", SINGLE_REQUEST, "--retrieved-at", TIMESTAMP],
                "--note",
            ),
            (["check-fragment"], "request_id"),
        )
        for arguments, message in cases:
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                with self.assertRaises(SystemExit) as raised:
                    seed_evidence.run(
                        [
                            "--root",
                            os.fspath(self.fixture.root),
                            "supplemental",
                            *arguments,
                        ]
                    )
            with self.subTest(command=arguments[0]):
                self.assertEqual(raised.exception.code, 2)
                self.assertIn(message, stderr.getvalue())

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
