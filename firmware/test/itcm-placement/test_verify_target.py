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

    def test_non_library_objects_are_not_gated(self) -> None:
        """An ITCM-emitting object outside the LDF shape is the cold-object and
        hot-object checks' subject; this gate must not double-judge it."""
        paths = self.PATHS + ("src/optical_main.cpp.o",)
        libraries, gated = self.verify(
            paths=paths, texts={"src/optical_main.cpp.o": HOT}
        )
        self.assertEqual((libraries, gated), (2, 2))


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


if __name__ == "__main__":
    unittest.main()
