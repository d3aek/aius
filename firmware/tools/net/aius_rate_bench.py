#!/usr/bin/env python3
"""Bounded, read-only Aius sample-rate acceptance runner and analyzer.

The device transport performs only GET /api/status and GET /api/events. Source
selection, carrier-rate changes, encoded fixture injection, capture and forced
trim remain bench/operator actions because the firmware exposes no control for
them. Raw response JSON is retained beside the parsed evidence.

Every sample record carries `attempts` (its total GET count, status plus
events) alongside the per-fetch `status_attempts` / `events_attempts`, so a
sample the transport had to retry is distinguishable from a clean one. The
analysis record reports `transport_retries`, `transport_retried_samples`,
`samples_without_attempts` and the board's own `http_client_drops_delta`.
"""

import argparse
import datetime
import http.client
import json
import math
import sys
import time
from pathlib import Path


SCHEMA_VERSION = 1
MAX_DURATION_S = 4 * 60 * 60
MIN_INTERVAL_S = 0.1
MAX_INTERVAL_S = 60.0
TX_CAPACITY_FRAMES = 2048
INGRESS_CAPACITY_WORDS = 8192
SUPPORTED_RATES = (32000, 44100, 48000)
# One sample is exactly two GETs (status, then events), so a clean sample
# records `attempts` == 2 and anything above that is a transport retry.
SAMPLE_REQUESTS = 2

SCENARIOS = {
    "48k-10m": {"duration": 600.0, "minimum": 600.0, "settle": 30.0},
    "pcm-48k-smoke": {"duration": 120.0, "minimum": 120.0, "settle": 30.0},
    "pcm-120s": {"duration": 120.0, "minimum": 120.0, "settle": 10.0},
    "transitions": {"duration": 300.0, "minimum": 0.0, "settle": 0.0},
    "negative": {"duration": 30.0, "minimum": 0.0, "settle": 5.0},
    "encoded": {"duration": 120.0, "minimum": 0.0, "settle": 10.0},
    "waveform": {"duration": 120.0, "minimum": 0.0, "settle": 10.0},
}

# Defect 41 (aius-network-hardening.md): the send-path stall that drops a
# response holds the transaction for the firmware's full RESPONSE_TIMEOUT_MS
# (5 s), and the 0.75 s client timeout expires long before the board gives up.
# A retry budget shorter than 5 s therefore spends every attempt INSIDE the
# same stall and still fails the run, so the wait has to come from the backoff
# rather than from the socket timeout: four sleeps of 0.25 / 1 / 2 / 3 s put
# the last attempt about 9.3 s after the first (0.75 s per timed-out attempt
# plus the sleeps), covering a 5 s stall with margin, and bound one GET at
# about 10 s in the worst case.
RETRY_BACKOFF_S = (0.25, 1.0, 2.0, 3.0)
DEFAULT_ATTEMPTS = len(RETRY_BACKOFF_S) + 1

# These are lifetime uint32_t diagnostics. Summing adjacent modulo deltas makes
# a run crossing UINT32_MAX unambiguous without mistaking a wrap for a reset.
#
# http_client_drops is deliberately NOT in this tuple. It counts the board side
# of the same defect-41 drop the transport retries through: the response was
# lost, not the audio. Nothing reached the DAC late or twice, so gating a
# 10-minute acceptance run on it would fail the run for a diagnostic the runner
# already recovered from. It is reported instead, as metrics.http_client_drops_
# delta beside metrics.transport_retries, so a run that limped is never read as
# a clean one.
COMMON_HARMFUL_COUNTERS = (
    "overruns",
    "event_overflows",
    "network_budget_overruns",
    "network_restarts",
    "http_service_overruns",
    "audio_diag.ingress_dropped_chunks",
    "audio_diag.ingress_dropped_words",
    "audio_diag.service_budget_overruns",
    "audio_diag.parse_rejects",
    "audio_diag.decode_rejects",
    "audio_diag.output_drops",
    "audio_diag.au_abandons",
    "audio_diag.runtime_epoch_budget_overruns",
    "clock.tx_underrun_frames",
    "clock.clock_faults",
)

CLOCK_FIELDS = (
    "carrier_locked",
    "carrier_ready",
    "carrier_rate_millihz",
    "nominal_rate_hz",
    "stream_rate_hz",
    "output_rate_millihz",
    "trim_ppm",
    "master",
    "state",
    "rate_changes",
    "trim_updates",
    "structural_replans",
    "invalid_windows",
    "clock_faults",
    "last_clock_fault",
    "mismatch_blocks",
    "unsupported_blocks",
    "unsupported_episodes",
    "tx_level_frames",
    "tx_underrun_frames",
    "ingress_level_words",
    "ingress_high_water_words",
)

AUDIO_FIELDS = (
    "ingress_generation",
    "ingress_dropped_chunks",
    "ingress_dropped_words",
    "service_generation_resets",
    "service_requested_resets",
    "service_budget_overruns",
    "parse_rejects",
    "decode_rejects",
    "output_drops",
    "au_abandons",
    "committed_output_blocks",
    "runtime_epoch_budget_overruns",
)


class BenchError(Exception):
    """An operational or evidence-format error."""


class HttpTransport:
    """Small transport boundary; analysis below has no network dependency."""

    def __init__(self, target, timeout=0.75):
        self.target = target
        self.timeout = timeout

    def get_json(self, path, attempts=DEFAULT_ATTEMPTS):
        # The device serves one HTTP client at a time and drops a connection
        # that arrives before the previous is fully released, and a momentary
        # send-path stall can leave a response unanswered until the transaction
        # times out (defect 41). Both surface as a transport error on ~0.5% of
        # polls (measured), and a 10-minute acceptance run must not abort on one.
        # Retry transient transport errors -- as aius-sd-sync.sh does for the
        # same reason -- but never retry an HTTP or JSON error, which is real.
        # The attempt count rides back on the result so a sample that limped is
        # never written as an identical record to one that did not.
        host, port = self.target
        raw_bytes = None
        status = None
        used = 0
        for attempt in range(attempts):
            used = attempt + 1
            connection = http.client.HTTPConnection(host, port, timeout=self.timeout)
            try:
                connection.request("GET", path, headers={"Connection": "close"})
                response = connection.getresponse()
                raw_bytes = response.read()
                status = response.status
                break
            except (OSError, http.client.HTTPException) as error:
                if used >= attempts:
                    raise BenchError(
                        f"GET {path}: {type(error).__name__}: {error} "
                        f"(after {attempts} attempts)") from error
            finally:
                # Release the stale socket BEFORE the backoff, not after it:
                # the board serves one client at a time, so holding a dead
                # connection across a multi-second wait is the very condition
                # that makes the next attempt drop too.
                connection.close()
            time.sleep(RETRY_BACKOFF_S[min(attempt, len(RETRY_BACKOFF_S) - 1)])
        try:
            raw = raw_bytes.decode("utf-8")
        except UnicodeDecodeError as error:
            raise BenchError(f"GET {path}: response is not UTF-8 JSON") from error
        if status != 200:
            raise BenchError(f"GET {path}: HTTP {status}: {raw[:160]}")
        try:
            body = json.loads(raw)
        except json.JSONDecodeError as error:
            raise BenchError(f"GET {path}: malformed JSON: {error}") from error
        if not isinstance(body, dict):
            raise BenchError(f"GET {path}: JSON root is not an object")
        return {"http_status": status, "raw": raw, "body": body,
                "attempts": used}


