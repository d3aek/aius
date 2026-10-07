"""stack-bound: W8 fixture cases (linked by the suite) and the optical images.

The optical cases skip, naming the missing artifact, until parent-wiring
lands -fstack-usage, -Wl,-Map and -Wl,--emit-relocs. Environment:
AIUS_PIO_BUILD (default firmware/.pio/build), AIUS_STACK_ENV (default
teensy41-optical-slot-a, the canonical image).
"""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "producer-isolation"))

import aius_walk as aw  # noqa: E402
import fixture_build as fb  # noqa: E402
import stack_bound as sb  # noqa: E402

SKIP = fb.available()
FW = os.path.normpath(os.path.join(HERE, "..", ".."))


def bound(src, t=None, st=None, defines=(), debug=False):
    b = fb.build(src, defines, debug)
    img = aw.Image(b["elf"], b["map"])
    return sb.StackBound(img, t or aw.Tables(), st or sb.StackTables(), sb.load_su([b["dir"]]))


@unittest.skipIf(SKIP, SKIP or "")
class Fixtures(unittest.TestCase):
    def test_veneer_adds_no_frame_and_itcm_frame_counts(self):
        s = bound("fx_veneer.c", debug=True)
        d, path = s.root_depth([("fx_flash_root", None)])
        f = {n: s.frame(s.img.resolve(n)[0]) for n in ("fx_flash_root", "fx_itcm_fn", "yield", "__fx_itcm_fn_veneer")}
        self.assertEqual(f["__fx_itcm_fn_veneer"], 0)
        self.assertGreaterEqual(f["fx_itcm_fn"], 200)
        self.assertEqual(d, f["fx_flash_root"] + f["fx_itcm_fn"] + f["yield"])
        self.assertIn("fx_itcm_fn", path)
        self.assertEqual(s.failures, [])

    def test_indirect_site_charged_its_deepest_target(self):
        row = aw.SiteRow("fx_ind_root", None, 1, ("taken_by", r"fx_indirect"))
        s = bound("fx_indirect.c", aw.Tables(sites=[row]))
        d, path = s.root_depth([("fx_ind_root", None)])
        fr = lambda n: s.frame(s.img.resolve(n)[0])  # noqa: E731
        self.assertEqual(d, fr("fx_ind_root") + max(fr("fx_tgt_big"), fr("fx_jump_table")))
        self.assertIn("fx_tgt_big", path)
        self.assertEqual(s.failures, [])

    def test_unrowed_site_fails(self):
        s = bound("fx_indirect.c")
        s.root_depth([("fx_ind_root", None)])
        self.assertTrue(any("unlisted indirect site" in f for f in s.w.failures), s.w.failures)

    def test_recursion_charged_bound_times_chain_plus_exit(self):
        row = aw.RecursionRow(("fx_cyc_a", None), 2, [("fx_cyc_a", None), ("fx_cyc_b", None)], size=2)
        s = bound("fx_alloc.c", aw.Tables(recursion=[row]))
        d, _ = s.root_depth([("fx_cycle_root", None)])
        fr = lambda n: s.frame(s.img.resolve(n)[0])  # noqa: E731
        self.assertEqual(d, fr("fx_cycle_root") + 2 * (fr("fx_cyc_a") + fr("fx_cyc_b")) + fr("fx_exit_fn"))
        self.assertEqual(s.failures, [])

    def test_undeclared_recursion_fails(self):
        s = bound("fx_alloc.c")
        s.root_depth([("fx_cycle_root", None)])
        self.assertTrue(any("undeclared cycle" in f for f in s.failures), s.failures)

    def test_dynamic_frame_fails(self):
        s = bound("fx_alloc.c")
        s.root_depth([("fx_dyn_root", None)])
        self.assertTrue(any("fx_dyn_fn" in f and "dynamic" in f for f in s.failures), s.failures)

    def test_leaf_needs_declared_inclusive_depth(self):
        s = bound("fx_alloc.c")
        s.root_depth([("fx_alloc_root", None)])
        self.assertEqual(sorted(s.failures), ["stack-bound: no declared inclusive depth for leaf __wrap_calloc",
                                              "stack-bound: no declared inclusive depth for leaf __wrap_realloc"])
        st = sb.StackTables(leaf_inclusive={"__wrap_calloc": 64, "__wrap_realloc": 96})
        s = bound("fx_alloc.c", st=st)
        d, _ = s.root_depth([("fx_alloc_root", None)])
        self.assertEqual(d, s.frame(s.img.resolve("fx_alloc_root")[0]) + 96)
        self.assertEqual(s.failures, [])

    def test_symbol_without_su_needs_declared_frame(self):
        b = fb.build("fx_veneer.c", (), True)
        img = aw.Image(b["elf"], b["map"])
        s = sb.StackBound(img, aw.Tables(), sb.StackTables(declared={("fx_flash_root", None): 40}), {})
        s.root_depth([("fx_flash_root", None)])
        self.assertEqual(sorted(s.failures), ["stack-bound: no frame for fx_itcm_fn (fx_veneer.c.o) (.su or declared table)",
                                              "stack-bound: no frame for yield (fx_veneer.c.o) (.su or declared table)"])

    def test_declared_chain_hops_are_checked(self):
        s = bound("fx_veneer.c", debug=True)
        ok = sb.Chain("ok", [("fx_reset", None, "call"), ("fx_flash_root", None, "call"), ("fx_itcm_fn", None, "call")])
        self.assertEqual(s.check_chain(ok)[0], [])  # the second hop crosses a veneer
        missing = sb.Chain("missing", [("fx_reset", None, "call"), ("fx_itcm_fn", None, "call")])
        self.assertEqual(s.check_chain(missing)[0],
                         ["stack-bound: chain missing: fx_reset has no edge to fx_itcm_fn"])
        s = bound("fx_indirect.c")
        ind = sb.Chain("ind", [("fx_ind_root", None, "call"), ("fx_tgt_big", None, "indirect: fx_ops")])
        self.assertEqual(s.check_chain(ind)[0], [])
        bad = sb.Chain("bad", [("fx_reset", None, "call"), ("fx_tgt_big", None, "indirect: none")])
        self.assertEqual(len(s.check_chain(bad)[0]), 1)  # fx_reset has no indirect site

    def test_below_root_charges_only_hook_reaching_callees(self):
        s = bound("fx_alloc.c")
        d, path = s.below_root(s.img.resolve("fx_malloc_helper")[0], unwindowed=True)
        fr = lambda n: s.frame(s.img.resolve(n)[0])  # noqa: E731
        self.assertEqual(d, fr("fx_malloc_helper") + fr("aius_sdio_wait_hook"))
        self.assertEqual(path, ["fx_malloc_helper", "aius_sdio_wait_hook"])

    def test_below_root_hook_field(self):
        # Phase 4f: a Chain may end its walk at another hook; the default is
        # the card-wait hook, and a root that cannot reach the named hook fails.
        s = bound("fx_alloc.c")
        root = s.img.resolve("fx_malloc_helper")[0]
        self.assertEqual(s.below_root(root, True, aw.HOOK), s.below_root(root, True))
        self.assertEqual(s.failures, [])
        self.assertEqual(s.below_root(root, True, "fx_exit_fn"), (0, []))
        self.assertTrue(any("does not reach fx_exit_fn" in f for f in s.failures), s.failures)
        self.assertIsNone(sb.Chain("c", []).hook)

    def test_margin(self):
        self.assertEqual(sb.check_margin(1000, 3048), [])
        self.assertEqual(len(sb.check_margin(1001, 3048)), 1)

    def test_fixture_stack_zone(self):
        img = aw.Image(fb.build("fx_alloc.c")["elf"], fb.build("fx_alloc.c")["map"])
        self.assertGreater(sb.stack_zone(img), 0)


