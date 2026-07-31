#!/usr/bin/env python3
"""Collect a frame-synchronised ALSA playback/capture probe.

The helper deliberately uses libasound through ctypes. The target guest has
libasound.so.2 but no ALSA development headers. Playback is prefilled below its
automatic start threshold, linked to capture, and both streams are started by
one explicit trigger.
"""

from __future__ import annotations

import argparse
import ctypes
import errno
import json
from pathlib import Path
import threading
import time
import urllib.request

import numpy as np


SND_PCM_STREAM_PLAYBACK = 0
SND_PCM_STREAM_CAPTURE = 1
SND_PCM_ACCESS_RW_INTERLEAVED = 3
SND_PCM_FORMAT_S16_LE = 2
SND_PCM_TSTAMP_ENABLE = 1
SND_PCM_TSTAMP_TYPE_MONOTONIC = 1
SND_PCM_STATE_RUNNING = 3
ALSA_EPIPE = -errno.EPIPE
MAX_LINKED_TRIGGER_SKEW_FRAMES = 1.0
UINT32_MASK = 0xFFFFFFFF
# The firmware applies at most one same-nominal clock trim every ten seconds.
# Both preflight acceptance modes and the formal bracket budget are expressed
# against that single device fact.
TRIM_RATE_LIMIT_SECONDS = 10.0
# An already-converged device emits no further trim edge, so acceptance also
# comes from a quiet window. It is deliberately longer than the rate limit: a
# pending trim would have to land inside a window of this length.
SETTLED_WINDOW_SECONDS = 15.0
# The preflight feed runs its status fetches inline, so its ring is sized to
# about a second instead of the formal buffer-period (~80 ms) that one slow
# HTTP response underruns. The formal collection keeps its exact geometry.
PREFLIGHT_BUFFER_SECONDS = 1.0
MAX_PREFLIGHT_XRUN_RECOVERIES = 5
ACTIVE_SOURCE_MODE_EDGE = "edge"
ACTIVE_SOURCE_MODE_SETTLED = "settled"
ACTIVE_SOURCE_ACCEPTED_MODES = (
    ACTIVE_SOURCE_MODE_EDGE,
    ACTIVE_SOURCE_MODE_SETTLED,
)
# Gate decisions. "settled" doubles as its acceptance mode; "confirm" asks the
# caller for exactly one fresh status snapshot before an edge is accepted.
ACTIVE_SOURCE_CONTINUE = "continue"
ACTIVE_SOURCE_CONFIRM = "confirm"
ACTIVE_SOURCE_COUNTER_FIELDS = ("trim_updates", "invalid_windows")


class AlsaError(RuntimeError):
    """A libasound call failed."""


class AlsaTimespec(ctypes.Structure):
    _fields_ = [
        ("seconds", ctypes.c_long),
        ("nanoseconds", ctypes.c_long),
    ]


