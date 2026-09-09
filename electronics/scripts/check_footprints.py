#!/usr/bin/env python3
"""Check manufactured pad interfaces against the reviewed source drawings.

scripts/kicad.sh --python electronics/scripts/check_footprints.py
scripts/kicad.sh --python electronics/scripts/check_footprints.py --board <pcb>

The board mode checks embedded footprints as well as the library mode. It
deliberately fails on the old routed board until that board is rebuilt.
"""
from __future__ import annotations

import argparse
from pathlib import Path

import pcbnew

ELEC = Path(__file__).resolve().parent.parent


def mm(point):
    return tuple(round(pcbnew.ToMM(v), 3) for v in (point.x, point.y))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", type=Path)
    args = parser.parse_args()
    names = {
        "U10": "Teensy41_Sockets_SSW-124_MMS-103_SSW-105",
        "U20": "Cliff_ORJ5_TOSLINK",
        "U24": "Cliff_OTJ5_TOSLINK",
        "J20": "Pulse_J0011D21BNL_TabDown",
    }
    board = pcbnew.LoadBoard(str(args.board)) if args.board else None
    fps = {fp.GetReference(): fp for fp in board.GetFootprints()} if board else {
        ref: pcbnew.FootprintLoad(str(ELEC / "lib/aius.pretty"), name)
        for ref, name in names.items()
    }
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
        # Four asymmetric corners detect reflected or transposed main rows.
        points("U10", "VIN", [(0, 0)])
        points("U10", "0", [(2.54, 15.24)])
        points("U10", "33", [(58.42, 0)])
        points("U10", "32", [(58.42, 15.24)])
        auxiliary = {
            "UH5V": (6.35, 12.19), "UHDM": (8.89, 12.19),
            "UHDP": (11.43, 12.19), "UHGND": (13.97, 12.19),
            "ERXP": (12.97, 2.45), "ERXN": (14.97, 2.45),
            "ELED": (12.97, 4.45), "EGND": (14.97, 4.45),
            "ETXN": (12.97, 6.45), "ETXP": (14.97, 6.45),
            "ONOFF": (45.72, 2.54), "PGM": (45.72, 5.08),
            "VBAT": (45.72, 12.7), "VUSB": (2.54, 2.54),
        }
        for pin, pos in auxiliary.items():
            points("U10", pin, [pos])
        points("U10", "GND", [(0, 15.24), (2.54, 0), (35.56, 0),
                                (16.51, 12.19), (45.72, 7.62)])
        points("U10", "3V3", [(5.08, 0), (35.56, 15.24), (45.72, 10.16)])
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
        expect(len(list(fps["U10"].Pads())) == 65, "Teensy must have 65 socket pads")
        for p in fps["U10"].Pads():
            expected = 0.79 if p.GetNumber() in {"ERXP", "ERXN", "ETXP", "ETXN", "ELED", "EGND"} else 1.04
            expect(round(pcbnew.ToMM(p.GetDrillSize().x), 3) == expected,
                   f"U10.{p.GetNumber()}: drill must be {expected} mm")
        if board:
            original = {fp.GetReference(): fp for fp in board.GetFootprints()}
            u1 = original["U1"]
            expect(u1.GetFPID().GetLibItemName() == "Texas_DRT-3", "U1 package is not TI DRT")
            j20pads = {p.GetNumber(): p.GetNetname() for p in original["J20"].Pads()}
            expect(j20pads["8"] == "GND", "J20.8 termination return must be PCB GND")
            expect(j20pads["SH"] == "CHASSIS", "J20 shield must remain on CHASSIS")

    for failure in failures:
        print(f"FAIL: {failure}")
    print(f"Footprint checks: {checks - len(failures)}/{checks} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
