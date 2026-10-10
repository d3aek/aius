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
from collections.abc import Iterable, Sequence
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
# The status poller's cadence. The board's 32-entry event ring can overflow
# between polls (events beyond the depth are lost; up to the depth they are
# deferred and paged out via `next`), and `event_overflows` is one of the
# transitions gate's harmful counters, so the cadence is part of the
# measurement -- see STATUS_POLL_NOTE.
STATUS_POLL_INTERVAL_S = 0.25
STATUS_POLL_NOTE = (
    "clock.event_overflows is sensitive to this tool's own status/events "
    "poll cadence: events the board rings out between two polls are deferred "
    "up to the ring's 32-entry depth and paged out via `next`; only those "
    "beyond it are lost and counted as an overflow. A transitions "
    "`harmful-counter` failure that "
    "cites event_overflows ALONE is therefore tool-induced and not a board "
    "defect; any other harmful counter, or event_overflows alongside one, is "
    "a real finding.")
# Margin added to the retention budget when checking the recorder ring can
# hold the transition. It covers what the phase durations do not: the
# aplay start-up gap between phases (<=0.1 s each, two of them), the
# FREEZE_DWELL_S after the board is seen tracking again plus one poll
# interval of detection latency, the freeze POST itself, and the
# PRE_FIDUCIAL_WINDOW_FRAMES of the first phase's tail (~0.085 s) -- about
# 0.6 s at 48 kHz in all. The board's acceptance latency is NOT in it: a
# board sitting `unsupported` stages nothing (live 2026-09-03: the slot's
# 44.1 kHz run was 8192 frames on the run whose polls showed 0.26 s of
# 44.1 kHz tracking and ~37700 on the runs that showed 0.78 s), so a slow
# follow only lowers the requirement and the budget below is the
# instant-acceptance worst case.
STIMULUS_MARGIN_S = 1.0
# The rate the middle phase is played at; the run is REFERENCE -> this ->
# REFERENCE, and the status polls must show the board reaching it or the
# phase was shorter than the board's follow latency.
TRANSITION_RATE = 44100
# The board's acceptance latency after a carrier rate switch: it sits
# `unsupported` at the OLD stream rate, gate closed, until it accepts the
# new one. 48 -> 44.1 kHz measured 1.05-1.85 s across the 2026-09-02/03
# runs (1.85 s on the run that failed `transition-sequence`); 44.1 -> 48 kHz
# measured ~1.13 s on every run (51200 frames after the second fiducial,
# four runs out of four). The bench analyzer labels a phase only from
# `tracking` samples and wants `phase_samples 2`, so the middle phase must
# outlast this budget by at least three poll intervals: two samples are
# guaranteed only once the tracking window exceeds two full intervals.
ACCEPTANCE_LATENCY_BUDGET_S = 1.85
# Per-phase defaults: 48 kHz lead-in, 44.1 kHz middle, 48 kHz trailing.
# The lead-in costs no ring (only its last classify block must survive the
# freeze), so it stays long enough to settle and to give the analyzer its
# first phase whatever carrier the U24 was idling at. The middle outlasts
# ACCEPTANCE_LATENCY_BUDGET_S by the three-interval rule. The trailing
# phase is short because the freeze is gated on the board being SEEN
# tracking the reference rate again (see FREEZE_DWELL_S), not on the phase
# length, and a short trailing phase is what lets a 3 s middle fit the
# board's 262144-frame slot without --allow-rolloff: required_capture_frames
# for these defaults is 252300.
DEFAULT_PHASE_SECONDS = (2.0, 3.0, 1.5)
# After the trailing aplay returns, the freeze waits (bounded) until a
# status poll since the switch back shows the board tracking the reference
# rate -- i.e. the second fiducial has been stamped -- then dwells this long
# so the classifier has blocks after it. If no such poll arrives within
# FREEZE_ACCEPTANCE_TIMEOUT_S the freeze goes ahead anyway (the old fixed
# delay behaviour, later); the timeout is short because in the one case it
# fires with the board actually tracking (poller dead) every second of it
# costs 48000 ring frames.
FREEZE_DWELL_S = 0.3
FREEZE_ACCEPTANCE_TIMEOUT_S = 1.0


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