class Alsa:
    def __init__(self) -> None:
        self.lib = ctypes.CDLL("libasound.so.2")
        self._declare()

    def _declare(self) -> None:
        lib = self.lib
        void_pp = ctypes.POINTER(ctypes.c_void_p)
        ulong_p = ctypes.POINTER(ctypes.c_ulong)
        uint_p = ctypes.POINTER(ctypes.c_uint)
        int_p = ctypes.POINTER(ctypes.c_int)

        lib.snd_strerror.argtypes = [ctypes.c_int]
        lib.snd_strerror.restype = ctypes.c_char_p
        lib.snd_pcm_open.argtypes = [
            void_pp, ctypes.c_char_p, ctypes.c_int, ctypes.c_int
        ]
        lib.snd_pcm_open.restype = ctypes.c_int
        lib.snd_pcm_close.argtypes = [ctypes.c_void_p]
        lib.snd_pcm_close.restype = ctypes.c_int
        lib.snd_pcm_hw_params_malloc.argtypes = [void_pp]
        lib.snd_pcm_hw_params_malloc.restype = ctypes.c_int
        lib.snd_pcm_hw_params_free.argtypes = [ctypes.c_void_p]
        lib.snd_pcm_hw_params_any.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p
        ]
        lib.snd_pcm_hw_params_any.restype = ctypes.c_int
        lib.snd_pcm_hw_params_set_access.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int
        ]
        lib.snd_pcm_hw_params_set_access.restype = ctypes.c_int
        lib.snd_pcm_hw_params_set_format.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int
        ]
        lib.snd_pcm_hw_params_set_format.restype = ctypes.c_int
        lib.snd_pcm_hw_params_set_channels.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint
        ]
        lib.snd_pcm_hw_params_set_channels.restype = ctypes.c_int
        lib.snd_pcm_hw_params_set_rate_near.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, uint_p, int_p
        ]
        lib.snd_pcm_hw_params_set_rate_near.restype = ctypes.c_int
        lib.snd_pcm_hw_params_set_period_size_near.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ulong_p, int_p
        ]
        lib.snd_pcm_hw_params_set_period_size_near.restype = ctypes.c_int
        lib.snd_pcm_hw_params_set_buffer_size_near.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ulong_p
        ]
        lib.snd_pcm_hw_params_set_buffer_size_near.restype = ctypes.c_int
        lib.snd_pcm_hw_params.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p
        ]
        lib.snd_pcm_hw_params.restype = ctypes.c_int
        lib.snd_pcm_sw_params_malloc.argtypes = [void_pp]
        lib.snd_pcm_sw_params_malloc.restype = ctypes.c_int
        lib.snd_pcm_sw_params_free.argtypes = [ctypes.c_void_p]
        lib.snd_pcm_sw_params_current.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p
        ]
        lib.snd_pcm_sw_params_current.restype = ctypes.c_int
        lib.snd_pcm_sw_params_set_avail_min.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong
        ]
        lib.snd_pcm_sw_params_set_avail_min.restype = ctypes.c_int
        lib.snd_pcm_sw_params_set_start_threshold.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong
        ]
        lib.snd_pcm_sw_params_set_start_threshold.restype = ctypes.c_int
        lib.snd_pcm_sw_params_set_tstamp_mode.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int
        ]
        lib.snd_pcm_sw_params_set_tstamp_mode.restype = ctypes.c_int
        lib.snd_pcm_sw_params_set_tstamp_type.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int
        ]
        lib.snd_pcm_sw_params_set_tstamp_type.restype = ctypes.c_int
        lib.snd_pcm_sw_params.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p
        ]
        lib.snd_pcm_sw_params.restype = ctypes.c_int
        lib.snd_pcm_link.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        lib.snd_pcm_link.restype = ctypes.c_int
        lib.snd_pcm_unlink.argtypes = [ctypes.c_void_p]
        lib.snd_pcm_unlink.restype = ctypes.c_int
        lib.snd_pcm_prepare.argtypes = [ctypes.c_void_p]
        lib.snd_pcm_prepare.restype = ctypes.c_int
        lib.snd_pcm_start.argtypes = [ctypes.c_void_p]
        lib.snd_pcm_start.restype = ctypes.c_int
        lib.snd_pcm_drop.argtypes = [ctypes.c_void_p]
        lib.snd_pcm_drop.restype = ctypes.c_int
        lib.snd_pcm_writei.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong
        ]
        lib.snd_pcm_writei.restype = ctypes.c_long
        lib.snd_pcm_readi.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong
        ]
        lib.snd_pcm_readi.restype = ctypes.c_long
        lib.snd_pcm_delay.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_long)
        ]
        lib.snd_pcm_delay.restype = ctypes.c_int
        lib.snd_pcm_status_malloc.argtypes = [void_pp]
        lib.snd_pcm_status_malloc.restype = ctypes.c_int
        lib.snd_pcm_status_free.argtypes = [ctypes.c_void_p]
        lib.snd_pcm_status_free.restype = None
        lib.snd_pcm_status.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p
        ]
        lib.snd_pcm_status.restype = ctypes.c_int
        lib.snd_pcm_status_get_state.argtypes = [ctypes.c_void_p]
        lib.snd_pcm_status_get_state.restype = ctypes.c_int
        lib.snd_pcm_status_get_trigger_htstamp.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(AlsaTimespec)
        ]
        lib.snd_pcm_status_get_trigger_htstamp.restype = None
        lib.snd_pcm_status_get_htstamp.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(AlsaTimespec)
        ]
        lib.snd_pcm_status_get_htstamp.restype = None
        lib.snd_pcm_status_get_delay.argtypes = [ctypes.c_void_p]
        lib.snd_pcm_status_get_delay.restype = ctypes.c_long
        lib.snd_pcm_status_get_avail.argtypes = [ctypes.c_void_p]
        lib.snd_pcm_status_get_avail.restype = ctypes.c_ulong
        lib.snd_pcm_status_get_avail_max.argtypes = [ctypes.c_void_p]
        lib.snd_pcm_status_get_avail_max.restype = ctypes.c_ulong
        lib.snd_pcm_status_get_overrange.argtypes = [ctypes.c_void_p]
        lib.snd_pcm_status_get_overrange.restype = ctypes.c_ulong

    def check(self, result: int, operation: str) -> int:
        if result < 0:
            message = self.lib.snd_strerror(result).decode(
                "utf-8", errors="replace"
            )
            raise AlsaError(f"{operation}: {message} ({result})")
        return result

    def open(self, device: str, stream: int) -> ctypes.c_void_p:
        handle = ctypes.c_void_p()
        self.check(
            self.lib.snd_pcm_open(
                ctypes.byref(handle), device.encode("utf-8"), stream, 0
            ),
            "snd_pcm_open",
        )
        return handle

    def configure(
        self,
        handle: ctypes.c_void_p,
        rate: int,
        period: int,
        buffer_size: int,
        playback: bool,
        exact: bool = True,
    ) -> dict:
        """Negotiate stream geometry.

        The formal collection requires the exact requested geometry. The
        preflight passes ``exact=False``: it asks for a large ring and keeps
        whatever the device grants, because only the rate is load-bearing there.
        """
        hw = ctypes.c_void_p()
        sw = ctypes.c_void_p()
        self.check(
            self.lib.snd_pcm_hw_params_malloc(ctypes.byref(hw)),
            "snd_pcm_hw_params_malloc",
        )
        try:
            self.check(
                self.lib.snd_pcm_hw_params_any(handle, hw),
                "snd_pcm_hw_params_any",
            )
            self.check(
                self.lib.snd_pcm_hw_params_set_access(
                    handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED
                ),
                "snd_pcm_hw_params_set_access",
            )
            self.check(
                self.lib.snd_pcm_hw_params_set_format(
                    handle, hw, SND_PCM_FORMAT_S16_LE
                ),
                "snd_pcm_hw_params_set_format",
            )
            self.check(
                self.lib.snd_pcm_hw_params_set_channels(handle, hw, 2),
                "snd_pcm_hw_params_set_channels",
            )
            actual_rate = ctypes.c_uint(rate)
            direction = ctypes.c_int(0)
            self.check(
                self.lib.snd_pcm_hw_params_set_rate_near(
                    handle, hw, ctypes.byref(actual_rate),
                    ctypes.byref(direction)
                ),
                "snd_pcm_hw_params_set_rate_near",
            )
            actual_period = ctypes.c_ulong(period)
            direction = ctypes.c_int(0)
            self.check(
                self.lib.snd_pcm_hw_params_set_period_size_near(
                    handle, hw, ctypes.byref(actual_period),
                    ctypes.byref(direction)
                ),
                "snd_pcm_hw_params_set_period_size_near",
            )
            actual_buffer = ctypes.c_ulong(buffer_size)
            self.check(
                self.lib.snd_pcm_hw_params_set_buffer_size_near(
                    handle, hw, ctypes.byref(actual_buffer)
                ),
                "snd_pcm_hw_params_set_buffer_size_near",
            )
            self.check(
                self.lib.snd_pcm_hw_params(handle, hw),
                "snd_pcm_hw_params",
            )
        finally:
            self.lib.snd_pcm_hw_params_free(hw)

        if actual_rate.value != rate:
            raise AlsaError(
                f"inexact ALSA rate: {actual_rate.value} != {rate}"
            )
        if exact and (
            actual_period.value != period
            or actual_buffer.value != buffer_size
        ):
            raise AlsaError(
                "inexact ALSA geometry: "
                f"rate={actual_rate.value}, period={actual_period.value}, "
                f"buffer={actual_buffer.value}"
            )

        self.check(
            self.lib.snd_pcm_sw_params_malloc(ctypes.byref(sw)),
            "snd_pcm_sw_params_malloc",
        )
        try:
            self.check(
                self.lib.snd_pcm_sw_params_current(handle, sw),
                "snd_pcm_sw_params_current",
            )
            self.check(
                self.lib.snd_pcm_sw_params_set_avail_min(
                    handle, sw, actual_period.value
                ),
                "snd_pcm_sw_params_set_avail_min",
            )
            threshold = (
                actual_buffer.value if playback else actual_period.value
            )
            self.check(
                self.lib.snd_pcm_sw_params_set_start_threshold(
                    handle, sw, threshold
                ),
                "snd_pcm_sw_params_set_start_threshold",
            )
            self.check(
                self.lib.snd_pcm_sw_params_set_tstamp_mode(
                    handle, sw, SND_PCM_TSTAMP_ENABLE
                ),
                "snd_pcm_sw_params_set_tstamp_mode",
            )
            self.check(
                self.lib.snd_pcm_sw_params_set_tstamp_type(
                    handle, sw, SND_PCM_TSTAMP_TYPE_MONOTONIC
                ),
                "snd_pcm_sw_params_set_tstamp_type",
            )
            self.check(
                self.lib.snd_pcm_sw_params(handle, sw),
                "snd_pcm_sw_params",
            )
        finally:
            self.lib.snd_pcm_sw_params_free(sw)

        return {
            "rate_hz": actual_rate.value,
            "period_frames": actual_period.value,
            "buffer_frames": actual_buffer.value,
            "start_threshold_frames": threshold,
            "timestamp_type": "monotonic",
        }

    def status(self, handle: ctypes.c_void_p) -> dict:
        status = ctypes.c_void_p()
        self.check(
            self.lib.snd_pcm_status_malloc(ctypes.byref(status)),
            "snd_pcm_status_malloc",
        )
        try:
            self.check(
                self.lib.snd_pcm_status(handle, status),
                "snd_pcm_status",
            )
            trigger = AlsaTimespec()
            observed = AlsaTimespec()
            self.lib.snd_pcm_status_get_trigger_htstamp(
                status, ctypes.byref(trigger)
            )
            self.lib.snd_pcm_status_get_htstamp(
                status, ctypes.byref(observed)
            )
            return {
                "state": self.lib.snd_pcm_status_get_state(status),
                "trigger_timestamp_ns": (
                    trigger.seconds * 1_000_000_000
                    + trigger.nanoseconds
                ),
                "observed_timestamp_ns": (
                    observed.seconds * 1_000_000_000
                    + observed.nanoseconds
                ),
                "delay_frames": self.lib.snd_pcm_status_get_delay(status),
                "available_frames": (
                    self.lib.snd_pcm_status_get_avail(status)
                ),
                "available_max_frames": (
                    self.lib.snd_pcm_status_get_avail_max(status)
                ),
                "overrange_frames": (
                    self.lib.snd_pcm_status_get_overrange(status)
                ),
            }
        finally:
            self.lib.snd_pcm_status_free(status)


