#!/usr/bin/env python3
"""Source-contract tests for the vendored SdFat 2.1.2 and SD wrapper (Phase 4e).

Pins both snapshots by hash (UPSTREAM_SHA256 for the pristine framework bytes,
PATCHED_SHA256 for every intentional Aius patch) and by structure: where the
SDIO wait hook fires (E1, E6), the DTCM-only DMA rule (E2), the busy-timeout
counter (E7), the error reset, the stored FAT volume serial (CR5-S4) and the
SD wrapper API the sd-adapter calls. Run: python3 -m unittest discover -s
firmware/test/sdfat-vendor -p 'test_*.py' (or run this file directly).
"""

import hashlib
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path, PurePosixPath


FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
SDFAT_ROOT = FIRMWARE_ROOT / "vendor" / "SdFat"
SD_ROOT = FIRMWARE_ROOT / "vendor" / "SD"
SDIO_CPP = SDFAT_ROOT / "src" / "SdCard" / "SdioTeensy.cpp"
SDIO_H = SDFAT_ROOT / "src" / "SdCard" / "SdioCard.h"
FAT_PARTITION_CPP = SDFAT_ROOT / "src" / "FatLib" / "FatPartition.cpp"
FAT_PARTITION_H = SDFAT_ROOT / "src" / "FatLib" / "FatPartition.h"
FS_VOLUME_H = SDFAT_ROOT / "src" / "FsLib" / "FsVolume.h"
SD_CPP = SD_ROOT / "src" / "SD.cpp"
SD_H = SD_ROOT / "src" / "SD.h"

AIUS_ADDITIONS = {"NOTICE.md", "library.json", ".gitattributes", "UPSTREAM_SHA256", "PATCHED_SHA256"}

SDFAT_PATCHED = {
    "library.properties",
    "src/FatLib/FatPartition.cpp",
    "src/FatLib/FatPartition.h",
    "src/FsLib/FsVolume.h",
    "src/SdCard/SdioCard.h",
    "src/SdCard/SdioTeensy.cpp",
}
SD_PATCHED = {"library.properties", "src/SD.cpp", "src/SD.h"}

HOOK_CALL = re.compile(r"\baius_sdio_wait_hook\s*\(\s*\)\s*;")


