#!/usr/bin/env python3
"""Generate, collect, and analyze physical PCM or AC-3 latency probes."""

from __future__ import annotations

import argparse
import contextlib
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
import platform
from pathlib import Path
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
import wave

import numpy as np

from linked_probe import (
    ACTIVE_SOURCE_ACCEPTED_MODES,
    MAX_LINKED_TRIGGER_SKEW_FRAMES,
    MAX_PREFLIGHT_XRUN_RECOVERIES,
    SETTLED_WINDOW_SECONDS,
    TRIM_RATE_LIMIT_SECONDS,
    collect,
    preflight_active_source,
)


RATE = 48000
CHANNELS = 2
CODE_COUNT = 24
SYNC_FRAMES = 127
PCM_CODE_FRAMES = 2048
AC3_CODE_FRAMES = 1024
PCM_GUARD_FRAMES = 4096
AC3_GUARD_FRAMES = 3072
# Stimulus spacing exceeds the analyzed quiet guard so the allowed affine skew
# and residual cannot move a neighboring code into the guard window. AC-3
# spacing remains access-unit aligned: 1,024 active + 5,120 gap = four AUs.
PCM_PROBE_GAP_FRAMES = PCM_GUARD_FRAMES + 256
AC3_PROBE_GAP_FRAMES = 5120
# FFmpeg's measured AC-3 decode alignment places the first reference code at
# frame 256 within its access unit, leaving the whole 1,024-frame code inside.
WARMUP_FRAMES = 98304
AC3_IEC_PERIOD_FRAMES = 1536
ACTIVE_SOURCE_PREFLIGHT_MIN_SECONDS = 3.0
# A post-reboot device needed five to seven rate-limited trims to converge
# (docs/plans/aius-e2e-latency.md), so the budget covers seven trim gaps plus
# the minimum hold before acceptance is even considered.
ACTIVE_SOURCE_PREFLIGHT_TIMEOUT_SECONDS = 75.0
ACTIVE_SOURCE_PREFLIGHT_POLL_SECONDS = 0.25
MAXIMUM_FOLLOW_ERROR_MILLIHZ = 1000
FORMAL_WARMUP_FRAMES = WARMUP_FRAMES
FORMAL_STATUS_DELAY_SECONDS = 1.5
# Formal-bracket allowances, each measured against the code path that spends it.
# `linked_probe.collect` opens, configures, prepares, links and prefills both
# streams before the trigger, after the preflight stream is dropped and its
# artifact hashed; one GET /api/status is spent before the trigger and one
# after the last frame drains. The 5 s urlopen timeout in `get_json` bounds a
# failure, not a healthy LAN fetch, so it is not the budget here.
FORMAL_ALSA_SETUP_ALLOWANCE_SECONDS = 0.5
FORMAL_STATUS_FETCH_ALLOWANCE_SECONDS = 0.5
# The bypass row brackets collection with one GET /api/dsp/config at each edge,
# so its effective-config hash gate is spent inside the same trim-limit budget.
FORMAL_CONFIG_FETCH_ALLOWANCE_SECONDS = 0.5
# The device samples its clock counters asynchronously from the HTTP response,
# so an accepted edge may already be this stale when the preflight sees it.
STATUS_STALENESS_ALLOWANCE_SECONDS = 0.5
# Version 3 adds the DSP row mode (`production` or `bypass`) to the contract.
# Version 2 evidence is production evidence by construction, so it stays
# reusable under --allow-legacy-evidence; no bypass evidence can predate 3.
ACCEPTANCE_CONTRACT_VERSION = 3
DSP_MODE_PRODUCTION = "production"
DSP_MODE_BYPASS = "bypass"
DSP_MODES = (DSP_MODE_PRODUCTION, DSP_MODE_BYPASS)
# The four documented bypass switches the production model can express. The
# fifth, "dither-affecting width changes", is NOT a model field: the transmit
# word width is pinned at compile time (`optical_main.cpp` OUTPUT_WIDTH_BITS =
# AIUS_DSP_OUT_WIDTH_24, a plain copy that never runs the TPDF dither stage),
# and the wire model has no width or dither key at all. The runner therefore
# asserts that invariant instead of flipping a switch it cannot express.
BYPASS_FEATURE_SWITCHES = ("input_eq", "matrix", "output_eq", "limiter")
DSP_OUTPUT_WIDTH_BITS = 24
# `meters` is deliberately left at its production value: it publishes levels
# and changes no sample, so disabling it would move the row away from the
# production signal path for no measured reason.
SD_AUTH_HEADER = "X-Aius-SD-Auth"
SD_DSP_CONFIG_URL = "http://192.0.2.66/sd/aius/dsp.json"
HTTP_TIMEOUT_SECONDS = 5.0
# The device applies a staged model at the next audio block boundary, then
# republishes it through GET /api/dsp/config; the poll is the settle observer.
DSP_APPLY_POLL_SECONDS = 0.25
DSP_APPLY_TIMEOUT_SECONDS = 15.0
# PUT refusals the device documents as happening before the SD job and the
# staged-model swap: auth, body validation and the optimistic-concurrency
# token. Any other failure leaves the board's state unproven.
NON_MUTATING_PUT_STATUSES = (400, 401, 409)
BYPASS_CONFIG_PREFIX = "dsp-config-bypass"
HARNESS_PATHS = (
    "firmware/tools/latency",
    "firmware/test/latency-runner",
)
TAIL_FRAMES = RATE // 4
MAX_LAG_FRAMES = RATE // 4
MAX_DSP_CONFIG_BYTES = 12288
MIN_CORRELATION = 0.35
MIN_PEAK_RATIO = 2.0
MAIN_LOBE_FRAMES = 64
MIN_SIGNAL_TO_GUARD_DB = 30.0
MIN_ACCEPTED_CODES = 20
MAX_AFFINE_RESIDUAL_FRAMES = 128.0
MIN_CLOCK_RATIO = 0.9995
MAX_CLOCK_RATIO = 1.0005
SAMPLE_RATE_EVIDENCE_REVISION = (
    "01477755955b509eb0f55b10282156f6a0a771cf"
)
PHASE1_IMAGE_REVISION = (
    "45153bb5ad43a0d110f5f694e92014b266e7dea8"
)
ZERO_DELTA_PATHS = (
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
)
PROGRESS_PATHS = (
    ("audio_diag", "input_words_consumed"),
    ("audio_diag", "service_steps_committed"),
    ("audio_diag", "committed_output_blocks"),
)
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


def artifact_record(path: Path) -> dict:
    return {"path": path.name, "sha256": sha256(path)}


def mode_geometry(mode: str) -> tuple[int, int]:
    if mode == "pcm":
        return PCM_CODE_FRAMES, PCM_GUARD_FRAMES
    if mode == "ac3":
        return AC3_CODE_FRAMES, AC3_GUARD_FRAMES
    raise ValueError(f"unsupported latency mode: {mode}")


def mode_probe_gap(mode: str) -> int:
    if mode == "pcm":
        return PCM_PROBE_GAP_FRAMES
    if mode == "ac3":
        return AC3_PROBE_GAP_FRAMES
    raise ValueError(f"unsupported latency mode: {mode}")


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def active_source_contract() -> dict:
    return {
        "minimum_seconds": ACTIVE_SOURCE_PREFLIGHT_MIN_SECONDS,
        "timeout_seconds": ACTIVE_SOURCE_PREFLIGHT_TIMEOUT_SECONDS,
        "poll_seconds": ACTIVE_SOURCE_PREFLIGHT_POLL_SECONDS,
        "settled_window_seconds": SETTLED_WINDOW_SECONDS,
        "trim_rate_limit_seconds": TRIM_RATE_LIMIT_SECONDS,
        "maximum_follow_error_millihz": MAXIMUM_FOLLOW_ERROR_MILLIHZ,
        "maximum_xrun_recoveries": MAX_PREFLIGHT_XRUN_RECOVERIES,
        "accepted_modes": list(ACTIVE_SOURCE_ACCEPTED_MODES),
    }


def bypass_contract() -> dict:
    """The gates the PCM-bypass row adds to the PCM production gates."""
    return {
        "feature_switches": list(BYPASS_FEATURE_SWITCHES),
        "output_width_bits": DSP_OUTPUT_WIDTH_BITS,
        "output_width_expressible_in_model": False,
        "effective_config_hash_bracket": ["initial", "final"],
        "card_file_semantic_match_required": True,
        "active_sd_override_required": True,
        "restoration_verified_required": True,
    }


def validate_dsp_mode(dsp: str) -> str:
    if dsp not in DSP_MODES:
        raise ValueError(f"unsupported dsp mode: {dsp}")
    return dsp


def acceptance_contract(
    mode: str, dsp: str = DSP_MODE_PRODUCTION
) -> dict:
    code_frames, guard_frames = mode_geometry(mode)
    validate_dsp_mode(dsp)
    contract = {
        "contract_version": ACCEPTANCE_CONTRACT_VERSION,
        "dsp": dsp,
        "active_source_preflight": active_source_contract(),
        "expected_codes": CODE_COUNT,
        "minimum_accepted_codes": MIN_ACCEPTED_CODES,
        "code_frames": code_frames,
        "sync_prefix_frames": SYNC_FRAMES,
        "guard_frames_each_side": guard_frames,
        "minimum_correlation": MIN_CORRELATION,
        "main_lobe_exclusion_frames": MAIN_LOBE_FRAMES,
        "minimum_peak_ratio": MIN_PEAK_RATIO,
        "minimum_signal_to_guard_db": MIN_SIGNAL_TO_GUARD_DB,
        "guard_rms_floor_s16_lsb": 1.0,
        "minimum_clock_ratio": MIN_CLOCK_RATIO,
        "maximum_clock_ratio": MAX_CLOCK_RATIO,
        "maximum_affine_residual_frames": MAX_AFFINE_RESIDUAL_FRAMES,
        "maximum_linked_trigger_skew_frames": (
            MAX_LINKED_TRIGGER_SKEW_FRAMES
        ),
        "maximum_lag_frames": MAX_LAG_FRAMES,
        "clipping_values_s16": [-32768, 32767],
    }
    if dsp == DSP_MODE_BYPASS:
        contract["bypass"] = bypass_contract()
    return contract


