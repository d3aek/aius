#!/usr/bin/env python3
"""Independent tests for the ITCM placement guard's generic LDF gate.

The gate is default-deny over PlatformIO's LDF-resolved libraries, so the thing
worth testing is that it still discriminates: an unknown library emitting ITCM
must fail, an allowlisted one must pass, an expected one that vanished from the
link must fail, and objects outside the LDF shape must be left alone. Driven by
`readelf -SW` and platformio.ini snippets rather than a cross-build, so it runs
in the host suite on a machine with no ARM toolchain.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import re
import sys
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("verify_target.py")
SPEC = importlib.util.spec_from_file_location("aius_verify_itcm", MODULE_PATH)
assert SPEC is not None
assert SPEC.loader is not None
VERIFY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VERIFY
SPEC.loader.exec_module(VERIFY)

FIRMWARE_ROOT = MODULE_PATH.resolve().parents[2]


def sections(*rows: tuple[str, int, str]) -> str:
    """A `readelf -SW` section table carrying the given rows.

    Each row is (name, size, flags); `AXG` is the COMDAT marking. The empty
    `.text` and the `.flashmem` output of an annotated definition are always
    present, exactly as GCC emits them, so a fixture with no rows is a wholly
    cold object rather than an empty file.
    """
    text = (
        "There are 12 section headers, starting at offset 0x7e8:\n"
        "\n"
        "Section Headers:\n"
        "  [Nr] Name              Type            Addr     Off    Size   ES "
        "Flg Lk Inf Al\n"
        "  [ 0]                   NULL            00000000 000000 000000 00 "
        "     0   0  0\n"
        "  [ 1] .text             PROGBITS        00000000 000034 000000 00  "
        "AX  0   0  2\n"
        "  [ 4] .flashmem         PROGBITS        00000000 000034 00031c 00  "
        "AX  0   0  4\n"
    )
    for index, (name, size, flags) in enumerate(rows, start=5):
        text += (
            f"  [{index:2d}] {name} PROGBITS        00000000 000034 "
            f"{size:06x} 00 {flags:>3}  0   0  4\n"
        )
    return text


COLD = sections()
HOT = sections((".text.some_hot_function", 0x58, "AX"))
COMDAT_ONLY = sections((".text._ZN5Print5flushEv", 0x04, "AXG"))
EMPTY_TEXT = sections((".text.gc_dropped", 0, "AX"))


class ParseFixtureTests(unittest.TestCase):
    """The fixtures must parse the way real readelf output does, or every gate
    test below is asserting against nothing."""

    def test_cold_fixture_has_no_text(self) -> None:
        self.assertEqual(VERIFY.parse_text_sections(COLD), {})

    def test_hot_fixture_parses_size_and_comdat(self) -> None:
        self.assertEqual(
            VERIFY.parse_text_sections(HOT),
            {".text.some_hot_function": (0x58, False)},
        )
        self.assertEqual(
            VERIFY.parse_text_sections(COMDAT_ONLY),
            {".text._ZN5Print5flushEv": (0x04, True)},
        )

    def test_zero_size_section_is_not_text(self) -> None:
        self.assertEqual(VERIFY.parse_text_sections(EMPTY_TEXT), {})


class GroupingTests(unittest.TestCase):
    def test_groups_ldf_objects_by_library_name(self) -> None:
        self.assertEqual(
            VERIFY.group_ldf_objects([
                "lib547/aius_dsp_runtime_config/aius_dsp_runtime_config.c.o",
                "lib1a3/aius_dsp/aius_measure.c.o",
                "lib1a3/aius_dsp/aius_dsp.c.o",
                "lib2d0/FNET/stack/fnet_ip4.c.o",
            ]),
            {
                "FNET": ["lib2d0/FNET/stack/fnet_ip4.c.o"],
                "aius_dsp": [
                    "lib1a3/aius_dsp/aius_dsp.c.o",
                    "lib1a3/aius_dsp/aius_measure.c.o",
                ],
                "aius_dsp_runtime_config": [
                    "lib547/aius_dsp_runtime_config/"
                    "aius_dsp_runtime_config.c.o",
                ],
            },
        )

    def test_ignores_every_non_library_shape(self) -> None:
        """The src tree, the build_src_filter copies of lib/, the vendored core
        and the framework archive are other checks' business."""
        self.assertEqual(
            VERIFY.group_ldf_objects([
                "src/optical_main.cpp.o",
                "lib/aius_clock/src/aius_clock.c.o",
                "vendor/dcadec-core/core_decoder.c.o",
                "FrameworkArduino/AudioStream.cpp.o",
                "lib547/aius_dsp_runtime_config/aius_dsp_runtime_config.c.a",
            ]),
            {},
        )


