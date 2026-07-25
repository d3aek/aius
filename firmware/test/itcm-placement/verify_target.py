#!/usr/bin/env python3
"""Post-link proof that the cold engines contribute no ITCM.

On Teensy 4.1 a function with no FLASHMEM attribute lands in ITCM, and ITCM
shares the 512 KB FlexRAM with DTCM -- so cold code in ITCM is taken straight
out of the audio path's headroom. The deciding principle (see
docs/plans/aius-network-hardening.md) is: ITCM belongs to code that genuinely
runs per audio block; everything else is FLASHMEM.

This check reads the per-object section table of every object named in
COLD_OBJECTS and fails the link if any of them still emits executable `.text`.
COMDAT (`G`) sections are excluded: those are out-of-line copies of inline or
virtual members that framework headers such as <SD.h> force into whichever
translation unit uses them. They are not definitions of the annotated file, they
are deduplicated at link time, and no edit inside an owned source file can move
them.

COLD_TREES makes the list non-optional: a new object compiled out of a cold
engine tree that nobody added to COLD_OBJECTS fails the build, so an unannotated
engine cannot land silently.
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
    "lib/aius_boot/src/aius_boot.c.o",
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
    "lib/aius_boot/",
    "lib/aius_http/",
    "lib/aius_net/",
    "lib/aius_runtime/",
    "lib/aius_sd/",
)

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


class VerificationError(RuntimeError):
    """A cold object still contributes ITCM."""


def parse_itcm_sections(
    output: str,
    allowed: Iterable[str] = (),
) -> tuple[dict[str, int], int, int]:
    """Split `.text*` into owned ITCM, COMDAT bytes and framework-clone bytes."""
    allowed = set(allowed)
    itcm: dict[str, int] = {}
    comdat_bytes = 0
    clone_bytes = 0
    for line in output.splitlines():
        match = SECTION_LINE.match(line)
        if not match:
            continue
        name, size_hex, flags = match.group(1), match.group(2), match.group(3)
        size = int(size_hex, 16)
        if size == 0:
            continue
        if "G" in flags:
            comdat_bytes += size
            continue
        if name in allowed:
            clone_bytes += size
            continue
        itcm[name] = size
    return itcm, comdat_bytes, clone_bytes


def inspect_object(
    obj: Path,
    readelf: str,
    allowed: Iterable[str] = (),
) -> tuple[dict[str, int], int, int]:
    result = subprocess.run(
        [readelf, "-SW", str(obj)],
        check=True,
        text=True,
        capture_output=True,
    )
    return parse_itcm_sections(result.stdout, allowed)


def verify_declared_set(linked: Iterable[str]) -> None:
    linked = set(linked)
    missing = [name for name in COLD_OBJECTS if name not in linked]
    if missing:
        raise VerificationError(
            f"cold objects were not linked (renamed or dropped?): {missing}; "
            "update COLD_OBJECTS in test/itcm-placement/verify_target.py"
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


def _summary(count: int, comdat: int, clones: int) -> str:
    return (
        f"cold-object ITCM placement: PASS ({count} objects, 0 owned ITCM bytes, "
        f"{comdat} COMDAT + {clones} framework-clone bytes excluded)"
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
    print(_summary(*verify_placement(build_dir, linked, _resolve_readelf(env))))


def _main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("build_dir", type=Path)
    parser.add_argument(
        "--readelf",
        default="arm-none-eabi-readelf",
        help="readelf-compatible executable",
    )
    args = parser.parse_args(argv)
    build_dir = args.build_dir.resolve()
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
