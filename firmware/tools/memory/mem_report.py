#!/usr/bin/env python3
"""Per-bank memory report for the Aius Teensy 4.1 capability firmware (gate P4).

Reads the LINKED ELF -- its section table and the linker's own symbols -- and
projects it onto the four physical banks:

    FLASH  — program image (0x60000000, 7936 KiB on the Teensy 4.1)
    ITCM   — instruction tightly-coupled RAM (RAM1 code block, 32 KiB-granular)
    DTCM   — data tightly-coupled RAM (RAM1 variables + the stack/local free zone)
    OCRAM  — RAM2 (0x20200000, 512 KiB; DMAMEM + malloc/new heap)

Why the ELF and not `teensy_size:`
----------------------------------
This tool used to regex the `teensy_size:` summary the Teensy platform prints at
link time. That summary counts only the sections it knows by name, so any section
the linker ORPHAN-PLACES inside a bank is invisible to it. The optical image has
one: `NonCacheable` (FNET's `fnet_fec0_buf`) lands between `.data` and `.bss`
inside DTCM. It occupies that address space, but `teensy_size` excluded it from
`RAM1: variables:` AND from `free for local variables:`, so the gate overstated
the DTCM free-for-locals zone by the whole 9,756 bytes of it.

The linker's own symbols cannot drift that way. `_estack - _ebss` IS the
free-for-locals zone: `_ebss` is the top of everything statically placed in DTCM
(orphans included, because they sit below `.bss`), and `_estack` is where the
stack starts growing down. Bank occupancy is therefore taken as the high-water
mark of the linker's watermark symbol AND every SHF_ALLOC section whose address
falls in the bank's region — so a future orphan placed ABOVE `_ebss` is counted
too, rather than reintroducing the same class of bug from the other side.

The `teensy_size:` summary is still parsed when a build log is supplied, but only
to CROSS-CHECK: any bank where the summary disagrees with the ELF is reported,
with the sections that account for the difference named.

The bank geometry is read from `cores/teensy4/imxrt1062_t41.ld`:
ITCM and DTCM share the 512 KiB FlexRAM, the ITCM block is `_itcm_block_count`
32 KiB banks, and `_estack = ORIGIN(DTCM) + ((16 - _itcm_block_count) << 15)` --
so the DTCM capacity is exactly the FlexRAM the ITCM block did not claim.

Note the DTCM region is 512 KiB but the DTCM *bank* ends at `_estack`. The linker
does NOT error when `.bss` runs past `_estack` (the region has room), it just
silently eats the stack -- which is precisely what this gate exists to catch.

The decoder's statically-reserved arena is uninitialised `.bss` and therefore
lives in DTCM; `--arena-bytes` annotates how much of the DTCM figure it is. When
it is not supplied the arena symbol is looked up in the ELF instead.

Usage:
    mem_report.py --size-log <pio-build-log> [--arena-bytes N]
    mem_report.py --size-log - < build.log        # read log from stdin
    mem_report.py --elf firmware.elf              # ELF only, no cross-check
    mem_report.py --size-log <log> --elf <path>   # explicit ELF

With `--size-log` alone the ELF path is taken from the log's `Linking`/`Checking
size` line and resolved against the firmware root.

Exit codes:
    0  every bank fits
    2  a bank is over capacity (reported loudly)
    3  input unusable: no teensy_size summary, or the ELF is missing/unreadable
    4  usage error
"""
import argparse
import re
import struct
import sys
from pathlib import Path

FIRMWARE_ROOT = Path(__file__).resolve().parents[2]

# MEMORY {} from cores/teensy4/imxrt1062_t41.ld. `length` is the REGION the
# linker may place into, which for DTCM is deliberately wider than the bank.
ITCM_ORIGIN, ITCM_REGION = 0x00000000, 512 * 1024
DTCM_ORIGIN, DTCM_REGION = 0x20000000, 512 * 1024
OCRAM_ORIGIN, OCRAM_REGION = 0x20200000, 512 * 1024
FLASH_ORIGIN, FLASH_REGION = 0x60000000, 7936 * 1024
ERAM_ORIGIN, ERAM_REGION = 0x70000000, 32768 * 1024

