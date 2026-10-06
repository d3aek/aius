#!/usr/bin/env python3
"""Analyse an aius_status_poll.py JSONL for the Phase 3 fill-sensor capture.

Reads {"host_utc", "status"} lines and reports the two pass criteria and the
"Recorded here" inputs of docs/plans/aius-sample-rate-following.md
(Device proof and noise capture):

  * resolution: fill_level_q4 continuity across each 128-frame step of the
    tx_level_frames floor, and its slope against the underrun cadence;
  * no audio change: deltas of the harmful counters;
  * steady fill after each gate open and its spread; window-to-window and
    sub-window noise (de-duplicated by fill_windows; polled vs unpolled by
    sample interval); PCM and compressed floors, their difference and the
    transition shape; exclusion and torn-read rates per stream class.

Floor steps that toggle 0/128 after the floor reaches 0 are expected (near an
empty ring), not a defect; raise --step-persist to thin them.

Without the clock.fill_* keys (images before Phase 3) the fill parts report
"absent" and the tx_level floor, underrun and counter parts still run.

Stream class is pipeline_diag.class (0 unknown, 1 PCM, 2 IEC 61937
compressed, e.g. AC-3), falling back to the top-level format when absent.
A gate open is tx_gate.open 0 -> 1; a relock is each such edge.

Usage:
  aius_fill_capture_analysis.py session.jsonl [more.jsonl ...]
  aius_fill_capture_analysis.py session.jsonl --format json --output out.json
  aius_fill_capture_analysis.py --self-test

Assumptions: fill_windows, fill_subwindows_excluded and fill_torn_reads are
cumulative counters (a decrease is treated as a reset and skipped);
fill_level_q4 is in 1/16 frame; an unpolled pair is a sample interval of
--unpolled-min-interval or more.
"""

from __future__ import annotations

import argparse
import datetime
import json
import math
import statistics
import sys
import tempfile
import types
import os

CLASS_NAMES = {0: "unknown", 1: "pcm"}
# IEC 61937 Pc of the last decoded burst (pipeline_diag.codec_pc); 0 before any burst.
CODEC_NAMES = {1: "ac3", 11: "dts", 12: "dts", 13: "dts", 21: "eac3"}  # 12/13: DTS-1024/2048
HARMFUL = (
    ("clock", "underrun_episode_count"),
    ("clock", "tx_underrun_frames"),
    ("clock", "governor_trims"),
    ("clock", "governor_trimmed_frames"),
    ("clock", "tx_missed_fills"),
    ("clock", "tx_missed_fill_frames"),
    ("clock", "clock_faults"),
    ("clock", "structural_replans"),
    ("clock", "mismatch_blocks"),
    ("clock", "unsupported_blocks"),
    ("pipeline_diag", "crc_rejects"),
    ("audio_diag", "au_abandons"),
    ("overruns",),
)
FILL_KEYS = ("fill_level_q4", "fill_subwindow_spread_frames", "fill_windows",
             "fill_subwindows_excluded", "fill_torn_reads")


def dig(doc, path):
    for key in path:
        if not isinstance(doc, dict):
            return None
        doc = doc.get(key)
    return doc


def num(value):
    return value if isinstance(value, (int, float)) and not isinstance(value, bool) else None


def rnd(value, digits=3):
    return None if value is None else round(value, digits)


def summary(values):
    values = sorted(v for v in values if v is not None)
    if not values:
        return {"n": 0}
    return {
        "n": len(values),
        "min": rnd(values[0]),
        "median": rnd(statistics.median(values)),
        "mean": rnd(statistics.fmean(values)),
        "p95": rnd(values[min(len(values) - 1, int(0.95 * len(values)))]),
        "max": rnd(values[-1]),
        "stdev": rnd(statistics.pstdev(values)) if len(values) > 1 else 0.0,
    }


def fit(points):
    """Least-squares (slope, value at mean t, mean t) over (t, y) points."""
    if len(points) < 2:
        return None
    mt = statistics.fmean(p[0] for p in points)
    my = statistics.fmean(p[1] for p in points)
    var = sum((p[0] - mt) ** 2 for p in points)
    if var == 0:
        return None
    slope = sum((p[0] - mt) * (p[1] - my) for p in points) / var
    return slope, my, mt


