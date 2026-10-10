#!/usr/bin/env python3
"""measure_run.replay_start retries only 503 fill_busy (stubbed HTTP)."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "measure_run", ROOT / "tools" / "measure_vectors" / "measure_run.py")
MR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MR)

FILL_BUSY = json.dumps({"ok": False, "error": "fill_busy"}).encode()


class ReplayStartFillBusyTests(unittest.TestCase):
    def _run(self, replies, budget_s=120.0):
        sleeps, now, queue = [], [0.0], list(replies)

        def sleep(seconds):
            sleeps.append(seconds)
            now[0] += seconds

        try:
            result = MR.replay_start("v", post=lambda: queue.pop(0),
                                     sleep=sleep, clock=lambda: now[0],
                                     budget_s=budget_s)
        except RuntimeError as error:
            result = error
        return result, sleeps, queue

    def test_retries_then_succeeds(self):
        result, sleeps, _ = self._run([
            (503, FILL_BUSY, {"Retry-After": "4"}),
            (503, FILL_BUSY, {}),
            (200, b'{"ok":true}', {})])
        self.assertEqual(result, (200, {"ok": True}))
        self.assertEqual(sleeps, [4.0, 1.0])

    def test_budget_exhausted_raises(self):
        result, sleeps, _ = self._run(
            [(503, FILL_BUSY, {"Retry-After": "30"})] * 10)
        self.assertIsInstance(result, RuntimeError)
        self.assertIn("fill_busy", str(result))
        self.assertEqual(sum(sleeps), 120.0)

    def test_other_503_not_retried(self):
        result, sleeps, queue = self._run([
            (503, b'{"ok":false,"error":"sd_busy"}', {"Retry-After": "2"}),
            (200, b"{}", {})])
        self.assertEqual(result[0], 503)
        self.assertEqual(sleeps, [])
        self.assertEqual(len(queue), 1)


if __name__ == "__main__":
    unittest.main()