FLEXRAM_TOTAL = 512 * 1024  # ITCM + DTCM share this 512 KiB block.
FLEXRAM_BANK = 32 * 1024  # ITCM block granularity.

# Sections `teensy_size` knows by name, per bank. Anything else the linker put in
# a bank is invisible to the summary -- that is the bug this tool exists past.
SUMMARY_SECTIONS = {
    # FLASH `code` is `.text.code` + `_etext`, and `_etext` spans `.text.itcm`
    # plus `.ARM.exidx` -- but not `.fini`, which sits between them.
    "FLASH": {".text.headers", ".text.code", ".text.progmem", ".text.csf",
              ".text.itcm", ".ARM.exidx", ".data"},
    "ITCM": {".text.itcm", ".ARM.exidx"},
    "DTCM": {".data", ".bss"},
    "OCRAM": {".bss.dma"},
}

SHF_ALLOC = 0x2
SHT_SYMTAB = 2
PT_LOAD = 1
SHN_UNDEF = 0

ELF_PATH_RE = re.compile(
    r"^(?:Linking|Checking size|Calculating size)\s+(\S+\.elf)\s*$", re.M)


class InputError(Exception):
    """The input could not be used at all (exit 3)."""


# --------------------------------------------------------------------------
# ELF
# --------------------------------------------------------------------------

class Section:
    def __init__(self, name, kind, flags, addr, size):
        self.name = name
        self.kind = kind
        self.flags = flags
        self.addr = addr
        self.size = size

    @property
    def alloc(self):
        return bool(self.flags & SHF_ALLOC)

    @property
    def end(self):
        return self.addr + self.size


class Segment:
    def __init__(self, kind, vaddr, paddr, filesz, memsz):
        self.kind = kind
        self.vaddr = vaddr
        self.paddr = paddr
        self.filesz = filesz
        self.memsz = memsz


