#!/usr/bin/env python3
"""dsp_model_pad.py turns the committed live model into a 12,288 B body the host
parser accepts and decodes to the same model (Phase 4f F10, CR-S3, R5-S3).

Compiles dsp_model_dump.c against the real parser, dumps input and padded
output and compares. Usage: python3 test_dsp_model_pad.py [FIRMWARE_ROOT]
(or python3 -m unittest with FIRMWARE_ROOT unset: the root is inferred).
"""
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
FW = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 and Path(sys.argv[1]).is_dir() else HERE.parents[1]
TOOL = FW / "tools" / "dsp_model_pad.py"
FIXTURE = HERE / "fixtures" / "dsp_config_live_2026-10-07.json"
LIMIT = 12288
NUMBER_TEXT_MAX = 63
# The compile flags match test.sh's dsp-config arm.
CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=address,undefined"]
TOKEN = re.compile(r'"(?:[^"\\]|\\.)*"|-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?')


def numbers_in(text):
    return [m.group(0) for m in TOKEN.finditer(text) if m.group(0)[0] != '"']


class ModelPad(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dump = Path(cls.tmp.name) / "dsp_model_dump"
        cc = os.environ.get("CC", "cc")
        subprocess.run(
            [cc, *CFLAGS,
             f"-I{FW}/lib/aius_dsp_config/include", f"-I{FW}/lib/aius_json/include",
             str(HERE / "dsp_model_dump.c"),
             str(FW / "lib/aius_dsp_config/src/aius_dsp_config.c"),
             str(FW / "lib/aius_json/src/aius_json.c"),
             "-lm", "-o", str(cls.dump)],
            check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def dump_of(self, path):
        r = subprocess.run([str(self.dump), str(path)], capture_output=True, text=True)
        return r.returncode, r.stdout, r.stderr

    def put_input(self):
        """The live GET model as a PUT body (adds base_generation, as the setter does)."""
        body = json.loads(FIXTURE.read_text())
        body["base_generation"] = body["config_generation"]
        path = Path(self.tmp.name) / "put_input.json"
        path.write_text(json.dumps(body, separators=(",", ":")))
        return path

    def pad(self, src, *extra):
        out = Path(self.tmp.name) / "padded.json"
        r = subprocess.run([sys.executable, "-I", str(TOOL), str(src), "-o", str(out), *extra],
                           capture_output=True, text=True)
        return r, out

    def test_pads_to_limit_and_decodes_to_same_model(self):
        src = self.put_input()
        r, out = self.pad(src)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(out.stat().st_size, LIMIT)
        rc_in, dump_in, err_in = self.dump_of(src)
        self.assertEqual(rc_in, 0, err_in)
        rc_out, dump_out, err_out = self.dump_of(out)
        self.assertEqual(rc_out, 0, err_out)
        self.assertGreater(dump_in.count("\n"), 60)
        self.assertEqual(dump_in, dump_out)
        # Sanity: the dump really sees the model (not an all-default blank).
        self.assertIn("input.fc.eq0.frequency_hz=45\n", dump_in)
        self.assertIn("matrix.center_gain_linear=1.5\n", dump_in)

    def test_split_matches_counted_share_and_numbers_are_at_longest(self):
        r, out = self.pad(self.put_input())
        self.assertEqual(r.returncode, 0, r.stderr)
        split = json.loads(r.stdout)
        text = out.read_text()
        counted = numbers_in(text)
        self.assertEqual(split["numbers"], len(counted))
        self.assertEqual(split["number_bytes"], sum(len(n) for n in counted))
        self.assertEqual(split["whitespace_bytes"], len(text) - len(text.rstrip(" ")))
        self.assertEqual(split["total"], LIMIT)
        self.assertAlmostEqual(split["number_share"], sum(len(n) for n in counted) / LIMIT)
        self.assertGreater(split["whitespace_bytes"], 0)
        # No number has a longer accepted spelling left: each is at the parser's
        # 63 B cap, or (never for this fixture) it could not be grown.
        for n in counted:
            self.assertEqual(len(n), NUMBER_TEXT_MAX, n)
        self.assertEqual(split["numbers_at_max"], len(counted))
        # And the cap is real: one byte more is refused by the parser.
        longer = text.rstrip(" ").replace(counted[0], counted[0] + "0", 1)
        probe = Path(self.tmp.name) / "longer.json"
        probe.write_text(longer)
        rc, _, err = self.dump_of(probe)
        self.assertEqual(rc, 1)
        self.assertIn("range", err.lower())

    def test_base_generation_flag_lands_in_the_model(self):
        r, out = self.pad(FIXTURE, "--base-generation", "7")
        self.assertEqual(r.returncode, 0, r.stderr)
        rc, dump, err = self.dump_of(out)
        self.assertEqual(rc, 0, err)
        self.assertIn("base_generation=7\n", dump)
        self.assertEqual(out.stat().st_size, LIMIT)

    def test_input_over_the_limit_fails(self):
        big = Path(self.tmp.name) / "big.json"
        body = json.loads(FIXTURE.read_text())
        body["pad"] = "a" * LIMIT
        big.write_text(json.dumps(body))
        r, _ = self.pad(big)
        self.assertEqual(r.returncode, 1)
        self.assertIn("limit", r.stderr)

    def test_invalid_json_fails(self):
        bad = Path(self.tmp.name) / "bad.json"
        bad.write_text('{"source":')
        r, _ = self.pad(bad)
        self.assertEqual(r.returncode, 1)


if __name__ == "__main__":
    if len(sys.argv) > 1 and Path(sys.argv[1]).is_dir():
        del sys.argv[1]
    unittest.main()