class LdfGateTests(unittest.TestCase):
    """Every scenario runs against a two-library synthetic link so the tables
    under test are the ones the assertion names, not the real build's."""

    PATHS = (
        "lib111/known_cold/known_cold.c.o",
        "lib222/newcomer/newcomer.c.o",
    )

    def verify(self, paths=None, texts=None, expected=("known_cold",),
               allowed=()):
        texts = texts or {}
        with mock.patch.object(
            VERIFY, "EXPECTED_LDF_LIBS", frozenset(expected)
        ), mock.patch.object(
            VERIFY, "ITCM_ALLOWED_LDF_LIBS", dict.fromkeys(allowed, "test")
        ):
            return VERIFY.verify_ldf_libraries(
                self.PATHS if paths is None else paths,
                lambda path: texts.get(path, COLD),
            )

    def test_unknown_library_emitting_itcm_fails(self) -> None:
        with self.assertRaisesRegex(
            VERIFY.VerificationError, r"LDF libraries emit ITCM code"
        ) as caught:
            self.verify(texts={"lib222/newcomer/newcomer.c.o": HOT})
        message = str(caught.exception)
        self.assertIn("lib222/newcomer/newcomer.c.o", message)
        self.assertIn(".text.some_hot_function", message)
        self.assertIn("88 bytes", message)

    def test_allowlisted_library_may_emit_itcm(self) -> None:
        libraries, gated = self.verify(
            texts={"lib222/newcomer/newcomer.c.o": HOT},
            allowed=("newcomer",),
        )
        self.assertEqual((libraries, gated), (2, 1))

    def test_gated_library_stays_green_while_cold(self) -> None:
        self.assertEqual(self.verify(), (2, 2))

    def test_second_source_in_a_gated_library_is_covered(self) -> None:
        """The hole the bespoke single-object check left: one file per library
        was inspected, so a sibling could leak."""
        paths = self.PATHS + ("lib111/known_cold/second.c.o",)
        with self.assertRaisesRegex(
            VERIFY.VerificationError, r"lib111/known_cold/second\.c\.o"
        ):
            self.verify(paths=paths, texts={"lib111/known_cold/second.c.o": HOT})

    def test_comdat_is_an_offender_in_a_gated_library(self) -> None:
        """COMDAT is excluded for COLD_OBJECTS because it is a copy of a
        framework header's member, not of the annotated file. A library nobody
        has reviewed has no such claim, so the gate counts it."""
        with self.assertRaisesRegex(
            VERIFY.VerificationError, r"_ZN5Print5flushEv"
        ):
            self.verify(texts={"lib222/newcomer/newcomer.c.o": COMDAT_ONLY})

    def test_expected_library_absent_from_the_link_fails(self) -> None:
        with self.assertRaisesRegex(
            VERIFY.VerificationError, r"expected LDF libraries are absent"
        ) as caught:
            self.verify(expected=("known_cold", "aius_recorder"))
        self.assertIn("aius_recorder", str(caught.exception))

    def test_a_link_carrying_the_retired_replay_library_fails(self) -> None:
        """Phase 4i: aius_replay left every optical env; a link that still
        carries it fails even when its objects are cold."""
        self.assertNotIn("aius_replay", VERIFY.EXPECTED_LDF_LIBS)
        self.assertNotIn("aius_replay", VERIFY.ITCM_ALLOWED_LDF_LIBS)
        paths = self.PATHS + ("lib333/aius_replay/aius_replay.c.o",)
        with self.assertRaisesRegex(
            VERIFY.VerificationError, r"retired LDF libraries are in the link"
        ) as caught:
            self.verify(paths=paths)
        self.assertIn("aius_replay", str(caught.exception))

    def test_a_symbol_naming_replay_fails(self) -> None:
        text = (
            "Symbol table '.symtab' contains 3 entries:\n"
            "   Num:    Value  Size Type    Bind   Vis      Ndx Name\n"
            "     1: 60001000    40 FUNC    LOCAL  DEFAULT    2 setup\n"
            "     2: 20200000   128 OBJECT  LOCAL  DEFAULT    5 g_replay_service\n"
        )
        names = VERIFY.parse_symbol_names(text)
        self.assertEqual(names, ["setup", "g_replay_service"])
        with self.assertRaisesRegex(
            VERIFY.VerificationError, r"g_replay_service"
        ):
            VERIFY.verify_no_retired_symbols(names)
        self.assertEqual(VERIFY.verify_no_retired_symbols(["setup"]), 1)

    def test_non_library_objects_are_not_gated(self) -> None:
        """An ITCM-emitting object outside the LDF shape is the cold-object and
        hot-object checks' subject; this gate must not double-judge it."""
        paths = self.PATHS + ("src/optical_main.cpp.o",)
        libraries, gated = self.verify(
            paths=paths, texts={"src/optical_main.cpp.o": HOT}
        )
        self.assertEqual((libraries, gated), (2, 2))


class ProducerYieldPinTests(unittest.TestCase):
    """Phase 4e: the producer-yield object keeps only its pinned set."""

    FULL = {
        ".text.aius_producer_yield_guard": 72,
        ".text.aius_producer_yield_wait": 120,
        ".text.aius_producer_yield_point": 92,
        ".text.aius_producer_yield_visit_begin": 16,
        ".text.aius_producer_yield_visit_end": 40,
        ".text.nested_pass.constprop.0": 104,
    }

    def test_the_built_shape_passes_with_its_bytes(self) -> None:
        self.assertEqual(
            VERIFY.verify_producer_yield_sections(self.FULL), 444
        )

    def test_an_inlined_nested_pass_still_passes(self) -> None:
        rows = dict(self.FULL)
        del rows[".text.nested_pass.constprop.0"]
        self.assertEqual(VERIFY.verify_producer_yield_sections(rows), 340)

    def test_an_unpinned_function_fails_and_is_named(self) -> None:
        rows = dict(self.FULL)
        rows[".text.aius_producer_yield_configure"] = 40
        with self.assertRaisesRegex(
            VERIFY.VerificationError, "aius_producer_yield_configure"
        ):
            VERIFY.verify_producer_yield_sections(rows)

    def test_a_missing_entry_point_fails(self) -> None:
        rows = dict(self.FULL)
        del rows[".text.aius_producer_yield_wait"]
        with self.assertRaisesRegex(
            VERIFY.VerificationError, "aius_producer_yield_wait"
        ):
            VERIFY.verify_producer_yield_sections(rows)

    def test_the_object_parses_from_readelf(self) -> None:
        text = sections(
            *((name, size, "AX") for name, size in self.FULL.items())
        )
        parsed, comdat, clones = VERIFY.parse_itcm_sections(text)
        self.assertEqual((comdat, clones), (0, 0))
        self.assertEqual(VERIFY.verify_producer_yield_sections(parsed), 444)


class RealTableTests(unittest.TestCase):
    """The shipped tables, checked for the internal consistency no synthetic
    scenario can speak for."""

    def test_allowlist_is_a_subset_of_the_expected_set(self) -> None:
        self.assertLessEqual(
            set(VERIFY.ITCM_ALLOWED_LDF_LIBS), VERIFY.EXPECTED_LDF_LIBS
        )

    def test_every_allowlist_entry_carries_a_reason(self) -> None:
        for name, reason in VERIFY.ITCM_ALLOWED_LDF_LIBS.items():
            with self.subTest(library=name):
                self.assertTrue(reason.strip(), f"{name} has no justification")

    def test_the_dsp_runtime_adapter_is_gated_not_allowlisted(self) -> None:
        """The regression that motivated the guard: 30eeed071's binder defaulted
        into ITCM. The bespoke check that caught it was folded into the gate, so
        the gate is now the only thing holding that contract."""
        self.assertIn("aius_dsp_runtime_config", VERIFY.EXPECTED_LDF_LIBS)
        self.assertNotIn(
            "aius_dsp_runtime_config", VERIFY.ITCM_ALLOWED_LDF_LIBS
        )

    def test_the_web_cache_is_gated_not_allowlisted(self) -> None:
        """Phase 4g: the PSRAM web cache runs from the HTTP send path, never
        per audio block, so the gate holds it to zero `.text`."""
        self.assertIn("aius_web_cache", VERIFY.EXPECTED_LDF_LIBS)
        self.assertNotIn("aius_web_cache", VERIFY.ITCM_ALLOWED_LDF_LIBS)

    def test_the_ab_and_sd_overlay_libraries_are_gated_not_allowlisted(self) -> None:
        """Phase 4h (N4): both libraries are expected in the link and held to
        zero `.text`, and a link missing either fails against the real table."""
        for library in ("aius_ab", "aius_sd_overlay"):
            with self.subTest(library=library):
                self.assertIn(library, VERIFY.EXPECTED_LDF_LIBS)
                self.assertNotIn(library, VERIFY.ITCM_ALLOWED_LDF_LIBS)
                paths = [
                    f"lib{index:03x}/{name}/{name}.c.o"
                    for index, name in enumerate(
                        sorted(VERIFY.EXPECTED_LDF_LIBS - {library}), start=1
                    )
                ]
                with self.assertRaisesRegex(
                    VERIFY.VerificationError, r"expected LDF libraries are absent"
                ) as caught:
                    VERIFY.verify_ldf_libraries(paths, lambda path: COLD)
                self.assertIn(library, str(caught.exception))


