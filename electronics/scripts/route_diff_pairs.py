#!/usr/bin/env python3
"""H3e diff-pair rework: rip and re-route the four controlled-impedance pairs
as genuinely coupled differential traces at the netclass width and gap,
length-matched, and locked so a later Freerouting pass treats them as fixed.

The H3e second-pass review (`.scratch/h3e-review/` probe scripts) measured
that Freerouting routed all four pairs single-ended: USB_DP/USB_DM 6.15 mm
skew with 2 vias each and 2.55 mm apart on B.Cu; USBH_DP/USBH_DM 16 mm apart;
ETH_TXP/ETH_TXN min gap 1.07 mm; ETH_RXP on B.Cu vs ETH_RXN on F.Cu; ETH_TXN
coupled to ETH_RXP for 4.8 mm; the USB pair necked to 0.151-0.188 mm at the
J1/U1 fan-out. This script replaces all eight nets' copper with genuinely
coupled parallel traces at the netclass width (USB90 0.251 mm, Eth100
0.201 mm) and a 0.15 mm edge gap, length-matched to <=0.5 mm skew. USB host
and Ethernet TX route at 0 vias, entirely on F.Cu, as specified; USB device
needs two disclosed via pairs (a genuine planar-crossing topology forced by
the pads' fixed connector pinout -- see `_route_crossing_leg` and
`_route_bridge_pair`), printed as `DISCLOSED EXCEPTION` lines at run time.

Residual findings (real `kicad-cli pcb drc`, not just this script's own
sampling-based reporting): the J1 USB-C fan-out is dense enough (four 0.3 x
1.15 mm pads on a single row within ~1.5 mm) that some `shorting_items` /
`clearance` violations remain around it even after routing, and ETH_RXN /
ETH_TXN run close enough together near the shared J20/J18 corridor to report
one more -- both are reported to the operator rather than chased through
further constant-tuning (this script checks a pair's own P/N clearance and
foreign-net clearance, but never one pair's copper against a *different*
pair's copper). This is the honest stopping point for the scripted pass,
consistent with this project's own "bounded iterations, stop at diminishing
returns" convention (H3c); the residual is interactive-judgment cleanup.

Routing model: each pair is a list of hops -- (footprint, pad) tuples for the
P and N side in lockstep, in physical order along the signal path (connector
pad -> any series element pad-to-pad, e.g. the U1 ESD shunt -> destination
pad). Between each consecutive pair of hops ("leg") the script builds one
45-degree-only centerline from the midpoint of the P/N pad positions at each
end, offsets it by +-half the pair pitch (track width + gap) to get the
coupled P/N polylines for that leg, then adds a short 45-degree lead from
each actual pad position to its polyline endpoint (the fan-in/out neck --
kept at class width, per pair pitch mismatches at the connector pads). A
mirror pin (the USB-C connector's flipped-orientation twin pad) gets a short
same-net bridge to its leg's primary pad instead of its own coupled run --
real diff-pair length-matching never counts a stub's length.

Read the actual pad positions off the board every run (never hardcoded mm
coordinates) so a re-scaffold or footprint edit does not silently invalidate
this script; PAIR_SPECS below only fixes the *topology* (which ref/pad hops
in what order), not geometry.

Implementation note -- KiCad 10.0.4's pcbnew Python bindings return an
un-downcast, unusable SwigPyObject from every board-level container accessor
(board.GetTracks() / GetFootprints() / GetNetsByName() / FindNet(); the
GetFootprints() case segfaults the interpreter outright rather than raising)
once a single board.Remove() call has happened on that board object --
confirmed empirically against this container: an object fetched *before* any
Remove() keeps working fine afterward (including board.Add() of many new
tracks, board.BuildConnectivity(), and pcbnew.SaveBoard()), but re-querying a
board container after Remove() does not. This script therefore resolves
every net, pad, and existing-track snapshot it needs up front, computes all
new geometry and every foreign-copper rip decision from those held
references (no board mutation yet), and only then performs every
board.Remove() / board.Add() call in one final pass.

Run: python3 electronics/scripts/route_diff_pairs.py
Run against a scaffolded candidate: python3 electronics/scripts/route_diff_pairs.py \
  --board <candidate.kicad_pcb>
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

import pcbnew

ELEC = Path(__file__).resolve().parent.parent
DEFAULT_BOARD = ELEC / "aius-carrier.kicad_pcb"

# name -> (track/diff-pair width mm, edge-to-edge gap mm) -- H2a/scaffold_pcb.py
# NETCLASS_SPECS geometry (USB 90 ohm / Ethernet 100 ohm over the In1 GND plane).
CLASS_GEOMETRY_MM = {
    "USB90": (0.251, 0.15),
    "Eth100": (0.201, 0.15),
}

MAX_SKEW_MM = 0.5

# H2a/route_export_dsn.py standard via (0.60/0.30 mm, (0.60-0.30)/2 = 0.15 mm
# annular) -- used only by the disclosed crossing-leg fallback below.
VIA_DIAMETER_MM = 0.60
VIA_DRILL_MM = 0.30

# Each pair: bare P/N net names, its netclass, the ordered pad-to-pad hop list
# shared by both sides (same length, same order -- element i is "the same
# physical waypoint" on the P and N side), and any mirror-pin bridge pads that
# tie onto the first hop instead of running their own coupled leg (USB-C's
# orientation-flip twin D+/D- pins).
PAIR_SPECS = [
    {
        # J1's stacked A/B mirror pins carry identical nets but opposite
        # left/right order (A6 right of A7; B6 left of B7); B6/B7 is the pair
        # that preserves DP-left-of-DM into U1 (A6/A7 would force the two
        # traces to swap sides over the 3 mm J1-to-U1 hop -- a same-layer
        # crossing), so B6/B7 is the primary hop and A6/A7 the bridged pin.
        "label": "USB device",
        "p_net": "USB_DP", "n_net": "USB_DM", "class": "USB90",
        "p_hops": [("J1", "B6"), ("U1", "1"), ("J2", "3")],
        "n_hops": [("J1", "B7"), ("U1", "2"), ("J2", "2")],
        "p_bridge": ("J1", "A6"), "n_bridge": ("J1", "A7"),
    },
    {
        "label": "USB host",
        "p_net": "USBH_DP", "n_net": "USBH_DM", "class": "USB90",
        "p_hops": [("J14", "2"), ("U10", "UHDP")],
        "n_hops": [("J14", "3"), ("U10", "UHDM")],
    },
    {
        "label": "ETH TX",
        "p_net": "ETH_TXP", "n_net": "ETH_TXN", "class": "Eth100",
        "p_hops": [("J20", "1"), ("J18", "6")],
        "n_hops": [("J20", "2"), ("J18", "5")],
    },
    {
        "label": "ETH RX",
        "p_net": "ETH_RXP", "n_net": "ETH_RXN", "class": "Eth100",
        "p_hops": [("J20", "3"), ("J18", "1")],
        "n_hops": [("J20", "6"), ("J18", "2")],
    },
]


def _bare_net_name(name: str) -> str:
    return name.rsplit("/", 1)[-1]


def _target_bare_names(specs) -> set[str]:
    names = set()
    for spec in specs:
        names.add(spec["p_net"])
        names.add(spec["n_net"])
    return names


def _find_net(board, bare_name: str):
    for name, net in board.GetNetsByName().items():
        if _bare_net_name(str(name)) == bare_name:
            return net
    raise ValueError(f"no net on board matches bare name {bare_name!r}")


def _find_pad(board, ref: str, number: str):
    for footprint in board.GetFootprints():
        if footprint.GetReference() != ref:
            continue
        for pad in footprint.Pads():
            if pad.GetNumber() == number:
                return pad
    raise ValueError(f"no pad {ref}.{number} on board")


class Pt:
    """A plain 2D point in mm -- board VECTOR2I is integer nm and awkward to
    do offset/midpoint arithmetic on directly."""

    __slots__ = ("x", "y")

    def __init__(self, x: float, y: float) -> None:
        self.x = x
        self.y = y

    def __add__(self, other: "Pt") -> "Pt":
        return Pt(self.x + other.x, self.y + other.y)

    def __sub__(self, other: "Pt") -> "Pt":
        return Pt(self.x - other.x, self.y - other.y)

    def __mul__(self, scale: float) -> "Pt":
        return Pt(self.x * scale, self.y * scale)

    __rmul__ = __mul__

    def length(self) -> float:
        return math.hypot(self.x, self.y)

    def as_vector2i(self) -> "pcbnew.VECTOR2I":
        return pcbnew.VECTOR2I(pcbnew.FromMM(self.x), pcbnew.FromMM(self.y))


def _pad_point(pad) -> Pt:
    pos = pad.GetPosition()
    return Pt(pos.x / 1e6, pos.y / 1e6)


def _midpoint(a: Pt, b: Pt) -> Pt:
    return Pt((a.x + b.x) / 2, (a.y + b.y) / 2)


def _unit(vector: Pt) -> Pt:
    length = vector.length()
    if length == 0:
        raise ValueError("cannot take the unit vector of a zero-length vector")
    return Pt(vector.x / length, vector.y / length)


def _perp(direction: Pt) -> Pt:
    """90-degree rotation of a unit direction vector (right-hand turn)."""
    return Pt(-direction.y, direction.x)


def _dedup(points: list[Pt], epsilon_mm: float = 1e-6) -> list[Pt]:
    out = [points[0]]
    for point in points[1:]:
        if (point - out[-1]).length() > epsilon_mm:
            out.append(point)
    return out


def _bend_path(a: Pt, b: Pt, diagonal_first: bool = True) -> list[Pt]:
    """A 45-degree-only polyline from a to b: a single diagonal leg covering
    whichever axis has less distance to travel, then straight for the
    remainder on the other axis (`diagonal_first=True`, diagonal leg leaves
    `a`) or the mirror image (`diagonal_first=False`, diagonal leg arrives at
    `b`). Degenerates to one direct segment when a and b are already
    axis-aligned or exactly diagonal, in which case both variants coincide."""
    dx, dy = b.x - a.x, b.y - a.y
    if abs(dx) < 1e-9 and abs(dy) < 1e-9:
        return [a]
    if abs(dx) < 1e-9 or abs(dy) < 1e-9 or abs(abs(dx) - abs(dy)) < 1e-9:
        return [a, b]
    diagonal = min(abs(dx), abs(dy))
    if diagonal_first:
        bend = Pt(a.x + math.copysign(diagonal, dx), a.y + math.copysign(diagonal, dy))
    else:
        bend = Pt(b.x - math.copysign(diagonal, dx), b.y - math.copysign(diagonal, dy))
    return [a, bend, b]


def _offset_polyline(centerline: list[Pt], side_sign: float, half_pitch_mm: float) -> list[Pt]:
    """Offset each segment of `centerline` by half_pitch_mm perpendicular to
    that segment's own direction (side_sign chooses which of the two
    perpendiculars), then flatten into one polyline. Consecutive segments'
    offset endpoints are simply adjacent points in the flattened list -- at a
    bend this is a tiny natural miter connector, not a special case, because
    every centerline segment is independently offset and both its endpoints
    are emitted."""
    points: list[Pt] = []
    for start, end in zip(centerline, centerline[1:]):
        direction = _unit(end - start)
        offset = _perp(direction) * (side_sign * half_pitch_mm)
        points.append(start + offset)
        points.append(end + offset)
    return _dedup(points)


def _path_length(points: list[Pt]) -> float:
    return sum((b - a).length() for a, b in zip(points, points[1:]))


def _point_to_segment_distance_mm(point: Pt, seg_start: Pt, seg_end: Pt) -> float:
    dx, dy = seg_end.x - seg_start.x, seg_end.y - seg_start.y
    length_sq = dx * dx + dy * dy
    if length_sq == 0.0:
        return (point - seg_start).length()
    t = max(0.0, min(1.0, ((point.x - seg_start.x) * dx + (point.y - seg_start.y) * dy) / length_sq))
    closest = Pt(seg_start.x + t * dx, seg_start.y + t * dy)
    return (point - closest).length()


def _mark_blocking_foreign_copper(points: list[Pt], width_mm: float, clearance_mm: float,
                                   foreign_candidates: list, blocked_ids: set[int],
                                   sample_step_mm: float = 0.2) -> set[str]:
    """Mark (by id()) every foreign F.Cu track/via in `foreign_candidates`
    whose copper would violate clearance against the planned polyline
    `points`, without mutating the board -- the actual board.Remove() calls
    all happen in one later pass (see the module docstring). Returns the set
    of bare net names marked."""
    ripped_nets: set[str] = set()
    half_width = width_mm / 2
    for start, end in zip(points, points[1:]):
        length = (end - start).length()
        if length < 1e-6:
            continue
        steps = max(1, int(length / sample_step_mm))
        for step in range(steps + 1):
            t = step / steps
            sample = Pt(start.x + t * (end.x - start.x), start.y + t * (end.y - start.y))
            for track in foreign_candidates:
                if isinstance(track, pcbnew.PCB_VIA):
                    other_radius = track.GetWidth(pcbnew.F_Cu) / 1e6 / 2
                    distance = (sample - Pt(track.GetPosition().x / 1e6, track.GetPosition().y / 1e6)).length()
                else:
                    other_radius = track.GetWidth() / 1e6 / 2
                    track_start = Pt(track.GetStart().x / 1e6, track.GetStart().y / 1e6)
                    track_end = Pt(track.GetEnd().x / 1e6, track.GetEnd().y / 1e6)
                    distance = _point_to_segment_distance_mm(sample, track_start, track_end)
                if distance - half_width - other_radius < clearance_mm:
                    blocked_ids.add(id(track))
                    ripped_nets.add(track.GetNetname())
    return {_bare_net_name(name) for name in ripped_nets}


def _add_serpentine(points: list[Pt], extra_needed_mm: float, side_sign: float) -> list[Pt]:
    """Insert one 45-degree meander tooth into the longest straight run of
    `points` to add exactly extra_needed_mm of path length, deviating away
    from the paired net. `side_sign` is this net's own offset sign from
    `_build_leg` (the same sign `_offset_polyline` used to place it on its
    side of the centerline) -- deviating *further* along that same
    perpendicular moves away from centerline and therefore away from the
    other net, which sits on the opposite (-side_sign) side. A single
    square-cornered tooth of amplitude `a` replacing a straight run of
    length L adds 2*a*(sqrt(2)-1) of extra length (two 45-degree legs of
    length a*sqrt(2) each replacing a total run of 2*a along the original
    direction); solving for a gives the amplitude needed."""
    lengths = [(index, (b - a).length()) for index, (a, b) in enumerate(zip(points, points[1:]))]
    index, longest = max(lengths, key=lambda item: item[1])
    tooth_extra_per_mm_amplitude = 2 * (math.sqrt(2) - 1)
    amplitude = extra_needed_mm / tooth_extra_per_mm_amplitude
    amplitude = min(amplitude, longest / 2 - 1e-6)
    if amplitude <= 0:
        raise ValueError("no straight run long enough to absorb the required serpentine")
    start, end = points[index], points[index + 1]
    direction = _unit(end - start)
    perp = _perp(direction) * side_sign
    mid = _midpoint(start, end)
    tooth = [
        mid + direction * (-amplitude) + perp * amplitude,
        mid + direction * amplitude + perp * amplitude,
    ]
    return points[:index + 1] + tooth + points[index + 1:]


def _emit_polyline(board, net, layer: int, width_mm: float, points: list[Pt]) -> list:
    tracks = []
    for start, end in zip(points, points[1:]):
        if (end - start).length() < 1e-6:
            continue
        track = pcbnew.PCB_TRACK(board)
        track.SetStart(start.as_vector2i())
        track.SetEnd(end.as_vector2i())
        track.SetWidth(pcbnew.FromMM(width_mm))
        track.SetLayer(layer)
        track.SetNet(net)
        track.SetLocked(True)
        board.Add(track)
        tracks.append(track)
    return tracks


def _gap_stats_mm(p_tracks, n_tracks, sample_step_mm: float = 0.1):
    """Edge-to-edge gap between every P track and every N track *on the same
    copper layer* -- a disclosed crossing leg deliberately lets P and N cross
    in the XY plane while one of them is dipped to the opposite layer, so an
    unfiltered comparison would flag that intentional, physically-safe
    crossing as a false clearance violation."""
    gaps = []
    for track in p_tracks:
        same_layer_others = [other for other in n_tracks if other.GetLayer() == track.GetLayer()]
        if not same_layer_others:
            continue
        start = Pt(track.GetStart().x / 1e6, track.GetStart().y / 1e6)
        end = Pt(track.GetEnd().x / 1e6, track.GetEnd().y / 1e6)
        length = (end - start).length()
        steps = max(1, int(length / sample_step_mm))
        for step in range(steps + 1):
            t = step / steps
            sample = Pt(start.x + t * (end.x - start.x), start.y + t * (end.y - start.y))
            best = min(
                (_point_to_segment_distance_mm(
                    sample,
                    Pt(other.GetStart().x / 1e6, other.GetStart().y / 1e6),
                    Pt(other.GetEnd().x / 1e6, other.GetEnd().y / 1e6),
                ) - track.GetWidth() / 1e6 / 2 - other.GetWidth() / 1e6 / 2)
                for other in same_layer_others
            )
            gaps.append(best)
    if not gaps:
        return None, None
    return min(gaps), sum(gaps) / len(gaps)


def _side_sign_at(reference: Pt, hop: Pt, local_direction: Pt) -> float:
    """Which perpendicular side of `local_direction` the point `reference`
    sits on, relative to `hop` (the centerline point local_direction leaves
    from or arrives at)."""
    to_reference = reference - hop
    perp = _perp(local_direction)
    return 1.0 if (to_reference.x * perp.x + to_reference.y * perp.y) >= 0 else -1.0


def _build_leg(p_a: Pt, p_b: Pt, n_a: Pt, n_b: Pt, half_pitch_mm: float) -> tuple[list[Pt], list[Pt], float]:
    """One coupled leg between two hops: centerline from the P/N midpoints,
    offset +-half_pitch_mm per segment, then a 45-degree lead from each real
    pad position to its polyline end. Returns (p_points, n_points, side_sign)
    -- side_sign is which offset side the P polyline ended up on, needed so a
    later serpentine deviates outward rather than into the coupled partner.

    A per-segment perpendicular offset keeps a constant distance from a bent
    centerline (a standard parallel-curve/ribbon construction), but which
    physical pad (P or N) that offset lands nearest to is a *local* property
    at each end of the centerline -- on some corridors the near-end and
    far-end local directions disagree about which side P is on, which would
    force the P and N leads to swap sides mid-route (an actual conductor
    crossing, impossible on one copper layer without a via). Trying both
    45-degree bend shapes (diagonal leg first vs. diagonal leg last) picks a
    different bend point and therefore different local segment directions at
    each end; one of the two usually agrees end to end for a given corridor.

    The centerline is inset from each raw hop by up to 2*half_pitch_mm along
    the leg direction before offsetting. Without this, a leg whose pads sit
    exactly level with each other (e.g. both ends of a straight vertical
    hop) produces a coupled offset line starting at the *same height* as the
    pads themselves, so the lead from a pad to its offset-line start is pure
    sideways travel along that shared pad row -- and this board stacks up to
    four same-row pins within about 1.5 mm (a mirror-pin connector), so that
    sideways lead can sweep straight across a *different* pad's position
    (and any stub routed from it, e.g. a mirror-pin bridge) at zero
    clearance. Insetting first forces every lead to leave the pad row on a
    genuine diagonal instead.

    2*half_pitch_mm is a compromise, not a proof: J1's own USB-C high-speed
    pads measure 0.3 x 1.15 mm (tall, not a point), and fully clearing that
    height from a pad on the same crowded row needs an inset around 0.9 mm --
    empirically (a real kicad-cli pcb drc comparison) that larger inset
    clears the J1 fan-out but pushes the *shorter* Ethernet legs' coupled
    offset line into each other's corridor instead, more total violations
    net. This module-wide constant is deliberately the smaller, safer value;
    the residual J1-area shorting_items DRC findings this leaves are
    reported rather than chased further -- see the module docstring's
    residual-findings note."""
    hop_a, hop_b = _midpoint(p_a, n_a), _midpoint(p_b, n_b)
    leg_vector = hop_b - hop_a
    leg_length = leg_vector.length()
    if leg_length > 1e-6:
        inset = min(2 * half_pitch_mm, leg_length * 0.3)
        direction = _unit(leg_vector)
        hop_a = hop_a + direction * inset
        hop_b = hop_b - direction * inset
    centerline = None
    side_sign = None
    for diagonal_first in (True, False):
        candidate = _bend_path(hop_a, hop_b, diagonal_first)
        near_direction = _unit(candidate[1] - candidate[0])
        far_direction = _unit(candidate[-1] - candidate[-2])
        near_sign = _side_sign_at(p_a, candidate[0], near_direction)
        far_sign = _side_sign_at(p_b, candidate[-1], far_direction)
        if near_sign == far_sign:
            centerline, side_sign = candidate, near_sign
            break
    if centerline is None:
        raise ValueError(
            "no 45-degree bend keeps this leg's P/N handedness consistent end to end -- "
            "the pads' physical winding needs an actual conductor crossing (a via) on this corridor")

    p_offset = _offset_polyline(centerline, side_sign, half_pitch_mm)
    n_offset = _offset_polyline(centerline, -side_sign, half_pitch_mm)

    p_points = _dedup(_bend_path(p_a, p_offset[0])[:-1] + p_offset + _bend_path(p_offset[-1], p_b)[1:])
    n_points = _dedup(_bend_path(n_a, n_offset[0])[:-1] + n_offset + _bend_path(n_offset[-1], n_b)[1:])
    return p_points, n_points, side_sign


def _cross_axis_value_at(points: list[Pt], axis: str, coordinate: float) -> float:
    """Linear-interpolated *other* coordinate of the polyline `points` at
    `coordinate` along `axis` ("x" or "y") -- e.g. axis="y" returns x at a
    given height. Assumes `points` is monotonic-ish along `axis`, true for
    every leg and bridge on this board (each corridor runs predominantly
    along one axis). `coordinate` outside the polyline's own span clamps to
    the nearer endpoint's other-axis value."""
    other = "x" if axis == "y" else "y"
    for start, end in zip(points, points[1:]):
        start_value, end_value = getattr(start, axis), getattr(end, axis)
        if start_value == end_value:
            continue
        low, high = min(start_value, end_value), max(start_value, end_value)
        if low - 1e-9 <= coordinate <= high + 1e-9:
            fraction = (coordinate - start_value) / (end_value - start_value)
            return getattr(start, other) + fraction * (getattr(end, other) - getattr(start, other))
    first_distance = abs(coordinate - getattr(points[0], axis))
    last_distance = abs(coordinate - getattr(points[-1], axis))
    return getattr(points[0], other) if first_distance <= last_distance else getattr(points[-1], other)


