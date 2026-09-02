"""Host-side unit tests for tools/aius-recorder-pull.py.

Runs inside the `rate-runner` suite: scripts/test.sh invokes
test_rate_bench.py, whose entry point discovers every test_*.py beside it.
No board, no socket -- the device is a scripted `fetch`.
"""

import contextlib
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest
import urllib.error


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE = ROOT / "tools" / "aius-recorder-pull.py"
SPEC = importlib.util.spec_from_file_location("aius_recorder_pull", MODULE)
PULL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PULL)


def slot_entry(index, state="absent", generation=0, valid_frames=0):
    return {
        "slot": index,
        "state": state,
        "sample_rate_hz": 48000,
        "rate_changes": 0,
        "ring_frames": 480000,
        "valid_frames": valid_frames,
        "oldest_frame": 0,
        "freeze_generation": generation,
        "gap_count": 0,
        "last_gap_frame": 0,
        "suppressed_triggers": 0,
        "postroll_captured": 0,
        "trigger": {"which": "underrun", "at_frame": 0, "at_block": 416,
                    "at_ms": 0},
    }


def bank(slots, active_slot=0, newest_frozen_slot="omit"):
    document = {
        "fitted_mb": 16,
        "encoding": "s32le",
        "significant_bits": 24,
        "interleave": "lr",
        "frame_bytes": 8,
        "boot_generation": 3,
        "chunk_max_bytes": 32768,
        "active_slot": active_slot,
        "slot_count": len(slots),
        "slots_recycled": 0,
        "slots": slots,
    }
    if newest_frozen_slot != "omit":
        document["newest_frozen_slot"] = newest_frozen_slot
    return document


@contextlib.contextmanager
def scripted_fetch(result):
    """Replace the tool's one network primitive. `result` is bytes to return
    or an exception to raise."""
    original = PULL.fetch

    def fetch(_url, _token, timeout_s=10.0):
        if isinstance(result, Exception):
            raise result
        return result

    PULL.fetch = fetch
    try:
        yield
    finally:
        PULL.fetch = original


def run_main(document_or_error, argv):
    stdout = io.StringIO()
    stderr = io.StringIO()
    payload = document_or_error
    if isinstance(payload, dict):
        payload = json.dumps(payload).encode("utf-8")
    with scripted_fetch(payload):
        with contextlib.redirect_stdout(stdout), \
                contextlib.redirect_stderr(stderr):
            code = PULL.main(argv)
    return code, stdout.getvalue(), stderr.getvalue()


class ChooseSlotTests(unittest.TestCase):
    """The default slot is the NEWEST FROZEN one -- the device's own default
    -- not active_slot, which on a bank that just froze and auto-advanced is
    the slot still recording."""

    def test_explicit_slot_wins_over_every_default(self):
        document = bank([slot_entry(0, "frozen", 9)], newest_frozen_slot=0)
        slot, reason = PULL.choose_slot(document, 2)
        self.assertEqual(slot, 2)
        self.assertIn("--slot", reason)

    def test_defaults_to_newest_frozen_slot_field(self):
        document = bank([
            slot_entry(0, "frozen", 4),
            slot_entry(1, "frozen", 7),
            slot_entry(2, "recording"),
            slot_entry(3, "absent"),
        ], active_slot=2, newest_frozen_slot=1)
        slot, reason = PULL.choose_slot(document, None)
        self.assertEqual(slot, 1)
        self.assertIn("newest frozen", reason)

    def test_scans_slots_when_older_firmware_omits_the_field(self):
        document = bank([
            slot_entry(0, "frozen", 4),
            slot_entry(1, "recording"),
            slot_entry(2, "frozen", 11),
            slot_entry(3, "frozen", 6),
        ], active_slot=1)
        slot, reason = PULL.choose_slot(document, None)
        self.assertEqual(slot, 2)
        self.assertIn("freeze_generation", reason)

    def test_null_field_still_scans_for_a_frozen_slot(self):
        document = bank([
            slot_entry(0, "recording"),
            slot_entry(1, "frozen", 2),
        ], active_slot=0, newest_frozen_slot=None)
        slot, _reason = PULL.choose_slot(document, None)
        self.assertEqual(slot, 1)

    def test_falls_back_to_active_slot_when_nothing_is_frozen(self):
        document = bank([
            slot_entry(0, "absent"),
            slot_entry(1, "recording"),
        ], active_slot=1, newest_frozen_slot=None)
        slot, reason = PULL.choose_slot(document, None)
        self.assertEqual(slot, 1)
        self.assertIn("no frozen slot", reason)

    def test_slot_zero_is_a_real_choice_not_a_falsy_one(self):
        document = bank([
            slot_entry(0, "frozen", 5),
            slot_entry(1, "recording"),
        ], active_slot=1, newest_frozen_slot=0)
        slot, reason = PULL.choose_slot(document, None)
        self.assertEqual(slot, 0)
        self.assertIn("newest frozen", reason)


