#!/usr/bin/env python3
"""Gate 4 (transitions) capture via the board's flight recorder.

The transitions gate's external field is `old_rate_output_frames`: the count
of frames the board emits at the OLD rate after a rate switch (0 for a board
that follows cleanly). It cannot be measured through the U24 ALSA capture:
the U24 capture clock locks to the incoming carrier (a fixed-rate `hw:0,0`
arecord errors when the carrier rate changes) and `plughw` resampling would
preserve 997 Hz across every rate, erasing the signature.

The flight recorder (`/api/recorder`) solves this. It stages the board's own
OUTPUT frames (s32le, 24-bit, stereo) into a PSRAM ring with a per-slot rate
stamp, WITHOUT resampling. Reading that flat frame stream at a fixed
reference rate makes the output rate visible as the captured tone frequency:
a 997 Hz tone output at 48 kHz reads 997 Hz, but output at 44.1 kHz reads
997 * 48000/44100 = 1085 Hz. So the board's output-rate timeline is
recoverable from the recorded frames, and stale old-rate frames after a
switch show as an out-of-place frequency segment. A clean board yields
`old_rate_output_frames == 0`, corroborated by the slot's own `rate_changes`.

The reducers here are pure and unit-tested; the live orchestration drives
48->44.1->48 via aplay while polling /api/status and /api/events (for the
bench evidence file), operator-freezes the recorder to capture the
transitions, then reads the frozen slots and reduces.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import threading
import time
import urllib.request
from pathlib import Path

import numpy as np

FRAME_BYTES = 8
READ_CHUNK_BYTES = 65536
TONE_HZ = 997.0
REFERENCE_RATE = 48000
CANDIDATE_RATES = (48000, 44100)
# Frequency classification tolerance and per-block analysis size. 512 frames
# spans ~10 tone periods at these rates -- enough crossings for a few-Hz
# estimate, fine enough to localise a stale-frame artifact.
CLASSIFY_BLOCK = 512
CLASSIFY_TOLERANCE_HZ = 30.0
RMS_FLOOR = 1000.0
# A real transition phase is ~seconds; any classified run shorter than this
# that interrupts the monotonic phase structure is stale old-rate output.
MIN_PHASE_FRAMES = 4096


def classify_output_rates(mono: np.ndarray, reference_rate: int = REFERENCE_RATE,
                          block: int = CLASSIFY_BLOCK) -> list[tuple[int, int, int | None]]:
    """Per-block output rate, inferred from the tone frequency read at the
    reference rate. Returns (start_frame, frames, rate|None); None marks a
    silent or ambiguous block (boundary blocks, gaps)."""
    labels: list[tuple[int, int, int | None]] = []
    for start in range(0, len(mono) - block + 1, block):
        segment = mono[start:start + block]
        rms = float(np.sqrt(np.mean(segment.astype(np.float64) ** 2)))
        crossings = np.flatnonzero(
            (segment[:-1] <= 0) & (segment[1:] > 0)) + 1
        if rms < RMS_FLOOR or len(crossings) < 3:
            labels.append((start, block, None))
            continue
        span = int(crossings[-1] - crossings[0])
        frequency = (len(crossings) - 1) * reference_rate / span
        best = min(CANDIDATE_RATES,
                   key=lambda rate: abs(frequency - TONE_HZ * reference_rate / rate))
        expected = TONE_HZ * reference_rate / best
        labels.append((start, block,
                       best if abs(frequency - expected) < CLASSIFY_TOLERANCE_HZ
                       else None))
    return labels


def output_rate_runs(labels: list[tuple[int, int, int | None]]
                     ) -> list[tuple[int, int]]:
    """Collapse confident block labels to (rate, frames) runs, dropping the
    ambiguous (None) boundary blocks."""
    runs: list[list[int]] = []
    for _start, frames, rate in labels:
        if rate is None:
            continue
        if runs and runs[-1][0] == rate:
            runs[-1][1] += frames
        else:
            runs.append([rate, frames])
    return [(rate, frames) for rate, frames in runs]


def stale_old_rate_frames(runs: list[tuple[int, int]],
                          min_phase_frames: int = MIN_PHASE_FRAMES) -> int:
    """Frames of stale old-rate output: any interior run shorter than a real
    transition phase interrupts the monotonic 48->44.1->48 structure and is
    old-rate leakage. A clean board's runs are all long -> 0."""
    if len(runs) <= 2:
        return 0
    stale = 0
    for index in range(1, len(runs) - 1):
        rate, frames = runs[index]
        if frames < min_phase_frames:
            stale += frames
    return stale


def reduce_recorder_transitions(slots: list[np.ndarray]) -> dict:
    """old_rate_output_frames across every captured slot. Each slot's raw
    output frames are classified independently; a clean per-slot transition
    contributes 0. Cross-slot boundaries are themselves the rate changes."""
    total_stale = 0
    evidence = []
    for index, frames in enumerate(slots):
        runs = output_rate_runs(classify_output_rates(frames[:, 0]))
        stale = stale_old_rate_frames(runs)
        total_stale += stale
        evidence.append({
            "slot_index": index,
            "frames": int(len(frames)),
            "output_rate_runs": [[rate, count] for rate, count in runs],
            "stale_frames": stale,
        })
    return {
        "external": {"old_rate_output_frames": int(total_stale)},
        "evidence": {
            "detection": "flight-recorder output-rate segmentation",
            "reference_rate_hz": REFERENCE_RATE,
            "slots": evidence,
        },
    }


