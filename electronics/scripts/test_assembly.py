#!/usr/bin/env python3
"""Exercise assembly checks and native DNP emission through their CLIs."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
SCRATCH = ROOT / ".scratch" / os.environ.get("CODEX_THREAD_ID", "assembly-tests")


class Assembly(unittest.TestCase):
    def directory(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        return Path(tempfile.mkdtemp(prefix="assembly-", dir=SCRATCH))

    def gate(self, fault=None):
        xml = ET.Element("export")
        comps = ET.SubElement(xml, "components")
        nets = ET.SubElement(xml, "nets")
        for name in ("teensy-expansion", "ethernet"):
            spec = json.loads((ROOT / f"electronics/connectivity/{name}.json").read_text())
            for c in spec["components"]:
                comp = ET.SubElement(comps, "comp", ref=c["ref"])
                if c.get("dnp") and not (fault == "populated" and c["ref"] == "J10"):
                    ET.SubElement(comp, "property", name="dnp", value="")
            for net_name, nodes in spec["nets"].items():
                net = ET.SubElement(nets, "net", name=f"/{name}/{net_name}")
                for ref, pin in nodes:
                    if fault == "missing-consumer" and ref == "J17" and pin == "6":
                        continue
                    if fault == "reversed-pair" and ref == "J18" and pin in ("1", "2"):
                        pin = "2" if pin == "1" else "1"
                    ET.SubElement(net, "node", ref=ref, pin=pin)
        if fault == "service-short":
            ET.SubElement(net, "node", ref="J19", pin="4")
        path = self.directory() / "netlist.xml"
        ET.ElementTree(xml).write(path)
        return subprocess.run([sys.executable, str(ROOT / "electronics/scripts/check_assembly.py"),
                               "--netlist", str(path)], capture_output=True, text=True)

    def test_stock_mapping(self):
        result = self.gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_reversed_pair(self):
        self.assertNotEqual(self.gate("reversed-pair").returncode, 0)

    def test_populated_expansion(self):
        self.assertNotEqual(self.gate("populated").returncode, 0)

    def test_unused_service_wire(self):
        self.assertNotEqual(self.gate("service-short").returncode, 0)

    def test_disconnected_consumer(self):
        self.assertNotEqual(self.gate("missing-consumer").returncode, 0)

    def generate(self, dnp):
        spec = json.loads((ROOT / "electronics/connectivity/ethernet.json").read_text())
        spec["components"][0]["dnp"] = dnp
        directory = self.directory()
        path, output = directory / "sheet.json", directory / "sheet.kicad_sch"
        path.write_text(json.dumps(spec))
        result = subprocess.run([sys.executable, str(ROOT / "electronics/generate.py"),
                                 str(path), str(output), f"--lib-dir={ROOT / 'electronics/lib'}"],
                                capture_output=True, text=True)
        return result, output.read_text() if output.exists() else ""

    def test_native_dnp_emission(self):
        result, text = self.generate(True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(text.count("(dnp yes)"), 1)
        self.assertEqual(text.count("(dnp no)"), 2)

    def test_string_false_rejected(self):
        result, _ = self.generate("false")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("dnp must be a JSON boolean", result.stderr)


if __name__ == "__main__":
    unittest.main()
