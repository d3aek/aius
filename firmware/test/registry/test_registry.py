#!/usr/bin/env python3
"""Registry guard: the suite registry and the filesystem must agree.

`scripts/test.sh` is a hand-maintained registry. A directory under `test/` only
ever runs if somebody remembered to add an arm to its `case`; a 2026-07-25 audit
found `test/auth/test_auth.c` and `test/config/test_config.c` were live,
committed, compiling code that had never once been executed, and the firmware
README/header catalogues had drifted far enough to disagree about how many
suites even exist. "The suites are green" is only a checkable statement if
nothing can be silently absent from the run, so this suite fails whenever the
registry and the tree disagree.

Five checks, all failures collected and reported in one run:

  1. suite-dir-has-arm     every directory under `test/` (minus NON_SUITE_DIRS)
                           is referenced by at least one arm.
  2. arm-paths-exist       every arm names at least one path under `test/`, and
                           every `test/` path an arm names exists on disk.
  3. entry-point-runs      every test entry point (`test_*` / `fuzz_*` source)
                           under a suite directory is named by `scripts/test.sh`.
                           This is the file-level orphan class -- a registered
                           directory whose arm forgot one of its sources, which
                           is exactly how `test/config/test_config.c` hid.
  4. header-catalogue      the `# Suites:` list in the `scripts/test.sh` header
                           is exactly the set of arms.
  5. readme-catalogue      the suites table in `README.md` names exactly the set
                           of arms.

The registry is PARSED, never duplicated: the arm names come out of
`scripts/test.sh` itself. A guard carrying its own copy of the suite list would
just be a fourth list to drift.

Every check is exercised against synthetic input on every run (see
`run_self_test`), so a guard that has silently stopped discriminating fails
before it can report a false green.

Usage:

    test/registry/test_registry.py
    test/registry/test_registry.py --firmware-root <dir>

`--firmware-root` points the guard at a mirrored firmware tree instead of its
own; it exists so the guard can be negative-controlled against a tree with a
deliberately planted orphan without disturbing the real one.

Exit codes: 0 agreement; 1 drift (the report names every disagreement and the
line to add). `scripts/test.sh` reserves 2 and 3 for its own use.
"""

import argparse
import re
import sys
from pathlib import Path

# Directories under `test/` that are deliberately not suites. Explicit, not a
# heuristic: a pattern match ("anything without a test_*.c", "anything named
# like data") would silently excuse a real suite that happens to fit it, which
# is the failure this whole file exists to prevent. Adding an entry here is a
# reviewable claim that the directory has nothing to run.
#
#   fixtures/  Shared binary test data (LFS), read by suites that live
#              elsewhere -- `test/fixtures/status/` holds the recorded status
#              frames that the `net-runner` and `recovery-runner` suites decode.
#              It has no runner of its own and nothing in it is compiled.
#   evidence/  Runtime OUTPUT, not input: the run logs the live recovery smoke
#              (`tools/net/aius_recovery_smoke.py`) writes per invocation, into
#              `evidence/network-hardening/`. Gitignored, so on a clean checkout
#              the directory does not exist at all; it appears only once someone
#              has run the smoke against a board. Nothing reads it back and
#              nothing in it is compiled, so there is nothing here to register.
NON_SUITE_DIRS = frozenset({"fixtures", "evidence"})

# A test entry point is a file a suite arm is expected to name directly. Suite
# support files (fakes, harness translation units, generators, vectors) are not
# entry points and are reached through the entry point that uses them.
ENTRY_POINT_RE = re.compile(r"^(?:test|fuzz)_.+\.(?:c|cpp|py)$")

CASE_OPEN = 'case "$suite" in'
ARM_RE = re.compile(r"^([a-z0-9][a-z0-9-]*)\)\s*$")
FW_ROOT_TEST_RE = re.compile(r"\$fw_root/(test/[A-Za-z0-9._/-]+)")
# Repo-root suites: the SD-card web UI vitest suites live at <repo>/test/, one
# level above the firmware tree, and their arm names them through $aius_root.
# They are catalogued and existence-checked like any other arm; they are not
# firmware test/ directories, so the directory-orphan and entry-point checks
# (which walk firmware/test/) do not see them.
AIUS_ROOT_TEST_RE = re.compile(r"\$aius_root/(test/[A-Za-z0-9._/-]+)")
HEADER_OPEN = "# Suites:"
README_TABLE_HEADER = "| Suite | Covers |"
BACKTICKED_RE = re.compile(r"`([^`]+)`")


