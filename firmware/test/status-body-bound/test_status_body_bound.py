#!/usr/bin/env python3
"""Worst-case GET /api/status body against its response buffer.

The status body is assembled by http_status_json() and the emitters it hands
the response buffer to, every byte through a bounded printf primitive
(snprintf, http_json_append, diag_append). This suite reads those calls out of
the firmware source and sums, for each one, the literal text plus the widest
output each conversion can produce on the Teensy's ILP32 ABI, multiplied by the
iteration bound of every loop around it. The sum must leave room for the NUL
inside AIUS_RUNTIME_TEENSY_STATUS_BODY (src/aius_runtime_teensy.h; the
runtime app body the dispatcher renders into is static_asserted to hold it),
so a key that pushes the worst case past the buffer fails here, on the host,
instead of as a 500 on a board.

The sum is an upper bound by construction: both arms of every branch and every
#if block are counted, and the rewind over the first literal's closing brace
is ignored. Every step fails closed rather than skipping:

  - a call that hands the buffer (an argument naming `capacity`) to a function
    with no definition under src/ throws;
  - a format that is not a literal or a same-function PROGMEM literal throws;
  - a `%s` whose argument is not a literal, a ternary of literals, a function
    returning only literals, a parameter supplied by a literal, a sized char
    array, or the build-identity macro throws;
  - a float conversion of anything but a `float` throws (a double `%f` has no
    useful bound);
  - a loop around an append whose condition names no resolvable constant
    throws;
  - a direct write into the buffer or the length outside the primitives throws.

Run: scripts/test.sh --environment native --suite status-body-bound

`--limit N` judges against N bytes instead of the header's value, so the
verdict can be exercised at a chosen size: test.sh also runs it at the worst
case itself (`--limit-at-worst`), which must fail.
"""

import argparse
import configparser
import re
import sys
from pathlib import Path

FW = Path(__file__).resolve().parents[2]
SRC = FW / "src"
LIMIT_HEADER = SRC / "aius_runtime_teensy.h"
LIMIT_MACRO = "AIUS_RUNTIME_TEENSY_STATUS_BODY"
ROOT_EMITTER = "http_status_json"

# Primitive -> (capacity argument index, format argument index).
PRIMITIVES = {"snprintf": (1, 2), "http_json_append": (1, 3), "diag_append": (1, 3)}
CONTROL = {"if", "for", "while", "switch", "return", "sizeof", "do", "defined", "catch"}

# Teensy 4.1 (Cortex-M7, arm-none-eabi): int and long are 32-bit, long long
# 64-bit. Widest decimal/hex/octal text per length modifier.
SIGNED = {"hh": 4, "h": 6, "": 11, "l": 11, "ll": 20, "z": 11, "j": 20, "t": 11}
UNSIGNED = {"hh": 3, "h": 5, "": 10, "l": 10, "ll": 20, "z": 10, "j": 20, "t": 10}
HEX = {"hh": 2, "h": 4, "": 8, "l": 8, "ll": 16, "z": 8, "j": 16, "t": 8}
OCTAL = {"hh": 3, "h": 6, "": 11, "l": 11, "ll": 22, "z": 11, "j": 22, "t": 11}
FLT_MAX_INTEGER_DIGITS = 39  # FLT_MAX ~ 3.40e38

CONVERSION = re.compile(
    r"%(?P<flags>[-+ #0]*)(?P<width>\*|\d+)?(?:\.(?P<precision>\*|\d*))?"
    r"(?P<length>hh|h|ll|l|L|j|z|t)?(?P<conv>[diouxXeEfFgGaAscp%])"
)


class BoundError(Exception):
    pass


# --- source scanning ---------------------------------------------------------


def end_of_string(text, start):
    index = start + 1
    while index < len(text):
        if text[index] == "\\":
            index += 2
            continue
        if text[index] == '"':
            return index + 1
        if text[index] == "\n":
            break
        index += 1
    raise BoundError(f"unterminated string literal at offset {start}")


CHAR_LITERAL = re.compile(r"'(?:\\.|[^'\\])'")


