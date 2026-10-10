#!/usr/bin/env python3
"""eac3_acceptance.start retries only 503 fill_busy (stubbed HTTP).

The tool is a top-level script, so the test execs its source up to the
"Phase 1" marker (argparse, helpers) and leaves the device loop unrun.
"""

from __future__ import annotations

import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "tools" / "replay_samples" / "eac3_acceptance.py").read_text()
HELPERS = SOURCE.split("# Phase 1", 1)[0]

FILL_BUSY = '{"ok":false,"error":"fill_busy"}'


def load_helpers():
    scope = {"__name__": "eac3_acceptance_helpers"}
    saved = sys.argv
    with tempfile.TemporaryDirectory() as out:
        sys.argv = ["eac3_acceptance.py", "--token", "t", "--out", out]
        try:
            exec(compile(HELPERS, "eac3_acceptance.py", "exec"), scope)
        finally:
            sys.argv = saved
    return scope


class StartFillBusyTests(unittest.TestCase):
    def setUp(self):
        self.start = load_helpers()["start"]

    def _run(self, replies, budget_s=120.0):
        sleeps, now, queue = [], [0.0], list(replies)

        def sleep(seconds):
            sleeps.append(seconds)
            now[0] += seconds

        try:
            result = self.start("id", post=lambda: queue.pop(0), sleep=sleep,
                                clock=lambda: now[0], budget_s=budget_s)
        except RuntimeError as error:
            result = error
        return result, sleeps, queue

    def test_retries_then_succeeds(self):
        result, sleeps, _ = self._run([
            (503, FILL_BUSY, "5"), (503, FILL_BUSY, None), (200, "{}", None)])
        self.assertEqual(result, (200, "{}"))
        self.assertEqual(sleeps, [5.0, 1.0])

    def test_budget_exhausted_raises(self):
        result, sleeps, _ = self._run([(503, FILL_BUSY, "30")] * 10)
        self.assertIsInstance(result, RuntimeError)
        self.assertIn("fill_busy", str(result))
        self.assertEqual(sum(sleeps), 120.0)

    def test_other_503_not_retried(self):
        result, sleeps, queue = self._run([
            (503, '{"ok":false,"error":"sd_busy"}', "2"), (200, "{}", None)])
        self.assertEqual(result[0], 503)
        self.assertEqual(sleeps, [])
        self.assertEqual(len(queue), 1)


if __name__ == "__main__":
    unittest.main()