def _point_on_axis(axis: str, coordinate: float, other_value: float) -> Pt:
    return Pt(other_value, coordinate) if axis == "y" else Pt(coordinate, other_value)


def _route_crossing_leg(p_a: Pt, p_b: Pt, n_a: Pt, n_b: Pt, dip_margin_mm: float = 1.0
                         ) -> tuple[list[tuple[list[Pt], int]], list[tuple[list[Pt], int]]]:
    """Fallback for a leg (or mirror-pin bridge pair) where `_build_leg`
    cannot find a 45-degree bend keeping P/N handedness consistent end to
    end -- the pads' fixed relative left/right (or, for a mostly-horizontal
    bridge, near/far) order genuinely reverses between the two ends (a
    topological fact: two planar curves whose endpoint order swaps must cross
    somewhere), so no amount of bend-shape juggling on F.Cu alone resolves it.

    P keeps its plain 45-degree pad-to-pad path on F.Cu, uninterrupted. N
    takes the same shape but dips to B.Cu for a short span bracketing the one
    point where the two paths swap order, then returns to F.Cu -- the
    minimum-possible via count (one pair) to make the crossing physically
    valid instead of two conductors touching on one layer. The scan axis is
    whichever of x/y the two paths together span more of (a vertical
    corridor scans by height; a horizontal bridge pair scans by x), found by
    comparing each path's own cross-axis coordinate at matching positions
    along that axis -- not by matching sample index, which does not
    correspond to the same physical location when the two paths differ in
    length or starting position. This is a deliberate, narrowly-scoped,
    disclosed exception to the 0-via routing goal for exactly this leg; see
    the script's docstring note and the run's printed disclosure line."""
    p_path = _bend_path(p_a, p_b)
    n_path = _bend_path(n_a, n_b)
    n_runs = _dip_across_paths(p_path, n_path, n_a, n_b, dip_margin_mm)
    return [(p_path, pcbnew.F_Cu)], n_runs


