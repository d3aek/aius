#!/usr/bin/env python3
"""Cross-check every host fake's constants against the real framework headers.

WHY THIS EXISTS. A fake is only useful if it lies about *timing and side effects*
and tells the truth about *shape and values*. A fake with a wrong constant does
not fail -- it agrees with itself. The suite goes green, the target does something
else, and the divergence is invisible until hardware disagrees with a passing test.

That is not hypothetical here. Rebuilding `test/network-transport/fake/fnet.h`
against FNET's own headers found 19 wrong constants, including `AF_INET` holding
the real `AF_INET6`'s value and `SD_WRITE` holding `SD_READ`'s. This guard was
written after the same audit reached the sibling fakes and found:

  - all three `FILE_*` modes off by one in BOTH SD fakes, so their `FILE_READ`
    was the real `FILE_WRITE`;
  - `SRC_SRSR_WARM_BOOT`, a name that does not exist in `imxrt.h`, defined at the
    value `imxrt.h` gives `SRC_SRSR_WDOG3_RST_B`.

None of it was caught by the suites, because the tests assert against the same
macros the fakes define -- both sides move together. Only an *external* reference
can catch this class, which is what this file is.

Scope is deliberately CONSTANTS, not signatures. Signature drift already fails at
compile time when the adapter is built against the real headers for the target,
and `test/network-transport/test_vendor_source.py` compares the fake's `fnet_*`
declarations to FNET's own. A wrong constant value fails nowhere else.

THREE KINDS OF CHECK, because a constant can be wrong in three ways:

  - CHECKS -- a named macro must exist upstream and hold the same value.
  - ENUM_PAIRINGS -- *every* enum member a fake declares must exist upstream and
    hold the same value, with no list for anyone to forget to extend. This is
    what a hand-maintained list cannot do: the FNET option enum is entirely
    positional, so deleting one member silently renumbers every later one, and
    `SO_SNDNUM` was the member the send-capacity model depends on.
  - INVENTION_PREFIXES -- a macro matching a framework's naming family must
    exist upstream at all.

SELF_CHECKS then mutates the fake in memory and requires each kind to report the
mutation. A guard that cannot see the defect it was written for is worse than no
guard, because it reads as evidence.

Exit 0 all agree, 2 on a mismatch, 3 when the framework package is absent
(skip, so the native suite stays runnable off a toolchain host), 4 usage error.
"""

import re
import sys
from pathlib import Path

FW = Path(__file__).resolve().parents[2]
PKG = Path.home() / ".platformio/packages/framework-arduinoteensy"

# Reverse direction: any macro in a fake matching one of these prefixes MUST exist
# upstream. The forward check below only compares names someone remembered to list,
# so it catches an invented constant only by luck -- and `SRC_SRSR_WARM_BOOT` was
# exactly that: a plausible name at a real bit position, which the forward check
# noticed only because the name it displaced happened to be listed. This finds the
# invention directly.
INVENTION_PREFIXES = [
    ("test/boot-target/fake/Arduino.h", "cores/teensy4/imxrt.h",
     ("SRC_SRSR_", "WDOG_WCR_", "CCM_CCGR")),
    ("test/sd-target/fake/SD.h", "cores/teensy4/FS.h", ("FILE_",)),
    ("test/runtime-target/fake/SD.h", "cores/teensy4/FS.h", ("FILE_",)),
]

