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
stamp, WITHOUT resampling. The recorded frames therefore carry the output
rate directly as SAMPLES PER CYCLE of the 997 Hz tone: 48.14 at 48 kHz,
44.23 at 44.1 kHz, 32.10 at 32 kHz, independent of the rate the stream is
read back at.

MEASUREMENT MODEL -- the fiducial matters.

Stale old-rate frames emitted after a switch are CONTIGUOUS with the
pre-switch run and carry the same samples-per-cycle, so no run-length
analysis can separate them: the two merge into one long run and the answer
is always 0 whatever the board did. The quantity gate 4 asks for is a
DISTANCE, and a distance needs an origin the board itself declares.

That origin is the per-slot fiducial in the recorder header:

    slots[].rate_change_frames  STREAM frame indexes -- frames staged since
                                the slot armed (aius_recorder.h:163-171) --
                                at which the board changed output rate, up
                                to 4. NOT ring indexes.
    slots[].rate_change_count   how many of them are populated
    slots[].stream_end_frame    frames staged at POSTROLL exit

The pulled frames are a different index space: read_slot unrolls the ring
from `oldest_frame`, so the array is indexed 0..valid_frames-1 with 0 = the
oldest RETAINED frame. Converting a fiducial is therefore a subtraction of
the stream index of that oldest retained frame:

    wav_index = fiducial - (stream_end_frame - valid_frames)

    old_rate_output_frames = sum over converted fiducials of
        max(0, (frame where samples-per-cycle actually changes) - wav_index)

A fiducial converting outside [0, valid_frames) rolled off the front of the
ring before the freeze; it is dropped and listed in `fiducials_rolled_off`.

When the header has no fiducial, or has fiducials but no `stream_end_frame`
(their origin is then unknown and guessing one would fabricate the answer),
the reducer falls back to run-length analysis and marks the evidence
`"method": "run-length (no fiducial)"` with `lower_bound: true` -- the number
is then a LOWER BOUND, not the measurement, and must not be read as a clean
pass on its own.

Zero captured slots is a FAILURE, never a clean 0: the reducer emits
`old_rate_output_frames: null`, `captured_slots: 0` and
`failure: "no-transition-captured"`, which the bench's gate-4 check rejects
as a non-integer external observation rather than accepting as clean.

The reducers here are pure and unit-tested; the live orchestration drives
48->44.1->48 via aplay while polling /api/status and /api/events (for the
bench evidence file), operator-freezes the recorder to capture the
transitions, then reads the frozen slots and reduces.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import wave
from pathlib import Path

import numpy as np

FRAME_BYTES = 8
READ_CHUNK_BYTES = 65536
TONE_HZ = 997.0
REFERENCE_RATE = 48000
# Every nominal rate the classifier will name. A block at a rate outside the
# 48/44.1 pair is COUNTED at that rate, never discarded as ambiguous: a
# transient third rate in the output is exactly the artifact gate 4 exists
# to see.
NOMINAL_RATES = (32000, 44100, 48000, 88200, 96000)
# Samples-per-cycle classification. 256 frames spans ~5 tone periods at
# these rates -- enough crossings for a ~0.2 samples-per-cycle estimate,
# fine enough to localise the rate-change frame to within a block. The
# nearest-nominal snap tolerance is in samples per cycle; 48000/997 and
# 44100/997 are 3.9 apart, so 1.5 is a wide margin that still rejects noise.
CLASSIFY_BLOCK = 256
SPC_TOLERANCE = 1.5
RMS_FLOOR = 1000.0
# A real transition phase is ~seconds; any classified run shorter than this
# that interrupts the monotonic phase structure is stale old-rate output.
# Only used by the no-fiducial fallback.
MIN_PHASE_FRAMES = 4096
# How far past a fiducial to look for the samples-per-cycle change before
# giving up (2 s at 48 kHz; a board that has not switched by then has not
# switched).
FIDUCIAL_SEARCH_FRAMES = 96000
# How far BEFORE a fiducial the old rate is established. The board stamps
# `rate_change_frames` at block granularity, and the live 2026-09-02 capture
# shows the output already switching ~400 frames before the stamp: the
# blocks adjacent to the fiducial straddle the discontinuity and classify
# as nothing (one read 97 samples/cycle, a phase glitch). Reading the old
# rate from a single adjacent block therefore fails exactly when the switch
# is clean. The old rate is instead the first classifiable block this far
# before the fiducial, and the change is searched forward from there, so a
# switch that precedes its stamp measures as 0 stale frames rather than as
# unresolved. 4096 frames is 85 ms at 48 kHz: no board switches twice in
# that window, and a stamp lagging its switch by more than that would be a
# firmware defect worth seeing as an unresolved fiducial.
PRE_FIDUCIAL_WINDOW_FRAMES = 4096
# The status poller's cadence. The board's event ring can overflow between
# polls, and `event_overflows` is one of the transitions gate's harmful
# counters, so the cadence is part of the measurement -- see
# STATUS_POLL_NOTE.
STATUS_POLL_INTERVAL_S = 0.25
STATUS_POLL_NOTE = (
    "clock.event_overflows is sensitive to this tool's own status/events "
    "poll cadence: events the board rings out between two polls are lost and "
    "counted as an overflow. A transitions `harmful-counter` failure that "
    "cites event_overflows ALONE is therefore tool-induced and not a board "
    "defect; any other harmful counter, or event_overflows alongside one, is "
    "a real finding.")
