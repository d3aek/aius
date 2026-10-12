#!/usr/bin/env python3
"""Host suite for tools/dsp_config/aius_dsp_set.py: the PUT body it builds, its
refusals, --wait and exit codes, over an injected transport (no socket, no
board). The GET body is a real live capture (fixtures/)."""
from __future__ import annotations

import contextlib
import copy
import io
import http.server
import json
import math
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "tools" / "dsp_config"))

import aius_dsp_set as tool  # noqa: E402

LIVE = json.loads((HERE / "fixtures" / "dsp_config_live_2026-10-07.json").read_text())


SENTINEL = "TOKEN-SENTINEL-7f3a"


class Clock:
    def __init__(self):
        self.t = 0.0
        self.sleeps = 0

    def now(self):
        return self.t

    def sleep(self, seconds):
        self.t += seconds
        self.sleeps += 1


def fake(live=None, put_status=200, put_text="{}", after=None):
    """GET answers live first, then each entry of `after` (last one repeats)."""
    calls = []
    gets = [copy.deepcopy(LIVE if live is None else live)]
    gets += after or []

    def transport(method, url, headers, body, timeout):
        calls.append((method, url, headers, body))
        if method == "GET":
            item = gets.pop(0) if len(gets) > 1 else gets[0]
            return 200, item if isinstance(item, str) else json.dumps(item)
        return put_status, put_text

    return transport, calls


def run(sets, dry=False, wait=0, live=None, after=None, **kw):
    transport, calls = fake(live=live, after=after, **kw)
    out, err, clock = io.StringIO(), io.StringIO(), Clock()
    code = tool.run("http://x", "tok", [tool.parse_assignment(s) for s in sets],
                    dry, transport, out, err, wait=wait, sleep=clock.sleep,
                    now=clock.now)
    return code, calls, out.getvalue(), err.getvalue(), clock


def with_state(generation, pending, **extra):
    cfg = copy.deepcopy(LIVE)
    cfg.update(config_generation=generation, config_pending=pending, **extra)
    return cfg


def applied(generation, pending, **extra):
    """The state after our `features.limiter=true` landed."""
    cfg = with_state(generation, pending, **extra)
    cfg["features"]["limiter"] = True
    return cfg


