#!/usr/bin/env python3
"""Generate the Teensy 4.1 socket footprint from a reviewable pad table.

The Teensy 4.1 mounts on the carrier through socket strips: two 24-pin
SSW-124-02-G-S (main rows, 2.54 mm pitch, rows 15.24 mm apart), one 5-pin
SSW-105-02-G-S (USB host, underside), and one 2x3 MMS-103-01-L-DV (native
Ethernet header, 2.00 mm pitch, underside). This emits an aius: library
footprint whose pads carry the Teensy pin identifiers so the (partial) Teensy41
schematic symbol nets map by name; unused socket holes are present for the module
to seat but carry no net.

MAIN-ROW ORDER is transcribed from the PJRC pinout card
(https://www.pjrc.com/teensy/card11a_rev4_web.pdf) and is exact.
UNDERSIDE CLUSTER XY (USB host, Ethernet, service column, VUSB) is NOT
dimensioned on the PJRC cards — the positions below are best-estimates from
card11b relative placement and are flagged for pad-by-pad verification against a
physical Teensy 4.1 at the pre-order footprint gate. Cluster PAD NAMES and
pitches are authoritative; only their board XY is provisional.

Run: ./gen_teensy_footprint.py   (writes the .kicad_mod beside this script)
"""

from __future__ import annotations

from pathlib import Path

PITCH = 2.54
ROW_GAP = 15.24  # 0.6 in between the two main rows

# Main rows, column 0 nearest the USB end, from card11a (component-side).
BOTTOM_ROW = ["GND", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10",
              "11", "12", "3V3", "24", "25", "26", "27", "28", "29", "30", "31", "32"]
TOP_ROW = ["VIN", "GND", "3V3", "23", "22", "21", "20", "19", "18", "17", "16",
           "15", "14", "13", "GND", "41", "40", "39", "38", "37", "36", "35", "34", "33"]

# Underside clusters: (name, x, y). Names authoritative; XY provisional (see header).
# Placed inboard between the main rows (0 < y < 15.24), non-colliding.
USB_HOST = [  # SSW-105 5-pin single row near the USB end; card11b: +5V D+ D- GND GND
    ("UH5V", 7.62, 11.0), ("UHDP", 10.16, 11.0), ("UHDM", 12.70, 11.0),
    ("UHGND", 15.24, 11.0), ("GND", 17.78, 11.0),
]
ETHERNET = [  # MMS-103 2x3 @ 2.00 mm; card11b rows: RX+/LED/TX- and RX-/GND/TX+
    ("ERXP", 28.48, 8.62), ("ELED", 30.48, 8.62), ("ETXN", 32.48, 8.62),
    ("ERXN", 28.48, 6.62), ("EGND", 30.48, 6.62), ("ETXP", 32.48, 6.62),
]
SERVICE = [  # underside column near the SD end; card11b: On/Off Program GND 3.3V VBAT
    ("ONOFF", 53.34, 12.0), ("PGM", 53.34, 9.46), ("GND", 53.34, 6.92),
    ("3V3", 53.34, 4.38), ("VBAT", 53.34, 1.84),
]
VUSB = [("VUSB", 5.08, 4.0)]  # USB-device group pad, card11b

FP_NAME = "Teensy41_Sockets_SSW-124_MMS-103_SSW-105"
DRILL = 1.0
PAD = 1.7


def pad(name: str, x: float, y: float, shape: str, index: int) -> str:
    uuid = f"c7ee0001-0000-4000-8000-{index:012d}"
    named = f'"{name}"' if name else '""'
    return (f'\t(pad {named} thru_hole {shape}\n'
            f'\t\t(at {x:.2f} {y:.2f})\n'
            f'\t\t(size {PAD} {PAD})\n'
            f'\t\t(drill {DRILL})\n'
            f'\t\t(layers "*.Cu" "*.Mask")\n'
            f'\t\t(uuid "{uuid}")\n'
            f'\t)\n')


