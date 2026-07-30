#!/usr/bin/env python3
"""Repeat one linked latency row and aggregate run-to-run phase."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

import numpy as np


def write_json(path: Path, value: object) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def distribution(values: list[float]) -> dict:
    samples = np.asarray(values, dtype=np.float64)
    return {
        "min": float(np.min(samples)),
        "median": float(np.median(samples)),
        "p95": float(np.percentile(samples, 95)),
        "max": float(np.max(samples)),
        "spread": float(np.max(samples) - np.min(samples)),
    }


def latency_by_initial_tx_level(trials: list[dict]) -> dict:
    groups: dict[int, list[float]] = {}
    for trial in trials:
        level = int(
            trial["manifest"]["status_initial"]["clock"]["tx_level_frames"]
        )
        groups.setdefault(level, []).append(
            float(trial["summary"]["raw_latency"]["median_frames"])
        )
    return {
        str(level): {
            "trials": len(values),
            "latency_frames": distribution(values),
        }
        for level, values in sorted(groups.items())
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("pcm", "ac3"), required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--period", type=int, default=256)
    parser.add_argument("--buffer", type=int, default=4096)
    parser.add_argument("--device", default="hw:ICUSBAUDIO7D,0")
    parser.add_argument("--settle-between", type=float, default=2.0)
    parser.add_argument(
        "--reuse-existing",
        action="store_true",
        help="aggregate existing trial directories without collecting again",
    )
    parser.add_argument(
        "--status-url", default="http://192.0.2.66/api/status"
    )
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    bench = Path(__file__).with_name("bench.py")
    trials = []
    failures = []

    for number in range(1, args.trials + 1):
        root = args.output / f"trial-{number}"
        root.mkdir(parents=True, exist_ok=True)
        if not args.reuse_existing:
            command = [
                sys.executable, str(bench),
                "--mode", args.mode,
                "--output", str(root),
                "--period", str(args.period),
                "--buffer", str(args.buffer),
                "--device", args.device,
                "--status-url", args.status_url,
            ]
            result = subprocess.run(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            (root / "runner.log").write_bytes(result.stdout)
            if result.returncode != 0:
                failures.append({
                    "trial": number,
                    "returncode": result.returncode,
                    "log": str(root / "runner.log"),
                })
                print(
                    f"trial {number}: failed ({result.returncode})",
                    flush=True,
                )
                if number != args.trials and not args.reuse_existing:
                    time.sleep(args.settle_between)
                continue
        summary_path = root / "summary.json"
        manifest_path = root / "manifest.json"
        if not summary_path.exists() or not manifest_path.exists():
            failures.append({
                "trial": number,
                "returncode": None,
                "log": str(root / "runner.log"),
                "reason": "missing summary.json or manifest.json",
            })
            print(f"trial {number}: missing artifacts", flush=True)
            continue
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        trials.append({
            "number": number,
            "summary": summary,
            "manifest": manifest,
        })
        print(
            f"trial {number}: "
            f"{summary['raw_latency']['median_frames']:.0f} frames, "
            f"{summary['raw_latency']['median_ms']:.3f} ms",
            flush=True,
        )
        if number != args.trials and not args.reuse_existing:
            time.sleep(args.settle_between)

    if not trials:
        write_json(args.output / "summary.json", {
            "mode": args.mode,
            "period_frames": args.period,
            "buffer_frames": args.buffer,
            "trials_requested": args.trials,
            "trials_passed": 0,
            "failures": failures,
        })
        return 1

    counter_clean_trials = [
        trial for trial in trials
        if not any(
            value != 0
            for value in trial["summary"]["counter_deltas"].values()
        )
    ]
    aggregation_trials = (
        counter_clean_trials if counter_clean_trials else trials
    )
    latency_frames = [
        float(trial["summary"]["raw_latency"]["median_frames"])
        for trial in aggregation_trials
    ]
    latency_ms = [
        float(trial["summary"]["raw_latency"]["median_ms"])
        for trial in aggregation_trials
    ]
    aggregate = {
        "mode": args.mode,
        "period_frames": args.period,
        "buffer_frames": args.buffer,
        "trials_requested": args.trials,
        "trials_passed": len(trials),
        "trials_counter_clean": len(counter_clean_trials),
        "distribution_basis": (
            "counter_clean_trials"
            if counter_clean_trials
            else "all_passed_no_counter_clean_trials"
        ),
        "failures": failures,
        "build_revisions": sorted({
            trial["summary"]["build_revision"] for trial in trials
        }),
        "accepted_occurrences": sum(
            int(trial["summary"]["accepted_occurrences"])
            for trial in aggregation_trials
        ),
        "latency_frames": distribution(latency_frames),
        "latency_ms": distribution(latency_ms),
        "latency_by_initial_tx_level_frames": (
            latency_by_initial_tx_level(aggregation_trials)
        ),
        "max_abs_follow_error_millihz": max(
            abs(
                int(status["clock"]["output_rate_millihz"])
                - int(status["clock"]["carrier_rate_millihz"])
            )
            for trial in aggregation_trials
            for status in (
                trial["manifest"]["status_initial"],
                trial["manifest"]["status_final"],
            )
        ),
        "within_trial_residual_max_abs_frames": max(
            float(trial["summary"]["affine"]["residual_max_abs_frames"])
            for trial in aggregation_trials
        ),
        "correlation_min": min(
            float(trial["summary"]["correlation_min"])
            for trial in aggregation_trials
        ),
        "nonzero_counter_deltas": [{
            "trial": trial["number"],
            "deltas": {
                key: value for key, value
                in trial["summary"]["counter_deltas"].items() if value != 0
            },
        } for trial in trials
            if any(
                value != 0
                for value in trial["summary"]["counter_deltas"].values()
            )],
        "trial_context": [{
            "trial": trial["number"],
            "counter_clean": trial in counter_clean_trials,
            "latency_frames": float(
                trial["summary"]["raw_latency"]["median_frames"]
            ),
            "latency_ms": float(
                trial["summary"]["raw_latency"]["median_ms"]
            ),
            "startup_tx_level_frames": int(
                trial["manifest"].get(
                    "status_startup",
                    trial["manifest"]["status_initial"],
                )["clock"][
                    "tx_level_frames"
                ]
            ),
            "initial_tx_level_frames": int(
                trial["manifest"]["status_initial"]["clock"][
                    "tx_level_frames"
                ]
            ),
            "final_tx_level_frames": int(
                trial["manifest"]["status_final"]["clock"][
                    "tx_level_frames"
                ]
            ),
            "initial_follow_error_millihz": (
                int(
                    trial["manifest"]["status_initial"]["clock"][
                        "output_rate_millihz"
                    ]
                )
                - int(
                    trial["manifest"]["status_initial"]["clock"][
                        "carrier_rate_millihz"
                    ]
                )
            ),
            "final_follow_error_millihz": (
                int(
                    trial["manifest"]["status_final"]["clock"][
                        "output_rate_millihz"
                    ]
                )
                - int(
                    trial["manifest"]["status_final"]["clock"][
                        "carrier_rate_millihz"
                    ]
                )
            ),
        } for trial in trials],
    }
    if args.mode == "ac3":
        tail_ms = [
            float(trial["summary"]["access_unit_tail"]["median_ms"])
            for trial in aggregation_trials
        ]
        aggregate["access_unit_tail_ms"] = distribution(tail_ms)
    write_json(args.output / "summary.json", aggregate)
    print(json.dumps(aggregate, indent=2, sort_keys=True), flush=True)
    if not counter_clean_trials:
        return 3
    return 0 if len(trials) == args.trials else 2


if __name__ == "__main__":
    raise SystemExit(main())
