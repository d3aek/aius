#!/usr/bin/env python3
"""Capture the board's optical S/PDIF output and reduce it to the
external-observation JSON that aius_rate_bench.py's sample-rate gates read.

Three acceptance gates assert properties of the board's actual optical
output that HTTP /api/status cannot self-report:

    pcm-120s   -> {"channel_status_rate_hz", "continuous_output"}
    transitions-> {"old_rate_output_frames"}
    waveform   -> {"capture_frame_gaps", "discontinuities_outside_invalid",
                   "invalid_intervals_frames", "correlated_trim_updates",
                   "correlated_invalid_windows"}

This tool records board optical OUT -> U24 optical IN via ALSA `arecord`,
drives the scenario stimulus via `aplay`, polls /api/status in lockstep to
build the correlation series, reduces the waveform to those exact fields,
and writes <output>/external-observation.json plus a full
<output>/capture-analysis.json of the supporting evidence.

Capture chassis (ALSA orchestration, WAV reader, zero-run and impulse
primitives) mirrors ../source_qualification/direct_loop_bracket.py; the
gate reducers and their JSON schema are new and unrelated to that tool's
summary.json.

Two design choices are pinned by the current bench hardware and recorded
in docs/plans/aius-rate-gate-capture-tool.md:

  - channel_status_rate_hz is INFERRED from the captured tone frequency.
    The U24 XL exposes no IEC958 capture-status control (verified via
    `amixer -c0 controls`), so a true channel-status decode is not
    available on this interface. The field carries a
    channel_status_rate_hz_source: "inferred" marker.

  - Capture<->status time correlation uses BOUNDED LOCAL ALIGNMENT around
    each status-reported counter increment, not a global frame->wall-time
    map (which accumulates clock-drift error over a 600 s run). See
    correlate_counter_events.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import time
import wave
from pathlib import Path

import numpy as np

CHANNELS = 2
SAMPLE_WIDTH = 2
# A muted invalid window / dropout shows as an all-zero run. A 997 Hz tone
# touches exact zero at most one frame per crossing, so a run of >= 2 frames
# is a genuine gap, never an incidental crossing.
DROPOUT_MIN_FRAMES = 2
# The board emits one 512-frame PCM block per declared trim/invalid window.
DECLARED_WINDOW_FRAMES = 512
# Adjacent-sample jump that marks a waveform discontinuity (matches the
# source-qualification bracket's isolated-impulse threshold).
IMPULSE_THRESHOLD = 1000
# The validation tone; channel-status rate is inferred against it.
TONE_HZ = 997.0
# Bounded-alignment half-window, in seconds, added on each side of a
# status increment's [t_prev, t_curr] interval. Covers poll jitter plus the
# board's worst-case output-clock drift against the host capture clock.
ALIGNMENT_MARGIN_S = 1.5


def write_json(path: Path, value: dict) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def stop_process(process: subprocess.Popen | None) -> None:
    if process is None:
        return
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3.0)


def read_wav(path: Path) -> tuple[np.ndarray, dict]:
    with wave.open(str(path), "rb") as capture:
        geometry = {
            "channels": capture.getnchannels(),
            "sample_rate_hz": capture.getframerate(),
            "sample_width_bytes": capture.getsampwidth(),
            "frames": capture.getnframes(),
        }
        if geometry["sample_width_bytes"] != SAMPLE_WIDTH:
            raise RuntimeError(f"unexpected WAV geometry: {geometry}")
        raw = capture.readframes(capture.getnframes())
    samples = np.frombuffer(raw, dtype="<i2").reshape(
        -1, geometry["channels"])
    return samples.astype(np.int32), geometry


def active_bounds(samples: np.ndarray, block: int = 128) -> tuple[int, int]:
    """First and last blocks whose RMS clears a quiet-floor gate, so the
    reducers ignore the aplay lead-in and trailing silence."""
    count = len(samples) // block
    if count == 0:
        raise RuntimeError("capture too short to gate")
    blocks = samples[:count * block].reshape(count, block, samples.shape[1])
    rms = np.sqrt(np.mean(blocks.astype(np.float64) ** 2, axis=(1, 2)))
    active = np.flatnonzero(rms >= 100.0)
    if not len(active):
        raise RuntimeError("capture has no active tone")
    start = int(active[0]) * block
    end = min(len(samples), (int(active[-1]) + 1) * block)
    return start, end


def zero_runs(active: np.ndarray, min_frames: int = DROPOUT_MIN_FRAMES
              ) -> list[tuple[int, int]]:
    """All-channel-zero runs of at least min_frames, as [start, end)
    active-frame ranges. These are the capture's candidate dropouts and
    declared invalid windows."""
    mask = np.all(active == 0, axis=1)
    padded = np.pad(mask.astype(np.int8), (1, 1))
    edges = np.diff(padded)
    starts = np.flatnonzero(edges == 1)
    ends = np.flatnonzero(edges == -1)
    return [
        (int(start), int(end))
        for start, end in zip(starts, ends)
        if end - start >= min_frames
    ]


def discontinuity_threshold(jump: np.ndarray) -> float:
    """Adjacent-jump magnitude above which a sample is a discontinuity, not
    a point on the tone. A bandlimited sine's adjacent jumps cluster below
    its peak slope, so the 95th percentile tracks that peak at any
    amplitude; a genuine spike or phase step sits far above it. Floored at
    IMPULSE_THRESHOLD so a near-silent capture never lowers the bar."""
    if not jump.size:
        return float(IMPULSE_THRESHOLD)
    baseline = float(np.percentile(jump, 95))
    return max(float(IMPULSE_THRESHOLD), 3.0 * baseline)


def discontinuities(active: np.ndarray, exclude: list[tuple[int, int]]
                    ) -> list[int]:
    """Active-frame indexes of adjacent-sample jumps over the adaptive
    discontinuity threshold, collapsed to one index per contiguous jump
    group, excluding frames within (or on the edges of) the supplied
    zero-run spans (whose own edges are jumps into and out of silence)."""
    if len(active) < 2:
        return []
    jump = np.abs(np.diff(active.astype(np.int64), axis=0)).max(axis=1)
    threshold = discontinuity_threshold(jump)
    hits = np.flatnonzero(jump > threshold) + 1
    blocked = np.zeros(len(active), dtype=bool)
    for start, end in exclude:
        blocked[max(0, start - 2):min(len(active), end + 2)] = True
    events: list[int] = []
    previous = None
    for frame in hits:
        frame = int(frame)
        if blocked[frame]:
            continue
        if previous is not None and frame - previous <= 1:
            previous = frame
            continue
        events.append(frame)
        previous = frame
    return events


def estimate_hz(active: np.ndarray, capture_rate: int) -> float | None:
    """Tone frequency from positive-going zero crossings, averaged over
    both channels. None when the region has too few crossings."""
    estimates = []
    for channel in range(active.shape[1]):
        values = active[:, channel]
        crossings = np.flatnonzero(
            (values[:-1] <= 0) & (values[1:] > 0)) + 1
        if len(crossings) >= 2:
            span = int(crossings[-1] - crossings[0])
            if span:
                estimates.append((len(crossings) - 1) * capture_rate / span)
    if not estimates:
        return None
    return float(np.mean(estimates))


def counter_increment_intervals(status_series: list[dict], key: str
                                 ) -> list[dict]:
    """Wall-time intervals over which a monotonic status counter advanced,
    one entry per unit increment so bounded alignment can match each to a
    single capture event."""
    intervals: list[dict] = []
    for previous, current in zip(status_series, status_series[1:]):
        delta = int(current[key]) - int(previous[key])
        if delta <= 0:
            continue
        for _ in range(delta):
            intervals.append({
                "t_prev": float(previous["elapsed_s"]),
                "t_curr": float(current["elapsed_s"]),
            })
    return intervals


def _window_frames(interval: dict, capture_rate: int,
                   total_frames: int) -> tuple[int, int]:
    lo = int((interval["t_prev"] - ALIGNMENT_MARGIN_S) * capture_rate)
    hi = int((interval["t_curr"] + ALIGNMENT_MARGIN_S) * capture_rate)
    return max(0, lo), min(total_frames, hi)


def correlate_counter_events(intervals: list[dict], event_frames: list[int],
                             capture_rate: int, total_frames: int
                             ) -> tuple[int, list[int]]:
    """Bounded local alignment: greedily assign each status-counter unit
    increment to the nearest unmatched capture event whose frame lies inside
    that increment's drift-padded wall-time window. Returns the number of
    matched increments and the indexes of matched events."""
    matched_events: list[int] = []
    used = [False] * len(event_frames)
    correlated = 0
    for interval in intervals:
        lo, hi = _window_frames(interval, capture_rate, total_frames)
        centre = 0.5 * (interval["t_prev"] + interval["t_curr"]) * capture_rate
        best = None
        best_distance = None
        for index, frame in enumerate(event_frames):
            if used[index] or frame < lo or frame >= hi:
                continue
            distance = abs(frame - centre)
            if best_distance is None or distance < best_distance:
                best = index
                best_distance = distance
        if best is not None:
            used[best] = True
            matched_events.append(best)
            correlated += 1
    return correlated, matched_events


def reduce_pcm(active: np.ndarray, capture_rate: int, expected_rate: int
               ) -> dict:
    """Gate 2 (pcm-120s) fields plus supporting evidence."""
    runs = zero_runs(active)
    inferred = estimate_hz(active, capture_rate)
    channel_status_rate = None
    if inferred is not None:
        channel_status_rate = int(round(expected_rate * inferred / TONE_HZ))
    return {
        "external": {
            "channel_status_rate_hz": channel_status_rate,
            "continuous_output": bool(len(runs) == 0),
            "channel_status_rate_hz_source": "inferred",
        },
        "evidence": {
            "measured_tone_hz": inferred,
            "reference_tone_hz": TONE_HZ,
            "zero_run_count": len(runs),
            "zero_run_lengths": [end - start for start, end in runs],
        },
    }


def reduce_waveform(active: np.ndarray, capture_rate: int,
                    status_series: list[dict]) -> dict:
    """Gate 7 (waveform) fields via bounded local alignment of capture
    events against status trim_updates / invalid_windows increments."""
    total = len(active)
    runs = zero_runs(active)
    run_starts = [start for start, _ in runs]

    invalid_intervals = counter_increment_intervals(
        status_series, "invalid_windows")
    trim_intervals = counter_increment_intervals(
        status_series, "trim_updates")

    correlated_invalid, matched_runs = correlate_counter_events(
        invalid_intervals, run_starts, capture_rate, total)
    matched_run_set = set(matched_runs)
    invalid_lengths = [
        runs[index][1] - runs[index][0] for index in sorted(matched_run_set)
    ]
    capture_frame_gaps = len(runs) - len(matched_run_set)

    jumps = discontinuities(active, runs)
    correlated_trim, matched_jumps = correlate_counter_events(
        trim_intervals, jumps, capture_rate, total)
    discontinuities_outside = len(jumps) - len(matched_jumps)

    return {
        "external": {
            "capture_frame_gaps": int(capture_frame_gaps),
            "discontinuities_outside_invalid": int(discontinuities_outside),
            "invalid_intervals_frames": [int(x) for x in invalid_lengths],
            "correlated_trim_updates": int(correlated_trim),
            "correlated_invalid_windows": int(correlated_invalid),
        },
        "evidence": {
            "zero_run_count": len(runs),
            "zero_run_lengths": [end - start for start, end in runs],
            "discontinuity_frames": jumps,
            "status_invalid_increments": len(invalid_intervals),
            "status_trim_increments": len(trim_intervals),
            "correlation": "bounded-local-alignment",
            "alignment_margin_s": ALIGNMENT_MARGIN_S,
        },
    }


def reduce_transitions(active: np.ndarray, capture_rate: int,
                       status_series: list[dict], tolerance_hz: float = 20.0
                       ) -> dict:
    """Gate 4 (transitions) old_rate_output_frames: after each stream-rate
    change reported in the status series, count captured frames whose local
    tone matches the OLD rate's expected frequency rather than the new one.

    The captured tone frequency is preserved through the board's rate
    conversion, so old-rate output shows as the tone briefly holding the
    old effective frequency past the transition boundary. Boundaries come
    from status stream_rate_hz changes mapped to capture frames by wall
    time; the count is bounded to a short post-boundary window."""
    boundaries = []
    for previous, current in zip(status_series, status_series[1:]):
        if current["stream_rate_hz"] != previous["stream_rate_hz"]:
            boundaries.append({
                "frame": int(current["elapsed_s"] * capture_rate),
                "old_rate": int(previous["stream_rate_hz"]),
                "new_rate": int(current["stream_rate_hz"]),
            })
    # ~20 ms holds ~20 tone periods, enough crossings for a few-Hz estimate;
    # scan the first 250 ms past each boundary for stale old-rate output.
    window = max(2, capture_rate // 50)
    post = int(0.25 * capture_rate)
    old_rate_frames = 0
    for boundary in boundaries:
        old_hz = TONE_HZ * boundary["old_rate"] / boundary["new_rate"]
        pos = boundary["frame"]
        limit = min(len(active), boundary["frame"] + post)
        while pos + window <= limit:
            local = estimate_hz(active[pos:pos + window], capture_rate)
            pos += window
            if local is None:
                continue
            if abs(local - old_hz) < abs(local - TONE_HZ) and (
                    abs(local - old_hz) < tolerance_hz):
                old_rate_frames += window
    return {
        "external": {"old_rate_output_frames": int(old_rate_frames)},
        "evidence": {
            "boundaries": boundaries,
            "post_boundary_window_frames": window,
            "post_boundary_scan_frames": post,
            "detection_model": "tone-frequency-shift (provisional; "
                               "needs live calibration)",
        },
    }


def build_status_series(polls: list[dict]) -> list[dict]:
    """Flatten raw /api/status polls to the minimal correlation series the
    reducers consume."""
    series = []
    for poll in polls:
        clock = poll["status"]["clock"]
        series.append({
            "elapsed_s": float(poll["elapsed_s"]),
            "trim_updates": int(clock["trim_updates"]),
            "invalid_windows": int(clock["invalid_windows"]),
            "stream_rate_hz": int(clock["stream_rate_hz"]),
        })
    return series


def reduce_scenario(scenario: str, active: np.ndarray, capture_rate: int,
                    status_series: list[dict], expected_rate: int) -> dict:
    if scenario == "pcm-120s":
        return reduce_pcm(active, capture_rate, expected_rate)
    if scenario == "transitions":
        return reduce_transitions(active, capture_rate, status_series)
    if scenario == "waveform":
        return reduce_waveform(active, capture_rate, status_series)
    raise RuntimeError(f"unknown scenario {scenario!r}")


def poll_status(status_url: str, auth: str | None) -> dict:
    import urllib.request

    request = urllib.request.Request(status_url)
    if auth:
        request.add_header("X-Aius-SD-Auth", auth)
    with urllib.request.urlopen(request, timeout=5.0) as response:
        return json.loads(response.read().decode("utf-8"))


def capture_and_poll(args) -> tuple[Path, list[dict]]:
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    capture_path = output / "capture.wav"
    capture_log_path = output / "arecord.log"
    playback_log_path = output / "aplay.log"
    capture_log = capture_log_path.open("wb")
    playback_log = playback_log_path.open("wb")
    capture = None
    playback = None
    polls: list[dict] = []
    started = time.monotonic()
    try:
        capture = subprocess.Popen([
            "arecord", "-D", args.device, "-f", "S16_LE",
            "-r", str(args.rate), "-c", str(CHANNELS),
            "-d", str(args.seconds), str(capture_path),
        ], stdout=capture_log, stderr=subprocess.STDOUT)
        time.sleep(0.1)
        if args.tone:
            playback = subprocess.Popen([
                "aplay", "-D", args.device, "-d", str(args.seconds),
                str(args.tone),
            ], stdout=playback_log, stderr=subprocess.STDOUT)
        while capture.poll() is None:
            polls.append({
                "elapsed_s": time.monotonic() - started,
                "status": poll_status(args.status_url, args.auth),
            })
            time.sleep(1.0)
        capture.wait(timeout=15)
        if playback is not None:
            playback.wait(timeout=15)
    finally:
        stop_process(capture)
        stop_process(playback)
        capture_log.close()
        playback_log.close()
    write_json(output / "status-polls.json", {"polls": polls})
    return capture_path, polls


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="hw:0,0")
    parser.add_argument("--scenario", required=True,
                        choices=("pcm-120s", "transitions", "waveform"))
    parser.add_argument("--rate", type=int, default=48000,
                        help="arecord capture rate")
    parser.add_argument("--expected-rate", type=int,
                        help="gate 2 declared S/PDIF rate (default: --rate)")
    parser.add_argument("--seconds", type=int, default=130)
    parser.add_argument("--status-url", default="http://192.0.2.66/api/status")
    parser.add_argument("--auth")
    parser.add_argument("--tone", type=Path,
                        help="WAV stimulus to aplay; omitted for a passive "
                             "capture where the board is already sourcing")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    if args.expected_rate is None:
        args.expected_rate = args.rate

    capture_path, polls = capture_and_poll(args)
    samples, geometry = read_wav(capture_path)
    start, end = active_bounds(samples)
    active = samples[start:end]
    status_series = build_status_series(polls)
    reduced = reduce_scenario(
        args.scenario, active, args.rate, status_series, args.expected_rate)

    output = Path(args.output)
    write_json(output / "external-observation.json", reduced["external"])
    write_json(output / "capture-analysis.json", {
        "scenario": args.scenario,
        "geometry": geometry,
        "capture_rate_hz": args.rate,
        "active_start_frame": start,
        "active_end_frame_exclusive": end,
        "active_frames": int(end - start),
        "status_sample_count": len(status_series),
        "external": reduced["external"],
        "evidence": reduced["evidence"],
    })
    print(json.dumps(reduced["external"], indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