# (fake path, real header, [constant names]) -- the forward check: every listed
# constant must exist upstream and hold the same value. Inventions are caught by
# INVENTION_PREFIXES above, not here.
CHECKS = [
    (
        "test/boot-target/fake/Arduino.h",
        "cores/teensy4/imxrt.h",
        [
            "SRC_SRSR_IPP_RESET_B",
            "SRC_SRSR_LOCKUP_SYSRESETREQ",
            "SRC_SRSR_CSU_RESET_B",
            "SRC_SRSR_IPP_USER_RESET_B",
            "SRC_SRSR_WDOG_RST_B",
            "SRC_SRSR_JTAG_RST_B",
            "SRC_SRSR_JTAG_SW_RST",
            "SRC_SRSR_WDOG3_RST_B",
            "SRC_SRSR_TEMPSENSE_RST_B",
            "WDOG_WCR_WDE",
            "WDOG_WCR_WDT",
            "WDOG_WCR_SRS",
            "WDOG_WCR_SRE",
        ],
    ),
    (
        # IRQ_PIT decides whether the fake observes the poll timer's demotion on
        # the interrupt the hardware actually shares between all four PIT
        # channels. A wrong number here would record the write at an index
        # nothing reads, and the audio-priority assertion would pass against a
        # firmware that had demoted the wrong interrupt.
        "test/network-transport/fake/Arduino.h",
        "cores/teensy4/imxrt.h",
        ["IRQ_PIT"],
    ),
    (
        # fake/fnet.h's address macros. Everything else it declares is an enum
        # member, covered by ENUM_PAIRINGS. AF_INET is the sharpest row in this
        # file: the fake this replaced held 2, which is the real AF_INET6, and
        # nothing failed -- every caller round-tripped through the same wrong
        # macro, so the fake and the suite agreed with each other about a number
        # FNET had never heard of.
        "test/network-transport/fake/fnet.h",
        "libraries/FNET/src/stack/fnet_socket.h",
        ["AF_UNSPEC", "AF_INET", "AF_INET6", "INADDR_ANY", "INADDR_BROADCAST"],
    ),
    (
        # The configuration macros resolve per macro, not per header, so each is
        # pinned against the header that actually decides it. fnet_config.h
        # includes fnet_user_config.h first and every stack default is behind an
        # #ifndef, so a user override wins and a macro the user file leaves
        # commented falls through to the stack default.
        #
        # FNET_CFG_SOCKET_MAX has no override, so the stack default is effective.
        # The earlier fake said 8, which is the vendor's MAX_SOCK_NUM -- a
        # different limit that EthernetClass::setSocketNum() clamps against this
        # one, so the two are not interchangeable. FNET_CFG_IP6 is likewise not
        # overridden (fnet_user_config.h:333 leaves it commented), and it decides
        # sizeof(struct fnet_sockaddr) -- the threshold
        # _fnet_socket_addr_check_len() enforces on every accept.
        "test/network-transport/fake/fnet.h",
        "libraries/FNET/src/stack/fnet_stack_config.h",
        ["FNET_CFG_SOCKET_MAX", "FNET_CFG_IP6"],
    ),
    (
        # These three the user file does override, so its value is effective and
        # the stack default is dead. FNET_CFG_TLS is deliberately absent: the
        # user file says 2 while both the target and this suite pin 0 through a
        # build flag, which is a different kind of agreement and is already
        # guarded by test_vendor_source.py's
        # test_target_and_suite_pin_the_same_fnet_cfg_tls.
        "test/network-transport/fake/fnet.h",
        "libraries/FNET/src/fnet_user_config.h",
        ["FNET_CFG_IP4", "FNET_CFG_MULTITHREADING", "FNET_CFG_TIMER_ALT"],
    ),
    (
        "test/sd-target/fake/SD.h",
        "cores/teensy4/FS.h",
        ["FILE_READ", "FILE_WRITE", "FILE_WRITE_BEGIN"],
    ),
    (
        "test/runtime-target/fake/SD.h",
        "cores/teensy4/FS.h",
        ["FILE_READ", "FILE_WRITE", "FILE_WRITE_BEGIN"],
    ),
    (
        "test/sd-target/fake/SD.h",
        "vendor/SD/src/SD.h",
        ["BUILTIN_SDCARD"],
    ),
    (
        "test/runtime-target/fake/SD.h",
        "vendor/SD/src/SD.h",
        ["BUILTIN_SDCARD"],
    ),
    (
        # The DAT3 re-mux's pin mode (Phase 4h NS3). A wrong value here would
        # leave the host proving a pull-down the board never applies.
        "test/sd-target/fake/SD.h",
        "cores/teensy4/core_pins.h",
        ["INPUT_PULLDOWN"],
    ),
]