def strip_comments_and_literals(text):
    pattern = re.compile(r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'", re.DOTALL)
    return pattern.sub(lambda match: " " * len(match.group(0)), text)


def code(path):
    return strip_comments_and_literals(path.read_text())


def matching_brace(text, opening):
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    raise AssertionError("unterminated block")


def function_bodies(text):
    """Map each top-level function definition name to its body (comment-free)."""
    bodies = {}
    signature = re.compile(r"^[A-Za-z_][\w\s\*:<>,&]*?\b([A-Za-z_][\w:]*)\s*\(([^;{}]*)\)\s*(?:const\s*)?\{", re.MULTILINE)
    for match in signature.finditer(text):
        opening = match.end() - 1
        closing = matching_brace(text, opening)
        name = match.group(1)
        key = name if name not in bodies else f"{name}#{len([k for k in bodies if k.split('#')[0] == name])}"
        bodies[key] = text[opening + 1 : closing]
    return bodies


def bodies_named(text, name):
    return [body for key, body in function_bodies(text).items() if key.split("#")[0] == name]


def one_body(text, name):
    found = bodies_named(text, name)
    assert len(found) == 1, f"expected one definition of {name}, found {len(found)}"
    return found[0]


def read_manifest(path):
    manifest = {}
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        digest, relative = line.split("  ", 1)
        assert re.fullmatch(r"[0-9a-f]{64}", digest), line
        manifest[relative] = digest
    return manifest


def loop_blocks(body, keyword):
    """Bodies of every `keyword (...) {` or `do {` block in body."""
    blocks = []
    if keyword == "do":
        starts = [m.end() - 1 for m in re.finditer(r"\bdo\s*\{", body)]
    else:
        starts = [m.end() - 1 for m in re.finditer(rf"\b{keyword}\s*\([^{{;]*\)\s*\{{", body)]
    for opening in starts:
        blocks.append(body[opening + 1 : matching_brace(body, opening)])
    return blocks


class SnapshotHashes(unittest.TestCase):
    def check_snapshot(self, root, patched, library_name):
        upstream = read_manifest(root / "UPSTREAM_SHA256")
        patched_manifest = read_manifest(root / "PATCHED_SHA256")
        copied = {
            PurePosixPath(path.relative_to(root).as_posix()).as_posix()
            for path in root.rglob("*")
            if path.is_file() and path.relative_to(root).as_posix() not in AIUS_ADDITIONS
        }
        self.assertEqual(set(upstream), copied)
        self.assertEqual(set(patched_manifest), patched)
        notice = (root / "NOTICE.md").read_text()
        for relative in sorted(copied):
            actual = hashlib.sha256((root / relative).read_bytes()).hexdigest()
            if relative in patched:
                self.assertNotEqual(actual, upstream[relative], relative)
                self.assertEqual(actual, patched_manifest[relative], relative)
                self.assertIn(f"`{relative}`", notice)
            else:
                self.assertEqual(actual, upstream[relative], relative)
        self.assertIn(f'"name": "{library_name}"', (root / "library.json").read_text())
        self.assertIn(f"name={library_name}\n", (root / "library.properties").read_text())
        self.assertIn("framework-arduinoteensy` version\n`1.162.0`", notice)
        self.assertNotIn("\u2014", notice)
        self.assertFalse((root / "examples").exists())

    def test_sdfat_snapshot(self):
        self.check_snapshot(SDFAT_ROOT, SDFAT_PATCHED, "AiusSdFat")
        self.assertIn("version=2.1.2", (SDFAT_ROOT / "library.properties").read_text())
        self.assertTrue((SDFAT_ROOT / "LICENSE.md").is_file())

    def test_sd_snapshot(self):
        self.check_snapshot(SD_ROOT, SD_PATCHED, "AiusSD")


class WaitHookSites(unittest.TestCase):
    """E1, E6: the hook fires only between card commands."""

    def setUp(self):
        self.text = code(SDIO_CPP)

    def test_weak_default_and_declaration(self):
        self.assertRegex(SDIO_CPP.read_text(), r'extern\s+"C"\s+__attribute__\(\(weak\)\)\s+void\s+aius_sdio_wait_hook\(void\)\s*\{\s*\}')
        self.assertIn('extern "C" void aius_sdio_wait_hook(void);', SDIO_H.read_text())

    def test_hook_called_only_in_yield_timeout_and_begin(self):
        callers = {key.split("#")[0] for key, body in function_bodies(self.text).items() if HOOK_CALL.search(body)}
        self.assertEqual(callers, {"yieldTimeout", "SdioCard::begin"})
        self.assertEqual(len(HOOK_CALL.findall(self.text)), 2)

    def test_hook_once_per_yield_timeout_iteration(self):
        loops = loop_blocks(one_body(self.text, "yieldTimeout"), "while")
        self.assertEqual(len(loops), 1)
        self.assertEqual(len(HOOK_CALL.findall(loops[0])), 1)

    def test_hook_once_per_acmd41_iteration(self):
        acmd41 = [block for block in loop_blocks(one_body(self.text, "SdioCard::begin"), "do") if "ACMD41_XFERTYP" in block]
        self.assertEqual(len(acmd41), 1)
        self.assertEqual(len(HOOK_CALL.findall(acmd41[0])), 1)

    def test_no_hook_or_yield_inside_a_command_or_fifo_loop(self):
        for name in ("waitTimeout", "cardCommand", "SdioCard::readData", "SdioCard::writeData"):
            body = one_body(self.text, name)
            self.assertIsNone(HOOK_CALL.search(body), name)
            self.assertNotRegex(body, r"\byield(Timeout)?\s*\(", name)

    def test_no_hook_between_xfertyp_write_and_cc(self):
        command = one_body(self.text, "cardCommand")
        write = command.index("SDHC_XFERTYP")
        tail = command[write:]
        self.assertIn("waitTimeout(isBusyCommandComplete)", tail)
        self.assertNotRegex(tail, r"\b(aius_sdio_wait_hook|yield|yieldTimeout)\s*\(")

    def test_discriminates(self):
        mutated = self.text.replace("static bool waitTimeout(bool (*fcn)()) {\n", "static bool waitTimeout(bool (*fcn)()) {\n  aius_sdio_wait_hook();\n", 1)
        self.assertNotEqual(mutated, self.text)
        callers = {key.split("#")[0] for key, body in function_bodies(mutated).items() if HOOK_CALL.search(body)}
        self.assertIn("waitTimeout", callers)


class DtcmOnlyDma(unittest.TestCase):
    """E2: no DMA touches a buffer the DTCM predicate has not passed."""

    def setUp(self):
        self.text = code(SDIO_CPP)

    def test_rdwr_sectors_and_cmd6_refuse_before_programming_the_controller(self):
        for name, buffer, length in (("rdWrSectors", "buf", "512*n"), ("cardCMD6", "status", "64")):
            body = one_body(self.text, name)
            guard = body.index(f"!aiusDmaBufferOk({buffer}, {length})")
            self.assertLess(guard, body.index("SDHC_DSADDR"), name)
            self.assertLess(guard, body.index("cardCommand("), name)
            self.assertIn("SD_CARD_ERROR_DMA", body[guard : body.index("SDHC_DSADDR")], name)

    def test_single_sector_pair_bounces_non_dtcm(self):
        read = one_body(self.text, "SdioCard::readSector")
        self.assertIn("aiusDmaBufferOk(dst, 512) ? dst : aligned", read)
        write = one_body(self.text, "SdioCard::writeSector")
        self.assertRegex(write, r"if \(!aiusDmaBufferOk\(src, 512\)\) \{\s*ptr = aligned;\s*memcpy\(aligned, src, 512\);")
        for body in (read, write):
            self.assertIn("uint8_t aligned[512] __attribute__((aligned(4)));", body)

    def test_multi_sector_pair_loops_per_sector_for_non_dtcm(self):
        for name, buffer, single, multi in (
            ("SdioCard::readSectors", "dst", "readSector(", "CMD18_DMA_XFERTYP"),
            ("SdioCard::writeSectors", "ptr", "writeSector(", "CMD25_DMA_XFERTYP"),
        ):
            body = one_body(self.text, name)
            guard = body.index(f"if (!aiusDmaBufferOk({buffer}, 512*n)) {{")
            block_end = matching_brace(body, body.index("{", guard))
            block = body[guard:block_end]
            self.assertIn(single, block, name)
            self.assertIn("return true;", block, name)
            self.assertGreater(body.index(multi), block_end, name)
            self.assertNotRegex(body, r"&\s*3\b", name)

    def test_every_dma_call_site_is_guarded(self):
        self.assertEqual(len(re.findall(r"\brdWrSectors\(", self.text)), 1 + 4)  # definition, 4 sector calls
        self.assertEqual(len(re.findall(r"\bcardCMD6\(", self.text)), 1 + 2)  # definition, 2 calls in begin
        self.assertIn("uint8_t status[64] __attribute__((aligned(4)));", self.text)

    def test_predicate_bounded_by_linker_estack(self):
        source = SDIO_CPP.read_text()
        start = source.index("static bool aiusDmaBufferOk(")
        predicate = source[start : matching_brace(source, source.index("{", start)) + 1]
        self.assertIn("const uint32_t kDtcmEnd = (uint32_t)_estack;", predicate)
        self.assertNotIn("0x20080000", predicate)
        self.assertIn('extern "C" char _estack[];', source[:start])

    def test_predicate_behaviour(self):
        compiler = shutil.which("g++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no host C++ compiler")
        source = SDIO_CPP.read_text()
        start = source.index("static bool aiusDmaBufferOk(")
        predicate = source[start : matching_brace(source, source.index("{", start)) + 1]
        # The harness supplies _estack as the 6-ITCM-block link places it
        # (0x20050000); the target takes it from the linker script.
        program = (
            "#include <cstdint>\n#include <cstddef>\n#include <cstdio>\n"
            "#define _estack 0x20050000u\n"
            + predicate
            + "\nint main() {\n"
            "  struct { std::uintptr_t a; std::size_t n; bool ok; } cases[] = {\n"
            "    {0x20000000u, 512, true}, {0x2004FE00u, 512, true}, {0x2004FFFCu, 4, true},\n"
            "    {0x2004FE04u, 512, false}, {0x2004FFFCu, 8, false},\n"
            "    {0x20050000u, 0, false}, {0x20050000u, 512, false}, {0x20050004u, 4, false},\n"
            "    {0x2007FE00u, 512, false}, {0x20080000u, 512, false},\n"
            "    {0x20000002u, 512, false}, {0x2004FDFEu, 512, false}, {0x1FFFFE00u, 512, false},\n"
            "    {0x20200000u, 512, false}, {0x70000000u, 512, false}, {0x20001000u, 4096, true},\n"
            "    {0x2004F000u, 8192, false}, {0xFFFFFE00u, 512, false} };\n"
            "  int bad = 0;\n"
            "  for (auto& c : cases) if (aiusDmaBufferOk((const void*)c.a, c.n) != c.ok) { std::printf(\"%lx\\n\", (unsigned long)c.a); bad++; }\n"
            "  return bad;\n}\n"
        )
        program = program.replace("uint32_t a = (uint32_t)buf;", "uint32_t a = (uint32_t)(std::uintptr_t)buf;")
        with tempfile.TemporaryDirectory() as scratch:
            src = Path(scratch) / "predicate.cpp"
            binary = Path(scratch) / "predicate"
            src.write_text(program)
            subprocess.run([compiler, "-std=c++11", "-D__IMXRT1062__", "-o", str(binary), str(src)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout)


class CounterAndErrorReset(unittest.TestCase):
    def setUp(self):
        self.text = code(SDIO_CPP)

    def test_every_busy_timeout_expiry_counts(self):
        for name in ("yieldTimeout", "waitTimeout", "SdioCard::begin"):
            body = one_body(self.text, name)
            for block in re.finditer(r"if \(\(micros\(\) - m\) > BUSY_TIMEOUT_MICROS\) \{", body):
                inner = body[block.end() : matching_brace(body, block.end() - 1)]
                self.assertIn("m_busyTimeouts++;", inner, name)
            self.assertIn("m_busyTimeouts++;", body, name)
        self.assertRegex(one_body(self.text, "SdioCard::busyTimeouts"), r"^\s*return m_busyTimeouts;\s*$")
        self.assertNotRegex(one_body(self.text, "SdioCard::begin"), r"m_busyTimeouts\s*=")

    def test_clear_error_resets_file_static_state(self):
        body = one_body(self.text, "SdioCard::clearError")
        self.assertIn("m_errorCode = SD_CARD_ERROR_NONE;", body)
        self.assertIn("m_errorLine = 0;", body)
        header = SDIO_H.read_text()
        self.assertIn("static void clearError();", header)
        self.assertIn("static uint32_t busyTimeouts();", header)


class VolumeSerial(unittest.TestCase):
    """CR5-S4: stored at init from the boot sector already read; no card read."""

    def test_init_stores_serial_from_the_boot_sector(self):
        text = code(FAT_PARTITION_CPP)
        inits = bodies_named(text, "FatPartition::init")
        self.assertEqual(len(inits), 2)
        for body in inits:
            zero = body.index("m_volumeSerial = 0;")
            boot = body.index("dataCachePrepare(volumeStartSector")
            store = body.index("m_volumeSerial = m_fatType == 32")
            self.assertLess(zero, boot)
            self.assertLess(boot, store)
            between = body[boot + 1 : store]
            self.assertNotRegex(between, r"\b(dataCachePrepare|cacheSafeRead|readSector|cacheSync)\s*\(")
            self.assertIn("getLe32(bpb->volumeSerialNumber)", body[store:])
            self.assertIn("reinterpret_cast<BpbFat16_t*>(pbs->bpb)->volumeSerialNumber", body[store:])
            self.assertLess(store, body.rindex("return true;"))

    def test_accessors_read_only_the_member(self):
        header = code(FAT_PARTITION_H)
        self.assertRegex(header, r"uint32_t volumeSerial\(\) const \{\s*return m_volumeSerial;\s*\}")
        self.assertRegex(header, r"uint32_t m_volumeSerial = 0;")
        self.assertRegex(header, r"m_fatType = 0;\s*m_volumeSerial = 0;\s*return cacheClear\(\);")
        volume = code(FS_VOLUME_H)
        self.assertRegex(volume, r"uint32_t volumeSerial\(\) const \{\s*return m_fVol \? m_fVol->volumeSerial\(\) : 0;\s*\}")


class CacheClearForwarder(unittest.TestCase):
    """Phase 4h CR4-B2: FsVolume forwards the mounted partition's cacheClear()."""

    FORWARDER = (
        r"uint8_t\* cacheClear\(\) \{\s*return m_fVol \? m_fVol->cacheClear\(\) :"
        r"\s*m_xVol \? m_xVol->cacheClear\(\) : nullptr;\s*\}"
    )

    def test_forwards_to_the_mounted_partition_or_fails(self):
        self.assertRegex(code(FS_VOLUME_H), self.FORWARDER)

    def test_forwarder_is_public(self):
        volume = code(FS_VOLUME_H)
        body = volume[volume.index("class FsVolume {"):]
        forwarder = re.search(self.FORWARDER, body)
        self.assertIsNotNone(forwarder)
        self.assertIn("public:", body[: forwarder.start()])
        self.assertNotRegex(body[: forwarder.start()], r"\b(private|protected):")

    def test_discriminates(self):
        volume = code(FS_VOLUME_H)
        for original, mutated in (
            ("m_xVol->cacheClear() : nullptr", "m_xVol->cacheClear() : m_cache"),
            ("return m_fVol ? m_fVol->cacheClear()", "return m_xVol ? m_xVol->cacheClear()"),
        ):
            self.assertIn(original, volume)
            self.assertNotRegex(volume.replace(original, mutated, 1), self.FORWARDER)


class SdWrapperApi(unittest.TestCase):
    def setUp(self):
        self.header = code(SD_H)
        self.text = code(SD_CPP)

    def test_declarations(self):
        for declaration in (
            "bool begin(uint8_t csPin = 10, bool dma = false);",
            "bool cardDetected();",
            "void clearError();",
            "uint8_t sdErrorCode();",
            "uint32_t busyTimeouts();",
        ):
            self.assertIn(declaration, self.header)

    def test_begin_mounts_either_mode_in_one_call(self):
        body = one_body(self.text, "SDClass::begin")
        self.assertIn("sdfs.begin(SdioConfig(dma ? DMA_SDIO : FIFO_SDIO))", body)
        self.assertNotIn("SdioConfig(FIFO_SDIO)", body)

    def test_card_detected_reads_dat3_only(self):
        body = one_body(self.text, "SDClass::cardDetected")
        self.assertIn("digitalReadFast(_SD_DAT3)", body)
        self.assertNotRegex(body, r"\b(restart|begin|status|pinMode|readCID)\s*\(")

    def test_forwarders_are_safe_before_mount(self):
        self.assertRegex(one_body(self.text, "SDClass::clearError"), r"SdioCard::clearError\(\);")
        self.assertRegex(one_body(self.text, "SDClass::busyTimeouts"), r"return SdioCard::busyTimeouts\(\);")
        self.assertRegex(one_body(self.text, "SDClass::sdErrorCode"), r"return card \? card->errorCode\(\) : 0;")


if __name__ == "__main__":
    unittest.main()
