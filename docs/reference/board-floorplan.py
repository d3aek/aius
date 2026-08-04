#!/usr/bin/env python3
"""Generate the Aius carrier placement floorplan (board-floorplan.svg).

A concept placement of the board inside the Hammond 1455N1201BK enclosure,
derived from docs/reference/interfaces-and-states.md + functional-topology.md and
the evidence/design/provisional-components.csv BOM. This is NOT a routed board,
NOT exact mechanical CAD, and NOT manufacturing data -- it shows the physical
arrangement so a reader can see what is being built. Placement is data-driven
below so it is easy to iterate.

Run: python3 board-floorplan.py  ->  writes board-floorplan.svg alongside.
"""
from pathlib import Path

# Board envelope (mm). Provisional: <=100x100mm cost tier, inside the ~103mm-wide
# 1455N1201BK. Rear (connector) edge is y=0 (top); front is y=BOARD_D.
BOARD_W, BOARD_D = 96.0, 100.0
ENC_W, ENC_D = 103.0, 120.0          # enclosure extrusion footprint
MM = 6.0                              # px per mm

PAL = {
    "optical": "#4C9BE8", "usb": "#E8804C", "eth": "#5BAF6A", "ic": "#B07AD1",
    "passive": "#C9A227", "teensy": "#2F9E44", "service": "#8A8F98",
    "header": "#D46A9F", "power": "#E05A5A",
}

# n, x, y, w, h, category, short label, description (for the legend), MPN/note.
PARTS = [
    (1, 6, 2, 15, 12, "optical", "RX", "TOSLINK optical receiver", "RS PRO ORJ-5 (805-1671)"),
    (2, 23, 2, 15, 12, "optical", "TX", "TOSLINK optical transmitter", "RS PRO OTJ-5 (805-1680)"),
    (3, 45, 1, 9, 8, "usb", "USB-C", "Rear USB-C sink (sole 5V + data)", "GCT USB4105-GF-A"),
    (4, 60, 2, 16, 12, "eth", "RJ45", "Ethernet magjack (Teensy native PHY)", "Pulse J0011D21BNL"),
    (5, 82, 2, 11, 6, "service", "PROG", "Remote Program + RTC header", "JST GH"),
    (6, 9, 18, 7, 5, "ic", "GATE", "S/PDIF TX gate (reset-safe disable)", "TI SN74LVC1G126DBVR"),
    (7, 27, 18, 7, 5, "ic", "MUX", "Diagnostic raw-bypass mux", "TI SN74LVC1G3157DBVR"),
    (8, 44, 12, 6, 4, "ic", "ESD", "USB high-speed ESD", "TI TPD2EUSB30ADRTR"),
    (9, 52, 12, 6, 4, "power", "PWR", "USB 5V load switch / protection", "TI TPS2553DBVR"),
    (10, 62, 18, 12, 5, "passive", "MAG", "Magjack center-tap + support passives", "100 nF 0402 + terms"),
    (11, 44, 28, 14, 8, "usb", "USB-A", "Internal USB-A (feeds Teensy)", "TE 292303-1"),
    (12, 17, 41, 61, 18, "teensy", "TEENSY 4.1", "Socketed Teensy 4.1 module", "Samtec SSW/MMS sockets"),
    (13, 6, 64, 7, 5, "ic", "ISO", "Expansion I2C isolator", "TI TCA9517A"),
    (14, 83, 66, 8, 8, "service", "TC", "Bring-up / test point", "Tag-Connect TC2030-IDC"),
    (15, 6, 88, 9, 5, "header", "I2C", "Expansion: I2C control", "JST GH"),
    (16, 17, 88, 9, 5, "header", "SPI", "Expansion: SPI (6-circuit)", "JST GH"),
    (17, 28, 88, 9, 5, "header", "UART", "Expansion: UART / debug", "JST GH"),
    (18, 39, 88, 9, 5, "header", "I2S", "Expansion: I2S / TDM audio", "JST GH"),
    (19, 50, 88, 9, 5, "header", "HOST", "Expansion: USB host", "JST GH"),
    (20, 61, 88, 9, 5, "header", "GPIO", "Expansion: GPIO / ADC (+ trigger out)", "JST GH"),
]

# Board origin in canvas px.
OX, OY = 60, 128


def bx(x):
    return OX + x * MM


