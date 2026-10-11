#!/usr/bin/env python3
"""replay_retire.py against a fake in-process HTTP device (127.0.0.1)."""

from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import json
import pathlib
import subprocess
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "replay_retire", ROOT / "tools" / "replay_retire.py")
RR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RR)

TOKEN = "s3cret-token"


class FakeDevice:
    def __init__(self):
        self.iec = {}      # id -> bytes
        self.part = {}     # id -> size
        self.truncated = False
        self.busy = 0      # next N requests answer 503 sd_busy
        self.drain_after = 0  # status polls answering drained false
        self.never_drained = False
        self.log = []      # (method, path)
        self.tokens = []
        self.listing_size_override = {}
        self.served_bytes = {}  # id -> bytes returned instead of the capture
        self.server = None

    def rows(self):
        out = [{"id": i, "bytes": self.listing_size_override.get(i, len(b)),
                "duration_seconds": 1.0, "upload_next_offset": len(b),
                "complete": True, "active": False} for i, b in self.iec.items()]
        out += [{"id": i, "bytes": n, "duration_seconds": None,
                 "upload_next_offset": n, "complete": False, "active": False}
                for i, n in self.part.items()]
        return out

    def start(self):
        dev = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def reply(self, code, obj=None, raw=None, headers=None):
                body = raw if raw is not None else json.dumps(obj or {}).encode()
                self.send_response(code)
                for k, v in (headers or {}).items():
                    self.send_header(k, v)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def handle_any(self):
                length = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(length) if length else b""
                dev.log.append((self.command, self.path))
                dev.tokens.append(self.headers.get("X-Aius-SD-Auth"))
                if dev.busy > 0:
                    dev.busy -= 1
                    return self.reply(503, {"ok": False, "error": "sd_busy"},
                                      headers={"Retry-After": "0"})
                if self.command == "GET" and self.path == "/api/replays":
                    return self.reply(200, {"ok": True, "files": dev.rows(),
                                            "truncated": dev.truncated})
                if self.command == "GET" and self.path.startswith("/sd/replays/"):
                    ident = self.path.rsplit("/", 1)[1]
                    if ident not in dev.iec:
                        return self.reply(404, {"ok": False})
                    return self.reply(200, raw=dev.served_bytes.get(ident, dev.iec[ident]))
                if self.command == "DELETE" and self.path == "/api/replays":
                    ident = json.loads(body)["id"]
                    if ident in dev.iec:
                        del dev.iec[ident]
                    elif ident in dev.part:
                        del dev.part[ident]
                    else:
                        return self.reply(404, {"ok": False})
                    return self.reply(200, {"ok": True})
                if self.command == "DELETE" and self.path.startswith("/sd/www/"):
                    return self.reply(200, {"ok": True})
                if self.command == "GET" and self.path == "/api/status":
                    drained = not dev.never_drained and dev.drain_after <= 0
                    dev.drain_after -= 1
                    return self.reply(200, {"build_revision": "abc123def456",
                                            "sd_flush": {"drained": drained}})
                return self.reply(404, {"ok": False})

            do_GET = do_DELETE = handle_any

        self.server = HTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        return f"http://127.0.0.1:{self.server.server_port}"

    def stop(self):
        self.server.shutdown()
        self.server.server_close()