def strip_comments(text):
    out = []
    index = 0
    while index < len(text):
        c = text[index]
        if c == '"':
            end = end_of_string(text, index)
            out.append(text[index:end])
            index = end
            continue
        if c == "'":
            literal = CHAR_LITERAL.match(text, index)
            if literal:
                out.append(literal.group(0))
                index = literal.end()
                continue
        if text.startswith("//", index):
            while index < len(text) and text[index] != "\n":
                index += 1
            continue
        if text.startswith("/*", index):
            end = text.index("*/", index + 2)
            out.append("\n" * text.count("\n", index, end))
            index = end + 2
            continue
        out.append(c)
        index += 1
    return "".join(out)


def match_delimiter(text, start, open_, close):
    depth = 0
    index = start
    while index < len(text):
        c = text[index]
        if c == '"':
            index = end_of_string(text, index)
            continue
        if c == "'":
            literal = CHAR_LITERAL.match(text, index)
            if literal:
                index = literal.end()
                continue
        if c == open_:
            depth += 1
        elif c == close:
            depth -= 1
            if depth == 0:
                return index
        index += 1
    raise BoundError(f"unbalanced {open_}{close} from offset {start}")


def split_top(text, separator=","):
    parts, depth, start, index = [], 0, 0, 0
    while index < len(text):
        c = text[index]
        if c == '"':
            index = end_of_string(text, index)
            continue
        if c == "'":
            literal = CHAR_LITERAL.match(text, index)
            if literal:
                index = literal.end()
                continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == separator and depth == 0:
            parts.append(text[start:index])
            start = index + 1
        index += 1
    if text.strip():
        parts.append(text[start:])
    return [part.strip() for part in parts]


ESCAPES = {'"': '"', "\\": "\\", "'": "'", "n": "\n", "t": "\t", "r": "\r", "0": "\0"}


def literal_value(text):
    """Value of a run of adjacent string literals, or None."""
    index, value, count = 0, "", 0
    text = text.strip()
    while index < len(text):
        while index < len(text) and text[index].isspace():
            index += 1
        if index >= len(text) or text[index] != '"':
            break
        end = end_of_string(text, index)
        raw = text[index + 1 : end - 1]
        i = 0
        while i < len(raw):
            if raw[i] == "\\":
                i += 1
                if raw[i] not in ESCAPES:
                    raise BoundError(f"unsupported escape \\{raw[i]} in {text!r}")
                value += ESCAPES[raw[i]]
            else:
                value += raw[i]
            i += 1
        count += 1
        index = end
    return value if count and index >= len(text) else None


class Sources:
    def __init__(self):
        self.code = {path: strip_comments(path.read_text()) for path in sorted(SRC.glob("*.cpp"))}
        headers = sorted(SRC.glob("*.h")) + sorted((FW / "lib").rglob("*.h"))
        headers += sorted((FW / "lib").rglob("*.c")) + sorted((FW / "lib").rglob("*.cpp"))
        self.all_text = list(self.code.values()) + [strip_comments(p.read_text(errors="replace")) for p in headers]
        self.functions = {}

    def function(self, name):
        """(parameter list, body) of the single definition of `name` under src/."""
        if name in self.functions:
            return self.functions[name]
        found = []
        pattern = re.compile(rf"\b{name}\s*\(")
        for text in self.code.values():
            for match in pattern.finditer(text):
                open_paren = match.end() - 1
                close_paren = match_delimiter(text, open_paren, "(", ")")
                index = close_paren + 1
                while index < len(text) and text[index].isspace():
                    index += 1
                if index >= len(text) or text[index] != "{":
                    continue
                # A call inside a definition is followed by `{` only in a
                # control statement; the definition's prefix is a type.
                prefix = text[max(0, match.start() - 160) : match.start()]
                if re.search(r"\b(if|while|for|switch|return)\s*\($", prefix.rstrip()) or prefix.rstrip().endswith("("):
                    continue
                close_brace = match_delimiter(text, index, "{", "}")
                params = split_top(text[open_paren + 1 : close_paren])
                found.append((params, text[index + 1 : close_brace]))
        if len(found) != 1:
            raise BoundError(f"expected one definition of {name}() under src/, found {len(found)}")
        self.functions[name] = found[0]
        return found[0]

    def definitions(self, name):
        """Every `#define NAME expr` / `const[expr] T NAME = expr;` value text."""
        values = []
        define = re.compile(rf"^[ \t]*#[ \t]*define[ \t]+{name}[ \t]+(.+)$", re.M)
        const = re.compile(rf"\b(?:constexpr|const)\b[^;=(){{}}]*?\b{name}\s*=\s*([^;]+);")
        for text in self.all_text:
            values += [m.group(1).strip() for m in define.finditer(text)]
            values += [m.group(1).strip() for m in const.finditer(text)]
        return values

    def char_array_sizes(self, field):
        sizes = []
        pattern = re.compile(rf"\bchar\s+{field}\s*\[\s*([^\]]+?)\s*\]")
        for text in self.all_text:
            sizes += [m.group(1) for m in pattern.finditer(text)]
        return sizes

    def is_float(self, name):
        pattern = re.compile(rf"\b(?:volatile\s+)?float\s+(?:volatile\s+)?{name}\b")
        double = re.compile(rf"\bdouble\s+(?:volatile\s+)?{name}\b")
        return any(pattern.search(t) for t in self.all_text) and not any(double.search(t) for t in self.all_text)