def by(y):
    return OY + y * MM


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def build():
    parts = []
    W = OX + ENC_W * MM + 470
    H = OY + ENC_D * MM + 60
    parts.append(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W:.0f}" height="{H:.0f}" '
        f'viewBox="0 0 {W:.0f} {H:.0f}" font-family="Helvetica,Arial,sans-serif">'
    )
    parts.append(f'<rect width="{W:.0f}" height="{H:.0f}" fill="#fbfcfd"/>')
    parts.append(
        f'<text x="{OX}" y="42" font-size="22" font-weight="700" fill="#1a2430">'
        f'Aius carrier &#8212; placement floorplan</text>'
    )
    parts.append(
        f'<text x="{OX}" y="66" font-size="12" fill="#5a6675">Concept placement from the '
        f'interfaces/topology spec + BOM.</text>'
    )
    parts.append(
        f'<text x="{OX}" y="84" font-size="12" fill="#5a6675">Not routed, not exact '
        f'mechanical CAD, not manufacturing data.</text>'
    )

    # Enclosure outline (centered over the board envelope).
    encx = OX - (ENC_W - BOARD_W) / 2 * MM
    ency = OY - 6 * MM
    parts.append(
        f'<rect x="{encx:.0f}" y="{ency:.0f}" width="{ENC_W*MM:.0f}" height="{ENC_D*MM:.0f}" '
        f'rx="10" fill="none" stroke="#b9c2cd" stroke-width="2" stroke-dasharray="6 5"/>'
    )
    parts.append(
        f'<text x="{encx+6:.0f}" y="{ency+ENC_D*MM-8:.0f}" font-size="11" fill="#9aa6b2">'
        f'Hammond 1455N1201BK enclosure (~{ENC_W:.0f} x {ENC_D:.0f} mm, provisional)</text>'
    )

    # Board.
    parts.append(
        f'<rect x="{bx(0):.0f}" y="{by(0):.0f}" width="{BOARD_W*MM:.0f}" height="{BOARD_D*MM:.0f}" '
        f'rx="6" fill="#eef3ee" stroke="#3c7a3c" stroke-width="2"/>'
    )
    parts.append(
        f'<text x="{bx(BOARD_W)-6:.0f}" y="{by(BOARD_D)-8:.0f}" font-size="11" fill="#3c7a3c" '
        f'text-anchor="end">2-layer PCB, &#8804; {BOARD_W:.0f} x {BOARD_D:.0f} mm (hypothesis)</text>'
    )
    # Rear panel band.
    parts.append(
        f'<line x1="{bx(0):.0f}" y1="{by(0):.0f}" x2="{bx(BOARD_W):.0f}" y2="{by(0):.0f}" '
        f'stroke="#3c7a3c" stroke-width="4"/>'
    )
    parts.append(
        f'<text x="{bx(BOARD_W/2):.0f}" y="{by(0)-8:.0f}" font-size="11" fill="#3c7a3c" '
        f'text-anchor="middle" font-weight="600">REAR PANEL (machined by hand / CNC)</text>'
    )

    # Parts.
    for n, x, y, w, h, cat, short, _desc, _mpn in PARTS:
        col = PAL[cat]
        parts.append(
            f'<rect x="{bx(x):.0f}" y="{by(y):.0f}" width="{w*MM:.0f}" height="{h*MM:.0f}" '
            f'rx="3" fill="{col}" fill-opacity="0.82" stroke="{col}" stroke-width="1.4"/>'
        )
        cx, cy = bx(x + w / 2), by(y + h / 2)
        parts.append(
            f'<text x="{cx:.0f}" y="{cy-2:.0f}" font-size="10" fill="#12202c" '
            f'text-anchor="middle" font-weight="700">{n}</text>'
        )
        parts.append(
            f'<text x="{cx:.0f}" y="{cy+9:.0f}" font-size="8.5" fill="#12202c" '
            f'text-anchor="middle">{esc(short)}</text>'
        )

    # Internal USB cable: USB-A (11) -> Teensy micro-B (right end of Teensy).
    ax, ay = bx(44 + 7), by(28 + 8)
    tx, ty = bx(78), by(41 + 9)
    parts.append(
        f'<path d="M{ax:.0f},{ay:.0f} C{ax:.0f},{ay+40:.0f} {tx:.0f},{ty-30:.0f} {tx:.0f},{ty:.0f}" '
        f'fill="none" stroke="#E8804C" stroke-width="2" stroke-dasharray="4 3"/>'
    )
    parts.append(
        f'<text x="{bx(52):.0f}" y="{by(39):.0f}" font-size="8.5" fill="#c25a2a">'
        f'internal cable (sole 5V + data)</text>'
    )
    # microSD / Ethernet note below the Teensy.
    parts.append(
        f'<text x="{bx(BOARD_W/2):.0f}" y="{by(63):.0f}" font-size="9" fill="#3c7a3c" '
        f'text-anchor="middle">microSD + native Ethernet pins on the Teensy stay accessible</text>'
    )

    # Legend / BOM list on the right.
    lx = OX + ENC_W * MM + 30
    ly = OY - 6 * MM + 6
    parts.append(f'<text x="{lx}" y="{ly:.0f}" font-size="14" font-weight="700" fill="#1a2430">Placed blocks</text>')
    ly += 20
    for n, _x, _y, _w, _h, cat, _short, desc, mpn in PARTS:
        parts.append(f'<rect x="{lx}" y="{ly-9:.0f}" width="11" height="11" rx="2" fill="{PAL[cat]}"/>')
        parts.append(
            f'<text x="{lx+18}" y="{ly:.0f}" font-size="11" fill="#26313d">'
            f'<tspan font-weight="700">{n}.</tspan> {esc(desc)} '
            f'<tspan fill="#8a95a1">&#8212; {esc(mpn)}</tspan></text>'
        )
        ly += 18

    ly += 8
    for cat, lbl in [
        ("optical", "optical S/PDIF"), ("usb", "USB"), ("eth", "Ethernet"),
        ("teensy", "Teensy module"), ("ic", "logic IC"), ("power", "power protection"),
        ("passive", "passives"), ("header", "expansion header"), ("service", "service / test"),
    ]:
        parts.append(f'<rect x="{lx}" y="{ly-9:.0f}" width="11" height="11" rx="2" fill="{PAL[cat]}"/>')
        parts.append(f'<text x="{lx+18}" y="{ly:.0f}" font-size="10.5" fill="#5a6675">{esc(lbl)}</text>')
        ly += 16

    parts.append("</svg>")
    return "\n".join(parts)


if __name__ == "__main__":
    out = Path(__file__).with_name("board-floorplan.svg")
    out.write_text(build())
    print(f"wrote {out}")
