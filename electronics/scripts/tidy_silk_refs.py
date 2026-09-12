#!/usr/bin/env python3
"""H3e third-pass silkscreen tidy: relocate a reference designator's text off
any obstacle it currently sits inside, by trying eight fixed positions
around the part's own courtyard.

Every footprint's `Reference()` text on `F.SilkS` is tested against four
obstacle classes: (a) any OTHER footprint's `F.CrtYd` courtyard polygon, (b)
any OTHER footprint's pad on `F.Cu` or `F.Mask` (a footprint's own pads are
excluded -- a reference sitting near its own two-pin part is normal reviewed
layout; every one of the seven reported findings is a neighbour intrusion,
not a self-overlap), (c) any OTHER silk text item (another footprint's
reference/value/graphical text, or a standalone board silkscreen text) on
`F.SilkS`, and (d) the board edge (`Edge.Cuts`) interior, with a small
keep-in margin. A footprint whose reference clears all four is left
untouched.

For a violator, eight candidate positions are tried in this fixed order:
above, below, left, right of the footprint's own `F.CrtYd` courtyard
bounding box at a 0.3 mm gap, then the four diagonals (corner-aligned, same
gap on both axes) -- first at the reference's current rotation, then (if no
candidate at that rotation clears every obstacle) at the other of 0/90
degrees. The first candidate that intersects nothing is taken; only the
text's position (and, only if needed, its rotation) changes -- size and
stroke thickness are never touched, so the 0.16 mm DFM stroke floor
(`check_dfm.sh`) is preserved automatically. A violator with no
collision-free candidate in either rotation is left at its current position
and reported as a residual.

Every other footprint's own courtyard/pad/silk-text set is read fresh off
the board before each violator is evaluated, so an already-relocated
reference is a real obstacle for the next one -- this script never places
two reference texts on top of each other.

Geometry is intentionally axis-aligned-bounding-box based for the text
(pcbnew's own `GetBoundingBox()`, which already accounts for the text's
current rotation) and true-polygon based for courtyards and pads
(`SHAPE_POLY_SET.Collide` against the courtyard polygon / the pad's own
`GetEffectiveShape()`) -- a conservative, never-tighter-than-real test for
the text side, matched against the pads/courtyards' real geometry.

Run against a scratch copy first (never the canonical board by default):
  scripts/kicad.sh --python electronics/scripts/tidy_silk_refs.py \\
    --board <scratch>/aius-carrier.kicad_pcb
Preview without saving:
  scripts/kicad.sh --python electronics/scripts/tidy_silk_refs.py \\
    --board <scratch>/aius-carrier.kicad_pcb --dry-run
Apply to the canonical board once the scratch run is proven clean
(`kicad-cli pcb drc --severity-all` 0/0, `check_dfm.sh` 0):
  scripts/kicad.sh --python electronics/scripts/tidy_silk_refs.py

Exit: 0 every violator found a collision-free candidate (or none existed);
1 at least one violator is left as a residual.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import pcbnew

ELEC = Path(__file__).resolve().parent.parent
DEFAULT_BOARD = ELEC / "aius-carrier.kicad_pcb"

GAP_MM = 0.3
BOARD_EDGE_MARGIN_MM = 0.2
ROTATION_CANDIDATES_DEG = (0.0, 90.0)

# Fixed try-order: four cardinal sides of the part's own courtyard, then the
# four corners -- see module docstring.
DIRECTIONS = ("N", "S", "W", "E", "NE", "NW", "SE", "SW")


def _mm(value: int) -> float:
    return pcbnew.ToMM(value)


def _iu(value_mm: float) -> int:
    return pcbnew.FromMM(value_mm)


def _rect_polyset(x_mm: float, y_mm: float, w_mm: float, h_mm: float) -> "pcbnew.SHAPE_POLY_SET":
    """An axis-aligned rectangle (mm, top-left corner `(x_mm, y_mm)`) as a
    one-outline `SHAPE_POLY_SET` -- the common shape both the candidate text
    box and every "other silk text" obstacle are built from."""
    poly = pcbnew.SHAPE_POLY_SET()
    poly.NewOutline()
    for corner_x, corner_y in (
        (x_mm, y_mm), (x_mm + w_mm, y_mm),
        (x_mm + w_mm, y_mm + h_mm), (x_mm, y_mm + h_mm),
    ):
        poly.Append(_iu(corner_x), _iu(corner_y))
    return poly


def _box_mm(box) -> tuple[float, float, float, float]:
    """A pcbnew `BOX2I` -> `(x, y, w, h)` mm tuple."""
    return (_mm(box.GetX()), _mm(box.GetY()), _mm(box.GetWidth()), _mm(box.GetHeight()))


def _text_rect_mm(text) -> tuple[float, float, float, float]:
    """`text`'s own `GetBoundingBox()` in mm -- already axis-aligned and
    already accounts for the text's current rotation."""
    return _box_mm(text.GetBoundingBox())