def get_json(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=5.0) as response:
        return json.load(response)


def active_source_required_health(status: dict) -> bool:
    """Every latched condition the source must satisfy to be measurable."""
    clock = status.get("clock", {})
    pipeline = status.get("pipeline_diag", {})
    audio = status.get("audio_diag", {})
    return (
        status.get("carrier") == 1
        and clock.get("carrier_locked") is True
        and clock.get("carrier_ready") is True
        and clock.get("master") == "input"
        and clock.get("state") == "tracking"
        and clock.get("nominal_rate_hz") == 48000
        and clock.get("stream_rate_hz") == 48000
        and pipeline.get("class") == 1
        and pipeline.get("pcm_confirmed") == 1
        and pipeline.get("last_status") == 0
        and audio.get("pipeline_last_failure") == 0
        and audio.get("service_last_failure") == 0
    )


def active_source_follow_error_millihz(status: dict) -> int | None:
    clock = status.get("clock", {})
    carrier = clock.get("carrier_rate_millihz")
    output = clock.get("output_rate_millihz")
    for value in (carrier, output):
        if not isinstance(value, int) or isinstance(value, bool):
            return None
    return abs(output - carrier)


def active_source_counters(status: dict) -> tuple[int, ...] | None:
    clock = status.get("clock", {})
    values = []
    for field in ACTIVE_SOURCE_COUNTER_FIELDS:
        value = clock.get(field)
        if not isinstance(value, int) or isinstance(value, bool):
            return None
        values.append(value)
    return tuple(values)


