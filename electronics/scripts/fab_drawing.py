#!/usr/bin/env python3
"""H3d fabrication drawing: a one-page PDF combining the board outline
(dimensioned), the two Hammond slot-edge keepout bands, the rear datum
(y = 0), and a stackup/DFM notes block, for the manufacturing package
`export_fab.sh` builds.

Every drawn value is read off `--board` or its own sidecar files, never
hardcoded, except the controlled-impedance ruling prose and the fixed
JLCPCB-4L fab-capability minimums (trace/space, via) that have no board-file
representation of their own:

  - board outline corners -- every `Edge.Cuts` `PCB_SHAPE` endpoint on the
    loaded board (the true drawn rectangle, not the line-width-inflated
    `GetBoardEdgesBoundingBox()`).
  - the two copper-keepout rule areas (`scaffold_pcb.py`'s Hammond
    slot-edge bands) -- every `ZONE` with `GetIsRuleArea()` and
    `GetDoNotAllowTracks()`, located by its own bounding box; the
    footprint-only USB cable envelope (`GetDoNotAllowTracks() == False`) is
    excluded.
  - NPTH mounting-hole count/diameter -- every pad with
    `PAD_ATTRIB_NPTH`, grouped by drill diameter.
  - the per-layer 4-layer stackup (copper/prepreg/core thicknesses, total,
    solder-mask colour, silkscreen colour, copper finish) -- parsed from the
    board file's own textual `(setup (stackup ...))` block. `pcbnew` 10.0.4's
    Python bindings expose no typed reader for `BOARD_STACKUP` (the same gap
    `scaffold_pcb.py` documents on the write side), so this is a bounded
    regex read of the S-expression pcbnew itself generated, not a
    hand-maintained duplicate; the parsed total is asserted against the
    board's own `(general (thickness ...))` field.
  - via drill/diameter -- the board's sidecar `.kicad_pro`
    `net_settings.classes` "Default" entry (every class in this design
    shares one via size).
  - min trace/space and the fabricator-nominal (pre-lamination) 1.6 mm
    finished thickness -- `feasibility/stackups.json`'s `jlcpcb-4L-1.6`
    entry, whose `copper_t_mm` is cross-checked against the board's own
    parsed outer-copper thickness.

Every drawing item lands on an in-memory `pcbnew.BOARD` loaded from a
disposable scratch copy of `--board` -- this script never re-saves over the
canonical/candidate file it reads. The scratch copy is then exported with
`kicad-cli pcb export pdf --mode-single -l Edge.Cuts,User.Drawings,F.Fab` to
a single-page PDF.

Run: scripts/kicad.sh --python electronics/scripts/fab_drawing.py
Run against a candidate: scripts/kicad.sh --python \
  electronics/scripts/fab_drawing.py --board <candidate.kicad_pcb> \
  --output <candidate-dir>/manufacturing/<stem>-fab-drawing.pdf

Exit: 0 on a written PDF; 3 on a missing/malformed stackups.json, a board
with no parseable `(stackup ...)` block, or a missing `kicad-cli`.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pcbnew

ELEC = Path(__file__).resolve().parent.parent
AIUS_ROOT = ELEC.parent
DEFAULT_BOARD = ELEC / "aius-carrier.kicad_pcb"
DEFAULT_STACKUPS = ELEC / "feasibility" / "stackups.json"
STACKUP_ID = "jlcpcb-4L-1.6"

EXPORT_LAYERS = "Edge.Cuts,User.Drawings,F.Fab"

# Fixed JLCPCB 4L fab-capability minimums with no board-file representation
# of their own (electronics/dfm/jlcpcb-4l.kicad_dru documents the same
# annular-ring-derived via floor; the design's OWN via, read from the
# board's .kicad_pro below, is the number actually drawn).
CONTROLLED_IMPEDANCE_NOTES = (
    "Controlled impedance: NOT REQUESTED -- accepted-pair contract per the",
    "2026-09-12 owner tolerance ruling (nominal net-class widths, <=5 mm intra-pair",
    "skew, no serpentine length matching; docs/plans/aius-h2-h3-manufacturing.md",
    "\"Owner rulings (2026-09-12)\").",
)

TITLE_TEXT_SIZE_MM = 2.2
NOTES_TEXT_SIZE_MM = 1.2
NOTES_LINE_SPACING_MM = 1.8
CALLOUT_TEXT_SIZE_MM = 1.0
DIMENSION_OFFSET_MM = 10.0
MARGIN_MM = 8.0


def _mm(nanometres: int) -> float:
    return nanometres / 1e6


def _edge_cuts_bounds(board: "pcbnew.BOARD") -> tuple[float, float, float, float]:
    """(min_x, min_y, max_x, max_y) mm of every `Edge.Cuts` `PCB_SHAPE`
    endpoint -- the true drawn outline, not the line-width-inflated
    `GetBoardEdgesBoundingBox()`."""
    xs: list[float] = []
    ys: list[float] = []
    for drawing in board.GetDrawings():
        if isinstance(drawing, pcbnew.PCB_SHAPE) and drawing.GetLayer() == pcbnew.Edge_Cuts:
            for point in (drawing.GetStart(), drawing.GetEnd()):
                xs.append(_mm(point.x))
                ys.append(_mm(point.y))
    if not xs or not ys:
        raise SystemExit("no Edge.Cuts drawing items found on the board")
    return min(xs), min(ys), max(xs), max(ys)


def _copper_keepout_bands(board: "pcbnew.BOARD") -> list[tuple[float, float, float, float]]:
    """Bounding box (x, y, width, height) mm of every copper-forbidding rule
    area (the Hammond slot-edge bands `scaffold_pcb.py` adds), sorted by x.
    Excludes the footprint-only USB cable envelope."""
    bands = []
    for zone in board.Zones():
        if zone.GetIsRuleArea() and zone.GetDoNotAllowTracks():
            bbox = zone.GetBoundingBox()
            bands.append((_mm(bbox.GetLeft()), _mm(bbox.GetTop()),
                          _mm(bbox.GetWidth()), _mm(bbox.GetHeight())))
    bands.sort(key=lambda band: band[0])
    return bands


def _npth_summary(board: "pcbnew.BOARD") -> dict[float, int]:
    """{diameter_mm: count} for every NPTH (mounting-hole) pad on the
    board."""
    counts: dict[float, int] = {}
    for footprint in board.GetFootprints():
        for pad in footprint.Pads():
            if pad.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH:
                diameter_mm = round(_mm(pad.GetDrillSize().x), 3)
                counts[diameter_mm] = counts.get(diameter_mm, 0) + 1
    return counts


def _via_drill_diameter_mm(board_path: Path) -> tuple[float, float]:
    """(drill_mm, diameter_mm) of the board's own "Default" net class, from
    the sidecar `.kicad_pro`. Every net class in this design shares one via
    size, so the Default entry stands for the whole board."""
    project_path = board_path.with_suffix(".kicad_pro")
    with project_path.open(encoding="utf-8") as project_file:
        project = json.load(project_file)
    for net_class in project["net_settings"]["classes"]:
        if net_class["name"] == "Default":
            return net_class["via_drill"], net_class["via_diameter"]
    raise SystemExit(f"no Default net class in {project_path}")


def _board_stackup(board_path: Path) -> tuple[list[tuple[str, float]], float, str, str, str]:
    """(layers, total_mm, mask_color, silk_color, copper_finish) parsed from
    the board file's own textual `(setup (stackup ...))` block -- `pcbnew`
    10.0.4 exposes no typed `BOARD_STACKUP` reader, only this raw
    S-expression (the same one `scaffold_pcb.py` inserts textually on the
    write side). `layers` is the physical layer/dielectric sequence in
    stack-up order (e.g. `[("F.Cu", 0.035), ("prepreg", 0.2104), ...]`).
    `total_mm` is cross-checked against the board's own top-level
    `(general (thickness ...))` field."""
    text = board_path.read_text(encoding="utf-8")

    stackup_match = re.search(r"\(stackup\n(.*?)\n\t\t\t\(copper_finish \"([^\"]*)\"", text, re.DOTALL)
    if stackup_match is None:
        raise SystemExit(f"no (setup (stackup ...)) block found in {board_path}")
    body, copper_finish = stackup_match.group(1), stackup_match.group(2)

    general_match = re.search(r"\(general\s*\n\s*\(thickness ([\d.]+)\)", text)
    if general_match is None:
        raise SystemExit(f"no (general (thickness ...)) field found in {board_path}")
    declared_total_mm = float(general_match.group(1))

    layer_blocks = re.findall(r'\(layer "([^"]+)"\s*\(type "([^"]+)"\)(.*?)(?=\n\t\t\t\(layer "|\Z)',
                               body, re.DOTALL)

    layers: list[tuple[str, float]] = []
    mask_color = ""
    silk_color = ""
    stack_types = {"copper", "prepreg", "core"}
    for name, layer_type, rest in layer_blocks:
        thickness_match = re.search(r"\(thickness ([\d.]+)\)", rest)
        color_match = re.search(r'\(color "([^"]+)"\)', rest)
        if thickness_match is not None and layer_type in stack_types:
            label = {"prepreg": "prepreg", "core": "core"}.get(layer_type, name)
            layers.append((label, float(thickness_match.group(1))))
        if color_match is not None and name.endswith("Mask"):
            mask_color = color_match.group(1)
        if color_match is not None and name.endswith("SilkS"):
            silk_color = color_match.group(1)

    total_mm = round(sum(thickness for _label, thickness in layers), 4)
    if abs(total_mm - declared_total_mm) > 1e-6:
        raise SystemExit(f"parsed stackup total {total_mm} mm disagrees with "
                          f"declared board thickness {declared_total_mm} mm")
    if not mask_color or not silk_color:
        raise SystemExit(f"could not find both mask and silk colours in {board_path}")

    return layers, total_mm, mask_color, silk_color, copper_finish


def _fab_capability(stackups_path: Path, outer_copper_mm: float) -> tuple[float, float, float]:
    """(min_trace_mm, min_space_mm, finished_mm) from the `jlcpcb-4L-1.6`
    entry, cross-checked against the board's own parsed outer-copper
    thickness."""
    with stackups_path.open(encoding="utf-8") as stackups_file:
        stackups = json.load(stackups_file)
    stackup = next((entry for entry in stackups["stackups"] if entry["id"] == STACKUP_ID), None)
    if stackup is None:
        raise SystemExit(f"no {STACKUP_ID!r} entry in {stackups_path}")
    if abs(stackup["copper_t_mm"] - outer_copper_mm) > 1e-6:
        raise SystemExit(f"{stackups_path} copper_t_mm disagrees with the board's own outer copper")
    return stackup["min_trace_mm"], stackup["min_space_mm"], stackup["finished_mm"]


def _add_dimension(board: "pcbnew.BOARD", start_mm: tuple[float, float],
                    end_mm: tuple[float, float], offset_mm: float) -> None:
    dimension = pcbnew.PCB_DIM_ALIGNED(board, pcbnew.PCB_DIM_ALIGNED_T)
    dimension.SetLayer(pcbnew.Dwgs_User)
    dimension.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(start_mm[0]), pcbnew.FromMM(start_mm[1])))
    dimension.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(end_mm[0]), pcbnew.FromMM(end_mm[1])))
    dimension.SetHeight(pcbnew.FromMM(offset_mm))
    dimension.SetPrecision(pcbnew.DIM_PRECISION_X_XX)
    dimension.SetUnitsMode(pcbnew.DIM_UNITS_MODE_MM)
    dimension.SetUnitsFormat(pcbnew.DIM_UNITS_FORMAT_BARE_SUFFIX)
    dimension.Update()
    board.Add(dimension)


def _add_text(board: "pcbnew.BOARD", position_mm: tuple[float, float], text: str,
              size_mm: float, angle_deg: float = 0.0,
              horiz_justify: int = pcbnew.GR_TEXT_H_ALIGN_LEFT) -> None:
    item = pcbnew.PCB_TEXT(board)
    item.SetLayer(pcbnew.Dwgs_User)
    item.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(position_mm[0]), pcbnew.FromMM(position_mm[1])))
    item.SetText(text)
    item.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(size_mm), pcbnew.FromMM(size_mm)))
    item.SetTextAngleDegrees(angle_deg)
    item.SetHorizJustify(horiz_justify)
    board.Add(item)


def _add_rectangle(board: "pcbnew.BOARD", x_mm: float, y_mm: float,
                    width_mm: float, height_mm: float) -> None:
    rectangle = pcbnew.PCB_SHAPE(board)
    rectangle.SetShape(pcbnew.SHAPE_T_RECTANGLE)
    rectangle.SetLayer(pcbnew.Dwgs_User)
    rectangle.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(x_mm), pcbnew.FromMM(y_mm)))
    rectangle.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(x_mm + width_mm), pcbnew.FromMM(y_mm + height_mm)))
    board.Add(rectangle)


def _draw_annotations(board: "pcbnew.BOARD", board_path: Path, stackups_path: Path) -> None:
    min_x, min_y, max_x, max_y = _edge_cuts_bounds(board)

    # Board outline, dimensioned: width below the front edge, height beside
    # the right edge.
    _add_dimension(board, (min_x, max_y), (max_x, max_y), DIMENSION_OFFSET_MM)
    _add_dimension(board, (max_x, min_y), (max_x, max_y), DIMENSION_OFFSET_MM)

    # Rear datum (y = 0, per PLACEMENT's "rear edge = y small" convention):
    # called out in place, at the rear edge itself. Offset a few mm below
    # the edge itself (rather than right on it) so the text clears the
    # sheet's own top margin.
    _add_text(board, (min_x + 2.0, min_y + 4.5), f"REAR DATUM (Y = {min_y:.2f} mm)",
              CALLOUT_TEXT_SIZE_MM)

    # The two Hammond slot-edge keepout bands, called out in place: an
    # outline over the actual rule-area footprint plus a rotated label
    # inside each band.
    bands = _copper_keepout_bands(board)
    for band_x, band_y, band_width, band_height in bands:
        _add_rectangle(board, band_x, band_y, band_width, band_height)
        _add_text(board, (band_x + band_width / 2 + 0.5, band_y + band_height / 2),
                  f"{band_width:.2f} mm NO-COPPER / NO-FOOTPRINT KEEPOUT (Hammond 1455N1201BK slot face)",
                  CALLOUT_TEXT_SIZE_MM, angle_deg=90.0)

    # Title + notes block, clear of the board and both dimensions. Starts a
    # few mm below the board's own top edge to clear the sheet's own top
    # margin (the board's rear edge sits close to the printable area's own
    # top bound).
    notes_x = max_x + DIMENSION_OFFSET_MM + MARGIN_MM
    notes_y = min_y + 6.0
    _add_text(board, (notes_x, notes_y), "AIUS CARRIER -- FABRICATION DRAWING (H3d)",
              TITLE_TEXT_SIZE_MM)
    notes_y += NOTES_LINE_SPACING_MM * 2

    layers, total_mm, mask_color, silk_color, copper_finish = _board_stackup(board_path)
    min_trace_mm, min_space_mm, finished_mm = _fab_capability(stackups_path, layers[0][1])
    via_drill_mm, via_diameter_mm = _via_drill_diameter_mm(board_path)
    npth = _npth_summary(board)

    stackup_line = " / ".join(f"{label} {thickness:g}" for label, thickness in layers)
    npth_line = ("none" if not npth else
                 ", ".join(f"{count}x Ø{diameter:g} mm" for diameter, count in sorted(npth.items())))

    notes = [
        "NOTES:",
        f"4-layer JLC04161H-7628 stackup (mm): {stackup_line}, total {total_mm:g} mm.",
        *CONTROLLED_IMPEDANCE_NOTES,
        f"Surface finish: {'TBD by quote' if copper_finish in ('', 'None') else copper_finish}.",
        f"Minimum trace/space: {min_trace_mm:g} / {min_space_mm:g} mm.",
        f"Minimum via: {via_drill_mm:g} mm drill / {via_diameter_mm:g} mm finished diameter.",
        f"NPTH mounting holes: {npth_line}.",
        f"Solder mask: {mask_color.lower()}. Silkscreen: {silk_color.lower()}.",
        f"Board thickness: {finished_mm:g} mm nominal (finished, pre-tolerance).",
    ]
    for line in notes:
        _add_text(board, (notes_x, notes_y), line, NOTES_TEXT_SIZE_MM)
        notes_y += NOTES_LINE_SPACING_MM


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--board", type=Path, default=DEFAULT_BOARD,
                         help="board .kicad_pcb path (default: canonical aius-carrier.kicad_pcb)")
    parser.add_argument("--stackups", type=Path, default=DEFAULT_STACKUPS,
                         help="feasibility stackups.json path (default: electronics/feasibility/stackups.json)")
    parser.add_argument("--output", type=Path, default=None,
                         help="output PDF path (default: <board-dir>/manufacturing/<board-stem>-fab-drawing.pdf)")
    args = parser.parse_args()

    board_path = args.board.resolve()
    if not args.stackups.exists():
        print(f"stackups file not found: {args.stackups}", file=sys.stderr)
        return 3
    if shutil.which("kicad-cli") is None:
        print("kicad-cli is required", file=sys.stderr)
        return 3

    board_dir = board_path.parent
    board_stem = board_path.stem
    output_path = args.output if args.output is not None else board_dir / "manufacturing" / f"{board_stem}-fab-drawing.pdf"
    output_path.parent.mkdir(parents=True, exist_ok=True)

    scratch_dir = AIUS_ROOT / ".scratch" / "fabdwg" / board_stem
    if scratch_dir.exists():
        shutil.rmtree(scratch_dir)
    scratch_dir.mkdir(parents=True)
    scratch_board_path = scratch_dir / f"{board_stem}.kicad_pcb"
    shutil.copy(board_path, scratch_board_path)
    shutil.copy(board_path.with_suffix(".kicad_pro"), scratch_board_path.with_suffix(".kicad_pro"))
    for sidecar in ("fp-lib-table", "sym-lib-table"):
        source = ELEC / sidecar
        if source.exists():
            shutil.copy(source, scratch_dir / sidecar)
    lib_source = ELEC / "lib"
    if lib_source.exists():
        shutil.copytree(lib_source, scratch_dir / "lib")

    board = pcbnew.LoadBoard(str(scratch_board_path))
    _draw_annotations(board, board_path, args.stackups)
    pcbnew.SaveBoard(str(scratch_board_path), board)

    print(f"==> Exporting fabrication drawing from {scratch_board_path}")
    subprocess.run(
        ["kicad-cli", "pcb", "export", "pdf", "--mode-single",
         "-l", EXPORT_LAYERS, "-o", str(output_path), str(scratch_board_path)],
        check=True,
    )
    print(f"==> Fabrication drawing written to {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
