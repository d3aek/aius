#!/usr/bin/env python3
"""Pad a DSP model JSON body to exactly AIUS_DSP_MODEL_MAX_JSON_BYTES.

Builds the full-size Apply body for the Phase 4f F7 device run from the
owner's current model, so the parser walks a 12,288 B body that decodes to the
model already playing (plan F10).

Fill order (R5-S3): whitespace is the parser's cheapest path per byte, so
first every number gets the longest spelling the parser accepts for the same
double, then JSON whitespace takes the remainder. Accepted spellings come
from firmware/lib/aius_json aius_json_scan_number and
firmware/lib/aius_dsp_config decode_scanned_number:
  - no leading zeros on the integer part (scan_number rejects "01");
  - fraction digits accumulate as digit * scale, so appended "0" digits add
    exactly 0.0 and the double is unchanged;
  - an exponent whose digits sum to 0 is skipped, and leading zeros in a
    nonzero exponent do not change it;
  - take_number refuses a number of AIUS_DSP_CONFIG_NUMBER_BUFFER_MAX (64)
    bytes or more, so the longest spelling is 63 bytes.
A number's longest spelling is its text with zeros appended to the fraction
(a "." added when absent), any exponent kept as written. An input that
cannot be padded to the limit (invalid JSON, non-ASCII, already over the
limit) fails with exit 1; growth stops at the limit, so number growth never
overshoots.

Usage: dsp_model_pad.py INPUT [-o OUTPUT] [--base-generation N]
Prints the split to stdout as one JSON line:
  {"total", "number_bytes", "whitespace_bytes", "other_bytes", "numbers",
   "numbers_at_max", "number_share"}
number_bytes counts the bytes of number tokens in the output; whitespace_bytes
is the padding added. With -o omitted the body goes to stdout and the split to
stderr.
Exit: 0 padded, 1 cannot reach the limit or input not a model body, 4 usage.
"""
import argparse
import json
import re
import sys

MAX_JSON_BYTES = 12288  # AIUS_DSP_MODEL_MAX_JSON_BYTES
NUMBER_TEXT_MAX = 63  # AIUS_DSP_CONFIG_NUMBER_BUFFER_MAX (64) - 1

# A JSON token outside strings: either a string (skipped) or a number.
TOKEN = re.compile(r'"(?:[^"\\]|\\.)*"|-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?')
NUMBER = re.compile(r"-?(?:0|[1-9][0-9]*)(?:\.([0-9]+))?(?:([eE][+-]?)([0-9]+))?\Z")


def longest_spelling(text, budget):
    """Longest accepted spelling of `text` within `budget` bytes (>= len(text))."""
    m = NUMBER.match(text)
    if m is None:
        raise ValueError(f"not a number: {text!r}")
    room = min(budget, NUMBER_TEXT_MAX) - len(text)
    if room <= 0:
        return text
    mantissa, _, tail = text.partition("e") if "e" in text else text.partition("E")
    sep = "e" if "e" in text else "E" if "E" in text else ""
    if m.group(1) is None:
        # "." plus at least one zero; one byte of room cannot hold both.
        if room < 2:
            return text
        mantissa += "."
        room -= 1
        zeros = room
        return mantissa + "0" * zeros + (sep + tail if sep else "")
    if not sep:
        return mantissa + "0" * room
    # Fraction present and an exponent: fill the fraction, exponent unchanged.
    return mantissa + "0" * room + sep + tail


def tokenize(body):
    """Yield (start, end, is_number) for every string and number token."""
    for m in TOKEN.finditer(body):
        yield m.start(), m.end(), m.group(0)[0] != '"'


def pad(body, base_generation=None):
    """Return (padded_text, split dict). Raises ValueError if the limit is unreachable."""
    json.loads(body)  # input must be valid JSON
    if base_generation is not None:
        if '"base_generation"' in body:
            raise ValueError("input already carries base_generation")
        brace = body.index("{")
        body = body[: brace + 1] + f'"base_generation":{int(base_generation)},' + body[brace + 1 :]
    raw = len(body.encode("utf-8"))
    if raw > MAX_JSON_BYTES:
        raise ValueError(f"input is {raw} B, over the {MAX_JSON_BYTES} B limit")
    spans = [(s, e) for s, e, is_num in tokenize(body) if is_num]
    spare = MAX_JSON_BYTES - raw
    out, last, number_bytes, at_max = [], 0, 0, 0
    for s, e in spans:
        text = body[s:e]
        grown = longest_spelling(text, len(text) + spare)
        spare -= len(grown) - len(text)
        at_max += grown == longest_spelling(text, NUMBER_TEXT_MAX)
        out.append(body[last:s])
        out.append(grown)
        number_bytes += len(grown)
        last = e
    out.append(body[last:])
    text = "".join(out)
    whitespace = MAX_JSON_BYTES - len(text.encode("utf-8"))
    text += " " * whitespace
    total = len(text.encode("utf-8"))
    assert total == MAX_JSON_BYTES
    if len(text.encode("utf-8")) != len(text):
        raise ValueError("non-ASCII input is not supported")
    split = {
        "total": total,
        "number_bytes": number_bytes,
        "whitespace_bytes": whitespace,
        "other_bytes": total - number_bytes - whitespace,
        "numbers": len(spans),
        "numbers_at_max": at_max,
        "number_share": number_bytes / total,
    }
    return text, split


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("input")
    ap.add_argument("-o", "--output")
    ap.add_argument("--base-generation", type=int)
    try:
        args = ap.parse_args(argv)
    except SystemExit as exc:
        return 4 if exc.code else 0
    try:
        with open(args.input, encoding="utf-8") as f:
            body = f.read()
        text, split = pad(body.strip(), args.base_generation)
    except (OSError, ValueError) as exc:
        print(f"dsp_model_pad: {exc}", file=sys.stderr)
        return 1
    line = json.dumps(split)
    if args.output:
        with open(args.output, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        print(line)
    else:
        sys.stdout.write(text)
        print(line, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