def driven_phase_labels(with_replay: bool) -> list[str]:
    """The bench phase sequence this run actually drives, recorded into the
    evidence metadata as the `expected_phases` analysis option.

    The stimulus is 48 -> 44.1 -> 48; the loss->replay leg is driven only
    under `--with-replay`. Without it the run plays three phases, and a
    bench that asserts a fourth fails the gate for a phase the stimulus
    never played rather than for anything the board did."""
    labels = [str(REFERENCE_RATE), str(TRANSITION_RATE), str(REFERENCE_RATE)]
    if with_replay:
        labels.append("replay")
    return labels


def slot_capacity_frames(header: dict, meta: dict | None) -> int:
    """Frames one slot's ring holds. `ring_frames` is the direct statement;
    the fitted_mb/slot_count/frame_bytes arithmetic is the fallback."""
    if meta and int(meta.get("ring_frames") or 0) > 0:
        return int(meta["ring_frames"])
    fitted_mb = int(header.get("fitted_mb") or 0)
    slot_count = int(header.get("slot_count") or 1) or 1
    frame_bytes = int(header.get("frame_bytes") or FRAME_BYTES) or FRAME_BYTES
    return (fitted_mb * 1024 * 1024) // slot_count // frame_bytes


def required_capture_frames(phase_seconds: Sequence[float],
                            phase_rates: Sequence[int],
                            latency_s: float = STIMULUS_MARGIN_S) -> int:
    """Frames the recorder must still hold at the freeze for every rate
    change to be measurable: from just before the FIRST rate boundary to
    the freeze.

    The recorder stages the board's OUTPUT frames at the output rate, and
    only while the board is tracking (an `unsupported` board stages
    nothing), so every phase after the first costs `seconds x rate` frames
    at ITS rate -- a 44.1 kHz phase costs 44100 frames per second, not
    48000 -- for the instant-acceptance worst case. The body of the first
    phase need not survive: the measurement reads only one classify block
    before the first fiducial, and the stimulus margin (at the reference
    rate) covers that block plus the aplay gaps, freeze dwell and freeze
    POST. A ring shorter than this rolls the first fiducial off the front
    before the freeze and the reduction then measures a transition that is
    no longer there."""
    if len(phase_seconds) != len(phase_rates):
        raise ValueError("one rate per phase")
    later_phases = sum(seconds * rate for seconds, rate
                       in zip(phase_seconds[1:], phase_rates[1:]))
    return int(later_phases + latency_s * REFERENCE_RATE)


def parse_phase_seconds(text: str) -> tuple[float, float, float]:
    """`--phase-seconds` value: one positive number for all three aplay
    phases, or `first,middle,last` (48 kHz lead-in, 44.1 kHz middle, 48 kHz
    trailing). One flag rather than three because the phases are one
    ordered stimulus and the evidence records them as one list."""
    parts = [part.strip() for part in text.split(",")]
    if len(parts) == 1:
        parts = parts * 3
    if len(parts) != 3:
        raise argparse.ArgumentTypeError(
            f"expected one duration or first,middle,last; got {text!r}")
    try:
        values = tuple(float(part) for part in parts)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            f"phase durations must be numbers; got {text!r}") from error
    if any(value <= 0 for value in values):
        raise argparse.ArgumentTypeError(
            f"phase durations must be positive; got {text!r}")
    return values  # type: ignore[return-value]


def tracking_seen_since(polls: Iterable[dict], rate: int,
                        since_elapsed_s: float) -> float | None:
    """elapsed_s of the first status poll stamped at or after
    `since_elapsed_s` that shows the board TRACKING `rate`, or None.

    Used to gate the freeze on the board having accepted the reference
    rate after the switch back: a poll stamped after the trailing aplay
    started that reads `stream_rate_hz == rate` with `state == tracking`
    can only mean the second fiducial has been stamped. A poll stamped
    before the switch is ignored whatever it reads (the board was tracking
    the middle rate then, and a stale rate in it is not acceptance)."""
    for poll in polls:
        if float(poll.get("elapsed_s") or 0.0) < since_elapsed_s:
            continue
        clock = (poll.get("status") or {}).get("clock") or {}
        if clock.get("stream_rate_hz") == rate and (
                clock.get("state") == "tracking"):
            return float(poll["elapsed_s"])
    return None


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


