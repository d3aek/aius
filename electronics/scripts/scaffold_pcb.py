#!/usr/bin/env python3
"""H3b: scaffold the Aius carrier .kicad_pcb from the schematic netlist.

Runs inside the pinned KiCad 10.0.4 container (needs the `pcbnew` Python module).
Creates the board that H3c routes: a 4-layer stackup (L1 signal / L2 GND / L3
GND+power / L4 signal), the H2c-dimensioned edge outline, every component imported
from its footprint library and placed by functional zone (connectors on the rear
edge, Teensy central, expansion on the front edge), all pad nets assigned from the
netlist, and a solid L2 ground plane (the H2b return plane) plus outer GND pours.

This is an INITIAL placement + connectivity scaffold, not a routed board: no
signal tracks are laid, and positions are refined during H3c routing. It is fully
regenerable from the netlist.

Run (from repo root, in the container):
  python3 electronics/scripts/scaffold_pcb.py
Inputs:  electronics/aius-carrier.net (kicad-cli sch export netlist)
Output:  electronics/aius-carrier.kicad_pcb
"""

from __future__ import annotations

import sys
from pathlib import Path

import pcbnew

ELEC = Path(__file__).resolve().parent.parent  # electronics/
NETLIST = ELEC / "aius-carrier.net"
OUT = ELEC / "aius-carrier.kicad_pcb"
AIUS_PRETTY = str((ELEC / "lib" / "aius.pretty").resolve())
STOCK_FP = "/usr/share/kicad/footprints"

# Board outline (mm). Width fits the Hammond 1455N1201BK 100 mm extrusion slots
# (final trim to the Hammond slot spec is a pre-order mechanical-review item);
# depth stays inside the <=100 mm cap and the 120 mm barrel.
BOARD_W = 96.0
BOARD_H = 95.0

# Explicit placement for the load-bearing parts (mm, footprint anchor position,
# rotation deg). Rear edge = y small; front edge = y large. Everything not listed
# is grid-placed in a passive band. Refined during H3c.
PLACEMENT = {
    "U20": (14.0, 12.0, 180.0),   # optical RX  - rear edge, opening faces -Y
    "U24": (32.0, 12.0, 180.0),   # optical TX  - rear edge
    "J1": (50.0, 8.0, 0.0),       # USB-C       - rear edge, sole power/data entry
    "J20": (76.0, 13.0, 0.0),     # RJ45 magjack - rear edge
    "J2": (50.0, 30.0, 0.0),      # internal USB-A (mid-board, faces USB-C)
    "U1": (42.0, 22.0, 0.0),      # USB ESD
    "U2": (58.0, 22.0, 0.0),      # TPS2553 load switch
    "U21": (20.0, 26.0, 0.0),     # S/PDIF bypass mux
    "U23": (34.0, 26.0, 0.0),     # S/PDIF TX gate
    "U10": (19.0, 40.0, 0.0),     # Teensy 4.1 socket (centres ~x19..77, y40..55)
    "U11": (10.0, 58.0, 0.0),     # TCA9517A I2C isolator
    "U12": (24.0, 58.0, 0.0),     # expansion USB-host load switch
}
# Expansion connectors along the front edge.
FRONT_ROW = ["J10", "J11", "J12", "J13", "J14", "J15", "J16", "J17"]
FRONT_Y = 88.0

# Net-name -> (track width mm, clearance mm) intent from H2a, applied as the board
# default here; per-class assignment is set during H3c.
DEFAULT_TRACK_MM = 0.2
DEFAULT_CLEAR_MM = 0.15


def read_sexpr(text):
    """Minimal S-expression parser -> nested lists of str."""
    tokens, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in "()":
            tokens.append(c); i += 1
        elif c.isspace():
            i += 1
        elif c == '"':
            j = i + 1; buf = []
            while j < n and text[j] != '"':
                if text[j] == "\\" and j + 1 < n:
                    buf.append(text[j + 1]); j += 2
                else:
                    buf.append(text[j]); j += 1
            tokens.append('"' + "".join(buf)); i = j + 1
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in "()":
                j += 1
            tokens.append(text[i:j]); i = j

    def parse(pos):
        node = []
        while pos < len(tokens):
            t = tokens[pos]
            if t == "(":
                child, pos = parse(pos + 1)
                node.append(child)
            elif t == ")":
                return node, pos + 1
            else:
                node.append(t[1:] if t.startswith('"') else t)
                pos += 1
        return node, pos

    root, _ = parse(0)
    return root[0]


def find(node, tag):
    return [c for c in node if isinstance(c, list) and c and c[0] == tag]


def first(node, tag):
    got = find(node, tag)
    return got[0] if got else None


def val(node, tag):
    got = first(node, tag)
    return got[1] if (got and len(got) > 1) else None


