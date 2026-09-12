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
        # Expansion headers plus the DNP CHASSIS-GND RC tie (R6/C5).
        self.assertEqual({r for r, fp in fps.items() if fp.IsDNP()},
                         {"J10", "J11", "J12", "J13", "J14", "J15", "R6", "C5"})
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

    def test_keepout_rule_areas(self):
        """H3e design decisions (2026-09-12): both Hammond-slot edge bands and
        the internal USB cable envelope are emitted as no-copper/no-footprint
        rule areas with the expected bounds."""
        result, path = self.build()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        board = pcbnew.LoadBoard(str(path))
        rule_areas = [zone for zone in board.Zones() if zone.GetIsRuleArea()]
        self.assertEqual(len(rule_areas), 3)
        for zone in rule_areas:
            self.assertTrue(zone.GetDoNotAllowFootprints())
            # Edge bands ban copper; the cable envelope (F.Cu only) bans parts
            # only, so the I2S corridor under the cable stays routable.
            copper_banned = zone.GetLayerSet().Contains(pcbnew.In1_Cu)
            self.assertEqual(zone.GetDoNotAllowTracks(), copper_banned)
            self.assertEqual(zone.GetDoNotAllowVias(), copper_banned)
            self.assertEqual(zone.GetDoNotAllowPads(), copper_banned)
            self.assertEqual(zone.GetDoNotAllowZoneFills(), copper_banned)

        def bbox_mm(zone):
            bbox = zone.GetBoundingBox()
            return (bbox.GetLeft() / 1e6, bbox.GetTop() / 1e6,
                    bbox.GetRight() / 1e6, bbox.GetBottom() / 1e6)

        edge_zones = [zone for zone in rule_areas if zone.GetLayerSet().Contains(pcbnew.In1_Cu)]
        self.assertEqual(len(edge_zones), 2, "both Hammond-slot edge bands must be on every copper layer")
        width = scaffold_pcb.EDGE_KEEPOUT_WIDTH_MM
        edge_bboxes = sorted(bbox_mm(zone) for zone in edge_zones)
        left, top, right, bottom = edge_bboxes[0]
        self.assertAlmostEqual(left, 0.0, places=3)
        self.assertAlmostEqual(right, width, places=3)
        self.assertAlmostEqual(top, 0.0, places=3)
        self.assertAlmostEqual(bottom, scaffold_pcb.BOARD_H, places=3)
        left, top, right, bottom = edge_bboxes[1]
        self.assertAlmostEqual(left, scaffold_pcb.BOARD_W - width, places=3)
        self.assertAlmostEqual(right, scaffold_pcb.BOARD_W, places=3)
        self.assertAlmostEqual(top, 0.0, places=3)
        self.assertAlmostEqual(bottom, scaffold_pcb.BOARD_H, places=3)

        envelope_zones = [zone for zone in rule_areas if zone not in edge_zones]
        self.assertEqual(len(envelope_zones), 1)
        envelope = envelope_zones[0]
        self.assertTrue(envelope.GetLayerSet().Contains(pcbnew.F_Cu))
        self.assertFalse(envelope.GetLayerSet().Contains(pcbnew.In1_Cu))
        self.assertFalse(envelope.GetLayerSet().Contains(pcbnew.B_Cu))
        left, top, right, bottom = bbox_mm(envelope)
        jx, jy = scaffold_pcb.USB_CABLE_J2_OPENING_MM
        half_w = scaffold_pcb.USB_CABLE_PLUG_WIDTH_MM / 2
        # The plug-body rectangle (J2's own opening) must be fully enclosed.
        self.assertLessEqual(left, jx - half_w)
        self.assertGreaterEqual(right, jx + half_w)
        self.assertLessEqual(top, jy)
        self.assertGreaterEqual(bottom, jy + scaffold_pcb.USB_CABLE_PLUG_HEIGHT_MM)
        # The corridor must stop clear of U10's own already-placed footprint
        # (see USB_CABLE_U10_COURTYARD_LEFT_EDGE_MM) -- never re-claim U10's
        # own courtyard as a foreign intrusion.
        self.assertLess(right, scaffold_pcb.USB_CABLE_U10_COURTYARD_LEFT_EDGE_MM)

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
        # Count only the *added* tracks' own keepout violations -- isolates
        # this test's own In1.Cu/In2.Cu fault from any other "items not
        # allowed" finding the scaffold's own H3e keepout rule areas might
        # independently report (e.g. against a footprint, not a track).
        track_violations = [v for v in drc["violations"] if v["type"] == "items_not_allowed"
                             and any("Track" in item["description"] for item in v["items"])]
        self.assertEqual(len(track_violations), 2, json.dumps(track_violations, indent=2))


if __name__ == "__main__":
    unittest.main()
