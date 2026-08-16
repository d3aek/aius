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
"""

from __future__ import annotations

from pathlib import Path

import pcbnew

ELEC = Path(__file__).resolve().parent.parent
BOARD = str(ELEC / "aius-carrier.kicad_pcb")
DSN = str(ELEC / "aius-carrier.dsn")

TRACK_MM = 0.20
CLEAR_MM = 0.15
VIA_DIA_MM = 0.60   # both fabs' standard via; annular (0.60-0.30)/2 = 0.15 mm
VIA_DRILL_MM = 0.30


def main() -> int:
    board = pcbnew.LoadBoard(BOARD)
    ncs = board.GetAllNetClasses()
    default = ncs["Default"]
    default.SetTrackWidth(pcbnew.FromMM(TRACK_MM))
    default.SetClearance(pcbnew.FromMM(CLEAR_MM))
    default.SetViaDiameter(pcbnew.FromMM(VIA_DIA_MM))
    default.SetViaDrill(pcbnew.FromMM(VIA_DRILL_MM))
    print(f"default net class: track {TRACK_MM} mm, clearance {CLEAR_MM} mm, "
          f"via {VIA_DIA_MM}/{VIA_DRILL_MM} mm", flush=True)

    ok = pcbnew.ExportSpecctraDSN(board, DSN)
    print(f"ExportSpecctraDSN -> {DSN}: {ok}", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
