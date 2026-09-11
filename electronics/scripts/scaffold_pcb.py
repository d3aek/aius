#!/usr/bin/env python3
"""H3b: scaffold the Aius carrier .kicad_pcb from the schematic netlist.

Runs inside the pinned KiCad 10.0.4 container (needs the `pcbnew` Python module).
Creates the board that H3c routes: four layers (F.Cu signal / L2 GND / L3
GND / B.Cu signal) on the JLCPCB JLC04161H-7628 stackup declared in the saved
`(stackup ...)` block, a slot-fit outline, every component imported from its
footprint library and placed by functional zone (connectors on the rear edge,
Teensy central, expansion on the front edge), all pad nets assigned from the
netlist and classified into the Power/USB90/Eth100/Default net classes emitted
in the companion `.kicad_pro`, and a solid L2 ground plane (the H2b return
plane) plus outer GND pours.

This is an INITIAL placement + connectivity scaffold, not a routed board: no
signal tracks are laid, and positions are refined during H3c routing. It is fully
regenerable from the netlist.

Run from Aius root after exporting a KiCad XML netlist:
  scripts/kicad.sh --python electronics/scripts/scaffold_pcb.py \
    --netlist <kicadxml> --output <candidate.kicad_pcb>
Both paths are mandatory: exploration never overwrites the routed board implicitly.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pcbnew

ELEC = Path(__file__).resolve().parent.parent  # electronics/
AIUS_PRETTY = str((ELEC / "lib" / "aius.pretty").resolve())
STOCK_FP = "/usr/share/kicad/footprints"

# Board outline (mm). Width fits the Hammond 1455N1201BK 100 mm extrusion slots
# (final trim to the Hammond slot spec is a pre-order mechanical-review item);
# depth stays inside the <=100 mm cap and the 120 mm barrel.
BOARD_W = 99.0
BOARD_H = 95.0

# Explicit placement for the load-bearing parts (mm, footprint anchor position,
# rotation deg). Rear edge = y small; front edge = y large. Missing placement
# is an error. Refined during H3c.
PLACEMENT = {
    "U20": (12.0, 8.5, 0.0),      # optical housing face on rear datum, nose through panel
    "U24": (28.0, 8.5, 0.0),
    "J1": (45.0, 3.675, 180.0),   # USB-C port faces rear -Y
    "J20": (81.555, 19.685, 0.0), # RJ45 axis x86, housing face y0
    "J2": (43.0, 42.0, 0.0),      # USB-A faces front; cable returns to Teensy USB
    "U1": (45.0, 10.0, 0.0),      # device USB shunt ESD near entry
    "U2": (53.0, 18.0, 0.0),      # upstream power limiter
    "U21": (18.0, 20.0, 0.0),
    "U23": (28.0, 17.0, 0.0),
    "U10": (59.0, 59.89, 90.0),   # SD toward rear; .62mm pad copper-to-edge
    "U11": (10.0, 66.0, 0.0),
    "J18": (84.0, 29.0, 0.0),     # Ethernet ribbon terminates beside magnetics
    "J19": (84.0, 44.0, 0.0),     # top-service cable, clear of SD/USB
    "R1": (49.5, 9.0, 0.0), "R2": (40.5, 9.0, 0.0),
    "R3": (49.5, 20.0, 0.0), "R4": (55.5, 20.0, 0.0),
    "R5": (53.0, 22.5, 0.0),
    "C1": (49.5, 16.75, 0.0), "C2": (49.0, 13.5, 90.0),
    "C3": (55.5, 14.5, 90.0), "C4": (39.5, 32.0, 0.0),
    "C20": (14.5, 14.0, 0.0), "C21": (9.5, 14.0, 0.0),
    "C22": (21.5, 20.0, 0.0), "C24": (32.0, 16.0, 0.0),
    "C25": (27.5, 14.0, 0.0), "R40": (32.0, 19.0, 0.0),
    "R17": (55.0, 25.0, 0.0), "R18": (55.0, 28.0, 0.0),
    "C13": (55.0, 45.0, 0.0), "C14": (55.0, 48.0, 90.0),
    "C10": (86.0, 70.0, 0.0), "C11": (90.0, 70.0, 0.0),
    "C30": (88.0, 23.0, 0.0),
    "R10": (7.0, 62.0, 90.0), "R11": (10.0, 62.0, 90.0),
    "R12": (14.0, 62.0, 90.0), "R13": (17.0, 62.0, 90.0),
    "R20": (17.0, 81.0, 90.0), "R21": (20.0, 81.0, 90.0),
    "R22": (23.0, 81.0, 90.0), "R23": (26.0, 81.0, 90.0),
    "R24": (80.0, 80.0, 0.0), "R25": (85.0, 80.0, 0.0),
    "R26": (17.0, 72.0, 90.0), "R27": (21.0, 72.0, 90.0),
    "R28": (25.0, 72.0, 90.0), "R29": (29.0, 72.0, 90.0),
    "R30": (86.0, 76.0, 0.0), "R31": (90.0, 76.0, 0.0),
}
# Expansion connectors along the front edge.
FRONT_ROW = ["J10", "J11", "J12", "J13", "J14", "J15", "J16", "J17"]
FRONT_Y = 88.0

# Default net class (everything not named in NETCLASS_SPECS below).
DEFAULT_TRACK_MM = 0.2
DEFAULT_CLEAR_MM = 0.15

# H2a stackup (docs/plans/aius-h2-h3-manufacturing.md Phase H2a;
# electronics/feasibility/stackups.json "jlcpcb-4L-1.6" / JLC04161H-7628): L1 0.035
# / prepreg 7628 0.2104 / L2 0.0152 / core 1.065 / L3 0.0152 / prepreg 0.2104 / L4
# 0.035 mm. Outer copper is 1 oz (0.035 mm); inner is JLC's quoted 0.0152 mm.
# (layer name, type, thickness mm, material, epsilon_r, loss_tangent); material/
# epsilon_r/loss_tangent are None for copper layers.
STACKUP_LAYERS = [
    ("F.Cu", "copper", 0.035, None, None, None),
    ("dielectric 1", "prepreg", 0.2104, "FR4", 4.6, 0.02),
    ("In1.Cu", "copper", 0.0152, None, None, None),
    ("dielectric 2", "core", 1.065, "FR4", 4.6, 0.02),
    ("In2.Cu", "copper", 0.0152, None, None, None),
    ("dielectric 3", "prepreg", 0.2104, "FR4", 4.6, 0.02),
    ("B.Cu", "copper", 0.035, None, None, None),
]
# Sum of the recorded per-layer thicknesses above (not the fab's rounded nominal
# 1.6 mm) so the stackup block and the board's overall thickness stay consistent.
BOARD_THICKNESS_MM = round(sum(layer[2] for layer in STACKUP_LAYERS), 4)

# M2 / H2a net classes (docs/plans/aius-h2-h3-manufacturing.md M2 + "Recovery audit
# (2026-09-09)" corrected geometry). name -> (track_width mm, clearance mm,
# diff_pair_width mm, diff_pair_gap mm); via_diameter/via_drill are shared across
# every class (0.6/0.3 mm, unchanged from the prior single Default class).
NETCLASS_SPECS = {
    "Default": (DEFAULT_TRACK_MM, DEFAULT_CLEAR_MM, 0.2, 0.15),
    # M2: undersized power traces (0.2 mm for ~1 A VBUS) widened to >=0.5 mm.
    "Power": (0.5, DEFAULT_CLEAR_MM, 0.2, 0.15),
    # Recovery audit corrected geometry: 90 ohm differential needs 0.251 mm width
    # at 0.15 mm edge spacing on the JLCPCB 4-layer stackup above.
    "USB90": (0.251, 0.15, 0.251, 0.15),
    # 100 ohm differential needs 0.201 mm width at 0.15 mm edge spacing.
    "Eth100": (0.201, 0.15, 0.201, 0.15),
}
NETCLASS_VIA_DIAMETER_MM = 0.6
NETCLASS_VIA_DRILL_MM = 0.3

# Net assignment is keyed by the *bare* net name (trailing path component after
# the last "/"): KiCad's hierarchical netlist exporter prefixes sheet-local nets
# with "/<sheet>/" (e.g. "/power-usb/VBUS_RAW") while global-labelled nets stay
# bare (e.g. "VBUS_5V"); matching on the bare name survives either form.
NET_CLASS_BY_BARE_NAME = {
    "VBUS_RAW": "Power", "VBUS_5V": "Power", "EXP_USBH_VBUS": "Power",
    "USB_DP": "USB90", "USB_DM": "USB90", "USBH_DP": "USB90", "USBH_DM": "USB90",
    "ETH_TXP": "Eth100", "ETH_TXN": "Eth100", "ETH_RXP": "Eth100", "ETH_RXN": "Eth100",
}


def _stackup_block() -> str:
    """The (stackup ...) S-expression body, tab-indented to nest as the first
    child of a (setup ...) block at one tab of indentation (see _insert_stackup).

    pcbnew's Python bindings do not expose BOARD_STACKUP in KiCad 10.0.4:
    board.GetDesignSettings().GetStackupDescriptor() returns a bare, unusable
    SwigPyObject (no GetList/BuildDefaultStackupList), and "BOARD_STACKUP" is
    absent from the pcbnew module's own symbol table -- confirmed by probing the
    live container's pcbnew before writing this. This mirrors the exact
    key/nesting KiCad's own writer emits (cross-checked against a real
    KiCad-generated 4-copper-layer board, e.g. /usr/share/kicad/demos/video in
    the pinned image), including the cosmetic non-copper mask/paste/silk entries
    KiCad always emits alongside a stackup so the block parses the same way a
    GUI-authored one would.
    """
    lines = [
        "\t\t(stackup",
        '\t\t\t(layer "F.SilkS"',
        '\t\t\t\t(type "Top Silk Screen")',
        '\t\t\t\t(color "White")',
        "\t\t\t)",
        '\t\t\t(layer "F.Paste"',
        '\t\t\t\t(type "Top Solder Paste")',
        "\t\t\t)",
        '\t\t\t(layer "F.Mask"',
        '\t\t\t\t(type "Top Solder Mask")',
        '\t\t\t\t(color "Green")',
        "\t\t\t\t(thickness 0.01)",
        "\t\t\t)",
    ]
    for name, kind, thickness_mm, material, epsilon_r, loss_tangent in STACKUP_LAYERS:
        lines.append(f'\t\t\t(layer "{name}"')
        lines.append(f'\t\t\t\t(type "{kind}")')
        lines.append(f"\t\t\t\t(thickness {thickness_mm})")
        if material is not None:
            lines.append(f'\t\t\t\t(material "{material}")')
            lines.append(f"\t\t\t\t(epsilon_r {epsilon_r})")
            lines.append(f"\t\t\t\t(loss_tangent {loss_tangent})")
        lines.append("\t\t\t)")
    lines += [
        '\t\t\t(layer "B.Mask"',
        '\t\t\t\t(type "Bottom Solder Mask")',
        '\t\t\t\t(color "Green")',
        "\t\t\t\t(thickness 0.01)",
        "\t\t\t)",
        '\t\t\t(layer "B.Paste"',
        '\t\t\t\t(type "Bottom Solder Paste")',
        "\t\t\t)",
        '\t\t\t(layer "B.SilkS"',
        '\t\t\t\t(type "Bottom Silk Screen")',
        '\t\t\t\t(color "White")',
        "\t\t\t)",
        # No surface finish has been selected/quoted yet (electronics/feasibility
        # records dielectric/copper data only); "None" is KiCad's own
        # unspecified-finish enum value, not a fabricated finish.
        '\t\t\t(copper_finish "None")',
        "\t\t\t(dielectric_constraints no)",
        "\t\t)",
    ]
    return "\n".join(lines) + "\n"


def _insert_stackup(path: Path) -> None:
    """Insert the (stackup ...) block as the first child of (setup ...) in the
    saved .kicad_pcb. Post-save textual insertion: see _stackup_block for why
    the pcbnew Python API cannot write BOARD_STACKUP directly in this container.
    """
    text = path.read_text()
    marker = "\t(setup\n"
    index = text.index(marker) + len(marker)
    path.write_text(text[:index] + _stackup_block() + text[index:])


def _bare_net_name(name: str) -> str:
    return name.rsplit("/", 1)[-1]


def _write_net_classes(project: dict, net_by_name: dict) -> None:
    """M2 / H2a: replace the single Default class with the full class set and
    assign the Power/USB90/Eth100 member nets by pattern. This is the mechanism
    kicad-cli pcb drc actually honours for netclass clearance/width -- pcbnew's
    NET_SETTINGS has no Python Serialize() path and pcbnew.LoadBoard() does not
    reload a sidecar .kicad_pro's net_settings, so the board-level NETCLASS/
    NET_SETTINGS API objects cannot be persisted through SaveBoard(); confirmed
    empirically (a hand-written .kicad_pro net_settings.classes/netclass_patterns
    pair does drive a real kicad-cli pcb drc clearance violation) before
    settling on this JSON-only approach.
    """
    classes = []
    for name, (track_mm, clearance_mm, diff_width_mm, diff_gap_mm) in NETCLASS_SPECS.items():
        classes.append({
            "name": name,
            "clearance": clearance_mm,
            "track_width": track_mm,
            "via_diameter": NETCLASS_VIA_DIAMETER_MM,
            "via_drill": NETCLASS_VIA_DRILL_MM,
            "diff_pair_gap": diff_gap_mm,
            "diff_pair_width": diff_width_mm,
        })
    project["net_settings"]["classes"] = classes

    patterns = []
    for net_name in net_by_name:
        net_class = NET_CLASS_BY_BARE_NAME.get(_bare_net_name(net_name))
        if net_class is not None:
            patterns.append({"netclass": net_class, "pattern": net_name})
    project["net_settings"]["netclass_patterns"] = patterns
    print(f"assigned {len(patterns)} nets to non-default net classes", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--netlist", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tree = ET.parse(args.netlist).getroot()

    comps = []
    for comp in tree.findall("components/comp"):
        ref = comp.get("ref")
        value = comp.findtext("value", "")
        fp = comp.findtext("footprint", "")
        if not fp:
            if ref.startswith("#"):
                continue
            raise ValueError(f"{ref}: missing footprint")
        dnp = comp.find("property[@name='dnp']") is not None
        comps.append((ref, value, fp, dnp))

    nets = []
    for net in tree.findall("nets/net"):
        name = net.get("name")
        nodes = [(nd.get("ref"), nd.get("pin")) for nd in net.findall("node")]
        nets.append((name, nodes))

    print(f"parsed {len(comps)} components, {len(nets)} nets", flush=True)

    board = pcbnew.CreateEmptyBoard()
    board.SetCopperLayerCount(4)
    for layer in (pcbnew.In1_Cu, pcbnew.In2_Cu):
        board.SetLayerType(layer, pcbnew.LT_POWER)
    # B2: declare the overall board thickness from the H2a stackup (see
    # BOARD_THICKNESS_MM / STACKUP_LAYERS above); the (stackup ...) block itself
    # is inserted textually into the saved file by _insert_stackup.
    board.GetDesignSettings().SetBoardThickness(pcbnew.FromMM(BOARD_THICKNESS_MM))

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
    for ref, value, fpstr, dnp in comps:
        lib, _, fpname = fpstr.partition(":")
        libdir = AIUS_PRETTY if lib == "aius" else f"{STOCK_FP}/{lib}.pretty"
        fp = pcbnew.FootprintLoad(libdir, fpname)
        if fp is None:
            missing.append((ref, fpstr))
            continue
        fp.SetReference(ref)
        fp.SetValue(value)
        fp.SetFPID(pcbnew.LIB_ID(lib, fpname))
        fp.SetDNP(dnp)
        board.Add(fp)
        fp_by_ref[ref] = fp
    if missing:
        for ref, fpstr in missing:
            print(f"  MISSING FOOTPRINT {ref}: {fpstr}", file=sys.stderr)
        raise SystemExit(f"{len(missing)} footprints failed to load")
    print(f"loaded {len(fp_by_ref)} footprints", flush=True)

    # Assign pad nets.
    for ref, fp in fp_by_ref.items():
        numbers = {pad.GetNumber() for pad in fp.Pads()}
        missing_pins = {pin for comp, pin in pad_net if comp == ref} - numbers
        if missing_pins:
            raise ValueError(f"{ref}: netlist pins absent from footprint: {sorted(missing_pins)}")
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
    # New components require intentional placement, never a silent grid fallback.
    unplaced = set(fp_by_ref) - placed
    if unplaced:
        raise ValueError(f"missing placement: {sorted(unplaced)}")
    for ref, position in {"J1": (51.0, 5.0), "U1": (47.5, 11.0),
                          "J20": (92.0, 24.0), "U11": (6.0, 68.5),
                          "U2": (51.5, 15.0), "U23": (23.5, 17.0),
                          "R4": (57.0, 22.0), "R1": (51.5, 9.0),
                          "R2": (38.5, 9.0)}.items():
        field = fp_by_ref[ref].Reference()
        field.SetPosition(pcbnew.VECTOR2I(*(pcbnew.FromMM(v) for v in position)))
        field.SetTextAngle(pcbnew.EDA_ANGLE(0, pcbnew.DEGREES_T))

    # L2 solid GND plane (H2b return plane) + outer GND pours.
    gnd = net_by_name.get("GND")
    if gnd is not None:
        inset = 0.3
        zc = [(inset, inset), (BOARD_W - inset, inset),
              (BOARD_W - inset, BOARD_H - inset), (inset, BOARD_H - inset)]
        # Inner-layer GND planes only: L2 is the solid H2b return plane and L3
        # the second GND reference. Outer-layer (F/B) copper pours are added during
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
    args.output.parent.mkdir(parents=True, exist_ok=True)
    pcbnew.SaveBoard(str(args.output), board)
    _insert_stackup(args.output)
    # A scratch board needs the same explicit library/project context as the
    # canonical design. Use relative paths so it also opens outside the container.
    project = json.loads((ELEC / "aius-carrier.kicad_pro").read_text())
    _write_net_classes(project, net_by_name)
    args.output.with_suffix(".kicad_pro").write_text(json.dumps(project, indent=2) + "\n")
    args.output.with_suffix(".kicad_dru").write_text((ELEC / "aius-carrier.kicad_dru").read_text())
    libraries = (ELEC / "fp-lib-table").read_text()
    relative = Path(os.path.relpath(ELEC, args.output.parent)).as_posix()
    libraries = libraries.replace("${KIPRJMOD}/lib", "${KIPRJMOD}/" + relative + "/lib")
    (args.output.parent / "fp-lib-table").write_text(libraries)
    print(f"wrote {args.output}")
    print(f"  placed {len(fp_by_ref)} footprints, {len(net_by_name)} nets, "
          f"{board.GetCopperLayerCount()} copper layers, {len(board.Zones())} zones, "
          f"{BOARD_THICKNESS_MM} mm stackup, {len(NETCLASS_SPECS)} net classes")


if __name__ == "__main__":
    main()