FILL_BUSY_BUDGET_S = 120.0


def board_post_full(base: str, auth: str, path: str,
                    body: str) -> tuple[int, str, str | None]:
    """Like board_post, but also return (body text, Retry-After header)."""
    request = urllib.request.Request(base + path, method="POST",
                                     data=body.encode("utf-8"))
    request.add_header("X-Aius-SD-Auth", auth)
    try:
        with urllib.request.urlopen(request, timeout=8) as response:
            return (response.status, response.read().decode("utf-8", "replace"),
                    response.headers.get("Retry-After"))
    except urllib.error.HTTPError as error:
        return (error.code, error.read().decode("utf-8", "replace"),
                error.headers.get("Retry-After") if error.headers else None)


def replay_start(base: str, auth: str, body: str, *, post=board_post_full,
                 sleep=time.sleep, clock=time.monotonic,
                 budget_s: float = FILL_BUSY_BUDGET_S) -> int:
    """POST /api/replays/start, retrying only `503 fill_busy` (the web-cache
    fill walk is incomplete) per Retry-After (absent or bad = 1 s) within
    `budget_s`; any other status is returned for the caller's own handling."""
    started = clock()
    while True:
        status, text, retry_after = post(base, auth, "/api/replays/start", body)
        if status != 503 or "fill_busy" not in text:
            return status
        try:
            wait = max(float(retry_after), 0.0)
        except (TypeError, ValueError):
            wait = 1.0
        if clock() - started + wait > budget_s:
            raise RuntimeError(
                f"replays/start still 503 fill_busy after {budget_s:.0f} s "
                "(web-cache fill walk never completed)")
        sleep(wait)


