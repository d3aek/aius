#!/usr/bin/env python3
"""Black-box tests for the per-bank memory gate, `tools/memory/mem_report.py`.

`build.sh` fails the build on this tool's exit code, so it is a release gate. It
shipped with no coverage of any kind and silently overstated the DTCM
free-for-locals zone by the whole size of an orphan-placed section
(`NonCacheable`, 9,756 B, between `.data` and `.bss`). These tests pin the
arithmetic that was wrong.

Everything is driven through the CLI -- exit code plus the printed table -- which
is exactly what `build.sh` consumes.

Fixtures
--------
`fixtures/teensy41-optical.elf` is the REAL linked optical image, reduced to its
section table, program headers and the linker symbols. It is byte-for-byte the
linker's own output for those structures, not a reconstruction. Regenerate it
after a relink with:

    ELF=.pio/build/teensy41-optical/firmware.elf
    TC=$PLATFORMIO_CORE_DIR/packages/toolchain-gccarmnoneeabi-teensy/bin
    $TC/arm-none-eabi-objcopy --only-keep-debug $ELF stage.elf
    $TC/arm-none-eabi-objcopy -R .debug_frame -R .debug_line_str -R .comment \
        --strip-unneeded --keep-symbols=test/memory/fixtures/keep-symbols.txt \
        stage.elf test/memory/fixtures/teensy41-optical.elf
    $TC/arm-none-eabi-readelf -lSW test/memory/fixtures/teensy41-optical.elf \
        > test/memory/fixtures/teensy41-optical.readelf.txt

`--only-keep-debug` turns every allocated section into NOBITS but preserves its
name, flags, address and size, which is all this tool reads. The `.readelf.txt`
capture is the human-reviewable view of the same bytes, so a reviewer does not
need a cross toolchain to check what the binary fixture claims.

`fixtures/teensy41-optical-size.log` is the verbatim tail of the matching
PlatformIO build log, including the `teensy_size:` summary that disagrees with it.

Variants that a real link cannot cheaply provide (an orphan above `_ebss`, a
`.bss` that has eaten the stack) are made by patching individual fields of that
real ELF with `Elf32` below -- an independent reader/writer, so the tests are not
checking `mem_report`'s parser against itself.
"""

import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
MEM_REPORT = FIRMWARE_ROOT / "tools" / "memory" / "mem_report.py"
FIXTURES = Path(__file__).resolve().parent / "fixtures"
ELF_FIXTURE = FIXTURES / "teensy41-optical.elf"
LOG_FIXTURE = FIXTURES / "teensy41-optical-size.log"

DTCM_ORIGIN = 0x20000000
BANK_ORDER = ["FLASH", "ITCM", "DTCM", "FlexRAM", "OCRAM"]
ROW_RE = re.compile(r"^  (\w+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+-?[\d.]+%")

EXIT_OK = 0
EXIT_OVER_CAPACITY = 2
EXIT_UNUSABLE_INPUT = 3
EXIT_USAGE = 4


# --------------------------------------------------------------------------
# Independent ELF32 reader/writer, used to read watermarks and to derive
# variants of the real fixture. Field offsets are the ELF32 spec's.
# --------------------------------------------------------------------------