def parse_target(text):
    if not text:
        raise BenchError("target is empty")
    if ":" in text:
        host, port_text = text.rsplit(":", 1)
        if not host or not port_text.isdigit():
            raise BenchError("target must be HOST or HOST:PORT")
        port = int(port_text)
        if port < 1 or port > 65535:
            raise BenchError("target port must be in 1..65535")
        return host, port
    return text, 80


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(
        timespec="milliseconds").replace("+00:00", "Z")


def unsigned_delta(before, after, bits=32):
    """Return the modulo delta for an unsigned counter."""
    if isinstance(before, bool) or isinstance(after, bool):
        raise ValueError("counter values must be integers, not booleans")
    if not isinstance(before, int) or not isinstance(after, int):
        raise ValueError("counter values must be integers")
    limit = 1 << bits
    if before < 0 or before >= limit or after < 0 or after >= limit:
        raise ValueError(f"counter values must fit uint{bits}")
    return (after - before) % limit


def least_squares_slope(points):
    """Return the ordinary least-squares y-on-x slope."""
    if len(points) < 2:
        raise ValueError("at least two points are required")
    xs = [float(point[0]) for point in points]
    ys = [float(point[1]) for point in points]
    if not all(math.isfinite(value) for value in xs + ys):
        raise ValueError("points must be finite")
    mean_x = math.fsum(xs) / len(xs)
    mean_y = math.fsum(ys) / len(ys)
    denominator = math.fsum((value - mean_x) ** 2 for value in xs)
    if denominator == 0.0:
        raise ValueError("point times must not all be equal")
    numerator = math.fsum(
        (x - mean_x) * (y - mean_y) for x, y in zip(xs, ys))
    return numerator / denominator


def dotted_get(value, path):
    current = value
    for component in path.split("."):
        if not isinstance(current, dict) or component not in current:
            raise BenchError(f"status is missing {path}")
        current = current[component]
    return current


def require_uint(value, path, bits=32):
    if isinstance(value, bool) or not isinstance(value, int):
        raise BenchError(f"{path} must be an unsigned integer")
    if value < 0 or value >= 1 << bits:
        raise BenchError(f"{path} must fit uint{bits}")
    return value


def validate_status(status):
    if not isinstance(status, dict):
        raise BenchError("status payload must be an object")
    revision = status.get("build_revision")
    build_utc = status.get("build_utc")
    if not isinstance(revision, str) or not revision or "unknown" in revision:
        raise BenchError("status build_revision is missing or not exact")
    if not isinstance(build_utc, str) or not build_utc or build_utc == "unknown":
        raise BenchError("status build_utc is missing or not exact")
    for path in (
            "overruns", "event_overflows", "network_budget_overruns",
            "network_restarts", "http_service_overruns", "boot_generation",
            "watchdog_early_resets"):
        require_uint(dotted_get(status, path), path,
                     8 if path == "watchdog_early_resets" else 32)
    for object_name, fields in (("clock", CLOCK_FIELDS),
                                ("audio_diag", AUDIO_FIELDS)):
        obj = status.get(object_name)
        if not isinstance(obj, dict):
            raise BenchError(f"status {object_name} must be an object")
        for field in fields:
            if field not in obj:
                raise BenchError(f"status is missing {object_name}.{field}")
    clock = status["clock"]
    for field in ("carrier_locked", "carrier_ready"):
        if not isinstance(clock[field], bool):
            raise BenchError(f"clock.{field} must be boolean")
    for field in CLOCK_FIELDS:
        if field == "trim_ppm":
            if isinstance(clock[field], bool) or not isinstance(clock[field], int):
                raise BenchError("clock.trim_ppm must be an integer")
        elif field not in ("carrier_locked", "carrier_ready", "master", "state"):
            require_uint(clock[field], f"clock.{field}")
    if clock["master"] not in ("input", "output"):
        raise BenchError("clock.master has an unknown value")
    if clock["state"] not in (
            "tracking", "acquiring", "refused", "unsupported", "unlocked",
            "replay", "fault"):
        raise BenchError("clock.state has an unknown value")
    for field in AUDIO_FIELDS:
        require_uint(status["audio_diag"][field], f"audio_diag.{field}")
    tx_gate = status.get("tx_gate")
    if not isinstance(tx_gate, dict) or "open" not in tx_gate:
        raise BenchError("status is missing tx_gate.open")
    if tx_gate["open"] not in (0, 1, False, True):
        raise BenchError("tx_gate.open must be boolean or 0/1")
    require_uint(status.get("format"), "format", 8)
    pipeline = status.get("pipeline_diag")
    if not isinstance(pipeline, dict):
        raise BenchError("status pipeline_diag must be an object")
    for field in ("class", "codec_pc"):
        require_uint(pipeline.get(field), f"pipeline_diag.{field}", 16)
    return status


def validate_events(events):
    if not isinstance(events, dict):
        raise BenchError("events payload must be an object")
    require_uint(events.get("cursor"), "events.cursor")
    require_uint(events.get("next"), "events.next")
    items = events.get("events")
    if not isinstance(items, list):
        raise BenchError("events.events must be an array")
    for index, event in enumerate(items):
        if not isinstance(event, dict):
            raise BenchError(f"events.events[{index}] must be an object")
        for field, bits in (
                ("sequence", 32), ("uptime_ms", 32), ("type", 8),
                ("state", 8), ("detail", 16)):
            require_uint(event.get(field),
                         f"events.events[{index}].{field}", bits)
    return events