def stimulus_frames(
    mode: str,
    warmup_frames: int,
    probe_gap_frames: int,
    tail_frames: int,
) -> int:
    """Frames the device plays for one formal collection."""
    code_frames, _ = mode_geometry(mode)
    frames = (
        warmup_frames
        + CODE_COUNT * (code_frames + probe_gap_frames)
        + tail_frames
    )
    if mode == "ac3":
        # The IEC carrier the device actually plays is padded out to whole
        # 1,536-frame access units, so round up rather than under-count.
        periods = -(-frames // AC3_IEC_PERIOD_FRAMES)
        frames = periods * AC3_IEC_PERIOD_FRAMES
    return frames


def formal_bracket_projection(
    mode: str,
    warmup_frames: int,
    probe_gap_frames: int,
    tail_frames: int,
    dsp: str = DSP_MODE_PRODUCTION,
) -> dict:
    """Worst-case wall clock from the accepted edge to the final status read.

    Every term is a named allowance over an actual code path, so a CLI knob that
    pushes the bracket across a rate-limited trim is visible before the run
    touches the device.
    """
    validate_dsp_mode(dsp)
    frames = stimulus_frames(
        mode, warmup_frames, probe_gap_frames, tail_frames
    )
    playback_seconds = frames / RATE
    config_fetches = 2 if dsp == DSP_MODE_BYPASS else 0
    bracket_seconds = (
        FORMAL_ALSA_SETUP_ALLOWANCE_SECONDS
        + FORMAL_STATUS_FETCH_ALLOWANCE_SECONDS
        + playback_seconds
        + FORMAL_STATUS_FETCH_ALLOWANCE_SECONDS
        + config_fetches * FORMAL_CONFIG_FETCH_ALLOWANCE_SECONDS
    )
    budget_seconds = (
        TRIM_RATE_LIMIT_SECONDS
        - ACTIVE_SOURCE_PREFLIGHT_POLL_SECONDS
        - STATUS_STALENESS_ALLOWANCE_SECONDS
    )
    projection = {
        "stimulus_frames": frames,
        "playback_seconds": playback_seconds,
        "setup_allowance_seconds": FORMAL_ALSA_SETUP_ALLOWANCE_SECONDS,
        "status_fetch_allowance_seconds": (
            FORMAL_STATUS_FETCH_ALLOWANCE_SECONDS
        ),
        "status_fetches": 2,
        "poll_allowance_seconds": ACTIVE_SOURCE_PREFLIGHT_POLL_SECONDS,
        "status_staleness_allowance_seconds": (
            STATUS_STALENESS_ALLOWANCE_SECONDS
        ),
        "trim_rate_limit_seconds": TRIM_RATE_LIMIT_SECONDS,
        "bracket_seconds": bracket_seconds,
        "budget_seconds": budget_seconds,
        "fits": bracket_seconds <= budget_seconds,
    }
    if config_fetches:
        projection["config_fetches"] = config_fetches
        projection["config_fetch_allowance_seconds"] = (
            FORMAL_CONFIG_FETCH_ALLOWANCE_SECONDS
        )
    return projection


def validate_formal_bracket(
    mode: str,
    warmup_frames: int,
    probe_gap_frames: int,
    tail_frames: int,
    dsp: str = DSP_MODE_PRODUCTION,
) -> dict:
    projection = formal_bracket_projection(
        mode, warmup_frames, probe_gap_frames, tail_frames, dsp
    )
    if not projection["fits"]:
        raise ValueError(
            "formal collection bracket of "
            f"{projection['bracket_seconds']:.3f} s exceeds the "
            f"{projection['budget_seconds']:.3f} s that fit inside the "
            f"{TRIM_RATE_LIMIT_SECONDS:.1f} s device trim rate limit "
            f"({projection['stimulus_frames']} stimulus frames); reduce "
            "--warmup-frames or --tail-frames"
        )
    return projection


def tool_versions(mode: str) -> dict:
    versions = {
        "python": platform.python_version(),
        "numpy": np.__version__,
    }
    if mode == "ac3":
        completed = subprocess.run(
            ["ffmpeg", "-version"],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if completed.returncode != 0:
            raise RuntimeError("ffmpeg -version failed")
        versions["ffmpeg"] = completed.stdout.splitlines()[0]
    return versions


def repository_identity() -> dict:
    root = Path(__file__).resolve().parents[6]
    head = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=root,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if head.returncode != 0:
        raise RuntimeError("cannot resolve latency harness revision")
    revision = head.stdout.strip()
    phase1_ancestor = subprocess.run(
        [
            "git",
            "merge-base",
            "--is-ancestor",
            PHASE1_IMAGE_REVISION,
            revision,
        ],
        cwd=root,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    phase1_is_ancestor = phase1_ancestor.returncode == 0
    if not phase1_is_ancestor:
        raise RuntimeError(
            "accepted Phase 1 image is not an ancestor"
        )
    evidence_ancestor = subprocess.run(
        [
            "git",
            "merge-base",
            "--is-ancestor",
            SAMPLE_RATE_EVIDENCE_REVISION,
            PHASE1_IMAGE_REVISION,
        ],
        cwd=root,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    evidence_is_ancestor = evidence_ancestor.returncode == 0
    if not evidence_is_ancestor:
        raise RuntimeError(
            "accepted sample-rate evidence is not an image ancestor"
        )
    dirty = subprocess.run(
        ["git", "status", "--porcelain", "--", *HARNESS_PATHS],
        cwd=root,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if dirty.returncode != 0:
        raise RuntimeError("cannot inspect the latency harness working tree")
    dirty_files = [
        line[3:].strip()
        for line in dirty.stdout.splitlines()
        if line.strip()
    ]
    return {
        "harness_revision": revision,
        "expected_build_revision": (
            f"{PHASE1_IMAGE_REVISION[:12]}+teensy41-optical"
        ),
        "phase1_image_revision": PHASE1_IMAGE_REVISION,
        "phase1_image_is_ancestor": phase1_is_ancestor,
        "sample_rate_evidence_revision": (
            SAMPLE_RATE_EVIDENCE_REVISION
        ),
        "sample_rate_evidence_is_ancestor": evidence_is_ancestor,
        "harness_paths": list(HARNESS_PATHS),
        "harness_dirty": bool(dirty_files),
        "harness_dirty_files": dirty_files,
    }


def require_clean_harness(repository: dict) -> None:
    """Refuse formal collection from an uncommitted harness.

    The manifest claims a harness revision; an edited working tree makes that
    claim false, and the evidence is unreproducible from the recorded revision.
    """
    if not repository.get("harness_dirty"):
        return
    raise RuntimeError(
        "refusing formal collection: the latency harness working tree is "
        "dirty at "
        + ", ".join(repository.get("harness_dirty_files", []))
        + "; commit or stash it, or reanalyze with --reuse-existing"
    )


def make_preflight(
    *,
    mode: str,
    device: str,
    status_url: str,
    config_url: str,
    renderer: Path,
    period: int,
    buffer_size: int,
    warmup_frames: int,
    status_delay_seconds: float,
    probe_gap_frames: int,
    tail_frames: int,
    command_argv: list[str],
    versions: dict,
    repository: dict,
    started_utc: str,
    dsp: str = DSP_MODE_PRODUCTION,
    sd_config_url: str | None = None,
) -> dict:
    validate_dsp_mode(dsp)
    endpoints = {
        "status": status_url,
        "effective_dsp_config": config_url,
    }
    if dsp == DSP_MODE_BYPASS:
        endpoints["sd_dsp_config"] = sd_config_url
    return {
        "schema": "aius-e2e-latency-evidence-v1",
        "state": "preflight",
        "started_utc": started_utc,
        "mode": mode,
        "dsp": dsp,
        "command_argv": command_argv,
        "requested_io": {
            "device": device,
            "format": "S16_LE",
            "channels": CHANNELS,
            "rate_hz": RATE,
            "period_frames": period,
            "buffer_frames": buffer_size,
        },
        "endpoints": endpoints,
        "renderer": str(renderer),
        "tool_versions": versions,
        "repository": repository,
        "acceptance": acceptance_contract(mode, dsp),
        "stimulus": {
            "seed_sha256": hashlib.sha256(SEED).hexdigest(),
            "source_peak_s16": 6000,
            "warmup_frames": warmup_frames,
            "probe_gap_frames": probe_gap_frames,
            "tail_frames": tail_frames,
        },
        "collection": {
            "status_delay_seconds": status_delay_seconds,
            "formal_bracket": formal_bracket_projection(
                mode, warmup_frames, probe_gap_frames, tail_frames, dsp
            ),
            "active_source_preflight": active_source_contract(),
        },
    }


def validate_evidence_manifest(
    manifest: dict,
    mode: str,
    allow_legacy: bool = False,
    dsp: str = DSP_MODE_PRODUCTION,
) -> None:
    validate_dsp_mode(dsp)
    if manifest.get("schema") != "aius-e2e-latency-evidence-v1":
        raise RuntimeError("unsupported latency evidence schema")
    if manifest.get("state") != "accepted":
        raise RuntimeError("latency evidence is not in the accepted state")
    if manifest.get("mode") != mode:
        raise RuntimeError("latency evidence mode does not match")
    contract = manifest.get("acceptance")
    version = (
        contract.get("contract_version")
        if isinstance(contract, dict)
        else None
    )
    legacy = version != ACCEPTANCE_CONTRACT_VERSION
    if legacy and dsp != DSP_MODE_PRODUCTION:
        # Every contract below 3 predates the DSP row mode, so the evidence is
        # a production row by construction: it can never satisfy a bypass
        # request, with or without --allow-legacy-evidence.
        raise RuntimeError(
            f"latency evidence acceptance contract is version {version!r}, "
            f"which has no {dsp} row; only production evidence exists below "
            f"contract version {ACCEPTANCE_CONTRACT_VERSION}"
        )
    if legacy and not allow_legacy:
        raise RuntimeError(
            f"latency evidence acceptance contract is version {version!r}, "
            f"expected {ACCEPTANCE_CONTRACT_VERSION}; the DSP row mode is "
            "not part of that evidence. Re-collect it, or pass "
            "--allow-legacy-evidence to reanalyze it as-is"
        )
    if not legacy:
        if manifest.get("dsp") != dsp:
            raise RuntimeError(
                "latency evidence DSP row mode does not match: "
                f"{manifest.get('dsp')!r} != {dsp!r}"
            )
        if contract != acceptance_contract(mode, dsp):
            raise RuntimeError(
                "latency evidence acceptance contract drifted"
            )
        if dsp == DSP_MODE_BYPASS:
            validate_bypass_evidence(manifest)
        preflight = manifest.get("active_source_preflight")
        if (
            not isinstance(preflight, dict)
            or preflight.get("accepted") is not True
            or preflight.get("mode") not in ACTIVE_SOURCE_ACCEPTED_MODES
        ):
            raise RuntimeError(
                "latency evidence has no accepted active-source preflight"
            )
    generation = manifest.get("generation")
    code_frames, _ = mode_geometry(mode)
    if (
        not isinstance(generation, dict)
        or generation.get("code_count") != CODE_COUNT
        or generation.get("code_frames") != code_frames
        or len(generation.get("input_positions", [])) != CODE_COUNT
        or len(generation.get("reference_positions", [])) != CODE_COUNT
    ):
        raise RuntimeError("latency evidence generation is malformed")
    artifacts = manifest.get("artifacts")
    required = {
        "carrier",
        "capture",
        "effective_dsp_config",
        "renderer_dsp_config",
        "native_reference",
        "native_decoded",
        "native_render_manifest",
        "summary",
    }
    if not legacy:
        required.add("active_source_preflight")
    if dsp == DSP_MODE_BYPASS:
        required |= BYPASS_ARTIFACT_NAMES
    if not isinstance(artifacts, dict) or not required <= artifacts.keys():
        raise RuntimeError("latency evidence artifact set is incomplete")
    for name, artifact in artifacts.items():
        if not isinstance(artifact, dict):
            raise RuntimeError(f"artifact {name} is malformed")
        path = artifact.get("path")
        digest = artifact.get("sha256")
        if (
            not isinstance(path, str)
            or Path(path).is_absolute()
            or ".." in Path(path).parts
            or not isinstance(digest, str)
            or len(digest) != 64
            or any(character not in "0123456789abcdef" for character in digest)
        ):
            raise RuntimeError(f"artifact {name} is malformed")
    for field in ("status_initial", "status_final", "collector_acceptance"):
        if not isinstance(manifest.get(field), dict):
            raise RuntimeError(f"latency evidence {field} is missing")


def codes(mode: str = "ac3") -> list[np.ndarray]:
    code_frames, _ = mode_geometry(mode)
    result = []
    for index in range(CODE_COUNT):
        digest = hashlib.sha256(
            SEED + index.to_bytes(4, "little")
        ).digest()
        rng = np.random.default_rng(
            int.from_bytes(digest[:8], "little")
        )
        bipolar = rng.choice(
            np.array([-1.0, 1.0]), size=code_frames + 2
        )
        shaped = (
            bipolar[:-2] + 2.0 * bipolar[1:-1] + bipolar[2:]
        ) / 4.0
        shaped -= np.mean(shaped)
        shaped /= np.max(np.abs(shaped))
        code = np.rint(shaped * 6000.0).astype(np.int16)
        sync_rng = np.random.default_rng(
            int.from_bytes(hashlib.sha256(
                SEED + b"sync" + index.to_bytes(4, "little")
            ).digest()[:8], "little")
        )
        code[:SYNC_FRAMES] = sync_rng.choice(
            np.array([-6000, 6000], dtype=np.int16),
            size=SYNC_FRAMES,
        )
        result.append(code)
    return result


def make_pcm(
    path: Path,
    probe_codes: list[np.ndarray],
    warmup_frames: int = WARMUP_FRAMES,
    guard_frames: int = PCM_PROBE_GAP_FRAMES,
    tail_frames: int = TAIL_FRAMES,
) -> list[int]:
    code_frames = len(probe_codes[0])
    total = (
        warmup_frames
        + CODE_COUNT * (code_frames + guard_frames)
        + tail_frames
    )
    samples = np.zeros((total, CHANNELS), dtype="<i2")
    positions = []
    position = warmup_frames
    for code in probe_codes:
        positions.append(position)
        samples[position:position + code_frames, 0] = code
        samples[position:position + code_frames, 1] = code
        position += code_frames + guard_frames
    samples.tofile(path)
    return positions


def run_command(argv: list[str], log_path: Path) -> None:
    result = subprocess.run(
        argv,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    header = json.dumps({
        "argv": argv,
        "returncode": result.returncode,
    }, separators=(",", ":"), sort_keys=True).encode()
    log_path.write_bytes(
        header
        + b"\n--- stdout ---\n"
        + result.stdout
        + b"\n--- stderr ---\n"
        + result.stderr
    )
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


def write_effective_config(
    raw: bytes, root: Path, prefix: str = "dsp-config"
) -> tuple[Path, Path]:
    effective_path = root / f"{prefix}-effective.json"
    normalized_path = root / f"{prefix}-renderer.json"
    effective_path.write_bytes(raw)
    normalized_path.write_bytes(normalize_effective_config(raw))
    return effective_path, normalized_path


def capture_effective_config(url: str, root: Path) -> tuple[Path, Path]:
    with urllib.request.urlopen(url, timeout=HTTP_TIMEOUT_SECONDS) as response:
        raw = response.read(MAX_DSP_CONFIG_BYTES + 1)
    return write_effective_config(raw, root)


# ---------------------------------------------------------------------------
# DSP model: parse, canonicalize, compare, and the PCM-bypass state machine.
# ---------------------------------------------------------------------------

MODEL_WIRE_KEYS = (
    "source",
    "config_generation",
    "sample_rate_hz",
    "features",
    "input_channels",
    "output_eq",
    "matrix",
    "limiter",
)
# Everything a PUT changes about the audio. `source`, `config_generation` and
# `base_generation` are wire/concurrency fields, not signal-processing choices.
MODEL_SEMANTIC_KEYS = MODEL_WIRE_KEYS[2:]
MODEL_ALL_KEYS = MODEL_WIRE_KEYS + ("base_generation",)
DERIVED_HEADROOM = "derived-by-clipfree"
BYPASS_ARTIFACT_NAMES = frozenset({
    "sd_dsp_config",
    "bypass_effective_dsp_config",
    "dsp_bypass_put_body",
    "dsp_restoration_put_body",
    "dsp_bypass_session",
})


def _load_build_config():
    """The checked-in factory-model validator, reused for its exact bounds.

    `lib/aius_dsp_config` states that it mirrors `build_config.validate_config`
    bounds exactly, so the runner parses through that one authority rather than
    keeping a second copy of every numeric range.
    """
    path = Path(__file__).resolve().parents[2] / "scripts" / "build_config.py"
    spec = importlib.util.spec_from_file_location("aius_build_config", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BUILD_CONFIG = _load_build_config()


def parse_dsp_model(raw: bytes, label: str) -> dict:
    """Parse a DSP model body exactly as the device's PUT parser accepts it.

    Accepts both wire shapes the runner sees: the effective `GET
    /api/dsp/config` response (eight keys) and a PUT body or persisted card
    file (those keys plus `base_generation`). `matrix.layout_aware` is
    optional and reads TRUE when absent, matching
    `aius_dsp_config.c`'s absent-reads-on default.
    """
    if not raw or len(raw) > MAX_DSP_CONFIG_BYTES:
        raise RuntimeError(f"{label} has invalid size")
    try:
        model = json.loads(raw)
    except ValueError as error:
        raise RuntimeError(f"{label} is not valid JSON: {error}") from error
    if not isinstance(model, dict):
        raise RuntimeError(f"{label} is not an object")
    unknown = sorted(set(model) - set(MODEL_ALL_KEYS))
    if unknown:
        raise RuntimeError(f"{label} has unknown keys: {', '.join(unknown)}")
    missing = [key for key in MODEL_WIRE_KEYS if key not in model]
    if missing:
        raise RuntimeError(f"{label} is missing keys: {', '.join(missing)}")
    if model["source"] not in ("sd", "default"):
        raise RuntimeError(f"{label} has an invalid source")
    for key in ("config_generation", "base_generation"):
        value = model.get(key)
        if key not in model:
            continue
        if (
            not isinstance(value, int)
            or isinstance(value, bool)
            or value < 0
        ):
            raise RuntimeError(f"{label} has an invalid {key}")
    model = json.loads(json.dumps(model))
    matrix = model["matrix"]
    if not isinstance(matrix, dict):
        raise RuntimeError(f"{label} matrix is not an object")
    matrix.setdefault("layout_aware", True)
    probe = {key: model[key] for key in MODEL_WIRE_KEYS}
    probe["source"] = "default"
    probe["config_generation"] = 0
    try:
        BUILD_CONFIG.validate_config(probe)
    except ValueError as error:
        raise RuntimeError(f"{label} is not a valid model: {error}") from error
    return model


def canonical_model(model: dict) -> dict:
    """The semantic model with every number widened and parser-noise removed.

    JSON round-trips write `0` and `0.0` differently while the device parses
    both to the same double, so the comparison form must not depend on which
    spelling a card file or serializer happened to use. Floats additionally
    quantize to 12 significant digits: the device's embedded JSON number
    parser is not correctly rounded (observed on hardware as a 1-ULP
    `lfe_gain_linear` divergence between the card text and the effective
    serialization of its own parse), so exact-double equality across the two
    parser implementations is unattainable while a restore PUT of the same
    digits still reproduces the identical board state. Twelve digits absorb
    that rounding by three orders of magnitude and still expose any genuine
    parameter difference, which lives in the leading digits.
    """
    def widen(value: object) -> object:
        if isinstance(value, bool):
            return value
        if isinstance(value, int):
            return float(value)
        if isinstance(value, float):
            return float(f"{value:.12g}")
        if isinstance(value, dict):
            return {key: widen(item) for key, item in value.items()}
        if isinstance(value, list):
            return [widen(item) for item in value]
        return value

    return {key: widen(model[key]) for key in MODEL_SEMANTIC_KEYS}


def comparable_model(model: dict) -> dict:
    """The canonical model minus the fields the device derives, not echoes.

    `GET /api/dsp/config` serializes the RESOLVED matrix headroom
    (`optical_main.cpp` http_dsp_config_json), so a `clipfree` model's stored
    `headroom_linear` is not observable in the effective response. Comparing it
    would reject a faithful card file, so this form marks it derived on both
    sides instead. With `clipfree` false the two forms are identical.
    """
    comparable = canonical_model(model)
    if comparable["matrix"].get("clipfree") is True:
        comparable["matrix"]["headroom_linear"] = DERIVED_HEADROOM
    return comparable


def _hash_model(model: dict) -> str:
    return hashlib.sha256(
        json.dumps(model, separators=(",", ":"), sort_keys=True).encode()
    ).hexdigest()


def model_hash(model: dict) -> str:
    """Hash of the exact semantic model, wire fields excluded."""
    return _hash_model(canonical_model(model))


def comparable_hash(model: dict) -> str:
    """Hash used for every card-file / device / model comparison."""
    return _hash_model(comparable_model(model))


def bypass_model(model: dict) -> dict:
    """The production model with the four expressible bypass switches off.

    Built by flipping flags on the parsed production model, never hand-written:
    the normal stereo unity route, every gain, EQ section, matrix value and
    limiter parameter is carried over verbatim so the only measured difference
    is the feature resolution the runtime adapter performs.
    """
    bypass = json.loads(json.dumps(model))
    for switch in BYPASS_FEATURE_SWITCHES:
        bypass["features"][switch] = False
    return bypass


def bypass_switch_evidence(production: dict, bypass: dict) -> dict:
    """Prove the two models differ in exactly the documented switches."""
    switches = {
        switch: {
            "production": production["features"].get(switch),
            "bypass": bypass["features"].get(switch),
        }
        for switch in BYPASS_FEATURE_SWITCHES
    }
    unexpected = [
        switch
        for switch, value in switches.items()
        if value["bypass"] is not False
    ]
    retained = {
        key: value
        for key, value in production["features"].items()
        if key not in BYPASS_FEATURE_SWITCHES
    }
    if retained != {
        key: value
        for key, value in bypass["features"].items()
        if key not in BYPASS_FEATURE_SWITCHES
    }:
        unexpected.append("retained-features")
    if canonical_model_without_features(
        production
    ) != canonical_model_without_features(bypass):
        unexpected.append("non-feature-field")
    if unexpected:
        raise RuntimeError(
            "bypass model differs from production outside the documented "
            f"switches: {', '.join(sorted(unexpected))}"
        )
    return {
        "switches": switches,
        "retained_features": retained,
        # The fifth documented switch has no model field; see
        # BYPASS_FEATURE_SWITCHES. The firmware pins the transmit word width,
        # so no PUT can introduce a dither-affecting width change.
        "dither_affecting_width": {
            "expressible_in_model": False,
            "output_width_bits": DSP_OUTPUT_WIDTH_BITS,
            "dither_engaged": False,
            "asserted": "model wire shape carries no width or dither key",
        },
    }


def canonical_model_without_features(model: dict) -> dict:
    canonical = canonical_model(model)
    canonical.pop("features")
    return canonical


def dsp_put_body(model: dict, base_generation: int) -> bytes:
    """The exact PUT /api/dsp/config body for `model` at `base_generation`.

    `source` is always the literal `sd` the parser demands, and
    `config_generation` is required-but-ignored, so it carries the same token.
    """
    body = {key: model[key] for key in MODEL_WIRE_KEYS}
    body["source"] = "sd"
    body["config_generation"] = base_generation
    body["base_generation"] = base_generation
    return json.dumps(body, separators=(",", ":"), sort_keys=True).encode()


def http_get(url: str, token: str | None = None) -> tuple[int, bytes]:
    """GET a bounded body, returning the status instead of raising on 4xx.

    A 404 on the card file is evidence the runner must record and abort on, not
    a transport failure, so every HTTP status reaches the caller.
    """
    headers = {} if token is None else {SD_AUTH_HEADER: token}
    request = urllib.request.Request(url, headers=headers, method="GET")
    try:
        with urllib.request.urlopen(
            request, timeout=HTTP_TIMEOUT_SECONDS
        ) as response:
            return int(response.status), response.read(
                MAX_DSP_CONFIG_BYTES + 1
            )
    except urllib.error.HTTPError as error:
        with error:
            return int(error.code), error.read(MAX_DSP_CONFIG_BYTES + 1)


def http_put(url: str, body: bytes, token: str) -> tuple[int, bytes]:
    request = urllib.request.Request(
        url,
        data=body,
        method="PUT",
        headers={
            SD_AUTH_HEADER: token,
            "Content-Type": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(
            request, timeout=HTTP_TIMEOUT_SECONDS
        ) as response:
            return int(response.status), response.read(
                MAX_DSP_CONFIG_BYTES + 1
            )
    except urllib.error.HTTPError as error:
        with error:
            return int(error.code), error.read(MAX_DSP_CONFIG_BYTES + 1)


def validate_bypass_evidence(manifest: dict) -> None:
    """Every claim a bypass row's manifest must carry to be admissible."""
    evidence = manifest.get("dsp_bypass")
    if not isinstance(evidence, dict):
        raise RuntimeError("bypass evidence is missing")
    download = evidence.get("sd_download")
    if (
        not isinstance(download, dict)
        or download.get("status") != 200
        or not isinstance(download.get("sha256"), str)
    ):
        raise RuntimeError("bypass evidence has no accepted card download")
    for section in ("production", "bypass"):
        block = evidence.get(section)
        if not isinstance(block, dict) or not isinstance(
            block.get("semantic_sha256"), str
        ):
            raise RuntimeError(f"bypass evidence {section} model is missing")
    if evidence["bypass"].get("applied") is not True:
        raise RuntimeError("bypass evidence does not record an applied model")
    bracket = evidence.get("bracket_config_hashes")
    expected = evidence["bypass"].get("comparable_sha256")
    if (
        not isinstance(bracket, dict)
        or bracket.get("initial") != expected
        or bracket.get("final") != expected
    ):
        raise RuntimeError(
            "bypass evidence bracket config hashes are not the bypass model"
        )
    restoration = evidence.get("restoration")
    if (
        not isinstance(restoration, dict)
        or restoration.get("verified") is not True
    ):
        raise RuntimeError(
            "bypass evidence does not record a verified restoration"
        )


class DspBypassSession:
    """Capture, verify, apply and restore the board's DSP model for one row.

    Fail-closed by construction. `prepare()` downloads the authoritative card
    file, proves it parses and is semantically the board's effective model, and
    parses the exact restoration body locally -- an absent, unparseable or
    mismatched file aborts there, before any write, and is never "restored" by
    inventing a default PUT. From the first PUT request onward `restore()` runs
    from `__exit__`, from SIGINT and from SIGTERM, and records exactly what
    state the board may be in when it cannot finish.
    """

    def __init__(
        self,
        *,
        config_url: str,
        sd_url: str,
        token: str,
        root: Path,
        effective_raw: bytes,
        poll_seconds: float = DSP_APPLY_POLL_SECONDS,
        timeout_seconds: float = DSP_APPLY_TIMEOUT_SECONDS,
        get=None,
        put=None,
        sleep=time.sleep,
        monotonic=time.monotonic,
    ) -> None:
        self.config_url = config_url
        self.sd_url = sd_url
        self.token = token
        self.root = root
        self.effective_raw = effective_raw
        self.poll_seconds = poll_seconds
        self.timeout_seconds = timeout_seconds
        self._get = http_get if get is None else get
        self._put = http_put if put is None else put
        self._sleep = sleep
        self._monotonic = monotonic

        self.sd_path = root / "sd-dsp-config.json"
        self.bypass_body_path = root / "dsp-bypass-put-body.json"
        self.restoration_body_path = root / "dsp-restoration-put-body.json"
        self.evidence_path = root / "dsp-bypass-session.json"

        self.effective_model: dict | None = None
        self.production_model: dict | None = None
        self.bypass = None
        self.base_generation: int | None = None
        self.sd_download: dict | None = None
        self.production_evidence: dict | None = None
        self.bypass_evidence: dict | None = None
        self.bracket_hashes: dict = {}
        self.restoration: dict = {"attempted": False, "verified": False}
        self.mutation_possible = False
        self.applied = False
        self.applied_raw: bytes | None = None
        self.applied_generation: int | None = None
        self.interrupted_signal: int | None = None
        self._previous_handlers: dict = {}
        self._restoring = False

    # -- capture and verification, all before any mutation -----------------

    def prepare(self) -> dict:
        """Download and verify the card file; build and parse both bodies."""
        status, raw = self._get(self.sd_url, self.token)
        self.sd_path.write_bytes(raw)
        self.sd_download = {
            "url": self.sd_url,
            "status": status,
            "bytes": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest(),
            "path": self.sd_path.name,
        }
        if status == 404:
            raise RuntimeError(
                f"{self.sd_url} returned 404: the board has no restorable "
                "DSP override, and the public API cannot delete an absent "
                "one, so no PUT is issued. Provide the authoritative card "
                "file before running the bypass row"
            )
        if status != 200:
            raise RuntimeError(
                f"{self.sd_url} returned HTTP {status}; refusing to mutate "
                "the board without an authoritative restoration artifact"
            )
        self.production_model = parse_dsp_model(raw, "card DSP model")
        self.effective_model = parse_dsp_model(
            self.effective_raw, "effective DSP config"
        )
        if self.effective_model["source"] != "sd":
            # Before this row's own PUTs, `source` still means what it says: a
            # `default` board is running the baked model with no SD override
            # in force. The public API cannot delete an override, so restoring
            # the card file would leave the board on `sd` rather than where it
            # started. That needs an explicit preparation decision, not a PUT.
            raise RuntimeError(
                "the board's effective DSP model reports source "
                f"{self.effective_model['source']!r}, not 'sd': no SD "
                "override is in force, so this row cannot be undone. "
                "Aborting before any mutation"
            )
        card_hash = comparable_hash(self.production_model)
        effective_hash = comparable_hash(self.effective_model)
        if card_hash != effective_hash:
            raise RuntimeError(
                "card DSP model does not match the board's effective model "
                f"({card_hash} != {effective_hash}); aborting before any "
                "mutation, because restoring it would not restore what the "
                "board is running"
            )
        self.base_generation = self.effective_model["config_generation"]
        self.production_evidence = {
            "semantic_sha256": model_hash(self.production_model),
            "comparable_sha256": card_hash,
            "effective_semantic_sha256": model_hash(self.effective_model),
            "effective_sha256": hashlib.sha256(
                self.effective_raw
            ).hexdigest(),
            "base_generation": self.base_generation,
            "source": self.effective_model["source"],
        }
        self.bypass = bypass_model(self.production_model)
        switches = bypass_switch_evidence(self.production_model, self.bypass)
        parse_dsp_model(
            dsp_put_body(self.bypass, self.base_generation),
            "bypass PUT body",
        )
        # The restoration body is parsed locally at a fresh token, so the
        # runner never proves it with a mutating dry run. `restore()` rebuilds
        # it against the generation in force at that moment.
        parse_dsp_model(
            dsp_put_body(self.production_model, self.base_generation + 1),
            "restoration PUT body",
        )
        self.bypass_evidence = {
            "semantic_sha256": model_hash(self.bypass),
            "comparable_sha256": comparable_hash(self.bypass),
            "applied": False,
            **switches,
        }
        return self.evidence()

    # -- mutation ----------------------------------------------------------

    def apply(self) -> dict:
        """PUT the bypass model and wait for the block-boundary apply."""
        if self.bypass is None:
            raise RuntimeError("bypass session was not prepared")
        body = dsp_put_body(self.bypass, self.base_generation)
        self.bypass_body_path.write_bytes(body)
        self.mutation_possible = True
        status, response = self._put(self.config_url, body, self.token)
        self.bypass_evidence.update({
            "put_status": status,
            "put_body_sha256": hashlib.sha256(body).hexdigest(),
            "put_base_generation": self.base_generation,
            "put_body_path": self.bypass_body_path.name,
        })
        if status not in (200, 204):
            if status in NON_MUTATING_PUT_STATUSES:
                # A bad token, an invalid body and a stale base_generation are
                # all refused before the SD job and before the staged-model
                # swap, so the board is provably untouched and needs no
                # restoration PUT. Anything else stays fail-safe.
                self.mutation_possible = False
            raise RuntimeError(
                f"bypass PUT rejected with HTTP {status}: "
                f"{response[:200]!r}"
            )
        settled = self._settle(
            self.bypass, self.base_generation, "bypass model"
        )
        self.applied = True
        self.applied_raw = settled.pop("raw")
        self.applied_generation = settled["config_generation"]
        self.bypass_evidence.update({"applied": True, **settled})
        return settled

    def bracket_hash(self, label: str) -> str:
        """Gate one bracket edge on the effective config being the bypass."""
        status, raw = self._get(self.config_url, None)
        if status != 200:
            raise RuntimeError(
                f"effective DSP config at bracket {label} returned "
                f"HTTP {status}"
            )
        model = parse_dsp_model(raw, f"effective DSP config ({label})")
        digest = comparable_hash(model)
        self.bracket_hashes[label] = digest
        self.bracket_hashes[f"{label}_response_sha256"] = hashlib.sha256(
            raw
        ).hexdigest()
        self.bracket_hashes[f"{label}_config_generation"] = model[
            "config_generation"
        ]
        expected = self.bypass_evidence["comparable_sha256"]
        if digest != expected:
            raise RuntimeError(
                f"effective DSP config hash at bracket {label} is not the "
                f"bypass model ({digest} != {expected})"
            )
        return digest

    # -- restoration -------------------------------------------------------

    def restore(self) -> dict:
        """Restore the production model. Idempotent, and never raises.

        A failed attempt does not latch: an exit path that runs after a failed
        in-band restore gets one more genuine attempt at leaving the board on
        its production model. Every attempt is retained.
        """
        if (
            not self.mutation_possible
            or self.restoration["verified"]
            or self._restoring
        ):
            return self.restoration
        attempt: dict = {"utc": utc_now()}
        self._restoring = True
        try:
            attempt.update(self._restore())
            attempt["verified"] = True
        except BaseException as error:  # noqa: BLE001 - recorded, not raised
            attempt["verified"] = False
            attempt["error"] = str(error)
        finally:
            # A signal delivered inside the restore must not start a second
            # one on top of the first.
            self._restoring = False
        self.restoration["attempted"] = True
        self.restoration.setdefault("attempts", []).append(attempt)
        self.restoration.update(attempt)
        if attempt["verified"]:
            self.restoration.pop("error", None)
        self.restoration["board_state"] = self.board_state()
        return self.restoration

    def _restore(self) -> dict:
        status, raw = self._get(self.config_url, None)
        if status != 200:
            raise RuntimeError(
                f"cannot read a fresh token: effective DSP config returned "
                f"HTTP {status}"
            )
        current = parse_dsp_model(raw, "effective DSP config")
        token = current["config_generation"]
        body = dsp_put_body(self.production_model, token)
        parse_dsp_model(body, "restoration PUT body")
        self.restoration_body_path.write_bytes(body)
        put_status, response = self._put(self.config_url, body, self.token)
        record = {
            "token_base_generation": token,
            "put_status": put_status,
            "put_body_sha256": hashlib.sha256(body).hexdigest(),
            "put_body_path": self.restoration_body_path.name,
        }
        if put_status not in (200, 204):
            raise RuntimeError(
                f"restoration PUT rejected with HTTP {put_status}: "
                f"{response[:200]!r}"
            )
        settled = self._settle(
            self.production_model, token, "production model"
        )
        restored_raw = settled.pop("raw")
        restored = parse_dsp_model(restored_raw, "restored DSP config")
        semantic = model_hash(restored)
        baseline = self.production_evidence["effective_semantic_sha256"]
        failures = []
        if comparable_hash(restored) != comparable_hash(self.production_model):
            failures.append("restored semantic model is not the card model")
        if semantic != baseline:
            failures.append(
                f"restored effective config hash {semantic} is not the "
                f"captured {baseline}"
            )
        if restored["source"] != "sd":
            failures.append(
                f"restored active source is {restored['source']!r}, not 'sd'"
            )
        if failures:
            raise RuntimeError("; ".join(failures))
        record.update(settled)
        record.update({
            "semantic_sha256": semantic,
            "comparable_sha256": comparable_hash(restored),
            "effective_sha256": hashlib.sha256(restored_raw).hexdigest(),
            "active_source": restored["source"],
        })
        return record

    def _settle(
        self, target: dict, previous_generation: int, label: str
    ) -> dict:
        """Poll the effective config until the board publishes `target`.

        The device applies a staged model at the next audio block boundary, so
        settle is observed rather than assumed: the published model must be the
        target, `source` must be `sd`, and the generation must have advanced.
        """
        expected = comparable_hash(target)
        started = self._monotonic()
        deadline = started + self.timeout_seconds
        blocker = "no response"
        while True:
            self._sleep(self.poll_seconds)
            status, raw = self._get(self.config_url, None)
            if status != 200:
                blocker = f"HTTP {status}"
            else:
                try:
                    model = parse_dsp_model(raw, "effective DSP config")
                except RuntimeError as error:
                    blocker = str(error)
                else:
                    digest = comparable_hash(model)
                    generation = model["config_generation"]
                    if digest != expected:
                        blocker = f"effective hash {digest} != {expected}"
                    elif model["source"] != "sd":
                        blocker = f"active source is {model['source']!r}"
                    elif generation == previous_generation:
                        blocker = (
                            f"config_generation is still {generation}"
                        )
                    else:
                        return {
                            "config_generation": generation,
                            "settle_seconds": (
                                self._monotonic() - started
                            ),
                            "raw": raw,
                        }
            if self._monotonic() >= deadline:
                raise RuntimeError(
                    f"{label} did not settle within "
                    f"{self.timeout_seconds:.1f} s: {blocker}"
                )

    # -- lifecycle ---------------------------------------------------------

    def __enter__(self) -> "DspBypassSession":
        self.install_handlers()
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> bool:
        self.remove_handlers()
        self.restore()
        return False

    def install_handlers(self) -> None:
        for number in (signal.SIGINT, signal.SIGTERM):
            self._previous_handlers[number] = signal.getsignal(number)
            signal.signal(number, self._on_signal)

    def remove_handlers(self) -> None:
        while self._previous_handlers:
            number, handler = self._previous_handlers.popitem()
            signal.signal(number, handler)

    def _on_signal(self, number: int, frame) -> None:
        self.interrupted_signal = int(number)
        self.restore()
        raise KeyboardInterrupt(
            f"interrupted by signal {int(number)}; DSP restore "
            + ("verified" if self.restoration["verified"] else "FAILED")
        )

    # -- evidence ----------------------------------------------------------

    def board_state(self) -> str | None:
        """What state the board may be in, when the restore did not finish."""
        if not self.mutation_possible or self.restoration["verified"]:
            return None
        card = self.sd_download["sha256"] if self.sd_download else "unknown"
        running = (
            "the bypass model "
            f"(semantic sha256 {self.bypass_evidence['semantic_sha256']})"
            if self.applied
            else "either the production or the bypass model: the PUT was "
            "issued but never confirmed applied"
        )
        return (
            f"DSP NOT RESTORED. The board is running {running}. The bypass "
            f"PUT rewrites /aius/dsp.json, so the card file no longer holds "
            f"the production model and a reboot reloads whatever it now "
            f"holds. The original card bytes (sha256 {card}) are retained at "
            f"{self.sd_path.name} and the exact restoration body at "
            f"{self.restoration_body_path.name}; re-apply it with a fresh "
            "base_generation from GET /api/dsp/config"
        )

    def restore_rejection(self) -> str:
        return (
            "DSP restoration failed: "
            f"{self.restoration.get('error', 'unknown error')}. "
            + (self.board_state() or "")
        )

    def evidence(self) -> dict:
        return {
            "sd_download": self.sd_download,
            "production": self.production_evidence,
            "bypass": self.bypass_evidence,
            "bracket_config_hashes": dict(self.bracket_hashes),
            "restoration": dict(self.restoration),
            "interrupted_signal": self.interrupted_signal,
            "board_state": self.board_state(),
        }

    def write_evidence(self) -> Path:
        write_json(self.evidence_path, self.evidence())
        return self.evidence_path

    def artifact_records(self) -> dict:
        records = {}
        for name, path in (
            ("sd_dsp_config", self.sd_path),
            ("dsp_bypass_put_body", self.bypass_body_path),
            ("dsp_restoration_put_body", self.restoration_body_path),
            ("dsp_bypass_session", self.evidence_path),
        ):
            if path.is_file():
                records[name] = artifact_record(path)
        return records


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
    decoded_path = root / "native-decoded.s32le"
    render_manifest_path = root / "native-render-manifest.json"
    run_command([
        str(renderer),
        "--carrier", str(carrier_path),
        "--config", str(config_path),
        "--pcm", str(reference_path),
        "--decoded", str(decoded_path),
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
    decoded = manifest.get("decoded", {})
    decoded_frames = decoded.get("frames")
    decoded_channels = decoded.get("channels")
    if (
        not isinstance(decoded_frames, int)
        or not isinstance(decoded_channels, int)
        or decoded_frames <= 0
        or not 1 <= decoded_channels <= 6
        or not decoded_path.is_file()
        or decoded_path.stat().st_size
        != decoded_frames * decoded_channels * 4
    ):
        raise RuntimeError("native decoded stream length is invalid")
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
    carrier = carrier_path.read_bytes()
    for unit in manifest.get("units", []):
        start = (unit["pa_index"] + 4) * 2
        padded_bytes = (unit["payload_bytes"] + 1) & ~1
        payload = carrier[start:start + padded_bytes]
        if len(payload) != padded_bytes:
            raise RuntimeError("native unit payload extends beyond carrier")
        if unit["byte_order"] == "little-endian":
            payload = b"".join(
                payload[index:index + 2][::-1]
                for index in range(0, len(payload), 2)
            )
        elif unit["byte_order"] != "big-endian":
            raise RuntimeError("native unit has unknown byte order")
        payload = payload[:unit["payload_bytes"]]
        unit["elementary_sha256"] = hashlib.sha256(payload).hexdigest()
    manifest["carrier_sha256"] = sha256(carrier_path)
    manifest["config_sha256"] = sha256(config_path)
    write_json(render_manifest_path, manifest)
    return reference, manifest


def ac3_commands(root: Path) -> tuple[list[str], list[str]]:
    source_path = root / "source-5.1.s16le"
    ac3_path = root / "carrier.ac3"
    carrier_path = root / "carrier.s16le"
    encode = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "s16le", "-ar", str(RATE), "-ac", "6",
        "-channel_layout", "5.1", "-i", str(source_path),
        "-map", "0:a", "-ar", str(RATE), "-c:a", "ac3", "-b:a", "448k",
        "-f", "ac3", str(ac3_path),
    ]
    wrap = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(ac3_path), "-map", "0:a", "-c:a", "copy",
        "-f", "spdif", str(carrier_path),
    ]
    return encode, wrap


def make_ac3(
    root: Path,
    probe_codes: list[np.ndarray],
    warmup_frames: int = WARMUP_FRAMES,
    guard_frames: int = AC3_PROBE_GAP_FRAMES,
    tail_frames: int = TAIL_FRAMES,
) -> tuple[Path, list[int], list[int]]:
    code_frames = len(probe_codes[0])
    total = (
        warmup_frames
        + CODE_COUNT * (code_frames + guard_frames)
        + tail_frames
    )
    source = np.zeros((total, 6), dtype="<i2")
    source_positions = []
    position = warmup_frames
    for code in probe_codes:
        source_positions.append(position)
        source[position:position + code_frames, 0] = code
        source[position:position + code_frames, 1] = code
        position += code_frames + guard_frames
    source_path = root / "source-5.1.s16le"
    source.tofile(source_path)
    source_wav_path = root / "source-5.1.wav"
    with wave.open(str(source_wav_path), "wb") as source_wav:
        source_wav.setnchannels(6)
        source_wav.setsampwidth(2)
        source_wav.setframerate(RATE)
        source_wav.writeframes(source.tobytes())
    ac3_path = root / "carrier.ac3"
    carrier_path = root / "carrier.s16le"

    encode, wrap = ac3_commands(root)
    run_command(encode, root / "ffmpeg-encode.log")
    run_command(wrap, root / "ffmpeg-wrap.log")

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
    if np.any(periods != AC3_IEC_PERIOD_FRAMES):
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
    evidence = correlation_evidence(signal, template, low, high)
    return evidence["position"], evidence["correlation_abs"]


def correlation_evidence(
    signal: np.ndarray,
    template: np.ndarray,
    low: int,
    high: int,
) -> dict:
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
    magnitudes = np.abs(normalized)
    index = int(np.argmax(magnitudes))
    excluded_low = max(0, index - MAIN_LOBE_FRAMES)
    excluded_high = min(len(magnitudes), index + MAIN_LOBE_FRAMES + 1)
    candidates = np.concatenate((
        magnitudes[:excluded_low],
        magnitudes[excluded_high:],
    ))
    second = float(np.max(candidates)) if len(candidates) else 0.0
    primary = float(magnitudes[index])
    return {
        "position": low + index,
        "correlation": float(normalized[index]),
        "correlation_abs": primary,
        "second_correlation_abs": second,
        "peak_ratio": (
            (1e300 if primary > 0.0 else 0.0)
            if second == 0.0
            else primary / second
        ),
    }


def signal_to_guard_db(
    signal: np.ndarray,
    position: int,
    code_frames: int,
    guard_frames: int,
) -> float:
    before = position - guard_frames
    after = position + code_frames + guard_frames
    if before < 0 or after > len(signal):
        raise RuntimeError("capture does not retain the full probe guard")
    active = signal[position:position + code_frames].astype(np.float64)
    guard = np.concatenate((
        signal[before:position],
        signal[position + code_frames:after],
    )).astype(np.float64)
    signal_rms = float(np.sqrt(np.mean(active * active)))
    guard_rms = max(float(np.sqrt(np.mean(guard * guard))), 1.0)
    if signal_rms == 0.0:
        return float("-inf")
    return 20.0 * float(np.log10(signal_rms / guard_rms))


def validate_render_level(reference: np.ndarray, model: dict) -> dict:
    if reference.size == 0:
        raise RuntimeError("native reference is empty")
    peak = int(np.max(np.abs(reference.astype(np.int64))))
    if peak == 0 or peak > 8388607:
        raise RuntimeError("native reference peak is outside the 24-bit scale")
    sample_peak_dbfs = 20.0 * float(np.log10(peak / 8388607.0))
    conservative_true_peak_dbfs = sample_peak_dbfs + 1.0
    features = model.get("features", {})
    limiter = model.get("limiter", {})
    limiter_active = (
        features.get("limiter") is True
        and limiter.get("enabled") is True
    )
    if limiter_active:
        threshold = limiter.get("threshold_dbfs")
        if not isinstance(threshold, (int, float)):
            raise RuntimeError("effective limiter threshold is invalid")
        maximum = float(threshold) - 6.0
    else:
        maximum = -6.0
    if conservative_true_peak_dbfs > maximum:
        raise RuntimeError(
            "native reference lacks 6 dB limiter/headroom margin"
        )
    return {
        "sample_peak_s24": peak,
        "sample_peak_dbfs": sample_peak_dbfs,
        "conservative_true_peak_dbfs": conservative_true_peak_dbfs,
        "true_peak_bound": "sample_peak_plus_1_db",
        "limiter_active": limiter_active,
        "maximum_true_peak_dbfs": maximum,
        "margin_db": maximum - conservative_true_peak_dbfs,
    }


def robust_affine_fit(inputs: np.ndarray, captures: np.ndarray) -> dict:
    slopes = [
        (captures[right] - captures[left])
        / (inputs[right] - inputs[left])
        for left in range(len(inputs))
        for right in range(left + 1, len(inputs))
        if inputs[right] != inputs[left]
    ]
    if not slopes:
        raise RuntimeError("robust affine fit has no finite pair")
    ratio = float(np.median(np.asarray(slopes, dtype=np.float64)))
    offset = float(np.median(captures - ratio * inputs))
    residuals = captures - (offset + ratio * inputs)
    return {
        "capture_per_playback_ratio": ratio,
        "offset_frames": offset,
        "residual_max_abs_frames": float(
            np.max(np.abs(residuals))
        ),
        "points": len(inputs),
        "rejected_points": 0,
        "method": "median_pairwise_slope",
    }


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
    code_frames: int = AC3_CODE_FRAMES,
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
                and reference_position + code_frames - 1
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
    excluded_indices: set[int] | None = None,
    dsp: str = DSP_MODE_PRODUCTION,
) -> dict:
    code_frames, guard_frames = mode_geometry(mode)
    validate_dsp_mode(dsp)
    if (
        capture.ndim != 2
        or reference.ndim != 2
        or capture.shape[1] != CHANNELS
        or reference.shape[1] != CHANNELS
    ):
        raise RuntimeError("capture and reference must be stereo")
    if (
        len(reference_positions) != CODE_COUNT
        or len(input_positions) != CODE_COUNT
    ):
        raise RuntimeError("probe position count is not exactly 24")
    excluded = set() if excluded_indices is None else set(excluded_indices)
    if any(index < 0 or index >= CODE_COUNT for index in excluded):
        raise RuntimeError("excluded probe index is out of range")
    expected_codes = CODE_COUNT - len(excluded)
    if expected_codes < MIN_ACCEPTED_CODES:
        raise RuntimeError("fewer than 20 probes remain after exclusions")
    if np.any((capture == -32768) | (capture == 32767)):
        raise RuntimeError("capture contains clipped S16 samples")
    capture_mono = np.mean(capture.astype(np.float64), axis=1)
    reference_mono = np.mean(reference.astype(np.float64), axis=1)
    occurrences = []
    for index, (reference_position, input_position) in enumerate(zip(
        reference_positions, input_positions
    )):
        if index in excluded:
            continue
        template = reference_mono[
            reference_position:reference_position + code_frames
        ]
        evidence = correlation_evidence(
            capture_mono,
            template,
            input_position,
            input_position + MAX_LAG_FRAMES,
        )
        capture_position = evidence["position"]
        signal_to_guard = signal_to_guard_db(
            capture_mono,
            capture_position,
            code_frames,
            guard_frames,
        )
        channel_positions = []
        channel_correlations = []
        channel_peak_ratios = []
        channel_signal_to_guard = []
        for channel in range(CHANNELS):
            channel_evidence = correlation_evidence(
                capture[:, channel],
                reference[
                    reference_position:reference_position + code_frames,
                    channel,
                ],
                input_position,
                input_position + MAX_LAG_FRAMES,
            )
            channel_position = channel_evidence["position"]
            channel_positions.append(channel_position)
            channel_correlations.append(
                channel_evidence["correlation_abs"]
            )
            channel_peak_ratios.append(channel_evidence["peak_ratio"])
            channel_signal_to_guard.append(signal_to_guard_db(
                capture[:, channel],
                channel_position,
                code_frames,
                guard_frames,
            ))
        accepted = (
            evidence["correlation_abs"] >= MIN_CORRELATION
            and evidence["peak_ratio"] >= MIN_PEAK_RATIO
            and signal_to_guard >= MIN_SIGNAL_TO_GUARD_DB
            and all(
                value >= MIN_CORRELATION
                for value in channel_correlations
            )
            and all(
                value >= MIN_PEAK_RATIO
                for value in channel_peak_ratios
            )
            and all(
                value >= MIN_SIGNAL_TO_GUARD_DB
                for value in channel_signal_to_guard
            )
        )
        occurrences.append({
            "index": index,
            "accepted": accepted,
            "input_frame": input_position,
            "reference_frame": reference_position,
            "capture_frame": capture_position,
            "latency_frames": capture_position - input_position,
            "latency_ms": (
                (capture_position - input_position) * 1000.0 / RATE
            ),
            "correlation": evidence["correlation_abs"],
            "correlation_signed": evidence["correlation"],
            "second_correlation": evidence["second_correlation_abs"],
            "peak_ratio": evidence["peak_ratio"],
            "signal_to_guard_db": signal_to_guard,
            "channel_capture_frames": channel_positions,
            "channel_latency_frames": [
                position - input_position for position in channel_positions
            ],
            "channel_correlations": channel_correlations,
            "channel_peak_ratios": channel_peak_ratios,
            "channel_signal_to_guard_db": channel_signal_to_guard,
            "channel_skew_frames": (
                channel_positions[1] - channel_positions[0]
            ),
        })

    accepted_occurrences = sum(
        item["accepted"] for item in occurrences
    )
    if accepted_occurrences != expected_codes:
        rejected = [
            item["index"] for item in occurrences if not item["accepted"]
        ]
        raise RuntimeError(
            f"only {accepted_occurrences}/{expected_codes} codes meet "
            f"formal acceptance; rejected={rejected}"
        )
    inputs = np.array(
        [item["input_frame"] for item in occurrences], dtype=np.float64
    )
    captures = np.array(
        [item["capture_frame"] for item in occurrences], dtype=np.float64
    )
    robust = robust_affine_fit(inputs, captures)
    if not (
        MIN_CLOCK_RATIO
        <= robust["capture_per_playback_ratio"]
        <= MAX_CLOCK_RATIO
    ):
        raise RuntimeError(
            "robust affine ratio is out of bounds: "
            f"{robust['capture_per_playback_ratio']}"
        )
    if (
        robust["residual_max_abs_frames"]
        > MAX_AFFINE_RESIDUAL_FRAMES
    ):
        raise RuntimeError(
            "robust affine residual exceeds 128 frames: "
            f"{robust['residual_max_abs_frames']}"
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
    channel_accepted = [
        sum(
            item["channel_correlations"][channel] >= MIN_CORRELATION
            and item["channel_peak_ratios"][channel] >= MIN_PEAK_RATIO
            and item["channel_signal_to_guard_db"][channel]
            >= MIN_SIGNAL_TO_GUARD_DB
            for item in occurrences
        )
        for channel in range(CHANNELS)
    ]
    if np.any(np.diff(captures) <= 0):
        raise RuntimeError("capture associations are not monotonic")
    if not MIN_CLOCK_RATIO <= ratio <= MAX_CLOCK_RATIO:
        raise RuntimeError(f"affine ratio is out of bounds: {ratio}")
    residual_max = float(np.max(np.abs(residuals)))
    if residual_max > MAX_AFFINE_RESIDUAL_FRAMES:
        raise RuntimeError(
            f"affine residual exceeds 128 frames: {residual_max}"
        )
    summary = {
        "mode": mode,
        "dsp": dsp,
        "unequal_stereo_frames": int(np.count_nonzero(
            capture[:, 0] != capture[:, 1]
        )),
        "unequal_stereo_fraction": float(np.mean(
            capture[:, 0] != capture[:, 1]
        )),
        "occurrences": occurrences,
        "excluded_indices": sorted(excluded),
        "accepted_occurrences": accepted_occurrences,
        "channel_accepted_occurrences": channel_accepted,
        "correlation_min": min(
            item["correlation"] for item in occurrences
        ),
        "channel_correlation_min": min(
            min(item["channel_correlations"]) for item in occurrences
        ),
        "peak_ratio_min": min(
            item["peak_ratio"] for item in occurrences
        ),
        "channel_peak_ratio_min": min(
            min(item["channel_peak_ratios"]) for item in occurrences
        ),
        "signal_to_guard_db_min": min(
            item["signal_to_guard_db"] for item in occurrences
        ),
        "channel_signal_to_guard_db_min": min(
            min(item["channel_signal_to_guard_db"])
            for item in occurrences
        ),
        "thresholds": acceptance_contract(mode, dsp),
        "robust_initial_affine": robust,
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
    result = {}
    for path in ZERO_DELTA_PATHS:
        before = nested_value(initial, path)
        after = nested_value(final, path)
        result[".".join(path)] = (int(after) - int(before)) & 0xFFFFFFFF
    return result


def nested_value(value: dict, path: tuple[str, ...]) -> object:
    current: object = value
    for part in path:
        if not isinstance(current, dict) or part not in current:
            raise RuntimeError(
                f"status is missing required field {'.'.join(path)}"
            )
        current = current[part]
    return current


def uint32_delta(initial: dict, final: dict, path: tuple[str, ...]) -> int:
    before = int(nested_value(initial, path))
    after = int(nested_value(final, path))
    if not 0 <= before <= 0xFFFFFFFF or not 0 <= after <= 0xFFFFFFFF:
        raise RuntimeError(
            f"status counter is not uint32: {'.'.join(path)}"
        )
    return (after - before) & 0xFFFFFFFF


def validate_status_bracket(
    mode: str,
    initial: dict,
    final: dict,
    effective_config: dict,
    expected_build_revision: str | None = None,
) -> dict:
    failures = []
    for field in (
        "build_revision",
        "build_utc",
        "boot_generation",
        "reset_source",
        "watchdog_early_resets",
    ):
        if initial.get(field) != final.get(field):
            failures.append(f"{field} changed")
    if effective_config.get("sample_rate_hz") != RATE:
        failures.append("effective DSP config is not 48 kHz")
    if (
        expected_build_revision is not None
        and final.get("build_revision") != expected_build_revision
    ):
        failures.append("firmware build revision is not the harness image")

    for label, status in (("initial", initial), ("final", final)):
        clock = status.get("clock", {})
        pipeline = status.get("pipeline_diag", {})
        audio = status.get("audio_diag", {})
        if status.get("carrier") != 1:
            failures.append(f"{label} carrier is not present")
        expected_clock = {
            "carrier_locked": True,
            "carrier_ready": True,
            "nominal_rate_hz": RATE,
            "stream_rate_hz": RATE,
            "master": "input",
            "state": "tracking",
        }
        for field, expected in expected_clock.items():
            if clock.get(field) != expected:
                failures.append(
                    f"{label} clock.{field} != {expected!r}"
                )
        if (
            isinstance(clock.get("output_rate_millihz"), int)
            and isinstance(clock.get("carrier_rate_millihz"), int)
        ):
            if abs(
                clock["output_rate_millihz"]
                - clock["carrier_rate_millihz"]
            ) > 1000:
                failures.append(
                    f"{label} clock follow error exceeds 1000 millihertz"
                )
        else:
            failures.append(f"{label} clock rates are missing")
        expected_class = 1 if mode == "pcm" else 2
        if pipeline.get("class") != expected_class:
            failures.append(f"{label} pipeline class is wrong")
        if mode == "pcm":
            if pipeline.get("pcm_confirmed") != 1:
                failures.append(f"{label} PCM is not confirmed")
        elif pipeline.get("codec_pc") != 1:
            failures.append(f"{label} codec Pc is not AC-3")
        for path in (
            ("audio_diag", "service_last_failure"),
            ("audio_diag", "pipeline_last_failure"),
            ("clock", "last_clock_fault"),
            ("pipeline_diag", "last_status"),
            ("pipeline_diag", "alloc_count"),
            ("pipeline_diag", "dts_alloc_count"),
            ("pipeline_diag", "ac3_alloc_count"),
        ):
            if int(nested_value(status, path)) != 0:
                failures.append(
                    f"{label} {'.'.join(path)} is nonzero"
                )
        if not isinstance(audio, dict) or not isinstance(pipeline, dict):
            failures.append(f"{label} diagnostics are malformed")

    zero_deltas = counter_deltas(initial, final)
    for name, delta in zero_deltas.items():
        if delta != 0:
            failures.append(f"{name} advanced by {delta}")
    progress = {
        ".".join(path): uint32_delta(initial, final, path)
        for path in PROGRESS_PATHS
    }
    if mode == "ac3":
        for path in (
            ("audio_diag", "decode_units"),
            ("pipeline_diag", "bursts_ok"),
        ):
            progress[".".join(path)] = uint32_delta(
                initial, final, path
            )
    for name, delta in progress.items():
        if delta == 0:
            failures.append(f"{name} made no progress")
    if failures:
        raise RuntimeError(
            "status bracket rejected: " + "; ".join(failures)
        )
    return {
        "accepted": True,
        "build_revision": final["build_revision"],
        "build_utc": final["build_utc"],
        "zero_deltas": zero_deltas,
        "progress_deltas": progress,
        "maximum_follow_error_millihz": max(
            abs(
                status["clock"]["output_rate_millihz"]
                - status["clock"]["carrier_rate_millihz"]
            )
            for status in (initial, final)
        ),
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("pcm", "ac3"), required=True)
    parser.add_argument(
        "--dsp",
        choices=DSP_MODES,
        default=DSP_MODE_PRODUCTION,
        help=(
            "DSP row: the board's production model, or the PCM-only bypass "
            "model that disables input EQ, matrix, output EQ and limiter"
        ),
    )
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--device", default="hw:ICUSBAUDIO7D,0")
    parser.add_argument(
        "--status-url", default="http://192.0.2.66/api/status"
    )
    parser.add_argument(
        "--config-url", default="http://192.0.2.66/api/dsp/config"
    )
    parser.add_argument(
        "--sd-config-url",
        default=SD_DSP_CONFIG_URL,
        help="authenticated download of the authoritative card DSP model",
    )
    parser.add_argument(
        "--auth",
        default=os.environ.get("AIUS_SD_AUTH"),
        help=f"{SD_AUTH_HEADER} token (default: $AIUS_SD_AUTH)",
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
    parser.add_argument(
        "--warmup-frames", type=int, default=FORMAL_WARMUP_FRAMES
    )
    parser.add_argument(
        "--probe-gap-frames",
        "--guard-frames",
        dest="guard_frames",
        type=int,
    )
    parser.add_argument("--tail-frames", type=int, default=TAIL_FRAMES)
    parser.add_argument(
        "--status-delay",
        type=float,
        default=FORMAL_STATUS_DELAY_SECONDS,
    )
    parser.add_argument(
        "--reuse-existing",
        action="store_true",
        help="reanalyze an existing carrier, capture, and manifest",
    )
    parser.add_argument(
        "--allow-legacy-evidence",
        action="store_true",
        help=(
            "reanalyze evidence collected under an older acceptance "
            "contract version (requires --reuse-existing)"
        ),
    )
    return parser


def validate_arguments(args: argparse.Namespace) -> dict:
    """Reject CLI knobs that cannot produce valid evidence.

    Runs before the harness touches the device, and returns the formal-bracket
    derivation so the evidence records what was proven here.
    """
    _, required_guard_frames = mode_geometry(args.mode)
    default_probe_gap_frames = mode_probe_gap(args.mode)
    guard_frames = (
        default_probe_gap_frames
        if args.guard_frames is None
        else args.guard_frames
    )
    validate_dsp_mode(args.dsp)
    if args.dsp == DSP_MODE_BYPASS:
        if args.mode != "pcm":
            raise ValueError(
                "--dsp bypass is a PCM-only row: the encoded rows measure "
                "the production decoder-plus-matrix path"
            )
        if not args.reuse_existing and not args.auth:
            raise ValueError(
                "--dsp bypass needs an authenticated device: pass --auth or "
                "set AIUS_SD_AUTH"
            )
    if args.allow_legacy_evidence and not args.reuse_existing:
        raise ValueError(
            "--allow-legacy-evidence only applies to --reuse-existing"
        )
    if args.allow_legacy_evidence and args.dsp != DSP_MODE_PRODUCTION:
        raise ValueError(
            "--allow-legacy-evidence only applies to production evidence: "
            f"no {args.dsp} row exists below acceptance contract version "
            f"{ACCEPTANCE_CONTRACT_VERSION}"
        )
    if args.warmup_frames < RATE:
        raise ValueError("warm-up must be at least one second")
    if guard_frames < 0 or args.tail_frames < 0:
        raise ValueError("guard and tail frames must be non-negative")
    if guard_frames != default_probe_gap_frames:
        raise ValueError("probe gap must match the formal mode geometry")
    if args.tail_frames < required_guard_frames:
        raise ValueError("tail must retain one complete probe guard")
    if args.status_delay >= args.warmup_frames / RATE:
        raise ValueError("status bracket must begin before the first code")
    return validate_formal_bracket(
        args.mode, args.warmup_frames, guard_frames, args.tail_frames
    )


def finalize_session_evidence(root: Path, session: DspBypassSession) -> None:
    """Land the session's evidence in the manifest, however the row ends.

    Registered before the session context, so it runs after the last restore
    attempt. A row whose restoration never verified is rejected here even when
    the collection itself was clean, and the manifest names the board state.
    """
    session.write_evidence()
    path = root / "manifest.json"
    manifest: dict = {}
    if path.is_file():
        manifest = json.loads(path.read_text(encoding="utf-8"))
    manifest["dsp_bypass"] = session.evidence()
    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, dict):
        artifacts = {}
    artifacts.update(session.artifact_records())
    manifest["artifacts"] = artifacts
    if not session.restoration["verified"]:
        manifest["state"] = "rejected"
        manifest["completed_utc"] = utc_now()
        manifest["rejection"] = session.restore_rejection()
        manifest["board_state"] = session.board_state()
    write_json(path, manifest)


def reanalyze_existing(args: argparse.Namespace) -> int:
    manifest_path = args.output / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    validate_evidence_manifest(
        manifest, args.mode, args.allow_legacy_evidence, args.dsp
    )
    manifest["legacy_evidence_allowed"] = args.allow_legacy_evidence
    previous_summary = json.loads(
        (args.output / "summary.json").read_text(encoding="utf-8")
    )
    if previous_summary["mode"] != args.mode:
        raise RuntimeError(
            "requested mode does not match the existing summary"
        )
    if previous_summary.get("dsp", DSP_MODE_PRODUCTION) != args.dsp:
        raise RuntimeError(
            "requested DSP row does not match the existing summary"
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
    reference_positions = generation["reference_positions"]
    reference_path = (
        args.output / manifest["artifacts"]["native_reference"]["path"]
    )
    reference = np.fromfile(
        reference_path, dtype="<i4"
    ).reshape(-1, CHANNELS)
    input_positions = generation["input_positions"]
    summary = analyze(
        args.mode,
        capture,
        reference,
        reference_positions,
        input_positions,
        None,
        args.dsp,
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
    summary["legacy_evidence_allowed"] = args.allow_legacy_evidence
    write_json(args.output / "summary.json", summary)
    if "schema" in manifest:
        manifest["state"] = "accepted"
        manifest["reanalyzed_utc"] = utc_now()
        manifest["artifacts"]["summary"] = {
            "path": "summary.json",
            "sha256": sha256(args.output / "summary.json"),
        }
        write_json(manifest_path, manifest)
    print(json.dumps(summary, indent=2, sort_keys=True), flush=True)
    return 0


def main() -> int:
    args = build_parser().parse_args()
    if args.guard_frames is None:
        args.guard_frames = mode_probe_gap(args.mode)
    args.output.mkdir(parents=True, exist_ok=True)
    if args.reuse_existing:
        return reanalyze_existing(args)
    validate_arguments(args)
    repository = repository_identity()
    require_clean_harness(repository)
    return run_formal_collection(args, repository)


def run_formal_collection(
    args: argparse.Namespace, repository: dict
) -> int:
    preflight = make_preflight(
        mode=args.mode,
        device=args.device,
        status_url=args.status_url,
        config_url=args.config_url,
        renderer=args.native_renderer,
        period=args.period,
        buffer_size=args.buffer,
        warmup_frames=args.warmup_frames,
        status_delay_seconds=args.status_delay,
        probe_gap_frames=args.guard_frames,
        tail_frames=args.tail_frames,
        command_argv=[sys.executable, *sys.argv],
        versions=tool_versions(args.mode),
        repository=repository,
        started_utc=utc_now(),
        dsp=args.dsp,
        sd_config_url=args.sd_config_url,
    )
    preflight_path = args.output / "preflight.json"
    write_json(preflight_path, preflight)
    effective_config_path, renderer_config_path = capture_effective_config(
        args.config_url, args.output
    )
    effective_model = json.loads(effective_config_path.read_bytes())
    session = None
    if args.dsp == DSP_MODE_BYPASS:
        session = DspBypassSession(
            config_url=args.config_url,
            sd_url=args.sd_config_url,
            token=args.auth,
            root=args.output,
            effective_raw=effective_config_path.read_bytes(),
            poll_seconds=DSP_APPLY_POLL_SECONDS,
            timeout_seconds=DSP_APPLY_TIMEOUT_SECONDS,
        )
        try:
            session.prepare()
        except BaseException as error:
            # Aborting before any mutation: the card file is absent,
            # unparseable, or not what the board is running. Retain exactly
            # what was proven and leave the board untouched.
            session.write_evidence()
            aborted = dict(preflight)
            aborted["state"] = "rejected"
            aborted["completed_utc"] = utc_now()
            aborted["rejection"] = str(error)
            aborted["dsp_bypass"] = session.evidence()
            aborted["artifacts"] = {
                "preflight": artifact_record(preflight_path),
                "effective_dsp_config": artifact_record(
                    effective_config_path
                ),
                "renderer_dsp_config": artifact_record(
                    renderer_config_path
                ),
                **session.artifact_records(),
            }
            write_json(args.output / "manifest.json", aborted)
            raise
    config_paths = {
        "production_effective": effective_config_path,
        "row_effective": effective_config_path,
        "renderer": renderer_config_path,
    }
    with contextlib.ExitStack() as stack:
        if session is not None:
            # LIFO: the session's restore runs first, then this callback
            # lands the outcome in the manifest.
            stack.callback(finalize_session_evidence, args.output, session)
            stack.enter_context(session)
            try:
                session.apply()
                (
                    config_paths["row_effective"],
                    config_paths["renderer"],
                ) = write_effective_config(
                    session.applied_raw, args.output, BYPASS_CONFIG_PREFIX
                )
            except BaseException as error:
                rejected = dict(preflight)
                rejected["state"] = "rejected"
                rejected["completed_utc"] = utc_now()
                rejected["rejection"] = str(error)
                rejected["artifacts"] = {
                    "preflight": artifact_record(preflight_path),
                    "effective_dsp_config": artifact_record(
                        config_paths["production_effective"]
                    ),
                    "renderer_dsp_config": artifact_record(
                        config_paths["renderer"]
                    ),
                    **session.artifact_records(),
                }
                write_json(args.output / "manifest.json", rejected)
                raise
            effective_model = json.loads(session.applied_raw)
        return collect_row(
            args,
            preflight,
            preflight_path,
            config_paths,
            effective_model,
            session,
        )


def collect_row(
    args: argparse.Namespace,
    preflight: dict,
    preflight_path: Path,
    config_paths: dict,
    effective_model: dict,
    session: DspBypassSession | None,
) -> int:
    """Generate, render, collect and analyze one row against the live board."""
    code_frames, _ = mode_geometry(args.mode)
    probe_codes = codes(args.mode)
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
            config_paths["renderer"],
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
            "code_frames": code_frames,
            "probe_gap_frames": args.guard_frames,
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
            config_paths["renderer"],
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
            "code_frames": code_frames,
            "probe_gap_frames": args.guard_frames,
            "warmup_frames": args.warmup_frames,
            "tail_frames": args.tail_frames,
            "source_positions": source_positions,
            "reference_positions": reference_positions,
            "input_positions": input_positions,
            "reference_unit_indices": unit_indices,
            "pa_positions": pa_positions,
            "ffmpeg_encode_argv": ac3_commands(args.output)[0],
            "ffmpeg_wrap_argv": ac3_commands(args.output)[1],
        }

    generation["render_level"] = validate_render_level(
        reference, effective_model
    )
    playback = np.fromfile(carrier_path, dtype="<i2")
    if len(playback) % 2:
        raise RuntimeError("carrier has a partial stereo frame")
    playback = np.ascontiguousarray(playback.reshape(-1, 2))
    artifacts = {
        "preflight": artifact_record(preflight_path),
        "carrier": artifact_record(carrier_path),
        "effective_dsp_config": artifact_record(
            config_paths["production_effective"]
        ),
        "renderer_dsp_config": artifact_record(config_paths["renderer"]),
        "native_reference": artifact_record(
            args.output / "native-reference.s32le"
        ),
        "native_decoded": artifact_record(
            args.output / "native-decoded.s32le"
        ),
        "native_render_manifest": artifact_record(
            args.output / "native-render-manifest.json"
        ),
        "native_render_log": artifact_record(
            args.output / "native-render.log"
        ),
    }
    if session is not None:
        # `effective_dsp_config` stays the session-start production capture;
        # the row itself ran against the applied bypass model.
        artifacts["bypass_effective_dsp_config"] = artifact_record(
            config_paths["row_effective"]
        )
        artifacts.update(session.artifact_records())
    if args.mode == "ac3":
        for key, artifact_path in (
            ("source_raw", args.output / "source-5.1.s16le"),
            ("source_wav", args.output / "source-5.1.wav"),
            ("elementary_stream", args.output / "carrier.ac3"),
            ("ffmpeg_encode_log", args.output / "ffmpeg-encode.log"),
            ("ffmpeg_wrap_log", args.output / "ffmpeg-wrap.log"),
        ):
            artifacts[key] = artifact_record(artifact_path)
    active_source_preflight_path = (
        args.output / "active-source-preflight.json"
    )
    try:
        active_source = preflight_active_source(
            args.device,
            RATE,
            args.period,
            args.buffer,
            args.status_url,
            ACTIVE_SOURCE_PREFLIGHT_MIN_SECONDS,
            ACTIVE_SOURCE_PREFLIGHT_TIMEOUT_SECONDS,
            ACTIVE_SOURCE_PREFLIGHT_POLL_SECONDS,
            MAXIMUM_FOLLOW_ERROR_MILLIHZ,
            artifact_path=active_source_preflight_path,
        )
        write_json(active_source_preflight_path, active_source)
        artifacts["active_source_preflight"] = artifact_record(
            active_source_preflight_path
        )
        if (
            active_source.get("state") != "accepted"
            or active_source.get("mode") not in ACTIVE_SOURCE_ACCEPTED_MODES
        ):
            raise RuntimeError(
                "active-source preflight did not accept the source: "
                f"state={active_source.get('state')!r}, "
                f"mode={active_source.get('mode')!r}"
            )
        if session is not None:
            session.bracket_hash("initial")
        capture, manifest = collect(
            args.device,
            playback,
            RATE,
            args.period,
            args.buffer,
            args.status_url,
            args.status_delay,
        )
        if session is not None:
            session.bracket_hash("final")
    except BaseException as error:
        failed = getattr(error, "aius_collection_manifest", {})
        if not isinstance(failed, dict):
            failed = {}
        active_failure = getattr(
            error, "aius_active_source_manifest", None
        )
        if isinstance(active_failure, dict):
            write_json(active_source_preflight_path, active_failure)
            artifacts["active_source_preflight"] = artifact_record(
                active_source_preflight_path
            )
            failed["active_source_preflight"] = active_failure
        failed.update(preflight)
        failed["state"] = "rejected"
        failed["completed_utc"] = utc_now()
        failed["rejection"] = str(error)
        failed["artifacts"] = artifacts
        failed["generation"] = generation
        if session is not None:
            failed["dsp_bypass"] = session.evidence()
        write_json(args.output / "manifest.json", failed)
        raise
    manifest["active_source_preflight"] = {
        "accepted": active_source["state"] == "accepted",
        "mode": active_source["mode"],
        "elapsed_seconds": active_source["elapsed_seconds"],
        "xrun_recoveries": active_source["xrun_recoveries"],
        "accepted_status": active_source["accepted_status"],
    }
    capture_path = args.output / "capture.s16le"
    capture.tofile(capture_path)
    artifacts["capture"] = artifact_record(capture_path)
    manifest.update(preflight)
    manifest["state"] = "captured"
    manifest["artifacts"] = artifacts
    manifest["generation"] = generation
    if "status_initial" in manifest and "status_final" in manifest:
        manifest["counter_deltas"] = counter_deltas(
            manifest["status_initial"], manifest["status_final"]
        )
    write_json(args.output / "manifest.json", manifest)

    try:
        manifest["status_acceptance"] = validate_status_bracket(
            args.mode,
            manifest["status_initial"],
            manifest["status_final"],
            effective_model,
            preflight["repository"]["expected_build_revision"],
        )
        summary = analyze(
            args.mode,
            capture,
            reference,
            reference_positions,
            input_positions,
            None,
            args.dsp,
        )
        if session is not None:
            # Restore before the row is accepted: an unrestored board is not
            # an accepted result, however clean the measurement was.
            session.restore()
            session.write_evidence()
            manifest["dsp_bypass"] = session.evidence()
            manifest["artifacts"].update(session.artifact_records())
            if not session.restoration["verified"]:
                raise RuntimeError(session.restore_rejection())
    except BaseException as error:
        manifest["state"] = "rejected"
        manifest["completed_utc"] = utc_now()
        manifest["rejection"] = str(error)
        if session is not None:
            manifest["board_state"] = session.board_state()
        write_json(args.output / "manifest.json", manifest)
        raise
    summary["build_revision"] = manifest["status_final"][
        "build_revision"
    ]
    summary["counter_deltas"] = manifest.get("counter_deltas")
    write_json(args.output / "summary.json", summary)
    manifest["state"] = "accepted"
    manifest["completed_utc"] = utc_now()
    manifest["artifacts"]["summary"] = {
        "path": "summary.json",
        "sha256": sha256(args.output / "summary.json"),
    }
    write_json(args.output / "manifest.json", manifest)
    print(json.dumps(summary, indent=2, sort_keys=True), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