# (fake path, real header, class, method) -- a member declaration the fake must
# spell as the vendored header does (return type, name, parameters). The
# module docstring scopes this file to constants because signature drift
# fails the target compile; these rows are the exception the plan names
# (Phase 4h N5): the adapter's route to cacheClear() is not compiled for the
# target in any host run, so nothing else would notice the fake drifting.
SIGNATURES = [
    (
        "test/sd-target/fake/SD.h",
        "vendor/SdFat/src/FatLib/FatPartition.h",
        "FatPartition",
        "cacheClear",
    ),
    (
        "test/sd-target/fake/SD.h",
        "vendor/SdFat/src/ExFatLib/ExFatPartition.h",
        "ExFatPartition",
        "cacheClear",
    ),
    (
        # The Aius vendor forwarder the adapter's CACHE_DROP calls (owner,
        # 2026-10-10; vendor/SdFat/NOTICE.md).
        "test/sd-target/fake/SD.h",
        "vendor/SdFat/src/FsLib/FsVolume.h",
        "FsVolume",
        "cacheClear",
    ),
    (
        # The Phase 4zd free-count forwarder the chunked used-size scan calls
        # (vendor/SdFat/NOTICE.md).
        "test/sd-target/fake/SD.h",
        "vendor/SdFat/src/FatLib/FatPartition.h",
        "FatPartition",
        "aiusFreeClusters",
    ),
    (
        "test/sd-target/fake/SD.h",
        "vendor/SdFat/src/FsLib/FsVolume.h",
        "FsVolume",
        "aiusFreeClusters",
    ),
]

# Kept as a switch for a future vendor bump that removes the forwarder; live
# since the owner chose the FsVolume accessor route (2026-10-10).
SIGNATURES_PARKED = False

# (fake path, real header tree, what the tree is) -- every enum member the fake
# declares is compared, with no name list at all. FNET's socket-option enum
# assigns no explicit values, so a member added, removed or reordered upstream
# renumbers everything after it; a list of remembered names cannot see that, and
# the option block is where three of the original 19 wrong constants aliased an
# IPPROTO_IP option onto a SOL_SOCKET one.
ENUM_PAIRINGS = [
    (
        "test/network-transport/fake/fnet.h",
        "libraries/FNET/src",
        "FNET 4.7.1",
    ),
]

# (fake path, text to replace, replacement, the name the guard must then name).
# Each row is a real historical defect shape rather than an invented one.
SELF_CHECKS = [
    (
        # The 2026-07 audit's own finding: AF_INET holding AF_INET6's value.
        # A macro, so this exercises CHECKS.
        "test/network-transport/fake/fnet.h",
        "#define AF_INET (1U)",
        "#define AF_INET (2U)",
        "AF_INET",
    ),
    (
        # A dropped member, which renumbers every later one. SO_SNDNUM is the
        # member that matters: socketSendAvailable() is SO_SNDBUF - SO_SNDNUM,
        # and a fake that answered the wrong option there is what concealed the
        # send path's would-block-as-fault collapse.
        "test/network-transport/fake/fnet.h",
        "    SO_TYPE,\n",
        "",
        "SO_SNDNUM",
    ),
    (
        # The code whose ABSENCE from the fake cost a live defect: with no
        # closure code in scope the only expressible "peer closed" row was
        # recv_result = 0, which is POSIX's convention and not FNET's.
        "test/network-transport/fake/fnet.h",
        "    FNET_ERR_CONNCLOSED = -26,",
        "    FNET_ERR_CONNCLOSED = -25,",
        "FNET_ERR_CONNCLOSED",
    ),
    (
        # The audit's other finding, verbatim: SD_WRITE holding SD_READ's value,
        # so a fake that ever modelled shutdown would have disabled reception
        # where the vendor asked to disable transmission.
        "test/network-transport/fake/fnet.h",
        "    SD_WRITE = 0x2U,",
        "    SD_WRITE = 0x1U,",
        "SD_WRITE",
    ),
    (
        # An invented enum member -- the SRC_SRSR_WARM_BOOT shape, one enum over.
        "test/network-transport/fake/fnet.h",
        "    FNET_ERR_OK = 0,",
        "    FNET_ERR_OK = 0,\n    FNET_ERR_DRAINED = -99,",
        "FNET_ERR_DRAINED",
    ),
    (
        # And the invention check itself, on the fake it was written for.
        "test/boot-target/fake/Arduino.h",
        "#define SRC_SRSR_WDOG3_RST_B",
        "#define SRC_SRSR_WARM_BOOT",
        "SRC_SRSR_WARM_BOOT",
    ),
    (
        # The DAT3 re-mux's INPUT_PULLDOWN at the core's INPUT_PULLUP value.
        "test/sd-target/fake/SD.h",
        "#define INPUT_PULLDOWN 3",
        "#define INPUT_PULLDOWN 2",
        "INPUT_PULLDOWN",
    ),
]