# --------------------------------------------------------------------------
# Parsing. Each parser takes text and returns plain data, so the self-test can
# drive every check from synthetic registries with no filesystem involved.
# --------------------------------------------------------------------------

def parse_arms(test_sh_text):
    """Arm name -> arm body, in registry order, for the `case "$suite" in` block.

    The `*)` unknown-suite fallback is not a suite and is skipped. The argument
    parser's own `case "$1" in` is skipped too: only the block opened by
    CASE_OPEN is read, and it ends at the first unindented `esac`.
    """
    lines = test_sh_text.splitlines()
    try:
        start = next(i for i, line in enumerate(lines) if line.strip() == CASE_OPEN)
    except StopIteration:
        raise AssertionError(
            f"scripts/test.sh has no `{CASE_OPEN}` block -- the registry guard "
            "cannot find the suite registry to parse. If the dispatch shape "
            "changed, update CASE_OPEN in test/registry/test_registry.py."
        )
    arms = {}
    current = None
    for line in lines[start + 1:]:
        if line == "esac":
            break
        match = ARM_RE.match(line)
        if match:
            current = match.group(1)
            arms[current] = []
            continue
        if line.startswith("*)"):
            current = None
            continue
        if current is not None:
            arms[current].append(line)
    return {name: "\n".join(body) for name, body in arms.items()}


def parse_header_suites(test_sh_text):
    """The `# Suites: ...` catalogue in the script header, as a list of names.

    The list wraps over continuation comment lines and terminates on the line
    ending in a full stop.
    """
    lines = test_sh_text.splitlines()
    try:
        start = next(i for i, line in enumerate(lines) if line.startswith(HEADER_OPEN))
    except StopIteration:
        raise AssertionError(
            f"scripts/test.sh header has no `{HEADER_OPEN}` line -- the "
            "human-facing catalogue is missing entirely. Restore it as a "
            "comma-separated list terminated by a full stop."
        )
    collected = lines[start][len(HEADER_OPEN):].strip()
    for line in lines[start + 1:]:
        if collected.endswith("."):
            break
        if not line.startswith("#"):
            break
        collected += " " + line[1:].strip()
    collected = collected.rstrip(".")
    return [name.strip() for name in collected.split(",") if name.strip()]


def parse_readme_suites(readme_text):
    """Suite names from the first column of the README suites table."""
    lines = readme_text.splitlines()
    try:
        start = next(
            i for i, line in enumerate(lines) if line.strip() == README_TABLE_HEADER
        )
    except StopIteration:
        raise AssertionError(
            f"README.md has no `{README_TABLE_HEADER}` table -- the suites "
            "catalogue is missing entirely. Restore it under "
            "`## Running host tests`."
        )
    names = []
    for line in lines[start + 2:]:
        if not line.startswith("|"):
            break
        cell = line.split("|")[1]
        names.extend(BACKTICKED_RE.findall(cell))
    return names


def referenced_test_paths(arm_body):
    """`test/...` paths an arm body names through `$fw_root`."""
    return sorted(set(FW_ROOT_TEST_RE.findall(arm_body)))


def referenced_repo_paths(arm_body):
    """`test/...` paths an arm body names through `$aius_root`, returned
    firmware-root-relative (`../test/...`) so one `exists` predicate serves
    both roots."""
    return sorted({"../" + path for path in AIUS_ROOT_TEST_RE.findall(arm_body)})


# --------------------------------------------------------------------------
# Checks. Every check returns a list of failure strings; each failure names the
# suite, the list it is missing from, and the line to add.
# --------------------------------------------------------------------------

def suite_dirs_of(arms):
    """Directory under `test/` -> the arms that reference it.

    Suite name and directory name are deliberately not assumed equal: one
    directory legitimately backs several arms (`test/dts/` backs `dts`,
    `dts-arena` and `dts-fuzz`) and one arm's name legitimately differs from its
    directory (`codec-api-contract` lives in `test/codec-api/`).
    """
    owners = {}
    for name, body in arms.items():
        for path in referenced_test_paths(body):
            parts = path.split("/")
            if len(parts) < 2:
                continue
            owners.setdefault(parts[1], []).append(name)
    return owners


