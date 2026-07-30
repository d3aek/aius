import importlib.util
import pathlib
import sys
import tempfile
import unittest

import numpy as np


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools" / "latency"
sys.path.insert(0, str(TOOLS))

BENCH_SPEC = importlib.util.spec_from_file_location(
    "aius_latency_bench", TOOLS / "bench.py"
)
BENCH = importlib.util.module_from_spec(BENCH_SPEC)
BENCH_SPEC.loader.exec_module(BENCH)

LINKED_SPEC = importlib.util.spec_from_file_location(
    "aius_linked_probe", TOOLS / "linked_probe.py"
)
LINKED = importlib.util.module_from_spec(LINKED_SPEC)
LINKED_SPEC.loader.exec_module(LINKED)

REPEAT_SPEC = importlib.util.spec_from_file_location(
    "aius_latency_repeat", TOOLS / "repeat.py"
)
REPEAT = importlib.util.module_from_spec(REPEAT_SPEC)
REPEAT_SPEC.loader.exec_module(REPEAT)


class GeneratorTests(unittest.TestCase):
    def test_codes_are_deterministic_unique_and_bounded(self):
        first = BENCH.codes()
        second = BENCH.codes()

        self.assertEqual(len(first), BENCH.CODE_COUNT)
        self.assertEqual(
            len({code.tobytes() for code in first}), BENCH.CODE_COUNT
        )
        for left, right in zip(first, second):
            np.testing.assert_array_equal(left, right)
            self.assertEqual(len(left), BENCH.CODE_FRAMES)
            self.assertLessEqual(int(np.max(np.abs(left))), 6000)

    def test_pcm_positions_and_carrier_geometry(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "carrier.s16le"
            positions = BENCH.make_pcm(path, BENCH.codes())
            carrier = np.fromfile(path, dtype="<i2").reshape(-1, 2)

        self.assertEqual(len(positions), BENCH.CODE_COUNT)
        self.assertEqual(positions[0], BENCH.WARMUP_FRAMES)
        self.assertTrue(np.all(
            np.diff(positions) == BENCH.CODE_FRAMES + BENCH.GUARD_FRAMES
        ))
        self.assertTrue(np.all(carrier[:BENCH.WARMUP_FRAMES] == 0))
        self.assertTrue(np.all(carrier[-BENCH.TAIL_FRAMES:] == 0))

    def test_ac3_codes_remain_inside_access_units(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            _, decoded, source_positions, pa_positions = BENCH.make_ac3(
                root, BENCH.codes()
            )
            reference_positions = BENCH.locate_reference_codes(
                decoded, BENCH.codes(), source_positions
            )

        self.assertGreater(len(pa_positions), BENCH.CODE_COUNT)
        self.assertTrue(np.all(np.diff(pa_positions) == 1536))
        self.assertTrue(all(
            position % 1536 + BENCH.CODE_FRAMES <= 1536
            for position in reference_positions
        ))


class AnalyzerTests(unittest.TestCase):
    def make_delayed_capture(self, delay):
        probe_codes = BENCH.codes()
        positions = [
            2048 + index * (BENCH.CODE_FRAMES + 1024)
            for index in range(BENCH.CODE_COUNT)
        ]
        frames = positions[-1] + BENCH.CODE_FRAMES + delay + 2048
        reference = np.zeros((frames, 2), dtype="<i2")
        for position, code in zip(positions, probe_codes):
            reference[
                position:position + BENCH.CODE_FRAMES, 0
            ] = code
            reference[
                position:position + BENCH.CODE_FRAMES, 1
            ] = code
        capture = np.zeros_like(reference)
        capture[delay:] = reference[:-delay]
        return reference, capture, positions

    def test_pcm_analysis_recovers_exact_delay(self):
        reference, capture, positions = self.make_delayed_capture(317)
        result = BENCH.analyze(
            "pcm", capture, reference, positions, positions
        )

        self.assertEqual(result["accepted_occurrences"], BENCH.CODE_COUNT)
        self.assertEqual(result["raw_latency"]["median_frames"], 317)
        self.assertAlmostEqual(
            result["affine"]["capture_per_playback_ratio"], 1.0
        )
        self.assertLess(
            result["affine"]["residual_max_abs_frames"], 1e-6
        )
        self.assertEqual(result["channel_skew"]["max_frames"], 0)
        self.assertEqual(result["unequal_stereo_frames"], 0)

    def test_channel_phase_slip_is_reported_separately(self):
        reference, capture, positions = self.make_delayed_capture(317)
        capture[:, 1] = 0
        capture[318:, 1] = reference[:-318, 1]

        result = BENCH.analyze(
            "pcm", capture, reference, positions, positions
        )

        self.assertEqual(
            result["channel_latency"][0]["median_frames"], 317
        )
        self.assertEqual(
            result["channel_latency"][1]["median_frames"], 318
        )
        self.assertEqual(result["channel_skew"]["median_frames"], 1)
        self.assertGreater(result["unequal_stereo_frames"], 0)

    def test_ac3_tail_projects_from_first_output(self):
        reference, capture, positions = self.make_delayed_capture(71)
        result = BENCH.analyze(
            "ac3", capture, reference, positions, positions
        )

        self.assertEqual(result["raw_latency"]["median_frames"], 71)
        self.assertEqual(
            result["access_unit_tail"]["median_frames"], 71 + 1535
        )

    def test_silence_and_clipping_fail_closed(self):
        reference, capture, positions = self.make_delayed_capture(71)
        with self.assertRaisesRegex(RuntimeError, "only 0/24"):
            BENCH.analyze(
                "pcm", np.zeros_like(capture), reference, positions, positions
            )

        capture[0, 0] = -32768
        with self.assertRaisesRegex(RuntimeError, "clipped"):
            BENCH.analyze(
                "pcm", capture, reference, positions, positions
            )

    def test_counter_delta_wraps_at_uint32(self):
        initial = self.status()
        final = self.status()
        initial["clock"]["trim_updates"] = 0xFFFFFFFF
        final["clock"]["trim_updates"] = 1

        deltas = BENCH.counter_deltas(initial, final)

        self.assertEqual(deltas["clock.trim_updates"], 2)

    def test_run_latency_is_grouped_by_measurement_queue_level(self):
        trials = [
            self.trial(1, 640, 800),
            self.trial(2, 768, 900),
            self.trial(3, 640, 820),
        ]

        groups = REPEAT.latency_by_initial_tx_level(trials)

        self.assertEqual(groups["640"]["trials"], 2)
        self.assertEqual(
            groups["640"]["latency_frames"]["median"], 810
        )
        self.assertEqual(groups["768"]["trials"], 1)

    @staticmethod
    def trial(number, level, latency):
        return {
            "number": number,
            "manifest": {
                "status_initial": {
                    "clock": {"tx_level_frames": level}
                }
            },
            "summary": {
                "raw_latency": {"median_frames": latency}
            },
        }

    @staticmethod
    def status():
        return {
            "overruns": 0,
            "event_overflows": 0,
            "audio_diag": {
                "ingress_dropped_chunks": 0,
                "ingress_dropped_words": 0,
                "source_changes": 0,
                "source_flushes": 0,
                "gap_flushes": 0,
                "service_generation_resets": 0,
                "service_requested_resets": 0,
                "service_budget_overruns": 0,
                "runtime_epoch_budget_overruns": 0,
                "transmit_stops": 0,
                "transmit_stalls": 0,
                "parse_rejects": 0,
                "decode_rejects": 0,
                "output_drops": 0,
                "au_abandons": 0,
            },
            "clock": {
                "tx_underrun_frames": 0,
                "rate_changes": 0,
                "structural_replans": 0,
                "trim_updates": 0,
                "invalid_windows": 0,
                "clock_faults": 0,
                "mismatch_blocks": 0,
                "unsupported_blocks": 0,
                "unsupported_episodes": 0,
            },
            "pipeline_diag": {
                "decode_rejects": 0,
                "rejects_no_space": 0,
                "rejects_malformed": 0,
                "rejects_unsupported": 0,
                "resyncs": 0,
                "alloc_count": 0,
                "dts_alloc_count": 0,
                "ac3_alloc_count": 0,
            },
        }


class CollectorContractTests(unittest.TestCase):
    def test_collect_rejects_non_stereo_input_before_opening_alsa(self):
        mono = np.zeros((128, 1), dtype="<i2")
        with self.assertRaisesRegex(
            ValueError, "contiguous stereo"
        ):
            LINKED.collect(
                "not-opened", mono, 48000, 256, 1024, None, 0
            )


if __name__ == "__main__":
    unittest.main()