# --- constant and string-argument resolution ----------------------------------


def evaluate(sources, expr, body="", depth=0):
    if depth > 20:
        raise BoundError(f"constant recursion resolving {expr!r}")
    text = expr.strip()
    text = re.sub(r"\((?:const\s+)?(?:unsigned\s+)?(?:u?int\d+_t|size_t|unsigned|int|long|uint32_t)\)", "", text)
    text = re.sub(r"\b(?:static_cast|uint32_t|size_t)\s*<[^>]*>", "", text)

    def replace(match):
        token = match.group(0)
        if re.fullmatch(r"\d+[uUlL]*", token) or re.fullmatch(r"0[xX][0-9a-fA-F]+[uUlL]*", token):
            return str(int(token.rstrip("uUlL"), 0))
        local = re.search(rf"\b(?:const\s+)?(?:u?int\d+_t|size_t|unsigned)\s+(?:const\s+)?{token}\s*=\s*([^;]+);", body)
        if local:
            return str(evaluate_limit(sources, local.group(1), body, depth + 1))
        candidates = {evaluate(sources, value, "", depth + 1) for value in sources.definitions(token)}
        if len(candidates) != 1:
            raise BoundError(f"constant {token} resolves to {sorted(candidates) or 'nothing'}")
        return str(candidates.pop())

    text = re.sub(r"\b0[xX][0-9a-fA-F]+[uUlL]*\b|\b\d+[uUlL]*\b|\b[A-Za-z_]\w*\b", replace, text)
    if not re.fullmatch(r"[\d\s()+\-*/]+", text):
        raise BoundError(f"not an integer constant expression: {expr!r}")
    return int(eval(text.replace("/", "//"), {"__builtins__": {}}))  # noqa: S307 - digits and operators only


def evaluate_limit(sources, expr, body, depth=0):
    """A loop bound: a constant, or a `a < B ? a : B` minimum whose B is one."""
    text = expr.strip()
    ternary = split_ternary(text)
    if ternary:
        condition, left, right = ternary
        comparison = re.fullmatch(r"\(?\s*(.+?)\s*<=?\s*(.+?)\s*\)?", condition)
        if comparison and comparison.group(1) == left.strip() and comparison.group(2) == right.strip():
            return evaluate(sources, right, body, depth)
        raise BoundError(f"loop limit {expr!r} is a ternary but not a minimum")
    return evaluate(sources, text, body, depth)


def split_ternary(text):
    """(condition, then, else) for a top-level ternary, or None."""
    depth, index, question = 0, 0, -1
    while index < len(text):
        c = text[index]
        if c == '"':
            index = end_of_string(text, index)
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif depth == 0 and c == "?" and question < 0:
            question = index
        elif depth == 0 and c == ":" and question >= 0 and text[index - 1] != ":" and text[index + 1 : index + 2] != ":":
            nested = 0
            for inner in text[question + 1 : index]:
                nested += inner == "?"
            colons = text[question + 1 : index].count(":")
            if nested == colons:
                return text[:question], text[question + 1 : index], text[index + 1 :]
        index += 1
    return None


def strip_parens(text):
    text = text.strip()
    while text.startswith("(") and match_delimiter(text, 0, "(", ")") == len(text) - 1:
        text = text[1:-1].strip()
    return text


class Context:
    def __init__(self, name, params, body, args, caller):
        self.name, self.body, self.caller = name, body, caller
        names = [re.findall(r"\w+", p)[-1] for p in params if re.findall(r"\w+", p) and p.strip() != "void"]
        self.params = dict(zip(names, args)) if args is not None else {}