def check_suite_dir_has_arm(arms, suite_dirs):
    owners = suite_dirs_of(arms)
    failures = []
    for directory in sorted(suite_dirs):
        if directory in NON_SUITE_DIRS or directory in owners:
            continue
        failures.append(
            f"test/{directory}/ has no arm in scripts/test.sh -- it has never run.\n"
            f"      Register it, or declare it a non-suite:\n"
            f"        1. add an arm to the `case \"$suite\" in` block in "
            f"scripts/test.sh:\n"
            f"             {directory})\n"
            f"             \tpython3 \"$fw_root/test/{directory}/test_{directory.replace('-', '_')}.py\"\n"
            f"             \t;;\n"
            f"        2. add `{directory}` to the `# Suites:` list in the "
            f"scripts/test.sh header\n"
            f"        3. add a row to the suites table in README.md:\n"
            f"             | `{directory}` | <what it covers>. |\n"
            f"        -- or, if test/{directory}/ is shared data rather than a "
            f"suite, add it to\n"
            f"           NON_SUITE_DIRS in test/registry/test_registry.py with "
            f"its justification."
        )
    return failures


def check_arm_paths_exist(arms, exists):
    failures = []
    for name in sorted(arms):
        paths = referenced_test_paths(arms[name]) + referenced_repo_paths(arms[name])
        if not paths:
            failures.append(
                f"arm `{name}` in scripts/test.sh names no path under test/ -- "
                f"there is no suite\n"
                f"      directory behind it. Point it at its sources "
                f"(\"$fw_root/test/<dir>/...\", or \"$aius_root/test/<dir>\" for a\n"
                f"      repo-root suite) or delete the arm, its `# Suites:` header "
                f"entry and its\n"
                f"      README.md table row."
            )
            continue
        for path in paths:
            if exists(path):
                continue
            failures.append(
                f"arm `{name}` in scripts/test.sh names {path}, which does not "
                f"exist.\n"
                f"      It would fail confusingly at run time. Restore the file "
                f"or correct the path\n"
                f"      in the `{name})` arm."
            )
    return failures


def check_entry_point_runs(entry_points, test_sh_text):
    failures = []
    for path in sorted(entry_points):
        if f'$fw_root/{path}' in test_sh_text:
            continue
        directory = path.split("/")[1]
        failures.append(
            f"{path} is a test entry point that scripts/test.sh never runs.\n"
            f"      Its directory is registered but this file is not, so it has "
            f"never executed.\n"
            f"      Add it to the `{directory})` arm (or whichever arm owns it), "
            f"e.g.:\n"
            f"        \"$fw_root/{path}\"\n"
            f"      If it is a support file rather than an entry point, rename "
            f"it so it does not\n"
            f"      match {ENTRY_POINT_RE.pattern}."
        )
    return failures


def _catalogue_failures(arms, listed, list_label, add_hint, drop_hint):
    failures = []
    arm_names = set(arms)
    listed_names = set(listed)
    for name in sorted(arm_names - listed_names):
        failures.append(
            f"suite `{name}` has an arm in scripts/test.sh but is missing from "
            f"{list_label}.\n"
            f"      {add_hint(name)}"
        )
    for name in sorted(listed_names - arm_names):
        failures.append(
            f"{list_label} names `{name}`, which has no arm in scripts/test.sh "
            f"and cannot run.\n"
            f"      {drop_hint(name)}"
        )
    duplicates = sorted({name for name in listed if listed.count(name) > 1})
    for name in duplicates:
        failures.append(
            f"{list_label} names `{name}` more than once. Remove the duplicate."
        )
    return failures


def check_header_catalogue(arms, header_suites):
    return _catalogue_failures(
        arms,
        header_suites,
        "the `# Suites:` list in the scripts/test.sh header",
        lambda name: f"Add `{name}` to that comma-separated list.",
        lambda name: (
            f"Remove `{name}` from that list, or add the missing `{name})` arm."
        ),
    )


def check_readme_catalogue(arms, readme_suites):
    return _catalogue_failures(
        arms,
        readme_suites,
        "the suites table in README.md",
        lambda name: f"Add a row: | `{name}` | <what it covers>. |",
        lambda name: (
            f"Remove its row, or add the missing `{name})` arm to scripts/test.sh."
        ),
    )


CHECKS = (
    ("suite-dir-has-arm", lambda r: check_suite_dir_has_arm(r.arms, r.suite_dirs)),
    ("arm-paths-exist", lambda r: check_arm_paths_exist(r.arms, r.exists)),
    ("entry-point-runs", lambda r: check_entry_point_runs(r.entry_points, r.test_sh)),
    ("header-catalogue", lambda r: check_header_catalogue(r.arms, r.header_suites)),
    ("readme-catalogue", lambda r: check_readme_catalogue(r.arms, r.readme_suites)),
)


class Registry:
    """Everything the checks read, so they never touch the filesystem directly."""

    def __init__(self, test_sh, readme, suite_dirs, entry_points, exists):
        self.test_sh = test_sh
        self.arms = parse_arms(test_sh)
        self.header_suites = parse_header_suites(test_sh)
        self.readme_suites = parse_readme_suites(readme)
        self.suite_dirs = suite_dirs
        self.entry_points = entry_points
        self.exists = exists