GOOD_INI = """; Fake project.
[common]
platform = teensy@5.2.0
build_flags =
    -O2
    ; A rationale block that happens to mention -flto in prose.
    -DFNET_CFG_TLS=0

[env:plain]
extends = common

[env:derived]
extends = env:plain
build_flags =
    ${common.build_flags}
    -DAIUS_INSTRUMENT=1
"""


class LtoTests(unittest.TestCase):
    def flags(self, text: str, environment: str) -> list[str]:
        return VERIFY.environment_build_flags(
            VERIFY.parse_ini_sections(text), environment
        )

    def test_folds_continuations_and_inherits_through_extends(self) -> None:
        self.assertEqual(
            self.flags(GOOD_INI, "plain"), ["-O2", "-DFNET_CFG_TLS=0"]
        )

    def test_expands_interpolation_and_walks_a_two_step_chain(self) -> None:
        self.assertEqual(
            self.flags(GOOD_INI, "derived"),
            ["-O2", "-DFNET_CFG_TLS=0", "-DAIUS_INSTRUMENT=1",
             "-O2", "-DFNET_CFG_TLS=0"],
        )

    def test_comment_prose_is_not_read_as_a_flag(self) -> None:
        self.assertEqual(VERIFY.verify_no_lto(GOOD_INI, "plain"), 2)

    def test_rejects_lto_in_the_environment(self) -> None:
        text = GOOD_INI.replace(
            "    -DAIUS_INSTRUMENT=1", "    -DAIUS_INSTRUMENT=1\n    -flto"
        )
        with self.assertRaisesRegex(
            VERIFY.VerificationError, r"one object file at a time"
        ):
            VERIFY.verify_no_lto(text, "derived")

    def test_rejects_lto_inherited_from_a_parent_section(self) -> None:
        text = GOOD_INI.replace("    -O2", "    -O2\n    -flto=auto")
        for environment in ("plain", "derived"):
            with self.subTest(environment=environment):
                with self.assertRaisesRegex(
                    VERIFY.VerificationError, r"-flto=auto"
                ):
                    VERIFY.verify_no_lto(text, environment)

    def test_rejects_an_unknown_environment(self) -> None:
        with self.assertRaisesRegex(
            VERIFY.VerificationError, r"no \[env:ghost\] section"
        ):
            VERIFY.verify_no_lto(GOOD_INI, "ghost")

    def test_rejects_a_dangling_extends(self) -> None:
        text = GOOD_INI.replace("extends = common", "extends = absent")
        with self.assertRaisesRegex(
            VERIFY.VerificationError, r"which does not exist"
        ):
            VERIFY.verify_no_lto(text, "plain")

    def test_the_shipped_optical_environments_are_lto_free(self) -> None:
        """Not redundant with the build hook: this runs in the host suite, so
        the flag is rejected before anyone waits on a cross-build."""
        ini = VERIFY.read_platformio_ini(FIRMWARE_ROOT)
        for environment in ("teensy41-optical", "teensy41-optical-fault"):
            with self.subTest(environment=environment):
                self.assertGreater(VERIFY.verify_no_lto(ini, environment), 0)


def symbol_table(*rows: tuple[str, int]) -> dict[str, int]:
    """A parsed symbol table holding every dark-window symbol in ITCM, with
    the given (name, address) rows overriding or extending it. C++ statics
    appear in their real _ZL mangling; the two C symbols stay plain -- the
    same mix `readelf -sW` on the linked image produces."""
    table: dict[str, int] = {}
    address = 0x100
    for name in VERIFY.DARK_WINDOW_SYMBOLS:
        if name.startswith("aius_"):
            table[name] = address
        else:
            table[f"_ZL{len(name)}{name}Pv"] = address
        address += 0x40
    for name, value in rows:
        table[name] = value
    return table


class DarkWindowTests(unittest.TestCase):
    """The linked-image residency gate: a dark-window function that was
    inlined away (no symbol) or landed outside ITCM must fail, because both
    spell 'the dark window executes from wherever the caller lives' -- the
    2026-08-01 incident's mechanism."""

    def test_parses_func_symbols_only(self) -> None:
        output = (
            "Symbol table '.symtab' contains 4 entries:\n"
            "   Num:    Value  Size Type    Bind   Vis      Ndx Name\n"
            "     1: 00000239   200 FUNC    LOCAL  DEFAULT    1 "
            "_ZL11ota_quiesceP14OtaIrqSnapshot\n"
            "     2: 20046c38    64 OBJECT  LOCAL  DEFAULT    4 "
            "_ZL22g_ota_rehearsal_health\n"
            "     3: 00000169    76 FUNC    GLOBAL DEFAULT    1 "
            "aius_ota_activate_check_erase_in\n"
        )
        parsed = VERIFY.parse_function_symbols(output)
        self.assertEqual(
            parsed,
            {
                "_ZL11ota_quiesceP14OtaIrqSnapshot": 0x239,
                "aius_ota_activate_check_erase_in": 0x169,
            },
        )

    def test_a_complete_itcm_table_passes(self) -> None:
        self.assertEqual(
            VERIFY.verify_dark_window_symbols(symbol_table()),
            len(VERIFY.DARK_WINDOW_SYMBOLS),
        )

    def test_an_inlined_run_loop_fails_as_missing(self) -> None:
        table = symbol_table()
        del table["_ZL29ota_activate_run_and_finalizePv"]
        with self.assertRaisesRegex(
            VERIFY.VerificationError, r"inlined away.*run_and_finalize"
        ):
            VERIFY.verify_dark_window_symbols(table)

    def test_a_flash_resident_symbol_fails_as_misplaced(self) -> None:
        # Same key the fixture generates, so the row OVERRIDES the ITCM entry
        # with a flash address -- the folded-into-loop() shape that shipped.
        table = symbol_table(("_ZL16ota_rehearse_runPv", 0x60017000))
        with self.assertRaisesRegex(
            VERIFY.VerificationError, r"outside ITCM.*0x60017000"
        ):
            VERIFY.verify_dark_window_symbols(table)

    def test_the_length_prefix_is_exact_name_matching(self) -> None:
        """The inject wrapper's symbol must NOT satisfy ota_flash_read's
        entry: _ZL14 cannot match a 32-character name."""
        table = symbol_table()
        del table["_ZL14ota_flash_readPv"]
        with self.assertRaisesRegex(
            VERIFY.VerificationError,
            r"inlined away(?!.*inject).*ota_flash_read",
        ):
            VERIFY.verify_dark_window_symbols(table)

    def test_dtcm_counts_as_outside_itcm(self) -> None:
        table = symbol_table(("_ZL12ota_wdt_feedPv", 0x20001000))
        with self.assertRaisesRegex(
            VERIFY.VerificationError, r"outside ITCM.*ota_wdt_feed"
        ):
            VERIFY.verify_dark_window_symbols(table)


