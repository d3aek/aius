"""Fake-fixture host tests for tools/net/aius_recovery_smoke.py.

Every gate is driven against a scripted device on a virtual clock: no board, no
socket, no serial port. Each gate is exercised on all three of its result paths
-- pass, fail, and the unresolved (unreachable or not-opted-into) path -- because
an unresolved gate that reads as a pass is the failure mode this runner exists
to prevent.
"""

import contextlib
import importlib.util
import io
import pathlib
import tempfile
import unittest
import unittest.mock


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE = ROOT / "tools" / "net" / "aius_recovery_smoke.py"
SPEC = importlib.util.spec_from_file_location("aius_recovery_smoke", MODULE)
SMOKE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SMOKE)

SOFTWARE_RESET = 0x02
WATCHDOG_RESET = 0x10

STATUS_FIELDS = {
    "state": 3,
    "format": 1,
    "sample_rate": 48000,
    "network_state": 3,
    "recovery_safe": 0,
    "network_last_fault": 0,
    "last_checkpoint": 10,
    "network_generation": 1,
    "reset_source": SOFTWARE_RESET,
    "watchdog_early_resets": 0,
    "loop_epoch": 4096,
    "network_restarts": 0,
    "http_service_overruns": 0,
    "network_drops": 0,
    "build_revision": "abc1234",
    "build_utc": "2026-07-25T00:00:00Z",
}


def status(generation, **overrides):
    body = dict(STATUS_FIELDS)
    body["boot_generation"] = generation
    body["ip"] = "192.0.2.45"
    body["dhcp_pending"] = 0
    body.update(overrides)
    return SMOKE.normalize_recovery(body)


class FakeLink:
    """The device seam, on a virtual clock. Every read fails by default."""

    def __init__(self):
        self.clock = 0.0
        self.mutations = []

    def now(self):
        return self.clock

    def sleep(self, seconds):
        self.clock += max(0.0, seconds)

    def http_status(self, timeout=None):
        raise SMOKE.LinkError("no HTTP")

    def udp_status(self, retries=3, timeout=None):
        raise SMOKE.LinkError("no UDP")

    def usb_present(self):
        return False

    def usb_id(self):
        raise SMOKE.LinkError("no USB")

    def http_json(self, method, path, body=None, auth=False, timeout=None):
        raise SMOKE.LinkError("no HTTP for %s %s" % (method, path))

    def http_bytes(self, path):
        raise SMOKE.LinkError("no HTTP for %s" % path)

    def reboot(self):
        raise SMOKE.LinkError("no HTTP for POST /api/reboot")


class RebootLink(FakeLink):
    """A board that reboots on request and comes back channel by channel."""

    CHANNELS = ("http", "udp", "dhcp", "usb_enum", "usb_id")

    def __init__(self, delays, reset_source=SOFTWARE_RESET, generation=10,
                 early=0, recovery_safe=False, generation_step=1,
                 refuse_reboot_after=None):
        super().__init__()
        self.delays = delays
        self.ready_at = {channel: 0.0 for channel in self.CHANNELS}
        self.generation = generation
        self.reset_source = reset_source
        self.early = early
        self.recovery_safe = recovery_safe
        self.generation_step = generation_step
        self.refuse_reboot_after = refuse_reboot_after

    def _status(self):
        return status(self.generation, reset_source=self.reset_source,
                      watchdog_early_resets=self.early,
                      recovery_safe=self.recovery_safe,
                      dhcp_pending=0 if self.clock >= self.ready_at["dhcp"] else 1,
                      ip="192.0.2.45" if self.clock >= self.ready_at["dhcp"]
                      else "0.0.0.0")

    def http_status(self, timeout=None):
        if self.clock < self.ready_at["http"]:
            raise SMOKE.LinkError("no HTTP")
        return self._status()

    def udp_status(self, retries=3, timeout=None):
        if self.clock < self.ready_at["udp"]:
            raise SMOKE.LinkError("no UDP")
        return self._status()

    def usb_present(self):
        return self.clock >= self.ready_at["usb_enum"]

    def usb_id(self):
        if self.clock < self.ready_at["usb_id"]:
            raise SMOKE.LinkError("no USB")
        return self._status()

    def reboot(self):
        self.mutations.append("reboot")
        if self.refuse_reboot_after is not None and \
                len(self.mutations) > self.refuse_reboot_after:
            raise SMOKE.LinkError("POST /api/reboot answered 500")
        self.generation += self.generation_step
        for channel in self.CHANNELS:
            self.ready_at[channel] = self.clock + self.delays.get(channel, 0.0)
        return {"status": 200, "body": {"ok": True, "rebooting": True}}