# Margin added to the retention budget when checking the recorder ring can
# hold the transition: aplay start-up per phase plus the freeze delay. The
# board's follow latency is NOT in it -- a slow follow puts the first
# fiducial later, i.e. nearer the freeze, so ignoring it is conservative.
STIMULUS_MARGIN_S = 1.0
# The rate the middle phase is played at; the run is REFERENCE -> this ->
# REFERENCE, and the status polls must show the board reaching it or the
# phase was shorter than the board's follow latency.
TRANSITION_RATE = 44100


def samples_per_cycle(segment: np.ndarray) -> float | None:
    """Samples per tone cycle in a block of recorded OUTPUT frames, from
    positive-going zero crossings. This is a property of the frames
    themselves (rate_out / 997), not of the rate they are read back at, so
    it names the board's output rate directly."""
    if len(segment) < 4:
        return None
    rms = float(np.sqrt(np.mean(segment.astype(np.float64) ** 2)))
    crossings = np.flatnonzero((segment[:-1] <= 0) & (segment[1:] > 0)) + 1
    if rms < RMS_FLOOR or len(crossings) < 3:
        return None
    span = int(crossings[-1] - crossings[0])
    if span <= 0:
        return None
    return float(span / (len(crossings) - 1))


def rate_from_samples_per_cycle(spc: float | None,
                                tolerance: float = SPC_TOLERANCE
                                ) -> int | None:
    """Nearest nominal output rate for a samples-per-cycle estimate, or None
    when nothing is close enough. Every NOMINAL_RATES entry is a candidate,
    so a block at a third rate is named rather than dropped."""
    if spc is None or spc <= 0:
        return None
    nearest = min(NOMINAL_RATES, key=lambda rate: abs(spc - rate / TONE_HZ))
    if abs(spc - nearest / TONE_HZ) <= tolerance:
        return int(nearest)
    return None


def classify_output_rates(mono: np.ndarray,
                          block: int = CLASSIFY_BLOCK
                          ) -> list[tuple[int, int, int | None]]:
    """Per-block output rate from the block's samples-per-cycle. Returns
    (start_frame, frames, rate|None); None marks a silent or unclassifiable
    block (boundary blocks, gaps)."""
    labels: list[tuple[int, int, int | None]] = []
    for start in range(0, len(mono) - block + 1, block):
        segment = mono[start:start + block]
        labels.append((start, block,
                       rate_from_samples_per_cycle(samples_per_cycle(segment))))
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


