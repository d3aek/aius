#!/usr/bin/env python3
"""Check the placement builder through its CLI in the pinned KiCad container.

scripts/kicad.sh --python electronics/scripts/test_scaffold.py
The candidate is intentionally unrouted. DRC violations must be zero, while
unconnected items remain open for routing; neither is silently ignored.
"""
import importlib.util
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

# Import the builder as a module (its CLI guard keeps this side-effect-free) so
# the stackup/net-class assertions below share the same constants the builder
# writes from, rather than duplicating the magic numbers.
_spec = importlib.util.spec_from_file_location("scaffold_pcb", BUILDER)
scaffold_pcb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(scaffold_pcb)


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

    def test_stackup_declared(self):
        result, path = self.build()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        text = path.read_text()
        self.assertIn("(stackup", text)
        copper_layers = 0
        for name, kind, thickness_mm, *_ in scaffold_pcb.STACKUP_LAYERS:
            self.assertIn(f'(layer "{name}"', text, f"missing stackup layer {name}")
            self.assertIn(f"(thickness {thickness_mm})", text, f"wrong thickness for {name}")
            if kind == "copper":
                copper_layers += 1
        self.assertEqual(copper_layers, 4)
        self.assertIn(f"(thickness {scaffold_pcb.BOARD_THICKNESS_MM})", text,
                      "general.thickness must match the summed stackup")

    def test_net_classes_assigned(self):
        result, path = self.build()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        project = json.loads(path.with_suffix(".kicad_pro").read_text())
        classes = {entry["name"]: entry for entry in project["net_settings"]["classes"]}
        for name, (track_mm, clearance_mm, diff_width_mm, diff_gap_mm) in scaffold_pcb.NETCLASS_SPECS.items():
            self.assertIn(name, classes)
            self.assertEqual(classes[name]["track_width"], track_mm, name)
            self.assertEqual(classes[name]["clearance"], clearance_mm, name)
            self.assertEqual(classes[name]["diff_pair_width"], diff_width_mm, name)
            self.assertEqual(classes[name]["diff_pair_gap"], diff_gap_mm, name)
        by_class = {}
        for entry in project["net_settings"]["netclass_patterns"]:
            by_class.setdefault(entry["netclass"], set()).add(entry["pattern"])
        representative = {"Power": "VBUS_RAW", "USB90": "USB_DP", "Eth100": "ETH_TXP"}
        for net_class, bare_name in representative.items():
            members = by_class.get(net_class, set())
            self.assertGreater(len(members), 0, net_class)
            self.assertTrue(any(scaffold_pcb._bare_net_name(pattern) == bare_name for pattern in members),
                            f"{net_class} missing {bare_name}: {members}")

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
