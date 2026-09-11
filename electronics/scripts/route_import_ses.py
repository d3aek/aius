#!/usr/bin/env python3
"""H3c step 2: import the Freerouting .ses back into the board and fill pours.

Runs in the pinned KiCad container (pcbnew Python API). Loads the scaffolded
board, imports the routed session (tracks + vias from Freerouting), fills the
copper zones (L2/L3 GND planes), and saves the routed board.

Run: python3 electronics/scripts/route_import_ses.py
Run against a scaffolded candidate: python3 electronics/scripts/route_import_ses.py \
  --board <candidate.kicad_pcb>
"""

from __future__ import annotations

import argparse
from pathlib import Path

import pcbnew

ELEC = Path(__file__).resolve().parent.parent
DEFAULT_BOARD = ELEC / "aius-carrier.kicad_pcb"


def set_fab_rules(board) -> None:
    """DRC minimums to both fabs' standard 4-layer capability (PCBWay / JLCPCB).

    KiCad's defaults are more conservative than the fabs; align them so DRC checks
    against real capability and the routed geometry (0.20 mm track, 0.15 mm
    clearance, 0.60/0.30 mm vias) validates cleanly. All values are inside both
    fabs' standard (non-advanced) process."""
    ds = board.GetDesignSettings()
    ds.m_MinClearance = pcbnew.FromMM(0.127)
    ds.m_TrackMinWidth = pcbnew.FromMM(0.127)
    ds.m_ViasMinSize = pcbnew.FromMM(0.45)
    ds.m_MinThroughDrill = pcbnew.FromMM(0.30)
    ds.m_ViasMinAnnularWidth = pcbnew.FromMM(0.13)
    ds.m_HoleToHoleMin = pcbnew.FromMM(0.45)
    # The DRC checks against the persisted net class (NET_SETTINGS in KiCad 10 -
    # the legacy GetAllNetClasses() NETCLASS does not persist), so it must match
    # what Freerouting routed to (DSN net class): 0.15 mm min track / clearance,
    # 0.60/0.30 mm vias. Impedance pairs were routed at 0.20 mm; 0.15 is the min
    # (fanout / SMD-pad necks).
    ns = ds.m_NetSettings
    default = ns.GetDefaultNetclass()
    default.SetTrackWidth(pcbnew.FromMM(0.15))
    default.SetClearance(pcbnew.FromMM(0.15))
    default.SetViaDiameter(pcbnew.FromMM(0.60))
    default.SetViaDrill(pcbnew.FromMM(0.30))
    ns.RecomputeEffectiveNetclasses()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", type=Path, default=DEFAULT_BOARD,
                         help="board .kicad_pcb path (default: canonical aius-carrier.kicad_pcb)")
    args = parser.parse_args()
    board_path = str(args.board)
    ses_path = str(args.board.with_suffix(".ses"))

    board = pcbnew.LoadBoard(board_path)
    set_fab_rules(board)
    before = len(list(board.GetTracks()))
    ok = pcbnew.ImportSpecctraSES(board, ses_path)
    after = len(list(board.GetTracks()))
    print(f"ImportSpecctraSES: {ok}; tracks {before} -> {after}", flush=True)

    try:
        filler = pcbnew.ZONE_FILLER(board)
        filler.Fill(board.Zones())
        print(f"filled {len(board.Zones())} zones", flush=True)
    except Exception as exc:  # noqa: BLE001 - fill is best-effort; KiCad refills on open
        print(f"zone fill skipped ({exc}); KiCad fills on open", flush=True)

    pcbnew.SaveBoard(board_path, board)
    print(f"saved {board_path}", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
