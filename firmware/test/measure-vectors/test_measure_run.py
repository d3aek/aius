#!/usr/bin/env python3
"""measure_run: U24 aplay arming, one looped render, --baseline pass rule.

Subprocess and HTTP are mocked; nothing here touches the device or the U24."""

from __future__ import annotations

import contextlib
import copy
import importlib.util
import io
import pathlib
import threading
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "measure_run", ROOT / "tools" / "measure_vectors" / "measure_run.py")
MR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MR)

TRACKING = {"tx_gate": {"open": 1}, "clock": {"state": "tracking"}}


class FakeStdin:
    def __init__(self, log):
        self.log, self.writes = log, 0
        self.first = threading.Event()

    def write(self, data):
        self.writes += 1
        self.log.append(("write", len(data)))
        self.first.set()
        if self.writes > 3:
            raise BrokenPipeError

    def flush(self):
        pass


class FakeProc:
    def __init__(self, argv, log):
        self.argv, self.stdin, self.terminated = argv, FakeStdin(log), False

    def poll(self):
        return 0 if self.terminated else None

    def terminate(self):
        self.terminated = True

    def wait(self, timeout=None):
        return 0

    def kill(self):
        self.terminated = True


class Rig:
    """Records the ordered subprocess and HTTP calls of a run."""

    def __init__(self, mixer_readback="  : values=110,110\n", status=None,
                 session=None):
        self.log, self.procs = [], []
        self.mixer_readback = mixer_readback
        self.status = status or TRACKING
        self.session = session

    def run(self, argv, **kw):
        self.log.append(("run", tuple(argv)))
        out = self.mixer_readback if "cget" in argv else ""
        return mock.Mock(stdout=out, returncode=0)

    def popen(self, argv, **kw):
        self.log.append(("popen", tuple(argv)))
        proc = FakeProc(argv, self.log)
        self.procs.append(proc)
        return proc

    def call_json(self, method, path, obj=None):
        self.log.append(("http", method, path))
        if path == "/api/status":
            return 200, self.status
        if path == "/api/measure" and method == "GET":
            return 200, self.session
        return 200, {"ok": True}

    def patch(self):
        stack = [
            mock.patch.object(MR.subprocess, "run", self.run),
            mock.patch.object(MR.subprocess, "Popen", self.popen),
            mock.patch.object(MR, "call_json", self.call_json),
            mock.patch.object(MR.time, "sleep", lambda s: None),
        ]
        for p in stack:
            p.start()
        return lambda: [p.stop() for p in stack]

    def kinds(self):
        return [e[0] + ":" + (e[1][0] if e[0] != "http" else e[2])
                if e[0] != "write" else "write" for e in self.log]


def done_session(**over):
    ch = {"thd_n_db": -90.0, "snr_db": 90.0, "thd_db": -92.0,
          "probes_dbfs": []}
    session = {"state": "done", "fail_reason": "none",
               "figures": {"l": dict(ch), "r": dict(ch)},
               "floors": {"thd_n_db": -96.0, "snr_db": 96.0,
                          "thd_dbc": -95.0}}
    session.update(over)
    return session


