#!/usr/bin/env python3
"""H3c step 1: set net-class widths and export the Specctra DSN for Freerouting.

Runs in the pinned KiCad container (pcbnew Python API). Loads the scaffolded
board, sets the default net class to the H2a impedance geometry (0.20 mm track /
0.15 mm clearance - the USB 90 ohm and Ethernet 100 ohm width over the L2 plane,
and fine for logic), and writes aius-carrier.dsn for the autorouter.

Note: autorouters route differential pairs as independent single-ended traces at
the class width, not tightly coupled. Over the solid L2 GND plane on this small
board's short runs that is acceptable; pair coupling/length-matching is an H3e
review refinement.

Run: python3 electronics/scripts/route_export_dsn.py
Run against a scaffolded candidate: python3 electronics/scripts/route_export_dsn.py \
  --board <candidate.kicad_pcb>
"""

from __future__ import annotations

import argparse
from pathlib import Path

import pcbnew

ELEC = Path(__file__).resolve().parent.parent
DEFAULT_BOARD = ELEC / "aius-carrier.kicad_pcb"

TRACK_MM = 0.20
CLEAR_MM = 0.15
VIA_DIA_MM = 0.60   # both fabs' standard via; annular (0.60-0.30)/2 = 0.15 mm
VIA_DRILL_MM = 0.30


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", type=Path, default=DEFAULT_BOARD,
                         help="board .kicad_pcb path (default: canonical aius-carrier.kicad_pcb)")
    args = parser.parse_args()
    board_path = args.board
    dsn_path = board_path.with_suffix(".dsn")

    board = pcbnew.LoadBoard(str(board_path))
    ncs = board.GetAllNetClasses()
    default = ncs["Default"]
    default.SetTrackWidth(pcbnew.FromMM(TRACK_MM))
    default.SetClearance(pcbnew.FromMM(CLEAR_MM))
    default.SetViaDiameter(pcbnew.FromMM(VIA_DIA_MM))
    default.SetViaDrill(pcbnew.FromMM(VIA_DRILL_MM))
    print(f"default net class: track {TRACK_MM} mm, clearance {CLEAR_MM} mm, "
          f"via {VIA_DIA_MM}/{VIA_DRILL_MM} mm", flush=True)

    # pcbnew's Specctra DSN exporter writes every rule area as a plain DSN
    # `(keepout ...)` block regardless of its own DoNotAllowTracks/Vias flags
    # (confirmed against the exported DSN: scaffold_pcb.py's USB cable
    # envelope, a footprint-only rule area with DoNotAllowTracks/Vias both
    # False, still lands as `(keepout "" (polygon F.Cu ...))`, indistinguishable
    # from the copper-forbidding edge keepouts) -- Freerouting then treats it
    # as a track/via keepout too, even though nothing in the source board
    # forbids routing there. Detach every footprint-only rule area from this
    # in-memory board *before* exporting so Freerouting never sees it; this
    # board is never saved, so the canonical/candidate .kicad_pcb keeps its
    # rule area (still enforced by kicad-cli pcb drc against footprint
    # placement) unchanged.
    detached = 0
    for zone in list(board.Zones()):
        if zone.GetIsRuleArea() and not zone.GetDoNotAllowTracks() and not zone.GetDoNotAllowVias():
            board.Remove(zone)
            detached += 1
    print(f"detached {detached} footprint-only rule area(s) before DSN export "
          f"(Specctra export has no way to express a footprint-only keepout)", flush=True)

    ok = pcbnew.ExportSpecctraDSN(board, str(dsn_path))
    print(f"ExportSpecctraDSN -> {dsn_path}: {ok}", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