def build_revision_bound():
    """AIUS_BUILD_GIT_REV is `<rev>+<env>[.cpN]` (scripts/build_metadata.py):
    a full 40-hex revision bounds `git rev-parse --short=12`, and the widest
    env is read from platformio.ini, "fault" standing in for the fault env."""
    parser = configparser.ConfigParser(interpolation=None, strict=False)
    parser.read(FW / "platformio.ini")
    envs = [s.split(":", 1)[1] for s in parser.sections() if s.startswith("env:")]
    metadata = (FW / "scripts" / "build_metadata.py").read_text()
    if '"--short=12"' not in metadata or 'f"{identity}.cp{checkpoint}"' not in metadata:
        raise BoundError("build_metadata.py no longer has the modelled revision shape")
    return 40 + 1 + max(len(e) for e in envs) + len(".cp9")


STRING_MACROS = {
    "AIUS_BUILD_GIT_REV": build_revision_bound,
    # `%Y-%m-%dT%H:%M:%SZ` (build_metadata.py), or the "unknown" default.
    "AIUS_BUILD_UTC": lambda: len("2026-01-01T00:00:00Z"),
}


def string_bound(sources, expr, ctx, depth=0):
    if depth > 20:
        raise BoundError(f"{ctx.name}(): string recursion resolving {expr!r}")
    text = strip_parens(re.sub(r"^\(\s*const\s+char\s*\*\s*\)", "", expr.strip()))
    value = literal_value(text)
    if value is not None:
        return len(value)
    ternary = split_ternary(text)
    if ternary:
        return max(string_bound(sources, ternary[1], ctx, depth + 1), string_bound(sources, ternary[2], ctx, depth + 1))
    if text in STRING_MACROS:
        return STRING_MACROS[text]()
    if re.fullmatch(r"[A-Za-z_]\w*", text):
        if text in ctx.params and ctx.caller is not None:
            return string_bound(sources, ctx.params[text], ctx.caller, depth + 1)
        local = re.search(rf"\bconst\s+char\s*\*\s*(?:const\s+)?{text}\s*=\s*([^;]+);", ctx.body)
        if local:
            return string_bound(sources, local.group(1), ctx, depth + 1)
        array = re.search(rf"\bchar\s+{text}\s*\[\s*([^\]]+)\]", ctx.body)
        if array:
            return evaluate(sources, array.group(1), ctx.body) - 1
        strings = [literal_value(v) for v in sources.definitions(text)]
        if strings and all(s is not None for s in strings):
            return max(len(s) for s in strings)
        sizes = {evaluate(sources, s) for s in sources.char_array_sizes(text)}
        if len(sizes) == 1:
            return sizes.pop() - 1  # a file-scope char array: its NUL bounds it
        raise BoundError(f"{ctx.name}(): cannot bound the %s argument {text!r}")
    call = re.fullmatch(r"([A-Za-z_]\w*)\s*\((.*)\)", text, re.S)
    if call:
        _, body = sources.function(call.group(1))
        returns = re.findall(r"\breturn\b([^;]+);", body)
        if not returns:
            raise BoundError(f"{call.group(1)}() returns nothing to bound")
        inner = Context(call.group(1), [], body, None, None)
        return max(string_bound(sources, r, inner, depth + 1) for r in returns)
    member = re.fullmatch(r"[A-Za-z_][\w.\->\[\]]*?(?:\.|->)([A-Za-z_]\w*)", text)
    if member:
        sizes = {evaluate(sources, s) for s in sources.char_array_sizes(member.group(1))}
        if len(sizes) != 1:
            raise BoundError(f"{ctx.name}(): char array {member.group(1)} has sizes {sorted(sizes) or 'none'}")
        return sizes.pop() - 1
    raise BoundError(f"{ctx.name}(): cannot bound the %s argument {text!r}")


NARROW_TYPES = {"bool": 1, "uint8_t": 3, "int8_t": 4, "uint16_t": 5, "int16_t": 6}
INTEGER_CAST = re.compile(r"^\(\s*(?:const\s+)?(?:unsigned|signed)?\s*(?:long\s+long|long|int|char|short|u?int\d+_t|size_t)?\s*\)")


