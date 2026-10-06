#!/usr/bin/env python3
"""Capture the board's optical S/PDIF output and reduce it to the
external-observation JSON that aius_rate_bench.py's sample-rate gates read.

Three acceptance gates assert properties of the board's actual optical
output that HTTP /api/status cannot self-report:

    pcm-120s   -> {"audio_rate_hz", "continuous_output"}
    transitions-> {"old_rate_output_frames"}
    waveform   -> {"capture_frame_gaps", "discontinuities_outside_invalid",
                   "invalid_intervals_frames", "correlated_trim_updates",
                   "correlated_invalid_windows"}

This tool records board optical OUT -> U24 optical IN via ALSA `arecord`,
drives the scenario stimulus via `aplay`, polls /api/status in lockstep to
build the correlation series, reduces the waveform to those exact fields,
and writes <output>/external-observation.json plus a full
<output>/capture-analysis.json of the supporting evidence.

Capture chassis (ALSA orchestration, WAV reader, zero-run primitive)
mirrors ../source_qualification/direct_loop_bracket.py; the gate reducers
and their JSON schema are new and unrelated to that tool's summary.json.

Four design choices are pinned by the current bench hardware and recorded
in docs/plans/aius-rate-gate-capture-tool.md:

  - Gate 2's rate is reported as audio_rate_hz, INFERRED from the captured
    tone frequency by the physical model R_out = 997 * R_capture / f_meas.
    The U24 XL exposes no IEC958 capture-status control (verified via
    `amixer -c0 controls`), so a true channel-status decode is not available
    on this interface. The raw estimate is snapped to the nearest NOMINAL_RATES
    entry within NOMINAL_RATE_TOLERANCE; both values are reported
    (audio_rate_hz_raw, audio_rate_hz).

  - That inference is SELF-REFERENTIAL whenever arecord is opened at the
    rate under test. The U24 capture clock locks to the carrier under test,
    so f_meas == 997 Hz by construction and the reported rate is just the
    rate arecord was opened at. The evidence then says so explicitly
    (self_referential: true, audio_rate_hz_source: "capture-clock-locked"),
    so a gate-2 pass is never read as an independent measurement.

  - Capture<->status time correlation uses BOUNDED LOCAL ALIGNMENT around
    each status-reported counter increment, not a global frame->wall-time
    map (which accumulates clock-drift error over a 600 s run). The time
    origin is the moment arecord starts producing frames, NOT the start of
    the run: the stimulus and the board's rate lock precede arecord by up
    to lock_budget_s, and folding that wait into the map offsets every
    status increment by many seconds. See correlate_counter_events and
    capture_start_offset_s.

  - The discontinuity detector is derived from the MEASURED stimulus, never
    from a fixed LSB floor. The documented +-410 tone_gen stimulus has a
    peak adjacent-sample slope of ~54 LSB at 997 Hz / 48 kHz, so any
    absolute threshold above that is blind to every artifact by
    construction. See detector_thresholds.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import subprocess
import sys
import time
import wave
from datetime import datetime, timezone
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
# The validation tone; the output rate is inferred against it.
TONE_HZ = 997.0
# Bounded-alignment half-window, in seconds, added on each side of a
# status increment's [t_prev, t_curr] interval. Covers poll jitter plus the
# board's worst-case output-clock drift against the host capture clock.
ALIGNMENT_MARGIN_S = 1.5
# Nominal S/PDIF rates the inferred audio rate snaps to, and the fractional
# tolerance for that snap. A raw estimate further than this from every
# nominal rate is reported as None rather than silently rounded.
NOMINAL_RATES = (32000, 44100, 48000, 88200, 96000)
NOMINAL_RATE_TOLERANCE = 0.02

# --- Discontinuity-detector geometry -------------------------------------
# Every threshold below is a multiple of the tone's own expected peak
# adjacent-sample slope S = 2*A*sin(pi*f/Fs), computed from the measured
# amplitude A. At the documented A=410 stimulus, S is ~54 LSB.
#
#   JUMP_SLOPE_FACTOR    a spike/splice edge must exceed this many S.
#   HELD_SLOPE_FRACTION  |first difference| below this fraction of S counts
#                        as "not moving"; a sine only sits there for ~2
#                        samples per half cycle, so a run is a held block.
#   HELD_MIN_FRAMES      shortest such run reported (>> the ~2-sample
#                        natural dwell at the sine's turning points).
#   PHASE_SLIP_FRACTION  a sample slip steps the demodulated tone phase by
#                        2*pi*f/Fs; half of that is the detection floor.
JUMP_SLOPE_FACTOR = 1.6
JUMP_ABSOLUTE_FLOOR = 4.0
HELD_SLOPE_FRACTION = 0.08
HELD_MIN_FRAMES = 8
PHASE_SLIP_FRACTION = 0.5

# arecord writes the 44-byte WAV header before the first captured frame, so
# a larger file is the observable "frames are flowing now" edge.
WAV_HEADER_BYTES = 44
# Captured duration may fall this far short of the requested duration before
# the capture is declared failed (arecord's own start-up rounding).
CAPTURE_SHORTFALL_TOLERANCE_S = 0.5
ALSA_XRUN_RE = re.compile(r"overrun|xrun", re.IGNORECASE)

# Evidence artifacts live under firmware/test/evidence/ (gitignored) so a
# run's digest can be cited from a plan without committing the capture.
DEFAULT_EVIDENCE_ROOT = (
    Path(__file__).resolve().parents[2] / "test" / "evidence" /
    "rate-gate-capture")


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
    reducers ignore the aplay lead-in and trailing silence. The gate is 100
    LSB RMS: the documented +-410 tone_gen stimulus reads ~290 RMS, so it
    clears the gate with margin while a near-silent lead-in does not."""
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