def build_identity(status):
    validate_status(status)
    return {
        "build_revision": status["build_revision"],
        "build_utc": status["build_utc"],
    }


def accumulated_delta(samples, path, bits=32):
    values = [dotted_get(sample["status"], path) for sample in samples]
    for value in values:
        require_uint(value, path, bits)
    return sum(unsigned_delta(before, after, bits)
               for before, after in zip(values, values[1:]))


def transport_metrics(samples, metrics):
    """Report per-sample transport retries and the board's own drop counter.

    Evidence written before the retry counter existed carries no `attempts`, so
    those samples are counted as unrecorded rather than as clean: a zero total
    over old evidence would be a claim the run never made.
    """
    retries = 0
    retried_samples = 0
    unrecorded = 0
    for sample in samples:
        attempts = sample.get("attempts")
        if (isinstance(attempts, bool) or not isinstance(attempts, int) or
                attempts < SAMPLE_REQUESTS):
            unrecorded += 1
            continue
        if attempts > SAMPLE_REQUESTS:
            retries += attempts - SAMPLE_REQUESTS
            retried_samples += 1
    metrics["transport_retries"] = retries
    metrics["transport_retried_samples"] = retried_samples
    metrics["samples_without_attempts"] = unrecorded
    # http_client_drops is the board's count of the same event; it is reported,
    # never gated (see COMMON_HARMFUL_COUNTERS). Older status documents may not
    # carry it, which is a missing metric rather than a failure.
    drops = [sample["status"].get("http_client_drops") for sample in samples]
    if all(not isinstance(value, bool) and isinstance(value, int) and
           0 <= value < 1 << 32 for value in drops):
        metrics["http_client_drops_delta"] = sum(
            unsigned_delta(before, after)
            for before, after in zip(drops, drops[1:]))
    else:
        metrics["http_client_drops_delta"] = None


def add_failure(failures, code, message):
    failures.append({"code": code, "message": message})


def settled_samples(samples, settle_s):
    return [sample for sample in samples if sample["elapsed_s"] >= settle_s]


def rate_sample_ok(status, rate_hz, max_follow_error_millihz):
    clock = status["clock"]
    return (
        clock["carrier_locked"] and clock["carrier_ready"] and
        clock["nominal_rate_hz"] == rate_hz and
        clock["stream_rate_hz"] == rate_hz and
        clock["master"] == "input" and clock["state"] == "tracking" and
        bool(status["tx_gate"]["open"]) and
        abs(clock["output_rate_millihz"] -
            clock["carrier_rate_millihz"]) <= max_follow_error_millihz
    )


def stable_phase_labels(samples, minimum_samples, max_follow_error_millihz):
    raw = []
    for sample in samples:
        status = sample["status"]
        clock = status["clock"]
        label = None
        for rate in SUPPORTED_RATES:
            if rate_sample_ok(status, rate, max_follow_error_millihz):
                label = str(rate)
                break
        if (clock["state"] == "replay" and clock["master"] == "output" and
                not clock["carrier_locked"] and
                clock["nominal_rate_hz"] == 48000 and
                clock["stream_rate_hz"] == 48000 and
                clock["output_rate_millihz"] == 48000000 and
                bool(status["tx_gate"]["open"])):
            label = "replay"
        raw.append(label)

    labels = []
    indexes = []
    index = 0
    while index < len(raw):
        if raw[index] is None:
            index += 1
            continue
        end = index + 1
        while end < len(raw) and raw[end] == raw[index]:
            end += 1
        if end - index >= minimum_samples:
            if not labels or labels[-1] != raw[index]:
                labels.append(raw[index])
                indexes.append((index, end - 1))
        index = end
    return labels, indexes


def check_common(samples, failures, metrics, allow_clock_mismatch=False,
                 allow_generation_change=False,
                 allow_dark_tx_underrun=False,
                 allow_unsupported_episodes=False):
    deltas = {}
    for path in COMMON_HARMFUL_COUNTERS:
        deltas[path] = accumulated_delta(samples, path)
        allowed = (
            allow_dark_tx_underrun and
            path == "clock.tx_underrun_frames"
        )
        if deltas[path] != 0 and not allowed:
            add_failure(failures, "harmful-counter",
                        f"{path} advanced by {deltas[path]}")
    if not allow_generation_change:
        for path in ("audio_diag.ingress_generation",
                     "audio_diag.service_generation_resets"):
            deltas[path] = accumulated_delta(samples, path)
            if deltas[path] != 0:
                add_failure(failures, "ingress-reset",
                            f"{path} advanced by {deltas[path]}")
    for path in (
            "clock.mismatch_blocks", "clock.unsupported_blocks",
            "clock.unsupported_episodes"):
        deltas[path] = accumulated_delta(samples, path)
        if deltas[path] and not (allow_clock_mismatch and
                                 path == "clock.mismatch_blocks") and not (
                                     allow_unsupported_episodes and
                                     path == "clock.unsupported_episodes"):
            add_failure(failures, "clock-refusal",
                        f"{path} advanced by {deltas[path]}")
    if samples[0]["status"]["boot_generation"] != \
            samples[-1]["status"]["boot_generation"]:
        add_failure(failures, "device-reset",
                    "boot_generation changed during the run")
    early = accumulated_delta(samples, "watchdog_early_resets", bits=8)
    deltas["watchdog_early_resets"] = early
    if early:
        add_failure(failures, "watchdog-reset",
                    f"watchdog_early_resets advanced by {early}")
    metrics["counter_deltas"] = deltas


def counter_advance_intervals(samples, path):
    intervals = []
    for index in range(1, len(samples)):
        before = dotted_get(samples[index - 1]["status"], path)
        after = dotted_get(samples[index]["status"], path)
        delta = unsigned_delta(before, after)
        if delta:
            intervals.append({
                "from_index": index - 1,
                "to_index": index,
                "from_elapsed_s": samples[index - 1]["elapsed_s"],
                "to_elapsed_s": samples[index]["elapsed_s"],
                "delta": delta,
                "from_gate_open": bool(
                    samples[index - 1]["status"]["tx_gate"]["open"]),
                "to_gate_open": bool(
                    samples[index]["status"]["tx_gate"]["open"]),
            })
    return intervals


def interval_in_transition_window(interval, windows):
    return any(
        start <= interval["from_index"] and interval["to_index"] <= end
        for start, end in windows
    )


