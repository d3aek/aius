"""Unit tests for the rate-gate capture reducers.

Drives the public reducer interface of firmware/tools/rate_gate_capture/
capture.py and recorder_transitions.py against synthetic waveforms and
status series: gap-injected, 512-run-injected, held-block, sample-slip and
rate-step signals stand in for the live U24 capture and the board's flight
recorder, so every field reduction is provable off-board.

Every test drives the REAL reducers -- there is no reimplementation of a
detector here. Where a fixture exists to convict a specific defect the
docstring names it.
"""

import contextlib
import importlib.util
import io
import json
import math
import pathlib
import tempfile
import unittest

import numpy as np


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE = ROOT / "tools" / "rate_gate_capture" / "capture.py"
SPEC = importlib.util.spec_from_file_location("rate_gate_capture", MODULE)
CAP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAP)

# Load the rate-bench test module (which itself loads aius_rate_bench) so the
# evidence-format integration test drives the real bench analyzer.
BENCH_TEST_PATH = ROOT / "test" / "rate-runner" / "test_rate_bench.py"
BENCH_SPEC = importlib.util.spec_from_file_location(
    "test_rate_bench", BENCH_TEST_PATH)
BENCH_TEST = importlib.util.module_from_spec(BENCH_SPEC)
BENCH_SPEC.loader.exec_module(BENCH_TEST)

REC_MODULE = ROOT / "tools" / "rate_gate_capture" / "recorder_transitions.py"
REC_SPEC = importlib.util.spec_from_file_location(
    "recorder_transitions", REC_MODULE)
REC = importlib.util.module_from_spec(REC_SPEC)
REC_SPEC.loader.exec_module(REC)

# The documented stimulus: tools/source_qualification/tone_gen.py defaults to
# peak 410. Every detector test that claims "the documented level" uses it,
# because a threshold tuned at 8000 can be blind at 410 and still look green.
TONE_GEN_AMPLITUDE = 410


def output_segment(rate, frames, freq=997.0, amp=8000):
    """A 997 Hz tone as the board would OUTPUT it at `rate` -- i.e. sampled
    at `rate`. Its samples-per-cycle (rate / 997) is what the recorder-based
    detector reads the output rate from, whatever rate it is played back
    at."""
    n = np.arange(frames, dtype=np.float64)
    mono = np.rint(amp * np.sin(2 * math.pi * freq * n / rate))
    return np.repeat(mono.astype(np.int32)[:, None], 2, axis=1)


def output_stream(segments):
    return np.concatenate([output_segment(rate, frames)
                           for rate, frames in segments], axis=0)


def tone(frames, capture_rate, hz=CAP.TONE_HZ, amp=8000):
    n = np.arange(frames, dtype=np.float64)
    mono = np.rint(amp * np.sin(2 * math.pi * hz * n / capture_rate))
    return np.repeat(mono.astype(np.int32)[:, None], 2, axis=1)


def status_series(seconds, invalid_at=None, trim_at=None, rate=48000,
                  rate_changes=None, offset_s=0.0):
    """One entry per second. invalid_at / trim_at are the elapsed_s from
    which the counter reads 1 (a single increment); rate_changes maps
    elapsed_s -> new stream_rate_hz. offset_s shifts every elapsed_s, which
    is what a real run does: the status clock starts when the stimulus does,
    arecord opens only after the board has locked."""
    rate_changes = rate_changes or {}
    series = []
    current_rate = rate
    for second in range(seconds):
        if second in rate_changes:
            current_rate = rate_changes[second]
        series.append({
            "elapsed_s": float(second) + offset_s,
            "trim_updates": 1 if trim_at is not None and second >= trim_at
            else 0,
            "invalid_windows": 1 if invalid_at is not None
            and second >= invalid_at else 0,
            "stream_rate_hz": current_rate,
        })
    return series


class ZeroRunTests(unittest.TestCase):
    def test_detects_only_runs_at_least_two_frames(self):
        active = tone(2000, 48000)
        active[500] = 0  # single incidental zero -> not a run
        active[900:900 + 512] = 0
        runs = CAP.zero_runs(active)
        self.assertEqual(runs, [(900, 900 + 512)])

    def test_ignores_natural_single_frame_crossings(self):
        active = tone(48000, 48000)
        self.assertEqual(CAP.zero_runs(active), [])


class PcmReducerTests(unittest.TestCase):
    def test_continuous_output_true_and_rate_reported(self):
        active = tone(48000, 44100)
        result = CAP.reduce_pcm(active, 44100, 44100)
        self.assertTrue(result["external"]["continuous_output"])
        self.assertEqual(result["external"]["audio_rate_hz"], 44100)

    def test_dropout_breaks_continuity(self):
        active = tone(48000, 44100)
        active[10000:10000 + 512] = 0
        result = CAP.reduce_pcm(active, 44100, 44100)
        self.assertFalse(result["external"]["continuous_output"])

    def test_rate_model_recovers_the_output_rate_read_at_another_rate(self):
        """Frames the board rendered at 44.1 kHz, read back at 48 kHz.

        The physical answer is 44100 (R_out = 997 * R_capture / f_meas).
        The inverted model this fixture convicts (expected * f_meas / 997)
        reports 52245."""
        active = tone(4 * 48000, 44100)          # rendered at 44.1 kHz
        result = CAP.reduce_pcm(active, 48000, 44100)
        self.assertEqual(result["external"]["audio_rate_hz"], 44100)
        self.assertLess(abs(result["evidence"]["audio_rate_hz_raw"] - 44100),
                        200)
        self.assertEqual(result["evidence"]["audio_rate_hz_source"],
                         "inferred-from-tone")
        self.assertFalse(result["evidence"]["self_referential"])

    def test_self_referential_capture_is_declared_not_measured(self):
        """arecord opened at the rate under test cannot measure that rate:
        the U24 capture clock locks to the carrier, so f_meas is 997 Hz by
        construction. Gate 2 must not read that as an independent
        measurement."""
        active = tone(4 * 48000, 48000)
        evidence = CAP.reduce_pcm(active, 48000, 48000)["evidence"]
        self.assertTrue(evidence["self_referential"])
        self.assertEqual(evidence["audio_rate_hz_source"],
                         "capture-clock-locked")
        self.assertIn("not independently measured",
                      evidence["rate_measurement_note"])
        self.assertEqual(evidence["capture_rate_hz"], 48000)

    def test_raw_estimate_and_snapped_rate_are_both_reported(self):
        # A tone 10 Hz-equivalent off nominal: the raw estimate is not a
        # nominal rate, the reported rate snaps to one.
        active = tone(4 * 48000, 48000, hz=CAP.TONE_HZ * 47990 / 48000)
        evidence = CAP.reduce_pcm(active, 48000, 48000)["evidence"]
        self.assertEqual(evidence["audio_rate_hz"], 48000)
        self.assertNotEqual(int(round(evidence["audio_rate_hz_raw"])), 48000)
        self.assertLess(abs(evidence["audio_rate_hz_raw"] - 48000), 100)

    def test_rate_far_from_every_nominal_is_not_snapped(self):
        # A 700 Hz tone read at 48 kHz implies ~68 kHz out: no nominal rate
        # is within tolerance, so the field is null rather than a rounded
        # number gate 2 could accidentally match.
        active = tone(4 * 48000, 48000, hz=700.0)
        result = CAP.reduce_pcm(active, 48000, 48000)
        self.assertIsNone(result["external"]["audio_rate_hz"])
        self.assertIsNotNone(result["evidence"]["audio_rate_hz_raw"])