def rate_change_frame(mono: np.ndarray, fiducial: int,
                      block: int = CLASSIFY_BLOCK,
                      search_frames: int = FIDUCIAL_SEARCH_FRAMES,
                      window_frames: int = PRE_FIDUCIAL_WINDOW_FRAMES
                      ) -> int | None:
    """First frame whose samples-per-cycle differs from the rate in force
    `window_frames` before `fiducial`, searched forward from there up to
    `fiducial + search_frames`.

    The board declares WHEN it changed rate (the fiducial); this finds when
    the OUTPUT actually changed. A change found after the fiducial is stale
    old-rate output; a change found before it (the stamp lags the switch by
    up to a block or so) is not, and the caller clamps it to 0. Returns None
    when no classifiable block exists in the pre-fiducial window or no
    change is found within search_frames."""
    origin = max(0, fiducial - window_frames)
    old_rate = None
    start = origin
    while start + block <= fiducial and old_rate is None:
        old_rate = rate_from_samples_per_cycle(
            samples_per_cycle(mono[start:start + block]))
        start += block
    if old_rate is None:
        return None
    limit = min(len(mono), fiducial + search_frames)
    while start + block <= limit:
        rate = rate_from_samples_per_cycle(
            samples_per_cycle(mono[start:start + block]))
        if rate is not None and rate != old_rate:
            return start
        start += block
    return None


def stale_old_rate_frames(runs: list[tuple[int, int]],
                          min_phase_frames: int = MIN_PHASE_FRAMES) -> int:
    """FALLBACK ONLY (no fiducial in the header): frames of stale old-rate
    output read as any interior run shorter than a real transition phase.

    This is a LOWER BOUND and structurally blind to the common case: stale
    old-rate frames immediately after a switch are contiguous with the
    pre-switch run at the same samples-per-cycle, so they merge into it and
    this returns 0 however many there were. Use the fiducial path."""
    if len(runs) <= 2:
        return 0
    stale = 0
    for index in range(1, len(runs) - 1):
        _rate, frames = runs[index]
        if frames < min_phase_frames:
            stale += frames
    return stale


def normalize_fiducials(entry) -> dict:
    """Accept either a fiducials_from_slot() record, a bare list of already
    converted WAV indexes, or None, and return the record shape."""
    if entry is None:
        return {"fiducial_frames": [], "fiducials_rolled_off": [],
                "fiducials_usable": False,
                "fiducial_note": "no fiducials supplied"}
    if isinstance(entry, dict):
        record = dict(entry)
        record.setdefault("fiducial_frames", [])
        record.setdefault("fiducials_rolled_off", [])
        record.setdefault("fiducials_usable", bool(record["fiducial_frames"]))
        return record
    frames = [int(value) for value in entry]
    return {"fiducial_frames": frames, "fiducials_rolled_off": [],
            "fiducials_usable": bool(frames)}


