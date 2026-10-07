"""Links the W8 fixture ELFs from fixtures/*.c with the PlatformIO toolchain.

No binary is committed: each test process compiles into a temporary
directory, keeping the link map, the .su files and the .d files beside the
objects as the optical build does (-Wl,-Map, -fstack-usage, -MMD,
-Wl,--emit-relocs).
"""
from __future__ import annotations

import atexit
import os
import shutil
import subprocess
import tempfile

import aius_walk

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "fixtures")
CFLAGS = ["-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16", "-Os",
          "-ffunction-sections", "-fdata-sections", "-fno-ipa-icf", "-fstack-usage", "-MMD",
          "-ffreestanding", "-fno-builtin"]

_root: str | None = None
_cache: dict = {}


def available() -> str | None:
    """None when the toolchain is present, else the reason the fixtures cannot link."""
    if not aius_walk.tool("gcc") or not aius_walk.tool("objdump"):
        return "arm-none-eabi-gcc/objdump not found (PlatformIO toolchain-gccarmnoneeabi-teensy or $AIUS_ARM_TOOLCHAIN)"
    return None


def build(source: str, defines: tuple = (), debug: bool = False) -> dict:
    """Compile and link one fixture; returns paths {elf, map, dir, obj}."""
    global _root
    key = (source, defines, debug)
    if key in _cache:
        return _cache[key]
    if _root is None:
        keep = os.environ.get("AIUS_WALK_FIXTURE_DIR")
        if keep:
            _root = keep
            os.makedirs(_root, exist_ok=True)
        else:
            _root = tempfile.mkdtemp(prefix="aius-walk-fx-")
            atexit.register(shutil.rmtree, _root, True)
    tag = os.path.splitext(source)[0] + "".join("_" + d.lower() for d in defines) + ("_g" if debug else "")
    out = os.path.join(_root, tag)
    os.makedirs(out, exist_ok=True)
    obj = os.path.join(out, os.path.splitext(source)[0] + ".c.o")
    gcc = aius_walk.tool("gcc")
    flags = CFLAGS + (["-g"] if debug else []) + ["-D" + d for d in defines]
    subprocess.run([gcc, *flags, "-c", os.path.join(FIXTURES, source), "-o", obj],
                   check=True, cwd=out, capture_output=True, text=True)
    elf = os.path.join(out, "firmware.elf")
    mp = os.path.join(out, "firmware.map")
    subprocess.run([gcc, "-mcpu=cortex-m7", "-mthumb", "-nostdlib", "-nostartfiles",
                    "-T", os.path.join(FIXTURES, "two_region.ld"), "-Wl,--emit-relocs",
                    "-Wl,-Map=" + mp, obj, "-o", elf],
                   check=True, cwd=out, capture_output=True, text=True)
    res = {"elf": elf, "map": mp, "dir": out, "obj": obj}
    _cache[key] = res
    return res


def image(source: str, defines: tuple = (), debug: bool = False) -> aius_walk.Image:
    b = build(source, defines, debug)
    return aius_walk.Image(b["elf"], b["map"])
