#!/usr/bin/env python3
"""Generate a KiCad 10 schematic sheet from a connectivity spec.

Reads a structured connectivity JSON (components + nets) and emits a
`.kicad_sch` whose connectivity is expressed entirely by net labels: every
component is placed on a grid, and every pin gets a short wire stub plus a
local label carrying its net name. Nets connect by matching label text, which
is ERC-valid and avoids programmatic wire routing.

Symbol geometry (pin positions) and the embedded `lib_symbols` cache come from
the `.kicad_sym` files in the library dir: the custom `aius.kicad_sym` plus the
vendored `standard-cache.kicad_sym` (copies of the stock KiCad symbols used,
pinned to the digest in `compose.yaml`).

Usage:
  electronics/generate.py connectivity/power-usb.json aius-carrier.kicad_sch
  electronics/generate.py <spec.json> <out.kicad_sch> [--lib-dir=electronics/lib]
  electronics/generate.py --project <manifest.json> <out_dir> [--lib-dir=DIR]

`--lib-dir` takes the `=DIR` form only; a space-separated value is parsed as a
positional argument and the run fails.

Coordinates: KiCad symbol libraries use Y-up; schematics use Y-down, so a
placed pin at library (px, py) lands at schematic (X + px, Y - py). A pin's
`(at x y angle)` is its connection tip and `angle` points from the tip toward
the body, so the outward (label) direction is angle + 180.
"""
import json
import math
import os
import sys
import uuid

NS = uuid.UUID("a1005ee1-0000-4000-8000-a105c0de0001")  # fixed => stable output
STUB = 5.08          # mm, wire stub length
GRID = 0.01          # rounding grid


def uid(key):
    return str(uuid.uuid5(NS, key))


def rnd(v):
    return round(v / GRID) * GRID


# ---------------------------------------------------------------------------
# Minimal S-expression parser (enough for .kicad_sym symbol bodies).
# ---------------------------------------------------------------------------
class QStr(str):
    """A token that was a quoted string in the source."""


def tokenize(text):
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in "() ":
            if c in "()":
                yield c
            i += 1
            continue
        if c in " \t\r\n":
            i += 1
            continue
        if c == '"':
            i += 1
            buf = []
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    buf.append(text[i:i + 2])
                    i += 2
                    continue
                buf.append(text[i])
                i += 1
            i += 1  # closing quote
            yield ('Q', "".join(buf))
            continue
        j = i
        while j < n and text[j] not in "() \t\r\n\"":
            j += 1
        yield text[i:j]
        i = j


def parse(text):
    """Parse the first S-expression in text; return (node, end_index)."""
    toks = list(tokenize(text))
    pos = 0

    def build():
        nonlocal pos
        tok = toks[pos]
        pos += 1
        if tok == "(":
            lst = []
            while toks[pos] != ")":
                lst.append(build())
            pos += 1
            return lst
        if isinstance(tok, tuple):  # quoted
            return QStr(tok[1])
        return tok

    return build()


def find_all(node, head):
    """Yield every list node whose first element == head, recursively."""
    if isinstance(node, list):
        if node and node[0] == head:
            yield node
        for child in node:
            yield from find_all(child, head)