def _footprint_courtyard_bbox_mm(footprint) -> tuple[float, float, float, float]:
    """`footprint`'s own `F.CrtYd` courtyard bounding box in mm. Falls back
    to the footprint's own overall bounding box when it declares no front
    courtyard outline (an approximation for the rare part with none)."""
    courtyard = footprint.GetCourtyard(pcbnew.F_CrtYd)
    if courtyard.OutlineCount() == 0:
        return _box_mm(footprint.GetBoundingBox())
    return _box_mm(courtyard.BBox())


def _silk_reference_targets(board) -> list:
    """Every footprint whose `Reference()` text sits on `F.SilkS` -- the only
    text this script ever considers moving, per the module contract."""
    targets = []
    for footprint in board.GetFootprints():
        reference = footprint.Reference()
        if reference.GetLayer() == pcbnew.F_SilkS:
            targets.append(footprint)
    return targets


def _other_courtyards(board, exclude_reference: str) -> list:
    """Every OTHER footprint's `F.CrtYd` courtyard polygon. Exclusion is by
    reference-designator string, not Python object identity: pcbnew's SWIG
    bindings hand back a fresh proxy object on every `board.GetFootprints()`
    call, so `is` comparison across two separate calls never matches even
    for the same underlying footprint -- confirmed empirically before
    settling on this comparison (an identity check here silently included
    every footprint's own courtyard as a "foreign" obstacle, flagging
    nearly every reference on the board)."""
    return [
        footprint.GetCourtyard(pcbnew.F_CrtYd)
        for footprint in board.GetFootprints()
        if footprint.GetReference() != exclude_reference
        and footprint.GetCourtyard(pcbnew.F_CrtYd).OutlineCount() > 0
    ]


def _front_pad_shapes(board, exclude_reference: str) -> list:
    """Every OTHER footprint's pad with copper or mask on the front side --
    `F.Cu` or `F.Mask`, matching `F.SilkS` reference text. A footprint's own
    pads are excluded: its own reference sitting near its own two-pin pads
    is normal, reviewed layout, not the neighbour-intrusion class of finding
    this pass exists to fix (per the H3e review, all seven reported cases
    are one part's silkscreen landing on a DIFFERENT footprint). Exclusion
    is by reference-designator string -- see `_other_courtyards` for why."""
    shapes = []
    for footprint in board.GetFootprints():
        if footprint.GetReference() == exclude_reference:
            continue
        for pad in footprint.Pads():
            if pad.IsOnLayer(pcbnew.F_Cu):
                shapes.append(pad.GetEffectiveShape(pcbnew.F_Cu))
            if pad.IsOnLayer(pcbnew.F_Mask):
                shapes.append(pad.GetEffectiveShape(pcbnew.F_Mask))
    return shapes


