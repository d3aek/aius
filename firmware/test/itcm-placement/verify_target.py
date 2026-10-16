#!/usr/bin/env python3
"""Post-link proof that ITCM holds only code that runs per audio block.

On Teensy 4.1 a function with no FLASHMEM attribute lands in ITCM, and ITCM
shares the 512 KB FlexRAM with DTCM -- so cold code in ITCM is taken straight
out of the audio path's headroom. ITCM is reserved for the bounded producer code
that must preempt the serial AUDIO epoch. Epoch processing remains FLASHMEM even
when it runs per block.

Six checks: the no-LTO precondition, then the cold, optical composition, mixed
ingress-producer, producer-yield and LDF-library translation-unit shapes.

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
    "src/aius_sd_free_scan.cpp.o",
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

# The producer-yield object (aius-sample-rate-following.md Phase 4e) is the
# third mixed object: its guard, the SD wait hook body, the checkpoint yield
# point, the regular-visit pair and the nested-pass ledger wrapper run on every
# SD wait and checkpoint boundary during live input, so they stay in ITCM (no
# attribute); configure, the window pair and the diag pair are FLASHMEM. It is
# compiled through build_src_filter (lib_ignore'd, so never LDF-shaped) and is
# outside COLD_TREES, so without this pin no other check would see it. The
# pinned set is by section name with GCC's clone suffixes (.constprop.N,
# .isra.N, .part.N) stripped; the static nested pass may also be inlined away,
# so only the five public entry points are required. Any other ITCM section in
# the object fails and is named.
PRODUCER_YIELD_HOT_OBJECT = "lib/aius_producer_yield/src/aius_producer_yield.c.o"
PRODUCER_YIELD_HOT_SECTIONS = (
    ".text.aius_producer_yield_guard",
    ".text.aius_producer_yield_wait",
    ".text.aius_producer_yield_point",
    ".text.aius_producer_yield_visit_begin",
    ".text.aius_producer_yield_visit_end",
)
PRODUCER_YIELD_OPTIONAL_SECTIONS = (
    ".text.nested_pass",
    ".text.producer_leave",
    ".text.segment_close",
)
CLONE_SUFFIX = re.compile(r"(\.(constprop|isra|part)\.\d+)+$")

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
    # The A/B slot logic and the SD write-behind overlay (Phase 4h): both are
    # linked by every optical env, gated to 0 ITCM bytes, never allowlisted.
    "aius_ab",
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
    "aius_sd_overlay",
    "aius_usb_control",
    # The PSRAM web cache (Phase 4g): resolved by LDF in every optical env,
    # gated (every definition FLASHMEM, no `.text`), never allowlisted.
    "aius_web_cache",
})

# Libraries retired from the firmware (Phase 4i: replay). A link that still
# carries one fails outright, whatever its ITCM: the removal is the contract.
RETIRED_LDF_LIBS = frozenset({"aius_replay"})

# No linked symbol of a retired feature may survive in an optical image.
RETIRED_SYMBOL_PATTERN = re.compile(r"replay", re.IGNORECASE)

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

# The S/PDIF transmit path's ITCM code, pinned in the LINKED image by exact
# symbol name, with its bytes reported against ITCM headroom
# (aius-sample-rate-following.md Phase 4). lib/aius_audio/src/
# aius_spdif_sources.cpp.o is neither cold nor the pinned hot object, so no
# section check above sees it; and FASTRUN emits `.fastrun`, not `.text.*`, so
# the linked symbol table is the only oracle.
TX_ITCM_SYMBOLS = (
    # AiusSpdifTx24Hook::isr (anonymous namespace): the TX DMA vector, with the
    # period-boundary handshake (the trim kind's stamp included) inlined.
    "_ZN12_GLOBAL__N_117AiusSpdifTx24Hook3isrEv",
    # FASTRUN + noinline: the gate open's one-SADDR position read, called
    # inside its masked section, so a flash copy would run with interrupts
    # masked across a FlexSPI fetch and a folded copy would lose the pin.
    "aius_spdif_tx24_dma_read_position",
)

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


def verify_producer_yield_sections(sections: Mapping[str, int]) -> int:
    """Pure half: `sections` maps the object's non-COMDAT `.text*` names to
    sizes. Returns the pinned ITCM bytes."""
    allowed = set(PRODUCER_YIELD_HOT_SECTIONS) | set(
        PRODUCER_YIELD_OPTIONAL_SECTIONS
    )
    offenders = {
        name: size
        for name, size in sections.items()
        if CLONE_SUFFIX.sub("", name) not in allowed
    }
    if offenders:
        raise VerificationError(
            f"{PRODUCER_YIELD_HOT_OBJECT} emits ITCM outside its pinned "
            "producer-yield set: "
            + ", ".join(
                f"{name}: {size} bytes"
                for name, size in sorted(offenders.items())
            )
            + ". Put FLASHMEM on the definition, or add the section to "
            "PRODUCER_YIELD_HOT_SECTIONS in test/itcm-placement/"
            "verify_target.py if it runs on every SD wait"
        )
    present = {CLONE_SUFFIX.sub("", name) for name in sections}
    missing = [
        name for name in PRODUCER_YIELD_HOT_SECTIONS if name not in present
    ]
    if missing:
        raise VerificationError(
            f"{PRODUCER_YIELD_HOT_OBJECT} no longer emits its producer-yield "
            f"code: {missing} (renamed, inlined away or given FLASHMEM)"
        )
    return sum(sections.values())


def verify_producer_yield_object(build_dir: Path, readelf: str) -> int:
    obj = build_dir / PRODUCER_YIELD_HOT_OBJECT
    if not obj.is_file():
        raise VerificationError(
            f"producer-yield object is missing from the build: {obj}"
        )
    sections, comdat, clones = inspect_object(obj, readelf)
    if comdat or clones:
        raise VerificationError(
            f"{PRODUCER_YIELD_HOT_OBJECT} unexpectedly emits COMDAT/clone code"
        )
    return verify_producer_yield_sections(sections)


def _producer_yield_summary(pinned: int) -> str:
    return (
        "producer-yield ITCM placement: PASS "
        f"({PRODUCER_YIELD_HOT_OBJECT}, {pinned} bytes in the pinned set)"
    )


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


def parse_function_sizes(output: str) -> dict[str, tuple[int, int]]:
    """FUNC symbols from `readelf -sW` on the linked ELF: name -> (st_value,
    st_size). readelf prints the size in decimal, or in hex with 0x past
    99999; int(..., 0) reads both."""
    symbols: dict[str, tuple[int, int]] = {}
    for line in output.splitlines():
        parts = line.split()
        if len(parts) < 8 or parts[3] != "FUNC":
            continue
        try:
            value = int(parts[1], 16)
            size = int(parts[2], 0)
        except ValueError:
            continue
        symbols.setdefault(parts[7], (value, size))
    return symbols


def verify_tx_itcm_symbols(
    symbols: Mapping[str, tuple[int, int]],
) -> dict[str, int]:
    """Every TX_ITCM_SYMBOLS name exists as a FUNC in ITCM. Pure half:
    returns name -> bytes for the PASS line."""
    missing = [name for name in TX_ITCM_SYMBOLS if name not in symbols]
    misplaced = [
        f"{name} at 0x{symbols[name][0]:08x}"
        for name in TX_ITCM_SYMBOLS
        if name in symbols and symbols[name][0] >= ITCM_ADDRESS_LIMIT
    ]
    if missing or misplaced:
        details = []
        if missing:
            details.append(
                "absent (renamed or inlined away): " + ", ".join(sorted(missing))
            )
        if misplaced:
            details.append("outside ITCM: " + ", ".join(sorted(misplaced)))
        raise VerificationError(
            "TX ITCM residency: " + "; ".join(details) + ". The TX DMA ISR "
            "and aius_spdif_tx24_dma_read_position (FASTRUN + noinline, "
            "lib/aius_audio/src/aius_spdif_sources.cpp) must run from ITCM; "
            "restore the attribute, or update TX_ITCM_SYMBOLS in "
            "test/itcm-placement/verify_target.py if the symbol was renamed"
        )
    return {name: symbols[name][1] for name in TX_ITCM_SYMBOLS}


def verify_tx_itcm_residency(build_dir: Path, readelf: str) -> dict[str, int]:
    elf = build_dir / "firmware.elf"
    if not elf.exists():
        raise VerificationError(f"linked image not found: {elf}")
    result = subprocess.run(
        [readelf, "-sW", str(elf)],
        check=True,
        text=True,
        capture_output=True,
    )
    return verify_tx_itcm_symbols(parse_function_sizes(result.stdout))


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


# THE STEP-LOOP CLOSURE (aius-sample-rate-following.md Phase 4z). Residency
# of the named symbols is not enough: 57977135e472's ota_flash_read lived in
# ITCM and still called FLASHMEM aius_ab_remap_for_slot through a linker
# veneer, whose literal loads refilled a canonical D-cache line the identity
# copy then read stale. So this walks the linked image's disassembly: from the
# roots below plus every function src/optical_main.cpp assigns into an
# activation primitives table, every direct branch (bl, b, b.w, conditional
# and cbz forms) is followed to its symbol, and the build fails when one
# targets anything outside ITCM [0, _etext), any `__*_veneer` stub included,
# or when any literal-pool word in the closure names flash. Indirect calls
# (`blx rN` through `primitives` and `ctx`) are out of reach by construction;
# each primitive is a root here instead. ab_activation_mark_pending is the
# named exception the plan records: it runs after all slot programming and is
# not reached from these roots.
STEP_LOOP_ROOTS = (
    "aius_ota_activate_step",
    "ota_flash_read",
    "ota_flash_program",
    "ota_flash_erase",
)
PRIMITIVES_SOURCE = "src/optical_main.cpp"
PRIMITIVES_HEADER = "lib/aius_ota/include/aius_ota_activate.h"
PRIMITIVES_STRUCT = "AiusOtaActivatePrimitives"
# Every `primitives.<field> = <rhs>` line, whatever its right-hand side.
PRIMITIVE_ASSIGNMENT = re.compile(
    r"^[ \t]*primitives\.(\w+)[ \t]*=(?!=)(.*)$", re.MULTILINE
)
# The only accepted right-hand sides: `name;` or `&name;`.
PRIMITIVE_RHS = re.compile(r"^\s*&?\s*([A-Za-z_]\w*)\s*;\s*(?://.*)?$")
PRIMITIVE_NON_FUNCTIONS = frozenset({"nullptr", "NULL"})
FLASH_LITERAL_LOW = 0x60000000
FLASH_LITERAL_HIGH = 0x60FFFFFF
DTCM_LOW = 0x20000000
DTCM_HIGH = 0x20080000
# The DTCM copy of the running slot's remap ota_flash_read consults.
REMAP_SYMBOL = "g_ab_read_remap"
# Only the A/B slot envs compile the remap (AIUS_AB_SLOT != 0).
REMAP_ENVS = frozenset({"teensy41-optical-slot-a", "teensy41-optical-slot-b"})

DISASM_SYMBOL = re.compile(r"^([0-9a-f]+) <([^>]+)>:$")
DISASM_LINE = re.compile(r"^\s+([0-9a-f]+):\s+(\S+)(?:\s+(.*))?$")
DISASM_TARGET = re.compile(r"^(?:\S+,\s*)?([0-9a-f]+) <([^>+]+)(?:\+0x[0-9a-f]+)?>")
BRANCH_MNEMONIC = re.compile(
    r"^(?:bl|blx|b|cbn?z)"
    r"(?:eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?"
    r"(?:\.n|\.w)?$"
)
DISASM_COMMENT = re.compile(r"[@;]")

# aius-sample-rate-following.md Phase 4za (D2): the identity-window restore
# in ota_flash_read must leave no canonical line in the D-cache or the
# FlexSPI AHB buffer. The code addresses its registers as base plus offset
# (GPR32 is `str [r3, #128]` from a `.word 0x400ac000` literal, SCB cache ops
# `str [ip, #0xf74]` from `mov.w ip, #0xe000e000`), so the check resolves each
# load/store address by tracking register immediates (mov/movs/mov.w/movw/
# movt/mvn, register copies, add/sub of an immediate) and pc-relative literal
# loads, then adding the offset. The scan is linear in address order and
# ignores control flow: a value set before a loop label stays known inside
# the loop. Anything that writes a register it cannot model forgets it, so an
# unmodelled shape loses resolution and the check fails loudly rather than
# passing. LIMIT: being linear, the trace proves address order, not that
# every path runs the sequence; a forward branch could skip the pass or a
# barrier and still read as ordered, and an IT block makes a store
# conditional while it still reads as a plain store. So the check refuses,
# from the GPR32 restore store to the PRIMASK restore, any forward branch,
# any branch entering from outside, and any IT block or predicated
# instruction (the SWRESET poll is backward); the helper gets the same
# bans, and its two loops must each close on a branch fed by its own cmp.
# Register values flowing into the DCCISW and SWRESET stores are checked by
# a last-definition lookup in address order, which matches straight-line
# code and loop bodies like the built ones; any other shape fails. Inside
# the restore sequence a load or store through an unresolved base fails
# (sp-relative stack slots excepted), as does any msr or cpsie before the
# final PRIMASK restore. The three shapes 4za first accepted as loud on the
# device (N4) are caught too (Phase 4zc): a branch from inside the identity
# window (flip store on) past the PRIMASK restore, or a return there, which
# skips the whole restore; a poll whose backward branch is anything but
# `bne` after its `ands`/`tst` #1, so its exit is unconditional or
# inverted; and a backward branch from inside the restore sequence to
# before the restore store. Branches before the flip store (the span-check
# error exit) may cross the whole sequence: the window is not open there.
DCACHE_HELPER = "ota_dcache_clean_invalidate_all"
IDENTITY_READ_PATH = "ota_flash_read"
IOMUXC_GPR_GPR32_ADDRESS = 0x400AC080
FLEXSPI_MCR0_ADDRESS = 0x402A8000
SCB_DCCISW_ADDRESS = 0xE000EF74
SCB_DCISW_ADDRESS = 0xE000EF60
SCB_ICIALLU_ADDRESS = 0xE000EF50
# The helper's two loop bounds: set index stepped by the 32-byte line to
# 256 x 32, and the way count.
DCACHE_SET_SPAN = 8192
DCACHE_WAYS = 4
DCACHE_LINE_BYTES = 32
# The way index sits in DCCISW bits 31:30.
DCACHE_WAY_SHIFT = 30
CONDITION_CODES = "eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le"
IT_MNEMONIC = re.compile(r"^it[te]{0,3}$")
# A load/store carrying a condition suffix (`strne.w`, `ldrbeq`): objdump
# prints one inside an IT block, and a fixture may plant one without.
PREDICATED_MEMORY = re.compile(
    rf"^(?:ldr|str)(?:ex[bhd]?|s[bh]|[bhd]|[bh]?t)?(?:{CONDITION_CODES})"
    r"(?:\.w|\.n)?$"
)
CONDITIONAL_BRANCH = re.compile(rf"^b(?:{CONDITION_CODES})(?:\.w|\.n)?$")
FLAG_SETTING = re.compile(
    r"^(?:cmp|cmn|tst|teq|(?:mov|mvn|add|adc|sub|sbc|rsb|and|orr|eor|bic|orn"
    r"|lsl|lsr|asr|ror|rrx|mul)s)(?:\.w|\.n)?$"
)
REGISTER_ALIASES = {"sb": "r9", "sl": "r10", "fp": "r11", "ip": "r12"}
REGISTER_TOKEN = re.compile(r"\b(r1[0-5]|r[0-9]|sb|sl|fp|ip|sp|lr|pc)\b")
CALLER_SAVED = ("r0", "r1", "r2", "r3", "r12", "lr")
MEMORY_OPERAND = re.compile(
    r"^(?P<regs>[^\[]*)\[(?P<base>\w+)(?:,\s*#(?P<offset>-?(?:0x[0-9a-f]+|\d+)))?\]"
    r"(?P<writeback>!|,\s*#-?(?:0x[0-9a-f]+|\d+))?$"
)
IMMEDIATE_MOVE = re.compile(r"^(?:mov|movs|mvn|mvns|movw)(?:\.w|\.n)?$")
CONDITION_SUFFIX = re.compile(
    r"(?:eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)(?:\.n|\.w)?$"
)
NON_WRITING_MNEMONIC = re.compile(
    r"^(?:cmp|cmn|tst|teq|dsb|dmb|isb|msr|cpsi[de]|nop|it[te]*|bx|bkpt|svc"
    r"|wfi|wfe|sev|\.word|\.short|\.byte)(?:\.n|\.w)?(?:\s|$)"
)


def parse_primitive_assignments(source_text: str) -> list[tuple[str, str]]:
    """Every `primitives.<field> = <name>;` / `= &<name>;` as (field, name), in
    source order. Fails on any other right-hand side (ternary, designated
    initialiser, call, multi-line): the root set must not silently shrink."""
    found: list[tuple[str, str]] = []
    for match in PRIMITIVE_ASSIGNMENT.finditer(source_text):
        field, rhs = match.groups()
        parsed = PRIMITIVE_RHS.match(rhs)
        if not parsed:
            line = source_text.count("\n", 0, match.start()) + 1
            raise VerificationError(
                f"step-loop closure: {PRIMITIVES_SOURCE}:{line}: "
                f"`primitives.{field} ={rhs.rstrip()}` is not a bare "
                "identifier or `&identifier`; the closure walk cannot name "
                "its root. Assign through a plain function name."
            )
        found.append((field, parsed.group(1)))
    return found


def primitive_roots(source_text: str) -> list[str]:
    """Every function name assigned into an activation primitives table
    (`primitives.<field> = <fn>;`), in source order, deduplicated."""
    roots: list[str] = []
    for field, name in parse_primitive_assignments(source_text):
        if field == "ctx" or name in PRIMITIVE_NON_FUNCTIONS:
            continue
        if name not in roots:
            roots.append(name)
    return roots


def primitive_function_fields(header_text: str) -> list[str]:
    """Function-pointer field names of AiusOtaActivatePrimitives: fields whose
    type is a `typedef ... (*Name)(...)` in the same header."""
    text = re.sub(r"/\*.*?\*/", "", header_text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    fn_types = set(re.findall(r"\(\s*\*\s*(\w+)\s*\)\s*\(", text))
    struct = re.search(
        r"typedef\s+struct\s*(?:\w+\s*)?\{([^{}]*)\}\s*"
        + PRIMITIVES_STRUCT
        + r"\s*;",
        text,
    )
    if not struct:
        raise VerificationError(
            f"step-loop closure: struct {PRIMITIVES_STRUCT} not found in "
            f"{PRIMITIVES_HEADER}"
        )
    fields = [
        field
        for type_name, field in re.findall(
            r"(\w+)\s+(\w+)\s*;", struct.group(1)
        )
        if type_name in fn_types
    ]
    if not fields:
        raise VerificationError(
            f"step-loop closure: no function-pointer field parsed from "
            f"{PRIMITIVES_STRUCT}"
        )
    return fields


def verify_primitive_fields_assigned(
    source_text: str, header_text: str
) -> int:
    """Every function-pointer field of the primitives struct must be assigned
    in some parsed table. Returns the field count."""
    assigned = {field for field, _ in parse_primitive_assignments(source_text)}
    fields = primitive_function_fields(header_text)
    missing = [field for field in fields if field not in assigned]
    if missing:
        raise VerificationError(
            f"step-loop closure: {PRIMITIVES_STRUCT} function-pointer "
            f"field(s) never assigned in {PRIMITIVES_SOURCE}: "
            + ", ".join(missing)
            + "; their roots would be missing from the closure walk."
        )
    return len(fields)


def parse_disassembly(output: str) -> dict[str, dict]:
    """`objdump -d --no-show-raw-insn` -> symbol -> {start, branches, words,
    insns}. branches: (insn address, target address, target symbol); words:
    (address, value) of every `.word` the disassembler placed under the
    symbol; insns: (address, mnemonic, operands) of every line, comment
    (`@` from objdump, `;` in the fixtures) stripped."""
    functions: dict[str, dict] = {}
    current = None
    for line in output.splitlines():
        header = DISASM_SYMBOL.match(line)
        if header:
            current = functions.setdefault(
                header.group(2),
                {
                    "start": int(header.group(1), 16),
                    "branches": [],
                    "words": [],
                    "insns": [],
                },
            )
            continue
        if current is None:
            continue
        insn = DISASM_LINE.match(line)
        if not insn:
            continue
        address = int(insn.group(1), 16)
        mnemonic = insn.group(2)
        current["insns"].append(
            (address, mnemonic, DISASM_COMMENT.split(insn.group(3) or "")[0].strip())
        )
        operands = (insn.group(3) or "").split(";")[0].strip()
        if mnemonic == ".word":
            try:
                current["words"].append((address, int(operands, 0)))
            except ValueError:
                pass
        elif BRANCH_MNEMONIC.match(mnemonic):
            target = DISASM_TARGET.match(operands)
            if target:
                current["branches"].append(
                    (address, int(target.group(1), 16), target.group(2))
                )
    return functions


def resolve_function(functions: Mapping[str, dict], name: str):
    """The disassembly symbol for `name`: exact, or a C++ file-static via
    its _ZL<len><name> mangling (exact-name, as dark_window_symbol_address)."""
    if name in functions:
        return name
    mangled = f"_ZL{len(name)}{name}"
    for symbol in functions:
        if symbol.startswith(mangled):
            return symbol
    return None


def verify_step_loop_closure(
    functions: Mapping[str, dict], etext: int, roots: Iterable[str]
) -> tuple[int, int, int]:
    """Walk direct branches from `roots`. Fails on a missing root, a branch
    outside [0, etext), a veneer, or a literal word in flash. Pure half;
    returns (functions walked, branches checked, literal words checked)."""
    pending: list[str] = []
    missing = []
    for name in roots:
        symbol = resolve_function(functions, name)
        if symbol is None:
            missing.append(name)
        elif symbol not in pending:
            pending.append(symbol)
    if missing:
        raise VerificationError(
            "step-loop closure: root absent from the ITCM disassembly "
            "(inlined away or moved to flash): " + ", ".join(sorted(missing))
        )
    seen: set[str] = set()
    offenders: list[str] = []
    branches = 0
    words = 0
    while pending:
        symbol = pending.pop(0)
        if symbol in seen:
            continue
        seen.add(symbol)
        body = functions[symbol]
        for insn, target, target_symbol in body["branches"]:
            branches += 1
            where = f"{symbol}+0x{insn - body['start']:x}"
            if not 0 <= target < etext:
                offenders.append(
                    f"{where} branches to 0x{target:08x} <{target_symbol}> "
                    f"outside ITCM [0, 0x{etext:x})"
                )
            elif target_symbol.endswith("_veneer"):
                offenders.append(
                    f"{where} branches to veneer <{target_symbol}>"
                )
            elif target_symbol != symbol:
                if target_symbol in functions:
                    pending.append(target_symbol)
                else:
                    offenders.append(
                        f"{where} branches to 0x{target:08x} <{target_symbol}> "
                        "with no disassembled body"
                    )
        for address, value in body["words"]:
            words += 1
            if FLASH_LITERAL_LOW <= value <= FLASH_LITERAL_HIGH:
                offenders.append(
                    f"{symbol}+0x{address - body['start']:x} literal "
                    f"0x{value:08x} names flash"
                )
    if offenders:
        raise VerificationError(
            "step-loop closure: " + "; ".join(offenders) + ". Nothing the "
            "activation step loop reaches may touch flash: a flash fetch "
            "inside the GPR32 identity window refills a canonical cache line "
            "with the running image's bytes (aius-sample-rate-following.md "
            "Phase 4z). Make the callee FASTRUN, or move the value to DTCM."
        )
    return len(seen), branches, words


def verify_remap_in_dtcm(nm_output: str) -> int:
    """nm puts the DTCM remap copy in [0x20000000, 0x20080000). Pure half;
    returns its address."""
    hits = []
    for line in nm_output.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        name = parts[-1]
        if name == REMAP_SYMBOL or name.startswith(
            f"_ZL{len(REMAP_SYMBOL)}{REMAP_SYMBOL}"
        ):
            try:
                hits.append((name, int(parts[0], 16)))
            except ValueError:
                continue
    if not hits:
        raise VerificationError(
            f"step-loop closure: {REMAP_SYMBOL} absent from the linked image; "
            "ota_flash_read must read the remap from DTCM"
        )
    for name, value in hits:
        if not DTCM_LOW <= value < DTCM_HIGH:
            raise VerificationError(
                f"step-loop closure: {name} at 0x{value:08x} is not DTCM; "
                "ota_flash_read would read the remap through flash"
            )
    return hits[0][1]


def parse_nm_value(nm_output: str, name: str):
    for line in nm_output.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[-1] == name:
            return int(parts[0], 16)
    return None


def _sibling_tool(readelf: str, tool: str) -> str:
    """objdump/nm beside the readelf in use (same toolchain prefix)."""
    if readelf.endswith("readelf"):
        return readelf[: -len("readelf")] + tool
    return f"arm-none-eabi-{tool}"


def _register(token: str) -> str:
    return REGISTER_ALIASES.get(token, token)


def _immediate(text: str):
    text = text.strip()
    if not text.startswith("#"):
        return None
    try:
        return int(text[1:], 0) & 0xFFFFFFFF
    except ValueError:
        return None


def _operand_registers(text: str) -> list[str]:
    return [_register(token) for token in REGISTER_TOKEN.findall(text)]


def trace_memory_accesses(body: Mapping) -> list[tuple[str, int, object]]:
    """Events of one disassembled function, in address order: ("store" |
    "load", insn, resolved address or None), ("dsb" | "isb" | "msr" |
    "return" | "cpsie", insn, None), ("cmp", insn, immediate or None), ("bl", insn,
    target symbol), ("branch", insn, target address), ("it", insn,
    mnemonic) for an IT instruction and ("pred", insn, mnemonic) for each
    instruction it predicates or any condition-suffixed load/store (that
    instruction's own event follows, unconditional in this model). See the
    DCACHE_HELPER block for the register model and its limits."""
    words = dict(body["words"])
    known: dict[str, int] = {}
    events: list[tuple[str, int, object]] = []

    def forget(*registers: str) -> None:
        for register in registers:
            known.pop(register, None)

    it_left = 0
    for address, mnemonic, operands in sorted(body["insns"]):
        parts = [part.strip() for part in operands.split(",")]
        dest = _register(parts[0]) if parts and parts[0] else ""
        if IT_MNEMONIC.match(mnemonic):
            events.append(("it", address, mnemonic))
            it_left = len(mnemonic) - 1
            continue
        if it_left or PREDICATED_MEMORY.match(mnemonic):
            events.append(("pred", address, mnemonic))
        it_left = max(it_left - 1, 0)
        # `bls.n`, `blt`, `ble`, `blo` are conditional b, not calls.
        if mnemonic in ("bl", "bl.w") or mnemonic.startswith("blx"):
            target = DISASM_TARGET.match(operands)
            if target:
                events.append(("bl", address, target.group(2)))
            forget(*CALLER_SAVED)
        elif BRANCH_MNEMONIC.match(mnemonic):
            target = DISASM_TARGET.match(operands)
            if target:
                events.append(("branch", address, int(target.group(1), 16)))
        elif mnemonic.startswith(("dsb", "isb", "msr")):
            events.append((mnemonic[:3], address, None))
        elif mnemonic == "cpsie":
            events.append(("cpsie", address, None))
        elif mnemonic.startswith("cmp"):
            events.append((
                "cmp", address, _immediate(parts[1]) if len(parts) == 2 else None
            ))
        elif (mnemonic == "bx" and operands == "lr") or (
            mnemonic.startswith("pop") and "pc" in _operand_registers(operands)
        ):
            events.append(("return", address, None))
            forget(*_operand_registers(operands))
        elif IMMEDIATE_MOVE.match(mnemonic) and len(parts) == 2:
            value = _immediate(parts[1])
            if value is None and parts[1] in REGISTER_TOKEN.findall(parts[1]):
                value = known.get(_register(parts[1]))
            elif value is not None and mnemonic.startswith("mvn"):
                value = ~value & 0xFFFFFFFF
            forget(dest)
            if value is not None:
                known[dest] = value
        elif mnemonic in ("movt", "movt.w") and len(parts) == 2:
            value = _immediate(parts[1])
            low = known.pop(dest, None)
            if value is not None and low is not None:
                known[dest] = (low & 0xFFFF) | ((value & 0xFFFF) << 16)
        elif re.match(r"^(?:add|adds|sub|subs)(?:\.w)?$|^(?:addw|subw)$", mnemonic):
            source = _register(parts[1]) if len(parts) == 3 else dest
            value = _immediate(parts[-1])
            base = known.get(source)
            forget(dest)
            if value is not None and base is not None and len(parts) in (2, 3):
                sign = -1 if mnemonic.startswith("sub") else 1
                known[dest] = (base + sign * value) & 0xFFFFFFFF
        elif mnemonic.startswith(("str", "ldr")):
            memory = MEMORY_OPERAND.match(operands)
            is_load = mnemonic.startswith("ldr")
            if memory is None:
                if is_load or mnemonic.startswith("strex"):
                    forget(*_operand_registers(operands.split("[")[0]))
                continue
            base = _register(memory.group("base"))
            offset = int(memory.group("offset") or "0", 0)
            if base == "pc":
                literal = words.get(((address + 4) & ~3) + offset)
                forget(dest)
                if mnemonic in ("ldr", "ldr.w", "ldr.n") and literal is not None:
                    known[dest] = literal & 0xFFFFFFFF
                continue
            resolved = (
                (known[base] + offset) & 0xFFFFFFFF if base in known else None
            )
            events.append(("load" if is_load else "store", address, resolved))
            if is_load or mnemonic.startswith("strex"):
                forget(*_operand_registers(memory.group("regs")))
            if memory.group("writeback"):
                forget(base)
        elif mnemonic.startswith(("ldm", "pop")):
            forget(*_operand_registers(operands))
        elif mnemonic.startswith(("stm", "push")):
            if "!" in operands:
                forget(_register(operands.split("!")[0].strip()))
        elif NON_WRITING_MNEMONIC.match(mnemonic):
            continue
        elif re.match(r"^[us](?:mull|mlal)", mnemonic):
            forget(*_operand_registers(",".join(parts[:2])))
        elif dest:
            forget(dest)
    return events


def _split_operands(operands: str) -> list[str]:
    return [part.strip() for part in operands.split(",")]


def _is_register(text: str) -> bool:
    return REGISTER_TOKEN.fullmatch(text) is not None


def _written_registers(mnemonic: str, operands: str) -> tuple[str, ...]:
    """Registers one instruction writes, conservatively (a call writes every
    caller-saved register). Used for def lookups, never for resolution."""
    if mnemonic in ("bl", "bl.w") or mnemonic.startswith("blx"):
        return CALLER_SAVED
    if (
        BRANCH_MNEMONIC.match(mnemonic)
        or IT_MNEMONIC.match(mnemonic)
        or NON_WRITING_MNEMONIC.match(mnemonic)
    ):
        return ()
    registers = _operand_registers(operands)
    if mnemonic.startswith(("ldm", "pop")):
        return tuple(registers)
    memory = MEMORY_OPERAND.match(operands)
    writeback = (
        [_register(memory.group("base"))]
        if memory and memory.group("writeback") else []
    )
    if mnemonic.startswith(("stm", "push")):
        return tuple(registers[:1]) if "!" in operands else ()
    if mnemonic.startswith("str"):
        status = registers[:1] if mnemonic.startswith("strex") else []
        return tuple(status + writeback)
    if mnemonic.startswith("ldr"):
        if memory is None:
            return tuple(registers)
        return tuple(_operand_registers(memory.group("regs")) + writeback)
    if re.match(r"^[us](?:mull|mlal)", mnemonic):
        return tuple(registers[:2])
    return tuple(registers[:1])


def _last_def(insns, register: str, before: int, low: int):
    """The last instruction in [low, before) that writes `register`, in
    address order, or None."""
    for insn in reversed(insns):
        address, mnemonic, operands = insn
        if low <= address < before and register in _written_registers(
            mnemonic, operands
        ):
            return insn
    return None


def _flag_source(insns, branch: int, low: int):
    """The instruction whose flags a conditional branch at `branch` reads:
    the last flag setter in [low, branch). None when a call (flags
    clobbered) or nothing in the range sets them."""
    for insn in reversed(insns):
        address, mnemonic, _ = insn
        if not low <= address < branch:
            continue
        if mnemonic in ("bl", "bl.w") or mnemonic.startswith("blx"):
            return None
        if FLAG_SETTING.match(mnemonic):
            return insn
    return None


def _shift_amount(insn, source: str):
    """N for `mov rd, <source>, lsl #N` or `lsl rd, <source>, #N`."""
    _, mnemonic, operands = insn
    parts = _split_operands(operands)
    if len(parts) != 3 or _register(parts[1]) != source:
        return None
    if re.match(r"^movs?(?:\.w)?$", mnemonic):
        shift = re.fullmatch(r"lsl #(\d+)", parts[2])
        return int(shift.group(1)) if shift else None
    if re.match(r"^lsls?(?:\.w)?$", mnemonic):
        return _immediate(parts[2])
    return None


def _verify_dcache_helper(symbol: str, body: Mapping, events) -> int:
    """The helper is straight-line apart from two nested counted loops: the
    DCCISW store sits inside an inner backward conditional branch fed by
    `cmp #8192` and an outer one fed by `cmp #4`, and stores (inner counter)
    | (outer counter lsl #30). No IT block, predicated instruction or
    forward branch. Returns the DCCISW store count."""
    start = body["start"]
    insns = sorted(body["insns"])
    by_address = {insn[0]: insn for insn in insns}

    def at(insn: int) -> str:
        return f"{symbol}+0x{insn - start:x}"

    predicated = [insn for kind, insn, _ in events if kind in ("it", "pred")]
    if predicated:
        raise VerificationError(
            f"{at(predicated[0])} is an IT block or a predicated instruction "
            f"({by_address[predicated[0]][1]}); the set/way pass must run "
            "unconditionally"
        )
    forward = [
        (insn, target) for kind, insn, target in events
        if kind == "branch" and target > insn
    ]
    if forward:
        insn, target = forward[0]
        raise VerificationError(
            f"{at(insn)} branches forward to 0x{target:x}; no path through "
            "the set/way pass may skip the DCCISW store"
        )
    dccisw = [
        insn for kind, insn, value in events
        if kind == "store" and value == SCB_DCCISW_ADDRESS
    ]
    if not dccisw:
        raise VerificationError(
            f"{symbol} has no resolved store to SCB_CACHE_DCCISW "
            f"(0x{SCB_DCCISW_ADDRESS:08x})"
        )
    loops = []
    for kind, insn, target in events:
        if kind != "branch" or not start <= target < insn:
            continue
        bound = counter = None
        if CONDITIONAL_BRANCH.match(by_address[insn][1]):
            source = _flag_source(insns, insn, target)
            if source is not None and source[1].startswith("cmp"):
                parts = _split_operands(source[2])
                if len(parts) == 2 and _is_register(parts[0]):
                    bound = _immediate(parts[1])
                    counter = _register(parts[0])
        loops.append((target, insn, bound, counter))
    store = dccisw[0]
    around = [loop for loop in loops if loop[0] <= store < loop[1]]
    if len(around) < 2:
        raise VerificationError(
            f"{at(store)} DCCISW store sits inside {len(around)} backward "
            "branch(es); the pass needs a set loop nested in a way loop, or "
            "it does not cover the whole D-cache"
        )
    around.sort(key=lambda loop: loop[1] - loop[0])
    inner, outer = around[0], around[-1]
    for loop, bound, meaning in (
        (inner, DCACHE_SET_SPAN, "inner (set) loop, 256 sets x 32-byte lines"),
        (outer, DCACHE_WAYS, "outer (way) loop, 4 ways"),
    ):
        if loop[2] != bound:
            seen = "no cmp" if loop[2] is None else f"cmp #{loop[2]}"
            raise VerificationError(
                f"{at(loop[1])} closes the {meaning} but its conditional "
                f"branch is fed by {seen}, not cmp #{bound}; the pass would "
                "not cover the whole 32 KiB D-cache"
            )
    if not (outer[0] <= inner[0] and inner[1] < outer[1]):
        raise VerificationError(
            f"{symbol} loops at {at(inner[1])} and {at(outer[1])} overlap "
            "without nesting"
        )
    for loop, step, meaning in (
        (inner, DCACHE_LINE_BYTES, "inner (set)"),
        (outer, 1, "outer (way)"),
    ):
        target, branch, _, counter = loop
        if not re.fullmatch(r"bne(?:\.n|\.w)?", by_address[branch][1]):
            raise VerificationError(
                f"{at(branch)} closes the {meaning} loop with "
                f"{by_address[branch][1]}, not bne; the count must stop at "
                "exactly its bound"
            )
        init = _last_def(insns, counter, target, start)
        init_parts = _split_operands(init[2]) if init else []
        if not (
            init is not None
            and IMMEDIATE_MOVE.match(init[1])
            and not init[1].startswith("mvn")
            and len(init_parts) == 2
            and _immediate(init_parts[1]) == 0
        ):
            raise VerificationError(
                f"{symbol} {meaning} loop counter {counter} does not start at "
                f"0 before {at(target)}"
                + (f" (set by {init[1]} {init[2]})" if init else "")
            )
        steps = [
            insn for insn in insns
            if target <= insn[0] < branch
            and counter in _written_registers(insn[1], insn[2])
        ]
        step_parts = _split_operands(steps[0][2]) if len(steps) == 1 else []
        if not (
            len(steps) == 1
            and re.fullmatch(r"adds?(?:\.w)?", steps[0][1])
            and len(step_parts) in (2, 3)
            and all(_register(p) == counter for p in step_parts[:-1])
            and _immediate(step_parts[-1]) == step
        ):
            raise VerificationError(
                f"{symbol} {meaning} loop counter {counter} is not stepped by "
                f"exactly one `add {counter}, #{step}` inside its loop"
                + (f" (found: {'; '.join(f'{m} {o}' for _, m, o in steps)})"
                   if steps else "")
            )
    value = _register(_split_operands(by_address[store][2])[0])
    combine = _last_def(insns, value, store, start)
    composed = False
    if combine is not None and combine[1].startswith("orr"):
        parts = _split_operands(combine[2])
        if len(parts) == 3 and all(_is_register(part) for part in parts[1:]):
            first, second = _register(parts[1]), _register(parts[2])
            for way, sets in ((first, second), (second, first)):
                shift = _last_def(insns, way, combine[0], start)
                if (
                    sets == inner[3]
                    and shift is not None
                    and _shift_amount(shift, outer[3]) == DCACHE_WAY_SHIFT
                ):
                    composed = True
    if not composed:
        raise VerificationError(
            f"{at(store)} DCCISW value {value} is not (set counter "
            f"{inner[3]}) | (way counter {outer[3]} lsl #{DCACHE_WAY_SHIFT}); "
            "the way index must land in bits 31:30"
        )
    return len(dccisw)


def verify_identity_restore(functions: Mapping[str, dict]) -> tuple[int, int, int]:
    """Phase 4za (D2). The helper: see _verify_dcache_helper. ota_flash_read's
    restore side, from the GPR32 restore store to the PRIMASK restore, is
    straight-line (no forward branch inside it, no backward branch out of
    it, no branch into it from outside, no branch or return from the
    identity window past it, no IT block or predicated instruction) and
    runs: GPR32 restore store, GPR32 read-back, dsb, MCR0 SWRESET store of a
    value through `orr #1`, a backward MCR0-load poll loop closed by bne on
    `ands`/`tst` #1, dsb, isb, exactly one bl to the
    helper, dsb, ICIALLU store, dsb, isb, PRIMASK restore. Neither function
    may store DCISW. Pure half; returns (helper DCCISW stores, restore store
    offset, helper call offset) within ota_flash_read."""
    helper = resolve_function(functions, DCACHE_HELPER)
    reader = resolve_function(functions, IDENTITY_READ_PATH)
    absent = [
        name for name, symbol in (
            (DCACHE_HELPER, helper), (IDENTITY_READ_PATH, reader)
        ) if symbol is None
    ]
    if absent:
        raise VerificationError(
            "absent from the ITCM disassembly (inlined away or moved to "
            "flash): " + ", ".join(absent)
        )
    helper_events = trace_memory_accesses(functions[helper])
    reader_body = functions[reader]
    reader_start = reader_body["start"]
    reader_insns = sorted(reader_body["insns"])
    reader_by_address = {insn[0]: insn for insn in reader_insns}
    reader_events = trace_memory_accesses(reader_body)

    def where(insn: int) -> str:
        return f"+0x{insn - reader_start:x}"

    for symbol, events in ((helper, helper_events), (reader, reader_events)):
        if any(
            kind == "store" and value == SCB_DCISW_ADDRESS
            for kind, _, value in events
        ):
            raise VerificationError(
                f"{symbol} stores SCB_CACHE_DCISW (0x{SCB_DCISW_ADDRESS:08x}); "
                "the pass must clean+invalidate (DCCISW), or dirty PSRAM and "
                "DMAMEM lines are dropped and verify can pass on stale bytes"
            )
    stores = _verify_dcache_helper(helper, functions[helper], helper_events)
    gpr32 = [
        insn for kind, insn, value in reader_events
        if kind == "store" and value == IOMUXC_GPR_GPR32_ADDRESS
    ]
    if len(gpr32) < 2:
        raise VerificationError(
            f"{reader} has {len(gpr32)} resolved IOMUXC_GPR_GPR32 "
            f"(0x{IOMUXC_GPR_GPR32_ADDRESS:08x}) store(s); the identity flip "
            "and its restore need two"
        )
    restore = gpr32[-1]
    swreset = next(
        (
            insn for kind, insn, value in reader_events
            if kind == "store" and value == FLEXSPI_MCR0_ADDRESS
            and insn > restore
        ),
        None,
    )
    if swreset is None:
        raise VerificationError(
            f"{reader} has no FLEXSPI_MCR0 (0x{FLEXSPI_MCR0_ADDRESS:08x}) "
            f"SWRESET store after the GPR32 restore at {where(restore)}"
        )
    swreset_value = _register(
        _split_operands(reader_by_address[swreset][2])[0]
    )
    source = _last_def(reader_insns, swreset_value, swreset, reader_start)
    sets_bit0 = False
    if source is not None:
        parts = _split_operands(source[2])
        immediate = _immediate(parts[-1]) if parts else None
        sets_bit0 = immediate is not None and immediate & 1 == 1 and (
            source[1].startswith("orr")
            or (IMMEDIATE_MOVE.match(source[1]) and len(parts) == 2
                and not source[1].startswith("mvn"))
        )
    if not sets_bit0:
        raise VerificationError(
            f"{reader} MCR0 store at {where(swreset)} writes {swreset_value}, "
            "which no `orr #1` (or bit-0 immediate) produced; it is not a "
            "SWRESET"
        )
    between = [
        (kind, insn) for kind, insn, value in reader_events
        if restore < insn < swreset
        and (kind == "dsb" or (kind == "load" and value == IOMUXC_GPR_GPR32_ADDRESS))
    ]
    readback = next((insn for kind, insn in between if kind == "load"), None)
    if readback is None:
        raise VerificationError(
            f"{reader} has no IOMUXC_GPR_GPR32 read-back between the GPR32 "
            f"restore at {where(restore)} and the SWRESET at {where(swreset)}"
        )
    if not any(kind == "dsb" and insn > readback for kind, insn in between):
        raise VerificationError(
            f"{reader} has no dsb between the GPR32 restore at "
            f"{where(restore)} (after its read-back) and the SWRESET at "
            f"{where(swreset)}; an early AIPS ack can race the AHB buffer reset"
        )
    calls = [
        insn for kind, insn, target in reader_events
        if kind == "bl" and target == helper
    ]
    if len(calls) != 1:
        raise VerificationError(
            f"{reader} calls {helper} {len(calls)} times; the restore side "
            "needs exactly one pass"
        )
    call = calls[0]
    if call < restore:
        raise VerificationError(
            f"{reader}{where(call)} calls {helper} before the GPR32 restore "
            f"store at {where(restore)}; a line filled after the pass "
            "survives the restore"
        )
    if call < swreset:
        raise VerificationError(
            f"{reader}{where(call)} calls {helper} before the restore SWRESET "
            f"at {where(swreset)}; the AHB buffer can refill a line after the "
            "pass"
        )
    leave = next(
        (
            insn for kind, insn, _ in reader_events
            if insn > call and kind in ("msr", "return")
        ),
        None,
    )
    if leave is None:
        raise VerificationError(
            f"{reader} has no PRIMASK restore or return after the {helper} "
            f"call at {where(call)}"
        )
    # The identity window opens at the GPR32 flip store. A branch before it
    # (the span-check error exit at the top) may cross everything; one from
    # inside it must not leave without passing the restore store.
    flip = gpr32[0]
    for kind, insn, target in reader_events:
        if kind == "return" and flip <= insn < restore:
            raise VerificationError(
                f"{reader}{where(insn)} returns inside the identity window "
                f"(flip at {where(flip)}) before the GPR32 restore at "
                f"{where(restore)}; the running slot's remap is never restored"
            )
        if kind != "branch":
            continue
        inside = restore <= insn < leave
        problem = None
        if inside and target > insn:
            problem = "forward inside"
        elif inside and target < restore:
            problem = "backward out of"
        elif not inside and restore < target <= leave:
            problem = "into"
        elif flip <= insn < restore and target > leave:
            problem = "from the identity window past"
        if problem is not None:
            raise VerificationError(
                f"{reader}{where(insn)} branches to 0x{target:x}, {problem}"
                f" the restore sequence ({where(restore)} to "
                f"{where(leave)}); the linear trace cannot prove the pass and "
                "its barriers run on every path"
            )
    predicated = [
        insn for kind, insn, _ in reader_events
        if kind in ("it", "pred") and restore <= insn <= leave
    ]
    if predicated:
        raise VerificationError(
            f"{reader}{where(predicated[0])} is an IT block or a predicated "
            f"instruction ({reader_by_address[predicated[0]][1]}) inside the "
            "restore sequence; every step must run unconditionally"
        )
    unmasked = [
        (kind, insn) for kind, insn, _ in reader_events
        if kind in ("msr", "cpsie") and restore < insn < leave
    ]
    if unmasked:
        kind, insn = unmasked[0]
        raise VerificationError(
            f"{reader}{where(insn)} is an early {reader_by_address[insn][1]} "
            f"{reader_by_address[insn][2]} inside the restore sequence (before "
            f"the PRIMASK restore at {where(leave)}); an interrupt taken there "
            "runs against a half-restored remap or an uncleaned D-cache"
        )
    for kind, insn, value in reader_events:
        if kind not in ("load", "store") or value is not None:
            continue
        if not restore <= insn <= leave:
            continue
        memory = MEMORY_OPERAND.match(reader_by_address[insn][2])
        if memory is not None and _register(memory.group("base")) == "sp":
            continue
        raise VerificationError(
            f"{reader}{where(insn)} {reader_by_address[insn][1]} "
            f"{reader_by_address[insn][2]} goes through an unresolved base "
            "inside the restore sequence; the guard cannot tell which "
            "peripheral it touches (only sp-relative stack slots are exempt)"
        )
    pre_dsb = next(
        (i for k, i, _ in reader_events if k == "dsb" and swreset < i < call),
        None,
    )
    pre_isb = next(
        (
            i for k, i, _ in reader_events
            if k == "isb" and pre_dsb is not None and pre_dsb < i < call
        ),
        None,
    )
    if pre_isb is None:
        raise VerificationError(
            f"{reader} has no dsb then isb between the SWRESET at "
            f"{where(swreset)} and the {helper} call at {where(call)}"
        )
    polls = [
        i for k, i, v in reader_events
        if k == "load" and v == FLEXSPI_MCR0_ADDRESS and swreset < i < pre_dsb
    ]
    loops = [
        (i, target, poll) for poll in polls for k, i, target in reader_events
        if k == "branch" and swreset < target <= poll < i < pre_dsb
    ]
    if not loops:
        raise VerificationError(
            f"{reader} has no backward MCR0-load poll loop between the "
            f"SWRESET at {where(swreset)} and the dsb at {where(pre_dsb)}; "
            "the pass could start before the AHB buffer reset completes"
        )

    def polls_bit0(branch: int, target: int, poll: int) -> bool:
        source = _flag_source(reader_insns, branch, target)
        if source is None or not re.fullmatch(
            r"(?:ands|tst)(?:\.w)?", source[1]
        ):
            return False
        parts = _split_operands(source[2])
        tested = parts[1] if source[1].startswith("ands") and len(parts) == 3 \
            else parts[0]
        if not _is_register(tested) or _immediate(parts[-1]) != 1:
            return False
        loaded = _last_def(reader_insns, _register(tested), source[0], target)
        return loaded is not None and loaded[0] == poll

    fed = [loop for loop in loops if polls_bit0(*loop)]
    if not fed:
        branch = loops[0][0]
        raise VerificationError(
            f"{reader}{where(branch)} poll branch is not fed by `ands`/`tst` "
            "#1 of the MCR0 load; it does not wait on the SWRESET bit"
        )
    if not any(
        re.fullmatch(r"bne(?:\.n|\.w)?", reader_by_address[loop[0]][1])
        for loop in fed
    ):
        branch = fed[0][0]
        raise VerificationError(
            f"{reader}{where(branch)} poll branch is "
            f"{reader_by_address[branch][1]}, not bne; after `ands`/`tst` #1 "
            "only bne loops while SWRESET is still set, so this exit is "
            "unconditional, inverted or never taken"
        )
    tail = [
        (kind, insn, value) for kind, insn, value in reader_events
        if call < insn < leave
    ]
    iciallu = next(
        (
            i for k, i, v in tail
            if k == "store" and v == SCB_ICIALLU_ADDRESS
        ),
        None,
    )
    if iciallu is None:
        raise VerificationError(
            f"{reader} has no ICIALLU (0x{SCB_ICIALLU_ADDRESS:08x}) store "
            f"between the {helper} call at {where(call)} and the PRIMASK "
            f"restore at {where(leave)}; slot-A code may stay in the I-cache"
        )
    if not any(k == "dsb" and i < iciallu for k, i, _ in tail):
        raise VerificationError(
            f"{reader} has no dsb between the {helper} call at {where(call)} "
            f"and the ICIALLU at {where(iciallu)}; the DCCISW pass is not "
            "complete when the window closes"
        )
    i_dsb = next((i for k, i, _ in tail if k == "dsb" and i > iciallu), None)
    if i_dsb is None:
        raise VerificationError(
            f"{reader} has no dsb after the ICIALLU at {where(iciallu)} and "
            f"before the PRIMASK restore at {where(leave)}"
        )
    if not any(k == "isb" and i > i_dsb for k, i, _ in tail):
        raise VerificationError(
            f"{reader} has no isb after the dsb at {where(i_dsb)} and before "
            f"the PRIMASK restore at {where(leave)}"
        )
    return stores, restore - reader_start, call - reader_start


def _itcm_disassembly(build_dir: Path, readelf: str) -> tuple[str, int, str]:
    """(nm output, _etext, objdump of ITCM [0, _etext)) of the linked image."""
    elf = build_dir / "firmware.elf"
    if not elf.exists():
        raise VerificationError(f"linked image not found: {elf}")
    nm = subprocess.run(
        [_sibling_tool(readelf, "nm"), str(elf)],
        check=True,
        text=True,
        capture_output=True,
    ).stdout
    etext = parse_nm_value(nm, "_etext")
    if etext is None:
        raise VerificationError("_etext absent from nm")
    disassembly = subprocess.run(
        [
            _sibling_tool(readelf, "objdump"),
            "-d",
            "--no-show-raw-insn",
            "--start-address=0",
            f"--stop-address=0x{etext:x}",
            str(elf),
        ],
        check=True,
        text=True,
        capture_output=True,
    ).stdout
    return nm, etext, disassembly


def verify_identity_restore_in_build(
    build_dir: Path, readelf: str, environment: str
):
    """Only the A/B slot envs compile the identity branch; None elsewhere."""
    if environment not in REMAP_ENVS:
        return None
    _, _, disassembly = _itcm_disassembly(build_dir, readelf)
    return verify_identity_restore(parse_disassembly(disassembly))


def _identity_restore_summary(result, environment: str) -> str:
    if result is None:
        return (
            f"identity-window restore: SKIP ({environment} compiles no remap)"
        )
    stores, restore, call = result
    return (
        "identity-window restore: PASS "
        f"(GPR32 restore at +0x{restore:x}, read-back and dsb before the "
        f"SWRESET, one bl to {DCACHE_HELPER} at +0x{call:x} then dsb, "
        "ICIALLU, dsb, isb, no forward branch; "
        f"{stores} DCCISW store(s) in a loop bounded by #{DCACHE_SET_SPAN} "
        f"and #{DCACHE_WAYS}, no DCISW)"
    )


def verify_step_loop_closure_in_build(
    build_dir: Path, readelf: str, project_dir: Path, environment: str
) -> tuple[int, int, int, int | None]:
    try:
        nm, etext, disassembly = _itcm_disassembly(build_dir, readelf)
    except VerificationError as error:
        raise VerificationError(f"step-loop closure: {error}") from error
    source = (project_dir / PRIMITIVES_SOURCE).read_text(encoding="utf-8")
    table = primitive_roots(source)
    verify_primitive_fields_assigned(
        source,
        (project_dir / PRIMITIVES_HEADER).read_text(encoding="utf-8"),
    )
    if not table:
        raise VerificationError(
            f"step-loop closure: no primitives assignment in "
            f"{PRIMITIVES_SOURCE}; the root set would silently shrink"
        )
    walked = verify_step_loop_closure(
        parse_disassembly(disassembly), etext, (*STEP_LOOP_ROOTS, *table)
    )
    remap = verify_remap_in_dtcm(nm) if environment in REMAP_ENVS else None
    return (*walked, remap)


def _step_loop_summary(
    functions: int, branches: int, words: int, remap: int | None
) -> str:
    where = (
        f", {REMAP_SYMBOL} in DTCM at 0x{remap:08x}" if remap is not None
        else ""
    )
    return (
        "step-loop closure: PASS "
        f"({functions} functions, {branches} direct branches all ITCM, "
        f"{words} literal words none flash{where})"
    )


def parse_symbol_names(output: str) -> list[str]:
    """Every named symbol from `readelf -sW` on the linked ELF, any type."""
    names: list[str] = []
    for line in output.splitlines():
        parts = line.split()
        if len(parts) < 8 or not parts[0][:-1].isdigit() or not parts[0].endswith(":"):
            continue
        names.append(parts[7])
    return names


def verify_no_retired_symbols(names: Iterable[str]) -> int:
    """Phase 4i: no symbol of the linked image names replay."""
    names = list(names)
    offenders = sorted({n for n in names if RETIRED_SYMBOL_PATTERN.search(n)})
    if offenders:
        raise VerificationError(
            "retired-feature symbols are in the linked image: "
            + ", ".join(offenders)
        )
    return len(names)


def verify_no_retired_symbols_in_build(build_dir: Path, readelf: str) -> int:
    elf = build_dir / "firmware.elf"
    if not elf.exists():
        raise VerificationError(f"linked image not found: {elf}")
    result = subprocess.run(
        [readelf, "-sW", str(elf)],
        check=True,
        text=True,
        capture_output=True,
    )
    return verify_no_retired_symbols(parse_symbol_names(result.stdout))


def _retired_symbols_summary(checked: int) -> str:
    return (
        "retired-feature symbols: PASS "
        f"({checked} symbols in the linked image, none names replay)"
    )


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
    retired = sorted(RETIRED_LDF_LIBS & set(groups))
    if retired:
        raise VerificationError(
            f"retired LDF libraries are in the link: {retired}; a retired "
            "library must not be linked into any optical image"
        )
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


def _tx_itcm_summary(sizes: Mapping[str, int]) -> str:
    detail = ", ".join(f"{name} {size} B" for name, size in sizes.items())
    return (
        "TX ITCM residency: PASS "
        f"({len(sizes)} symbols in ITCM, {sum(sizes.values())} bytes: {detail})"
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
    print(_producer_yield_summary(
        verify_producer_yield_object(build_dir, readelf)
    ))
    print(_ldf_summary(*verify_ldf_libraries_in_build(build_dir, readelf)))
    print(_retired_symbols_summary(
        verify_no_retired_symbols_in_build(build_dir, readelf)
    ))
    print(_dark_window_summary(
        verify_dark_window_residency(build_dir, readelf)
    ))
    print(_step_loop_summary(*verify_step_loop_closure_in_build(
        build_dir, readelf, project_dir, environment
    )))
    print(_identity_restore_summary(
        verify_identity_restore_in_build(build_dir, readelf, environment),
        environment,
    ))
    print(_tx_itcm_summary(verify_tx_itcm_residency(build_dir, readelf)))
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
        pinned = verify_producer_yield_object(build_dir, args.readelf)
    except (OSError, subprocess.CalledProcessError, VerificationError) as error:
        print(f"producer-yield ITCM placement: FAIL: {error}", file=sys.stderr)
        return 1
    print(_producer_yield_summary(pinned))
    try:
        ldf = verify_ldf_libraries_in_build(build_dir, args.readelf)
    except (OSError, subprocess.CalledProcessError, VerificationError) as error:
        print(f"LDF-library ITCM placement: FAIL: {error}", file=sys.stderr)
        return 1
    print(_ldf_summary(*ldf))
    try:
        retired_checked = verify_no_retired_symbols_in_build(
            build_dir, args.readelf
        )
    except (OSError, subprocess.CalledProcessError, VerificationError) as error:
        print(f"retired-feature symbols: FAIL: {error}", file=sys.stderr)
        return 1
    print(_retired_symbols_summary(retired_checked))
    try:
        dark = verify_dark_window_residency(build_dir, args.readelf)
    except (OSError, subprocess.CalledProcessError, VerificationError) as error:
        print(f"dark-window ITCM residency: FAIL: {error}", file=sys.stderr)
        return 1
    print(_dark_window_summary(dark))
    try:
        step_loop = verify_step_loop_closure_in_build(
            build_dir, args.readelf, project_dir, environment
        )
    except (OSError, subprocess.CalledProcessError, VerificationError) as error:
        print(f"step-loop closure: FAIL: {error}", file=sys.stderr)
        return 1
    print(_step_loop_summary(*step_loop))
    try:
        identity = verify_identity_restore_in_build(
            build_dir, args.readelf, environment
        )
    except (OSError, subprocess.CalledProcessError, VerificationError) as error:
        print(f"identity-window restore: FAIL: {error}", file=sys.stderr)
        return 1
    print(_identity_restore_summary(identity, environment))
    try:
        tx_sizes = verify_tx_itcm_residency(build_dir, args.readelf)
    except (OSError, subprocess.CalledProcessError, VerificationError) as error:
        print(f"TX ITCM residency: FAIL: {error}", file=sys.stderr)
        return 1
    print(_tx_itcm_summary(tx_sizes))
    return 0


try:
    Import("env")  # type: ignore[name-defined]  # PlatformIO/SCons injection
except NameError:
    env = None

# Every env that links the optical image: the A/B slot envs extend
# teensy41-optical and are the deployed image since the slot-agnostic chooser,
# so a guard keyed on the bench env alone leaves the canonical image unguarded
# (register, 2026-09-07).
OPTICAL_IMAGE_ENVS = frozenset({
    "teensy41-optical",
    "teensy41-optical-slot-a",
    "teensy41-optical-slot-b",
    "teensy41-optical-fault",
})

if env is not None and env.subst("$PIOENV") in OPTICAL_IMAGE_ENVS:
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", verify_cold_placement)


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv[1:]))
