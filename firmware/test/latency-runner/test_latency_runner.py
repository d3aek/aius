import importlib.util
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
import wave

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
        for mode, frames in (
            ("pcm", BENCH.PCM_CODE_FRAMES),
            ("ac3", BENCH.AC3_CODE_FRAMES),
        ):
            with self.subTest(mode=mode):
                first = BENCH.codes(mode)
                second = BENCH.codes(mode)

                self.assertEqual(len(first), BENCH.CODE_COUNT)
                self.assertEqual(
                    len({code.tobytes() for code in first}),
                    BENCH.CODE_COUNT,
                )
                self.assertEqual(
                    len({
                        code[:BENCH.SYNC_FRAMES].tobytes()
                        for code in first
                    }),
                    BENCH.CODE_COUNT,
                )
                for left, right in zip(first, second):
                    np.testing.assert_array_equal(left, right)
                    self.assertEqual(len(left), frames)
                    self.assertLessEqual(
                        int(np.max(np.abs(left))), 6000
                    )

    def test_pcm_positions_and_carrier_geometry(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "carrier.s16le"
            positions = BENCH.make_pcm(path, BENCH.codes("pcm"))
            carrier = np.fromfile(path, dtype="<i2").reshape(-1, 2)

        self.assertEqual(len(positions), BENCH.CODE_COUNT)
        self.assertEqual(positions[0], BENCH.WARMUP_FRAMES)
        self.assertTrue(np.all(
            np.diff(positions)
            == BENCH.PCM_CODE_FRAMES + BENCH.PCM_PROBE_GAP_FRAMES
        ))
        self.assertTrue(np.all(carrier[:BENCH.WARMUP_FRAMES] == 0))
        self.assertTrue(np.all(carrier[-BENCH.TAIL_FRAMES:] == 0))

    def test_ac3_carrier_has_fixed_access_unit_geometry(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            _, source_positions, pa_positions = BENCH.make_ac3(
                root, BENCH.codes("ac3")
            )
            with wave.open(str(root / "source-5.1.wav"), "rb") as source:
                wav_geometry = (
                    source.getnchannels(),
                    source.getsampwidth(),
                    source.getframerate(),
                )

        self.assertGreater(len(pa_positions), BENCH.CODE_COUNT)
        self.assertTrue(np.all(np.diff(pa_positions) == 1536))
        self.assertEqual(len(source_positions), BENCH.CODE_COUNT)
        self.assertEqual(wav_geometry, (6, 2, 48000))

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
    def make_delayed_capture(self, delay, mode="pcm"):
        code_frames, guard_frames = BENCH.mode_geometry(mode)
        probe_gap_frames = BENCH.mode_probe_gap(mode)
        probe_codes = BENCH.codes(mode)
        positions = [
            8192 + index * (code_frames + probe_gap_frames)
            for index in range(BENCH.CODE_COUNT)
        ]
        frames = (
            positions[-1]
            + code_frames
            + guard_frames
            + delay
            + 1024
        )
        reference = np.zeros((frames, 2), dtype="<i2")
        for position, code in zip(positions, probe_codes):
            reference[
                position:position + code_frames, 0
            ] = code
            reference[
                position:position + code_frames, 1
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
        reference, capture, positions = self.make_delayed_capture(
            71, "ac3"
        )
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

    def test_missing_and_swapped_codes_fail_closed(self):
        reference, capture, positions = self.make_delayed_capture(71)
        width = BENCH.PCM_CODE_FRAMES
        missing = capture.copy()
        missing[positions[0] + 71:positions[0] + 71 + width] = 0
        with self.assertRaisesRegex(RuntimeError, "only 23/24"):
            BENCH.analyze(
                "pcm", missing, reference, positions, positions
            )

        swapped = capture.copy()
        first = positions[0] + 71
        second = positions[1] + 71
        first_code = swapped[first:first + width].copy()
        swapped[first:first + width] = swapped[second:second + width]
        swapped[second:second + width] = first_code
        with self.assertRaisesRegex(RuntimeError, "formal acceptance"):
            BENCH.analyze(
                "pcm", swapped, reference, positions, positions
            )

    def test_periodic_duplicate_peak_fails_ambiguity_gate(self):
        template = BENCH.codes("pcm")[0]
        signal = np.zeros(10000, dtype="<i2")
        signal[1000:1000 + len(template)] = template
        signal[6000:6000 + len(template)] = template

        evidence = BENCH.correlation_evidence(
            signal, template, 0, len(signal) - len(template)
        )

        self.assertAlmostEqual(evidence["peak_ratio"], 1.0)
        self.assertLess(
            evidence["peak_ratio"], BENCH.MIN_PEAK_RATIO
        )

    def test_signal_to_guard_threshold_discriminates_one_step(self):
        frames = BENCH.PCM_CODE_FRAMES
        guard = BENCH.PCM_GUARD_FRAMES
        signal = np.full(2 * guard + frames, 100, dtype="<i2")
        position = guard
        signal[position:position + frames] = 3163
        passing = BENCH.signal_to_guard_db(
            signal, position, frames, guard
        )
        signal[position:position + frames] = 3162
        failing = BENCH.signal_to_guard_db(
            signal, position, frames, guard
        )

        self.assertGreaterEqual(
            passing, BENCH.MIN_SIGNAL_TO_GUARD_DB
        )
        self.assertLess(failing, BENCH.MIN_SIGNAL_TO_GUARD_DB)

    def test_render_level_fails_without_six_db_limiter_margin(self):
        model = {
            "features": {"limiter": True},
            "limiter": {"enabled": True, "threshold_dbfs": -1},
        }
        full_scale = 8388607
        passing_peak = int(
            full_scale * 10 ** (-8.01 / 20.0)
        )
        failing_peak = int(
            full_scale * 10 ** (-7.99 / 20.0)
        )
        passing = np.array([[passing_peak, passing_peak]], dtype="<i4")
        failing = np.array([[failing_peak, failing_peak]], dtype="<i4")

        evidence = BENCH.validate_render_level(passing, model)

        self.assertGreaterEqual(evidence["margin_db"], 0)
        with self.assertRaisesRegex(RuntimeError, "lacks 6 dB"):
            BENCH.validate_render_level(failing, model)

    def test_exclusions_are_explicit_and_leave_at_least_twenty(self):
        reference, capture, positions = self.make_delayed_capture(71)
        result = BENCH.analyze(
            "pcm",
            capture,
            reference,
            positions,
            positions,
            excluded_indices={0, 1, 2, 3},
        )

        self.assertEqual(result["accepted_occurrences"], 20)
        self.assertEqual(result["excluded_indices"], [0, 1, 2, 3])
        with self.assertRaisesRegex(RuntimeError, "fewer than 20"):
            BENCH.analyze(
                "pcm",
                capture,
                reference,
                positions,
                positions,
                excluded_indices={0, 1, 2, 3, 4},
            )

    def test_affine_ratio_and_residual_bounds_fail_closed(self):
        reference, _, positions = self.make_delayed_capture(71)
        width = BENCH.PCM_CODE_FRAMES
        capture = np.zeros_like(reference)
        for position in positions:
            target = round(71 + 1.001 * position)
            capture[target:target + width] = (
                reference[position:position + width]
            )
        with self.assertRaisesRegex(RuntimeError, "ratio is out of bounds"):
            BENCH.analyze(
                "pcm", capture, reference, positions, positions
            )

        _, residual_capture, _ = self.make_delayed_capture(71)
        moved = residual_capture.copy()
        source = positions[12] + 71
        code = moved[source:source + width].copy()
        moved[source:source + width] = 0
        moved[source + 200:source + 200 + width] = code
        with self.assertRaisesRegex(RuntimeError, "residual exceeds"):
            BENCH.analyze(
                "pcm", moved, reference, positions, positions
            )

    def test_acceptance_contract_pins_every_numeric_gate(self):
        contract = BENCH.acceptance_contract("ac3")

        self.assertEqual(contract["expected_codes"], 24)
        self.assertEqual(contract["minimum_accepted_codes"], 20)
        self.assertEqual(contract["code_frames"], 1024)
        self.assertEqual(contract["sync_prefix_frames"], 127)
        self.assertEqual(contract["guard_frames_each_side"], 3072)
        self.assertEqual(contract["minimum_correlation"], 0.35)
        self.assertEqual(contract["minimum_peak_ratio"], 2.0)
        self.assertEqual(contract["minimum_signal_to_guard_db"], 30.0)
        self.assertEqual(contract["minimum_clock_ratio"], 0.9995)
        self.assertEqual(contract["maximum_clock_ratio"], 1.0005)
        self.assertEqual(
            contract["maximum_linked_trigger_skew_frames"], 1.0
        )
        self.assertEqual(
            contract["maximum_affine_residual_frames"], 128.0
        )

    def test_preflight_manifest_serializes_contract_before_collection(self):
        manifest = BENCH.make_preflight(
            mode="pcm",
            device="hw:test,0",
            status_url="http://device/api/status",
            config_url="http://device/api/dsp/config",
            renderer=pathlib.Path("/renderer"),
            period=256,
            buffer_size=4096,
            warmup_frames=48000,
            status_delay_seconds=0.5,
            probe_gap_frames=BENCH.PCM_PROBE_GAP_FRAMES,
            tail_frames=12000,
            command_argv=["bench.py", "--mode", "pcm"],
            versions={"python": "test", "numpy": "test"},
            repository={
                "harness_revision": "a" * 40,
                "expected_build_revision": (
                    "aaaaaaaaaaaa+teensy41-optical"
                ),
                "sample_rate_evidence_revision": "b" * 40,
                "sample_rate_evidence_is_ancestor": True,
            },
            started_utc="2026-07-31T00:00:00Z",
        )

        self.assertEqual(
            manifest["schema"], "aius-e2e-latency-evidence-v1"
        )
        self.assertEqual(manifest["state"], "preflight")
        self.assertEqual(
            manifest["acceptance"], BENCH.acceptance_contract("pcm")
        )
        self.assertEqual(
            manifest["requested_io"]["rate_hz"], BENCH.RATE
        )
        self.assertEqual(
            manifest["stimulus"]["probe_gap_frames"],
            BENCH.PCM_PROBE_GAP_FRAMES,
        )
        self.assertEqual(
            manifest["collection"]["status_delay_seconds"], 0.5
        )

    def test_formal_warmup_covers_active_source_clock_settle(self):
        self.assertEqual(BENCH.FORMAL_WARMUP_FRAMES % 1536, 0)
        self.assertGreaterEqual(
            BENCH.FORMAL_WARMUP_FRAMES / BENCH.RATE, 50.0
        )
        self.assertGreaterEqual(
            BENCH.FORMAL_STATUS_DELAY_SECONDS, 49.0
        )
        self.assertLess(
            BENCH.FORMAL_STATUS_DELAY_SECONDS,
            BENCH.FORMAL_WARMUP_FRAMES / BENCH.RATE,
        )

    def test_repository_identity_proves_sample_rate_ancestor(self):
        identity = BENCH.repository_identity()

        self.assertTrue(identity["phase1_image_is_ancestor"])
        self.assertEqual(
            identity["phase1_image_revision"],
            BENCH.PHASE1_IMAGE_REVISION,
        )
        self.assertTrue(identity["sample_rate_evidence_is_ancestor"])
        self.assertEqual(
            identity["sample_rate_evidence_revision"],
            BENCH.SAMPLE_RATE_EVIDENCE_REVISION,
        )
        self.assertEqual(
            identity["expected_build_revision"],
            f"{BENCH.PHASE1_IMAGE_REVISION[:12]}"
            "+teensy41-optical",
        )

    def test_evidence_manifest_rejects_schema_contract_and_paths(self):
        manifest = BENCH.make_preflight(
            mode="pcm",
            device="hw:test,0",
            status_url="http://device/api/status",
            config_url="http://device/api/dsp/config",
            renderer=pathlib.Path("/renderer"),
            period=256,
            buffer_size=4096,
            warmup_frames=48000,
            status_delay_seconds=0.5,
            probe_gap_frames=BENCH.PCM_PROBE_GAP_FRAMES,
            tail_frames=12000,
            command_argv=["bench.py"],
            versions={"python": "test", "numpy": "test"},
            repository={
                "harness_revision": "a" * 40,
                "expected_build_revision": (
                    "aaaaaaaaaaaa+teensy41-optical"
                ),
                "sample_rate_evidence_revision": "b" * 40,
                "sample_rate_evidence_is_ancestor": True,
            },
            started_utc="2026-07-31T00:00:00Z",
        )
        manifest["state"] = "accepted"
        manifest["generation"] = {
            "code_count": 24,
            "code_frames": 2048,
            "input_positions": list(range(24)),
            "reference_positions": list(range(24)),
        }
        manifest["artifacts"] = {
            name: {"path": f"{name}.bin", "sha256": "0" * 64}
            for name in (
                "carrier",
                "capture",
                "effective_dsp_config",
                "renderer_dsp_config",
                "native_reference",
                "native_decoded",
                "native_render_manifest",
                "summary",
            )
        }
        manifest["status_initial"] = {}
        manifest["status_final"] = {}
        manifest["collector_acceptance"] = {}
        BENCH.validate_evidence_manifest(manifest, "pcm")

        mutations = {
            "schema": lambda value: value.update(schema="unknown"),
            "contract": lambda value: value["acceptance"].update(
                minimum_correlation=0.34
            ),
            "path": lambda value: value["artifacts"]["carrier"].update(
                path="../carrier.bin"
            ),
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label):
                malformed = json.loads(json.dumps(manifest))
                mutate(malformed)
                with self.assertRaises(RuntimeError):
                    BENCH.validate_evidence_manifest(malformed, "pcm")

    def test_counter_delta_wraps_at_uint32(self):
        initial = self.status()
        final = self.status()
        initial["clock"]["trim_updates"] = 0xFFFFFFFF
        final["clock"]["trim_updates"] = 1

        deltas = BENCH.counter_deltas(initial, final)

        self.assertEqual(deltas["clock.trim_updates"], 2)

    def test_status_bracket_accepts_clean_pcm_and_ac3_progress(self):
        for mode in ("pcm", "ac3"):
            with self.subTest(mode=mode):
                initial = self.status()
                final = json.loads(json.dumps(initial))
                if mode == "ac3":
                    for status in (initial, final):
                        status["pipeline_diag"]["class"] = 2
                        status["pipeline_diag"]["codec_pc"] = 1
                        status["pipeline_diag"]["pcm_confirmed"] = 0
                    final["audio_diag"]["decode_units"] = 24
                    final["pipeline_diag"]["bursts_ok"] = 24
                final["audio_diag"]["input_words_consumed"] = 1000
                final["audio_diag"]["service_steps_committed"] = 50
                final["audio_diag"]["committed_output_blocks"] = 50

                result = BENCH.validate_status_bracket(
                    mode,
                    initial,
                    final,
                    {"sample_rate_hz": 48000},
                    "revision",
                )

                self.assertTrue(result["accepted"])
                self.assertGreater(
                    result["progress_deltas"][
                        "audio_diag.input_words_consumed"
                    ],
                    0,
                )

    def test_status_bracket_fails_each_runtime_gate(self):
        mutations = {
            "follow": lambda status: status["clock"].update(
                output_rate_millihz=48001001
            ),
            "counter": lambda status: status["audio_diag"].update(
                output_drops=1
            ),
            "class": lambda status: status["pipeline_diag"].update(
                **{"class": 0}
            ),
            "failure": lambda status: status["audio_diag"].update(
                service_last_failure=1
            ),
            "progress": lambda status: status["audio_diag"].update(
                input_words_consumed=0
            ),
            "build": lambda status: status.update(
                build_revision="other"
            ),
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label):
                initial = self.status()
                final = json.loads(json.dumps(initial))
                final["audio_diag"]["input_words_consumed"] = 1000
                final["audio_diag"]["service_steps_committed"] = 50
                final["audio_diag"]["committed_output_blocks"] = 50
                mutate(final)
                with self.assertRaisesRegex(
                    RuntimeError, "status bracket rejected"
                ):
                    BENCH.validate_status_bracket(
                        "pcm",
                        initial,
                        final,
                        {"sample_rate_hz": 48000},
                        "revision",
                    )

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
            "build_revision": "revision",
            "build_utc": "2026-07-31T00:00:00Z",
            "boot_generation": 1,
            "reset_source": 1,
            "watchdog_early_resets": 0,
            "carrier": 1,
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
                "input_words_consumed": 0,
                "service_steps_committed": 0,
                "committed_output_blocks": 0,
                "decode_units": 0,
                "service_last_failure": 0,
                "pipeline_last_failure": 0,
            },
            "clock": {
                "carrier_locked": True,
                "carrier_ready": True,
                "carrier_rate_millihz": 48000000,
                "nominal_rate_hz": 48000,
                "stream_rate_hz": 48000,
                "output_rate_millihz": 48000000,
                "master": "input",
                "state": "tracking",
                "tx_underrun_frames": 0,
                "rate_changes": 0,
                "structural_replans": 0,
                "trim_updates": 0,
                "invalid_windows": 0,
                "clock_faults": 0,
                "mismatch_blocks": 0,
                "unsupported_blocks": 0,
                "unsupported_episodes": 0,
                "last_clock_fault": 0,
            },
            "pipeline_diag": {
                "class": 1,
                "codec_pc": 0,
                "pcm_confirmed": 1,
                "last_status": 0,
                "decode_rejects": 0,
                "rejects_no_space": 0,
                "rejects_malformed": 0,
                "rejects_unsupported": 0,
                "resyncs": 0,
                "alloc_count": 0,
                "dts_alloc_count": 0,
                "ac3_alloc_count": 0,
                "bursts_ok": 0,
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
        decoded = root / f"{label}-decoded.s32le"
        manifest = root / f"{label}.json"
        command = [
            NATIVE_RENDERER,
            "--carrier", str(vector),
            "--config", str(self.normalized_config(root)),
            "--pcm", str(pcm),
            "--decoded", str(decoded),
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
        result = json.loads(manifest.read_text())
        self.assertEqual(
            decoded.stat().st_size,
            result["decoded"]["frames"]
            * result["decoded"]["channels"]
            * 4,
        )
        return pcm, manifest, result

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
                6,
            ),
            (
                "dts-be",
                vectors / "dts_be16.iec",
                13,
                512,
                1,
                "big-endian",
                2,
            ),
            (
                "dts-le",
                vectors / "dts_le16.iec",
                13,
                512,
                1,
                "little-endian",
                2,
            ),
        )
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for (
                label,
                vector,
                pc,
                frames,
                calls,
                byte_order,
                channels,
            ) in cases:
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
                        result["decoded"]["channels"], channels
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

    def test_wrapper_hashes_normalized_elementary_payload(self):
        vectors = ROOT / "tools" / "iec_corpus" / "vectors"
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            _, manifest = BENCH.render_native(
                pathlib.Path(NATIVE_RENDERER),
                vectors / "dts_le16.iec",
                self.normalized_config(root),
                root,
            )

        expected = hashlib.sha256(
            (vectors / "dts_le16.pay").read_bytes()
        ).hexdigest()
        self.assertEqual(
            manifest["units"][0]["elementary_sha256"], expected
        )

    def test_pcm_carrier_uses_native_pipeline_and_dsp_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            carrier = root / "pcm.s16le"
            source_positions = BENCH.make_pcm(
                carrier, BENCH.codes("pcm")
            )
            reference, manifest = BENCH.render_native(
                pathlib.Path(NATIVE_RENDERER),
                carrier,
                self.normalized_config(root),
                root,
            )
            reference_positions = BENCH.locate_reference_codes(
                reference, BENCH.codes("pcm"), source_positions
            )

        self.assertEqual(manifest["pipeline"]["class"], 1)
        self.assertEqual(manifest["units"], [])
        self.assertEqual(manifest["decoded"]["channels"], 2)
        self.assertEqual(len(reference_positions), BENCH.CODE_COUNT)
        self.assertTrue(np.all(np.diff(reference_positions) > 0))

    def test_generated_ac3_codes_map_through_native_unit_oracles(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            carrier, source_positions, pa_positions = BENCH.make_ac3(
                root, BENCH.codes("ac3")
            )
            reference, manifest = BENCH.render_native(
                pathlib.Path(NATIVE_RENDERER),
                carrier,
                self.normalized_config(root),
                root,
            )
            reference_positions = BENCH.locate_reference_codes(
                reference, BENCH.codes("ac3"), source_positions
            )
            input_positions, unit_indices = (
                BENCH.map_encoded_input_positions(
                    reference_positions, manifest
                )
            )
            render_level = BENCH.validate_render_level(
                reference,
                json.loads(
                    (ROOT / "config" / "optical_dsp.json").read_text()
                ),
            )

        self.assertEqual(manifest["pipeline"]["codec_pc"], 1)
        self.assertEqual(len(input_positions), BENCH.CODE_COUNT)
        self.assertEqual(len(set(unit_indices)), BENCH.CODE_COUNT)
        self.assertGreaterEqual(render_level["margin_db"], 0)
        self.assertEqual(
            [unit["pa_index"] // 2 for unit in manifest["units"]],
            pa_positions,
        )


class CollectorContractTests(unittest.TestCase):
    class ShortIo:
        class Lib:
            @staticmethod
            def snd_pcm_writei(handle, pointer, frames):
                return frames - 1

            @staticmethod
            def snd_pcm_readi(handle, pointer, frames):
                return frames - 1

        lib = Lib()

        @staticmethod
        def check(result, operation):
            return result

    @staticmethod
    def manifest():
        status = {
            "state": 3,
            "trigger_timestamp_ns": 123456,
            "observed_timestamp_ns": 123500,
            "delay_frames": 0,
            "available_frames": 0,
            "available_max_frames": 0,
            "overrange_frames": 0,
        }
        geometry = {
            "rate_hz": 48000,
            "period_frames": 256,
            "buffer_frames": 4096,
            "start_threshold_frames": 4096,
            "timestamp_type": "monotonic",
        }
        capture_geometry = dict(geometry)
        capture_geometry["start_threshold_frames"] = 256
        return {
            "playback": dict(geometry),
            "capture": capture_geometry,
            "linked": True,
            "xrun_recoveries": 0,
            "short_transfers": 0,
            "alsa_status_pre_trigger": {
                "playback": dict(status),
                "capture": dict(status),
            },
            "alsa_status_post_trigger": {
                "playback": dict(status),
                "capture": dict(status),
            },
            "alsa_status_end": {
                "playback": dict(status),
                "capture": dict(status),
            },
        }

    def test_collector_manifest_accepts_exact_linked_trigger(self):
        result = LINKED.validate_collection_manifest(
            self.manifest(), 48000, 256, 4096
        )

        self.assertTrue(result["accepted"])
        self.assertEqual(
            result["linked_trigger_timestamp_ns"], 123456
        )
        self.assertEqual(result["linked_trigger_skew_ns"], 0)
        self.assertEqual(result["linked_trigger_skew_frames"], 0.0)

    def test_collector_manifest_accepts_only_subframe_trigger_skew(self):
        manifest = self.manifest()
        manifest["alsa_status_post_trigger"]["capture"][
            "trigger_timestamp_ns"
        ] += 1839

        result = LINKED.validate_collection_manifest(
            manifest, 48000, 256, 4096
        )

        self.assertEqual(result["linked_trigger_skew_ns"], 1839)
        self.assertAlmostEqual(
            result["linked_trigger_skew_frames"],
            1839 * 48000 / 1_000_000_000,
        )

    def test_alsa_status_abi_symbols_resolve(self):
        self.assertIsNotNone(LINKED.Alsa().lib.snd_pcm_status)

    def test_short_playback_and_capture_transfers_fail(self):
        samples = np.zeros((32, 2), dtype="<i2")
        with self.assertRaisesRegex(
            LINKED.AlsaError, "short playback transfer"
        ):
            LINKED.write_all(self.ShortIo(), None, samples, 0, 32)
        with self.assertRaisesRegex(
            LINKED.AlsaError, "short capture transfer"
        ):
            LINKED.read_all(self.ShortIo(), None, samples, 32)

    def test_collector_manifest_fails_geometry_xrun_and_trigger_drift(self):
        mutations = {
            "geometry": lambda manifest: manifest["capture"].update(
                rate_hz=44100
            ),
            "xrun": lambda manifest: manifest.update(
                xrun_recoveries=1
            ),
            "trigger": lambda manifest: manifest[
                "alsa_status_post_trigger"
            ]["capture"].update(trigger_timestamp_ns=144290),
            "not-running": lambda manifest: manifest[
                "alsa_status_post_trigger"
            ]["capture"].update(state=2),
            "overrange": lambda manifest: manifest[
                "alsa_status_end"
            ]["capture"].update(overrange_frames=1),
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label):
                manifest = self.manifest()
                mutate(manifest)
                with self.assertRaisesRegex(
                    LINKED.AlsaError, "collector evidence rejected"
                ):
                    LINKED.validate_collection_manifest(
                        manifest, 48000, 256, 4096
                    )

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
