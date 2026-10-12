#!/usr/bin/env python3
"""Drive a measurement-loopback session against a live board.

The committed entry point for the acceptance evidence in
docs/reference/signal-path.md §Measured output quality: plays a stimulus
vector through the U24 with aplay (as dsp_matrix_round.py does), runs a
measurement, and runs the ISR-cost distribution comparison.

Usage:
  measure_run.py measure <id> [f0_l f0_r [probe ...]] [--baseline FILE]
  measure_run.py suite [--baseline FILE]   # every vector, then isr-cost
  measure_run.py isr-cost                  # armed-vs-not dsp_cycles_max gate
  --auth-file <path> (any position) reads the token from a file instead of env
  --baseline FILE (any position) compares against a baseline: a JSON object
      {vector_id: GET /api/measure session}, which `suite` prints on stdout

Arming (measure, isr-cost): the U24 playback mixer is set to unity
(amixer -c 0 cset numid=5 110,110) and read back, ONE continuous aplay
stream is started that repeats the vector's PCM (never an aplay re-run per
loop: a restart gap or carrier blip fails the criterion), then the run waits
for /api/status tx_gate.open and clock.state tracking before
POST /api/measure/start. The render stops when the run ends or fails.

Baseline pass rule, per vector: fail_reason none; each channel's thd_n_db,
snr_db and thd_db within 1.0 dB of the baseline's (a figure at its floor in
both runs counts as equal; the floors objects must match);
pcm_fr_multitone probes_dbfs entries within 0.1 dB; isr-cost PASS.

Environment variables:
  AIUS_DEVICE       board base URL         (default: http://192.0.2.66)
  AIUS_SD_AUTH      X-Aius-SD-Auth token   (required for measure/isr-cost)
  AIUS_ALSA_DEVICE  U24 playback device    (default: hw:U24XL,0)

Requests are paced and retried: the transport's TIME_WAIT cliff (network
hardening defect 24, closed as characterized) drops connections by count, not
rate, so a dropped connection here is the documented behaviour, not an error.
Exit: 0 ok / gate passed; 1 failure or gate exceeded; 2 usage.
"""
import json
import os
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

BASE = os.environ.get("AIUS_DEVICE", "http://192.0.2.66")
TOKEN = os.environ.get("AIUS_SD_AUTH", "")
ALSA_DEVICE = os.environ.get("AIUS_ALSA_DEVICE", "hw:U24XL,0")
VECTORS = Path(__file__).resolve().parent / "vectors"
ISR_WINDOW_S = 10.0
ISR_GATE_CYCLES = 30000  # the existing block-to-block dsp_cycles_max spread
FS = 48000
MIXER_NUMID = 5
MIXER_UNITY = "110,110"
ARM_TIMEOUT_S = 30.0
FIGURE_TOL_DB = 1.0
PROBE_TOL_DB = 0.1
FEED_LOOPS_PER_WRITE = 4  # 1 s of audio per pipe write

# vectors/manifest.txt: id -> (f0_l, f0_r, probes)
SUITE = {
    "pcm440_m6": (440, 440, []),
    "pcm440_full": (440, 440, []),
    "pcm440_660_split": (440, 660, []),
    "pcm_fr_multitone": (440, 440, [40, 100, 440, 1000, 4000, 10000, 16000]),
}


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


def set_unity_mixer():
    """amixer cset numid=5 110,110, then read it back; RuntimeError on drift."""
    subprocess.run(["amixer", "-c", "0", "cset", f"numid={MIXER_NUMID}",
                    MIXER_UNITY], check=True, capture_output=True, timeout=10)
    got = subprocess.run(["amixer", "-c", "0", "cget", f"numid={MIXER_NUMID}"],
                         check=True, capture_output=True, text=True,
                         timeout=10).stdout
    if f"values={MIXER_UNITY}" not in got:
        raise RuntimeError(f"U24 mixer numid={MIXER_NUMID} did not read back "
                           f"{MIXER_UNITY}: {got.strip()[-80:]}")