def main() -> int:
    pads = []
    index = 100
    # Main rows.
    for col, name in enumerate(BOTTOM_ROW):
        shape = "rect" if (col == 1) else "circle"  # pin 0 corner marker
        pads.append(pad(name, col * PITCH, 0.0, shape, index)); index += 1
    for col, name in enumerate(TOP_ROW):
        pads.append(pad(name, col * PITCH, ROW_GAP, "circle", index)); index += 1
    # Underside clusters.
    for group in (USB_HOST, ETHERNET, SERVICE, VUSB):
        for name, x, y in group:
            pads.append(pad(name, x, y, "circle", index)); index += 1

    x_min, x_max = -2.0, 23 * PITCH + 2.0
    y_min, y_max = -2.0, ROW_GAP + 2.0
    body = (
        f'\t(fp_rect\n\t\t(start {x_min:.2f} {y_min:.2f})\n\t\t(end {x_max:.2f} {y_max:.2f})\n'
        f'\t\t(stroke (width 0.12) (type solid))\n\t\t(fill no)\n\t\t(layer "F.Fab")\n'
        f'\t\t(uuid "c7ee0001-0000-4000-8000-000000000901")\n\t)\n'
        f'\t(fp_rect\n\t\t(start {x_min - 0.5:.2f} {y_min - 0.5:.2f})\n\t\t(end {x_max + 0.5:.2f} {y_max + 0.5:.2f})\n'
        f'\t\t(stroke (width 0.05) (type solid))\n\t\t(fill no)\n\t\t(layer "F.CrtYd")\n'
        f'\t\t(uuid "c7ee0001-0000-4000-8000-000000000902")\n\t)\n'
    )

    header = (
        f'(footprint "{FP_NAME}"\n'
        f'\t(version 20260206)\n'
        f'\t(generator "aius-gen-teensy-footprint")\n'
        f'\t(generator_version "10.0")\n'
        f'\t(layer "F.Cu")\n'
        f'\t(descr "Teensy 4.1 socketed carrier footprint: 2x SSW-124 (24-pin, 2.54mm, rows 15.24mm apart), SSW-105 (5-pin USB host), MMS-103 (2x3 native-Ethernet, 2.00mm). Main-row pin order exact from PJRC card11a; underside cluster (USB host / Ethernet / service / VUSB) XY estimated from card11b relative placement and to be pad-by-pad verified at the pre-order gate. Pads carry Teensy pin ids so the partial Teensy41 symbol maps by name; unused holes carry no net.")\n'
        f'\t(tags "teensy 4.1 socket module ssw-124 mms-103 ssw-105")\n'
        f'\t(property "Reference" "REF**"\n\t\t(at {23 * PITCH / 2:.2f} -3.5 0)\n\t\t(layer "F.SilkS")\n'
        f'\t\t(uuid "c7ee0001-0000-4000-8000-000000000001")\n'
        f'\t\t(effects (font (size 1 1) (thickness 0.15)))\n\t)\n'
        f'\t(property "Value" "{FP_NAME}"\n\t\t(at {23 * PITCH / 2:.2f} {ROW_GAP + 3.5:.2f} 0)\n\t\t(layer "F.Fab")\n'
        f'\t\t(uuid "c7ee0001-0000-4000-8000-000000000002")\n'
        f'\t\t(effects (font (size 1 1) (thickness 0.15)))\n\t)\n'
        f'\t(property "Datasheet" "https://www.pjrc.com/teensy/card11a_rev4_web.pdf"\n\t\t(at 0 0 0)\n\t\t(layer "F.Fab")\n\t\t(hide yes)\n'
        f'\t\t(uuid "c7ee0001-0000-4000-8000-000000000003")\n'
        f'\t\t(effects (font (size 1 1) (thickness 0.15)))\n\t)\n'
        f'\t(property "Description" "Teensy 4.1 socketed module carrier footprint"\n\t\t(at 0 0 0)\n\t\t(layer "F.Fab")\n\t\t(hide yes)\n'
        f'\t\t(uuid "c7ee0001-0000-4000-8000-000000000004")\n'
        f'\t\t(effects (font (size 1 1) (thickness 0.15)))\n\t)\n'
        f'\t(attr through_hole)\n'
    )

    content = header + "".join(pads) + body + '\t(embedded_fonts no)\n)\n'
    out = Path(__file__).with_name("aius.pretty") / f"{FP_NAME}.kicad_mod"
    out.write_text(content)
    print(f"wrote {out} ({len(pads)} pads)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
