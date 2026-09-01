import contextlib
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE = ROOT / "tools" / "net" / "aius_rate_bench.py"
SPEC = importlib.util.spec_from_file_location("aius_rate_bench", MODULE)
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


def make_status(rate=48000, ingress_level=100, output_blocks=0):
    return {
        "build_revision": "0123456789ab+teensy41-optical",
        "build_utc": "2026-07-29T00:00:00Z",
        "event_sequence": 0,
        "overruns": 0,
        "event_overflows": 0,
        "network_budget_overruns": 0,
        "network_restarts": 0,
        "http_service_overruns": 0,
        "boot_generation": 17,
        "watchdog_early_resets": 0,
        "format": 1,
        "tx_gate": {"open": 1},
        "clock": {
            "carrier_locked": True,
            "carrier_ready": True,
            "carrier_rate_millihz": rate * 1000 + 125,
            "nominal_rate_hz": rate,
            "stream_rate_hz": rate,
            "output_rate_millihz": rate * 1000 + 125,
            "trim_ppm": 3,
            "master": "input",
            "state": "tracking",
            "rate_changes": 0,
            "trim_updates": 0,
            "structural_replans": 0,
            "invalid_windows": 0,
            "clock_faults": 0,
            "last_clock_fault": 0,
            "mismatch_blocks": 0,
            "unsupported_blocks": 0,
            "unsupported_episodes": 0,
            "tx_level_frames": 512,
            "tx_underrun_frames": 0,
            "ingress_level_words": ingress_level,
            "ingress_high_water_words": max(ingress_level, 442),
        },
        "audio_diag": {
            "ingress_generation": 7,
            "ingress_dropped_chunks": 0,
            "ingress_dropped_words": 0,
            "service_generation_resets": 0,
            "service_requested_resets": 0,
            "service_budget_overruns": 0,
            "parse_rejects": 0,
            "decode_rejects": 0,
            "output_drops": 0,
            "au_abandons": 0,
            "committed_output_blocks": output_blocks,
            "runtime_epoch_budget_overruns": 0,
        },
        "pipeline_diag": {"class": 1, "codec_pc": 0},
    }


def sample(elapsed, **status_args):
    return {"elapsed_s": elapsed, "status": make_status(**status_args)}


def transition_fixture():
    samples = []
    events = []
    output_blocks = 0
    changes = 0
    resets = 0
    unsupported = 0
    underruns = 0
    elapsed = 0
    phases = [
        (48000, False),
        (44100, False),
        (48000, False),
        (48000, True),
    ]
    unsupported_details = (40000, 42000)
    for phase_index, (rate, replay) in enumerate(phases):
        if phase_index:
            if phase_index <= 2:
                unsupported += 1
                events.append({
                    "sequence": len(events) + 1,
                    "uptime_ms": elapsed * 1000,
                    "type": 12,
                    "state": 3,
                    "detail": unsupported_details[phase_index - 1],
                })
            underruns += 128
            dark = sample(
                elapsed, rate=phases[phase_index - 1][0],
                output_blocks=output_blocks)
            dark["status"]["tx_gate"]["open"] = 0
            dark["status"]["clock"].update({
                "carrier_ready": False,
                "state": "unsupported" if phase_index <= 2 else "unlocked",
                "unsupported_episodes": unsupported,
                "tx_underrun_frames": underruns,
                "rate_changes": changes,
            })
            dark["status"]["audio_diag"][
                "service_requested_resets"] = resets
            samples.append(dark)
            elapsed += 1
            resets += 1
            if phase_index <= 2:
                changes += 1
                events.append({
                    "sequence": len(events) + 1,
                    "uptime_ms": elapsed * 1000,
                    "type": 11,
                    "state": 3,
                    "detail": rate,
                })
        for _ in range(2):
            output_blocks += 1
            entry = sample(
                elapsed, rate=rate, output_blocks=output_blocks)
            entry["status"]["clock"].update({
                "rate_changes": changes,
                "unsupported_episodes": unsupported,
                "tx_underrun_frames": underruns,
            })
            entry["status"]["audio_diag"][
                "service_requested_resets"] = resets
            if replay:
                entry["status"]["clock"].update({
                    "carrier_locked": False,
                    "carrier_ready": False,
                    "output_rate_millihz": 48000000,
                    "master": "output",
                    "state": "replay",
                })
            samples.append(entry)
            elapsed += 1
    return samples, events


def analyze_48(samples, **options):
    configured = {
        "minimum_duration_s": 0,
        "settle_s": 0,
        "max_follow_error_millihz": 1000,
        **options,
    }
    return BENCH.analyze_samples(samples, [], "48k-10m", configured)


