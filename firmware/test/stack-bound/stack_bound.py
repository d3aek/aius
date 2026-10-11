"""stack-bound: worst-case main-stack depth over the producer-isolation graph.

Frames come from -fstack-usage (.su beside each object); a symbol without a
.su takes its frame from a declared table, and an unlisted one fails. A
veneer adds no frame; a tail call is charged as a call; an indirect site is
charged its deepest W4 target; a W3 leaf is not descended (as in the
isolation walk) and is charged its declared inclusive depth; a W6 component
is charged bound x (frame sum of its guard-admitted chain) plus the deepest
exit from any member. Below an unwindowed SD root the pass entry is charged
its own frame only (the hook call there never runs a pass).
"""
from __future__ import annotations

import dataclasses
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "producer-isolation"))

import aius_walk as aw  # noqa: E402

FPU_EXTENDED_FRAME = 104
MARGIN = 2048


def obj_stem(obj: str) -> str:
    m = re.search(r"\(([^)]+)\)$", obj)
    b = m.group(1) if m else os.path.basename(obj)
    return re.sub(r"\.o$", "", b)


def su_key(name: str) -> str:
    """'bool SdioCard::readSector(uint32_t, uint8_t*)' and 'foo.constprop.0' -> 'SdioCard::readSector', 'foo'."""
    # The demangler spells an anonymous namespace "(anonymous namespace)",
    # GCC's .su "{anonymous}"; one spelling, so both name the same key
    # (Phase 4g: write_body_chunk in aius_runtime_network_http.cpp).
    name = name.replace("(anonymous namespace)", "{anonymous}")
    n = aw.base_name(name.split("(", 1)[0].strip())
    return n.split()[-1] if n else n


def load_su(roots) -> dict:
    """{(object stem, key): [(bytes, kind)]} from every .su under the given directories."""
    out: dict = {}
    for root in roots:
        for d, _dirs, files in os.walk(root):
            for f in files:
                if not f.endswith(".su"):
                    continue
                stem = f[:-3]
                with open(os.path.join(d, f), encoding="utf-8", errors="replace") as fh:
                    for line in fh:
                        parts = line.rstrip("\n").split("\t")
                        if len(parts) < 3:
                            continue
                        name = parts[0].split(":", 3)[-1]
                        out.setdefault((stem, su_key(name)), []).append((int(parts[1]), parts[2]))
    return out


def demangle(names) -> dict:
    names = list(names)
    filt = aw.tool("c++filt")
    if not filt or not names:
        return {n: n for n in names}
    res = subprocess.run([filt], input="\n".join(names), capture_output=True, text=True, check=True).stdout
    return dict(zip(names, res.splitlines()))


@dataclass
class Chain:
    """A declared caller chain from a thread entry to a W2 SD root.

    hops: [(name, obj, via)], first the entry (loop), last the SD root; via is
    "call" (a direct edge, veneers resolved) or "indirect: <registration cite>"
    (the caller has an indirect site and the hop is address-taken).
    hook: the symbol the below-root walk ends at, the card-wait hook unless
    set (Phase 4f: the status render's chain ends at aius_producer_yield_point,
    reached through each render append).
    full: charge the root's whole resolved closure (its deepest path, every
    callee, W4 sites recorded on every visited node), not only the callees
    that reach a hook (Phase 4h: the accept, flush-input and Q2 re-put roots
    reach neither the card wait nor a yield, W2: a root may be any linked
    symbol, and the hop check fails one inlined away).
    under_hook_paths: the chain's total must not exceed the deepest hook
    chain's (Phase 4h D1: the Q2 re-put's charge sits under the deepest
    hook path).
    """
    label: str
    hops: list
    unwindowed: bool = False
    hook: str | None = None
    full: bool = False
    under_hook_paths: bool = False


@dataclass
class StackTables:
    declared: dict = field(default_factory=dict)  # (name, obj or None) -> bytes, for symbols with no .su
    leaf_inclusive: dict = field(default_factory=dict)  # W3 leaf name -> inclusive bytes
    pass_entry: str = "aius_producer_yield_wait"
    main_roots: list = field(default_factory=list)  # [(name, obj)]
    handlers: list = field(default_factory=list)  # [Handler]
    chains: list = field(default_factory=list)  # [Chain]
    extra_sites: list = field(default_factory=list)  # W4 rows only stack-bound's walks visit


