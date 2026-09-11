#!/usr/bin/env python3
"""Check manufactured pad interfaces against the reviewed source drawings.

scripts/kicad.sh --python electronics/scripts/check_footprints.py
scripts/kicad.sh --python electronics/scripts/check_footprints.py --board <pcb>

The board mode checks embedded footprints as well as the library mode. It
deliberately fails on the old routed board until that board is rebuilt.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import pcbnew

ELEC = Path(__file__).resolve().parent.parent


def mm(point):
    return tuple(round(pcbnew.ToMM(v), 3) for v in (point.x, point.y))


def bare_net_name(name):
    """Strip a KiCad hierarchical sheet prefix ("/ethernet/ETH_TXP" ->
    "ETH_TXP"), matching scaffold_pcb.py's own net-class assignment rule:
    sheet-local nets are exported prefixed, global-labelled nets stay bare."""
    return name.rsplit("/", 1)[-1]


def connectivity_pin_map(json_path, ref):
    """Pull the pad-number -> expected-net map for `ref` straight out of a
    connectivity spec's own "nets" / "no_connects" tables, so the expected
    mapping is never a second hand-authored copy. Returns (net_by_pin,
    no_connect_pins); a footprint pad number in neither is uncovered by the
    spec and must be reported, not assumed."""
    spec = json.loads(json_path.read_text())
    net_by_pin = {}
    for net_name, endpoints in spec.get("nets", {}).items():
        for endpoint_ref, pin in endpoints:
            if endpoint_ref == ref:
                net_by_pin[pin] = net_name
    no_connect_pins = {pin for endpoint_ref, pin in spec.get("no_connects", []) if endpoint_ref == ref}
    return net_by_pin, no_connect_pins


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", type=Path)
    args = parser.parse_args()
    names = {
        "J1": "USB_C_GCT_USB4105_NPTH_Clearance",
        "U10": "Teensy41_Sockets_SSW-124_Host_SSW-105",
        "J19": "Samtec_HTSW-105-07-G-S",
        "U20": "Cliff_ORJ5_TOSLINK",
        "U24": "Cliff_OTJ5_TOSLINK",
        "J20": "Pulse_J0011D21BNL_TabDown",
    }
    board = pcbnew.LoadBoard(str(args.board)) if args.board else None
    fps = {fp.GetReference(): fp for fp in board.GetFootprints()} if board else {
        ref: pcbnew.FootprintLoad(str(ELEC / "lib/aius.pretty"), name)
        for ref, name in names.items()
    }
    names["J18"] = "PinHeader_2x03_P2.00mm_Vertical"
    if not board:
        fps["J18"] = pcbnew.FootprintLoad(
            "/usr/share/kicad/footprints/Connector_PinHeader_2.00mm.pretty", names["J18"])
    failures = []
    checks = 0

    def expect(condition, message):
        nonlocal checks
        checks += 1
        if not condition:
            failures.append(message)

    def points(ref, number, expected):
        fp = fps[ref]
        # Footprints are normalized in memory; this checker never saves a board.
        actual = sorted(mm(p.GetPosition()) for p in fp.Pads() if p.GetNumber() == number)
        expect(actual == sorted(expected), f"{ref}.{number}: expected {expected}, got {actual}")

    for ref in names:
        expect(ref in fps and fps[ref] is not None, f"missing footprint {ref}")
        if fps.get(ref) is None:
            continue
        fps[ref].SetOrientationDegrees(0)
        fps[ref].SetPosition(pcbnew.VECTOR2I(0, 0))

    if not failures:
        points("J1", "", [(-2.89, -2.605), (2.89, -2.605)])
        for number, x in (("A1", -3.2), ("B12", -3.2), ("A12", 3.2), ("B1", 3.2)):
            points("J1", number, [(x, -3.69)])
            pad = next(p for p in fps["J1"].Pads() if p.GetNumber() == number)
            expect(mm(pad.GetSize()) == (0.6, 1.13), f"J1.{number}: adapted GND land size")
        # Four asymmetric corners detect reflected or transposed main rows.
        points("U10", "VIN", [(0, 0)])
        points("U10", "0", [(2.54, 15.24)])
        points("U10", "33", [(58.42, 0)])
        points("U10", "32", [(58.42, 15.24)])
        auxiliary = {
            "UH5V": (6.35, 12.19), "UHDM": (8.89, 12.19),
            "UHDP": (11.43, 12.19), "UHGND": (13.97, 12.19),
        }
        for pin, pos in auxiliary.items():
            points("U10", pin, [pos])
        points("U10", "GND", [(0, 15.24), (2.54, 0), (35.56, 0),
                                (16.51, 12.19)])
        points("U10", "3V3", [(5.08, 0), (35.56, 15.24)])
        for pin in ("ERXP", "ERXN", "ELED", "EGND", "ETXN", "ETXP",
                    "ONOFF", "PGM", "VBAT", "VUSB"):
            points("U10", pin, [])
        for number in range(1, 7):
            points("J18", str(number), [((number - 1) % 2 * 2, (number - 1) // 2 * 2)])
        for number in range(1, 6):
            points("J19", str(number), [(0, round((number - 1) * 2.54, 3))])
        for ref in ("U20", "U24"):
            points(ref, "1", [(-2.54, 0)])
            points(ref, "2", [(0, 0)])
            points(ref, "3", [(2.54, 0)])
            points(ref, "", [(-4.6, 2.1), (4.6, 2.1), (0, -3.9)])
        magjack = {
            "1": (8.89, -2.54), "2": (7.62, 0), "3": (6.35, -2.54),
            "4": (5.08, 0), "5": (3.81, -2.54), "6": (2.54, 0),
            "7": (1.27, -2.54), "8": (0, 0),
            "9": (10.77, -13.79), "10": (8.23, -12.27),
            "11": (0.66, -13.79), "12": (-1.88, -12.27),
        }
        for pin, pos in magjack.items():
            points("J20", pin, [pos])
        points("J20", "SH", [(-3.43, -5.84), (12.32, -5.84)])
        points("J20", "", [(-1.27, -8.89), (10.16, -8.89)])
        expect(len(list(fps["U10"].Pads())) == 53, "Teensy must have 53 main/host socket pads")
        for ref, expected in (("U10", 1.04), ("J18", 0.8), ("J19", 1.02)):
            for p in fps[ref].Pads():
                expect(round(pcbnew.ToMM(p.GetDrillSize().x), 3) == expected,
                       f"{ref}.{p.GetNumber()}: drill must be {expected} mm")
        if board:
            original = {fp.GetReference(): fp for fp in board.GetFootprints()}
            u1 = original["U1"]
            expect(u1.GetFPID().GetLibItemName() == "Texas_DRT-3", "U1 package is not TI DRT")

            # H3e / B4 closure: assert pad-number -> net-name identity for every
            # J20 and J1 pin the connectivity JSON defines a net (or an explicit
            # no_connect) for, sourced from the spec files themselves.
            #   - J20 (Pulse J0011D21BNL): pin roles are the manufacturer
            #     schematic, docs/reference/datasheets.mdAIUS-ETHERNET-MAGJACK-MFR/
            #     DATASHEET_J403.pdf page 5 (pins 1/2 TX+/TX-, 3/6 RX+/RX-,
            #     4/5 center taps, 8 GND return; page 6 for the LED pins).
            #   - J1 (GCT USB4105-GF-A): the stock KiCad
            #     USB_C_Receptacle_USB2.0_16P pin layout is cross-checked
            #     against the GCT mechanical drawing (docs/reference/datasheets.md
            #     GCT-USB4105/usb4105.pdf); no separate schematic page is filed
            #     for this connector (docs/plans/aius-h2-h3-manufacturing.md B4).
            # Net names on the board may carry a "/<sheet>/" prefix for
            # sheet-local nets; compare on the bare trailing name, same as
            # scripts/scaffold_pcb.py's own net-class matching.
            for ref, json_path in (
                ("J20", ELEC / "connectivity/ethernet.json"),
                ("J1", ELEC / "connectivity/power-usb.json"),
            ):
                net_by_pin, no_connect_pins = connectivity_pin_map(json_path, ref)
                # Unnumbered pads are mechanical mounting posts/pegs, not
                # signal pins the connectivity JSON ever assigns a net to.
                pads = {p.GetNumber(): p.GetNetname() for p in original[ref].Pads() if p.GetNumber() != ""}
                for pin, expected_net in sorted(net_by_pin.items()):
                    actual = bare_net_name(pads.get(pin, ""))
                    expect(actual == expected_net,
                           f"{ref}.{pin}: expected net {expected_net!r} per "
                           f"{json_path.name}, got {actual!r}")
                for pin in sorted(no_connect_pins):
                    actual = pads.get(pin, "")
                    expect(actual == "" or actual.startswith("unconnected-"),
                           f"{ref}.{pin}: expected no_connect per {json_path.name}, "
                           f"got net {actual!r}")
                uncovered = sorted(set(pads) - set(net_by_pin) - no_connect_pins)
                if uncovered:
                    print(f"UNCOVERED: {ref} pin(s) {', '.join(uncovered)} have no "
                          f"pin->net mapping in {json_path.name} - not asserted, "
                          "not invented")

    for failure in failures:
        print(f"FAIL: {failure}")
    print(f"Footprint checks: {checks - len(failures)}/{checks} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