ETEXT = 0x2000


def disassembly(**bodies: tuple[int, list[str]]) -> str:
    """An `objdump -d --no-show-raw-insn` listing: one block per symbol,
    each (start address, instruction lines "addr:\\tmnemonic\\toperands")."""
    text = (
        "\nfirmware.elf:     file format elf32-littlearm\n\n\n"
        "Disassembly of section .text.itcm:\n"
    )
    for name, (start, lines) in bodies.items():
        text += f"\n{start:08x} <{name}>:\n"
        for line in lines:
            text += f"     {line}\n"
    return text


def clean_bodies() -> dict[str, tuple[int, list[str]]]:
    """The four named roots plus one primitive, all ITCM; ota_flash_read
    reaches flash_wait only by bl, so the walk must follow it (N1)."""
    return {
        "aius_ota_activate_step": (0x100, [
            "100:\tpush\t{r4, lr}",
            "102:\tcbz\tr0, 10a <aius_ota_activate_step+0xa>",
            "104:\tbl\t400 <aius_ota_activate_check_span_in>",
            "108:\tbeq.n\t10a <aius_ota_activate_step+0xa>",
            "10a:\tblx\tr3",
            "10c:\tpop\t{r4, pc}",
            "110:\t.word\t0x20004000",
        ]),
        "_ZL14ota_flash_readPvmPhm": (0x200, [
            "200:\tpush\t{r3, lr}",
            "202:\tldr\tr3, [pc, #8]\t; (20c <_ZL14ota_flash_readPvmPhm+0xc>)",
            "204:\tbl\t500 <flash_wait>",
            "208:\tb.w\t600 <memcpy>",
            "20c:\t.word\t0xe000ef5c",
            "210:\t.word\t0x402a8000",
        ]),
        "_ZL17ota_flash_programPvmPKhm": (0x300, ["300:\tbx\tlr"]),
        "_ZL15ota_flash_erasePvm": (0x340, ["340:\tbx\tlr"]),
        "_ZL12ota_wdt_feedPv": (0x380, ["380:\tbx\tlr"]),
        "aius_ota_activate_check_span_in": (0x400, ["400:\tbx\tlr"]),
        "flash_wait": (0x500, ["500:\tbx\tlr", "504:\t.word\t0x402a8000"]),
        "memcpy": (0x600, ["600:\tbx\tlr"]),
        "__aius_ab_remap_for_slot_veneer": (0x1f00, [
            "1f00:\tldr.w\tpc, [pc]\t; 1f04 <__aius_ab_remap_for_slot_veneer+0x4>",
            "1f04:\t.word\t0x60011b61",
        ]),
    }


CLOSURE_ROOTS = (*VERIFY.STEP_LOOP_ROOTS, "ota_wdt_feed")