@dataclass
class Handler:
    name: str
    obj: str | None
    priority: int
    aius_sites: list = field(default_factory=list)  # ["firmware/src/x.cpp:292"]
    framework_cite: tuple | None = None  # (path under framework-arduinoteensy, line, substring)
    # A declared inclusive depth: the handler is charged this and not walked
    # (its paths are cut at the handler entry). Used where 4e leaves a
    # handler's paths unchanged and the device floor backs the figure.
    declared_depth: int | None = None
    declared_why: str = ""


class StackBound:
    def __init__(self, img: aw.Image, t: aw.Tables, st: StackTables, su: dict):
        if st.extra_sites:
            t = dataclasses.replace(t, sites=list(t.sites) + list(st.extra_sites))
        self.img, self.t, self.st, self.su = img, t, st, su
        self.w = aw.Walker(img, t)
        self.failures: list[str] = []
        self.dem = demangle({f.name for f in img.funcs.values()})
        self.memo: dict = {}
        self.best: dict = {}
        self.unwindowed = set(self.w.roots(t.sd_unwindowed)) if t.sd_unwindowed else set()
        self.comp_of: dict = {}
        self.comp_charge: dict = {}

    def frame(self, a: int) -> int:
        if a in self.img.veneer:
            return 0
        f = self.img.funcs[a]
        hits = self.su.get((obj_stem(f.obj), su_key(self.dem.get(f.name, f.name)))) if f.obj else None
        if not hits:
            hits = self.su.get((obj_stem(f.obj) if f.obj else "", su_key(f.name)))
        if hits:
            bad = [k for (_b, k) in hits if k != "static"]
            if bad:
                self.failures.append(f"stack-bound: {self.img.name(a)} has a {bad[0]} frame")
            return max(b for (b, _k) in hits)
        for key in ((f.name, f.obj), (f.name, None)):
            for (n, o), v in self.st.declared.items():
                if n == aw.base_name(key[0]) and (o is None or (key[1] and o in key[1])):
                    return v
        self.failures.append(f"stack-bound: no frame for {self.img.name(a)} (.su or declared table)")
        return 0

    def prepare(self, roots):
        seen, _ = self.w.walk(roots)
        for comp in self.w.sccs(self.w.graph(seen)):
            row, fails = self.w.match_recursion(comp)
            self.failures += fails
            cid = len(self.comp_charge)
            for a in comp:
                self.comp_of[a] = cid
            chain = set()
            if row and row.charge == "sum":
                # every member frame counted once: an upper bound on any simple
                # path through the component, without a guard-admitted chain
                self.comp_charge[cid] = (comp, sum(self.frame(a) for a in comp))
            elif row:
                for (n, o) in row.chain:
                    chain |= set(self.img.resolve(n, o)) & set(comp)
                self.comp_charge[cid] = (comp, row.bound * sum(self.frame(a) for a in chain))
            else:
                self.comp_charge[cid] = (comp, sum(self.frame(a) for a in comp))

    def succ(self, a):
        return {self.img.collapse(b) for b in self.w.successors(a)}

    def depth(self, a: int, cut: bool = False) -> int:
        a = self.img.collapse(a)
        cut = cut or a in self.unwindowed
        key = (a, cut)
        if key in self.memo:
            return self.memo[key]
        self.memo[key] = 0  # cycles outside a declared component are reported by prepare()
        name = self.img.funcs[a].name
        if aw.is_leaf(name):
            if name not in self.st.leaf_inclusive:
                self.failures.append(f"stack-bound: no declared inclusive depth for leaf {name}")
            val, child = self.st.leaf_inclusive.get(name, 0), None
        elif cut and name == self.st.pass_entry:
            val, child = self.frame(a), None
        elif a in self.comp_of:
            comp, charge = self.comp_charge[self.comp_of[a]]
            cs = set(comp)
            exits = [(self.depth(x, cut), x) for m in comp for x in self.succ(m) if x not in cs]
            best = max(exits, default=(0, None))
            val, child = charge + best[0], best[1]
        else:
            kids = [(self.depth(b, cut), b) for b in self.succ(a)]
            best = max(kids, default=(0, None))
            val, child = self.frame(a) + best[0], best[1]
        self.memo[key] = val
        self.best[key] = child
        return val

    def path(self, a: int) -> list[str]:
        out, cut = [], False
        cur = self.img.collapse(a)
        while cur is not None and len(out) < 400:
            cut = cut or cur in self.unwindowed
            out.append(self.img.funcs[cur].name)
            cur = self.best.get((cur, cut))
        return out

    def root_depth(self, rows) -> tuple[int, str]:
        roots = self.w.roots(rows)
        self.prepare(roots)
        best = max(((self.depth(r), r) for r in roots), default=(0, None))
        return best[0], (" > ".join(self.path(best[1])) if best[1] is not None else "")

    # -- 4e scope: the paths through the card-wait hook -------------------
    def check_chain(self, ch: Chain):
        """Resolve a declared chain and check every hop edge; returns (failures, addresses)."""
        addrs = []
        for (n, o, _via) in ch.hops:
            got = self.img.resolve(n, o)
            if len(got) != 1:
                return [f"stack-bound: chain {ch.label}: hop {n} resolves to {len(got)} symbols"], None
            addrs.append(got[0])
        fails = []
        # Every hop's indirect sites are a W4 row at the measured count (a
        # hop is not walked, so the walk's own record never sees its drift).
        for a in addrs:
            n = len(self.img.sites[a]) if a not in self.img.veneer else 0
            if not n:
                continue
            r = self.w.row_for(a)
            if r is None:
                fails.append(f"stack-bound: chain {ch.label}: hop {self.img.funcs[a].name} has {n} "
                             f"unlisted indirect site(s)")
            elif r.count != n:
                fails.append(f"stack-bound: chain {ch.label}: hop {self.img.funcs[a].name} has {n} "
                             f"indirect site(s), row records {r.count}")
        for x, y, (n, _o, via) in zip(addrs, addrs[1:], ch.hops[1:]):
            if via == "call":
                if y not in {self.img.collapse(b) for b in self.img.edges[x]}:
                    fails.append(f"stack-bound: chain {ch.label}: {self.img.funcs[x].name} has no edge to {n}")
            elif not self.img.sites[x] or y not in self.img.taken:
                fails.append(f"stack-bound: chain {ch.label}: {self.img.funcs[x].name} has no indirect "
                             f"site reaching {n} (sites {len(self.img.sites[x])}, taken {y in self.img.taken})")
            else:
                r = self.w.row_for(x)
                if r is not None and y not in {self.img.collapse(b) for b in self.w.row_targets(r)}:
                    fails.append(f"stack-bound: chain {ch.label}: {self.img.funcs[x].name}'s W4 row "
                                 f"does not target {n}")
        return fails, addrs

    def below_root(self, root: int, unwindowed: bool, hook_name: str | None = None) -> tuple[int, list]:
        """Deepest path from a root to the hook (then the pass, or none when unwindowed).

        Only callees from which the hook is reachable are charged: paths that
        never reach the hook are unchanged by 4e (device floor). Components
        are charged as W6 rows; an undeclared one fails. The hook is the
        card-wait hook unless hook_name names another (a Chain's hook).
        """
        hook = set(self.img.resolve(hook_name or aw.HOOK))
        if hook_name is None:
            seen, _ = self.w.walk([root])
        else:
            # A chain with its own hook (the 4f render chain) records unlisted
            # sites only on hook-reaching nodes (dh below): its root also
            # reaches paths that never yield (the status render's FNET reads),
            # unchanged by 4f and backed by the device floor, as 4e's scope.
            seen, st = set(), [root]
            while st:
                a = st.pop()
                if a in seen:
                    continue
                seen.add(a)
                if not aw.is_leaf(self.img.funcs[a].name):
                    st.extend(b for b in self.w.successors(a, record=False) if b not in seen)
        hook_name = hook_name or aw.HOOK
        adj = {a: {self.img.collapse(b) for b in self.w.successors(a, record=False)} & seen
               for a in seen if a not in self.img.veneer and not aw.is_leaf(self.img.funcs[a].name)}
        radj: dict = {}
        for a, bs in adj.items():
            for b in bs:
                radj.setdefault(b, set()).add(a)
        reach, st = set(), list(hook & seen)
        while st:
            a = st.pop()
            if a not in reach:
                reach.add(a)
                st.extend(radj.get(a, ()))
        sub = {a: adj.get(a, set()) & reach for a in reach if a not in hook}
        comp_of, charge = {}, {}
        for comp in self.w.sccs(sub):
            row, fails = self.w.match_recursion(comp)
            self.failures += fails
            if row and row.charge == "sum":
                c = sum(self.frame(a) for a in comp)
            elif row:
                chain = set()
                for (n, o) in row.chain:
                    chain |= set(self.img.resolve(n, o)) & set(comp)
                c = row.bound * sum(self.frame(a) for a in chain)
            else:
                c = sum(self.frame(a) for a in comp)
            for a in comp:
                comp_of[a] = id(comp)
            charge[id(comp)] = (set(comp), c)
        for h in hook:
            self.prepare([h])
        memo, best = {}, {}

        def dh(a):
            if a in memo:
                return memo[a]
            memo[a] = 0
            if a in hook:
                v, child = self.depth(a, cut=unwindowed), None
            elif a in comp_of:
                cs, c = charge[comp_of[a]]
                exits = [(dh(x), x) for m in cs for x in sub.get(m, ()) if x not in cs]
                b = max(exits, default=(0, None))
                v, child = c + b[0], b[1]
            else:
                for _ in self.w.successors(a):  # record unlisted sites on hook-reaching nodes
                    break
                kids = [(dh(b), b) for b in sub.get(a, ())]
                b = max(kids, default=(0, None))
                v, child = self.frame(a) + b[0], b[1]
            memo[a], best[a] = v, child
            return v

        if root not in reach:
            self.failures.append(f"stack-bound: root {self.img.funcs[root].name} does not reach {hook_name}")
            return 0, []
        total = dh(root)
        path, cur = [], root
        while cur is not None and len(path) < 200:
            path.append(self.img.funcs[cur].name)
            cur = best.get(cur)
        if path and path[-1] == hook_name and not unwindowed:
            path.append("<pass>")
        return total, path

    def sd_paths(self) -> list:
        """[(label, prefix, below, path)] for every declared chain."""
        out = []
        for ch in self.st.chains:
            fails, addrs = self.check_chain(ch)
            self.failures += fails
            if addrs is None:
                continue
            prefix = sum(self.frame(a) for a in addrs[:-1])
            if ch.full:
                below, path = self.full_below(addrs[-1])
            else:
                below, path = self.below_root(addrs[-1], ch.unwindowed, ch.hook)
            out.append((ch.label, prefix, below, [h[0] for h in ch.hops[:-1]] + path))
        return out

    def full_below(self, root: int) -> tuple[int, list]:
        """Deepest path over the root's whole resolved closure (a Chain's full mode).

        The same charging as the handler walks: .su or declared frames, W3
        leaves at their declared inclusive depth, W4 sites at their deepest
        row target (an unlisted site fails), W6 components as rows (an
        undeclared one fails), and a pass under any yield or hook reached.
        """
        self.prepare([root])
        return self.depth(root), self.path(root)

    def exception_chain(self) -> int:
        """Per NVIC preemption level, the deepest handler path plus the FPU extended frame."""
        levels: dict = {}
        for h in self.st.handlers:
            got = self.img.resolve(h.name, h.obj)
            if not got:
                self.failures.append(f"stack-bound: handler {h.name} matches no symbol")
                continue
            if h.declared_depth is not None:
                d = h.declared_depth
            else:
                self.prepare(got)
                d = max(self.depth(a) for a in got)
            lvl = h.priority >> 4
            levels[lvl] = max(levels.get(lvl, 0), d + FPU_EXTENDED_FRAME)
        return sum(levels.values())


