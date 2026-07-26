#!/usr/bin/env python3
"""Cross-language guard: the SD editor's PUT body carries every key the validator
requires.

WHY THIS EXISTS. `PUT /api/dsp/config` validates the *complete* model -- a missing
key is a 400, not a defaulted field -- and `sd/www/dsp-editor.js` builds its request
body by naming each key explicitly rather than round-tripping the GET. Those two
lists are in different languages, in different directories, with no compiler
between them, so adding a key to the model silently breaks every save from the
editor until someone happens to try one.

That is not hypothetical: `matrix.lfe_crossover_hz` was added to the validator with
the LFE fold-in and the editor was not updated in the same change, so every save
400'd on a missing key and nothing failed. No test covered the editor at all.

The guard compares SETS OF KEY NAMES, deliberately -- not values, not bounds, not
order. Bounds already have a home (the validator pins them and `test_dsp_config.c`
proves them); duplicating them here would create a third copy of the numbers, which
is the same class of defect this file exists to catch.

Usage: test_editor_keys.py <firmware-root>
Exit 0 all keys agree, 2 on a mismatch, 4 on usage error.
"""

import re
import sys
from pathlib import Path

# Nested objects in the model whose keys the editor names one by one. `features` and
# `input_channels` are excluded because the editor builds both by iterating its own
# table (`for (const [key] of FEATURES)`), so a new key there cannot go missing the
# same way -- the loop picks it up or the table itself is wrong, which is visible.
OBJECTS = ("matrix", "limiter")


def validator_keys(source: str, obj: str) -> set[str]:
    """Key names the validator recognises for one object, from its aius_json_key_is
    calls. Scoped to the parsing function for that object so `limiter` and `matrix`
    do not pool their keys."""
    # Any number of qualifier words before the name -- the real signature is
    # `static FLASHMEM AiusDspConfigErrorCode parse_matrix(`, and a regex that
    # assumed exactly one qualifier is how the first draft of this guard failed.
    fn = re.search(
        r"^static\s+(?:\w+[\s*]+)+parse_" + obj + r"\s*\(.*?^}", source, re.S | re.M
    )
    if not fn:
        raise SystemExit(f"validator: no parse_{obj}() found -- has it been renamed?")
    return set(re.findall(r'aius_json_key_is\([^;]*?"([a-z0-9_]+)"\)', fn.group(0)))


def editor_keys(source: str, obj: str) -> set[str]:
    """Key names the editor puts in its request body for one object, from the
    object literal it builds in collectModel()."""
    lit = re.search(r"^\s{6}" + obj + r":\s*\{(.*?)^\s{6}\},", source, re.S | re.M)
    if not lit:
        raise SystemExit(
            f"editor: no `{obj}: {{...}}` literal found in the PUT body -- if the "
            "body is now built some other way, this guard needs rewriting, not deleting"
        )
    # Keys of the literal, skipping the values (which are numberOf(...)/field(...)
    # calls that mention the same names as string arguments).
    return set(re.findall(r"^\s+([a-z0-9_]+):", lit.group(1), re.M))


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 4
    root = Path(sys.argv[1])
    validator = (root / "lib/aius_dsp_config/src/aius_dsp_config.c").read_text()
    editor = (root / "sd/www/dsp-editor.js").read_text()

    failures = 0
    for obj in OBJECTS:
        want = validator_keys(validator, obj)
        got = editor_keys(editor, obj)
        if not want:
            print(f"FAIL: {obj}: validator declares no keys -- guard is not reading it")
            failures += 1
            continue
        missing = sorted(want - got)
        extra = sorted(got - want)
        if missing:
            print(
                f"FAIL: {obj}: the editor omits {missing}, which the PUT validator "
                f"requires -- every save from the SD editor will 400 on a missing key"
            )
            failures += 1
        if extra:
            print(
                f"FAIL: {obj}: the editor sends {extra}, which the validator does not "
                f"recognise -- an unknown key is rejected, so every save will 400"
            )
            failures += 1
        if not missing and not extra:
            print(f"  {obj:<9} {len(want)} keys agree ({', '.join(sorted(want))})")

    if failures:
        print(f"RESULT: {failures} key-set mismatch(es)")
        return 2
    print("RESULT: SD editor PUT body matches the validator's model")
    return 0


if __name__ == "__main__":
    sys.exit(main())