class StepLoopClosureTests(unittest.TestCase):
    """aius-sample-rate-following.md Phase 4z: nothing the activation step
    loop reaches may fetch flash, by branch or by literal."""

    def walk(self, bodies):
        return VERIFY.verify_step_loop_closure(
            VERIFY.parse_disassembly(disassembly(**bodies)), ETEXT,
            CLOSURE_ROOTS,
        )

    def test_a_clean_closure_passes_and_walks_bl_only_callees(self) -> None:
        functions, branches, words = self.walk(clean_bodies())
        # 5 roots + check_span_in, flash_wait, memcpy; the veneer is unreached.
        self.assertEqual(functions, 8)
        # cbz, bl, beq.n, bl, b.w; `blx r3` is indirect and uncounted.
        self.assertEqual(branches, 5)
        self.assertEqual(words, 4)

    def test_a_planted_veneer_call_fails(self) -> None:
        bodies = clean_bodies()
        start, lines = bodies["_ZL14ota_flash_readPvmPhm"]
        lines.insert(1, "201:\tbl\t1f00 <__aius_ab_remap_for_slot_veneer>")
        with self.assertRaisesRegex(
            VERIFY.VerificationError,
            r"ota_flash_readPvmPhm\+0x1 branches to veneer "
            r"<__aius_ab_remap_for_slot_veneer>",
        ):
            self.walk(bodies)

    def test_a_planted_flash_literal_in_a_callee_fails(self) -> None:
        bodies = clean_bodies()
        bodies["flash_wait"][1].append("508:\t.word\t0x60011b8c")
        with self.assertRaisesRegex(
            VERIFY.VerificationError,
            r"flash_wait\+0x8 literal 0x60011b8c names flash",
        ):
            self.walk(bodies)

    def test_the_flash_range_edges_are_inclusive(self) -> None:
        for value, fails in (
            (0x5FFFFFFF, False), (0x60000000, True),
            (0x60FFFFFF, True), (0x61000000, False),
        ):
            bodies = clean_bodies()
            bodies["memcpy"][1].append(f"604:\t.word\t0x{value:08x}")
            with self.subTest(value=hex(value)):
                if fails:
                    with self.assertRaises(VERIFY.VerificationError):
                        self.walk(bodies)
                else:
                    self.walk(bodies)

    def test_a_tail_call_into_flash_fails(self) -> None:
        bodies = clean_bodies()
        bodies["_ZL12ota_wdt_feedPv"] = (0x380, [
            "380:\tb.w\t60011b60 <aius_ab_remap_for_slot>",
        ])
        with self.assertRaisesRegex(
            VERIFY.VerificationError,
            r"ota_wdt_feedPv\+0x0 branches to 0x60011b60 "
            r"<aius_ab_remap_for_slot> outside ITCM",
        ):
            self.walk(bodies)

    def test_a_conditional_tail_call_past_etext_fails(self) -> None:
        bodies = clean_bodies()
        bodies["aius_ota_activate_check_span_in"] = (0x400, [
            "400:\tbne.w\t2400 <past_etext>",
            "404:\tbx\tlr",
        ])
        with self.assertRaisesRegex(
            VERIFY.VerificationError, r"0x00002400 <past_etext> outside ITCM"
        ):
            self.walk(bodies)

    def test_an_absent_root_fails(self) -> None:
        bodies = clean_bodies()
        del bodies["_ZL15ota_flash_erasePvm"]
        with self.assertRaisesRegex(
            VERIFY.VerificationError, r"root absent.*ota_flash_erase"
        ):
            self.walk(bodies)

    def test_the_primitives_table_roots_are_parsed(self) -> None:
        source = (
            "\tprimitives.ctx = nullptr;\n"
            "\tprimitives.erase_sector = ota_flash_erase;\n"
            "\tprimitives.ctx = &g_ota_rehearsal_window;\n"
            "\t\tprimitives.flash_read = ota_flash_read_inject_head_fault;\n"
            "\tprimitives.erase_sector = ota_flash_erase;\n"
        )
        self.assertEqual(
            VERIFY.primitive_roots(source),
            ["ota_flash_erase", "ota_flash_read_inject_head_fault"],
        )

    def test_the_live_primitives_tables_name_every_primitive(self) -> None:
        """N2: both tables (activation and rehearsal) are roots."""
        roots = VERIFY.primitive_roots(
            (FIRMWARE_ROOT / VERIFY.PRIMITIVES_SOURCE).read_text(
                encoding="utf-8"
            )
        )
        for name in (
            "ota_flash_read", "ota_flash_read_inject_head_fault",
            "ota_wdt_feed", "ota_dark_stamp_write",
            "ota_rehearsal_dark_stamp_write", "ota_rehearsal_health_write",
            "ota_flash_cycles", "ota_health_read", "ota_health_write",
            "ota_rehearsal_health_read",
        ):
            self.assertIn(name, roots)
        self.assertNotIn("nullptr", roots)

    def test_a_ternary_right_hand_side_is_rejected(self) -> None:
        with self.assertRaisesRegex(
            VERIFY.VerificationError,
            r"optical_main\.cpp:2:.*primitives\.flash_read.*not a bare",
        ):
            VERIFY.primitive_roots(
                "\tprimitives.cycles = ota_flash_cycles;\n"
                "\tprimitives.flash_read = inject ? a : b;\n"
            )

    def test_an_address_of_right_hand_side_is_accepted(self) -> None:
        self.assertEqual(
            VERIFY.primitive_roots("\tprimitives.cycles = &ota_fn;\n"),
            ["ota_fn"],
        )

    def test_an_unassigned_function_pointer_field_is_rejected(self) -> None:
        header = (
            "typedef int (*FooFn)(void *ctx);\n"
            "typedef struct {\n\tvoid *ctx;\n\tFooFn a; /* x */\n"
            "\tFooFn b;\n} AiusOtaActivatePrimitives;\n"
        )
        self.assertEqual(VERIFY.primitive_function_fields(header), ["a", "b"])
        self.assertEqual(
            VERIFY.verify_primitive_fields_assigned(
                "primitives.a = x;\nprimitives.b = &y;\n", header
            ),
            2,
        )
        with self.assertRaisesRegex(
            VERIFY.VerificationError, r"never assigned.*: b\b"
        ):
            VERIFY.verify_primitive_fields_assigned(
                "primitives.ctx = nullptr;\nprimitives.a = x;\n", header
            )

    def test_the_live_header_fields_are_all_assigned(self) -> None:
        count = VERIFY.verify_primitive_fields_assigned(
            (FIRMWARE_ROOT / VERIFY.PRIMITIVES_SOURCE).read_text(
                encoding="utf-8"
            ),
            (FIRMWARE_ROOT / VERIFY.PRIMITIVES_HEADER).read_text(
                encoding="utf-8"
            ),
        )
        self.assertEqual(count, 8)

    def test_the_remap_must_sit_in_dtcm(self) -> None:
        self.assertEqual(
            VERIFY.verify_remap_in_dtcm(
                "00012000 A _etext\n20001a40 b _ZL15g_ab_read_remap\n"
            ),
            0x20001A40,
        )
        with self.assertRaisesRegex(VERIFY.VerificationError, r"not DTCM"):
            VERIFY.verify_remap_in_dtcm("60012000 r _ZL15g_ab_read_remap\n")
        with self.assertRaisesRegex(VERIFY.VerificationError, r"absent"):
            VERIFY.verify_remap_in_dtcm("20001a40 b g_ab_record\n")

    def test_the_summary_names_the_remap_address(self) -> None:
        line = VERIFY._step_loop_summary(8, 6, 4, 0x20001A40)
        self.assertIn("8 functions, 6 direct branches", line)
        self.assertIn("g_ab_read_remap in DTCM at 0x20001a40", line)


HELPER = "_ZL31ota_dcache_clean_invalidate_allv"
READER = "_ZL14ota_flash_readPvmPhm"
# `objdump -d --no-show-raw-insn` of the Phase 4za build-1 slot-a link,
# unabridged: the helper [0x6b0, 0x6dc) and ota_flash_read [0x848, 0x968).
# Regenerate from a new slot-a ELF with those two --start/--stop-address
# runs when the code moves; the planted negatives name these addresses.
RESTORE_FIXTURE = MODULE_PATH.with_name("fixtures") / "slot-a-identity-restore.objdump"
LINE_ADDRESS = re.compile(r"^\s+([0-9a-f]+):")


def listing() -> list[str]:
    return RESTORE_FIXTURE.read_text(encoding="utf-8").splitlines()


def address_of(line: str):
    match = LINE_ADDRESS.match(line)
    return int(match.group(1), 16) if match else None


def dropped(lines: list[str], *addresses: int) -> list[str]:
    kept = [line for line in lines if address_of(line) not in addresses]
    assert len(kept) == len(lines) - len(addresses), addresses
    return kept


def replaced(lines: list[str], address: int, old: str, new: str) -> list[str]:
    out = [
        line.replace(old, new) if address_of(line) == address else line
        for line in lines
    ]
    assert out != lines, (address, old)
    return out