def check_under_hook_paths(chains, paths) -> list[str]:
    """Phase 4h D1: a chain flagged under_hook_paths totals no more than the deepest hook chain."""
    by = {label: p + b for (label, p, b, _path) in paths}
    hooked = [by[c.label] for c in chains if not c.full and c.label in by]
    worst = max(hooked, default=0)
    return [f"stack-bound: chain {c.label}: {by[c.label]} B exceeds the deepest hook path {worst} B"
            for c in chains if c.under_hook_paths and c.label in by and by[c.label] > worst]


MPU_GUARD = 32  # NOACCESS region at _ebss trapping overflow, cores/teensy4/startup.c:313-314


def stack_zone(img: aw.Image) -> int:
    """Usable main stack: _estack - _ebss less the 32 B MPU guard at _ebss."""
    return img.symbol_values["_estack"] - img.symbol_values["_ebss"] - MPU_GUARD


def check_margin(total: int, zone: int, margin: int = MARGIN) -> list[str]:
    if total + margin > zone:
        return [f"stack-bound: {total} B + {margin} B margin exceeds the {zone} B stack zone"]
    return []


INSTALL = re.compile(r"\b(attachInterruptVector|NVIC_SET_PRIORITY)\s*\(")


FUNC_NAME = re.compile(r"(\w+)\s*\(")


