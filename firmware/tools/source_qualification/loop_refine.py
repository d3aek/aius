#!/usr/bin/env python3
"""Refined analysis of an optical self-loop capture.

Companion to direct_loop_bracket.py: computes the CM6206 companion-defect
metrics (stereo identity, crossing cadence against the source's two legal
intervals) plus a bit-exact alignment of the returned active region
against the source tone -- the strongest check a digital self-loop
allows. Requires the tone_gen.py source (integer frequency, so the tone
is exactly periodic with period `rate` frames and alignment is unique
modulo one second).
"""

from __future__ import annotations

import argparse
import json
import wave
from pathlib import Path

import numpy as np

BLOCK = 128


def read_wav(path: Path) -> tuple[np.ndarray, dict]:
    with wave.open(str(path), "rb") as capture:
        geometry = {
            "channels": capture.getnchannels(),
            "sample_rate_hz": capture.getframerate(),
            "sample_width_bytes": capture.getsampwidth(),
            "frames": capture.getnframes(),
        }
        raw = capture.readframes(capture.getnframes())
    samples = np.frombuffer(raw, dtype="<i2").reshape(
        -1, geometry["channels"])
    return samples.astype(np.int32), geometry


def active_bounds(samples: np.ndarray) -> tuple[int, int]:
    count = len(samples) // BLOCK
    blocks = samples[:count * BLOCK].reshape(count, BLOCK, 2)
    rms = np.sqrt(np.mean(blocks.astype(np.float64) ** 2, axis=(1, 2)))
    active = np.flatnonzero(rms >= 100.0)
    if not len(active):
        raise RuntimeError("capture has no active tone")
    start = int(active[0]) * BLOCK
    end = min(len(samples), (int(active[-1]) + 1) * BLOCK)
    return start, end


def stereo_identity(source: np.ndarray, active: np.ndarray) -> dict:
    unequal = active[:, 0] != active[:, 1]
    result = {
        "source_channels_are_identical": bool(
            np.array_equal(source[:, 0], source[:, 1])),
        "returned_unequal_stereo_frames": int(unequal.sum()),
        "returned_unequal_stereo_fraction": float(unequal.mean()),
    }
    hits = np.flatnonzero(unequal)
    if len(hits):
        first = int(hits[0])
        result["first_unequal_active_frame"] = first
        result["first_unequal_values"] = [
            int(active[first, 0]), int(active[first, 1])]
    return result


def cadence_refinement(active: np.ndarray, rate: int,
                       freq: float) -> list[dict]:
    low = int(rate // freq)
    high = low + 1
    rows = []
    for channel in range(2):
        values = active[:, channel]
        crossings = np.flatnonzero(
            (values[:-1] <= 0) & (values[1:] > 0)) + 1
        intervals = np.diff(crossings)
        rows.append({
            "channel": channel,
            "positive_crossings": int(len(crossings)),
            "legal_intervals_frames": [low, high],
            "intervals_outside_legal": int(
                ((intervals < low) | (intervals > high)).sum()),
            "minimum_interval_frames": (
                int(intervals.min()) if len(intervals) else None),
            "maximum_interval_frames": (
                int(intervals.max()) if len(intervals) else None),
        })
    return rows


def bit_exact_alignment(source: np.ndarray, active: np.ndarray,
                        rate: int) -> dict:
    """Align the returned active region against the source and count
    mismatching frames. The tone has exact period `rate` frames (997 and
    the rate are coprime), so alignment is unique modulo one second."""
    window = min(len(active), 4 * rate)
    src_mono = source[:, 0]
    ret_mono = active[:window, 0]
    best_offset = None
    best_mismatch = None
    for offset in range(rate):
        mism = int(
            (src_mono[offset:offset + window] != ret_mono).sum())
        if best_mismatch is None or mism < best_mismatch:
            best_mismatch = mism
            best_offset = offset
            if mism == 0:
                break
    span = min(len(active), len(src_mono) - best_offset)
    aligned_src = source[best_offset:best_offset + span]
    aligned_ret = active[:span]
    mismatch_frames = np.flatnonzero(
        np.any(aligned_src != aligned_ret, axis=1))
    detail = []
    for frame in mismatch_frames[:50]:
        frame = int(frame)
        detail.append({
            "active_frame": frame,
            "expected": [int(aligned_src[frame, 0]),
                         int(aligned_src[frame, 1])],
            "observed": [int(aligned_ret[frame, 0]),
                         int(aligned_ret[frame, 1])],
        })
    return {
        "alignment_window_frames": window,
        "alignment_offset_frames": int(best_offset),
        "alignment_window_mismatches": int(best_mismatch),
        "compared_frames": int(span),
        "mismatch_frame_count": int(len(mismatch_frames)),
        "bit_exact": bool(len(mismatch_frames) == 0),
        "first_mismatches": detail,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tone", type=Path, required=True)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--rate", type=int, required=True)
    parser.add_argument("--freq", type=float, default=997.0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    source, source_geometry = read_wav(args.tone)
    returned, returned_geometry = read_wav(args.capture)
    start, end = active_bounds(returned)
    active = returned[start:end]
    nonzero = np.flatnonzero(np.any(active != 0, axis=1))
    lead_trim = int(nonzero[0])
    trail_trim = int(len(active) - 1 - nonzero[-1])
    active = active[lead_trim:len(active) - trail_trim]
    start += lead_trim
    end -= trail_trim
    summary = {
        "tone": str(args.tone),
        "capture": str(args.capture),
        "source_geometry": source_geometry,
        "returned_geometry": returned_geometry,
        "active_start_frame": start,
        "edge_silence_trimmed_frames": [lead_trim, trail_trim],
        "active_frames": int(end - start),
        "active_duration_s": (end - start) / args.rate,
        "stereo_identity": stereo_identity(source, active),
        "cadence_refinement": cadence_refinement(
            active, args.rate, args.freq),
        "bit_exact_alignment": bit_exact_alignment(
            source, active, args.rate),
    }
    args.output.write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    print(json.dumps({
        "active_duration_s": summary["active_duration_s"],
        "unequal_stereo_frames": summary["stereo_identity"][
            "returned_unequal_stereo_frames"],
        "intervals_outside_legal": [
            row["intervals_outside_legal"]
            for row in summary["cadence_refinement"]],
        "bit_exact": summary["bit_exact_alignment"]["bit_exact"],
        "mismatch_frame_count": summary["bit_exact_alignment"][
            "mismatch_frame_count"],
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