def reduce_slot(frames: np.ndarray, fiducials) -> dict:
    """One slot's stale-frame count plus its evidence. With usable fiducials
    the count is a real measurement; without, it is the run-length lower
    bound."""
    mono = frames[:, 0]
    runs = output_rate_runs(classify_output_rates(mono))
    supplied = normalize_fiducials(fiducials)
    record = {
        "frames": int(len(frames)),
        "output_rate_runs": [[rate, count] for rate, count in runs],
        "fiducial_frames": list(supplied["fiducial_frames"]),
        "fiducials_rolled_off": list(supplied["fiducials_rolled_off"]),
    }
    if supplied.get("stream_end_frame") is not None:
        record["stream_end_frame"] = supplied["stream_end_frame"]
        record["oldest_retained_stream_frame"] = supplied[
            "oldest_retained_stream_frame"]
    if supplied["fiducials_usable"] and supplied["fiducial_frames"]:
        stale = 0
        detected: list[int | None] = []
        unresolved: list[int] = []
        for fiducial in supplied["fiducial_frames"]:
            change = rate_change_frame(mono, int(fiducial))
            detected.append(None if change is None else int(change))
            if change is not None:
                stale += max(0, int(change) - int(fiducial))
            else:
                unresolved.append(int(fiducial))
        record.update({
            "method": "fiducial (slots[].rate_change_frames)",
            "lower_bound": bool(unresolved),
            "rate_change_frames_detected": detected,
            "unresolved_fiducials": unresolved,
            "stale_frames": int(stale),
        })
        if unresolved:
            # A fiducial with no measurable change contributes 0 above, which
            # is the one value it must not silently read as: either the
            # pre-fiducial rate could not be classified or the output never
            # changed within the search window (the stale run then exceeds
            # the window). Say so rather than report a clean 0.
            record["lower_bound_note"] = (
                f"{len(unresolved)} fiducial(s) had no measurable output-rate "
                f"change within {FIDUCIAL_SEARCH_FRAMES} frames (or no "
                f"classifiable block in the {PRE_FIDUCIAL_WINDOW_FRAMES}-frame "
                f"pre-fiducial window) and contributed 0; the count is a "
                f"lower bound, not a measurement")
        return record
    stale = stale_old_rate_frames(runs)
    record.update({
        "method": "run-length (no fiducial)",
        "lower_bound": True,
        "lower_bound_note": (
            supplied.get("fiducial_note")
            or "no usable slots[].rate_change_frames fiducial remained after "
               "converting stream indexes to pulled-frame indexes")
        + "; stale old-rate frames contiguous with the pre-switch run cannot "
          "be separated from it, so this count is a lower bound, not a "
          "measurement",
        "stale_frames": int(stale),
    })
    return record


def reduce_recorder_transitions(slots: list[np.ndarray],
                                fiducial_sets: list | None = None) -> dict:
    """old_rate_output_frames across every captured slot.

    ZERO captured slots is a failure, not a clean 0: the field is emitted as
    null with a `no-transition-captured` marker so the bench's gate-4 check
    rejects it ("old_rate_output_frames must be an unsigned integer") rather
    than reading a vacuous pass."""
    if not slots:
        return {
            "external": {
                "old_rate_output_frames": None,
                "captured_slots": 0,
                "failure": "no-transition-captured",
            },
            "evidence": {
                "detection": "flight-recorder output-rate segmentation",
                "reference_rate_hz": REFERENCE_RATE,
                "captured_slots": 0,
                "failure": "no-transition-captured",
                "failure_note": (
                    "no frozen slot with a rate change was captured, so "
                    "nothing was measured; old_rate_output_frames is null "
                    "rather than 0 so the gate fails visibly instead of "
                    "passing on absent evidence"),
                "slots": [],
            },
        }
    total_stale = 0
    evidence = []
    lower_bound = False
    methods: list[str] = []
    for index, frames in enumerate(slots):
        fiducials = None
        if fiducial_sets is not None and index < len(fiducial_sets):
            fiducials = fiducial_sets[index]
        record = reduce_slot(frames, fiducials)
        record["slot_index"] = index
        total_stale += record["stale_frames"]
        lower_bound = lower_bound or record["lower_bound"]
        if record["method"] not in methods:
            methods.append(record["method"])
        evidence.append(record)
    return {
        "external": {
            "old_rate_output_frames": int(total_stale),
            "captured_slots": len(slots),
        },
        "evidence": {
            "detection": "flight-recorder output-rate segmentation",
            "reference_rate_hz": REFERENCE_RATE,
            "captured_slots": len(slots),
            "lower_bound": lower_bound,
            "method": methods[0] if len(methods) == 1 else " + ".join(methods),
            "slots": evidence,
        },
    }