def check_transition_dark_effects(samples, events, indexes, failures, metrics):
    transition_windows = [
        (indexes[index - 1][1], indexes[index][0])
        for index in range(1, len(indexes))
    ]
    live_windows = transition_windows[:-1]
    metrics["transition_windows"] = transition_windows

    underruns = counter_advance_intervals(
        samples, "clock.tx_underrun_frames")
    metrics["transition_tx_underrun_intervals"] = underruns
    for interval in underruns:
        if interval["from_gate_open"] and interval["to_gate_open"]:
            add_failure(
                failures, "transition-open-gate-underrun",
                "tx_underrun_frames advanced between two open-gate samples")
        elif not interval_in_transition_window(interval, transition_windows):
            add_failure(
                failures, "transition-unattributed-underrun",
                "tx_underrun_frames advanced outside an observed transition")

    unsupported = counter_advance_intervals(
        samples, "clock.unsupported_episodes")
    metrics["transition_unsupported_intervals"] = unsupported
    for interval in unsupported:
        if interval["from_gate_open"] and interval["to_gate_open"]:
            add_failure(
                failures, "transition-open-gate-unsupported",
                "unsupported_episodes advanced between two open-gate samples")
        elif not interval_in_transition_window(interval, live_windows):
            add_failure(
                failures, "transition-unattributed-unsupported",
                "unsupported_episodes advanced outside a live-rate transition")

    unsupported_delta = accumulated_delta(
        samples, "clock.unsupported_episodes")
    unsupported_events = [
        event for event in events if event["type"] == 12
    ]
    details = [event["detail"] for event in unsupported_events]
    metrics["unsupported_event_details"] = details
    if len(unsupported_events) != unsupported_delta:
        add_failure(
            failures, "unsupported-event-count",
            f"unsupported event count {len(unsupported_events)} does not match "
            f"unsupported_episodes delta {unsupported_delta}")
    supported_details = [
        detail for detail in details
        if any(
            lower <= detail * 1000 <= upper
            for lower, upper in (
                (31360000, 32640000),
                (43218000, 44982000),
                (47040000, 48960000),
            )
        )
    ]
    if supported_details:
        add_failure(
            failures, "unsupported-event-detail",
            f"unsupported events reported supported-rate details "
            f"{supported_details}")


def check_live_rate(samples, rate_hz, settle_s, max_follow_error_millihz,
                    failures, metrics):
    stable = settled_samples(samples, settle_s)
    if len(stable) < 2:
        add_failure(failures, "settling",
                    "fewer than two samples remain after settling")
        return []
    mismatches = [
        sample["elapsed_s"] for sample in stable
        if not rate_sample_ok(sample["status"], rate_hz,
                              max_follow_error_millihz)
    ]
    if mismatches:
        add_failure(
            failures, "rate-state-mismatch",
            f"{len(mismatches)} settled samples did not report locked, ready, "
            f"tracking input at {rate_hz} Hz with an open gate")
    errors = [
        abs(sample["status"]["clock"]["output_rate_millihz"] -
            sample["status"]["clock"]["carrier_rate_millihz"])
        for sample in stable
    ]
    metrics["max_follow_error_millihz"] = max(errors)
    for name, capacity in (
            ("clock.tx_level_frames", TX_CAPACITY_FRAMES),
            ("clock.ingress_level_words", INGRESS_CAPACITY_WORDS)):
        values = [dotted_get(sample["status"], name) for sample in stable]
        metrics[f"max_{name.replace('.', '_')}"] = max(values)
        if any(value < 0 or value > capacity for value in values):
            add_failure(failures, "ring-level",
                        f"{name} exceeded its 0..{capacity} bound")
    progress = accumulated_delta(stable,
                                 "audio_diag.committed_output_blocks")
    metrics["committed_output_blocks_delta"] = progress
    if progress == 0:
        add_failure(failures, "no-output-progress",
                    "committed_output_blocks did not advance after settling")
    return stable


def check_external_pcm(external, rate_hz, failures):
    if not isinstance(external, dict):
        add_failure(failures, "external-observation",
                    "pcm-120s requires external observation JSON")
        return
    if external.get("audio_rate_hz") != rate_hz:
        add_failure(failures, "audio-rate",
                    f"external audio rate is not {rate_hz} Hz")
    if external.get("continuous_output") is not True:
        add_failure(failures, "continuous-output",
                    "external observation does not confirm continuous output")


def check_waveform(external, trim_delta, invalid_delta, failures, metrics):
    if not isinstance(external, dict):
        add_failure(failures, "external-observation",
                    "waveform requires capture-derived observation JSON")
        return
    for field in (
            "capture_frame_gaps", "discontinuities_outside_invalid",
            "correlated_trim_updates", "correlated_invalid_windows"):
        value = external.get(field)
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            add_failure(failures, "external-observation",
                        f"waveform observation {field} is missing or invalid")
            return
    # Gate 7 contract (design decision 2026-09-02): no discontinuity outside the
    # board-declared invalid windows; the windows need NOT be externally
    # visible. An empty interval list is therefore a valid observation, and
    # the capture may correlate FEWER events than status declares -- never
    # more, since a captured artifact the board did not declare is exactly
    # the failure the gate exists to catch. Every window that IS visible must
    # still be the exact 512-frame hold and must pair with a correlated
    # window.
    intervals = external.get("invalid_intervals_frames")
    if not isinstance(intervals, list) or any(
            isinstance(value, bool) or not isinstance(value, int)
            for value in intervals):
        add_failure(failures, "external-observation",
                    "waveform invalid_intervals_frames must be an integer array")
        return
    metrics["waveform"] = external
    if external["capture_frame_gaps"] != 0:
        add_failure(failures, "capture-cadence",
                    "capture contains frame-cadence gaps")
    if external["discontinuities_outside_invalid"] != 0:
        add_failure(failures, "waveform-discontinuity",
                    "capture has discontinuities outside declared invalid intervals")
    if any(value != 512 for value in intervals):
        add_failure(failures, "invalid-window-length",
                    "every captured invalid interval must be exactly 512 frames")
    if external["correlated_trim_updates"] > trim_delta:
        add_failure(failures, "trim-correlation",
                    "captured trim count exceeds status trim_updates delta")
    if external["correlated_invalid_windows"] > invalid_delta:
        add_failure(failures, "invalid-correlation",
                    "captured invalid-window count exceeds status delta")
    if len(intervals) != external["correlated_invalid_windows"]:
        add_failure(failures, "invalid-interval-evidence",
                    "captured invalid-interval count differs from correlated windows")


