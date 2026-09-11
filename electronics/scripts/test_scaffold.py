#!/usr/bin/env python3
"""Check the placement builder through its CLI in the pinned KiCad container.

scripts/kicad.sh --python electronics/scripts/test_scaffold.py
The candidate is intentionally unrouted. DRC violations must be zero, while
unconnected items remain open for routing; neither is silently ignored.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import pcbnew

ROOT = Path(__file__).resolve().parents[2]
BUILDER = ROOT / "electronics/scripts/scaffold_pcb.py"


class Scaffold(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = ROOT / ".scratch" / os.environ.get("CODEX_THREAD_ID", "scaffold-tests")
        scratch.mkdir(parents=True, exist_ok=True)
        cls.directory = Path(tempfile.mkdtemp(prefix="placement-", dir=scratch))
        cls.netlist = cls.directory / "netlist.xml"
        subprocess.run(["kicad-cli", "sch", "export", "netlist", "--format", "kicadxml",
                        "-o", str(cls.netlist), str(ROOT / "electronics/aius-carrier.kicad_sch")],
                       check=True, capture_output=True, text=True)

    def build(self, fault=None):
        xml = ET.parse(self.netlist)
        comp = xml.find("components/comp[@ref='J1']")
        if fault == "missing-footprint":
            comp.remove(comp.find("footprint"))
        elif fault == "missing-pad":
            xml.find("nets/net/node[@ref='J1']").set("pin", "NONEXISTENT")
        elif fault == "unplaced":
            extra = ET.SubElement(xml.find("components"), "comp", ref="R99")
            ET.SubElement(extra, "value").text = "1k"
            ET.SubElement(extra, "footprint").text = "Resistor_SMD:R_0402_1005Metric"
        stem = fault or "valid"
        source, output = self.directory / f"{stem}.xml", self.directory / f"{stem}.kicad_pcb"
        xml.write(source)
        result = subprocess.run([sys.executable, str(BUILDER), "--netlist", str(source),
                                 "--output", str(output)], capture_output=True, text=True)
        return result, output

    def test_valid_geometry_nets_and_dnp(self):
        result, path = self.build()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        board = pcbnew.LoadBoard(str(path))
        fps = {fp.GetReference(): fp for fp in board.GetFootprints()}
        xml = ET.parse(self.netlist)
        expected_refs = {c.get("ref") for c in xml.findall("components/comp") if c.findtext("footprint")}
        self.assertEqual(set(fps), expected_refs)
        self.assertEqual({r for r, fp in fps.items() if fp.IsDNP()},
                         {"J10", "J11", "J12", "J13", "J14", "J15"})
        self.assertEqual(board.GetCopperLayerCount(), 4)
        self.assertEqual(len(list(board.GetTracks())), 0)
        for layer in (pcbnew.In1_Cu, pcbnew.In2_Cu):
            self.assertEqual(board.GetLayerType(layer), pcbnew.LT_POWER)
        expected = {}
        for net in xml.findall("nets/net"):
            for node in net.findall("node"):
                expected[(node.get("ref"), node.get("pin"))] = net.get("name")
        for ref, fp in fps.items():
            for pad in fp.Pads():
                self.assertEqual(pad.GetNetname(), expected.get((ref, pad.GetNumber()), ""),
                                 f"{ref}.{pad.GetNumber()}")
        report = path.with_suffix(".drc.json")
        subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "-o", str(report), str(path)],
                       check=True, capture_output=True, text=True)
        drc = json.loads(report.read_text())
        self.assertEqual(drc["violations"], [], json.dumps(drc["violations"], indent=2))
        self.assertGreater(len(drc["unconnected_items"]), 0, "unrouted candidate must not claim connectivity")

    def assert_rejected(self, fault, message):
        result, output = self.build(fault)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(message, result.stderr)
        self.assertFalse(output.exists(), "invalid input must not produce a board")

    def test_missing_footprint_rejected(self):
        self.assert_rejected("missing-footprint", "missing footprint")

    def test_missing_pad_rejected(self):
        self.assert_rejected("missing-pad", "netlist pins absent from footprint")

    def test_unplaced_part_rejected(self):
        self.assert_rejected("unplaced", "missing placement")

    def test_explicit_paths_required(self):
        result = subprocess.run([sys.executable, str(BUILDER)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("--netlist", result.stderr)
        self.assertIn("--output", result.stderr)

    def test_inner_plane_routing_rejected(self):
        result, path = self.build()
        self.assertEqual(result.returncode, 0, result.stderr)
        board = pcbnew.LoadBoard(str(path))
        for layer in (pcbnew.In1_Cu, pcbnew.In2_Cu):
            track = pcbnew.PCB_TRACK(board)
            track.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(2), pcbnew.FromMM(40)))
            track.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(2), pcbnew.FromMM(45)))
            track.SetWidth(pcbnew.FromMM(0.2))
            track.SetLayer(layer)
            track.SetNetCode(board.FindNet("GND").GetNetCode())
            board.Add(track)
        pcbnew.SaveBoard(str(path), board)
        report = path.with_suffix(".plane-fault.json")
        subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "-o", str(report), str(path)],
                       check=True, capture_output=True, text=True)
        drc = json.loads(report.read_text())
        self.assertEqual(sum(v["type"] == "items_not_allowed" for v in drc["violations"]), 2)


if __name__ == "__main__":
    unittest.main()