def fiducials_from_slot(meta: dict) -> dict:
    """Per-slot fiducials converted from STREAM indexes to WAV indexes.

    `slots[].rate_change_frames` are STREAM indexes -- frames staged since the
    slot armed (aius_recorder.h:163-171) -- NOT ring indexes. The array
    read_slot returns is already unrolled from `oldest_frame` and is indexed
    0..valid_frames-1 with 0 = the oldest RETAINED frame, so the conversion is
    a subtraction of the stream index of that oldest retained frame:

        wav_index = fiducial - (stream_end_frame - valid_frames)

    where `stream_end_frame` is the slot's frames-staged count at POSTROLL
    exit. A fiducial landing outside [0, valid_frames) rolled off the front of
    the ring before the freeze; it is dropped and recorded in
    `fiducials_rolled_off` so a short ring is visible rather than silently
    shrinking the measurement.

    When `stream_end_frame` is ABSENT the origin is unknown and the fiducials
    are unusable -- there is no safe guess, so the caller must take the
    run-length fallback with `lower_bound: true`."""
    raw = meta.get("rate_change_frames")
    valid = int(meta.get("valid_frames") or 0)
    stream_end = meta.get("stream_end_frame")
    if not isinstance(raw, list) or not raw:
        return {
            "fiducial_frames": [],
            "fiducials_rolled_off": [],
            "fiducials_usable": False,
            "fiducial_note": ("the recorder header carries no "
                              "slots[].rate_change_frames fiducial"),
        }
    if isinstance(stream_end, bool) or not isinstance(stream_end, int):
        return {
            "fiducial_frames": [],
            "fiducials_rolled_off": [],
            "fiducials_usable": False,
            "fiducial_note": (
                "slots[].rate_change_frames are stream indexes but the header "
                "carries no slots[].stream_end_frame, so their origin in the "
                "pulled frames is unknown; guessing one would fabricate the "
                "measurement, so the run-length lower bound is used instead"),
        }
    count = meta.get("rate_change_count")
    if isinstance(count, int) and not isinstance(count, bool) and (
            0 <= count <= len(raw)):
        raw = raw[:count]
    base = int(stream_end) - valid
    usable: list[int] = []
    rolled_off: list[int] = []
    for value in raw:
        if isinstance(value, bool) or not isinstance(value, int):
            continue
        wav_index = int(value) - base
        if 0 <= wav_index < valid:
            usable.append(wav_index)
        else:
            rolled_off.append(int(value))
    return {
        "fiducial_frames": sorted(usable),
        "fiducials_rolled_off": sorted(rolled_off),
        "fiducials_usable": bool(usable),
        "stream_end_frame": int(stream_end),
        "oldest_retained_stream_frame": base,
    }


def newest_frozen_slot(header: dict) -> dict | None:
    """The slot this run's freeze captured.

    Prefers the header's own `newest_frozen_slot` pointer; falls back to the
    frozen slot with the highest `freeze_generation`. The old
    `(active_slot - 1) % slot_count` guess assumed the bank's auto-advance
    had already landed within the tool's 0.6 s sleep, which is a race, not a
    fact."""
    slots = header.get("slots") or []
    pointer = header.get("newest_frozen_slot")
    if isinstance(pointer, int):
        for slot in slots:
            if slot.get("slot") == pointer:
                return slot
    frozen = [slot for slot in slots if slot.get("state") == "frozen"]
    if not frozen:
        return None
    return max(frozen, key=lambda slot: int(slot.get("freeze_generation") or 0))


def frozen_identities(header: dict) -> set[tuple[int, int]]:
    """(slot, freeze_generation) of every frozen slot in a header.

    `freeze_generation` is PER SLOT: it counts that slot's own thaws
    (aius_recorder.h:56) and starts at 0, so comparing it across slots says
    nothing -- on a fresh boot every frozen slot reads 0 and a "newer than
    the baseline generation" check rejects every real capture. The pair is
    the identity of one frozen capture: a slot refrozen after a thaw carries
    the next generation, and a slot that was not frozen at baseline is new
    by construction."""
    identities: set[tuple[int, int]] = set()
    for slot in header.get("slots") or []:
        if slot.get("state") != "frozen":
            continue
        identities.add((int(slot.get("slot")),
                        int(slot.get("freeze_generation") or 0)))
    return identities


def captured_slot(header: dict,
                  baseline: set[tuple[int, int]]) -> dict | None:
    """The slot this run's freeze produced: the newest frozen slot, unless
    it is a capture that already existed before the run (its identity is in
    `baseline`), in which case the freeze never landed and there is nothing
    to measure."""
    meta = newest_frozen_slot(header)
    if meta is None:
        return None
    identity = (int(meta.get("slot")),
                int(meta.get("freeze_generation") or 0))
    if identity in baseline:
        return None
    return meta