class InstallSites(unittest.TestCase):
    def test_scan_and_rows(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "src"))
            with open(os.path.join(d, "src", "a.cpp"), "w") as fh:
                fh.write("// NVIC_SET_PRIORITY(IRQ_X, 1);\n/* attachInterruptVector(\n */\n"
                         "namespace {\nstatic void arm(int x)\n{\n"
                         "\tattachInterruptVector(IRQ_GPT1, isr);\n\tNVIC_SET_PRIORITY(IRQ_GPT1,  0);\n}\n}\n")
            sites = sb.install_sites(d)
            a = ("firmware/src/a.cpp", "arm", "attachInterruptVector(IRQ_GPT1, isr);")
            b = ("firmware/src/a.cpp", "arm", "NVIC_SET_PRIORITY(IRQ_GPT1, 0);")
            self.assertEqual(sites, sorted([a, b]))
            h = sb.Handler("isr", None, 0, aius_sites=[a])
            self.assertEqual(sb.check_install_sites([h], sites, None),
                             [f"stack-bound: interrupt install site {b} is in no handler row"])
            # a line-only move (code above the function grows) keeps the key
            with open(os.path.join(d, "src", "a.cpp"), "r+") as fh:
                text = fh.read()
                fh.seek(0)
                fh.write("int pad1;\nint pad2;\n" + text)
            self.assertEqual(sb.install_sites(d), sorted([a, b]))
            with open(os.path.join(d, "core.c"), "w") as fh:
                fh.write("x\n\tattach(usb_isr);\n")
            ok = sb.Handler("usb_isr", None, 112, framework_cite=("core.c", 2, "usb_isr"))
            moved = sb.Handler("usb_isr", None, 112, framework_cite=("core.c", 1, "usb_isr"))
            self.assertEqual(sb.check_install_sites([ok], [], d), [])
            self.assertEqual(len(sb.check_install_sites([moved], [], d)), 1)

    def test_firmware_tree_sites_are_rows(self):
        from tables_stack import STACK
        sites = sb.install_sites(FW)
        listed = {s for h in STACK.handlers for s in h.aius_sites}
        self.assertEqual(sorted(set(sites) - listed), [])