def _dip_across_paths(p_path: list[Pt], n_path: list[Pt], n_a: Pt, n_b: Pt, dip_margin_mm: float
                       ) -> list[tuple[list[Pt], int]]:
    """Find the single point where two already-built P/N paths swap relative
    order and return N re-expressed as F.Cu / B.Cu / F.Cu runs that dip under
    P for a short span bracketing it (see `_route_crossing_leg`). Raises
    `ValueError` if the two paths never swap order over their shared range on
    either axis (nothing to dip -- the caller should keep N as one F.Cu run)."""
    combined = p_path + n_path
    y_span = max(point.y for point in combined) - min(point.y for point in combined)
    x_span = max(point.x for point in combined) - min(point.x for point in combined)
    axes = ("y", "x") if y_span >= x_span else ("x", "y")

    for axis in axes:
        lo = max(min(getattr(point, axis) for point in p_path), min(getattr(point, axis) for point in n_path))
        hi = min(max(getattr(point, axis) for point in p_path), max(getattr(point, axis) for point in n_path))
        if lo >= hi:
            continue

        samples = 64
        flip_at = None
        previous_sign = None
        for step in range(samples):
            coordinate = lo + (hi - lo) * step / (samples - 1)
            sign = (1 if _cross_axis_value_at(n_path, axis, coordinate)
                    - _cross_axis_value_at(p_path, axis, coordinate) >= 0 else -1)
            if previous_sign is not None and sign != previous_sign:
                flip_at = coordinate
                break
            previous_sign = sign
        if flip_at is None:
            continue

        n_lo = min(getattr(point, axis) for point in n_path)
        n_hi = max(getattr(point, axis) for point in n_path)
        n_forward = getattr(n_a, axis) <= getattr(n_b, axis)
        near_coordinate = max(n_lo, flip_at - dip_margin_mm) if n_forward else min(n_hi, flip_at + dip_margin_mm)
        far_coordinate = min(n_hi, flip_at + dip_margin_mm) if n_forward else max(n_lo, flip_at - dip_margin_mm)
        dip_start = _point_on_axis(axis, near_coordinate, _cross_axis_value_at(n_path, axis, near_coordinate))
        dip_end = _point_on_axis(axis, far_coordinate, _cross_axis_value_at(n_path, axis, far_coordinate))
        return [
            (_bend_path(n_a, dip_start), pcbnew.F_Cu),
            (_bend_path(dip_start, dip_end), pcbnew.B_Cu),
            (_bend_path(dip_end, n_b), pcbnew.F_Cu),
        ]
    raise ValueError("_dip_across_paths: expected an order flip between P and N but found none on either axis")