def stimulus_amplitude(active: np.ndarray) -> float:
    """Peak amplitude of the captured tone, from its RMS (a sine's peak is
    sqrt(2) x RMS). Robust where a max() would key on one spike."""
    if not active.size:
        return 0.0
    rms = float(np.sqrt(np.mean(active.astype(np.float64) ** 2)))
    return float(math.sqrt(2.0) * rms)


def expected_peak_slope(amplitude: float, capture_rate: int,
                        tone_hz: float = TONE_HZ) -> float:
    """Largest adjacent-sample difference a clean tone of this amplitude can
    produce: S = 2*A*sin(pi*f/Fs). Everything the discontinuity detector
    thresholds on is a multiple of this, so the detector tracks the actual
    stimulus rather than an absolute LSB floor that the documented +-410
    stimulus never reaches."""
    return float(2.0 * amplitude * math.sin(math.pi * tone_hz / capture_rate))


def detector_thresholds(active: np.ndarray, capture_rate: int,
                        tone_hz: float = TONE_HZ) -> dict:
    """Every discontinuity threshold, derived from the measured stimulus.
    Emitted verbatim into the evidence so a reader can see what the detector
    was actually able to see."""
    amplitude = stimulus_amplitude(active)
    slope = expected_peak_slope(amplitude, capture_rate, tone_hz)
    period = max(2, int(round(capture_rate / tone_hz)))
    return {
        "stimulus_amplitude": amplitude,
        "expected_peak_slope": slope,
        "jump_threshold": max(JUMP_SLOPE_FACTOR * slope, JUMP_ABSOLUTE_FLOOR),
        "held_slope_floor": HELD_SLOPE_FRACTION * slope,
        "held_min_frames": HELD_MIN_FRAMES,
        "phase_window_frames": period,
        "phase_slip_threshold_rad":
            PHASE_SLIP_FRACTION * 2.0 * math.pi * tone_hz / capture_rate,
        "detector_tone_hz": tone_hz,
    }


def _runs_over(mask: np.ndarray, min_length: int) -> list[tuple[int, int]]:
    padded = np.pad(mask.astype(np.int8), (1, 1))
    edges = np.diff(padded)
    starts = np.flatnonzero(edges == 1)
    ends = np.flatnonzero(edges == -1)
    return [
        (int(start), int(end))
        for start, end in zip(starts, ends)
        if end - start >= min_length
    ]


def held_runs(jump: np.ndarray, slope_floor: float,
              min_frames: int = HELD_MIN_FRAMES) -> list[tuple[int, int]]:
    """Frame spans over which the signal does not move: a run of near-zero
    first difference against a tone whose expected slope is nonzero. This is
    the signature of a sample-and-hold / repeated PCM block, which no
    amplitude threshold can see (a held block's interior is flat, and its
    edges are only ~2 x the tone's own peak slope at +-410)."""
    if not jump.size:
        return []
    runs = _runs_over(jump <= slope_floor, min_frames)
    # jump[i] compares frames i and i+1, so a flat jump span [a, b) is a
    # held frame span [a+1, b+1).
    return [(start + 1, end + 1) for start, end in runs]


def tone_phase(mono: np.ndarray, capture_rate: int,
               tone_hz: float = TONE_HZ) -> tuple[np.ndarray, int]:
    """Unwrapped phase of the tone against a fitted reference at tone_hz.
    Demodulate to baseband and box-average one tone period; the residual
    angle is the tone's phase, and a dropped or repeated sample steps it by
    2*pi*f/Fs per slipped sample regardless of where in the cycle it lands
    (which is exactly why an amplitude threshold cannot see a slip near the
    waveform's turning points)."""
    window = max(2, int(round(capture_rate / tone_hz)))
    index = np.arange(len(mono), dtype=np.float64)
    reference = np.exp(-2j * math.pi * tone_hz * index / capture_rate)
    kernel = np.ones(window, dtype=np.float64) / window
    baseband = np.convolve(mono.astype(np.float64) * reference, kernel,
                           mode="same")
    return np.unwrap(np.angle(baseband)), window