def main():
    tree = read_sexpr(NETLIST.read_text())

    comps = []
    for comp in find(first(tree, "components"), "comp"):
        ref = val(comp, "ref")
        value = val(comp, "value") or ""
        fp = val(comp, "footprint")
        comps.append((ref, value, fp))

    nets = []
    for net in find(first(tree, "nets"), "net"):
        name = val(net, "name")
        nodes = [(val(nd, "ref"), val(nd, "pin")) for nd in find(net, "node")]
        nets.append((name, nodes))

    print(f"parsed {len(comps)} components, {len(nets)} nets", flush=True)

    board = pcbnew.CreateEmptyBoard()
    board.SetCopperLayerCount(4)

    ds = board.GetDesignSettings()
    try:
        ds.SetCopperEdgeClearance(pcbnew.FromMM(0.3))
    except Exception:
        pass

    # Edge.Cuts outline.
    corners = [(0, 0), (BOARD_W, 0), (BOARD_W, BOARD_H), (0, BOARD_H)]
    for a, b in zip(corners, corners[1:] + corners[:1]):
        seg = pcbnew.PCB_SHAPE(board)
        seg.SetShape(pcbnew.SHAPE_T_SEGMENT)
        seg.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(a[0]), pcbnew.FromMM(a[1])))
        seg.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(b[0]), pcbnew.FromMM(b[1])))
        seg.SetLayer(pcbnew.Edge_Cuts)
        seg.SetWidth(pcbnew.FromMM(0.1))
        board.Add(seg)

    # Nets.
    net_by_name = {}
    for name, _ in nets:
        if name not in net_by_name:
            ni = pcbnew.NETINFO_ITEM(board, name)
            board.Add(ni)
            net_by_name[name] = ni
    pad_net = {}
    for name, nodes in nets:
        for ref, pin in nodes:
            pad_net[(ref, pin)] = name

    # Footprints.
    fp_by_ref = {}
    missing = []
    for ref, value, fpstr in comps:
        lib, _, fpname = fpstr.partition(":")
        libdir = AIUS_PRETTY if lib == "aius" else f"{STOCK_FP}/{lib}.pretty"
        fp = pcbnew.FootprintLoad(libdir, fpname)
        if fp is None:
            missing.append((ref, fpstr))
            continue
        fp.SetReference(ref)
        fp.SetValue(value)
        board.Add(fp)
        fp_by_ref[ref] = fp
    if missing:
        for ref, fpstr in missing:
            print(f"  MISSING FOOTPRINT {ref}: {fpstr}", file=sys.stderr)
        raise SystemExit(f"{len(missing)} footprints failed to load")
    print(f"loaded {len(fp_by_ref)} footprints", flush=True)

    # Assign pad nets.
    for ref, fp in fp_by_ref.items():
        for pad in fp.Pads():
            key = (ref, pad.GetNumber())
            name = pad_net.get(key)
            if name:
                pad.SetNet(net_by_name[name])

    # Placement.
    placed = set()
    for ref, (x, y, rot) in PLACEMENT.items():
        if ref in fp_by_ref:
            fp_by_ref[ref].SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
            fp_by_ref[ref].SetOrientationDegrees(rot)
            placed.add(ref)
    for i, ref in enumerate(FRONT_ROW):
        if ref in fp_by_ref:
            x = 8.0 + i * ((BOARD_W - 16.0) / (len(FRONT_ROW) - 1))
            fp_by_ref[ref].SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(FRONT_Y)))
            placed.add(ref)
    # Grid the rest (mostly 0402/0805 passives) in the free band y=64..82,
    # clear of the Teensy (y40..55) and the two ICs (y58).
    gx, gy = 8.0, 64.0
    for ref, fp in fp_by_ref.items():
        if ref in placed:
            continue
        fp.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(gx), pcbnew.FromMM(gy)))
        gx += 5.0
        if gx > BOARD_W - 8.0:
            gx = 8.0
            gy += 5.0

    # L2 solid GND plane (H2b return plane) + outer GND pours.
    gnd = net_by_name.get("GND")
    if gnd is not None:
        inset = 0.3
        zc = [(inset, inset), (BOARD_W - inset, inset),
              (BOARD_W - inset, BOARD_H - inset), (inset, BOARD_H - inset)]
        # Inner-layer GND planes only: L2 is the solid H2b return plane and L3
        # the GND+power layer. Outer-layer (F/B) copper pours are added during
        # H3c routing, around the tracks, not in the scaffold.
        for layer in (pcbnew.In1_Cu, pcbnew.In2_Cu):
            zone = pcbnew.ZONE(board)
            zone.SetLayer(layer)
            zone.SetNetCode(gnd.GetNetCode())
            zone.SetIsFilled(False)
            poly = zone.Outline()
            poly.NewOutline()
            for x, y in zc:
                poly.Append(pcbnew.FromMM(x), pcbnew.FromMM(y))
            board.Add(zone)
        # Zones are left unfilled here; KiCad fills them on open and H3c refills
        # after routing. Headless ZONE_FILLER on a freshly scripted board is
        # crash-prone, and an unfilled outline carries the same design intent.
        print(f"added {len(board.Zones())} GND zones (unfilled)", flush=True)

    board.BuildListOfNets()
    print("saving board...", flush=True)
    pcbnew.SaveBoard(str(OUT), board)
    print(f"wrote {OUT}")
    print(f"  placed {len(fp_by_ref)} footprints, {len(net_by_name)} nets, "
          f"{board.GetCopperLayerCount()} copper layers, {len(board.Zones())} zones")


if __name__ == "__main__":
    main()