def evidence_file(path, samples, scenario="48k-10m", analysis_options=None,
                  external=None):
    identity = BENCH.build_identity(samples[0]["status"])
    records = [{
        "kind": "metadata",
        "schema_version": BENCH.SCHEMA_VERSION,
        "scenario": scenario,
        "build_identity": identity,
        "analysis_options": analysis_options or {},
    }]
    if external is not None:
        records[0]["raw_external_observation_json"] = json.dumps(external)
    empty_events = json.dumps({"cursor": 0, "next": 0, "events": []},
                              separators=(",", ":"))
    for entry in samples:
        records.append({
            "kind": "sample",
            "elapsed_s": entry["elapsed_s"],
            "raw_status_json": json.dumps(
                entry["status"], separators=(",", ":")),
            "raw_events_json": empty_events,
        })
    path.write_text(
        "".join(json.dumps(record) + "\n" for record in records),
        encoding="utf-8")


class MathTests(unittest.TestCase):
    def test_least_squares_slope_is_exact_for_a_line(self):
        points = [(0, 3), (2, 8), (5, 15.5), (9, 25.5)]
        self.assertEqual(BENCH.least_squares_slope(points), 2.5)

    def test_unsigned_delta_crosses_uint32_wrap(self):
        self.assertEqual(BENCH.unsigned_delta(0xFFFFFFFE, 1), 3)
        with self.assertRaises(ValueError):
            BENCH.unsigned_delta(True, 1)


