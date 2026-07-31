import importlib.util
import json
import os
import pathlib
import subprocess
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

NATIVE_RENDERER = os.environ.get("AIUS_LATENCY_RENDERER")


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

    def test_ac3_carrier_has_fixed_access_unit_geometry(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            _, source_positions, pa_positions = BENCH.make_ac3(
                root, BENCH.codes()
            )

        self.assertGreater(len(pa_positions), BENCH.CODE_COUNT)
        self.assertTrue(np.all(np.diff(pa_positions) == 1536))
        self.assertEqual(len(source_positions), BENCH.CODE_COUNT)

    def test_effective_config_normalization_changes_only_wire_fields(self):
        model = json.loads(
            (ROOT / "config" / "optical_dsp.json").read_text()
        )
        model["config_generation"] = 37
        normalized = json.loads(BENCH.normalize_effective_config(
            json.dumps(model).encode()
        ))

        self.assertEqual(normalized["source"], "sd")
        self.assertEqual(normalized["base_generation"], 37)
        for key, value in model.items():
            if key != "source":
                self.assertEqual(normalized[key], value)

    def test_effective_config_normalization_is_bounded(self):
        for raw in (b"", b" " * (BENCH.MAX_DSP_CONFIG_BYTES + 1)):
            with self.subTest(length=len(raw)):
                with self.assertRaisesRegex(RuntimeError, "invalid size"):
                    BENCH.normalize_effective_config(raw)

    def test_encoded_positions_use_native_unit_timing_oracles(self):
        manifest = {
            "units": [
                {
                    "index": 4,
                    "pa_index": 2000,
                    "post_frame_zero": 120,
                    "post_frame_last": 1655,
                },
                {
                    "index": 5,
                    "pa_index": 5072,
                    "post_frame_zero": 1656,
                    "post_frame_last": 3191,
                },
            ]
        }

        positions, units = BENCH.map_encoded_input_positions(
            [256, 1800], manifest
        )

        self.assertEqual(positions, [1136, 2680])
        self.assertEqual(units, [4, 5])


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


@unittest.skipUnless(NATIVE_RENDERER, "native renderer not built")
class NativeRendererTests(unittest.TestCase):
    def normalized_config(self, root):
        model = json.loads(
            (ROOT / "config" / "optical_dsp.json").read_text()
        )
        model["source"] = "sd"
        model["base_generation"] = 0
        path = root / "normalized-dsp.json"
        path.write_text(json.dumps(model, separators=(",", ":")))
        return path

    def render(self, root, vector, label):
        pcm = root / f"{label}.s24le32"
        manifest = root / f"{label}.json"
        command = [
            NATIVE_RENDERER,
            "--carrier", str(vector),
            "--config", str(self.normalized_config(root)),
            "--pcm", str(pcm),
            "--manifest", str(manifest),
        ]
        completed = subprocess.run(
            command, check=False, text=True, capture_output=True
        )
        self.assertEqual(
            completed.returncode,
            0,
            f"{completed.stdout}\n{completed.stderr}",
        )
        return pcm, manifest, json.loads(manifest.read_text())

    def test_exact_ac3_and_dts_carriers_render_continuously(self):
        vectors = ROOT / "tools" / "iec_corpus" / "vectors"
        cases = (
            (
                "ac3",
                vectors / "ac3_surround51_ident.iec",
                1,
                1536,
                6,
                "big-endian",
            ),
            (
                "dts-be",
                vectors / "dts_be16.iec",
                13,
                512,
                1,
                "big-endian",
            ),
            (
                "dts-le",
                vectors / "dts_le16.iec",
                13,
                512,
                1,
                "little-endian",
            ),
        )
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for label, vector, pc, frames, calls, byte_order in cases:
                with self.subTest(label=label):
                    pcm, _, result = self.render(
                        root, vector, label
                    )
                    units = result["units"]
                    self.assertGreater(len(units), 0)
                    self.assertEqual(
                        {unit["pc"] for unit in units}, {pc}
                    )
                    self.assertEqual(
                        result["pipeline"]["parse_rejects"], 0
                    )
                    self.assertEqual(
                        result["pipeline"]["decode_rejects"], 0
                    )
                    self.assertEqual(
                        result["parser"]["scan_malformed"], 0
                    )
                    self.assertEqual(
                        result["parser"]["pipeline_malformed"], 0
                    )
                    self.assertEqual(
                        pcm.stat().st_size,
                        result["rendered_frames"] * 2 * 4,
                    )
                    previous_pa = -1
                    previous_end = 0
                    for unit in units:
                        self.assertEqual(unit["byte_order"], byte_order)
                        self.assertGreater(unit["pa_index"], previous_pa)
                        self.assertGreater(unit["period_words"], 0)
                        self.assertEqual(unit["decoded_frames"], frames)
                        self.assertEqual(unit["decoder_calls"], calls)
                        self.assertGreater(unit["channel_mask"], 0)
                        self.assertEqual(unit["sample_rate_hz"], 48000)
                        self.assertEqual(
                            unit["pre_end"] - unit["pre_start"],
                            frames,
                        )
                        self.assertEqual(
                            unit["post_frame_last"]
                            - unit["post_frame_zero"] + 1,
                            frames,
                        )
                        self.assertEqual(
                            unit["pre_start"], previous_end
                        )
                        previous_pa = unit["pa_index"]
                        previous_end = unit["pre_end"]

    def test_native_render_is_byte_deterministic(self):
        vector = (
            ROOT
            / "tools"
            / "iec_corpus"
            / "vectors"
            / "ac3_surround51_ident.iec"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            first_pcm, first_manifest, _ = self.render(
                root, vector, "first"
            )
            second_pcm, second_manifest, _ = self.render(
                root, vector, "second"
            )
            self.assertEqual(
                first_pcm.read_bytes(), second_pcm.read_bytes()
            )
            self.assertEqual(
                first_manifest.read_bytes(),
                second_manifest.read_bytes(),
            )

    def test_pcm_carrier_uses_native_pipeline_and_dsp_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            carrier = root / "pcm.s16le"
            source_positions = BENCH.make_pcm(carrier, BENCH.codes())
            reference, manifest = BENCH.render_native(
                pathlib.Path(NATIVE_RENDERER),
                carrier,
                self.normalized_config(root),
                root,
            )
            reference_positions = BENCH.locate_reference_codes(
                reference, BENCH.codes(), source_positions
            )

        self.assertEqual(manifest["pipeline"]["class"], 1)
        self.assertEqual(manifest["units"], [])
        self.assertEqual(len(reference_positions), BENCH.CODE_COUNT)
        self.assertTrue(np.all(np.diff(reference_positions) > 0))

    def test_generated_ac3_codes_map_through_native_unit_oracles(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            carrier, source_positions, pa_positions = BENCH.make_ac3(
                root, BENCH.codes()
            )
            reference, manifest = BENCH.render_native(
                pathlib.Path(NATIVE_RENDERER),
                carrier,
                self.normalized_config(root),
                root,
            )
            reference_positions = BENCH.locate_reference_codes(
                reference, BENCH.codes(), source_positions
            )
            input_positions, unit_indices = (
                BENCH.map_encoded_input_positions(
                    reference_positions, manifest
                )
            )

        self.assertEqual(manifest["pipeline"]["codec_pc"], 1)
        self.assertEqual(len(input_positions), BENCH.CODE_COUNT)
        self.assertEqual(len(set(unit_indices)), BENCH.CODE_COUNT)
        self.assertEqual(
            [unit["pa_index"] // 2 for unit in manifest["units"]],
            pa_positions,
        )


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
