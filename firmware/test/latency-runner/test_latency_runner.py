import importlib.util
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
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


def device_status():
    """A healthy `GET /api/status` frame: every gate the harness reads."""
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


def active_source_status(trim=0, invalid=0, follow_millihz=0, **clock):
    """A healthy status with the counters and follow error under test control."""
    status = device_status()
    status["clock"]["trim_updates"] = trim
    status["clock"]["invalid_windows"] = invalid
    status["clock"]["output_rate_millihz"] = (
        status["clock"]["carrier_rate_millihz"] + follow_millihz
    )
    status["clock"].update(clock)
    return status


class VirtualClock:
    """Monotonic seconds the tests advance instead of sleeping."""

    def __init__(self, start=0.0):
        self.seconds = start

    def monotonic(self):
        return self.seconds

    def advance(self, seconds):
        self.seconds += seconds


class FakeAlsa:
    """Enough of the libasound wrapper to run the preflight with no device.

    `lib` is the fake itself, so the ctypes-level entry points the preflight
    calls (`alsa.lib.snd_pcm_writei`) and the wrapper-level ones (`alsa.open`,
    `alsa.configure`, `alsa.status`) share one recorder. Every feed write
    advances the virtual clock by its own duration, which is what makes the
    deadline and settled-window behaviour observable.
    """

    def __init__(
        self,
        clock,
        rate=48000,
        granted_period=256,
        granted_buffer=48000,
        xrun_faults=0,
        state=None,
    ):
        self.clock = clock
        self.rate = rate
        self.granted_period = granted_period
        self.granted_buffer = granted_buffer
        self.pending_xruns = xrun_faults
        self.state = (
            LINKED.SND_PCM_STATE_RUNNING if state is None else state
        )
        self.lib = self
        self.configured = []
        self.opened = []
        self.prepared = 0
        self.starts = 0
        self.drops = 0
        self.closes = 0
        self.prefills = 0
        self.feeds = 0

    def check(self, result, operation):
        if result < 0:
            raise LINKED.AlsaError(f"{operation}: fake failure ({result})")
        return result

    def open(self, device, stream):
        self.opened.append((device, stream))
        return object()

    def configure(self, handle, rate, period, buffer_size, playback,
                  exact=True):
        self.configured.append({
            "rate": rate,
            "period": period,
            "buffer_size": buffer_size,
            "playback": playback,
            "exact": exact,
        })
        return {
            "rate_hz": rate,
            "period_frames": self.granted_period,
            "buffer_frames": self.granted_buffer,
            "start_threshold_frames": self.granted_buffer,
            "timestamp_type": "monotonic",
        }

    def status(self, handle):
        return {
            "state": self.state,
            "trigger_timestamp_ns": 1,
            "observed_timestamp_ns": 2,
            "delay_frames": 0,
            "available_frames": 0,
            "available_max_frames": 0,
            "overrange_frames": 0,
        }

    def snd_pcm_prepare(self, handle):
        self.prepared += 1
        return 0

    def snd_pcm_start(self, handle):
        self.starts += 1
        return 0

    def snd_pcm_drop(self, handle):
        self.drops += 1
        return 0

    def snd_pcm_close(self, handle):
        self.closes += 1
        return 0

    def snd_pcm_writei(self, handle, pointer, frames):
        if frames != self.granted_period:
            self.prefills += 1
            return frames
        self.feeds += 1
        self.clock.advance(frames / self.rate)
        if self.pending_xruns:
            self.pending_xruns -= 1
            return LINKED.ALSA_EPIPE
        return frames


