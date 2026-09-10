#!/usr/bin/env python3
"""Regression gate for the Teensy USB-host supply, in sources and exported XML.

python3 electronics/scripts/check_host_power.py [--netlist <kicadxml>]
The exported-netlist check catches stale generated schematics as well as pin types.
"""
import argparse
import json
from pathlib import Path
import xml.etree.ElementTree as ET

ELEC = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--netlist", type=Path)
    args = parser.parse_args()
    spec = json.loads((ELEC / "connectivity/teensy-expansion.json").read_text())
    expected = {("U10", "UH5V"), ("J14", "1"), ("C10", "1"), ("C11", "1")}
    failures = []
    actual = {tuple(node) for node in spec["nets"]["EXP_USBH_VBUS"]}
    if actual != expected:
        failures.append(f"host VBUS must be native VHST -> J14 and decoupling only; got {sorted(actual)}")
    forbidden = {"U12", "R14", "R15", "R16", "C12"}
    if forbidden & {c["ref"] for c in spec["components"]}:
        failures.append("obsolete parallel host supply parts remain")
    if any(name.startswith("EXP_USBH_") and name != "EXP_USBH_VBUS" for name in spec["nets"]):
        failures.append("orphan carrier host-switch control nets remain")
    if args.netlist:
        tree = ET.parse(args.netlist)
        nets = [n for n in tree.findall("./nets/net")
                if any(p.get("ref") == "U10" and p.get("pin") == "UH5V" for p in n.findall("node"))]
        if len(nets) != 1:
            failures.append("exported netlist must connect UH5V exactly once")
        else:
            nodes = nets[0].findall("node")
            if {(n.get("ref"), n.get("pin")) for n in nodes} != expected:
                failures.append("exported host VBUS connectivity differs from the reviewed source")
            sources = {(n.get("ref"), n.get("pin")) for n in nodes if n.get("pintype") == "power_out"}
            if sources != {("U10", "UH5V")}:
                failures.append(f"native UH5V must be the sole power_out; got {sorted(sources)}")
    for failure in failures:
        print(f"FAIL: {failure}")
    print("USB-host power: " + ("FAIL" if failures else "PASS"))
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
