#!/usr/bin/env python3
"""E-AC-3 phase-4 on-box acceptance runner.

Polls the device until the RX carrier drops (the operator darkens the bench
optical feed first — see the open-work register §Waiting on the operator),
then, in order:

1. starts the looped E-AC-3 replay and samples /api/status +
   /api/replays/status (codec_pc 21, decode counters, eac3_decode_cycles
   against the 8 ms budget);
2. re-measures the AC-3 and DTS controls beside it (their tables moved to
   flash-cached reads in the phase-4 image);
3. stops every replay it started and snapshots a final status.

Evidence lands as JSONL under --out. A replay drains a fixed 96,000 words/s
against the type-21 12,288-word/32 ms burst period, so the E-AC-3 pass runs
at ~1/4 real time: this is a structural decode proof, never gapless playback
(plan phase-4 acceptance note).
"""
import argparse
import json
import os
import time
import urllib.error
import urllib.request

parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
parser.add_argument("--device", default="http://192.0.2.66",
                    help="device base URL")
parser.add_argument("--token", default=os.environ.get("AIUS_SD_AUTH"),
                    help="X-Aius-SD-Auth token (or env AIUS_SD_AUTH)")
parser.add_argument("--out", default="eac3-acceptance-out",
                    help="evidence output directory")
parser.add_argument("--wait", type=float, default=14400.0,
                    help="max seconds to wait for the carrier to drop")
parser.add_argument("--eac3-id", default="eac3_id51_3s")
parser.add_argument("--ac3-id", default="channel_id_51_ac3")
parser.add_argument("--dts-id", default="channel_id_51")
args = parser.parse_args()
if not args.token:
    parser.error("--token (or AIUS_SD_AUTH) is required")

POLL_S = 4.0
os.makedirs(args.out, exist_ok=True)


def req(path, body=None, method=None, timeout=5.0):
    status, text, _ = req_full(path, body, method, timeout)
    return status, text


def req_full(path, body=None, method=None, timeout=5.0):
    """As req, plus the Retry-After header: (status, text, retry_after)."""
    if method is None:
        method = "POST" if body is not None else "GET"
    r = urllib.request.Request(args.device + path,
                               data=body.encode() if body is not None else
                               (b"" if method == "POST" else None),
                               method=method)
    r.add_header("X-Aius-SD-Auth", args.token)
    if body is not None:
        r.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            return resp.status, resp.read().decode(), None
    except urllib.error.HTTPError as err:
        return (err.code, err.read().decode(),
                err.headers.get("Retry-After") if err.headers else None)
    except Exception as err:  # unreachable / timeout
        return None, str(err), None


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


FILL_BUSY_BUDGET_S = 120.0


def start(replay_id, post=None, sleep=time.sleep, clock=time.monotonic,
          budget_s=FILL_BUSY_BUDGET_S):
    """Start a looped replay; retry only `503 fill_busy` (web-cache fill walk
    incomplete) per Retry-After (absent or bad = 1 s) within `budget_s`, then
    raise RuntimeError. Any other status is returned for the caller."""
    post = post or (lambda: req_full(
        "/api/replays/start", json.dumps({"id": replay_id, "loop": True})))
    started = clock()
    while True:
        status, text, retry_after = post()
        if status != 503 or '"fill_busy"' not in text:
            return status, text
        try:
            wait = max(float(retry_after), 0.0)
        except (TypeError, ValueError):
            wait = 1.0
        if clock() - started + wait > budget_s:
            raise RuntimeError(
                f"replay start still 503 fill_busy after {budget_s:.0f} s "
                "(web-cache fill walk never completed)")
        sleep(wait)


def stop():
    return req("/api/replays/stop", body="{}")


def sample(tag, seconds):
    path = os.path.join(args.out, f"{tag}-samples.jsonl")
    t0 = time.monotonic()
    n = 0
    with open(path, "a") as sink:
        while time.monotonic() - t0 < seconds:
            st, body = req("/api/status")
            rt, rbody = req("/api/replays/status")
            line = {"t": round(time.monotonic() - t0, 2), "http": st}
            if st == 200:
                try:
                    line["status"] = json.loads(body)
                except ValueError:
                    line["raw"] = body[:500]
            else:
                line["error"] = (body or "")[:200]
            if rt == 200:
                try:
                    line["replay"] = json.loads(rbody)
                except ValueError:
                    pass
            sink.write(json.dumps(line) + "\n")
            n += 1
            time.sleep(2.0)
    log(f"{tag}: {n} samples -> {path}")


def run_replay(replay_id, tag, seconds):
    st, body = start(replay_id)
    log(f"start {replay_id}: {st} {body.strip()[:120]}")
    if st != 200:
        return False
    try:
        sample(tag, seconds)
    finally:
        st, body = stop()
        log(f"stop after {tag}: {st} {body.strip()[:80]}")
    time.sleep(2.0)
    return True


# Phase 1 — the start attempt is the exact probe the firmware runs
# (app_replay_start refuses on AudioInputSPDIF3::pllLocked()).
log("phase 1: waiting for the RX carrier to drop")
t0 = time.monotonic()
won = False
last_carrier = None
with open(os.path.join(args.out, "carrier-watch.jsonl"), "a") as sink:
    while time.monotonic() - t0 < args.wait:
        st, body = req("/api/status")
        carrier = None
        srpc = None
        if st == 200:
            try:
                parsed = json.loads(body)
                carrier = parsed.get("carrier")
                srpc = (parsed.get("clk_regs") or {}).get("spdif_srpc")
            except ValueError:
                pass
        sink.write(json.dumps({"t": round(time.monotonic() - t0, 1),
                               "http": st, "carrier": carrier,
                               "srpc": srpc}) + "\n")
        sink.flush()
        if carrier != last_carrier:
            log(f"carrier -> {carrier} (srpc {srpc}) "
                f"at +{time.monotonic() - t0:.0f}s")
            last_carrier = carrier
        rc, rbody = start(args.eac3_id)
        if rc == 200:
            log(f"E-AC-3 REPLAY STARTED at +{time.monotonic() - t0:.0f}s: "
                f"{rbody.strip()}")
            won = True
            break
        if rc not in (409, None):
            log(f"unexpected start result {rc}: {rbody.strip()[:200]}")
        time.sleep(POLL_S)

if not won:
    # carrier=0 with a persisting 409 here convicts raw pllLocked()
    # asserting without light (boot-lottery fossil family) — register it.
    log("NEVER STARTED: carrier never dropped within the window")
    raise SystemExit(1)

# Phase 2 — E-AC-3 structural decode evidence.
try:
    sample("eac3", 45)
finally:
    st, body = stop()
    log(f"stop after eac3: {st} {body.strip()[:80]}")
time.sleep(2.0)

# Phase 3 — AC-3 and DTS controls with flash-cached tables.
run_replay(args.ac3_id, "ac3", 15)
run_replay(args.dts_id, "dts", 20)

st, body = req("/api/status")
with open(os.path.join(args.out, "final-status.json"), "w") as sink:
    sink.write(body if st == 200 else json.dumps({"http": st, "err": body}))
log("done")