def integer_bound(sources, expr, ctx, natural, depth=0):
    """Widest decimal text of one integer argument: the conversion's own width,
    narrowed only by what the source proves: a literal or constant, a ternary
    of those, or a name every firmware declaration of which is a narrow type."""
    if depth > 10:
        return natural
    text = strip_parens(expr)
    while INTEGER_CAST.match(text) and INTEGER_CAST.match(text).end() > 2:
        text = strip_parens(text[INTEGER_CAST.match(text).end() :])
    ternary = split_ternary(text)
    if ternary:
        return max(integer_bound(sources, b, ctx, natural, depth + 1) for b in ternary[1:])
    try:
        value = evaluate(sources, text, ctx.body)
        return min(natural, len(str(value)))
    except BoundError:
        pass
    name = re.fullmatch(r"(?:[A-Za-z_]\w*(?:\[[^\]]*\])?(?:\.|->))*([A-Za-z_]\w*)(\[[^\]]*\])?", text)
    if name is None:
        return natural
    array = name.group(2) is not None
    pattern = re.compile(
        rf"\b(?:static\s+)?(?:volatile\s+)?(?:const\s+)?(bool|u?int\d+_t|unsigned|int|long|float|double|size_t|[A-Z]\w*)\s+"
        rf"(?:volatile\s+)?{name.group(1)}\s*(\[)?"
    )
    types = set()
    for source in [ctx.body] + sources.all_text:
        for match in pattern.finditer(source):
            if (match.group(2) is not None) == array:
                types.add(match.group(1))
    if len(types) == 1 and next(iter(types)) in NARROW_TYPES:
        return min(natural, NARROW_TYPES[next(iter(types))])
    return natural


def float_name(expr):
    text = strip_parens(re.sub(r"^\(\s*double\s*\)", "", strip_parens(expr)))
    match = re.fullmatch(r"(?:[A-Za-z_][\w\[\]]*(?:\.|->))*([A-Za-z_]\w*)", text)
    return match.group(1) if match else None


# --- format width ------------------------------------------------------------


def format_bound(sources, fmt, args, ctx):
    """Worst-case characters one format produces (without the NUL)."""
    total, index, arg = 0, 0, 0
    while index < len(fmt):
        if fmt[index] != "%":
            total += 1
            index += 1
            continue
        match = CONVERSION.match(fmt, index)
        if match is None:
            raise BoundError(f"{ctx.name}(): unparsed conversion at {fmt[index:index + 12]!r}")
        index = match.end()
        conv, length = match.group("conv"), match.group("length") or ""
        if conv == "%":
            total += 1
            continue
        width = 0
        if match.group("width") == "*":
            width = evaluate(sources, args[arg], ctx.body)
            arg += 1
        elif match.group("width"):
            width = int(match.group("width"))
        precision = match.group("precision")
        if precision == "*":
            raise BoundError(f"{ctx.name}(): `.*` precision is not modelled")
        if arg >= len(args):
            raise BoundError(f"{ctx.name}(): format {fmt[:40]!r}... has more conversions than arguments")
        value = args[arg]
        arg += 1
        flags = match.group("flags")
        if conv in "di":
            natural = integer_bound(sources, value, ctx, SIGNED[length])
        elif conv == "u":
            natural = integer_bound(sources, value, ctx, UNSIGNED[length]) + ("+" in flags or " " in flags)
        elif conv in "xX":
            natural = HEX[length] + 2 * ("#" in flags)
        elif conv == "o":
            natural = OCTAL[length]
        elif conv == "c":
            natural = 1
        elif conv == "p":
            natural = 10
        elif conv == "s":
            natural = string_bound(sources, value, ctx)
            if precision not in (None, ""):
                natural = min(natural, int(precision))
        elif conv in "gG":
            digits = int(precision) if precision not in (None, "") else 6
            natural = 1 + max(digits, 1) + 1 + len("e-308")
        elif conv in "eE":
            digits = int(precision) if precision not in (None, "") else 6
            natural = 1 + 1 + 1 + digits + len("e-308")
        elif conv in "fF":
            name = float_name(value)
            if name is None or not sources.is_float(name):
                raise BoundError(f"{ctx.name}(): %{conv} of {value!r}, which is not a float, has no useful bound")
            digits = int(precision) if precision not in (None, "") else 6
            natural = 1 + FLT_MAX_INTEGER_DIGITS + (1 + digits if digits else 0)
        else:
            raise BoundError(f"{ctx.name}(): conversion %{conv} is not modelled")
        total += max(width, natural)
    if arg != len(args):
        raise BoundError(f"{ctx.name}(): format {fmt[:40]!r}... consumes {arg} of {len(args)} arguments")
    return total