def inserted(lines: list[str], address: int, text: str) -> list[str]:
    """Plant `text` ("mnemonic\\toperands") at `address`, right after the
    nearest earlier instruction of the same function."""
    index = max(
        i for i, line in enumerate(lines)
        if (a := address_of(line)) is not None and address - 0x80 < a < address
    )
    return lines[: index + 1] + [f"     {address:x}:\t{text}"] + lines[index + 1:]


class IdentityRestoreTests(unittest.TestCase):
    """aius-sample-rate-following.md Phase 4za (D2), on the real slot-a
    listing: every planted regression fails, the built shape passes."""

    def check(self, lines):
        return VERIFY.verify_identity_restore(
            VERIFY.parse_disassembly("\n".join(lines))
        )

    def assertFails(self, lines, pattern: str) -> None:
        with self.assertRaisesRegex(VERIFY.VerificationError, pattern):
            self.check(lines)

    def test_the_built_shape_passes(self) -> None:
        self.assertEqual(self.check(listing()), (1, 0xc6, 0xee))

    def test_base_plus_offset_stores_resolve(self) -> None:
        events = VERIFY.trace_memory_accesses(
            VERIFY.parse_disassembly("\n".join(listing()))[READER]
        )
        self.assertEqual(
            [v for kind, _, v in events if kind == "store" and v is not None],
            [0xE000EF5C, 0x400AC080, 0x402A8000, 0xE000EF5C, 0x400AC080,
             0x402A8000, 0xE000EF50],
        )

    def test_movw_movt_addressing_resolves(self) -> None:
        body = VERIFY.parse_disassembly(
            f"\n00000100 <x>:\n"
            "     100:\tmovw\tr0, #61300\t@ 0xef74\n"
            "     104:\tmovt\tr0, #57344\t@ 0xe000\n"
            "     108:\tstr\tr2, [r0, #0]\n"
        )["x"]
        self.assertEqual(
            VERIFY.trace_memory_accesses(body)[0], ("store", 0x108, 0xE000EF74)
        )

    # S1: two nested loops, each closed by a branch fed by its own cmp.
    def test_planted_negative_inner_bne_dropped(self) -> None:
        self.assertFails(
            dropped(listing(), 0x6cc), r"inside 1 backward branch"
        )

    def test_planted_negative_outer_bne_dropped(self) -> None:
        self.assertFails(
            dropped(listing(), 0x6d6), r"inside 1 backward branch"
        )

    def test_planted_negative_128_set_bound(self) -> None:
        self.assertFails(
            replaced(listing(), 0x6c4, "#8192", "#4096"),
            r"\+0x1c closes the inner \(set\) loop.*fed by cmp #4096, not "
            r"cmp #8192",
        )

    def test_planted_negative_2_way_bound(self) -> None:
        self.assertFails(
            replaced(listing(), 0x6d2, "#4", "#2"),
            r"\+0x26 closes the outer \(way\) loop.*fed by cmp #2, not cmp #4",
        )

    def test_a_flag_setter_after_the_cmp_unfeeds_the_branch(self) -> None:
        lines = inserted(listing(), 0x6ca, "adds\tr1, #0")
        self.assertFails(lines, r"inner \(set\) loop.*fed by no cmp")

    def test_planted_negative_store_outside_the_loop(self) -> None:
        lines = replaced(
            dropped(listing(), 0x6c8), 0x6d8, "bx\tlr",
            "str.w\tr2, [r0, #3956]\t@ 0xf74",
        )
        self.assertFails(
            inserted(lines, 0x6dc, "bx\tlr"), r"inside 0 backward branch"
        )

    # N2: the way counter reaches DCCISW through lsl #30.
    def test_planted_negative_way_shift_29(self) -> None:
        self.assertFails(
            replaced(listing(), 0x6b8, "lsl #30", "lsl #29"),
            r"DCCISW value r2 is not \(set counter r3\) \| \(way counter r12 "
            r"lsl #30\)",
        )

    # S5 and N1: the helper is unconditional and straight-line.
    def test_planted_negative_forward_branch_in_the_helper(self) -> None:
        self.assertFails(
            inserted(listing(), 0x6c6, f"beq.n\t6cc <{HELPER}+0x1c>"),
            r"\+0x16 branches forward to 0x6cc",
        )

    def test_planted_negative_predicated_dccisw(self) -> None:
        self.assertFails(
            replaced(listing(), 0x6c8, "str.w", "strne.w"),
            r"\+0x18 is an IT block or a predicated instruction \(strne\.w\)",
        )

    def test_an_it_block_in_the_helper_fails(self) -> None:
        lines = inserted(listing(), 0x6c6, "it\tne")
        self.assertFails(
            replaced(lines, 0x6c8, "str.w", "strne.w"),
            r"\+0x16 is an IT block or a predicated instruction \(it\)",
        )

    def test_planted_negative_predicated_iciallu(self) -> None:
        lines = inserted(listing(), 0x943, "it\tne")
        self.assertFails(
            replaced(lines, 0x944, "str.w", "strne.w"),
            r"\+0xfb is an IT block or a predicated instruction \(it\) inside "
            r"the restore sequence",
        )

    def test_a_suffix_predicated_iciallu_fails(self) -> None:
        self.assertFails(
            replaced(listing(), 0x944, "str.w", "strne.w"),
            r"\+0xfc is an IT block or a predicated instruction \(strne\.w\)",
        )

    # S2: straight-line from the GPR32 restore store to the PRIMASK restore.
    def test_planted_negative_beq_910_to_91a(self) -> None:
        self.assertFails(
            inserted(listing(), 0x910, f"beq.n\t91a <{READER}+0xd2>"),
            r"\+0xc8 branches to 0x91a, forward inside the restore sequence",
        )

    def test_planted_negative_bne_918_to_950(self) -> None:
        self.assertFails(
            inserted(listing(), 0x918, f"bne.n\t950 <{READER}+0x108>"),
            r"\+0xd0 branches to 0x950, forward inside",
        )

    def test_planted_negative_beq_90d_to_950(self) -> None:
        self.assertFails(
            inserted(listing(), 0x90d, f"beq.n\t950 <{READER}+0x108>"),
            r"\+0xc5 branches to 0x950, into the restore sequence "
            r"\(\+0xc6 to \+0x108\)",
        )

    def test_planted_negative_cbz_around_the_call(self) -> None:
        self.assertFails(
            inserted(listing(), 0x934, f"cbz\tr3, 93a <{READER}+0xf2>"),
            r"\+0xec branches to 0x93a, forward inside",
        )

    # S3: dsb then isb between the SWRESET and the call.
    def test_planted_negative_pre_call_dsb_dropped(self) -> None:
        self.assertFails(
            dropped(listing(), 0x92e),
            r"no dsb then isb between the SWRESET at \+0xda and the .* call "
            r"at \+0xee",
        )

    def test_planted_negative_pre_call_isb_dropped(self) -> None:
        self.assertFails(dropped(listing(), 0x932), r"no dsb then isb")

    def test_planted_negative_pre_call_barriers_dropped(self) -> None:
        self.assertFails(dropped(listing(), 0x92e, 0x932), r"no dsb then isb")

    # S4: the SWRESET poll.
    def test_planted_negative_poll_removed(self) -> None:
        self.assertFails(
            dropped(listing(), 0x924, 0x926, 0x92a),
            r"no backward MCR0-load poll loop between the SWRESET at \+0xda "
            r"and the dsb at \+0xe6",
        )

    # N3: the SWRESET store sets bit 0.
    def test_planted_negative_swreset_orr_dropped(self) -> None:
        self.assertFails(
            dropped(listing(), 0x91e),
            r"MCR0 store at \+0xda writes r3, which no `orr #1`",
        )

    # The restore order, the call and the I-side tail.
    def test_planted_negative_bl_before_the_gpr32_store(self) -> None:
        # Before the 0x90c base reload, so the bl does not unresolve it.
        lines = inserted(dropped(listing(), 0x936), 0x90b, f"bl\t6b0 <{HELPER}>")
        self.assertFails(
            lines, r"\+0xc3 calls .* before the GPR32 restore store at \+0xc6"
        )

    def test_planted_negative_restore_dsb_dropped(self) -> None:
        self.assertFails(
            dropped(listing(), 0x916),
            r"no dsb between the GPR32 restore at \+0xc6.*SWRESET at \+0xda",
        )

    def test_a_dropped_read_back_fails(self) -> None:
        self.assertFails(
            dropped(listing(), 0x912), r"no IOMUXC_GPR_GPR32 read-back"
        )

    def test_a_call_between_restore_and_swreset_fails(self) -> None:
        lines = inserted(dropped(listing(), 0x936), 0x918, f"bl\t6b0 <{HELPER}>")
        self.assertFails(lines, r"before the restore SWRESET at \+0xda")

    def test_a_second_pass_fails(self) -> None:
        self.assertFails(
            inserted(listing(), 0x93c, f"bl\t6b0 <{HELPER}>"), r"calls .* 2 times"
        )

    def test_planted_negative_post_pass_dsb_dropped(self) -> None:
        self.assertFails(
            dropped(listing(), 0x93a),
            r"no dsb between the .* call at \+0xee and the ICIALLU at \+0xfc",
        )

    def test_planted_negative_iciallu_dropped(self) -> None:
        self.assertFails(
            dropped(listing(), 0x944), r"no ICIALLU \(0xe000ef50\) store"
        )

    def test_planted_negative_dsb_after_iciallu_dropped(self) -> None:
        self.assertFails(
            dropped(listing(), 0x948), r"no dsb after the ICIALLU at \+0xfc"
        )

    def test_planted_negative_isb_dropped(self) -> None:
        self.assertFails(
            dropped(listing(), 0x94c), r"no isb after the dsb at \+0x100"
        )

    def test_a_missing_primask_restore_fails(self) -> None:
        self.assertFails(
            dropped(listing(), 0x950, 0x954), r"no PRIMASK restore or return"
        )

    # Round 2 S1: nothing unmasks before the final PRIMASK restore.
    def test_planted_negative_msr_before_the_swreset(self) -> None:
        self.assertFails(
            inserted(listing(), 0x919, "msr\tPRIMASK, r4"),
            r"\+0xd1 is an early msr PRIMASK, r4 inside the restore sequence "
            r"\(before the PRIMASK restore at \+0x108\)",
        )

    def test_planted_negative_msr_before_the_call(self) -> None:
        self.assertFails(
            inserted(listing(), 0x934, "msr\tPRIMASK, r4"),
            r"\+0xec is an early msr PRIMASK, r4",
        )

    def test_planted_negative_cpsie_before_the_call(self) -> None:
        self.assertFails(
            inserted(listing(), 0x934, "cpsie\ti"), r"\+0xec is an early cpsie i"
        )

    # Round 2 N1: the exact loop shape.
    def test_planted_negative_inner_bhi(self) -> None:
        self.assertFails(
            replaced(listing(), 0x6cc, "bne.n", "bhi.n"),
            r"\+0x1c closes the inner \(set\) loop with bhi\.n, not bne",
        )

    def test_planted_negative_outer_bhi(self) -> None:
        self.assertFails(
            replaced(listing(), 0x6d6, "bne.n", "bhi.n"),
            r"\+0x26 closes the outer \(way\) loop with bhi\.n, not bne",
        )

    def test_planted_negative_way_step_2(self) -> None:
        self.assertFails(
            replaced(listing(), 0x6ce, "#1", "#2"),
            r"outer \(way\) loop counter r12 is not stepped by exactly one "
            r"`add r12, #1`.*found: add\.w ip, ip, #2",
        )

    def test_planted_negative_set_start_4096(self) -> None:
        self.assertFails(
            replaced(listing(), 0x6bc, "r3, #0", "r3, #4096"),
            r"inner \(set\) loop counter r3 does not start at 0 .*movs r3, #4096",
        )

    def test_planted_negative_way_start_2(self) -> None:
        self.assertFails(
            replaced(listing(), 0x6b0, "ip, #0", "ip, #2"),
            r"outer \(way\) loop counter r12 does not start at 0",
        )

    def test_a_second_counter_write_in_the_loop_fails(self) -> None:
        self.assertFails(
            inserted(listing(), 0x6c3, "adds\tr3, #32"),
            r"inner \(set\) loop counter r3 is not stepped by exactly one",
        )

    # Round 2 N2: the poll waits on bit 0 of the MCR0 load.
    def test_planted_negative_poll_ands_2(self) -> None:
        self.assertFails(
            replaced(listing(), 0x926, "#1", "#2"),
            r"\+0xe2 poll branch is not fed by `ands`/`tst` #1 of the MCR0 load",
        )

    def test_a_poll_on_some_other_register_fails(self) -> None:
        self.assertFails(
            replaced(listing(), 0x926, "r3, r3, #1", "r3, r1, #1"),
            r"poll branch is not fed by",
        )

    # Round 2 N3: an unresolved base inside the restore sequence fails.
    def test_planted_negative_gpr32_store_after_the_call_via_r3(self) -> None:
        self.assertFails(
            inserted(listing(), 0x938, "str.w\tr7, [r3, #128]"),
            r"\+0xf0 str\.w r7, \[r3, #128\] goes through an unresolved base",
        )

    def test_stack_slots_in_the_restore_sequence_are_exempt(self) -> None:
        # 0x92c / 0x942 spill and reload r3 through sp in the real link.
        events = VERIFY.trace_memory_accesses(
            VERIFY.parse_disassembly("\n".join(listing()))[READER]
        )
        self.assertIn(("store", 0x92c, None), events)
        self.assertIn(("load", 0x942, None), events)
        self.check(listing())

    def test_an_invalidate_only_pass_fails(self) -> None:
        self.assertFails(
            replaced(listing(), 0x6c8, "#3956]", "#3936]"),
            r"stores SCB_CACHE_DCISW \(0xe000ef60\)",
        )

    def test_an_inlined_helper_fails_as_absent(self) -> None:
        functions = VERIFY.parse_disassembly("\n".join(listing()))
        del functions[HELPER]
        with self.assertRaisesRegex(
            VERIFY.VerificationError, r"absent .*ota_dcache_clean_invalidate_all"
        ):
            VERIFY.verify_identity_restore(functions)

    def test_an_unmodelled_base_write_loses_resolution(self) -> None:
        self.assertFails(
            inserted(listing(), 0x6b6, "adds\tr0, r0, r1"),
            r"no resolved store to SCB_CACHE_DCCISW",
        )

    def test_a_call_forgets_the_caller_saved_base(self) -> None:
        lines = inserted(
            inserted(dropped(listing(), 0x90c), 0x90b, "ldr\tr3, [pc, #84]"),
            0x90d, "blx\tr5",
        )
        self.assertFails(lines, r"1 resolved IOMUXC_GPR_GPR32")

    def test_the_closure_walk_follows_the_helper_call(self) -> None:
        functions = VERIFY.parse_disassembly("\n".join(listing()))
        for name, start in (
            ("aius_ota_activate_check_span_in", 0x2a0), ("memcpy", 0x1d2f8)
        ):
            functions[name] = {
                "start": start, "branches": [], "words": [], "insns": []
            }
        walked, _, _ = VERIFY.verify_step_loop_closure(
            functions, 0x20000, ("ota_flash_read",)
        )
        self.assertEqual(walked, 4)

    def test_a_non_remap_env_skips(self) -> None:
        self.assertIsNone(VERIFY.verify_identity_restore_in_build(
            Path("/nonexistent"), "readelf", "teensy41-optical"
        ))
        self.assertIn(
            "SKIP", VERIFY._identity_restore_summary(None, "teensy41-optical")
        )
        self.assertIn(
            "one bl to ota_dcache_clean_invalidate_all at +0xee",
            VERIFY._identity_restore_summary((1, 0xc6, 0xee), "x"),
        )