def install_sites(fw_root: str, subdirs=("src", "lib", "vendor")) -> list[tuple]:
    """Every attachInterruptVector / NVIC_SET_PRIORITY call (comments excluded).

    Keyed by (file, enclosing function, call text), never by line, so an
    edit elsewhere in the file does not move the key.
    """
    out = []
    for sd in subdirs:
        for d, _dirs, files in os.walk(os.path.join(fw_root, sd)):
            for f in sorted(files):
                if not f.endswith((".c", ".cpp", ".h", ".hpp")):
                    continue
                p = os.path.join(d, f)
                rel = "firmware/" + os.path.relpath(p, fw_root)
                in_block, depth, sig, func, blocks = False, 0, "", "", []
                with open(p, encoding="utf-8", errors="replace") as fh:
                    for line in fh:
                        code = line
                        if in_block:
                            if "*/" not in code:
                                continue
                            code, in_block = code.split("*/", 1)[1], False
                        code = re.sub(r"/\*.*?\*/", "", code)
                        code = re.sub(r'"(\\.|[^"\\])*"', '""', code).split("//", 1)[0]
                        if "/*" in code:
                            code, in_block = code.split("/*", 1)[0], True
                        if code.lstrip().startswith("#"):
                            continue
                        if depth > 0 and INSTALL.search(code):
                            out.append((rel, func, " ".join(code.split())))
                        # depth counts function-body braces only: namespace and
                        # extern "C" blocks are transparent.
                        for ch in code:
                            if ch == "{":
                                if depth == 0 and re.search(r"\b(namespace|extern)\b", sig):
                                    blocks.append("ns")
                                    sig = ""
                                    continue
                                if depth == 0:
                                    m = FUNC_NAME.search(sig)
                                    func = m.group(1) if m else ""
                                blocks.append("fn")
                                depth += 1
                            elif ch == "}":
                                kind = blocks.pop() if blocks else "fn"
                                if kind == "fn":
                                    depth = max(0, depth - 1)
                                if depth == 0:
                                    sig = ""
                            elif depth == 0:
                                sig = "" if ch == ";" else sig + ch
    return sorted(out)


def check_install_sites(handlers, sites, framework_root: str | None) -> list[str]:
    listed = {tuple(s) for h in handlers for s in h.aius_sites}
    out = [f"stack-bound: interrupt install site {s} is in no handler row" for s in sites if s not in listed]
    out += [f"stack-bound: handler row cites {s}, which is no install site" for s in sorted(listed - set(sites))]
    for h in handlers:
        if h.aius_sites:
            continue
        if not h.framework_cite:
            out.append(f"stack-bound: framework handler {h.name} cites no install line")
            continue
        path, line, text = h.framework_cite
        p = os.path.join(framework_root or "", path)
        try:
            with open(p, encoding="utf-8", errors="replace") as fh:
                lines = fh.readlines()
        except OSError:
            out.append(f"stack-bound: {h.name} install cite {path} not found")
            continue
        if not (0 < line <= len(lines) and text in lines[line - 1]):
            out.append(f"stack-bound: {h.name} install cite {path}:{line} no longer reads '{text}'")
    return out