class StateAdviceTests(unittest.TestCase):
    """A refusal names what the bank is actually doing. Telling an operator to
    freeze while a freeze is already draining fires a second trigger at a bank
    that was about to be ready."""

    def _refusal(self, state):
        document = bank([slot_entry(0, state)], active_slot=0,
                        newest_frozen_slot=None)
        code, _stdout, stderr = run_main(document, [
            "--device", "http://board", "--auth", "token", "out.wav"])
        self.assertEqual(code, 2, stderr)
        return stderr

    def test_recording_says_freeze_first(self):
        stderr = self._refusal("recording")
        self.assertIn("freeze it first", stderr)

    def test_postroll_says_wait_not_freeze(self):
        stderr = self._refusal("postroll")
        self.assertIn("already in flight", stderr)
        self.assertNotIn("freeze it first", stderr)

    def test_draining_says_wait_not_freeze(self):
        stderr = self._refusal("draining")
        self.assertIn("already in flight", stderr)
        self.assertNotIn("freeze it first", stderr)

    def test_absent_explains_no_psram_or_parked(self):
        stderr = self._refusal("absent")
        self.assertIn("no PSRAM", stderr)
        self.assertIn("parked", stderr)

    def test_refusal_prints_the_chosen_slot_and_why(self):
        stderr = self._refusal("recording")
        self.assertIn("slot 0:", stderr)


class RequestFailureTests(unittest.TestCase):
    """Every documented exit code has a one-line reason; nothing reaches the
    operator as a raw traceback."""

    @staticmethod
    def _http_error(code, reason):
        return urllib.error.HTTPError("http://board/api/recorder", code,
                                      reason, {}, None)

    def test_401_is_a_usage_error_naming_the_token(self):
        failure = PULL.request_failure(self._http_error(401, "Unauthorized"),
                                       "GET /api/recorder")
        self.assertEqual(failure.code, 4)
        self.assertIn("X-Aius-SD-Auth", str(failure))

    def test_400_is_a_usage_error(self):
        failure = PULL.request_failure(self._http_error(400, "Bad Request"),
                                       "chunk at offset 0")
        self.assertEqual(failure.code, 4)
        self.assertIn("slot out of range", str(failure))

    def test_409_says_the_generation_moved(self):
        failure = PULL.request_failure(self._http_error(409, "Conflict"),
                                       "chunk at offset 65536")
        self.assertEqual(failure.code, 1)
        self.assertIn("freeze_generation moved", str(failure))

    def test_404_and_urlerror_read_as_unreachable(self):
        missing = PULL.request_failure(self._http_error(404, "Not Found"),
                                       "GET /api/recorder")
        self.assertEqual(missing.code, 1)
        self.assertIn("no such route", str(missing))
        unreachable = PULL.request_failure(
            urllib.error.URLError("Connection refused"), "GET /api/recorder")
        self.assertEqual(unreachable.code, 1)
        self.assertIn("unreachable", str(unreachable))

    def test_header_fetch_401_exits_4_without_a_traceback(self):
        code, _stdout, stderr = run_main(
            self._http_error(401, "Unauthorized"),
            ["--device", "http://board", "--auth", "wrong", "out.wav"])
        self.assertEqual(code, 4)
        self.assertIn("401", stderr)
        self.assertIn("AIUS_SD_AUTH", stderr)

    def test_header_fetch_unreachable_exits_1_without_a_traceback(self):
        code, _stdout, stderr = run_main(
            urllib.error.URLError("No route to host"),
            ["--device", "http://board", "--auth", "token", "out.wav"])
        self.assertEqual(code, 1)
        self.assertIn("unreachable", stderr)


class HeaderSidecarTests(unittest.TestCase):
    def test_header_json_is_the_device_document_verbatim(self):
        # Nothing enumerates fields, so per-slot additions
        # (rate_change_frames / rate_change_count) reach the sidecar with no
        # change to this tool.
        slot = slot_entry(0, "recording")
        slot["rate_change_frames"] = [128, 4096]
        slot["rate_change_count"] = 2
        document = bank([slot], active_slot=0, newest_frozen_slot=None)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "header.json"
            code, _stdout, _stderr = run_main(document, [
                "--device", "http://board", "--auth", "token",
                "--header-json", str(path), "out.wav"])
            self.assertEqual(code, 2)
            written = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(written, document)
        self.assertEqual(written["slots"][0]["rate_change_count"], 2)


if __name__ == "__main__":
    unittest.main()