class ArmingTests(unittest.TestCase):
    def test_order_mixer_render_wait_start_stop(self):
        rig = Rig(session=done_session())
        undo = rig.patch()
        self.addCleanup(undo)
        rc, session = MR.measure("pcm440_m6", 440, 440, [])
        self.assertEqual(rc, 0)
        self.assertEqual(session["state"], "done")
        ev = [e for e in rig.log if e[0] != "write"]
        self.assertEqual(ev[0], ("run", ("amixer", "-c", "0", "cset",
                                         "numid=5", "110,110")))
        self.assertEqual(ev[1], ("run", ("amixer", "-c", "0", "cget",
                                         "numid=5")))
        self.assertEqual(ev[2][0:1], ("popen",))
        argv = ev[2][1]
        self.assertEqual(argv[:3], ("aplay", "-D", "hw:U24XL,0"))
        self.assertIn("-", argv)
        status_at = ev.index(("http", "GET", "/api/status"))
        start_at = ev.index(("http", "POST", "/api/measure/start"))
        self.assertTrue(2 < status_at < start_at)
        self.assertTrue(rig.procs[0].terminated)

    def test_one_looped_render_and_no_replay_calls(self):
        rig = Rig(session=done_session())
        self.addCleanup(rig.patch())
        MR.measure("pcm440_m6", 440, 440, [])
        self.assertEqual(len(rig.procs), 1)  # never an aplay re-run per loop
        proc = rig.procs[0]
        self.assertTrue(proc.stdin.first.wait(5))
        blob = (MR.VECTORS / "pcm440_m6.iec").read_bytes()
        self.assertEqual(MR.FEED_LOOPS_PER_WRITE * len(blob),
                         [e for e in rig.log if e[0] == "write"][0][1])
        self.assertFalse([e for e in rig.log
                          if e[0] == "http" and "replays" in e[2]])
        self.assertFalse(hasattr(MR, "replay_start"))
        self.assertFalse(hasattr(MR, "upload"))

    def test_mixer_not_read_back_aborts_before_render(self):
        rig = Rig(mixer_readback="  : values=70,70\n")
        self.addCleanup(rig.patch())
        rc, session = MR.measure("pcm440_m6", 440, 440, [])
        self.assertEqual((rc, session), (1, None))
        self.assertEqual(rig.procs, [])
        self.assertNotIn(("http", "POST", "/api/measure/start"), rig.log)

    def test_not_tracking_times_out_and_stops_render(self):
        rig = Rig(status={"tx_gate": {"open": 0}, "clock": {"state": "acquiring"}})
        self.addCleanup(rig.patch())
        now = [0.0]

        def clock():
            now[0] += 5.0
            return now[0]

        with mock.patch.object(MR.time, "monotonic", clock):
            with self.assertRaises(RuntimeError):
                MR.wait_tracking(sleep=lambda s: None, clock=clock)
            rc, session = MR.measure("pcm440_m6", 440, 440, [])
        self.assertEqual((rc, session), (1, None))
        self.assertTrue(rig.procs[0].terminated)
        self.assertNotIn(("http", "POST", "/api/measure/start"), rig.log)

    def test_failed_run_still_stops_render(self):
        rig = Rig(session=done_session(state="failed", fail_reason="x"))
        self.addCleanup(rig.patch())
        rc, _ = MR.measure("pcm440_m6", 440, 440, [])
        self.assertEqual(rc, 1)
        self.assertTrue(rig.procs[0].terminated)

    def test_alsa_device_override(self):
        rig = Rig(session=done_session())
        self.addCleanup(rig.patch())
        with mock.patch.object(MR, "ALSA_DEVICE", "hw:X,0"):
            MR.measure("pcm440_m6", 440, 440, [])
        self.assertIn("hw:X,0", rig.procs[0].argv)


class BaselineTests(unittest.TestCase):
    def cmp(self, base, run):
        return MR.compare_session("v", base, run)

    def test_identical_passes(self):
        self.assertEqual(self.cmp(done_session(), done_session()), [])

    def test_within_one_db_passes_beyond_fails(self):
        run = done_session()
        run["figures"]["l"]["thd_n_db"] = -89.1
        self.assertEqual(self.cmp(done_session(), run), [])
        run["figures"]["l"]["thd_n_db"] = -88.9
        self.assertEqual(len(self.cmp(done_session(), run)), 1)

    def test_each_figure_checked(self):
        for key, value in (("snr_db", 88.9), ("thd_db", -90.9)):
            run = done_session()
            run["figures"]["r"][key] = value
            self.assertEqual(len(self.cmp(done_session(), run)), 1, key)

    def test_both_at_floor_counts_equal(self):
        base, run = done_session(), done_session()
        base["figures"]["l"].update(thd_n_db=-96.0, snr_db=96.0, thd_db=-99.0)
        run["figures"]["l"].update(thd_n_db=-98.5, snr_db=97.9, thd_db=-95.2)
        self.assertEqual(self.cmp(base, run), [])
        run["figures"]["l"]["thd_n_db"] = -90.0  # one side off the floor
        self.assertEqual(len(self.cmp(base, run)), 1)

    def test_fail_reason_and_floors(self):
        self.assertTrue(self.cmp(done_session(),
                                 done_session(fail_reason="overrun")))
        other = done_session()
        other["floors"]["thd_n_db"] = -97.0
        self.assertTrue(self.cmp(done_session(), other))

    def test_probes_within_point_one(self):
        base, run = done_session(), done_session()
        base["figures"]["l"]["probes_dbfs"] = [-18.0, -18.1]
        run["figures"]["l"]["probes_dbfs"] = [-18.1, -18.0]
        self.assertEqual(self.cmp(base, run), [])
        run["figures"]["l"]["probes_dbfs"] = [-18.0, -18.25]
        self.assertEqual(len(self.cmp(base, run)), 1)
        run["figures"]["l"]["probes_dbfs"] = [-18.0]
        self.assertEqual(len(self.cmp(base, run)), 1)

    def test_suite_gates_on_isr_cost(self):
        baseline = {v: done_session() for v in MR.SUITE}
        for isr_rc, want in ((0, 0), (1, 1)):
            with mock.patch.object(MR, "measure",
                                   lambda v, *a: (0, copy.deepcopy(done_session()))), \
                    mock.patch.object(MR, "isr_cost", lambda: isr_rc):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(MR.suite(baseline), want)

    def test_suite_fails_on_figure_drift(self):
        baseline = {v: done_session() for v in MR.SUITE}
        drift = done_session()
        drift["figures"]["l"]["thd_db"] = -80.0
        with mock.patch.object(MR, "measure", lambda v, *a: (0, drift)), \
                mock.patch.object(MR, "isr_cost", lambda: 0):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(MR.suite(baseline), 1)


if __name__ == "__main__":
    unittest.main()
