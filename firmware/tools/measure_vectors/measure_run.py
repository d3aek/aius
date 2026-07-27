#!/usr/bin/env python3
"""Drive a measurement-loopback session against a live board.

The committed entry point for the acceptance evidence in
docs/reference/signal-path.md §Measured output quality: uploads the stimulus
corpus, runs a measurement, and runs the ISR-cost distribution comparison.

Usage:
  measure_run.py upload [names...]      # PUT the corpus to /sd/replays/<id>
  measure_run.py measure <id> [f0_l f0_r [probe ...]]
  measure_run.py isr-cost               # armed-vs-not dsp_cycles_max gate

Env:
  AIUS_DEVICE    board base URL         (default: http://192.0.2.66)
  AIUS_SD_AUTH   X-Aius-SD-Auth token   (required for upload/measure/isr-cost)

Requests are paced and retried: the transport's TIME_WAIT cliff (network
hardening defect 24, closed as characterized) drops connections by count, not
rate, so a dropped connection here is the documented behaviour, not an error.
Exit: 0 ok / gate passed; 1 failure or gate exceeded; 2 usage.
"""
import json
import os
import sys
import time
import urllib.request
from pathlib import Path

BASE = os.environ.get("AIUS_DEVICE", "http://192.0.2.66")
TOKEN = os.environ.get("AIUS_SD_AUTH", "")
CHUNK = 8192  # under the device's 13,312-byte request buffer with headers
VECTORS = Path(__file__).resolve().parent / "vectors"
ISR_WINDOW_S = 10.0
ISR_GATE_CYCLES = 30000  # the existing block-to-block dsp_cycles_max spread


def call(method, path, data=None, headers=None, retries=4):
    last = None
    for attempt in range(retries):
        req = urllib.request.Request(BASE + path, data=data, method=method)
        if TOKEN:
            req.add_header("X-Aius-SD-Auth", TOKEN)
        for key, value in (headers or {}).items():
            req.add_header(key, value)
        try:
            with urllib.request.urlopen(req, timeout=10) as response:
                return response.status, response.read()
        except urllib.error.HTTPError as error:
            return error.code, error.read()
        except Exception as exc:  # RemoteDisconnected / timeout: defect 24
            last = exc
            time.sleep(1.5 * (attempt + 1))
    raise last


def call_json(method, path, obj=None):
    status, body = call(
        method, path, json.dumps(obj).encode() if obj is not None else None,
        {"Content-Type": "application/json"} if obj is not None else None)
    return status, json.loads(body) if body else {}


def upload(names):
    for name in names:
        blob = (VECTORS / f"{name}.iec").read_bytes()
        total = len(blob)
        for start in range(0, total, CHUNK):
            end = min(start + CHUNK, total) - 1
            status, body = call(
                "PUT", f"/sd/replays/{name}", blob[start:end + 1],
                {"Content-Range": f"bytes {start}-{end}/{total}",
                 "Content-Type": "application/octet-stream"})
            if status != 200:
                print(f"{name}: chunk {start}-{end} -> {status} {body[:120]}")
                return 1
        print(f"{name}: uploaded {total} bytes")
    return 0


def measure(vector, f0_l, f0_r, probes):
    body = {"id": vector, "f0_l": f0_l, "f0_r": f0_r}
    if probes:
        body["probes"] = probes
    status, reply = call_json("POST", "/api/replays/start",
                              {"id": vector, "loop": True})
    if status != 200:
        print(f"replay start refused: {status} {reply}")
        return 1
    status, reply = call_json("POST", "/api/measure/start", body)
    if status != 200:
        print(f"arm refused: {status} {reply}")
        return 1
    for _ in range(40):
        time.sleep(0.5)
        status, session = call_json("GET", "/api/measure")
        if session["state"] in ("done", "failed"):
            break
    call_json("POST", "/api/replays/stop")
    print(json.dumps(session, indent=1))
    return 0 if session["state"] == "done" else 1


def isr_phase(armed):
    call_json("POST", "/api/clear")
    lasts = []
    started = time.monotonic()
    while time.monotonic() - started < ISR_WINDOW_S:
        if armed:
            _, session = call_json("GET", "/api/measure")
            if session["state"] in ("idle", "done", "failed"):
                call_json("POST", "/api/measure/start", {"id": "pcm440_m6"})
        _, snapshot = call_json("GET", "/api/meters")
        lasts.append(snapshot["dsp_cycles_last"])
        time.sleep(0.6)
    _, final = call_json("GET", "/api/meters")
    lasts.sort()
    return {"last_p50": lasts[len(lasts) // 2], "last_min": lasts[0],
            "last_max": lasts[-1], "cycles_max": final["dsp_cycles_max"]}


def isr_cost():
    status, reply = call_json("POST", "/api/replays/start",
                              {"id": "pcm440_m6", "loop": True})
    if status != 200:
        print(f"replay start refused: {status} {reply}")
        return 1
    baseline = isr_phase(armed=False)
    time.sleep(25)  # TIME_WAIT drain between phases (defect 24's 24 s bound)
    armed = isr_phase(armed=True)
    call_json("POST", "/api/replays/stop")
    delta = armed["cycles_max"] - baseline["cycles_max"]
    print("baseline:", baseline)
    print("armed   :", armed)
    print(f"dsp_cycles_max delta {delta:+d} vs gate +/-{ISR_GATE_CYCLES}")
    print("PASS" if abs(delta) <= ISR_GATE_CYCLES else "FAIL")
    return 0 if abs(delta) <= ISR_GATE_CYCLES else 1


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    command = sys.argv[1]
    if command == "upload":
        names = sys.argv[2:] or [p.stem for p in sorted(VECTORS.glob("*.iec"))]
        return upload(names)
    if command == "measure" and len(sys.argv) >= 3:
        f0_l = int(sys.argv[3]) if len(sys.argv) > 3 else 440
        f0_r = int(sys.argv[4]) if len(sys.argv) > 4 else 440
        probes = [int(p) for p in sys.argv[5:]]
        return measure(sys.argv[2], f0_l, f0_r, probes)
    if command == "isr-cost":
        return isr_cost()
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
