#!/usr/bin/env python3
"""Generate the Teensy 4.1 socket footprint from a reviewable pad table.

Two SSW-124-02-G-S main sockets mate with HTSW-124-07-G-S headers.
The optional underside USB-host row uses SSW-105-02-G-S / HTSW-105-07-G-S.
All share an 11.05 mm nominal hard-stop stack. Ethernet and service headers
mount on TOP of the Teensy and cable to separate carrier J18/J19 footprints;
there are deliberately no carrier holes beneath those headers or VUSB.
Pad names map to the partial Teensy41 symbol; unused main-row holes are unnetted.

Component-side coordinates follow PJRC's dimensions_teensy41.png, with VIN
at (0,0), USB to the left and microSD to the right. The top row is y=0;
the GND/pin-0 row is y=15.24. Pin names come from PJRC card11a/card11b.
See docs/reference/carrier-footprint-review.md for the coordinate derivation.

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

# Through-hole auxiliary clusters projected onto the component-side view.
USB_HOST = [  # Names from the component-side USB-host photograph on card11b.
    ("UH5V", 6.35, 12.19), ("UHDM", 8.89, 12.19), ("UHDP", 11.43, 12.19),
    ("UHGND", 13.97, 12.19), ("GND", 16.51, 12.19),
]
ETHERNET = [  # center x=13.97, middle row y=4.45; 2.00 mm pitch.
    ("ERXP", 12.97, 2.45), ("ERXN", 14.97, 2.45),
    ("ELED", 12.97, 4.45), ("EGND", 14.97, 4.45),
    ("ETXN", 12.97, 6.45), ("ETXP", 14.97, 6.45),
]
SERVICE = [
    ("ONOFF", 45.72, 2.54), ("PGM", 45.72, 5.08), ("GND", 45.72, 7.62),
    ("3V3", 45.72, 10.16), ("VBAT", 45.72, 12.70),
]
VUSB = [("VUSB", 2.54, 2.54)]

FP_NAME = "Teensy41_Sockets_SSW-124_Host_SSW-105"
PAD = 1.7


def pad(name: str, x: float, y: float, shape: str, index: int, drill: float = 1.04) -> str:
    uuid = f"c7ee0001-0000-4000-8000-{index:012d}"
    named = f'"{name}"' if name else '""'
    return (f'\t(pad {named} thru_hole {shape}\n'
            f'\t\t(at {x:.2f} {y:.2f})\n'
            f'\t\t(size {PAD} {PAD})\n'
            f'\t\t(drill {drill})\n'
            f'\t\t(layers "*.Cu" "*.Mask")\n'
            f'\t\t(uuid "{uuid}")\n'
            f'\t)\n')


def main() -> int:
    pads = []
    index = 100
    # Main rows.
    for col, name in enumerate(BOTTOM_ROW):
        shape = "rect" if (col == 1) else "circle"  # pin 0 corner marker
        pads.append(pad(name, col * PITCH, ROW_GAP, shape, index)); index += 1
    for col, name in enumerate(TOP_ROW):
        pads.append(pad(name, col * PITCH, 0.0, "circle", index)); index += 1
    # Only USB host remains an underside contact. Other tables document the
    # module's top-side cable locations, not carrier drill coordinates.
    for group, drill in ((USB_HOST, 1.04),):
        for name, x, y in group:
            pads.append(pad(name, x, y, "circle", index, drill)); index += 1

    x_min, x_max = -1.27, 59.69
    y_min, y_max = -1.27, 16.51
    body = (
        f'\t(fp_rect\n\t\t(start {x_min:.2f} {y_min:.2f})\n\t\t(end {x_max:.2f} {y_max:.2f})\n'
        f'\t\t(stroke (width 0.12) (type solid))\n\t\t(fill no)\n\t\t(layer "F.Fab")\n'
        f'\t\t(uuid "c7ee0001-0000-4000-8000-000000000901")\n\t)\n'
        f'\t(fp_rect\n\t\t(start -3.10 {y_min - 0.5:.2f})\n\t\t(end {x_max + 0.5:.2f} {y_max + 0.5:.2f})\n'
        f'\t\t(stroke (width 0.05) (type solid))\n\t\t(fill no)\n\t\t(layer "F.CrtYd")\n'
        f'\t\t(uuid "c7ee0001-0000-4000-8000-000000000902")\n\t)\n'
    )

    header = (
        f'(footprint "{FP_NAME}"\n'
        f'\t(version 20260206)\n'
        f'\t(generator "aius-gen-teensy-footprint")\n'
        f'\t(generator_version "10.0")\n'
        f'\t(layer "F.Cu")\n'
        f'\t(descr "Teensy 4.1 main and optional USB-host carrier sockets; VIN origin, USB left. Matched HTSW-xxx-07 / SSW-xxx-02, 11.05mm nominal stack. Ethernet/service cable to J18/J19 from module top. PJRC dimensions/card11a/card11b.")\n'
        f'\t(tags "teensy 4.1 socket module ssw-124 ssw-105 htsw")\n'
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
