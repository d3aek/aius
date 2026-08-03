#!/usr/bin/env python3
"""Direct-loop latency calibration rows: source out -> source in, no Teensy.

The three matrix rows `direct-loop-pcm`, `direct-loop-ac3-carrier` and
`direct-loop-dts-carrier` (docs/plans/aius-e2e-latency.md, Phase 2) measure
the measurement path's own transport so the Teensy rows' raw round trips can
be decomposed. Per the plan's acceptance: no Teensy status gate, exact ALSA
parameters and verified linked start (linked_probe.collect), no XRUN, no
clipping, monotonic one-to-one association, transport min/median/p95/max,
affine residuals and a bootstrap 95% confidence interval.

PCM rows reuse bench.py's code stimulus and analyzer wholesale. Encoded rows
(`--mode iec`) play a caller-supplied IEC 61937 S16 stereo stimulus (AC-3 or
DTS carrier fixture) and one-to-one match playback and captured
`(Pa, Pc, payload_bits, payload_hash)` tuples, so a corrupted, dropped,
duplicated or reordered burst is a hard failure, never an averaged one.

Usage:
  direct_loop.py --mode pcm --device hw:0,0 --output <dir>
  direct_loop.py --mode iec --stimulus ac3.iec.wav --device hw:0,0 \
      --output <dir> --label ac3-carrier
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import wave

import numpy as np

import bench
from linked_probe import collect

RATE = 48000
CHANNELS = 2
PERIOD = 256
BUFFER = 4096
BOOTSTRAP_ROUNDS = 10000
BOOTSTRAP_SEED = b"aius-direct-loop-bootstrap-v1"
IEC_PA = 0xF872
IEC_PB = 0x4E1F


def bootstrap_median_ci(latencies: np.ndarray) -> dict:
    """95% bootstrap confidence interval of the median transport latency.

    Seeded deterministically so a re-analysis of retained evidence
    reproduces the recorded interval bit-for-bit."""
    if latencies.size == 0:
        raise RuntimeError("bootstrap needs at least one latency")
    rng = np.random.default_rng(
        int.from_bytes(hashlib.sha256(BOOTSTRAP_SEED).digest()[:8], "little")
    )
    samples = rng.choice(
        latencies, size=(BOOTSTRAP_ROUNDS, latencies.size), replace=True
    )
    medians = np.median(samples, axis=1)
    return {
        "median_frames": float(np.median(latencies)),
        "ci95_low_frames": float(np.percentile(medians, 2.5)),
        "ci95_high_frames": float(np.percentile(medians, 97.5)),
        "rounds": BOOTSTRAP_ROUNDS,
        "method": "seeded_bootstrap_of_median",
    }


def word_stream(samples: np.ndarray) -> np.ndarray:
    """S16 stereo frames -> the IEC 61937 16-bit word stream (L, R, L, R)."""
    if samples.ndim != 2 or samples.shape[1] != CHANNELS:
        raise RuntimeError("IEC stream must be stereo")
    return samples.astype("<i2").view("<u2").reshape(-1)


def parse_iec_bursts(samples: np.ndarray) -> list[dict]:
    """Every IEC 61937 burst in capture order.

    A tuple is (frame of Pa, Pc, payload_bits, payload sha256). Payload
    length is ceil(Pd bits / 16) words; a burst whose payload runs past the
    end of the stream is truncated capture and a hard failure."""
    words = word_stream(samples)
    bursts = []
    index = 0
    limit = len(words) - 3
    while index < limit:
        if words[index] != IEC_PA or words[index + 1] != IEC_PB:
            index += 1
            continue
        pc = int(words[index + 2])
        bits = int(words[index + 3])
        payload_words = (bits + 15) // 16
        payload_start = index + 4
        payload_end = payload_start + payload_words
        if payload_end > len(words):
            raise RuntimeError(
                f"IEC burst at word {index} runs past the capture end"
            )
        payload = words[payload_start:payload_end].astype("<u2").tobytes()
        bursts.append({
            "frame": index // CHANNELS,
            "pc": pc,
            "payload_bits": bits,
            "payload_sha256": hashlib.sha256(payload).hexdigest(),
        })
        index = payload_end
    return bursts


def match_bursts(playback: list[dict], captured: list[dict]) -> list[dict]:
    """One-to-one in-order tuple match; any divergence is a hard failure."""
    if not playback:
        raise RuntimeError("playback stimulus contains no IEC bursts")
    if len(captured) != len(playback):
        raise RuntimeError(
            f"burst count mismatch: played {len(playback)}, "
            f"captured {len(captured)}"
        )
    pairs = []
    for position, (sent, received) in enumerate(zip(playback, captured)):
        for key in ("pc", "payload_bits", "payload_sha256"):
            if sent[key] != received[key]:
                raise RuntimeError(
                    f"burst {position} {key} mismatch: "
                    f"played {sent[key]}, captured {received[key]}"
                )
        pairs.append({
            "index": position,
            "pc": sent["pc"],
            "payload_bits": sent["payload_bits"],
            "playback_frame": sent["frame"],
            "capture_frame": received["frame"],
            "latency_frames": received["frame"] - sent["frame"],
        })
    latencies = [pair["latency_frames"] for pair in pairs]
    if any(late < 0 for late in latencies):
        raise RuntimeError("negative burst transport latency")
    deltas = [
        right["capture_frame"] - left["capture_frame"]
        for left, right in zip(pairs, pairs[1:])
    ]
    if any(delta <= 0 for delta in deltas):
        raise RuntimeError("captured bursts are not monotonic")
    return pairs


def transport_summary(latencies: np.ndarray) -> dict:
    return {
        "min_frames": float(np.min(latencies)),
        "median_frames": float(np.median(latencies)),
        "p95_frames": float(np.percentile(latencies, 95)),
        "max_frames": float(np.max(latencies)),
        "min_ms": float(np.min(latencies) * 1000.0 / RATE),
        "median_ms": float(np.median(latencies) * 1000.0 / RATE),
        "p95_ms": float(np.percentile(latencies, 95) * 1000.0 / RATE),
        "max_ms": float(np.max(latencies) * 1000.0 / RATE),
        "bootstrap": bootstrap_median_ci(latencies.astype(np.float64)),
    }


def analyze_iec(playback: np.ndarray, capture: np.ndarray) -> dict:
    # No clipping guard here, deliberately: IEC payload words are bitstream
    # data and legitimately take every 16-bit value, 0x8000 and 0x7FFF
    # included. Integrity is the per-burst payload hash match, which is
    # strictly stronger than any amplitude test.
    played = parse_iec_bursts(playback)
    received = parse_iec_bursts(capture)
    pairs = match_bursts(played, received)
    latencies = np.array(
        [pair["latency_frames"] for pair in pairs], dtype=np.float64
    )
    inputs = np.array(
        [pair["playback_frame"] for pair in pairs], dtype=np.float64
    )
    captures = np.array(
        [pair["capture_frame"] for pair in pairs], dtype=np.float64
    )
    return {
        "mode": "iec",
        "bursts": len(pairs),
        "pairs": pairs,
        "transport": transport_summary(latencies),
        "affine": bench.robust_affine_fit(inputs, captures),
    }


def analyze_pcm(
    reference: np.ndarray, positions: list[int], capture: np.ndarray
) -> dict:
    summary = bench.analyze("pcm", capture, reference, positions, positions)
    latencies = np.array([
        item["latency_frames"]
        for item in summary["occurrences"]
        if item["accepted"]
    ], dtype=np.float64)
    summary["transport"] = transport_summary(latencies)
    summary["mode"] = "pcm-direct-loop"
    return summary


def read_stimulus(path: Path) -> np.ndarray:
    if path.suffix.lower() == ".wav":
        with wave.open(str(path), "rb") as stimulus:
            if (
                stimulus.getnchannels() != CHANNELS
                or stimulus.getsampwidth() != 2
                or stimulus.getframerate() != RATE
            ):
                raise RuntimeError("stimulus WAV must be S16 stereo 48 kHz")
            raw = stimulus.readframes(stimulus.getnframes())
        return np.frombuffer(raw, dtype="<i2").reshape(-1, CHANNELS).copy()
    raw = np.fromfile(path, dtype="<i2")
    if raw.size % CHANNELS:
        raise RuntimeError("raw stimulus is not whole stereo frames")
    return raw.reshape(-1, CHANNELS).copy()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--mode", choices=("pcm", "iec"), required=True)
    parser.add_argument("--device", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--stimulus", type=Path,
        help="IEC-carrier S16 stereo stimulus (.wav or raw), --mode iec only",
    )
    parser.add_argument("--label", default=None)
    parser.add_argument("--period", type=int, default=PERIOD)
    parser.add_argument("--buffer", type=int, default=BUFFER)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    if args.mode == "pcm":
        probe_codes = bench.codes("pcm")
        stimulus_path = args.output / "stimulus.raw"
        positions = bench.make_pcm(stimulus_path, probe_codes)
        playback = np.fromfile(
            stimulus_path, dtype="<i2"
        ).reshape(-1, CHANNELS)
    else:
        if args.stimulus is None:
            parser.error("--mode iec requires --stimulus")
        playback = read_stimulus(args.stimulus)

    tail = np.zeros((args.buffer, CHANNELS), dtype="<i2")
    padded = np.ascontiguousarray(np.vstack([playback, tail]))
    capture, manifest = collect(
        args.device, padded, RATE, args.period, args.buffer, None, 0.0
    )
    if args.mode == "pcm":
        summary = analyze_pcm(playback, positions, capture)
    else:
        summary = analyze_iec(playback, capture)
    summary["label"] = args.label or args.mode
    summary["collection"] = manifest
    bench.write_json(args.output / "direct-loop-summary.json", summary)
    print(json.dumps({
        "label": summary["label"],
        "transport": summary["transport"],
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