def _other_silk_text_rects_mm(board, exclude_reference: str) -> list[tuple[float, float, float, float]]:
    """Every other `F.SilkS` TEXT item's own bounding box in mm: every
    footprint's reference/value/graphical text plus standalone board
    silkscreen text, excluding the `Reference()` field of the footprint
    named `exclude_reference` -- the exact item this pass may be about to
    move. Filtered to `PCB_TEXT` instances only (`isinstance`, not
    `hasattr("GetText")`, which would also true-positive on some non-text
    SWIG proxies) -- `footprint.GraphicalItems()` also yields plain
    `PCB_SHAPE` silkscreen graphics (footprint outline lines, polarity
    marks), which are not the "other silk text" this pass reads about and
    are expected to sit close to a part's own reference. Exclusion is
    index-based (candidates[0] is always `Reference()`), not object
    identity: see `_other_courtyards` for why an identity check against a
    value read from a separate `board.GetFootprints()`/field accessor call
    never matches pcbnew's SWIG proxies, even for the same underlying item."""
    rects = []
    for footprint in board.GetFootprints():
        candidates = [footprint.Reference(), footprint.Value()]
        candidates.extend(footprint.GraphicalItems())
        for index, item in enumerate(candidates):
            if index == 0 and footprint.GetReference() == exclude_reference:
                continue
            if not isinstance(item, pcbnew.PCB_TEXT):
                continue
            if item.GetLayer() != pcbnew.F_SilkS:
                continue
            rects.append(_text_rect_mm(item))
    for drawing in board.GetDrawings():
        if isinstance(drawing, pcbnew.PCB_TEXT) and drawing.GetLayer() == pcbnew.F_SilkS:
            rects.append(_text_rect_mm(drawing))
    return rects


def _board_interior_mm(board) -> tuple[float, float, float, float]:
    """The board's own `Edge.Cuts` bounding box, shrunk by
    `BOARD_EDGE_MARGIN_MM` on every side -- a candidate rect must stay fully
    inside this to count as clear of the board edge."""
    edge_box = board.GetBoardEdgesBoundingBox()
    x, y, w, h = _box_mm(edge_box)
    margin = BOARD_EDGE_MARGIN_MM
    return (x + margin, y + margin, w - 2 * margin, h - 2 * margin)


def _rect_outside_interior(rect_mm: tuple[float, float, float, float],
                            interior_mm: tuple[float, float, float, float]) -> bool:
    rect_x, rect_y, rect_w, rect_h = rect_mm
    interior_x, interior_y, interior_w, interior_h = interior_mm
    return (rect_x < interior_x or rect_y < interior_y
            or rect_x + rect_w > interior_x + interior_w
            or rect_y + rect_h > interior_y + interior_h)


def _collides_with_any(poly, shapes: list) -> bool:
    return any(poly.Collide(shape) for shape in shapes)


class _Obstacles:
    """One footprint's worth of obstacles, snapshotted fresh off the board
    immediately before that footprint's reference is evaluated -- so an
    already-relocated reference from an earlier footprint in this same pass
    is a real obstacle for every footprint evaluated after it."""

    __slots__ = ("courtyard_polys", "pad_shapes", "other_text_rects", "interior_mm")

    def __init__(self, board, exclude_reference: str) -> None:
        self.courtyard_polys = _other_courtyards(board, exclude_reference)
        self.pad_shapes = _front_pad_shapes(board, exclude_reference)
        self.other_text_rects = _other_silk_text_rects_mm(board, exclude_reference)
        self.interior_mm = _board_interior_mm(board)

    def candidate_clear(self, rect_mm: tuple[float, float, float, float]) -> bool:
        if _rect_outside_interior(rect_mm, self.interior_mm):
            return False
        candidate_poly = _rect_polyset(*rect_mm)
        if _collides_with_any(candidate_poly, self.courtyard_polys):
            return False
        if _collides_with_any(candidate_poly, self.pad_shapes):
            return False
        for other_rect in self.other_text_rects:
            if _rects_overlap(rect_mm, other_rect):
                return False
        return True


def _rects_overlap(a: tuple[float, float, float, float], b: tuple[float, float, float, float]) -> bool:
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    return ax < bx + bw and ax + aw > bx and ay < by + bh and ay + ah > by