class OpticalImage(unittest.TestCase):
    def test_total_plus_margin_fits_stack_zone(self):
        from tables_optical import TABLES
        from tables_stack import STACK
        env = os.environ.get("AIUS_STACK_ENV", "teensy41-optical-slot-a")
        d = os.path.join(os.environ.get("AIUS_PIO_BUILD", os.path.join(FW, ".pio", "build")), env)
        elf, mp = os.path.join(d, "firmware.elf"), os.path.join(d, "firmware.map")
        if not os.path.exists(elf):
            self.skipTest(f"{env}: no linked image at {elf}")
        if not os.path.exists(mp):
            self.skipTest(f"{env}: no link map (needs -Wl,-Map in platformio.ini, parent-wiring)")
        su = sb.load_su([d])
        if not su:
            self.skipTest(f"{env}: no .su files (needs -fstack-usage in platformio.ini, parent-wiring)")
        img = aw.Image(elf, mp)
        if not img.has_relocs:
            self.skipTest(f"{env}: no relocations (needs -Wl,--emit-relocs, parent-wiring)")
        s = sb.StackBound(img, TABLES, STACK, su)
        paths = s.sd_paths()
        exc = s.exception_chain()
        zone = sb.stack_zone(img)
        report = [f"{label}: prefix {p} + below {b} + exceptions {exc} + {sb.MARGIN} = "
                  f"{p + b + exc + sb.MARGIN} / {zone} B: {' > '.join(path)}"
                  for (label, p, b, path) in sorted(paths, key=lambda x: -(x[1] + x[2]))]
        print("\n".join(["", "stack-bound (zone = _estack - _ebss - 32 B MPU guard):"] + report))
        fails = sorted(set(s.failures + s.w.failures)) + sb.check_install_sites(
            STACK.handlers, sb.install_sites(FW),
            os.path.expanduser("~/.platformio/packages/framework-arduinoteensy"))
        self.assertEqual(len(paths), len(STACK.chains))
        worst = max(p + b for (_l, p, b, _path) in paths)
        fails += sb.check_margin(worst + exc, zone)
        self.assertEqual(fails, [], "\n".join(report))


if __name__ == "__main__":
    unittest.main()
