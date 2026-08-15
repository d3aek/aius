#!/usr/bin/env python3
"""Aius H2a: controlled-impedance geometry feasibility for the carrier board.

Computes the trace-width / edge-spacing needed to hit the 90 ohm USB and 100 ohm
Ethernet differential targets on each fabricator stackup in ``stackups.json``,
using the IPC-2141 / Wadell closed-form differential-microstrip model. This is a
FEASIBILITY tool: it decides whether a stackup can carry manufacturable
controlled-impedance pairs at all, and what layer count that argues for. Final
impedance is confirmed against the fabricator's own controlled-impedance service
at H3c.

Model (edge-coupled differential microstrip, IPC-2141A):
    Z0    = (87 / sqrt(er + 1.41)) * ln(5.98*h / (0.8*w + t))     # single-ended
    Zdiff = 2 * Z0 * (1 - 0.48 * exp(-0.96 * s / h))              # differential

  h = dielectric height to the reference plane, w = trace width,
  s = edge-to-edge spacing, t = copper thickness (all mm). Valid roughly for
  0.1 <= w/h <= 2.0; widths outside that band are reported with a caveat because
  the point there is the gross (in)feasibility, not a precise number.

Run:  ./impedance.py [path/to/stackups.json]   (defaults to sibling stackups.json)
Emits a human table to stdout and, with --json, a machine record.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path


def z0_single(w_mm: float, h_mm: float, t_mm: float, er: float) -> float:
    """Single-ended microstrip impedance (IPC-2141A)."""
    return (87.0 / math.sqrt(er + 1.41)) * math.log(5.98 * h_mm / (0.8 * w_mm + t_mm))


def zdiff(w_mm: float, s_mm: float, h_mm: float, t_mm: float, er: float) -> float:
    """Edge-coupled differential-microstrip impedance (IPC-2141A)."""
    return 2.0 * z0_single(w_mm, h_mm, t_mm, er) * (1.0 - 0.48 * math.exp(-0.96 * s_mm / h_mm))


def solve_width(target_ohm: float, s_mm: float, h_mm: float, t_mm: float, er: float) -> float | None:
    """Width (mm) that hits target Zdiff at the given spacing, by bisection.

    Zdiff decreases monotonically as w grows. Returns None if the target is not
    bracketed within a sane width window (0.05 .. 8 mm)."""
    lo, hi = 0.05, 8.0
    z_lo, z_hi = zdiff(lo, s_mm, h_mm, t_mm, er), zdiff(hi, s_mm, h_mm, t_mm, er)
    if not (min(z_lo, z_hi) <= target_ohm <= max(z_lo, z_hi)):
        return None
    for _ in range(80):
        mid = 0.5 * (lo + hi)
        if (zdiff(mid, s_mm, h_mm, t_mm, er) - target_ohm) > 0:
            lo = mid  # need wider to lower Z
        else:
            hi = mid
    return 0.5 * (lo + hi)


def evaluate(stackup: dict, target: dict) -> dict:
    """Feasibility of one target on one stackup."""
    h, t, er = stackup["signal_ref_h_mm"], stackup["copper_t_mm"], stackup["er"]
    min_s, min_w = stackup["min_space_mm"], stackup["min_trace_mm"]
    z = float(target["z_ohm"])

    # Representative spacings: fab minimum, and two looser coupled options.
    spacings = sorted({round(min_s, 4), 0.15, 0.20})
    solutions = []
    for s in spacings:
        w = solve_width(z, s, h, t, er)
        solutions.append({
            "s_mm": s,
            "w_mm": (round(w, 3) if w is not None else None),
            "w_over_h": (round(w / h, 2) if w is not None else None),
            "manufacturable": (w is not None and w >= min_w and w <= 0.60),
        })

    # What a narrow, routing-friendly pair actually measures on this stackup.
    narrow_w, narrow_s = max(min_w, 0.15), max(min_s, 0.15)
    narrow_z = zdiff(narrow_w, narrow_s, h, t, er)

    feasible = any(sol["manufacturable"] for sol in solutions)
    return {
        "target": target["id"],
        "z_target_ohm": z,
        "solutions": solutions,
        "narrow_pair": {
            "w_mm": round(narrow_w, 3), "s_mm": round(narrow_s, 3),
            "zdiff_ohm": round(narrow_z, 1),
            "err_pct": round(100.0 * (narrow_z - z) / z, 1),
        },
        "feasible": feasible,
    }


def run(spec_path: Path) -> dict:
    spec = json.loads(spec_path.read_text())
    by_id = {s["id"]: s for s in spec["stackups"]}

    # Self-test anchor.
    st = spec["self_test"]
    s_stk = by_id[st["stackup"]]
    got = zdiff(st["w_mm"], st["s_mm"], s_stk["signal_ref_h_mm"], s_stk["copper_t_mm"], s_stk["er"])
    anchor = {
        "stackup": st["stackup"], "expect_ohm": st["expect_zdiff_ohm"],
        "got_ohm": round(got, 1), "abs_tol_ohm": st["abs_tol_ohm"],
        "pass": abs(got - st["expect_zdiff_ohm"]) <= st["abs_tol_ohm"],
    }

    results = []
    for stk in spec["stackups"]:
        evals = [evaluate(stk, tgt) for tgt in spec["targets"]]
        results.append({
            "id": stk["id"], "fab": stk["fab"], "layers": stk["layers"],
            "h_mm": stk["signal_ref_h_mm"], "er": stk["er"],
            "min_trace_mm": stk["min_trace_mm"], "min_space_mm": stk["min_space_mm"],
            "ref_plane": stk["ref_plane"],
            "targets": evals,
            "all_feasible": all(e["feasible"] for e in evals),
        })
    return {"anchor": anchor, "stackups": results}


def print_human(report: dict) -> None:
    a = report["anchor"]
    status = "PASS" if a["pass"] else "FAIL"
    print(f"Self-test anchor ({a['stackup']}): closed-form {a['got_ohm']} ohm "
          f"vs expected {a['expect_ohm']} ohm (tol +/-{a['abs_tol_ohm']}) -> {status}\n")

    for stk in report["stackups"]:
        verdict = "FEASIBLE" if stk["all_feasible"] else "NOT FEASIBLE"
        print(f"== {stk['id']}  ({stk['fab']} {stk['layers']}L, h={stk['h_mm']} mm, "
              f"er={stk['er']}, min trace/space {stk['min_trace_mm']}/{stk['min_space_mm']} mm)"
              f"  -> {verdict}")
        print(f"   reference plane: {stk['ref_plane']}")
        for e in stk["targets"]:
            print(f"   {e['target'].upper()} {int(e['z_target_ohm'])} ohm diff:")
            for sol in e["solutions"]:
                if sol["w_mm"] is None:
                    print(f"      s={sol['s_mm']:.3f} mm  ->  no width in 0.05..8 mm hits target")
                else:
                    mark = "ok" if sol["manufacturable"] else "IMPRACTICAL"
                    print(f"      s={sol['s_mm']:.3f} mm  ->  w={sol['w_mm']:.3f} mm "
                          f"(w/h={sol['w_over_h']})  [{mark}]")
            np = e["narrow_pair"]
            print(f"      narrow routable pair w={np['w_mm']}/s={np['s_mm']} mm measures "
                  f"{np['zdiff_ohm']} ohm ({np['err_pct']:+.0f}% vs target)")
        print()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("spec", nargs="?", default=str(Path(__file__).with_name("stackups.json")))
    ap.add_argument("--json", action="store_true", help="emit the machine record instead of the table")
    args = ap.parse_args()

    report = run(Path(args.spec))
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print_human(report)
    return 0 if report["anchor"]["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