def tx_symbol_table(
    *rows: tuple[str, int, int],
) -> dict[str, tuple[int, int]]:
    """Every TX_ITCM_SYMBOLS name in ITCM with a size, the given (name,
    address, size) rows overriding or extending it."""
    table: dict[str, tuple[int, int]] = {}
    address = 0x200
    for name in VERIFY.TX_ITCM_SYMBOLS:
        table[name] = (address, 0x80)
        address += 0x100
    for name, value, size in rows:
        table[name] = (value, size)
    return table


class TxItcmResidencyTests(unittest.TestCase):
    """The TX DMA ISR and the gate open's masked SADDR accessor must link
    into ITCM (aius-sample-rate-following Phase 4): a flash copy runs a
    FlexSPI fetch with interrupts masked, a folded copy loses the pin."""

    def test_parses_sizes_in_decimal_and_hex(self) -> None:
        output = (
            "     1: 00000401   484 FUNC    LOCAL  DEFAULT    1 "
            "_ZN12_GLOBAL__N_117AiusSpdifTx24Hook3isrEv\n"
            "     2: 00000601 0x186a0 FUNC    GLOBAL DEFAULT    1 "
            "aius_spdif_tx24_dma_read_position\n"
            "     3: 20001000    16 OBJECT  GLOBAL DEFAULT    4 "
            "g_aius_tx_fill_applied_at_ms\n"
        )
        self.assertEqual(
            VERIFY.parse_function_sizes(output),
            {
                "_ZN12_GLOBAL__N_117AiusSpdifTx24Hook3isrEv": (0x401, 484),
                "aius_spdif_tx24_dma_read_position": (0x601, 100000),
            },
        )

    def test_a_complete_itcm_table_passes_with_sizes(self) -> None:
        table = tx_symbol_table(
            ("aius_spdif_tx24_dma_read_position", 0x900, 52),
        )
        sizes = VERIFY.verify_tx_itcm_symbols(table)
        self.assertEqual(set(sizes), set(VERIFY.TX_ITCM_SYMBOLS))
        self.assertEqual(sizes["aius_spdif_tx24_dma_read_position"], 52)

    def test_a_flash_resident_accessor_fails_as_outside_itcm(self) -> None:
        table = tx_symbol_table(
            ("aius_spdif_tx24_dma_read_position", 0x60012340, 52),
        )
        with self.assertRaisesRegex(
            VERIFY.VerificationError,
            r"outside ITCM: aius_spdif_tx24_dma_read_position at 0x60012340",
        ):
            VERIFY.verify_tx_itcm_symbols(table)

    def test_a_dtcm_isr_fails_as_outside_itcm(self) -> None:
        table = tx_symbol_table(
            ("_ZN12_GLOBAL__N_117AiusSpdifTx24Hook3isrEv", 0x20000400, 484),
        )
        with self.assertRaisesRegex(
            VERIFY.VerificationError, r"outside ITCM.*AiusSpdifTx24Hook3isr"
        ):
            VERIFY.verify_tx_itcm_symbols(table)

    def test_a_folded_accessor_fails_as_missing(self) -> None:
        table = tx_symbol_table()
        del table["aius_spdif_tx24_dma_read_position"]
        with self.assertRaisesRegex(
            VERIFY.VerificationError,
            r"absent \(renamed or inlined away\): "
            r"aius_spdif_tx24_dma_read_position",
        ):
            VERIFY.verify_tx_itcm_symbols(table)

    def test_the_summary_names_each_symbol_and_the_total(self) -> None:
        line = VERIFY._tx_itcm_summary(
            {"isr": 484, "aius_spdif_tx24_dma_read_position": 52}
        )
        self.assertIn("2 symbols in ITCM, 536 bytes", line)
        self.assertIn("isr 484 B", line)


if __name__ == "__main__":
    unittest.main()
