#!/usr/bin/env python3
"""Set named paths in the live DSP config and PUT it back, stdlib only.

Run from the repository root; the auth file is .secrets/aius-sd-auth there.

  python3 firmware/tools/dsp_config/aius_dsp_set.py --device http://<addr> --auth-file .secrets/aius-sd-auth --set features.limiter=true
  python3 firmware/tools/dsp_config/aius_dsp_set.py --device http://<addr> --auth-file .secrets/aius-sd-auth --set matrix.center_gain_linear=1.2 --dry-run
  python3 firmware/tools/dsp_config/aius_dsp_set.py ... --set features.limiter=true --wait 30

Wire shape (aius_dsp_config.h, app_store_dsp_config in optical_main.cpp):
PUT /api/dsp/config takes the WHOLE config, the keys GET returns plus
"base_generation" (the optimistic-concurrency token). The firmware answers
409 (stale) when base_generation != its applied config_generation, so the
tool carries the GET's config_generation as base_generation. A 200 means the
SD commit landed; config_generation advances only at the audio block-boundary
apply, so with no carrier GET keeps the old generation and config_pending true.

Each --set is PATH=JSON (dotted path; a numeric segment indexes a list). A
path that does not already exist in the config is refused, as are NaN and
Infinity. If the GET already shows config_pending true (another client's save
is staged and would be overwritten) the tool refuses; --dry-run still prints
the diff but warns. --dry-run makes no PUT.

After a 200 the tool re-GETs and prints config_generation and config_pending.
A save applies only when an audio block arrives (a carrier or a replay); until
then GET shows config_pending true. --wait SECONDS polls (1 s interval): exit 0
when config_pending is false and the applied config matches what we sent
(generation is ignored, it restarts at boot); exit 1 when pending is false, the
config differs and the generation is above the base (another client's save won);
otherwise keep polling. Without a carrier --wait exits 3 with the save staged.

If the PUT fails at transport level the card write may have committed: the tool
re-GETs once, treats a pending or matching config as saved, else exits 1 with
"outcome unknown, re-run --dry-run to check".

Known limits: a -0 value is stored as 0; with matrix.clipfree true any save
stores the computed headroom; any save may move a float by a few ulps (relative
1e-12 tolerance in the applied-config compare)
(firmware decoder rounding); another client's save that commits between our GET
and our PUT is overwritten (the firmware's stale-check design).

Exit codes:
  0  done (applied, or saved when --wait is not given; or a dry run)
  1  HTTP or transport failure (the device's refusal body is printed), outcome
     unknown after a failed PUT, or another client's save was applied instead
  2  local refusal or bad input (usage, bad --wait, unknown path, NaN/Infinity,
     config_pending at the initial GET, missing, empty or multi-line auth file,
     non-JSON GET)
  3  --wait timed out: saved to the card but not yet applied (no carrier?), or
     the board never answered (rebooted? the save loads at boot)
"""
import argparse
import copy
import json
import math
import sys
import time
import urllib.error
import urllib.request

ROUTE = "/api/dsp/config"


class UsageError(Exception):
    pass


