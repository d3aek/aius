"""Post-link call-graph walk shared by producer-isolation and stack-bound.

Implements docs/plans/aius-sample-rate-following.md §Walk specification
W1 to W6 over a linked ELF, its `arm-none-eabi-objdump -d`, its `.symtab`,
its kept relocations (`-Wl,--emit-relocs`) and its link map (`-Wl,-Map`).
Every check returns a list of failure strings; an empty list is a pass.
"""
from __future__ import annotations

import os
import re
import shutil
import struct
import subprocess
from bisect import bisect_right
from dataclasses import dataclass, field

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

TOOLCHAIN_DEFAULT = os.path.expanduser(
    "~/.platformio/packages/toolchain-gccarmnoneeabi-teensy/bin")


def tool(name: str) -> str | None:
    """Path to an arm-none-eabi tool: $AIUS_ARM_TOOLCHAIN, PlatformIO, then PATH."""
    for d in (os.environ.get("AIUS_ARM_TOOLCHAIN"), TOOLCHAIN_DEFAULT):
        if d and os.access(os.path.join(d, "arm-none-eabi-" + name), os.X_OK):
            return os.path.join(d, "arm-none-eabi-" + name)
    return shutil.which("arm-none-eabi-" + name)


# W3 leaves, W5 forbidden and I/O stdio sets (names are linked symbol names).
ALLOC = re.compile(r"^(__wrap_|__real_|_)?(malloc|calloc|realloc|free)(_r)?$")
TERMINAL = {"abort", "__assert_func"}
FORMAT_ONLY = re.compile(r"^_?v?snprintf(_r)?$")
STDIO = re.compile(
    r"^_?(fputs|fputc|fwrite|puts|printf|fprintf|vfprintf|fiprintf|write|putc|putchar)(_r)?$"
    r"|^_(fputs|fputc|fwrite|puts|printf|fprintf|vfprintf|fiprintf|write|putc)_r$"
    r"|^__(swbuf|sfvwrite)_r$")
FORBID = re.compile(
    r"SdFat|SdioCard|DedicatedSpiCard|FatFile|FatPartition|FatVolume|ExFat|SDClass|SDFile|"
    r"FsFile|FsBaseFile|_ZN4File|_ZNK4File|fnet_|NativeEthernet|EthernetClass|^yield$|"
    r"usb_serial|Serial|aius_sdio_wait_hook|aius_producer_yield")
HOOK = "aius_sdio_wait_hook"
YIELD_POINT = "aius_producer_yield_point"
CLONE_SUFFIX = re.compile(r"(\.(isra|part|constprop|cold|lto_priv)\.\d+)+$")

DIRECT = re.compile(r"^(bl|b|b\.[nw]|b(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)(\.[nw])?|cbn?z)$")
INSN = re.compile(r"^\s*([0-9a-f]+):\s+([a-z.][a-z0-9.]*)\s*(.*)$")
LABEL = re.compile(r"^([0-9a-f]{8}) <(.+)>:$")
VENEER_NAME = re.compile(r"^__.+_veneer$")


def is_leaf(name: str) -> bool:
    return bool(ALLOC.match(name)) or name in TERMINAL or bool(FORMAT_ONLY.match(name))


def is_danger(name: str) -> bool:
    """W5 (2) start set: allocator family, I/O stdio, abort, __assert_func."""
    return bool(ALLOC.match(name)) or bool(STDIO.match(name)) or name in TERMINAL


def base_name(name: str) -> str:
    return CLONE_SUFFIX.sub("", name)


# ---------------------------------------------------------------- link map

@dataclass
class MapSection:
    name: str
    addr: int
    size: int
    obj: str
    symbols: list = field(default_factory=list)


def parse_map(path: str) -> list[MapSection]:
    """Input sections (name, address, size, object) from a GNU ld map."""
    out: list[MapSection] = []
    in_mem = False
    pending = None
    cur = None
    head = re.compile(r"^ (\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
    cont = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
    sym = re.compile(r"^\s+0x([0-9a-f]+)\s+([^\s=]+)\s*$")
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if line.startswith("Linker script and memory map"):
                in_mem = True
                continue
            if not in_mem:
                continue
            if pending is not None:
                m = cont.match(line)
                if m:
                    cur = MapSection(pending, int(m.group(1), 16), int(m.group(2), 16), m.group(3).strip())
                    out.append(cur)
                    pending = None
                    continue
                pending = None
            m = head.match(line)
            if m and not m.group(1).startswith("*"):
                cur = MapSection(m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4).strip())
                out.append(cur)
                continue
            if re.match(r"^ \.\S+$", line) or re.match(r"^ COMMON$", line):
                pending = line.strip()
                continue
            m = sym.match(line)
            if m and cur is not None:
                cur.symbols.append(m.group(2))
    return out