# Run only while SIGNATURES is live (see SIGNATURES_PARKED): a cacheClear that
# returns nothing, the drift a fake written from memory would carry.
SIGNATURE_SELF_CHECKS = [
    (
        "test/sd-target/fake/SD.h",
        "class FatPartition {\npublic:\n\tuint8_t* cacheClear();",
        "class FatPartition {\npublic:\n\tvoid cacheClear();",
        "FatPartition::cacheClear",
    ),
]

COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# The lookahead rejects a function-like macro AND forbids the name group from
# backtracking to a shorter prefix -- `(?!\()` alone lets `FNET_HTONS(x)` match as
# a constant named `FNET_HTON`.
DEFINE_RE = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_][A-Za-z_0-9]*)(?![A-Za-z_0-9(])(.*)$",
    re.M,
)
# An enum body contains no nested braces, so the excluding class is exact.
ENUM_BODY_RE = re.compile(r"\benum\b[^{;]*\{([^{}]*)\}", re.S)
MEMBER_RE = re.compile(r"([A-Za-z_][A-Za-z_0-9]*)(?:\s*=\s*(.+))?", re.S)
CAST_RE = re.compile(
    r"\((?:unsigned |signed )?"
    r"(?:u?int\d+_t|fnet_[a-z_0-9]+_t|char|short|int|long)\)"
)
WRAPPER_RE = re.compile(r"\b(?:UINT|INT)\d+_C\s*\(([^)]*)\)")
LITERAL_RE = re.compile(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]*\b")
NAME_RE = re.compile(r"\b[A-Za-z_][A-Za-z_0-9]*\b")
ARITHMETIC_RE = re.compile(r"[0-9xXa-fA-F()<>\s+|*&^~-]+")


class FrameworkMissing(Exception):
    """The framework package or one of its headers is not installed."""