def _detour_around_pad_row(a: Pt, b: Pt, offset_mm: float, side: float) -> list[Pt]:
    """A 45-degree up/across/down detour from `a` to `b` (both on the same
    pad row) that leaves the row entirely -- `side` (+1/-1) picks which
    perpendicular direction the detour bows toward, so two bridges sharing a
    pad row can bow to *opposite* sides and never occupy the same space."""
    direction_sign = 1.0 if b.x >= a.x else -1.0
    detour_y = a.y + side * offset_mm
    corner_near = Pt(a.x + direction_sign * offset_mm, detour_y)
    corner_far = Pt(b.x - direction_sign * offset_mm, detour_y)
    return _dedup([a, corner_near, corner_far, b])


def _route_bridge_pair(p_a: Pt, p_b: Pt, n_a: Pt, n_b: Pt, offset_mm: float
                        ) -> tuple[list[tuple[list[Pt], int]], list[tuple[list[Pt], int]]]:
    """Two same-net mirror-pin bridge stubs (each ties a USB-C connector's
    flipped-orientation twin pad to its leg's primary pad) can sit exactly
    collinear with *interleaved* spans -- both mirror pins live on the same
    connector pad row, in B/A/A/B pin order, so each bridge's straight span
    passes directly through the *other* net's pad position, not just its
    copper. When the two straight stubs don't actually share the row, route
    both directly.

    Otherwise both detour off the row on the same side, away from the
    coupled legs continuing from p_b/n_b (a detour toward the legs' own
    direction of travel collides with them instead of the row problem it was
    meant to solve). B6/A7/A6/B7 interleaving means even two detoured paths
    still cross each other once (P has to pass from one side of N's pad to
    the other, and vice versa) -- provably unavoidable in the plane for an
    order-reversing connection, the same fact `_route_crossing_leg` handles
    for a leg. `_dip_across_paths` finds that single point and gives N a
    short B.Cu dip there; a `ValueError` (no crossing after detouring) means
    the plain two-detour paths are already clear, so return them as is."""
    collinear = abs(p_a.y - p_b.y) < 1e-6 and abs(n_a.y - n_b.y) < 1e-6 and abs(n_a.y - p_a.y) < 1e-6
    overlapping = collinear and min(n_a.x, n_b.x) < max(p_a.x, p_b.x) and min(p_a.x, p_b.x) < max(n_a.x, n_b.x)
    if not overlapping:
        return [(_bend_path(p_a, p_b), pcbnew.F_Cu)], [([n_a, n_b], pcbnew.F_Cu)]

    away_from_legs = -1.0 if p_b.y >= p_a.y else 1.0
    p_detoured = _detour_around_pad_row(p_a, p_b, offset_mm, side=away_from_legs)
    n_detoured = _detour_around_pad_row(n_a, n_b, offset_mm, side=away_from_legs)
    try:
        n_runs = _dip_across_paths(p_detoured, n_detoured, n_a, n_b, dip_margin_mm=offset_mm)
    except ValueError:
        n_runs = [(n_detoured, pcbnew.F_Cu)]
    return [(p_detoured, pcbnew.F_Cu)], n_runs