class Render:
    """One continuous aplay stream fed the vector's PCM repeated."""

    def __init__(self, blob):
        self.proc = subprocess.Popen(
            ["aplay", "-D", ALSA_DEVICE, "-f", "S16_LE", "-r", str(FS),
             "-c", "2", "-t", "raw", "-q", "-"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        self._stop = threading.Event()
        self._chunk = blob * FEED_LOOPS_PER_WRITE
        self._thread = threading.Thread(target=self._feed, daemon=True)
        self._thread.start()

    def _feed(self):
        try:
            while not self._stop.is_set():
                self.proc.stdin.write(self._chunk)
                self.proc.stdin.flush()
        except (BrokenPipeError, ValueError, OSError):
            pass

    def alive(self):
        return self.proc.poll() is None

    def stop(self):
        self._stop.set()
        self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()
        self._thread.join(timeout=5)


def wait_tracking(timeout_s=ARM_TIMEOUT_S, sleep=None, clock=None):
    """Poll /api/status until tx_gate.open and clock.state == tracking."""
    sleep = sleep or time.sleep
    clock = clock or time.monotonic
    started = clock()
    while True:
        status, body = call_json("GET", "/api/status")
        gate = body.get("tx_gate", {}).get("open")
        state = body.get("clock", {}).get("state")
        if status == 200 and gate and state == "tracking":
            return
        if clock() - started > timeout_s:
            raise RuntimeError(
                f"gate/clock not tracking after {timeout_s:.0f} s "
                f"(tx_gate.open={gate}, clock.state={state})")
        sleep(0.5)


def arm(vector):
    """Mixer to unity (read back), start the render, wait for the lock.

    Returns the live Render; the caller stops it. Raises RuntimeError."""
    set_unity_mixer()
    render = Render((VECTORS / f"{vector}.iec").read_bytes())
    try:
        wait_tracking()
        if not render.alive():
            raise RuntimeError("aplay exited before the run")
    except BaseException:
        render.stop()
        raise
    return render


def measure(vector, f0_l, f0_r, probes):
    """Run one session; return (rc, session or None)."""
    body = {"id": vector, "f0_l": f0_l, "f0_r": f0_r}
    if probes:
        body["probes"] = probes
    try:
        render = arm(vector)
    except (RuntimeError, subprocess.SubprocessError, OSError) as error:
        print(f"arm failed: {error}")
        return 1, None
    try:
        status, reply = call_json("POST", "/api/measure/start", body)
        if status != 200:
            print(f"arm refused: {status} {reply}")
            return 1, None
        session = {"state": "idle"}
        for _ in range(40):
            time.sleep(0.5)
            status, session = call_json("GET", "/api/measure")
            if session["state"] in ("done", "failed"):
                break
    finally:
        render.stop()
    return (0 if session["state"] == "done" else 1), session


def _at_floor(key, value, floors):
    if key == "thd_n_db":
        return value <= floors["thd_n_db"]
    if key == "snr_db":
        return value >= floors["snr_db"]
    return value <= floors["thd_dbc"]  # thd_db


def compare_session(vector, base, run):
    """Failures (list of str) of `run` against `base` for one vector."""
    out = []
    if run.get("fail_reason") != "none" or run.get("state") != "done":
        out.append(f"{vector}: fail_reason {run.get('fail_reason')!r} "
                   f"state {run.get('state')!r}")
        return out
    if run.get("floors") != base.get("floors"):
        out.append(f"{vector}: floors differ {run.get('floors')} vs "
                   f"{base.get('floors')}")
        return out
    floors = run["floors"]
    for ch in ("l", "r"):
        a, b = base["figures"][ch], run["figures"][ch]
        for key in ("thd_n_db", "snr_db", "thd_db"):
            if _at_floor(key, a[key], floors) and _at_floor(key, b[key], floors):
                continue
            if abs(a[key] - b[key]) > FIGURE_TOL_DB + 1e-9:
                out.append(f"{vector} {ch} {key}: {b[key]} vs baseline "
                           f"{a[key]} (> {FIGURE_TOL_DB} dB)")
        pa, pb = a.get("probes_dbfs", []), b.get("probes_dbfs", [])
        if len(pa) != len(pb):
            out.append(f"{vector} {ch} probes_dbfs: {len(pb)} entries vs "
                       f"baseline {len(pa)}")
            continue
        for i, (x, y) in enumerate(zip(pa, pb)):
            if abs(x - y) > PROBE_TOL_DB + 1e-9:
                out.append(f"{vector} {ch} probes_dbfs[{i}]: {y} vs baseline "
                           f"{x} (> {PROBE_TOL_DB} dB)")
    return out


def load_baseline(path):
    return json.loads(Path(path).read_text())


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
    try:
        render = arm("pcm440_m6")
    except (RuntimeError, subprocess.SubprocessError, OSError) as error:
        print(f"arm failed: {error}")
        return 1
    try:
        baseline = isr_phase(armed=False)
        time.sleep(25)  # TIME_WAIT drain between phases (defect 24's 24 s bound)
        armed = isr_phase(armed=True)
    finally:
        render.stop()
    delta = armed["cycles_max"] - baseline["cycles_max"]
    print("baseline:", baseline)
    print("armed   :", armed)
    print(f"dsp_cycles_max delta {delta:+d} vs gate +/-{ISR_GATE_CYCLES}")
    print("PASS" if abs(delta) <= ISR_GATE_CYCLES else "FAIL")
    return 0 if abs(delta) <= ISR_GATE_CYCLES else 1


def suite(baseline):
    """Every vector in SUITE, then isr-cost when comparing a baseline."""
    sessions, failures = {}, []
    for vector, (f0_l, f0_r, probes) in SUITE.items():
        rc, session = measure(vector, f0_l, f0_r, probes)
        if session is None:
            failures.append(f"{vector}: no session")
            continue
        sessions[vector] = session
        if baseline is not None:
            if vector not in baseline:
                failures.append(f"{vector}: absent from the baseline")
            else:
                failures += compare_session(vector, baseline[vector], session)
        elif rc != 0:
            failures.append(f"{vector}: state {session.get('state')!r}")
    print(json.dumps(sessions, indent=1))
    if baseline is not None and isr_cost() != 0:
        failures.append("isr_cost: FAIL")
    for line in failures:
        print("FAIL", line)
    if failures:
        print("FAIL")
    else:
        print("baseline: PASS" if baseline is not None else "run: done")
    return 1 if failures else 0


def pop_option(name):
    if name not in sys.argv:
        return None
    at = sys.argv.index(name)
    value = sys.argv[at + 1]
    del sys.argv[at:at + 2]
    return value


def main():
    global TOKEN
    auth = pop_option("--auth-file")
    if auth:
        TOKEN = Path(auth).read_text().strip()
    baseline_path = pop_option("--baseline")
    baseline = load_baseline(baseline_path) if baseline_path else None
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    command = sys.argv[1]
    if command == "measure" and len(sys.argv) >= 3:
        vector = sys.argv[2]
        f0_l = int(sys.argv[3]) if len(sys.argv) > 3 else 440
        f0_r = int(sys.argv[4]) if len(sys.argv) > 4 else 440
        probes = [int(p) for p in sys.argv[5:]]
        rc, session = measure(vector, f0_l, f0_r, probes)
        if session is not None:
            print(json.dumps(session, indent=1))
        if baseline is not None and session is not None:
            if vector not in baseline:
                print(f"FAIL {vector}: absent from the baseline")
                return 1
            failures = compare_session(vector, baseline[vector], session)
            for line in failures:
                print("FAIL", line)
            return 1 if failures or rc else 0
        return rc
    if command == "suite":
        return suite(baseline)
    if command == "isr-cost":
        return isr_cost()
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
