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


class AlsaError(RuntimeError):
    """A libasound call failed."""


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
        }


def get_json(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=5.0) as response:
        return json.load(response)


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

    alsa = Alsa()
    playback = alsa.open(device, SND_PCM_STREAM_PLAYBACK)
    capture = alsa.open(device, SND_PCM_STREAM_CAPTURE)
    linked = False
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
    }
    try:
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

        delay = ctypes.c_long()
        if alsa.lib.snd_pcm_delay(playback, ctypes.byref(delay)) >= 0:
            manifest["playback_delay_at_end_frames"] = delay.value
        if alsa.lib.snd_pcm_delay(capture, ctypes.byref(delay)) >= 0:
            manifest["capture_delay_at_end_frames"] = delay.value
    finally:
        if linked:
            alsa.lib.snd_pcm_unlink(capture)
        alsa.lib.snd_pcm_drop(capture)
        alsa.lib.snd_pcm_drop(playback)
        alsa.lib.snd_pcm_close(capture)
        alsa.lib.snd_pcm_close(playback)

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