# ---------------------------------------------------------------- image

@dataclass
class Func:
    addr: int
    size: int
    name: str
    aliases: list
    obj: str = ""


class Image:
    """W1 graph of one linked ELF."""

    def __init__(self, elf_path: str, map_path: str | None = None, objdump: str | None = None):
        self.elf_path = elf_path
        self.map_path = map_path if map_path and os.path.exists(map_path) else None
        self.failures: list[str] = []
        self._load_symbols()
        self.map_sections = parse_map(self.map_path) if self.map_path else []
        self._map_index()
        for f in self.funcs.values():
            f.obj = self.obj_of(f.addr)
        self._disassemble(objdump or tool("objdump"))
        self._relocations()

    # symbols ---------------------------------------------------------
    def _load_symbols(self):
        raw = {}
        self.objects = []  # (addr, size, name, bind)
        self.symbol_values = {}
        with open(self.elf_path, "rb") as fh:
            elf = ELFFile(fh)
            self.sections = [(s.name, s["sh_addr"], s["sh_size"], s["sh_flags"], s["sh_type"])
                             for s in elf.iter_sections()]
            st = elf.get_section_by_name(".symtab")
            for s in st.iter_symbols():
                typ = s["st_info"]["type"]
                if s.name:
                    self.symbol_values.setdefault(s.name, s["st_value"])
                if typ == "STT_FUNC" or (VENEER_NAME.match(s.name or "") and typ != "STT_SECTION"):
                    a = s["st_value"] & ~1
                    raw.setdefault(a, []).append((s["st_size"], s.name, s["st_info"]["bind"]))
                elif typ == "STT_OBJECT" and s["st_size"]:
                    self.objects.append((s["st_value"], s["st_size"], s.name, s["st_info"]["bind"]))
        self.objects.sort()
        self.funcs: dict[int, Func] = {}
        for a, lst in raw.items():
            lst.sort(key=lambda x: (x[2] != "STB_GLOBAL", -x[0]))
            size = max(x[0] for x in lst)
            self.funcs[a] = Func(a, size, lst[0][1], [x[1] for x in lst])
        self.starts = sorted(self.funcs)
        for i, a in enumerate(self.starts):
            if self.funcs[a].size == 0:
                nxt = self.starts[i + 1] if i + 1 < len(self.starts) else a + 16
                self.funcs[a].size = nxt - a
        self.by_name: dict[str, list[int]] = {}
        for a, f in self.funcs.items():
            for n in set(f.aliases):
                self.by_name.setdefault(n, []).append(a)

    def owner(self, addr: int) -> int | None:
        i = bisect_right(self.starts, addr) - 1
        if i < 0:
            return None
        a = self.starts[i]
        return a if addr < a + self.funcs[a].size else None

    def data_symbol_at(self, addr: int) -> str:
        o = self.owner(addr)
        if o is not None:
            return self.funcs[o].name
        keys = [x[0] for x in self.objects]
        i = bisect_right(keys, addr) - 1
        if i >= 0 and self.objects[i][0] <= addr < self.objects[i][0] + self.objects[i][1]:
            return self.objects[i][2]
        return "?"

    # link map --------------------------------------------------------
    def _map_index(self):
        # Non-loaded sections (.debug_*, .ARM.attributes, .comment) are listed
        # at 0-based addresses that overlap ITCM; only loaded ones place code.
        unloaded = re.compile(r"^\.(debug|ARM\.attributes|comment|note|stab|gnu\.attributes|rel\.)")
        secs = sorted((s for s in self.map_sections if s.size and not unloaded.match(s.name)),
                      key=lambda s: s.addr)
        self._map_starts = [s.addr for s in secs]
        self._map_secs = secs

    def obj_of(self, addr: int) -> str:
        if not self._map_secs:
            return ""
        i = bisect_right(self._map_starts, addr) - 1
        for j in (i, i - 1):
            if j >= 0:
                s = self._map_secs[j]
                if s.addr <= addr < s.addr + s.size:
                    return s.obj
        return ""

    # disassembly -----------------------------------------------------
    def _disassemble(self, objdump: str | None):
        if not objdump:
            raise RuntimeError("arm-none-eabi-objdump not found")
        text = subprocess.run([objdump, "-d", "-z", "--no-show-raw-insn", self.elf_path],
                              check=True, capture_output=True, text=True, errors="replace").stdout
        self.insns: dict[int, list] = {a: [] for a in self.funcs}
        for line in text.splitlines():
            m = INSN.match(line)
            if not m:
                continue
            ia = int(m.group(1), 16)
            o = self.owner(ia)
            if o is not None:
                self.insns[o].append((ia, m.group(2), m.group(3)))
        self.veneer: dict[int, int] = {}
        self.edges: dict[int, set] = {a: set() for a in self.funcs}
        self.tail_edges: dict[int, set] = {a: set() for a in self.funcs}
        self.sites: dict[int, list] = {a: [] for a in self.funcs}
        self.jump_tables: list = []
        for a, f in self.funcs.items():
            body = self.insns[a]
            if VENEER_NAME.match(f.name):
                self._veneer(a, f, body)
                continue
            end = a + f.size
            for k, (ia, mn, ops) in enumerate(body):
                if DIRECT.match(mn):
                    t = re.search(r"\b([0-9a-f]+) <", ops)
                    if t:
                        tgt = int(t.group(1), 16)
                        if not (a <= tgt < end):
                            # A branch into another symbol (libgcc's shared float tails enter
                            # mid-symbol) is an edge to that symbol; one into no symbol fails.
                            o = self.owner(tgt)
                            if o is None:
                                self.failures.append(
                                    f"W1: branch in {f.name} @{ia:08x} to {tgt:08x}, inside no symbol")
                            if o is not None:
                                self.edges[a].add(o)
                                if not mn.startswith("bl"):
                                    self.tail_edges[a].add(o)
                    continue
                if mn in ("blx", "bx") and not ops.startswith("lr"):
                    self.sites[a].append((ia, mn, ops))
                elif mn.startswith("ldr") and ops.startswith("pc,"):
                    self._ldr_pc(a, f, body, k, ia, mn, ops)
                elif mn.startswith("mov") and ops.startswith("pc,"):
                    self.sites[a].append((ia, mn, ops))

    def _veneer(self, a, f, body):
        ok = (len(body) >= 2 and body[0][1].startswith("ldr") and
              re.match(r"^pc, \[pc(, #-?0)?\]", body[0][2]) and body[1][1] == ".word")
        if not ok:
            self.failures.append(f"W1: veneer {f.name} has an unrecognised shape")
            return
        lit = int(body[1][2].split()[0], 16) & ~1
        if lit not in self.funcs:
            self.failures.append(f"W1: veneer {f.name} literal {lit:08x} is not a symbol start")
            return
        self.veneer[a] = lit
        self.edges[a].add(lit)

    def _ldr_pc(self, a, f, body, k, ia, mn, ops):
        if re.match(r"^pc, \[sp\]", ops):
            return  # pop-shaped return
        # The base may be lr: `addw lr, pc, #4` then `ldr.w pc, [lr, rN, lsl #2]`
        # (Phase 4zd link: aius_sd_job_step), the table following as words.
        if re.match(r"^pc, \[(r\d+|ip|sl|fp|lr), (r\d+|ip|sl|fp), lsl #2\]", ops):
            # The table's length is the bounds check before it (`cmp rN, #k`
            # then `bhi`): k + 1 entries. Without it the run of .word lines
            # is the table; with it, words past the table are a literal pool
            # the assembler placed after it (Phase 4g link:
            # aius_http_transaction_step), not entries.
            limit = None
            for x in reversed(body[max(0, k - 6):k]):
                m = re.match(r"^(r\d+|ip|sl|fp|lr), #(\d+)", x[2]) if x[1].startswith("cmp") else None
                if m:
                    limit = int(m.group(2)) + 1
                    break
            tbl = []
            for x in body[k + 1:]:
                if x[1] == "nop" and not tbl:
                    continue
                if x[1] != ".word" or (limit is not None and len(tbl) == limit):
                    break
                tbl.append(int(x[2].split()[0], 16) & ~1)
            if tbl:
                outside = [t for t in tbl if not (a <= t < a + f.size)]
                if outside:
                    self.failures.append(f"W1: jump table in {f.name} @{ia:08x} has entries outside it")
                else:
                    self.jump_tables.append((a, ia, len(tbl)))
                return
        self.sites[a].append((ia, mn, ops))

    # relocations -----------------------------------------------------
    def _relocations(self):
        """W1 address-taken: R_ARM_ABS32 in SHF_ALLOC sections resolving to a function start."""
        self.taken: dict[int, set] = {}
        self.reloc_words: list = []  # (where, value) for every alloc ABS32
        self.nonalloc_abs32_targets: set = set()
        self.has_relocs = False
        with open(self.elf_path, "rb") as fh:
            elf = ELFFile(fh)
            for sec in elf.iter_sections():
                if not isinstance(sec, RelocationSection):
                    continue
                tgt = elf.get_section(sec["sh_info"])
                if tgt is None or tgt["sh_type"] == "SHT_NOBITS":
                    continue
                self.has_relocs = True
                alloc = bool(tgt["sh_flags"] & 2)
                data = tgt.data()
                symtab = elf.get_section(sec["sh_link"])
                for r in sec.iter_relocations():
                    if r["r_info_type"] != 2:  # R_ARM_ABS32
                        continue
                    if alloc:
                        off = r["r_offset"] - tgt["sh_addr"]
                        if not (0 <= off <= len(data) - 4):
                            continue
                        v = struct.unpack_from("<I", data, off)[0]
                        self.reloc_words.append((r["r_offset"], v))
                        f = v & ~1
                        if f in self.funcs and self.owner(r["r_offset"]) not in self.veneer:
                            self.taken.setdefault(f, set()).add(
                                (self.data_symbol_at(r["r_offset"]), self.obj_of(r["r_offset"])))
                    else:
                        s = symtab.get_symbol(r["r_info_sym"])
                        off = r["r_offset"]
                        if 0 <= off <= len(data) - 4:
                            addend = struct.unpack_from("<I", data, off)[0]
                        else:
                            addend = 0
                        val = (s["st_value"] + addend) & ~1 & 0xFFFFFFFF
                        for cand in (val, s["st_value"] & ~1):
                            if cand in self.funcs:
                                self.nonalloc_abs32_targets.add(cand)

    def value_scan_taken(self) -> set:
        """The rejected W1 alternative: any loaded word equal to a Thumb function address."""
        out = set()
        with open(self.elf_path, "rb") as fh:
            elf = ELFFile(fh)
            for sec in elf.iter_sections():
                if not (sec["sh_flags"] & 2) or sec["sh_type"] == "SHT_NOBITS":
                    continue
                data = sec.data()
                for off in range(0, len(data) - 3, 4):
                    v = struct.unpack_from("<I", data, off)[0]
                    if v & 1 and (v & ~1) in self.funcs:
                        out.add(v & ~1)
        return out

    # resolution helpers ----------------------------------------------
    def resolve(self, name: str, obj: str | None = None) -> list[int]:
        """Addresses of linked symbols named `name` (clone suffixes ignored), in `obj` if given."""
        out = []
        for a, f in self.funcs.items():
            if any(base_name(n) == name for n in f.aliases) and (not obj or obj in f.obj):
                out.append(a)
        return sorted(out)

    def name(self, a: int) -> str:
        f = self.funcs[a]
        return f.name + (f" ({os.path.basename(f.obj)})" if f.obj else "")

    def collapse(self, a: int) -> int:
        while a in self.veneer:
            a = self.veneer[a]
        return a

    def vtable_entries(self, vt_name: str) -> set:
        out = set()
        for (va, sz, n, _b) in self.objects:
            if n == vt_name:
                for where, v in self.reloc_words:
                    if va <= where < va + sz and (v & ~1) in self.funcs:
                        out.add(v & ~1)
        return out

    def taken_by_obj(self, pattern: str) -> set:
        rx = re.compile(pattern)
        return {f for f, refs in self.taken.items() if any(rx.search(o or "") for (_s, o) in refs)}

    def vtable_symbols(self) -> list[str]:
        return sorted({n for (_a, _s, n, _b) in self.objects if n.startswith("_ZTV")})