def load(paths):
    records = []
    for path in paths:
        with open(path, encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    raw = json.loads(line)
                    when = datetime.datetime.fromisoformat(raw["host_utc"])
                    status = raw["status"]
                except (ValueError, KeyError, TypeError):
                    continue
                if not isinstance(status, dict):
                    continue
                records.append(Rec(when.timestamp(), raw["host_utc"], status))
    records.sort(key=lambda r: r.t)
    for a, b in zip(records, records[1:]):
        b.reset = any(x is not None and y is not None and x != y for x, y in zip(a.trims, b.trims)) \
            or (a.windows is not None and b.windows is not None and b.windows < a.windows)
        b.underrun = a.open and b.open and (
            (a.ep is not None and b.ep is not None and b.ep > a.ep)
            or (a.under_frames is not None and b.under_frames is not None and b.under_frames > a.under_frames))
    for u in (r for r in records if r.underrun):
        for r in records:
            if abs(r.t - u.t) <= 3:
                r.near_underrun = True
    return records


class Rec:
    def __init__(self, t, stamp, status):
        self.t = t
        self.stamp = stamp
        self.status = status
        clock = status.get("clock") if isinstance(status.get("clock"), dict) else {}
        self.clock = clock
        self.open = bool(dig(status, ("tx_gate", "open")))
        cls = dig(status, ("pipeline_diag", "class"))
        if cls is None:
            cls = status.get("format")
        if cls == 2:
            self.cls = CODEC_NAMES.get(dig(status, ("pipeline_diag", "codec_pc")), "compressed-unknown")
        else:
            self.cls = CLASS_NAMES.get(cls, f"class{cls}") if cls is not None else "unknown"
        q4 = num(clock.get("fill_level_q4"))
        self.fill = q4 / 16.0 if q4 is not None else None
        self.level = num(clock.get("tx_level_frames"))
        self.windows = num(clock.get("fill_windows"))
        self.spread = num(clock.get("fill_subwindow_spread_frames"))
        self.excluded = num(clock.get("fill_subwindows_excluded"))
        self.torn = num(clock.get("fill_torn_reads"))
        self.has_fill = any(k in clock for k in FILL_KEYS)
        # Counters whose change resets the fill window/level (governor and gate-open trims, missed fills).
        self.trims = tuple(num(clock.get(k)) for k in ("governor_trims", "gate_open_trims", "tx_missed_fills"))
        self.reset = False  # set by load(): discontinuity between the previous record and this one
        self.ep = num(clock.get("underrun_episode_count"))
        self.under_frames = num(clock.get("tx_underrun_frames"))
        self.underrun = False  # set by load(): an underrun landed since the previous (open) record
        self.near_underrun = False  # set by load(): within 3 s of an underrun (blended window)


def hms(t):
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%H:%M:%S")


def episodes(records, gap, settle):
    """Gate-open runs split on class change or sample gap."""
    out = []
    cur = None
    prev = None
    opens = 0
    for r in records:
        if prev is not None and r.t - prev.t > gap:
            cur = None
            prev = None
        if r.open:
            fresh = prev is None or not prev.open
            if fresh:
                opens += 1
            if cur is None or fresh or r.cls != cur["class"]:
                cur = {"class": r.cls, "gate_open_index": opens,
                       "after_gate_open": fresh, "start": r.t, "recs": []}
                out.append(cur)
            cur["recs"].append(r)
        else:
            cur = None
        prev = r
    for e in out:
        e["steady"] = [r for r in e["recs"] if r.t >= e["recs"][0].t + settle] \
            if e["after_gate_open"] else e["recs"]
    return out


def counters(records):
    first, last = records[0], records[-1]
    out = {}
    for path in HARMFUL:
        a, b = num(dig(first.status, path)), num(dig(last.status, path))
        key = ".".join(path)
        if a is None or b is None:
            out[key] = None
            continue
        resets = 0
        total = 0
        prev = None
        for r in records:
            v = num(dig(r.status, path))
            if v is None:
                continue
            if prev is not None:
                total += v - prev if v >= prev else 0
                resets += v < prev
            prev = v
        out[key] = {"first": a, "last": b, "delta": total, "resets": resets}
    return out


def underrun_rate(records):
    """Underrun frames/s and episodes/s over gate-open consecutive pairs."""
    frames = episodes_n = dt = 0.0
    for a, b in zip(records, records[1:]):
        if not (a.open and b.open):
            continue
        fa, fb = num(a.clock.get("tx_underrun_frames")), num(b.clock.get("tx_underrun_frames"))
        ea, eb = num(a.clock.get("underrun_episode_count")), num(b.clock.get("underrun_episode_count"))
        if fa is None or fb is None or fb < fa:
            continue
        frames += fb - fa
        episodes_n += (eb - ea) if ea is not None and eb is not None and eb >= ea else 0
        dt += b.t - a.t
    if dt <= 0:
        return None
    return {"frames_per_s": rnd(frames / dt, 4), "episodes_per_s": rnd(episodes_n / dt, 5),
            "seconds": rnd(dt, 1)}


def floor_steps(records, args, eps):
    """Steps of the tx_level_frames floor (min per bin) within gate-open runs."""
    bins = {}
    settling = [(e["start"], e["start"] + args.settle) for e in eps if e["after_gate_open"]]
    for r in records:
        if r.open and r.level is not None and not any(a <= r.t < b for a, b in settling):
            bins.setdefault(int(r.t // args.floor_bin), []).append(r)
    keys = sorted(bins)
    floor = {k: min(r.level for r in bins[k]) for k in keys}
    steps = []
    for i in range(1, len(keys)):
        a, b = keys[i - 1], keys[i]
        if b != a + 1:
            continue
        delta = floor[b] - floor[a]
        if abs(delta) < args.step_min_frames:
            continue
        held = [k for k in range(b, b + args.step_persist) if floor.get(k) == floor[b]]
        if len(held) < args.step_persist:
            continue  # not persistent: flicker, not a step
        steps.append({"t": b * args.floor_bin, "from": floor[a], "to": floor[b], "delta": delta})
    return steps


def continuity_bounds(records, t0, gap):
    """(lo, hi): fits around t0 use samples with lo < t <= (< hi); cut at gate closes,
    trim/reset records and polling gaps, where the fill level is discontinuous."""
    gaps = [q.t - 1e-6 for p, q in zip(records, records[1:]) if q.t - p.t > gap]
    lo = max([r.t for r in records if r.t <= t0 and not r.open]
             + [r.t - 1e-6 for r in records if r.t <= t0 and (r.reset or r.underrun)]
             + [g for g in gaps if g <= t0], default=-math.inf)
    hi = min([r.t for r in records if r.t > t0 and (not r.open or r.reset or r.underrun)]
             + [g + 1e-6 for g in gaps if g > t0], default=math.inf)
    return lo, hi


def underrun_jumps(records, n=3):
    """Fill jump at each underrun: mean of up to n valid fills just after minus just before.

    An underrun plays 128 frames of silence while the producer keeps writing, so the
    occupancy genuinely jumps (about +120 frames); that is not a sensor discontinuity."""
    out = []
    for i, u in enumerate(records):
        if not u.underrun:
            continue
        row = {"utc": hms(u.t), "t": u.t}
        # Window-aware: the window holding the episode is blended (pre- and post-underrun
        # sub-window minima), so compare the last window published before it with the first after.
        k = next((k for k in range(max(i - 1, 1), len(records))
                  if records[k].t - u.t > 10 or not records[k].open or records[k].reset
                  or (records[k].excluded is not None and records[k - 1].excluded is not None
                      and records[k].excluded > records[k - 1].excluded
                      and records[k].t >= u.t - 5)
                  ), None)
        if k is not None and records[k].t - u.t <= 10 and records[k].open and not records[k].reset \
                and records[k].windows is not None and records[k].fill is not None:
            wb = records[k].windows
            prior = next((records[m] for m in range(k - 1, -1, -1)
                          if records[m].open and not records[m].reset and records[m].windows is not None
                          and records[m].windows < wb and records[m].fill is not None
                          and u.t - records[m].t <= 30), None)
            later = next((records[m] for m in range(k + 1, len(records))
                          if records[m].open and records[m].windows is not None and records[m].windows > wb
                          and records[m].fill is not None and records[m].t - u.t <= 30), None)
            if prior is not None and later is not None:
                row["blended_window_fill"] = rnd(records[k].fill, 2)
                row["before_window_fill"] = rnd(prior.fill, 2)
                row["after_window_fill"] = rnd(later.fill, 2)
                row["fill_jump_frames"] = rnd(later.fill - prior.fill, 2)
                row["method"] = "windows"
                out.append(row)
                continue
        before, after = [], []
        j = i - 1
        while j >= 0 and records[j].open and not records[j].reset and len(before) < n \
                and u.t - records[j].t <= 10:
            if records[j].fill is not None:
                before.append(records[j].fill)
            j -= 1
        j = i
        while j < len(records) and records[j].open and len(after) < n and records[j].t - u.t <= 10 \
                and not (j > i and (records[j].underrun or records[j].reset)):
            if records[j].fill is not None:
                after.append(records[j].fill)
            j += 1
        if before and after:
            row["fill_jump_frames"] = rnd(statistics.fmean(after) - statistics.fmean(before), 2)
            row["method"] = "records (no window evidence; may include the blended window)"
        out.append(row)
    return out


def episode_free_slope(win, args):
    """Pooled fill slope over spans free of underruns, resets, closes and gaps (weighted by points).

    `win` carries the closed records too: a gate close (a closed record, or a gate-open pad
    between two open polls) ends the span, so a relock's re-based floor is never fitted as
    drift (the 2026-10-05 +0.38 frames/s artefact, aius-sample-rate-following Phase 4)."""
    spans, cur, prev = [], [], None
    for r in win:
        if prev is not None and (not r.open or not prev.open or r.underrun or r.reset
                                 or r.t - prev.t > args.gap
                                 or r.clock.get("gate_open_padded_frames")
                                 != prev.clock.get("gate_open_padded_frames")):
            spans.append(cur)
            cur = []
        if r.open and r.fill is not None:
            cur.append((r.t, r.fill))
        prev = r
    spans.append(cur)
    fits = [(f[0], len(s)) for s in spans if len(s) >= 10 for f in [fit(s)] if f]
    if not fits:
        return None
    return sum(sl * n for sl, n in fits) / sum(n for _, n in fits)


def step_report(records, steps, args, have_fill, cadence=None):
    out = []
    jumps = underrun_jumps(records) if have_fill else []
    for s in steps:
        t0 = s["t"]
        win = [r for r in records if r.open and t0 - args.step_window <= r.t <= t0 + args.step_window]
        rate = underrun_rate(win)
        row = {"utc": hms(t0), "floor_from": s["from"], "floor_to": s["to"],
               "underrun": rate}
        if have_fill:
            episodes = [r.t for r in win if r.underrun]
            lo, hi = continuity_bounds(records, t0, args.gap)  # trims, relocks, underruns cut the fits
            pts = [(r.t, r.fill) for r in win if r.fill is not None and lo < r.t < hi]
            pre = fit([p for p in pts if p[0] < t0])
            post = fit([p for p in pts if p[0] >= t0])
            slope = episode_free_slope([r for r in records
                                        if t0 - args.step_window <= r.t <= t0 + args.step_window], args)
            if episodes:
                row["straddles_underrun"] = True  # occupancy jumps at an underrun: no continuity verdict
                row["underrun_utcs"] = [hms(t) for t in episodes]
                row["underrun_jumps"] = [{k: v for k, v in j.items() if k != "t"}
                                         for j in jumps if t0 - args.step_window <= j["t"] <= t0 + args.step_window]
            elif pre and post:
                at_pre = pre[1] + pre[0] * (t0 - pre[2])
                at_post = post[1] + post[0] * (t0 - post[2])
                jump = at_post - at_pre
                row.update({"fill_jump_frames": rnd(jump, 2),
                            "continuous": abs(jump) <= args.continuity_frames})
            else:
                row["fill"] = "insufficient valid samples around step"
            if slope is not None:
                row["fill_slope_frames_per_s_episode_free"] = rnd(slope, 4)
                if cadence:
                    row["slope_matches_underrun"] = abs(abs(slope) - cadence) <= max(0.1, 0.5 * cadence)
        out.append(row)
    return out


def noise(eps, args):
    """Window-to-window sigma and sub-window spread, polled vs unpolled."""
    groups = {}

    def group(cls, mode):
        return groups.setdefault((cls, mode), {"pairs": [], "levels": [], "spread": []})

    for e in eps:
        recs = e["steady"]
        prev = None
        for r in recs:
            if r.reset or r.underrun or r.fill is None or r.near_underrun:
                prev = None  # a trim, underrun (occupancy jump), missed fill or invalid window breaks the chain
            if r.near_underrun:
                continue  # the episode's window is blended; never a chain end
            if r.fill is None or r.windows is None:
                continue
            if prev is not None and r.windows != prev.windows:
                dt = r.t - prev.t
                lag = r.windows - prev.windows
                if dt <= args.gap and lag > 0:
                    mode = "unpolled" if dt >= args.unpolled_min_interval else "polled"
                    group(e["class"], mode)["pairs"].append((r.fill - prev.fill, lag))
            if prev is None or r.windows != prev.windows:
                prev = r
    # levels and spread, de-duplicated by window, mode by median interval
    for e in eps:
        recs = [r for r in e["steady"] if r.fill is not None and r.windows is not None]
        uniq = []
        for r in recs:
            if not uniq or r.windows != uniq[-1].windows:
                uniq.append(r)
        if len(uniq) < 2:
            continue
        med = statistics.median(b.t - a.t for a, b in zip(uniq, uniq[1:]))
        mode = "unpolled" if med >= args.unpolled_min_interval else "polled"
        g = group(e["class"], mode)
        g["levels"].extend(r.fill for r in uniq)
        g["spread"].extend(r.spread for r in uniq if r.spread is not None)
    out = {}
    for (cls, mode), g in sorted(groups.items()):
        d1 = [d for d, lag in g["pairs"] if lag == 1]
        multi = [(d, lag) for d, lag in g["pairs"] if lag > 1]
        dn = [d / math.sqrt(lag) for d, lag in multi]
        dw = [d for d, _ in multi]
        lags = {}
        for _, lag in g["pairs"]:
            lags[str(lag)] = lags.get(str(lag), 0) + 1
        out[f"{cls}/{mode}"] = {
            "windows": len(g["levels"]),
            # consecutive windows only (fill_windows lag 1)
            "window_to_window_sigma_frames": rnd(statistics.pstdev(d1) / math.sqrt(2), 4) if len(d1) > 1 else None,
            "window_diff_pairs_lag1": len(d1),
            "lag_distribution": dict(sorted(lags.items(), key=lambda kv: int(kv[0]))),
            # not window-to-window: each pair diff divided by sqrt(lag), then /sqrt(2)
            # lag>1 pairs only; the two figures bracket the noise model, neither is window-to-window:
            # white window noise (diff does not grow with lag): mean-removed stdev / sqrt 2
            "lag_gt1_white_sigma_frames": rnd(statistics.pstdev(dw) / math.sqrt(2), 4) if len(dw) > 1 else None,
            # random-walk noise (diff variance grows with lag): diff / sqrt(lag), then / sqrt 2
            "lag_gt1_random_walk_sigma_frames": rnd(statistics.pstdev(dn) / math.sqrt(2), 4) if len(dn) > 1 else None,
            "raw_level_stdev_frames": rnd(statistics.pstdev(g["levels"]), 4) if len(g["levels"]) > 1 else None,
            "subwindow_spread_frames": summary(g["spread"]),
        }
    return out


def rates(eps):
    """Exclusion and torn-read rates per class from counter deltas."""
    acc = {}
    for e in eps:
        for a, b in zip(e["recs"], e["recs"][1:]):
            if None in (a.windows, b.windows, a.excluded, b.excluded, a.torn, b.torn):
                continue
            dw, dx, dt_ = b.windows - a.windows, b.excluded - a.excluded, b.torn - a.torn
            if dw < 0 or dx < 0 or dt_ < 0:
                continue  # reset
            g = acc.setdefault(e["class"], [0, 0, 0, 0.0])
            g[0] += dw
            g[1] += dx
            g[2] += dt_
            g[3] += b.t - a.t
    return {cls: {"windows": g[0], "excluded": g[1], "torn_reads": g[2], "seconds": rnd(g[3], 1),
                  "excluded_per_window": rnd(g[1] / g[0], 5) if g[0] else None,
                  "torn_per_window": rnd(g[2] / g[0], 5) if g[0] else None,
                  "excluded_per_s": rnd(g[1] / g[3], 5) if g[3] else None,
                  "torn_per_s": rnd(g[2] / g[3], 5) if g[3] else None}
            for cls, g in sorted(acc.items())}


def transitions(records, args):
    out = []
    offsets = (-60, -30, -10, -3, -1, 0, 1, 3, 10, 30, 60, 120)
    open_recs = [r for r in records if r.open and r.cls not in ("unknown", "compressed-unknown")]
    for a, b in zip(open_recs, open_recs[1:]):
        if a.cls == b.cls or "compressed-unknown" in (a.cls, b.cls):
            continue  # unknown = codec_pc not yet latched or absent
        via_close = any(not r.open for r in records if a.t <= r.t <= b.t)
        t0 = b.t
        W = args.step_window
        # Continuity bounds: the last close/reset before t0 and the first after it.
        via_gap = (b.t - a.t) > args.gap  # polling dropout: a relock may have gone unobserved
        lo, hi = continuity_bounds(records, t0, args.gap)
        win = [r for r in records if r.open and t0 - W <= r.t <= t0 + W]
        pre = [r for r in win if r.t < t0 and r.t > lo]
        after = [r for r in win if r.t >= t0 and r.t < hi]
        # Post fit starts one new fill window after t0, not at --settle.
        post = [r for r in after if r.t > t0 and (r.windows > b.windows if r.windows is not None
                                                  and b.windows is not None else True)]
        via_close_any = via_close or via_gap
        row = {"utc": hms(t0), "from": a.cls, "to": b.cls, "via_gate_close": via_close, "via_gap": via_gap,
               "tx_level_floor_before": min((r.level for r in pre if r.level is not None), default=None),
               "tx_level_floor_after": min((r.level for r in after if r.level is not None), default=None)}
        if any(r.fill is not None for r in win):
            fa = fit([(r.t, r.fill) for r in pre if r.fill is not None])
            fb = fit([(r.t, r.fill) for r in post if r.fill is not None])
            if fa and fb:
                at_a = fa[1] + fa[0] * (t0 - fa[2])
                at_b = fb[1] + fb[0] * (t0 - fb[2])
                row["fill_pre_at_t0"] = rnd(at_a, 2)
                row["fill_post_at_t0"] = rnd(at_b, 2)
                row["fill_slopes_pre_post_frames_per_s"] = [rnd(fa[0], 4), rnd(fb[0], 4)]
                # Headline: drift-free class difference, both fits evaluated at t0.
                row["fill_difference_frames"] = rnd(at_b - at_a, 2)
                outside = [r.t for r in after if r.fill is not None
                           and abs(r.fill - (fb[1] + fb[0] * (r.t - fb[2]))) > args.continuity_frames]
                row["seconds_to_settle"] = rnd(max(outside) - t0, 1) if outside else 0.0
            elif via_close_any:
                row["fill_difference_note"] = ("relock between classes: the gate-open trim resets the fill "
                                               "level, so no class difference is derivable; see steady_fill")
            else:
                row["fill_difference_note"] = "insufficient valid fill samples either side"
            shape = {}
            for off in offsets:
                near = min((r for r in win if r.fill is not None), key=lambda r: abs(r.t - (t0 + off)), default=None)
                if near is not None and abs(near.t - (t0 + off)) <= max(2.0, abs(off) * 0.5):
                    shape[str(off)] = rnd(near.fill, 2)
            row["fill_at_offset_s"] = shape
        out.append(row)
    return out


def floors(eps, records, args, have_fill):
    out = {}
    for cls in sorted({e["class"] for e in eps}):
        recs = [r for e in eps if e["class"] == cls for r in e["steady"]]
        row = {"samples": len(recs),
               "tx_level_floor_frames": min((r.level for r in recs if r.level is not None), default=None)}
        if have_fill:
            row["fill_median_frames"] = rnd(statistics.median([r.fill for r in recs if r.fill is not None]), 2) \
                if any(r.fill is not None for r in recs) else None
            row["fill_min_frames"] = rnd(min((r.fill for r in recs if r.fill is not None), default=None), 2)
        out[cls] = row
    if "pcm" in out:
        # Cross-time medians confound class with drift (~0.23 frames/s) and per-relock level;
        # the drift-free difference is transitions[].fill_difference_frames.
        a = out["pcm"]
        diff = {}
        for cls, b in list(out.items()):
            if cls in ("pcm", "unknown"):
                continue
            row = {}
            if a["tx_level_floor_frames"] is not None and b["tx_level_floor_frames"] is not None:
                row["tx_level_floor"] = b["tx_level_floor_frames"] - a["tx_level_floor_frames"]
            if have_fill and a.get("fill_median_frames") is not None and b.get("fill_median_frames") is not None:
                row["fill_median"] = rnd(b["fill_median_frames"] - a["fill_median_frames"], 2)
            diff[f"{cls}_minus_pcm"] = row
        if diff:
            out["difference_drift_confounded"] = diff
    return out


def steady_levels(eps, args):
    rows = []
    for e in eps:
        if not e["after_gate_open"]:
            continue
        t0 = e["recs"][0].t
        vals = [r.fill for r in e["recs"]
                if r.fill is not None and t0 + args.settle <= r.t <= t0 + args.settle + args.steady_span]
        rows.append({"utc": hms(t0), "class": e["class"], "seconds_open": rnd(e["recs"][-1].t - t0, 1),
                     "steady_fill_frames": rnd(statistics.median(vals), 2) if vals else None,
                     "samples": len(vals)})
    levels = [r["steady_fill_frames"] for r in rows if r["steady_fill_frames"] is not None]
    spread = {"n": len(levels)}
    if levels:
        spread.update({"min": min(levels), "max": max(levels), "range": rnd(max(levels) - min(levels), 2),
                       "stdev": rnd(statistics.pstdev(levels), 3) if len(levels) > 1 else 0.0})
    return {"per_gate_open": rows, "spread": spread}


def analyse(records, args):
    if not records:
        return {"error": "no records"}
    have_fill = any(r.has_fill and r.fill is not None for r in records)
    eps = episodes(records, args.gap, args.settle)
    steps = floor_steps(records, args, eps)
    report = {
        "session": {"first_utc": records[0].stamp, "last_utc": records[-1].stamp,
                    "seconds": rnd(records[-1].t - records[0].t, 1), "samples": len(records),
                    "median_interval_s": rnd(statistics.median(b.t - a.t for a, b in zip(records, records[1:])), 2)
                    if len(records) > 1 else None,
                    "gate_opens": sum(1 for e in eps if e["after_gate_open"]),
                    "classes_seen": sorted({r.cls for r in records if r.open}),
                    "fill_keys_present": have_fill,
                    "valid_fill_samples": sum(1 for r in records if r.fill is not None)},
        "counters": counters(records),
        "underrun_cadence": underrun_rate(records),
        "tx_level_floor_steps": step_report(records, steps, args, have_fill,
                                            (underrun_rate(records) or {}).get("frames_per_s")),
        "underrun_jumps": [{k: v for k, v in j.items() if k != "t"}
                           for j in underrun_jumps(records)] if have_fill else [],
        "floors": floors(eps, records, args, have_fill),
        "transitions": transitions(records, args),
    }
    if have_fill:
        report["steady_fill"] = steady_levels(eps, args)
        report["noise"] = noise(eps, args)
        report["exclusion_and_torn_rates"] = rates(eps)
    else:
        report["fill"] = "absent: no clock.fill_* keys with a valid value; fill parts skipped"
    return report


def render_text(r):
    if "error" in r:
        return r["error"]
    L = []
    s = r["session"]
    L.append(f"session {s['first_utc']} .. {s['last_utc']}  {s['seconds']} s, {s['samples']} samples, "
             f"median interval {s['median_interval_s']} s")
    L.append(f"gate opens {s['gate_opens']}; classes {', '.join(s['classes_seen']) or '-'}; "
             f"fill keys {'present' if s['fill_keys_present'] else 'ABSENT'} ({s['valid_fill_samples']} valid)")
    L.append("\n== No audio change: counter deltas")
    for k, v in r["counters"].items():
        L.append(f"  {k:36s} " + ("n/a" if v is None else
                                  f"{v['first']} -> {v['last']}  delta {v['delta']}"
                                  + (f"  resets {v['resets']}" if v["resets"] else "")))
    u = r["underrun_cadence"]
    if u:
        L.append(f"  underrun cadence (gate open): {u['frames_per_s']} frames/s, {u['episodes_per_s']} episodes/s over {u['seconds']} s")
    L.append("\n== Resolution: tx_level_frames floor steps")
    if not r["tx_level_floor_steps"]:
        L.append("  none detected")
    for st in r["tx_level_floor_steps"]:
        line = f"  {st['utc']}  floor {st['floor_from']} -> {st['floor_to']}"
        slope = ""
        if "fill_slope_frames_per_s_episode_free" in st:
            slope = (f"  episode-free slope {st['fill_slope_frames_per_s_episode_free']} fr/s"
                     + (f" ({'match' if st.get('slope_matches_underrun') else 'MISMATCH'} cadence)"
                        if "slope_matches_underrun" in st else ""))
        if st.get("straddles_underrun"):
            jumps = ", ".join(f"{j['utc']} {j.get('fill_jump_frames')}" for j in st["underrun_jumps"])
            line += f"  STRADDLES underrun at {', '.join(st['underrun_utcs'])} (jumps: {jumps}); no continuity verdict" + slope
        elif "fill_jump_frames" in st:
            line += (f"  fill jump {st['fill_jump_frames']} fr ({'continuous' if st['continuous'] else 'DISCONTINUOUS'})"
                     + slope)
        elif "fill" in st:
            line += "  " + st["fill"]
        else:
            line += f"  underrun {st['underrun']['frames_per_s'] if st['underrun'] else 'n/a'} fr/s (fill absent)"
        L.append(line)
    if r.get("underrun_jumps"):
        L.append("  fill jump at each underrun (after minus before, frames): "
                 + ", ".join(f"{j['utc']} {j.get('fill_jump_frames')} (blended {j.get('blended_window_fill')})"
                             for j in r["underrun_jumps"]))
    L.append("\n== Floors per class (steady, gate open)")
    for k, v in r["floors"].items():
        L.append(f"  {k}: {json.dumps(v)}")
    L.append("\n== PCM/compressed transitions")
    if not r["transitions"]:
        L.append("  none")
    for t in r["transitions"]:
        L.append(f"  {json.dumps(t)}")
    if "fill" in r:
        L.append("\n== Fill: " + r["fill"])
        return "\n".join(L)
    st = r["steady_fill"]
    L.append("\n== Steady fill per gate open (frames)")
    for row in st["per_gate_open"]:
        L.append(f"  {row['utc']} {row['class']:10s} fill {row['steady_fill_frames']}  ({row['samples']} samples, open {row['seconds_open']} s)")
    L.append(f"  spread {json.dumps(st['spread'])}")
    L.append("\n== Noise (window-to-window sigma = stdev of consecutive-window diffs / sqrt 2)")
    for k, v in r["noise"].items():
        L.append(f"  {k}: {json.dumps(v)}")
    L.append("\n== Exclusion and torn-read rates per class")
    for k, v in r["exclusion_and_torn_rates"].items():
        L.append(f"  {k}: {json.dumps(v)}")
    return "\n".join(L)


def synthetic(path):
    """Synthetic session: PCM then compressed, two gate opens, a floor step, polled then unpolled."""
    import random
    rng = random.Random(7)
    t0 = datetime.datetime(2026, 10, 4, 20, 0, 0, tzinfo=datetime.timezone.utc)
    rows = []
    state = {"windows": 0, "excl": 0, "torn": 0, "under": 0, "eps": 0, "gov": 0}

    def rec(sec, is_open, cls, fill, level, spread, codec=0, nwin=1, force_excl=None):
        state["windows"] += nwin if is_open else 0
        draw = rng.random() < 0.02
        state["excl"] += 1 if is_open and (draw if force_excl is None else force_excl) else 0
        state["torn"] += 1 if is_open and rng.random() < 0.01 else 0
        clock = {"tx_level_frames": level, "tx_underrun_frames": state["under"],
                 "underrun_episode_count": state["eps"], "governor_trims": state["gov"],
                 "fill_level_q4": None if fill is None else int(round(fill * 16)),
                 "fill_subwindow_spread_frames": spread, "fill_windows": state["windows"],
                 "fill_subwindows_excluded": state["excl"], "fill_torn_reads": state["torn"]}
        when = (t0 + datetime.timedelta(seconds=sec)).isoformat()
        rows.append({"host_utc": when, "status": {
            "carrier": 1, "format": cls, "overruns": 0, "tx_gate": {"open": int(is_open)},
            "pipeline_diag": {"class": cls, "codec_pc": codec, "crc_rejects": 0}, "audio_diag": {"au_abandons": 0},
            "clock": clock}})

    sec = 0
    opens = [(0, 1, 300.0, 1500), (1500, 2, 310.0, 1500)]  # (start, class, fill start, length)
    # gate open 1: PCM, polled 1 Hz, floor steps at 800 s; underruns 0.23 fr/s => fill falls
    for start, cls, f0, length in opens:
        for s in range(0, length, 1):
            sec = start + s
            # Floor step at 720 (window holds the s=560 underrun: straddles) and at 840
            # (episode-free window at --step-window 200); a +100 trim sits 60 s before each step.
            step_s = 720 if start == 0 else 840
            if s == step_s - 60:
                state["gov"] += 1
            # an underrun plays 128 frames of silence: occupancy jumps +120 at each episode
            f = (f0 - 0.23 * s + (100 if s >= step_s - 60 else 0) + 120 * (s >= 560) + 120 * (s >= 1120)
                 + rng.gauss(0, 0.3))
            floor = 256 if s < step_s else 128
            level = floor + 128 * (s % 4)
            if s % 560 == 0:
                state["under"] += 128
                state["eps"] += 1
            # the episode's own window is blended: halfway between pre and post occupancy
            if s in (560, 1120):
                f -= 60
            ex = True if s in (560, 1120) else (False if min(abs(s - 560), abs(s - 1120)) <= 10 else None)
            rec(sec, True, cls, None if s < 5 else f, level, abs(rng.gauss(2, 0.5)),
                1 if cls == 2 else 0, force_excl=ex)  # Pc 1 = AC-3
        rec(start + length, False, 0, None, 0, None)
    # unpolled segment after a third gate open: 30 s interval, compressed
    base = 3100
    for s in range(0, 700, 30):
        rec(base + s, True, 2, 330.0 + rng.gauss(0, 0.3), 384 + 128 * (s % 3), abs(rng.gauss(3, 0.5)), 11,
            nwin=7)  # Pc 11 = DTS; 7 windows per 30 s record (lag 7), white noise
    # In-gate PCM -> AC-3 change (no relock) with 0.23 frames/s drift, a governor trim at s=200
    # (+100 frames, invalid window) and a true class step of +6 frames at s=400.
    base = 4000
    for s in range(0, 800):
        ac3 = s >= 400
        if s == 200:
            state["gov"] += 1
            rec(base + s, True, 1, None, 256 + 128 * (s % 4), None)
            continue
        if s == 560:
            state["under"] += 128
            state["eps"] += 1
        fill = (200 + (100 if s >= 200 else 0) - 0.23 * s + (6 if ac3 else 0) + (120 if s >= 560 else 0)
                + rng.gauss(0, 0.3))
        if s == 560:
            fill -= 60  # blended window
        ex = True if s == 560 else (False if abs(s - 560) <= 10 else None)
        rec(base + s, True, 2 if ac3 else 1, fill, 256 + 128 * (s % 4), abs(rng.gauss(2, 0.5)), 1 if ac3 else 0,
            force_excl=ex)
    rec(base + 800, False, 0, None, 0, None)
    with open(path, "w", encoding="utf-8") as h:
        for r in rows:
            h.write(json.dumps(r) + "\n")


def self_test():
    ns = build_parser().parse_args(["x", "--step-window", "200"])
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "synthetic.jsonl")
        synthetic(path)
        report = analyse(load([path]), ns)
        assert report["session"]["fill_keys_present"], report["session"]
        assert report["session"]["gate_opens"] == 4, report["session"]
        assert set(report["session"]["classes_seen"]) == {"pcm", "ac3", "dts"}, report["session"]
        steps = report["tx_level_floor_steps"]
        assert any(s["floor_from"] == 256 and s["floor_to"] == 128 for s in steps), steps
        by_utc = {s["utc"]: s for s in steps if s["floor_from"] == 256}
        assert len(by_utc) == 2, steps
        straddle, clean = [by_utc[k] for k in sorted(by_utc)]
        # underrun inside the window: no continuity verdict, reported with its jump (about +120)
        assert straddle.get("straddles_underrun") and "continuous" not in straddle, straddle
        sj = straddle["underrun_jumps"][0]
        assert 100 < sj["fill_jump_frames"] < 140 and sj["method"] == "windows", straddle
        # a +120 step with one blended window: jump taken across it, blended value reported apart
        assert abs(sj["fill_jump_frames"] - 120) < 3, sj
        assert 40 < sj["blended_window_fill"] - sj["before_window_fill"] < 80, sj
        # clean step (trim 60 s before it is cut, not read as a jump) stays continuous
        assert clean["continuous"] and not clean.get("straddles_underrun"), clean
        assert clean["slope_matches_underrun"] and straddle["slope_matches_underrun"], (clean, straddle)
        assert report["underrun_jumps"] and all(90 < j["fill_jump_frames"] < 150
                                                for j in report["underrun_jumps"]), report["underrun_jumps"]
        assert len(report["steady_fill"]["per_gate_open"]) == 4
        assert {"dts/unpolled", "ac3/polled", "pcm/polled"} <= set(report["noise"]), report["noise"]
        # a mid-run trim (+100 frames, invalid window) must not inflate sigma
        for key in ("pcm/polled", "ac3/polled"):
            sigma = report["noise"][key]["window_to_window_sigma_frames"]
            assert sigma is not None and sigma < 0.6, (key, sigma)
        assert report["noise"]["pcm/polled"]["lag_distribution"], report["noise"]["pcm/polled"]
        # unpolled path (lag 7, white noise): the white-model figure matches the polled sigma,
        # the random-walk figure reads low by about sqrt(7)
        un = report["noise"]["dts/unpolled"]
        assert set(un["lag_distribution"]) == {"7"}, un
        polled = report["noise"]["pcm/polled"]["window_to_window_sigma_frames"]
        assert abs(un["lag_gt1_white_sigma_frames"] - polled) < 0.12, (un, polled)
        assert un["lag_gt1_random_walk_sigma_frames"] < 0.6 * un["lag_gt1_white_sigma_frames"], un
        assert report["exclusion_and_torn_rates"]["pcm"]["windows"] > 0
        assert report["exclusion_and_torn_rates"]["ac3"]["windows"] > 0
        assert report["transitions"], "no PCM-to-AC-3 transition"
        assert report["transitions"][0]["from"] == "pcm" and report["transitions"][0]["to"] == "ac3"
        assert report["transitions"][0]["via_gate_close"] is True, report["transitions"][0]
        assert "fill_difference_frames" not in report["transitions"][0], report["transitions"][0]
        in_gate = [t for t in report["transitions"] if t["from"] == "pcm" and t["to"] == "ac3"
                   and not t["via_gate_close"]]
        assert len(in_gate) == 1, report["transitions"]
        diff = in_gate[0]["fill_difference_frames"]
        assert abs(diff - 6) <= 2, in_gate[0]  # recovered despite 0.23 frames/s drift and a trim
        assert in_gate[0]["seconds_to_settle"] <= 5, in_gate[0]
        assert {"ac3_minus_pcm", "dts_minus_pcm"} <= set(report["floors"]["difference_drift_confounded"]), report["floors"]
        assert "compressed" not in report["floors"]
        print(render_text(report))
        # degraded: strip fill keys
        degraded = os.path.join(d, "degraded.jsonl")
        with open(path, encoding="utf-8") as src, open(degraded, "w", encoding="utf-8") as dst:
            for line in src:
                row = json.loads(line)
                for k in FILL_KEYS:
                    row["status"]["clock"].pop(k, None)
                dst.write(json.dumps(row) + "\n")
        rep = analyse(load([degraded]), ns)
        assert not rep["session"]["fill_keys_present"] and "fill" in rep and "steady_fill" not in rep
        assert rep["tx_level_floor_steps"], "degraded mode lost floor steps"
        render_text(rep)
        # no codec_pc: compressed falls back to compressed-unknown, fill parts intact
        nocodec = os.path.join(d, "nocodec.jsonl")
        with open(path, encoding="utf-8") as src, open(nocodec, "w", encoding="utf-8") as dst:
            for line in src:
                row = json.loads(line)
                row["status"]["pipeline_diag"].pop("codec_pc", None)
                dst.write(json.dumps(row) + "\n")
        rep = analyse(load([nocodec]), ns)
        assert set(rep["session"]["classes_seen"]) == {"pcm", "compressed-unknown"}, rep["session"]
        assert "steady_fill" in rep and not rep["transitions"]
        render_text(rep)
    # A gate close inside a span: two flat runs whose floor re-bases +80 frames at the relock.
    # Fitted across the close the pair reads as about +0.67 frames/s of drift; split, it is flat.
    # Then the close without a closed poll (the relock seen only as a gate-open pad step).
    def span_rec(t, is_open, fill, padded):
        return types.SimpleNamespace(t=float(t), open=is_open, fill=fill if is_open else None,
                                     underrun=False, reset=False,
                                     clock={"gate_open_padded_frames": padded})
    flat_a = [span_rec(t, True, 300.0 + 0.01 * (t % 2), 128) for t in range(0, 60)]
    flat_b = [span_rec(t, True, 380.0 + 0.01 * (t % 2), 256) for t in range(61, 121)]
    with_close = flat_a + [span_rec(60, False, None, 128)] + flat_b
    slope = episode_free_slope(with_close, ns)
    assert slope is not None and abs(slope) < 0.01, slope
    assert fit([(r.t, r.fill) for r in flat_a + flat_b])[0] > 0.5, "the fixture does not re-base"
    slope = episode_free_slope(flat_a + flat_b, ns)
    assert slope is not None and abs(slope) < 0.01, ("unpolled close not split", slope)
    print("self-test ok")
    return 0


def build_parser():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("input", nargs="*", help="JSONL file(s) from aius_status_poll.py")
    p.add_argument("--format", choices=("text", "json"), default="text")
    p.add_argument("--output", help="write the report here instead of stdout")
    p.add_argument("--self-test", action="store_true", help="run against a built-in synthetic session")
    p.add_argument("--settle", type=float, default=60.0, help="seconds after a gate open before steady state")
    p.add_argument("--steady-span", type=float, default=120.0, help="seconds of steady fill averaged per gate open")
    p.add_argument("--gap", type=float, default=120.0, help="sample gap in seconds that breaks a run")
    p.add_argument("--unpolled-min-interval", type=float, default=10.0,
                   help="sample interval in seconds from which a pair counts as unpolled")
    p.add_argument("--floor-bin", type=float, default=60.0, help="bin width in seconds for the tx_level floor")
    p.add_argument("--step-min-frames", type=float, default=64.0, help="minimum floor change counted as a step")
    p.add_argument("--step-persist", type=int, default=2,
                   help="consecutive bins (including the step bin) the new floor must hold; "
                        "0/128 floor toggles after the floor reaches 0 (near an empty ring) are expected")
    p.add_argument("--step-window",type=float, default=300.0, help="seconds either side of a step for fits")
    p.add_argument("--continuity-frames", type=float, default=4.0, help="fill jump tolerated across a step")
    return p


def main() -> int:
    args = build_parser().parse_args()
    if args.self_test:
        return self_test()
    if not args.input:
        print("error: no input file (or use --self-test)", file=sys.stderr)
        return 2
    report = analyse(load(args.input), args)
    text = json.dumps(report, indent=2) if args.format == "json" else render_text(report)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as h:
            h.write(text + "\n")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
