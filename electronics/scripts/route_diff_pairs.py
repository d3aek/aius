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
Ethernet TX route at 0 vias, entirely on F.Cu. Rotating U1 180 degrees
(`scaffold_pcb.py` PLACEMENT -- a symmetric ESD chip with no directional
meaning) lines its own D+/D- pad order up with J1's A6/A7 mirror pins and
J2's fixed pad order, so the whole J1-to-U1-to-J2 chain *would* route at
0 vias on that basis alone. J1's own B6/A7/A6/B7 mirror-pin interleaving no
longer forces the *bridge ties themselves* to cross (H3h, see
`_route_j1_via_free_bridge`) -- each mirror pin's bridge now dips off the
row on its own side (D+ above, D- below) instead of sharing the row's own
height, so the two bridges' interleaved spans never actually occupy the
same territory, genuinely 0 vias for that bond. But A6's own bridge, going
above the row, boxes A7 in on three sides (its own two "legs" plus the flat
top -- J1's exact 0.5 mm pitch centres that bridge's span on A7 the same
way it already centres the connector's own order-reversal fact), so A7's
own continuing trunk to U1 cannot leave from A7 itself any more; it launches
from B7 instead (bonded to A7 by its own via-free bridge), which sits on
the *wrong* side of the coupled pair for this corridor and needs the same
kind of disclosed via pair ETH_RXP/ETH_RXN already needs, for the same
underlying reason -- see `_route_usb_device_pair`'s own comment for the
full argument (and why no F.Cu-only exit shape or B.Cu dip placement
avoids it). ETH_RXP/ETH_RXN's own disclosed via pair: J20's magjack pin
field and the RXP/RXN-to-J18 cable-pin contract (`ethernet.json`, "1 RX+,
2 RX-, ... 6 TX+") fix ETH_TX and ETH_RX to *opposite* handedness
conventions at J18 (confirmed by direct pad-position measurement), so no
single J18 rotation can satisfy both -- rotating J18 to fix RX's crossing
measurably breaks TX's own 0-via route instead. Both disclosed pairs print
a `DISCLOSED EXCEPTION` line at run time. J2 (the USB-A jack) may be
relocated/rotated freely per an design decision, but was not needed: the U1
rotation above already clears the J1-to-J2 chain's own order reversal
without touching J2.

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

# Standoff for `_route_crossing_leg`'s own transition via from an SMD
# endpoint with no drill of its own to collide with (unlike
# MIN_VIA_STANDOFF_FROM_PAD_MM's own through-hole case) -- used for J1's own
# B7 launch in `_route_usb_device_pair` (H3h). Swept empirically against
# this leg's own real geometry: 0.1 left the dip's own via close enough to
# A6's own pad for a real `kicad-cli pcb drc` `clearance` finding; 0.5 moved
# the dip late enough to re-collide with P's own coupled offset line instead
# (the same self-clearance failure mode a plain vertical A7 exit hit before
# this leg moved to B7 at all). 0.2 is the best point found in that bounded
# sweep (H3c) -- it does not fully clear either neighbour (a residual
# `clearance`/`hole_clearance` finding against A6 remains, disclosed rather
# than chased further), but it is materially better than either extreme.
J1_CROSSING_VIA_STANDOFF_MM = 0.2

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
        # left/right order (A6 right of A7; B6 left of B7). U1 is rotated
        # 180 degrees (scaffold_pcb.py PLACEMENT -- it is a symmetric ESD
        # chip with no directional/mechanical meaning) so its own D+/D- pad
        # order flips to DM-left/DP-right, matching J2's fixed DM-left/
        # DP-right pad order; A6/A7 (also DM-left/DP-right) is therefore the
        # primary hop, staying coupled with zero vias all the way to J2, and
        # B6/B7 (the opposite-handed mirror pins) is the bridge -- via-free
        # (H3h, see `_route_j1_via_free_bridge`), but A6's own bridge then
        # boxes A7 in, so N/D-'s own leg to U1 needs its own disclosed via
        # pair instead (see `_route_usb_device_pair`'s own comment): the
        # connector's physical B/A/A/B row interleaving forces a crossing
        # somewhere in this corridor regardless of which mirror pin is
        # chosen primary -- an inherent USB-C topology fact, not fixable by
        # rotation.
        "label": "USB device",
        "p_net": "USB_DP", "n_net": "USB_DM", "class": "USB90",
        "p_hops": [("J1", "A6"), ("U1", "1"), ("J2", "3")],
        "n_hops": [("J1", "A7"), ("U1", "2"), ("J2", "2")],
        "p_bridge": ("J1", "B6"), "n_bridge": ("J1", "B7"),
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
    return _insert_lateral_tooth(points, index, amplitude, side_sign)


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


def _leg_lead_from(pad: Pt, exit_point: Pt, target: Pt) -> list[Pt]:
    """Path from `pad` to `target`, straight to `exit_point` first when it
    differs from `pad` (see `_exit_point`)."""
    if exit_point.x == pad.x and exit_point.y == pad.y:
        return _bend_path(pad, target)
    return _dedup([pad] + _bend_path(exit_point, target))


def _leg_lead_to(source: Pt, exit_point: Pt, pad: Pt) -> list[Pt]:
    """Mirror of `_leg_lead_from` for a path arriving at `pad`."""
    if exit_point.x == pad.x and exit_point.y == pad.y:
        return _bend_path(source, pad)
    return _dedup(_bend_path(source, exit_point) + [pad])


def _leg_centerline(p_a: Pt, p_b: Pt, n_a: Pt, n_b: Pt, half_pitch_mm: float,
                     exit_a: Pt | None, exit_b: Pt | None) -> tuple[list[Pt], float]:
    """The centerline + side_sign portion of `_build_leg`, factored out so
    `_route_leg_avoiding_pads` can locate an obstacle against the same base
    geometry every retry targets a tooth into (see `_insert_lateral_tooth`),
    without re-deriving the exit-point/inset/bend-shape logic itself."""
    exit_p_a, exit_n_a = _exit_point(p_a, exit_a), _exit_point(n_a, exit_a)
    exit_p_b, exit_n_b = _exit_point(p_b, exit_b), _exit_point(n_b, exit_b)
    hop_a, hop_b = _midpoint(exit_p_a, exit_n_a), _midpoint(exit_p_b, exit_n_b)
    leg_vector = hop_b - hop_a
    leg_length = leg_vector.length()
    if leg_length > 1e-6:
        inset = min(2 * half_pitch_mm, leg_length * 0.3)
        direction = _unit(leg_vector)
        hop_a = hop_a + direction * inset
        hop_b = hop_b - direction * inset
    for diagonal_first in (True, False):
        candidate = _bend_path(hop_a, hop_b, diagonal_first)
        near_direction = _unit(candidate[1] - candidate[0])
        far_direction = _unit(candidate[-1] - candidate[-2])
        near_sign = _side_sign_at(p_a, candidate[0], near_direction)
        far_sign = _side_sign_at(p_b, candidate[-1], far_direction)
        if near_sign == far_sign:
            return candidate, near_sign
    raise ValueError(
        "no 45-degree bend keeps this leg's P/N handedness consistent end to end -- "
        "the pads' physical winding needs an actual conductor crossing (a via) on this corridor")


def _build_leg(p_a: Pt, p_b: Pt, n_a: Pt, n_b: Pt, half_pitch_mm: float,
                exit_a: Pt | None = None, exit_b: Pt | None = None,
                avoidance_teeth: dict[int, tuple[float, float]] | None = None
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
    own segments off that grid entirely."""
    exit_p_a, exit_n_a = _exit_point(p_a, exit_a), _exit_point(n_a, exit_a)
    exit_p_b, exit_n_b = _exit_point(p_b, exit_b), _exit_point(n_b, exit_b)
    centerline, side_sign = _leg_centerline(p_a, p_b, n_a, n_b, half_pitch_mm, exit_a, exit_b)
    if avoidance_teeth is not None:
        for segment_index in sorted(avoidance_teeth, reverse=True):
            amplitude_mm, tooth_side = avoidance_teeth[segment_index]
            centerline = _insert_lateral_tooth(centerline, segment_index, amplitude_mm, tooth_side)

    p_offset = _offset_polyline(centerline, side_sign, half_pitch_mm)
    n_offset = _offset_polyline(centerline, -side_sign, half_pitch_mm)

    p_points = _dedup(_leg_lead_from(p_a, exit_p_a, p_offset[0])[:-1]
                       + p_offset
                       + _leg_lead_to(p_offset[-1], exit_p_b, p_b)[1:])
    n_points = _dedup(_leg_lead_from(n_a, exit_n_a, n_offset[0])[:-1]
                       + n_offset
                       + _leg_lead_to(n_offset[-1], exit_n_b, n_b)[1:])
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
# should stop growing rather than produce an absurdly large detour.
MAX_AVOIDANCE_TOOTH_MM = 4.8


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
    base_centerline, _base_side_sign = _leg_centerline(p_a, p_b, n_a, n_b, half_pitch_mm, exit_a, exit_b)
    avoidance_teeth: dict[int, tuple[float, float]] = {}
    best_shortfall: float | None = None
    best_points: tuple[list[Pt], list[Pt], float] | None = None
    for _attempt in range(max_attempts):
        try:
            p_points, n_points, leg_side_sign = _build_leg(
                p_a, p_b, n_a, n_b, half_pitch_mm, exit_a, exit_b, avoidance_teeth or None)
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
    # `via_standoff_mm` is overridable for an SMD endpoint with no nearby
    # drill of its own to collide with (see J1's own B7 launch in
    # `_route_usb_device_pair`): the default standoff, sized for a
    # through-hole neighbour, clamps the dip's own start well past where the
    # two paths actually swap order for a short corridor like that one --
    # confirmed by a real same-pair self-clearance violation (the dip's own
    # growing-margin retry cannot fix this, since the standoff clamp -- not
    # the margin -- is what is binding).
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
U1_FAN_MARGIN_AFTER_GND_MM = 0.15
U1_FAN_PITCH_ROUND_STEP_MM = 0.05



def _round_up_to_step_mm(value_mm: float, step_mm: float) -> float:
    return math.ceil(value_mm / step_mm - 1e-9) * step_mm


def _route_usb_device_pair(board, p_pads: list, n_pads: list,
                            p_obstacles: list[tuple[Pt, float]], n_obstacles: list[tuple[Pt, float]],
                            half_pitch_mm: float, width_mm: float, gap_mm: float,
                            foreign_candidates: list, blocked_ids: set[int], n_bridge_pad
                            ) -> tuple[list, list, float, float, float, set[str], dict, bool]:
    """Route the USB device pair's J1->U1->J2 corridor (`p_pads`/`n_pads` are
    the three resolved (J1, U1, J2) pads for each side, in that order) as one
    fan-through run per net instead of the ordinary two independent
    `_build_leg` hops -- see the module comment above. `n_bridge_pad` is
    N/D-'s own J1 mirror pin (B7) -- see the comment at its own call site
    below for why this leg launches from there instead of N's own primary
    hop pad (A7). Returns (p_legs, n_legs, p_length, n_length,
    last_side_sign, ripped_nets, report, needed_crossing) -- the first seven
    in the same shapes `main`'s generic per-hop loop produces, so the
    caller's skew-matching and bridge-routing code afterward is unchanged;
    `needed_crossing` tells `main` whether to print its own disclosure line."""
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
    full_pitch_min = gnd_width_mm + 2 * U1_FAN_CLEARANCE_MM + width_mm
    widened_half = _round_up_to_step_mm(full_pitch_min, U1_FAN_PITCH_ROUND_STEP_MM) / 2
    fan_out_span = (native_half - coupled_half) + (widened_half - native_half)
    fan_in_span = widened_half - coupled_half

    y_native = p_u1.y
    y_entry_boundary = y_native - (native_half - coupled_half)
    y_wide_start = y_native + (widened_half - native_half)
    y_wide_end = gnd_point.y + gnd_half_height_mm + U1_FAN_MARGIN_AFTER_GND_MM
    y_exit_boundary = y_wide_end + fan_in_span

    def _fan_points(side: float) -> list[Pt]:
        return [
            Pt(centreline_x + side * coupled_half, y_entry_boundary),
            Pt(centreline_x + side * widened_half, y_wide_start),
            Pt(centreline_x + side * widened_half, y_wide_end),
            Pt(centreline_x + side * coupled_half, y_exit_boundary),
        ]

    p_fan, n_fan = _fan_points(p_side), _fan_points(n_side)

    # A7/D-'s own row exit cannot leave from A7 itself any more (H3h): A6's
    # own via-free bridge (`_route_j1_via_free_bridge`) dips to the *board*
    # side directly above A7's own column -- J1's exact 0.5 mm mirror-pin
    # pitch centres that bridge's span on A7 the same way it already centres
    # the connector's own inherent order-reversal fact (see the README's own
    # "not converted" note) -- so any path leaving A7 upward is boxed in on
    # three sides by the bridge's own two "legs" and flat top, confirmed by a
    # real same-pair self-clearance violation at every F.Cu-only exit shape
    # tried (a plain vertical, a 45-degree sideways dodge in either
    # direction), and by a real `kicad-cli pcb drc` `hole_clearance`/
    # `hole_to_hole`/`clearance` finding at every via size and standoff tried
    # for a B.Cu dip landing anywhere inside that same box (J1's 0.5 mm pitch
    # leaves less room than any board-legal via needs from the row's own
    # neighbouring pads, let alone the bridge's own copper too). A7 is
    # already bonded to B7 by its own via-free bridge (bullet 1), so this
    # leg instead launches from B7 -- clear of the box entirely, at the cost
    # of needing the same disclosed via pair `_route_crossing_leg` already
    # gives ETH_RXP/ETH_RXN for an inherent order reversal: B7 sits on the
    # *wrong* side of the coupled pair for this corridor (J1's own B6/A7/A6/
    # B7 interleaving again), so its path to the fan entry must swap sides
    # with P's own, provably impossible on one copper layer alone.
    n_bridge_point = _pad_point(n_bridge_pad)
    # B7 is SMD (no drill of its own to collide with), unlike the
    # through-hole neighbours `_route_crossing_leg`'s own default standoff
    # (MIN_VIA_STANDOFF_FROM_PAD_MM) is sized for -- that default clamps the
    # dip's own start well past the point where P's and N's raw paths
    # actually swap order on this short, near-vertical corridor (P's own
    # lead converges to its coupled column almost immediately off A6), which
    # a real same-pair self-clearance violation confirmed the dip's own
    # growing-margin retry cannot fix (the standoff clamp, not the margin,
    # is what is binding). J1_CROSSING_VIA_STANDOFF_MM matches the smaller
    # standoff an SMD pad needs instead.
    p_pre_runs, n_pre_runs = _route_crossing_leg_avoiding_self_cross(
        p_j1, p_fan[0], n_bridge_point, n_fan[0], width_mm, gap_mm,
        via_standoff_mm=J1_CROSSING_VIA_STANDOFF_MM)
    p_post, n_post, post_side_sign = _route_leg_avoiding_pads(
        p_fan[-1], p_j2, n_fan[-1], n_j2, coupled_half, width_mm,
        p_obstacles, n_obstacles, gap_mm, exit_a=None, exit_b=None)

    p_last_points, p_last_layer = p_pre_runs[-1]
    p_pre_runs = p_pre_runs[:-1] + [(_dedup(p_last_points + p_fan[1:] + p_post[1:]), p_last_layer)]
    n_last_points, n_last_layer = n_pre_runs[-1]
    n_pre_runs = n_pre_runs[:-1] + [(_dedup(n_last_points + n_fan[1:] + n_post[1:]), n_last_layer)]
    p_runs, n_runs = p_pre_runs, n_pre_runs
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
    }
    return [p_runs], [n_runs], p_length, n_length, post_side_sign, ripped_nets, report, len(n_runs) > 1


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
        # Every other pair's pads (and every unrelated net's pads) are real
        # obstacles this pair's own leg must route around -- a sibling target
        # pair's pads count as foreign here even though they are exempt from
        # `foreign_candidates`' track-ripping (see the module docstring).
        # Excluded per side, not together: P must still dodge N's own pads
        # (and vice versa) -- see `_route_leg_avoiding_pads`'s docstring.
        p_obstacles = _foreign_pad_obstacles(board, {spec["p_net"]}, pcbnew.F_Cu)
        n_obstacles = _foreign_pad_obstacles(board, {spec["n_net"]}, pcbnew.F_Cu)

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
                                            entry["n_bridge_pad"]))
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
                n_legs[-1][-1] = (_add_serpentine(last_points, extra, side_sign=-last_side_sign), last_layer)
                n_length += extra
            else:
                last_points, last_layer = p_legs[-1][-1]
                p_legs[-1][-1] = (_add_serpentine(last_points, extra, side_sign=last_side_sign), last_layer)
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
            print("   DISCLOSED EXCEPTION: N/D-'s own J1-to-U1 leg needed a via pair -- A7's own "
                  "primary pad is boxed in by A6's own via-free bridge (see _route_j1_via_free_bridge), "
                  "so this leg launches from B7 instead (bonded to A7 by its own bridge), whose physical "
                  "left/right order still reverses end to end on this corridor, a planar crossing no "
                  "45-degree bend choice can avoid on one copper layer alone", flush=True)
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