# ---------------------------------------------------------------------------
# Symbol library loading.
# ---------------------------------------------------------------------------
def extract_symbol_blocks(text):
    """Yield (name, raw_text) for each top-level (symbol "..." ...) block."""
    key = '(symbol "'
    i = 0
    while True:
        i = text.find(key, i)
        if i < 0:
            return
        # name
        js = i + len(key)
        je = text.find('"', js)
        name = text[js:je]
        # paren-match from i
        depth, j = 0, i
        while j < len(text):
            if text[j] == "(":
                depth += 1
            elif text[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        raw = text[i:j + 1]
        # only top-level symbols (name without a unit suffix like _0_1)
        yield name, raw
        i = j + 1


def load_libs(lib_dir):
    """base_name -> {raw, pins:[{num,name,x,y,ang,hide}], numbers:[...]}"""
    syms = {}
    for fn in sorted(os.listdir(lib_dir)):
        if not fn.endswith(".kicad_sym"):
            continue
        text = open(os.path.join(lib_dir, fn)).read()
        for name, raw in extract_symbol_blocks(text):
            if "_" in name and name.rsplit("_", 2)[-2:] and name[-1].isdigit():
                # skip nested unit sub-symbols (e.g. R_1_1) at top scan; the
                # paren scanner already only sees top-level, but guard anyway.
                pass
            tree = parse(raw)
            pins = []
            for p in find_all(tree, "pin"):
                at = next(c for c in p if isinstance(c, list) and c and c[0] == "at")
                x, y, ang = float(at[1]), float(at[2]), float(at[3])
                nm = next((c[1] for c in p if isinstance(c, list) and c and c[0] == "name"), "")
                num = next((c[1] for c in p if isinstance(c, list) and c and c[0] == "number"), "")
                hide = any(isinstance(c, list) and c[:2] == ["hide", "yes"] for c in p)
                pins.append({"num": str(num), "name": str(nm), "x": x, "y": y,
                             "ang": ang, "hide": hide})
            if name in syms:
                continue
            syms[name] = {"raw": raw, "pins": pins,
                          "numbers": [p["num"] for p in pins]}
    return syms


# ---------------------------------------------------------------------------
# Geometry.
# ---------------------------------------------------------------------------
def pin_geometry(inst_x, inst_y, pin):
    """Return (conn_xy, stub_end_xy, label_angle) for a pin, rot=0/no-mirror."""
    cx = rnd(inst_x + pin["x"])
    cy = rnd(inst_y - pin["y"])          # Y flip: lib is Y-up, sheet Y-down
    out = (pin["ang"] + 180.0) % 360.0   # outward direction in library frame
    dx = math.cos(math.radians(out))
    dy = -math.sin(math.radians(out))    # Y flip applied to direction too
    sx = rnd(cx + STUB * dx)
    sy = rnd(cy + STUB * dy)
    lang = int(round(out)) % 360
    if lang not in (0, 90, 180, 270):
        lang = 0
    return (cx, cy), (sx, sy), lang


# ---------------------------------------------------------------------------
# Emission.
# ---------------------------------------------------------------------------
def fmt(v):
    s = ("%.4f" % v).rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


def emit(spec, syms, project="aius-carrier", instance_path=None,
         file_uuid=None, page="1"):
    """Emit one sheet's `.kicad_sch` body.

    A sheet is standalone (its own file uuid = the schematic root uuid) when
    `instance_path` is None; in a hierarchical project it is a sub-sheet whose
    symbol instances live at `<root_uuid>/<sheet_symbol_uuid>` and whose own
    file uuid is `file_uuid`. Nets named in `spec["global_nets"]` are emitted as
    global labels (project-wide, cross-sheet); all others stay local labels.
    """
    out = []
    w = out.append
    paper = spec.get("paper", "A3")
    global_nets = set(spec.get("global_nets", []))
    file_uuid = file_uuid or uid("root")
    inst_path = instance_path or ("/" + file_uuid)

    w("(kicad_sch")
    w("\t(version 20250114)")
    w('\t(generator "aius-generate")')
    w('\t(generator_version "10.0")')
    w('\t(uuid "%s")' % file_uuid)
    w('\t(paper "%s")' % paper)

    # lib_symbols: one embedded copy per unique lib_id.
    lib_ids = []
    for comp in spec["components"]:
        if comp["lib_id"] not in lib_ids:
            lib_ids.append(comp["lib_id"])
    w("\t(lib_symbols")
    for lib_id in lib_ids:
        base = lib_id.split(":", 1)[1]
        raw = syms[base]["raw"]
        renamed = raw.replace('(symbol "%s"' % base, '(symbol "%s"' % lib_id, 1)
        for line in renamed.splitlines():
            w("\t\t" + line.strip("\n"))
    w("\t)")

    # Components + their pin stubs/labels.
    for comp in spec["components"]:
        ref = comp["ref"]
        base = comp["lib_id"].split(":", 1)[1]
        sym = syms[base]
        x, y = comp["at"]
        rot = comp.get("rot", 0)
        w("\t(symbol")
        w('\t\t(lib_id "%s")' % comp["lib_id"])
        w("\t\t(at %s %s %d)" % (fmt(x), fmt(y), rot))
        w("\t\t(unit 1)")
        w("\t\t(exclude_from_sim no)")
        w("\t\t(in_bom yes)")
        w("\t\t(on_board yes)")
        w("\t\t(dnp no)")
        w('\t\t(uuid "%s")' % uid("sym:" + ref))
        # Reference + Value placed just outside the body.
        w('\t\t(property "Reference" "%s"' % ref)
        w("\t\t\t(at %s %s 0)" % (fmt(x - 10.16), fmt(y - 12.7)))
        w("\t\t\t(effects (font (size 1.27 1.27)) (justify left))")
        w("\t\t)")
        w('\t\t(property "Value" "%s"' % comp.get("value", ""))
        w("\t\t\t(at %s %s 0)" % (fmt(x - 10.16), fmt(y + 12.7)))
        w("\t\t\t(effects (font (size 1.27 1.27)) (justify left))")
        w("\t\t)")
        w('\t\t(property "Footprint" "%s"' % comp.get("footprint", ""))
        w("\t\t\t(at %s %s 0)" % (fmt(x), fmt(y)))
        w("\t\t\t(effects (font (size 1.27 1.27)) (hide yes))")
        w("\t\t)")
        if comp.get("mpn"):
            w('\t\t(property "MPN" "%s"' % comp["mpn"])
            w("\t\t\t(at %s %s 0)" % (fmt(x), fmt(y)))
            w("\t\t\t(effects (font (size 1.27 1.27)) (hide yes))")
            w("\t\t)")
        for num in sym["numbers"]:
            w('\t\t(pin "%s"' % num)
            w('\t\t\t(uuid "%s")' % uid("pin:%s:%s" % (ref, num)))
            w("\t\t)")
        w("\t\t(instances")
        w('\t\t\t(project "%s"' % project)
        w('\t\t\t\t(path "%s"' % inst_path)
        w('\t\t\t\t\t(reference "%s")' % ref)
        w("\t\t\t\t\t(unit 1)")
        w("\t\t\t\t)")
        w("\t\t\t)")
        w("\t\t)")
        w("\t)")

    # Build pin lookup: (ref, num) -> pin dict, and instance origin.
    pinmap = {}
    for comp in spec["components"]:
        base = comp["lib_id"].split(":", 1)[1]
        for p in syms[base]["pins"]:
            pinmap[(comp["ref"], p["num"])] = (comp["at"], p)

    # Net stubs + labels.
    for net, endpoints in spec["nets"].items():
        is_global = net in global_nets
        for ref, num in endpoints:
            key = (ref, str(num))
            if key not in pinmap:
                raise SystemExit("net %s: unknown pin %s.%s" % (net, ref, num))
            (ix, iy), pin = pinmap[key]
            conn, stub, lang = pin_geometry(ix, iy, pin)
            # wire stub
            w("\t(wire")
            w("\t\t(pts (xy %s %s) (xy %s %s))" %
              (fmt(conn[0]), fmt(conn[1]), fmt(stub[0]), fmt(stub[1])))
            w("\t\t(stroke (width 0) (type default))")
            w('\t\t(uuid "%s")' % uid("wire:%s:%s:%s" % (net, ref, num)))
            w("\t)")
            # label at stub end (global => cross-sheet net, else sheet-local)
            just = "right" if lang == 180 else "left"
            if is_global:
                w('\t(global_label "%s"' % net)
                w("\t\t(shape bidirectional)")
                w("\t\t(at %s %s %d)" % (fmt(stub[0]), fmt(stub[1]), lang))
                w("\t\t(effects (font (size 1.27 1.27)) (justify %s))" % just)
                w('\t\t(uuid "%s")' % uid("glabel:%s:%s:%s" % (net, ref, num)))
                w('\t\t(property "Intersheetrefs" "${INTERSHEET_REFS}"')
                w("\t\t\t(at %s %s %d)" % (fmt(stub[0]), fmt(stub[1]), lang))
                w("\t\t\t(effects (font (size 1.27 1.27)) (hide yes))")
                w("\t\t)")
                w("\t)")
            else:
                w('\t(label "%s"' % net)
                w("\t\t(at %s %s %d)" % (fmt(stub[0]), fmt(stub[1]), lang))
                w("\t\t(effects (font (size 1.27 1.27)) (justify %s bottom))"
                  % just)
                w('\t\t(uuid "%s")' % uid("label:%s:%s:%s" % (net, ref, num)))
                w("\t)")

    # No-connects.
    for ref, num in spec.get("no_connects", []):
        (ix, iy), pin = pinmap[(ref, str(num))]
        conn, _, _ = pin_geometry(ix, iy, pin)
        w("\t(no_connect")
        w("\t\t(at %s %s)" % (fmt(conn[0]), fmt(conn[1])))
        w('\t\t(uuid "%s")' % uid("nc:%s:%s" % (ref, num)))
        w("\t)")

    w("\t(sheet_instances")
    w('\t\t(path "/"')
    w('\t\t\t(page "%s")' % page)
    w("\t\t)")
    w("\t)")
    w("\t(embedded_fonts no)")
    w(")")
    return "\n".join(out) + "\n"


def emit_root(project, sheets, root_uuid, paper="A3"):
    """Emit the hierarchical root schematic that instantiates each sub-sheet.

    `sheets` is a list of dicts: {name, file, sym_uuid, page, at:[x,y],
    size:[w,h]}. Cross-sheet connectivity is by global label inside the
    sub-sheets, so the root only carries the sheet symbols.
    """
    out = []
    w = out.append
    w("(kicad_sch")
    w("\t(version 20250114)")
    w('\t(generator "aius-generate")')
    w('\t(generator_version "10.0")')
    w('\t(uuid "%s")' % root_uuid)
    w('\t(paper "%s")' % paper)
    w("\t(lib_symbols)")
    for sh in sheets:
        x, y = sh["at"]
        sw, sh_h = sh["size"]
        w("\t(sheet")
        w("\t\t(at %s %s)" % (fmt(x), fmt(y)))
        w("\t\t(size %s %s)" % (fmt(sw), fmt(sh_h)))
        w("\t\t(exclude_from_sim no)")
        w("\t\t(in_bom yes)")
        w("\t\t(on_board yes)")
        w("\t\t(dnp no)")
        w("\t\t(fields_autoplaced yes)")
        w("\t\t(stroke (width 0.1524) (type solid))")
        w("\t\t(fill (color 0 0 0 0.0000))")
        w('\t\t(uuid "%s")' % sh["sym_uuid"])
        w('\t\t(property "Sheetname" "%s"' % sh["name"])
        w("\t\t\t(at %s %s 0)" % (fmt(x), fmt(y - 0.7112)))
        w("\t\t\t(effects (font (size 1.27 1.27)) (justify left bottom))")
        w("\t\t)")
        w('\t\t(property "Sheetfile" "%s"' % sh["file"])
        w("\t\t\t(at %s %s 0)" % (fmt(x), fmt(y + sh_h + 0.7112)))
        w("\t\t\t(effects (font (size 1.27 1.27)) (justify left top))")
        w("\t\t)")
        w("\t\t(instances")
        w('\t\t\t(project "%s"' % project)
        w('\t\t\t\t(path "/%s"' % root_uuid)
        w('\t\t\t\t\t(page "%s")' % sh["page"])
        w("\t\t\t\t)")
        w("\t\t\t)")
        w("\t\t)")
        w("\t)")
    w("\t(sheet_instances")
    w('\t\t(path "/"')
    w('\t\t\t(page "1")')
    w("\t\t)")
    for sh in sheets:
        w('\t\t(path "/%s"' % sh["sym_uuid"])
        w('\t\t\t(page "%s")' % sh["page"])
        w("\t\t)")
    w("\t)")
    w("\t(embedded_fonts no)")
    w(")")
    return "\n".join(out) + "\n"


def build_project(manifest_path, out_dir, lib_dir):
    """Generate the root schematic + every sub-sheet from a project manifest."""
    manifest = json.load(open(manifest_path))
    project = manifest["project"]
    paper = manifest.get("paper", "A3")
    base_dir = os.path.dirname(os.path.abspath(manifest_path))
    syms = load_libs(lib_dir)
    root_uuid = uid("root")

    sheets = []
    for i, sh in enumerate(manifest["sheets"]):
        name = sh["name"]
        sheets.append({
            "name": name,
            "file": name + ".kicad_sch",
            "spec": os.path.join(base_dir, sh["spec"]),
            "sym_uuid": uid("sheet-symbol:" + name),
            "file_uuid": uid("sheet-file:" + name),
            "page": str(sh.get("page", i + 2)),
            "at": sh.get("at", [25.4 + i * 60.96, 25.4]),
            "size": sh.get("size", [50.8, 25.4]),
        })

    root_path = os.path.join(out_dir, project + ".kicad_sch")
    open(root_path, "w").write(emit_root(project, sheets, root_uuid, paper))
    print("wrote %s (root, %d sheets)" % (root_path, len(sheets)))

    for sh in sheets:
        spec = json.load(open(sh["spec"]))
        inst_path = "/%s/%s" % (root_uuid, sh["sym_uuid"])
        text = emit(spec, syms, project=project, instance_path=inst_path,
                    file_uuid=sh["file_uuid"], page=sh["page"])
        sheet_path = os.path.join(out_dir, sh["file"])
        open(sheet_path, "w").write(text)
        print("wrote %s (%d components, %d nets)" %
              (sheet_path, len(spec["components"]), len(spec["nets"])))


def resolve_lib_dir(lib_dir, here):
    if os.path.isabs(lib_dir) or os.path.isdir(lib_dir):
        return lib_dir
    return os.path.join(here, "lib")


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    lib_dir = "electronics/lib"
    for a in argv:
        if a.startswith("--lib-dir="):
            lib_dir = a.split("=", 1)[1]
    if len(args) < 2:
        raise SystemExit(
            "usage: generate.py <spec.json> <out.kicad_sch> [--lib-dir=DIR]\n"
            "       generate.py --project <manifest.json> <out_dir> [--lib-dir=DIR]")
    here = os.path.dirname(os.path.abspath(__file__))
    lib_dir = resolve_lib_dir(lib_dir, here)

    if "--project" in argv:
        build_project(args[0], args[1], lib_dir)
        return

    spec_path, out_path = args[0], args[1]
    spec = json.load(open(spec_path))
    syms = load_libs(lib_dir)
    text = emit(spec, syms)
    open(out_path, "w").write(text)
    print("wrote %s (%d components, %d nets)" %
          (out_path, len(spec["components"]), len(spec["nets"])))


if __name__ == "__main__":
    main(sys.argv[1:])
