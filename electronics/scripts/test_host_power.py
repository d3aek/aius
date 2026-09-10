#!/usr/bin/env python3
"""Exercise the host-power gate through its exported-netlist CLI."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
SCRATCH = ROOT / ".scratch" / os.environ.get("CODEX_THREAD_ID", "host-power-tests")


class HostPowerGate(unittest.TestCase):
    def run_gate(self, fault=None):
        root = ET.Element("export")
        nets = ET.SubElement(root, "nets")
        net = ET.SubElement(nets, "net", name="/teensy-expansion/EXP_USBH_VBUS")
        for ref, pin in (("U10", "UH5V"), ("J14", "1"), ("C10", "1"), ("C11", "1")):
            ET.SubElement(net, "node", ref=ref, pin=pin,
                          pintype="power_out" if ref == "U10" else "passive")
        if fault == "parallel":
            ET.SubElement(net, "node", ref="U12", pin="6", pintype="power_out")
        elif fault == "wrong-type":
            net[0].set("pintype", "power_in")
        elif fault == "disconnected":
            net.remove(net[0])
        SCRATCH.mkdir(parents=True, exist_ok=True)
        directory = tempfile.mkdtemp(prefix="host-power-test-", dir=SCRATCH)
        path = Path(directory) / "netlist.xml"
        ET.ElementTree(root).write(path)
        return subprocess.run([sys.executable, str(ROOT / "electronics/scripts/check_host_power.py"),
                               "--netlist", str(path)], capture_output=True, text=True)

    def test_native_source_passes(self):
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_parallel_source_rejected(self):
        result = self.run_gate("parallel")
        self.assertEqual(result.returncode, 1)
        self.assertIn("sole power_out", result.stdout)

    def test_wrong_module_pin_type_rejected(self):
        result = self.run_gate("wrong-type")
        self.assertEqual(result.returncode, 1)
        self.assertIn("sole power_out", result.stdout)

    def test_missing_source_rejected(self):
        result = self.run_gate("disconnected")
        self.assertEqual(result.returncode, 1)
        self.assertIn("connect UH5V exactly once", result.stdout)


if __name__ == "__main__":
    unittest.main()
