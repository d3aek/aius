#!/usr/bin/env python3
"""Post-link proof that ITCM holds only code that runs per audio block.

On Teensy 4.1 a function with no FLASHMEM attribute lands in ITCM, and ITCM
shares the 512 KB FlexRAM with DTCM -- so cold code in ITCM is taken straight
out of the audio path's headroom. ITCM is reserved for the bounded producer code
that must preempt the serial AUDIO epoch. Epoch processing remains FLASHMEM even
when it runs per block.

Five checks: the no-LTO precondition, then the cold, optical composition, mixed
ingress-producer and LDF-library translation-unit shapes.

The no-LTO precondition. Every other check reads ONE OBJECT FILE at a time, so
it sees only placement the compiler has already committed to. Link-time
optimisation moves that decision past the last point this file can observe and
every object would look innocent, so `-flto` is not a style question here: it
blinds the guard. The check parses platformio.ini for the environment under
inspection, walks its `extends` chain, expands `${section.key}` references and
refuses the build if any resulting flag is `-flto`. It reads platformio.ini
only; a platform default outside that file is beyond its reach.

Wholly-cold objects: this reads the per-object section table of every object
named in COLD_OBJECTS and fails the link if any of them still emits executable
`.text`. COMDAT (`G`) sections are excluded: those are out-of-line copies of
inline or virtual members that framework headers such as <SD.h> force into
whichever translation unit uses them. They are not definitions of the annotated
file, they are deduplicated at link time, and no edit inside an owned source
file can move them.

COLD_TREES makes the list non-optional: a new object compiled out of a cold
engine tree that nobody added to COLD_OBJECTS fails the build, so an unannotated
engine cannot land silently. It matches `build_src_filter` source paths by
prefix, so it reaches exactly what platformio.ini compiles into the src tree and
nothing else.

LDF-resolved libraries are the other half of the link and structurally outside
COLD_TREES' reach: PlatformIO lands their objects at
$BUILD_DIR/lib<hash>/<libname>/<file>.o, under a directory name no source path
predicts. The LDF gate closes that half by default instead of by enumeration --
every object matching that shape is grouped by <libname>, and a library that
emits any `.text` at all must be named in ITCM_ALLOWED_LDF_LIBS. A brand new
library, or a second source file inside a library that used to be wholly cold,
fails the build until somebody records the claim there. EXPECTED_LDF_LIBS is the
other direction: a library that stops appearing under that shape fails loudly
rather than quietly taking its coverage with it. aius_dsp_runtime_config -- the
library whose binder defaulting into ITCM regressed live ingress, and which used
to have a check naming its one object -- is held by this gate now, so a second
source file added beside it is covered too.

The mixed ingress object keeps only its five producer entry points in ITCM.
Initialisation, consumption, flush and diagnostics are epoch work in FLASH.

The optical hot object: `src/optical_main.cpp` defines the audio processor, so it
cannot join COLD_OBJECTS -- some of its ITCM is exactly what ITCM is for. Its
contents are pinned instead, by an exact per-symbol allowlist (HOT_SECTIONS plus
the two kinds no attribute can reach), so "this is legitimately hot" is an
enforced, reviewable claim rather than an opinion. Any other ITCM section in
that object fails the build and is named.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys
from typing import Iterable, Mapping


# Objects that must contribute zero ITCM, relative to $BUILD_DIR. Every function
# defined in these translation units carries FLASHMEM.
COLD_OBJECTS = (
    "lib/aius_clock/src/aius_clock.c.o",
    "lib/aius_audio/src/aius_audio_clock.cpp.o",
    "lib/aius_audio_service/src/aius_audio_service.c.o",
    "lib/aius_boot/src/aius_boot.c.o",
    "lib/aius_eac3/src/aius_eac3.c.o",
    "lib/aius_http/src/aius_http.c.o",
    "lib/aius_http/src/aius_http_transaction.c.o",
    "lib/aius_net/src/aius_net.c.o",
    "lib/aius_runtime/src/aius_runtime.c.o",
    "lib/aius_sd/src/aius_sd.c.o",
    "src/aius_boot_teensy.cpp.o",
    "src/aius_sd_teensy.cpp.o",
)

# Sections that are framework code the annotated source cannot reach. GCC's
# IPA-SRA clones a COMDAT member out of a framework header into a local,
# non-COMDAT `.isra` copy, which loses the COMDAT marking the blanket exclusion
# below keys on. `File::operator=(File const&)` is <FS.h>'s, pulled in by the SD
# adapter's `adapter->file = opened;`. Naming the exact section keeps the
# contract tight: any OTHER ITCM section in these objects still fails the build.
FRAMEWORK_CLONES = {
    "src/aius_sd_teensy.cpp.o": (".text._ZN4FileaSERKS_.isra.0",),
}

# Every linked object under one of these trees must appear in COLD_OBJECTS.
# Adding a source to one of these engines without annotating it is a build
# failure rather than silent ITCM.
COLD_TREES = (
    "lib/aius_clock/",
    "lib/aius_audio_service/",
    "lib/aius_boot/",
    "lib/aius_eac3/",
    "lib/aius_http/",
    "lib/aius_net/",
    "lib/aius_runtime/",
    "lib/aius_sd/",
)

# The one object allowed to contribute ITCM, and the exact set it may contribute.
HOT_OBJECT = "src/optical_main.cpp.o"

# 1. The software-audio-ISR functions, by exact section name. `update` is now
#    only a bounded ingress producer and `audio_ingress_publish_live` is its
#    exact 128-frame interleave walk. `codec_cycle_clock` is the target DWT read
#    bracketing each decoder work unit; keeping that 12-byte callback hot avoids
#    adding flash-call latency to every measurement. Decode, DSP, metering,
#    recording and the transmit-ring producer run from the epoch's AUDIO
#    checkpoint and carry FLASHMEM, so they no longer consume the ISR-hot bank.
#    `stall_witness_isr` is not per-block but must be ITCM anyway: it is the
#    priority-0 GPT1 heartbeat that classifies the ~1.79 s global-stall family,
#    and a FLASHMEM witness would itself stall on the FlexSPI bus holds it
#    exists to witness, corrupting the verdict.
HOT_SECTIONS = (
    ".text._ZL17codec_cycle_clockPv",
    ".text._ZL26audio_ingress_publish_livePtPK18audio_block_structS2_",
    ".text._ZN18AiusAudioProcessor6updateEv",
    ".text._ZL17stall_witness_isrv",
    ".text.aius_stall_witness_body",
)

INGRESS_HOT_OBJECT = "lib/aius_audio_ingress/src/aius_audio_ingress.c.o"
INGRESS_HOT_SECTIONS = (
    ".text.aius_audio_ingress_reserve",
    ".text.aius_audio_ingress_commit",
    ".text.aius_audio_ingress_cancel_gap",
    ".text.aius_audio_ingress_record_gap",
    ".text.aius_audio_ingress_record_source_change",
)

# An object PlatformIO's library dependency finder compiled, by the only thing
# that identifies one: the `lib<hash>/<libname>/` prefix the LDF gives each
# resolved library's build directory. The hash is derived from the library's
# path, so it is neither stable nor predictable from any source path -- which is
# exactly why COLD_TREES cannot see these objects.
LDF_OBJECT = re.compile(r"^lib[0-9a-fA-F]{3,}/([^/]+)/.+\.o$")

# Every LDF library the two optical environments resolve. This is a presence
# assertion, not a permission: a name that disappears from the link (renamed
# library, dropped lib_deps entry, a lib_ignore addition) fails the build instead
# of silently removing whatever coverage the entry carried. Adding a library here
# without also deciding its ITCM_ALLOWED_LDF_LIBS status is not possible -- the
# gate below is the default-deny half.
EXPECTED_LDF_LIBS = frozenset({
    "FNET",
    "NativeEthernet",
    "SD",
    "SPI",
    "SdFat",
    "aius_a52",
    "aius_auth",
    "aius_boot",
    "aius_codec_api",
    "aius_config",
    "aius_dsp",
    "aius_dsp_config",
    "aius_dsp_runtime_config",
    "aius_dts",
    "aius_iec61937",
    "aius_json",
    "aius_pipeline",
    "aius_recorder",
    "aius_replay",
    "aius_usb_control",
})

# LDF libraries permitted to emit `.text`. ADDING AN ENTRY HERE REMOVES THE WHOLE
# LIBRARY FROM THE GATE, present sources and future ones alike -- it is not a
# per-object or per-function exemption, and nothing else in this file will notice
# a new hot function inside an allowlisted library. Prefer FLASHMEM on the
# definitions; reach for this table only when the library's ITCM is a fact about
# the build rather than a choice the source can make, and say which of the three
# kinds below it is.
ITCM_ALLOWED_LDF_LIBS = {
    # 1. Third-party code. Not ours to annotate, and its ITCM is the standing
    #    cost of depending on it rather than a regression this guard can price.
    "FNET": "vendored network stack; no FLASHMEM annotation to give",
    "NativeEthernet": "vendored Ethernet driver; same",
    "SD": "framework library",
    "SPI": "framework library",
    "SdFat": "framework library",
    # 2. An LDF copy of a source platformio.ini ALSO compiles through
    #    build_src_filter. The archive member loses to the src-tree object at
    #    link time, so its `.text` never reaches the image; the copy that does is
    #    covered by COLD_OBJECTS or by one of the pinned hot-object checks.
    "aius_a52": "duplicate of the build_src_filter aius_a52 objects",
    "aius_iec61937": "duplicate of the build_src_filter aius_iec61937 object",
    "aius_pipeline": "duplicate of the build_src_filter aius_pipeline object",
    "aius_replay": "duplicate of the build_src_filter aius_replay object",
    # 3. Linked out of the archive and genuinely emitting ITCM today. These
    #    predate the placement contract and were never under any check; the entry
    #    records that, and removing one is the unit of work that brings a library
    #    under the gate.
    "aius_codec_api": "arena allocator; unannotated",
    "aius_dsp": "aius_measure.c is live here and its producer block is hot",
    "aius_dts": "DTS adapter; unannotated",
    # aius_usb_control left this table 2026-08-16: every definition is now
    # FLASHMEM (funding the OTA dark-stamp's FASTRUN additions,
    # aius-ota-resilience.md Phase 1), so the library is under the gate.
    # aius_recorder, aius_config and aius_auth left it 2026-09-02: every
    # definition in aius_recorder.c, aius_recorder_bank.c, aius_config.c and
    # aius_auth.c is now FLASHMEM. The recorder's "producer" is a writer
    # context, not an ISR -- it is reached from AiusAudioProcessor::enqueue(),
    # itself FLASHMEM -- so none of the three had an ITCM claim to make. The
    # named RECORDER_COLD_SECTIONS check that used to pin the recorder's one
    # bring-up helper retired with them: the generic gate now holds the whole
    # library to zero `.text`.
}

# 2. The dynamic-initialization thunk GCC emits for the file's statics. It has
#    no declaration to annotate and GCC emits it into `.text.startup.*`
#    regardless, so no edit inside the file can move it; it is admitted by kind.
#    GCC names it after an arbitrary symbol of the translation unit, so the
#    match is the prefix rather than the full name; if that prefix ever stops
#    describing the build, the thunk falls through to the offender check and is
#    named there. It grows when a new dynamically-initialized static appears
#    rather than producing a new section, so its size is reported on the PASS
#    line instead of being enforced here.
STATIC_INIT_PREFIX = ".text.startup._GLOBAL__sub_I_"

# 3. COMDAT members of the framework classes <SD.h> forces into whichever
#    translation unit uses it -- the same exclusion COLD_OBJECTS gets, narrowed
#    to the classes actually involved so an unrelated framework type still
#    fails. Keyed on the mangled class name rather than a full member list
#    because member churn across a Teensy core or SD library upgrade is not this
#    file's to control, whereas which classes it touches is.
FRAMEWORK_COMDAT_CLASSES = (
    "_ZN4File",                        # SD.h's File
    "_ZN6SDFile",                      # SD.h's SDFile
    "_ZN10StreamFileI10FsBaseFileyE",  # SdFat's StreamFile<FsBaseFile, uint64_t>
    "_ZN5Print",                       # Print, File's base
)

TEXT_PREFIX = ".text."

SECTION_LINE = re.compile(
    r"^\s*\[\s*\d+\]\s+"
    r"(\.text\S*)\s+"          # name
    r"PROGBITS\s+"             # type
    r"[0-9a-fA-F]+\s+"         # address
    r"[0-9a-fA-F]+\s+"         # file offset
    r"([0-9a-fA-F]+)\s+"       # size
    r"[0-9a-fA-F]+\s+"         # entry size
    r"(\S+)\s"                 # flags
)

# The OTA dark-window call graph, pinned in the LINKED image. The 2026-08-01
# incident's second finding (aius-ota-upgrade.md §Incident): this core's
# FASTRUN is section placement ONLY -- no noinline -- so GCC folded the
# single-caller FASTRUN run-to-completion loop into its FLASHMEM caller and
# the dark window executed FROM THE FLASH BEING ERASED. Section-table checks
# on the .o cannot see that (the inlined copy has no section of its own), so
# this check reads the linked ELF's symbol table: every name below must EXIST
# as a FUNC and its address must be ITCM. A symbol that vanished was inlined
# into some caller -- wherever that caller lives, the guarantee is gone.
# C++ file-statics are matched through their _ZL<len><name> mangling, length
# prefix included, so `ota_flash_read` cannot swallow
# `ota_flash_read_inject_head_fault`.
DARK_WINDOW_SYMBOLS = (
    # optical_main.cpp: the window drivers and both run loops.
    "ota_park_interrupt_sources",
    "ota_restore_interrupt_sources",
    "ota_quiesce",
    "ota_activate_run_and_finalize",
    "ota_rehearse_run",
    # optical_main.cpp: the primitives the sequencer calls through pointers.
    "ota_flash_erase",
    "ota_flash_program",
    "ota_flash_read",
    "ota_flash_read_inject_head_fault",
    "ota_wdt_feed",
    "ota_flash_cycles",
    "ota_health_read",
    "ota_health_write",
    "ota_rehearsal_health_read",
    "ota_rehearsal_health_write",
    # lib/aius_ota: the in-window step entry and the published bounds.
    "aius_ota_activate_step",
    "aius_ota_activate_check_erase_in",
    "aius_ota_activate_check_span_in",
)
# ITCM occupies the bottom of the address map; DTCM starts at 0x20000000 and
# flash at 0x60000000, so anything at or above this bound is not ITCM code.
ITCM_ADDRESS_LIMIT = 0x00080000

# platformio.ini, read only to prove the flag that would blind every per-object
# check above is absent. `${section.key}` is PlatformIO's interpolation syntax;
# INI_REF finds one occurrence of it.
INI_SECTION = re.compile(r"^\[([^\]]+)\]$")
INI_REF = re.compile(r"\$\{([^}]+)\}")
INI_REF_DEPTH = 8
LTO_FLAG_PREFIX = "-flto"


class VerificationError(RuntimeError):
    """A cold object contributes ITCM, or the hot object contributes more than
    its pinned allowlist."""


def parse_text_sections(output: str) -> dict[str, tuple[int, bool]]:
    """Every non-empty `.text*` section: name -> (size, is_comdat)."""
    sections: dict[str, tuple[int, bool]] = {}
    for line in output.splitlines():
        match = SECTION_LINE.match(line)
        if not match:
            continue
        name, size_hex, flags = match.group(1), match.group(2), match.group(3)
        size = int(size_hex, 16)
        if size == 0:
            continue
        sections[name] = (size, "G" in flags)
    return sections


def parse_itcm_sections(
    output: str,
    allowed: Iterable[str] = (),
) -> tuple[dict[str, int], int, int]:
    """Split `.text*` into owned ITCM, COMDAT bytes and framework-clone bytes."""
    allowed = set(allowed)
    itcm: dict[str, int] = {}
    comdat_bytes = 0
    clone_bytes = 0
    for name, (size, comdat) in parse_text_sections(output).items():
        if comdat:
            comdat_bytes += size
        elif name in allowed:
            clone_bytes += size
        else:
            itcm[name] = size
    return itcm, comdat_bytes, clone_bytes


def parse_hot_sections(
    output: str,
) -> tuple[dict[str, int], dict[str, int], int, dict[str, int]]:
    """Split the hot object's `.text*` into the three admitted kinds plus the
    offenders: (audio-block, static-init thunk, framework COMDAT bytes, rest)."""
    audio: dict[str, int] = {}
    startup: dict[str, int] = {}
    framework_bytes = 0
    offenders: dict[str, int] = {}
    for name, (size, comdat) in parse_text_sections(output).items():
        mangled = name[len(TEXT_PREFIX):] if name.startswith(TEXT_PREFIX) else name
        if name in HOT_SECTIONS:
            audio[name] = size
        elif name.startswith(STATIC_INIT_PREFIX):
            startup[name] = size
        elif comdat and mangled.startswith(FRAMEWORK_COMDAT_CLASSES):
            framework_bytes += size
        else:
            offenders[name] = size
    return audio, startup, framework_bytes, offenders


def read_sections(obj: Path, readelf: str) -> str:
    result = subprocess.run(
        [readelf, "-SW", str(obj)],
        check=True,
        text=True,
        capture_output=True,
    )
    return result.stdout


def inspect_object(
    obj: Path,
    readelf: str,
    allowed: Iterable[str] = (),
) -> tuple[dict[str, int], int, int]:
    return parse_itcm_sections(read_sections(obj, readelf), allowed)


def verify_declared_set(linked: Iterable[str]) -> None:
    linked = set(linked)
    missing = [
        name
        for name in COLD_OBJECTS + (HOT_OBJECT, INGRESS_HOT_OBJECT)
        if name not in linked
    ]
    if missing:
        raise VerificationError(
            f"declared objects were not linked (renamed or dropped?): {missing}; "
            "update COLD_OBJECTS / HOT_OBJECT in "
            "test/itcm-placement/verify_target.py"
        )
    undeclared = sorted(
        name for name in linked
        if name.startswith(COLD_TREES) and name not in COLD_OBJECTS
    )
    if undeclared:
        raise VerificationError(
            f"objects in a cold engine tree are not declared cold: {undeclared}; "
            "put FLASHMEM on every definition in those sources and add them to "
            "COLD_OBJECTS in test/itcm-placement/verify_target.py"
        )


def verify_placement(
    build_dir: Path,
    linked: Iterable[str],
    readelf: str,
) -> tuple[int, int, int]:
    verify_declared_set(linked)
    offenders: dict[str, Mapping[str, int]] = {}
    comdat_total = 0
    clone_total = 0
    for name in COLD_OBJECTS:
        obj = build_dir / name
        if not obj.is_file():
            raise VerificationError(f"cold object is missing from the build: {obj}")
        itcm, comdat, clones = inspect_object(
            obj, readelf, FRAMEWORK_CLONES.get(name, ())
        )
        comdat_total += comdat
        clone_total += clones
        if itcm:
            offenders[name] = itcm
    if offenders:
        detail = "; ".join(
            f"{name}: {sum(sections.values())} bytes in "
            f"{sorted(sections)}"
            for name, sections in sorted(offenders.items())
        )
        raise VerificationError(
            f"cold objects still emit ITCM code: {detail}. "
            "ITCM belongs to code that runs per audio block; put FLASHMEM on "
            "the definition (not a prototype -- GCC does not merge a `section` "
            "attribute from a forward declaration) of every function listed"
        )
    return len(COLD_OBJECTS), comdat_total, clone_total


def verify_hot_object(build_dir: Path, readelf: str) -> tuple[int, int, int]:
    """The hot object may contribute ITCM, but only its pinned allowlist."""
    obj = build_dir / HOT_OBJECT
    if not obj.is_file():
        raise VerificationError(f"hot object is missing from the build: {obj}")
    audio, startup, framework, offenders = parse_hot_sections(
        read_sections(obj, readelf)
    )
    if offenders:
        detail = "; ".join(
            f"{name}: {size} bytes" for name, size in sorted(offenders.items())
        )
        raise VerificationError(
            f"{HOT_OBJECT} emits ITCM code outside its pinned hot set: {detail}. "
            "ITCM belongs to code that runs per audio block; put FLASHMEM on the "
            "definition (not a prototype -- GCC does not merge a `section` "
            "attribute from a forward declaration) of every function listed. If "
            "one really does run per block, add it to HOT_SECTIONS in "
            "test/itcm-placement/verify_target.py with that justification"
        )
    missing = [name for name in HOT_SECTIONS if name not in audio]
    if missing:
        raise VerificationError(
            f"{HOT_OBJECT} no longer emits its audio-block code: {missing}. "
            "The hot set has been renamed, inlined away, or wrongly given "
            "FLASHMEM; update HOT_SECTIONS in "
            "test/itcm-placement/verify_target.py to match"
        )
    return sum(audio.values()), sum(startup.values()), framework


def verify_ingress_hot_object(build_dir: Path, readelf: str) -> int:
    """The mixed ingress object keeps only producer entry points in ITCM."""
    obj = build_dir / INGRESS_HOT_OBJECT
    if not obj.is_file():
        raise VerificationError(
            f"ingress hot object is missing from the build: {obj}"
        )
    sections, comdat, clones = inspect_object(obj, readelf)
    if comdat or clones:
        raise VerificationError(
            f"{INGRESS_HOT_OBJECT} unexpectedly emits COMDAT/clone code"
        )
    offenders = {
        name: size
        for name, size in sections.items()
        if name not in INGRESS_HOT_SECTIONS
    }
    if offenders:
        raise VerificationError(
            f"{INGRESS_HOT_OBJECT} emits ITCM outside its producer set: "
            + ", ".join(
                f"{name}: {size} bytes"
                for name, size in sorted(offenders.items())
            )
        )
    missing = [
        name for name in INGRESS_HOT_SECTIONS if name not in sections
    ]
    if missing:
        raise VerificationError(
            f"{INGRESS_HOT_OBJECT} no longer emits producer code: {missing}"
        )
    return sum(sections.values())


def parse_function_symbols(output: str) -> dict[str, int]:
    """FUNC symbols from `readelf -sW` on the linked ELF: name -> st_value.
    The Thumb bit rides in the value and is irrelevant to a bank compare."""
    symbols: dict[str, int] = {}
    for line in output.splitlines():
        parts = line.split()
        if len(parts) < 8 or parts[3] != "FUNC":
            continue
        try:
            value = int(parts[1], 16)
        except ValueError:
            continue
        symbols.setdefault(parts[7], value)
    return symbols


def dark_window_symbol_address(symbols: dict[str, int], name: str):
    """The address of `name` -- a plain C symbol, or a C++ file-static through
    its _ZL<len><name> mangling. The length prefix is the disambiguator: it
    makes the match exact-name, never prefix (ota_flash_read's 14 cannot match
    the inject wrapper's 32). None when the symbol does not exist at all."""
    if name in symbols:
        return symbols[name]
    mangled = f"_ZL{len(name)}{name}"
    for symbol, value in symbols.items():
        if symbol.startswith(mangled):
            return value
    return None


def verify_dark_window_symbols(symbols: dict[str, int]) -> int:
    """Every dark-window function exists as its own FUNC symbol AND lives in
    ITCM. Pure half, driven by the suite over fabricated symbol tables."""
    missing = []
    misplaced = []
    for name in DARK_WINDOW_SYMBOLS:
        value = dark_window_symbol_address(symbols, name)
        if value is None:
            missing.append(name)
        elif value >= ITCM_ADDRESS_LIMIT:
            misplaced.append(f"{name} at 0x{value:08x}")
    if missing or misplaced:
        details = []
        if missing:
            details.append(
                "inlined away (no symbol): " + ", ".join(sorted(missing))
            )
        if misplaced:
            details.append("outside ITCM: " + ", ".join(sorted(misplaced)))
        raise VerificationError(
            "dark-window residency: " + "; ".join(details) + ". FASTRUN on "
            "this core is section-only (no noinline); a vanished symbol was "
            "folded into its caller and executes from wherever THAT lives -- "
            "for a FLASHMEM caller, from the flash being erased "
            "(aius-ota-upgrade.md §Incident 2026-08-01). Restore "
            "AIUS_OTA_DARK_WINDOW_FN (FASTRUN + noinline) on the named "
            "function."
        )
    return len(DARK_WINDOW_SYMBOLS)


def verify_dark_window_residency(build_dir: Path, readelf: str) -> int:
    elf = build_dir / "firmware.elf"
    if not elf.exists():
        raise VerificationError(f"linked image not found: {elf}")
    result = subprocess.run(
        [readelf, "-sW", str(elf)],
        check=True,
        text=True,
        capture_output=True,
    )
    return verify_dark_window_symbols(parse_function_symbols(result.stdout))


# The ldscript pins whole vendored trees' .text to flash (TweetNaCl for the
# OTA crypto, the ffmpeg-eac3 fork for E-AC-3 decode -- E-AC-3 plan phase 4).
# An object-section check cannot see an ld-level pin (the .o still carries
# plain .text), so the LINKED image is the oracle: each anchor below must
# exist as a FUNC symbol and must NOT be ITCM. A regression that un-pins a
# tree moves its anchors with it and fails here; an anchor slimmer than the
# tree still catches the whole-tree regression, which is the only kind an
# ldscript edit can produce.
FLASH_PINNED_SYMBOLS = (
    # vendor/tweetnacl (previously enforced only by the six-block ASSERT,
    # which a <2 KiB miss could slip under). The linked name is the
    # ed25519_tweet-suffixed one; crypto_sign_open is its wrapper macro.
    "crypto_sign_ed25519_tweet_open",
    # vendor/ffmpeg-eac3: the fork's entry points and its table-driven CRC.
    "aius_eac3_core_init",
    "aius_eac3_core_frame",
    "av_crc",
    # lib/aius_eac3: FLASHMEM-annotated and COLD_OBJECTS-checked besides;
    # this anchor additionally proves the LINKED placement.
    "aius_eac3_decode_access_unit",
)


def verify_flash_pinned_symbols(symbols: dict[str, int]) -> int:
    """Every flash-pinned anchor exists and lives OUTSIDE ITCM. Pure half."""
    missing = []
    misplaced = []
    for name in FLASH_PINNED_SYMBOLS:
        value = dark_window_symbol_address(symbols, name)
        if value is None:
            missing.append(name)
        elif value < ITCM_ADDRESS_LIMIT:
            misplaced.append(f"{name} at 0x{value:08x}")
    if missing or misplaced:
        details = []
        if missing:
            details.append("absent from the link: " + ", ".join(sorted(missing)))
        if misplaced:
            details.append("inside ITCM: " + ", ".join(sorted(misplaced)))
        raise VerificationError(
            "flash-pinned residency: " + "; ".join(details) + ". The "
            "ldscript's vendor .text pins (link/aius_teensy41.ld .text.code) "
            "keep TweetNaCl and the ffmpeg-eac3 fork out of the six-block "
            "ITCM budget; restore the pin or the missing source."
        )
    return len(FLASH_PINNED_SYMBOLS)


def verify_flash_pinned_residency(build_dir: Path, readelf: str) -> int:
    elf = build_dir / "firmware.elf"
    if not elf.exists():
        raise VerificationError(f"linked image not found: {elf}")
    result = subprocess.run(
        [readelf, "-sW", str(elf)],
        check=True,
        text=True,
        capture_output=True,
    )
    return verify_flash_pinned_symbols(parse_function_symbols(result.stdout))


def group_ldf_objects(paths: Iterable[str]) -> dict[str, list[str]]:
    """LDF-shaped object paths grouped by library name.

    Paths are relative to $BUILD_DIR. Anything that is not
    `lib<hash>/<libname>/<file>.o` -- the src tree, the vendored core, the
    framework -- is not an LDF object and is left to the other checks.
    """
    groups: dict[str, list[str]] = {}
    for path in paths:
        match = LDF_OBJECT.match(path)
        if match:
            groups.setdefault(match.group(1), []).append(path)
    return {name: sorted(groups[name]) for name in sorted(groups)}


def verify_ldf_libraries(paths: Iterable[str], read) -> tuple[int, int]:
    """Default-deny gate over every LDF-resolved library in the link.

    `read` maps a $BUILD_DIR-relative object path to that object's `readelf -SW`
    output. A library outside ITCM_ALLOWED_LDF_LIBS must emit no `.text` at all:
    COMDAT and IPA-SRA clones are offenders here rather than exclusions, because
    an unreviewed library has no annotated definitions for them to be copies of.
    """
    groups = group_ldf_objects(paths)
    missing = sorted(EXPECTED_LDF_LIBS - set(groups))
    if missing:
        raise VerificationError(
            f"expected LDF libraries are absent from the link: {missing}; a "
            "renamed, ignored or dropped library takes its ITCM coverage with "
            "it. Update EXPECTED_LDF_LIBS in "
            "test/itcm-placement/verify_target.py to match the build"
        )
    offenders: dict[str, dict[str, int]] = {}
    for name, objects in groups.items():
        if name in ITCM_ALLOWED_LDF_LIBS:
            continue
        for path in objects:
            sections = {
                section: size
                for section, (size, _comdat) in parse_text_sections(
                    read(path)
                ).items()
            }
            if sections:
                offenders[path] = sections
    if offenders:
        detail = "; ".join(
            f"{path}: {sum(sections.values())} bytes in {sorted(sections)}"
            for path, sections in sorted(offenders.items())
        )
        raise VerificationError(
            f"LDF libraries emit ITCM code: {detail}. ITCM belongs to code that "
            "runs per audio block; put FLASHMEM on the definition (not a "
            "prototype -- GCC does not merge a `section` attribute from a "
            "forward declaration) of every function listed. If the library's "
            "ITCM is a fact about the build rather than a choice its source can "
            "make, add it to ITCM_ALLOWED_LDF_LIBS in "
            "test/itcm-placement/verify_target.py with that reason -- which "
            "exempts the WHOLE library, now and in future"
        )
    gated = sum(1 for name in groups if name not in ITCM_ALLOWED_LDF_LIBS)
    return len(groups), gated


def verify_ldf_libraries_in_build(
    build_dir: Path,
    readelf: str,
) -> tuple[int, int]:
    paths = [
        path.relative_to(build_dir).as_posix()
        for path in sorted(build_dir.glob("lib*/*/**/*.o"))
    ]
    return verify_ldf_libraries(
        paths, lambda path: read_sections(build_dir / path, readelf)
    )


def parse_ini_sections(text: str) -> dict[str, dict[str, str]]:
    """platformio.ini as {section: {key: value}}, continuation lines folded.

    Comment lines are dropped wherever they appear, including inside a folded
    multi-line value -- platformio.ini carries long `;` rationale blocks between
    build_flags entries, and a flag scan must not read them as flags.
    """
    sections: dict[str, dict[str, str]] = {}
    current: dict[str, str] | None = None
    key: str | None = None
    for raw in text.splitlines():
        stripped = raw.strip()
        if not stripped or stripped.startswith((";", "#")):
            continue
        header = INI_SECTION.match(stripped)
        if header:
            current = sections.setdefault(header.group(1), {})
            key = None
            continue
        if current is None:
            continue
        if raw[:1].isspace() and key is not None:
            current[key] += " " + stripped
            continue
        if "=" in stripped:
            name, _, value = stripped.partition("=")
            key = name.strip()
            current[key] = value.strip()
    return sections


def expand_ini_value(
    value: str,
    sections: Mapping[str, Mapping[str, str]],
    depth: int = 0,
) -> str:
    """Resolve `${section.key}` references. An unknown reference expands empty,
    which is what PlatformIO does with one this file cannot see."""
    if depth >= INI_REF_DEPTH:
        return value

    def resolve(match: re.Match[str]) -> str:
        section, _, key = match.group(1).rpartition(".")
        return expand_ini_value(
            sections.get(section, {}).get(key, ""), sections, depth + 1
        )

    return INI_REF.sub(resolve, value)


def environment_build_flags(
    sections: Mapping[str, Mapping[str, str]],
    environment: str,
) -> list[str]:
    """Every build flag `[env:<environment>]` inherits, in `extends` order."""
    name = f"env:{environment}"
    if name not in sections:
        raise VerificationError(
            f"platformio.ini has no [{name}] section, so the flags of the "
            "environment under inspection cannot be read"
        )
    chain: list[str] = []
    seen: set[str] = set()
    while name and name not in seen:
        seen.add(name)
        chain.append(name)
        parent = sections[name].get("extends", "").strip()
        if not parent:
            break
        name = parent if parent in sections else f"env:{parent}"
        if name not in sections:
            raise VerificationError(
                f"platformio.ini section [{chain[-1]}] extends {parent!r}, "
                "which does not exist"
            )
    flags: list[str] = []
    for section in chain:
        value = sections[section].get("build_flags")
        if value:
            flags.extend(expand_ini_value(value, sections).split())
    return flags


def verify_no_lto(ini_text: str, environment: str) -> int:
    """Link-time optimisation would defer placement past every per-object check
    in this file, so the guard refuses to certify a build that enables it."""
    flags = environment_build_flags(parse_ini_sections(ini_text), environment)
    found = [
        flag
        for flag in flags
        if flag == LTO_FLAG_PREFIX or flag.startswith(LTO_FLAG_PREFIX + "=")
    ]
    if found:
        raise VerificationError(
            f"environment {environment} builds with {found}. Every check in "
            "test/itcm-placement/verify_target.py reads one object file at a "
            "time, and link-time optimisation moves the ITCM/FLASHMEM decision "
            "past the last point an object file records it -- so every object "
            "would read as innocent while ITCM grew. Drop the flag, or replace "
            "these checks with ones that read the linked image"
        )
    return len(flags)


def read_platformio_ini(project_dir: Path) -> str:
    ini = project_dir / "platformio.ini"
    try:
        return ini.read_text()
    except OSError as error:
        raise VerificationError(
            f"cannot read {ini}, so -flto cannot be ruled out: {error}"
        ) from error


def _summary(count: int, comdat: int, clones: int) -> str:
    return (
        f"cold-object ITCM placement: PASS ({count} objects, 0 owned ITCM bytes, "
        f"{comdat} COMDAT + {clones} framework-clone bytes excluded)"
    )


def _hot_summary(audio: int, startup: int, framework: int) -> str:
    return (
        f"hot-object ITCM placement: PASS ({HOT_OBJECT}, {audio} audio-block "
        f"bytes in {len(HOT_SECTIONS)} pinned sections, {startup} static-init "
        f"thunk + {framework} framework COMDAT bytes excluded)"
    )


def _ingress_hot_summary(producer: int) -> str:
    return (
        "ingress-object ITCM placement: PASS "
        f"({INGRESS_HOT_OBJECT}, {producer} producer bytes in "
        f"{len(INGRESS_HOT_SECTIONS)} pinned sections)"
    )


def _dark_window_summary(checked: int) -> str:
    return (
        "dark-window ITCM residency: PASS "
        f"({checked} OTA symbols present in the linked image, all ITCM)"
    )


def _flash_pinned_summary(checked: int) -> str:
    return (
        "flash-pinned residency: PASS "
        f"({checked} vendor/adapter anchors in the linked image, none ITCM)"
    )


def _ldf_summary(libraries: int, gated: int) -> str:
    return (
        "LDF-library ITCM placement: PASS "
        f"({libraries} libraries, {gated} gated to 0 ITCM bytes, "
        f"{libraries - gated} allowlisted)"
    )


def _lto_summary(environment: str, flags: int) -> str:
    return (
        f"per-object visibility: PASS ({environment}, no -flto in "
        f"{flags} build flags)"
    )


def _resolve_readelf(env) -> str:
    readelf = env.WhereIs("arm-none-eabi-readelf")
    if not readelf:
        raise VerificationError("arm-none-eabi-readelf is unavailable")
    return str(readelf)


def verify_cold_placement(source, target, env) -> None:
    del target
    build_dir = Path(env.subst("$BUILD_DIR")).resolve()
    linked = []
    for node in source:
        path = Path(str(node)).resolve()
        try:
            linked.append(path.relative_to(build_dir).as_posix())
        except ValueError:
            continue
    environment = env.subst("$PIOENV")
    project_dir = Path(env.subst("$PROJECT_DIR")).resolve()
    print(_lto_summary(
        environment,
        verify_no_lto(read_platformio_ini(project_dir), environment),
    ))
    readelf = _resolve_readelf(env)
    print(_summary(*verify_placement(build_dir, linked, readelf)))
    print(_hot_summary(*verify_hot_object(build_dir, readelf)))
    print(_ingress_hot_summary(verify_ingress_hot_object(build_dir, readelf)))
    print(_ldf_summary(*verify_ldf_libraries_in_build(build_dir, readelf)))
    print(_dark_window_summary(
        verify_dark_window_residency(build_dir, readelf)
    ))
    print(_flash_pinned_summary(
        verify_flash_pinned_residency(build_dir, readelf)
    ))


def _main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("build_dir", type=Path)
    parser.add_argument(
        "--readelf",
        default="arm-none-eabi-readelf",
        help="readelf-compatible executable",
    )
    parser.add_argument(
        "--environment",
        help="PlatformIO environment (default: the build directory's name)",
    )
    parser.add_argument(
        "--project-dir",
        type=Path,
        help="directory holding platformio.ini (default: derived from build_dir)",
    )
    args = parser.parse_args(argv)
    build_dir = args.build_dir.resolve()
    # $BUILD_DIR is <project>/.pio/build/<environment> unless somebody moved it.
    environment = args.environment or build_dir.name
    project_dir = (
        args.project_dir.resolve() if args.project_dir else build_dir.parents[2]
    )
    try:
        flags = verify_no_lto(read_platformio_ini(project_dir), environment)
    except (OSError, VerificationError) as error:
        print(f"per-object visibility: FAIL: {error}", file=sys.stderr)
        return 1
    print(_lto_summary(environment, flags))
    linked = [
        path.relative_to(build_dir).as_posix()
        for path in sorted(build_dir.glob("**/*.o"))
    ]
    try:
        summary = verify_placement(build_dir, linked, args.readelf)
    except (OSError, subprocess.CalledProcessError, VerificationError) as error:
        print(f"cold-object ITCM placement: FAIL: {error}", file=sys.stderr)
        return 1
    print(_summary(*summary))
    try:
        hot_summary = verify_hot_object(build_dir, args.readelf)
    except (OSError, subprocess.CalledProcessError, VerificationError) as error:
        print(f"hot-object ITCM placement: FAIL: {error}", file=sys.stderr)
        return 1
    print(_hot_summary(*hot_summary))
    try:
        ingress = verify_ingress_hot_object(build_dir, args.readelf)
    except (OSError, subprocess.CalledProcessError, VerificationError) as error:
        print(f"ingress-object ITCM placement: FAIL: {error}", file=sys.stderr)
        return 1
    print(_ingress_hot_summary(ingress))
    try:
        ldf = verify_ldf_libraries_in_build(build_dir, args.readelf)
    except (OSError, subprocess.CalledProcessError, VerificationError) as error:
        print(f"LDF-library ITCM placement: FAIL: {error}", file=sys.stderr)
        return 1
    print(_ldf_summary(*ldf))
    try:
        dark = verify_dark_window_residency(build_dir, args.readelf)
    except (OSError, subprocess.CalledProcessError, VerificationError) as error:
        print(f"dark-window ITCM residency: FAIL: {error}", file=sys.stderr)
        return 1
    print(_dark_window_summary(dark))
    return 0


try:
    Import("env")  # type: ignore[name-defined]  # PlatformIO/SCons injection
except NameError:
    env = None

if env is not None and env.subst("$PIOENV") in {
    "teensy41-optical",
    "teensy41-optical-fault",
}:
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", verify_cold_placement)


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv[1:]))