class AnalyzerTests(unittest.TestCase):
    def test_48k_pass_pins_slope_and_progress(self):
        samples = [
            sample(0, ingress_level=100, output_blocks=10),
            sample(4, ingress_level=101, output_blocks=20),
            sample(8, ingress_level=102, output_blocks=30),
        ]
        result = analyze_48(samples)
        self.assertEqual(result["verdict"], "pass")
        self.assertEqual(
            result["metrics"]["ingress_slope_words_per_s"], 0.25)
        self.assertEqual(
            result["metrics"]["committed_output_blocks_delta"], 20)

    def test_trim_cadence_counts_only_samples_after_settling(self):
        samples = [
            sample(0, output_blocks=1),
            sample(10, output_blocks=2),
            sample(20, output_blocks=3),
            sample(30, output_blocks=4),
        ]
        samples[0]["status"]["clock"]["trim_updates"] = 7
        samples[1]["status"]["clock"]["trim_updates"] = 9
        samples[2]["status"]["clock"]["trim_updates"] = 11
        samples[3]["status"]["clock"]["trim_updates"] = 12
        result = analyze_48(samples, settle_s=10)
        self.assertEqual(result["metrics"]["trim_updates_after_settle"], 3)
        self.assertIn("trim-cadence",
                      {failure["code"] for failure in result["failures"]})

    def test_rate_state_mismatch_fails(self):
        samples = [
            sample(0, output_blocks=1),
            sample(1, output_blocks=2),
            sample(2, output_blocks=3),
        ]
        samples[1]["status"]["clock"]["stream_rate_hz"] = 44100
        result = analyze_48(samples)
        self.assertIn("rate-state-mismatch",
                      {failure["code"] for failure in result["failures"]})

    def test_harmful_counter_delta_fails_across_wrap(self):
        samples = [
            sample(0, output_blocks=1),
            sample(1, output_blocks=2),
            sample(2, output_blocks=3),
        ]
        samples[0]["status"]["audio_diag"]["output_drops"] = 0xFFFFFFFF
        samples[1]["status"]["audio_diag"]["output_drops"] = 0
        samples[2]["status"]["audio_diag"]["output_drops"] = 0
        result = analyze_48(samples)
        self.assertEqual(
            result["metrics"]["counter_deltas"]["audio_diag.output_drops"], 1)
        self.assertIn("harmful-counter",
                      {failure["code"] for failure in result["failures"]})

    def test_missing_and_malformed_payloads_fail_cleanly(self):
        missing = [
            sample(0, output_blocks=1),
            sample(1, output_blocks=2),
        ]
        del missing[1]["status"]["clock"]["carrier_ready"]
        result = analyze_48(missing)
        self.assertEqual(result["verdict"], "fail")
        self.assertEqual(result["failures"][0]["code"], "payload")

        malformed = [
            sample(0, output_blocks=1),
            sample(1, output_blocks=2),
        ]
        malformed[1]["status"]["clock"]["trim_updates"] = "one"
        result = analyze_48(malformed)
        self.assertEqual(result["failures"][0]["code"], "payload")

    def test_negative_requires_counted_mute_without_output_progress(self):
        samples = [
            sample(0, rate=48000, output_blocks=10),
            sample(5, rate=48000, output_blocks=10),
            sample(10, rate=48000, output_blocks=10),
        ]
        for entry in samples:
            entry["status"]["clock"]["stream_rate_hz"] = 44100
            entry["status"]["clock"]["state"] = "refused"
            entry["status"]["tx_gate"]["open"] = 0
        samples[1]["status"]["clock"]["tx_underrun_frames"] = 128
        samples[2]["status"]["clock"]["tx_underrun_frames"] = 512
        samples[1]["status"]["clock"]["mismatch_blocks"] = 2
        samples[2]["status"]["clock"]["mismatch_blocks"] = 4
        result = BENCH.analyze_samples(samples, [], "negative", {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "carrier_rate_hz": 48000,
            "stream_rate_hz": 44100,
        })
        self.assertEqual(result["verdict"], "pass")
        self.assertEqual(result["metrics"]["mismatch_blocks_after_settle"], 4)
        self.assertEqual(
            result["metrics"]["counter_deltas"]["clock.tx_underrun_frames"],
            512,
        )

    def test_pcm_and_encoded_non_48k_gates(self):
        pcm_samples = [
            sample(0, rate=44100, output_blocks=1),
            sample(1, rate=44100, output_blocks=2),
            sample(2, rate=44100, output_blocks=3),
        ]
        pcm = BENCH.analyze_samples(pcm_samples, [], "pcm-120s", {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "rate_hz": 44100,
            "max_follow_error_millihz": 1000,
        }, {
            "audio_rate_hz": 44100,
            "continuous_output": True,
        })
        self.assertEqual(pcm["verdict"], "pass")

        encoded_samples = [
            sample(0, rate=44100, output_blocks=1),
            sample(1, rate=44100, output_blocks=2),
            sample(2, rate=44100, output_blocks=3),
        ]
        for entry in encoded_samples:
            entry["status"]["format"] = 2
            entry["status"]["pipeline_diag"] = {"class": 2, "codec_pc": 11}
        encoded = BENCH.analyze_samples(encoded_samples, [], "encoded", {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "rate_hz": 44100,
            "codec": "dts",
            "max_follow_error_millihz": 1000,
        })
        self.assertEqual(encoded["verdict"], "pass")

    def test_pcm_48k_smoke_requires_confirmed_progress(self):
        samples = [
            sample(0, rate=48000, output_blocks=1),
            sample(60, rate=48000, output_blocks=2),
            sample(120, rate=48000, output_blocks=3),
        ]
        for entry in samples:
            entry["status"]["pipeline_diag"]["pcm_confirmed"] = 1

        result = BENCH.analyze_samples(samples, [], "pcm-48k-smoke", {
            "minimum_duration_s": 120,
            "settle_s": 30,
            "max_follow_error_millihz": 1000,
        })

        self.assertEqual(result["verdict"], "pass")

        samples[-1]["status"]["pipeline_diag"]["pcm_confirmed"] = 0
        rejected = BENCH.analyze_samples(samples, [], "pcm-48k-smoke", {
            "minimum_duration_s": 120,
            "settle_s": 30,
            "max_follow_error_millihz": 1000,
        })
        self.assertEqual(rejected["failures"][0]["code"], "format")

    def test_transition_sequence_events_resets_and_progress(self):
        samples, events = transition_fixture()
        result = BENCH.analyze_samples(samples, events, "transitions", {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "phase_samples": 2,
            "max_follow_error_millihz": 1000,
        }, {"old_rate_output_frames": 0})
        self.assertEqual(result["verdict"], "pass")
        self.assertEqual(
            result["metrics"]["transition_reset_deltas"], [1, 1, 1])
        self.assertEqual(
            result["metrics"]["counter_deltas"][
                "clock.tx_underrun_frames"], 384)
        self.assertEqual(
            result["metrics"]["counter_deltas"][
                "clock.unsupported_episodes"], 2)

    def test_transition_rejects_open_gate_underrun(self):
        samples, events = transition_fixture()
        for entry in samples[4:]:
            entry["status"]["clock"]["tx_underrun_frames"] += 64
        result = BENCH.analyze_samples(samples, events, "transitions", {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "phase_samples": 2,
            "max_follow_error_millihz": 1000,
        }, {"old_rate_output_frames": 0})
        self.assertIn(
            "transition-open-gate-underrun",
            {failure["code"] for failure in result["failures"]})

    def test_transition_rejects_open_gate_unsupported_episode(self):
        samples, events = transition_fixture()
        for entry in samples[4:]:
            entry["status"]["clock"]["unsupported_episodes"] += 1
        events.append({
            "sequence": len(events) + 1,
            "uptime_ms": 4000,
            "type": 12,
            "state": 3,
            "detail": 40000,
        })
        result = BENCH.analyze_samples(samples, events, "transitions", {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "phase_samples": 2,
            "max_follow_error_millihz": 1000,
        }, {"old_rate_output_frames": 0})
        self.assertIn(
            "transition-open-gate-unsupported",
            {failure["code"] for failure in result["failures"]})

    def test_transition_rejects_unsupported_event_mismatch(self):
        samples, events = transition_fixture()
        events = [
            event for event in events
            if not (event["type"] == 12 and event["detail"] == 42000)
        ]
        result = BENCH.analyze_samples(samples, events, "transitions", {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "phase_samples": 2,
            "max_follow_error_millihz": 1000,
        }, {"old_rate_output_frames": 0})
        self.assertIn(
            "unsupported-event-count",
            {failure["code"] for failure in result["failures"]})

    def test_transition_rejects_supported_unsupported_event_detail(self):
        samples, events = transition_fixture()
        next(event for event in events if event["type"] == 12)[
            "detail"] = 44100
        result = BENCH.analyze_samples(samples, events, "transitions", {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "phase_samples": 2,
            "max_follow_error_millihz": 1000,
        }, {"old_rate_output_frames": 0})
        self.assertIn(
            "unsupported-event-detail",
            {failure["code"] for failure in result["failures"]})

    def test_transition_rejects_unmeasured_external_capture(self):
        samples, events = transition_fixture()
        result = BENCH.analyze_samples(samples, events, "transitions", {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "phase_samples": 2,
            "max_follow_error_millihz": 1000,
        }, {"old_rate_output_frames": None})
        codes = {failure["code"] for failure in result["failures"]}
        self.assertIn("external-observation", codes)
        self.assertNotIn("old-rate-output", codes)

    def test_waveform_requires_exact_capture_correlation(self):
        samples = [
            sample(0, output_blocks=1),
            sample(1, output_blocks=2),
            sample(2, output_blocks=3),
        ]
        samples[1]["status"]["clock"]["trim_updates"] = 1
        samples[2]["status"]["clock"]["trim_updates"] = 1
        samples[1]["status"]["clock"]["invalid_windows"] = 1
        samples[2]["status"]["clock"]["invalid_windows"] = 1
        external = {
            "capture_frame_gaps": 0,
            "discontinuities_outside_invalid": 0,
            "invalid_intervals_frames": [512],
            "correlated_trim_updates": 1,
            "correlated_invalid_windows": 1,
        }
        result = BENCH.analyze_samples(samples, [], "waveform", {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "max_follow_error_millihz": 1000,
        }, external)
        self.assertEqual(result["verdict"], "pass")

    def test_waveform_rejects_missing_invalid_interval_measurement(self):
        samples = [
            sample(0, output_blocks=1),
            sample(1, output_blocks=2),
            sample(2, output_blocks=3),
        ]
        samples[1]["status"]["clock"]["trim_updates"] = 1
        samples[2]["status"]["clock"]["trim_updates"] = 2
        samples[1]["status"]["clock"]["invalid_windows"] = 1
        samples[2]["status"]["clock"]["invalid_windows"] = 2
        external = {
            "capture_frame_gaps": 0,
            "discontinuities_outside_invalid": 0,
            "invalid_intervals_frames": [512],
            "correlated_trim_updates": 2,
            "correlated_invalid_windows": 2,
        }
        result = BENCH.analyze_samples(samples, [], "waveform", {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "max_follow_error_millihz": 1000,
        }, external)
        self.assertEqual(result["verdict"], "fail")
        self.assertIn(
            "invalid-interval-evidence",
            {failure["code"] for failure in result["failures"]})


