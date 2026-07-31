#!/usr/bin/env python3
"""Generate, collect, and analyze physical PCM or AC-3 latency probes."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import urllib.request

import numpy as np

from linked_probe import collect


RATE = 48000
CHANNELS = 2
CODE_COUNT = 24
CODE_FRAMES = 1024
GUARD_FRAMES = 3584
# FFmpeg's measured AC-3 decode alignment places the first reference code at
# frame 256 within its access unit, leaving the whole 1,024-frame code inside.
WARMUP_FRAMES = 98304
TAIL_FRAMES = RATE // 4
MAX_LAG_FRAMES = RATE // 4
MAX_DSP_CONFIG_BYTES = 12288
SEED = bytes.fromhex(
    "f7cbe4302535d7f633b30578057935e554ae17896ba9c2b4d49f977dd1d8a9ea"
)


def write_json(path: Path, value: object) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def codes() -> list[np.ndarray]:
    result = []
    for index in range(CODE_COUNT):
        digest = hashlib.sha256(
            SEED + index.to_bytes(4, "little")
        ).digest()
        rng = np.random.default_rng(
            int.from_bytes(digest[:8], "little")
        )
        bipolar = rng.choice(
            np.array([-1.0, 1.0]), size=CODE_FRAMES + 2
        )
        shaped = (
            bipolar[:-2] + 2.0 * bipolar[1:-1] + bipolar[2:]
        ) / 4.0
        shaped -= np.mean(shaped)
        shaped /= np.max(np.abs(shaped))
        result.append(np.rint(shaped * 6000.0).astype(np.int16))
    return result


def make_pcm(
    path: Path,
    probe_codes: list[np.ndarray],
    warmup_frames: int = WARMUP_FRAMES,
    guard_frames: int = GUARD_FRAMES,
    tail_frames: int = TAIL_FRAMES,
) -> list[int]:
    total = (
        warmup_frames
        + CODE_COUNT * (CODE_FRAMES + guard_frames)
        + tail_frames
    )
    samples = np.zeros((total, CHANNELS), dtype="<i2")
    positions = []
    position = warmup_frames
    for code in probe_codes:
        positions.append(position)
        samples[position:position + CODE_FRAMES, 0] = code
        samples[position:position + CODE_FRAMES, 1] = code
        position += CODE_FRAMES + guard_frames
    samples.tofile(path)
    return positions


def run_command(argv: list[str], log_path: Path) -> None:
    result = subprocess.run(
        argv,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    log_path.write_bytes(result.stdout)
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(argv)}"
        )


def normalize_effective_config(raw: bytes) -> bytes:
    if not raw or len(raw) > MAX_DSP_CONFIG_BYTES:
        raise RuntimeError("effective DSP config has invalid size")
    model = json.loads(raw)
    if not isinstance(model, dict):
        raise RuntimeError("effective DSP config is not an object")
    generation = model.get("config_generation")
    if (
        not isinstance(generation, int)
        or isinstance(generation, bool)
        or generation < 0
    ):
        raise RuntimeError(
            "effective DSP config has invalid config_generation"
        )
    normalized = dict(model)
    # These are wire/concurrency fields, not signal-processing choices.
    # A successful PUT is SD-backed, and the parser requires a fresh-token
    # base_generation even when used read-only by the native renderer.
    normalized["source"] = "sd"
    normalized["base_generation"] = generation
    return (
        json.dumps(normalized, separators=(",", ":"), sort_keys=True) + "\n"
    ).encode()


def capture_effective_config(url: str, root: Path) -> tuple[Path, Path]:
    with urllib.request.urlopen(url, timeout=5.0) as response:
        raw = response.read(MAX_DSP_CONFIG_BYTES + 1)
    effective_path = root / "dsp-config-effective.json"
    normalized_path = root / "dsp-config-renderer.json"
    effective_path.write_bytes(raw)
    normalized_path.write_bytes(normalize_effective_config(raw))
    return effective_path, normalized_path


def render_native(
    renderer: Path,
    carrier_path: Path,
    config_path: Path,
    root: Path,
) -> tuple[np.ndarray, dict]:
    if not renderer.is_file():
        raise RuntimeError(
            f"native renderer not found: {renderer}; run "
            "scripts/test.sh --environment native --suite latency-runner"
        )
    reference_path = root / "native-reference.s32le"
    render_manifest_path = root / "native-render-manifest.json"
    run_command([
        str(renderer),
        "--carrier", str(carrier_path),
        "--config", str(config_path),
        "--pcm", str(reference_path),
        "--manifest", str(render_manifest_path),
        "--rate", str(RATE),
    ], root / "native-render.log")
    manifest = json.loads(render_manifest_path.read_text(encoding="utf-8"))
    rendered = np.fromfile(reference_path, dtype="<i4")
    if len(rendered) % CHANNELS:
        raise RuntimeError("native reference has a partial stereo frame")
    reference = rendered.reshape(-1, CHANNELS)
    if manifest.get("rendered_frames") != len(reference):
        raise RuntimeError("native reference length disagrees with manifest")
    pipeline = manifest.get("pipeline", {})
    if any(
        pipeline.get(key) != 0
        for key in (
            "parse_rejects",
            "decode_rejects",
            "output_drops",
            "last_failure",
        )
    ):
        raise RuntimeError("native renderer reported a pipeline failure")
    return reference, manifest


def make_ac3(
    root: Path,
    probe_codes: list[np.ndarray],
    warmup_frames: int = WARMUP_FRAMES,
    guard_frames: int = GUARD_FRAMES,
    tail_frames: int = TAIL_FRAMES,
) -> tuple[Path, list[int], list[int]]:
    total = (
        warmup_frames
        + CODE_COUNT * (CODE_FRAMES + guard_frames)
        + tail_frames
    )
    source = np.zeros((total, 6), dtype="<i2")
    source_positions = []
    position = warmup_frames
    for code in probe_codes:
        source_positions.append(position)
        source[position:position + CODE_FRAMES, 0] = code
        source[position:position + CODE_FRAMES, 1] = code
        position += CODE_FRAMES + guard_frames
    source_path = root / "source-5.1.s16le"
    source.tofile(source_path)
    ac3_path = root / "carrier.ac3"
    carrier_path = root / "carrier.s16le"

    run_command([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "s16le", "-ar", str(RATE), "-ac", "6",
        "-channel_layout", "5.1", "-i", str(source_path),
        "-map", "0:a", "-ar", str(RATE), "-c:a", "ac3", "-b:a", "448k",
        "-f", "ac3", str(ac3_path),
    ], root / "ffmpeg-encode.log")
    run_command([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(ac3_path), "-map", "0:a", "-c:a", "copy",
        "-f", "spdif", str(carrier_path),
    ], root / "ffmpeg-wrap.log")

    carrier = np.fromfile(carrier_path, dtype="<u2")
    if len(carrier) % 2:
        raise RuntimeError("IEC carrier has a partial stereo frame")
    carrier = carrier.reshape(-1, 2)
    pa_positions = np.flatnonzero(
        (carrier[:, 0] == 0xF872) & (carrier[:, 1] == 0x4E1F)
    ).astype(int).tolist()
    if len(pa_positions) < 2:
        raise RuntimeError("IEC carrier contains fewer than two AC-3 bursts")
    periods = np.diff(pa_positions)
    if np.any(periods != 1536):
        raise RuntimeError(
            f"unexpected AC-3 IEC periods: {np.unique(periods).tolist()}"
        )

    return carrier_path, source_positions, pa_positions


def correlation_peak(
    signal: np.ndarray,
    template: np.ndarray,
    low: int,
    high: int,
) -> tuple[int, float]:
    template64 = template.astype(np.float64)
    template64 -= np.mean(template64)
    template_energy = np.dot(template64, template64)
    if template_energy == 0:
        raise RuntimeError("zero-energy correlation template")

    low = max(0, low)
    high = min(high, len(signal) - len(template))
    if high < low:
        raise RuntimeError("empty correlation search interval")
    segment = signal[low:high + len(template)].astype(np.float64)
    fft_size = 1 << (
        len(segment) + len(template64) - 1
    ).bit_length()
    products = np.fft.rfft(segment, fft_size) * np.fft.rfft(
        template64[::-1], fft_size
    )
    convolution = np.fft.irfft(products, fft_size)
    valid_count = high - low + 1
    dots = convolution[
        len(template64) - 1:len(template64) - 1 + valid_count
    ]

    cumulative = np.concatenate((
        np.array([0.0]),
        np.cumsum(segment),
    ))
    cumulative_sq = np.concatenate((
        np.array([0.0]),
        np.cumsum(segment * segment),
    ))
    width = len(template64)
    sums = cumulative[width:] - cumulative[:-width]
    sums_sq = cumulative_sq[width:] - cumulative_sq[:-width]
    variance = np.maximum(sums_sq - sums * sums / width, 0.0)
    normalized = np.zeros_like(dots)
    energetic = variance >= float(width)
    normalized[energetic] = dots[energetic] / np.sqrt(
        template_energy * variance[energetic]
    )
    index = int(np.argmax(normalized))
    return low + index, float(normalized[index])


def locate_reference_codes(
    decoded: np.ndarray,
    probe_codes: list[np.ndarray],
    expected_positions: list[int],
) -> list[int]:
    left = decoded[:, 0].astype(np.float64)
    positions = []
    for code, expected in zip(probe_codes, expected_positions):
        position, score = correlation_peak(
            left, code, expected - 4096, expected + 4096
        )
        if score < 0.20:
            raise RuntimeError(
                f"decoded AC-3 code correlation too weak: {score:.3f}"
            )
        positions.append(position)
    return positions


def map_encoded_input_positions(
    reference_positions: list[int],
    render_manifest: dict,
) -> tuple[list[int], list[int]]:
    units = render_manifest.get("units")
    if not isinstance(units, list) or not units:
        raise RuntimeError("native render manifest has no encoded units")
    input_positions = []
    unit_indices = []
    for reference_position in reference_positions:
        matches = [
            unit for unit in units
            if (
                unit["post_frame_zero"] <= reference_position
                and reference_position + CODE_FRAMES - 1
                <= unit["post_frame_last"]
            )
        ]
        if len(matches) != 1:
            raise RuntimeError(
                "reference code does not belong to exactly one decoded unit"
            )
        unit = matches[0]
        pa_word_index = unit["pa_index"]
        if pa_word_index % CHANNELS:
            raise RuntimeError("encoded Pa is not stereo-frame aligned")
        frame_in_unit = reference_position - unit["post_frame_zero"]
        input_positions.append(
            pa_word_index // CHANNELS + frame_in_unit
        )
        unit_indices.append(unit["index"])
    if any(
        right <= left for left, right in zip(
            input_positions, input_positions[1:]
        )
    ):
        raise RuntimeError("encoded input associations are not monotonic")
    return input_positions, unit_indices


def analyze(
    mode: str,
    capture: np.ndarray,
    reference: np.ndarray,
    reference_positions: list[int],
    input_positions: list[int],
) -> dict:
    if np.any((capture == -32768) | (capture == 32767)):
        raise RuntimeError("capture contains clipped S16 samples")
    capture_mono = np.mean(capture.astype(np.float64), axis=1)
    reference_mono = np.mean(reference.astype(np.float64), axis=1)
    occurrences = []
    for index, (reference_position, input_position) in enumerate(zip(
        reference_positions, input_positions
    )):
        template = reference_mono[
            reference_position:reference_position + CODE_FRAMES
        ]
        capture_position, score = correlation_peak(
            capture_mono,
            template,
            input_position,
            input_position + MAX_LAG_FRAMES,
        )
        channel_positions = []
        channel_correlations = []
        for channel in range(CHANNELS):
            channel_position, channel_score = correlation_peak(
                capture[:, channel],
                reference[
                    reference_position:reference_position + CODE_FRAMES,
                    channel,
                ],
                input_position,
                input_position + MAX_LAG_FRAMES,
            )
            channel_positions.append(channel_position)
            channel_correlations.append(channel_score)
        occurrences.append({
            "index": index,
            "input_frame": input_position,
            "reference_frame": reference_position,
            "capture_frame": capture_position,
            "latency_frames": capture_position - input_position,
            "latency_ms": (
                (capture_position - input_position) * 1000.0 / RATE
            ),
            "correlation": score,
            "channel_capture_frames": channel_positions,
            "channel_latency_frames": [
                position - input_position for position in channel_positions
            ],
            "channel_correlations": channel_correlations,
            "channel_skew_frames": (
                channel_positions[1] - channel_positions[0]
            ),
        })

    inputs = np.array(
        [item["input_frame"] for item in occurrences], dtype=np.float64
    )
    captures = np.array(
        [item["capture_frame"] for item in occurrences], dtype=np.float64
    )
    ratio, offset = np.polyfit(inputs, captures, 1)
    residuals = captures - (offset + ratio * inputs)
    latencies = np.array(
        [item["latency_frames"] for item in occurrences], dtype=np.float64
    )
    channel_latencies = [
        np.array([
            item["channel_latency_frames"][channel]
            for item in occurrences
        ], dtype=np.float64)
        for channel in range(CHANNELS)
    ]
    channel_skews = np.array([
        item["channel_skew_frames"] for item in occurrences
    ], dtype=np.float64)
    accepted_occurrences = sum(
        item["correlation"] >= 0.35 for item in occurrences
    )
    if accepted_occurrences < 20:
        raise RuntimeError(
            f"only {accepted_occurrences}/24 codes meet correlation"
        )
    channel_accepted = [
        sum(
            item["channel_correlations"][channel] >= 0.35
            for item in occurrences
        )
        for channel in range(CHANNELS)
    ]
    if any(count < 20 for count in channel_accepted):
        raise RuntimeError(
            "fewer than 20/24 codes meet per-channel correlation: "
            f"{channel_accepted}"
        )
    if np.any(np.diff(captures) <= 0):
        raise RuntimeError("capture associations are not monotonic")
    if not 0.9995 <= ratio <= 1.0005:
        raise RuntimeError(f"affine ratio is out of bounds: {ratio}")
    residual_max = float(np.max(np.abs(residuals)))
    if residual_max > 128:
        raise RuntimeError(
            f"affine residual exceeds 128 frames: {residual_max}"
        )
    summary = {
        "mode": mode,
        "unequal_stereo_frames": int(np.count_nonzero(
            capture[:, 0] != capture[:, 1]
        )),
        "unequal_stereo_fraction": float(np.mean(
            capture[:, 0] != capture[:, 1]
        )),
        "occurrences": occurrences,
        "accepted_occurrences": accepted_occurrences,
        "channel_accepted_occurrences": channel_accepted,
        "correlation_min": min(
            item["correlation"] for item in occurrences
        ),
        "channel_correlation_min": min(
            min(item["channel_correlations"]) for item in occurrences
        ),
        "affine": {
            "capture_per_playback_ratio": ratio,
            "offset_frames": offset,
            "offset_ms": offset * 1000.0 / RATE,
            "residual_max_abs_frames": residual_max,
        },
        "raw_latency": {
            "min_frames": float(np.min(latencies)),
            "median_frames": float(np.median(latencies)),
            "p95_frames": float(np.percentile(latencies, 95)),
            "max_frames": float(np.max(latencies)),
            "min_ms": float(np.min(latencies) * 1000.0 / RATE),
            "median_ms": float(np.median(latencies) * 1000.0 / RATE),
            "p95_ms": float(np.percentile(latencies, 95) * 1000.0 / RATE),
            "max_ms": float(np.max(latencies) * 1000.0 / RATE),
        },
        "channel_latency": [{
            "channel": channel,
            "min_frames": float(np.min(values)),
            "median_frames": float(np.median(values)),
            "p95_frames": float(np.percentile(values, 95)),
            "max_frames": float(np.max(values)),
        } for channel, values in enumerate(channel_latencies)],
        "channel_skew": {
            "min_frames": float(np.min(channel_skews)),
            "median_frames": float(np.median(channel_skews)),
            "p95_abs_frames": float(
                np.percentile(np.abs(channel_skews), 95)
            ),
            "max_frames": float(np.max(channel_skews)),
        },
    }
    if mode == "ac3":
        summary["access_unit_tail"] = {
            key.replace("latency", "tail"): value + (
                1535 if key.endswith("_frames") else 1535 * 1000.0 / RATE
            )
            for key, value in summary["raw_latency"].items()
        }
    return summary


def counter_deltas(initial: dict, final: dict) -> dict:
    paths = [
        ("overruns",),
        ("event_overflows",),
        ("audio_diag", "ingress_dropped_chunks"),
        ("audio_diag", "ingress_dropped_words"),
        ("audio_diag", "source_changes"),
        ("audio_diag", "source_flushes"),
        ("audio_diag", "gap_flushes"),
        ("audio_diag", "service_generation_resets"),
        ("audio_diag", "service_requested_resets"),
        ("audio_diag", "service_budget_overruns"),
        ("audio_diag", "runtime_epoch_budget_overruns"),
        ("audio_diag", "transmit_stops"),
        ("audio_diag", "transmit_stalls"),
        ("audio_diag", "parse_rejects"),
        ("audio_diag", "decode_rejects"),
        ("audio_diag", "output_drops"),
        ("audio_diag", "au_abandons"),
        ("clock", "tx_underrun_frames"),
        ("clock", "rate_changes"),
        ("clock", "structural_replans"),
        ("clock", "trim_updates"),
        ("clock", "invalid_windows"),
        ("clock", "clock_faults"),
        ("clock", "mismatch_blocks"),
        ("clock", "unsupported_blocks"),
        ("clock", "unsupported_episodes"),
        ("pipeline_diag", "decode_rejects"),
        ("pipeline_diag", "rejects_no_space"),
        ("pipeline_diag", "rejects_malformed"),
        ("pipeline_diag", "rejects_unsupported"),
        ("pipeline_diag", "resyncs"),
        ("pipeline_diag", "alloc_count"),
        ("pipeline_diag", "dts_alloc_count"),
        ("pipeline_diag", "ac3_alloc_count"),
    ]
    result = {}
    for path in paths:
        before: object = initial
        after: object = final
        for part in path:
            before = before[part]  # type: ignore[index]
            after = after[part]  # type: ignore[index]
        result[".".join(path)] = (int(after) - int(before)) & 0xFFFFFFFF
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("pcm", "ac3"), required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--device", default="hw:ICUSBAUDIO7D,0")
    parser.add_argument(
        "--status-url", default="http://192.0.2.66/api/status"
    )
    parser.add_argument(
        "--config-url", default="http://192.0.2.66/api/dsp/config"
    )
    parser.add_argument(
        "--native-renderer",
        type=Path,
        default=Path(os.environ.get(
            "AIUS_LATENCY_RENDERER",
            Path(__file__).resolve().parents[2]
            / ".build/native/latency-native-renderer",
        )),
    )
    parser.add_argument("--period", type=int, default=256)
    parser.add_argument("--buffer", type=int, default=4096)
    parser.add_argument("--warmup-frames", type=int, default=WARMUP_FRAMES)
    parser.add_argument("--guard-frames", type=int, default=GUARD_FRAMES)
    parser.add_argument("--tail-frames", type=int, default=TAIL_FRAMES)
    parser.add_argument("--status-delay", type=float, default=1.5)
    parser.add_argument(
        "--reuse-existing",
        action="store_true",
        help="reanalyze an existing carrier, capture, and manifest",
    )
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.reuse_existing:
        manifest_path = args.output / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        previous_summary = json.loads(
            (args.output / "summary.json").read_text(encoding="utf-8")
        )
        if previous_summary["mode"] != args.mode:
            raise RuntimeError(
                "requested mode does not match the existing summary"
            )
        for artifact in manifest["artifacts"].values():
            artifact_path = args.output / artifact["path"]
            if sha256(artifact_path) != artifact["sha256"]:
                raise RuntimeError(
                    f"artifact hash mismatch: {artifact['path']}"
                )
        capture = np.fromfile(
            args.output / manifest["artifacts"]["capture"]["path"],
            dtype="<i2",
        ).reshape(-1, CHANNELS)
        generation = manifest["generation"]
        reference_positions = generation.get(
            "reference_positions", generation.get("input_positions")
        )
        if args.mode == "pcm":
            reference_artifact = manifest["artifacts"].get(
                "native_reference",
                manifest["artifacts"]["carrier"],
            )
            reference_dtype = (
                "<i4" if "native_reference" in manifest["artifacts"]
                else "<i2"
            )
            reference = np.fromfile(
                args.output / reference_artifact["path"],
                dtype=reference_dtype,
            ).reshape(-1, CHANNELS)
            input_positions = generation["input_positions"]
        else:
            if "native_reference" in manifest["artifacts"]:
                reference_path = (
                    args.output
                    / manifest["artifacts"]["native_reference"]["path"]
                )
                reference = np.fromfile(
                    reference_path, dtype="<i4"
                ).reshape(-1, CHANNELS)
                input_positions = generation["input_positions"]
            else:
                decoded_path = args.output / "decoded-5.1.s32le"
                reference = np.fromfile(
                    decoded_path, dtype="<i4"
                ).reshape(-1, 6)[:, :2]
                input_positions = []
                for reference_position in reference_positions:
                    access_unit = reference_position // 1536
                    input_positions.append(
                        generation["pa_positions"][access_unit]
                        + reference_position % 1536
                    )
        summary = analyze(
            args.mode,
            capture,
            reference,
            reference_positions,
            input_positions,
        )
        manifest["counter_deltas"] = counter_deltas(
            manifest["status_initial"], manifest["status_final"]
        )
        write_json(manifest_path, manifest)
        summary["build_revision"] = manifest["status_final"][
            "build_revision"
        ]
        summary["counter_deltas"] = manifest["counter_deltas"]
        summary["reused_existing_artifacts"] = True
        write_json(args.output / "summary.json", summary)
        print(json.dumps(summary, indent=2, sort_keys=True), flush=True)
        return 0
    if args.warmup_frames < RATE:
        raise ValueError("warm-up must be at least one second")
    if args.guard_frames < 0 or args.tail_frames < 0:
        raise ValueError("guard and tail frames must be non-negative")
    if args.status_delay >= args.warmup_frames / RATE:
        raise ValueError("status bracket must begin before the first code")

    effective_config_path, renderer_config_path = capture_effective_config(
        args.config_url, args.output
    )
    probe_codes = codes()
    if args.mode == "pcm":
        carrier_path = args.output / "carrier.s16le"
        source_positions = make_pcm(
            carrier_path,
            probe_codes,
            args.warmup_frames,
            args.guard_frames,
            args.tail_frames,
        )
        reference, render_manifest = render_native(
            args.native_renderer,
            carrier_path,
            renderer_config_path,
            args.output,
        )
        if (
            render_manifest["pipeline"]["class"] != 1
            or render_manifest["units"]
        ):
            raise RuntimeError("native renderer did not classify PCM")
        reference_positions = locate_reference_codes(
            reference, probe_codes, source_positions
        )
        input_positions = source_positions
        generation = {
            "seed_sha256": hashlib.sha256(SEED).hexdigest(),
            "code_count": CODE_COUNT,
            "code_frames": CODE_FRAMES,
            "guard_frames": args.guard_frames,
            "warmup_frames": args.warmup_frames,
            "tail_frames": args.tail_frames,
            "input_positions": input_positions,
            "reference_positions": reference_positions,
        }
    else:
        (
            carrier_path,
            source_positions,
            pa_positions,
        ) = make_ac3(
            args.output,
            probe_codes,
            args.warmup_frames,
            args.guard_frames,
            args.tail_frames,
        )
        reference, render_manifest = render_native(
            args.native_renderer,
            carrier_path,
            renderer_config_path,
            args.output,
        )
        if (
            render_manifest["pipeline"]["class"] != 2
            or render_manifest["pipeline"]["codec_pc"] != 1
        ):
            raise RuntimeError("native renderer did not classify AC-3 Pc=1")
        reference_positions = locate_reference_codes(
            reference, probe_codes, source_positions
        )
        input_positions, unit_indices = map_encoded_input_positions(
            reference_positions, render_manifest
        )
        native_pa_positions = [
            unit["pa_index"] // CHANNELS
            for unit in render_manifest["units"]
        ]
        if native_pa_positions != pa_positions:
            raise RuntimeError(
                "native IEC unit positions disagree with generated carrier"
            )
        generation = {
            "seed_sha256": hashlib.sha256(SEED).hexdigest(),
            "code_count": CODE_COUNT,
            "code_frames": CODE_FRAMES,
            "guard_frames": args.guard_frames,
            "warmup_frames": args.warmup_frames,
            "tail_frames": args.tail_frames,
            "source_positions": source_positions,
            "reference_positions": reference_positions,
            "input_positions": input_positions,
            "reference_unit_indices": unit_indices,
            "pa_positions": pa_positions,
        }

    playback = np.fromfile(carrier_path, dtype="<i2")
    if len(playback) % 2:
        raise RuntimeError("carrier has a partial stereo frame")
    playback = np.ascontiguousarray(playback.reshape(-1, 2))
    capture, manifest = collect(
        args.device,
        playback,
        RATE,
        args.period,
        args.buffer,
        args.status_url,
        args.status_delay,
    )
    capture_path = args.output / "capture.s16le"
    capture.tofile(capture_path)
    manifest["artifacts"] = {
        "carrier": {
            "path": carrier_path.name,
            "sha256": sha256(carrier_path),
        },
        "capture": {
            "path": capture_path.name,
            "sha256": sha256(capture_path),
        },
        "effective_dsp_config": {
            "path": effective_config_path.name,
            "sha256": sha256(effective_config_path),
        },
        "renderer_dsp_config": {
            "path": renderer_config_path.name,
            "sha256": sha256(renderer_config_path),
        },
        "native_reference": {
            "path": "native-reference.s32le",
            "sha256": sha256(args.output / "native-reference.s32le"),
        },
        "native_render_manifest": {
            "path": "native-render-manifest.json",
            "sha256": sha256(args.output / "native-render-manifest.json"),
        },
        "native_render_log": {
            "path": "native-render.log",
            "sha256": sha256(args.output / "native-render.log"),
        },
    }
    if args.mode == "ac3":
        for key, artifact_path in (
            ("source", args.output / "source-5.1.s16le"),
            ("elementary_stream", args.output / "carrier.ac3"),
        ):
            manifest["artifacts"][key] = {
                "path": artifact_path.name,
                "sha256": sha256(artifact_path),
            }
    manifest["generation"] = generation
    if "status_initial" in manifest and "status_final" in manifest:
        manifest["counter_deltas"] = counter_deltas(
            manifest["status_initial"], manifest["status_final"]
        )
    write_json(args.output / "manifest.json", manifest)

    summary = analyze(
        args.mode,
        capture,
        reference,
        reference_positions,
        input_positions,
    )
    summary["build_revision"] = manifest["status_final"][
        "build_revision"
    ]
    summary["counter_deltas"] = manifest.get("counter_deltas")
    write_json(args.output / "summary.json", summary)
    print(json.dumps(summary, indent=2, sort_keys=True), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