def urllib_transport(method, url, headers, body, timeout):
    """One HTTP exchange: (status | None, raw body text)."""
    req = urllib.request.Request(url, data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return response.status, response.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode("utf-8", "replace")
    except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as error:
        return None, str(error)
    except Exception as error:  # noqa: BLE001 - class name only: the message may carry the token
        return None, f"transport error ({type(error).__name__}, details withheld)"


def _step(node, segment, path):
    if isinstance(node, dict) and segment in node:
        return segment
    if isinstance(node, list) and segment.isdigit() and int(segment) < len(node):
        return int(segment)
    raise UsageError(f"unknown path: {path}")


def _reject_constant(name):
    raise ValueError(f"{name} is not allowed")


def _has_non_finite(value):
    if isinstance(value, float):
        return not math.isfinite(value)
    if isinstance(value, dict):
        return any(_has_non_finite(v) for v in value.values())
    if isinstance(value, list):
        return any(_has_non_finite(v) for v in value)
    return False


def parse_assignment(text):
    path, sep, raw = text.partition("=")
    if not sep or not path:
        raise UsageError(f"--set wants PATH=JSON, got: {text}")
    try:
        value = json.loads(raw, parse_constant=_reject_constant)
    except (json.JSONDecodeError, ValueError) as error:
        raise UsageError(f"--set {path}: value is not finite JSON ({error})")
    if _has_non_finite(value):
        raise UsageError(f"--set {path}: value is not finite JSON (overflows to Infinity)")
    return path, value


def apply_sets(config, assignments):
    """A deep copy of config with each (path, value) set; every path must
    already exist."""
    result = copy.deepcopy(config)
    for path, value in assignments:
        node = result
        segments = path.split(".")
        for segment in segments[:-1]:
            node = node[_step(node, segment, path)]
        node[_step(node, segments[-1], path)] = value
    return result


def build_put_body(live, assignments):
    body = apply_sets(live, assignments)
    body["base_generation"] = live["config_generation"]
    return body


def diff_paths(before, after, prefix=""):
    """[(path, old, new)] for leaves that differ."""
    if isinstance(before, dict) and isinstance(after, dict):
        pairs = [(k, before.get(k), after.get(k)) for k in dict.fromkeys([*before, *after])]
    elif isinstance(before, list) and isinstance(after, list) and len(before) == len(after):
        pairs = [(str(i), b, a) for i, (b, a) in enumerate(zip(before, after))]
    else:
        return [] if before == after else [(prefix, before, after)]
    out = []
    for key, b, a in pairs:
        out.extend(diff_paths(b, a, f"{prefix}.{key}" if prefix else key))
    return out


def validate_token(text):
    """The token, or UsageError (message never includes it)."""
    token = text.strip()
    if not token:
        raise UsageError("auth file is empty")
    if not all(0x20 <= ord(c) < 0x7F for c in token):
        raise UsageError("auth file must hold one line of printable ASCII")
    return token


def wait_seconds(text):
    try:
        value = float(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"not a number: {text}")
    if not math.isfinite(value) or value < 0:
        raise argparse.ArgumentTypeError("must be a finite number >= 0")
    return value


# The firmware PUT decoder is not correctly rounded: newly typed values land
# several ulps off (0.03 reads back as 0.030000000000000006, 2 ulps; up to 5
# measured). See docs/plans/aius-open-work-register.md, Open defects.
NUMBER_RELATIVE_TOLERANCE = 1e-12

_IGNORED = {"config_generation", "config_pending", "staged_generation", "base_generation"}


_IGNORED_TOP = _IGNORED | {"source"}  # a PUT always reads back as "sd"


def matches_applied(sent, applied):
    """True when the applied GET is what we sent."""
    return not config_differences(sent, applied)


def config_differences(sent, applied):
    """[(path, sent, applied)] where the applied GET departs from what we sent:
    bookkeeping keys and source ignored, matrix.headroom_linear skipped when
    clipfree serves it computed, numbers within a few ulps (relative 1e-12
    tolerance) equal."""
    sent, applied = copy.deepcopy(sent), copy.deepcopy(applied)
    for cfg in (sent, applied):
        for key in _IGNORED_TOP:
            cfg.pop(key, None)
    matrix = applied.get("matrix")
    if isinstance(matrix, dict) and matrix.get("clipfree") is True:
        matrix.pop("headroom_linear", None)
        if isinstance(sent.get("matrix"), dict):
            sent["matrix"].pop("headroom_linear", None)
    return _differences(sent, applied, "")


def _differences(a, b, path):
    """Leaf departures, numbers within a few ulps (relative 1e-12 tolerance) equal."""
    join = (lambda key: f"{path}.{key}" if path else str(key))
    if isinstance(a, dict) and isinstance(b, dict):
        out = []
        for key in dict.fromkeys([*a, *b]):
            if key in a and key in b:
                out.extend(_differences(a[key], b[key], join(key)))
            else:
                out.append((join(key), a.get(key), b.get(key)))
        return out
    if isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        out = []
        for i, (x, y) in enumerate(zip(a, b)):
            out.extend(_differences(x, y, join(i)))
        return out
    numeric = (int, float)
    if (isinstance(a, numeric) and isinstance(b, numeric)
            and not isinstance(a, bool) and not isinstance(b, bool)):
        same = a == b or abs(a - b) <= NUMBER_RELATIVE_TOLERANCE * max(abs(a), abs(b))
    else:
        same = a == b
    return [] if same else [(path, a, b)]


def _describe(differences, limit=5):
    shown = [f"{p}: sent {json.dumps(s)}, applied {json.dumps(a)}"
             for p, s, a in differences[:limit]]
    more = len(differences) - limit
    return "; ".join(shown) + (f"; and {more} more" if more > 0 else "")


def _get_config(transport, url):
    """(config dict | None, error text, exit code, http status)."""
    status, text = transport("GET", url, {}, None, 10)
    if status != 200:
        return None, f"GET {ROUTE} failed: {status} {text}", 1, status
    try:
        live = json.loads(text)
    except ValueError:
        return None, f"GET {ROUTE} returned non-JSON: {text[:200]}", 2, status
    if not isinstance(live, dict) or "config_generation" not in live:
        return None, f"GET {ROUTE} returned no config_generation: {text[:200]}", 2, status
    return live, "", 0, status


def _state(config):
    return (f"config_generation={config.get('config_generation')} "
            f"config_pending={json.dumps(config.get('config_pending'))}")


def run(base, token, assignments, dry_run, transport=urllib_transport,
        out=sys.stdout, err=sys.stderr, wait=0, sleep=time.sleep,
        now=time.monotonic):
    url = base.rstrip("/") + ROUTE
    live, message, code, _ = _get_config(transport, url)
    if live is None:
        print(message, file=err)
        return code
    pending = live.get("config_pending") is True
    if pending:
        warning = (f"config_pending is true: another save is staged "
                   f"(staged_generation={live.get('staged_generation')}, "
                   f"config_generation={live['config_generation']}) and a PUT "
                   f"would overwrite it")
        if not dry_run:
            print(f"refused: {warning}", file=err)
            return 2
        print(f"warning: {warning}", file=err)
    try:
        body = build_put_body(live, assignments)
    except UsageError as error:
        print(error, file=err)
        return 2
    changes = diff_paths(live, body)
    # base_generation is the carried token, not a requested change.
    changes = [c for c in changes if c[0] != "base_generation"]
    for path, old, new in changes:
        print(f"{path}: {json.dumps(old)} -> {json.dumps(new)}", file=out)
    if not changes:
        print("no change", file=out)
    if dry_run:
        return 0
    headers = {"X-Aius-SD-Auth": token, "Content-Type": "application/json"}
    status, text = transport("PUT", url, headers,
                             json.dumps(body, allow_nan=False).encode("utf-8"), 30)
    if status is not None and not 200 <= status < 300:
        print(f"PUT {ROUTE} refused: {status} {text}", file=err)
        return 1
    if status is None:
        # Transport fault: the card write may have committed anyway.
        print(f"PUT {ROUTE} transport fault: {text}", file=err)
        check, _, _, _ = _get_config(transport, url)
        diffs = config_differences(body, check) if check is not None else None
        if check is not None and check.get("config_pending") is True and diffs:
            print(f"another client's save is staged; ours did not land "
                  f"({_describe(diffs)})", file=err)
            return 1
        if check is not None and not diffs:
            print("the save landed (matching config on the board)", file=out)
        else:
            print("outcome unknown, re-run --dry-run to check", file=err)
            return 1
    else:
        print(f"PUT ok: {status} {text}", file=out)
    base_generation = live["config_generation"]
    deadline = now() + wait
    answered = False
    last_diffs = None  # differences at the last answered poll (None: not applied-looking)
    last_generation = None
    while True:
        after, message, _, poll_status = _get_config(transport, url)
        answered = answered or poll_status is not None
        if after is None:
            print(f"post-PUT {message}", file=err)
        else:
            print(_state(after), file=out)
            diffs = config_differences(body, after)
            last_generation = after["config_generation"]
            last_diffs = diffs if after.get("config_pending") is False else None
            if wait and after.get("config_pending") is False:
                if not diffs:
                    return 0
                if after["config_generation"] > base_generation:
                    print("applied config differs from the one we sent: another "
                          "client's save was applied in place of ours "
                          f"({_describe(diffs)})", file=err)
                    return 1
        if not wait:
            return 0
        if now() >= deadline:
            if last_diffs and last_generation <= base_generation:
                print(f"timed out after {wait} s: the board is running a different "
                      f"config than ours (rebooted, or another client's save): "
                      f"{_describe(last_diffs)}", file=err)
            elif answered:
                print(f"timed out after {wait} s: the save is committed to the card "
                      f"but not yet applied (no carrier?)", file=err)
            else:
                print(f"timed out after {wait} s: the board did not answer "
                      f"(rebooted?); the save is on the card and loads at boot",
                      file=err)
            return 3
        sleep(1)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", required=True,
                        help="base URL, e.g. http://192.0.2.66")
    parser.add_argument("--auth-file", required=True,
                        help="file holding the X-Aius-SD-Auth token (one line)")
    parser.add_argument("--set", dest="sets", action="append", required=True,
                        metavar="PATH=JSON", help="repeatable, e.g. features.limiter=true")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the diff against the live GET; no PUT")
    parser.add_argument("--wait", type=wait_seconds, default=0, metavar="SECONDS",
                        help="after the PUT, poll until the config is applied")
    args = parser.parse_args()
    try:
        assignments = [parse_assignment(s) for s in args.sets]
    except UsageError as error:
        print(error, file=sys.stderr)
        return 2
    try:
        with open(args.auth_file, "r", encoding="utf-8") as handle:
            token = validate_token(handle.read())
    except OSError as error:
        print(f"cannot read --auth-file {args.auth_file}: {error.strerror}",
              file=sys.stderr)
        return 2
    except UnicodeDecodeError:
        print(f"--auth-file {args.auth_file} is not ASCII text", file=sys.stderr)
        return 2
    except UsageError as error:
        print(f"--auth-file {args.auth_file}: {error}", file=sys.stderr)
        return 2
    return run(args.device, token, assignments, args.dry_run, wait=args.wait)


if __name__ == "__main__":
    sys.exit(main())