class DiscontinuityDetectorTests(unittest.TestCase):
    """The detector must see artifacts at the DOCUMENTED stimulus level.

    tone_gen.py defaults to peak 410, whose largest adjacent-sample jump at
    997 Hz / 48 kHz is ~54 LSB. A fixed 1000-LSB floor is therefore blind to
    every artifact below by construction: a 512-frame held block, a 1-sample
    drop and a 2-sample drop all read as clean."""

    def _base(self, amp=TONE_GEN_AMPLITUDE, seconds=3):
        return tone(seconds * 48000, 48000, amp=amp)

    def test_clean_tone_has_no_discontinuities(self):
        events, _evidence = CAP.discontinuities(self._base(), 48000, [])
        self.assertEqual(events, [])

    def test_held_block_detected_at_documented_amplitude(self):
        active = self._base()
        active[48000:48000 + 512] = active[48000 - 1]
        events, evidence = CAP.discontinuities(active, 48000, [])
        self.assertEqual(len(events), 1)
        self.assertLess(abs(events[0] - 48000), 128)
        self.assertEqual(evidence["held_run_count"], 1)

    def test_one_sample_drop_detected_at_documented_amplitude(self):
        base = self._base()
        active = np.concatenate([base[:48000], base[48001:]])
        events, _evidence = CAP.discontinuities(active, 48000, [])
        self.assertEqual(len(events), 1)
        self.assertLess(abs(events[0] - 48000), 128)

    def test_two_sample_drop_detected_at_documented_amplitude(self):
        base = self._base()
        active = np.concatenate([base[:48000], base[48002:]])
        events, _evidence = CAP.discontinuities(active, 48000, [])
        self.assertEqual(len(events), 1)
        self.assertLess(abs(events[0] - 48000), 128)

    def test_thresholds_track_the_measured_amplitude(self):
        quiet = CAP.discontinuities(self._base(amp=410), 48000, [])[1]
        loud = CAP.discontinuities(self._base(amp=8000), 48000, [])[1]
        self.assertLess(abs(quiet["stimulus_amplitude"] - 410), 20)
        self.assertLess(abs(loud["stimulus_amplitude"] - 8000), 200)
        # ~54 LSB peak slope at 410; a fixed 1000-LSB floor is 18x that.
        self.assertLess(quiet["expected_peak_slope"], 60)
        self.assertLess(quiet["jump_threshold"], 100)
        self.assertGreater(loud["jump_threshold"],
                           quiet["jump_threshold"] * 10)


class WaveformReducerTests(unittest.TestCase):
    def _base(self, seconds=9):
        return tone(seconds * 48000, 48000)

    def test_clean_correlated_run_and_trim(self):
        active = self._base()
        active[3 * 48000:3 * 48000 + 512] = 0          # invalid window at 3 s
        active[3 * 48000 + 4000] = 20000     # spike by the full apply at 3 s
        active[3 * 48000 + 4001] = -20000
        series = status_series(9, invalid_at=3)
        external = CAP.reduce_waveform(active, 48000, series)["external"]
        self.assertEqual(external["capture_frame_gaps"], 0)
        self.assertEqual(external["discontinuities_outside_invalid"], 0)
        self.assertEqual(external["invalid_intervals_frames"], [512])
        self.assertEqual(external["correlated_trim_updates"], 1)
        self.assertEqual(external["correlated_invalid_windows"], 1)

    def test_uncorrelated_zero_run_is_a_gap(self):
        active = self._base()
        active[3 * 48000:3 * 48000 + 512] = 0     # correlated invalid window
        active[7 * 48000:7 * 48000 + 300] = 0     # gap, no status increment
        series = status_series(9, invalid_at=3)
        external = CAP.reduce_waveform(active, 48000, series)["external"]
        self.assertEqual(external["correlated_invalid_windows"], 1)
        self.assertEqual(external["invalid_intervals_frames"], [512])
        self.assertEqual(external["capture_frame_gaps"], 1)

    def test_discontinuity_without_trim_counts_outside(self):
        active = self._base()
        active[5 * 48000] = 25000                  # spike, no trim increment
        active[5 * 48000 + 1] = -25000
        series = status_series(9)
        external = CAP.reduce_waveform(active, 48000, series)["external"]
        self.assertEqual(external["correlated_trim_updates"], 0)
        self.assertGreaterEqual(
            external["discontinuities_outside_invalid"], 1)

    def test_invalid_window_edges_are_not_counted_as_discontinuities(self):
        active = self._base()
        active[3 * 48000:3 * 48000 + 512] = 0
        series = status_series(9, invalid_at=3)
        external = CAP.reduce_waveform(active, 48000, series)["external"]
        self.assertEqual(external["discontinuities_outside_invalid"], 0)

    def _offset_case(self, lock_wait_s):
        """A run whose status clock starts lock_wait_s before arecord opens
        -- the real shape, since aplay must start and the board must lock
        before the capture device can be opened."""
        active = self._base()
        active[3 * 48000:3 * 48000 + 512] = 0      # capture-domain 3 s
        active[3 * 48000 + 4000] = 20000
        active[3 * 48000 + 4001] = -20000
        series = status_series(9, invalid_at=3, offset_s=lock_wait_s)
        return active, series

    def test_lock_wait_offset_correlates_when_the_origin_is_subtracted(self):
        """The board's rate lock costs up to 20 s before arecord opens, so
        every status elapsed_s is that far ahead of its capture frame. With
        the capture origin supplied, an 8 s offset correlates exactly."""
        active, series = self._offset_case(8.0)
        external = CAP.reduce_waveform(
            active, 48000, series, capture_start_offset_s=8.0)["external"]
        self.assertEqual(external["correlated_invalid_windows"], 1)
        self.assertEqual(external["correlated_trim_updates"], 1)
        self.assertEqual(external["invalid_intervals_frames"], [512])
        self.assertEqual(external["capture_frame_gaps"], 0)

    def test_lock_wait_offset_ignored_cannot_correlate(self):
        """The negative control for the fix: 8 s is far outside the 1.5 s
        alignment margin, so treating the run origin as the capture origin
        correlates nothing at all."""
        active, series = self._offset_case(8.0)
        external = CAP.reduce_waveform(
            active, 48000, series, capture_start_offset_s=0.0)["external"]
        self.assertEqual(external["correlated_invalid_windows"], 0)
        self.assertEqual(external["correlated_trim_updates"], 0)
        self.assertEqual(external["capture_frame_gaps"], 1)

    def test_active_bounds_trim_is_subtracted_too(self):
        """active_bounds() drops the silent lead-in, so active frame 0 is
        already some way into the capture; that trim is the second origin."""
        active, series = self._offset_case(0.0)
        trimmed = active[2 * 48000:]
        external = CAP.reduce_waveform(
            trimmed, 48000, series, active_start_frame=2 * 48000)["external"]
        self.assertEqual(external["correlated_invalid_windows"], 1)
        self.assertEqual(external["correlated_trim_updates"], 1)

    def test_fill_writes_in_every_poll_never_excuse_a_jump(self):
        """Fill writes are carrier-continuous: a jump far from any full
        apply stays outside even when a fill stamp moves in every poll."""
        active = self._base()
        active[6 * 48000] = 20000
        active[6 * 48000 + 1] = -20000
        series = status_series(9)
        for index, entry in enumerate(series):
            entry["trim_applied_at_ms"] = 100
            entry["fill_applied_at_ms"] = 1000 + index
        external = CAP.reduce_waveform(active, 48000, series)
        self.assertEqual(external["external"]["correlated_trim_updates"], 0)
        self.assertGreaterEqual(
            external["external"]["discontinuities_outside_invalid"], 1)
        self.assertEqual(
            external["evidence"]["status_trim_increments"], 8)

    def test_trim_commit_without_full_apply_does_not_excuse_a_jump(self):
        active = self._base()
        active[6 * 48000] = 20000
        active[6 * 48000 + 1] = -20000
        series = status_series(9, trim_at=6)
        external = CAP.reduce_waveform(active, 48000, series)["external"]
        self.assertEqual(external["correlated_trim_updates"], 0)
        self.assertGreaterEqual(
            external["discontinuities_outside_invalid"], 1)