class ExitSemanticsTests(unittest.TestCase):
    def run_main(self, argv):
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), \
                contextlib.redirect_stderr(stderr):
            code = BENCH.main(argv)
        return code, stdout.getvalue(), stderr.getvalue()

    def test_analyze_returns_zero_for_pass_and_one_for_gate_failure(self):
        samples = [
            sample(0, ingress_level=100, output_blocks=1),
            sample(300, ingress_level=100, output_blocks=2),
            sample(600, ingress_level=100, output_blocks=3),
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "evidence.jsonl"
            evidence_file(path, samples)
            code, stdout, stderr = self.run_main([
                "analyze", "48k-10m", "--evidence", str(path),
                "--settle", "0",
            ])
            self.assertEqual(code, 0, stderr)
            self.assertEqual(json.loads(stdout)["verdict"], "pass")

            samples[-1]["status"]["audio_diag"]["output_drops"] = 1
            evidence_file(path, samples)
            code, stdout, stderr = self.run_main([
                "analyze", "48k-10m", "--evidence", str(path),
                "--settle", "0",
            ])
            self.assertEqual(code, 1, stderr)
            self.assertEqual(json.loads(stdout)["verdict"], "fail")

    def test_analyze_returns_two_for_invalid_raw_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "evidence.jsonl"
            path.write_text('{"kind":"metadata"}\nnot-json\n',
                            encoding="utf-8")
            code, _stdout, stderr = self.run_main([
                "analyze", "48k-10m", "--evidence", str(path),
            ])
        self.assertEqual(code, 2)
        self.assertIn("malformed JSON", stderr)

    def test_analyze_reuses_recorded_options_and_external_observation(self):
        samples = [
            sample(0, rate=44100, output_blocks=1),
            sample(60, rate=44100, output_blocks=2),
            sample(120, rate=44100, output_blocks=3),
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "pcm.jsonl"
            evidence_file(
                path, samples, scenario="pcm-120s",
                analysis_options={
                    "settle_s": 0,
                    "rate_hz": 44100,
                    "max_follow_error_millihz": 1000,
                    "phase_samples": 2,
                },
                external={
                    "audio_rate_hz": 44100,
                    "continuous_output": True,
                })
            code, stdout, stderr = self.run_main([
                "analyze", "pcm-120s", "--evidence", str(path),
            ])
        self.assertEqual(code, 0, stderr)
        self.assertEqual(json.loads(stdout)["verdict"], "pass")


class TransportRetryTests(unittest.TestCase):
    """get_json retries transient transport drops (the device's
    one-client-at-a-time close and momentary send-stall timeouts) but never an
    HTTP or JSON error, which is real."""

    class _Resp:
        def __init__(self, status, body):
            self.status = status
            self._body = body

        def read(self):
            return self._body

    class _Conn:
        def __init__(self, outcome):
            self._outcome = outcome

        def request(self, *args, **kwargs):
            pass

        def getresponse(self):
            if self._outcome == "drop":
                raise BENCH.http.client.RemoteDisconnected("closed")
            status, body = self._outcome
            return TransportRetryTests._Resp(status, body)

        def close(self):
            pass

    @contextlib.contextmanager
    def _patched(self, outcomes):
        calls = {"n": 0}

        def factory(host, port, timeout=None):
            outcome = outcomes[calls["n"]]
            calls["n"] += 1
            return TransportRetryTests._Conn(outcome)

        original_conn = BENCH.http.client.HTTPConnection
        original_sleep = BENCH.time.sleep
        BENCH.http.client.HTTPConnection = factory
        BENCH.time.sleep = lambda _seconds: None
        try:
            yield calls
        finally:
            BENCH.http.client.HTTPConnection = original_conn
            BENCH.time.sleep = original_sleep

    def test_retries_transient_drops_then_succeeds(self):
        body = json.dumps({"ok": True}).encode("utf-8")
        transport = BENCH.HttpTransport(("10.0.0.1", 80), timeout=0.01)
        with self._patched(["drop", "drop", (200, body)]) as calls:
            result = transport.get_json("/api/status", attempts=4)
        self.assertEqual(result["body"], {"ok": True})
        self.assertEqual(calls["n"], 3)

    def test_raises_after_exhausting_attempts(self):
        transport = BENCH.HttpTransport(("10.0.0.1", 80), timeout=0.01)
        with self._patched(["drop", "drop", "drop", "drop"]) as calls:
            with self.assertRaises(BENCH.BenchError):
                transport.get_json("/api/status", attempts=4)
        self.assertEqual(calls["n"], 4)

    def test_does_not_retry_http_error(self):
        transport = BENCH.HttpTransport(("10.0.0.1", 80), timeout=0.01)
        with self._patched([(503, b'{"error":"sd_busy"}')]) as calls:
            with self.assertRaises(BENCH.BenchError):
                transport.get_json("/api/status", attempts=4)
        self.assertEqual(calls["n"], 1)


if __name__ == "__main__":
    unittest.main()
