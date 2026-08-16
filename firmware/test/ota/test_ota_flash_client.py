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

    def transport(self, method, url, headers, body, timeout):
        path = url.split("://", 1)[-1].split("/", 1)[1]
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
        if verb == "activate":
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