# ---------------------------------------------------------------- tables

@dataclass
class SiteRow:
    """W4 row: a containing symbol (and object), its site count, one target-set source.

    source: ("names", [(name, obj)]), ("taken_by", obj_regex), ("vtables", [names]) or ("empty",).
    """
    name: str
    obj: str | None
    count: int
    source: tuple
    note: str = ""


@dataclass
class RecursionRow:
    """W6 row: a component identified by its anchor, its bound and guard-admitted chain."""
    anchor: tuple  # (name, obj)
    bound: int
    chain: list  # [(name, obj)]
    size: int | None = None
    guard: str = ""
    charge: str = "chain"  # "chain": bound x chain frame sum; "sum": every member frame once


@dataclass
class Tables:
    producer_roots: list = field(default_factory=list)  # [(name, obj)]
    yield_roots: list = field(default_factory=list)
    sd_windowed: list = field(default_factory=list)
    sd_unwindowed: list = field(default_factory=list)
    sd_stack_obj: str = r"libAiusSdFat\.a|libAiusSD\.a|aius_sd_teensy\.cpp\.o"
    sd_stack_name: str = (r"SDClass|SDFile|FsFile|FsBaseFile|FatFile|FatPartition|FatVolume|FsVolume|"
                          r"FsCache|FsName|ExFat|SdioCard|SdSpi|DedicatedSpi|SdCard|SdFat|_ZN4File|"
                          r"_ZNK4File|aius_sd_teensy_")
    sites: list = field(default_factory=list)  # [SiteRow]
    libc_row: dict = field(default_factory=dict)  # name -> site count (not followed)
    vtables: dict = field(default_factory=dict)  # _ZTV name -> classification
    recursion: list = field(default_factory=list)  # [RecursionRow]
    empty_bodies: list = field(default_factory=list)


