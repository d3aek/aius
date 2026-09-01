"""Unit tests for the rate-gate capture reducers.

Drives the public reducer interface of firmware/tools/rate_gate_capture/
capture.py against synthetic waveforms and status series: gap-injected,
512-run-injected, and frequency-step WAVs stand in for the live U24
capture so the field reductions are provable off-board.
"""

import importlib.util
import math
import pathlib
import unittest

import numpy as np


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE = ROOT / "tools" / "rate_gate_capture" / "capture.py"
SPEC = importlib.util.spec_from_file_location("rate_gate_capture", MODULE)
CAP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAP)


def tone(frames, capture_rate, hz=CAP.TONE_HZ, amp=8000):
    n = np.arange(frames, dtype=np.float64)
    mono = np.rint(amp * np.sin(2 * math.pi * hz * n / capture_rate))
    return np.repeat(mono.astype(np.int32)[:, None], 2, axis=1)


def stepped_tone(capture_rate, first_hz, second_hz, boundary_frame, frames):
    """Phase-continuous stereo tone that switches frequency at
    boundary_frame."""
    n = np.arange(frames, dtype=np.float64)
    phase = np.empty(frames, dtype=np.float64)
    head = min(boundary_frame, frames)
    phase[:head] = 2 * math.pi * first_hz * n[:head] / capture_rate
    if head < frames:
        base = phase[head - 1] if head else 0.0
        tail = np.arange(1, frames - head + 1, dtype=np.float64)
        phase[head:] = base + 2 * math.pi * second_hz * tail / capture_rate
    mono = np.rint(8000 * np.sin(phase))
    return np.repeat(mono.astype(np.int32)[:, None], 2, axis=1)


def status_series(seconds, invalid_at=None, trim_at=None, rate=48000,
                  rate_changes=None):
    """One entry per second. invalid_at / trim_at are the elapsed_s from
    which the counter reads 1 (a single increment); rate_changes maps
    elapsed_s -> new stream_rate_hz."""
    rate_changes = rate_changes or {}
    series = []
    current_rate = rate
    for second in range(seconds):
        if second in rate_changes:
            current_rate = rate_changes[second]
        series.append({
            "elapsed_s": float(second),
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
    def test_continuous_output_true_and_rate_inferred(self):
        active = tone(48000, 44100)
        result = CAP.reduce_pcm(active, 44100, 44100)
        self.assertTrue(result["external"]["continuous_output"])
        self.assertEqual(
            result["evidence"]["audio_rate_hz_source"], "inferred-from-tone")
        self.assertLess(
            abs(result["external"]["audio_rate_hz"] - 44100), 50)

    def test_dropout_breaks_continuity(self):
        active = tone(48000, 44100)
        active[10000:10000 + 512] = 0
        result = CAP.reduce_pcm(active, 44100, 44100)
        self.assertFalse(result["external"]["continuous_output"])


class WaveformReducerTests(unittest.TestCase):
    def _base(self, seconds=9):
        return tone(seconds * 48000, 48000)

    def test_clean_correlated_run_and_trim(self):
        active = self._base()
        active[3 * 48000:3 * 48000 + 512] = 0          # invalid window at 3 s
        active[6 * 48000] = 20000                        # trim spike at 6 s
        active[6 * 48000 + 1] = -20000
        series = status_series(9, invalid_at=3, trim_at=6)
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


class TransitionReducerTests(unittest.TestCase):
    def test_clean_transition_has_no_old_rate_frames(self):
        active = tone(6 * 48000, 48000)            # steady 997 Hz throughout
        series = status_series(6, rate_changes={3: 44100})
        external = CAP.reduce_transitions(active, 48000, series)["external"]
        self.assertEqual(external["old_rate_output_frames"], 0)

    def test_stale_old_rate_output_is_counted(self):
        boundary = 3 * 48000
        old_hz = CAP.TONE_HZ * 48000 / 44100        # effective old-rate tone
        # Tone holds the old effective frequency for 100 ms past the boundary,
        # then snaps to 997 Hz.
        active = stepped_tone(
            48000, CAP.TONE_HZ, old_hz, boundary, 6 * 48000)
        stale_end = boundary + 48000 // 10
        active[stale_end:] = tone(
            6 * 48000 - stale_end, 48000)[: 6 * 48000 - stale_end]
        series = status_series(6, rate_changes={3: 44100})
        external = CAP.reduce_transitions(active, 48000, series)["external"]
        self.assertGreater(external["old_rate_output_frames"], 0)


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


if __name__ == "__main__":
    unittest.main()