class RetireTests(unittest.TestCase):
    def setUp(self):
        self.dev = FakeDevice()
        self.url = self.dev.start()
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self.tmp.name)
        self.archive = self.dir / "archive"
        self.mpath = self.dir / "manifest" / "m.json"
        self.token_file = self.dir / "token"
        self.token_file.write_text(TOKEN + "\n")
        self.saved = (RR.sleep, RR.clock)
        self.now = [0.0]
        RR.sleep = lambda s: self.now.__setitem__(0, self.now[0] + s)
        RR.clock = lambda: self.now[0]

    def tearDown(self):
        RR.sleep, RR.clock = self.saved
        self.dev.stop()
        self.tmp.cleanup()

    def run_cli(self, *args, check="none", url=None):
        argv = ["--device", url or self.url, "--auth-file", str(self.token_file),
                "--archive-dir", str(self.archive), "--manifest", str(self.mpath),
                "--committed-check", check, *args]
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            code = RR.main(argv)
        return code, err.getvalue()

    def manifest(self):
        return json.loads(self.mpath.read_text())

    def test_truncated_listing_refuses_archive(self):
        self.dev.iec["a"] = b"xx"
        self.dev.truncated = True
        code, err = self.run_cli("archive")
        self.assertEqual(code, 1)
        self.assertIn("truncated", err)
        self.assertFalse(self.mpath.exists())

    def test_truncated_listing_refuses_delete_captures(self):
        self.dev.iec["a"] = b"xx"
        self.run_cli("archive")
        self.dev.truncated = True
        code, _ = self.run_cli("delete-captures")
        self.assertEqual(code, 1)
        self.assertNotIn(("DELETE", "/api/replays"), self.dev.log)

    def test_part_rows_recorded_never_fetched(self):
        self.dev.iec["a"] = b"hello"
        self.dev.part["b"] = 77
        self.assertEqual(self.run_cli("archive")[0], 0)
        m = self.manifest()
        self.assertEqual(m["captures"], [{"id": "a", "bytes": 5,
                                          "sha256": hashlib.sha256(b"hello").hexdigest()}])
        self.assertEqual(m["parts"], [{"name": "b.part", "id": "b", "bytes": 77}])
        self.assertEqual(m["build_revision"], "abc123def456")
        self.assertNotIn("date", json.dumps(m))
        self.assertEqual([p for _, p in self.dev.log if p.startswith("/sd/replays/")],
                         ["/sd/replays/a"])
        self.assertEqual((self.archive / "a.iec").read_bytes(), b"hello")

    def test_size_mismatch_fails(self):
        self.dev.iec["a"] = b"hello"
        self.dev.served_bytes["a"] = b"hel"
        code, err = self.run_cli("archive")
        self.assertEqual(code, 1)
        self.assertIn("3 bytes", err)

    def test_sd_busy_retried_then_success(self):
        self.dev.iec["a"] = b"hello"
        self.dev.busy = 3
        self.assertEqual(self.run_cli("archive")[0], 0)
        self.assertEqual(self.dev.log[:3], [("GET", "/api/replays")] * 3)
        self.assertEqual(self.manifest()["captures"][0]["id"], "a")

    def test_sd_busy_is_bounded(self):
        self.dev.busy = 10 ** 6
        code, err = self.run_cli("archive")
        self.assertEqual(code, 1)
        self.assertIn("sd_busy", err)

    def test_auth_header_read_from_file(self):
        self.dev.iec["a"] = b"hello"
        self.run_cli("archive")
        self.assertTrue(self.dev.tokens)
        self.assertEqual(set(self.dev.tokens), {TOKEN})

    def test_sha_mismatch_skips_iec_and_blocks_part_delete(self):
        self.dev.iec["a"] = b"aaa"
        self.dev.iec["c"] = b"ccc"
        self.dev.part["b"] = 9
        self.run_cli("archive")
        (self.archive / "a.iec").write_bytes(b"tampered")
        code, err = self.run_cli("delete-captures")
        self.assertEqual(code, 1)
        self.assertIn("remaining .iec: a", err)
        self.assertNotIn("c", err.split("remaining .iec:")[1])
        self.assertIn("a", self.dev.iec)
        self.assertNotIn("c", self.dev.iec)
        self.assertIn("b", self.dev.part)

    def test_unarchived_iec_blocks_part_delete(self):
        self.dev.iec["a"] = b"aaa"
        self.dev.part["b"] = 9
        self.run_cli("archive")
        self.dev.iec["new"] = b"zzz"
        code, err = self.run_cli("delete-captures")
        self.assertEqual(code, 1)
        self.assertIn("new", err)
        self.assertIn("b", self.dev.part)

    def test_happy_path_ends_empty(self):
        self.dev.iec["a"] = b"aaa"
        self.dev.iec["c"] = b"ccc"
        self.dev.part["b"] = 9
        self.assertEqual(self.run_cli("archive")[0], 0)
        self.assertEqual(self.run_cli("delete-captures")[0], 0)
        self.assertEqual((self.dev.iec, self.dev.part), ({}, {}))
        self.assertEqual(self.dev.rows(), [])

    def test_uncommitted_archive_is_skipped_with_git_check(self):
        self.dev.iec["a"] = b"aaa"
        self.run_cli("archive")
        # tmp dir is no git repo: manifest not committed
        code, err = self.run_cli("delete-captures", check="git")
        self.assertEqual(code, 1)
        self.assertIn("a", self.dev.iec)

    def test_size_differs_from_manifest_is_skipped(self):
        self.dev.iec["a"] = b"aaa"
        self.dev.part["b"] = 9
        self.run_cli("archive")
        self.dev.listing_size_override["a"] = 999
        code, err = self.run_cli("delete-captures")
        self.assertEqual(code, 1)
        self.assertIn("card size 999", err)
        self.assertIn("a", self.dev.iec)
        self.assertIn("b", self.dev.part)

    def test_unreachable_device_is_an_error_line(self):
        code, err = self.run_cli("archive", url="http://127.0.0.1:1")
        self.assertEqual(code, 1)
        self.assertIn("error:", err)

    def test_archive_refuses_existing_manifest_without_force(self):
        self.dev.iec["a"] = b"aaa"
        self.assertEqual(self.run_cli("archive")[0], 0)
        code, err = self.run_cli("archive")
        self.assertEqual(code, 1)
        self.assertIn("--force", err)
        self.assertEqual(self.run_cli("archive", "--force")[0], 0)

    # --- committed check, in a throwaway git repo -------------------------

    def git(self, *args):
        subprocess.run(["git", "-C", str(self.dir), "-c", "user.name=t",
                        "-c", "user.email=t@example.com", "-c", "commit.gpgsign=false",
                        *args], check=True, capture_output=True)

    def git_repo_archive(self, *, commit):
        """Archive into a git repo at self.dir; commit the given paths."""
        self.git("init", "-q")
        self.run_cli("archive")
        self.git("add", "--", *commit)
        self.git("commit", "-q", "-m", "fixtures")

    def test_git_clean_committed_passes(self):
        self.dev.iec["a"] = b"aaa"
        self.dev.part["b"] = 9
        self.git_repo_archive(commit=["archive", "manifest"])
        self.assertEqual(self.run_cli("delete-captures", check="git")[0], 0)
        self.assertEqual((self.dev.iec, self.dev.part), ({}, {}))

    def test_git_staged_not_committed_is_skipped(self):
        self.dev.iec["a"] = b"aaa"
        self.dev.iec["c"] = b"ccc"
        self.git_repo_archive(commit=["manifest", "archive/a.iec"])
        self.git("add", "--", "archive/c.iec")
        code, err = self.run_cli("delete-captures", check="git")
        self.assertEqual(code, 1)
        self.assertIn("skip c: archive not committed", err)
        self.assertNotIn("a", self.dev.iec)
        self.assertIn("c", self.dev.iec)

    def test_git_modified_after_commit_is_skipped(self):
        self.dev.iec["a"] = b"aaa"
        self.dev.iec["c"] = b"ccc"
        self.git_repo_archive(commit=["archive", "manifest"])
        (self.archive / "c.iec").write_bytes(b"ccc")  # same bytes: still clean
        (self.archive / "a.iec").write_bytes(b"changed")
        code, err = self.run_cli("delete-captures", check="git")
        self.assertEqual(code, 1)
        self.assertIn("skip a: archive not committed", err)
        self.assertIn("a", self.dev.iec)
        self.assertNotIn("c", self.dev.iec)

    def test_git_reuploaded_same_id_rearchived_is_refused(self):
        self.dev.iec["x"] = b"old"
        self.git_repo_archive(commit=["archive", "manifest"])
        self.dev.iec["x"] = b"new-bytes"  # card X re-uploaded under the same id
        self.assertEqual(self.run_cli("archive", "--force")[0], 0)
        code, err = self.run_cli("delete-captures", check="git")
        self.assertEqual(code, 1)
        self.assertIn("not committed", err)
        self.assertEqual(self.dev.iec["x"], b"new-bytes")

    def test_delete_www_polls_until_drained(self):
        self.dev.drain_after = 3
        self.assertEqual(self.run_cli("delete-www")[0], 0)
        deletes = [p for m, p in self.dev.log if m == "DELETE"]
        self.assertEqual(deletes, ["/sd/www/replays.html", "/sd/www/replays.js",
                                   "/sd/www/replays.css", "/sd/www/replays.html.gz"])
        self.assertEqual(sum(p == "/api/status" for _, p in self.dev.log), 4)

    def test_delete_www_times_out(self):
        self.dev.never_drained = True
        code, err = self.run_cli("delete-www")
        self.assertEqual(code, 1)
        self.assertIn("drained", err)


if __name__ == "__main__":
    unittest.main()