class ActiveSourceGate:
    """Judge whether the optical source's clock has provably converged.

    Two acceptance modes, both expressed against the device's one-trim-per-ten-
    seconds rate limit:

    * ``edge`` -- cumulative deltas against a healthy counter baseline reach
      exactly +1 ``trim_updates`` and +1 ``invalid_windows``. The two increments
      may straddle separate polls, because the device publishes them
      independently. A cumulative delta above one means a trim burst is still
      running, so the baseline moves to the current values. A candidate edge is
      accepted only after one fresh confirming snapshot that is healthy, inside
      the follow-error bound and carries no further counter movement.
    * ``settled`` -- a continuous window of at least ``settled_window_seconds``
      in which every poll is healthy, inside the follow-error bound and moves
      neither counter. A device that converged before the preflight opened
      never produces an edge, and would otherwise burn the whole timeout.
    """

    def __init__(
        self,
        minimum_seconds: float,
        maximum_follow_error_millihz: int,
        settled_window_seconds: float = SETTLED_WINDOW_SECONDS,
    ) -> None:
        self.minimum_seconds = minimum_seconds
        self.maximum_follow_error_millihz = maximum_follow_error_millihz
        self.settled_window_seconds = settled_window_seconds
        self.baseline: tuple[int, ...] | None = None
        self.candidate: tuple[int, ...] | None = None
        self.settled_since: float | None = None
        self.settled_counters: tuple[int, ...] | None = None

    def _within_bound(self, status: dict) -> bool:
        follow = active_source_follow_error_millihz(status)
        return (
            follow is not None
            and follow <= self.maximum_follow_error_millihz
        )

    def _deltas(self, counters: tuple[int, ...]) -> tuple[int, ...]:
        return tuple(
            (value - base) & UINT32_MASK
            for value, base in zip(counters, self.baseline)
        )

    def observe(self, status: dict, elapsed_seconds: float) -> str:
        """Fold one poll in; return a decision from the gate vocabulary."""
        counters = active_source_counters(status)
        healthy = active_source_required_health(status)
        clean = healthy and self._within_bound(status)
        if counters is None:
            self.settled_since = None
            self.settled_counters = None
            return ACTIVE_SOURCE_CONTINUE
        if clean and self.settled_counters == counters:
            if (
                self.settled_since is not None
                and elapsed_seconds - self.settled_since
                >= self.settled_window_seconds
                and elapsed_seconds >= self.minimum_seconds
            ):
                return ACTIVE_SOURCE_MODE_SETTLED
        elif clean:
            self.settled_since = elapsed_seconds
            self.settled_counters = counters
        else:
            self.settled_since = None
            self.settled_counters = None
        if self.baseline is None:
            if healthy:
                self.baseline = counters
            return ACTIVE_SOURCE_CONTINUE
        deltas = self._deltas(counters)
        if any(delta > 1 for delta in deltas):
            self.baseline = counters
            return ACTIVE_SOURCE_CONTINUE
        if (
            all(delta == 1 for delta in deltas)
            and healthy
            and elapsed_seconds >= self.minimum_seconds
        ):
            self.candidate = counters
            return ACTIVE_SOURCE_CONFIRM
        return ACTIVE_SOURCE_CONTINUE

    def confirm(self, status: dict, elapsed_seconds: float) -> bool:
        """Judge the one fresh snapshot taken after a candidate edge."""
        candidate = self.candidate
        self.candidate = None
        counters = active_source_counters(status)
        if (
            counters is None
            or counters != candidate
            or not active_source_required_health(status)
        ):
            # The counters moved again (or health lapsed): the burst is still
            # running, so start over from what the device reports now.
            self.baseline = counters
            return False
        if not self._within_bound(status):
            # The edge was clean but the follow error has not converged yet;
            # keep polling from the confirmed counter values for the next one.
            self.baseline = candidate
            return False
        return True


