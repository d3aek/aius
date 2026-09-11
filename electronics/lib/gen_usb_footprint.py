#!/usr/bin/env python3
"""Derive USB4105 lands with 0.20 mm NPTH clearance from pinned KiCad stock.

scripts/kicad.sh --python electronics/lib/gen_usb_footprint.py
Only the hole-facing edge of the four overlapping GND pads retreats 0.02 mm.
See carrier-footprint-review.md: GCT's layout tolerance is ±0.05 mm.
"""
from pathlib import Path
import pcbnew

NAME = "USB_C_GCT_USB4105_NPTH_Clearance"


def main():
    fp = pcbnew.FootprintLoad("/usr/share/kicad/footprints/Connector_USB.pretty",
                             "USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal")
    if fp is None:
        raise RuntimeError("pinned stock USB4105 footprint missing")
    for pad in fp.Pads():
        if pad.GetNumber() not in {"A1", "A12", "B1", "B12"}:
            continue
        old = pad.GetPosition()
        pad.SetPosition(pcbnew.VECTOR2I(old.x, pcbnew.FromMM(-3.690)))
        pad.SetSize(pcbnew.VECTOR2I(pcbnew.FromMM(0.60), pcbnew.FromMM(1.13)))
    fp.SetFPID(pcbnew.LIB_ID("aius", NAME))
    fp.SetValue(NAME)
    fp.SetLibDescription("GCT USB4105: stock geometry except GND hole-facing edge retreats 0.02mm; "
                      "0.2138mm nominal NPTH clearance, within GCT layout tolerance. "
                      "Connector datum and locator holes unchanged.")
    path = Path(__file__).resolve().parent / "aius.pretty"
    pcbnew.FootprintSave(str(path), fp)
    print(f"wrote {path / (NAME + '.kicad_mod')}")


if __name__ == "__main__":
    main()