# --- the walk ----------------------------------------------------------------

LOOP = re.compile(r"\b(for|while)\s*\(")
DIRECT_WRITES = [
    re.compile(r"\b(?:memcpy|memmove|strcpy|strncpy|strcat|strncat|sprintf|vsnprintf)\s*\("),
    re.compile(r"\*\s*length\s*(?:[+\-]?=(?!=)|\+\+|--)"),
    re.compile(r"(?<![\w*])length\s*(?:[+\-]=|\+\+|--)"),
    re.compile(r"(?<!size_t )(?<![\w*])length\s*=(?!=)"),
    re.compile(r"\b(?:body|output)\s*\["),
]


def loop_spans(sources, body, name):
    """[(start, end, bound)] for every loop body in `body`."""
    spans = []
    for match in LOOP.finditer(body):
        open_paren = match.end() - 1
        close_paren = match_delimiter(body, open_paren, "(", ")")
        header = body[open_paren + 1 : close_paren]
        index = close_paren + 1
        while body[index].isspace():
            index += 1
        if body[index] == ";":
            continue  # the `while (...)` closing a do-loop, or an empty loop
        end = match_delimiter(body, index, "{", "}") if body[index] == "{" else body.index(";", index)
        condition = split_top(header, ";")[1] if match.group(1) == "for" else header
        bounds = []
        for conjunct in condition.split("&&"):
            limit = re.fullmatch(r"\s*\(?\s*[A-Za-z_]\w*\s*(<=?)\s*(.+?)\s*\)?\s*", conjunct)
            if limit is None:
                continue
            try:
                bounds.append(evaluate_limit(sources, limit.group(2), body) + (limit.group(1) == "<="))
            except BoundError:
                continue
        if not bounds:
            raise BoundError(f"{name}(): the loop `{match.group(1)} ({header.strip()})` has no constant bound")
        spans.append((index, end, min(bounds)))
    return spans


def call_sites(body):
    pattern = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
    for match in pattern.finditer(body):
        if match.group(1) in CONTROL:
            continue
        open_paren = match.end() - 1
        close_paren = match_delimiter(body, open_paren, "(", ")")
        yield match.start(), match.group(1), split_top(body[open_paren + 1 : close_paren])


def named_format(body, argument):
    name = re.fullmatch(r"\s*([A-Za-z_]\w*)\s*", argument)
    if name is None:
        return None
    declaration = re.search(
        rf"\bstatic\s+const\s+char\s+{name.group(1)}\s*\[\s*\]\s*PROGMEM\s*=\s*((?:\"(?:\\.|[^\"\\])*\"\s*)+);", body
    )
    return literal_value(declaration.group(1)) if declaration else None


def walk(sources, name, args=None, caller=None, rows=None, depth=0):
    """Worst-case bytes `name` appends to the response buffer."""
    if depth > 12:
        raise BoundError(f"{name}(): emitter recursion too deep")
    params, body = sources.function(name)
    ctx = Context(name, params, body, args, caller)
    for pattern in DIRECT_WRITES:
        hit = pattern.search(body)
        if hit:
            raise BoundError(f"{name}() writes the buffer or its length outside the primitives: {hit.group(0)!r}")
    spans = loop_spans(sources, body, name)
    total = 0
    for position, callee, call_args in call_sites(body):
        multiplier = 1
        for start, end, bound in spans:
            if start <= position <= end:
                multiplier *= bound
        if callee in PRIMITIVES:
            capacity_index, format_index = PRIMITIVES[callee]
            if not re.search(r"\bcapacity\b", call_args[capacity_index]):
                continue  # a scratch buffer of its own; bounded where it is printed
            fmt = literal_value(call_args[format_index])
            if fmt is None:
                fmt = named_format(body, call_args[format_index])
            if fmt is None:
                raise BoundError(f"{name}() calls {callee}() with an unreadable format {call_args[format_index]!r}")
            total += multiplier * format_bound(sources, fmt, call_args[format_index + 1 :], ctx)
            continue
        if any(re.search(r"\bcapacity\b", a) for a in call_args):
            child = walk(sources, callee, call_args, ctx, None, depth + 1) * multiplier
            if rows is not None:
                rows.append((callee, child))
            total += child
    return total


