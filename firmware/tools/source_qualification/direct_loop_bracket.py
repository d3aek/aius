#!/usr/bin/env python3
"""Capture and analyze a bounded optical S/PDIF self-loop on a USB source.

Plays a validation tone out of a USB audio interface whose optical output
is looped by fibre into its own optical input, captures the return, and
reports isolated impulses, dropouts, clipping and crossing cadence. This
is the CM6206 direct-loop discriminator bracket (see
docs/reference/cm6206-playback-corruption.md) made reusable for
qualifying any candidate source; pair it with loop_refine.py for the
stereo-identity, cadence and bit-exact checks.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import time
import wave
from pathlib import Path

import numpy as np


RATE = 44100
CHANNELS = 2
SAMPLE_WIDTH = 2
IMPULSE_THRESHOLD = 1000
BLOCK = 128


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


def read_wav(path: Path, expected_channels: int = CHANNELS
             ) -> tuple[np.ndarray, dict]:
    with wave.open(str(path), "rb") as capture:
        geometry = {
            "channels": capture.getnchannels(),
            "sample_rate_hz": capture.getframerate(),
            "sample_width_bytes": capture.getsampwidth(),
            "frames": capture.getnframes(),
        }
        if (
            geometry["channels"],
            geometry["sample_rate_hz"],
            geometry["sample_width_bytes"],
        ) != (expected_channels, RATE, SAMPLE_WIDTH):
            raise RuntimeError(f"unexpected WAV geometry: {geometry}")
        raw = capture.readframes(capture.getnframes())
    samples = np.frombuffer(raw, dtype="<i2").reshape(-1, expected_channels)
    return samples.astype(np.int32), geometry


def active_bounds(samples: np.ndarray) -> tuple[int, int]:
    count = len(samples) // BLOCK
    blocks = samples[:count * BLOCK].reshape(count, BLOCK, CHANNELS)
    rms = np.sqrt(np.mean(blocks.astype(np.float64) ** 2, axis=(1, 2)))
    active = np.flatnonzero(rms >= 100.0)
    if not len(active):
        raise RuntimeError("capture has no active tone")
    start = int(active[0]) * BLOCK
    end = min(len(samples), (int(active[-1]) + 1) * BLOCK)
    return start, end


def true_runs(mask: np.ndarray) -> list[tuple[int, int]]:
    padded = np.pad(mask.astype(np.int8), (1, 1))
    edges = np.diff(padded)
    starts = np.flatnonzero(edges == 1)
    ends = np.flatnonzero(edges == -1)
    return [(int(start), int(end)) for start, end in zip(starts, ends)]


def isolated_impulses(samples: np.ndarray, start: int,
                      end: int) -> list[dict]:
    into = np.abs(samples[1:-1].astype(np.int64) - samples[:-2])
    out = np.abs(samples[2:].astype(np.int64) - samples[1:-1])
    hits = np.argwhere(
        (into > IMPULSE_THRESHOLD) & (out > IMPULSE_THRESHOLD)
    )
    events = []
    for frame0, channel0 in hits:
        frame = int(frame0) + 1
        channel = int(channel0)
        if frame < start or frame >= end:
            continue
        events.append({
            "frame": frame,
            "active_frame": frame - start,
            "time_s": frame / RATE,
            "active_time_s": (frame - start) / RATE,
            "channel": channel,
            "previous": int(samples[frame - 1, channel]),
            "value": int(samples[frame, channel]),
            "next": int(samples[frame + 1, channel]),
            "into_delta": int(
                samples[frame, channel] - samples[frame - 1, channel]),
            "out_delta": int(
                samples[frame + 1, channel] - samples[frame, channel]),
        })
    return events


def cadence(samples: np.ndarray, start: int, end: int) -> list[dict]:
    result = []
    for channel in range(CHANNELS):
        values = samples[start:end, channel]
        crossings = np.flatnonzero(
            (values[:-1] <= 0) & (values[1:] > 0)
        ) + 1
        if len(crossings) >= 2:
            span = int(crossings[-1] - crossings[0])
            frequency = (
                (len(crossings) - 1) * RATE / span if span else None
            )
            intervals = np.diff(crossings)
            result.append({
                "channel": channel,
                "positive_crossings": int(len(crossings)),
                "estimated_hz": frequency,
                "interval_min_frames": int(intervals.min()),
                "interval_max_frames": int(intervals.max()),
                "interval_median_frames": float(np.median(intervals)),
            })
        else:
            result.append({
                "channel": channel,
                "positive_crossings": int(len(crossings)),
                "estimated_hz": None,
            })
    return result


def analyze(samples: np.ndarray, geometry: dict,
            active_start: int, active_end: int) -> dict:
    active = samples[active_start:active_end]
    zero_runs = true_runs(np.all(active == 0, axis=1))
    adjacent = np.abs(np.diff(active.astype(np.int64), axis=0))
    large_edges = np.argwhere(adjacent > IMPULSE_THRESHOLD)
    impulses = isolated_impulses(samples, active_start, active_end)
    clipped_positive = np.argwhere(active == 32767)
    clipped_negative = np.argwhere(active == -32768)
    return {
        "geometry": geometry,
        "total_duration_s": len(samples) / RATE,
        "active_start_frame": active_start,
        "active_end_frame_exclusive": active_end,
        "active_frames": len(active),
        "active_duration_s": len(active) / RATE,
        "isolated_impulse_threshold": IMPULSE_THRESHOLD,
        "isolated_impulse_count": len(impulses),
        "isolated_impulses": impulses,
        "adjacent_jump_count_over_threshold": int(len(large_edges)),
        "adjacent_jump_max": int(adjacent.max()) if adjacent.size else None,
        "zero_run_count": len(zero_runs),
        "longest_zero_run_frames": max(
            (end - start for start, end in zero_runs),
            default=0,
        ),
        "zero_runs_at_least_2_frames": sum(
            end - start >= 2 for start, end in zero_runs),
        "zero_runs_at_least_16_frames": sum(
            end - start >= 16 for start, end in zero_runs),
        "zero_runs_at_least_256_frames": sum(
            end - start >= 256 for start, end in zero_runs),
        "positive_clip_sample_count": int(len(clipped_positive)),
        "negative_clip_sample_count": int(len(clipped_negative)),
        "peak_abs": int(np.abs(active.astype(np.int64)).max()),
        "cadence": cadence(samples, active_start, active_end),
    }


def main() -> int:
    global RATE
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", required=True)
    parser.add_argument("--tone", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--rate", type=int, default=RATE)
    parser.add_argument("--playback-channels", type=int, default=CHANNELS)
    parser.add_argument("--seconds", type=int, default=125)
    args = parser.parse_args()
    RATE = args.rate
    args.output.mkdir(parents=True, exist_ok=True)

    source_all, source_geometry = read_wav(
        args.tone, args.playback_channels)
    source = source_all[:, :CHANNELS]
    source_summary = analyze(source, source_geometry, 0, len(source))
    write_json(args.output / "source-validation.json", source_summary)

    capture_path = args.output / "direct-return.wav"
    capture_log_path = args.output / "arecord.log"
    playback_log_path = args.output / "aplay.log"
    capture_log = capture_log_path.open("wb")
    playback_log = playback_log_path.open("wb")
    capture = None
    playback = None
    started_monotonic = time.monotonic()
    capture_rc = None
    playback_rc = None
    try:
        capture = subprocess.Popen([
            "arecord",
            "-D", args.device,
            "-f", "S16_LE",
            "-r", str(RATE),
            "-c", str(CHANNELS),
            "-d", str(args.seconds),
            str(capture_path),
        ], stdout=capture_log, stderr=subprocess.STDOUT)
        time.sleep(0.1)
        playback = subprocess.Popen([
            "aplay",
            "-D", args.device,
            "-d", str(args.seconds),
            str(args.tone),
        ], stdout=playback_log, stderr=subprocess.STDOUT)
        capture_rc = capture.wait(timeout=args.seconds + 15)
        playback_rc = playback.wait(timeout=args.seconds + 15)
    finally:
        stop_process(capture)
        stop_process(playback)
        capture_log.close()
        playback_log.close()
    elapsed = time.monotonic() - started_monotonic
    if capture_rc != 0 or playback_rc != 0:
        raise RuntimeError(
            f"ALSA failed: arecord={capture_rc}, aplay={playback_rc}")

    returned, returned_geometry = read_wav(capture_path)
    active_start, active_end = active_bounds(returned)
    returned_summary = analyze(
        returned, returned_geometry, active_start, active_end)
    arecord_log = capture_log_path.read_text(
        encoding="utf-8", errors="replace")
    aplay_log = playback_log_path.read_text(
        encoding="utf-8", errors="replace")
    summary = {
        "device": args.device,
        "requested_capture_seconds": args.seconds,
        "elapsed_wall_seconds": elapsed,
        "arecord_returncode": capture_rc,
        "aplay_returncode": playback_rc,
        "arecord_log": arecord_log,
        "aplay_log": aplay_log,
        "source": source_summary,
        "returned": returned_summary,
        "active_duration_at_least_120_seconds": (
            returned_summary["active_duration_s"] >= 120.0),
        "impulse_class_reproduced": (
            returned_summary["isolated_impulse_count"] > 0),
    }
    write_json(args.output / "summary.json", summary)
    print(json.dumps({
        "arecord_returncode": capture_rc,
        "aplay_returncode": playback_rc,
        "active_duration_s": returned_summary["active_duration_s"],
        "isolated_impulse_count": (
            returned_summary["isolated_impulse_count"]),
        "adjacent_jump_count_over_threshold": (
            returned_summary["adjacent_jump_count_over_threshold"]),
        "longest_zero_run_frames": (
            returned_summary["longest_zero_run_frames"]),
        "peak_abs": returned_summary["peak_abs"],
    }, indent=2), flush=True)
    if returned_summary["active_duration_s"] < 120.0:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