def registry_from_tree(firmware_root):
    test_root = firmware_root / "test"
    test_sh = (firmware_root / "scripts" / "test.sh").read_text()
    readme = (firmware_root / "README.md").read_text()
    suite_dirs = sorted(
        child.name for child in test_root.iterdir() if child.is_dir()
    )
    entry_points = []
    for directory in suite_dirs:
        if directory in NON_SUITE_DIRS:
            continue
        for path in sorted((test_root / directory).rglob("*")):
            if path.is_file() and ENTRY_POINT_RE.match(path.name):
                entry_points.append(str(path.relative_to(firmware_root)))
    return Registry(
        test_sh=test_sh,
        readme=readme,
        suite_dirs=suite_dirs,
        entry_points=entry_points,
        exists=lambda path: (firmware_root / path).exists(),
    )


def run_checks(registry):
    failures = []
    for name, check in CHECKS:
        for failure in check(registry):
            failures.append(f"  - [{name}] {failure}")
    return failures


# --------------------------------------------------------------------------
# Self-test: each check must fail on a registry mutated to break exactly it.
# A guard that has stopped discriminating is worse than no guard, because it
# reports green.
# --------------------------------------------------------------------------

GOOD_TEST_SH = """#!/usr/bin/env bash
# Fake runner.
#
# Suites: alpha, beta,
#         gamma.
#
set -euo pipefail
while [ $# -gt 0 ]; do
\tcase "$1" in
\t--suite)
\t\tsuite="$2"
\t\t;;
\tesac
done
case "$suite" in
alpha)
\tpython3 "$fw_root/test/alpha/test_alpha.py"
\t;;
beta)
\t"$cc" "$fw_root/test/beta/test_beta.c" -o "$out"
\t;;
gamma)
\tpython3 "$fw_root/test/beta/test_gamma.py"
\t;;
*)
\techo "unknown suite: $suite" >&2
\texit 2
\t;;
esac
"""

GOOD_README = """# Fake

| Suite | Covers |
| --- | --- |
| `alpha` | The alpha thing. |
| `beta`, `gamma` | The beta things. |

Trailing prose.
"""

GOOD_DIRS = ["alpha", "beta", "fixtures"]
GOOD_ENTRY_POINTS = [
    "test/alpha/test_alpha.py",
    "test/beta/test_beta.c",
    "test/beta/test_gamma.py",
]
GOOD_PATHS = frozenset(GOOD_ENTRY_POINTS)


def _registry(test_sh=None, readme=None, suite_dirs=None, entry_points=None,
              paths=None):
    known = GOOD_PATHS if paths is None else frozenset(paths)
    return Registry(
        test_sh=GOOD_TEST_SH if test_sh is None else test_sh,
        readme=GOOD_README if readme is None else readme,
        suite_dirs=list(GOOD_DIRS if suite_dirs is None else suite_dirs),
        entry_points=list(
            GOOD_ENTRY_POINTS if entry_points is None else entry_points
        ),
        exists=lambda path: path in known,
    )


def _expect_green(registry, label):
    failures = run_checks(registry)
    if failures:
        raise AssertionError(
            f"registry self-test {label}: expected no failures, got:\n"
            + "\n".join(failures)
        )


def _expect_fail(registry, label, needle):
    failures = run_checks(registry)
    if not failures:
        raise AssertionError(
            f"registry self-test {label}: the check did not fire -- "
            "the guard is not discriminating."
        )
    if not any(needle in failure for failure in failures):
        raise AssertionError(
            f"registry self-test {label}: fired, but no failure mentioned "
            f"{needle!r}:\n" + "\n".join(failures)
        )


