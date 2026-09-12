#!/usr/bin/env python3
"""H3f routing-geometry gate: every copper track is straight (no curves or
arcs) and bends only at multiples of 15 degrees, with straight sections
between bends (Owner ruling, `docs/plans/aius-h2-h3-manufacturing.md`
"Owner rulings (2026-09-12)").

Inspects every copper track item on `--board` (default: canonical
`aius-carrier.kicad_pcb`) via `board.GetTracks()` -- zone fills
(`board.Zones()`) and footprint graphics (pads, silkscreen, courtyard
drawings) are separate board containers and are never visited. Reports four
finding classes:

  (a) `arc` -- any `PCB_ARC` (a curved track segment). Always non-compliant.
  (b) `off-grid-angle` -- a straight segment whose own direction angle (mod
      180 degrees, since a line and its reverse share one axis) is not a
      multiple of 15 degrees within ANGLE_TOLERANCE_DEG.
  (c) `off-grid-bend` -- a pair of same-net, same-layer, endpoint-connected
      straight segments whose turn (bend) angle at the shared point is not
      a multiple of 15 degrees within ANGLE_TOLERANCE_DEG. Derivable from
      (b) once every individual segment already passes it (two on-grid
      segment angles cannot combine into an off-grid turn), but reported
      as its own class per the routing-geometry contract.
  (d) `short-segment` -- a straight segment shorter than
      MIN_STRAIGHT_LENGTH_MM. Report-only: never affects the exit code, a
      short bend-to-bend run is a fabrication-margin note, not itself an
      angle violation.

Vias are connection points, not segments, and are excluded from every
angle check; a same-net same-layer segment pair meeting at a via is still
checked for its bend angle like any other junction.

Prints per-net and per-layer finding totals, then the worst offenders
(largest angle deviation, or largest length shortfall for (d)) with board
coordinates.

`--fix` is a narrowly bounded repair, not a re-route: for each `(b)`
off-grid-angle segment it rotates whichever endpoint is a genuinely free
point (no other track/via endpoint and no pad within OCCUPANCY_EPSILON_MM)
about the other, fixed endpoint, snapping the segment's own direction to
the nearest 15-degree multiple -- but only when that endpoint's own
required move is <= MAX_FIX_MOVE_MM and the board's own
`kicad-cli pcb drc --severity-all` violation count does not increase
versus the pre-fix baseline. A segment whose both endpoints are shared
junctions or pads (moving either would strand other copper), or whose
required move exceeds the budget, or whose attempted snap regresses DRC,
is left unchanged and reported -- this is deliberately not a general
re-router. `--fix` saves the board in place (like `route_cleanup.py`).

Run: scripts/kicad.sh --python electronics/scripts/check_routing_geometry.py
Run against a candidate: scripts/kicad.sh --python \
  electronics/scripts/check_routing_geometry.py --board <candidate.kicad_pcb>
Attempt the bounded snap-to-grid fix: scripts/kicad.sh --python \
  electronics/scripts/check_routing_geometry.py --board <candidate.kicad_pcb> --fix

Exit: 0 no (a)/(b) findings; 1 any (a) or (b) finding present, evaluated
against the board as it stands after `--fix` when given. (c)/(d) findings
are always reported but never change the exit code.
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import tempfile
from pathlib import Path

import pcbnew

ELEC = Path(__file__).resolve().parent.parent
DEFAULT_BOARD = ELEC / "aius-carrier.kicad_pcb"

ANGLE_STEP_DEG = 15.0
ANGLE_TOLERANCE_DEG = 0.5
MIN_STRAIGHT_LENGTH_MM = 0.2

WORST_OFFENDERS_SHOWN = 20

EXIT_WORTHY_KINDS = ("arc", "off-grid-angle")
ALL_KINDS = ("arc", "off-grid-angle", "off-grid-bend", "short-segment")

# --fix bounds (design decision + task contract): a snap may move a segment's
# free endpoint by at most this much, and only takes effect when the
# board's own DRC violation count does not increase.
MAX_FIX_MOVE_MM = 0.05
# KiCad connectivity at this granularity is exact-coordinate (integer
# nanometres); this epsilon only absorbs float round-trip through mm, not a
# real tolerance on the board's own geometry.
OCCUPANCY_EPSILON_MM = 0.01


class Finding:
    """One reportable geometry finding: its class, the net/layer it sits
    on, board coordinates (mm) for locating it, a one-line human detail, a
    severity used only to rank the worst-offenders table, and (for
    `off-grid-angle` findings only) the originating track object `--fix`
    needs to attempt a repair."""

    __slots__ = ("kind", "net", "layer", "x_mm", "y_mm", "detail", "severity", "track")

    def __init__(self, kind: str, net: str, layer: str, x_mm: float, y_mm: float,
                 detail: str, severity: float, track: object | None = None) -> None:
        self.kind = kind
        self.net = net
        self.layer = layer
        self.x_mm = x_mm
        self.y_mm = y_mm
        self.detail = detail
        self.severity = severity
        self.track = track


def _mm(nanometres: int) -> float:
    return nanometres / 1e6


def _angle_deviation_deg(angle_deg: float, step_deg: float = ANGLE_STEP_DEG) -> float:
    """Signed distance from `angle_deg` to the nearest multiple of `step_deg`."""
    nearest = round(angle_deg / step_deg) * step_deg
    return angle_deg - nearest


def _segment_angle_deg(start, end) -> float:
    """Direction angle of a straight segment's own axis, folded into
    [0, 180) -- a line and its reverse describe the same axis, so 0 and 180
    degrees are the same orientation."""
    angle = math.degrees(math.atan2(end.y - start.y, end.x - start.x))
    return angle % 180.0


def _turn_angle_deg(direction_in: tuple[int, int], direction_out: tuple[int, int]) -> float:
    """Signed turn angle from `direction_in` to `direction_out`, folded to
    (-180, 180]. 0 means the two segments continue straight through the
    shared point; +-180 means the second segment doubles back on the first."""
    angle_in = math.atan2(direction_in[1], direction_in[0])
    angle_out = math.atan2(direction_out[1], direction_out[0])
    turn = math.degrees(angle_out - angle_in)
    while turn <= -180.0:
        turn += 360.0
    while turn > 180.0:
        turn -= 360.0
    return turn


def _split_tracks(board) -> tuple[list, list]:
    """`board.GetTracks()` split into (straight segments, arcs). A via
    extends `PCB_TRACK` in the pcbnew hierarchy too, so it must be excluded
    explicitly rather than falling through to the straight-segment bucket;
    order matters -- check the most specific subclass first."""
    straights = []
    arcs = []
    for track in board.GetTracks():
        if isinstance(track, pcbnew.PCB_ARC):
            arcs.append(track)
        elif isinstance(track, pcbnew.PCB_VIA):
            continue
        elif isinstance(track, pcbnew.PCB_TRACK):
            straights.append(track)
    return straights, arcs


def _collect_findings(board) -> list[Finding]:
    straights, arcs = _split_tracks(board)
    findings: list[Finding] = []

    for arc in arcs:
        net = arc.GetNetname()
        layer = board.GetLayerName(arc.GetLayer())
        start = arc.GetStart()
        findings.append(Finding(
            "arc", net, layer, _mm(start.x), _mm(start.y),
            "PCB_ARC (curved track segment)", severity=1e9))

    for track in straights:
        net = track.GetNetname()
        layer = board.GetLayerName(track.GetLayer())
        start, end = track.GetStart(), track.GetEnd()
        start_mm, end_mm = (_mm(start.x), _mm(start.y)), (_mm(end.x), _mm(end.y))
        length_mm = math.hypot(end_mm[0] - start_mm[0], end_mm[1] - start_mm[1])

        angle = _segment_angle_deg(start, end)
        deviation = _angle_deviation_deg(angle)
        if abs(deviation) > ANGLE_TOLERANCE_DEG:
            findings.append(Finding(
                "off-grid-angle", net, layer, start_mm[0], start_mm[1],
                f"segment angle {angle:.3f} deg (mod 180), {deviation:+.3f} deg "
                f"off nearest {ANGLE_STEP_DEG:g} deg -- end ({end_mm[0]:.4f}, {end_mm[1]:.4f}) mm",
                severity=abs(deviation), track=track))

        if length_mm < MIN_STRAIGHT_LENGTH_MM:
            findings.append(Finding(
                "short-segment", net, layer, start_mm[0], start_mm[1],
                f"segment length {length_mm:.4f} mm < {MIN_STRAIGHT_LENGTH_MM:g} mm -- "
                f"end ({end_mm[0]:.4f}, {end_mm[1]:.4f}) mm",
                severity=MIN_STRAIGHT_LENGTH_MM - length_mm))

    findings.extend(_bend_findings(board, straights))
    return findings


def _bend_findings(board, straights: list) -> list[Finding]:
    """Bend-angle findings ((c) above) for every same-net, same-layer pair
    of straight segments that share an exact endpoint. KiCad connectivity
    at this granularity is exact-coordinate (integer nanometres), so a
    plain dict keyed on the endpoint's raw (x, y) tuple is enough -- no
    epsilon-matching pass is needed."""
    by_net_layer: dict[tuple[int, int], list] = {}
    for track in straights:
        key = (track.GetNetCode(), track.GetLayer())
        by_net_layer.setdefault(key, []).append(track)

    findings: list[Finding] = []
    for (_net_code, layer_id), tracks in by_net_layer.items():
        endpoints: dict[tuple[int, int], list[tuple[object, bool]]] = {}
        for track in tracks:
            start, end = track.GetStart(), track.GetEnd()
            endpoints.setdefault((start.x, start.y), []).append((track, True))
            endpoints.setdefault((end.x, end.y), []).append((track, False))

        for point, occupants in endpoints.items():
            if len(occupants) < 2:
                continue
            for index_a in range(len(occupants)):
                for index_b in range(index_a + 1, len(occupants)):
                    track_a, a_is_start = occupants[index_a]
                    track_b, b_is_start = occupants[index_b]
                    if track_a is track_b:
                        continue
                    far_a = track_a.GetEnd() if a_is_start else track_a.GetStart()
                    far_b = track_b.GetEnd() if b_is_start else track_b.GetStart()
                    direction_in = (point[0] - far_a.x, point[1] - far_a.y)
                    direction_out = (far_b.x - point[0], far_b.y - point[1])
                    if direction_in == (0, 0) or direction_out == (0, 0):
                        continue
                    turn = _turn_angle_deg(direction_in, direction_out)
                    deviation = _angle_deviation_deg(turn)
                    if abs(deviation) > ANGLE_TOLERANCE_DEG:
                        findings.append(Finding(
                            "off-grid-bend", track_a.GetNetname(), board.GetLayerName(layer_id),
                            _mm(point[0]), _mm(point[1]),
                            f"bend {turn:.3f} deg, {deviation:+.3f} deg off nearest "
                            f"{ANGLE_STEP_DEG:g} deg",
                            severity=abs(deviation)))
    return findings


def _rotate_point(point_mm: tuple[float, float], anchor_mm: tuple[float, float],
                   delta_deg: float) -> tuple[float, float]:
    """`point_mm` rotated by `delta_deg` about `anchor_mm`. Rotating either
    endpoint of a two-point segment about the other by the same signed
    `delta_deg` reorients the segment's own direction by `delta_deg`
    regardless of which endpoint moves -- the rotation is a property of the
    connecting vector, not of which end is labelled the anchor."""
    radians = math.radians(delta_deg)
    cos_delta, sin_delta = math.cos(radians), math.sin(radians)
    dx, dy = point_mm[0] - anchor_mm[0], point_mm[1] - anchor_mm[1]
    return (anchor_mm[0] + dx * cos_delta - dy * sin_delta,
            anchor_mm[1] + dx * sin_delta + dy * cos_delta)


def _occupancy_points(board, exclude_track) -> list[tuple[float, float]]:
    """Every pad center, via center, and track endpoint on `board` other
    than `exclude_track`'s own two endpoints -- the set of points a `--fix`
    snap must not silently strand copper against. Deliberately
    net-agnostic: foreign-net copper occupies a point just as much as
    same-net copper does."""
    points: list[tuple[float, float]] = []
    for footprint in board.GetFootprints():
        for pad in footprint.Pads():
            position = pad.GetPosition()
            points.append((_mm(position.x), _mm(position.y)))
    for track in board.GetTracks():
        if track is exclude_track:
            continue
        if isinstance(track, pcbnew.PCB_VIA):
            position = track.GetPosition()
            points.append((_mm(position.x), _mm(position.y)))
            continue
        start, end = track.GetStart(), track.GetEnd()
        points.append((_mm(start.x), _mm(start.y)))
        points.append((_mm(end.x), _mm(end.y)))
    return points


def _is_occupied(point_mm: tuple[float, float], occupancy: list[tuple[float, float]],
                  epsilon_mm: float = OCCUPANCY_EPSILON_MM) -> bool:
    return any(math.hypot(point_mm[0] - other[0], point_mm[1] - other[1]) <= epsilon_mm
               for other in occupancy)


def _run_drc_violation_count(board_path: str) -> int:
    """`.violations` count from a fresh `kicad-cli pcb drc` run against
    `board_path` -- the same metric `check_dfm.sh` uses (`jq '.violations |
    length'`). `kicad-cli` exits non-zero when violations are found, so the
    run is deliberately not `check=True`."""
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as report_file:
        report_path = report_file.name
    subprocess.run(
        ["kicad-cli", "pcb", "drc", "--format", "json", "--severity-all", "-o", report_path, board_path],
        check=False, capture_output=True, text=True,
    )
    with open(report_path, encoding="utf-8") as report_file:
        report = json.load(report_file)
    return len(report.get("violations", []))


def _attempt_fix(board, board_path: str, track, drc_baseline: int) -> tuple[bool, str]:
    """Try to snap one off-grid-angle `track` to the nearest 15-degree
    direction. Tries the smaller-displacement endpoint first; an endpoint
    is eligible only when it is not shared with any other track/via/pad
    (`_is_occupied`) and its own required move is within
    MAX_FIX_MOVE_MM. An eligible move is applied, saved, and re-checked
    against `drc_baseline`; a regression reverts it and the next candidate
    (if any) is tried. Returns (fixed, one-line detail)."""
    start_v, end_v = track.GetStart(), track.GetEnd()
    start_mm = (_mm(start_v.x), _mm(start_v.y))
    end_mm = (_mm(end_v.x), _mm(end_v.y))
    # The folded (mod 180) deviation is numerically identical to the raw
    # signed-angle deviation here (folding only ever adds/subtracts a
    # multiple of 180, itself a multiple of the 15-degree step), so the
    # same delta correctly rotates the real direction vector.
    deviation = _angle_deviation_deg(_segment_angle_deg(start_v, end_v))
    delta_deg = -deviation

    candidates = [
        ("end", start_mm, end_mm, _rotate_point(end_mm, start_mm, delta_deg)),
        ("start", end_mm, start_mm, _rotate_point(start_mm, end_mm, delta_deg)),
    ]
    candidates.sort(key=lambda item: math.hypot(item[3][0] - item[2][0], item[3][1] - item[2][1]))

    occupancy = _occupancy_points(board, exclude_track=track)
    reasons = []

    for which, _anchor_mm, original_mm, new_mm in candidates:
        move_mm = math.hypot(new_mm[0] - original_mm[0], new_mm[1] - original_mm[1])
        if move_mm > MAX_FIX_MOVE_MM:
            reasons.append(f"{which} move {move_mm * 1000:.1f} um exceeds {MAX_FIX_MOVE_MM * 1000:.0f} um budget")
            continue
        if _is_occupied(original_mm, occupancy):
            reasons.append(f"{which} endpoint is a shared junction/pad, not a free point")
            continue

        new_point = pcbnew.VECTOR2I(round(new_mm[0] * 1e6), round(new_mm[1] * 1e6))
        original_point = track.GetEnd() if which == "end" else track.GetStart()
        if which == "end":
            track.SetEnd(new_point)
        else:
            track.SetStart(new_point)
        pcbnew.SaveBoard(board_path, board)

        post_count = _run_drc_violation_count(board_path)
        if post_count <= drc_baseline:
            return True, (f"snapped {which} endpoint by {move_mm * 1000:.1f} um "
                          f"(DRC violations {drc_baseline} -> {post_count})")

        if which == "end":
            track.SetEnd(original_point)
        else:
            track.SetStart(original_point)
        pcbnew.SaveBoard(board_path, board)
        reasons.append(f"{which} snap regressed DRC ({drc_baseline} -> {post_count}), reverted")

    return False, "; ".join(reasons) if reasons else "no candidate endpoint available"


def _run_fix_pass(board, board_path: str, findings: list[Finding]) -> int:
    drc_baseline = _run_drc_violation_count(board_path)
    print(f"==> Pre-fix DRC baseline: {drc_baseline} violation(s)")

    off_grid = [finding for finding in findings if finding.kind == "off-grid-angle"]
    fixed_count = 0
    for finding in off_grid:
        fixed, detail = _attempt_fix(board, board_path, finding.track, drc_baseline)
        status = "fixed" if fixed else "left unchanged"
        print(f"[fix] {finding.net}\t{finding.layer}\t"
              f"({finding.x_mm:.4f}, {finding.y_mm:.4f}) mm\t{status} -- {detail}")
        if fixed:
            fixed_count += 1

    print(f"\n==> Fix summary: {fixed_count}/{len(off_grid)} off-grid-angle segment(s) snapped, "
          f"{len(off_grid) - fixed_count} left unchanged and reported")
    return fixed_count


def _report(findings: list[Finding]) -> None:
    print("==> Findings by class")
    for kind in ALL_KINDS:
        count = sum(1 for finding in findings if finding.kind == kind)
        print(f"{count}\t{kind}")

    print("\n==> Totals by net")
    by_net: dict[str, int] = {}
    for finding in findings:
        by_net[finding.net] = by_net.get(finding.net, 0) + 1
    for net, count in sorted(by_net.items(), key=lambda item: (-item[1], item[0])):
        print(f"{count}\t{net}")

    print("\n==> Totals by layer")
    by_layer: dict[str, int] = {}
    for finding in findings:
        by_layer[finding.layer] = by_layer.get(finding.layer, 0) + 1
    for layer, count in sorted(by_layer.items(), key=lambda item: (-item[1], item[0])):
        print(f"{count}\t{layer}")

    ranked = sorted(findings, key=lambda finding: finding.severity, reverse=True)
    print(f"\n==> Worst offenders (top {WORST_OFFENDERS_SHOWN} of {len(findings)})")
    for finding in ranked[:WORST_OFFENDERS_SHOWN]:
        print(f"{finding.kind}\t{finding.net}\t{finding.layer}\t"
              f"({finding.x_mm:.4f}, {finding.y_mm:.4f}) mm\t{finding.detail}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--board", type=Path, default=DEFAULT_BOARD,
                         help="board .kicad_pcb path (default: canonical aius-carrier.kicad_pcb)")
    parser.add_argument("--fix", action="store_true",
                         help="attempt the bounded snap-to-15-degree fix on off-grid-angle "
                              "segments (saves the board in place); see module docstring")
    args = parser.parse_args()
    board_path = str(args.board)

    board = pcbnew.LoadBoard(board_path)
    findings = _collect_findings(board)

    if args.fix:
        _run_fix_pass(board, board_path, findings)
        print()
        board = pcbnew.LoadBoard(board_path)
        findings = _collect_findings(board)

    _report(findings)

    exit_worthy = sum(1 for finding in findings if finding.kind in EXIT_WORTHY_KINDS)
    print(f"\n==> {exit_worthy} exit-worthy finding(s) (arc / off-grid-angle) "
          f"of {len(findings)} total")
    return 1 if exit_worthy else 0


if __name__ == "__main__":
    raise SystemExit(main())
