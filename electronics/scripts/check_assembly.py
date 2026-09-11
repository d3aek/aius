#!/usr/bin/env python3
"""Check stock cable mapping and the first-build DNP contract.

python3 electronics/scripts/check_assembly.py --netlist <KiCad XML export>
Without --netlist, check the source JSON only. This is not a mating test or
approval to populate independently powered expansion.
"""
import argparse
import json
from pathlib import Path
import xml.etree.ElementTree as ET

ELEC = Path(__file__).resolve().parents[1]
DNP = {"J10", "J11", "J12", "J13", "J14", "J15"}
CABLES = {
    ("J18", "1"): "ETH_RXP", ("J18", "2"): "ETH_RXN",
    ("J18", "3"): "ETH_LED", ("J18", "4"): "GND",
    ("J18", "5"): "ETH_TXN", ("J18", "6"): "ETH_TXP",
    ("J19", "1"): "ON_OFF", ("J19", "2"): "PROGRAM",
    ("J19", "3"): "GND", ("J19", "5"): "VBAT",
}
CONSUMERS = {
    ("J20", "1"): "ETH_TXP", ("J20", "2"): "ETH_TXN",
    ("J20", "3"): "ETH_RXP", ("J20", "6"): "ETH_RXN",
    ("J20", "9"): "ETH_LED", ("J20", "10"): "GND",
    ("J16", "1"): "PROGRAM", ("J16", "2"): "VBAT", ("J16", "3"): "GND",
    ("J17", "1"): "+3V3", ("J17", "2"): "PROGRAM", ("J17", "3"): "GND",
    ("J17", "4"): "ON_OFF", ("J17", "5"): "UART_TX", ("J17", "6"): "UART_RX",
    ("U10", "29"): "UART_TX_T", ("U10", "28"): "UART_RX_T",
    ("R24", "1"): "UART_TX_T", ("R24", "2"): "UART_TX",
    ("R25", "1"): "UART_RX_T", ("R25", "2"): "UART_RX",
}
REMOVED = {"VUSB", "VBAT", "PGM", "ONOFF", "ERXP", "ERXN", "ELED", "EGND", "ETXP", "ETXN"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--netlist", type=Path)
    args = parser.parse_args()
    connections = {}
    components = {}
    errors = []
    for filename in ("teensy-expansion.json", "ethernet.json"):
        spec = json.loads((ELEC / "connectivity" / filename).read_text())
        components.update((c["ref"], c) for c in spec["components"])
        for net, nodes in spec["nets"].items():
            for ref, pin in nodes:
                connections.setdefault((ref, pin), set()).add(net)
    if args.netlist:
        xml = ET.parse(args.netlist).getroot()
        connections = {}
        for net in xml.findall("nets/net"):
            for node in net.findall("node"):
                connections.setdefault((node.get("ref"), node.get("pin")), set()).add(
                    net.get("name", "").rsplit("/", 1)[-1])
        actual_dnp = {c.get("ref") for c in xml.findall("components/comp")
                      if c.find("property[@name='dnp']") is not None}
        if actual_dnp != DNP:
            errors.append(f"exported DNP set: expected {sorted(DNP)}, got {sorted(actual_dnp)}")
    source_dnp = {ref for ref, c in components.items() if c.get("dnp") is True}
    if source_dnp != DNP:
        errors.append(f"source DNP set: expected {sorted(DNP)}, got {sorted(source_dnp)}")
    for endpoint, net in (CABLES | CONSUMERS).items():
        if connections.get(endpoint) != {net}:
            errors.append(f"cable {endpoint}: expected {net}, got {connections.get(endpoint)}")
    # KiCad may export an explicit no-connect as an unconnected-(...) net.
    if any(not n.startswith("unconnected-") for n in connections.get(("J19", "4"), set())):
        errors.append("J19.4 must remain unused; omit the Teensy 3V3 service wire")
    if any(ref == "U10" and pin in REMOVED for ref, pin in connections):
        errors.append("removed auxiliary pads still appear on U10")
    for error in errors:
        print("FAIL:", error)
    print(f"Cable/DNP assembly checks: {'FAIL' if errors else 'PASS'}")
    return bool(errors)


if __name__ == "__main__":
    raise SystemExit(main())