# ---------------------------------------------------------------- walk

class Walker:
    def __init__(self, img: Image, tables: Tables):
        self.img = img
        self.t = tables
        self.failures: list[str] = []
        self._row_cache: dict = {}

    def row_for(self, a: int) -> SiteRow | None:
        f = self.img.funcs[a]
        for r in self.t.sites:
            if any(base_name(n) == r.name for n in f.aliases) and (not r.obj or r.obj in f.obj):
                return r
        return None

    def row_targets(self, r: SiteRow) -> set:
        key = (r.name, r.obj, repr(r.source))
        if key in self._row_cache:
            return self._row_cache[key]
        kind = r.source[0]
        if kind == "names":
            out = set()
            for (n, o) in r.source[1]:
                got = self.img.resolve(n, o)
                if not got:
                    self.failures.append(f"W4: row {r.name} names {n} which matches no symbol")
                out |= set(got)
        elif kind == "taken_by":
            out = self.img.taken_by_obj(r.source[1])
        elif kind == "vtables":
            out = set()
            for v in r.source[1]:
                out |= self.img.vtable_entries(v)
        elif kind == "empty":
            out = set()
        elif kind == "name_match":
            # registration rows: the address-taken functions of one object
            # whose names match the registered set
            ro, rn = re.compile(r.source[1]), re.compile(r.source[2])
            out = {f for f in self.img.taken
                   if ro.search(self.img.funcs[f].obj or "") and rn.search(self.img.funcs[f].name)}
            if not out:
                self.failures.append(f"W4: row {r.name} name_match source matches no address-taken function")
        elif kind == "union":
            out = set()
            for sub in r.source[1]:
                out |= self.row_targets(SiteRow(r.name, r.obj, r.count, sub))
        else:
            raise ValueError(kind)
        self._row_cache[key] = out
        return out

    def successors(self, a: int, follow_sites=True, record=True) -> set:
        """Resolved successors of a non-leaf node: direct edges (veneers kept as nodes) plus W4 targets."""
        nx = set(self.img.edges[a])
        if follow_sites and self.img.sites[a] and a not in self.img.veneer:
            r = self.row_for(a)
            n = len(self.img.sites[a])
            if r is None:
                if record:
                    self.failures.append(
                        f"W4: {n} unlisted indirect site(s) in {self.img.name(a)}")
            else:
                if r.count != n and record:
                    self.failures.append(
                        f"W4: {self.img.name(a)} has {n} indirect site(s), row records {r.count}")
                nx |= self.row_targets(r)
        return nx

    def walk(self, roots, stop_leaves=True, follow_veneers=True, follow_sites=True):
        """Forward closure; returns (seen, parent) with parent edges for reporting."""
        seen, parent = set(), {}
        stack = [(r, None) for r in roots]
        while stack:
            a, p = stack.pop()
            if a in seen:
                continue
            seen.add(a)
            parent[a] = p
            if stop_leaves and is_leaf(self.img.funcs[a].name):
                continue
            if not follow_veneers and a in self.img.veneer:
                continue
            for b in self.successors(a, follow_sites):
                if b not in seen:
                    stack.append((b, a))
        return seen, parent

    def edge_to(self, a, parent) -> str:
        p = parent.get(a)
        while p is not None and p in self.img.veneer:
            p = parent.get(p)
        return f"{self.img.name(p) if p is not None else '<root>'} -> {self.img.name(a)}"

    def roots(self, rows) -> list[int]:
        out = []
        for (n, o) in rows:
            got = self.img.resolve(n, o)
            if not got:
                self.failures.append(f"W2: row {n} matches no symbol")
            out += got
        return out

    # W6 ---------------------------------------------------------------
    def graph(self, seen) -> dict:
        adj = {}
        for a in seen:
            if a in self.img.veneer:
                continue
            if is_leaf(self.img.funcs[a].name):
                adj[a] = set()
                continue
            nx = {self.img.collapse(b) for b in self.successors(a, record=False)}
            adj[a] = {b for b in nx if b in seen}
        return adj

    @staticmethod
    def sccs(adj) -> list[list]:
        index, low, on, stack, out = {}, {}, set(), [], []
        counter = 0
        for v0 in adj:
            if v0 in index:
                continue
            work = [(v0, iter(sorted(adj[v0])))]
            index[v0] = low[v0] = counter
            counter += 1
            stack.append(v0)
            on.add(v0)
            while work:
                v, it = work[-1]
                pushed = False
                for w in it:
                    if w not in index:
                        index[w] = low[w] = counter
                        counter += 1
                        stack.append(w)
                        on.add(w)
                        work.append((w, iter(sorted(adj.get(w, ())))))
                        pushed = True
                        break
                    if w in on:
                        low[v] = min(low[v], index[w])
                if pushed:
                    continue
                work.pop()
                if work:
                    low[work[-1][0]] = min(low[work[-1][0]], low[v])
                if low[v] == index[v]:
                    comp = []
                    while True:
                        w = stack.pop()
                        on.discard(w)
                        comp.append(w)
                        if w == v:
                            break
                    if len(comp) > 1 or v in adj.get(v, ()):
                        out.append(sorted(comp))
        return out

    def match_recursion(self, comp) -> tuple[RecursionRow | None, list[str]]:
        cs = set(comp)
        fails = []
        for r in self.t.recursion:
            anchors = set(self.img.resolve(*r.anchor))
            if not anchors & cs:
                continue
            for (n, o) in r.chain:
                if not set(self.img.resolve(n, o)) & cs:
                    fails.append(f"W6: chain function {n} is outside the {r.anchor[0]} component")
            if r.size is not None and r.size != len(comp):
                fails.append(f"W6: {r.anchor[0]} component has {len(comp)} functions, row records {r.size}")
            return r, fails
        names = sorted(self.img.name(a) for a in comp)
        return None, [f"W6: undeclared cycle of {len(comp)}: {', '.join(names[:6])}"]

    def check_cycles(self, seen) -> list[str]:
        out = []
        for comp in self.sccs(self.graph(seen)):
            out += self.match_recursion(comp)[1]
        return out