class DspSetTest(unittest.TestCase):
    def test_only_named_paths_change_and_generation_is_carried(self):
        code, calls, out, _, _ = run(["features.limiter=true",
                                      "input_channels.fc.eq.0.q=0.5"])
        self.assertEqual(code, 0)
        method, _, headers, body = [c for c in calls if c[0] == "PUT"][0]
        self.assertEqual(headers["X-Aius-SD-Auth"], "tok")
        sent = json.loads(body)
        self.assertEqual(sent.pop("base_generation"), LIVE["config_generation"])
        expected = copy.deepcopy(LIVE)
        expected["features"]["limiter"] = True
        expected["input_channels"]["fc"]["eq"][0]["q"] = 0.5
        self.assertEqual(sent, expected)
        self.assertNotIn("tok", out)

    def test_unknown_path_refused_without_put(self):
        for path in ("features.nope=1", "nope=1", "input_channels.fc.eq.3.q=1"):
            code, calls, _, err, _ = run([path])
            self.assertEqual(code, 2)
            self.assertEqual([c[0] for c in calls], ["GET"])
            self.assertIn("unknown path", err)

    def test_non_json_and_non_finite_values_are_usage_errors(self):
        for text in ("features.limiter=maybe", "matrix.center_gain_linear=NaN",
                     "matrix.center_gain_linear=Infinity",
                     "matrix.center_gain_linear=-Infinity",
                     "matrix.center_gain_linear=1e999",
                     "matrix.center_gain_linear=[1e999]"):
            with self.assertRaises(tool.UsageError, msg=text):
                tool.parse_assignment(text)

    def test_device_refusals_exit_1_with_body(self):
        for status in (409, 400, 503):
            code, _, _, err, _ = run(["features.limiter=true"], put_status=status,
                                     put_text='{"error":"x"}')
            self.assertEqual(code, 1)
            self.assertIn('{"error":"x"}', err)

    def test_dry_run_prints_only_changed_paths_and_never_puts(self):
        code, calls, out, _, _ = run(["features.limiter=true"], dry=True)
        self.assertEqual(code, 0)
        self.assertEqual([c[0] for c in calls], ["GET"])
        self.assertEqual(out.strip(), "features.limiter: false -> true")

    def test_pending_at_initial_get_refuses_naming_staged_generation(self):
        pending = with_state(1, True, staged_generation=9)
        code, calls, _, err, _ = run(["features.limiter=true"], live=pending)
        self.assertEqual(code, 2)
        self.assertEqual([c[0] for c in calls], ["GET"])
        self.assertIn("staged_generation=9", err)

    def test_dry_run_with_pending_prints_diff_and_warns(self):
        pending = with_state(1, True, staged_generation=9)
        code, calls, out, err, _ = run(["features.limiter=true"], dry=True, live=pending)
        self.assertEqual(code, 0)
        self.assertEqual([c[0] for c in calls], ["GET"])
        self.assertIn("features.limiter: false -> true", out)
        self.assertIn("staged_generation=9", err)

    def test_without_wait_prints_post_put_state_once(self):
        after = [with_state(1, True, staged_generation=2)]
        code, calls, out, _, clock = run(["features.limiter=true"], after=after)
        self.assertEqual(code, 0)
        self.assertEqual([c[0] for c in calls], ["GET", "PUT", "GET"])
        self.assertIn("config_generation=1 config_pending=true", out)
        self.assertEqual(clock.sleeps, 0)

    def test_wait_polls_until_generation_advances_and_pending_clears(self):
        after = [with_state(1, True), with_state(1, True),
                 with_state(2, True), applied(2, False)]
        code, calls, out, _, clock = run(["features.limiter=true"], wait=30, after=after)
        self.assertEqual(code, 0)
        self.assertEqual([c[0] for c in calls], ["GET", "PUT", "GET", "GET", "GET", "GET"])
        self.assertEqual(clock.sleeps, 3)
        self.assertTrue(out.strip().endswith("config_generation=2 config_pending=false"))

    def test_wait_timeout_exits_3_saying_committed_not_applied(self):
        after = [with_state(1, True)]
        code, calls, _, err, clock = run(["features.limiter=true"], wait=3, after=after)
        self.assertEqual(code, 3)
        self.assertIn("committed to the card", err)
        self.assertIn("not yet applied", err)
        self.assertEqual(clock.sleeps, 3)

    def test_exit_3_help_and_timeout_message_name_no_carrier(self):
        code, _, _, err, _ = run(["features.limiter=true"], wait=3,
                                 after=[with_state(1, True)])
        self.assertEqual(code, 3)
        self.assertNotIn("carrier", err.lower())
        self.assertIn("service epoch", err)
        out = io.StringIO()
        with mock.patch.object(sys, "argv", ["aius_dsp_set.py", "--help"]), \
                contextlib.redirect_stdout(out), self.assertRaises(SystemExit):
            tool.main()
        self.assertNotIn("carrier", out.getvalue().lower())

    def test_wait_applied_other_clients_save_exits_1(self):
        other = with_state(2, False)  # limiter still false: not what we sent
        code, _, _, err, _ = run(["features.limiter=true"], wait=30, after=[other])
        self.assertEqual(code, 1)
        self.assertIn("another client's save", err)

    def test_wait_applied_tolerates_one_ulp(self):
        sets = ["matrix.center_gain_linear=1.2"]
        nudged = applied(2, False)
        nudged["features"]["limiter"] = False
        nudged["matrix"]["center_gain_linear"] = math.nextafter(1.2, 2)
        code, *_ = run(sets, wait=30, after=[nudged])
        self.assertEqual(code, 0)
        nudged["matrix"]["center_gain_linear"] = 1.3
        code, *_ = run(sets, wait=30, after=[nudged])
        self.assertEqual(code, 1)

    def test_wait_timeout_when_board_never_answers(self):
        calls = []

        def transport(method, url, headers, body, timeout):
            calls.append(method)
            if method == "GET" and calls.count("GET") == 1:
                return 200, json.dumps(LIVE)
            if method == "PUT":
                return 200, "{}"
            return None, "timed out"

        out, err, clock = io.StringIO(), io.StringIO(), Clock()
        code = tool.run("http://x", "tok", [tool.parse_assignment("features.limiter=true")],
                        False, transport, out, err, wait=2, sleep=clock.sleep, now=clock.now)
        self.assertEqual(code, 3)
        self.assertIn("board did not answer", err.getvalue())
        self.assertNotIn("no carrier", err.getvalue())

    def test_bad_wait_rejected_at_parse_exit_2(self):
        for value in ("nan", "inf", "-inf", "=-1"):
            flag = ["--wait" + value] if value.startswith("=") else ["--wait", value]
            argv = ["aius_dsp_set.py", "--device", "http://x", "--auth-file", "a",
                    "--set", "features.limiter=true", *flag]
            with mock.patch.object(sys, "argv", argv), \
                    contextlib.redirect_stderr(io.StringIO()), \
                    self.assertRaises(SystemExit) as raised:
                tool.main()
            self.assertEqual(raised.exception.code, 2, value)

    def test_bad_auth_files_exit_2_without_leaking_token(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name, content in (("empty", "\n"), ("inner-lf", f"{SENTINEL}\nx\n"),
                                  ("inner-cr", f"{SENTINEL}\rx"),
                                  ("ctl", f"{SENTINEL}\x01x")):
                path = Path(tmp) / name
                path.write_text(content, encoding="utf-8")
                argv = ["aius_dsp_set.py", "--device", "http://x", "--auth-file",
                        str(path), "--set", "features.limiter=true"]
                err = io.StringIO()
                with mock.patch.object(sys, "argv", argv), contextlib.redirect_stderr(err):
                    self.assertEqual(tool.main(), 2, name)
                self.assertNotIn(SENTINEL, err.getvalue(), name)

    def test_transport_echoes_only_the_exception_class(self):
        boom = ValueError(f"Invalid header value b'{SENTINEL}'")
        with mock.patch.object(tool.urllib.request, "urlopen", side_effect=boom):
            status, text = tool.urllib_transport("PUT", "http://x/y",
                                                 {"X-Aius-SD-Auth": SENTINEL}, b"", 1)
        self.assertIsNone(status)
        self.assertNotIn(SENTINEL, text)
        self.assertIn("ValueError", text)

    def test_reboot_generation_restart_is_success(self):
        code, *_ = run(["features.limiter=true"], wait=30,
                       live=with_state(5, False), after=[applied(1, False)])
        self.assertEqual(code, 0)

    def test_source_default_to_sd_is_not_a_mismatch(self):
        code, *_ = run(["features.limiter=true"], wait=30,
                       live=with_state(1, False, source="default"),
                       after=[applied(2, False, source="sd")])
        self.assertEqual(code, 0)

    def test_clipfree_applied_headroom_is_compared(self):
        live = with_state(1, False)
        live["matrix"]["clipfree"] = True
        done = applied(2, False)
        done["matrix"]["clipfree"] = True
        done["matrix"]["headroom_linear"] = 0.71  # differs from the stored pad
        code, *_ = run(["features.limiter=true"], wait=30, live=live, after=[done])
        self.assertEqual(code, 1)
        done["matrix"]["headroom_linear"] = live["matrix"]["headroom_linear"]
        code, *_ = run(["features.limiter=true"], wait=30, live=live, after=[done])
        self.assertEqual(code, 0)

    def test_pre_4t_board_serving_resolved_clipfree_headroom_is_tolerated(self):
        live = with_state(1, False)
        live["matrix"]["clipfree"] = True
        done = applied(2, False)
        m = done["matrix"]
        m["clipfree"] = True
        denom = (1.0 + m["center_gain_linear"] + m["surround_gain_linear"]
                 + abs(m["lfe_gain_linear"]))
        resolved = 1.0 if denom <= 1.0 else 1.0 / denom
        self.assertNotAlmostEqual(resolved, live["matrix"]["headroom_linear"])
        m["headroom_linear"] = resolved
        code, *_ = run(["features.limiter=true"], wait=30, live=live, after=[done])
        self.assertEqual(code, 0)
        # Not tolerated when clipfree is off in the applied config.
        m["clipfree"] = False
        live["matrix"]["clipfree"] = False
        code, *_ = run(["features.limiter=true"], wait=30, live=live, after=[done])
        self.assertEqual(code, 1)

    def test_wait_success_with_real_lfe_float_drift(self):
        self.assertEqual(LIVE["matrix"]["lfe_gain_linear"], 0.3162277660168385)
        done = applied(2, False)
        done["matrix"]["lfe_gain_linear"] = 0.31622776601683855
        code, *_ = run(["features.limiter=true"], wait=30, after=[done])
        self.assertEqual(code, 0)

    def test_wait_tolerates_multi_ulp_decoder_drift_but_flags_real_changes(self):
        cases = (("matrix.lfe_gain_linear=0.03", 0.030000000000000006, 0),
                 ("matrix.lfe_gain_linear=1e-5", 1e-5, 0),
                 ("matrix.center_gain_linear=1.5", 1.5000001, 1))
        for sets, got, expected in cases:
            if sets.endswith("1e-5"):
                for _ in range(5):
                    got = math.nextafter(got, 1)  # 5 ulps off a value near 1e-5
            done = applied(2, False)
            done["features"]["limiter"] = False
            done["matrix"][sets.split("=")[0].split(".")[1]] = got
            code, *_ = run([sets], wait=30, after=[done])
            self.assertEqual(code, expected, sets)

    def test_failed_put_with_pending_or_matching_config_counts_as_saved(self):
        for after in ([applied(1, True)], [applied(2, False)]):
            code, calls, out, err, _ = run(["features.limiter=true"],
                                           put_status=None, put_text="reset",
                                           after=after)
            self.assertEqual(code, 0)
            self.assertIn("the save landed", out)

    def test_failed_put_with_pending_mismatch_is_another_clients_save(self):
        other = with_state(1, True)  # staged model lacks our limiter=true
        other["matrix"]["center_gain_linear"] = 0.9
        code, _, _, err, _ = run(["features.limiter=true"], put_status=None,
                                 put_text="reset", after=[other])
        self.assertEqual(code, 1)
        self.assertIn("another client's save is staged; ours did not land", err)

    def test_timeout_with_different_applied_config_names_it_not_carrier(self):
        rebooted = with_state(1, False)  # same generation as base, limiter false
        code, _, _, err, _ = run(["features.limiter=true"], wait=2, after=[rebooted])
        self.assertEqual(code, 3)
        self.assertIn("running a different config than ours", err)
        self.assertIn("features.limiter", err)
        self.assertNotIn("no carrier", err)

    def test_mismatch_message_lists_up_to_five_paths_with_values(self):
        other = applied(2, False)
        for key in ("center_gain_linear", "surround_gain_linear", "lfe_gain_linear",
                    "lfe_crossover_hz", "headroom_linear", "surround_delay_ms"):
            other["matrix"][key] = 77
        code, _, _, err, _ = run(["features.limiter=true"], wait=30, after=[other])
        self.assertEqual(code, 1)
        self.assertIn("matrix.center_gain_linear: sent 1.5, applied 77", err)
        self.assertEqual(err.count("applied 77"), 5)
        self.assertIn("and 1 more", err)

    def test_failed_put_with_unchanged_config_is_outcome_unknown(self):
        code, _, _, err, _ = run(["features.limiter=true"], put_status=None,
                                 put_text="reset", after=[with_state(1, False)])
        self.assertEqual(code, 1)
        self.assertIn("outcome unknown, re-run --dry-run to check", err)

    def test_real_transport_prints_409_body(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def _send(self, status, text):
                data = text.encode()
                self.send_response(status)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                self._send(200, json.dumps(LIVE))

            def do_PUT(self):
                self.rfile.read(int(self.headers["Content-Length"]))
                self._send(409, '{"error":"stale_generation"}')

            def log_message(self, *args):
                pass

        server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            base = f"http://127.0.0.1:{server.server_address[1]}"
            status, text = tool.urllib_transport("PUT", base + tool.ROUTE, {}, b"{}", 5)
            self.assertEqual((status, text), (409, '{"error":"stale_generation"}'))
            out, err = io.StringIO(), io.StringIO()
            code = tool.run(base, "tok", [tool.parse_assignment("features.limiter=true")],
                            False, out=out, err=err)
            self.assertEqual(code, 1)
            self.assertIn('{"error":"stale_generation"}', err.getvalue())
        finally:
            server.shutdown()
            server.server_close()

    def test_non_json_get_exits_2_cleanly(self):
        code, calls, _, err, _ = run(["features.limiter=true"], live="<html>nope")
        self.assertEqual(code, 2)
        self.assertEqual([c[0] for c in calls], ["GET"])
        self.assertIn("non-JSON", err)

    def test_missing_auth_file_exits_2_cleanly(self):
        argv = ["aius_dsp_set.py", "--device", "http://x", "--auth-file",
                str(HERE / "no-such-auth-file"), "--set", "features.limiter=true"]
        err = io.StringIO()
        with mock.patch.object(sys, "argv", argv), contextlib.redirect_stderr(err):
            self.assertEqual(tool.main(), 2)
        self.assertIn("cannot read --auth-file", err.getvalue())

    def test_help_documents_exit_codes_and_known_limits(self):
        out = io.StringIO()
        with mock.patch.object(sys, "argv", ["aius_dsp_set.py", "--help"]), \
                contextlib.redirect_stdout(out), self.assertRaises(SystemExit):
            tool.main()
        for needle in ("Exit codes", "clipfree", "-0", "3  --wait"):
            self.assertIn(needle, out.getvalue())


if __name__ == "__main__":
    unittest.main()