def _candidate_rect_mm(direction: str, courtyard_mm: tuple[float, float, float, float],
                        width_mm: float, height_mm: float) -> tuple[float, float, float, float]:
    court_x, court_y, court_w, court_h = courtyard_mm
    gap = GAP_MM
    if direction == "N":
        x, y = court_x + (court_w - width_mm) / 2, court_y - gap - height_mm
    elif direction == "S":
        x, y = court_x + (court_w - width_mm) / 2, court_y + court_h + gap
    elif direction == "W":
        x, y = court_x - gap - width_mm, court_y + (court_h - height_mm) / 2
    elif direction == "E":
        x, y = court_x + court_w + gap, court_y + (court_h - height_mm) / 2
    elif direction == "NE":
        x, y = court_x + court_w + gap, court_y - gap - height_mm
    elif direction == "NW":
        x, y = court_x - gap - width_mm, court_y - gap - height_mm
    elif direction == "SE":
        x, y = court_x + court_w + gap, court_y + court_h + gap
    elif direction == "SW":
        x, y = court_x - gap - width_mm, court_y + court_h + gap
    else:
        raise ValueError(f"unknown direction {direction!r}")
    return (x, y, width_mm, height_mm)


def _is_violation(reference, obstacles: _Obstacles) -> bool:
    return not obstacles.candidate_clear(_text_rect_mm(reference))


def _attempt_relocate(footprint, reference, obstacles: _Obstacles) -> tuple[bool, str]:
    """Try every (rotation, direction) candidate in the module's fixed
    order; apply and return `(True, detail)` on the first collision-free
    hit, else leave the reference untouched and return `(False, detail)`."""
    original_angle = reference.GetTextAngleDegrees()
    original_position = reference.GetPosition()
    courtyard_mm = _footprint_courtyard_bbox_mm(footprint)

    rotations = [original_angle]
    for candidate_angle in ROTATION_CANDIDATES_DEG:
        if abs(candidate_angle - original_angle) > 0.01 and candidate_angle not in rotations:
            rotations.append(candidate_angle)

    tried = []
    for angle in rotations:
        reference.SetTextAngleDegrees(angle)
        current_x, current_y, width_mm, height_mm = _text_rect_mm(reference)
        for direction in DIRECTIONS:
            target_rect = _candidate_rect_mm(direction, courtyard_mm, width_mm, height_mm)
            tried.append(f"{angle:g}deg/{direction}")
            if not obstacles.candidate_clear(target_rect):
                continue
            target_x, target_y, _, _ = target_rect
            delta_x_mm, delta_y_mm = target_x - current_x, target_y - current_y
            reference.SetPosition(pcbnew.VECTOR2I(
                original_position.x + _iu(delta_x_mm), original_position.y + _iu(delta_y_mm)))
            detail = (f"{angle:g} deg, {direction} of own courtyard, "
                      f"{GAP_MM:g} mm gap ({len(tried)} candidate(s) tried)")
            return True, detail

    reference.SetTextAngleDegrees(original_angle)
    reference.SetPosition(original_position)
    return False, f"no collision-free candidate ({len(tried)} tried: {', '.join(tried)})"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--board", type=Path, default=DEFAULT_BOARD,
                         help="board .kicad_pcb path (default: canonical aius-carrier.kicad_pcb)")
    parser.add_argument("--dry-run", action="store_true",
                         help="print planned moves/residuals only; never saves the board")
    args = parser.parse_args()
    board_path = str(args.board)

    board = pcbnew.LoadBoard(board_path)
    targets = _silk_reference_targets(board)

    moved = 0
    residual = 0
    for footprint in targets:
        reference = footprint.Reference()
        ref_name = footprint.GetReference()
        obstacles = _Obstacles(board, ref_name)
        if not _is_violation(reference, obstacles):
            continue
        fixed, detail = _attempt_relocate(footprint, reference, obstacles)
        if fixed:
            moved += 1
            print(f"MOVED {ref_name}: {detail}")
        else:
            residual += 1
            print(f"RESIDUAL {ref_name}: {detail}")

    print(f"\n==> {moved} reference(s) moved, {residual} residual(s), "
          f"{len(targets)} F.SilkS reference(s) inspected")

    if args.dry_run:
        print("==> --dry-run: board not saved")
        return 1 if residual else 0

    pcbnew.SaveBoard(board_path, board)
    print(f"==> saved {board_path}")
    return 1 if residual else 0


if __name__ == "__main__":
    raise SystemExit(main())
