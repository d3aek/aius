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
0.201 mm) and a 0.15 mm edge gap, length-matched to <=0.5 mm skew, on
straight segments only with every bend a multiple of 45 degrees (a subset
of the 15-degree-multiple contract this board's fab review requires) --
`_insert_lateral_tooth` and `_detour_around_pad_row` are the two primitives
every detour in this script builds on, specifically so a growing
avoidance amplitude never drifts a bend off that grid. USB host and
Ethernet TX route at 0 vias, entirely on F.Cu. U1 sits at 0 degrees
(`scaffold_pcb.py` PLACEMENT -- a symmetric ESD chip with no directional
meaning), which lines its own pins 1/2 up DP-left/DM-right -- the same
handedness J1's own via-free B6/B7 mirror-pin bridges exit with (H3h, see
`_route_j1_via_free_bridge`; each bridge dips off the row on its own side,
D+ above / D- below, so the two bridges' interleaved spans never occupy the
same territory, genuinely 0 vias for that bond, and each only boxes in the
row's *other* mirror pin -- A7 for the D+ bridge, A6 for the D- bridge --
never the primary pad itself). B6/B7 (not A6/A7) are therefore this pair's
own primary J1 hop (PAIR_SPECS), so the whole J1-to-U1 leg routes as an
ordinary coupled hop: 0 vias, 0 crossings (H3i, 2026-09-12). J2's own fixed
pad order (DM-left/DP-right) is the opposite handedness from U1's, so the
pair's one unavoidable crossing lands on the open U1-to-J2 run instead,
placed at that run's own midpoint (comfortably clear of both ends) --
see `_route_usb_device_pair`'s own comment for the full argument.
ETH_RXP/ETH_RXN's own disclosed via pair: J20's magjack pin field and the
RXP/RXN-to-J18 cable-pin contract (`ethernet.json`, "1 RX+, 2 RX-, ... 6
TX+") fix ETH_TX and ETH_RX to *opposite* handedness conventions at J18
(confirmed by direct pad-position measurement), so no single J18 rotation
can satisfy both -- rotating J18 to fix RX's crossing measurably breaks
TX's own 0-via route instead. Both disclosed pairs print a
`DISCLOSED EXCEPTION` line at run time. J2 (the USB-A jack) may be
relocated/rotated freely per an design decision, but was not needed: U1's own
rotation clears the J1-to-U1 leg's order reversal without touching J2, and
the residual crossing this leaves on the U1-to-J2 leg is placed well clear
of both connectors instead.

This script also now avoids real foreign pads, not just foreign tracks: it
gathers every pad on the board that is not part of a leg's own P or N net
(`_foreign_pad_obstacles`, per-side -- P must dodge N's own hop pads too,
and vice versa) and, on a violation, grows a lateral tooth into whichever
base-centerline segment sits nearest the obstacle
(`_route_leg_avoiding_pads`), retrying with more than one simultaneous
tooth when two different obstacles pull in different directions (measured
oscillating forever with only one). Residual findings (real
`kicad-cli pcb drc` on the full `scaffold_pcb.py` -> here ->
`route_export_dsn.py` -> Freerouting -> `route_import_ses.py` ->
`route_cleanup.py` -> `route_gnd_stitch.py` pipeline, not just this
script's own sampling-based reporting): 35 violations / 7 unconnected
remain, concentrated in the same small set of genuinely congested
footprints this script's own retry loops already report as bounded --
U1's GND pin sitting within ~1 mm of its own D+/D- pins (a forced exit
direction for one leg's travel opposes the other leg's, and was measured
to trade U1's own GND clearance for a worse clearance against the *other*
differential pin instead), J1's and J2's dense multi-pin rows, U10's UHDM
lead, C30's centre-tap pad near the ETH_RX crossing, and the ETH_RXN
via/pad hole-to-hole at J18. These are reported to the operator rather
than chased through further constant-tuning, the same "bounded
iterations, stop at diminishing returns" convention (H3c) as the GND
punch-list in `route_gnd_stitch.py`; the residual is interactive-judgment
cleanup.

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

# Minimum distance (along the dip's own scan axis) a crossing-leg transition
# via keeps from a through-hole pad's own coordinate -- via drill radius
# (0.15 mm) plus the through-hole pad's own drill radius (0.4 mm, J18's 0.8
# mm magjack drill) plus the board's real hole-to-hole minimum (0.4495 mm,
# read off a `kicad-cli pcb drc` finding), rounded up with margin: 0.9995 mm
# needed center-to-center, along one axis when (as here) the via and pad
# share the other coordinate. Without this floor a crossing point close to
# one end clamps the dip boundary to that end's own pad location, landing
# the via's drilled hole too close to the pad's (a real `hole_to_hole` DRC
# finding regardless of net -- a hole-to-hole rule is a manufacturing
# constraint, not a net-clearance one). Deliberately not the 1.2 mm first
# tried: that value pushed the dip's own lead far enough along the corridor
# to clip the OTHER net's uninterrupted path instead (measured on
# ETH_RXP/RXN). A previous, smaller 0.6 mm standoff was not actually
# binding on ETH_RXN's own dip (the scan's own flip-point + dip-margin
# arithmetic landed closer to J18's pad2 than the standoff alone would have
# allowed) -- confirmed by a real `kicad-cli pcb drc` hole_to_hole finding at
# 0.372 mm actual, short of the 0.4495 mm minimum; 1.05 mm forces the
# standoff clamp to actually bind.
MIN_VIA_STANDOFF_FROM_PAD_MM = 1.05

# H3i U1-to-J2 crossing placement (see `_route_usb_device_pair`): half the
# span of the short local "swap leg" the pair's one unavoidable crossing
# happens on, bracketed on either side by a plain coupled-pitch extension --
# keeps the crossing's own diagonal (and therefore the order-swap point
# `_dip_across_paths` finds) localised to that short span around the run's
# own midpoint, rather than the shape `_route_crossing_leg` would otherwise
# produce hugging one of the leg's two real endpoints (U1's fan-out or J2)
# when handed the endpoints directly.
U1_J2_CROSSING_HALF_SPAN_MM = 1.5

# Pitch the U1-to-J2 pair fans out to for the crossing itself (see
# `_route_usb_device_pair`), replacing the coupled USB90 pitch (~0.4 mm) for
# just that local span. At the coupled pitch the crossing's own transition
# via sits close enough to the *other*, undipped line's own diagonal to
# violate its real clearance -- measured directly (a `kicad-cli pcb drc`
# `clearance` finding against the via, not just a same-layer track gap
# `_route_crossing_leg`'s own self-check ever samples): the via's real floor
# against foreign copper is a via radius (0.3 mm) plus the netclass gap
# (0.15 mm) plus a track half-width (0.1255 mm), ~0.58 mm, and at the
# coupled pitch the perpendicular distance from the via to the other line's
# own diagonal is only pitch/sqrt(2) =~ 0.28 mm -- short regardless of the
# dip's own margin, since growing the margin only moves the via *along* the
# other line's diagonal, never away from it. Doubling the pitch to 2 mm
# during the crossing (the same 45-degree fan-out/fan-in primitive as the
# U1 GND-clearance fan above) makes that same distance ~1.41 mm, comfortably
# past the 0.58 mm floor.
U1_J2_CROSSING_PITCH_MM = 2.0

# Standoff for the U1-to-J2 crossing's own transition via, overriding
# `_route_crossing_leg`'s default (`MIN_VIA_STANDOFF_FROM_PAD_MM`, sized for
# a through-hole neighbour): this crossing's own endpoints (`wide_p_lo`/
# `wide_n_lo`/`wide_p_hi`/`wide_n_hi` in `_route_usb_device_pair`) are
# synthetic waypoints, not real pads, so there is no drilled hole nearby to
# protect -- zero lets the dip cover the order-swap diagonal from its own
# first point rather than clamping past a real portion of it.
U1_J2_CROSSING_VIA_STANDOFF_MM = 0.0

# H3h J1 via-free mirror-pin bridge (see `_route_j1_via_free_bridge`): each of
# J1's two bridges dips off the row's own pad-height span entirely -- one
# below (D-, connector side, -Y) and one above (D+, board side, +Y) -- rather
# than sharing the row's own height, where the two bridges' B6/A7/A6/B7
# interleaving previously forced a crossing (a straight A7-B7 stub also
# shorts against A6's own pad, which sits exactly on that stub's midpoint --
# see the README's own "not converted" note for the arithmetic). Putting the
# two dips on *opposite* sides of the row means neither ever occupies the
# other's territory, so the crossing this connector's interleaving otherwise
# forces never actually has to happen in the plane. Standoff past the row's
# own pad-tip coordinate (half the 1.15 mm pad height) that the dip's flat
# bottom needs to clear a *neighbouring* row pad's own tip (A6's tip for the
# D- dip, A7's tip for the D+ dip, both directly in the flat's path since
# each dip spans two mirror pins one pin-pitch apart): the 0.15 mm netclass
# edge gap plus the 0.1255 mm track half-width is 0.2755 mm minimum; 0.45 mm
# gives comfortable measured margin (~0.32 mm actual, confirmed by a real
# `kicad-cli pcb drc` run) without extending so far the dip's own vertical
# run collides with anything else on the row's far side.
J1_BRIDGE_TIP_STANDOFF_MM = 0.45

# Horizontal inset fraction (of the bridge stub's own span) for each end's
# 45-degree corner in `_detour_around_pad_row` -- deliberately much smaller
# than that function's own general-purpose 0.4 default, so almost the entire
# 1 mm span is flat, full-depth bow rather than a short 45-degree corner near
# the row (the same reasoning the prior via-based bridge's own
# now-removed BRIDGE_CROSSING_INSET_FRACTION used).
J1_BRIDGE_INSET_FRACTION = 0.02

# Each pair: bare P/N net names, its netclass, the ordered pad-to-pad hop list
# shared by both sides (same length, same order -- element i is "the same
# physical waypoint" on the P and N side), and any mirror-pin bridge pads that
# tie onto the first hop instead of running their own coupled leg (USB-C's
# orientation-flip twin D+/D- pins).
PAIR_SPECS = [
    {
        # J1's stacked A/B mirror pins carry identical nets but opposite
        # left/right order (A6 right of A7, DM-left/DP-right; B6 left of B7,
        # DP-left/DM-right). U1 sits at 0 degrees (scaffold_pcb.py PLACEMENT
        # -- a symmetric ESD chip with no directional/mechanical meaning) so
        # its own pins 1/2 read DP-left/DM-right too -- matching B6/B7, not
        # A6/A7 -- so B6/B7 (not A6/A7) is this pair's primary hop, staying
        # coupled with zero vias, zero crossings all the way to U1, and A6/A7
        # (the opposite-handed mirror pins) is the bridge -- via-free (H3h,
        # see `_route_j1_via_free_bridge`); each bridge only boxes in the
        # *other* mirror pin (A7 for the D+ bridge, A6 for the D- bridge),
        # never the new primary pad itself, so this leg's own launch needs no
        # workaround. J2's own fixed pad order (DM-left/DP-right) is the
        # opposite handedness from U1's, so the pair's one unavoidable
        # crossing lands on the open U1-to-J2 run instead (see
        # `_route_usb_device_pair`'s own comment) -- an inherent USB-C
        # topology fact (someone in this J1-U1-J2 chain must reverse), not
        # fixable by rotation, just relocatable to the least-congested leg.
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
        # H3j (2026-09-12): routed before ETH TX -- RX's own leg needs the
        # disclosed crossing fallback (`_route_crossing_leg_avoiding_self_
        # cross`), which has no obstacle awareness at all (not even real
        # board pads, let alone a sibling pair's own track), while TX's own
        # ordinary leg goes through `_route_leg_avoiding_pads`'s ordinary
        # obstacle-aware retry. Routing RX first and folding its own
        # finalized track into every later pair's own obstacles
        # (`sibling_pair_obstacles` in `main`) lets TX's own retry dodge RX,
        # the only direction this module's own obstacle mechanisms can act
        # on -- confirmed necessary by a real `kicad-cli pcb drc`
        # `shorting_items` finding between ETH_TXN and ETH_RXP/ETH_RXN when
        # TX was routed first instead.
        "label": "ETH RX",
        "p_net": "ETH_RXP", "n_net": "ETH_RXN", "class": "Eth100",
        "p_hops": [("J20", "3"), ("J18", "1")],
        "n_hops": [("J20", "6"), ("J18", "2")],
    },
    {
        "label": "ETH TX",
        "p_net": "ETH_TXP", "n_net": "ETH_TXN", "class": "Eth100",
        "p_hops": [("J20", "1"), ("J18", "6")],
        "n_hops": [("J20", "2"), ("J18", "5")],
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


def _line_intersection(point_a: Pt, direction_a: Pt, point_b: Pt, direction_b: Pt) -> Pt | None:
    """Intersection of two infinite lines, each given as a point + unit
    direction. Returns None for (near-)parallel lines."""
    denominator = direction_a.x * direction_b.y - direction_a.y * direction_b.x
    if abs(denominator) < 1e-9:
        return None
    diff = point_b - point_a
    t = (diff.x * direction_b.y - diff.y * direction_b.x) / denominator
    return point_a + direction_a * t


def _offset_polyline(centerline: list[Pt], side_sign: float, half_pitch_mm: float) -> list[Pt]:
    """Offset `centerline` by half_pitch_mm perpendicular to each segment's
    own direction (side_sign chooses which of the two perpendiculars),
    joining consecutive offset segments at their exact line-line
    intersection (a true mitre) rather than concatenating both segments' own
    endpoints. The naive concatenation leaves a short chord across the
    inside of every bend that cuts the corner closer to the centerline than
    half_pitch_mm -- measured on this board's actual bends, that chord
    narrowed the gap to the paired net's own offset line from the intended
    0.15 mm down to ~0.09-0.12 mm actual, a real `kicad-cli pcb drc`
    clearance violation on every routed pair. A true mitre keeps every
    point of the offset line at exactly half_pitch_mm from the centerline,
    so P and N stay separated by exactly one pair pitch along the whole
    leg; safe here because every centerline this script builds bends at
    45/90 degrees only, never sharply enough for a mitre point to invert."""
    directions = [_unit(end - start) for start, end in zip(centerline, centerline[1:])]
    offset_vectors = [_perp(direction) * (side_sign * half_pitch_mm) for direction in directions]
    points = [centerline[0] + offset_vectors[0]]
    for index in range(len(directions) - 1):
        line_a_point = centerline[index] + offset_vectors[index]
        line_b_point = centerline[index + 1] + offset_vectors[index + 1]
        joint = _line_intersection(line_a_point, directions[index], line_b_point, directions[index + 1])
        points.append(joint if joint is not None else centerline[index + 1] + offset_vectors[index])
    points.append(centerline[-1] + offset_vectors[-1])
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


def _insert_lateral_tooth(points: list[Pt], segment_index: int, amplitude_mm: float, side_sign: float) -> list[Pt]:
    """Insert one square-cornered 45-degree tooth into `points`' segment at
    `segment_index`, deviating by `amplitude_mm` perpendicular to that
    segment's own direction (`side_sign` picks which of the two
    perpendiculars). Every new segment this produces is a 45- or 90-degree
    leg -- this is the one primitive both `_add_serpentine` (an amplitude
    solved from a target *length* addition) and
    `_route_leg_avoiding_pads`'s obstacle-avoidance retry (an amplitude
    solved from a target *lateral clearance*) build on, so every detour this
    script ever inserts stays on the same straight-segments,
    15-degree-multiple-bends grid as the rest of its geometry.

    The tooth's two new points are anchored `amplitude_mm` along `direction`
    from `start`/`end` themselves (not from the segment's midpoint by
    `amplitude_mm` each): anchoring from the midpoint only produces a true
    45-degree entry/exit leg when amplitude_mm happens to equal exactly
    length/4, since the entry leg's along-direction component is then
    length/2 - amplitude_mm while its perpendicular component is
    amplitude_mm -- unequal components at any other amplitude, so the
    "45-degree leg" the docstring above promises was silently an arbitrary
    angle whenever the caller's needed amplitude landed elsewhere. Anchoring
    from `start`/`end` makes the entry and exit legs exactly
    amplitude_mm*sqrt(2) each at exactly 45 degrees for every amplitude up
    to length/2, with the straight run between them exactly parallel to the
    original direction (length - 2*amplitude_mm long) -- matching the
    "two 45-degree legs of length a*sqrt(2)" this module has always
    documented, now actually true of the geometry it builds."""
    start, end = points[segment_index], points[segment_index + 1]
    length = (end - start).length()
    amplitude_mm = min(amplitude_mm, length / 2 - 1e-6)
    if amplitude_mm <= 0:
        raise ValueError("segment too short to absorb the required tooth")
    direction = _unit(end - start)
    perp = _perp(direction) * side_sign
    tooth = [
        start + direction * amplitude_mm + perp * amplitude_mm,
        end - direction * amplitude_mm + perp * amplitude_mm,
    ]
    return points[:segment_index + 1] + tooth + points[segment_index + 1:]


def _add_serpentine(points: list[Pt], extra_needed_mm: float, side_sign: float,
                     width_mm: float | None = None, obstacles: list[tuple[Pt, float]] | None = None,
                     clearance_mm: float = 0.15) -> list[Pt]:
    """Insert one 45-degree meander tooth into a straight run of `points` to
    add exactly extra_needed_mm of path length, deviating away from the
    paired net. `side_sign` is this net's own offset sign from `_build_leg`
    (the same sign `_offset_polyline` used to place it on its side of the
    centerline) -- deviating *further* along that same perpendicular moves
    away from centerline and therefore away from the other net, which sits
    on the opposite (-side_sign) side. A single square-cornered tooth of
    amplitude `a` replacing a straight run of length L adds
    2*a*(sqrt(2)-1) of extra length (two 45-degree legs of length a*sqrt(2)
    each replacing a total run of 2*a along the original direction); solving
    for a gives the amplitude needed.

    When `obstacles`/`width_mm` are given (H3i: a real foreign pad -- e.g.
    J14's own mounting pad -- can sit directly in the longest run's own
    path), tries every run longest-first rather than only the single
    longest, keeping the first violation-free tooth found or, failing that,
    the least-bad one tried (this module's own bounded-iterations
    convention) -- a serpentine landing on a real pad is exactly as real a
    short as any other track doing so, not a style residual."""
    lengths = sorted(
        ((index, (b - a).length()) for index, (a, b) in enumerate(zip(points, points[1:]))),
        key=lambda item: item[1], reverse=True)
    tooth_extra_per_mm_amplitude = 2 * (math.sqrt(2) - 1)
    amplitude_needed = extra_needed_mm / tooth_extra_per_mm_amplitude
    # Two independent "best" trackers -- a full-capacity candidate (one
    # whose own run is long enough to add the entire extra_needed_mm) is
    # always preferred over a capped one, however small its own violation,
    # since a capped tooth silently leaves most of the required skew
    # uncorrected even when it clears every obstacle outright (measured: a
    # naive single "lowest shortfall wins" comparison picked a capped tooth
    # on J14's own tiny 0.4 mm lead segment over a full-capacity run with a
    # small residual, correcting only ~6% of the needed length).
    best_full_candidate: list[Pt] | None = None
    best_full_shortfall: float | None = None
    best_partial_candidate: list[Pt] | None = None
    best_partial_shortfall: float | None = None
    for index, longest in lengths:
        amplitude = min(amplitude_needed, longest / 2 - 1e-6)
        if amplitude <= 0:
            continue
        candidate = _insert_lateral_tooth(points, index, amplitude, side_sign)
        if obstacles is None or width_mm is None:
            return candidate
        violation = _first_pad_violation(candidate, width_mm, obstacles, clearance_mm)
        shortfall = 0.0 if violation is None else violation[2]
        is_full_capacity = amplitude >= amplitude_needed - 1e-9
        if violation is None and is_full_capacity:
            return candidate
        if is_full_capacity:
            if best_full_shortfall is None or shortfall < best_full_shortfall:
                best_full_shortfall, best_full_candidate = shortfall, candidate
        elif best_partial_shortfall is None or shortfall < best_partial_shortfall:
            best_partial_shortfall, best_partial_candidate = shortfall, candidate
    if best_full_candidate is not None:
        return best_full_candidate
    if best_partial_candidate is not None:
        return best_partial_candidate
    raise ValueError("no straight run long enough to absorb the required serpentine")


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


# A per-hop straight-displacement exit vector (see `_exit_point`), keyed by
# hop reference designator, for a pad sitting on a row too crowded to bend
# diagonally off immediately -- a lead that turns diagonally right off such a
# pad sweeps sideways across the row's own neighbouring pads at close range;
# leaving straight first, until clear of the row's own height, avoids that.
# Empty today: J1's own row exit (the one hop that needed this) is now built
# bespoke in `_route_usb_device_pair` instead (A6/D+'s own exit is a plain +Y
# vector; A7/D-'s own leg launches from B7, its own mirror pin, instead of a
# displaced exit vector -- see that function's own comment for why), since
# the two nets now need genuinely different treatment and this dict's own
# mechanism applies one vector identically to both. U1's own GND-vs-D+/D-
# pin congestion was tried against this same
# mechanism and reverted: a forced exit direction for one leg's travel
# (toward J1) opposes the other leg's (toward J2), and `_exit_point`'s
# displacement only holds for a short inset before `_build_leg` re-converges
# the offset polyline onto the raw pad-to-pad centerline (by design, so the
# coupled line does not permanently detour off a long leg for a short-range
# pad-row problem) -- U1's own pin 3 sits far enough along the leg (0.85 mm
# past pins 1/2) that the re-convergence undoes the shift before it clears
# pin 3, measured no improvement over the retry loop's own convergence
# (`_route_leg_avoiding_pads`, which already treats pin 3 as a foreign-pad
# obstacle) at any exit vector tried. U1's residual is reported rather than
# forced through an exit direction; see the module docstring's
# residual-findings note. Kept as a live mechanism (not deleted) since a
# future crowded-row hop can still key into it.
PAD_ROW_EXIT_DIRECTION: dict[str, Pt] = {}


def _exit_point(pad: Pt, exit_direction: Pt | None) -> Pt:
    """`pad`, or `pad + exit_direction` when given -- see
    PAD_ROW_EXIT_DIRECTION. Used for *both* the centerline's own
    hop (so the coupled offset line only ever starts past the crowded row)
    and the lead back down to the real pad, so the two agree and a lead
    never has to double back toward the row to reach a still-too-close
    offset-line start."""
    return pad if exit_direction is None else pad + exit_direction


def _symmetric_pad_approach_fan(offset_end: Pt, pad: Pt, direction: Pt) -> list[Pt]:
    """H3i symmetric pad-approach fan: the final lead between a coupled
    offset line's own end (`offset_end`, still at the pair's coupled pitch)
    and the real pad it terminates at (`pad`, at that pad's own native
    pitch), when the two pitches differ. Decomposing `pad - offset_end`
    into its `direction`-parallel and perpendicular components gives the
    along-leg distance and the pitch difference for *this one line*; the
    perpendicular component's magnitude is exactly `pitch_difference / 2`
    for a true differential pair (both pads sit symmetric about the same
    centreline the coupled offset already used), so a single 45-degree leg
    of that same length closes it exactly -- the definition of a 45-degree
    bend is equal along/perpendicular run. Placing that leg immediately
    before `pad` (a straight run first, the 45-degree leg last) means the
    fan starts a distance `pitch_difference / 2` before the pad row, landing
    exactly on the pad's own axis, then continuing zero-length straight into
    the pad centre -- the docstring's own '~1 mm fan at <=2 mm pitch' shape.
    Called identically for both P and N (see `_leg_lead_from`/`_leg_lead_to`)
    -- each line's own perpendicular component is equal in magnitude and
    opposite in sign, producing the 'one symmetric fan' shape without any
    joint P/N computation. Falls back to the previous plain `_bend_path` when
    the pitches already match (nothing to fan) or the leg is too short to fit
    the fan's own straight run (a genuinely crowded hop -- unchanged from
    before this fix)."""
    diff = pad - offset_end
    perp = _perp(direction)
    along = diff.x * direction.x + diff.y * direction.y
    perp_component = diff.x * perp.x + diff.y * perp.y
    fan_length = abs(perp_component)
    if fan_length < 1e-6 or abs(along) < fan_length - 1e-9:
        return _bend_path(offset_end, pad)
    transition = offset_end + direction * (along - math.copysign(fan_length, along))
    return _dedup([offset_end, transition, pad])


def _lead_avoiding_pads(offset_end: Pt, pad: Pt, direction: Pt,
                         obstacles: list[tuple[Pt, float]] | None, width_mm: float | None,
                         clearance_mm: float, max_attempts: int = 20) -> list[Pt]:
    """`_symmetric_pad_approach_fan`, plus a bounded retry (this module's own
    convention) that grows a lateral tooth into the straight `offset_end`-to-
    `pad` chord when the fan's own shape clips a real foreign pad -- e.g.
    U10's own UHDM/UH5V leads sitting close enough to USBH_DP/USBH_DM's own
    approach for a real `kicad-cli pcb drc` `shorting_items`/`solder_mask_
    bridge` finding, not just a tight clearance (design decision, 2026-09-12:
    the fan need not stay symmetric or short, only >= clearance_mm from
    everything). `obstacles`/`width_mm` are optional -- omitted, this is
    exactly `_symmetric_pad_approach_fan` (used by the `_leg_lead_from`/
    `_leg_lead_to` departure/arrival wrappers before any obstacle is
    known)."""
    points = _symmetric_pad_approach_fan(offset_end, pad, direction)
    if obstacles is None or width_mm is None:
        return points
    chord = [offset_end, pad]
    amplitude = 0.0
    side = 1.0
    best_points, best_shortfall = points, None
    for _attempt in range(max_attempts):
        violation = _first_pad_violation(points, width_mm, obstacles, clearance_mm)
        shortfall = 0.0 if violation is None else violation[2]
        if best_shortfall is None or shortfall < best_shortfall:
            best_shortfall, best_points = shortfall, points
        if violation is None:
            return points
        obstacle_center, _closest_sample, _ = violation
        perp = _perp(direction)
        to_obstacle = obstacle_center - _midpoint(offset_end, pad)
        obstacle_side = 1.0 if (to_obstacle.x * perp.x + to_obstacle.y * perp.y) >= 0 else -1.0
        side = -obstacle_side
        amplitude = min(amplitude + shortfall + width_mm / 2 + clearance_mm + 0.1, MAX_AVOIDANCE_TOOTH_MM)
        try:
            points = _insert_lateral_tooth(chord, 0, amplitude, side)
        except ValueError:
            break
    return best_points


def _leg_lead_from(pad: Pt, exit_point: Pt, target: Pt, direction: Pt,
                    obstacles: list[tuple[Pt, float]] | None = None, width_mm: float | None = None,
                    clearance_mm: float = 0.15) -> list[Pt]:
    """Path from `pad` to `target`, straight to `exit_point` first when it
    differs from `pad` (see `_exit_point`); otherwise a symmetric
    pad-approach fan (`_lead_avoiding_pads`, reversed -- the fan lands at
    `pad`, the departure end, rather than at `target`), grown clear of any
    given foreign-pad `obstacles`."""
    if exit_point.x == pad.x and exit_point.y == pad.y:
        return list(reversed(_lead_avoiding_pads(target, pad, direction * -1.0, obstacles, width_mm, clearance_mm)))
    return _dedup([pad] + _bend_path(exit_point, target))


def _leg_lead_to(source: Pt, exit_point: Pt, pad: Pt, direction: Pt,
                  obstacles: list[tuple[Pt, float]] | None = None, width_mm: float | None = None,
                  clearance_mm: float = 0.15) -> list[Pt]:
    """Mirror of `_leg_lead_from` for a path arriving at `pad`."""
    if exit_point.x == pad.x and exit_point.y == pad.y:
        return _lead_avoiding_pads(source, pad, direction, obstacles, width_mm, clearance_mm)
    return _dedup(_bend_path(source, exit_point) + [pad])


# Extra straight-chord margin `_leg_centerline` adds on top of a fan's own
# pitch-difference requirement when sizing that end's own inset -- see the
# inset computation's own comment for why a chord sized to the fan alone
# leaves `_lead_avoiding_pads`'s own perpendicular-tooth retry too little
# amplitude headroom against a real, tightly-spaced destination pad.
#
# H3i re-verification (2026-09-12): reverted from 1.0 back to 0.0 (this
# constant's own pre-existing default) after a real `check_routing_geometry.py`
# run against the USB device pair's own U1-to-J2 leg found 7 off-grid
# bend/angle findings at the narrowed-leg-to-J2 approach -- the wider inset
# this margin forces there gives `_route_leg_avoiding_pads` a longer raw
# chord to retry against, and `_lead_avoiding_pads`'s own tooth insertion
# only stays on the 15-degree grid when the chord it inserts into already
# is (`_insert_lateral_tooth`'s own docstring), which a raw pad-to-fan chord
# is not guaranteed to be. At 0.0 the same pair re-routes DRC-clean (0
# violations) and geometry-clean (0 arcs / 0 off-grid) standalone. This
# constant was sized for the USB host leg's own approach to U10's tightly
# packed UHDP/UHDM/UH5V column (see the inset computation's own comment) --
# moot under the H3i default scope (`--pairs usb-device` only), since that
# leg is no longer hand-routed by default; re-enable non-zero only alongside
# re-verifying `--pairs usb-host` standalone.
LEAD_OBSTACLE_MARGIN_MM = 0.0


def _leg_centerline(p_a: Pt, p_b: Pt, n_a: Pt, n_b: Pt, half_pitch_mm: float,
                     exit_a: Pt | None, exit_b: Pt | None
                     ) -> tuple[list[Pt], float, Pt, Pt]:
    """The centerline + side_sign portion of `_build_leg`, factored out so
    `_route_leg_avoiding_pads` can locate an obstacle against the same base
    geometry every retry targets a tooth into (see `_insert_lateral_tooth`),
    without re-deriving the exit-point/inset/bend-shape logic itself. Also
    returns the centerline's own near/far segment directions -- the leg's
    direction of travel at each end -- so `_build_leg` can hand them to
    `_symmetric_pad_approach_fan` for that end's own pad-approach lead."""
    exit_p_a, exit_n_a = _exit_point(p_a, exit_a), _exit_point(n_a, exit_a)
    exit_p_b, exit_n_b = _exit_point(p_b, exit_b), _exit_point(n_b, exit_b)
    base_hop_a, base_hop_b = _midpoint(exit_p_a, exit_n_a), _midpoint(exit_p_b, exit_n_b)
    leg_vector = base_hop_b - base_hop_a
    leg_length = leg_vector.length()

    def _resolve(hop_a: Pt, hop_b: Pt) -> tuple[list[Pt], float, Pt, Pt] | None:
        for diagonal_first in (True, False):
            candidate = _bend_path(hop_a, hop_b, diagonal_first)
            near_direction = _unit(candidate[1] - candidate[0])
            far_direction = _unit(candidate[-1] - candidate[-2])
            near_sign = _side_sign_at(p_a, candidate[0], near_direction)
            far_sign = _side_sign_at(p_b, candidate[-1], far_direction)
            if near_sign == far_sign:
                return candidate, near_sign, near_direction, far_direction
        return None

    hop_a, hop_b = base_hop_a, base_hop_b
    if leg_length > 1e-6:
        base_inset = min(2 * half_pitch_mm, leg_length * 0.3)
        direction = _unit(leg_vector)
        hop_a = base_hop_a + direction * base_inset
        hop_b = base_hop_b - direction * base_inset

    resolved = _resolve(hop_a, hop_b)
    if resolved is None:
        raise ValueError(
            "no 45-degree bend keeps this leg's P/N handedness consistent end to end -- "
            "the pads' physical winding needs an actual conductor crossing (a via) on this corridor")

    if leg_length > 1e-6:
        # H3i symmetric pad-approach fan: reserve enough straight lead room
        # at whichever end has a real pin-pitch mismatch for
        # `_symmetric_pad_approach_fan`'s own 45-degree legs to actually
        # fit, not just the flat `2 * half_pitch_mm` `base_inset` above uses
        # -- that flat value left only ~0.5 mm of lead on ETH TX's own
        # approach to J18 (2 mm native pitch vs ~0.35 mm coupled), short of
        # the ~0.82 mm the fan needs, so the fan's own
        # `abs(along) < fan_length` guard silently fell back to the old
        # naive `_bend_path` lead there -- measured directly (a real
        # `kicad-cli pcb drc` 0.047 mm self-clearance finding at the
        # unfanned approach). Each end's own native half-pitch, read off its
        # real pads, sizes that end's own inset independently. Re-verified
        # against the same handedness check `base_inset` already passed
        # (never assumed): a wider inset shortens the centerline enough, on
        # some corridors, to flip which 45-degree bend shape keeps P/N
        # consistent, or to spuriously pass a corridor that actually needs a
        # via crossing (measured directly on ETH RX) -- keep the wider inset
        # only when it re-confirms the same non-crossing verdict.
        # The extra `+ LEAD_OBSTACLE_MARGIN_MM` beyond the fan's own
        # pitch-difference requirement gives `_lead_avoiding_pads`'s own
        # perpendicular-tooth retry a long enough chord to actually clear a
        # real foreign pad near the destination -- `_insert_lateral_tooth`
        # caps its own amplitude at half the chord length, so a chord sized
        # to the fan alone (e.g. ~1.1 mm approaching U10's own UHDP/UHDM,
        # each a large 1.7x1.7 mm THT pad needing ~1.48 mm real clearance
        # from a neighbour on the same column) can never reach the
        # amplitude a genuinely tight destination needs, independent of how
        # far `MAX_AVOIDANCE_TOOTH_MM` itself allows -- measured directly (a
        # real `kicad-cli pcb drc` `shorting_items` finding against U10's
        # own UHDM pad, unchanged by raising the global cap alone).
        native_half_a = (p_a - n_a).length() / 2
        native_half_b = (p_b - n_b).length() / 2
        inset_a = min(max(2 * half_pitch_mm, abs(native_half_a - half_pitch_mm) + LEAD_OBSTACLE_MARGIN_MM),
                       leg_length * 0.3)
        inset_b = min(max(2 * half_pitch_mm, abs(native_half_b - half_pitch_mm) + LEAD_OBSTACLE_MARGIN_MM),
                       leg_length * 0.3)
        if inset_a > base_inset or inset_b > base_inset:
            direction = _unit(leg_vector)
            wide_hop_a = base_hop_a + direction * inset_a
            wide_hop_b = base_hop_b - direction * inset_b
            wide_resolved = _resolve(wide_hop_a, wide_hop_b)
            if wide_resolved is not None:
                resolved = wide_resolved

    return resolved


def _build_leg(p_a: Pt, p_b: Pt, n_a: Pt, n_b: Pt, half_pitch_mm: float,
                exit_a: Pt | None = None, exit_b: Pt | None = None,
                avoidance_teeth: dict[int, tuple[float, float]] | None = None,
                width_mm: float | None = None, clearance_mm: float = 0.15,
                p_obstacles: list[tuple[Pt, float]] | None = None,
                n_obstacles: list[tuple[Pt, float]] | None = None
                ) -> tuple[list[Pt], list[Pt], float]:
    """One coupled leg between two hops: centerline from the P/N midpoints,
    offset +-half_pitch_mm per segment, then a lead from each real pad
    position to its polyline end (straight along `exit_a`/`exit_b` first,
    see PAD_ROW_EXIT_DIRECTION, when the hop sits on a crowded pad row).
    Returns (p_points, n_points, side_sign) -- side_sign is which offset
    side the P polyline ended up on, needed so a later serpentine deviates
    outward rather than into the coupled partner.

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

    The centerline's own hop is built from each end's *exit point*
    (`_exit_point`: the real pad when there is no forced row-exit direction
    for that end, or a further displacement along the row-exit direction)
    rather than the raw pad position, then inset a further
    2*half_pitch_mm along the leg direction before offsetting. Building the
    hop from the exit point, not the pad, is what keeps the coupled offset
    line clear of a crowded pad row in the first place -- computing the hop
    from the raw pad and only detouring the *lead* through the exit point
    still starts the offset line back at the row's own height, forcing the
    lead to double back toward the row to reach it (measured: exactly the
    zig-zag this script produced before hops were rebuilt from exit points).
    The further per-leg inset alone is not enough for a genuinely crowded
    row: J1's own USB-C high-speed pads measure 0.3 x 1.15 mm on a single
    row within ~1.5 mm of each other, and 2*half_pitch_mm (a fraction of a
    mm at these netclass pitches) does not clear that row's own height.

    `avoidance_teeth`, when given, maps a *base*-centerline segment index to
    its own `(amplitude_mm, side_sign)` -- the caller's bounded retry loop
    (`_route_leg_avoiding_pads`) inserts a square 45-degree tooth
    (`_insert_lateral_tooth`) into each named segment before offsetting, to
    walk the corridor away from a foreign pad it would otherwise clip,
    without changing which side P/N ends up on or which pads the leg
    actually terminates at. More than one entry is real: a leg can have two
    obstacles near its two different base segments, and fixing one alone
    can swing the *other* segment into the other obstacle -- measured
    oscillating forever between two single-segment fixes on ETH TX's own
    leg before this became a dict. Applied highest segment index first, so
    an earlier insertion's two new points never shift a not-yet-processed
    segment's own index. Every point this produces stays on the
    straight-segments, 15-degree-multiple-bends grid -- unlike an earlier
    arbitrary bend-point nudge this replaced, which could tilt the leg's
    own segments off that grid entirely.

    `width_mm`/`clearance_mm`/`p_obstacles`/`n_obstacles`, when given, make
    each end's own pad-approach lead obstacle-aware too (`_leg_lead_from`/
    `_leg_lead_to`'s own `_lead_avoiding_pads`) -- the interior avoidance
    teeth above never reach the lead itself (a fixed function of the pad and
    the offset line's own endpoint), so a real foreign pad sitting close to
    a fanned-out approach (U10's own UHDM/UH5V leads against USBH_DP/
    USBH_DM) needs this second, independent mechanism."""
    exit_p_a, exit_n_a = _exit_point(p_a, exit_a), _exit_point(n_a, exit_a)
    exit_p_b, exit_n_b = _exit_point(p_b, exit_b), _exit_point(n_b, exit_b)
    centerline, side_sign, near_direction, far_direction = _leg_centerline(
        p_a, p_b, n_a, n_b, half_pitch_mm, exit_a, exit_b)
    if avoidance_teeth is not None:
        for segment_index in sorted(avoidance_teeth, reverse=True):
            amplitude_mm, tooth_side = avoidance_teeth[segment_index]
            centerline = _insert_lateral_tooth(centerline, segment_index, amplitude_mm, tooth_side)

    p_offset = _offset_polyline(centerline, side_sign, half_pitch_mm)
    n_offset = _offset_polyline(centerline, -side_sign, half_pitch_mm)

    p_points = _dedup(_leg_lead_from(p_a, exit_p_a, p_offset[0], near_direction,
                                      p_obstacles, width_mm, clearance_mm)[:-1]
                       + p_offset
                       + _leg_lead_to(p_offset[-1], exit_p_b, p_b, far_direction,
                                      p_obstacles, width_mm, clearance_mm)[1:])
    # N's own lead-avoidance obstacles fold in P's own just-finalized points
    # too (`_polyline_obstacles`, computed first, above) -- each side's own
    # perpendicular-tooth retry only ever knows about *foreign* pads
    # (`_lead_avoiding_pads` never sees its own coupled partner), so both
    # sides independently pushing away from the same nearby foreign pad can
    # converge toward *each other* instead (measured: a real `kicad-cli pcb
    # drc` self-clearance short between P's and N's own leads at U10, each
    # individually clear of every foreign pad).
    n_obstacles_with_p = n_obstacles
    if width_mm is not None:
        n_obstacles_with_p = (n_obstacles or []) + _polyline_obstacles(p_points, width_mm)
    n_points = _dedup(_leg_lead_from(n_a, exit_n_a, n_offset[0], near_direction,
                                      n_obstacles_with_p, width_mm, clearance_mm)[:-1]
                       + n_offset
                       + _leg_lead_to(n_offset[-1], exit_n_b, n_b, far_direction,
                                      n_obstacles_with_p, width_mm, clearance_mm)[1:])
    return p_points, n_points, side_sign


def _first_pad_violation(points: list[Pt], width_mm: float, obstacles: list[tuple[Pt, float]],
                          clearance_mm: float, sample_step_mm: float = 0.15
                          ) -> tuple[Pt, Pt, float] | None:
    """Sample `points` (a track's own centreline) against every `obstacles`
    entry (a foreign pad's real center + its own circumscribing radius --
    see `_pad_obstacle_radius_mm`) and return the worst (obstacle_center,
    closest_sample_point, shortfall_mm) found, or None if every sample
    clears `clearance_mm`. The closest sample point (not just the obstacle
    center) is what a caller needs to compute a correct push-away direction
    -- the obstacle center alone says nothing about which local direction
    the violating segment actually approaches it from."""
    half_width = width_mm / 2
    worst: tuple[Pt, Pt, float] | None = None
    for start, end in zip(points, points[1:]):
        length = (end - start).length()
        if length < 1e-6:
            continue
        steps = max(1, int(length / sample_step_mm))
        for step in range(steps + 1):
            t = step / steps
            sample = Pt(start.x + t * (end.x - start.x), start.y + t * (end.y - start.y))
            for center, radius in obstacles:
                shortfall = clearance_mm - ((sample - center).length() - half_width - radius)
                if shortfall > 0 and (worst is None or shortfall > worst[2]):
                    worst = (center, sample, shortfall)
    return worst


# Bound on how far `_route_leg_avoiding_pads`'s own retry loop grows one
# avoidance tooth before giving up on this obstacle and keeping its
# least-bad attempt (this project's own "bounded iterations, stop at
# diminishing returns" convention, H3c) -- a genuinely blocked corridor
# should stop growing rather than produce an absurdly large detour. Raised
# from 4.8 (design decision, 2026-09-12: coupling gap/uncoupled-section length
# are no longer acceptance criteria, so a larger, simpler detour that
# actually clears a real pad -- e.g. U10's own tightly-spaced UHDP/UHDM/
# UH5V row -- beats a tighter one that still shorts).
MAX_AVOIDANCE_TOOTH_MM = 12.0


def _route_leg_avoiding_pads(p_a: Pt, p_b: Pt, n_a: Pt, n_b: Pt, half_pitch_mm: float, width_mm: float,
                              p_obstacles: list[tuple[Pt, float]], n_obstacles: list[tuple[Pt, float]],
                              clearance_mm: float, exit_a: Pt | None = None, exit_b: Pt | None = None,
                              max_attempts: int = 40) -> tuple[list[Pt], list[Pt], float]:
    """`_build_leg`, plus a bounded retry loop (this project's own "bounded
    iterations, stop at diminishing returns" convention, H3c) that inserts a
    growing `_insert_lateral_tooth` detour into the *base* centerline's
    segment nearest an obstacle pad the initial attempt clips.
    `p_obstacles`/`n_obstacles` are deliberately separate -- P's path must
    dodge every pad on any *other* net including N's own hop pads (and vice
    versa): a P/N pair excluding both nets together would let P's track run
    straight through N's own destination pad, since it is "the pair's own
    net" but not P's. Real finding: exactly this shorted ETH_TXN across
    J18's TXP pad before the exclusion was split per-side.

    Every retry targets the same base centerline (from `_leg_centerline`,
    computed once) rather than compounding onto the previous attempt's
    result -- an earlier version nudged an arbitrary point by an arbitrary
    vector each retry, which (a) could tilt the leg's own segments off the
    straight-segments/15-degree-bends grid entirely, and (b) let a small
    nudge near one end of a long leg swing the far end across several
    unrelated components (measured on this pair's own ~26 mm leg to J2).
    A single growing tooth at a fixed, obstacle-nearest segment avoids both:
    every new segment stays 45-degree-consistent by construction, and the
    tooth's own amplitude is the only thing that grows. Each retry grows
    the tooth by exactly the worst shortfall found plus a small margin; the
    loop tracks its least-bad attempt across every retry rather than its
    last, since a genuinely blocked corridor can exhaust max_attempts
    without ever reaching zero shortfall, and the caller's own DRC-facing
    report should reflect the best geometry actually tried, not whichever
    happened to be computed last."""
    base_centerline, _base_side_sign, _base_near_direction, _base_far_direction = _leg_centerline(
        p_a, p_b, n_a, n_b, half_pitch_mm, exit_a, exit_b)
    avoidance_teeth: dict[int, tuple[float, float]] = {}
    best_shortfall: float | None = None
    best_points: tuple[list[Pt], list[Pt], float] | None = None
    for _attempt in range(max_attempts):
        try:
            p_points, n_points, leg_side_sign = _build_leg(
                p_a, p_b, n_a, n_b, half_pitch_mm, exit_a, exit_b, avoidance_teeth or None,
                width_mm, clearance_mm, p_obstacles, n_obstacles)
        except ValueError:
            # A tooth grew past its segment's own length (too short to
            # absorb it) -- stop growing and keep the best attempt found.
            break
        violation = (_first_pad_violation(p_points, width_mm, p_obstacles, clearance_mm)
                     or _first_pad_violation(n_points, width_mm, n_obstacles, clearance_mm))
        shortfall = 0.0 if violation is None else violation[2]
        if best_shortfall is None or shortfall < best_shortfall:
            best_shortfall, best_points = shortfall, (p_points, n_points, leg_side_sign)
        if violation is None:
            break
        obstacle_center, _closest_sample, _ = violation
        segment_index = min(
            range(len(base_centerline) - 1),
            key=lambda index: _point_to_segment_distance_mm(
                obstacle_center, base_centerline[index], base_centerline[index + 1]))
        start, end = base_centerline[segment_index], base_centerline[segment_index + 1]
        segment_direction = _unit(end - start)
        perp = _perp(segment_direction)
        to_obstacle = obstacle_center - _midpoint(start, end)
        obstacle_side = 1.0 if (to_obstacle.x * perp.x + to_obstacle.y * perp.y) >= 0 else -1.0
        tooth_side = -obstacle_side
        previous_amplitude = avoidance_teeth[segment_index][0] if segment_index in avoidance_teeth else 0.0
        amplitude_mm = previous_amplitude + shortfall + half_pitch_mm + 0.1
        if amplitude_mm > MAX_AVOIDANCE_TOOTH_MM:
            break
        avoidance_teeth[segment_index] = (amplitude_mm, tooth_side)
    return best_points  # type: ignore[return-value]  -- set on the loop's first (>= 1) iteration


def _pad_obstacle_radius_mm(pad) -> float:
    """A circumscribing-circle radius for `pad`'s real outline (accounting
    for a 90-degree pad rotation swapping width/height) -- a conservative
    stand-in for the pad's exact polygon that never under-counts clearance,
    used to keep a routed path off a foreign pad it isn't terminating at."""
    size = pad.GetSize()
    half_w, half_h = size.x / 2e6, size.y / 2e6
    if abs(pad.GetOrientationDegrees() % 180 - 90) < 1e-6:
        half_w, half_h = half_h, half_w
    return math.hypot(half_w, half_h)


def _foreign_pad_obstacles(board, exclude_bare_names: set[str], layer: int) -> list[tuple[Pt, float]]:
    """Every pad on `board` on `layer` whose net is not in
    `exclude_bare_names` (a through-hole pad is on every copper layer; an
    SMD pad only on its own assigned side), as (center, radius) obstacles
    for `_first_pad_violation`."""
    obstacles: list[tuple[Pt, float]] = []
    for footprint in board.GetFootprints():
        for pad in footprint.Pads():
            if not pad.IsOnLayer(layer):
                continue
            if _bare_net_name(pad.GetNetname()) in exclude_bare_names:
                continue
            obstacles.append((_pad_point(pad), _pad_obstacle_radius_mm(pad)))
    return obstacles


def _polyline_obstacles(points: list[Pt], width_mm: float, sample_step_mm: float = 0.2
                         ) -> list[tuple[Pt, float]]:
    """Sample an already-decided polyline (e.g. J1's own via-free bridge, see
    `_route_j1_via_free_bridge`) into a run of (centre, radius) circle
    obstacles of radius `width_mm / 2`, in the same shape `_foreign_pad_
    obstacles` returns -- lets `_route_leg_avoiding_pads`'s existing
    obstacle-avoidance retry loop treat *this script's own, not-yet-emitted*
    copper as an obstacle for a different net's leg, the same way it already
    treats a foreign pad. A single circumscribing circle over the whole
    polyline would over-block a long bow's own diagonal ends (see
    `_detour_around_pad_row`); a run of samples tracks the actual shape."""
    half_width = width_mm / 2
    obstacles: list[tuple[Pt, float]] = []
    for start, end in zip(points, points[1:]):
        length = (end - start).length()
        if length < 1e-6:
            continue
        steps = max(1, int(length / sample_step_mm))
        for step in range(steps + 1):
            t = step / steps
            obstacles.append((Pt(start.x + t * (end.x - start.x), start.y + t * (end.y - start.y)), half_width))
    return obstacles


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


def _route_crossing_leg(p_a: Pt, p_b: Pt, n_a: Pt, n_b: Pt, dip_margin_mm: float = 1.0,
                         p_diagonal_first: bool = True, via_standoff_mm: float = MIN_VIA_STANDOFF_FROM_PAD_MM
                         ) -> tuple[list[tuple[list[Pt], int]], list[tuple[list[Pt], int]]]:
    """Fallback for a leg (or mirror-pin bridge pair) where `_build_leg`
    cannot find a 45-degree bend keeping P/N handedness consistent end to
    end -- the pads' fixed relative left/right (or, for a mostly-horizontal
    bridge, near/far) order genuinely reverses between the two ends (a
    topological fact: two planar curves whose endpoint order swaps must cross
    somewhere), so no amount of bend-shape juggling on F.Cu alone resolves it.

    P keeps its plain 45-degree pad-to-pad path on F.Cu, uninterrupted
    (`p_diagonal_first` picks which of the two equally-valid 45-degree bend
    shapes -- `_route_crossing_leg_avoiding_self_cross` tries both, since
    one shape can route P's own bend point closer to N's near-pad lead than
    the other on a dense corridor like J20's pin field). N takes the same
    shape but dips to B.Cu for a short span bracketing the one point where
    the two paths swap order, then returns to F.Cu -- the minimum-possible
    via count (one pair) to make the crossing physically valid instead of
    two conductors touching on one layer. The scan axis is whichever of x/y
    the two paths together span more of (a vertical corridor scans by
    height; a horizontal bridge pair scans by x), found by comparing each
    path's own cross-axis coordinate at matching positions along that axis
    -- not by matching sample index, which does not correspond to the same
    physical location when the two paths differ in length or starting
    position. This is a deliberate, narrowly-scoped, disclosed exception to
    the 0-via routing goal for exactly this leg; see the script's docstring
    note and the run's printed disclosure line."""
    p_path = _bend_path(p_a, p_b, p_diagonal_first)
    n_path = _bend_path(n_a, n_b)
    # n_a/n_b are usually a real connector's own pins (through-hole or SMD
    # with a neighbour's drilled hole nearby) -- keep the full pad standoff
    # by default so a transition via never lands on a drilled hole.
    # `via_standoff_mm` is overridable for a short corridor or an SMD
    # endpoint with no nearby drill of its own to collide with, where the
    # default (sized for a through-hole neighbour) would clamp the dip's own
    # start well past where the two paths actually swap order.
    n_runs = _dip_across_paths(p_path, n_path, n_a, n_b, dip_margin_mm, via_standoff_mm=via_standoff_mm)
    return [(p_path, pcbnew.F_Cu)], n_runs


def _route_crossing_leg_avoiding_self_cross(p_a: Pt, p_b: Pt, n_a: Pt, n_b: Pt, width_mm: float,
                                             clearance_mm: float, max_attempts: int = 12,
                                             via_standoff_mm: float = MIN_VIA_STANDOFF_FROM_PAD_MM
                                             ) -> tuple[list[tuple[list[Pt], int]], list[tuple[list[Pt], int]]]:
    """`_route_crossing_leg`, plus a bounded retry loop that grows the dip's
    own margin when P's uninterrupted F.Cu path and N's dipped path still
    come too close outside the B.Cu span -- the same self-verification
    `_route_bridge_pair_avoiding_self_cross` applies, for the leg case.
    Tries both of P's own 45-degree bend shapes (see `_route_crossing_leg`)
    and keeps whichever converges to the smaller worst-case violation, since
    growing the dip margin alone cannot fix a near-pad lead that a
    dense connector's own pin field forces close to P's bend point
    regardless of the dip's bracket width."""
    best_p_runs: list[tuple[list[Pt], int]] = []
    best_n_runs: list[tuple[list[Pt], int]] = []
    best_gap: float | None = None
    for p_diagonal_first in (True, False):
        dip_margin_mm = 1.0
        p_runs = n_runs = []
        gap: float | None = None
        for _attempt in range(max_attempts):
            p_runs, n_runs = _route_crossing_leg(p_a, p_b, n_a, n_b, dip_margin_mm, p_diagonal_first, via_standoff_mm)
            gap = _runs_min_gap_mm(p_runs, n_runs, width_mm)
            if gap is None or gap >= clearance_mm:
                break
            dip_margin_mm += (clearance_mm - gap) + 0.05
        if best_gap is None or (gap is not None and gap > best_gap):
            best_gap, best_p_runs, best_n_runs = gap, p_runs, n_runs
        if best_gap is not None and best_gap >= clearance_mm:
            break
    return best_p_runs, best_n_runs


def _dip_across_paths(p_path: list[Pt], n_path: list[Pt], n_a: Pt, n_b: Pt, dip_margin_mm: float,
                       via_standoff_mm: float) -> list[tuple[list[Pt], int]]:
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
        # Keep both transition vias `via_standoff_mm` from n_a/n_b's own
        # coordinate -- a flip point close to either end (this leg is
        # short, or the crossing sits near a connector) would otherwise
        # clamp a dip boundary to n_lo/n_hi, i.e. to n_a or n_b itself. For
        # a through-hole pin (`_route_crossing_leg`'s caller passes
        # MIN_VIA_STANDOFF_FROM_PAD_MM) that would land the via on top of
        # the pin's own drilled hole (`holes_co_located`, a real DRC short
        # risk even though the via and pad share a net); an SMD pad has no
        # drill to collide with, so `_route_bridge_pair` passes a much
        # smaller standoff instead, or the dip can never reach far enough
        # to actually clear the crossing on a short mirror-pin bridge stub.
        # Either standoff scales down on a short corridor rather than
        # applying unscaled and overshooting past the far pad entirely --
        # same "scale the margin to the available length" shape as
        # `_build_leg`'s own per-leg inset.
        span = abs(getattr(n_b, axis) - getattr(n_a, axis))
        standoff = min(via_standoff_mm, span * 0.3)
        if n_forward:
            near_coordinate = max(n_lo, flip_at - dip_margin_mm, getattr(n_a, axis) + standoff)
            far_coordinate = min(n_hi, flip_at + dip_margin_mm)
            far_limit = getattr(n_b, axis) - standoff
            if far_limit > near_coordinate:
                far_coordinate = min(far_coordinate, far_limit)
        else:
            near_coordinate = min(n_hi, flip_at + dip_margin_mm, getattr(n_a, axis) - standoff)
            far_coordinate = max(n_lo, flip_at - dip_margin_mm)
            far_limit = getattr(n_b, axis) + standoff
            if far_limit < near_coordinate:
                far_coordinate = max(far_coordinate, far_limit)
        dip_start = _point_on_axis(axis, near_coordinate, _cross_axis_value_at(n_path, axis, near_coordinate))
        dip_end = _point_on_axis(axis, far_coordinate, _cross_axis_value_at(n_path, axis, far_coordinate))
        return [
            (_bend_path(n_a, dip_start), pcbnew.F_Cu),
            (_bend_path(dip_start, dip_end), pcbnew.B_Cu),
            (_bend_path(dip_end, n_b), pcbnew.F_Cu),
        ]
    raise ValueError("_dip_across_paths: expected an order flip between P and N but found none on either axis")


def _detour_around_pad_row(a: Pt, b: Pt, bow_mm: float, side: float, inset_fraction: float = 0.4) -> list[Pt]:
    """A 45-degree-and-90-degree-only up/across/down detour from `a` to `b`
    (both on the same pad row) that leaves the row entirely -- `side`
    (+1/-1) picks which perpendicular direction the detour bows toward, so
    two bridges sharing a pad row can bow to *opposite* sides and never
    occupy the same space.

    The bow height (`bow_mm`, e.g. PAD_ROW_CLEAR_MM -- big enough to clear a
    real connector pad's own height) and each end's horizontal inset (how
    far the 45-degree corner pulls in from `a`/`b`) are capped independently
    at `span * inset_fraction` (J1's own mirror-pin bridge stubs run barely
    1 mm end to end, far shorter than the 0.9 mm bow their tall pads need, so
    a single 45-degree leg cannot reach the full bow height without
    overshooting past the opposite end). `inset_fraction` defaults to 0.4 for
    the general lateral-avoidance case; `_route_bridge_pair`'s own
    overlapping/crossing case passes BRIDGE_CROSSING_INSET_FRACTION instead,
    a smaller value that widens the flat bottom -- see that constant's own
    comment. When the capped inset cannot reach the full bow height on a
    45-degree leg alone, the remaining height is a *vertical* (90-degree) run
    at that inset's own x, still 15-degree-grid-consistent, rather than
    reusing the bow height as the diagonal's own run and producing an
    arbitrary-angle leg (a real bug this replaced: measured non-45-degree
    tracks on `kicad-cli`'s own geometry when bow_mm exceeded half the
    span)."""
    span = abs(b.x - a.x)
    diagonal_mm = min(bow_mm, span * inset_fraction)
    direction_sign = 1.0 if b.x >= a.x else -1.0
    detour_y = a.y + side * bow_mm
    near_diagonal_end = Pt(a.x + direction_sign * diagonal_mm, a.y + side * diagonal_mm)
    far_diagonal_start = Pt(b.x - direction_sign * diagonal_mm, b.y + side * diagonal_mm)
    points = [a, near_diagonal_end]
    if diagonal_mm < bow_mm - 1e-9:
        points.append(Pt(near_diagonal_end.x, detour_y))
        points.append(Pt(far_diagonal_start.x, detour_y))
    points.append(far_diagonal_start)
    points.append(b)
    return _dedup(points)


def _runs_min_gap_mm(p_runs: list[tuple[list[Pt], int]], n_runs: list[tuple[list[Pt], int]],
                      width_mm: float, sample_step_mm: float = 0.1) -> float | None:
    """Minimum same-layer edge-to-edge gap between every P run and every N
    run -- the (points, layer) run shape `_route_crossing_leg` returns,
    before they become board tracks. Mirrors `_gap_stats_mm`'s own
    same-layer-only filtering (a deliberate crossing dipped to the opposite
    layer is not a violation)."""
    best: float | None = None
    for p_points, p_layer in p_runs:
        for p_start, p_end in zip(p_points, p_points[1:]):
            length = (p_end - p_start).length()
            if length < 1e-6:
                continue
            steps = max(1, int(length / sample_step_mm))
            for step in range(steps + 1):
                t = step / steps
                sample = Pt(p_start.x + t * (p_end.x - p_start.x), p_start.y + t * (p_end.y - p_start.y))
                for n_points, n_layer in n_runs:
                    if n_layer != p_layer:
                        continue
                    for n_start, n_end in zip(n_points, n_points[1:]):
                        distance = _point_to_segment_distance_mm(sample, n_start, n_end) - width_mm
                        if best is None or distance < best:
                            best = distance
    return best


def _route_j1_via_free_bridge(bridge_point: Pt, primary_point: Pt, pad_half_height_mm: float,
                               side: float) -> list[tuple[list[Pt], int]]:
    """One of J1's two mirror-pin bridges (see the H3h module comment above),
    entirely on F.Cu, 0 vias: a `_detour_around_pad_row` bow that dips `side`
    (-1 connector side / +1 board side) past the row's own pad half-height
    (`pad_half_height_mm`) by `J1_BRIDGE_TIP_STANDOFF_MM`. `bridge_point` and
    `primary_point` may be given in either order -- `_detour_around_pad_row`'s
    own shape is symmetric in its two endpoints, so which pad is named first
    changes nothing about the routed copper, only which end of the returned
    polyline lists it."""
    bow_mm = pad_half_height_mm + J1_BRIDGE_TIP_STANDOFF_MM
    points = _detour_around_pad_row(bridge_point, primary_point, bow_mm, side,
                                     inset_fraction=J1_BRIDGE_INSET_FRACTION)
    return [(points, pcbnew.F_Cu)]


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


# H3g U1 fan-through (README §Routing "Recipe B"): U1 (the device-USB ESD
# shunt, a SOT-23-3 with pins 1/2 the D+/D- pass-through and pin 3 GND on the
# opposite side) sits on the *same* x centreline as J1's and J2's own D+/D-
# midpoints (measured: all three at x=45.0 mm on this board), so the whole
# J1-to-U1-to-J2 corridor is a single vertical run -- U1 is not a stub or a
# via, it is a straight electrical pass-through both lines cross on their way
# from J1 to J2. Treating it as an ordinary `_build_leg` hop (lead-in/out
# stopping exactly at the pad centre, then a fresh centerline for the next
# hop) does not model that: this function instead fans the coupled pair out
# to U1's own pin 1/2 pitch (read off the board every run, never a hardcoded
# mm value) so each line's centreline passes straight through its own pad,
# widens further past pin 3 by >=0.15 mm clearance on both edges (a
# `kicad-cli pcb drc` hole/copper clearance requirement, not a style choice),
# then fans back to the coupled pitch before continuing to J2 -- 0 vias, per
# the module's own "read positions off the board" convention.
U1_FAN_CLEARANCE_MM = 0.15
# The widened plateau must also leave room for U1's GND stitch via (placed by
# route_gnd_stitch.py in line with pin 3, on the side away from the pin row):
# pitch >= via diameter + 2 x clearance + track width, and the plateau runs on
# past the pad edge by via diameter + 2 x clearance before fanning back in.
# Under the 2026-09-12 tolerance ruling the extra uncoupled length is free.
U1_FAN_VIA_PITCH_MIN_MM = VIA_DIAMETER_MM + 2 * U1_FAN_CLEARANCE_MM
U1_FAN_MARGIN_AFTER_GND_MM = VIA_DIAMETER_MM + 2 * U1_FAN_CLEARANCE_MM
U1_FAN_PITCH_ROUND_STEP_MM = 0.05



def _round_up_to_step_mm(value_mm: float, step_mm: float) -> float:
    return math.ceil(value_mm / step_mm - 1e-9) * step_mm


def _pair_slug(label: str) -> str:
    return label.lower().replace(" ", "-")


def _select_pair_specs(pairs_argument: str | None) -> list[dict]:
    """Filter `PAIR_SPECS` down to the `--pairs` selection (comma-separated
    slugs, e.g. `usb-device,eth-tx`). Default (no `--pairs`): USB device
    only -- the H3i ruling hand-routes just that pair and leaves the other
    three to Freerouting (design decisions 2026-09-12: relaxed skew/via/layer
    tolerances mean Freerouting's own USBH/ETH results already stood on
    their own, 0.15/2.24/3.37 mm skew)."""
    known = {_pair_slug(spec["label"]): spec for spec in PAIR_SPECS}
    if not pairs_argument:
        selected = ["usb-device"]
    else:
        selected = [slug.strip() for slug in pairs_argument.split(",") if slug.strip()]
    unknown = [slug for slug in selected if slug not in known]
    if unknown:
        raise ValueError(f"unknown --pairs value(s) {unknown!r} -- known: {sorted(known)}")
    return [known[slug] for slug in selected]


def _net_track_stats(board, net) -> tuple[float, int, set[str]]:
    """Sum this net's currently-routed track length (mm), via count, and the
    set of copper layers its tracks occupy -- reads the board as-is, adds or
    removes nothing."""
    length_mm = 0.0
    via_count = 0
    layer_names: set[str] = set()
    net_code = net.GetNetCode()
    for track in board.GetTracks():
        if track.GetNetCode() != net_code:
            continue
        if isinstance(track, pcbnew.PCB_VIA):
            via_count += 1
            continue
        length_mm += track.GetLength() / 1e6
        layer_names.add("F.Cu" if track.GetLayer() == pcbnew.F_Cu else "B.Cu")
    return length_mm, via_count, layer_names


def _measure_pairs(board_path: str) -> int:
    """`--measure`: report P/N length, skew, via count and layers for all
    four `PAIR_SPECS` pairs exactly as currently routed on `board_path`,
    independent of which pairs this run's own `--pairs` selection routed --
    no track or via is added, removed, or moved. Used post-pipeline (after
    Freerouting + `route_import_ses.py` + cleanup) to check the relaxed
    skew tolerance (design decisions 2026-09-12, MAX_SKEW_MM's own 0.5 mm
    constant is this script's own tighter hand-routing target, not that
    5 mm acceptance bar) against every pair regardless of routing origin."""
    board = pcbnew.LoadBoard(board_path)
    for spec in PAIR_SPECS:
        p_net = _find_net(board, spec["p_net"])
        n_net = _find_net(board, spec["n_net"])
        p_length, p_vias, p_layers = _net_track_stats(board, p_net)
        n_length, n_vias, n_layers = _net_track_stats(board, n_net)
        skew_mm = p_length - n_length
        layer_names = sorted(p_layers | n_layers)
        print(f"== {spec['label']}: P {p_length:.3f} mm, N {n_length:.3f} mm, "
              f"skew {skew_mm:.3f} mm, vias {p_vias + n_vias}, layers {layer_names}", flush=True)
    return 0


def _route_usb_device_pair(board, p_pads: list, n_pads: list,
                            p_obstacles: list[tuple[Pt, float]], n_obstacles: list[tuple[Pt, float]],
                            half_pitch_mm: float, width_mm: float, gap_mm: float,
                            foreign_candidates: list, blocked_ids: set[int],
                            p_bridge_points: list[Pt]
                            ) -> tuple[list, list, float, float, float, set[str], dict, bool]:
    """Route the USB device pair's J1->U1->J2 corridor (`p_pads`/`n_pads` are
    the three resolved (J1, U1, J2) pads for each side, in that order) as one
    fan-through run per net instead of the ordinary two independent
    `_build_leg` hops -- see the module comment above. `p_bridge_points` is
    P/D+'s own already-computed J1 via-free bridge polyline (`main`'s own
    `bridge_runs["p"]`), needed to clear its own highest point before this
    leg's own J1-to-U1 lead starts. Returns (p_legs, n_legs, p_length,
    n_length, last_side_sign, ripped_nets, report, needed_crossing) -- the
    first seven in the same shapes `main`'s generic per-hop loop produces, so
    the caller's skew-matching and bridge-routing code afterward is
    unchanged; `needed_crossing` tells `main` whether to print its own
    disclosure line (always true here -- see below)."""
    p_j1, p_u1, p_j2 = (_pad_point(pad) for pad in p_pads)
    n_j1, n_u1, n_j2 = (_pad_point(pad) for pad in n_pads)
    if abs(p_u1.y - n_u1.y) > 1e-6:
        raise ValueError("U1 fan-through: pins 1/2 are not on the same row -- "
                          "re-derive this recipe for U1's current orientation")
    centreline_x = (p_u1.x + n_u1.x) / 2
    if abs((p_j1.x + n_j1.x) / 2 - centreline_x) > 0.01 or abs((p_j2.x + n_j2.x) / 2 - centreline_x) > 0.01:
        raise ValueError("U1 fan-through: J1/U1/J2 are not on one shared x centreline -- "
                          "the vertical-corridor assumption behind this recipe no longer holds")
    p_side = 1.0 if p_u1.x > centreline_x else -1.0
    n_side = -p_side

    gnd_pad = _find_pad(board, "U1", "3")
    gnd_point = _pad_point(gnd_pad)
    gnd_size = gnd_pad.GetSize()
    gnd_width_mm = gnd_size.x / 1e6
    gnd_half_height_mm = gnd_size.y / 2e6

    coupled_half = half_pitch_mm
    native_half = abs(p_u1.x - n_u1.x) / 2
    full_pitch_min = max(gnd_width_mm + 2 * U1_FAN_CLEARANCE_MM,
                         U1_FAN_VIA_PITCH_MIN_MM) + width_mm
    widened_half = _round_up_to_step_mm(full_pitch_min, U1_FAN_PITCH_ROUND_STEP_MM) / 2
    fan_out_span = (native_half - coupled_half) + (widened_half - native_half)
    fan_in_span = widened_half - coupled_half

    y_native = p_u1.y
    # H3i: which side of the native pin row GND falls on flips with U1's own
    # rotation (H3g assumed GND trails the row on the J2/exit side; at 0
    # degrees GND instead leads it on the J1/entry side -- read off the
    # board every run rather than assumed, so this recipe keeps working
    # under either rotation). The widened plateau that clears GND always
    # goes on GND's own side; the opposite side is a plain coupled<->native
    # transition with nothing left to clear.
    if gnd_point.y < y_native:
        y_wide_inner = y_native - (widened_half - native_half)
        y_wide_outer = gnd_point.y - gnd_half_height_mm - U1_FAN_MARGIN_AFTER_GND_MM
        y_entry_boundary = y_wide_outer - fan_in_span
        y_exit_boundary = y_native + (native_half - coupled_half)
        widened_ys = (y_wide_outer, y_wide_inner)
    else:
        y_wide_start = y_native + (widened_half - native_half)
        y_wide_end = gnd_point.y + gnd_half_height_mm + U1_FAN_MARGIN_AFTER_GND_MM
        y_entry_boundary = y_native - (native_half - coupled_half)
        y_exit_boundary = y_wide_end + fan_in_span
        widened_ys = (y_wide_start, y_wide_end)

    def _fan_points(side: float) -> list[Pt]:
        return [
            Pt(centreline_x + side * coupled_half, y_entry_boundary),
            Pt(centreline_x + side * widened_half, widened_ys[0]),
            Pt(centreline_x + side * widened_half, widened_ys[1]),
            Pt(centreline_x + side * coupled_half, y_exit_boundary),
        ]

    p_fan, n_fan = _fan_points(p_side), _fan_points(n_side)

    # J1 -> U1: an ordinary coupled hop, 0 vias, 0 crossings -- U1's own
    # DP-left/DM-right pin order (0 degrees, scaffold_pcb.py PLACEMENT)
    # matches this pair's own primary J1 hop (B6/B7, also DP-left/DM-right;
    # see the PAIR_SPECS "USB device" comment), so no handedness reversal
    # exists on this leg at all. Both exits go straight +Y first
    # (`PAD_ROW_EXIT_DIRECTION`'s own mechanism, given a local displacement
    # here rather than a table entry) before converging with 45-degree legs
    # to the coupled pitch: P's own via-free bridge (`p_bridge_points`) bows
    # up from B6 to clear the row (H3h), and N's own lead-out would
    # otherwise cross that bow's riser at close range (measured: a real
    # planar intersection, not just a clearance shortfall the pad-obstacle
    # retry loop can push away from, since the retry only bends the
    # centerline's own interior, never the fixed pad-to-offset-line lead).
    # Clearing the bridge's own highest point first, before either line's
    # own diagonal starts, keeps both leads on the correct side of it.
    exit_up_mm = max(point.y for point in p_bridge_points) - p_j1.y + width_mm + gap_mm
    j1_exit = Pt(0.0, exit_up_mm)
    p_pre, n_pre, _pre_side_sign = _route_leg_avoiding_pads(
        p_j1, p_fan[0], n_j1, n_fan[0], half_pitch_mm, width_mm,
        p_obstacles, n_obstacles, gap_mm, exit_a=j1_exit, exit_b=None)

    # U1 -> J2: J2's own fixed pad order (DM-left/DP-right) is the opposite
    # handedness from U1's (DP-left/DM-right), so exactly one crossing is
    # unavoidable somewhere on this corridor -- an inherent USB-C topology
    # fact once J1's own order-reversal is resolved at the J1/U1 end instead
    # (see the PAIR_SPECS comment). Placed at this open run's own midpoint
    # (`U1_J2_CROSSING_HALF_SPAN_MM`, comfortably clear of both U1's fan-out
    # and J2 on a ~20 mm run), and temporarily fanned out to a wider
    # `U1_J2_CROSSING_PITCH_MM` pitch for the swap itself: at the coupled
    # USB90 pitch (~0.4 mm) the crossing's own transition via sits close
    # enough to the *other*, undipped line's own diagonal to violate its
    # real clearance (measured directly -- growing `_route_crossing_leg`'s
    # own dip margin cannot fix this, since that retry only checks
    # same-layer track-to-track gaps, never a via's own footprint). Widening
    # first (a plain 45-degree fan, same primitive as the U1 GND-clearance
    # fan above) gives the diagonal a shallower approach and the via real
    # room, then narrows back to coupled pitch afterward before continuing
    # to J2 (whose own further pitch mismatch gets the ordinary symmetric
    # pad-approach fan, `_symmetric_pad_approach_fan`).
    cross_y = (p_fan[-1].y + p_j2.y) / 2
    crossing_half_mm = U1_J2_CROSSING_PITCH_MM / 2
    fan_span = crossing_half_mm - coupled_half
    y_swap_lo = cross_y - U1_J2_CROSSING_HALF_SPAN_MM
    y_swap_hi = cross_y + U1_J2_CROSSING_HALF_SPAN_MM
    y_narrow = y_swap_hi + fan_span

    wide_p_lo = Pt(centreline_x + p_side * crossing_half_mm, y_swap_lo)
    wide_n_lo = Pt(centreline_x + n_side * crossing_half_mm, y_swap_lo)
    wide_p_hi = Pt(centreline_x + n_side * crossing_half_mm, y_swap_hi)  # P swaps to N's own side
    wide_n_hi = Pt(centreline_x + p_side * crossing_half_mm, y_swap_hi)  # N swaps to P's own side
    narrow_p = Pt(centreline_x + n_side * coupled_half, y_narrow)
    narrow_n = Pt(centreline_x + p_side * coupled_half, y_narrow)

    p_widen = _bend_path(p_fan[-1], wide_p_lo)
    n_widen = _bend_path(n_fan[-1], wide_n_lo)
    p_cross_runs, n_cross_runs = _route_crossing_leg_avoiding_self_cross(
        wide_p_lo, wide_p_hi, wide_n_lo, wide_n_hi, width_mm, gap_mm,
        via_standoff_mm=U1_J2_CROSSING_VIA_STANDOFF_MM)
    p_narrow = _bend_path(wide_p_hi, narrow_p)
    n_narrow = _bend_path(wide_n_hi, narrow_n)
    p_stage_c, n_stage_c, post_side_sign = _route_leg_avoiding_pads(
        narrow_p, p_j2, narrow_n, n_j2, half_pitch_mm, width_mm,
        p_obstacles, n_obstacles, gap_mm)

    p_cross_points, p_cross_layer = p_cross_runs[-1]  # single F.Cu run -- P never dips
    p_runs = [(_dedup(p_pre + p_fan[1:] + p_widen[1:] + p_cross_points[1:] + p_narrow[1:] + p_stage_c[1:]),
               p_cross_layer)]

    n_cross_first_points, n_cross_first_layer = n_cross_runs[0]
    n_cross_last_points, n_cross_last_layer = n_cross_runs[-1]
    n_runs = (
        [(_dedup(n_pre + n_fan[1:] + n_widen[1:] + n_cross_first_points[1:]), n_cross_first_layer)]
        + list(n_cross_runs[1:-1])
        + [(_dedup(n_cross_last_points + n_narrow[1:] + n_stage_c[1:]), n_cross_last_layer)]
    )

    p_length = sum(_path_length(points) for points, _layer in p_runs)
    n_length = sum(_path_length(points) for points, _layer in n_runs)

    ripped_nets: set[str] = set()
    for points, layer in p_runs + n_runs:
        if layer != pcbnew.F_Cu:
            continue  # foreign_candidates only tracks F.Cu copper (the module docstring's own scope)
        ripped_nets |= _mark_blocking_foreign_copper(points, width_mm, gap_mm, foreign_candidates, blocked_ids)

    gnd_clearance_mm = widened_half - width_mm / 2 - gnd_width_mm / 2
    report = {
        "native_pitch_mm": native_half * 2,
        "widened_pitch_mm": widened_half * 2,
        "gnd_clearance_mm": gnd_clearance_mm,
        "uncoupled_span_mm": y_exit_boundary - y_entry_boundary,
        "fan_out_span_mm": fan_out_span,
        "crossing_y_mm": cross_y,
        "crossing_margin_u1_mm": cross_y - p_fan[-1].y,
        "crossing_margin_j2_mm": p_j2.y - cross_y,
    }
    return [p_runs], [n_runs], p_length, n_length, post_side_sign, ripped_nets, report, True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", type=Path, default=DEFAULT_BOARD,
                         help="board .kicad_pcb path (default: canonical aius-carrier.kicad_pcb)")
    parser.add_argument("--pairs", type=str, default=None,
                         help="comma-separated pair slugs to route (default: usb-device only); "
                              "known slugs: " + ", ".join(_pair_slug(spec["label"]) for spec in PAIR_SPECS))
    parser.add_argument("--measure", action="store_true",
                         help="report P/N length, skew, via count and layers for all four pairs as "
                              "currently routed, without touching any copper, then exit (ignores --pairs)")
    args = parser.parse_args()
    board_path = str(args.board)

    if args.measure:
        return _measure_pairs(board_path)

    selected_specs = _select_pair_specs(args.pairs)

    board = pcbnew.LoadBoard(board_path)
    target_bare_names = _target_bare_names(PAIR_SPECS)
    selected_bare_names = _target_bare_names(selected_specs)

    # ---- resolve everything the rest of the script needs from the board,
    # before any board.Remove() call (see the module docstring). ----
    resolved = []
    for spec in selected_specs:
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
    # Only the *selected* pairs' own copper is unconditionally stripped for
    # re-routing (`--pairs`, default usb-device only); an unselected pair's
    # copper stays on the board untouched. `target_bare_names` (all four
    # pairs) still gates `foreign_candidates` below so an unselected pair's
    # existing tracks are never misclassified as foreign, blocking copper --
    # they are simply not routed this run, never ripped.
    own_tracks = [track for track in all_tracks_snapshot
                  if _bare_net_name(track.GetNetname()) in selected_bare_names]
    foreign_candidates = [track for track in all_tracks_snapshot
                           if track.GetLayer() == pcbnew.F_Cu
                           and _bare_net_name(track.GetNetname()) not in target_bare_names]

    # ---- compute every pair's geometry and foreign-collision rip decisions
    # purely from the held pad/net references and the static foreign-copper
    # snapshot -- no board mutation yet. ----
    blocked_ids: set[int] = set()
    # Every pair's own finalized F.Cu points, sampled and folded into every
    # *later* pair's own obstacle set (`_polyline_obstacles`, the same
    # "not-yet-emitted copper as an obstacle" mechanism this module already
    # uses for J1's own bridges) -- `_foreign_pad_obstacles` only ever sees
    # real board pads, never a sibling pair's own routed track, so two
    # unrelated pairs terminating at neighbouring connectors (ETH TX and
    # ETH RX, both at J18/J20) could otherwise cross each other's copper
    # outright (measured: a real `kicad-cli pcb drc` `shorting_items`
    # finding between ETH_TXN and ETH_RXP with no shared obstacle to avoid).
    sibling_pair_obstacles: list[tuple[Pt, float]] = []
    pair_plans = []
    for entry in resolved:
        spec = entry["spec"]
        width_mm, gap_mm = CLASS_GEOMETRY_MM[spec["class"]]
        half_pitch_mm = (width_mm + gap_mm) / 2
        p_pads, n_pads = entry["p_pads"], entry["n_pads"]
        # Every other pair's pads (and every unrelated net's pads) are real
        # obstacles this pair's own leg must route around -- a sibling target
        # pair's pads count as foreign here even though they are exempt from
        # `foreign_candidates`' track-ripping (see the module docstring).
        # Excluded per side, not together: P must still dodge N's own pads
        # (and vice versa) -- see `_route_leg_avoiding_pads`'s docstring.
        p_obstacles = _foreign_pad_obstacles(board, {spec["p_net"]}, pcbnew.F_Cu) + sibling_pair_obstacles
        n_obstacles = _foreign_pad_obstacles(board, {spec["n_net"]}, pcbnew.F_Cu) + sibling_pair_obstacles

        bridge_runs = {}
        if "p_bridge_pad" in entry and "n_bridge_pad" in entry:
            # J1's own two mirror-pin bridges (H3h, README "Recipe C"),
            # computed *before* this pair's own legs: each dips off the row
            # on its own side (P/D+ above, N/D- below) -- see
            # `_route_j1_via_free_bridge`'s own docstring for why that keeps
            # the two bridges' interleaved connector pins from ever needing a
            # via.
            p_bridge_pad, n_bridge_pad = entry["p_bridge_pad"], entry["n_bridge_pad"]
            bridge_runs["p"] = _route_j1_via_free_bridge(
                _pad_point(p_bridge_pad), _pad_point(p_pads[0]),
                p_bridge_pad.GetSize().y / 2e6, side=1.0)
            bridge_runs["n"] = _route_j1_via_free_bridge(
                _pad_point(n_bridge_pad), _pad_point(n_pads[0]),
                n_bridge_pad.GetSize().y / 2e6, side=-1.0)
            # Each bridge's own bow rises well clear of J1's row (see
            # `_route_j1_via_free_bridge`), directly through the corridor the
            # *other* net's own primary leg launches into on its way to U1 --
            # a real same-run `kicad-cli pcb drc` clearance finding at the P
            # bridge's own riser confirmed this is a genuine collision, not
            # just close. Neither bridge is a foreign *pad*, so
            # `_foreign_pad_obstacles` never sees it; register each bridge's
            # own polyline as an extra obstacle for the other net's leg
            # (`_polyline_obstacles`, the same "not-yet-emitted copper as an
            # obstacle" mechanism this module already uses elsewhere).
            for points, _layer in bridge_runs["p"]:
                n_obstacles = n_obstacles + _polyline_obstacles(points, width_mm)
            for points, _layer in bridge_runs["n"]:
                p_obstacles = p_obstacles + _polyline_obstacles(points, width_mm)
        elif "p_bridge_pad" in entry:
            bridge_runs["p"] = [(_bend_path(_pad_point(entry["p_bridge_pad"]), _pad_point(p_pads[0])), pcbnew.F_Cu)]
        elif "n_bridge_pad" in entry:
            bridge_runs["n"] = [(_bend_path(_pad_point(entry["n_bridge_pad"]), _pad_point(n_pads[0])), pcbnew.F_Cu)]

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
        u1_fanthrough_report: dict | None = None
        usb_device_crossing = False

        if spec["label"] == "USB device":
            # Recipe B (README §Routing, H3g) -- U1 is a straight
            # pass-through on this pair's own leg, not an ordinary hop; see
            # `_route_usb_device_pair`'s own docstring.
            p_legs_new, n_legs_new, p_length, n_length, last_side_sign, fan_ripped, u1_fanthrough_report, \
                usb_device_crossing = (
                    _route_usb_device_pair(board, p_pads, n_pads, p_obstacles, n_obstacles,
                                            half_pitch_mm, width_mm, gap_mm, foreign_candidates, blocked_ids,
                                            bridge_runs["p"][0][0]))
            p_legs += p_legs_new
            n_legs += n_legs_new
            ripped_this_pair |= fan_ripped
        else:
            for leg_index in range(len(p_pads) - 1):
                p_a, p_b = _pad_point(p_pads[leg_index]), _pad_point(p_pads[leg_index + 1])
                n_a, n_b = _pad_point(n_pads[leg_index]), _pad_point(n_pads[leg_index + 1])
                exit_a = PAD_ROW_EXIT_DIRECTION.get(spec["p_hops"][leg_index][0])
                exit_b = PAD_ROW_EXIT_DIRECTION.get(spec["p_hops"][leg_index + 1][0])
                try:
                    p_points, n_points, side_sign = _route_leg_avoiding_pads(
                        p_a, p_b, n_a, n_b, half_pitch_mm, width_mm, p_obstacles, n_obstacles, gap_mm, exit_a, exit_b)
                    p_runs, n_runs = [(p_points, pcbnew.F_Cu)], [(n_points, pcbnew.F_Cu)]
                    last_side_sign = side_sign
                except ValueError:
                    p_runs, n_runs = _route_crossing_leg_avoiding_self_cross(p_a, p_b, n_a, n_b, width_mm, gap_mm)
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
                n_legs[-1][-1] = (_add_serpentine(last_points, extra, side_sign=-last_side_sign,
                                                   width_mm=width_mm, obstacles=n_obstacles, clearance_mm=gap_mm),
                                  last_layer)
                n_length += extra
            else:
                last_points, last_layer = p_legs[-1][-1]
                p_legs[-1][-1] = (_add_serpentine(last_points, extra, side_sign=last_side_sign,
                                                   width_mm=width_mm, obstacles=p_obstacles, clearance_mm=gap_mm),
                                  last_layer)
                p_length += extra
            skew_mm = p_length - n_length

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
            "crossing_legs": crossing_legs,
            "u1_fanthrough_report": u1_fanthrough_report,
            "usb_device_crossing": usb_device_crossing,
        })
        for leg in p_legs + n_legs:
            for points, layer in leg:
                if layer != pcbnew.F_Cu:
                    continue
                sibling_pair_obstacles += _polyline_obstacles(points, width_mm)

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
        if plan["usb_device_crossing"]:
            report = plan["u1_fanthrough_report"] or {}
            print("   DISCLOSED EXCEPTION: U1-to-J2 leg needed a via pair -- J2's own fixed "
                  "DM-left/DP-right pad order is the opposite handedness from U1's DP-left/DM-right "
                  f"pin order, a planar crossing no 45-degree bend choice can avoid on one copper layer "
                  f"alone; placed at y={report.get('crossing_y_mm', float('nan')):.3f} mm, "
                  f"{report.get('crossing_margin_u1_mm', float('nan')):.3f} mm clear of U1's own fan-out and "
                  f"{report.get('crossing_margin_j2_mm', float('nan')):.3f} mm clear of J2", flush=True)
        if plan["u1_fanthrough_report"] is not None:
            report = plan["u1_fanthrough_report"]
            print(f"   Recipe B (U1 fan-through): native pitch {report['native_pitch_mm']:.3f} mm, "
                  f"widened to {report['widened_pitch_mm']:.3f} mm (GND clearance "
                  f"{report['gnd_clearance_mm']:.3f} mm), uncoupled span {report['uncoupled_span_mm']:.3f} mm, "
                  f"0 vias", flush=True)

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
