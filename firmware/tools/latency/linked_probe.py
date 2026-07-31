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
MAX_LINKED_TRIGGER_SKEW_FRAMES = 1.0


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
    ) -> dict:
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

        if (
            actual_rate.value != rate
            or actual_period.value != period
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