def stream_rate_timeline(polls: list[dict]) -> list[list]:
    """(elapsed_s, stream_rate_hz) at every change of the board's reported
    stream rate across the run's status polls -- the run's own record of
    whether the board followed each stimulus phase."""
    timeline: list[list] = []
    last = None
    for poll in polls:
        clock = (poll.get("status") or {}).get("clock") or {}
        rate = clock.get("stream_rate_hz")
        if rate is None or rate == last:
            continue
        timeline.append([round(float(poll.get("elapsed_s") or 0.0), 3),
                         int(rate)])
        last = rate
    return timeline


def stimulus_followed(polls: list[dict], rate: int) -> bool:
    """Whether any status poll saw the board's stream rate reach `rate`. A
    run whose middle phase is shorter than the board's follow latency never
    switches (the board sits in `unsupported` for the whole phase), stamps
    no fiducial, and must be reported as a stimulus problem rather than a
    freeze that failed to land."""
    return any(rate == entry[1] for entry in stream_rate_timeline(polls))


def slot_capacity_frames(header: dict, meta: dict | None) -> int:
    """Frames one slot's ring holds. `ring_frames` is the direct statement;
    the fitted_mb/slot_count/frame_bytes arithmetic is the fallback."""
    if meta and int(meta.get("ring_frames") or 0) > 0:
        return int(meta["ring_frames"])
    fitted_mb = int(header.get("fitted_mb") or 0)
    slot_count = int(header.get("slot_count") or 1) or 1
    frame_bytes = int(header.get("frame_bytes") or FRAME_BYTES) or FRAME_BYTES
    return (fitted_mb * 1024 * 1024) // slot_count // frame_bytes


def required_capture_frames(phase_seconds: float, phases: int, rate: int,
                            latency_s: float = STIMULUS_MARGIN_S) -> int:
    """Frames the recorder must still hold at the freeze for every rate
    change to be measurable: from just before the FIRST rate boundary to
    the freeze, i.e. the last `phases - 1` phases plus the stimulus margin.
    The body of the first phase need not survive -- the measurement only
    reads one classify block before each fiducial -- but a ring shorter
    than this rolls the first fiducial off the front before the freeze and
    the reduction then measures a transition that is no longer there."""
    return int((phase_seconds * (phases - 1) + latency_s) * rate)


def tone_rate_hz(path: Path) -> int:
    """Sample rate of a stimulus WAV, so a phase can be expressed to aplay
    in frames (`--samples`) rather than whole seconds (`--duration`
    truncates 1.2 s to 1 s, which is shorter than the board's follow
    latency and produces a run with no rate change at all)."""
    with wave.open(str(path), "rb") as tone:
        return int(tone.getframerate())


def phase_frames(phase_seconds: float, rate: int) -> int:
    return int(round(phase_seconds * rate))


def usb_authorize(path: str, value: int) -> None:
    """Set a USB device's `authorized` flag. Deauthorizing the U24 (3-1.1)
    drops the board's source carrier, inducing the loss->replay leg; 0 = down,
    1 = up. The file is group-writable to plugdev (the operator's group).

    Written directly rather than through `sh -c "echo ... > path"`: the shell
    form interpolates a CLI-supplied path into a command string."""
    Path(path).write_text(str(int(value)), encoding="utf-8")


def board_post(base: str, auth: str, path: str, body: str) -> int:
    """POST a JSON body with the SD-auth header; return the HTTP status
    without raising on 4xx/5xx (a 409 replay_carrier_present is expected
    until the carrier has actually dropped)."""
    request = urllib.request.Request(base + path, method="POST",
                                     data=body.encode("utf-8"))
    request.add_header("X-Aius-SD-Auth", auth)
    try:
        with urllib.request.urlopen(request, timeout=8) as response:
            return response.status
    except urllib.error.HTTPError as error:
        return error.code


