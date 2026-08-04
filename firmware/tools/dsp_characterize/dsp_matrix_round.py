#!/usr/bin/env python3
"""DSP matrix (5.1 downmix) characterization via the channel-ID DTS vector.

channel_id_51 carries one steady identifying tone per source channel
(FL 300 / FR 600 / FC 900 / LFE 60 / SL 1200 / SR 1500 Hz), so a single
stereo capture reads every downmix weight at once: the amplitude of each
tone in L and R IS that source channel's routing weight (times the fixed
decode gain, cancelled by expressing variants relative to baseline).
All tones are multiples of 60 Hz -> exact bins of a 1 s (48,000-sample)
rectangular window.

Every variant's PUT body is built from the ORIGINAL document plus the one
override set, so variants are absolute against the same reference whatever
the staging state between rounds; apply lands at the next audio block
boundary, so each round starts the DTS playback first and polls
config_generation before capturing.

Usage: dsp_matrix_round.py <output-dir>
"""

import json
import math
import os
import subprocess
import sys
import time
import urllib.request

import numpy as np

BASE = os.environ.get("AIUS_BASE_URL", "http://192.0.2.66")
DEVICE = os.environ.get("AIUS_ALSA_DEVICE", "hw:U24XL,0")
FS = 48000
W = 48000  # 1 s analysis window, 1 Hz bins
IEC = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "replay_samples", "generated",
                   "channel_id_51.iec")

TONES = {"fl": 300, "fr": 600, "fc": 900, "lfe": 60, "sl": 1200, "sr": 1500}

VARIANTS = [
    ("baseline", {}),
    ("center_hot", {"center_gain_linear": 1.0}),
    ("surround_cut", {"surround_gain_linear": 0.3535533905932738}),
    ("lfe_hot", {"lfe_gain_linear": 0.6324555320336759}),
    ("headroom_up", {"headroom_linear": 0.7071067811865476}),
]


def token():
    value = os.environ.get("AIUS_SD_AUTH", "")
    if not value:
        raise SystemExit("AIUS_SD_AUTH is required (AIUS AUTH over CDC)")
    return value


def api_get(path):
    with urllib.request.urlopen(BASE + path, timeout=6) as response:
        return json.loads(response.read().decode())


def api_put_config(document):
    request = urllib.request.Request(
        BASE + "/api/dsp/config",
        data=json.dumps(document).encode(), method="PUT")
    request.add_header("X-Aius-SD-Auth", token())
    request.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(request, timeout=15) as response:
        return response.status


def put_from_original(original, matrix_overrides):
    """PUT original + overrides; returns the pre-PUT generation."""
    live_generation = api_get("/api/dsp/config")["config_generation"]
    document = json.loads(json.dumps(original))
    document["matrix"].update(matrix_overrides)
    body = {
        "source": "sd",
        "config_generation": live_generation,
        "sample_rate_hz": document["sample_rate_hz"],
        "features": document["features"],
        "input_channels": document["input_channels"],
        "output_eq": document["output_eq"],
        "matrix": document["matrix"],
        "limiter": document["limiter"],
        "base_generation": live_generation,
    }
    status = api_put_config(body)
    if status != 200:
        raise RuntimeError(f"PUT refused: {status}")
    return live_generation


def wait_applied(previous, deadline_s=15.0):
    end = time.time() + deadline_s
    while time.time() < end:
        if api_get("/api/dsp/config")["config_generation"] > previous:
            return
        time.sleep(0.3)
    raise RuntimeError("config did not apply")


def capture_tones(generation_to_wait):
    player = subprocess.Popen(
        ["aplay", "-D", DEVICE, "-f", "S16_LE", "-r", str(FS), "-c", "2",
         "-t", "raw", "-q", IEC],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if generation_to_wait is not None:
            wait_applied(generation_to_wait)
        time.sleep(4.0)  # classification + settle
        capture = subprocess.run(
            ["arecord", "-D", DEVICE, "-f", "S16_LE", "-r", str(FS),
             "-c", "2", "-t", "raw", "-q", "-d", "3", "/dev/stdout"],
            capture_output=True, timeout=20)
        raw = np.frombuffer(capture.stdout, dtype="<i2")
        frames = raw.reshape(-1, 2).astype(np.float64) / 32767.0
        if len(frames) < 2 * W:
            raise RuntimeError(f"short capture: {len(frames)}")
        segment = frames[W:2 * W]
        out = {}
        for channel, name in ((0, "L"), (1, "R")):
            spectrum = np.abs(np.fft.rfft(segment[:, channel])) / (W / 2)
            out[name] = {source: float(spectrum[hz])
                         for source, hz in TONES.items()}
        return out
    finally:
        player.terminate()
        player.wait(timeout=10)


def to_db(x):
    return 20 * math.log10(x + 1e-12)


def main():
    output = sys.argv[1]
    os.makedirs(output, exist_ok=True)
    original = api_get("/api/dsp/config")
    with open(os.path.join(output, "original-config.json"), "w") as handle:
        json.dump(original, handle, indent=1)

    results = {"tones_hz": TONES, "variants": {}}
    baseline = None
    for name, overrides in VARIANTS:
        generation = put_from_original(original, overrides)
        tones = capture_tones(generation)
        entry = {
            "overrides": overrides,
            "levels_db": {ch: {src: round(to_db(v), 2)
                               for src, v in tones[ch].items()}
                          for ch in tones},
        }
        if baseline is None:
            baseline = tones
            print("baseline: " + json.dumps(entry["levels_db"]), flush=True)
        else:
            deltas = {ch: {src: round(to_db(tones[ch][src]) -
                                      to_db(baseline[ch][src]), 2)
                           for src in TONES}
                      for ch in tones}
            entry["delta_vs_baseline_db"] = deltas
            print(f"{name}: {json.dumps(deltas)}", flush=True)
        results["variants"][name] = entry

    generation = put_from_original(original, {})
    print(f"restore PUT staged (applies at next audio): gen {generation}",
          flush=True)
    # One short playback to carry the restore over a block boundary.
    capture_tones(generation)
    print("restore applied", flush=True)

    with open(os.path.join(output, "results.json"), "w") as handle:
        json.dump(results, handle, indent=1)
    print("matrix round complete", flush=True)


if __name__ == "__main__":
    main()