# ---------------------------------------------------------------- W5

def check_producer(img: Image, t: Tables, follow_veneers=True) -> list[str]:
    """W5 (1) and (4) from the producer roots, plus the E14 empty bodies."""
    w = Walker(img, t)
    roots = w.roots(t.producer_roots)
    seen, parent = w.walk(roots, follow_veneers=follow_veneers)
    out = list(img.failures)
    for a in sorted(seen):
        n = img.funcs[a].name
        if a in img.veneer:
            continue
        if FORBID.search(n) or STDIO.match(n):
            out.append(f"W5 (1): forbidden {w.edge_to(a, parent)}")
    for n in t.empty_bodies:
        got = img.resolve(n)
        if not got:
            out.append(f"W5 (1): {n} is not in the image")
        for a in got:
            if img.edges[a] or img.sites[a]:
                out.append(f"W5 (1): {n} does not have an empty body")
    out += check_vtables(img, t)
    out += w.check_cycles(seen)
    return out + w.failures


def check_vtables(img: Image, t: Tables) -> list[str]:
    return [f"W4: vtable {v} is not classified" for v in img.vtable_symbols() if v not in t.vtables]


def check_danger_closure(img: Image, t: Tables) -> list[str]:
    """W5 (2): forward closure of the allocator, stdio and terminal symbols, no leaf stop."""
    w = Walker(img, t)
    danger = [a for a, f in img.funcs.items() if a not in img.veneer and is_danger(f.name)]
    seen, parent = w.walk(danger, stop_leaves=False, follow_sites=False)
    out = []
    for a in sorted(seen):
        if img.funcs[a].name in (HOOK, YIELD_POINT):
            out.append(f"W5 (2): {img.funcs[a].name} under an allocator or stdio function: "
                       f"{w.edge_to(a, parent)}")
    met = {}
    for a in seen:
        if img.sites[a] and a not in img.veneer:
            met[img.funcs[a].name] = met.get(img.funcs[a].name, 0) + len(img.sites[a])
    for n, c in sorted(met.items()):
        if n not in t.libc_row:
            out.append(f"W5 (2): {c} indirect site(s) in {n} outside the pinned libc row")
        elif t.libc_row[n] != c:
            out.append(f"W5 (2): {n} has {c} indirect site(s), libc row pins {t.libc_row[n]}")
    for n in sorted(set(t.libc_row) - set(met)):
        out.append(f"W5 (2): libc row {n} not met in the closure")
    return out