class BlockingRebootLink(RebootLink):
    """A reboot fixture whose transport reads cost wall time, like the real ones.

    The plain RebootLink advances the virtual clock only on sleep(), so a read
    that fails is free and the CDC port can be sampled arbitrarily often for
    nothing. On the board a failed http_status() costs one timeout and a failed
    udp_status() costs one per retry, and that is what let the ~1.5 s CDC
    enumeration gap fall between two presence samples -- every B6 reboot cycle
    reported `usb_basis=ack` and charged the board for host re-enumeration.

    This fixture charges each failed read its timeout and models the port
    staying enumerated for `usb_detach` after the reboot acknowledgement, which
    together are the two facts the defect needed.
    """

    def __init__(self, delays, usb_detach=0.3, timeout=1.0, udp_retries=3,
                 usb_wait=3.0, **kwargs):
        super().__init__(delays, **kwargs)
        self.usb_detach = usb_detach
        self.timeout = timeout
        self.udp_retries = udp_retries
        # DeviceLink._exchange() waits timeout + USB_ID_DEADLINE_S for the
        # framed answer, returning the moment it arrives.
        self.usb_wait = usb_wait
        self.detach_at = None

    def reboot(self):
        result = super().reboot()
        self.detach_at = self.clock + self.usb_detach
        return result

    def http_status(self, timeout=None):
        if self.clock < self.ready_at["http"]:
            self.clock += self.timeout
            raise SMOKE.LinkError("no HTTP")
        return self._status()

    def udp_status(self, retries=None, timeout=None):
        attempts = self.udp_retries if retries is None else retries
        if self.clock < self.ready_at["udp"]:
            self.clock += self.timeout * attempts
            raise SMOKE.LinkError("no UDP")
        return self._status()

    def usb_present(self):
        if self.detach_at is None or self.clock < self.detach_at:
            return True
        return self.clock >= self.ready_at["usb_enum"]

    def usb_id(self):
        """Block until the board answers, the port drops, or the bound expires."""
        if not self.usb_present():
            raise SMOKE.LinkError("no USB port")
        answer_at = self.ready_at["usb_id"]
        drop_at = (self.detach_at if self.detach_at is not None
                   and self.clock < self.detach_at else None)
        expiry = self.clock + self.usb_wait
        if drop_at is not None and drop_at <= min(answer_at, expiry):
            self.clock = drop_at
            raise SMOKE.LinkError("USB port went away mid-exchange")
        if answer_at <= expiry:
            self.clock = max(self.clock, answer_at)
            return self._status()
        self.clock = expiry
        raise SMOKE.LinkError("no USB answer")