def active_source_projection(
    status: dict,
    elapsed_seconds: float,
    phase: str = "poll",
) -> dict:
    """Every field the acceptance predicates read, plus the raw counters.

    A timed-out preflight is only diagnosable from its artifact, so the
    projection names each health condition rather than a summary verdict.
    """
    clock = status.get("clock", {})
    pipeline = status.get("pipeline_diag", {})
    audio = status.get("audio_diag", {})
    return {
        "phase": phase,
        "elapsed_seconds": elapsed_seconds,
        "poll_unix_seconds": time.time(),
        "healthy": active_source_required_health(status),
        "carrier": status.get("carrier"),
        "carrier_locked": clock.get("carrier_locked"),
        "carrier_ready": clock.get("carrier_ready"),
        "master": clock.get("master"),
        "state": clock.get("state"),
        "nominal_rate_hz": clock.get("nominal_rate_hz"),
        "stream_rate_hz": clock.get("stream_rate_hz"),
        "class": pipeline.get("class"),
        "pcm_confirmed": pipeline.get("pcm_confirmed"),
        "pipeline_last_status": pipeline.get("last_status"),
        "pipeline_last_failure": audio.get("pipeline_last_failure"),
        "service_last_failure": audio.get("service_last_failure"),
        "carrier_rate_millihz": clock.get("carrier_rate_millihz"),
        "output_rate_millihz": clock.get("output_rate_millihz"),
        "follow_error_millihz": active_source_follow_error_millihz(status),
        "trim_updates": clock.get("trim_updates"),
        "invalid_windows": clock.get("invalid_windows"),
    }