class CaptureHealthTests(unittest.TestCase):
    """arecord's exit status, its ALSA overrun log and a short WAV are
    capture-domain faults; none of them may reach the gate as a clean
    observation."""

    def _geometry(self, seconds, rate=48000):
        return {"channels": 2, "sample_rate_hz": rate,
                "sample_width_bytes": 2, "frames": int(seconds * rate)}

    def test_clean_capture_has_no_failures(self):
        health = {"arecord_returncode": 0, "alsa_overruns": 0,
                  "requested_seconds": 130.0}
        self.assertEqual(CAP.capture_failures(health, self._geometry(130)), [])

    def test_nonzero_arecord_exit_fails_the_capture(self):
        health = {"arecord_returncode": 1, "alsa_overruns": 0,
                  "requested_seconds": 130.0}
        failures = CAP.capture_failures(health, self._geometry(130))
        self.assertIn("arecord-exit-1", failures)

    def test_short_wav_fails_the_capture(self):
        health = {"arecord_returncode": 0, "alsa_overruns": 0,
                  "requested_seconds": 130.0}
        failures = CAP.capture_failures(health, self._geometry(90))
        self.assertTrue(any(name.startswith("capture-short")
                            for name in failures))

    def _skip_health(self, attempts, skips):
        return {"arecord_returncode": 0, "alsa_overruns": 0,
                "requested_seconds": 130.0,
                "poll_attempts": attempts, "poll_skips": skips}

    def test_skips_at_five_percent_pass(self):
        failures = CAP.capture_failures(
            self._skip_health(100, 5), self._geometry(130))
        self.assertEqual(failures, [])

    def test_skips_over_five_percent_fail(self):
        failures = CAP.capture_failures(
            self._skip_health(100, 6), self._geometry(130))
        self.assertTrue(any(name.startswith("status-poll-skips")
                            for name in failures))


class HttpGetStatusTests(unittest.TestCase):
    """A 503 is a counted skip; transport errors keep the retry."""

    def _patched(self, side_effect):
        import unittest.mock as mock
        return (mock.patch("urllib.request.urlopen", side_effect=side_effect),
                mock.patch.object(CAP.time, "sleep"))

    def test_503_is_a_skip_not_retried_not_raised(self):
        import urllib.error
        calls = []

        def fail(*_a, **_k):
            calls.append(1)
            raise urllib.error.HTTPError("u", 503, "busy", {}, None)

        opener, sleeper = self._patched(fail)
        with opener, sleeper:
            self.assertEqual(CAP.http_get("http://x/api/status", None),
                             (None, None))
        self.assertEqual(len(calls), 1)

    def test_other_http_error_is_raised(self):
        import urllib.error

        def fail(*_a, **_k):
            raise urllib.error.HTTPError("u", 500, "bad", {}, None)

        opener, sleeper = self._patched(fail)
        with opener, sleeper, self.assertRaises(urllib.error.HTTPError):
            CAP.http_get("http://x/api/status", None)

    def test_transport_error_still_retries(self):
        import urllib.error
        calls = []

        def fail(*_a, **_k):
            calls.append(1)
            raise urllib.error.URLError("reset")

        opener, sleeper = self._patched(fail)
        with opener, sleeper, self.assertRaises(RuntimeError):
            CAP.http_get("http://x/api/status", None, attempts=3)
        self.assertEqual(len(calls), 3)

    def test_overruns_become_capture_frame_gaps(self):
        external = {"capture_frame_gaps": 0}
        CAP.apply_capture_health("waveform", external,
                                 {"alsa_overruns": 3}, [])
        self.assertEqual(external["capture_frame_gaps"], 3)

    def test_hard_failure_neutralises_the_gate_fields(self):
        external = {"audio_rate_hz": 44100, "continuous_output": True}
        CAP.apply_capture_health("pcm-120s", external, {"alsa_overruns": 0},
                                 ["arecord-exit-1"])
        self.assertIsNone(external["audio_rate_hz"])
        self.assertFalse(external["continuous_output"])
        self.assertEqual(external["capture_failure"], ["arecord-exit-1"])

    def test_alsa_log_overruns_are_parsed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "arecord.log"
            path.write_text(
                "Recording WAVE 'capture.wav'\n"
                "overrun!!! (at least 12.345 ms long)\n"
                "overrun!!! (at least 4.000 ms long)\n",
                encoding="utf-8")
            count, lines = CAP.parse_alsa_log(path)
        self.assertEqual(count, 2)
        self.assertEqual(len(lines), 2)