class StallLink(FakeLink):
    """A fault image that watchdog-resets every `period` seconds."""

    def __init__(self, period, checkpoint, reset_source=WATCHDOG_RESET,
                 recovery_safe=False, generation=20):
        super().__init__()
        self.period = period
        self.checkpoint = checkpoint
        self.reset_source = reset_source
        self.recovery_safe = recovery_safe
        self.generation = generation

    def http_status(self, timeout=None):
        generation = self.generation + int(self.clock // self.period)
        return status(generation, last_checkpoint=self.checkpoint,
                      reset_source=self.reset_source,
                      recovery_safe=self.recovery_safe,
                      watchdog_early_resets=1)


class SilentThenRecoverySafeLink(FakeLink):
    """A fault image as one actually behaves: unreadable, then recovery-safe.

    Nothing answers for `silent_for` -- the board is resetting, and a stalled
    epoch serves USB at most once per boot and never reaches the HTTP or UDP
    checkpoints -- and then USB answers a board that has latched recovery-safe
    and stopped resetting, so the generation never advances again.
    """

    def __init__(self, silent_for, checkpoint, recovery_safe=True,
                 reset_source=WATCHDOG_RESET, early=2, generation=70):
        super().__init__()
        self.silent_for = silent_for
        self.checkpoint = checkpoint
        self.recovery_safe = recovery_safe
        self.reset_source = reset_source
        self.early = early
        self.generation = generation

    def _visible(self):
        return self.clock >= self.silent_for

    def usb_id(self):
        if not self._visible():
            raise SMOKE.LinkError("no USB answer")
        # Recovery-safe runs checkpoints 1, 2 and EPOCH_COMPLETE only, so the
        # live last_checkpoint can never be the stalled one.
        return status(self.generation, last_checkpoint=10,
                      reset_source=self.reset_source,
                      watchdog_early_resets=self.early,
                      recovery_safe=self.recovery_safe)

    def usb_present(self):
        return True


class SoakLink(FakeLink):
    """A board serving a steady status, optionally drifting or going away."""

    def __init__(self, generation=30, outage_after=None, drift=None,
                 drift_after=None):
        super().__init__()
        self.generation = generation
        self.outage_after = outage_after
        self.drift = drift or {}
        self.drift_after = drift_after
        self.samples = 0

    def http_status(self, timeout=None):
        self.samples += 1
        if self.outage_after is not None and self.samples > self.outage_after:
            raise SMOKE.LinkError("no HTTP")
        body = status(self.generation)
        if self.drift_after is not None and self.samples > self.drift_after:
            body.update(self.drift)
        return SMOKE.normalize_recovery(body)

    def http_json(self, method, path, body=None, auth=False, timeout=None):
        if path.startswith("/api/events"):
            return {"status": 200, "body": {"next": self.samples}}
        raise SMOKE.LinkError("no HTTP for %s %s" % (method, path))


DEFAULT_MODEL = {
    "source": "default",
    "config_generation": 0,
    "sample_rate_hz": 48000,
    "features": {"input_eq": True, "matrix": True, "output_eq": True,
                 "limiter": True, "meters": True},
}

METERS = {
    "sequence": 12,
    "sample_rate_hz": 48000,
    "input": [{"peak_dbfs": -20.0, "rms_dbfs": -26.0} for _ in range(6)],
    "output": {"peak_dbfs": [-18.0, -18.5], "rms_dbfs": [-24.0, -24.5]},
    "limiter_gain_reduction_db": 0.0,
    "waveform": [0, 1, 2, 3],
}


class DspLink(FakeLink):
    """The DSP control surface: config, meters and the SD editor resources."""

    def __init__(self, model=None, meters=None, put_status=200,
                 editor_status=200, editor_body=b"<html></html>"):
        super().__init__()
        self.model = dict(model or DEFAULT_MODEL)
        self.meters = dict(meters or METERS)
        self.put_status = put_status
        self.editor_status = editor_status
        self.editor_body = editor_body
        self.status_body = status(30)

    def http_status(self, timeout=None):
        return self.status_body

    def http_json(self, method, path, body=None, auth=False, timeout=None):
        if method == "GET" and path == "/api/dsp/config":
            return {"status": 200, "body": dict(self.model)}
        if method == "GET" and path == "/api/meters":
            return {"status": 200, "body": dict(self.meters)}
        if method == "PUT" and path == "/api/dsp/config":
            self.mutations.append(("put", body.get("base_generation")))
            if self.put_status != 200:
                return {"status": self.put_status, "body": {"ok": False}}
            if body.get("base_generation") != self.model["config_generation"]:
                return {"status": 409, "body": {"error": "stale_generation"}}
            committed = {key: value for key, value in body.items()
                         if key != "base_generation"}
            committed["source"] = "sd"
            committed["config_generation"] = self.model["config_generation"] + 1
            self.model = committed
            return {"status": 200, "body": {"ok": True}}
        raise SMOKE.LinkError("no route %s %s" % (method, path))

    def http_bytes(self, path):
        return {"status": self.editor_status,
                "body": self.editor_body if self.editor_status == 200 else b""}


class StagedOnlyDspLink(DspLink):
    """A board that writes the model to SD but never applies it.

    The measured shape of the deployed board: `source` and `config_generation`
    are written only at an audio block boundary, so with no optical carrier the
    PUT commits and neither field moves. `carrier` is a parameter because the
    same non-reporting board *with* audio is a genuine failure, and one fake
    should be able to produce both.
    """

    def __init__(self, carrier=0, blocks=0, start_job=True, fault_job=False,
                 job_state=None, **overrides):
        super().__init__(**overrides)
        self.carrier = carrier
        self.blocks = blocks
        self.start_job = start_job
        self.fault_job = fault_job
        self.job_state = SMOKE.SD_JOB_DONE if job_state is None else job_state
        self.jobs = 0
        self.faults = 0

    def http_status(self, timeout=None):
        body = dict(self.status_body)
        body["carrier"] = self.carrier
        body["blocks"] = self.blocks
        body["sd_diag"] = {"http_jobs": self.jobs, "http_faults": self.faults,
                           "job_state": self.job_state}
        return body

    def http_json(self, method, path, body=None, auth=False, timeout=None):
        if method != "PUT" or path != "/api/dsp/config":
            return super().http_json(method, path, body, auth, timeout)
        before = dict(self.model)
        result = super().http_json(method, path, body, auth, timeout)
        if result["status"] == 200:
            self.model["source"] = before["source"]
            self.model["config_generation"] = before["config_generation"]
            if self.start_job:
                self.jobs += 1
            if self.fault_job:
                self.faults += 1
        return result


def options(**overrides):
    values = {"target": "192.0.2.45", "poll_interval": 0.25, "reboots": 2}
    values.update(overrides)
    return SMOKE.SmokeOptions(**values)


class DecodeTests(unittest.TestCase):
    def test_reset_kind_mirrors_the_firmware_classification(self):
        self.assertEqual(SMOKE.reset_kind(0x10), "watchdog")
        self.assertEqual(SMOKE.reset_kind(0x11), "watchdog")
        self.assertEqual(SMOKE.reset_kind(0x01), "cold")
        self.assertEqual(SMOKE.reset_kind(0x04), "cold")
        self.assertEqual(SMOKE.reset_kind(0x02), "software")
        self.assertEqual(SMOKE.reset_kind(0x40), "software")
        self.assertEqual(SMOKE.reset_kind(0x80), "other")
        self.assertEqual(SMOKE.reset_kind(None), "unknown")

    def test_usb_id_decodes_to_the_shared_field_names(self):
        line = ("AIUS/1 OK 7 ID protocol=1 revision=abc1234 build=2026-07-25 "
                "mode=optical ip=192.0.2.45 rsafe=0 nstate=3 nfault=0 ckpt=9 "
                "early=0 ngen=1 bgen=12 rsrc=2 epoch=4096 nrst=0")
        decoded = SMOKE.parse_usb_id(line)
        self.assertEqual(decoded["request_id"], 7)
        self.assertEqual(decoded["revision"], "abc1234")
        self.assertEqual(decoded["boot_generation"], 12)
        self.assertEqual(decoded["last_checkpoint"], 9)
        self.assertEqual(decoded["network_generation"], 1)
        self.assertIs(decoded["recovery_safe"], False)
        self.assertEqual(SMOKE.reset_kind(decoded["reset_source"]), "software")

    def test_usb_id_rejects_an_error_or_malformed_answer(self):
        with self.assertRaises(ValueError):
            SMOKE.parse_usb_id("AIUS/1 ERR 7 BAD_COMMAND error")
        with self.assertRaises(ValueError):
            SMOKE.parse_usb_id("AIUS/1 OK 7 AUTH abcdef")
        with self.assertRaises(ValueError):
            SMOKE.parse_usb_id("AIUS/1 OK 7 ID protocol=1 bogus")

    def test_usb_response_is_selected_by_request_id_not_by_position(self):
        text = "partial line\nAIUS/1 OK 4 ID protocol=1\nAIUS/1 OK 5 ID protocol=1\n"
        self.assertIn(" 5 ID", SMOKE.select_usb_response(text, 5))
        self.assertIsNone(SMOKE.select_usb_response(text, 6))

    def test_recovery_safe_normalizes_across_channels(self):
        self.assertIs(SMOKE.normalize_recovery({"recovery_safe": 1})["recovery_safe"],
                      True)
        self.assertIs(SMOKE.normalize_recovery({"recovery_safe": False})["recovery_safe"],
                      False)

    def test_dhcp_is_bound_only_with_an_address_and_no_pending_lease(self):
        self.assertTrue(SMOKE.dhcp_bound({"ip": "192.0.2.45", "dhcp_pending": 0}))
        self.assertFalse(SMOKE.dhcp_bound({"ip": "0.0.0.0", "dhcp_pending": 1}))
        self.assertFalse(SMOKE.dhcp_bound({"ip": "192.0.2.45", "dhcp_pending": 1}))

    def test_deadline_failures_name_the_late_and_the_missing_channel(self):
        failures = SMOKE.deadline_failures({"http": 13.0, "udp": 1.0, "dhcp": None,
                                            "usb": 0.5})
        self.assertEqual(len(failures), 2)
        self.assertTrue(any("dhcp never became ready" in line for line in failures))
        self.assertTrue(any("http ready at 13.00s" in line for line in failures))
        self.assertEqual(SMOKE.deadline_failures({"http": 1.0}), [])


class DeviceLinkTests(unittest.TestCase):
    """The real link's decode and error wrapping, with both sockets faked out."""

    FIXTURE = ROOT / "test" / "fixtures" / "status" / "status-v1-recovery-99.bin"

    def link(self, **overrides):
        return SMOKE.DeviceLink(options(**overrides))

    def test_udp_status_decodes_through_the_shared_net_runner(self):
        payload = self.FIXTURE.read_bytes()
        with unittest.mock.patch.object(
                SMOKE.udp_runner, "request",
                return_value=({"payload": payload}, ("192.0.2.45", 4242))):
            decoded = self.link().udp_status()
        self.assertEqual(decoded["boot_generation"], 0xd1d2d3d4)
        self.assertEqual(decoded["last_checkpoint"], 10)
        self.assertIs(decoded["recovery_safe"], True)
        self.assertEqual(decoded["http_requests"], 0x81828384)

    def test_udp_failures_become_link_errors(self):
        for failure in (OSError("unreachable"), TimeoutError("no answer"),
                        ValueError("invalid crc")):
            with unittest.mock.patch.object(SMOKE.udp_runner, "request",
                                            side_effect=failure):
                with self.assertRaises(SMOKE.LinkError):
                    self.link().udp_status()

    def test_http_status_normalizes_recovery_safe_and_rejects_a_non_200(self):
        body = {"boot_generation": 4, "recovery_safe": 1, "ip": "192.0.2.45"}
        with unittest.mock.patch.object(SMOKE.http_runner, "request",
                                        return_value={"status": 200, "body": body}):
            self.assertIs(self.link().http_status()["recovery_safe"], True)
        with unittest.mock.patch.object(SMOKE.http_runner, "request",
                                        return_value={"status": 503, "body": {}}):
            with self.assertRaises(SMOKE.LinkError):
                self.link().http_status()

    def test_a_socket_failure_becomes_a_link_error_not_a_traceback(self):
        with unittest.mock.patch.object(SMOKE.http_runner, "request",
                                        side_effect=OSError("connection refused")):
            with self.assertRaises(SMOKE.LinkError):
                self.link().http_status()

    def test_usb_and_auth_report_the_missing_input_rather_than_guessing(self):
        with self.assertRaises(SMOKE.LinkError):
            self.link().usb_id()
        with self.assertRaises(SMOKE.LinkError):
            self.link(auth_token=None).http_json("PUT", "/api/dsp/config", {},
                                                 auth=True)


class RebootGateTests(unittest.TestCase):
    DELAYS = {"dhcp": 2.5, "udp": 2.8, "http": 3.0, "usb_enum": 1.0, "usb_id": 1.4}

    def test_pass_every_channel_ready_inside_its_deadline(self):
        link = RebootLink(self.DELAYS)
        result = SMOKE.gate_reboots(link, options(allow_mutation=True,
                                                  serial="/dev/ttyACM0"))
        self.assertEqual(result.outcome, SMOKE.PASS)
        self.assertEqual(link.mutations, ["reboot", "reboot"])
        self.assertIn("2/2", result.detail)
        self.assertTrue(any("usb budget from enumeration" in line
                            for line in result.observations))

    def test_usb_budget_starts_at_re_enumeration_when_reads_block(self):
        """The B6 measurement defect: the enumeration gap fell between samples.

        The port is enumerated for 0.3 s after the reboot ack, absent until
        1.8 s, and every read while the board is down costs a full timeout. The
        budget must therefore run from ~1.8 s (re-enumeration), not from the
        acknowledgement -- and `AIUS ID` answering at 2.4 s is 0.6 s of board
        time, inside the 2 s deadline, not 2.4 s of mostly-host time outside it.
        """
        link = BlockingRebootLink(
            {"dhcp": 5.0, "udp": 5.0, "http": 5.2, "usb_enum": 1.8, "usb_id": 2.4})
        baseline = link.generation
        link.reboot()
        latencies, basis = SMOKE.observe_ready(
            link, options(serial="/dev/ttyACM0"), link.now(), baseline, True)
        self.assertEqual(basis, "enumeration")
        self.assertIsNotNone(latencies["usb"])
        self.assertLess(latencies["usb"], SMOKE.USB_ID_DEADLINE_S)

    def test_usb_budget_stays_on_the_ack_when_the_port_never_dropped(self):
        """No absence observed is not the same evidence, and must not claim it."""
        link = BlockingRebootLink(
            {"dhcp": 2.0, "udp": 2.0, "http": 2.0, "usb_enum": 0.0, "usb_id": 1.0},
            usb_detach=99.0)
        baseline = link.generation
        link.reboot()
        latencies, basis = SMOKE.observe_ready(
            link, options(serial="/dev/ttyACM0"), link.now(), baseline, True)
        self.assertEqual(basis, "ack")
        self.assertIsNotNone(latencies["usb"])

    def test_fail_when_http_and_dhcp_miss_the_normative_deadline(self):
        link = RebootLink({"dhcp": 12.5, "udp": 12.5, "http": 13.0})
        result = SMOKE.gate_reboots(link, options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertTrue(any("http ready at" in line for line in result.observations))
        self.assertTrue(any("dhcp ready at" in line for line in result.observations))

    def test_fail_on_an_unexpected_watchdog_reset(self):
        link = RebootLink(self.DELAYS, reset_source=WATCHDOG_RESET, early=1)
        result = SMOKE.gate_reboots(link, options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("unexpected watchdog reset", " ".join(result.observations))

    def test_fail_when_the_boot_generation_does_not_advance_by_one(self):
        link = RebootLink(self.DELAYS, generation_step=2)
        result = SMOKE.gate_reboots(link, options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("expected +1", " ".join(result.observations))

    def test_fail_when_a_later_reboot_is_refused(self):
        link = RebootLink(self.DELAYS, refuse_reboot_after=1)
        result = SMOKE.gate_reboots(link, options(allow_mutation=True, reboots=3))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("reboot not accepted", " ".join(result.observations))

    def test_unresolved_without_the_mutation_opt_in(self):
        link = RebootLink(self.DELAYS)
        result = SMOKE.gate_reboots(link, options())
        self.assertEqual(result.outcome, SMOKE.UNRESOLVED)
        self.assertEqual(link.mutations, [])
        self.assertIn("read-only run", result.detail)

    def test_unresolved_when_the_device_is_unreachable_before_anything_is_proven(self):
        result = SMOKE.gate_reboots(FakeLink(), options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.UNRESOLVED)
        self.assertIn("unreachable", result.detail)

    def test_unresolved_when_the_very_first_reboot_cannot_be_requested(self):
        link = RebootLink(self.DELAYS, refuse_reboot_after=0)
        result = SMOKE.gate_reboots(link, options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.UNRESOLVED)
        self.assertIn("first reboot", result.detail)


class StallGateTests(unittest.TestCase):
    def test_pass_reset_inside_the_window_reporting_its_checkpoint(self):
        link = StallLink(period=9.0, checkpoint=4)
        result = SMOKE.gate_stalls(link, options(allow_mutation=True, checkpoint=4))
        self.assertEqual(result.outcome, SMOKE.PASS)
        self.assertIn("4 NETWORK", result.detail)
        self.assertIn("cycle basis", " ".join(result.observations))
        self.assertIn("normal", result.detail)

    def test_pass_when_service_returns_in_recovery_safe_mode(self):
        link = StallLink(period=9.0, checkpoint=0, recovery_safe=True)
        result = SMOKE.gate_stalls(link, options(allow_mutation=True, checkpoint=0))
        self.assertEqual(result.outcome, SMOKE.PASS)
        self.assertIn("recovery-safe", result.detail)

    def test_fail_when_the_reported_checkpoint_is_not_the_flashed_one(self):
        link = StallLink(period=9.0, checkpoint=5)
        result = SMOKE.gate_stalls(link, options(allow_mutation=True, checkpoint=3))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("reported checkpoint 5, expected 3", " ".join(result.observations))

    def test_fail_when_the_reset_takes_longer_than_the_watchdog_window(self):
        link = StallLink(period=13.0, checkpoint=8)
        result = SMOKE.gate_stalls(link, options(allow_mutation=True, checkpoint=8))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("above", result.detail)

    def test_fail_when_no_reset_happens_within_the_horizon(self):
        link = StallLink(period=1000.0, checkpoint=2)
        result = SMOKE.gate_stalls(link, options(allow_mutation=True, checkpoint=2,
                                                 stall_horizon=20.0))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("no reset within 20s", result.detail)

    def test_fail_when_the_reset_was_not_a_watchdog_reset(self):
        link = StallLink(period=9.0, checkpoint=1, reset_source=SOFTWARE_RESET)
        result = SMOKE.gate_stalls(link, options(allow_mutation=True, checkpoint=1))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("expected a watchdog reset", " ".join(result.observations))

    def test_unresolved_without_a_declared_fault_image(self):
        result = SMOKE.gate_stalls(StallLink(9.0, 4), options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.UNRESOLVED)
        self.assertIn("no fault image declared", result.detail)

    def test_unresolved_for_a_checkpoint_that_is_not_injectable(self):
        result = SMOKE.gate_stalls(StallLink(9.0, 10),
                                   options(allow_mutation=True, checkpoint=10))
        self.assertEqual(result.outcome, SMOKE.UNRESOLVED)
        self.assertIn("not injectable", result.detail)

    def test_unresolved_without_the_mutation_opt_in(self):
        result = SMOKE.gate_stalls(StallLink(9.0, 4), options(checkpoint=4))
        self.assertEqual(result.outcome, SMOKE.UNRESOLVED)
        self.assertIn("read-only run", result.detail)

    def test_unresolved_when_the_fault_image_is_unreachable(self):
        result = SMOKE.gate_stalls(FakeLink(),
                                   options(allow_mutation=True, checkpoint=4))
        self.assertEqual(result.outcome, SMOKE.UNRESOLVED)
        self.assertIn("no channel answered", result.detail)

    def test_pass_on_the_recovery_safe_latch_when_no_boot_is_observable(self):
        """The shape every real fault image has: silent, then recovery-safe.

        A stalled epoch serves USB at most once per boot and never reaches the
        HTTP or UDP checkpoints, so nothing answers until two early watchdog
        resets latch recovery-safe. The gate must accept that as evidence and
        say which criteria it could not check.
        """
        link = SilentThenRecoverySafeLink(silent_for=18.0, checkpoint=6)
        result = SMOKE.gate_stalls(link, options(allow_mutation=True, checkpoint=6,
                                                 serial="/dev/ttyACM0",
                                                 stall_horizon=45.0))
        self.assertEqual(result.outcome, SMOKE.PASS)
        self.assertIn("latched recovery-safe", result.detail)
        joined = " ".join(result.observations)
        self.assertIn("not checked on this path", joined)
        self.assertIn("watchdog_early_resets=2", joined)

    def test_fail_when_the_only_answer_is_a_board_that_never_reset(self):
        """The other reading of one boot and no generation change."""
        link = SilentThenRecoverySafeLink(silent_for=0.0, checkpoint=6,
                                          recovery_safe=False,
                                          reset_source=SOFTWARE_RESET, early=0)
        result = SMOKE.gate_stalls(link, options(allow_mutation=True, checkpoint=6,
                                                 serial="/dev/ttyACM0",
                                                 stall_horizon=20.0))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("not in recovery-safe mode", result.detail)

    def test_fail_when_recovery_safe_reports_a_zeroed_retained_ledger(self):
        """Plan defect 17a: the two states this gate must tell apart.

        A recovery-safe board whose boot_generation, reset_source and
        watchdog_early_resets all read 0 is indistinguishable from a correctly
        latched one unless the gate rejects it -- which is the whole reason the
        retained facts are published before the first epoch rather than from
        checkpoint 0.
        """
        link = SilentThenRecoverySafeLink(silent_for=12.0, checkpoint=6,
                                          reset_source=0, early=0, generation=0)
        result = SMOKE.gate_stalls(link, options(allow_mutation=True, checkpoint=6,
                                                 serial="/dev/ttyACM0",
                                                 stall_horizon=45.0))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("expected a watchdog reset", " ".join(result.observations))


class SoakGateTests(unittest.TestCase):
    def test_pass_with_no_reset_outage_or_leaked_generation(self):
        link = SoakLink()
        result = SMOKE.gate_soak(link, options(soak_duration=10.0,
                                               soak_interval=1.0))
        self.assertEqual(result.outcome, SMOKE.PASS)
        self.assertIn("10 samples", result.detail)

    def test_fail_on_an_unexpected_reset(self):
        link = SoakLink(drift={"boot_generation": 31}, drift_after=4)
        result = SMOKE.gate_soak(link, options(soak_duration=10.0,
                                               soak_interval=1.0))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("unexpected reset", result.detail)

    def test_fail_on_a_leaked_network_generation(self):
        link = SoakLink(drift={"network_generation": 2}, drift_after=3)
        result = SMOKE.gate_soak(link, options(soak_duration=10.0,
                                               soak_interval=1.0))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("leaked network generation", result.detail)

    def test_fail_on_an_outage(self):
        link = SoakLink(outage_after=3)
        result = SMOKE.gate_soak(link, options(soak_duration=10.0,
                                               soak_interval=1.0))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("outage", result.detail)

    def test_a_tolerated_outage_does_not_fail_the_gate(self):
        link = SoakLink(outage_after=3)
        result = SMOKE.gate_soak(link, options(soak_duration=6.0, soak_interval=1.0,
                                               soak_tolerated_outages=10))
        self.assertEqual(result.outcome, SMOKE.PASS)

    def test_unresolved_when_the_device_never_answers(self):
        result = SMOKE.gate_soak(FakeLink(), options(soak_duration=5.0))
        self.assertEqual(result.outcome, SMOKE.UNRESOLVED)
        self.assertIn("never started", result.detail)


class DspGateTests(unittest.TestCase):
    def test_pass_round_trip_meters_and_editor(self):
        link = DspLink()
        result = SMOKE.gate_dsp(link, options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.PASS)
        self.assertEqual([entry[0] for entry in link.mutations], ["put", "put"])
        self.assertEqual(link.model["source"], "sd")
        self.assertIn("meters and editor served", result.detail)

    def test_fail_when_the_default_audio_is_already_overridden(self):
        link = DspLink(model=dict(DEFAULT_MODEL, source="sd"))
        result = SMOKE.gate_dsp(link, options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("default audio changed", result.detail)

    def test_fail_when_meters_do_not_render_six_input_channels(self):
        link = DspLink(meters=dict(METERS, input=[{"peak_dbfs": 0.0,
                                                   "rms_dbfs": 0.0}]))
        result = SMOKE.gate_dsp(link, options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("1 input channels", " ".join(result.observations))

    def test_fail_when_the_sd_editor_does_not_load(self):
        link = DspLink(editor_status=404)
        result = SMOKE.gate_dsp(link, options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("editor resource", " ".join(result.observations))

    def test_fail_when_the_config_put_is_rejected(self):
        link = DspLink(put_status=400)
        result = SMOKE.gate_dsp(link, options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("answered 400", result.detail)

    def test_unresolved_read_only_run_skips_the_round_trip(self):
        link = DspLink()
        result = SMOKE.gate_dsp(link, options())
        self.assertEqual(result.outcome, SMOKE.UNRESOLVED)
        self.assertEqual(link.mutations, [])
        self.assertIn("round-trip was skipped", result.detail)

    def test_unresolved_when_the_put_is_unauthorized(self):
        link = DspLink(put_status=401)
        result = SMOKE.gate_dsp(link, options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.UNRESOLVED)
        self.assertIn("could not be driven", result.detail)

    def test_unresolved_when_the_device_is_unreachable(self):
        result = SMOKE.gate_dsp(FakeLink(), options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.UNRESOLVED)
        self.assertIn("unreachable", result.detail)

    def test_pass_on_a_silent_board_witnessed_by_the_sd_job(self):
        """No carrier, no block boundary, so the apply provably cannot run."""
        link = StagedOnlyDspLink()
        result = SMOKE.gate_dsp(link, options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.PASS)
        observations = " ".join(result.observations)
        self.assertIn("commit basis: sd-job", observations)
        self.assertIn("not checked on this path", observations)

    def test_fail_when_a_silent_board_starts_no_sd_job(self):
        link = StagedOnlyDspLink(start_job=False)
        result = SMOKE.gate_dsp(link, options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("started no SD job", result.detail)

    def test_fail_when_the_sd_write_faults(self):
        link = StagedOnlyDspLink(fault_job=True)
        result = SMOKE.gate_dsp(link, options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("SD write faulted", result.detail)

    def test_fail_when_the_sd_job_did_not_reach_done(self):
        link = StagedOnlyDspLink(job_state=16)
        result = SMOKE.gate_dsp(link, options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("expected DONE", result.detail)

    def test_fail_when_a_board_with_audio_does_not_report_the_commit(self):
        """The sd-job basis is not a general excuse: with audio it is a failure."""
        link = StagedOnlyDspLink(carrier=1, blocks=4096)
        result = SMOKE.gate_dsp(link, options(allow_mutation=True))
        self.assertEqual(result.outcome, SMOKE.FAIL)
        self.assertIn("did not commit to SD", result.detail)


class RunAndSummaryTests(unittest.TestCase):
    def test_a_default_run_mutates_nothing(self):
        link = DspLink()
        run = SMOKE.run_gates(link, options(soak_duration=3.0, soak_interval=1.0))
        self.assertEqual(link.mutations, [])
        self.assertEqual([result.name for result in run.results],
                         list(SMOKE.GATE_NAMES))
        outcomes = {result.name: result.outcome for result in run.results}
        self.assertEqual(outcomes["reboots"], SMOKE.UNRESOLVED)
        self.assertEqual(outcomes["stalls"], SMOKE.UNRESOLVED)
        self.assertEqual(outcomes["dsp"], SMOKE.UNRESOLVED)

    def test_selected_gates_are_the_only_ones_run(self):
        run = SMOKE.run_gates(SoakLink(), options(gates=("soak",),
                                                  soak_duration=2.0,
                                                  soak_interval=1.0))
        self.assertEqual([result.name for result in run.results], ["soak"])

    def test_exit_code_prefers_failure_then_unresolved(self):
        passed = SMOKE.GateResult("soak", SMOKE.PASS, "ok")
        failed = SMOKE.GateResult("dsp", SMOKE.FAIL, "bad")
        unresolved = SMOKE.GateResult("stalls", SMOKE.UNRESOLVED, "unknown")
        self.assertEqual(SMOKE.exit_code([passed]), 0)
        self.assertEqual(SMOKE.exit_code([passed, unresolved]), 3)
        self.assertEqual(SMOKE.exit_code([passed, unresolved, failed]), 2)

    def test_summary_reports_identity_gates_and_a_verdict(self):
        run = SMOKE.SmokeRun(target="192.0.2.45", started_utc="2026-07-25T10:00:00Z",
                             identity={"channel": "usb", "revision": "abc1234",
                                       "build": "2026-07-25", "boot_generation": 12,
                                       "last_checkpoint": 9})
        run.results = [SMOKE.GateResult("soak", SMOKE.PASS, "no reset", ["120 samples"]),
                       SMOKE.GateResult("dsp", SMOKE.FAIL, "meters missing")]
        summary = SMOKE.format_summary(run)
        self.assertIn("target=192.0.2.45", summary)
        self.assertIn("revision=abc1234", summary)
        self.assertIn("boot_generation=12", summary)
        self.assertIn("120 samples", summary)
        self.assertIn("==> FAIL: dsp", summary)

    def test_identity_falls_back_to_a_recorded_error(self):
        identity = SMOKE.read_identity(FakeLink(), False)
        self.assertEqual(identity["channel"], "none")
        self.assertIn("error", identity)

    def test_evidence_is_written_as_the_readable_per_run_summary(self):
        run = SMOKE.SmokeRun(target="192.0.2.45", started_utc="2026-07-25T10:00:00Z",
                             identity={"channel": "http"})
        run.results = [SMOKE.GateResult("soak", SMOKE.PASS, "no reset")]
        with tempfile.TemporaryDirectory() as directory:
            path = SMOKE.write_evidence(run, directory)
            self.assertTrue(path.exists())
            self.assertIn("soak", path.read_text(encoding="utf-8"))


class PlanOfWorkTests(unittest.TestCase):
    def test_each_fault_image_is_restored_before_the_next_is_built(self):
        lines = SMOKE.plan_of_work(options(gates=("stalls",)))
        builds = [index for index, line in enumerate(lines) if ": build " in line]
        restores = [index for index, line in enumerate(lines) if ": restore " in line]
        self.assertEqual(len(builds), len(SMOKE.INJECTABLE_CHECKPOINTS))
        self.assertEqual(len(restores), len(builds))
        for build, restore in zip(builds[1:], restores):
            self.assertLess(restore, build)

    def test_one_checkpoint_narrows_the_plan_to_that_image(self):
        lines = SMOKE.plan_of_work(options(gates=("stalls",), checkpoint=6))
        self.assertEqual(len([line for line in lines if ": build " in line]), 1)
        self.assertIn("6 UDP", lines[-1])

    def test_checkpoint_names_include_audio_and_epoch_complete(self):
        self.assertEqual(SMOKE.CHECKPOINT_NAMES[8:],
                         ("SD", "AUDIO", "EPOCH_COMPLETE"))
        audio = SMOKE.plan_of_work(options(gates=("stalls",), checkpoint=9))
        complete = SMOKE.plan_of_work(options(gates=("stalls",), checkpoint=10))
        self.assertTrue(any("checkpoint 9 AUDIO" in line for line in audio))
        self.assertTrue(any("checkpoint 10 EPOCH_COMPLETE" in line
                            for line in complete))

    def test_the_plan_names_the_mutating_gates_as_mutating(self):
        lines = SMOKE.plan_of_work(options())
        self.assertTrue(any("gate reboots (mutating)" in line for line in lines))
        self.assertTrue(any("gate soak (read-only)" in line for line in lines))


class CommandLineTests(unittest.TestCase):
    def parse(self, argv):
        parser = SMOKE.build_parser()
        return SMOKE.options_from_args(parser.parse_args(argv), parser)

    def test_gates_all_expands_to_every_gate(self):
        parsed = self.parse(["run", "--target", "192.0.2.45"])
        self.assertEqual(parsed.gates, SMOKE.GATE_NAMES)
        self.assertFalse(parsed.allow_mutation)

    def test_a_gate_subset_is_kept_in_order(self):
        parsed = self.parse(["run", "--target", "192.0.2.45", "--gates", "soak,dsp"])
        self.assertEqual(parsed.gates, ("soak", "dsp"))

    def test_an_unknown_gate_is_a_usage_error(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                self.parse(["run", "--target", "192.0.2.45", "--gates", "reboot"])

    def test_an_empty_gate_list_is_a_usage_error(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                self.parse(["run", "--target", "192.0.2.45", "--gates", ","])

    def test_run_without_a_target_is_a_usage_error(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                self.parse(["run"])

    def test_plan_needs_no_target_and_prints_without_a_device(self):
        parsed = self.parse(["plan"])
        self.assertEqual(parsed.target, "")
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            self.assertEqual(SMOKE.main(["plan"]), 0)
        self.assertIn("teensy41-optical-fault 8", printed.getvalue())

    def test_the_evidence_directory_defaults_under_the_firmware_tree(self):
        parsed = self.parse(["run", "--target", "192.0.2.45"])
        self.assertTrue(parsed.evidence_dir.endswith(
            "test/evidence/network-hardening"))


if __name__ == "__main__":
    unittest.main()
