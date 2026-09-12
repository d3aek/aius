#!/usr/bin/env python3
"""H3c stitching: bond every isolated GND island to the filled In1/In2 planes.

Freerouting ties most GND pads to the plane automatically (a PTH pad spans
every copper layer, so it lands inside the zone outline and bonds on fill),
but it does not add stitching vias for SMD GND pads or for short GND stub
tracks it routes between neighbouring SMD pads without ever reaching a via --
those stay their own electrically-isolated island, invisible to
`route_cleanup.py`'s solid-pad-connection pass because there is no PTH pad or
zone contact there to solidify.

For every non-plane GND island this script:
  - if the island already has routed copper (a stub track chain joining one or
    more SMD pads), lands a via at a "kink" point on that copper -- a point
    where two track segments join away from any pad -- so the existing trace
    itself becomes the new via's approach, adding no redundant track;
  - if the island is a bare pad with no routed copper at all, adds a via next
    to it plus a short Default-class track from the pad to the via;
  - either way, never centers a via inside a pad (KiCad discourages via-in-pad
    without a special fab process this project doesn't use), and searches a
    widening ring of offsets around the anchor point for a spot whose whole
    via-plus-connecting-track path clears every other net at the project's
    Default net-class clearance and every drilled hole's own clearance --
    checked against each candidate item's exact geometry (a bounding circle
    for a pad, exact point-to-segment distance for a track or via), not an
    axis-aligned bounding box, which false-positives against any long
    diagonal track spanning the whole search area.

After stitching, any GND island that is still just bare track with no pad at
all (a genuinely dangling stub, never observed on the routed canonical board
but possible on a future re-route) is deleted rather than left orphaned.

Four H3e follow-up passes then run, closing gaps the island walk above
cannot see because the affected pad's island is already bonded to the plane
through *someone else's* via:
  - every SMD GND pad still more than 1.0 mm from any via gets one, joined by
    a short connecting track;
  - the J1 GND pad group specifically gets a second via (its four GND pads
    collapse to two physical solder-pad locations, and one of those was
    observed reaching the plane only through the other's via, 6+ mm away);
  - each USB/Ethernet differential pair at J1/J2/J18/J20 gets a flanking GND
    via within 1.5 mm of its connector break-in (H2b's return-path
    argument), placed beside the pair rather than between its two pads;
  - any Power-class (>=0.5 mm track) net riding a single 0.3 mm-drill via
    gets a parallel same-net via (bridged on both copper layers, not just
    one), so no supply path depends on one plated hole.

Zones are refilled and the board saved in place.

Run: python3 electronics/scripts/route_gnd_stitch.py
Run against a scaffolded candidate: python3 electronics/scripts/route_gnd_stitch.py \
  --board <candidate.kicad_pcb>
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

import pcbnew

ELEC = Path(__file__).resolve().parent.parent
DEFAULT_BOARD = ELEC / "aius-carrier.kicad_pcb"

# Candidate via-center offsets (mm) tried around each anchor point, nearest
# first, in a ring of increasing radius: the anchor itself (no offset), then
# widening rings at a 24-point angular resolution up to 4.2 mm -- generous
# enough to clear the densest signal fanout on this board (the Teensy
# expansion I2C/I2S/SPI cluster near U10 needed the full range).
_RADII_MM = (0.0, 0.5, 0.8, 1.2, 1.6, 2.0, 2.4, 3.0, 3.6, 4.2)
_ANGLE_COUNT = 24
_ANGLES_RAD = [2 * math.pi * step / _ANGLE_COUNT for step in range(_ANGLE_COUNT)]
CANDIDATE_OFFSETS_MM = [(0.0, 0.0)] + [
    (radius_mm * math.cos(angle), radius_mm * math.sin(angle))
    for radius_mm in _RADII_MM[1:]
    for angle in _ANGLES_RAD
]


def _key(item):
    """A hashable identity for a board item that survives independent lookups
    (SWIG wrapper objects for the same underlying pad/track differ by Python
    identity across separate API calls, so identity has to be structural)."""
    if isinstance(item, pcbnew.PAD):
        footprint = item.GetParentFootprint()
        return ("PAD", footprint.GetReference(), item.GetNumber())
    if isinstance(item, pcbnew.PCB_TRACK):
        start, end = item.GetStart(), item.GetEnd()
        return ("TRACK", item.GetLayer(), start.x, start.y, end.x, end.y)
    if isinstance(item, pcbnew.ZONE):
        return ("ZONE", tuple(sorted(item.GetLayerSet().Seq())))
    raise TypeError(f"unsupported item type: {type(item).__name__}")


def _keys_touching_point(board, point):
    """Every pad or track/via whose position (or, for a track, either
    endpoint) exactly matches `point` -- the full "this is the anchor's own
    copper" set to exclude from a collision check anchored there. A pad
    almost always has at least one routed track leaving it, not just its own
    footprint, so excluding only the pad (as `_key(pad)` alone would) still
    trips on that track at the exact anchor point when the track's net
    differs from the net being excluded by `exclude_netcode` -- exactly the
    case for a differential-pair flanking via, whose GND netcode never
    matches the signal net the anchor pad and its trace are actually on."""
    keys = set()
    for footprint in board.GetFootprints():
        for pad in footprint.Pads():
            if pad.GetPosition() == point:
                keys.add(_key(pad))
    for track in board.GetTracks():
        if track.GetStart() == point or track.GetEnd() == point:
            keys.add(_key(track))
    return keys


def _gnd_islands(board, netcode):
    """Union-find grouping of every GND pad/track/zone into connectivity
    islands (a plain track/pad-only net walk, not `board.GetRatsnest()`,
    because the plane's own filled copper needs to count as one island
    member alongside the pads and stub tracks it actually touches)."""
    connectivity = board.GetConnectivity()
    items = []
    for footprint in board.GetFootprints():
        for pad in footprint.Pads():
            if pad.GetNetCode() == netcode:
                items.append(pad)
    for track in board.GetTracks():
        if track.GetNetCode() == netcode and not isinstance(track, pcbnew.PCB_VIA):
            items.append(track)
    for zone in board.Zones():
        if zone.GetNet() and zone.GetNetCode() == netcode:
            items.append(zone)

    parent = {_key(item): _key(item) for item in items}

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a, b):
        root_a, root_b = find(a), find(b)
        if root_a != root_b:
            parent[root_a] = root_b

    for item in items:
        item_key = _key(item)
        for connected in connectivity.GetConnectedItems(item):
            if not isinstance(connected, (pcbnew.PAD, pcbnew.PCB_TRACK, pcbnew.ZONE)):
                continue
            connected_key = _key(connected)
            if connected_key in parent:
                union(item_key, connected_key)

    islands = {}
    for item in items:
        islands.setdefault(find(_key(item)), []).append(item)
    return list(islands.values())


def _is_plane_island(members):
    return any(isinstance(member, pcbnew.ZONE) for member in members)


def _anchors(members):
    """Every candidate point + copper layer to stitch from, nearest-reuse
    first: every non-pad kink point on an already-routed stub-track chain
    (reuse, no new track needed at the anchor itself), then every pad's own
    position + layer as a fallback when a given kink point is too congested
    to land a via near (or the island has no routed copper at all)."""
    tracks = [member for member in members if isinstance(member, pcbnew.PCB_TRACK)]
    pads = [member for member in members if isinstance(member, pcbnew.PAD)]
    pad_points = {(pad.GetPosition().x, pad.GetPosition().y) for pad in pads}
    seen = set()
    candidates = []
    for track in tracks:
        for point in (track.GetStart(), track.GetEnd()):
            point_key = (point.x, point.y)
            if point_key in pad_points or point_key in seen:
                continue
            seen.add(point_key)
            candidates.append((pcbnew.VECTOR2I(point.x, point.y), track.GetLayer()))
    for pad in pads:
        candidates.append((pcbnew.VECTOR2I(pad.GetPosition().x, pad.GetPosition().y), pad.GetPrincipalLayer()))
    if not candidates:
        raise ValueError("GND island has neither routed copper nor a pad to anchor from")
    return candidates


def _distance_mm(point, other):
    return math.hypot((point.x - other.x) / 1e6, (point.y - other.y) / 1e6)


def _point_to_segment_distance_mm(point, seg_start, seg_end):
    """Perpendicular distance from `point` to the segment `seg_start`-`seg_end`,
    clamped to the segment (not the infinite line) -- exact geometry, unlike a
    bounding-box test, which wildly overestimates a long diagonal track's
    footprint (its bbox spans the whole board even though the copper itself is
    a thin line through it)."""
    ax, ay = seg_start.x / 1e6, seg_start.y / 1e6
    bx, by = seg_end.x / 1e6, seg_end.y / 1e6
    px, py = point.x / 1e6, point.y / 1e6
    dx, dy = bx - ax, by - ay
    length_sq = dx * dx + dy * dy
    if length_sq == 0.0:
        return math.hypot(px - ax, py - ay)
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length_sq))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def _orientation(point, query, other):
    value = (query.y - point.y) * (other.x - query.x) - (query.x - point.x) * (other.y - query.y)
    if value == 0:
        return 0
    return 1 if value > 0 else 2


def _on_segment(point, query, other):
    return (min(point.x, other.x) <= query.x <= max(point.x, other.x)
            and min(point.y, other.y) <= query.y <= max(point.y, other.y))


def _segments_intersect(a_start, a_end, b_start, b_end):
    """True when the straight segments `a_start`-`a_end` and `b_start`-`b_end`
    properly cross (including a collinear overlap or an endpoint landing on
    the other segment) -- a strict geometric test, independent of any
    clearance radius. A new track that literally crosses a foreign-net track
    is always a short regardless of how close the anchor already sat to that
    track before this search ran; `_path_collides_from_anchor`'s
    pre-existing-condition exemption forgives *proximity* near the anchor,
    never an outright intersection anywhere along the new copper."""
    o1 = _orientation(a_start, a_end, b_start)
    o2 = _orientation(a_start, a_end, b_end)
    o3 = _orientation(b_start, b_end, a_start)
    o4 = _orientation(b_start, b_end, a_end)
    if o1 != o2 and o3 != o4:
        return True
    if o1 == 0 and _on_segment(a_start, b_start, a_end):
        return True
    if o2 == 0 and _on_segment(a_start, b_end, a_end):
        return True
    if o3 == 0 and _on_segment(b_start, a_start, b_end):
        return True
    if o4 == 0 and _on_segment(b_start, a_end, b_end):
        return True
    return False


def _crosses_foreign_track(board, start, end, layer, exclude_netcode, exclude_keys=None):
    """True when the straight prospective connecting track `start`-`end` on
    `layer` properly crosses any other-net track on the same layer (vias
    excluded -- a via's own clearance is already the stricter, always-checked
    final-sample test). Runs once per candidate offset against the whole
    segment, rather than the discrete per-sample clearance scan, since a
    straight-line crossing is exact geometry, not something sampling could
    miss or over-forgive."""
    for track in board.GetTracks():
        if isinstance(track, pcbnew.PCB_VIA):
            continue
        if track.GetLayer() != layer:
            continue
        if track.GetNetCode() == exclude_netcode:
            continue
        if exclude_keys and _key(track) in exclude_keys:
            continue
        if _segments_intersect(start, end, track.GetStart(), track.GetEnd()):
            return True
    return False


def _colliders(board, center, keepout_radius, hole_keepout_radius, exclude_netcode, exclude_keys=None):
    """Yield the `_key()` of every pad/track/via colliding with a via
    centered at `center` -- via-in-pad (regardless of net), within
    `hole_keepout_radius` of any drilled hole (hole-to-hole / hole-to-copper
    spacing binds regardless of net -- two overlapping plated holes are a
    fab/structural problem even on the *same* net, unlike ordinary copper
    clearance, where same-net touching is the point), or within
    `keepout_radius` of any different-net copper (the project's Default
    clearance plus the via's own radius) -- the same conditions
    `kicad-cli pcb drc` would flag. Uses each item's real
    geometry (bounding circle for a pad, exact segment distance for a track
    or via) rather than axis-aligned bounding boxes, which false-positive on
    any diagonal track spanning the search area. Every pad is checked
    regardless of copper layer -- an NPTH mechanical mounting hole has no
    copper but still enforces hole clearance against a new via's own drill.

    `exclude_keys` (a set of `_key()` values) skips those specific pads/
    tracks/vias from every check below, regardless of net -- for a
    connecting track that deliberately starts on a real pad or via's own
    copper (or an anchor that *is* that exact pad, e.g. a differential pair
    with only one pad per net), that item is not a collision to avoid, it is
    the connection point.

    A generator rather than a single boolean so a caller that only wants a
    yes/no answer (`_collides`) can still short-circuit on the first hit,
    while a caller that wants to know *which* items already collide right at
    a real anchor point (`_path_collides_from_anchor`, to tell a pre-existing
    condition apart from a new one) can collect every one of them."""
    keepout_radius_mm = keepout_radius / 1e6
    hole_keepout_radius_mm = hole_keepout_radius / 1e6
    for footprint in board.GetFootprints():
        for pad in footprint.Pads():
            if exclude_keys and _key(pad) in exclude_keys:
                continue
            pad_radius_mm = pad.GetBoundingRadius() / 1e6
            distance = _distance_mm(center, pad.GetPosition())
            if distance <= pad_radius_mm:
                yield _key(pad)
                continue
            has_hole = pad.GetDrillSizeX() > 0
            if has_hole and distance <= pad_radius_mm + hole_keepout_radius_mm:
                yield _key(pad)
                continue
            if (not has_hole and pad.IsOnCopperLayer() and pad.GetNetCode() != exclude_netcode
                    and distance <= pad_radius_mm + keepout_radius_mm):
                yield _key(pad)
    for track in board.GetTracks():
        if not track.IsOnCopperLayer():
            continue
        if exclude_keys and _key(track) in exclude_keys:
            continue
        distance = _point_to_segment_distance_mm(center, track.GetStart(), track.GetEnd())
        # A via's drilled hole binds hole-to-hole spacing against every other
        # via regardless of net -- two plated holes physically overlapping is
        # a fab/structural problem even on the same net (unlike ordinary
        # copper clearance, where same-net touching is the point). Checked
        # before the same-net skip below, so a same-net via that already
        # bonds into the plane's own copper still gets its own drill spacing
        # enforced against a neighbour.
        if isinstance(track, pcbnew.PCB_VIA):
            drill_radius_mm = track.GetDrillValue() / 1e6 / 2
            if distance <= drill_radius_mm + hole_keepout_radius_mm:
                yield _key(track)
                continue
        if track.GetNetCode() == exclude_netcode:
            continue
        # PCB_VIA.GetWidth() with no layer argument asserts in KiCad 10 (a via's
        # copper diameter is queried per-layer); a through-hole via is the same
        # diameter on every layer, so F_Cu is as good a layer as any to ask.
        width = track.GetWidth(pcbnew.F_Cu) if isinstance(track, pcbnew.PCB_VIA) else track.GetWidth()
        half_width_mm = width / 1e6 / 2
        if distance <= half_width_mm + keepout_radius_mm:
            yield _key(track)


def _collides(board, center, keepout_radius, hole_keepout_radius, exclude_netcode, exclude_keys=None):
    """True when any item collides with a via centered at `center` -- see
    `_colliders` for the exact conditions checked. Stops at the first hit."""
    for _ in _colliders(board, center, keepout_radius, hole_keepout_radius, exclude_netcode, exclude_keys):
        return True
    return False


def _path_collides(board, start, end, keepout_radius, hole_keepout_radius, exclude_netcode, sample_count=12):
    """Same as `_collides`, sampled along the whole prospective connecting
    track from `start` to `end` -- checking only the via endpoint would miss
    a mid-track clearance violation against a pad the straight line grazes."""
    for step in range(sample_count + 1):
        t = step / sample_count
        point = pcbnew.VECTOR2I(
            int(start.x + t * (end.x - start.x)),
            int(start.y + t * (end.y - start.y)),
        )
        if _collides(board, point, keepout_radius, hole_keepout_radius, exclude_netcode):
            return True
    return False


def _path_collides_from_anchor(board, start, end, keepout_radius, hole_keepout_radius,
                                exclude_netcode, exclude_keys=None, sample_count=12,
                                track_keepout_radius=None, track_layer=None):
    """Same as `_path_collides`, with `exclude_keys` (see `_collides`)
    threaded through every sample except the last, and a separate, smaller
    `track_keepout_radius` for every sample except the last.

    `_place_via_near_point` below anchors directly on a real pad or via it
    intends to extend a track from (an SMD GND pad, a Power-class via, or a
    bare-pad GND island with no routed copper of its own -- the island walk
    in `main` falls back to exactly this case), or on the centroid of a
    differential pair's own pads (which, for a pair with only one pad per
    net, *is* that exact pad). Without `exclude_keys`, the unconditional
    via-in-pad check in `_collides` (`distance <= pad radius`, checked
    regardless of net) trips on that exact anchor at every sample near it,
    since the whole near-anchor stretch of a short connecting track sits
    inside the anchor's own pad radius -- not just the literal start point.
    (A stub-track kink point never has this problem on its own account, but
    `_keys_touching_point` excludes it too, at no cost, so one code path
    covers both anchor shapes.)

    Every sample before the last represents the *connecting track*'s own
    route, not the via -- checking it with `keepout_radius` (sized for the
    via's own diameter, e.g. 0.6 mm) demands a via-sized clearance gap from a
    section of copper that will only ever be `track_width` wide. A pad
    legitimately routed close enough to a foreign net for its own thin trace
    (well inside a via's much larger footprint) then makes every candidate
    direction spuriously "collide" at the anchor itself, regardless of where
    the via actually ends up. `track_keepout_radius` (the track's own
    clearance, `track_width / 2 + net clearance`) is the correct radius for
    that portion of the path; it defaults to `keepout_radius` when not given,
    matching plain `_path_collides`'s single-radius behaviour.

    The final sample (the candidate via position itself) is always checked
    with the full via `keepout_radius` and WITHOUT the exclusion, even though
    both relaxed for every sample before it: a via genuinely needs real
    hole-to-hole / via-in-pad clearance and via-sized copper clearance from
    its own anchor too (a through-hole anchor pad's drill does not stop
    binding just because a track will connect to it), not only from
    unrelated copper. Excluding the anchor there let a via land drilled
    straight against its own anchor pad's hole -- the exact `hole_to_hole`
    violation (actual 0.0 mm) this split fixes.

    `start` itself is a second, distinct pre-existing-condition case:
    whatever foreign copper already sits within a track's own clearance of
    the anchor (a differential-pair track routed close by a fine-pitch
    chip's own GND pin, say) was already that close before this search ever
    ran -- it is a fact about the board's existing geometry, not something
    the new via or connecting track introduces. Checking `start` itself (and
    the handful of samples nearest it, for a short candidate offset) against
    that condition rejects every candidate direction outright, regardless of
    how far the via and the bulk of the new track actually land, because
    every path shares the same first few millimetres. A one-time collision
    scan at `start` (at the same relaxed `track_keepout_radius` every other
    non-final sample uses) folds whatever already collides there into the
    per-path exclusion set, so a pre-existing near-anchor condition is
    tolerated for however long the new track happens to run close to it
    without weakening the genuinely new-copper check everywhere else,
    including the final via position, which is never widened by this.

    That per-path exemption is deliberately clearance-only. `track_layer`,
    when given, additionally runs an unconditional, exemption-proof check
    (`_crosses_foreign_track`) for a genuine geometric crossing between the
    whole straight `start`-`end` connecting track and any foreign-net track
    on that layer: two tracks physically overlapping is always a short, and
    nothing about a pre-existing near-anchor clearance condition licenses
    forgiving an outright intersection anywhere along the new copper, near
    the anchor or not."""
    near_anchor_radius = keepout_radius if track_keepout_radius is None else track_keepout_radius
    near_anchor_keys = set(exclude_keys or ())
    near_anchor_keys.update(_colliders(board, start, near_anchor_radius, hole_keepout_radius,
                                        exclude_netcode, exclude_keys))
    if track_layer is not None and _crosses_foreign_track(board, start, end, track_layer,
                                                            exclude_netcode, exclude_keys):
        return True
    for step in range(sample_count + 1):
        t = step / sample_count
        point = pcbnew.VECTOR2I(
            int(start.x + t * (end.x - start.x)),
            int(start.y + t * (end.y - start.y)),
        )
        is_last = step == sample_count
        keys = None if is_last else near_anchor_keys
        radius = keepout_radius if is_last or track_keepout_radius is None else track_keepout_radius
        if _collides(board, point, radius, hole_keepout_radius, exclude_netcode, keys):
            return True
    return False


# ---------------------------------------------------------------------------
# H3e follow-up passes. Island stitching above only ever looks at a GND
# island that isn't yet bonded to the plane; it is blind to a pad whose
# island *is* already bonded -- through a single via serving several other
# pads, or through a via far enough away that the pad's own return path is
# thin and long. These three passes close that gap:
#   - every SMD GND pad gets a via within 1.0 mm, regardless of island state;
#   - the J1 GND pad group specifically gets a second via (its four GND pads
#     collapse to two physical solder-pad locations, and one of those was
#     observed reaching the plane only through the other's via);
#   - each USB/Ethernet differential pair at J1/J2/J18/J20 gets a flanking
#     via near its connector break-in (H2b's return-path argument), placed
#     beside the pair rather than between its two pads;
#   - any Power-class (>=0.5 mm) net riding a single 0.3 mm-drill via gets a
#     parallel same-net via, so no supply path depends on one plated hole.
# All three reuse `_collides`/`_place_via_near_point`-style exact-geometry
# collision checks, never axis-aligned bounding boxes.

SMD_GAP_RADII_MM = (0.3, 0.5, 0.7, 0.85, 1.0)
SMD_GAP_OFFSETS_MM = [(0.0, 0.0)] + [
    (radius_mm * math.cos(angle), radius_mm * math.sin(angle))
    for radius_mm in SMD_GAP_RADII_MM
    for angle in _ANGLES_RAD
]

DIFF_PAIR_MAX_MM = 1.5
DIFF_PAIR_RADII_MM = (0.3, 0.5, 0.7, 0.9, 1.1, 1.3, 1.5)
# (connector reference, positive net, negative net) for every USB/Ethernet
# differential pair the H2b return-path argument covers.
DIFF_PAIRS = (
    ("J1", "/power-usb/USB_DP", "/power-usb/USB_DM"),
    ("J2", "/power-usb/USB_DP", "/power-usb/USB_DM"),
    ("J18", "/ethernet/ETH_TXP", "/ethernet/ETH_TXN"),
    ("J18", "/ethernet/ETH_RXP", "/ethernet/ETH_RXN"),
    ("J20", "/ethernet/ETH_TXP", "/ethernet/ETH_TXN"),
    ("J20", "/ethernet/ETH_RXP", "/ethernet/ETH_RXN"),
)
J1_GND_MIN_VIAS = 2
J1_GND_GROUP_RADIUS_MM = 3.0
POWER_VIA_MAX_DRILL_MM = 0.31
POWER_VIA_PAIR_MAX_MM = 1.0
# Extra headroom (mm) added, at increasing steps, on top of the exact
# hole-to-hole minimum computed per-via in `_power_parallel_offsets_mm` below.
POWER_PARALLEL_HEADROOM_STEPS_MM = (0.05, 0.2, 0.4, 0.6, 0.9, 1.2)


def _power_parallel_offsets_mm(drill, hole_keepout_radius):
    """Ring offsets for a same-net parallel via beside an existing via of
    drill `drill`, starting just past the exact DRC hole-to-hole minimum.

    `_collides` deliberately skips same-net copper (the whole point when
    stitching a new via *into* the plane it belongs to), so it never rejects
    a candidate that violates hole-to-hole spacing against the very via it is
    meant to parallel. `hole_keepout_radius` (`via_drill / 2 + hole_margin`,
    computed once in `main`) folds in the board's actual hole-to-hole/hole
    clearance minimums, so the center-to-center distance needed for two
    equal-drill holes to clear that margin is `drill / 2 + hole_keepout_radius`
    -- not a guessed constant, and correct if the board's DRC minimums differ
    from this design's defaults."""
    minimum_mm = (drill / 2 + hole_keepout_radius) / 1e6
    return [
        ((minimum_mm + headroom_mm) * math.cos(angle), (minimum_mm + headroom_mm) * math.sin(angle))
        for headroom_mm in POWER_PARALLEL_HEADROOM_STEPS_MM
        for angle in _ANGLES_RAD
    ]


def _smd_gnd_pads(board, netcode):
    return [pad for footprint in board.GetFootprints() for pad in footprint.Pads()
            if pad.GetNetCode() == netcode and pad.GetAttribute() == pcbnew.PAD_ATTRIB_SMD]


def _vias_on_net(board, netcode):
    return [track for track in board.GetTracks()
            if isinstance(track, pcbnew.PCB_VIA) and track.GetNetCode() == netcode]


def _nearest_via_distance_mm(point, vias):
    if not vias:
        return math.inf
    return min(_distance_mm(point, via.GetPosition()) for via in vias)


def _place_via_near_point(board, anchor_point, anchor_layer, offsets_mm, keepout_radius,
                           hole_keepout_radius, via_diameter, via_drill, netcode, track_width=None,
                           anchor_keys=None, track_keepout_radius=None):
    """Search `offsets_mm` around the anchor for a collision-free via center
    whose connecting track back to the anchor (when the chosen offset isn't
    the anchor itself) also clears every other net along its whole length.
    The anchor is a bare point (a pad or an existing via, a stub-track kink
    point, or the centroid of a small group of pads) that already has copper
    of its own, not a stub end to extend.

    When `track_width` is given, also adds a connecting track from
    `anchor_point` to the placed via on `anchor_layer` -- needed whenever the
    anchor's copper (an SMD pad, or another via on a net with no filled zone
    to bond through) would otherwise stay physically disconnected from the
    new via; a via with the right net code but no copper path to anything is
    a DRC `via_dangling` violation, not a real connection. When
    `track_width` is None, no track is added: the new via is meant to bond
    directly into a filled zone at its own location (e.g. the island walk's
    own stub-track kink-point case in `main`, which reuses existing copper
    without adding a new track).

    `anchor_keys` (a set of `_key()` values for the specific pad(s)/via(s)
    `anchor_point` was derived from) excludes those items from every
    collision check along the path, since they are the deliberate connection
    point (or, for a centroid anchor, the group it summarizes), not an
    obstacle -- without it, every candidate near a real pad/via anchor
    collides with that anchor's own copper. A stub-track kink point never
    trips this on its own account (no pad sits exactly there), but
    `_keys_touching_point` covers it too, so the island walk in `main` can
    call this function uniformly for both a kink-point anchor and a bare-pad
    anchor (a GND island with no routed copper at all, e.g. an isolated SMD
    pad) -- the earlier dedicated `_place_via` helper handled only the
    kink-point shape and silently failed every candidate for a bare-pad
    anchor, since its unexclusion-aware path check always sampled the
    anchor's own pad as its first point.

    `track_keepout_radius`, when given, is the connecting track's own
    clearance radius (`track_width / 2 + net clearance`) -- see
    `_path_collides_from_anchor` for why the path leading up to the via needs
    a smaller radius than the via itself. The connecting track's layer is
    passed through as `track_layer` only when a track will actually be
    added (`track_width is not None`), so `_path_collides_from_anchor` runs
    its unconditional foreign-track-crossing check exactly when there is a
    real track to check.

    Returns (via, track); `track` is None when `track_width` is None or the
    winning offset is exactly (0, 0) (anchor and via would coincide)."""
    for offset_x_mm, offset_y_mm in offsets_mm:
        candidate = pcbnew.VECTOR2I(
            anchor_point.x + pcbnew.FromMM(offset_x_mm),
            anchor_point.y + pcbnew.FromMM(offset_y_mm),
        )
        if _path_collides_from_anchor(board, anchor_point, candidate, keepout_radius,
                                       hole_keepout_radius, netcode, exclude_keys=anchor_keys,
                                       track_keepout_radius=track_keepout_radius,
                                       track_layer=anchor_layer if track_width is not None else None):
            continue
        via = pcbnew.PCB_VIA(board)
        via.SetPosition(candidate)
        via.SetWidth(via_diameter)
        via.SetDrill(via_drill)
        via.SetNetCode(netcode)
        via.SetViaType(pcbnew.VIATYPE_THROUGH)
        board.Add(via)
        track = None
        if track_width is not None and (offset_x_mm, offset_y_mm) != (0.0, 0.0):
            track = pcbnew.PCB_TRACK(board)
            track.SetStart(anchor_point)
            track.SetEnd(candidate)
            track.SetWidth(track_width)
            track.SetLayer(anchor_layer)
            track.SetNetCode(netcode)
            board.Add(track)
        return via, track
    return None, None


def _stitch_smd_pads_without_via(board, netcode, keepout_radius, hole_keepout_radius,
                                  via_diameter, via_drill, track_width, track_keepout_radius):
    """Every SMD GND pad without a via within 1.0 mm gets one, joined to the
    pad by a short connecting track (an SMD pad's own copper does not
    otherwise touch a via placed beside rather than under it)."""
    added = 0
    unresolved = []
    for pad in _smd_gnd_pads(board, netcode):
        if _nearest_via_distance_mm(pad.GetPosition(), _vias_on_net(board, netcode)) <= 1.0:
            continue
        footprint = pad.GetParentFootprint()
        ref = f"{footprint.GetReference()}.{pad.GetNumber()}" if footprint else pad.GetNumber()
        via, _track = _place_via_near_point(board, pad.GetPosition(), pad.GetPrincipalLayer(),
                                             SMD_GAP_OFFSETS_MM, keepout_radius, hole_keepout_radius,
                                             via_diameter, via_drill, netcode, track_width=track_width,
                                             anchor_keys=_keys_touching_point(board, pad.GetPosition()),
                                             track_keepout_radius=track_keepout_radius)
        if via is None:
            unresolved.append(ref)
            print(f"  no collision-free via site within 1.0 mm of SMD GND pad {ref}", flush=True)
            continue
        added += 1
        print(f"  added GND via near SMD pad {ref} at "
              f"({via.GetPosition().x / 1e6:.3f}, {via.GetPosition().y / 1e6:.3f})", flush=True)
    return added, unresolved


def _ensure_group_via_count(board, ref, netcode, min_vias, group_radius_mm,
                             keepout_radius, hole_keepout_radius, via_diameter, via_drill, track_width,
                             track_keepout_radius):
    """Ensure at least `min_vias` distinct via locations serve `ref`'s pads on
    `netcode` -- a whole connector's GND pad group sharing one via is
    invisible to both the per-pad 1.0 mm check (each pad may individually be
    close enough to *a* via) and the island walk (the group is already one
    bonded island)."""
    added = 0
    pads = [pad for footprint in board.GetFootprints() if footprint.GetReference() == ref
            for pad in footprint.Pads() if pad.GetNetCode() == netcode]
    if not pads:
        return added
    while True:
        vias = _vias_on_net(board, netcode)
        serving = {(via.GetPosition().x, via.GetPosition().y) for via in vias
                   if any(_distance_mm(pad.GetPosition(), via.GetPosition()) <= group_radius_mm
                          for pad in pads)}
        if len(serving) >= min_vias:
            return added
        target = max(pads, key=lambda pad: _nearest_via_distance_mm(pad.GetPosition(), vias))
        via, _track = _place_via_near_point(board, target.GetPosition(), target.GetPrincipalLayer(),
                                             CANDIDATE_OFFSETS_MM, keepout_radius, hole_keepout_radius,
                                             via_diameter, via_drill, netcode, track_width=track_width,
                                             anchor_keys=_keys_touching_point(board, target.GetPosition()),
                                             track_keepout_radius=track_keepout_radius)
        if via is None:
            print(f"  no collision-free site to raise {ref} GND via count above {len(serving)}", flush=True)
            return added
        added += 1
        print(f"  added redundancy GND via for {ref} GND group at "
              f"({via.GetPosition().x / 1e6:.3f}, {via.GetPosition().y / 1e6:.3f})", flush=True)


def _diff_pair_pads(board, ref, net_p, net_n):
    p_pads, n_pads = [], []
    for footprint in board.GetFootprints():
        if footprint.GetReference() != ref:
            continue
        for pad in footprint.Pads():
            if pad.GetNetname() == net_p:
                p_pads.append(pad)
            elif pad.GetNetname() == net_n:
                n_pads.append(pad)
    return p_pads, n_pads


def _centroid(pads):
    xs = [pad.GetPosition().x for pad in pads]
    ys = [pad.GetPosition().y for pad in pads]
    return pcbnew.VECTOR2I(int(sum(xs) / len(xs)), int(sum(ys) / len(ys)))


def _flanking_offsets(pad_point, other_point, radii_mm, angle_count=_ANGLE_COUNT):
    """Ring of offsets around `pad_point`, restricted to the half-plane facing
    away from `other_point` -- beside the pad group, never between the two
    halves of a differential pair (H2b's "flanking" placement)."""
    away_x = pad_point.x - other_point.x
    away_y = pad_point.y - other_point.y
    offsets = []
    for radius_mm in radii_mm:
        for step in range(angle_count):
            angle = 2 * math.pi * step / angle_count
            offset_x_mm = radius_mm * math.cos(angle)
            offset_y_mm = radius_mm * math.sin(angle)
            if offset_x_mm * away_x + offset_y_mm * away_y < 0:
                continue
            offsets.append((offset_x_mm, offset_y_mm))
    return offsets


def _stitch_diff_pair_flanking_vias(board, netcode, keepout_radius, hole_keepout_radius,
                                     via_diameter, via_drill):
    """One GND via within 1.5 mm of each named differential pair, beside (not
    between) its pads."""
    added = 0
    unresolved = []
    for ref, net_p, net_n in DIFF_PAIRS:
        p_pads, n_pads = _diff_pair_pads(board, ref, net_p, net_n)
        if not p_pads or not n_pads:
            print(f"  {ref} {net_p}/{net_n}: pads not found on this board, skipping", flush=True)
            continue
        all_pads = p_pads + n_pads
        vias = _vias_on_net(board, netcode)
        if min(_nearest_via_distance_mm(pad.GetPosition(), vias) for pad in all_pads) <= DIFF_PAIR_MAX_MM:
            continue
        centroid_p, centroid_n = _centroid(p_pads), _centroid(n_pads)
        via = None
        # A net with only one pad at this connector makes its own centroid
        # coincide exactly with that pad and its own routed trace (e.g. J20's
        # single-pad ETH_TXP/TXN) -- `_keys_touching_point` excludes both from
        # that anchor's own collision check, since they are the anchor, not
        # an obstacle. A multi-pad centroid (e.g. J1's two pads per net) sits
        # in genuinely open space between its own pads and touches nothing,
        # so it excludes nothing: a real collision there is real congestion,
        # not a self-collision artifact.
        for anchor, other in ((centroid_p, centroid_n), (centroid_n, centroid_p)):
            offsets = _flanking_offsets(anchor, other, DIFF_PAIR_RADII_MM)
            candidate_via, _track = _place_via_near_point(board, anchor, pcbnew.F_Cu, offsets, keepout_radius,
                                                            hole_keepout_radius, via_diameter, via_drill, netcode,
                                                            anchor_keys=_keys_touching_point(board, anchor))
            if candidate_via is None:
                continue
            nearest_mm = min(_distance_mm(pad.GetPosition(), candidate_via.GetPosition()) for pad in all_pads)
            if nearest_mm <= DIFF_PAIR_MAX_MM:
                via = candidate_via
                break
            board.Remove(candidate_via)
        if via is None:
            unresolved.append(f"{ref} {net_p}/{net_n}")
            print(f"  no collision-free flanking via within 1.5 mm of {ref} {net_p}/{net_n}", flush=True)
            continue
        added += 1
        print(f"  added flanking GND via for {ref} {net_p}/{net_n} at "
              f"({via.GetPosition().x / 1e6:.3f}, {via.GetPosition().y / 1e6:.3f})", flush=True)
    return added, unresolved


def _power_class_net_names(board):
    """Every net whose net class's track width is >=0.5 mm (the Power class
    on this design), by bare net-class membership rather than a hardcoded net
    name list."""
    net_settings = board.GetDesignSettings().m_NetSettings
    nets = board.GetNetsByName()
    names = []
    for net_name in nets.keys():
        class_name = nets[net_name].GetNetClassName().split(",")[0]
        net_class = net_settings.GetNetClassByName(class_name)
        if net_class is not None and net_class.GetTrackWidth() >= pcbnew.FromMM(0.5):
            names.append(str(net_name))
    return names


def _stitch_power_via_redundancy(board, keepout_radius, hole_keepout_radius, via_diameter):
    """Every Power-class via with a <=0.31 mm drill gets a parallel same-net
    via beside it (unless one is already within 1.0 mm), so no supply path
    depends on a single plated hole."""
    added = 0
    unresolved = []
    for net_name in _power_class_net_names(board):
        net_item = board.FindNet(net_name)
        if net_item is None:
            continue
        netcode = net_item.GetNetCode()
        class_name = net_item.GetNetClassName().split(",")[0]
        net_class = board.GetDesignSettings().m_NetSettings.GetNetClassByName(class_name)
        power_track_width = net_class.GetTrackWidth()
        power_track_keepout_radius = power_track_width // 2 + net_class.GetClearance()
        # Snapshot once and index the *same* list for the same-net exclusion
        # below: `_vias_on_net` wraps each PCB_VIA in a fresh SWIG proxy on
        # every call, so two separate calls never compare `is`-equal even for
        # the identical underlying via (see `_key`'s docstring above for the
        # same gotcha on pads/tracks).
        vias = _vias_on_net(board, netcode)
        for via in vias:
            if via.GetDrillValue() > pcbnew.FromMM(POWER_VIA_MAX_DRILL_MM):
                continue
            same_net_vias = [other for other in vias if other is not via]
            if _nearest_via_distance_mm(via.GetPosition(), same_net_vias) <= POWER_VIA_PAIR_MAX_MM:
                continue
            offsets = _power_parallel_offsets_mm(via.GetDrillValue(), hole_keepout_radius)
            parallel, _track = _place_via_near_point(board, via.GetPosition(), pcbnew.F_Cu, offsets,
                                                       keepout_radius, hole_keepout_radius, via_diameter,
                                                       via.GetDrillValue(), netcode, track_width=power_track_width,
                                                       anchor_keys=_keys_touching_point(board, via.GetPosition()),
                                                       track_keepout_radius=power_track_keepout_radius)
            location = f"({via.GetPosition().x / 1e6:.3f}, {via.GetPosition().y / 1e6:.3f})"
            if parallel is None:
                unresolved.append(f"{net_name} via at {location}")
                print(f"  no collision-free parallel via for {net_name} single via at {location}", flush=True)
                continue
            # `_place_via_near_point` only wired the F.Cu connecting track (the
            # `anchor_layer` it was called with). A via with copper touching it
            # on only one layer is a genuine DRC `via_dangling` finding, not a
            # false positive: it means this via is not actually parallel to
            # anything, just an expensive way to end an F.Cu stub. Bridge the
            # same two via positions on B.Cu too, so the new via genuinely
            # carries current on both layers alongside the original.
            if not _path_collides(board, via.GetPosition(), parallel.GetPosition(),
                                   keepout_radius, hole_keepout_radius, netcode):
                back_track = pcbnew.PCB_TRACK(board)
                back_track.SetStart(via.GetPosition())
                back_track.SetEnd(parallel.GetPosition())
                back_track.SetWidth(power_track_width)
                back_track.SetLayer(pcbnew.B_Cu)
                back_track.SetNetCode(netcode)
                board.Add(back_track)
            else:
                print(f"  parallel via for {net_name} at {location} could not get a B.Cu bridge "
                      f"(single-layer only)", flush=True)
            added += 1
            print(f"  added parallel via for {net_name} beside {location} at "
                  f"({parallel.GetPosition().x / 1e6:.3f}, {parallel.GetPosition().y / 1e6:.3f})", flush=True)
    return added, unresolved


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", type=Path, default=DEFAULT_BOARD,
                         help="board .kicad_pcb path (default: canonical aius-carrier.kicad_pcb)")
    args = parser.parse_args()
    board_path = str(args.board)

    board = pcbnew.LoadBoard(board_path)
    gnd = board.FindNet("GND")
    if gnd is None:
        print("no GND net on this board; nothing to stitch", flush=True)
        return 0
    netcode = gnd.GetNetCode()

    design_settings = board.GetDesignSettings()
    default_netclass = design_settings.m_NetSettings.GetDefaultNetclass()
    via_diameter = default_netclass.GetViaDiameter()
    via_drill = default_netclass.GetViaDrill()
    clearance = default_netclass.GetClearance()
    track_width = default_netclass.GetTrackWidth()
    keepout_radius = via_diameter // 2 + clearance
    # Hole-to-hole and hole-to-copper spacing are independent board-setup
    # constraints (m_HoleToHoleMin / m_HoleClearance), not the net-class
    # copper clearance; our new via's own drill has to clear whichever is
    # larger, from the center of any other drilled hole.
    hole_margin = max(design_settings.m_HoleToHoleMin, design_settings.m_HoleClearance)
    hole_keepout_radius = via_drill // 2 + hole_margin
    # The connecting-track portion of a stitching search (every path sample
    # before the final via position) only ever carries a `track_width`-wide
    # trace, not the via's own footprint -- see `_path_collides_from_anchor`.
    track_keepout_radius = track_width // 2 + clearance
    print(f"stitching via {via_diameter / 1e6:.3f}/{via_drill / 1e6:.3f} mm, "
          f"track {track_width / 1e6:.3f} mm, clearance {clearance / 1e6:.3f} mm, "
          f"hole margin {hole_margin / 1e6:.3f} mm", flush=True)

    board.BuildConnectivity()
    islands = _gnd_islands(board, netcode)
    isolated = [members for members in islands if not _is_plane_island(members)]
    print(f"found {len(islands)} GND islands, {len(isolated)} not yet bonded to the plane", flush=True)

    vias_added = 0
    tracks_added = 0
    unresolved = []
    for members in isolated:
        via, track = None, None
        for anchor_point, anchor_layer in _anchors(members):
            via, track = _place_via_near_point(
                board, anchor_point, anchor_layer, CANDIDATE_OFFSETS_MM, keepout_radius, hole_keepout_radius,
                via_diameter, via_drill, netcode, track_width=track_width,
                anchor_keys=_keys_touching_point(board, anchor_point),
                track_keepout_radius=track_keepout_radius)
            if via is not None:
                break
        if via is None:
            pad_refs = sorted({f"{m.GetParentFootprint().GetReference()}.{m.GetNumber()}"
                                for m in members if isinstance(m, pcbnew.PAD)})
            unresolved.append(pad_refs)
            print(f"  no collision-free via site near {pad_refs}", flush=True)
            continue
        vias_added += 1
        if track is not None:
            tracks_added += 1
        pad_refs = sorted({f"{m.GetParentFootprint().GetReference()}.{m.GetNumber()}"
                            for m in members if isinstance(m, pcbnew.PAD)})
        print(f"  stitched island ({pad_refs}) at "
              f"({via.GetPosition().x / 1e6:.3f}, {via.GetPosition().y / 1e6:.3f})"
              f"{' + new track' if track is not None else ' (reused stub end)'}", flush=True)

    board.BuildConnectivity()
    dangling_removed = 0
    for members in _gnd_islands(board, netcode):
        if _is_plane_island(members):
            continue
        if any(isinstance(member, pcbnew.PAD) for member in members):
            continue
        # A pure track/via island with no pad at all: a genuinely dangling
        # stub that stitching could not anchor to anything real. Remove it
        # rather than leave dead copper on the board.
        for member in members:
            board.Remove(member)
            dangling_removed += 1

    print(f"added {vias_added} vias, {tracks_added} connecting tracks, "
          f"removed {dangling_removed} dangling GND stub items, "
          f"{len(unresolved)} islands left unresolved", flush=True)

    smd_added, smd_unresolved = _stitch_smd_pads_without_via(
        board, netcode, keepout_radius, hole_keepout_radius, via_diameter, via_drill, track_width,
        track_keepout_radius)
    print(f"SMD GND pad coverage: added {smd_added} vias, "
          f"{len(smd_unresolved)} pads still >1.0 mm from a via", flush=True)

    j1_added = _ensure_group_via_count(
        board, "J1", netcode, J1_GND_MIN_VIAS, J1_GND_GROUP_RADIUS_MM,
        keepout_radius, hole_keepout_radius, via_diameter, via_drill, track_width, track_keepout_radius)
    print(f"J1 GND group redundancy: added {j1_added} vias", flush=True)

    pair_added, pair_unresolved = _stitch_diff_pair_flanking_vias(
        board, netcode, keepout_radius, hole_keepout_radius, via_diameter, via_drill)
    print(f"differential-pair flanking vias: added {pair_added}, "
          f"{len(pair_unresolved)} pairs unresolved", flush=True)

    power_added, power_unresolved = _stitch_power_via_redundancy(
        board, keepout_radius, hole_keepout_radius, via_diameter)
    print(f"Power-class via redundancy: added {power_added} parallel vias, "
          f"{len(power_unresolved)} single-via supply paths unresolved", flush=True)

    if board.Zones():
        pcbnew.ZONE_FILLER(board).Fill(board.Zones())
        print(f"refilled {len(board.Zones())} zones", flush=True)

    board.BuildConnectivity()
    pcbnew.SaveBoard(board_path, board)
    print(f"saved {board_path}", flush=True)
    any_unresolved = unresolved or smd_unresolved or pair_unresolved or power_unresolved
    return 1 if any_unresolved else 0


if __name__ == "__main__":
    raise SystemExit(main())
