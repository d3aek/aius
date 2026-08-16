#!/usr/bin/env python3
"""H3c cleanup: post-route fixes on the routed board (no reroute).

Operates in place on the routed aius-carrier.kicad_pcb:
  1. Widen any track Freerouting necked below 0.15 mm up to 0.20 mm (fixes the
     track_width punch-list items at SMD pad entries).
  2. Set the GND copper zones to a solid pad connection so GND through-hole pads
     tie straight into the L2/L3 planes (fixes the starved-thermal + GND
     unconnected items - a few GND pads were only thermally, weakly, or not
     connected to the plane).
  3. Refill the zones and save.

Run: python3 electronics/scripts/route_cleanup.py
"""

from __future__ import annotations

from pathlib import Path

import pcbnew

ELEC = Path(__file__).resolve().parent.parent
BOARD = str(ELEC / "aius-carrier.kicad_pcb")

MIN_KEEP_MM = 0.15
WIDEN_TO_MM = 0.20


def main() -> int:
    board = pcbnew.LoadBoard(BOARD)

    widened = 0
    for track in board.GetTracks():
        if isinstance(track, pcbnew.PCB_VIA):
            continue
        if track.GetWidth() < pcbnew.FromMM(MIN_KEEP_MM):
            track.SetWidth(pcbnew.FromMM(WIDEN_TO_MM))
            widened += 1
    print(f"widened {widened} sub-{MIN_KEEP_MM}mm tracks to {WIDEN_TO_MM}mm", flush=True)

    gnd_zones = 0
    for zone in board.Zones():
        if zone.GetNet() and zone.GetNet().GetNetname() == "GND":
            zone.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)
            gnd_zones += 1
    print(f"set {gnd_zones} GND zones to solid pad connection", flush=True)

    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.BuildConnectivity()
    pcbnew.SaveBoard(BOARD, board)
    print(f"saved {BOARD}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