class FakeStatusFeed:
    """Scripted `GET /api/status` responses on the virtual clock.

    `statuses` is either a callable of elapsed seconds or a list whose last
    entry repeats once exhausted -- a device that keeps reporting the same
    thing, which is exactly the settled and timeout cases.
    """

    def __init__(self, clock, statuses, fetch_seconds=0.01):
        self.clock = clock
        self.statuses = statuses
        self.fetch_seconds = fetch_seconds
        self.started = clock.monotonic()
        self.urls = []

    def __call__(self, url):
        self.urls.append(url)
        self.clock.advance(self.fetch_seconds)
        if callable(self.statuses):
            return self.statuses(self.clock.monotonic() - self.started)
        index = min(len(self.urls) - 1, len(self.statuses) - 1)
        return json.loads(json.dumps(self.statuses[index]))


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

    def test_acceptance_contract_pins_the_active_source_preflight(self):
        contract = BENCH.acceptance_contract("pcm")
        preflight = contract["active_source_preflight"]

        self.assertEqual(
            contract["contract_version"], BENCH.ACCEPTANCE_CONTRACT_VERSION
        )
        self.assertEqual(BENCH.ACCEPTANCE_CONTRACT_VERSION, 2)
        self.assertEqual(
            preflight["minimum_seconds"],
            BENCH.ACTIVE_SOURCE_PREFLIGHT_MIN_SECONDS,
        )
        self.assertEqual(
            preflight["timeout_seconds"],
            BENCH.ACTIVE_SOURCE_PREFLIGHT_TIMEOUT_SECONDS,
        )
        self.assertEqual(
            preflight["poll_seconds"],
            BENCH.ACTIVE_SOURCE_PREFLIGHT_POLL_SECONDS,
        )
        self.assertEqual(
            preflight["settled_window_seconds"],
            LINKED.SETTLED_WINDOW_SECONDS,
        )
        self.assertEqual(
            preflight["trim_rate_limit_seconds"],
            LINKED.TRIM_RATE_LIMIT_SECONDS,
        )
        self.assertEqual(
            preflight["maximum_follow_error_millihz"],
            BENCH.MAXIMUM_FOLLOW_ERROR_MILLIHZ,
        )
        self.assertEqual(
            preflight["maximum_xrun_recoveries"],
            LINKED.MAX_PREFLIGHT_XRUN_RECOVERIES,
        )
        self.assertEqual(preflight["accepted_modes"], ["edge", "settled"])

    def test_reuse_refuses_a_legacy_contract_without_the_explicit_flag(self):
        legacy = self.accepted_manifest()
        legacy["acceptance"]["contract_version"] = 1
        legacy["acceptance"].pop("active_source_preflight")
        legacy.pop("active_source_preflight")
        legacy["artifacts"].pop("active_source_preflight")

        with self.assertRaisesRegex(
            RuntimeError, "--allow-legacy-evidence"
        ):
            BENCH.validate_evidence_manifest(legacy, "pcm")

        BENCH.validate_evidence_manifest(legacy, "pcm", True)

    def test_allow_legacy_evidence_still_enforces_the_current_contract(self):
        current = self.accepted_manifest()
        current["acceptance"]["minimum_correlation"] = 0.34

        with self.assertRaisesRegex(RuntimeError, "contract drifted"):
            BENCH.validate_evidence_manifest(current, "pcm", True)

    def test_allow_legacy_evidence_requires_reuse_existing(self):
        args = BENCH.build_parser().parse_args([
            "--mode", "pcm", "--output", "out", "--allow-legacy-evidence",
        ])

        self.assertTrue(args.allow_legacy_evidence)
        with self.assertRaisesRegex(
            ValueError, "only applies to --reuse-existing"
        ):
            BENCH.validate_arguments(args)

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
        self.assertEqual(BENCH.FORMAL_WARMUP_FRAMES, BENCH.WARMUP_FRAMES)
        self.assertGreaterEqual(BENCH.ACTIVE_SOURCE_PREFLIGHT_MIN_SECONDS, 3.0)
        self.assertGreaterEqual(
            BENCH.ACTIVE_SOURCE_PREFLIGHT_TIMEOUT_SECONDS, 60.0
        )
        self.assertLess(
            BENCH.FORMAL_STATUS_DELAY_SECONDS,
            BENCH.FORMAL_WARMUP_FRAMES / BENCH.RATE,
        )

    def test_preflight_timeout_covers_seven_rate_limited_trims(self):
        # A post-reboot device needed five to seven rate-limited trims to
        # converge, and no acceptance is possible before the minimum hold.
        self.assertGreaterEqual(
            BENCH.ACTIVE_SOURCE_PREFLIGHT_TIMEOUT_SECONDS,
            7 * LINKED.TRIM_RATE_LIMIT_SECONDS
            + BENCH.ACTIVE_SOURCE_PREFLIGHT_MIN_SECONDS,
        )
        self.assertEqual(LINKED.TRIM_RATE_LIMIT_SECONDS, 10.0)
        self.assertGreater(
            LINKED.SETTLED_WINDOW_SECONDS, LINKED.TRIM_RATE_LIMIT_SECONDS
        )
        self.assertGreater(
            BENCH.ACTIVE_SOURCE_PREFLIGHT_TIMEOUT_SECONDS,
            LINKED.SETTLED_WINDOW_SECONDS,
        )

    def test_formal_bracket_fits_inside_one_trim_gap(self):
        for mode in ("pcm", "ac3"):
            with self.subTest(mode=mode):
                projection = BENCH.formal_bracket_projection(
                    mode,
                    BENCH.FORMAL_WARMUP_FRAMES,
                    BENCH.mode_probe_gap(mode),
                    BENCH.TAIL_FRAMES,
                )

                self.assertTrue(projection["fits"])
                self.assertLess(
                    projection["bracket_seconds"],
                    LINKED.TRIM_RATE_LIMIT_SECONDS,
                )
                self.assertEqual(
                    projection["budget_seconds"],
                    LINKED.TRIM_RATE_LIMIT_SECONDS
                    - BENCH.ACTIVE_SOURCE_PREFLIGHT_POLL_SECONDS
                    - BENCH.STATUS_STALENESS_ALLOWANCE_SECONDS,
                )
                self.assertAlmostEqual(
                    projection["bracket_seconds"],
                    BENCH.FORMAL_ALSA_SETUP_ALLOWANCE_SECONDS
                    + 2 * BENCH.FORMAL_STATUS_FETCH_ALLOWANCE_SECONDS
                    + projection["stimulus_frames"] / BENCH.RATE,
                )

    def test_oversized_warmup_is_rejected_before_touching_the_device(self):
        args = BENCH.build_parser().parse_args([
            "--mode", "pcm", "--output", "out",
            "--warmup-frames", str(10 * BENCH.RATE),
        ])

        with self.assertRaisesRegex(ValueError, "trim rate limit"):
            BENCH.validate_arguments(args)

    def test_oversized_tail_is_rejected_before_touching_the_device(self):
        args = BENCH.build_parser().parse_args([
            "--mode", "ac3", "--output", "out",
            "--tail-frames", str(8 * BENCH.RATE),
        ])

        with self.assertRaisesRegex(ValueError, "trim rate limit"):
            BENCH.validate_arguments(args)

    def test_default_arguments_pass_validation_for_both_modes(self):
        for mode in ("pcm", "ac3"):
            with self.subTest(mode=mode):
                args = BENCH.build_parser().parse_args(
                    ["--mode", mode, "--output", "out"]
                )

                projection = BENCH.validate_arguments(args)

                self.assertTrue(projection["fits"])

    def test_ac3_bracket_counts_whole_iec_access_units(self):
        frames = BENCH.stimulus_frames(
            "ac3",
            BENCH.FORMAL_WARMUP_FRAMES,
            BENCH.AC3_PROBE_GAP_FRAMES,
            BENCH.TAIL_FRAMES,
        )

        self.assertEqual(frames % BENCH.AC3_IEC_PERIOD_FRAMES, 0)
        self.assertGreaterEqual(
            frames,
            BENCH.FORMAL_WARMUP_FRAMES
            + BENCH.CODE_COUNT
            * (BENCH.AC3_CODE_FRAMES + BENCH.AC3_PROBE_GAP_FRAMES)
            + BENCH.TAIL_FRAMES,
        )

    @staticmethod
    def accepted_manifest(mode="pcm"):
        """An accepted evidence manifest at the current contract version."""
        manifest = BENCH.make_preflight(
            mode=mode,
            device="hw:test,0",
            status_url="http://device/api/status",
            config_url="http://device/api/dsp/config",
            renderer=pathlib.Path("/renderer"),
            period=256,
            buffer_size=4096,
            warmup_frames=48000,
            status_delay_seconds=0.5,
            probe_gap_frames=BENCH.mode_probe_gap(mode),
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
                "harness_dirty": False,
                "harness_dirty_files": [],
            },
            started_utc="2026-07-31T00:00:00Z",
        )
        code_frames, _ = BENCH.mode_geometry(mode)
        manifest["state"] = "accepted"
        manifest["generation"] = {
            "code_count": 24,
            "code_frames": code_frames,
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
                "active_source_preflight",
                "summary",
            )
        }
        manifest["active_source_preflight"] = {
            "accepted": True,
            "mode": "edge",
            "elapsed_seconds": 11.937,
            "xrun_recoveries": 0,
            "accepted_status": {},
        }
        manifest["status_initial"] = {}
        manifest["status_final"] = {}
        manifest["collector_acceptance"] = {}
        return manifest

    def test_evidence_manifest_rejects_schema_contract_and_paths(self):
        manifest = self.accepted_manifest()
        BENCH.validate_evidence_manifest(manifest, "pcm")

        mutations = {
            "schema": lambda value: value.update(schema="unknown"),
            "contract": lambda value: value["acceptance"].update(
                minimum_correlation=0.34
            ),
            "path": lambda value: value["artifacts"]["carrier"].update(
                path="../carrier.bin"
            ),
            "preflight-missing": lambda value: value.pop(
                "active_source_preflight"
            ),
            "preflight-mode": lambda value: value[
                "active_source_preflight"
            ].update(mode="guessed"),
            "preflight-artifact": lambda value: value["artifacts"].pop(
                "active_source_preflight"
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
        return device_status()


class RepositoryIdentityTests(unittest.TestCase):
    """The manifest's provenance claims, driven against a scripted git.

    Never invokes real git: the checks must be provable from their exit codes
    on a tree the test controls, including the dirty one that refuses
    collection.
    """

    HEAD = "c" * 40

    def git(self, phase1=0, evidence=0, dirty="", head=0):
        def run(argv, **kwargs):
            if argv[:2] == ["git", "rev-parse"]:
                return subprocess.CompletedProcess(
                    argv, head, stdout=f"{self.HEAD}\n", stderr=""
                )
            if argv[:3] == ["git", "merge-base", "--is-ancestor"]:
                if argv[3] == BENCH.PHASE1_IMAGE_REVISION:
                    return subprocess.CompletedProcess(argv, phase1)
                if argv[3] == BENCH.SAMPLE_RATE_EVIDENCE_REVISION:
                    return subprocess.CompletedProcess(argv, evidence)
            if argv[:3] == ["git", "status", "--porcelain"]:
                self.assertEqual(argv[3], "--")
                self.assertEqual(argv[4:], list(BENCH.HARNESS_PATHS))
                return subprocess.CompletedProcess(
                    argv, 0, stdout=dirty, stderr=""
                )
            raise AssertionError(f"unexpected git command: {argv}")

        return mock.patch.object(BENCH.subprocess, "run", run)

    def test_identity_derives_every_claim_from_a_check_result(self):
        with self.git():
            identity = BENCH.repository_identity()

        self.assertEqual(identity["harness_revision"], self.HEAD)
        self.assertTrue(identity["phase1_image_is_ancestor"])
        self.assertTrue(identity["sample_rate_evidence_is_ancestor"])
        self.assertFalse(identity["harness_dirty"])
        self.assertEqual(identity["harness_dirty_files"], [])
        self.assertEqual(
            identity["harness_paths"], list(BENCH.HARNESS_PATHS)
        )
        self.assertEqual(
            identity["expected_build_revision"],
            f"{BENCH.PHASE1_IMAGE_REVISION[:12]}+teensy41-optical",
        )
        BENCH.require_clean_harness(identity)

    def test_identity_fails_closed_on_every_failing_check(self):
        cases = {
            "head": ({"head": 128}, "harness revision"),
            "phase1": ({"phase1": 1}, "Phase 1 image"),
            "evidence": ({"evidence": 1}, "sample-rate evidence"),
        }
        for label, (kwargs, needle) in cases.items():
            with self.subTest(label=label):
                with self.git(**kwargs):
                    with self.assertRaisesRegex(RuntimeError, needle):
                        BENCH.repository_identity()

    def test_dirty_harness_is_recorded_and_refuses_collection(self):
        porcelain = (
            " M firmware/tools/latency/bench.py\n"
            "?? firmware/test/latency-runner/extra.py\n"
        )
        with self.git(dirty=porcelain):
            identity = BENCH.repository_identity()

        self.assertTrue(identity["harness_dirty"])
        self.assertEqual(
            identity["harness_dirty_files"],
            [
                "firmware/tools/latency/bench.py",
                "firmware/test/latency-runner/extra.py",
            ],
        )
        with self.assertRaisesRegex(RuntimeError, "refusing formal"):
            BENCH.require_clean_harness(identity)


class ActiveSourceGateTests(unittest.TestCase):
    """Preflight acceptance: cumulative trim edge, or a settled window."""

    def gate(self, minimum_seconds=3.0, follow_millihz=1000):
        return LINKED.ActiveSourceGate(
            minimum_seconds,
            follow_millihz,
            LINKED.SETTLED_WINDOW_SECONDS,
        )

    def test_straddled_edge_accepts_across_separate_polls(self):
        gate = self.gate()

        self.assertEqual(
            gate.observe(active_source_status(5, 6), 3.0),
            LINKED.ACTIVE_SOURCE_CONTINUE,
        )
        self.assertEqual(
            gate.observe(active_source_status(6, 6), 3.25),
            LINKED.ACTIVE_SOURCE_CONTINUE,
        )
        self.assertEqual(
            gate.observe(active_source_status(6, 7), 3.5),
            LINKED.ACTIVE_SOURCE_CONFIRM,
        )
        self.assertTrue(
            gate.confirm(active_source_status(6, 7), 3.75)
        )

    def test_edge_is_not_accepted_before_the_minimum_hold(self):
        gate = self.gate()
        gate.observe(active_source_status(5, 6), 0.0)

        self.assertEqual(
            gate.observe(active_source_status(6, 7), 1.0),
            LINKED.ACTIVE_SOURCE_CONTINUE,
        )
        self.assertEqual(
            gate.observe(active_source_status(6, 7), 3.0),
            LINKED.ACTIVE_SOURCE_CONFIRM,
        )

    def test_trim_burst_rebaselines_and_a_later_edge_accepts(self):
        gate = self.gate()
        gate.observe(active_source_status(5, 5), 3.0)

        self.assertEqual(
            gate.observe(active_source_status(7, 7), 3.25),
            LINKED.ACTIVE_SOURCE_CONTINUE,
        )
        self.assertEqual(gate.baseline, (7, 7))
        self.assertEqual(
            gate.observe(active_source_status(8, 8), 3.5),
            LINKED.ACTIVE_SOURCE_CONFIRM,
        )
        self.assertTrue(gate.confirm(active_source_status(8, 8), 3.75))

    def test_counter_wrap_is_read_as_a_single_step(self):
        gate = self.gate()
        gate.observe(
            active_source_status(0xFFFFFFFF, 0xFFFFFFFF), 3.0
        )

        self.assertEqual(
            gate.observe(active_source_status(0, 0), 3.25),
            LINKED.ACTIVE_SOURCE_CONFIRM,
        )

    def test_confirm_snapshot_rejects_then_a_later_edge_accepts(self):
        gate = self.gate()
        gate.observe(active_source_status(5, 5), 3.0)
        self.assertEqual(
            gate.observe(active_source_status(6, 6), 3.25),
            LINKED.ACTIVE_SOURCE_CONFIRM,
        )

        # Clean edge, but the follow error has not converged yet: keep the
        # confirmed counters as the baseline and wait for the next trim.
        self.assertFalse(
            gate.confirm(active_source_status(6, 6, 3500), 3.5)
        )
        self.assertEqual(gate.baseline, (6, 6))
        self.assertEqual(
            gate.observe(active_source_status(7, 7), 13.5),
            LINKED.ACTIVE_SOURCE_CONFIRM,
        )
        self.assertTrue(gate.confirm(active_source_status(7, 7), 13.75))

    def test_confirm_snapshot_rebaselines_when_the_counters_move_again(self):
        gate = self.gate()
        gate.observe(active_source_status(5, 5), 3.0)
        gate.observe(active_source_status(6, 6), 3.25)

        self.assertFalse(gate.confirm(active_source_status(7, 6), 3.5))
        self.assertEqual(gate.baseline, (7, 6))

    def test_confirm_snapshot_rejects_an_unhealthy_device(self):
        gate = self.gate()
        gate.observe(active_source_status(5, 5), 3.0)
        gate.observe(active_source_status(6, 6), 3.25)
        unhealthy = active_source_status(6, 6)
        unhealthy["carrier"] = 0

        self.assertFalse(gate.confirm(unhealthy, 3.5))

    def test_settled_window_accepts_a_converged_device(self):
        gate = self.gate()
        elapsed = 0.0
        while elapsed < LINKED.SETTLED_WINDOW_SECONDS:
            self.assertEqual(
                gate.observe(active_source_status(4, 4), elapsed),
                LINKED.ACTIVE_SOURCE_CONTINUE,
            )
            elapsed += 0.25

        self.assertEqual(
            gate.observe(active_source_status(4, 4), elapsed),
            LINKED.ACTIVE_SOURCE_MODE_SETTLED,
        )

    def test_settled_window_restarts_on_an_out_of_band_follow_error(self):
        gate = self.gate()
        elapsed = 0.0
        while elapsed < 10.0:
            gate.observe(active_source_status(4, 4), elapsed)
            elapsed += 0.25
        gate.observe(active_source_status(4, 4, 3500), elapsed)
        elapsed += 0.25
        restarted = elapsed

        while elapsed < restarted + LINKED.SETTLED_WINDOW_SECONDS:
            self.assertEqual(
                gate.observe(active_source_status(4, 4), elapsed),
                LINKED.ACTIVE_SOURCE_CONTINUE,
                f"accepted {elapsed - restarted:.2f}s into the window",
            )
            elapsed += 0.25

        self.assertEqual(
            gate.observe(active_source_status(4, 4), elapsed),
            LINKED.ACTIVE_SOURCE_MODE_SETTLED,
        )

    def test_settled_window_restarts_on_counter_movement(self):
        gate = self.gate()
        elapsed = 0.0
        while elapsed < 14.0:
            gate.observe(active_source_status(4, 4), elapsed)
            elapsed += 0.25
        gate.observe(active_source_status(4, 5), elapsed)
        elapsed += 0.25

        self.assertEqual(
            gate.observe(active_source_status(4, 5), elapsed + 5.0),
            LINKED.ACTIVE_SOURCE_CONTINUE,
        )

    def test_settled_window_ignores_an_unhealthy_poll(self):
        gate = self.gate()
        unhealthy = active_source_status(4, 4)
        unhealthy["pipeline_diag"]["last_status"] = 3
        elapsed = 0.0
        while elapsed < 14.0:
            gate.observe(active_source_status(4, 4), elapsed)
            elapsed += 0.25
        gate.observe(unhealthy, elapsed)

        self.assertEqual(
            gate.observe(active_source_status(4, 4), elapsed + 1.0),
            LINKED.ACTIVE_SOURCE_CONTINUE,
        )

    def test_health_predicate_names_every_required_condition(self):
        self.assertTrue(
            LINKED.active_source_required_health(active_source_status())
        )
        mutations = {
            "carrier": lambda status: status.update(carrier=0),
            "locked": lambda status: status["clock"].update(
                carrier_locked=False
            ),
            "ready": lambda status: status["clock"].update(
                carrier_ready=False
            ),
            "master": lambda status: status["clock"].update(
                master="internal"
            ),
            "state": lambda status: status["clock"].update(
                state="acquiring"
            ),
            "nominal": lambda status: status["clock"].update(
                nominal_rate_hz=44100
            ),
            "stream": lambda status: status["clock"].update(
                stream_rate_hz=44100
            ),
            "class": lambda status: status["pipeline_diag"].update(
                **{"class": 2}
            ),
            "pcm": lambda status: status["pipeline_diag"].update(
                pcm_confirmed=0
            ),
            "last-status": lambda status: status["pipeline_diag"].update(
                last_status=3
            ),
            "pipeline-failure": lambda status: status["audio_diag"].update(
                pipeline_last_failure=1
            ),
            "service-failure": lambda status: status["audio_diag"].update(
                service_last_failure=1
            ),
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label):
                status = active_source_status()
                mutate(status)
                self.assertFalse(
                    LINKED.active_source_required_health(status)
                )

    def test_projection_records_every_field_acceptance_reads(self):
        status = active_source_status(11, 12, -750)
        status["pipeline_diag"]["last_status"] = 3

        projection = LINKED.active_source_projection(status, 4.5)

        self.assertEqual(projection["phase"], "poll")
        self.assertEqual(projection["elapsed_seconds"], 4.5)
        self.assertFalse(projection["healthy"])
        self.assertEqual(projection["trim_updates"], 11)
        self.assertEqual(projection["invalid_windows"], 12)
        self.assertEqual(projection["follow_error_millihz"], 750)
        self.assertEqual(projection["pipeline_last_status"], 3)
        self.assertGreater(projection["poll_unix_seconds"], 0)
        for field, expected in (
            ("carrier", 1),
            ("carrier_locked", True),
            ("carrier_ready", True),
            ("master", "input"),
            ("state", "tracking"),
            ("nominal_rate_hz", 48000),
            ("stream_rate_hz", 48000),
            ("class", 1),
            ("pcm_confirmed", 1),
            ("pipeline_last_failure", 0),
            ("service_last_failure", 0),
            ("carrier_rate_millihz", 48000000),
            ("output_rate_millihz", 47999250),
        ):
            with self.subTest(field=field):
                self.assertEqual(projection[field], expected)

    def test_projection_survives_a_truncated_status(self):
        projection = LINKED.active_source_projection({}, 1.0)

        self.assertFalse(projection["healthy"])
        self.assertIsNone(projection["trim_updates"])
        self.assertIsNone(projection["follow_error_millihz"])


class ActiveSourcePreflightTests(unittest.TestCase):
    """The preflight loop itself, on a virtual clock with no libasound."""

    def preflight(
        self,
        statuses,
        clock=None,
        alsa=None,
        timeout_seconds=75.0,
        minimum_seconds=3.0,
        fetch_seconds=0.01,
        artifact_path="/evidence/active-source-preflight.json",
        **kwargs,
    ):
        clock = VirtualClock() if clock is None else clock
        alsa = FakeAlsa(clock) if alsa is None else alsa
        feed = FakeStatusFeed(clock, statuses, fetch_seconds)
        manifest = LINKED.preflight_active_source(
            "hw:test,0",
            48000,
            256,
            4096,
            "http://device/api/status",
            minimum_seconds,
            timeout_seconds,
            0.25,
            1000,
            artifact_path=artifact_path,
            alsa=alsa,
            fetch_status=feed,
            monotonic=clock.monotonic,
            **kwargs,
        )
        return manifest, alsa, feed

    def test_straddled_edge_is_accepted_and_records_its_mode(self):
        manifest, alsa, _ = self.preflight([
            active_source_status(5, 5),
            active_source_status(6, 5),
            active_source_status(6, 6),
        ])

        self.assertEqual(manifest["state"], "accepted")
        self.assertEqual(manifest["mode"], "edge")
        self.assertGreaterEqual(manifest["elapsed_seconds"], 3.0)
        self.assertEqual(manifest["xrun_recoveries"], 0)
        self.assertEqual(
            manifest["accepted_status"]["clock"]["trim_updates"], 6
        )
        self.assertEqual(
            [
                observation["phase"]
                for observation in manifest["observations"][-2:]
            ],
            ["poll", "confirm"],
        )
        self.assertEqual(alsa.drops, 1)
        self.assertEqual(alsa.closes, 1)

    def test_settled_device_is_accepted_without_any_edge(self):
        manifest, _, feed = self.preflight(
            [active_source_status(4, 4)], timeout_seconds=40.0
        )

        self.assertEqual(manifest["mode"], "settled")
        self.assertGreaterEqual(
            manifest["elapsed_seconds"], LINKED.SETTLED_WINDOW_SECONDS
        )
        self.assertLess(
            manifest["elapsed_seconds"],
            LINKED.SETTLED_WINDOW_SECONDS + 1.0,
        )
        self.assertEqual(
            manifest["settled_window_seconds"],
            LINKED.SETTLED_WINDOW_SECONDS,
        )
        self.assertTrue(
            all(url.endswith("/api/status") for url in feed.urls)
        )

    def test_out_of_band_follow_error_times_out_with_the_artifact_path(self):
        with self.assertRaises(LINKED.AlsaError) as raised:
            self.preflight(
                [active_source_status(4, 4, 5000)],
                timeout_seconds=20.0,
                artifact_path="/evidence/run/active-source-preflight.json",
            )

        message = str(raised.exception)
        self.assertIn(
            "/evidence/run/active-source-preflight.json", message
        )
        self.assertIn("timed out", message)
        manifest = raised.exception.aius_active_source_manifest
        self.assertEqual(manifest["state"], "rejected")
        self.assertTrue(manifest["observations"])
        for observation in manifest["observations"]:
            self.assertTrue(observation["healthy"])
            self.assertEqual(observation["follow_error_millihz"], 5000)

    def test_unhealthy_device_times_out_and_names_the_blocking_field(self):
        blocked = active_source_status(4, 4)
        blocked["clock"]["state"] = "acquiring"

        with self.assertRaises(LINKED.AlsaError) as raised:
            self.preflight([blocked], timeout_seconds=20.0)

        manifest = raised.exception.aius_active_source_manifest
        self.assertFalse(manifest["observations"][-1]["healthy"])
        self.assertEqual(manifest["observations"][-1]["state"], "acquiring")

    def test_deadline_is_rechecked_after_every_blocking_fetch(self):
        # get_json blocks for up to its 5 s timeout. The deadline is crossed
        # inside the second fetch, so no third fetch and no further feed may
        # run: one blocking call is the whole overshoot.
        clock = VirtualClock()
        alsa = FakeAlsa(clock)
        feed = FakeStatusFeed(clock, [active_source_status(4, 4)], 5.0)

        with self.assertRaisesRegex(LINKED.AlsaError, "timed out"):
            LINKED.preflight_active_source(
                "hw:test,0",
                48000,
                256,
                4096,
                "http://device/api/status",
                3.0,
                6.0,
                0.25,
                1000,
                artifact_path="/evidence/active-source-preflight.json",
                alsa=alsa,
                fetch_status=feed,
                monotonic=clock.monotonic,
            )

        self.assertEqual(len(feed.urls), 2)
        self.assertLess(clock.monotonic(), 6.0 + 5.0 + 0.25)

    def test_preflight_requires_a_status_url(self):
        clock = VirtualClock()
        with self.assertRaisesRegex(LINKED.AlsaError, "requires a status"):
            LINKED.preflight_active_source(
                "hw:test,0",
                48000,
                256,
                4096,
                None,
                3.0,
                75.0,
                0.25,
                1000,
                alsa=FakeAlsa(clock),
                monotonic=clock.monotonic,
            )

    def test_preflight_asks_for_a_second_of_ring_and_keeps_what_is_granted(
        self
    ):
        clock = VirtualClock()
        alsa = FakeAlsa(clock, granted_period=512, granted_buffer=16384)

        manifest, _, _ = self.preflight(
            [active_source_status(4, 4)],
            clock=clock,
            alsa=alsa,
            timeout_seconds=40.0,
        )

        request = alsa.configured[0]
        self.assertEqual(request["buffer_size"], 48000)
        self.assertFalse(request["exact"])
        self.assertEqual(manifest["requested_buffer_frames"], 48000)
        self.assertEqual(manifest["playback"]["buffer_frames"], 16384)
        self.assertEqual(manifest["playback"]["period_frames"], 512)

    def test_xrun_is_recovered_counted_and_bounded(self):
        clock = VirtualClock()
        alsa = FakeAlsa(
            clock,
            xrun_faults=LINKED.MAX_PREFLIGHT_XRUN_RECOVERIES,
        )

        manifest, _, _ = self.preflight(
            [active_source_status(4, 4)],
            clock=clock,
            alsa=alsa,
            timeout_seconds=40.0,
        )

        self.assertEqual(
            manifest["xrun_recoveries"],
            LINKED.MAX_PREFLIGHT_XRUN_RECOVERIES,
        )
        self.assertEqual(
            manifest["maximum_xrun_recoveries"],
            LINKED.MAX_PREFLIGHT_XRUN_RECOVERIES,
        )
        self.assertEqual(
            alsa.prepared, LINKED.MAX_PREFLIGHT_XRUN_RECOVERIES + 1
        )
        self.assertEqual(
            alsa.starts, LINKED.MAX_PREFLIGHT_XRUN_RECOVERIES + 1
        )
        self.assertEqual(manifest["mode"], "settled")

    def test_xrun_recoveries_above_the_bound_fail_the_preflight(self):
        clock = VirtualClock()
        alsa = FakeAlsa(
            clock,
            xrun_faults=LINKED.MAX_PREFLIGHT_XRUN_RECOVERIES + 1,
        )

        with self.assertRaisesRegex(LINKED.AlsaError, "underran"):
            self.preflight(
                [active_source_status(4, 4)],
                clock=clock,
                alsa=alsa,
                timeout_seconds=40.0,
            )

        self.assertEqual(alsa.drops, 1)
        self.assertEqual(alsa.closes, 1)

    def test_short_transfer_still_fails_the_preflight(self):
        class ShortFeedAlsa(FakeAlsa):
            def snd_pcm_writei(self, handle, pointer, frames):
                if frames == self.granted_period:
                    self.clock.advance(frames / self.rate)
                    return frames - 1
                return frames

        clock = VirtualClock()
        with self.assertRaisesRegex(LINKED.AlsaError, "short active-source"):
            self.preflight(
                [active_source_status(4, 4)],
                clock=clock,
                alsa=ShortFeedAlsa(clock),
            )

    def test_playback_must_still_be_running_at_acceptance(self):
        clock = VirtualClock()
        alsa = FakeAlsa(clock, state=LINKED.SND_PCM_STATE_RUNNING + 1)

        with self.assertRaisesRegex(LINKED.AlsaError, "not running"):
            self.preflight(
                [active_source_status(4, 4)],
                clock=clock,
                alsa=alsa,
                timeout_seconds=40.0,
            )


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