def run_self_test():
    # The reference registry agrees with itself, including the two shapes real
    # arms use: one directory behind several arms, and a non-suite directory.
    _expect_green(_registry(), "baseline")

    # 1. A suite directory nobody registered -- the orphan class that motivated
    #    this guard.
    _expect_fail(
        _registry(suite_dirs=GOOD_DIRS + ["delta"]),
        "suite-dir-has-arm",
        "test/delta/ has no arm",
    )

    # 1b. ...but a directory declared non-suite is excused.
    _expect_green(
        _registry(suite_dirs=GOOD_DIRS + ["fixtures"]), "non-suite-allowlist",
    )

    # 2. An arm naming a directory that does not exist.
    _expect_fail(
        _registry(
            test_sh=GOOD_TEST_SH.replace(
                'test/alpha/test_alpha.py', 'test/absent/test_absent.py'
            ),
            entry_points=[p for p in GOOD_ENTRY_POINTS if "alpha" not in p],
            suite_dirs=["beta", "fixtures"],
        ),
        "arm-paths-exist/missing",
        "does not exist",
    )

    # 2b. An arm naming no test/ path at all.
    _expect_fail(
        _registry(
            test_sh=GOOD_TEST_SH.replace(
                'python3 "$fw_root/test/alpha/test_alpha.py"', 'true'
            ),
            entry_points=[p for p in GOOD_ENTRY_POINTS if "alpha" not in p],
            suite_dirs=["beta", "fixtures"],
        ),
        "arm-paths-exist/empty",
        "names no path under test/",
    )

    # 2c. A repo-root suite (named through $aius_root) is a valid path, checked
    #     one level above the firmware tree; an absent one still fails.
    repo_root_sh = GOOD_TEST_SH.replace(
        'python3 "$fw_root/test/beta/test_gamma.py"',
        'pnpm exec vitest run --root "$aius_root" "$aius_root/test/gamma"',
    )
    repo_root_entry_points = [p for p in GOOD_ENTRY_POINTS if "gamma" not in p]
    _expect_green(
        _registry(
            test_sh=repo_root_sh,
            entry_points=repo_root_entry_points,
            paths=GOOD_PATHS | {"../test/gamma"},
        ),
        "arm-paths-exist/repo-root",
    )
    _expect_fail(
        _registry(test_sh=repo_root_sh, entry_points=repo_root_entry_points),
        "arm-paths-exist/repo-root-missing",
        "../test/gamma, which does not exist",
    )

    # 3. A registered directory whose arm forgot one of its sources -- the
    #    file-level orphan `test/config/test_config.c` was.
    _expect_fail(
        _registry(
            entry_points=GOOD_ENTRY_POINTS + ["test/beta/test_beta_extra.c"],
        ),
        "entry-point-runs",
        "test/beta/test_beta_extra.c is a test entry point",
    )

    # 4. Header catalogue drift, both directions.
    _expect_fail(
        _registry(test_sh=GOOD_TEST_SH.replace("# Suites: alpha, beta,", "# Suites: beta,")),
        "header-catalogue/missing",
        "suite `alpha` has an arm",
    )
    _expect_fail(
        _registry(
            test_sh=GOOD_TEST_SH.replace("#         gamma.", "#         gamma, ghost.")
        ),
        "header-catalogue/ghost",
        "names `ghost`",
    )

    # 5. README catalogue drift, both directions.
    _expect_fail(
        _registry(readme=GOOD_README.replace("| `alpha` | The alpha thing. |\n", "")),
        "readme-catalogue/missing",
        "suite `alpha` has an arm",
    )
    _expect_fail(
        _registry(readme=GOOD_README.replace("| `beta`, `gamma` |", "| `beta`, `gamma`, `ghost` |")),
        "readme-catalogue/ghost",
        "names `ghost`",
    )

    # The parsers themselves: a wrapped header list and a multi-name README row
    # must round-trip, or every catalogue check silently compares empty sets.
    header = parse_header_suites(GOOD_TEST_SH)
    if header != ["alpha", "beta", "gamma"]:
        raise AssertionError(f"registry self-test header parse: got {header}")
    readme = parse_readme_suites(GOOD_README)
    if readme != ["alpha", "beta", "gamma"]:
        raise AssertionError(f"registry self-test readme parse: got {readme}")
    arms = list(parse_arms(GOOD_TEST_SH))
    if arms != ["alpha", "beta", "gamma"]:
        raise AssertionError(f"registry self-test arm parse: got {arms}")


def main():
    parser = argparse.ArgumentParser(description="Aius firmware suite-registry guard.")
    parser.add_argument(
        "--firmware-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="firmware tree to check (default: this file's own firmware root)",
    )
    args = parser.parse_args()

    run_self_test()
    print(
        f"registry self-test (all {len(CHECKS)} checks discriminate): PASS",
        flush=True,
    )

    registry = registry_from_tree(args.firmware_root.resolve())
    failures = run_checks(registry)
    if failures:
        print(
            f"registry FAILED ({len(failures)} disagreement(s) between "
            f"scripts/test.sh, README.md and test/):\n" + "\n".join(failures),
            file=sys.stderr,
        )
        return 1
    print(
        f"registry: PASS ({len(registry.arms)} arms, "
        f"{len(registry.suite_dirs) - len(NON_SUITE_DIRS)} suite directories, "
        f"{len(registry.entry_points)} entry points, header and README in agreement)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