class Elf:
    """Just enough ELF32-little-endian to answer 'what is where, and how big'."""

    def __init__(self, path, data):
        self.path = path
        self.sections = []
        self.segments = []
        self.symbols = {}  # name -> (value, size)
        self._parse(data)

    def _parse(self, data):
        if len(data) < 52 or data[:4] != b"\x7fELF":
            raise InputError(f"{self.path} is not an ELF file")
        if data[4] != 1:
            raise InputError(f"{self.path} is not ELF32 (the Teensy 4.1 image is)")
        if data[5] != 1:
            raise InputError(f"{self.path} is not little-endian")
        (_type, _machine, _version, _entry, phoff, shoff, _flags,
         _ehsize, phentsize, phnum, shentsize, shnum,
         shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
        if shnum == 0 or shoff == 0:
            raise InputError(f"{self.path} has no section table")
        if shentsize < 40 or shoff + shnum * shentsize > len(data):
            raise InputError(f"{self.path} has a truncated section table")

        raw = []
        for i in range(shnum):
            raw.append(struct.unpack_from("<IIIIIIIIII", data,
                                          shoff + i * shentsize))
        if shstrndx >= shnum:
            raise InputError(f"{self.path} has an out-of-range section-name table")
        shstr = self._strings(data, raw[shstrndx])
        for entry in raw:
            self.sections.append(Section(
                name=_cstr(shstr, entry[0]), kind=entry[1], flags=entry[2],
                addr=entry[3], size=entry[5]))

        for i in range(phnum):
            if phentsize < 32 or phoff + (i + 1) * phentsize > len(data):
                break
            kind, _off, vaddr, paddr, filesz, memsz = struct.unpack_from(
                "<IIIIII", data, phoff + i * phentsize)
            self.segments.append(Segment(kind, vaddr, paddr, filesz, memsz))

        for index, entry in enumerate(raw):
            if entry[1] != SHT_SYMTAB:
                continue
            if entry[6] >= shnum:
                raise InputError(f"{self.path} symbol table {index} has no string table")
            strtab = self._strings(data, raw[entry[6]])
            self._read_symbols(data, entry, strtab)

    def _strings(self, data, shdr):
        offset, size = shdr[4], shdr[5]
        if offset + size > len(data):
            raise InputError(f"{self.path} has a truncated string table")
        return data[offset:offset + size]

    def _read_symbols(self, data, shdr, strtab):
        offset, size, entsize = shdr[4], shdr[5], shdr[9]
        if entsize < 16 or offset + size > len(data):
            raise InputError(f"{self.path} has a truncated symbol table")
        for pos in range(offset, offset + size - entsize + 1, entsize):
            name_off, value, sym_size, _info, _other, shndx = struct.unpack_from(
                "<IIIBBH", data, pos)
            name = _cstr(strtab, name_off)
            if not name or shndx == SHN_UNDEF:
                continue
            self.symbols.setdefault(name, (value, sym_size))

    def symbol(self, name, default=None):
        found = self.symbols.get(name)
        return default if found is None else found[0]

    def lma(self, section):
        """Load address of a section, via the segment that carries it."""
        for seg in self.segments:
            if seg.kind != PT_LOAD or seg.memsz == 0:
                continue
            if seg.vaddr <= section.addr < seg.vaddr + seg.memsz:
                return section.addr - seg.vaddr + seg.paddr
        return section.addr

    def allocated(self):
        return [s for s in self.sections if s.alloc and s.size > 0]


def _cstr(blob, offset):
    if offset >= len(blob):
        return ""
    end = blob.find(b"\0", offset)
    end = len(blob) if end < 0 else end
    return blob[offset:end].decode("utf-8", "replace")


def read_elf(path):
    try:
        data = Path(path).read_bytes()
    except OSError as exc:
        raise InputError(f"cannot read {path}: {exc.strerror}") from exc
    return Elf(path, data)


# --------------------------------------------------------------------------
# teensy_size summary (cross-check only)
# --------------------------------------------------------------------------

def _grab(pattern, text, label):
    m = re.search(pattern, text)
    if not m:
        raise InputError(f"could not find {label} in teensy_size output")
    return int(m.group(1))


def parse_teensy_size(text):
    """Extract the three teensy_size lines into a flat dict of byte counts."""
    if "teensy_size:" not in text:
        raise InputError("no 'teensy_size:' summary in the build output")
    return {
        "flash_code": _grab(r"FLASH:\s*code:(\d+)", text, "FLASH code"),
        "flash_data": _grab(r"FLASH:.*?data:(\d+)", text, "FLASH data"),
        "flash_headers": _grab(r"FLASH:.*?headers:(\d+)", text, "FLASH headers"),
        "flash_free": _grab(r"free for files:(\d+)", text, "FLASH free"),
        "ram1_vars": _grab(r"RAM1:\s*variables:(\d+)", text, "RAM1 variables"),
        "ram1_code": _grab(r"RAM1:.*?code:(\d+)", text, "RAM1 code"),
        "ram1_padding": _grab(r"RAM1:.*?padding:(\d+)", text, "RAM1 padding"),
        "ram1_free": _grab(r"free for local variables:(\d+)", text, "RAM1 free"),
        "ram2_vars": _grab(r"RAM2:\s*variables:(\d+)", text, "RAM2 variables"),
        "ram2_free": _grab(r"free for malloc/new:(\d+)", text, "RAM2 free"),
    }


def elf_path_from_log(text, size_log):
    """PlatformIO names the image it linked; take the last such line."""
    matches = ELF_PATH_RE.findall(text)
    if not matches:
        raise InputError(
            "no 'Linking <image>.elf' line in the build output; "
            "pass the image explicitly with --elf")
    named = Path(matches[-1])
    if named.is_absolute():
        return named
    bases = [FIRMWARE_ROOT]
    if size_log not in (None, "-"):
        # <root>/.pio/build-<env>.log -> <root>
        bases.append(Path(size_log).resolve().parent.parent)
    bases.append(Path.cwd())
    for base in bases:
        candidate = base / named
        if candidate.is_file():
            return candidate
    raise InputError(
        f"build output named {named}, but it was not found under "
        + ", ".join(str(b) for b in bases))


# --------------------------------------------------------------------------
# Banks
# --------------------------------------------------------------------------

class Bank:
    def __init__(self, name, origin, region, used, capacity, note=""):
        self.name = name
        self.origin = origin
        self.region = region
        self.used = used
        self.capacity = capacity
        self.note = note

    @property
    def free(self):
        return self.capacity - self.used

    @property
    def top(self):
        return self.origin + self.used

    @property
    def pct(self):
        return 100.0 * self.used / self.capacity if self.capacity else 0.0

    @property
    def over(self):
        return self.used > self.capacity


def _high_water(elf, origin, region, watermark):
    """Bank occupancy: the linker's own watermark symbol OR any allocated
    section that lands in the region, whichever reaches higher. The section
    scan is what makes an orphan-placed section count."""
    top = origin
    if watermark is not None:
        top = max(top, watermark)
    for section in elf.allocated():
        if origin <= section.addr < origin + region:
            top = max(top, section.end)
    return top - origin


def sections_in(elf, origin, region):
    return sorted((s for s in elf.allocated()
                   if origin <= s.addr < origin + region),
                  key=lambda s: s.addr)


def banks_from_elf(elf):
    """Project the linked image onto the four physical banks."""
    estack = elf.symbol("_estack")
    ebss = elf.symbol("_ebss")
    if estack is None or ebss is None:
        missing = " and ".join(
            n for n, v in (("_estack", estack), ("_ebss", ebss)) if v is None)
        raise InputError(
            f"{elf.path} defines no {missing}; it is not a Teensy 4.1 image "
            "linked with imxrt1062_t41.ld")

    blocks = elf.symbol("_itcm_block_count")
    itcm_used = _high_water(elf, ITCM_ORIGIN, ITCM_REGION, elf.symbol("_etext"))
    if blocks is None:
        blocks = -(-itcm_used // FLEXRAM_BANK)  # ceil, the linker's own rounding
    itcm_cap = blocks * FLEXRAM_BANK
    itcm_padding = itcm_cap - itcm_used

    dtcm_used = _high_water(elf, DTCM_ORIGIN, DTCM_REGION, ebss)
    dtcm_cap = estack - DTCM_ORIGIN

    ocram_used = _high_water(elf, OCRAM_ORIGIN, OCRAM_REGION,
                             elf.symbol("_heap_start"))

    image_len = elf.symbol("_flashimagelen")
    flash_used = _high_water(
        elf, FLASH_ORIGIN, FLASH_REGION,
        None if image_len is None else FLASH_ORIGIN + image_len)

    free_zone = estack - (DTCM_ORIGIN + dtcm_used)
    return [
        Bank("FLASH", FLASH_ORIGIN, FLASH_REGION, flash_used, FLASH_REGION,
             "program image incl. every load image (_flashimagelen)"),
        Bank("ITCM", ITCM_ORIGIN, ITCM_REGION, itcm_used, itcm_cap,
             f"fast code; {itcm_padding}B dead padding to the "
             f"{blocks}x32 KiB block"),
        Bank("DTCM", DTCM_ORIGIN, DTCM_REGION, dtcm_used, dtcm_cap,
             f"static vars; stack + locals get _estack {estack:#010x} - _ebss "
             f"{ebss:#010x} = {free_zone}B"),
        Bank("OCRAM", OCRAM_ORIGIN, OCRAM_REGION, ocram_used, OCRAM_REGION,
             "RAM2: DMAMEM + malloc/new heap"),
    ]


def flexram_rollup(banks):
    """The physically meaningful RAM1 view: ITCM + DTCM share one 512 KiB block,
    repartitioned at build time, so the elastic headroom is the shared free zone."""
    itcm = next(b for b in banks if b.name == "ITCM")
    dtcm = next(b for b in banks if b.name == "DTCM")
    used = itcm.capacity + dtcm.used  # ITCM block committed + DTCM statics.
    return Bank("FlexRAM", 0, FLEXRAM_TOTAL, used, FLEXRAM_TOTAL,
                "RAM1 = ITCM+DTCM; free zone is the shared stack/local budget")


def unplaced(elf):
    """Allocated sections that fall in no known region -- a future orphan that
    landed somewhere this report does not model. Loud, but not a gate failure."""
    known = ((ITCM_ORIGIN, ITCM_REGION), (DTCM_ORIGIN, DTCM_REGION),
             (OCRAM_ORIGIN, OCRAM_REGION), (FLASH_ORIGIN, FLASH_REGION),
             (ERAM_ORIGIN, ERAM_REGION))
    out = []
    for section in elf.allocated():
        if not any(o <= section.addr < o + r for o, r in known):
            out.append(section)
    return out


def arena_from_elf(elf):
    """The decoder arena, when build.sh did not resolve it. Matched on the
    substring so a C++ class-scoped static (name-mangled here) still resolves;
    the resolved symbol is printed so the annotation stays auditable."""
    best = None
    for name, (_value, size) in elf.symbols.items():
        if "g_arena_mem" in name and size > 0:
            if best is None or size > best[1]:
                best = (name, size)
    return best


# --------------------------------------------------------------------------
# Cross-check against the teensy_size summary
# --------------------------------------------------------------------------

def summary_used(fields):
    return {
        "FLASH": fields["flash_code"] + fields["flash_data"] + fields["flash_headers"],
        "ITCM": fields["ram1_code"],
        "DTCM": fields["ram1_vars"],
        "OCRAM": fields["ram2_vars"],
    }


def omitted_sections(elf, bank):
    """Allocated sections inside a bank that teensy_size does not count. FLASH
    also owns the load image of anything whose LMA lands there."""
    known = SUMMARY_SECTIONS[bank.name]
    out = []
    for section in elf.allocated():
        if section.name in known:
            continue
        inside = bank.origin <= section.addr < bank.origin + bank.region
        if bank.name == "FLASH" and not inside:
            lma = elf.lma(section)
            inside = bank.origin <= lma < bank.origin + bank.region
        if inside:
            out.append(section)
    return out


def crosscheck(elf, banks, fields):
    """Lines describing every bank where the summary and the ELF disagree."""
    lines = []
    claimed = summary_used(fields)
    for bank in banks:
        delta = bank.used - claimed[bank.name]
        if delta == 0:
            continue
        omits = omitted_sections(elf, bank)
        named = sum(s.size for s in omits)
        why = ", ".join(f"{s.name} {s.size}B" for s in omits) or "no named section"
        fill = delta - named
        if fill > 0:
            why += f" (+{fill}B alignment/fill)"
        elif fill < 0:
            why += f" (and counts {-fill}B the ELF does not)"
        lines.append(f"    {bank.name:<7} summary {claimed[bank.name]:>9}"
                     f"  elf {bank.used:>9}  delta {delta:+8}  omits: {why}")
    return lines


# --------------------------------------------------------------------------
# Render
# --------------------------------------------------------------------------

def kib(n):
    return f"{n / 1024.0:.1f}K"


def render(elf, banks, fields, arena_bytes, arena_symbol=None):
    lines = []
    lines.append("==> Aius Teensy 4.1 per-bank memory report")
    lines.append(f"  source: {elf.path} (section table + linker symbols)")
    header = (f"  {'BANK':<8} {'USED':>10} {'FREE':>12} {'CAPACITY':>12}"
              f"  {'USED%':>7}  {'FREE%':>6}")
    lines.append(header)
    lines.append("  " + "-" * (len(header) - 2))
    worst_over = False
    rows = list(banks)
    rows.insert(3, flexram_rollup(banks))  # FlexRAM roll-up just before OCRAM.
    for b in rows:
        flag = "  <<< OVER CAPACITY" if b.over else ""
        if b.over:
            worst_over = True
        lines.append(
            f"  {b.name:<8} {b.used:>10} {b.free:>12} {b.capacity:>12}"
            f"  {b.pct:>6.2f}%  {100.0 - b.pct:>5.1f}%{flag}"
        )
        lines.append(f"             {kib(b.used)} used / {kib(b.capacity)} — {b.note}")

    dtcm = next(b for b in banks if b.name == "DTCM")
    lines.append("  DTCM layout (every allocated section in the bank's region):")
    for s in sections_in(elf, DTCM_ORIGIN, DTCM_REGION):
        lines.append(f"    {s.addr:#010x} {s.size:>8}B  {s.name}")
    lines.append(f"    {dtcm.top:#010x} .......... free for locals "
                 f"({dtcm.free}B) up to _estack")

    if arena_bytes is not None:
        share = 100.0 * arena_bytes / dtcm.capacity if dtcm.capacity else 0.0
        origin = f" [{arena_symbol}]" if arena_symbol else ""
        lines.append(
            f"  note: shared codec decoder static arena{origin} = {arena_bytes}B "
            f"({kib(arena_bytes)}), {share:.1f}% of DTCM, inside the DTCM 'used' figure")

    for section in unplaced(elf):
        lines.append(f"  WARNING: {section.name} ({section.size}B at "
                     f"{section.addr:#010x}) is in no bank this report models")

    if fields is not None:
        divergence = crosscheck(elf, banks, fields)
        if divergence:
            lines.append("  note: the teensy_size summary printed above disagrees with "
                         "the linked ELF;")
            lines.append("        it counts only the sections it knows by name, so an "
                         "orphan-placed")
            lines.append("        section inside a bank is missing from its figures:")
            lines.extend(divergence)
        claimed_free = fields["ram1_free"]
        if claimed_free != dtcm.free:
            lines.append(f"        teensy_size claims 'free for local variables:"
                         f"{claimed_free}'; the linker leaves {dtcm.free}.")
            lines.append("        The stack, the main loop and the audio ISR all live "
                         "in that zone.")

    lines.append("  note: ITCM and DTCM split the 512 KiB FlexRAM elastically; the "
                 "32 KiB-granular ITCM block")
    lines.append("        rounding is why ITCM shows padding. The FlexRAM row is the "
                 "true RAM1 headroom.")
    return "\n".join(lines), worst_over


def main(argv):
    ap = argparse.ArgumentParser(add_help=True, description=__doc__)
    ap.add_argument("--size-log",
                    help="file with the pio/teensy_size build output, or '-' for stdin")
    ap.add_argument("--elf",
                    help="linked firmware.elf; defaults to the image the log names")
    ap.add_argument("--arena-bytes", type=int, default=None,
                    help="decoder static arena size, for the DTCM annotation")
    try:
        args = ap.parse_args(argv[1:])
    except SystemExit as exc:
        if exc.code == 0:  # --help printed its text; that is not a usage error.
            return 0
        return 4
    if not args.size_log and not args.elf:
        print("ERROR: pass --size-log, --elf, or both", file=sys.stderr)
        return 4

    try:
        fields, text = None, ""
        if args.size_log:
            if args.size_log == "-":
                text = sys.stdin.read()
            else:
                with open(args.size_log, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            fields = parse_teensy_size(text)

        elf_path = args.elf or elf_path_from_log(text, args.size_log)
        elf = read_elf(elf_path)
        banks = banks_from_elf(elf)
    except InputError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3

    arena_bytes, arena_symbol = args.arena_bytes, None
    if arena_bytes is None:
        found = arena_from_elf(elf)
        if found:
            arena_symbol, arena_bytes = found

    report, over = render(elf, banks, fields, arena_bytes, arena_symbol)
    print(report)
    if over:
        print("ERROR: a memory bank is over capacity — firmware does not fit",
              file=sys.stderr)
        return 2
    print("==> All banks fit.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
