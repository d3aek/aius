import contextlib
import importlib.util
import io
import json
import pathlib
import socket
import sys
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


def transition_fixture(with_replay=True):
    """The transitions stimulus as status samples + events.

    `with_replay=False` is the aplay-driven stimulus that never cuts the
    carrier: three phases, 48 -> 44.1 -> 48, and no loss->replay leg. Both
    shapes carry the same dark-gap artifacts (one unsupported episode and
    one underrun burst per rate change), so the no-replay fixture is what
    the analyzer must accept against `expected_phases` of three labels."""
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
    ]
    if with_replay:
        phases.append((48000, True))
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
                  external=None, attempts=None, events=None):
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
    # The run's events ride on the first sample; load_evidence keys them by
    # sequence, so one carrier record is the whole event list.
    all_events = json.dumps(
        {"cursor": 0, "next": len(events or []), "events": events or []},
        separators=(",", ":"))
    for index, entry in enumerate(samples):
        record = {
            "kind": "sample",
            "elapsed_s": entry["elapsed_s"],
            "raw_status_json": json.dumps(
                entry["status"], separators=(",", ":")),
            "raw_events_json": all_events if index == 0 else empty_events,
        }
        # attempts=None writes the pre-retry-counter evidence shape.
        if attempts is not None:
            record["attempts"] = attempts[index]
        records.append(record)
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
        self.assertTrue(result["metrics"]["trim_cadence_advisory"])
        self.assertNotIn("trim-cadence",
                         {failure["code"] for failure in result["failures"]})

    def test_trim_cadence_is_advisory_with_52_post_settle_commits(self):
        # Owner ruling D4 (b): ~52 commits on a healthy 10 min run.
        samples = [sample(0, output_blocks=1), sample(10, output_blocks=2),
                   sample(20, output_blocks=3)]
        for index, item in enumerate(samples):
            clock = item["status"]["clock"]
            clock["trim_updates"] = 100 + 52 * index
            clock["centre_writes"] = 4
            clock["structural_replans"] = 0
        result = analyze_48(samples, settle_s=10)
        self.assertEqual(result["verdict"], "pass", result["failures"])
        self.assertEqual(result["metrics"]["centre_commits_after_settle"], 52)
        self.assertEqual(result["metrics"]["centre_writes_after_settle"], 0)
        self.assertTrue(result["metrics"]["trim_cadence_advisory"])

    def test_trim_cadence_excludes_structural_replans(self):
        samples = [sample(0, output_blocks=1), sample(10, output_blocks=2),
                   sample(20, output_blocks=3)]
        for index, item in enumerate(samples):
            clock = item["status"]["clock"]
            clock["trim_updates"] = 100 + 5 * index
            clock["centre_writes"] = 4
            clock["structural_replans"] = 5 * index
        result = analyze_48(samples, settle_s=10)
        self.assertEqual(result["metrics"]["centre_commits_after_settle"], 0)
        self.assertNotIn("trim-cadence",
                         {failure["code"] for failure in result["failures"]})

    def test_stamp_change_count_skips_a_sample_missing_the_stamp(self):
        samples = [sample(0, output_blocks=1), sample(1, output_blocks=2),
                   sample(2, output_blocks=3), sample(3, output_blocks=4)]
        stamps = [100, 200, None, 300]
        for item, stamp in zip(samples, stamps):
            if stamp is not None:
                item["status"]["clock"]["fill_applied_at_ms"] = stamp
        self.assertEqual(
            BENCH.stamp_change_count(samples, "clock.fill_applied_at_ms"), 1)
        self.assertIsNone(
            BENCH.stamp_change_count(samples, "clock.trim_applied_at_ms"))

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

    def test_transitions_accept_the_declared_no_replay_sequence(self):
        # F13 + expected_phases: the aplay stimulus drives 48 -> 44.1 -> 48
        # and never cuts the carrier, so there is no replay leg. Both
        # transition windows are live, so the dark unsupported episode of
        # the SECOND one is attributed rather than reported unattributed.
        samples, events = transition_fixture(with_replay=False)
        result = BENCH.analyze_samples(samples, events, "transitions", {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "phase_samples": 2,
            "max_follow_error_millihz": 1000,
            "expected_phases": ["48000", "44100", "48000"],
        }, {"old_rate_output_frames": 0})
        self.assertEqual(result["verdict"], "pass", result["failures"])
        self.assertEqual(result["metrics"]["observed_phase_sequence"],
                         ["48000", "44100", "48000"])
        self.assertEqual(result["metrics"]["transition_windows"],
                         [(1, 3), (4, 6)])
        self.assertEqual(result["metrics"]["live_transition_windows"],
                         [(1, 3), (4, 6)])
        self.assertEqual(
            result["metrics"]["live_transition_rate_change_deltas"], [1, 1])
        self.assertEqual(
            result["metrics"]["transition_reset_deltas"], [1, 1])
        self.assertEqual(
            result["metrics"]["counter_deltas"][
                "clock.unsupported_episodes"], 2)

    def test_transitions_no_replay_run_still_fails_the_default_sequence(self):
        # Without the option the analyzer keeps asserting the replay leg, so
        # the same run is a sequence failure: the default is unchanged.
        samples, events = transition_fixture(with_replay=False)
        result = BENCH.analyze_samples(samples, events, "transitions", {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "phase_samples": 2,
            "max_follow_error_millihz": 1000,
        }, {"old_rate_output_frames": 0})
        self.assertEqual(result["verdict"], "fail")
        self.assertIn("transition-sequence",
                      {failure["code"] for failure in result["failures"]})
        self.assertEqual(result["metrics"]["expected_phase_sequence"],
                         ["48000", "44100", "48000", "replay"])

    def test_transitions_reject_unsupported_episode_before_replay(self):
        # The window leading INTO replay is not a live rate transition, so a
        # dark unsupported episode there is still unattributed -- the F13 fix
        # widens attribution to real live windows only.
        samples, events = transition_fixture()
        for entry in samples[8:]:
            entry["status"]["clock"]["unsupported_episodes"] += 1
        events.append({
            "sequence": len(events) + 1,
            "uptime_ms": 8000,
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
        codes = {failure["code"] for failure in result["failures"]}
        self.assertIn("transition-unattributed-unsupported", codes)
        self.assertNotIn("transition-open-gate-unsupported", codes)
        self.assertEqual(result["metrics"]["live_transition_windows"],
                         [(1, 3), (4, 6)])

    def test_transitions_report_event_overflows_without_gating_them(self):
        # Plan defect F8: an event_overflows-only harmful-counter failure is
        # the capture tool's poll cadence, not a board fault. Reported in
        # transitions, harmful everywhere else.
        samples, events = transition_fixture(with_replay=False)
        for entry in samples[3:]:
            entry["status"]["event_overflows"] = 10
        result = BENCH.analyze_samples(samples, events, "transitions", {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "phase_samples": 2,
            "max_follow_error_millihz": 1000,
            "expected_phases": ["48000", "44100", "48000"],
        }, {"old_rate_output_frames": 0})
        self.assertEqual(result["verdict"], "pass", result["failures"])
        self.assertEqual(result["metrics"]["event_overflows_delta"], 10)
        self.assertEqual(
            result["metrics"]["counter_deltas"]["event_overflows"], 10)

    def test_event_overflows_stay_harmful_outside_transitions(self):
        smoke = [
            sample(0, rate=48000, output_blocks=1),
            sample(60, rate=48000, output_blocks=2),
            sample(120, rate=48000, output_blocks=3),
        ]
        for entry in smoke:
            entry["status"]["pipeline_diag"]["pcm_confirmed"] = 1
        smoke[-1]["status"]["event_overflows"] = 10
        result = BENCH.analyze_samples(smoke, [], "pcm-48k-smoke", {
            "minimum_duration_s": 120,
            "settle_s": 30,
            "max_follow_error_millihz": 1000,
        })
        self.assertEqual(result["verdict"], "fail")
        self.assertIn(
            "event_overflows advanced by 10",
            {failure["message"] for failure in result["failures"]})
        self.assertNotIn("event_overflows_delta", result["metrics"])

        pcm = [
            sample(0, rate=44100, output_blocks=1),
            sample(1, rate=44100, output_blocks=2),
            sample(2, rate=44100, output_blocks=3),
        ]
        pcm[-1]["status"]["event_overflows"] = 4
        result = BENCH.analyze_samples(pcm, [], "pcm-120s", {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "rate_hz": 44100,
            "max_follow_error_millihz": 1000,
        }, {"audio_rate_hz": 44100, "continuous_output": True})
        self.assertEqual(result["verdict"], "fail")
        self.assertIn("harmful-counter",
                      {failure["code"] for failure in result["failures"]})

    def test_expected_phases_option_is_validated(self):
        samples, events = transition_fixture(with_replay=False)
        options = {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "phase_samples": 2,
            "max_follow_error_millihz": 1000,
        }
        rejected = (
            ["48000", "replay"],          # no live rate transition
            ["replay", "48000", "44100"],  # replay is not the final phase
            [],                            # empty
            ["48000", "96000"],            # label outside the supported set
        )
        for phases in rejected:
            result = BENCH.analyze_samples(
                samples, events, "transitions",
                {**options, "expected_phases": phases},
                {"old_rate_output_frames": 0})
            codes = {failure["code"] for failure in result["failures"]}
            self.assertIn("rate-option", codes, phases)
            # An unjudgeable option must not also claim a sequence verdict.
            self.assertNotIn("transition-sequence", codes, phases)

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

    def test_waveform_passes_when_declared_windows_are_not_visible(self):
        # Owner ruling 2026-09-02: the board may absorb its invalid windows
        # without any externally visible artifact. Twelve declared windows,
        # none captured, no discontinuity anywhere: pass.
        samples = [
            sample(0, output_blocks=1),
            sample(1, output_blocks=2),
            sample(2, output_blocks=3),
        ]
        samples[1]["status"]["clock"]["trim_updates"] = 6
        samples[2]["status"]["clock"]["trim_updates"] = 12
        samples[1]["status"]["clock"]["invalid_windows"] = 6
        samples[2]["status"]["clock"]["invalid_windows"] = 12
        external = {
            "capture_frame_gaps": 0,
            "discontinuities_outside_invalid": 0,
            "invalid_intervals_frames": [],
            "correlated_trim_updates": 0,
            "correlated_invalid_windows": 0,
        }
        result = BENCH.analyze_samples(samples, [], "waveform", {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "max_follow_error_millihz": 1000,
        }, external)
        self.assertEqual(result["verdict"], "pass", result["failures"])

    def test_waveform_rejects_more_captured_windows_than_declared(self):
        # The capture may see fewer windows than status declares, never more:
        # an undeclared artifact is the failure the gate exists to catch.
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
            "invalid_intervals_frames": [512, 512],
            "correlated_trim_updates": 2,
            "correlated_invalid_windows": 2,
        }
        result = BENCH.analyze_samples(samples, [], "waveform", {
            "minimum_duration_s": 0,
            "settle_s": 0,
            "max_follow_error_millihz": 1000,
        }, external)
        self.assertEqual(result["verdict"], "fail")
        codes = {failure["code"] for failure in result["failures"]}
        self.assertIn("trim-correlation", codes)
        self.assertIn("invalid-correlation", codes)

    def _fill_write_samples(self, fill_trims, window_seq, replans=None):
        # A fill write moves fill_trims and its stamp, never invalid_windows.
        samples = [sample(0, output_blocks=1), sample(1, output_blocks=2),
                   sample(2, output_blocks=3)]
        for index, item in enumerate(samples):
            clock = item["status"]["clock"]
            clock["trim_applied_at_ms"] = 100
            clock["fill_applied_at_ms"] = 100 if index == 0 else 900
            clock["fill_trims"] = fill_trims[index]
            clock["fill_window_seq"] = window_seq[index]
            clock["structural_replans"] = (replans or [0, 0, 0])[index]
            clock["invalid_windows"] = 0
        return samples

    def _run_waveform(self, samples):
        external = {
            "capture_frame_gaps": 0,
            "discontinuities_outside_invalid": 0,
            "invalid_intervals_frames": [],
            "correlated_trim_updates": 0,
            "correlated_invalid_windows": 0,
        }
        return BENCH.analyze_samples(samples, [], "waveform", {
            "minimum_duration_s": 0, "settle_s": 0,
            "max_follow_error_millihz": 1000,
        }, external)

    def test_waveform_sees_a_fill_write_through_the_applied_stamp(self):
        # trim_updates never moves: only the fill stamp does.
        result = self._run_waveform(
            self._fill_write_samples([0, 1, 1], [0, 2, 4]))
        self.assertEqual(result["verdict"], "pass", result["failures"])
        self.assertEqual(result["metrics"]["applied_write_delta"], 1)

    def test_waveform_fill_cadence_fails_on_more_fill_trims_than_windows(self):
        result = self._run_waveform(
            self._fill_write_samples([0, 3, 5], [0, 1, 2]))
        codes = {failure["code"] for failure in result["failures"]}
        self.assertIn("fill-cadence", codes)

    def test_waveform_fill_cadence_allows_one_write_for_a_pre_s0_window(self):
        result = self._run_waveform(
            self._fill_write_samples([0, 1, 3], [0, 1, 2]))
        codes = {failure["code"] for failure in result["failures"]}
        self.assertNotIn("fill-cadence", codes)

    def test_waveform_full_apply_without_invalid_window_fails(self):
        result = self._run_waveform(
            self._fill_write_samples([0, 1, 1], [0, 2, 4], [0, 1, 1]))
        codes = {failure["code"] for failure in result["failures"]}
        self.assertIn("invalid-window-count", codes)


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


class ExpectedPhasesCliTests(unittest.TestCase):
    def run_main(self, argv):
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), \
                contextlib.redirect_stderr(stderr):
            code = BENCH.main(argv)
        return code, stdout.getvalue(), stderr.getvalue()

    @staticmethod
    def _options(expected_phases=None):
        options = {
            "settle_s": 0,
            "phase_samples": 2,
            "max_follow_error_millihz": 1000,
        }
        if expected_phases is not None:
            options["expected_phases"] = expected_phases
        return options

    def _evidence(self, directory, expected_phases=None):
        samples, events = transition_fixture(with_replay=False)
        path = pathlib.Path(directory) / "transitions.jsonl"
        evidence_file(
            path, samples, scenario="transitions",
            analysis_options=self._options(expected_phases),
            external={"old_rate_output_frames": 0}, events=events)
        return path

    def test_analyze_reads_expected_phases_from_stored_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self._evidence(
                directory, ["48000", "44100", "48000"])
            code, stdout, stderr = self.run_main([
                "analyze", "transitions", "--evidence", str(path),
            ])
        self.assertEqual(code, 0, stderr)
        result = json.loads(stdout)
        self.assertEqual(result["verdict"], "pass", result["failures"])
        self.assertEqual(result["metrics"]["expected_phase_sequence"],
                         ["48000", "44100", "48000"])

    def test_analyze_without_stored_or_flagged_phases_keeps_the_default(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self._evidence(directory)
            code, stdout, stderr = self.run_main([
                "analyze", "transitions", "--evidence", str(path),
            ])
        self.assertEqual(code, 1, stderr)
        result = json.loads(stdout)
        self.assertIn("transition-sequence",
                      {failure["code"] for failure in result["failures"]})

    def test_expected_phases_flag_overrides_stored_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self._evidence(
                directory, ["48000", "44100", "48000", "replay"])
            code, stdout, stderr = self.run_main([
                "analyze", "transitions", "--evidence", str(path),
                "--expected-phases", "48000,44100,48000",
            ])
        self.assertEqual(code, 0, stderr)
        self.assertEqual(json.loads(stdout)["verdict"], "pass")


class _Response:
    def __init__(self, status, body, truncated=False):
        self.status = status
        self._body = body
        self._truncated = truncated

    def read(self):
        if self._truncated:
            # aius_http.c:605 promises a Content-Length on every response, so
            # a body cut short by a send-path stall arrives as an
            # IncompleteRead, not as a short read the caller could mistake
            # for valid JSON.
            raise BENCH.http.client.IncompleteRead(self._body[:4], 99)
        return self._body


class _Connection:
    """A scripted HTTPConnection. Outcomes: 'drop' (RemoteDisconnected from
    getresponse), 'timeout'/'reset' (socket.timeout/OSError from request),
    'truncated' (IncompleteRead from read), or a (status, body) pair."""

    def __init__(self, outcome, log):
        self._outcome = outcome
        self._log = log

    def request(self, *_args, **_kwargs):
        if self._outcome == "timeout":
            raise socket.timeout("timed out")
        if self._outcome == "reset":
            raise OSError(104, "Connection reset by peer")

    def getresponse(self):
        if self._outcome == "drop":
            raise BENCH.http.client.RemoteDisconnected("closed")
        if self._outcome == "truncated":
            return _Response(200, b'{"ok":true}', truncated=True)
        status, body = self._outcome
        return _Response(status, body)

    def close(self):
        self._log.append(("close",))


@contextlib.contextmanager
def scripted_transport(outcomes, clock=None):
    """Drive the real HttpTransport over a scripted connection factory.

    Records connection closes and backoff sleeps in one ordered log so the
    close-before-sleep ordering is observable. When `clock` is given, the
    fake monotonic clock advances by the socket timeout on each attempt and
    by each backoff sleep, so elapsed-time stamping is observable too.
    """
    calls = {"n": 0, "log": []}

    def factory(_host, _port, timeout=None):
        outcome = outcomes[calls["n"]]
        calls["n"] += 1
        if clock is not None:
            clock["now"] += timeout or 0.0
        return _Connection(outcome, calls["log"])

    def sleep(seconds):
        calls["log"].append(("sleep", seconds))
        if clock is not None:
            clock["now"] += seconds

    original_conn = BENCH.http.client.HTTPConnection
    original_sleep = BENCH.time.sleep
    original_monotonic = BENCH.time.monotonic
    BENCH.http.client.HTTPConnection = factory
    BENCH.time.sleep = sleep
    if clock is not None:
        BENCH.time.monotonic = lambda: clock["now"]
    try:
        yield calls
    finally:
        BENCH.http.client.HTTPConnection = original_conn
        BENCH.time.sleep = original_sleep
        BENCH.time.monotonic = original_monotonic


class TransportRetryTests(unittest.TestCase):
    """get_json retries transient transport drops (the device's
    one-client-at-a-time close, defect 41's 5 s send-path stall, and the
    truncated-body IncompleteRead its Content-Length promise produces) but
    never an HTTP or JSON error, which is real."""

    BODY = json.dumps({"ok": True}).encode("utf-8")

    def _transport(self):
        return BENCH.HttpTransport(("10.0.0.1", 80), timeout=0.75)

    def test_retries_transient_drops_then_succeeds(self):
        with scripted_transport(["drop", "drop", (200, self.BODY)]) as calls:
            result = self._transport().get_json("/api/status")
        self.assertEqual(result["body"], {"ok": True})
        self.assertEqual(calls["n"], 3)
        self.assertEqual(result["attempts"], 3)

    def test_retries_request_side_timeout_and_socket_error(self):
        with scripted_transport(
                ["timeout", "reset", (200, self.BODY)]) as calls:
            result = self._transport().get_json("/api/status")
        self.assertEqual(result["body"], {"ok": True})
        self.assertEqual(calls["n"], 3)
        self.assertEqual(result["attempts"], 3)

    def test_retries_incomplete_read_of_a_truncated_body(self):
        with scripted_transport(["truncated", (200, self.BODY)]) as calls:
            result = self._transport().get_json("/api/status")
        self.assertEqual(result["body"], {"ok": True})
        self.assertEqual(calls["n"], 2)
        self.assertEqual(result["attempts"], 2)

    def test_closes_the_connection_before_backing_off(self):
        # Holding the dead socket across a multi-second backoff would occupy
        # the board's single client slot for the whole wait.
        with scripted_transport(["drop", "drop", (200, self.BODY)]) as calls:
            self._transport().get_json("/api/status")
        self.assertEqual(calls["log"], [
            ("close",), ("sleep", BENCH.RETRY_BACKOFF_S[0]),
            ("close",), ("sleep", BENCH.RETRY_BACKOFF_S[1]),
            ("close",),
        ])

    def test_raises_after_exhausting_the_defect_41_budget(self):
        outcomes = ["drop"] * BENCH.DEFAULT_ATTEMPTS
        with scripted_transport(outcomes) as calls:
            with self.assertRaises(BENCH.BenchError):
                self._transport().get_json("/api/status")
        self.assertEqual(calls["n"], BENCH.DEFAULT_ATTEMPTS)
        sleeps = [entry[1] for entry in calls["log"] if entry[0] == "sleep"]
        self.assertEqual(sleeps, list(BENCH.RETRY_BACKOFF_S))

    def test_retry_budget_outlasts_the_five_second_stall(self):
        # Defect 41: the board holds a stalled transaction for the full
        # RESPONSE_TIMEOUT_MS (5 s). A budget shorter than that retries
        # entirely inside the stall.
        attempts = BENCH.DEFAULT_ATTEMPTS
        budget = sum(BENCH.RETRY_BACKOFF_S) + 0.75 * attempts
        self.assertGreater(budget, 5.0)
        self.assertLess(budget, 15.0)

    def test_does_not_retry_http_error(self):
        with scripted_transport([(503, b'{"error":"sd_busy"}')]) as calls:
            with self.assertRaises(BENCH.BenchError):
                self._transport().get_json("/api/status")
        self.assertEqual(calls["n"], 1)
        self.assertEqual(calls["log"], [("close",)])

    def test_retries_the_drop_but_not_the_http_error_behind_it(self):
        # Fails if the retry loop is deleted: without it the drop itself is
        # the raised error and the 503 is never reached.
        with scripted_transport(["drop", (503, b'{"error":"sd_busy"}')]) as calls:
            with self.assertRaises(BENCH.BenchError) as caught:
                self._transport().get_json("/api/status")
        self.assertEqual(calls["n"], 2)
        self.assertIn("HTTP 503", str(caught.exception))


class CollectOneTests(unittest.TestCase):
    """collect_one is the sample record's author: it counts attempts and
    stamps elapsed_s from the status RESPONSE, not from the request."""

    @staticmethod
    def _payloads():
        status_body = json.dumps(make_status(), separators=(",", ":"))
        events_body = json.dumps(
            {"cursor": 0, "next": 0, "events": []}, separators=(",", ":"))
        return status_body.encode("utf-8"), events_body.encode("utf-8")

    def test_drop_after_a_200_is_retried_and_counted(self):
        # The status GET already returned 200; the events GET drops. Both
        # GETs are read-only and offset-free, so the retry is idempotent and
        # the sample is still whole.
        status_bytes, events_bytes = self._payloads()
        outcomes = [(200, status_bytes), "drop", (200, events_bytes)]
        transport = BENCH.HttpTransport(("10.0.0.1", 80), timeout=0.75)
        with scripted_transport(outcomes) as calls:
            record = BENCH.collect_one(transport, 0, BENCH.time.monotonic())
        self.assertEqual(calls["n"], 3)
        self.assertEqual(record["status_attempts"], 1)
        self.assertEqual(record["events_attempts"], 2)
        self.assertEqual(record["attempts"], 3)
        self.assertEqual(record["status"]["boot_generation"], 17)
        self.assertEqual(record["events"], [])

    def test_elapsed_s_is_stamped_after_the_retried_status_fetch(self):
        status_bytes, events_bytes = self._payloads()
        outcomes = ["drop", (200, status_bytes), (200, events_bytes)]
        clock = {"now": 100.0}
        transport = BENCH.HttpTransport(("10.0.0.1", 80), timeout=0.75)
        with scripted_transport(outcomes, clock=clock):
            started = BENCH.time.monotonic()
            record = BENCH.collect_one(transport, 0, started)
        # 0.75 s timed-out attempt + 0.25 s backoff + 0.75 s of the attempt
        # that answered. A stamp taken before the fetch would read 0.0.
        self.assertGreaterEqual(record["elapsed_s"], 1.7)
        self.assertEqual(record["attempts"], 3)


class TransportEvidenceTests(unittest.TestCase):
    """The analysis reports the run's retry total and the board's own drop
    counter, and tolerates evidence written before either existed."""

    @staticmethod
    def _samples():
        return [
            sample(0, output_blocks=1),
            sample(1, output_blocks=2),
            sample(2, output_blocks=3),
        ]

    def test_reports_retry_total_from_sample_attempts(self):
        samples = self._samples()
        samples[0]["attempts"] = 2
        samples[1]["attempts"] = 5
        samples[2]["attempts"] = 2
        result = analyze_48(samples)
        self.assertEqual(result["verdict"], "pass")
        self.assertEqual(result["metrics"]["transport_retries"], 3)
        self.assertEqual(result["metrics"]["transport_retried_samples"], 1)
        self.assertEqual(result["metrics"]["samples_without_attempts"], 0)

    def test_old_evidence_without_attempts_is_unrecorded_not_clean(self):
        result = analyze_48(self._samples())
        self.assertEqual(result["verdict"], "pass")
        self.assertEqual(result["metrics"]["transport_retries"], 0)
        self.assertEqual(result["metrics"]["samples_without_attempts"], 3)
        self.assertIsNone(result["metrics"]["http_client_drops_delta"])

    def test_http_client_drops_is_reported_and_never_gated(self):
        samples = self._samples()
        for entry, drops in zip(samples, (5, 7, 7)):
            entry["status"]["http_client_drops"] = drops
        result = analyze_48(samples)
        # A retried drop cost the audio path nothing, so it must not fail the
        # run -- but it must be visible.
        self.assertEqual(result["verdict"], "pass")
        self.assertEqual(result["metrics"]["http_client_drops_delta"], 2)
        self.assertNotIn("http_client_drops", BENCH.COMMON_HARMFUL_COUNTERS)

    def test_load_evidence_round_trips_attempts(self):
        samples = self._samples()
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "evidence.jsonl"
            evidence_file(path, samples, attempts=[2, 4, 2])
            _metadata, loaded, _events = BENCH.load_evidence(path)
            self.assertEqual([entry["attempts"] for entry in loaded],
                             [2, 4, 2])
            result = BENCH.analyze_samples(loaded, [], "48k-10m", {
                "minimum_duration_s": 0,
                "settle_s": 0,
                "max_follow_error_millihz": 1000,
            })
            self.assertEqual(result["metrics"]["transport_retries"], 2)

            evidence_file(path, samples)
            _metadata, legacy, _events = BENCH.load_evidence(path)
            self.assertEqual([entry["attempts"] for entry in legacy],
                             [None, None, None])


if __name__ == "__main__":
    # The rate-runner arm of scripts/test.sh names this file only, so discover
    # every test_*.py beside it: test_recorder_pull.py runs in the same suite
    # without the runner having to name it. It is still a standalone entry
    # point (test/registry wants scripts/test.sh to name it too, which is a
    # change to a file this suite does not own); once the arm lists it, this
    # block can go back to a plain unittest.main().
    HERE = pathlib.Path(__file__).resolve().parent
    SUITE = unittest.TestLoader().discover(str(HERE), pattern="test_*.py")
    RESULT = unittest.TextTestRunner(verbosity=1).run(SUITE)
    sys.exit(0 if RESULT.wasSuccessful() else 1)