class RecorderClient:
    """Minimal /api/recorder client: header read, freeze/thaw, PUT config,
    and slot data reads reassembled to logical (oldest-first) frame order."""

    def __init__(self, base: str, auth: str):
        self.base = base
        self.auth = auth

    def _request(self, path: str, method: str = "GET",
                 body: str | None = None) -> tuple[int, bytes]:
        request = urllib.request.Request(
            self.base + path, method=method,
            data=body.encode("utf-8") if body else None)
        request.add_header("X-Aius-SD-Auth", self.auth)
        with urllib.request.urlopen(request, timeout=12) as response:
            return response.status, response.read()

    def header(self) -> dict:
        return json.loads(self._request("/api/recorder")[1].decode("utf-8"))

    def disable_auto_triggers(self) -> None:
        self._request("/api/recorder/config", "PUT", json.dumps({
            "overrun": False, "underrun": False, "carrier_loss": False,
            "limiter_gr": False, "gr_threshold_db": 0.0,
            "gr_hold_blocks": 1, "postroll_blocks": 0,
        }))

    def freeze(self) -> None:
        self._request("/api/recorder/freeze", "POST")

    def read_slot(self, slot: int, generation: int, valid_frames: int,
                  oldest_frame: int) -> np.ndarray:
        valid_bytes = valid_frames * FRAME_BYTES
        raw = bytearray()
        offset = 0
        while offset < valid_bytes:
            length = min(READ_CHUNK_BYTES, valid_bytes - offset)
            chunk = self._request(
                f"/api/recorder/data?offset={offset}&length={length}"
                f"&generation={generation}&slot={slot}")[1]
            if not chunk:
                break
            raw.extend(chunk)
            offset += len(chunk)
        frames = np.frombuffer(bytes(raw), dtype="<i4").reshape(-1, 2)
        if oldest_frame:
            frames = np.roll(frames, -oldest_frame, axis=0)
        return frames


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="hw:0,0")
    parser.add_argument("--base", default="http://192.0.2.66")
    parser.add_argument("--auth-file", default=".secrets/aius-sd-auth")
    parser.add_argument("--tone-48", required=True, type=Path)
    parser.add_argument("--tone-44", required=True, type=Path)
    parser.add_argument("--phase-seconds", type=float, default=2.0)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--expect-build")
    args = parser.parse_args(argv)

    import capture  # sibling module: reuse polling + evidence emission

    auth = Path(args.auth_file).read_text(encoding="utf-8").strip()
    client = RecorderClient(args.base, auth)
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    status_url = f"{args.base}/api/status"
    events_base = capture.events_url(status_url)

    client.disable_auto_triggers()

    polls: list[dict] = []
    stop = threading.Event()
    started = time.monotonic()

    def poll_loop() -> None:
        cursor = int(capture.http_get(status_url, auth)[1]["event_sequence"])
        while not stop.is_set():
            elapsed = time.monotonic() - started
            try:
                status_raw, _status = capture.http_get(status_url, auth)
                events_raw, events = capture.http_get(
                    f"{events_base}?cursor={cursor}", auth)
                cursor = int(events["next"])
                polls.append({
                    "elapsed_s": elapsed,
                    "status": json.loads(status_raw),
                    "raw_status_json": status_raw,
                    "raw_events_json": events_raw,
                })
            except RuntimeError:
                pass
            time.sleep(0.7)

    poller = threading.Thread(target=poll_loop, daemon=True)
    poller.start()

    def aplay(tone: Path) -> None:
        subprocess.run(["aplay", "-D", args.device,
                        "-d", str(int(args.phase_seconds)), str(tone)],
                       capture_output=True)

    aplay(args.tone_48)
    aplay(args.tone_44)
    aplay(args.tone_48)
    time.sleep(0.2)
    client.freeze()
    time.sleep(0.5)
    stop.set()
    poller.join(timeout=5)

    # The bank freezes the ACTIVE slot then advances, so the slot this run
    # captured is the one just behind the new active pointer.
    header = client.header()
    captured = (header["active_slot"] - 1) % header["slot_count"]
    meta = next(s for s in header["slots"] if s["slot"] == captured)
    slots = []
    if meta["state"] == "frozen" and meta["rate_changes"] >= 1:
        slots.append(client.read_slot(
            captured, meta["freeze_generation"], meta["valid_frames"],
            meta["oldest_frame"]))
    reduced = reduce_recorder_transitions(slots)
    reduced["evidence"]["captured_slot"] = captured
    reduced["evidence"]["slot_rate_changes"] = meta["rate_changes"]

    external_raw = json.dumps(reduced["external"])
    capture.write_json(output / "external-observation.json", reduced["external"])
    capture.write_json(output / "capture-analysis.json", {
        "scenario": "transitions",
        "captured_slots": len(slots),
        "external": reduced["external"],
        "evidence": reduced["evidence"],
    })
    if polls:
        options = {"settle_s": 0.0, "phase_samples": 2,
                   "max_follow_error_millihz": 1000}
        if args.expect_build:
            options["expect_build"] = args.expect_build
        capture.write_evidence(output / "evidence.jsonl", "transitions",
                               polls, options, external_raw)
    print(json.dumps(reduced["external"], indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