def analyze_samples(samples, events, scenario, options=None, external=None):
    """Pure acceptance analysis over already parsed samples and events."""
    options = dict(options or {})
    failures = []
    metrics = {}
    result = {
        "schema_version": SCHEMA_VERSION,
        "scenario": scenario,
        "verdict": "fail",
        "build_identity": None,
        "sample_count": len(samples) if isinstance(samples, list) else 0,
        "duration_s": 0.0,
        "metrics": metrics,
        "failures": failures,
    }
    if scenario not in SCENARIOS:
        add_failure(failures, "scenario", f"unknown scenario {scenario!r}")
        return result
    if not isinstance(samples, list) or len(samples) < 2:
        add_failure(failures, "samples", "at least two status samples are required")
        return result
    try:
        identities = []
        last_elapsed = None
        for index, sample in enumerate(samples):
            if not isinstance(sample, dict):
                raise BenchError(f"sample {index} must be an object")
            elapsed = sample.get("elapsed_s")
            if (isinstance(elapsed, bool) or not isinstance(elapsed, (int, float))
                    or not math.isfinite(elapsed) or elapsed < 0):
                raise BenchError(f"sample {index} elapsed_s is invalid")
            if last_elapsed is not None and elapsed <= last_elapsed:
                raise BenchError("sample elapsed_s values must strictly increase")
            last_elapsed = elapsed
            validate_status(sample.get("status"))
            identities.append(build_identity(sample["status"]))
        validate_events({"cursor": 0, "next": 0, "events": events})
    except BenchError as error:
        add_failure(failures, "payload", str(error))
        return result
    result["build_identity"] = identities[0]
    if any(identity != identities[0] for identity in identities[1:]):
        add_failure(failures, "build-changed",
                    "build identity changed during the run")
    expected_build = options.get("expect_build")
    if expected_build and identities[0]["build_revision"] != expected_build:
        add_failure(failures, "wrong-build",
                    f"observed {identities[0]['build_revision']}, expected "
                    f"{expected_build}")
    duration_s = samples[-1]["elapsed_s"] - samples[0]["elapsed_s"]
    result["duration_s"] = duration_s
    minimum = options.get("minimum_duration_s",
                          SCENARIOS[scenario]["minimum"])
    if duration_s + 1e-9 < minimum:
        add_failure(failures, "duration",
                    f"run is {duration_s:.3f}s; requires at least {minimum:.3f}s")
    settle_s = options.get("settle_s", SCENARIOS[scenario]["settle"])
    follow_bound = options.get("max_follow_error_millihz", 1000)
    metrics["settle_s"] = settle_s
    metrics["follow_error_bound_millihz"] = follow_bound
    transport_metrics(samples, metrics)

    allow_mismatch = scenario == "negative"
    allow_generation = scenario == "transitions"
    allow_transition_effects = scenario == "transitions"
    check_common(samples, failures, metrics, allow_mismatch, allow_generation,
                 allow_dark_tx_underrun=(
                     allow_mismatch or allow_transition_effects),
                 allow_unsupported_episodes=allow_transition_effects)
    trim_delta = accumulated_delta(samples, "clock.trim_updates")
    invalid_delta = accumulated_delta(samples, "clock.invalid_windows")
    metrics["trim_updates_delta"] = trim_delta
    metrics["invalid_windows_delta"] = invalid_delta

    if scenario == "pcm-48k-smoke":
        stable = check_live_rate(samples, 48000, settle_s, follow_bound,
                                 failures, metrics)
        if any(sample["status"]["format"] != 1 or
               sample["status"]["pipeline_diag"]["class"] != 1 or
               sample["status"]["pipeline_diag"].get("pcm_confirmed") != 1
               for sample in stable):
            add_failure(failures, "format",
                        "a settled sample did not confirm 48 kHz PCM")
        if accumulated_delta(stable, "clock.rate_changes") != 0:
            add_failure(failures, "rate-stability",
                        "nominal rate changed after settling")

    elif scenario == "48k-10m":
        stable = check_live_rate(samples, 48000, settle_s, follow_bound,
                                 failures, metrics)
        if len(stable) >= 2:
            try:
                slope = least_squares_slope([
                    (sample["elapsed_s"],
                     sample["status"]["clock"]["ingress_level_words"])
                    for sample in stable
                ])
                metrics["ingress_slope_words_per_s"] = slope
                if abs(slope) > options.get("max_slope_words_per_s", 0.25):
                    add_failure(failures, "ingress-slope",
                                f"absolute ingress slope {abs(slope):.6f} "
                                "words/s exceeds 0.25")
            except ValueError as error:
                add_failure(failures, "ingress-slope", str(error))
            trim_after_settle = accumulated_delta(
                stable, "clock.trim_updates")
            metrics["trim_updates_after_settle"] = trim_after_settle
            if trim_after_settle > 2:
                add_failure(failures, "trim-cadence",
                            f"{trim_after_settle} trim updates occurred after settling")
        if accumulated_delta(settled_samples(samples, settle_s),
                             "clock.rate_changes") != 0:
            add_failure(failures, "rate-stability",
                        "nominal rate changed after settling")

    elif scenario == "pcm-120s":
        rate = options.get("rate_hz")
        if rate not in (32000, 44100):
            add_failure(failures, "rate-option",
                        "pcm-120s requires rate_hz 32000 or 44100")
        else:
            stable = check_live_rate(samples, rate, settle_s, follow_bound,
                                     failures, metrics)
            if any(sample["status"]["format"] != 1 or
                   sample["status"]["pipeline_diag"]["class"] != 1
                   for sample in stable):
                add_failure(failures, "format",
                            "a settled sample did not classify as PCM")
            check_external_pcm(external, rate, failures)

    elif scenario == "encoded":
        rate = options.get("rate_hz")
        codec = options.get("codec")
        if rate not in (32000, 44100):
            add_failure(failures, "rate-option",
                        "encoded requires a non-48k supported rate_hz")
        elif codec not in ("dts", "ac3"):
            add_failure(failures, "codec-option",
                        "encoded requires codec dts or ac3")
        else:
            stable = check_live_rate(samples, rate, settle_s, follow_bound,
                                     failures, metrics)
            expected_pc = 11 if codec == "dts" else 1
            if any(sample["status"]["format"] != 2 or
                   sample["status"]["pipeline_diag"]["class"] != 2 or
                   sample["status"]["pipeline_diag"]["codec_pc"] != expected_pc
                   for sample in stable):
                add_failure(failures, "encoded-format",
                            f"a settled sample did not report {codec} PC "
                            f"{expected_pc}")

    elif scenario == "negative":
        carrier_rate = options.get("carrier_rate_hz")
        stream_rate = options.get("stream_rate_hz")
        stable = settled_samples(samples, settle_s)
        if (carrier_rate not in SUPPORTED_RATES or
                stream_rate not in SUPPORTED_RATES or
                carrier_rate == stream_rate):
            add_failure(failures, "rate-option",
                        "negative requires distinct supported carrier and stream rates")
        elif len(stable) < 2:
            add_failure(failures, "settling",
                        "fewer than two samples remain after settling")
        else:
            mismatch = accumulated_delta(stable, "clock.mismatch_blocks")
            metrics["mismatch_blocks_after_settle"] = mismatch
            if mismatch == 0:
                add_failure(failures, "mismatch-not-counted",
                            "mismatch_blocks did not advance")
            if any(sample["status"]["clock"]["nominal_rate_hz"] != carrier_rate
                   or sample["status"]["clock"]["stream_rate_hz"] != stream_rate
                   for sample in stable):
                add_failure(failures, "negative-rate-state",
                            "carrier and stream rates do not match the requested mismatch")
            if any(bool(sample["status"]["tx_gate"]["open"]) for sample in stable):
                add_failure(failures, "negative-not-muted",
                            "transmit gate opened during the mismatch")
            progress = accumulated_delta(
                stable, "audio_diag.committed_output_blocks")
            metrics["committed_output_blocks_delta"] = progress
            if progress:
                add_failure(failures, "negative-output-advanced",
                            f"{progress} output blocks committed during mismatch")

    elif scenario == "transitions":
        minimum_samples = options.get("phase_samples", 2)
        labels, indexes = stable_phase_labels(
            samples, minimum_samples, follow_bound)
        metrics["observed_phase_sequence"] = labels
        expected = ["48000", "44100", "48000", "replay"]
        check_transition_dark_effects(
            samples, events, indexes, failures, metrics)
        if labels != expected:
            add_failure(failures, "transition-sequence",
                        f"observed {labels}, expected {expected}")
        elif len(indexes) == len(expected):
            phase_progress = []
            for start, end in indexes:
                phase_progress.append(accumulated_delta(
                    samples[start:end + 1],
                    "audio_diag.committed_output_blocks"))
            metrics["phase_output_blocks_deltas"] = phase_progress
            if any(delta == 0 for delta in phase_progress):
                add_failure(failures, "transition-no-progress",
                            "one or more accepted phases made no output progress")

            reset_deltas = []
            for transition in range(1, len(indexes)):
                previous_end = indexes[transition - 1][1]
                current_end = indexes[transition][1]
                reset_deltas.append(accumulated_delta(
                    samples[previous_end:current_end + 1],
                    "audio_diag.service_requested_resets"))
            metrics["transition_reset_deltas"] = reset_deltas
            if any(delta != 1 for delta in reset_deltas):
                add_failure(failures, "transition-reset-count",
                            f"per-transition requested reset deltas are "
                            f"{reset_deltas}, expected one each")

            rate_window_deltas = []
            for transition in range(1, 3):
                previous_end = indexes[transition - 1][1]
                current_end = indexes[transition][1]
                rate_window_deltas.append(accumulated_delta(
                    samples[previous_end:current_end + 1],
                    "clock.rate_changes"))
            metrics["live_transition_rate_change_deltas"] = rate_window_deltas
            if any(delta != 1 for delta in rate_window_deltas):
                add_failure(failures, "rate-change-attribution",
                            f"per-live-transition rate deltas are "
                            f"{rate_window_deltas}, expected one each")
        live_transitions = [44100, 48000]
        rate_delta = accumulated_delta(samples, "clock.rate_changes")
        metrics["rate_changes_delta"] = rate_delta
        if rate_delta != len(live_transitions):
            add_failure(failures, "rate-change-count",
                        f"rate_changes advanced by {rate_delta}, expected "
                        f"{len(live_transitions)}")
        event_details = [
            event["detail"] for event in events if event["type"] == 11
        ]
        metrics["rate_change_event_details"] = event_details
        if event_details != live_transitions:
            add_failure(failures, "rate-change-events",
                        f"rate events {event_details}, expected {live_transitions}")
        if isinstance(external, dict):
            old_rate_frames = external.get("old_rate_output_frames")
            if (isinstance(old_rate_frames, bool) or
                    not isinstance(old_rate_frames, int) or
                    old_rate_frames < 0):
                add_failure(
                    failures, "external-observation",
                    "old_rate_output_frames must be an unsigned integer")
            elif old_rate_frames != 0:
                add_failure(failures, "old-rate-output",
                            f"external capture observed {old_rate_frames} "
                            "old-rate output frames")
        else:
            add_failure(failures, "external-observation",
                        "transitions requires old_rate_output_frames capture evidence")
        if indexes:
            final_start = indexes[-1][0]
            final = samples[final_start:]
            progress = accumulated_delta(
                final, "audio_diag.committed_output_blocks")
            metrics["final_replay_output_blocks_delta"] = progress
            if progress == 0:
                add_failure(failures, "transition-no-progress",
                            "output did not resume during the final replay phase")

    elif scenario == "waveform":
        stable = check_live_rate(samples, 48000, settle_s, follow_bound,
                                 failures, metrics)
        if trim_delta < 1:
            add_failure(failures, "trim-not-observed",
                        "no same-nominal trim update was observed")
        if invalid_delta < trim_delta:
            add_failure(failures, "invalid-window-count",
                        "invalid_windows advanced fewer times than trim_updates")
        if stable:
            check_waveform(external, trim_delta, invalid_delta,
                           failures, metrics)

    result["verdict"] = "pass" if not failures else "fail"
    return result