class TransitionReducerTests(unittest.TestCase):
    def test_clean_transition_has_no_old_rate_frames(self):
        """A clean 48 kHz -> 44.1 kHz switch seen by a FIXED 48 kHz capture:
        the post-boundary frames are rendered at 44.1 kHz, so they read at
        997 * 48000/44100 = 1085 Hz. A capture that kept reading 997 Hz past
        the boundary would be stale 48 kHz output, not a clean switch."""
        capture_rate = 48000
        active = np.concatenate([
            tone(3 * capture_rate, capture_rate),   # rendered at 48 kHz
            tone(3 * capture_rate, 44100),          # rendered at 44.1 kHz
        ])
        series = status_series(6, rate_changes={3: 44100})
        external = CAP.reduce_transitions(
            active, capture_rate, series)["external"]
        self.assertEqual(external["old_rate_output_frames"], 0)

    def test_stale_old_rate_output_is_counted(self):
        """Physical fixture: after a 48 kHz -> 44.1 kHz switch the capture
        runs at 44.1 kHz, so frames the board still rendered at 48 kHz read
        at 997 * 44100/48000 = 916 Hz. The inverted model this convicts
        predicts 1085 Hz and counts nothing."""
        capture_rate = 44100
        boundary = 3 * capture_rate
        active = tone(6 * capture_rate, capture_rate)
        stale = tone(capture_rate // 10, 48000)   # frames RENDERED at 48 kHz
        active[boundary:boundary + len(stale)] = stale
        series = status_series(6, rate_changes={3: 44100})
        external = CAP.reduce_transitions(
            active, capture_rate, series)["external"]
        self.assertGreater(external["old_rate_output_frames"], 0)

    def test_inverted_model_frequency_is_not_counted(self):
        """Negative control: a segment at the INVERTED model's predicted
        1085 Hz is not old-rate output and must not be counted."""
        capture_rate = 44100
        boundary = 3 * capture_rate
        active = tone(6 * capture_rate, capture_rate)
        wrong_hz = CAP.TONE_HZ * 48000 / 44100
        active[boundary:boundary + capture_rate // 10] = tone(
            capture_rate // 10, capture_rate, hz=wrong_hz)
        series = status_series(6, rate_changes={3: 44100})
        external = CAP.reduce_transitions(
            active, capture_rate, series)["external"]
        self.assertEqual(external["old_rate_output_frames"], 0)


class CorrelationTests(unittest.TestCase):
    def test_bounded_window_rejects_events_outside_margin(self):
        # An increment at t=3 s; an event 5 s away is well outside the
        # 1.5 s margin and must not correlate.
        intervals = CAP.counter_increment_intervals(
            status_series(9, invalid_at=3), "invalid_windows")
        far_event = [8 * 48000]
        correlated, matched = CAP.correlate_counter_events(
            intervals, far_event, 48000, 9 * 48000)
        self.assertEqual(correlated, 0)
        self.assertEqual(matched, [])

    def test_greedy_one_to_one_assignment(self):
        series = status_series(9)
        series[4]["invalid_windows"] = 1
        series[5]["invalid_windows"] = 2           # two increments t=4, t=5
        intervals = CAP.counter_increment_intervals(series, "invalid_windows")
        self.assertEqual(len(intervals), 2)
        events = [4 * 48000 + 100, 5 * 48000 + 100]
        correlated, matched = CAP.correlate_counter_events(
            intervals, events, 48000, 9 * 48000)
        self.assertEqual(correlated, 2)
        self.assertEqual(sorted(matched), [0, 1])


class EvidenceIntegrationTests(unittest.TestCase):
    """write_evidence must produce a file the bench's own load_evidence and
    analyze path accept, with the embedded external observation driving a real
    verdict -- the offline proxy for step-6 live integration."""

    def _polls(self, rate, seconds):
        empty_events = json.dumps({"cursor": 0, "next": 0, "events": []})
        polls = []
        for index, elapsed in enumerate(seconds):
            status = BENCH_TEST.make_status(rate=rate, output_blocks=index + 1)
            polls.append({
                "elapsed_s": float(elapsed),
                "status": status,
                "raw_status_json": json.dumps(status),
                "raw_events_json": empty_events,
            })
        return polls

    def _analyze(self, scenario, path):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), \
                contextlib.redirect_stderr(io.StringIO()):
            code = BENCH_TEST.BENCH.main(
                ["analyze", scenario, "--evidence", str(path)])
        return code, json.loads(stdout.getvalue())

    def test_pcm_evidence_reaches_pass_verdict(self):
        polls = self._polls(44100, [0, 60, 120])
        external = json.dumps(
            {"audio_rate_hz": 44100, "continuous_output": True})
        options = CAP.scenario_analysis_options("pcm-120s", 44100, 44100, None)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "evidence.jsonl"
            CAP.write_evidence(path, "pcm-120s", polls, options, external)
            metadata, samples, events = BENCH_TEST.BENCH.load_evidence(path)
            self.assertEqual(len(samples), 3)
            self.assertEqual(metadata["scenario"], "pcm-120s")
            code, result = self._analyze("pcm-120s", path)
        self.assertEqual(code, 0, result.get("failures"))
        self.assertEqual(result["verdict"], "pass")

    def test_pcm_evidence_fails_on_wrong_audio_rate(self):
        polls = self._polls(44100, [0, 60, 120])
        external = json.dumps(
            {"audio_rate_hz": 32000, "continuous_output": True})
        options = CAP.scenario_analysis_options("pcm-120s", 44100, 44100, None)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "evidence.jsonl"
            CAP.write_evidence(path, "pcm-120s", polls, options, external)
            code, result = self._analyze("pcm-120s", path)
        self.assertEqual(code, 1)
        self.assertIn("audio-rate",
                      {failure["code"] for failure in result["failures"]})

    def test_null_audio_rate_from_an_unsnappable_estimate_fails(self):
        polls = self._polls(44100, [0, 60, 120])
        external = json.dumps(
            {"audio_rate_hz": None, "continuous_output": True})
        options = CAP.scenario_analysis_options("pcm-120s", 44100, 44100, None)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "evidence.jsonl"
            CAP.write_evidence(path, "pcm-120s", polls, options, external)
            code, result = self._analyze("pcm-120s", path)
        self.assertEqual(code, 1)
        self.assertIn("audio-rate",
                      {failure["code"] for failure in result["failures"]})


class RecorderClassifierTests(unittest.TestCase):
    """The recorder reducer reads the output rate as SAMPLES PER CYCLE of the
    recorded frames, which is a property of the frames themselves."""

    def test_samples_per_cycle_names_each_nominal_rate(self):
        for rate in (32000, 44100, 48000):
            spc = REC.samples_per_cycle(output_segment(rate, 512)[:, 0])
            self.assertEqual(REC.rate_from_samples_per_cycle(spc), rate)

    def test_third_rate_block_is_counted_not_dropped(self):
        """A transient 32 kHz block inside a 48 kHz phase is exactly the
        artifact gate 4 looks for; discarding it as ambiguous erases it."""
        stream = output_stream([
            (48000, 20000), (32000, 8000), (48000, 20000)])
        runs = REC.output_rate_runs(REC.classify_output_rates(stream[:, 0]))
        self.assertEqual([rate for rate, _frames in runs],
                         [48000, 32000, 48000])

    def test_clean_three_phase_round_trip(self):
        """The plan's stimulus is 48 -> 44.1 -> 48: three phases, two rate
        boundaries."""
        stream = output_stream([
            (48000, 40000), (44100, 40000), (48000, 40000)])
        runs = REC.output_rate_runs(REC.classify_output_rates(stream[:, 0]))
        self.assertEqual([rate for rate, _frames in runs],
                         [48000, 44100, 48000])


class RecorderFiducialTests(unittest.TestCase):
    """old_rate_output_frames is a DISTANCE from the board's declared rate
    change to the frame where the output actually changed. Run-length
    analysis cannot measure it: stale old-rate frames are contiguous with
    the pre-switch run at the same samples-per-cycle and merge into it."""

    def test_clean_switch_at_the_fiducial_is_zero(self):
        stream = output_stream([(48000, 20000), (44100, 20000)])
        result = REC.reduce_recorder_transitions([stream], [[20000]])
        self.assertEqual(result["external"]["old_rate_output_frames"], 0)
        self.assertFalse(result["evidence"]["lower_bound"])

    def test_stale_frames_after_the_fiducial_are_measured(self):
        # 4096 further frames of 48 kHz output after the board declared the
        # switch; contiguous with the pre-switch 48 kHz run.
        stream = output_stream([(48000, 24096), (44100, 20000)])
        result = REC.reduce_recorder_transitions([stream], [[20000]])
        stale = result["external"]["old_rate_output_frames"]
        self.assertGreater(stale, 3500)
        self.assertLess(stale, 4700)

    def test_run_length_alone_cannot_see_those_stale_frames(self):
        """The negative control for the redesign: the same stream through
        run-length analysis reads 0 -- which is why the fiducial exists."""
        stream = output_stream([(48000, 24096), (44100, 20000)])
        runs = REC.output_rate_runs(REC.classify_output_rates(stream[:, 0]))
        self.assertEqual(REC.stale_old_rate_frames(runs), 0)

    def test_switch_shortly_before_the_stamp_measures_zero(self):
        """The live 2026-09-02 capture: the board stamps the fiducial at
        block granularity, ~400 frames AFTER the output already switched,
        so the blocks adjacent to the stamp straddle the discontinuity and
        classify as nothing. The old rate must come from a window before
        the stamp, and a switch preceding its stamp is 0 stale frames, not
        an unresolved fiducial."""
        stream = output_stream([(48000, 19600), (44100, 20000)])
        result = REC.reduce_recorder_transitions([stream], [[20000]])
        self.assertEqual(result["external"]["old_rate_output_frames"], 0)
        self.assertFalse(result["evidence"]["lower_bound"])
        slot = result["evidence"]["slots"][0]
        self.assertEqual(slot["unresolved_fiducials"], [])
        detected = slot["rate_change_frames_detected"][0]
        self.assertIsNotNone(detected)
        self.assertLess(detected, 20000)
        self.assertGreaterEqual(detected, 19600 - REC.CLASSIFY_BLOCK)

    def test_unresolved_fiducial_marks_the_count_a_lower_bound(self):
        """A fiducial after which the output never changes contributes 0,
        which is the one value it must not silently read as."""
        stream = output_stream([(48000, 60000)])
        result = REC.reduce_recorder_transitions([stream], [[20000]])
        self.assertEqual(result["external"]["old_rate_output_frames"], 0)
        self.assertTrue(result["evidence"]["lower_bound"])
        slot = result["evidence"]["slots"][0]
        self.assertEqual(slot["unresolved_fiducials"], [20000])
        self.assertEqual(slot["rate_change_frames_detected"], [None])
        self.assertIn("lower bound", slot["lower_bound_note"])

    def test_missing_fiducial_marks_the_count_a_lower_bound(self):
        stream = output_stream([(48000, 24096), (44100, 20000)])
        result = REC.reduce_recorder_transitions([stream], [[]])
        self.assertTrue(result["evidence"]["lower_bound"])
        self.assertEqual(result["evidence"]["method"],
                         "run-length (no fiducial)")
        self.assertIn("lower bound",
                      result["evidence"]["slots"][0]["lower_bound_note"])

    def test_two_fiducials_sum(self):
        stream = output_stream([
            (48000, 24096), (44100, 20000), (48000, 20000)])
        result = REC.reduce_recorder_transitions(
            [stream], [[20000, 44096]])
        self.assertGreater(result["external"]["old_rate_output_frames"], 3500)

    def test_stream_indexes_convert_to_pulled_frame_indexes(self):
        """`rate_change_frames` are STREAM indexes (frames staged since the
        slot armed); the pulled array is 0..valid_frames-1 from the oldest
        RETAINED frame. The origin is stream_end_frame - valid_frames."""
        meta = {"rate_change_frames": [130000, 150000, 0, 0],
                "rate_change_count": 2, "ring_frames": 100000,
                "oldest_frame": 500, "valid_frames": 100000,
                "stream_end_frame": 220000}
        record = REC.fiducials_from_slot(meta)
        # oldest retained frame is stream index 120000.
        self.assertEqual(record["fiducial_frames"], [10000, 30000])
        self.assertEqual(record["fiducials_rolled_off"], [])
        self.assertTrue(record["fiducials_usable"])
        self.assertEqual(record["oldest_retained_stream_frame"], 120000)

    def test_rolled_off_fiducial_is_dropped_and_recorded(self):
        meta = {"rate_change_frames": [50000, 130000], "rate_change_count": 2,
                "ring_frames": 100000, "oldest_frame": 0,
                "valid_frames": 100000, "stream_end_frame": 220000}
        record = REC.fiducials_from_slot(meta)
        self.assertEqual(record["fiducial_frames"], [10000])
        self.assertEqual(record["fiducials_rolled_off"], [50000])

    def test_missing_stream_end_frame_makes_fiducials_unusable(self):
        """Without the origin there is no safe conversion, so the reducer
        must take the run-length lower bound rather than guess."""
        meta = {"rate_change_frames": [130000], "rate_change_count": 1,
                "ring_frames": 100000, "oldest_frame": 0,
                "valid_frames": 100000}
        record = REC.fiducials_from_slot(meta)
        self.assertFalse(record["fiducials_usable"])
        self.assertEqual(record["fiducial_frames"], [])
        self.assertIn("stream_end_frame", record["fiducial_note"])

        stream = output_stream([(48000, 24096), (44100, 20000)])
        result = REC.reduce_recorder_transitions([stream], [record])
        self.assertTrue(result["evidence"]["lower_bound"])
        self.assertEqual(result["evidence"]["method"],
                         "run-length (no fiducial)")

    def test_absent_fiducial_field_yields_no_fiducials(self):
        record = REC.fiducials_from_slot(
            {"ring_frames": 1000, "oldest_frame": 0, "valid_frames": 1000,
             "stream_end_frame": 1000})
        self.assertEqual(record["fiducial_frames"], [])
        self.assertFalse(record["fiducials_usable"])

    def test_converted_fiducial_record_drives_the_measurement(self):
        """End to end through the real header conversion: a slot whose ring
        retained 20000 frames, with the board's declared rate change 4096
        frames before the output actually changed."""
        stream = output_stream([(48000, 14096), (44100, 20000)])
        meta = {"rate_change_frames": [110000], "rate_change_count": 1,
                "ring_frames": 40000, "oldest_frame": 0,
                "valid_frames": len(stream),
                "stream_end_frame": 100000 + len(stream)}
        record = REC.fiducials_from_slot(meta)
        self.assertEqual(record["fiducial_frames"], [10000])
        result = REC.reduce_recorder_transitions([stream], [record])
        stale = result["external"]["old_rate_output_frames"]
        self.assertGreater(stale, 3500)
        self.assertLess(stale, 4700)
        self.assertFalse(result["evidence"]["lower_bound"])


class RecorderCaptureFailureTests(unittest.TestCase):
    """Zero captured slots is a FAILURE, not a clean 0."""

    def test_no_slots_emits_a_null_and_a_marker(self):
        result = REC.reduce_recorder_transitions([])
        self.assertIsNone(result["external"]["old_rate_output_frames"])
        self.assertEqual(result["external"]["captured_slots"], 0)
        self.assertEqual(result["external"]["failure"],
                         "no-transition-captured")

    def test_bench_rejects_the_null_as_a_visible_fail(self):
        samples, events = BENCH_TEST.transition_fixture()
        external = REC.reduce_recorder_transitions([])["external"]
        result = BENCH_TEST.BENCH.analyze_samples(
            samples, events, "transitions",
            {"settle_s": 0.0, "phase_samples": 2,
             "max_follow_error_millihz": 1000},
            external)
        self.assertEqual(result["verdict"], "fail")
        self.assertIn("external-observation",
                      {failure["code"] for failure in result["failures"]})

    def test_bench_accepts_a_measured_zero(self):
        samples, events = BENCH_TEST.transition_fixture()
        stream = output_stream([(48000, 20000), (44100, 20000)])
        external = REC.reduce_recorder_transitions(
            [stream], [[20000]])["external"]
        result = BENCH_TEST.BENCH.analyze_samples(
            samples, events, "transitions",
            {"settle_s": 0.0, "phase_samples": 2,
             "max_follow_error_millihz": 1000},
            external)
        self.assertNotIn("external-observation",
                         {failure["code"] for failure in result["failures"]})
        self.assertNotIn("old-rate-output",
                         {failure["code"] for failure in result["failures"]})


class RecorderSlotSelectionTests(unittest.TestCase):
    """Which slot the run captured, and whether the ring could hold it."""

    def _header(self, **overrides):
        header = {
            "fitted_mb": 8, "frame_bytes": 8, "slot_count": 3,
            "slots": [
                {"slot": 0, "state": "frozen", "freeze_generation": 4,
                 "ring_frames": 300000},
                {"slot": 1, "state": "armed", "freeze_generation": 0,
                 "ring_frames": 300000},
                {"slot": 2, "state": "frozen", "freeze_generation": 9,
                 "ring_frames": 300000},
            ],
        }
        header.update(overrides)
        return header

    def test_newest_frozen_slot_pointer_wins(self):
        header = self._header(newest_frozen_slot=0)
        self.assertEqual(REC.newest_frozen_slot(header)["slot"], 0)

    def test_falls_back_to_the_highest_freeze_generation(self):
        self.assertEqual(REC.newest_frozen_slot(self._header())["slot"], 2)

    def test_no_frozen_slot_is_none(self):
        header = self._header(slots=[
            {"slot": 0, "state": "armed", "freeze_generation": 0}])
        self.assertIsNone(REC.newest_frozen_slot(header))

    def test_slot_capacity_prefers_ring_frames(self):
        header = self._header()
        self.assertEqual(
            REC.slot_capacity_frames(header, header["slots"][0]), 300000)

    def test_slot_capacity_falls_back_to_bank_arithmetic(self):
        header = self._header()
        self.assertEqual(
            REC.slot_capacity_frames(header, {"ring_frames": 0}),
            8 * 1024 * 1024 // 3 // 8)

    def test_required_frames_cover_the_later_phases_plus_margin(self):
        """Only the frames from just before the FIRST rate boundary to the
        freeze must survive: every later phase plus the margin, not the
        whole stimulus. Demanding all three phases refused every 2 s run
        against the board's 262144-frame slot for no reason."""
        rates = (48000, 44100, 48000)
        required = REC.required_capture_frames((2.0, 2.0, 2.0), rates,
                                               latency_s=1.0)
        self.assertEqual(required, 2 * 44100 + 2 * 48000 + 48000)
        self.assertLessEqual(required, 262144)
        self.assertGreater(required, REC.required_capture_frames(
            (1.0, 1.0, 1.0), rates, latency_s=1.0))

    def test_frozen_identities_are_per_slot_pairs(self):
        header = self._header()
        self.assertEqual(REC.frozen_identities(header), {(0, 4), (2, 9)})

    def test_captured_slot_accepts_a_fresh_slot_at_generation_zero(self):
        """The live 2026-09-02 failure: on a fresh boot every slot's
        freeze_generation is 0 (it is per slot and counts thaws), so a
        "newer generation than baseline" check rejected the run's own
        freeze. A slot that was not frozen at baseline is new whatever its
        generation reads."""
        before = self._header(newest_frozen_slot=0, slots=[
            {"slot": 0, "state": "frozen", "freeze_generation": 0},
            {"slot": 1, "state": "frozen", "freeze_generation": 0},
            {"slot": 2, "state": "recording", "freeze_generation": 0},
        ])
        after = self._header(newest_frozen_slot=2, slots=[
            {"slot": 0, "state": "frozen", "freeze_generation": 0},
            {"slot": 1, "state": "frozen", "freeze_generation": 0},
            {"slot": 2, "state": "frozen", "freeze_generation": 0},
        ])
        meta = REC.captured_slot(after, REC.frozen_identities(before))
        self.assertIsNotNone(meta)
        self.assertEqual(meta["slot"], 2)

    def test_captured_slot_rejects_a_capture_that_existed_at_baseline(self):
        header = self._header(newest_frozen_slot=2)
        self.assertIsNone(
            REC.captured_slot(header, REC.frozen_identities(header)))

    def test_captured_slot_accepts_a_refrozen_slot_by_generation(self):
        before = self._header(newest_frozen_slot=2)
        after = self._header(newest_frozen_slot=2, slots=[
            {"slot": 0, "state": "frozen", "freeze_generation": 4},
            {"slot": 1, "state": "armed", "freeze_generation": 0},
            {"slot": 2, "state": "frozen", "freeze_generation": 10},
        ])
        meta = REC.captured_slot(after, REC.frozen_identities(before))
        self.assertEqual((meta["slot"], meta["freeze_generation"]), (2, 10))

    def test_phase_frames_keep_fractional_seconds(self):
        """aplay -d truncates 1.2 s to 1 s; the phase is expressed in
        frames (--samples) so a fractional phase plays in full."""
        self.assertEqual(REC.phase_frames(1.2, 48000), 57600)
        self.assertEqual(REC.phase_frames(1.2, 44100), 52920)

    def test_tone_rate_is_read_from_the_wav_header(self):
        import wave
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "tone.wav"
            with wave.open(str(path), "wb") as handle:
                handle.setnchannels(2)
                handle.setsampwidth(2)
                handle.setframerate(44100)
                handle.writeframes(b"\x00" * 16)
            self.assertEqual(REC.tone_rate_hz(path), 44100)

    def _polls(self, rates):
        return [{"elapsed_s": float(index) * 0.25,
                 "status": {"clock": {"stream_rate_hz": rate}}}
                for index, rate in enumerate(rates)]

    def test_stream_rate_timeline_collapses_to_changes(self):
        polls = self._polls([48000, 48000, 44100, 44100, 48000])
        self.assertEqual(REC.stream_rate_timeline(polls),
                         [[0.0, 48000], [0.5, 44100], [1.0, 48000]])

    def test_stimulus_followed_needs_the_target_rate_in_the_polls(self):
        """The 2026-09-02 1 s-phase run: the board sat in `unsupported` at
        48000 for the whole 44.1 kHz phase, so nothing was stamped. That
        is a stimulus problem and must be named as one."""
        self.assertFalse(REC.stimulus_followed(
            self._polls([48000, 48000, 48000, 0, 48000]), 44100))
        self.assertTrue(REC.stimulus_followed(
            self._polls([48000, 44100, 48000]), 44100))


BOARD_SLOT_FRAMES = 262144
BOARD_RATES = (48000, 44100, 48000)


def run1_polls():
    """The 2026-09-03 replay-floor run 1 status timeline, condensed: the
    carrier switched to 44.1 kHz at ~2.1 s, the board sat `unsupported` at
    stream 48000 until 3.93 s, was tracking 44.1 kHz for ONE poll, the
    carrier switched back at ~4.1 s, the board sat `unsupported` at stream
    44100 until 5.23 s, then tracked 48 kHz until the U24 was deauthorized
    at ~7.0 s. Run 2 also carried a `stream_rate_hz 0 / tracking` poll at
    the 44.1 kHz relock (3.14 s), reproduced here at 3.67 s."""
    timeline = [
        (0.011, 48000, "tracking"), (1.842, 48000, "tracking"),
        (2.104, 48000, "unsupported"), (3.669, 0, "tracking"),
        (3.927, 44100, "tracking"),
        (4.189, 44100, "unsupported"), (4.972, 44100, "unsupported"),
        (5.232, 48000, "tracking"), (6.801, 48000, "tracking"),
        (7.062, 48000, "unlocked"), (9.677, 48000, "replay"),
    ]
    return [{"elapsed_s": elapsed,
             "status": {"clock": {"stream_rate_hz": rate, "state": state}}}
            for elapsed, rate, state in timeline]


class RecorderStimulusShapeTests(unittest.TestCase):
    """The per-phase stimulus, the corrected ring budget, and the freeze
    gated on the board's acceptance of the reference rate.

    Live 2026-09-03: the board accepts 44.1 kHz 1.05-1.85 s after the
    carrier switch and stages NOTHING into the recorder while it sits
    `unsupported` (the slot's 44.1 kHz run was 8192 frames on the run whose
    polls showed 0.26 s of tracking, ~37700 on the runs that showed
    0.78 s; frames after the second fiducial were 51200 on every run). So
    a phase costs ring frames at its own rate, only while tracked, and the
    lead-in costs nothing -- which is what lets the middle phase grow
    past the acceptance latency without --allow-rolloff."""

    def test_later_phases_are_counted_at_their_own_rates(self):
        required = REC.required_capture_frames((2.0, 3.0, 1.5), BOARD_RATES,
                                               latency_s=1.0)
        self.assertEqual(required, int(3.0 * 44100 + 1.5 * 48000 + 48000))

    def test_lead_in_length_does_not_enter_the_budget(self):
        short = REC.required_capture_frames((0.5, 3.0, 1.5), BOARD_RATES)
        long = REC.required_capture_frames((10.0, 3.0, 1.5), BOARD_RATES)
        self.assertEqual(short, long)

    def test_symmetric_two_second_phases_fit_the_board_slot(self):
        self.assertLessEqual(
            REC.required_capture_frames((2.0, 2.0, 2.0), BOARD_RATES),
            BOARD_SLOT_FRAMES)

    def test_symmetric_three_second_phases_do_not_fit(self):
        self.assertGreater(
            REC.required_capture_frames((3.0, 3.0, 3.0), BOARD_RATES),
            BOARD_SLOT_FRAMES)

    def test_three_second_middle_with_two_second_trailing_does_not_fit(self):
        """Why the trailing default is 1.5 s and not 2 s: with a 3 s middle
        a 2 s trailing phase overruns the slot for an instant-acceptance
        board (276300 > 262144)."""
        self.assertGreater(
            REC.required_capture_frames((2.0, 3.0, 2.0), BOARD_RATES),
            BOARD_SLOT_FRAMES)

    def test_default_stimulus_fits_the_board_slot(self):
        required = REC.required_capture_frames(REC.DEFAULT_PHASE_SECONDS,
                                               BOARD_RATES)
        self.assertEqual(required, 252300)
        self.assertLessEqual(required, BOARD_SLOT_FRAMES)

    def test_default_middle_outlasts_the_acceptance_budget_by_three_polls(
            self):
        """The analyzer labels a phase from `tracking` polls only and wants
        `phase_samples 2`; two polls are guaranteed inside a tracking
        window only once it exceeds two full poll intervals, so the middle
        phase must exceed the acceptance budget by three intervals. The
        2 s default lost this by 0.15 s on run 1 (1.85 s latency)."""
        middle = REC.DEFAULT_PHASE_SECONDS[1]
        floor = (REC.ACCEPTANCE_LATENCY_BUDGET_S
                 + 3 * REC.STATUS_POLL_INTERVAL_S)
        self.assertGreaterEqual(middle, floor)
        self.assertLess(2.0, floor)

    def test_default_trailing_plus_freeze_wait_covers_the_budget(self):
        """The trailing phase may be shorter than the acceptance latency
        because the freeze waits (bounded) for the board to be seen
        tracking 48 kHz again; the phase plus the wait must still cover an
        acceptance as slow as the worst 44.1 kHz one."""
        trailing = REC.DEFAULT_PHASE_SECONDS[2]
        self.assertGreaterEqual(trailing + REC.FREEZE_ACCEPTANCE_TIMEOUT_S,
                                REC.ACCEPTANCE_LATENCY_BUDGET_S)

    def test_replay_leg_is_outside_the_ring_budget(self):
        """--with-replay adds a fourth bench phase AFTER the freeze; the
        ring budget is over the three aplay phases in either case."""
        self.assertEqual(len(REC.driven_phase_labels(True)), 4)
        self.assertEqual(len(REC.DEFAULT_PHASE_SECONDS), 3)
        self.assertEqual(
            REC.required_capture_frames(REC.DEFAULT_PHASE_SECONDS,
                                        BOARD_RATES),
            252300)

    def test_one_rate_per_phase_is_required(self):
        with self.assertRaises(ValueError):
            REC.required_capture_frames((2.0, 3.0, 1.5), (48000, 44100))

    def test_phase_seconds_single_value_applies_to_every_phase(self):
        self.assertEqual(REC.parse_phase_seconds("2"), (2.0, 2.0, 2.0))

    def test_phase_seconds_accepts_first_middle_last(self):
        self.assertEqual(REC.parse_phase_seconds("2, 3,1.5"),
                         (2.0, 3.0, 1.5))

    def test_phase_seconds_rejects_wrong_count_and_bad_values(self):
        import argparse
        for text in ("2,3", "2,3,1,1", "0,3,1.5", "-1", "two", ""):
            with self.assertRaises(argparse.ArgumentTypeError, msg=text):
                REC.parse_phase_seconds(text)

    def test_tracking_seen_since_finds_the_acceptance_after_the_switch(self):
        """Run 1: the switch back was at ~4.1 s; the first tracking 48 kHz
        poll after it is 5.232 s. The lead-in's 48 kHz polls before the
        switch and the `unsupported` polls at stream 44100 do not count."""
        self.assertEqual(
            REC.tracking_seen_since(run1_polls(), 48000, 4.1), 5.232)

    def test_tracking_seen_since_ignores_stale_polls_before_the_switch(self):
        self.assertIsNone(REC.tracking_seen_since(run1_polls(), 48000, 6.9))
        self.assertEqual(
            REC.tracking_seen_since(run1_polls(), 44100, 2.1), 3.927)

    def test_tracking_seen_since_ignores_a_zero_rate_tracking_poll(self):
        """Run 2 carried a `stream_rate_hz 0 / state tracking` poll at the
        44.1 kHz relock; it is not acceptance of anything."""
        polls = [{"elapsed_s": 3.1,
                  "status": {"clock": {"stream_rate_hz": 0,
                                       "state": "tracking"}}}]
        self.assertIsNone(REC.tracking_seen_since(polls, 48000, 0.0))
        self.assertIsNone(REC.tracking_seen_since(polls, 44100, 0.0))


class RecorderUnequalPhaseAnalysisTests(unittest.TestCase):
    """The bench analyzer judges phases by observed labels, not by equal
    durations: a long middle phase and a short trailing phase must still
    reach the same verdict, and a middle phase that yields one tracking
    poll must still read as the run-1 stimulus failure."""

    @staticmethod
    def _fixture(counts):
        """transition_fixture's shape with `counts` tracking polls per
        phase (48 -> 44.1 -> 48, no replay leg): one dark `unsupported`
        poll, one rate-change event and one requested reset per rate
        boundary, exactly as the bench fixture models them."""
        make_sample = BENCH_TEST.sample
        samples = []
        events = []
        output_blocks = 0
        changes = 0
        resets = 0
        unsupported = 0
        elapsed = 0.0
        for phase_index, rate in enumerate(BOARD_RATES):
            if phase_index:
                unsupported += 1
                events.append({"sequence": len(events) + 1,
                               "uptime_ms": int(elapsed * 1000),
                               "type": 12, "state": 3, "detail": 40000})
                dark = make_sample(elapsed, rate=BOARD_RATES[phase_index - 1],
                                   output_blocks=output_blocks)
                dark["status"]["tx_gate"]["open"] = 0
                dark["status"]["clock"].update({
                    "carrier_ready": False, "state": "unsupported",
                    "unsupported_episodes": unsupported,
                    "rate_changes": changes})
                dark["status"]["audio_diag"][
                    "service_requested_resets"] = resets
                samples.append(dark)
                elapsed += 0.25
                resets += 1
                changes += 1
                events.append({"sequence": len(events) + 1,
                               "uptime_ms": int(elapsed * 1000),
                               "type": 11, "state": 3, "detail": rate})
            for _ in range(counts[phase_index]):
                output_blocks += 1
                entry = make_sample(elapsed, rate=rate,
                                    output_blocks=output_blocks)
                entry["status"]["clock"].update({
                    "rate_changes": changes,
                    "unsupported_episodes": unsupported})
                entry["status"]["audio_diag"][
                    "service_requested_resets"] = resets
                samples.append(entry)
                elapsed += 0.25
        return samples, events

    def _analyze(self, counts):
        samples, events = self._fixture(counts)
        polls = RecorderDrivenPhaseMetadataTests._polls(samples, events)
        external = json.dumps({"old_rate_output_frames": 0})
        options = {"settle_s": 0.0, "phase_samples": 2,
                   "max_follow_error_millihz": 1000,
                   "expected_phases": REC.driven_phase_labels(False)}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "evidence.jsonl"
            CAP.write_evidence(path, "transitions", polls, options, external)
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout), \
                    contextlib.redirect_stderr(io.StringIO()):
                code = BENCH_TEST.BENCH.main(
                    ["analyze", "transitions", "--evidence", str(path)])
        return code, json.loads(stdout.getvalue())

    def test_unequal_phases_pass_with_the_same_thresholds(self):
        """2 s lead-in / 3 s middle / 1.5 s trailing at the 0.25 s poll
        cadence: 8 / 12 / 6 tracking polls. No threshold moved."""
        code, result = self._analyze((8, 12, 6))
        self.assertEqual(code, 0, result.get("failures"))
        self.assertEqual(result["verdict"], "pass")
        self.assertEqual(result["metrics"]["observed_phase_sequence"],
                         ["48000", "44100", "48000"])
        self.assertEqual(result["metrics"]["transition_reset_deltas"],
                         [1, 1])

    def test_short_trailing_phase_still_needs_two_polls(self):
        code, result = self._analyze((8, 12, 2))
        self.assertEqual(code, 0, result.get("failures"))
        self.assertEqual(result["verdict"], "pass")

    def test_one_tracking_poll_in_the_middle_is_the_run1_failure(self):
        """The 2026-09-03 run 1 diagnosis, reproduced: a middle phase the
        board tracked for one poll collapses the sequence to a single
        48000 label and the rate blips have no live window to land in."""
        code, result = self._analyze((8, 1, 6))
        self.assertEqual(code, 1)
        codes = {failure["code"] for failure in result["failures"]}
        self.assertIn("transition-sequence", codes)
        self.assertIn("transition-unattributed-unsupported", codes)
        self.assertEqual(result["metrics"]["observed_phase_sequence"],
                         ["48000"])


class RecorderAggregationTests(unittest.TestCase):
    def test_stale_metric_ignores_first_and_last_runs(self):
        # Endpoints are legitimate phases regardless of length.
        runs = [(48000, 512), (44100, 40000), (48000, 512)]
        self.assertEqual(REC.stale_old_rate_frames(runs), 0)

    def test_old_rate_blip_inside_a_phase_is_counted_by_the_fallback(self):
        stream = output_stream([
            (48000, 40000), (44100, 20000), (48000, 3072),
            (44100, 20000), (48000, 40000)])
        runs = REC.output_rate_runs(REC.classify_output_rates(stream[:, 0]))
        self.assertGreater(REC.stale_old_rate_frames(runs), 0)

    def test_reduce_sums_across_slots(self):
        clean = output_stream([(48000, 20000), (44100, 20000)])
        result = REC.reduce_recorder_transitions(
            [clean, clean], [[20000], [20000]])
        self.assertEqual(result["external"]["old_rate_output_frames"], 0)
        self.assertEqual(result["external"]["captured_slots"], 2)
        self.assertEqual(len(result["evidence"]["slots"]), 2)


class RecorderDrivenPhaseMetadataTests(unittest.TestCase):
    """The recorder tool must record the phase sequence it actually drove
    into the evidence metadata, so `analyze` judges the stimulus that ran
    rather than the historic four-phase one."""

    @staticmethod
    def _polls(samples, events):
        empty_events = json.dumps({"cursor": 0, "next": 0, "events": []})
        all_events = json.dumps(
            {"cursor": 0, "next": len(events), "events": events})
        polls = []
        for index, entry in enumerate(samples):
            polls.append({
                "elapsed_s": float(entry["elapsed_s"]),
                "status": entry["status"],
                "raw_status_json": json.dumps(entry["status"]),
                "raw_events_json": all_events if index == 0 else empty_events,
            })
        return polls

    def _analyze(self, path):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), \
                contextlib.redirect_stderr(io.StringIO()):
            code = BENCH_TEST.BENCH.main(
                ["analyze", "transitions", "--evidence", str(path)])
        return code, json.loads(stdout.getvalue())

    def test_replay_label_only_when_the_leg_is_driven(self):
        self.assertEqual(REC.driven_phase_labels(False),
                         ["48000", "44100", "48000"])
        self.assertEqual(REC.driven_phase_labels(True),
                         ["48000", "44100", "48000", "replay"])

    def test_recorded_sequence_drives_the_bench_verdict(self):
        samples, events = BENCH_TEST.transition_fixture(with_replay=False)
        polls = self._polls(samples, events)
        external = json.dumps({"old_rate_output_frames": 0})
        options = {"settle_s": 0.0, "phase_samples": 2,
                   "max_follow_error_millihz": 1000,
                   "expected_phases": REC.driven_phase_labels(False)}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "evidence.jsonl"
            CAP.write_evidence(path, "transitions", polls, options, external)
            code, result = self._analyze(path)
        self.assertEqual(code, 0, result.get("failures"))
        self.assertEqual(result["metrics"]["expected_phase_sequence"],
                         ["48000", "44100", "48000"])

    def test_claiming_a_replay_leg_that_never_ran_still_fails(self):
        samples, events = BENCH_TEST.transition_fixture(with_replay=False)
        polls = self._polls(samples, events)
        external = json.dumps({"old_rate_output_frames": 0})
        options = {"settle_s": 0.0, "phase_samples": 2,
                   "max_follow_error_millihz": 1000,
                   "expected_phases": REC.driven_phase_labels(True)}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "evidence.jsonl"
            CAP.write_evidence(path, "transitions", polls, options, external)
            code, result = self._analyze(path)
        self.assertEqual(code, 1)
        self.assertIn("transition-sequence",
                      {failure["code"] for failure in result["failures"]})


