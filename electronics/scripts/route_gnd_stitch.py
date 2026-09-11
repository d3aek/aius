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


def _collides(board, center, keepout_radius, hole_keepout_radius, exclude_netcode):
    """True when a via centered at `center` would land inside any pad
    (via-in-pad, regardless of net), within `hole_keepout_radius` of any
    drilled hole (hole-to-hole / hole-to-copper spacing binds regardless of
    net -- two isolated nets' drills can still break into each other), or
    within `keepout_radius` of any different-net copper (the project's
    Default clearance plus the via's own radius) -- the same conditions
    `kicad-cli pcb drc` would flag. Uses each item's real geometry (bounding
    circle for a pad, exact segment distance for a track or via) rather than
    axis-aligned bounding boxes, which false-positive on any diagonal track
    spanning the search area. Every pad is checked regardless of copper layer
    -- an NPTH mechanical mounting hole has no copper but still enforces hole
    clearance against a new via's own drill."""
    keepout_radius_mm = keepout_radius / 1e6
    hole_keepout_radius_mm = hole_keepout_radius / 1e6
    for footprint in board.GetFootprints():
        for pad in footprint.Pads():
            pad_radius_mm = pad.GetBoundingRadius() / 1e6
            distance = _distance_mm(center, pad.GetPosition())
            if distance <= pad_radius_mm:
                return True
            has_hole = pad.GetDrillSizeX() > 0
            if has_hole and distance <= pad_radius_mm + hole_keepout_radius_mm:
                return True
            if (not has_hole and pad.IsOnCopperLayer() and pad.GetNetCode() != exclude_netcode
                    and distance <= pad_radius_mm + keepout_radius_mm):
                return True
    for track in board.GetTracks():
        if not track.IsOnCopperLayer():
            continue
        if track.GetNetCode() == exclude_netcode:
            continue
        # PCB_VIA.GetWidth() with no layer argument asserts in KiCad 10 (a via's
        # copper diameter is queried per-layer); a through-hole via is the same
        # diameter on every layer, so F_Cu is as good a layer as any to ask.
        width = track.GetWidth(pcbnew.F_Cu) if isinstance(track, pcbnew.PCB_VIA) else track.GetWidth()
        half_width_mm = width / 1e6 / 2
        distance = _point_to_segment_distance_mm(center, track.GetStart(), track.GetEnd())
        if distance <= half_width_mm + keepout_radius_mm:
            return True
        if isinstance(track, pcbnew.PCB_VIA):
            drill_radius_mm = track.GetDrillValue() / 1e6 / 2
            if distance <= drill_radius_mm + hole_keepout_radius_mm:
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


def _place_via(board, anchor_point, anchor_layer, keepout_radius, hole_keepout_radius,
                via_diameter, via_drill, track_width, netcode):
    """Search CANDIDATE_OFFSETS_MM around the anchor for a collision-free via
    center whose connecting track back to the anchor (when the chosen offset
    isn't the anchor itself) also clears every other net along its whole
    length. Returns (via, track|None) or (None, None) if every candidate
    collided."""
    for offset_x_mm, offset_y_mm in CANDIDATE_OFFSETS_MM:
        candidate = pcbnew.VECTOR2I(
            anchor_point.x + pcbnew.FromMM(offset_x_mm),
            anchor_point.y + pcbnew.FromMM(offset_y_mm),
        )
        if _path_collides(board, anchor_point, candidate, keepout_radius, hole_keepout_radius, netcode):
            continue
        via = pcbnew.PCB_VIA(board)
        via.SetPosition(candidate)
        via.SetWidth(via_diameter)
        via.SetDrill(via_drill)
        via.SetNetCode(netcode)
        via.SetViaType(pcbnew.VIATYPE_THROUGH)
        board.Add(via)
        track = None
        if (offset_x_mm, offset_y_mm) != (0.0, 0.0):
            track = pcbnew.PCB_TRACK(board)
            track.SetStart(anchor_point)
            track.SetEnd(candidate)
            track.SetWidth(track_width)
            track.SetLayer(anchor_layer)
            track.SetNetCode(netcode)
            board.Add(track)
        return via, track
    return None, None


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
            via, track = _place_via(board, anchor_point, anchor_layer, keepout_radius, hole_keepout_radius,
                                     via_diameter, via_drill, track_width, netcode)
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

    if board.Zones():
        pcbnew.ZONE_FILLER(board).Fill(board.Zones())
        print(f"refilled {len(board.Zones())} zones", flush=True)

    board.BuildConnectivity()
    pcbnew.SaveBoard(board_path, board)
    print(f"saved {board_path}", flush=True)
    return 1 if unresolved else 0


if __name__ == "__main__":
    raise SystemExit(main())