def _runs_length(runs: list[tuple[list[Pt], int]]) -> float:
    return sum(_path_length(points) for points, _ in runs)


def _emit_runs(board, net, width_mm: float, runs: list[tuple[list[Pt], int]]) -> list:
    """Emit every run's polyline as locked tracks on its own layer, adding a
    locked through-via at each layer transition between consecutive runs."""
    items = []
    previous_layer = None
    for points, layer in runs:
        if previous_layer is not None and previous_layer != layer:
            via = pcbnew.PCB_VIA(board)
            via.SetPosition(points[0].as_vector2i())
            via.SetWidth(pcbnew.FromMM(VIA_DIAMETER_MM))
            via.SetDrill(pcbnew.FromMM(VIA_DRILL_MM))
            via.SetNet(net)
            via.SetViaType(pcbnew.VIATYPE_THROUGH)
            via.SetLocked(True)
            board.Add(via)
            items.append(via)
        items += _emit_polyline(board, net, layer, width_mm, points)
        previous_layer = layer
    return items


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", type=Path, default=DEFAULT_BOARD,
                         help="board .kicad_pcb path (default: canonical aius-carrier.kicad_pcb)")
    args = parser.parse_args()
    board_path = str(args.board)

    board = pcbnew.LoadBoard(board_path)
    target_bare_names = _target_bare_names(PAIR_SPECS)

    # ---- resolve everything the rest of the script needs from the board,
    # before any board.Remove() call (see the module docstring). ----
    resolved = []
    for spec in PAIR_SPECS:
        entry = {
            "spec": spec,
            "p_net": _find_net(board, spec["p_net"]),
            "n_net": _find_net(board, spec["n_net"]),
            "p_pads": [_find_pad(board, ref, number) for ref, number in spec["p_hops"]],
            "n_pads": [_find_pad(board, ref, number) for ref, number in spec["n_hops"]],
        }
        if "p_bridge" in spec:
            entry["p_bridge_pad"] = _find_pad(board, *spec["p_bridge"])
        if "n_bridge" in spec:
            entry["n_bridge_pad"] = _find_pad(board, *spec["n_bridge"])
        resolved.append(entry)

    all_tracks_snapshot = list(board.GetTracks())
    own_tracks = [track for track in all_tracks_snapshot
                  if _bare_net_name(track.GetNetname()) in target_bare_names]
    foreign_candidates = [track for track in all_tracks_snapshot
                           if track.GetLayer() == pcbnew.F_Cu
                           and _bare_net_name(track.GetNetname()) not in target_bare_names]

    # ---- compute every pair's geometry and foreign-collision rip decisions
    # purely from the held pad/net references and the static foreign-copper
    # snapshot -- no board mutation yet. ----
    blocked_ids: set[int] = set()
    pair_plans = []
    for entry in resolved:
        spec = entry["spec"]
        width_mm, gap_mm = CLASS_GEOMETRY_MM[spec["class"]]
        half_pitch_mm = (width_mm + gap_mm) / 2
        p_pads, n_pads = entry["p_pads"], entry["n_pads"]

        # Each leg is a list of runs [(points, layer), ...]; the ordinary case
        # is exactly one F.Cu run, the disclosed crossing fallback is three
        # runs (F.Cu / B.Cu / F.Cu) with a via at each transition.
        p_legs: list[list[tuple[list[Pt], int]]] = []
        n_legs: list[list[tuple[list[Pt], int]]] = []
        ripped_this_pair: set[str] = set()
        p_length = 0.0
        n_length = 0.0
        last_side_sign = 1.0
        crossing_legs: list[int] = []

        for leg_index in range(len(p_pads) - 1):
            p_a, p_b = _pad_point(p_pads[leg_index]), _pad_point(p_pads[leg_index + 1])
            n_a, n_b = _pad_point(n_pads[leg_index]), _pad_point(n_pads[leg_index + 1])
            try:
                p_points, n_points, side_sign = _build_leg(p_a, p_b, n_a, n_b, half_pitch_mm)
                p_runs, n_runs = [(p_points, pcbnew.F_Cu)], [(n_points, pcbnew.F_Cu)]
                last_side_sign = side_sign
            except ValueError:
                p_runs, n_runs = _route_crossing_leg(p_a, p_b, n_a, n_b)
                crossing_legs.append(leg_index)
            p_length += _runs_length(p_runs)
            n_length += _runs_length(n_runs)
            for points, layer in p_runs + n_runs:
                if layer != pcbnew.F_Cu:
                    continue  # foreign_candidates only tracks F.Cu copper (the task's own scope)
                ripped_this_pair |= _mark_blocking_foreign_copper(
                    points, width_mm, gap_mm, foreign_candidates, blocked_ids)
            p_legs.append(p_runs)
            n_legs.append(n_runs)

        skew_mm = p_length - n_length
        if abs(skew_mm) > MAX_SKEW_MM:
            extra = abs(skew_mm)
            # Serpentine only ever touches a leg built by _build_leg (a
            # single F.Cu run with a well-defined coupling side); a crossing
            # leg's length is already whatever the topology forces.
            if skew_mm > 0:
                last_points, last_layer = n_legs[-1][-1]
                n_legs[-1][-1] = (_add_serpentine(last_points, extra, side_sign=-last_side_sign), last_layer)
                n_length += extra
            else:
                last_points, last_layer = p_legs[-1][-1]
                p_legs[-1][-1] = (_add_serpentine(last_points, extra, side_sign=last_side_sign), last_layer)
                p_length += extra
            skew_mm = p_length - n_length

        bridge_runs = {}
        bridge_crossing = False
        if "p_bridge_pad" in entry and "n_bridge_pad" in entry:
            # Both mirror-pin bridges present (USB-C's stacked A/B pins): they
            # sit on the same connector pad row, so a naive straight stub for
            # each can coincide over their whole overlapping span.
            bp_a, bp_b = _pad_point(entry["p_bridge_pad"]), _pad_point(p_pads[0])
            bn_a, bn_b = _pad_point(entry["n_bridge_pad"]), _pad_point(n_pads[0])
            p_runs, n_runs = _route_bridge_pair(bp_a, bp_b, bn_a, bn_b, width_mm + gap_mm)
            bridge_runs["p"], bridge_runs["n"] = p_runs, n_runs
            bridge_crossing = len(n_runs) > 1
        else:
            if "p_bridge_pad" in entry:
                bridge_runs["p"] = [(_bend_path(_pad_point(entry["p_bridge_pad"]), _pad_point(p_pads[0])), pcbnew.F_Cu)]
            if "n_bridge_pad" in entry:
                bridge_runs["n"] = [(_bend_path(_pad_point(entry["n_bridge_pad"]), _pad_point(n_pads[0])), pcbnew.F_Cu)]

        for runs in bridge_runs.values():
            for points, layer in runs:
                if layer != pcbnew.F_Cu:
                    continue
                ripped_this_pair |= _mark_blocking_foreign_copper(
                    points, width_mm, gap_mm, foreign_candidates, blocked_ids)

        pair_plans.append({
            "entry": entry, "width_mm": width_mm,
            "p_legs": p_legs, "n_legs": n_legs,
            "p_length": p_length, "n_length": n_length, "skew_mm": skew_mm,
            "bridge_runs": bridge_runs, "ripped": ripped_this_pair,
            "crossing_legs": crossing_legs, "bridge_crossing": bridge_crossing,
        })

    # ---- mutate the board: every removal first, then every addition. ----
    removed_tracks = sum(1 for track in own_tracks if not isinstance(track, pcbnew.PCB_VIA))
    removed_vias = sum(1 for track in own_tracks if isinstance(track, pcbnew.PCB_VIA))
    for track in own_tracks:
        board.Remove(track)
    foreign_removed = 0
    for track in foreign_candidates:
        if id(track) in blocked_ids:
            board.Remove(track)
            foreign_removed += 1
    print(f"removed {removed_tracks} existing tracks + {removed_vias} vias on the eight pair "
          f"nets, plus {foreign_removed} blocking foreign track(s)", flush=True)

    all_ripped_foreign: set[str] = set()
    for plan in pair_plans:
        entry = plan["entry"]
        spec = entry["spec"]
        width_mm = plan["width_mm"]
        p_net, n_net = entry["p_net"], entry["n_net"]

        emitted_p = []
        for runs in plan["p_legs"]:
            emitted_p += _emit_runs(board, p_net, width_mm, runs)
        emitted_n = []
        for runs in plan["n_legs"]:
            emitted_n += _emit_runs(board, n_net, width_mm, runs)
        if "p" in plan["bridge_runs"]:
            emitted_p += _emit_runs(board, p_net, width_mm, plan["bridge_runs"]["p"])
        if "n" in plan["bridge_runs"]:
            emitted_n += _emit_runs(board, n_net, width_mm, plan["bridge_runs"]["n"])

        all_ripped_foreign |= plan["ripped"]
        emitted_p_tracks = [item for item in emitted_p if not isinstance(item, pcbnew.PCB_VIA)]
        emitted_n_tracks = [item for item in emitted_n if not isinstance(item, pcbnew.PCB_VIA)]
        min_gap, mean_gap = _gap_stats_mm(emitted_p_tracks, emitted_n_tracks)
        gap_text = f"{min_gap:.3f}/{mean_gap:.3f} mm" if min_gap is not None else "n/a"
        via_count = sum(1 for item in emitted_p + emitted_n if isinstance(item, pcbnew.PCB_VIA))
        layer_names = sorted({"F.Cu" if track.GetLayer() == pcbnew.F_Cu else "B.Cu"
                               for track in emitted_p_tracks + emitted_n_tracks})
        print(f"== {spec['label']}: P {plan['p_length']:.3f} mm, N {plan['n_length']:.3f} mm, "
              f"skew {plan['skew_mm']:.3f} mm, gap min/mean {gap_text}, layers {layer_names}, "
              f"vias {via_count}, ripped foreign nets {sorted(plan['ripped'])}", flush=True)
        if plan["crossing_legs"]:
            print(f"   DISCLOSED EXCEPTION: leg(s) {plan['crossing_legs']} needed a via pair -- "
                  f"the pads' physical left/right order reverses end to end on this corridor, a planar "
                  f"crossing no 45-degree bend choice can avoid on one copper layer alone", flush=True)
        if plan["bridge_crossing"]:
            print("   DISCLOSED EXCEPTION: mirror-pin bridge needed a via pair -- the two bridges' "
                  "interleaved connector pins (B/A/A/B) force a crossing even after both detour off "
                  "the shared pad row", flush=True)

    print(f"total foreign nets ripped across all pairs: {sorted(all_ripped_foreign)}", flush=True)

    # Any new via needs the GND planes refilled around it -- the existing
    # fill (computed before this script ran) has no anti-pad cut out for a
    # via that didn't exist yet, which kicad-cli pcb drc reports as a
    # clearance violation against the unfilled-around via every time.
    if board.Zones():
        pcbnew.ZONE_FILLER(board).Fill(board.Zones())
        print(f"refilled {len(board.Zones())} zones", flush=True)

    board.BuildConnectivity()
    pcbnew.SaveBoard(board_path, board)
    print(f"saved {board_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