def sd_roots(img: Image, t: Tables) -> tuple[set, list[str]]:
    """W2 computed SD roots: callers outside the SD stack whose closure reaches the hook."""
    w = Walker(img, t)
    ro, rn = re.compile(t.sd_stack_obj), re.compile(t.sd_stack_name)

    def in_stack(a):
        f = img.funcs[a]
        return bool((f.obj and ro.search(f.obj)) or rn.search(f.name))

    hook = set(img.resolve(HOOK))
    out = set()
    for a in img.funcs:
        if a in img.veneer or in_stack(a):
            continue
        # The caller's edges into the SD stack (direct, or through its W4 rows'
        # targets); the closure through them, not through its other callees,
        # decides whether the SD call itself reaches the hook (W2).
        into = {img.collapse(b) for b in w.successors(a, record=False)}
        into = {b for b in into if in_stack(b)}
        if not into:
            continue
        seen, _p = w.walk(sorted(into))
        if seen & hook:
            out.add(a)
    return out, w.failures


def check_sd_and_yield_roots(img: Image, t: Tables) -> list[str]:
    """W5 (3): hook-reaching SD callers are rows; direct yield-point callers are yield roots."""
    w = Walker(img, t)
    out = []
    rows = set(w.roots(t.sd_windowed)) | set(w.roots(t.sd_unwindowed))
    computed, fails = sd_roots(img, t)
    out += fails
    for a in sorted(computed - rows):
        out.append(f"W5 (3): hook-reaching SD caller {img.name(a)} is in no SD-root row")
    ypt = set(img.resolve(YIELD_POINT))
    yrows = set(w.roots(t.yield_roots))
    callers = set()
    for a, f in img.funcs.items():
        if a in img.veneer:
            continue
        if {img.collapse(b) for b in img.edges[a]} & ypt:
            callers.add(a)
            if a not in yrows:
                out.append(f"W5 (3): {img.name(a)} calls {YIELD_POINT} and is no yield-root row")
    # The converse (Phase 4h): a yield-root row whose symbol has no direct
    # edge to the yield point names no yield point, so it proves nothing.
    for a in sorted(yrows - callers):
        out.append(f"W5 (3): yield-root row {img.name(a)} does not call {YIELD_POINT}")
    return out + w.failures