class RecorderClient:
    """Minimal /api/recorder client: header read, freeze/thaw, PUT config,
    and slot data reads reassembled to logical (oldest-first) frame order."""

    CONFIG_KEYS = ("overrun", "underrun", "carrier_loss", "limiter_gr",
                   "gr_threshold_db", "gr_hold_blocks", "postroll_blocks")

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

    def set_config(self, config: dict) -> None:
        body = {key: config[key] for key in self.CONFIG_KEYS if key in config}
        self._request("/api/recorder/config", "PUT", json.dumps(body))

    def disable_auto_triggers(self) -> None:
        """Silence every auto-trigger so the run's own freeze is the only
        capture. This MUTATES the board's persistent recorder config; the
        caller restores the previous config in a finally."""
        self.set_config({
            "overrun": False, "underrun": False, "carrier_loss": False,
            "limiter_gr": False, "gr_threshold_db": 0.0,
            "gr_hold_blocks": 1, "postroll_blocks": 0,
        })

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
    parser.add_argument("--output", type=Path,
                        help="evidence directory (default: a timestamped dir "
                             "under firmware/test/evidence/rate-gate-capture/)")
    parser.add_argument("--expect-build")
    parser.add_argument("--with-replay", action="store_true",
                        help="drive the loss->replay leg (USB-deauths the U24)")
    parser.add_argument("--replay-id", default="channel_id_51")
    parser.add_argument("--usb-path",
                        default="/sys/bus/usb/devices/3-1.1/authorized")
    parser.add_argument("--allow-rolloff", action="store_true",
                        help="run even when the recorder ring is too small to "
                             "hold the whole transition (the capture will "
                             "roll the first phase off the front)")
    args = parser.parse_args(argv)

    # Sibling module: reuse polling + evidence emission. Add this file's own
    # directory so the tool runs by absolute path, not only from its dir.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import capture

    auth = Path(args.auth_file).read_text(encoding="utf-8").strip()
    client = RecorderClient(args.base, auth)
    if args.output is None:
        args.output = capture.default_output_dir("transitions")
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    status_url = f"{args.base}/api/status"
    events_base = capture.events_url(status_url)

    # The three aplay phases are 48 -> 44.1 -> 48 (two rate boundaries); the
    # ring must hold all three plus the stimulus start-up latency or the
    # first boundary rolls off the front before the freeze.
    phases = 3
    baseline_header = client.header()
    baseline = frozen_identities(baseline_header)
    capacity = slot_capacity_frames(
        baseline_header,
        (baseline_header.get("slots") or [None])[0])
    required = required_capture_frames(args.phase_seconds, phases,
                                       REFERENCE_RATE)
    if capacity and required > capacity and not args.allow_rolloff:
        print(
            f"recorder slot holds {capacity} frames; the last "
            f"{phases - 1} of the {args.phase_seconds:g}s x {phases}-phase "
            f"transition plus {STIMULUS_MARGIN_S:g}s stimulus margin needs "
            f"{required}. The first rate change would roll off the ring "
            f"before the freeze. Shorten --phase-seconds or pass "
            f"--allow-rolloff to run anyway.", file=sys.stderr)
        return 3
    tone_frames = {
        str(args.tone_48): phase_frames(args.phase_seconds,
                                        tone_rate_hz(args.tone_48)),
        str(args.tone_44): phase_frames(args.phase_seconds,
                                        tone_rate_hz(args.tone_44)),
    }

    previous_config = dict(baseline_header.get("config") or {})

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
            time.sleep(STATUS_POLL_INTERVAL_S)

    def aplay(tone: Path) -> None:
        # --samples, not --duration: aplay's -d is whole seconds and would
        # truncate a fractional phase below the board's follow latency.
        subprocess.run(["aplay", "-D", args.device,
                        "-s", str(tone_frames[str(tone)]), str(tone)],
                       capture_output=True, check=False)

    poller = threading.Thread(target=poll_loop, daemon=True)
    meta = None
    header = baseline_header
    try:
        client.disable_auto_triggers()
        poller.start()

        aplay(args.tone_48)
        aplay(args.tone_44)
        aplay(args.tone_48)
        time.sleep(0.2)
        client.freeze()
        time.sleep(0.6)

        # Identify the captured slot NOW, before the replay leg perturbs the
        # bank, and only accept one that did not already exist at baseline
        # -- otherwise a freeze that never landed reads back as somebody
        # else's older capture. Identity is the per-slot (slot,
        # freeze_generation) pair; see frozen_identities.
        header = client.header()
        meta = captured_slot(header, baseline)

        if args.with_replay:
            try:
                print("loss->replay: deauthorizing U24", flush=True)
                usb_authorize(args.usb_path, 0)      # drop the source carrier
                time.sleep(2.5)                       # >=2 unlocked samples
                for _ in range(6):
                    status = board_post(
                        args.base, auth, "/api/replays/start",
                        json.dumps({"id": args.replay_id, "loop": False}))
                    print(f"replays/start -> {status}", flush=True)
                    if status == 200:
                        break
                    time.sleep(0.8)                   # carrier not gone yet
                time.sleep(3.5)                       # dwell in replay
            finally:
                print("loss->replay: reauthorizing U24", flush=True)
                usb_authorize(args.usb_path, 1)       # ALWAYS restore the U24
                time.sleep(1.5)
    finally:
        stop.set()
        poller.join(timeout=5)
        if previous_config:
            # disable_auto_triggers mutates the board's PERSISTENT recorder
            # config; leaving it applied silently disarms every auto-trigger
            # for whoever uses the board next.
            client.set_config(previous_config)

    slots = []
    fiducial_sets: list[list[int]] = []
    if meta is not None and meta.get("state") == "frozen" and int(
            meta.get("rate_changes") or 0) >= 1:
        slots.append(client.read_slot(
            int(meta["slot"]), meta["freeze_generation"],
            meta["valid_frames"], meta["oldest_frame"]))
        fiducial_sets.append(fiducials_from_slot(meta))
    reduced = reduce_recorder_transitions(slots, fiducial_sets)
    followed = stimulus_followed(polls, TRANSITION_RATE)
    reduced["evidence"].update({
        "captured_slot": None if meta is None else meta.get("slot"),
        "captured_trigger": None if meta is None
        else (meta.get("trigger") or {}).get("which"),
        "slot_rate_changes": None if meta is None else meta.get("rate_changes"),
        "baseline_frozen_identities": sorted(list(pair) for pair in baseline),
        "freeze_generation": None if meta is None
        else meta.get("freeze_generation"),
        "slot_capacity_frames": capacity,
        "required_capture_frames": required,
        "phase_seconds": args.phase_seconds,
        "phase_frames": tone_frames,
        "phases": phases,
        "stream_rate_timeline": stream_rate_timeline(polls),
        "stimulus_followed": followed,
        "status_poll_interval_s": STATUS_POLL_INTERVAL_S,
        "event_overflows_note": STATUS_POLL_NOTE,
        "recorder_config_restored": bool(previous_config),
    })
    if not slots and not followed:
        reduced["evidence"]["failure_reason"] = "stimulus-not-followed"
        reduced["evidence"]["failure_reason_note"] = (
            f"the status polls never saw stream_rate_hz reach "
            f"{TRANSITION_RATE}: the {args.phase_seconds:g}s middle phase "
            f"was shorter than the board's follow latency, so no rate change "
            f"was stamped and there was nothing to freeze. Lengthen "
            f"--phase-seconds; this is a stimulus problem, not a freeze that "
            f"failed to land")

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
        capture.announce_evidence(output / "evidence.jsonl")
    print(json.dumps(reduced["external"], indent=2, sort_keys=True))
    if not slots:
        reason = reduced["evidence"].get("failure_reason_note") or (
            "no frozen slot with a rate change that did not already exist "
            "at the pre-run baseline")
        print(f"transitions FAILED: no-transition-captured ({reason})",
              file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