class RecorderClient:
    """Minimal /api/recorder client: header read, freeze/thaw, PUT config,
    and slot data reads reassembled to logical (oldest-first) frame order."""

    CONFIG_KEYS = ("overrun", "underrun", "carrier_loss", "limiter_gr",
                   "governor_trim", "missed_fill", "crc_reject",
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
            "limiter_gr": False, "governor_trim": False,
            "missed_fill": False, "crc_reject": False,
            "gr_threshold_db": 0.0,
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
    parser.add_argument(
        "--phase-seconds", type=parse_phase_seconds,
        default=DEFAULT_PHASE_SECONDS,
        help="seconds per aplay phase: one value for all three, or "
             "first,middle,last (48 kHz lead-in, 44.1 kHz middle, 48 kHz "
             "trailing). Default 2,3,1.5: the middle outlasts the board's "
             "1.85 s 44.1 kHz acceptance latency by three status polls and "
             "the whole stimulus fits the 262144-frame recorder slot")
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
    # ring must hold everything from just before the first boundary to the
    # freeze -- the middle and trailing phases at their own rates plus the
    # stimulus margin -- or the first boundary rolls off the front before
    # the freeze. The lead-in's body is free.
    phases = 3
    phase_tones = [args.tone_48, args.tone_44, args.tone_48]
    phase_rates = [tone_rate_hz(tone) for tone in phase_tones]
    frames_per_phase = [phase_frames(seconds, rate) for seconds, rate
                        in zip(args.phase_seconds, phase_rates)]
    first_s, middle_s, last_s = args.phase_seconds
    baseline_header = client.header()
    baseline = frozen_identities(baseline_header)
    capacity = slot_capacity_frames(
        baseline_header,
        (baseline_header.get("slots") or [None])[0])
    required = required_capture_frames(args.phase_seconds, phase_rates)
    if capacity and required > capacity and not args.allow_rolloff:
        print(
            f"recorder slot holds {capacity} frames; the frames from just "
            f"before the first rate change to the freeze -- the {middle_s:g}s "
            f"middle phase at {phase_rates[1]} Hz ({frames_per_phase[1]}) "
            f"plus the {last_s:g}s trailing phase at {phase_rates[2]} Hz "
            f"({frames_per_phase[2]}) plus {STIMULUS_MARGIN_S:g}s stimulus "
            f"margin -- need {required}; the {first_s:g}s lead-in costs "
            f"nothing. The first rate change would roll off the ring before "
            f"the freeze. Shorten the middle or trailing value of "
            f"--phase-seconds or pass --allow-rolloff to run anyway.",
            file=sys.stderr)
        return 3

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

    def aplay(tone: Path, frames: int) -> None:
        # --samples, not --duration: aplay's -d is whole seconds and would
        # truncate a fractional phase below the board's follow latency.
        subprocess.run(["aplay", "-D", args.device,
                        "-s", str(frames), str(tone)],
                       capture_output=True, check=False)

    poller = threading.Thread(target=poll_loop, daemon=True)
    meta = None
    header = baseline_header
    switch_back_elapsed: float | None = None
    reference_seen_elapsed: float | None = None
    freeze_elapsed: float | None = None
    try:
        client.disable_auto_triggers()
        poller.start()

        aplay(phase_tones[0], frames_per_phase[0])
        aplay(phase_tones[1], frames_per_phase[1])
        switch_back_elapsed = time.monotonic() - started
        aplay(phase_tones[2], frames_per_phase[2])
        # Freeze once a poll since the switch back shows the board tracking
        # the reference rate again -- the second fiducial is then stamped
        # -- plus a dwell for the classifier; the U24 keeps the 48 kHz
        # carrier up after aplay returns, so acceptance can complete after
        # the trailing phase ends. Bounded: see FREEZE_ACCEPTANCE_TIMEOUT_S.
        deadline = time.monotonic() + FREEZE_ACCEPTANCE_TIMEOUT_S
        reference_seen_elapsed = tracking_seen_since(
            polls, REFERENCE_RATE, switch_back_elapsed)
        while reference_seen_elapsed is None and time.monotonic() < deadline:
            time.sleep(0.05)
            reference_seen_elapsed = tracking_seen_since(
                polls, REFERENCE_RATE, switch_back_elapsed)
        time.sleep(FREEZE_DWELL_S)
        client.freeze()
        freeze_elapsed = time.monotonic() - started
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
                    status = replay_start(
                        args.base, auth,
                        json.dumps({"id": args.replay_id, "loop": False}))
                    print(f"replays/start -> {status}", flush=True)
                    if status == 200:
                        break
                    time.sleep(0.8)                   # carrier not gone yet
                time.sleep(3.5)                       # dwell in replay
            finally:
                # The evidence ends with the declared stimulus (`expected_phases`
                # ends at `replay`). Restoring the U24 is bench housekeeping, and
                # its re-enumeration edge -- garbage-rate unsupported episodes,
                # the interrupted replay unit abandoned once with
                # pipeline_last_failure TRUNCATED (MALFORMED on images before
                # 2026-09-03) -- is a replay->live transition nobody declared;
                # polled, the analyzer judged it as unattributed (live run
                # 2026-09-03).
                stop.set()
                poller.join(timeout=5)
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
        # `phase_seconds` keeps its historic scalar meaning (one duration
        # for every phase) and is null when the phases differ; the
        # per-phase list is authoritative.
        "phase_seconds": first_s if first_s == middle_s == last_s else None,
        "phase_seconds_per_phase": list(args.phase_seconds),
        "phase_tones": [str(tone) for tone in phase_tones],
        "phase_rates_hz": phase_rates,
        "phase_frames": frames_per_phase,
        "phases": phases,
        "acceptance_latency_budget_s": ACCEPTANCE_LATENCY_BUDGET_S,
        "switch_back_elapsed_s": None if switch_back_elapsed is None
        else round(switch_back_elapsed, 3),
        "reference_tracking_seen_elapsed_s": None
        if reference_seen_elapsed is None else round(reference_seen_elapsed, 3),
        "freeze_elapsed_s": None if freeze_elapsed is None
        else round(freeze_elapsed, 3),
        "freeze_trigger": None if freeze_elapsed is None else (
            "reference-rate-tracking" if reference_seen_elapsed is not None
            else "acceptance-timeout"),
        "driven_phase_labels": driven_phase_labels(args.with_replay),
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
            f"{TRANSITION_RATE}: the {middle_s:g}s middle phase was shorter "
            f"than the board's follow latency, so no rate change was stamped "
            f"and there was nothing to freeze. Lengthen the middle value of "
            f"--phase-seconds (first,middle,last); this is a stimulus "
            f"problem, not a freeze that failed to land")

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
                   "max_follow_error_millihz": 1000,
                   "expected_phases": driven_phase_labels(args.with_replay)}
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