def preflight_active_source(
    device: str,
    rate: int,
    period: int,
    buffer_size: int,
    status_url: str | None,
    minimum_seconds: float,
    timeout_seconds: float,
    poll_seconds: float,
    maximum_follow_error_millihz: int,
    *,
    artifact_path: Path | str | None = None,
    settled_window_seconds: float = SETTLED_WINDOW_SECONDS,
    alsa: Alsa | None = None,
    fetch_status=get_json,
    monotonic=time.monotonic,
) -> dict:
    """Hold the optical source active until its clock is provably converged.

    Zero PCM frames keep the source active while `GET /api/status` is polled.
    Acceptance is an observed trim edge or a settled window; see
    `ActiveSourceGate`. The ring is sized to `PREFLIGHT_BUFFER_SECONDS` (or the
    caller's buffer, whichever is larger) because the status fetches run inline,
    and an XRUN is recovered rather than fatal -- up to
    `MAX_PREFLIGHT_XRUN_RECOVERIES` times. `alsa`, `fetch_status` and
    `monotonic` are host-test injection points; production uses the defaults.
    """
    manifest: dict = {
        "state": "running",
        "device": device,
        "format": "S16_LE",
        "channels": 2,
        "minimum_seconds": minimum_seconds,
        "timeout_seconds": timeout_seconds,
        "poll_seconds": poll_seconds,
        "settled_window_seconds": settled_window_seconds,
        "maximum_follow_error_millihz": (
            maximum_follow_error_millihz
        ),
        "maximum_xrun_recoveries": MAX_PREFLIGHT_XRUN_RECOVERIES,
        "artifact_path": (
            None if artifact_path is None else str(artifact_path)
        ),
        "xrun_recoveries": 0,
        "observations": [],
    }
    artifact_label = (
        "no artifact path was supplied"
        if artifact_path is None
        else str(artifact_path)
    )
    alsa = Alsa() if alsa is None else alsa
    playback: ctypes.c_void_p | None = None
    try:
        if status_url is None:
            raise AlsaError(
                "active-source preflight requires a status URL; "
                "pass --status-url"
            )
        requested_buffer = max(
            buffer_size, int(rate * PREFLIGHT_BUFFER_SECONDS)
        )
        manifest["requested_buffer_frames"] = requested_buffer
        playback = alsa.open(device, SND_PCM_STREAM_PLAYBACK)
        geometry = alsa.configure(
            playback, rate, period, requested_buffer, True, exact=False
        )
        manifest["playback"] = geometry
        granted_period = int(geometry["period_frames"])
        granted_buffer = int(geometry["buffer_frames"])
        if granted_buffer <= granted_period:
            raise AlsaError(
                "active-source ring is not larger than one period: "
                f"buffer={granted_buffer}, period={granted_period}"
            )
        prefill = np.zeros((granted_buffer - granted_period, 2), dtype="<i2")
        block = np.zeros((granted_period, 2), dtype="<i2")

        def prime() -> None:
            alsa.check(
                alsa.lib.snd_pcm_prepare(playback), "prepare playback"
            )
            prefilled = alsa.check(
                alsa.lib.snd_pcm_writei(
                    playback,
                    ctypes.c_void_p(prefill.ctypes.data),
                    len(prefill),
                ),
                "prefill active-source playback",
            )
            if prefilled != len(prefill):
                raise AlsaError(
                    f"short active-source prefill: {prefilled}/{len(prefill)}"
                )
            alsa.check(alsa.lib.snd_pcm_start(playback), "start playback")

        def feed() -> None:
            written = alsa.lib.snd_pcm_writei(
                playback,
                ctypes.c_void_p(block.ctypes.data),
                len(block),
            )
            if written == ALSA_EPIPE:
                manifest["xrun_recoveries"] += 1
                if (
                    manifest["xrun_recoveries"]
                    > MAX_PREFLIGHT_XRUN_RECOVERIES
                ):
                    raise AlsaError(
                        "active-source preflight underran "
                        f"{manifest['xrun_recoveries']} times, above the "
                        f"{MAX_PREFLIGHT_XRUN_RECOVERIES} allowed"
                    )
                prime()
                return
            alsa.check(written, "active-source playback")
            if written != len(block):
                raise AlsaError(
                    f"short active-source transfer: {written}/{len(block)}"
                )

        def accept(mode: str, status: dict, elapsed: float) -> dict:
            manifest["state"] = "accepted"
            manifest["mode"] = mode
            manifest["accepted_status"] = status
            manifest["elapsed_seconds"] = elapsed
            manifest["alsa_status_end"] = alsa.status(playback)
            if manifest["alsa_status_end"].get("state") != (
                SND_PCM_STATE_RUNNING
            ):
                raise AlsaError("active-source playback is not running")
            return manifest

        prime()
        # One deadline for the whole preflight, re-read after every blocking
        # call (a status fetch waits up to 5 s, snd_pcm_writei up to one ring),
        # so no single block can overshoot the budget.
        started = monotonic()
        deadline = started + timeout_seconds
        next_poll = started
        gate = ActiveSourceGate(
            minimum_seconds,
            maximum_follow_error_millihz,
            settled_window_seconds,
        )

        def expired(now: float) -> None:
            if now < deadline:
                return
            raise AlsaError(
                "active-source preflight timed out after "
                f"{now - started:.3f}s without a confirmed trim edge or a "
                f"{settled_window_seconds:.1f}s settled window; "
                f"observations: {artifact_label}"
            )

        while True:
            expired(monotonic())
            if monotonic() >= next_poll:
                current = fetch_status(status_url)
                now = monotonic()
                elapsed = now - started
                manifest["observations"].append(
                    active_source_projection(current, elapsed)
                )
                decision = gate.observe(current, elapsed)
                if decision == ACTIVE_SOURCE_MODE_SETTLED:
                    return accept(
                        ACTIVE_SOURCE_MODE_SETTLED, current, elapsed
                    )
                if decision == ACTIVE_SOURCE_CONFIRM:
                    confirming = fetch_status(status_url)
                    now = monotonic()
                    elapsed = now - started
                    manifest["observations"].append(
                        active_source_projection(
                            confirming, elapsed, "confirm"
                        )
                    )
                    if gate.confirm(confirming, elapsed):
                        return accept(
                            ACTIVE_SOURCE_MODE_EDGE, confirming, elapsed
                        )
                next_poll = now + poll_seconds
                expired(monotonic())
            feed()
    except BaseException as error:
        manifest["state"] = "rejected"
        manifest["failure"] = str(error)
        setattr(error, "aius_active_source_manifest", manifest)
        raise
    finally:
        if playback is not None:
            alsa.lib.snd_pcm_drop(playback)
            alsa.lib.snd_pcm_close(playback)


def validate_linked_trigger(
    playback: dict,
    capture: dict,
    rate: int,
) -> dict:
    playback_trigger = playback.get("trigger_timestamp_ns")
    capture_trigger = capture.get("trigger_timestamp_ns")
    if (
        not isinstance(playback_trigger, int)
        or playback_trigger == 0
        or not isinstance(capture_trigger, int)
        or capture_trigger == 0
    ):
        raise AlsaError(
            "linked streams do not report nonzero trigger timestamps"
        )
    if (
        playback.get("state") != SND_PCM_STATE_RUNNING
        or capture.get("state") != SND_PCM_STATE_RUNNING
    ):
        raise AlsaError("linked streams are not both running after trigger")
    skew_ns = abs(capture_trigger - playback_trigger)
    if rate <= 0 or skew_ns * rate >= 1_000_000_000:
        raise AlsaError(
            "linked trigger timestamp skew is not sub-frame"
        )
    return {
        "linked_trigger_timestamp_ns": playback_trigger,
        "linked_playback_trigger_timestamp_ns": playback_trigger,
        "linked_capture_trigger_timestamp_ns": capture_trigger,
        "linked_trigger_skew_ns": skew_ns,
        "linked_trigger_skew_frames": (
            skew_ns * rate / 1_000_000_000
        ),
        "maximum_linked_trigger_skew_frames": (
            MAX_LINKED_TRIGGER_SKEW_FRAMES
        ),
    }