def limit_bytes(sources):
    values = sources.definitions(LIMIT_MACRO)
    if not values:
        raise BoundError(f"{LIMIT_MACRO} is not defined")
    return evaluate(sources, values[0])


# --- self-checks of the model ------------------------------------------------


def self_checks(sources):
    ctx = Context("self_check", [], "const char *state = flag ? \"replay\" : \"acquiring\";", None, None)
    cases = [
        ("%lu", ["x"], 10),
        ("%ld", ["x"], 11),
        ("%llu", ["x"], 20),
        ("0x%08lX", ["x"], 10),
        ('"%s"', ['flag ? "true" : "false"'], 7),
        ("%s", ["state"], 9),
        ("%.17g", ["x"], 24),
        ("%u%%", ["x"], 11),
    ]
    for fmt, args, expected in cases:
        got = format_bound(sources, fmt, args, ctx)
        if got != expected:
            raise AssertionError(f"self-check: {fmt!r} bounds to {got}, expected {expected}")
    for fmt, args in [("%f", ["some_double"]), ("%s", ["mystery"]), ("%lu", [])]:
        try:
            format_bound(sources, fmt, args, ctx)
        except BoundError:
            continue
        raise AssertionError(f"self-check: {fmt!r} with {args} did not fail closed")
    unbounded = "for (;;) { http_json_append(output, capacity, length, \"x\"); }"
    try:
        loop_spans(sources, unbounded, "self_check")
    except BoundError:
        pass
    else:
        raise AssertionError("self-check: an unbounded loop did not fail closed")


def fits(worst, limit):
    """The verdict: the body plus its NUL must fit the buffer."""
    return worst + 1 <= limit


# Keys a plan spent headroom on, pinned to the emitter whose formats the walk
# bounds: a key moved to an emitter the walk never reaches (or renamed away)
# fails here instead of silently leaving the bound.
PINNED_KEYS = {
    # aius-sample-rate-following Phase 4: the fill-referenced trim and the
    # fill sensor's window sequence and last exclusion bits.
    "http_clock_json": (
        "fill_offset_millihz",
        "fill_setpoint_frames",
        "fill_open_boundary_pending",
        "fill_trims",
        "fill_trim_last_at_ms",
        "fill_applied_at_ms",
        "fill_window_seq",
        "fill_window_exclusions",
    ),
}


def check_pinned_keys(sources, rows):
    walked = {callee for callee, _ in rows}
    for emitter, keys in PINNED_KEYS.items():
        if emitter not in walked:
            raise BoundError(f"{emitter}() is not reached by the walk from {ROOT_EMITTER}()")
        _, body = sources.function(emitter)
        for key in keys:
            if f'\\"{key}\\":' not in body:
                raise BoundError(f"{emitter}() no longer renders {key!r}, so the bound does not cover it")


def main(argv):
    parser = argparse.ArgumentParser()
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--limit", type=int, help=f"bytes to judge against instead of {LIMIT_MACRO}")
    group.add_argument("--limit-at-worst", action="store_true", help="judge against the worst case itself (must fail)")
    args = parser.parse_args(argv)
    sources = Sources()
    self_checks(sources)
    rows = []
    worst = walk(sources, ROOT_EMITTER, rows=rows)
    check_pinned_keys(sources, rows)
    # The verdict itself, at the boundary: a buffer of exactly the worst case
    # has no room for the NUL and must fail; one byte more must pass.
    if fits(worst, worst) or not fits(worst, worst + 1):
        raise AssertionError(f"self-check: the verdict is off by one at {worst} B")
    if args.limit_at_worst:
        limit, source = worst, "the worst case"
    elif args.limit is not None:
        limit, source = args.limit, "--limit"
    else:
        limit, source = limit_bytes(sources), LIMIT_MACRO
    for callee, bytes_ in rows:
        print(f"  {callee:<40} {bytes_:>6}")
    print(f"status body worst case {worst} B + NUL against {source} {limit} B: headroom {limit - worst - 1} B")
    if not fits(worst, limit):
        print(
            f"FAIL: GET /api/status can need {worst + 1} B (body + NUL) but the buffer is {limit} B; "
            "a full render answers 500 and counts status_render_failures",
            file=sys.stderr,
        )
        return 1
    print("ok: status-body-bound")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except BoundError as error:
        print(f"FAIL: status-body-bound extractor: {error}", file=sys.stderr)
        sys.exit(1)