class ReplayStartFillBusyTests(unittest.TestCase):
    """recorder_transitions.replay_start retries only 503 fill_busy."""

    FILL_BUSY = '{"ok":false,"error":"fill_busy"}'

    def _run(self, replies, budget_s=120.0):
        calls, sleeps = [], []
        now = [0.0]
        queue = list(replies)

        def post(base, auth, path, body):
            calls.append(path)
            return queue.pop(0)

        def sleep(seconds):
            sleeps.append(seconds)
            now[0] += seconds

        try:
            result = REC.replay_start("b", "a", "{}", post=post, sleep=sleep,
                                      clock=lambda: now[0],
                                      budget_s=budget_s)
        except RuntimeError as error:
            result = error
        return result, calls, sleeps

    def test_retries_then_succeeds(self):
        result, calls, sleeps = self._run([
            (503, self.FILL_BUSY, "3"), (503, self.FILL_BUSY, None),
            (200, "{}", None)])
        self.assertEqual(result, 200)
        self.assertEqual(sleeps, [3.0, 1.0])
        self.assertEqual(calls, ["/api/replays/start"] * 3)

    def test_budget_exhausted_raises(self):
        result, calls, sleeps = self._run(
            [(503, self.FILL_BUSY, "30")] * 10, budget_s=120.0)
        self.assertIsInstance(result, RuntimeError)
        self.assertIn("fill_busy", str(result))
        self.assertEqual(sum(sleeps), 120.0)

    def test_other_503_not_retried(self):
        result, calls, sleeps = self._run(
            [(503, '{"ok":false,"error":"sd_busy"}', "2")])
        self.assertEqual(result, 503)
        self.assertEqual(len(calls), 1)
        self.assertEqual(sleeps, [])

    def test_409_passes_through(self):
        result, calls, _ = self._run(
            [(409, '{"error":"replay_carrier_present"}', None)])
        self.assertEqual(result, 409)
        self.assertEqual(len(calls), 1)


if __name__ == "__main__":
    unittest.main()