class Elf32:
    SH_NAME, SH_ADDR, SH_OFFSET, SH_SIZE, SH_LINK, SH_ENTSIZE = 0, 12, 16, 20, 24, 36
    ST_NAME, ST_VALUE = 0, 4
    SHT_SYMTAB = 2

    def __init__(self, path):
        self.data = bytearray(Path(path).read_bytes())
        assert self.data[:4] == b"\x7fELF", f"{path} is not an ELF file"
        self.shoff, = struct.unpack_from("<I", self.data, 32)
        self.shentsize, self.shnum, self.shstrndx = struct.unpack_from(
            "<HHH", self.data, 46)

    def _shdr(self, index):
        return self.shoff + index * self.shentsize

    def _field(self, base, offset):
        return struct.unpack_from("<I", self.data, base + offset)[0]

    def _set_field(self, base, offset, value):
        struct.pack_into("<I", self.data, base + offset, value)

    def _string(self, table_base, offset):
        start = self._field(table_base, self.SH_OFFSET) + offset
        end = self.data.index(b"\0", start)
        return self.data[start:end].decode()

    def section(self, name):
        shstr = self._shdr(self.shstrndx)
        for index in range(self.shnum):
            base = self._shdr(index)
            if self._string(shstr, self._field(base, self.SH_NAME)) == name:
                return base
        raise KeyError(f"no section {name}")

    def section_addr(self, name):
        return self._field(self.section(name), self.SH_ADDR)

    def section_size(self, name):
        return self._field(self.section(name), self.SH_SIZE)

    def set_section(self, name, addr=None, size=None):
        base = self.section(name)
        if addr is not None:
            self._set_field(base, self.SH_ADDR, addr)
        if size is not None:
            self._set_field(base, self.SH_SIZE, size)

    def _symtab(self):
        for index in range(self.shnum):
            base = self._shdr(index)
            if self._field(base, 4) == self.SHT_SYMTAB:
                return base
        raise KeyError("no symbol table")

    def _each_symbol(self):
        symtab = self._symtab()
        strtab = self._shdr(self._field(symtab, self.SH_LINK))
        offset = self._field(symtab, self.SH_OFFSET)
        size = self._field(symtab, self.SH_SIZE)
        entsize = self._field(symtab, self.SH_ENTSIZE)
        for pos in range(offset, offset + size, entsize):
            name_off = struct.unpack_from("<I", self.data, pos + self.ST_NAME)[0]
            yield pos, strtab, name_off

    def symbol(self, name):
        for pos, strtab, name_off in self._each_symbol():
            if self._string(strtab, name_off) == name:
                return struct.unpack_from("<I", self.data, pos + self.ST_VALUE)[0]
        raise KeyError(f"no symbol {name}")

    def set_symbol(self, name, value):
        for pos, strtab, name_off in self._each_symbol():
            if self._string(strtab, name_off) == name:
                struct.pack_into("<I", self.data, pos + self.ST_VALUE, value)
                return
        raise KeyError(f"no symbol {name}")

    def rename_symbol(self, old, new):
        """Overwrite the name in place; same length keeps every offset valid."""
        assert len(old) == len(new), "rename must preserve the string length"
        for _pos, strtab, name_off in self._each_symbol():
            if self._string(strtab, name_off) == old:
                start = self._field(strtab, self.SH_OFFSET) + name_off
                self.data[start:start + len(new)] = new.encode()
                return
        raise KeyError(f"no symbol {old}")

    def write(self, path):
        Path(path).write_bytes(bytes(self.data))
        return Path(path)


# --------------------------------------------------------------------------
# Driving the tool
# --------------------------------------------------------------------------

def run(*args, stdin=None):
    return subprocess.run(
        [sys.executable, str(MEM_REPORT), *args],
        capture_output=True, text=True, input=stdin, check=False)