def phase_slips(mono: np.ndarray, capture_rate: int, threshold_rad: float,
                tone_hz: float = TONE_HZ) -> list[int]:
    """Frame indexes where the fitted tone phase steps by more than
    threshold_rad over one tone period. The baseline is the median step, so
    a steady frequency offset (a capture read at the wrong rate) does not
    register; only a local phase discontinuity does."""
    phase, window = tone_phase(mono, capture_rate, tone_hz)
    if len(phase) <= 2 * window:
        return []
    delta = phase[window:] - phase[:-window]
    baseline = float(np.median(delta))
    hits = np.flatnonzero(np.abs(delta - baseline) > threshold_rad)
    return [int(hit) + window // 2 for hit in hits]


def collapse_events(frames: list[int], gap: int) -> list[int]:
    """One index per contiguous group: several detectors fire on the same
    artifact (a spike is both an amplitude jump and a phase step), and a
    single artifact must count once."""
    events: list[int] = []
    for frame in sorted(frames):
        if events and frame - events[-1] <= gap:
            continue
        events.append(frame)
    return events


def discontinuities(active: np.ndarray, capture_rate: int,
                    exclude: list[tuple[int, int]], tone_hz: float = TONE_HZ
                    ) -> tuple[list[int], dict]:
    """Active-frame indexes of waveform discontinuities, plus the evidence
    describing what the detector was thresholded at.

    Three signatures, all derived from the measured stimulus:

      - a HELD block (repeated / sample-and-hold PCM) as a run of near-zero
        first difference against a known-nonzero expected slope;
      - a SPIKE / splice edge as an adjacent jump over JUMP_SLOPE_FACTOR x
        the tone's own peak slope;
      - a SAMPLE SLIP as a step in the fitted tone's phase.

    Frames inside (or within two tone periods of) a supplied zero-run span
    are excluded: those edges are jumps into and out of declared silence,
    and the phase demodulator has no valid data there. The first and last
    two periods of the capture are excluded for the same reason."""
    thresholds = detector_thresholds(active, capture_rate, tone_hz)
    total = len(active)
    window = thresholds["phase_window_frames"]
    evidence = dict(thresholds)
    if total < 4 * window:
        evidence["held_runs"] = []
        evidence["detector"] = "amplitude-slope + held-run + phase-slip"
        return [], evidence

    jump = np.abs(np.diff(active.astype(np.int64), axis=0)).max(axis=1)
    blocked = np.zeros(total, dtype=bool)
    blocked[:2 * window] = True
    blocked[total - 2 * window:] = True
    for start, end in exclude:
        blocked[max(0, start - 2 * window):min(total, end + 2 * window)] = True

    held = [
        run for run in held_runs(jump, thresholds["held_slope_floor"],
                                 thresholds["held_min_frames"])
        if not blocked[min(run[0], total - 1)]
    ]
    for start, end in held:
        blocked[max(0, start - 2 * window):min(total, end + 2 * window)] = True

    spikes = [int(index) + 1
              for index in np.flatnonzero(jump > thresholds["jump_threshold"])]
    mono = active.astype(np.float64).mean(axis=1)
    slips = phase_slips(mono, capture_rate,
                        thresholds["phase_slip_threshold_rad"], tone_hz)
    candidates = {
        frame for frame in spikes + slips
        if 0 <= frame < total and not blocked[frame]
    }
    candidates.update(start for start, _end in held)
    # One artifact fires several detectors a fraction of a tone period
    # apart (the phase estimate is smeared by its own demodulation window),
    # so collapse over two periods.
    events = collapse_events(sorted(candidates), 2 * window)

    evidence["held_runs"] = [[start, end] for start, end in held]
    evidence["held_run_count"] = len(held)
    evidence["detector"] = "amplitude-slope + held-run + phase-slip"
    return events, evidence


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


def snap_nominal_rate(raw_rate: float | None,
                      tolerance: float = NOMINAL_RATE_TOLERANCE
                      ) -> int | None:
    """Nearest NOMINAL_RATES entry within a fractional tolerance, else None.
    Reporting None rather than a rounded raw estimate keeps gate 2's exact
    equality check from passing on a rate nothing actually ran at."""
    if raw_rate is None or not math.isfinite(raw_rate) or raw_rate <= 0:
        return None
    nearest = min(NOMINAL_RATES, key=lambda rate: abs(raw_rate - rate))
    if abs(raw_rate - nearest) <= tolerance * nearest:
        return int(nearest)
    return None


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


APPLIED_STAMP_KEYS = ("trim_applied_at_ms", "fill_applied_at_ms")


def applied_write_intervals(status_series: list[dict]) -> list[dict]:
    """Intervals over which a trim or fill write was applied, read from the
    two millis() applied-at stamps (a stamp change is one write; trim_updates
    counts commits, including no-write ones, and cannot see fill writes).
    A series without either stamp falls back to trim_updates."""
    if not any(key in entry for entry in status_series
               for key in APPLIED_STAMP_KEYS):
        return counter_increment_intervals(status_series, "trim_updates")
    intervals: list[dict] = []
    for previous, current in zip(status_series, status_series[1:]):
        for key in APPLIED_STAMP_KEYS:
            if key in previous and key in current and (
                    int(current[key]) != int(previous[key])):
                intervals.append({
                    "t_prev": float(previous["elapsed_s"]),
                    "t_curr": float(current["elapsed_s"]),
                })
    return intervals


def status_time_to_frame(elapsed_s: float, capture_rate: int,
                         capture_start_offset_s: float = 0.0,
                         active_start_frame: int = 0) -> float:
    """Status-domain wall time -> active-frame index.

    Two origins separate the two domains and BOTH must be subtracted:

      capture_start_offset_s  the run starts the stimulus and waits for the
                              board to lock (up to lock_budget_s) before
                              arecord opens, so capture frame 0 is that far
                              into the status series.
      active_start_frame      active_bounds() trims the capture's silent
                              lead-in, so active frame 0 is that far into
                              the capture.

    Dropping either offsets every status increment by seconds and gate 7's
    bounded local alignment can never match."""
    return ((elapsed_s - capture_start_offset_s) * capture_rate
            - active_start_frame)


def _window_frames(interval: dict, capture_rate: int, total_frames: int,
                   capture_start_offset_s: float = 0.0,
                   active_start_frame: int = 0) -> tuple[int, int]:
    lo = int(status_time_to_frame(
        interval["t_prev"] - ALIGNMENT_MARGIN_S, capture_rate,
        capture_start_offset_s, active_start_frame))
    hi = int(status_time_to_frame(
        interval["t_curr"] + ALIGNMENT_MARGIN_S, capture_rate,
        capture_start_offset_s, active_start_frame))
    return max(0, lo), min(total_frames, hi)


def correlate_counter_events(intervals: list[dict], event_frames: list[int],
                             capture_rate: int, total_frames: int,
                             capture_start_offset_s: float = 0.0,
                             active_start_frame: int = 0
                             ) -> tuple[int, list[int]]:
    """Bounded local alignment: greedily assign each status-counter unit
    increment to the nearest unmatched capture event whose frame lies inside
    that increment's drift-padded wall-time window. Returns the number of
    matched increments and the indexes of matched events."""
    matched_events: list[int] = []
    used = [False] * len(event_frames)
    correlated = 0
    for interval in intervals:
        lo, hi = _window_frames(interval, capture_rate, total_frames,
                                capture_start_offset_s, active_start_frame)
        centre = status_time_to_frame(
            0.5 * (interval["t_prev"] + interval["t_curr"]), capture_rate,
            capture_start_offset_s, active_start_frame)
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
    """Gate 2 (pcm-120s) fields plus supporting evidence.

    The physical model is R_out = 997 * R_capture / f_meas: frames the board
    rendered at R_out, read back at R_capture, carry 997 * R_capture / R_out
    Hz, so the output rate is recovered by inverting that. (The inverse
    form, expected_rate * f_meas / 997, reports 52245 Hz for a 44.1 kHz
    stream read at 48 kHz.)

    The estimate is SELF-REFERENTIAL whenever arecord is opened at the rate
    under test: the U24 capture clock locks to the carrier under test, so
    f_meas is 997 Hz by construction and the answer is the rate arecord was
    opened at, not an independent measurement. The evidence says so."""
    runs = zero_runs(active)
    measured = estimate_hz(active, capture_rate)
    raw_rate = None
    if measured is not None and measured > 0:
        raw_rate = TONE_HZ * capture_rate / measured
    snapped = snap_nominal_rate(raw_rate)
    self_referential = int(capture_rate) == int(expected_rate)
    if self_referential:
        source = "capture-clock-locked"
        note = ("audio_rate_hz is capture-clock-locked, not independently "
                "measured: arecord was opened at the rate under test and the "
                "U24 capture clock locks to the carrier under test, so the "
                "measured tone is 997 Hz by construction and the reported "
                "rate is the rate arecord was opened at. An independent "
                "measurement needs a capture rate different from the "
                "expected output rate, or a channel-status decode.")
    else:
        source = "inferred-from-tone"
        note = ("audio_rate_hz is inferred from the captured tone frequency "
                f"at a capture rate ({capture_rate} Hz) different from the "
                f"expected output rate ({expected_rate} Hz), so the tone "
                "frequency carries real information about the output rate.")
    return {
        "external": {
            "audio_rate_hz": snapped,
            "continuous_output": bool(len(runs) == 0),
        },
        "evidence": {
            "audio_rate_hz_source": source,
            "audio_rate_hz_raw": raw_rate,
            "audio_rate_hz": snapped,
            "capture_rate_hz": int(capture_rate),
            "expected_rate_hz": int(expected_rate),
            "self_referential": bool(self_referential),
            "rate_measurement_note": note,
            "rate_model": "R_out = 997 * R_capture / f_measured",
            "nominal_rates_hz": list(NOMINAL_RATES),
            "nominal_rate_tolerance": NOMINAL_RATE_TOLERANCE,
            "measured_tone_hz": measured,
            "reference_tone_hz": TONE_HZ,
            "stimulus_amplitude": stimulus_amplitude(active),
            "zero_run_count": len(runs),
            "zero_run_lengths": [end - start for start, end in runs],
        },
    }


def reduce_waveform(active: np.ndarray, capture_rate: int,
                    status_series: list[dict],
                    capture_start_offset_s: float = 0.0,
                    active_start_frame: int = 0) -> dict:
    """Gate 7 (waveform) fields via bounded local alignment of capture
    events against status invalid_windows (full apply) increments. Trim and
    fill writes never open a match window. correlated_trim_updates keeps its
    contract name but counts jumps matched to a full apply; the stamp-derived
    write count is evidence only (status_trim_increments)."""
    total = len(active)
    runs = zero_runs(active)
    run_starts = [start for start, _ in runs]

    invalid_intervals = counter_increment_intervals(
        status_series, "invalid_windows")
    # Only full applies (invalid_windows) open jump-match windows. Trim and
    # fill writes are carrier-continuous numerator steps: at fill cadence
    # their windows would cover the whole capture and excuse real glitches.
    write_intervals = applied_write_intervals(status_series)

    correlated_invalid, matched_runs = correlate_counter_events(
        invalid_intervals, run_starts, capture_rate, total,
        capture_start_offset_s, active_start_frame)
    matched_run_set = set(matched_runs)
    invalid_lengths = [
        runs[index][1] - runs[index][0] for index in sorted(matched_run_set)
    ]
    capture_frame_gaps = len(runs) - len(matched_run_set)

    jumps, detector_evidence = discontinuities(active, capture_rate, runs)
    correlated_trim, matched_jumps = correlate_counter_events(
        invalid_intervals, jumps, capture_rate, total,
        capture_start_offset_s, active_start_frame)
    discontinuities_outside = len(jumps) - len(matched_jumps)

    evidence = {
        "zero_run_count": len(runs),
        "zero_run_lengths": [end - start for start, end in runs],
        "discontinuity_frames": jumps,
        "status_invalid_increments": len(invalid_intervals),
        "status_trim_increments": len(write_intervals),
        "correlation": "bounded-local-alignment",
        "alignment_margin_s": ALIGNMENT_MARGIN_S,
        "capture_start_offset_s": float(capture_start_offset_s),
        "active_start_frame": int(active_start_frame),
    }
    evidence.update(detector_evidence)
    return {
        "external": {
            "capture_frame_gaps": int(capture_frame_gaps),
            "discontinuities_outside_invalid": int(discontinuities_outside),
            "invalid_intervals_frames": [int(x) for x in invalid_lengths],
            "correlated_trim_updates": int(correlated_trim),
            "correlated_invalid_windows": int(correlated_invalid),
        },
        "evidence": evidence,
    }


def reduce_transitions(active: np.ndarray, capture_rate: int,
                       status_series: list[dict], tolerance_hz: float = 20.0,
                       capture_start_offset_s: float = 0.0,
                       active_start_frame: int = 0) -> dict:
    """Gate 4 (transitions) old_rate_output_frames from an ALSA capture:
    after each stream-rate change reported in the status series, count
    captured frames whose local tone matches the OLD rate's expected read
    frequency rather than the new one.

    Frames the board rendered at R_out, read back at R_capture, carry
    997 * R_capture / R_out Hz. So stale 48 kHz frames read at 44.1 kHz show
    997 * 44100/48000 = 916 Hz, not 1085 Hz -- the inverse form is what a
    48 kHz READER of 44.1 kHz frames sees, which is the other direction.

    NOTE: this path is only usable when the capture clock does not follow
    the carrier. On the U24 the fixed-rate arecord errors out on a rate
    change, so gate 4 is measured through the flight recorder instead --
    see recorder_transitions.py."""
    boundaries = []
    for previous, current in zip(status_series, status_series[1:]):
        if current["stream_rate_hz"] != previous["stream_rate_hz"]:
            boundaries.append({
                "frame": int(status_time_to_frame(
                    current["elapsed_s"], capture_rate,
                    capture_start_offset_s, active_start_frame)),
                "old_rate": int(previous["stream_rate_hz"]),
                "new_rate": int(current["stream_rate_hz"]),
            })
    # ~20 ms holds ~20 tone periods, enough crossings for a few-Hz estimate;
    # scan the first 250 ms past each boundary for stale old-rate output.
    window = max(2, capture_rate // 50)
    post = int(0.25 * capture_rate)
    old_rate_frames = 0
    for boundary in boundaries:
        old_hz = TONE_HZ * capture_rate / boundary["old_rate"]
        new_hz = TONE_HZ * capture_rate / boundary["new_rate"]
        boundary["old_rate_read_hz"] = old_hz
        boundary["new_rate_read_hz"] = new_hz
        pos = max(0, boundary["frame"])
        limit = min(len(active), max(0, boundary["frame"]) + post)
        while pos + window <= limit:
            local = estimate_hz(active[pos:pos + window], capture_rate)
            pos += window
            if local is None:
                continue
            if abs(local - old_hz) < abs(local - new_hz) and (
                    abs(local - old_hz) < tolerance_hz):
                old_rate_frames += window
    return {
        "external": {"old_rate_output_frames": int(old_rate_frames)},
        "evidence": {
            "boundaries": boundaries,
            "post_boundary_window_frames": window,
            "post_boundary_scan_frames": post,
            "capture_start_offset_s": float(capture_start_offset_s),
            "active_start_frame": int(active_start_frame),
            "detection_model": "read-frequency 997 * R_capture / R_out",
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
            **{key: int(clock[key]) for key in APPLIED_STAMP_KEYS
               + ("centre_writes", "fill_window_seq") if key in clock},
            "invalid_windows": int(clock["invalid_windows"]),
            "stream_rate_hz": int(clock["stream_rate_hz"]),
        })
    return series


def reduce_scenario(scenario: str, active: np.ndarray, capture_rate: int,
                    status_series: list[dict], expected_rate: int,
                    capture_start_offset_s: float = 0.0,
                    active_start_frame: int = 0) -> dict:
    if scenario == "pcm-120s":
        return reduce_pcm(active, capture_rate, expected_rate)
    if scenario == "transitions":
        return reduce_transitions(
            active, capture_rate, status_series,
            capture_start_offset_s=capture_start_offset_s,
            active_start_frame=active_start_frame)
    if scenario == "waveform":
        return reduce_waveform(active, capture_rate, status_series,
                               capture_start_offset_s, active_start_frame)
    raise RuntimeError(f"unknown scenario {scenario!r}")


def http_get(url: str, auth: str | None, attempts: int = 3
             ) -> tuple[str, dict]:
    """GET a JSON endpoint, retrying the board's transient one-client-at-a-
    time transport drops (not HTTP or JSON errors, which are real)."""
    import urllib.error
    import urllib.request

    last = None
    for attempt in range(attempts):
        try:
            request = urllib.request.Request(url)
            if auth:
                request.add_header("X-Aius-SD-Auth", auth)
            with urllib.request.urlopen(request, timeout=5.0) as response:
                raw = response.read().decode("utf-8")
            return raw, json.loads(raw)
        except (urllib.error.URLError, ConnectionError, TimeoutError) as error:
            last = error
            time.sleep(0.2 * (attempt + 1))
    raise RuntimeError(f"status poll failed after {attempts} attempts: {last}")


def events_url(status_url: str) -> str:
    return status_url.rsplit("/api/status", 1)[0] + "/api/events"


def wait_for_rate(status_url: str, auth: str | None, rate: int,
                  timeout_s: float) -> bool:
    """Poll until the board's carrier is locked at `rate` (its output S/PDIF
    then matches the arecord rate, so the U24 capture clock can lock). The
    board follows the U24 TX with latency, so opening arecord before this
    holds gives an immediate read I/O error."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            _raw, status = http_get(status_url, auth)
        except RuntimeError:
            time.sleep(0.5)
            continue
        clock = status["clock"]
        if clock.get("carrier_locked") and clock.get("stream_rate_hz") == rate:
            return True
        time.sleep(0.5)
    return False


def wait_for_capture_frames(path: Path, process: subprocess.Popen,
                            timeout_s: float = 10.0) -> float | None:
    """Monotonic instant at which arecord's WAV first grows past its header,
    i.e. when capture frames actually start. This -- not the start of the
    run -- is capture frame 0 in the status series' time base."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return None
        try:
            if path.stat().st_size > WAV_HEADER_BYTES:
                return time.monotonic()
        except OSError:
            pass
        time.sleep(0.02)
    return None


def parse_alsa_log(path: Path) -> tuple[int, list[str]]:
    """ALSA overrun/xrun notices arecord wrote to its log. arecord reports
    each as `overrun!!! (at least N ms long)` on stderr; every one is a
    stretch of the board's output the capture did not receive, so they are
    capture-domain gaps and must reach the gate rather than sit in a log
    nobody reads."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return 0, []
    lines = [line.strip() for line in text.splitlines()
             if ALSA_XRUN_RE.search(line)]
    return len(lines), lines[:20]


def capture_and_poll(args) -> tuple[Path, list[dict], dict]:
    """Drive the scenario stimulus + arecord and poll /api/status and
    /api/events in lockstep. Each poll keeps the raw JSON text so the run
    can be written back as a bench-format evidence file.

    Order matters for the board->U24 loop: start aplay first so the board
    follows to the target rate, wait for its carrier to lock there, and only
    then open arecord (whose capture clock locks to the board's output).

    Because of that ordering the run origin and the capture origin are up to
    lock_budget_s apart. `elapsed_s` in the poll series stays run-relative
    (it is what the bench's own time base means); the returned health record
    carries `capture_start_offset_s` so the reducers can map status time
    onto capture frames."""
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    capture_path = output / "capture.wav"
    capture_log_path = output / "arecord.log"
    capture_log = capture_log_path.open("wb")
    playback_log = (output / "aplay.log").open("wb")
    capture = None
    playback = None
    polls: list[dict] = []
    events_base = events_url(args.status_url)
    lock_budget_s = 20.0
    started = time.monotonic()
    capture_start_offset_s = 0.0
    capture_start_source = "process-start"
    try:
        if args.tone:
            playback = subprocess.Popen([
                "aplay", "-D", args.device,
                "-d", str(int(args.seconds + lock_budget_s + 5)),
                str(args.tone),
            ], stdout=playback_log, stderr=subprocess.STDOUT)
            if not wait_for_rate(args.status_url, args.auth,
                                 args.follow_rate, lock_budget_s):
                raise RuntimeError(
                    f"board did not lock to {args.follow_rate} Hz within "
                    f"{lock_budget_s:.0f}s of stimulus start")
        capture = subprocess.Popen([
            "arecord", "-D", args.device, "-f", "S16_LE",
            "-r", str(args.rate), "-c", str(CHANNELS),
            "-d", str(args.seconds), str(capture_path),
        ], stdout=capture_log, stderr=subprocess.STDOUT)
        first_frames_at = wait_for_capture_frames(capture_path, capture)
        if first_frames_at is None:
            capture_start_offset_s = time.monotonic() - started
            capture_start_source = "process-start (frames not observed)"
        else:
            capture_start_offset_s = first_frames_at - started
            capture_start_source = "first-captured-frame"
        first_raw, first = http_get(args.status_url, args.auth)
        cursor = int(first["event_sequence"])
        while capture.poll() is None:
            elapsed = time.monotonic() - started
            status_raw, status = http_get(args.status_url, args.auth)
            events_raw, events = http_get(
                f"{events_base}?cursor={cursor}", args.auth)
            cursor = int(events["next"])
            polls.append({
                "elapsed_s": elapsed,
                "status": status,
                "raw_status_json": status_raw,
                "raw_events_json": events_raw,
            })
            time.sleep(1.0)
        capture.wait(timeout=15)
        # aplay is sized to outlast the capture; stop it in finally rather
        # than waiting out its remaining stimulus tail.
    finally:
        returncode = None if capture is None else capture.poll()
        stop_process(capture)
        stop_process(playback)
        capture_log.close()
        playback_log.close()
    overruns, overrun_lines = parse_alsa_log(capture_log_path)
    health = {
        "arecord_returncode": returncode,
        "alsa_overruns": overruns,
        "alsa_overrun_lines": overrun_lines,
        "requested_seconds": float(args.seconds),
        "capture_start_offset_s": float(capture_start_offset_s),
        "capture_start_source": capture_start_source,
    }
    return capture_path, polls, health


def capture_failures(health: dict, geometry: dict) -> list[str]:
    """Hard capture faults that make the reduced fields untrustworthy. An
    arecord that died early or a WAV short of the requested duration means
    the gate observed less output than it claims to have observed."""
    failures: list[str] = []
    returncode = health.get("arecord_returncode")
    if returncode is None:
        failures.append("arecord-did-not-exit")
    elif returncode != 0:
        failures.append(f"arecord-exit-{returncode}")
    rate = geometry.get("sample_rate_hz") or 0
    captured_s = (geometry.get("frames", 0) / rate) if rate else 0.0
    health["captured_seconds"] = float(captured_s)
    requested = float(health.get("requested_seconds", 0.0))
    if requested and captured_s < requested - CAPTURE_SHORTFALL_TOLERANCE_S:
        failures.append(
            f"capture-short-{captured_s:.2f}s-of-{requested:.2f}s")
    return failures


def apply_capture_health(scenario: str, external: dict, health: dict,
                         failures: list[str]) -> dict:
    """Fold capture-domain faults into the gate fields so a broken capture
    cannot report a clean external observation.

    ALSA overruns are capture frame gaps by definition, so they add to
    `capture_frame_gaps`. A hard failure neutralises the gate-critical field
    of whichever scenario is running and records the marker."""
    overruns = int(health.get("alsa_overruns", 0))
    if scenario == "waveform":
        external["capture_frame_gaps"] = int(
            external.get("capture_frame_gaps", 0)) + overruns
    if failures:
        external["capture_failure"] = list(failures)
        if scenario == "pcm-120s":
            external["audio_rate_hz"] = None
            external["continuous_output"] = False
        elif scenario == "waveform":
            external["capture_frame_gaps"] = int(
                external.get("capture_frame_gaps", 0)) + len(failures)
        elif scenario == "transitions":
            external["old_rate_output_frames"] = None
    return external


def scenario_analysis_options(scenario: str, rate: int, expected_rate: int,
                              expect_build: str | None) -> dict:
    """The analysis_options a `run` would embed, so `analyze` reaches the
    same verdict from this tool's evidence file without extra CLI flags."""
    options = {
        "settle_s": 10.0,
        "max_follow_error_millihz": 1000,
        "max_slope_words_per_s": 0.25,
        "phase_samples": 2,
    }
    if scenario == "pcm-120s":
        options["rate_hz"] = expected_rate
    if expect_build:
        options["expect_build"] = expect_build
    return options


def write_evidence(path: Path, scenario: str, polls: list[dict],
                   analysis_options: dict, external_raw: str) -> None:
    """Bench-format evidence JSONL (schema 1): one metadata record then one
    sample record per poll, with the external observation embedded."""
    identity = {
        "build_revision": polls[0]["status"]["build_revision"],
        "build_utc": polls[0]["status"]["build_utc"],
    }
    lines = [json.dumps({
        "kind": "metadata",
        "schema_version": 1,
        "scenario": scenario,
        "build_identity": identity,
        "analysis_options": analysis_options,
        "raw_external_observation_json": external_raw,
    })]
    for poll in polls:
        lines.append(json.dumps({
            "kind": "sample",
            "elapsed_s": poll["elapsed_s"],
            "raw_status_json": poll["raw_status_json"],
            "raw_events_json": poll["raw_events_json"],
        }))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def evidence_digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def announce_evidence(path: Path) -> None:
    """Print the evidence file's absolute path and SHA-256 so a plan or a
    run log can cite the exact artifact a claim came from. Nothing else in
    this repo pins a live result to a byte sequence."""
    print(f"evidence: {path.resolve()}", flush=True)
    print(f"evidence-sha256: {evidence_digest(path)}", flush=True)


def default_output_dir(scenario: str) -> Path:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return DEFAULT_EVIDENCE_ROOT / f"{scenario}-{stamp}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="hw:0,0")
    parser.add_argument("--scenario", required=True,
                        choices=("pcm-120s", "transitions", "waveform"))
    parser.add_argument("--rate", type=int, default=48000,
                        help="arecord capture rate")
    parser.add_argument("--expected-rate", type=int,
                        help="gate 2 declared S/PDIF rate (default: --rate)")
    parser.add_argument("--follow-rate", type=int,
                        help="rate the board must lock to before arecord "
                             "opens (default: --rate)")
    parser.add_argument("--seconds", type=int, default=130)
    parser.add_argument("--status-url", default="http://192.0.2.66/api/status")
    parser.add_argument("--auth")
    parser.add_argument("--expect-build",
                        help="pin the gate to this exact build_revision")
    parser.add_argument("--tone", type=Path,
                        help="WAV stimulus to aplay; omitted for a passive "
                             "capture where the board is already sourcing")
    parser.add_argument("--output", type=Path,
                        help="evidence directory (default: a timestamped dir "
                             "under firmware/test/evidence/rate-gate-capture/)")
    args = parser.parse_args(argv)
    if args.expected_rate is None:
        args.expected_rate = args.rate
    if args.follow_rate is None:
        args.follow_rate = args.rate
    if args.output is None:
        args.output = default_output_dir(args.scenario)

    capture_path, polls, health = capture_and_poll(args)
    samples, geometry = read_wav(capture_path)
    failures = capture_failures(health, geometry)
    start, end = active_bounds(samples)
    active = samples[start:end]
    status_series = build_status_series(polls)
    reduced = reduce_scenario(
        args.scenario, active, args.rate, status_series, args.expected_rate,
        capture_start_offset_s=health["capture_start_offset_s"],
        active_start_frame=start)
    external = apply_capture_health(
        args.scenario, reduced["external"], health, failures)

    output = Path(args.output)
    external_raw = json.dumps(external)
    write_json(output / "external-observation.json", external)
    write_json(output / "capture-analysis.json", {
        "scenario": args.scenario,
        "geometry": geometry,
        "capture_rate_hz": args.rate,
        "expected_rate_hz": args.expected_rate,
        "active_start_frame": start,
        "active_end_frame_exclusive": end,
        "active_frames": int(end - start),
        "status_sample_count": len(status_series),
        "capture_health": health,
        "capture_failures": failures,
        "external": external,
        "evidence": reduced["evidence"],
    })
    if polls:
        options = scenario_analysis_options(
            args.scenario, args.rate, args.expected_rate, args.expect_build)
        write_evidence(output / "evidence.jsonl", args.scenario, polls,
                       options, external_raw)
        announce_evidence(output / "evidence.jsonl")
    print(json.dumps(external, indent=2, sort_keys=True))
    if failures:
        print(f"capture FAILED: {', '.join(failures)}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
