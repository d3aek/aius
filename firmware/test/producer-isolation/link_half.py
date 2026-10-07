"""producer-isolation, link half (S7, CR3-N1, CR4-B3, CR5-S1).

Provenance of the SD stack in the optical image, from the link map (the ELF
carries no .debug_info) and the -MMD dependency files.

PlatformIO names each library archive after the library (libSdFat.a,
libSD.a), the same name the framework copies would get, so an archive name
proves nothing: an out-of-line definition passes only when it comes from an
archive member whose .d names its source under firmware/vendor/SdFat/src or
firmware/vendor/SD/src and names no framework SD or SdFat path.
"""
from __future__ import annotations

import os
import re

import aius_walk

OUT_OF_LINE_CLASSES = {"SdioCard", "DedicatedSpiCard", "FatFile", "FsBaseFile", "SDClass"}
# Header-only: SDFile (SD.h), FsFile (FsLib/FsFile.h) and the StreamFile
# template FsFile derives from; emitted as COMDATs by the src/ objects using them.
HEADER_INLINE = {"SDFile": ("SD", "FsFile"), "FsFile": ("SD", "FsFile"), "StreamFile": ("FsFile",)}
VENDORED_HEADER = {"SD": re.compile(r"(^|[\s/])vendor/SD/src/SD\.h(\s|$)"),
                   "FsFile": re.compile(r"(^|[\s/])vendor/SdFat/src/FsLib/FsFile\.h(\s|$)")}
VENDOR_SOURCE = re.compile(r"(^|[\s/])vendor/(SdFat|SD)/src/\S+\.(cpp|c)(\s|$)")
FRAMEWORK_SD = re.compile(r"framework-arduinoteensy/libraries/(SD|SdFat)/")
LEADING = re.compile(r"^_Z(?:N[KVr]*|T[VIS])(\d+)")


def leading_class(sym: str) -> str | None:
    """First name component of a mangled symbol ('_ZN10StreamFileI...' -> 'StreamFile')."""
    sym = re.sub(r"^\.(text|rodata|data|bss)\.", "", sym)
    m = LEADING.match(sym)
    if not m:
        return None
    n = int(m.group(1))
    return sym[m.end():m.end() + n]


def classes(sec: aius_walk.MapSection) -> set:
    return {c for c in (leading_class(x) for x in [sec.name] + list(sec.symbols)) if c}


def fw_root(build_dir: str) -> str:
    """firmware/ for a build directory firmware/.pio/build/<env>."""
    p = os.path.abspath(build_dir)
    return p.split(os.sep + ".pio" + os.sep, 1)[0] if (os.sep + ".pio" + os.sep) in p else p


def dep_file(obj: str, build_dir: str) -> str | None:
    """The -MMD file of an object or archive member (members: by basename beside the archive)."""
    root = fw_root(build_dir)
    m = re.match(r"^(.*)\(([^)]+)\)$", obj)
    if not m:
        d = re.sub(r"\.o$", ".d", obj)
        for cand in (d, os.path.join(root, d), os.path.join(build_dir, d)):
            if os.path.isabs(cand) and os.path.exists(cand) or os.path.exists(cand):
                return cand
        return None
    archive_dir = os.path.dirname(m.group(1))
    archive_dir = archive_dir if os.path.isabs(archive_dir) else os.path.join(root, archive_dir)
    want = re.sub(r"\.o$", ".d", m.group(2))
    hits = [os.path.join(r, want) for r, _d, fs in os.walk(archive_dir) if want in fs]
    return hits[0] if len(hits) == 1 else None


def _read(p):
    with open(p, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def check_out_of_line(sections, build_dir: str) -> list[str]:
    """Every input section defining an out-of-line SD-stack class symbol comes from a vendored archive member."""
    out = []
    verdict: dict = {}
    for s in sections:
        if not s.size or not classes(s) & OUT_OF_LINE_CLASSES:
            continue
        if s.obj not in verdict:
            if "(" not in s.obj:
                verdict[s.obj] = "is not an archive member"
            else:
                d = dep_file(s.obj, build_dir)
                if d is None:
                    verdict[s.obj] = "has no single .d"
                else:
                    text = _read(d)
                    if FRAMEWORK_SD.search(text):
                        verdict[s.obj] = f"names the framework SD or SdFat ({d})"
                    elif not VENDOR_SOURCE.search(text):
                        verdict[s.obj] = f"is not built from firmware/vendor/SdFat or SD ({d})"
                    else:
                        verdict[s.obj] = ""
        if verdict[s.obj]:
            out.append(f"link half: {s.name} comes from {s.obj}, which {verdict[s.obj]}")
    return out


def check_header_inline(sections, build_dir: str) -> list[str]:
    """No .d names the framework SD or SdFat; header-inline COMDAT objects name the vendored headers."""
    out = []
    ds = [os.path.join(r, f) for r, _d, fs in os.walk(build_dir) for f in fs if f.endswith(".d")]
    if not ds:
        return [f"link half: no .d files under {build_dir} (needs -MMD)"]
    for d in sorted(ds):
        if FRAMEWORK_SD.search(_read(d)):
            out.append(f"link half: {d} names the framework SD or SdFat library")
    need: dict = {}
    for s in sections:
        if s.size:
            for c in classes(s) & set(HEADER_INLINE):
                need.setdefault(s.obj, set()).update(HEADER_INLINE[c])
    for obj in sorted(need):
        d = dep_file(obj, build_dir)
        if d is None:
            out.append(f"link half: no .d for {obj}, which defines an SDFile, FsFile or StreamFile COMDAT")
            continue
        text = _read(d)
        for h in sorted(need[obj]):
            if not VENDORED_HEADER[h].search(text):
                out.append(f"link half: {d} does not name the vendored {h}.h")
    return out


def check_hook_definition(img: aius_walk.Image, obj_suffix="optical_main.cpp.o",
                          callee: str | None = "aius_producer_yield_wait") -> list[str]:
    """aius_sdio_wait_hook resolves to the strong definition in optical_main.cpp's object."""
    got = img.resolve(aius_walk.HOOK)
    if len(got) != 1:
        return [f"link half: {aius_walk.HOOK} resolves to {len(got)} symbols"]
    a = got[0]
    obj = img.funcs[a].obj
    out = []
    if "(" in obj or not obj.endswith(obj_suffix):
        out.append(f"link half: {aius_walk.HOOK} is defined in {obj or '<unknown>'}, not {obj_suffix}")
    if callee and not {img.funcs[img.collapse(b)].name for b in img.edges[a]} & {callee}:
        out.append(f"link half: {aius_walk.HOOK} does not call {callee}")
    return out
