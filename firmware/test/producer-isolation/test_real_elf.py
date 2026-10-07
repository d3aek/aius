"""producer-isolation over the linked optical images (W1 to W6, link half).

Skips, naming the missing artifact, until parent-wiring's platformio.ini
seam lands: the link map (-Wl,-Map=firmware.map in the build directory), kept
relocations (-Wl,--emit-relocs) and dependency files (-MMD). Environment:
AIUS_PIO_BUILD (default firmware/.pio/build), AIUS_ISOLATION_ENVS
(default "teensy41-optical teensy41-optical-slot-a").
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import aius_walk as aw  # noqa: E402
import link_half  # noqa: E402
from tables_optical import TABLES  # noqa: E402

BUILD = os.environ.get("AIUS_PIO_BUILD", os.path.normpath(os.path.join(HERE, "..", "..", ".pio", "build")))
ENVS = os.environ.get("AIUS_ISOLATION_ENVS", "teensy41-optical teensy41-optical-slot-a").split()
_images: dict = {}


def image_or_skip(env: str) -> aw.Image:
    d = os.path.join(BUILD, env)
    elf, mp = os.path.join(d, "firmware.elf"), os.path.join(d, "firmware.map")
    if not os.path.exists(elf):
        raise unittest.SkipTest(f"{env}: no linked image at {elf}")
    if not os.path.exists(mp):
        raise unittest.SkipTest(f"{env}: no link map {mp} (needs -Wl,-Map in platformio.ini, parent-wiring)")
    if not aw.tool("objdump"):
        raise unittest.SkipTest("arm-none-eabi-objdump not found")
    if env not in _images:
        _images[env] = aw.Image(elf, mp)
    img = _images[env]
    if not img.has_relocs:
        raise unittest.SkipTest(f"{env}: no relocation sections (needs -Wl,--emit-relocs, parent-wiring)")
    return img


class RealImage(unittest.TestCase):
    def each(self, fn):
        for env in ENVS:
            with self.subTest(env=env):
                img = image_or_skip(env)
                self.assertEqual(fn(img), [])

    def test_w1_graph(self):
        self.each(lambda img: list(img.failures))

    def test_w5_1_and_4_producer_roots(self):
        self.each(lambda img: aw.check_producer(img, TABLES))

    def test_w5_2_allocator_and_stdio_closure(self):
        self.each(lambda img: aw.check_danger_closure(img, TABLES))

    def test_w5_3_sd_and_yield_roots(self):
        def run(img):
            if not img.resolve(aw.HOOK):
                raise unittest.SkipTest(f"{aw.HOOK} not linked yet (parent-wiring's strong hook)")
            return aw.check_sd_and_yield_roots(img, TABLES)
        self.each(run)

    def test_link_half_out_of_line_provenance(self):
        self.each(lambda img: link_half.check_out_of_line(img.map_sections, os.path.dirname(img.elf_path)))

    def test_link_half_header_inline_dependencies(self):
        def run(img):
            d = os.path.dirname(img.elf_path)
            if not any(f.endswith(".d") for _r, _d, fs in os.walk(d) for f in fs):
                raise unittest.SkipTest(f"{d}: no .d files (needs -MMD in platformio.ini, parent-wiring)")
            return link_half.check_header_inline(img.map_sections, d)
        self.each(run)

    def test_link_half_strong_hook(self):
        self.each(link_half.check_hook_definition)


if __name__ == "__main__":
    unittest.main()