def validate_collection_manifest(
    manifest: dict,
    rate: int,
    period: int,
    buffer_size: int,
) -> dict:
    expected = {
        "rate_hz": rate,
        "period_frames": period,
        "buffer_frames": buffer_size,
    }
    failures = []
    for stream in ("playback", "capture"):
        geometry = manifest.get(stream, {})
        for field, value in expected.items():
            if geometry.get(field) != value:
                failures.append(f"{stream}.{field} is inexact")
        threshold = buffer_size if stream == "playback" else period
        if geometry.get("start_threshold_frames") != threshold:
            failures.append(f"{stream}.start_threshold_frames is wrong")
        if geometry.get("timestamp_type") != "monotonic":
            failures.append(f"{stream}.timestamp_type is not monotonic")
    if manifest.get("linked") is not True:
        failures.append("streams are not linked")
    post = manifest.get("alsa_status_post_trigger", {})
    trigger_evidence = {}
    try:
        trigger_evidence = validate_linked_trigger(
            post.get("playback", {}), post.get("capture", {}), rate
        )
    except AlsaError as error:
        failures.append(str(error))
    if manifest.get("xrun_recoveries") != 0:
        failures.append("XRUN recovery occurred")
    if manifest.get("short_transfers") != 0:
        failures.append("short transfer occurred")
    for phase in (
        "alsa_status_pre_trigger",
        "alsa_status_post_trigger",
        "alsa_status_end",
    ):
        snapshot = manifest.get(phase)
        if not isinstance(snapshot, dict):
            failures.append(f"{phase} is missing")
    end = manifest.get("alsa_status_end", {})
    for stream in ("playback", "capture"):
        status = end.get(stream, {}) if isinstance(end, dict) else {}
        if status.get("overrange_frames") != 0:
            failures.append(f"{stream} reported overrange")
    if failures:
        raise AlsaError(
            "collector evidence rejected: " + "; ".join(failures)
        )
    return {
        "accepted": True,
        **trigger_evidence,
        "rate_hz": rate,
        "period_frames": period,
        "buffer_frames": buffer_size,
        "xrun_recoveries": 0,
    }


def write_all(
    alsa: Alsa,
    handle: ctypes.c_void_p,
    samples: np.ndarray,
    start: int,
    chunk_frames: int,
) -> None:
    position = start
    while position < len(samples):
        requested = min(chunk_frames, len(samples) - position)
        view = np.ascontiguousarray(
            samples[position:position + requested]
        )
        written = alsa.check(
            alsa.lib.snd_pcm_writei(
                handle, ctypes.c_void_p(view.ctypes.data), len(view)
            ),
            "snd_pcm_writei",
        )
        if written == 0:
            raise AlsaError("snd_pcm_writei returned zero")
        if written != requested:
            raise AlsaError(
                f"short playback transfer: {written}/{requested}"
            )
        position += written


def read_all(
    alsa: Alsa,
    handle: ctypes.c_void_p,
    samples: np.ndarray,
    chunk_frames: int,
) -> None:
    position = 0
    while position < len(samples):
        requested = min(chunk_frames, len(samples) - position)
        view = samples[position:position + requested]
        read = alsa.check(
            alsa.lib.snd_pcm_readi(
                handle, ctypes.c_void_p(view.ctypes.data), len(view)
            ),
            "snd_pcm_readi",
        )
        if read == 0:
            raise AlsaError("snd_pcm_readi returned zero")
        if read != requested:
            raise AlsaError(
                f"short capture transfer: {read}/{requested}"
            )
        position += read


