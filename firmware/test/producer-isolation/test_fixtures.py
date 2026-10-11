"""W8: producer-isolation walk rules against fixture ELFs the suite links itself.

Each rule has a failing case a narrower walk would pass; the File-shaped
case has a passing case a stricter (reverse-closure) walk would fail.
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import aius_walk as aw  # noqa: E402
import fixture_build as fb  # noqa: E402
import link_half  # noqa: E402

SKIP = fb.available()


def tables(**kw) -> aw.Tables:
    return aw.Tables(**kw)


@unittest.skipIf(SKIP, SKIP or "")
class VeneerFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.img = fb.image("fx_veneer.c", debug=True)
        cls.t = tables(producer_roots=[("fx_flash_root", None)])

    def test_veneer_resolved_to_literal(self):
        ven = self.img.resolve("__fx_itcm_fn_veneer")
        self.assertEqual(len(ven), 1)
        self.assertEqual(self.img.veneer[ven[0]], self.img.resolve("fx_itcm_fn")[0])
        self.assertEqual(self.img.failures, [])

    def test_isolation_fails_naming_the_edge(self):
        fails = aw.check_producer(self.img, self.t)
        self.assertTrue(any("forbidden fx_itcm_fn" in f and "-> yield" in f for f in fails), fails)

    def test_walk_stopping_at_veneer_would_pass(self):
        self.assertEqual(aw.check_producer(self.img, self.t, follow_veneers=False), [])

    def test_unrelocated_data_word_is_not_address_taken(self):
        victim = self.img.resolve("fx_itcm_victim")[0]
        self.assertIn(victim, self.img.value_scan_taken())  # the word is there: a value scan counts it
        self.assertNotIn(victim, self.img.taken)

    def test_debug_frame_relocation_is_not_address_taken(self):
        victim = self.img.resolve("fx_itcm_victim")[0]
        self.assertIn(victim, self.img.nonalloc_abs32_targets)  # the FDE carries an R_ARM_ABS32 to it
        self.assertNotIn(victim, self.img.taken)

    def test_relocated_pointer_is_address_taken(self):
        tgt = self.img.resolve("fx_taken_target")[0]
        self.assertEqual({s for (s, _o) in self.img.taken[tgt]}, {"fx_taken_ptr"})


@unittest.skipIf(SKIP, SKIP or "")
class IndirectFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.img = fb.image("fx_indirect.c")

    def test_site_without_row_fails(self):
        fails = aw.check_producer(self.img, tables(producer_roots=[("fx_ind_root", None)]))
        self.assertEqual(len(fails), 1, fails)
        self.assertIn("1 unlisted indirect site(s) in fx_ind_root", fails[0])

    def test_site_with_row_passes_and_reaches_targets(self):
        row = aw.SiteRow("fx_ind_root", "fx_indirect.c.o", 1, ("taken_by", r"fx_indirect\.c\.o"))
        t = tables(producer_roots=[("fx_ind_root", None)], sites=[row])
        self.assertEqual(aw.check_producer(self.img, t), [])
        w = aw.Walker(self.img, t)
        seen, _ = w.walk(w.roots(t.producer_roots))
        self.assertTrue({self.img.funcs[a].name for a in seen} >= {"fx_tgt_small", "fx_tgt_big"})

    def test_changed_count_fails(self):
        row = aw.SiteRow("fx_ind_root", None, 2, ("taken_by", r"fx_indirect"))
        fails = aw.check_producer(self.img, tables(producer_roots=[("fx_ind_root", None)], sites=[row]))
        self.assertTrue(any("row records 2" in f for f in fails), fails)

    def test_ldr_pc_jump_table_is_not_a_site(self):
        jt = self.img.resolve("fx_jump_table")[0]
        self.assertEqual(self.img.sites[jt], [])
        self.assertEqual([(a, n) for (a, _i, n) in self.img.jump_tables], [(jt, 2)])


@unittest.skipIf(SKIP, SKIP or "")
class AllocatorFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.img = fb.image("fx_alloc.c")

    def test_root_calling_calloc_and_realloc_passes(self):
        self.assertEqual(aw.check_producer(self.img, tables(producer_roots=[("fx_alloc_root", None)])), [])

    def test_without_leaf_stop_yield_is_reached(self):
        t = tables(producer_roots=[("fx_alloc_root", None)])
        w = aw.Walker(self.img, t)
        seen, _ = w.walk(w.roots(t.producer_roots), stop_leaves=False)
        self.assertIn("yield", {self.img.funcs[a].name for a in seen})

    def test_malloc_reaching_hook_fails_w5_2(self):
        fails = aw.check_danger_closure(self.img, tables())
        self.assertEqual(len(fails), 1, fails)
        self.assertIn("aius_sdio_wait_hook under an allocator", fails[0])
        self.assertIn("fx_malloc_helper", fails[0])

    def test_undeclared_cycle_fails(self):
        fails = aw.check_producer(self.img, tables(producer_roots=[("fx_cycle_root", None)]))
        self.assertEqual(len(fails), 1, fails)
        self.assertIn("W6: undeclared cycle of 2", fails[0])

    def test_declared_cycle_passes(self):
        row = aw.RecursionRow(("fx_cyc_a", None), 2, [("fx_cyc_a", None), ("fx_cyc_b", None)], size=2)
        t = tables(producer_roots=[("fx_cycle_root", None)], recursion=[row])
        self.assertEqual(aw.check_producer(self.img, t), [])

    def test_chain_function_outside_component_fails(self):
        row = aw.RecursionRow(("fx_cyc_a", None), 2, [("fx_cyc_a", None), ("fx_exit_fn", None)])
        fails = aw.check_producer(self.img, tables(producer_roots=[("fx_cycle_root", None)], recursion=[row]))
        self.assertTrue(any("fx_exit_fn is outside" in f for f in fails), fails)


@unittest.skipIf(SKIP, SKIP or "")
class FileShapedFixture(unittest.TestCase):
    LIBC = {"_write": 1}

    def test_pinned_libc_row_passes(self):
        img = fb.image("fx_file.c")
        t = tables(libc_row=self.LIBC, vtables={"_ZTV4File": "Print slot"},
                   producer_roots=[("fx_producer_run", None)])
        self.assertEqual(aw.check_danger_closure(img, t), [])
        self.assertEqual(aw.check_producer(img, t), [])

    def test_reverse_closure_would_fail(self):
        img = fb.image("fx_file.c")
        fw = img.resolve("fx_file_write")[0]
        self.assertEqual({s for (s, _o) in img.taken[fw]}, {"_ZTV4File"})
        callers = [a for a in img.funcs if fw in img.edges[a]]
        self.assertEqual(callers, [])  # only an indirect call (in _write) reaches it

    def test_second_unpinned_site_fails(self):
        img = fb.image("fx_file.c", ("FX_SECOND_SITE",))
        fails = aw.check_danger_closure(img, tables(libc_row=self.LIBC))
        self.assertEqual(fails, ["W5 (2): _write has 2 indirect site(s), libc row pins 1"])

    def test_unclassified_vtable_fails(self):
        img = fb.image("fx_file.c")
        fails = aw.check_producer(img, tables(producer_roots=[("fx_producer_run", None)]))
        self.assertEqual(fails, ["W4: vtable _ZTV4File is not classified"])


@unittest.skipIf(SKIP, SKIP or "")
class SdAndYieldRoots(unittest.TestCase):
    def test_hook_reaching_caller_needs_a_row(self):
        img = fb.image("fx_file.c")
        t = tables(sd_stack_name=r"fx_fatfile_write", sd_stack_obj=r"^$")
        fails = aw.check_sd_and_yield_roots(img, t)
        self.assertEqual(fails, ["W5 (3): hook-reaching SD caller fx_file_write (fx_file.c.o) is in no SD-root row"])
        t.sd_unwindowed = [("fx_file_write", "fx_file.c.o")]
        self.assertEqual(aw.check_sd_and_yield_roots(img, t), [])

    def test_yield_root_row_without_yield_point_fails(self):
        # Phase 4h: a yield-root row is a direct caller of the yield point;
        # one naming a function that never calls it fails rather than passing.
        img = fb.image("fx_file.c")
        t = tables(yield_roots=[("fx_file_write", "fx_file.c.o")], sd_stack_obj=r"^$", sd_stack_name=r"^$")
        self.assertEqual(aw.check_sd_and_yield_roots(img, t),
                         [f"W5 (3): yield-root row fx_file_write (fx_file.c.o) does not call {aw.YIELD_POINT}"])

    def test_row_matching_no_symbol_fails(self):
        img = fb.image("fx_file.c")
        t = tables(sd_windowed=[("app_read_directory", None)], sd_stack_obj=r"^$", sd_stack_name=r"^$")
        self.assertEqual(aw.check_sd_and_yield_roots(img, t), ["W2: row app_read_directory matches no symbol"])


@unittest.skipIf(SKIP, SKIP or "")
class LinkHalf(unittest.TestCase):
    def test_map_assigns_objects(self):
        img = fb.image("fx_alloc.c")
        self.assertTrue(img.funcs[img.resolve("fx_cyc_a")[0]].obj.endswith("fx_alloc.c.o"))

    def test_hook_definition_object_and_body(self):
        img = fb.image("fx_alloc.c")
        self.assertEqual(link_half.check_hook_definition(img, "fx_alloc.c.o", None), [])
        fails = link_half.check_hook_definition(img, "optical_main.cpp.o")
        self.assertEqual(len(fails), 2, fails)

    def test_provenance_and_dependency_files(self):
        # Layout as PlatformIO writes it: firmware/.pio/build/<env>, paths in the
        # map and the .d files relative to firmware/. Both archives carry the
        # same names the framework copies would (libSdFat.a, libSD.a): only the
        # member's .d tells a vendored build from a framework one.
        with tempfile.TemporaryDirectory() as fw:
            env = os.path.join(fw, ".pio", "build", "env")
            rel = ".pio/build/env/"

            def put(path, text):
                p = os.path.join(fw, path)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "w") as fh:
                    fh.write(text)
            put(rel + "src/a.cpp.d", "a.cpp.o: src/a.cpp vendor/SD/src/SD.h vendor/SdFat/src/FsLib/FsFile.h\n")
            put(rel + "src/b.cpp.d", "b.cpp.o: src/b.cpp vendor/SdFat/src/FsLib/FsFile.h\n")
            put(rel + "lib1/SdFat/SdCard/SdioTeensy.cpp.d", "x.o: vendor/SdFat/src/SdCard/SdioTeensy.cpp\n")
            put(rel + "lib2/SD/SD.cpp.d",
                "x.o: /h/.platformio/packages/framework-arduinoteensy/libraries/SD/src/SD.cpp\n")
            secs = [aw.MapSection(".text._ZN6SDFile4readEPvj", 0x100, 8, rel + "src/a.cpp.o"),
                    aw.MapSection(".text._ZN10StreamFileI10FsBaseFileyE4peekEv", 0x108, 8, rel + "src/b.cpp.o"),
                    aw.MapSection(".text._ZN6FsFile5flushEv", 0x110, 8, rel + "src/b.cpp.o"),
                    aw.MapSection(".text._ZN8SdioCard10readSectorEmPh", 0x200, 8,
                                  rel + "lib1/libSdFat.a(SdioTeensy.cpp.o)"),
                    aw.MapSection(".text._ZN7SDClass5beginEh", 0x300, 8, rel + "lib2/libSD.a(SD.cpp.o)"),
                    aw.MapSection(".text._ZN7FatFile4readEPvj", 0x400, 8, rel + "src/a.cpp.o")]
            fails = link_half.check_out_of_line(secs, env)
            self.assertEqual(len(fails), 2, fails)
            self.assertIn("_ZN7SDClass5beginEh", fails[0] + fails[1])  # framework-built libSD.a member
            self.assertTrue(any("_ZN7FatFile4readEPvj" in f and "not an archive member" in f for f in fails))
            fails = link_half.check_header_inline(secs, env)
            # lib2's SD.cpp.d names the framework SD; b defines an FsFile COMDAT without the vendored SD.h.
            # StreamFile alone needs only FsFile.h, which b names.
            self.assertEqual(len(fails), 2, fails)
            self.assertTrue(any("SD.cpp.d names the framework" in f for f in fails), fails)
            self.assertTrue(any("b.cpp.d does not name the vendored SD.h" in f for f in fails), fails)
            self.assertEqual(link_half.leading_class("_ZN10StreamFileI10FsBaseFileyE4peekEv"), "StreamFile")


if __name__ == "__main__":
    unittest.main()
