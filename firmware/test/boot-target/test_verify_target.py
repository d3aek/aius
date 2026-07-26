#!/usr/bin/env python3
"""Independent mutation tests for the retained-slot post-link verifier."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


MODULE_PATH = Path(__file__).with_name("verify_target.py")
SPEC = importlib.util.spec_from_file_location("aius_verify_boot_target", MODULE_PATH)
assert SPEC is not None
assert SPEC.loader is not None
VERIFY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VERIFY
SPEC.loader.exec_module(VERIFY)


def sections(index: int = 12, name: str = ".bss.aius_retained") -> str:
    return (
        "There are 18 section headers:\n"
        "  [Nr] Name          Type      Address  Off    Size ES Flg Lk Inf Al\n"
        f"  [ {index}] {name:<13} NOBITS    20200000 001000 000080 "
        "00  WA  0   0 32\n"
    )


def symbols(
    address: int = 0x20200000,
    size: int = 128,
    symbol_type: str = "OBJECT",
    section: str = "12",
    name: str = "g_aius_boot_slots",
) -> str:
    return (
        "Symbol table '.symtab' contains 2 entries:\n"
        "   Num:    Value  Size Type    Bind   Vis      Ndx Name\n"
        f"   101: {address:08x}   {size} {symbol_type:<7} GLOBAL DEFAULT "
        f"{section:>3} {name}\n"
    )


class ParseTests(unittest.TestCase):
    def test_parses_wide_readelf_tables(self) -> None:
        parsed_sections = VERIFY.parse_sections(sections())
        parsed_symbols = VERIFY.parse_symbols(symbols())
        self.assertEqual(parsed_sections, {12: ".bss.aius_retained"})
        self.assertEqual(len(parsed_symbols), 1)
        self.assertEqual(parsed_symbols[0].address, 0x20200000)
        self.assertEqual(parsed_symbols[0].size, 128)
        self.assertEqual(parsed_symbols[0].section_index, 12)

    def test_ignores_similarly_named_symbol(self) -> None:
        parsed = VERIFY.parse_symbols(
            symbols(name="g_aius_boot_slots_shadow") + symbols()
        )
        self.assertEqual(len(parsed), 1)

    def test_rejects_undefined_symbol(self) -> None:
        with self.assertRaisesRegex(
            VERIFY.VerificationError, "not defined in a numbered section"
        ):
            VERIFY.parse_symbols(symbols(section="UND"))


class ContractMutationTests(unittest.TestCase):
    def verify(self, section_text: str, symbol_text: str):
        return VERIFY.verify_linked_contract(
            VERIFY.parse_sections(section_text),
            VERIFY.parse_symbols(symbol_text),
        )

    def assert_rejected(
        self,
        section_text: str,
        symbol_text: str,
        message: str,
    ) -> None:
        with self.assertRaisesRegex(VERIFY.VerificationError, message):
            self.verify(section_text, symbol_text)

    def test_accepts_the_pinned_retained_origin(self) -> None:
        symbol = self.verify(sections(), symbols())
        self.assertEqual(symbol.address, VERIFY.RETAINED_ORIGIN)
        self.assertEqual(symbol.address + symbol.size, 0x20200080)

    def test_rejects_missing_and_duplicate_symbols(self) -> None:
        self.assert_rejected(sections(), "", "exactly one")
        self.assert_rejected(
            sections(), symbols() + symbols(), "exactly one"
        )

    def test_rejects_wrong_type(self) -> None:
        self.assert_rejected(
            sections(), symbols(symbol_type="NOTYPE"), "expected OBJECT"
        )

    def test_rejects_every_wrong_size(self) -> None:
        for size in (0, 64, 127, 129, 256):
            with self.subTest(size=size):
                self.assert_rejected(
                    sections(), symbols(size=size), "size is"
                )

    def test_rejects_every_address_alignment_remainder(self) -> None:
        for remainder in range(1, 32):
            with self.subTest(remainder=remainder):
                self.assert_rejected(
                    sections(),
                    symbols(address=0x20200000 + remainder),
                    "not 32-byte aligned",
                )

    def test_rejects_wrong_or_missing_section(self) -> None:
        self.assert_rejected(
            sections(name=".bss.dma"), symbols(), "expected .bss.aius_retained"
        )
        self.assert_rejected(
            sections(index=13), symbols(), "expected .bss.aius_retained"
        )

    def test_rejects_first_aligned_overlap(self) -> None:
        self.assert_rejected(
            sections(),
            symbols(address=0x2027FF20),
            "beyond CrashReport boundary",
        )

    def test_rejects_a_floating_address_off_the_pinned_origin(self) -> None:
        """The regression: an aligned, correctly sized, correctly sectioned slot
        array that simply drifted. 0x20201120 is where it used to live and did
        retain; 0x20205160 is where the 24-bit ring pushed it and it did not.
        Neither is the pinned origin, so both are rejected now."""
        for address in (0x20200020, 0x20201120, 0x20203A60, 0x20205160):
            with self.subTest(address=hex(address)):
                self.assert_rejected(
                    sections(),
                    symbols(address=address),
                    "expected the pinned retained origin",
                )

    def test_rejects_every_boot_rom_clobbered_run(self) -> None:
        """Belt to the pinned-origin brace: if the origin constant is ever
        retargeted, the map still refuses an address the boot ROM overwrites."""
        original = VERIFY.RETAINED_ORIGIN
        try:
            for start, stop in VERIFY.BOOT_ROM_CLOBBERED:
                for address in (start, stop - 128, ((start + stop) // 2) & ~31):
                    with self.subTest(address=hex(address)):
                        VERIFY.RETAINED_ORIGIN = address
                        self.assert_rejected(
                            sections(),
                            symbols(address=address),
                            "boot ROM's OCRAM scratch",
                        )
        finally:
            VERIFY.RETAINED_ORIGIN = original

    def test_accepts_addresses_just_outside_a_clobbered_run(self) -> None:
        original = VERIFY.RETAINED_ORIGIN
        try:
            for address in (0x20200BE0, 0x20207F60):
                with self.subTest(address=hex(address)):
                    VERIFY.RETAINED_ORIGIN = address
                    symbol = self.verify(sections(), symbols(address=address))
                    self.assertEqual(symbol.address, address)
        finally:
            VERIFY.RETAINED_ORIGIN = original


if __name__ == "__main__":
    unittest.main()
