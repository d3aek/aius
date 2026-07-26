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
compile time when the adapter is built against the real headers for the target;
a wrong constant value fails nowhere.

Exit 0 all agree, 2 on a mismatch, 3 when the framework package is absent
(skip, so the native suite stays runnable off a toolchain host), 4 usage error.
"""

import re
import sys
from pathlib import Path

FW = Path(__file__).resolve().parents[2]
PKG = Path.home() / ".platformio/packages/framework-arduinoteensy"

# (fake path, real header, [constant names]) -- the forward check: every listed
# constant must exist upstream and hold the same value. Inventions are caught by
# INVENTION_PREFIXES above, not here.
# Reverse direction: any macro in a fake matching one of these prefixes MUST exist
# upstream. The forward check above only compares names someone remembered to list,
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
        "libraries/SD/src/SD.h",
        ["BUILTIN_SDCARD"],
    ),
    (
        "test/runtime-target/fake/SD.h",
        "libraries/SD/src/SD.h",
        ["BUILTIN_SDCARD"],
    ),
]


def value_of(source: str, name: str):
    """The integer a `#define NAME <expr>` resolves to, or None if absent.

    Evaluates the expression rather than string-matching it: the real headers
    write `((uint32_t)(1 << 4))` where a fake writes `UINT32_C(0x00000010)`, and
    those are the same number. Comparing text would report a false mismatch on
    every single row and the guard would be discarded as noise."""
    m = re.search(r"^\s*#define\s+" + re.escape(name) + r"\b(?!\()(.*)$", source, re.M)
    if not m:
        return None
    expr = m.group(1).split("//")[0].split("/*")[0].strip()
    # Strip C casts and fixed-width literal wrappers; what remains is arithmetic.
    expr = re.sub(r"\((?:unsigned\s+)?u?int\d+_t\)", "", expr)
    expr = re.sub(r"\b(?:UINT|INT)\d+_C\s*\(([^)]*)\)", r"\1", expr)
    expr = expr.replace("u", "").replace("U", "").replace("L", "")
    if not expr or not re.fullmatch(r"[0-9xXa-fA-F()<>\s+|*-]+", expr):
        return None
    try:
        return int(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307 - arithmetic only
    except Exception:
        return None


def main() -> int:
    if len(sys.argv) != 1:
        print(__doc__, file=sys.stderr)
        return 4
    if not PKG.is_dir():
        print(f"SKIP: framework-arduinoteensy not present at {PKG}")
        return 3

    failures = 0
    checked = 0
    for fake_rel, real_rel, names in CHECKS:
        fake_src = (FW / fake_rel).read_text()
        real_path = PKG / real_rel
        if not real_path.is_file():
            print(f"SKIP: {real_rel} absent")
            return 3
        real_src = real_path.read_text()
        for name in names:
            want = value_of(real_src, name)
            got = value_of(fake_src, name)
            if want is None:
                print(
                    f"FAIL: {fake_rel}: {name} does not exist in {real_rel} -- "
                    f"the fake declares a constant the framework does not, so nothing "
                    f"defines what it means on hardware"
                )
                failures += 1
                continue
            if got is None:
                print(f"FAIL: {fake_rel}: {name} missing or unparsable")
                failures += 1
                continue
            if got != want:
                print(
                    f"FAIL: {fake_rel}: {name} is {got:#x}, {real_rel} says {want:#x}"
                )
                failures += 1
                continue
            checked += 1

    for fake_rel, real_rel, prefixes in INVENTION_PREFIXES:
        fake_src = (FW / fake_rel).read_text()
        real_src = (PKG / real_rel).read_text()
        for m in re.finditer(r"^\s*#define\s+([A-Z0-9_]+)", fake_src, re.M):
            name = m.group(1)
            if not name.startswith(prefixes):
                continue
            if not re.search(r"^\s*#define\s+" + re.escape(name) + r"\b", real_src, re.M):
                print(
                    f"FAIL: {fake_rel}: {name} is not defined anywhere in {real_rel} -- "
                    f"the fake invented it, so its value means whatever the framework "
                    f"actually assigns that bit, which is something else"
                )
                failures += 1
            else:
                checked += 1

    if failures:
        print(f"RESULT: {failures} constant(s) disagree with the framework headers")
        return 2
    print(f"fake fidelity: PASS ({checked} constants across {len(CHECKS)} pairings)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
