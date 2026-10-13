#!/usr/bin/env python3
"""Dry-run suite for tools/ota/aius_ota_flash.py against a state-machine fake
(aius-ota-resilience.md Phase 4b's targeted proof).

The fake mirrors the firmware contract this client depends on: the strict
next-offset chunk protocol with the DEFERRED absorb (a faulted write does not
move the session), the verify -> arm order, and activation's gates. Every
scenario drives the real client code with an injected transport -- no socket,
no board.

Run directly, or through scripts/test.sh --suite ota.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "ota"))

import aius_ota_flash  # noqa: E402


class FakeDevice:
    """The firmware's OTA surface, at the fidelity the client depends on.

    Staging models the Phase 4a contract: a chunk faulted by `fault_at`
    answers its status WITHOUT advancing next_offset (the deferred absorb) --
    unless `landed` marks the write as having committed with the answer lost,
    the may-in-fact-have-landed case the resume protocol exists for.
    """

    def __init__(self, total, fault_plan=None):
        self.total = total
        self.offset = 0
        self.state = "idle"
        self.chunk_log = []
        self.verbs = []
        self.activation_enabled = False
        # fault_plan: {chunk_index: (status, landed)}
        self.fault_plan = dict(fault_plan or {})
        self.puts = 0
        # Phase 4h: sd_flush drain model. `dirty_polls` /api/status reads
        # report dirty 1 before the queue drains to 0 (None = never drains);
        # activate answers 409 sd_flush_pending while dirty.
        self.dirty_polls = 0
        self.flush_status_reads = 0
        self.activate_posts = 0
        self.rehearse_posts = 0
        self.activation_fault = "none"

    @property
    def dirty(self):
        return self.dirty_polls is None or self.flush_status_reads < self.dirty_polls

    def transport(self, method, url, headers, body, timeout):
        path = url.split("://", 1)[-1].split("/", 1)[1]
        if method == "GET" and path == "api/status":
            self.flush_status_reads += 1
            return 200, {"sd_flush": {"dirty": 1 if self.dirty else 0},
                         "ota_diag": {"activation_fault": self.activation_fault}}
        if method == "GET" and path == "api/ota":
            return 200, {
                "state": self.state,
                "next_offset": self.offset,
                "staged_bytes": self.offset,
                "total_bytes": self.total,
            }
        if method == "PUT" and path == "api/ota/image":
            return self._put(headers, body)
        if method == "POST" and path.startswith("api/ota/"):
            return self._post(path.split("/")[-1])
        return 404, {"error": "route"}

    def _put(self, headers, body):
        self.puts += 1
        start = int(headers["Content-Range"].split()[1].split("-")[0])
        plan = self.fault_plan.pop(self.puts, None)
        if plan is not None:
            status, landed = plan
            if landed:
                # The write committed but the answer was lost: the session
                # absorbed at the terminal, so next_offset moved.
                self._absorb(start, len(body))
            return status, {"error": "injected"}
        if self.state not in ("idle", "staging") and start != 0:
            return 409, {"error": "ota_state"}
        if start != self.offset:
            return 409, {"error": "ota_range", "next_offset": self.offset}
        self._absorb(start, len(body))
        return 200, {"ok": True, "next_offset": self.offset}

    def _absorb(self, start, length):
        assert start == self.offset, "fake absorbed out of order"
        self.offset = start + length
        self.chunk_log.append((start, length))
        self.state = "staged" if self.offset == self.total else "staging"

    def _post(self, verb):
        self.verbs.append(verb)
        if verb == "verify":
            if self.state != "staged":
                return 409, {"error": "ota_state"}
            self.state = "verified"
            return 200, {"ok": True}
        if verb == "arm":
            if self.state != "verified":
                return 409, {"error": "ota_state"}
            self.state = "armed"
            return 200, {"ok": True}
        if verb == "enable-activation":
            self.activation_enabled = True
            return 200, {"ok": True}
        if verb == "rehearse":
            self.rehearse_posts += 1
            if self.dirty_polls != 0 and self.dirty:
                return 409, {"error": "sd_flush_pending"}
            if not self.activation_enabled:
                return 409, {"error": "ota_activation_disabled"}
            if self.state != "armed":
                return 409, {"error": "ota_state"}
            return 200, {"ok": True, "rehearsing": True}
        if verb == "activate":
            self.activate_posts += 1
            if self.dirty_polls != 0 and self.dirty:
                return 409, {"error": "sd_flush_pending"}
            if not self.activation_enabled:
                return 409, {"error": "ota_activation_disabled"}
            if self.state != "armed":
                return 409, {"error": "ota_state"}
            self.state = "activation-requested"
            return 200, {"ok": True, "activating": True}
        return 404, {"error": "route"}


def client_over(fake):
    return aius_ota_flash.Device("http://fake", "token",
                                 transport=fake.transport)


def quiet(*_args, **_kwargs):
    pass


class StageTests(unittest.TestCase):
    IMAGE = bytes(range(256)) * 40  # 10,240 B = 2.5 chunks

    def test_clean_stage_reaches_armed_and_stops(self):
        fake = FakeDevice(len(self.IMAGE))
        code = aius_ota_flash.run(client_over(fake), self.IMAGE,
                                  activate=False, log=quiet, sleep=quiet)
        self.assertEqual(code, 0)
        self.assertEqual(fake.offset, len(self.IMAGE))
        self.assertEqual(fake.state, "armed")
        # The state machine's actual order, and NO activation without the
        # flag -- the interim rule's teeth.
        self.assertEqual(fake.verbs, ["verify", "arm"])

    def test_chunk_fault_not_landed_resumes_at_same_offset(self):
        # Chunk 2 faults 500 and the write did NOT land: the session has not
        # moved (the deferred absorb), so the client must retry the SAME
        # offset -- the incident's poisoned-session shape, now resumable.
        fake = FakeDevice(len(self.IMAGE), fault_plan={2: (500, False)})
        code = aius_ota_flash.run(client_over(fake), self.IMAGE,
                                  activate=False, log=quiet, sleep=quiet)
        self.assertEqual(code, 0)
        self.assertEqual(fake.state, "armed")
        # Every byte was absorbed exactly once, in order.
        self.assertEqual(sum(length for _, length in fake.chunk_log),
                         len(self.IMAGE))

    def test_chunk_fault_landed_resumes_past_it(self):
        # The answer was lost but the write committed: the device's
        # next_offset is the truth and the client adopts it rather than
        # re-sending a chunk the strict protocol would refuse.
        fake = FakeDevice(len(self.IMAGE), fault_plan={2: (503, True)})
        code = aius_ota_flash.run(client_over(fake), self.IMAGE,
                                  activate=False, log=quiet, sleep=quiet)
        self.assertEqual(code, 0)
        self.assertEqual(fake.state, "armed")
        self.assertEqual(sum(length for _, length in fake.chunk_log),
                         len(self.IMAGE))

    def test_transport_fault_is_resumable(self):
        fake = FakeDevice(len(self.IMAGE), fault_plan={1: (None, False)})
        code = aius_ota_flash.run(client_over(fake), self.IMAGE,
                                  activate=False, log=quiet, sleep=quiet)
        self.assertEqual(code, 0)
        self.assertEqual(fake.state, "armed")

    def test_non_retryable_refusal_aborts(self):
        fake = FakeDevice(len(self.IMAGE), fault_plan={2: (401, False)})
        code = aius_ota_flash.run(client_over(fake), self.IMAGE,
                                  activate=False, log=quiet, sleep=quiet)
        self.assertEqual(code, 1)

    def test_resume_from_partial_session(self):
        # A prior run staged 1.5 chunks; the client adopts the device's
        # offset instead of restarting the container.
        fake = FakeDevice(len(self.IMAGE))
        fake.offset = 6144
        fake.state = "staging"
        code = aius_ota_flash.run(client_over(fake), self.IMAGE,
                                  activate=False, log=quiet, sleep=quiet)
        self.assertEqual(code, 0)
        self.assertEqual(fake.chunk_log[0][0], 6144)
        self.assertEqual(fake.state, "armed")

    def test_activate_flag_walks_the_full_order(self):
        fake = FakeDevice(len(self.IMAGE))
        code = aius_ota_flash.run(client_over(fake), self.IMAGE,
                                  activate=True, log=quiet, sleep=quiet)
        self.assertEqual(code, 0)
        self.assertEqual(fake.state, "activation-requested")
        self.assertEqual(fake.verbs,
                         ["verify", "arm", "enable-activation", "activate"])

class RehearseTests(unittest.TestCase):
    IMAGE = bytes(range(256)) * 20

    def test_rehearse_enables_then_rehearses_and_never_activates(self):
        fake = FakeDevice(len(self.IMAGE))
        logs = []
        code = aius_ota_flash.run(client_over(fake), self.IMAGE,
                                  activate=False, rehearse=True,
                                  log=logs.append, sleep=quiet)
        self.assertEqual(code, 0)
        self.assertEqual(fake.verbs,
                         ["verify", "arm", "enable-activation", "rehearse"])
        self.assertEqual(fake.activate_posts, 0)
        self.assertTrue(any("rehearsal acknowledged; the board runs the "
                            "dark-window sequence over the inactive slot and "
                            "resets" in m for m in logs))
        self.assertTrue(any("rehearse: 200" in m and "rehearsing" in m
                            for m in logs))
        self.assertFalse(any("token" in m for m in logs))

    def test_rehearse_non_2xx_exits_nonzero(self):
        fake = FakeDevice(len(self.IMAGE))
        original = fake._post

        def refuse(verb):
            if verb == "rehearse":
                fake.verbs.append(verb)
                return 409, {"error": "ota_state"}
            return original(verb)

        fake._post = refuse
        logs = []
        code = aius_ota_flash.run(client_over(fake), self.IMAGE,
                                  activate=False, rehearse=True,
                                  log=logs.append, sleep=quiet)
        self.assertEqual(code, 1)
        self.assertFalse(any("rehearsal acknowledged" in m for m in logs))

    def test_rehearse_waits_out_flush_pending(self):
        fake = FakeDevice(len(self.IMAGE))
        fake.dirty_polls = 3
        code = aius_ota_flash.run(client_over(fake), self.IMAGE,
                                  activate=False, rehearse=True,
                                  log=quiet, sleep=quiet)
        self.assertEqual(code, 0)
        self.assertEqual(fake.rehearse_posts, 2)

    def test_activate_and_rehearse_are_mutually_exclusive(self):
        fake = FakeDevice(len(self.IMAGE))
        code = aius_ota_flash.run(client_over(fake), self.IMAGE,
                                  activate=True, rehearse=True,
                                  log=quiet, sleep=quiet)
        self.assertEqual(code, 2)
        self.assertEqual(fake.verbs, [])


class FlushGateTests(unittest.TestCase):
    IMAGE = bytes(range(256)) * 20

    def run_activate(self, fake):
        slept = []
        code = aius_ota_flash.run(client_over(fake), self.IMAGE, activate=True,
                                  log=quiet, sleep=slept.append)
        return code, sum(slept)

    def test_409_waits_then_stops_as_soon_as_drained(self):
        fake = FakeDevice(len(self.IMAGE))
        fake.dirty_polls = 4
        code, waited = self.run_activate(fake)
        self.assertEqual(code, 0)
        self.assertEqual(fake.state, "activation-requested")
        self.assertEqual(fake.activate_posts, 2)
        # Stopped at the drain, not at 60 s (the post-activate watch adds
        # its own fixed 15 s of quiet polling).
        self.assertLess(waited - aius_ota_flash.ACTIVATION_WATCH_S, 10)

    def test_409_still_pending_at_60s_exits_nonzero(self):
        fake = FakeDevice(len(self.IMAGE))
        fake.dirty_polls = None
        logs = []
        slept = []
        code = aius_ota_flash.run(client_over(fake), self.IMAGE, activate=True,
                                  log=logs.append, sleep=slept.append)
        self.assertNotEqual(code, 0)
        self.assertGreaterEqual(sum(slept), 60)
        self.assertLess(sum(slept), 70)
        self.assertEqual(fake.state, "armed")
        self.assertTrue(any("sd_flush_pending" in m and "dirty=1" in m
                            for m in logs))

    def test_abandoned_activation_faults_are_not_success(self):
        for fault in ("ota_flush_pending", "ota_sd_busy"):
            with self.subTest(fault=fault):
                fake = FakeDevice(len(self.IMAGE))
                fake.activation_fault = fault
                logs = []
                code = aius_ota_flash.run(client_over(fake), self.IMAGE,
                                          activate=True, log=logs.append,
                                          sleep=quiet)
                self.assertEqual(code, 1)
                self.assertTrue(any("ABANDONED" in m and fault in m
                                    for m in logs))


if __name__ == "__main__":
    unittest.main(verbosity=2)