def read_external(path):
    if path is None:
        return None, None
    try:
        raw = Path(path).read_text(encoding="utf-8")
        value = json.loads(raw)
    except (OSError, json.JSONDecodeError) as error:
        raise BenchError(f"cannot read external observation: {error}") from error
    if not isinstance(value, dict):
        raise BenchError("external observation JSON root must be an object")
    return value, raw


def parse_external_raw(raw):
    if not isinstance(raw, str):
        raise BenchError("embedded external observation is not raw JSON text")
    try:
        value = json.loads(raw)
    except json.JSONDecodeError as error:
        raise BenchError(
            f"embedded external observation is malformed: {error}") from error
    if not isinstance(value, dict):
        raise BenchError("embedded external observation root must be an object")
    return value


def evidence_record(stream, record):
    stream.write(json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n")
    stream.flush()


def collect_one(transport, cursor, started):
    """Collect one sample, timestamped when its status response arrived.

    `elapsed_s` is stamped from the status response, not from the moment the
    fetch was issued: a retried fetch can spend seconds inside get_json, and a
    timestamp taken before it would place the sample earlier than the counters
    it carries -- biasing the settle window, the ingress slope and the
    transition-window indexing on exactly the samples that had trouble.

    `attempts` is the sample's total GET count (status plus events), so a clean
    sample reads SAMPLE_REQUESTS and anything above it is a retry.
    """
    status_response = transport.get_json("/api/status")
    elapsed_s = time.monotonic() - started
    validate_status(status_response["body"])
    events_response = transport.get_json(f"/api/events?cursor={cursor}")
    validate_events(events_response["body"])
    return {
        "kind": "sample",
        "elapsed_s": elapsed_s,
        "observed_utc": utc_now(),
        "status_http_status": status_response["http_status"],
        "raw_status_json": status_response["raw"],
        "events_http_status": events_response["http_status"],
        "raw_events_json": events_response["raw"],
        "status_attempts": status_response["attempts"],
        "events_attempts": events_response["attempts"],
        "attempts": status_response["attempts"] + events_response["attempts"],
        "status": status_response["body"],
        "events": events_response["body"]["events"],
        "next_event_cursor": events_response["body"]["next"],
    }


def collect_live(args, analysis_options, external, external_raw):
    target = parse_target(args.target)
    transport = HttpTransport(target, args.timeout)
    evidence_path = Path(args.evidence)
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    mode = "w" if args.overwrite else "x"
    try:
        stream = evidence_path.open(mode, encoding="utf-8")
    except FileExistsError as error:
        raise BenchError(
            f"evidence already exists: {evidence_path}; use --overwrite explicitly"
        ) from error
    samples = []
    all_events = []
    seen_events = {}
    try:
        first_status = transport.get_json("/api/status")
        # Zero the run clock when the first status response is in hand, so
        # every elapsed_s below is measured from a response, never from a
        # request that may have been retried (see collect_one).
        started = time.monotonic()
        validate_status(first_status["body"])
        identity = build_identity(first_status["body"])
        cursor = first_status["body"].get("event_sequence")
        require_uint(cursor, "event_sequence")
        metadata = {
            "kind": "metadata",
            "schema_version": SCHEMA_VERSION,
            "scenario": args.scenario,
            "started_utc": utc_now(),
            "target": args.target,
            "duration_s": args.duration,
            "interval_s": args.interval,
            "timeout_s": args.timeout,
            "build_identity": identity,
            "analysis_options": analysis_options,
        }
        if external_raw is not None:
            metadata["raw_external_observation_json"] = external_raw
        evidence_record(stream, metadata)

        # Keep the already-read authoritative status as sample zero and fetch
        # events starting at its sequence, so pre-run events are excluded.
        events_response = transport.get_json(f"/api/events?cursor={cursor}")
        validate_events(events_response["body"])
        sample = {
            "kind": "sample",
            "elapsed_s": 0.0,
            "observed_utc": utc_now(),
            "status_http_status": first_status["http_status"],
            "raw_status_json": first_status["raw"],
            "events_http_status": events_response["http_status"],
            "raw_events_json": events_response["raw"],
            "status_attempts": first_status["attempts"],
            "events_attempts": events_response["attempts"],
            "attempts": first_status["attempts"] + events_response["attempts"],
            "status": first_status["body"],
            "events": events_response["body"]["events"],
            "next_event_cursor": events_response["body"]["next"],
        }
        evidence_record(stream, sample)
        samples.append({
            "elapsed_s": 0.0,
            "status": sample["status"],
            "attempts": sample["attempts"],
        })
        cursor = sample["next_event_cursor"]
        for event in sample["events"]:
            seen_events[event["sequence"]] = event
            all_events.append(event)

        while True:
            elapsed = time.monotonic() - started
            remaining = args.duration - elapsed
            if remaining <= 0:
                break
            time.sleep(min(args.interval, remaining))
            sample = collect_one(transport, cursor, started)
            if build_identity(sample["status"]) != identity:
                raise BenchError("device build identity changed during collection")
            evidence_record(stream, sample)
            samples.append({
                "elapsed_s": sample["elapsed_s"],
                "status": sample["status"],
                "attempts": sample["attempts"],
            })
            cursor = sample["next_event_cursor"]
            for event in sample["events"]:
                previous = seen_events.get(event["sequence"])
                if previous is not None and previous != event:
                    raise BenchError(
                        f"event sequence {event['sequence']} changed payload")
                if previous is None:
                    seen_events[event["sequence"]] = event
                    all_events.append(event)
        result = analyze_samples(samples, all_events, args.scenario,
                                 analysis_options, external)
        evidence_record(stream, {"kind": "analysis", **result})
        return result
    finally:
        stream.close()


def load_evidence(path):
    metadata = None
    samples = []
    events = []
    seen_events = {}
    try:
        lines = Path(path).read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise BenchError(f"cannot read evidence: {error}") from error
    for number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError as error:
            raise BenchError(f"evidence line {number}: malformed JSON: {error}") from error
        if not isinstance(record, dict):
            raise BenchError(f"evidence line {number}: record is not an object")
        if record.get("kind") == "metadata":
            if metadata is not None:
                raise BenchError("evidence contains more than one metadata record")
            metadata = record
        elif record.get("kind") == "sample":
            raw_status = record.get("raw_status_json")
            raw_events = record.get("raw_events_json")
            if not isinstance(raw_status, str) or not isinstance(raw_events, str):
                raise BenchError(
                    f"evidence line {number}: raw status/events JSON is missing")
            try:
                status = json.loads(raw_status)
                event_body = json.loads(raw_events)
            except json.JSONDecodeError as error:
                raise BenchError(
                    f"evidence line {number}: malformed raw JSON: {error}") from error
            validate_status(status)
            validate_events(event_body)
            # `attempts` post-dates the transport retry, so evidence written
            # before it simply has none; carry whatever is there and let
            # transport_metrics report the gap rather than inventing a 2.
            samples.append({
                "elapsed_s": record.get("elapsed_s"),
                "status": status,
                "attempts": record.get("attempts"),
            })
            for event in event_body["events"]:
                previous = seen_events.get(event["sequence"])
                if previous is not None and previous != event:
                    raise BenchError(
                        f"event sequence {event['sequence']} changed payload")
                if previous is None:
                    seen_events[event["sequence"]] = event
                    events.append(event)
    if metadata is None:
        raise BenchError("evidence is missing metadata")
    if metadata.get("schema_version") != SCHEMA_VERSION:
        raise BenchError("evidence schema_version is unsupported")
    if samples and metadata.get("build_identity") != build_identity(
            samples[0]["status"]):
        raise BenchError("metadata build identity does not match raw status JSON")
    return metadata, samples, events


def analysis_options_from_args(args):
    return {
        "settle_s": args.settle,
        "minimum_duration_s": SCENARIOS[args.scenario]["minimum"],
        "max_follow_error_millihz": args.max_follow_error_millihz,
        "max_slope_words_per_s": 0.25,
        "rate_hz": args.rate,
        "carrier_rate_hz": args.carrier_rate,
        "stream_rate_hz": args.stream_rate,
        "codec": args.codec,
        "phase_samples": args.phase_samples,
        "expect_build": args.expect_build,
    }


def validate_cli_args(args):
    if args.command == "run":
        if args.duration <= 0 or args.duration > MAX_DURATION_S:
            raise BenchError(
                f"duration must be greater than zero and at most {MAX_DURATION_S}s")
        minimum = SCENARIOS[args.scenario]["minimum"]
        if args.duration < minimum:
            raise BenchError(
                f"{args.scenario} requires at least {minimum:.0f}s")
        if args.interval < MIN_INTERVAL_S or args.interval > MAX_INTERVAL_S:
            raise BenchError(
                f"interval must be in {MIN_INTERVAL_S}..{MAX_INTERVAL_S}s")
        if args.timeout <= 0 or args.timeout > 10:
            raise BenchError("timeout must be greater than zero and at most 10s")
        if args.settle >= args.duration:
            raise BenchError("settle must be less than duration")
    if args.settle < 0:
        raise BenchError("settle must be non-negative")
    if args.max_follow_error_millihz < 0:
        raise BenchError("max-follow-error-millihz must be non-negative")
    if args.phase_samples < 1:
        raise BenchError("phase-samples must be at least one")
    if args.scenario == "pcm-120s" and args.rate not in (32000, 44100):
        raise BenchError("pcm-120s requires --rate 32000 or 44100")
    if args.scenario == "encoded":
        if args.rate not in (32000, 44100):
            raise BenchError("encoded requires --rate 32000 or 44100")
        if args.codec not in ("dts", "ac3"):
            raise BenchError("encoded requires --codec dts or ac3")
    if args.scenario == "negative":
        if (args.carrier_rate not in SUPPORTED_RATES or
                args.stream_rate not in SUPPORTED_RATES or
                args.carrier_rate == args.stream_rate):
            raise BenchError(
                "negative requires distinct --carrier-rate and --stream-rate")


def add_analysis_arguments(parser):
    parser.add_argument("scenario", choices=tuple(SCENARIOS))
    parser.add_argument("--settle", type=float)
    parser.add_argument("--rate", type=int, choices=SUPPORTED_RATES)
    parser.add_argument("--carrier-rate", type=int, choices=SUPPORTED_RATES)
    parser.add_argument("--stream-rate", type=int, choices=SUPPORTED_RATES)
    parser.add_argument("--codec", choices=("dts", "ac3"))
    parser.add_argument("--phase-samples", type=int)
    parser.add_argument("--max-follow-error-millihz", type=int)
    parser.add_argument("--expect-build")
    parser.add_argument("--external-observation")


def make_parser():
    parser = argparse.ArgumentParser(
        description="Bounded read-only Aius sample-rate acceptance bench")
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run", help="poll a live device and analyze it")
    add_analysis_arguments(run)
    run.add_argument("--target", required=True, help="device HOST or HOST:PORT")
    run.add_argument("--evidence", required=True, help="new JSONL evidence path")
    run.add_argument("--duration", type=float)
    run.add_argument("--interval", type=float, default=1.0)
    run.add_argument("--timeout", type=float, default=0.75)
    run.add_argument("--overwrite", action="store_true")

    analyze = commands.add_parser(
        "analyze", help="reanalyze JSONL evidence without device access")
    add_analysis_arguments(analyze)
    analyze.add_argument("--evidence", required=True)
    return parser


def main(argv=None):
    parser = make_parser()
    args = parser.parse_args(argv)
    defaults = SCENARIOS[args.scenario]
    try:
        if args.command == "run":
            if args.duration is None:
                args.duration = defaults["duration"]
            if args.settle is None:
                args.settle = defaults["settle"]
            if args.phase_samples is None:
                args.phase_samples = 2
            if args.max_follow_error_millihz is None:
                args.max_follow_error_millihz = 1000
            validate_cli_args(args)
            external, external_raw = read_external(
                args.external_observation)
            if (args.scenario in ("pcm-120s", "transitions", "waveform")
                    and external is None):
                raise BenchError(
                    f"{args.scenario} requires --external-observation")
            options = analysis_options_from_args(args)
            result = collect_live(
                args, options, external, external_raw)
        else:
            metadata, samples, events = load_evidence(args.evidence)
            if metadata.get("scenario") != args.scenario:
                raise BenchError(
                    f"evidence scenario is {metadata.get('scenario')!r}, "
                    f"not {args.scenario!r}")
            stored = metadata.get("analysis_options")
            if not isinstance(stored, dict):
                stored = {}
            for argument, option, fallback in (
                    ("settle", "settle_s", defaults["settle"]),
                    ("rate", "rate_hz", None),
                    ("carrier_rate", "carrier_rate_hz", None),
                    ("stream_rate", "stream_rate_hz", None),
                    ("codec", "codec", None),
                    ("phase_samples", "phase_samples", 2),
                    ("max_follow_error_millihz",
                     "max_follow_error_millihz", 1000),
                    ("expect_build", "expect_build", None)):
                if getattr(args, argument) is None:
                    setattr(args, argument, stored.get(option, fallback))
            validate_cli_args(args)
            external, _external_raw = read_external(
                args.external_observation)
            if external is None and "raw_external_observation_json" in metadata:
                external = parse_external_raw(
                    metadata["raw_external_observation_json"])
            options = analysis_options_from_args(args)
            result = analyze_samples(
                samples, events, args.scenario, options, external)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0 if result["verdict"] == "pass" else 1
    except BenchError as error:
        print(f"aius_rate_bench: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