def collect(
    device: str,
    playback_samples: np.ndarray,
    rate: int,
    period: int,
    buffer_size: int,
    status_url: str | None,
    status_delay_s: float,
) -> tuple[np.ndarray, dict]:
    if (
        playback_samples.dtype != np.dtype("<i2")
        or playback_samples.ndim != 2
        or playback_samples.shape[1] != 2
        or not playback_samples.flags.c_contiguous
    ):
        raise ValueError("playback must be contiguous stereo <i2")

    tail_reserve = buffer_size // 2
    if len(playback_samples) <= tail_reserve:
        raise ValueError("playback is shorter than the capture tail reserve")
    capture_samples = np.empty(
        (len(playback_samples) - tail_reserve, 2), dtype="<i2"
    )
    failures: list[BaseException] = []
    manifest: dict = {
        "device": device,
        "format": "S16_LE",
        "channels": 2,
        "playback_frames": len(playback_samples),
        "capture_frames": len(capture_samples),
        "capture_tail_reserve_frames": tail_reserve,
        "xrun_recoveries": 0,
        "short_transfers": 0,
    }
    alsa = Alsa()
    playback: ctypes.c_void_p | None = None
    capture: ctypes.c_void_p | None = None
    linked = False
    try:
        playback = alsa.open(device, SND_PCM_STREAM_PLAYBACK)
        capture = alsa.open(device, SND_PCM_STREAM_CAPTURE)
        manifest["playback"] = alsa.configure(
            playback, rate, period, buffer_size, True
        )
        manifest["capture"] = alsa.configure(
            capture, rate, period, buffer_size, False
        )
        alsa.check(alsa.lib.snd_pcm_prepare(playback), "prepare playback")
        alsa.check(alsa.lib.snd_pcm_prepare(capture), "prepare capture")
        alsa.check(alsa.lib.snd_pcm_link(capture, playback), "snd_pcm_link")
        linked = True

        prefill = min(len(playback_samples), buffer_size - period)
        manifest["prefill_frames"] = prefill
        prefill_view = np.ascontiguousarray(playback_samples[:prefill])
        prefilled = alsa.check(
            alsa.lib.snd_pcm_writei(
                playback, ctypes.c_void_p(prefill_view.ctypes.data), prefill
            ),
            "prefill playback",
        )
        if prefilled != prefill:
            raise AlsaError(
                f"short playback prefill: {prefilled}/{prefill}"
            )
        manifest["alsa_status_pre_trigger"] = {
            "playback": alsa.status(playback),
            "capture": alsa.status(capture),
        }

        start_workers = threading.Event()
        abort_workers = threading.Event()

        def run_writer() -> None:
            try:
                start_workers.wait()
                if abort_workers.is_set():
                    return
                write_all(
                    alsa, playback, playback_samples, prefill, period
                )
            except BaseException as error:
                failures.append(error)

        def run_reader() -> None:
            try:
                start_workers.wait()
                if abort_workers.is_set():
                    return
                read_all(alsa, capture, capture_samples, period)
            except BaseException as error:
                failures.append(error)

        if status_url is not None:
            manifest["status_startup"] = get_json(status_url)
        reader = threading.Thread(target=run_reader, name="alsa-capture")
        writer = threading.Thread(target=run_writer, name="alsa-playback")
        reader.start()
        writer.start()
        started_ns = time.monotonic_ns()
        try:
            alsa.check(
                alsa.lib.snd_pcm_start(capture), "linked group start"
            )
        except BaseException:
            abort_workers.set()
            start_workers.set()
            reader.join()
            writer.join()
            raise
        manifest["trigger_monotonic_ns"] = started_ns
        try:
            manifest["alsa_status_post_trigger"] = {
                "playback": alsa.status(playback),
                "capture": alsa.status(capture),
            }
            validate_linked_trigger(
                manifest["alsa_status_post_trigger"]["playback"],
                manifest["alsa_status_post_trigger"]["capture"],
                rate,
            )
        except BaseException:
            abort_workers.set()
            start_workers.set()
            reader.join()
            writer.join()
            raise
        manifest["linked"] = True
        manifest["trigger_stream"] = "capture"
        start_workers.set()

        status_error: BaseException | None = None
        if status_url is not None:
            try:
                time.sleep(status_delay_s)
                manifest["status_initial"] = get_json(status_url)
            except BaseException as error:
                status_error = error

        writer.join(timeout=len(playback_samples) / rate + 10.0)
        reader.join(timeout=len(playback_samples) / rate + 10.0)
        if writer.is_alive() or reader.is_alive():
            raise AlsaError("linked I/O threads did not finish")
        if failures:
            raise failures[0]
        if status_error is not None:
            raise status_error

        if status_url is not None:
            manifest["status_final"] = get_json(status_url)

        manifest["alsa_status_end"] = {
            "playback": alsa.status(playback),
            "capture": alsa.status(capture),
        }
        delay = ctypes.c_long()
        if alsa.lib.snd_pcm_delay(playback, ctypes.byref(delay)) >= 0:
            manifest["playback_delay_at_end_frames"] = delay.value
        if alsa.lib.snd_pcm_delay(capture, ctypes.byref(delay)) >= 0:
            manifest["capture_delay_at_end_frames"] = delay.value
    except BaseException as error:
        manifest["collection_failure"] = str(error)
        setattr(error, "aius_collection_manifest", manifest)
        raise
    finally:
        if linked and capture is not None:
            alsa.lib.snd_pcm_unlink(capture)
        if capture is not None:
            alsa.lib.snd_pcm_drop(capture)
            alsa.lib.snd_pcm_close(capture)
        if playback is not None:
            alsa.lib.snd_pcm_drop(playback)
            alsa.lib.snd_pcm_close(playback)

    try:
        manifest["collector_acceptance"] = validate_collection_manifest(
            manifest, rate, period, buffer_size
        )
    except BaseException as error:
        manifest["collection_failure"] = str(error)
        setattr(error, "aius_collection_manifest", manifest)
        raise
    return capture_samples, manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", default="hw:ICUSBAUDIO7D,0")
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--rate", type=int, default=48000)
    parser.add_argument("--period", type=int, default=256)
    parser.add_argument("--buffer", type=int, default=1024)
    parser.add_argument("--status-url")
    parser.add_argument("--status-delay", type=float, default=2.0)
    args = parser.parse_args()

    playback = np.fromfile(args.input, dtype="<i2")
    if len(playback) % 2:
        raise ValueError("input has a partial stereo frame")
    playback = np.ascontiguousarray(playback.reshape(-1, 2))
    capture, manifest = collect(
        args.device,
        playback,
        args.rate,
        args.period,
        args.buffer,
        args.status_url,
        args.status_delay,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    capture.tofile(args.output)
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