def run_ok(*args, stdin=None):
    result = run(*args, stdin=stdin)
    assert result.returncode == EXIT_OK, (
        f"expected exit 0, got {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result


def banks(stdout):
    """The printed table, as {bank: (used, free, capacity)} plus row order."""
    figures, order = {}, []
    for line in stdout.splitlines():
        match = ROW_RE.match(line)
        if match and match.group(1) in BANK_ORDER:
            figures[match.group(1)] = tuple(int(g) for g in match.groups()[1:])
            order.append(match.group(1))
    return figures, order


def report_on_fixture(*extra):
    return run_ok("--size-log", str(LOG_FIXTURE), "--elf", str(ELF_FIXTURE), *extra)


def variant(tmp, name, mutate):
    elf = Elf32(ELF_FIXTURE)
    mutate(elf)
    return elf.write(Path(tmp) / name)


# --------------------------------------------------------------------------
# The arithmetic that was wrong
# --------------------------------------------------------------------------

def test_dtcm_free_zone_is_estack_minus_ebss():
    """The headline defect: the free-for-locals zone is what the linker leaves
    between the top of the statics and the stack, not what teensy_size claims."""
    elf = Elf32(ELF_FIXTURE)
    estack, ebss = elf.symbol("_estack"), elf.symbol("_ebss")
    figures, _ = banks(report_on_fixture().stdout)
    used, free, capacity = figures["DTCM"]
    assert free == estack - ebss, f"DTCM free {free} != _estack - _ebss {estack - ebss}"
    assert used == ebss - DTCM_ORIGIN, f"DTCM used {used} != _ebss - ORIGIN"
    assert capacity == estack - DTCM_ORIGIN
    assert used + free == capacity


def test_orphan_section_inside_dtcm_is_counted():
    """`NonCacheable` is orphan-placed between `.data` and `.bss`. It occupies
    DTCM address space, and the old report excluded it from the used figure."""
    elf = Elf32(ELF_FIXTURE)
    orphan = elf.section_addr("NonCacheable")
    assert (elf.section_addr(".data") < orphan < elf.section_addr(".bss")), (
        "fixture no longer has NonCacheable placed between .data and .bss")
    occupied = (elf.section_size(".data") + elf.section_size("NonCacheable")
                + elf.section_size(".bss"))
    figures, _ = banks(report_on_fixture().stdout)
    assert figures["DTCM"][0] == occupied, (
        f"DTCM used {figures['DTCM'][0]} != .data + NonCacheable + .bss {occupied}")


def test_dtcm_used_exceeds_the_summary_by_exactly_the_orphan():
    """Pins the size of the bug: the whole of the orphan, and nothing else."""
    elf = Elf32(ELF_FIXTURE)
    summary_variables = int(re.search(r"RAM1:\s*variables:(\d+)",
                                      LOG_FIXTURE.read_text()).group(1))
    summary_free = int(re.search(r"free for local variables:(\d+)",
                                 LOG_FIXTURE.read_text()).group(1))
    orphan = elf.section_size("NonCacheable")
    figures, _ = banks(report_on_fixture().stdout)
    used, free, _ = figures["DTCM"]
    assert used - summary_variables == orphan
    assert summary_free - free == orphan


def test_orphan_placed_above_the_watermark_symbol_is_counted():
    """`_ebss` alone is not enough: a section the linker orphans ABOVE the top of
    `.bss` occupies the free zone too, and the section scan must see it."""
    with tempfile.TemporaryDirectory() as tmp:
        base = Elf32(ELF_FIXTURE)
        ebss = base.symbol("_ebss")
        path = variant(tmp, "orphan-high.elf", lambda e: e.set_section(
            "NonCacheable", addr=ebss, size=0x1000))
        figures, _ = banks(run_ok("--elf", str(path)).stdout)
        used, free, capacity = figures["DTCM"]
        assert used == (ebss - DTCM_ORIGIN) + 0x1000, (
            f"DTCM used {used} ignores a section above _ebss")
        assert free == capacity - used


def test_dtcm_overflow_is_detected_and_fails_the_gate():
    """A `.bss` that runs past `_estack` links without complaint -- the DTCM
    region has room, it just eats the stack. That is what the gate is for."""
    with tempfile.TemporaryDirectory() as tmp:
        base = Elf32(ELF_FIXTURE)
        estack = base.symbol("_estack")
        overflow = 0x4000

        def grow(elf):
            elf.set_section(".bss", size=elf.section_size(".bss") + overflow)
            elf.set_symbol("_ebss", elf.symbol("_ebss") + overflow)

        path = variant(tmp, "dtcm-overflow.elf", grow)
        result = run("--elf", str(path))
        assert result.returncode == EXIT_OVER_CAPACITY, (
            f"overflowing DTCM exited {result.returncode}, not {EXIT_OVER_CAPACITY}")
        assert "OVER CAPACITY" in result.stdout
        assert "over capacity" in result.stderr
        figures, _ = banks(result.stdout)
        assert figures["DTCM"][1] == -(overflow - (estack - base.symbol("_ebss")))
        assert figures["DTCM"][1] < 0, "an overflowing bank must show negative free"
        assert figures["FlexRAM"][1] < 0, "the FlexRAM roll-up must overflow too"


def test_banks_are_reported_in_order():
    _, order = banks(report_on_fixture().stdout)
    assert order == BANK_ORDER, f"bank rows out of order: {order}"


def test_flexram_rollup_agrees_with_dtcm():
    """ITCM and DTCM share the 512 KiB FlexRAM, so the roll-up's free zone IS
    the DTCM free zone; if they diverge one of the two is computed wrong."""
    figures, _ = banks(report_on_fixture().stdout)
    itcm_used, _, itcm_capacity = figures["ITCM"]
    dtcm_used, dtcm_free, dtcm_capacity = figures["DTCM"]
    flexram_used, flexram_free, flexram_capacity = figures["FlexRAM"]
    assert flexram_capacity == 512 * 1024
    assert itcm_capacity + dtcm_capacity == flexram_capacity
    assert flexram_used == itcm_capacity + dtcm_used
    assert flexram_free == dtcm_free
    assert itcm_used <= itcm_capacity


def test_flash_used_is_the_whole_linked_image():
    """`_flashimagelen` covers the load images too, including the orphan's; the
    summary's `code + data + headers` does not."""
    elf = Elf32(ELF_FIXTURE)
    figures, _ = banks(report_on_fixture().stdout)
    used, free, capacity = figures["FLASH"]
    assert used == elf.symbol("_flashimagelen")
    assert capacity == 7936 * 1024
    assert used + free == capacity


def test_ocram_is_the_dma_block_and_the_heap():
    elf = Elf32(ELF_FIXTURE)
    figures, _ = banks(report_on_fixture().stdout)
    used, free, capacity = figures["OCRAM"]
    assert used == elf.symbol("_heap_start") - 0x20200000
    assert free == elf.symbol("_heap_end") - elf.symbol("_heap_start")
    assert capacity == 512 * 1024


def test_itcm_capacity_is_the_flexram_block():
    elf = Elf32(ELF_FIXTURE)
    figures, _ = banks(report_on_fixture().stdout)
    used, _, capacity = figures["ITCM"]
    assert capacity == elf.symbol("_itcm_block_count") * 32 * 1024
    assert used == elf.section_addr(".ARM.exidx") + elf.section_size(".ARM.exidx")
    assert used <= capacity, "the ITCM block must contain the code it rounds up from"


def test_section_outside_every_known_region_is_flagged_not_swallowed():
    """The failure mode being fixed is a section going uncounted. One that lands
    somewhere this report does not model must be loud rather than silent."""
    with tempfile.TemporaryDirectory() as tmp:
        path = variant(tmp, "unplaced.elf",
                       lambda e: e.set_section(".bss.dma", addr=0x90000000))
        result = run_ok("--elf", str(path))
        assert "WARNING" in result.stdout and ".bss.dma" in result.stdout


# --------------------------------------------------------------------------
# Cross-check against the teensy_size summary
# --------------------------------------------------------------------------

def test_crosscheck_names_the_section_the_summary_omits():
    stdout = report_on_fixture().stdout
    orphan = Elf32(ELF_FIXTURE).section_size("NonCacheable")
    dtcm_line = next(line for line in stdout.splitlines()
                     if re.match(r"\s+DTCM\s+summary\s", line))
    assert f"+{orphan}" in dtcm_line, dtcm_line
    assert f"NonCacheable {orphan}B" in dtcm_line, dtcm_line
    assert "free for local variables:19036" in stdout
    assert "the linker leaves 9280" in stdout


def test_crosscheck_deltas_are_fully_accounted_for():
    """Every reported delta is attributed to named sections plus explicit fill,
    so a divergence can never be hand-waved."""
    for line in report_on_fixture().stdout.splitlines():
        match = re.match(r"\s+(\w+)\s+summary\s+(\d+)\s+elf\s+(\d+)\s+delta\s+"
                         r"([+-]\d+)\s+omits: (.*)$", line)
        if not match:
            continue
        summary, elf_used, delta = (int(match.group(i)) for i in (2, 3, 4))
        assert elf_used - summary == delta, line
        named = sum(int(n) for n in re.findall(r"(\d+)B", match.group(5)))
        assert named == delta, f"delta not fully accounted for: {line}"


def test_crosscheck_is_skipped_when_no_log_is_supplied():
    stdout = run_ok("--elf", str(ELF_FIXTURE)).stdout
    assert "teensy_size summary printed above" not in stdout
    assert "DTCM" in stdout


# --------------------------------------------------------------------------
# The input shapes the format actually presents
# --------------------------------------------------------------------------

def test_elf_is_resolved_from_the_relative_path_the_log_names():
    """PlatformIO names the image it linked, relative to the project dir. That
    is how build.sh's `--size-log`-only invocation finds the ELF."""
    with tempfile.TemporaryDirectory() as tmp:
        relative = ELF_FIXTURE.relative_to(FIRMWARE_ROOT)
        log = Path(tmp) / "build.log"
        log.write_text(LOG_FIXTURE.read_text().replace(
            ".pio/build/teensy41-optical/firmware.elf", str(relative)))
        assert str(ELF_FIXTURE) in run_ok("--size-log", str(log)).stdout


def test_elf_is_resolved_from_an_absolute_path_in_the_log():
    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / "build.log"
        log.write_text(LOG_FIXTURE.read_text().replace(
            ".pio/build/teensy41-optical/firmware.elf", str(ELF_FIXTURE)))
        assert str(ELF_FIXTURE) in run_ok("--size-log", str(log)).stdout


def test_explicit_elf_overrides_the_log():
    stdout = report_on_fixture().stdout
    assert str(ELF_FIXTURE) in stdout
    assert ".pio/build/teensy41-optical/firmware.elf" not in stdout


def test_log_can_be_read_from_stdin():
    result = run_ok("--size-log", "-", "--elf", str(ELF_FIXTURE),
                    stdin=LOG_FIXTURE.read_text())
    assert "free for local variables:19036" in result.stdout


def test_arena_annotation_prefers_the_flag_and_falls_back_to_the_symbol():
    explicit = report_on_fixture("--arena-bytes", "4096").stdout
    assert "static arena = 4096B" in explicit
    fallback = report_on_fixture().stdout
    assert "g_arena_memory" in fallback, (
        "with no --arena-bytes the arena symbol should be resolved from the ELF")


def expect_error(code, needle, *args, stdin=None):
    result = run(*args, stdin=stdin)
    assert result.returncode == code, (
        f"{args}: expected exit {code}, got {result.returncode}\n{result.stderr}")
    assert needle in result.stderr, f"{args}: {needle!r} not in {result.stderr!r}"
    assert "Traceback" not in result.stderr, f"{args} raised:\n{result.stderr}"


def test_unusable_input_exits_three_without_a_traceback():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        no_summary = tmp / "no-summary.log"
        no_summary.write_text("Linking firmware.elf\n[SUCCESS]\n")
        expect_error(EXIT_UNUSABLE_INPUT, "no 'teensy_size:' summary",
                     "--size-log", str(no_summary))

        no_image = tmp / "no-image.log"
        no_image.write_text("\n".join(
            line for line in LOG_FIXTURE.read_text().splitlines()
            if ".elf" not in line))
        expect_error(EXIT_UNUSABLE_INPUT, "--elf", "--size-log", str(no_image))

        missing = tmp / "gone.log"
        missing.write_text(LOG_FIXTURE.read_text().replace(
            ".pio/build/teensy41-optical/firmware.elf", "no/such/firmware.elf"))
        expect_error(EXIT_UNUSABLE_INPUT, "not found", "--size-log", str(missing))

        expect_error(EXIT_UNUSABLE_INPUT, "cannot read",
                     "--elf", str(tmp / "absent.elf"))

        not_elf = tmp / "not.elf"
        not_elf.write_bytes(b"MZ this is not an ELF file at all")
        expect_error(EXIT_UNUSABLE_INPUT, "not an ELF file", "--elf", str(not_elf))

        truncated = tmp / "truncated.elf"
        truncated.write_bytes(ELF_FIXTURE.read_bytes()[:200])
        expect_error(EXIT_UNUSABLE_INPUT, "truncated", "--elf", str(truncated))

        stripped = variant(tmp, "no-estack.elf",
                           lambda e: e.rename_symbol("_estack", "_estac0"))
        expect_error(EXIT_UNUSABLE_INPUT, "_estack", "--elf", str(stripped))


def test_usage_errors_exit_four_but_help_does_not():
    expect_error(EXIT_USAGE, "pass --size-log")
    expect_error(EXIT_USAGE, "unrecognized arguments", "--not-a-flag")
    assert "--size-log" in run_ok("--help").stdout


def test_a_fitting_image_passes_the_gate():
    result = report_on_fixture()
    assert result.returncode == EXIT_OK
    assert "All banks fit." in result.stdout
    assert "OVER CAPACITY" not in result.stdout


def main():
    test_dtcm_free_zone_is_estack_minus_ebss()
    test_orphan_section_inside_dtcm_is_counted()
    test_dtcm_used_exceeds_the_summary_by_exactly_the_orphan()
    test_orphan_placed_above_the_watermark_symbol_is_counted()
    test_dtcm_overflow_is_detected_and_fails_the_gate()
    test_banks_are_reported_in_order()
    test_flexram_rollup_agrees_with_dtcm()
    test_flash_used_is_the_whole_linked_image()
    test_ocram_is_the_dma_block_and_the_heap()
    test_itcm_capacity_is_the_flexram_block()
    test_section_outside_every_known_region_is_flagged_not_swallowed()
    test_crosscheck_names_the_section_the_summary_omits()
    test_crosscheck_deltas_are_fully_accounted_for()
    test_crosscheck_is_skipped_when_no_log_is_supplied()
    test_elf_is_resolved_from_the_relative_path_the_log_names()
    test_elf_is_resolved_from_an_absolute_path_in_the_log()
    test_explicit_elf_overrides_the_log()
    test_log_can_be_read_from_stdin()
    test_arena_annotation_prefers_the_flag_and_falls_back_to_the_symbol()
    test_unusable_input_exits_three_without_a_traceback()
    test_usage_errors_exit_four_but_help_does_not()
    test_a_fitting_image_passes_the_gate()
    print("mem_report per-bank gate tests: PASS")


if __name__ == "__main__":
    main()