def evaluate(expr, symbols):
    """The integer `expr` resolves to, or None when it is not arithmetic.

    Evaluates rather than string-matching: the real headers write
    `((uint32_t)(1 << 4))` where a fake writes `UINT32_C(0x00000010)`, and those
    are the same number. Comparing text would report a false mismatch on every
    single row and the guard would be discarded as noise.

    `symbols` carries the constants already resolved from the same file, so
    `SD_BOTH = (SD_READ | SD_WRITE)` is a number here rather than an unreadable
    expression. A name that is not in it makes the whole expression unresolvable
    -- answering None is the only honest result, because a partially substituted
    expression would evaluate to something.
    """
    expr = WRAPPER_RE.sub(r"\1", CAST_RE.sub("", expr))
    expr = LITERAL_RE.sub(r"\1", expr)
    unresolved = []

    def swap(match):
        name = match.group(0)
        if name in symbols:
            return str(symbols[name])
        unresolved.append(name)
        return name

    expr = NAME_RE.sub(swap, expr).strip()
    if unresolved or not expr or not ARITHMETIC_RE.fullmatch(expr):
        return None
    try:
        return int(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307 - arithmetic only
    except Exception:
        return None


def constants_of(source):
    """(every constant, the enum members among them) for one translation unit.

    Enum members are constants too, and the framework spells most of the ones a
    fake must agree with that way -- IRQ_PIT is `IRQ_PIT = 122,` inside imxrt.h's
    interrupt enum, and FNET's entire socket-option enum assigns no values at
    all. Reading only `#define` would report "the framework does not define
    this" about names it very much does.
    """
    source = COMMENT_RE.sub(" ", source)
    symbols = {}
    for name, body in DEFINE_RE.findall(source):
        value = evaluate(body, symbols)
        if value is not None:
            symbols[name] = value
    members = {}
    for body in ENUM_BODY_RE.findall(source):
        # A body carrying #if/#endif has members whose position -- and therefore
        # whose implicit value -- depends on a configuration this reader cannot
        # see. Guessing one would publish a wrong number as though it had been
        # checked, which is the failure this whole file exists to prevent.
        if re.search(r"^[ \t]*#", body, re.M):
            continue
        following = 0
        for member in body.split(","):
            text = member.strip()
            if not text:
                continue
            match = MEMBER_RE.fullmatch(text)
            if match is None:
                break
            value = (
                following
                if match.group(2) is None
                else evaluate(match.group(2), symbols)
            )
            if value is None:
                # Every later member of this enum is positioned relative to one
                # we could not read, so stop rather than publish a guess.
                break
            symbols[match.group(1)] = value
            members[match.group(1)] = value
            following = value + 1
    return symbols, members


def read_fake(relative):
    return (FW / relative).read_text()


def read_real(relative):
    # vendor/ paths are the linked copy under firmware/ (lib_ignore drops the
    # framework's own SD and SdFat); everything else is the framework package.
    path = (FW if relative.startswith("vendor/") else PKG) / relative
    if not path.is_file():
        raise FrameworkMissing(f"SKIP: {relative} absent")
    # FNET ships at least one header with a stray non-UTF-8 byte.
    return path.read_bytes().decode("utf-8", errors="replace")


def real_enum_members(tree_relative):
    """{name: value} for every enum member declared anywhere under a header tree."""
    root = PKG / tree_relative
    if not root.is_dir():
        raise FrameworkMissing(f"SKIP: {tree_relative} absent")
    found = {}
    for path in sorted(root.rglob("*.h")):
        if "third_party" in path.parts:
            continue
        _, members = constants_of(
            path.read_bytes().decode("utf-8", errors="replace")
        )
        for name, value in members.items():
            found.setdefault(name, value)
    return found


def named_constant_failures(fake_relative, fake_text, cache):
    """CHECKS: every listed name must exist upstream and match."""
    failures = []
    checked = 0
    fake_constants, _ = constants_of(fake_text)
    for fake_rel, real_rel, names in CHECKS:
        if fake_rel != fake_relative:
            continue
        if real_rel not in cache["constants"]:
            cache["constants"][real_rel] = constants_of(read_real(real_rel))[0]
        real_constants = cache["constants"][real_rel]
        for name in names:
            want = real_constants.get(name)
            got = fake_constants.get(name)
            if want is None:
                failures.append(
                    f"{fake_rel}: {name} does not exist in {real_rel} -- "
                    f"the fake declares a constant the framework does not, so "
                    f"nothing defines what it means on hardware"
                )
            elif got is None:
                failures.append(f"{fake_rel}: {name} missing or unparsable")
            elif got != want:
                failures.append(
                    f"{fake_rel}: {name} is {got:#x}, {real_rel} says {want:#x}"
                )
            else:
                checked += 1
    return failures, checked


def enum_member_failures(fake_relative, fake_text, cache):
    """ENUM_PAIRINGS: every enum member the fake declares, with no name list."""
    failures = []
    checked = 0
    _, members = constants_of(fake_text)
    for fake_rel, tree_rel, description in ENUM_PAIRINGS:
        if fake_rel != fake_relative:
            continue
        if tree_rel not in cache["enums"]:
            cache["enums"][tree_rel] = real_enum_members(tree_rel)
        real = cache["enums"][tree_rel]
        for name, value in sorted(members.items()):
            if name not in real:
                failures.append(
                    f"{fake_rel}: {name} = {value} is not an enum member "
                    f"anywhere in {description} -- the fake invented it, so its "
                    f"value is whatever the caller needed rather than what the "
                    f"dependency assigns"
                )
            elif real[name] != value:
                failures.append(
                    f"{fake_rel}: {name} is {value}, {description} says "
                    f"{real[name]}"
                )
            else:
                checked += 1
    return failures, checked


def invention_failures(fake_relative, fake_text, cache):
    """INVENTION_PREFIXES: a macro in a framework's naming family must exist."""
    failures = []
    checked = 0
    for fake_rel, real_rel, prefixes in INVENTION_PREFIXES:
        if fake_rel != fake_relative:
            continue
        if real_rel not in cache["sources"]:
            cache["sources"][real_rel] = read_real(real_rel)
        real_src = cache["sources"][real_rel]
        for match in re.finditer(r"^\s*#define\s+([A-Z0-9_]+)", fake_text, re.M):
            name = match.group(1)
            if not name.startswith(prefixes):
                continue
            if not re.search(
                r"^\s*#define\s+" + re.escape(name) + r"\b", real_src, re.M
            ):
                failures.append(
                    f"{fake_rel}: {name} is not defined anywhere in {real_rel} "
                    f"-- the fake invented it, so its value means whatever the "
                    f"framework actually assigns that bit, which is something "
                    f"else"
                )
            else:
                checked += 1
    return failures, checked


def member_declaration(source, class_name, method):
    """`return-type method(params)`, whitespace-normalised, or None.

    The first declaration of `method` after `class <class_name>`'s opening
    brace. Spacing around `*` and `&` is normalised so `uint8_t* f()` and
    `uint8_t *f()` compare equal; anything else that differs is drift.
    """
    clean = COMMENT_RE.sub(" ", source)
    opening = re.search(rf"\bclass\s+{re.escape(class_name)}\b[^;{{]*\{{", clean)
    if opening is None:
        return None
    match = re.search(
        rf"^[ \t]*([^\n;{{}}()]*?)\b{re.escape(method)}\s*\(([^)]*)\)",
        clean[opening.end():],
        re.M,
    )
    if match is None:
        return None
    text = f"{match.group(1)} {method}({match.group(2)})"
    text = re.sub(r"\s*([*&])\s*", r"\1 ", text)
    return " ".join(text.split())


def signature_failures(fake_relative, fake_text, cache, parked=None):
    """SIGNATURES: each listed member declared as the vendored header does."""
    if SIGNATURES_PARKED if parked is None else parked:
        return [], 0
    failures = []
    checked = 0
    for fake_rel, real_rel, class_name, method in SIGNATURES:
        if fake_rel != fake_relative:
            continue
        if real_rel not in cache["sources"]:
            cache["sources"][real_rel] = read_real(real_rel)
        want = member_declaration(cache["sources"][real_rel], class_name, method)
        got = member_declaration(fake_text, class_name, method)
        if want is None:
            failures.append(f"{real_rel}: no {class_name}::{method} to compare")
        elif got != want:
            failures.append(
                f"{fake_rel}: {class_name}::{method} is {got!r}, "
                f"{real_rel} says {want!r}"
            )
        else:
            checked += 1
    return failures, checked


def failures_for(fake_relative, fake_text, cache):
    """Every disagreement between one fake and the framework it stands for."""
    failures = []
    checked = 0
    for check in (named_constant_failures, enum_member_failures,
                  invention_failures, signature_failures):
        found, count = check(fake_relative, fake_text, cache)
        failures.extend(found)
        checked += count
    return failures, checked


def new_cache():
    """Parsed framework headers, kept across fakes and across the self-check."""
    return {"constants": {}, "enums": {}, "sources": {}}


def fake_paths():
    """Every fake any table names, in table order, without duplicates."""
    ordered = []
    for fake_rel, *_ in [*CHECKS, *ENUM_PAIRINGS, *INVENTION_PREFIXES]:
        if fake_rel not in ordered:
            ordered.append(fake_rel)
    return ordered


def self_check(cache):
    """Mutate each fake in memory and require the guard to name the mutation.

    Without this the suite's own PASS is unfalsifiable: a resolver that quietly
    answered None for every constant would report the same green line. Each row
    is a defect shape this repository actually shipped.
    """
    failures = []
    rows = SELF_CHECKS + ([] if SIGNATURES_PARKED else SIGNATURE_SELF_CHECKS)
    for fake_rel, original, replacement, expected in rows:
        text = read_fake(fake_rel)
        if original not in text:
            failures.append(
                f"{fake_rel}: the self-check has no text to mutate: {original!r}"
            )
            continue
        mutated = text.replace(original, replacement, 1)
        found, _ = failures_for(fake_rel, mutated, cache)
        if not any(f"{expected}" in entry for entry in found):
            failures.append(
                f"{fake_rel}: mutating {expected} was accepted -- the guard "
                f"cannot see the defect it exists for. Reported: {found}"
            )
    return failures


def main() -> int:
    if len(sys.argv) != 1:
        print(__doc__, file=sys.stderr)
        return 4
    if not PKG.is_dir():
        print(f"SKIP: framework-arduinoteensy not present at {PKG}")
        return 3

    cache = new_cache()
    failures = []
    checked = 0
    try:
        for fake_rel in fake_paths():
            found, count = failures_for(fake_rel, read_fake(fake_rel), cache)
            failures.extend(found)
            checked += count
        failures.extend(self_check(cache))
    except FrameworkMissing as missing:
        print(missing)
        return 3

    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        print(f"RESULT: {len(failures)} constant(s) disagree with the framework headers")
        return 2
    print(
        f"fake fidelity: PASS ({checked} constants across "
        f"{len(CHECKS) + len(ENUM_PAIRINGS)} pairings, "
        f"{len(SELF_CHECKS)} synthetic regressions caught)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
