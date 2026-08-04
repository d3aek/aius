#!/usr/bin/env python3
"""DSP characterization battery: commanded curve vs measured response.

Method: a bin-centered multitone (N=65536 @ 48 kHz, exact-period FFT, no
window leakage) plays through U24XL TX -> Teensy RX -> DSP -> Teensy TX ->
U24XL RX full-duplex capture. Round A (flat, limiter off) is the reference;
every curve is measured RELATIVE to it, cancelling the loop's own response.
Expected curves are RBJ-cookbook biquads at 48 kHz; measured-vs-expected
deltas are the characterization. A limiter round drives a 1 kHz tone up the
level ladder against the restored limiter. The original config is restored
(verbatim keys, fresh base_generation) at the end.

Config apply happens at the next audio block boundary, so the script starts
playback FIRST, then polls config_generation before capturing.

Environment: AIUS_SD_AUTH carries the token (the AIUS AUTH value; never
committed); AIUS_BASE_URL and AIUS_ALSA_DEVICE override the bench defaults.

Usage: AIUS_SD_AUTH=... dsp_characterize.py <output-dir>
"""

import json
import math
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
import wave

import numpy as np

BASE = os.environ.get("AIUS_BASE_URL", "http://192.0.2.66")
DEVICE = os.environ.get("AIUS_ALSA_DEVICE", "hw:U24XL,0")
FS = 48000
N = 65536
REPEATS = 8          # ~10.9 s of stimulus per measurement
SETTLE_S = 3.0       # play-start -> capture-start (latency + apply + settle)
CAPTURE_WINDOWS = 2  # capture 2*N frames, analyze the second-aligned window

TONES_HZ_NOMINAL = [
    20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400,
    500, 630, 800, 1000, 1250, 1600, 2000, 2200, 2500, 3150, 4000,
    5000, 6000, 6300, 8000, 10000, 12500, 16000, 20000,
]


def bin_center(freq_hz):
    k = max(1, round(freq_hz * N / FS))
    return k, k * FS / N


TONE_BINS = sorted({bin_center(f)[0] for f in TONES_HZ_NOMINAL})
TONE_HZ = [k * FS / N for k in TONE_BINS]


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


def put_body(config, base_generation):
    return {
        "source": "sd",
        "config_generation": config["config_generation"],
        "sample_rate_hz": config["sample_rate_hz"],
        "features": config["features"],
        "input_channels": config["input_channels"],
        "output_eq": config["output_eq"],
        "matrix": config["matrix"],
        "limiter": config["limiter"],
        "base_generation": base_generation,
    }


def apply_config(mutate):
    """GET -> mutate a deep copy -> PUT with the current generation."""
    current = api_get("/api/dsp/config")
    document = json.loads(json.dumps(current))
    mutate(document)
    body = put_body(document, current["config_generation"])
    status = api_put_config(body)
    if status != 200:
        raise RuntimeError(f"PUT refused: {status}")
    return current["config_generation"]


def wait_applied(previous_generation, deadline_s=10.0):
    """Generation increments at a block boundary -- audio must be flowing."""
    end = time.time() + deadline_s
    while time.time() < end:
        generation = api_get("/api/dsp/config")["config_generation"]
        if generation > previous_generation:
            return generation
        time.sleep(0.3)
    raise RuntimeError("config did not apply (no block boundary?)")


def multitone():
    rng = np.random.default_rng(51)
    phases = rng.uniform(0, 2 * np.pi, len(TONE_BINS))
    t = np.arange(N)
    block = np.zeros(N)
    for k, phase in zip(TONE_BINS, phases):
        block += np.sin(2 * np.pi * k * t / N + phase)
    block *= (10 ** (-12 / 20)) / np.max(np.abs(block))  # peak -12 dBFS
    return block


def write_wav(path, block, repeats):
    data = np.tile(block, repeats)
    pcm = (data * 32767.0).astype("<i2")
    stereo = np.repeat(pcm, 2)  # identical L/R
    with wave.open(path, "wb") as handle:
        handle.setnchannels(2)
        handle.setsampwidth(2)
        handle.setframerate(FS)
        handle.writeframes(stereo.tobytes())


def tone_wav(path, freq_bin, level_dbfs, seconds):
    t = np.arange(int(FS * seconds))
    signal = np.sin(2 * np.pi * freq_bin * t / N) * (10 ** (level_dbfs / 20))
    pcm = (np.clip(signal, -1, 1) * 32767.0).astype("<i2")
    with wave.open(path, "wb") as handle:
        handle.setnchannels(2)
        handle.setsampwidth(2)
        handle.setframerate(FS)
        handle.writeframes(np.repeat(pcm, 2).tobytes())


def play_and_capture(wav_path, capture_path, meters_probe=False):
    player = subprocess.Popen(
        ["aplay", "-D", DEVICE, "-q", wav_path],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    time.sleep(SETTLE_S)
    meters = None
    recorder = subprocess.Popen(
        ["arecord", "-D", DEVICE, "-f", "S16_LE", "-r", str(FS), "-c", "2",
         "-t", "raw", "-q", "-d", "4", capture_path],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if meters_probe:
        time.sleep(1.0)
        try:
            meters = api_get("/api/meters")
        except Exception:
            meters = None
    recorder.wait(timeout=20)
    player.terminate()
    player.wait(timeout=10)
    if recorder.returncode != 0:
        raise RuntimeError("arecord failed: " +
                           recorder.stderr.read().decode()[:200])
    return meters


def tone_spectrum(capture_path):
    raw = np.fromfile(capture_path, dtype="<i2")
    frames = raw.reshape(-1, 2).astype(np.float64) / 32767.0
    if len(frames) < 2 * N:
        raise RuntimeError(f"short capture: {len(frames)}")
    segment = frames[N:2 * N]  # steady-state window, exact N
    out = {}
    for channel, name in ((0, "L"), (1, "R")):
        spectrum = np.fft.rfft(segment[:, channel]) / (N / 2)
        out[name] = {k: complex(spectrum[k]) for k in TONE_BINS}
    return out


def rbj_response(sections, gain_db, freqs_hz):
    """Magnitude response (dB) of RBJ biquads + broadband gain at 48 kHz."""
    response = np.full(len(freqs_hz), float(gain_db))
    for section in sections:
        kind = section["type"]
        if kind == "bypass":
            continue
        f0 = section["frequency_hz"]
        q = section["q"]
        gain = section["gain_db"]
        w0 = 2 * np.pi * f0 / FS
        alpha = np.sin(w0) / (2 * q)
        cw = np.cos(w0)
        if kind == "peaking":
            a_lin = 10 ** (gain / 40)
            b = [1 + alpha * a_lin, -2 * cw, 1 - alpha * a_lin]
            a = [1 + alpha / a_lin, -2 * cw, 1 - alpha / a_lin]
        elif kind == "highpass":
            b = [(1 + cw) / 2, -(1 + cw), (1 + cw) / 2]
            a = [1 + alpha, -2 * cw, 1 - alpha]
        else:
            raise ValueError(kind)
        w = 2 * np.pi * np.asarray(freqs_hz) / FS
        z1 = np.exp(-1j * w)
        z2 = np.exp(-2j * w)
        h = (b[0] + b[1] * z1 + b[2] * z2) / (a[0] + a[1] * z1 + a[2] * z2)
        response += 20 * np.log10(np.abs(h))
    return response


CURVES = [
    ("peak_split", {
        "fl": {"gain_db": 0, "eq": [{"type": "peaking", "frequency_hz": 1000,
                                     "q": 1.0, "gain_db": 6}]},
        "fr": {"gain_db": 0, "eq": [{"type": "peaking", "frequency_hz": 4000,
                                     "q": 2.0, "gain_db": -6}]},
    }),
    ("notch_hiq", {
        "fl": {"gain_db": 0, "eq": [{"type": "peaking", "frequency_hz": 6000,
                                     "q": 10.0, "gain_db": -12}]},
        "fr": {"gain_db": 0, "eq": [{"type": "peaking", "frequency_hz": 250,
                                     "q": 8.0, "gain_db": -6}]},
    }),
    ("highpass", {
        "fl": {"gain_db": 0, "eq": [{"type": "highpass", "frequency_hz": 120,
                                     "q": 0.707, "gain_db": 0}]},
        "fr": {"gain_db": 0, "eq": [{"type": "highpass", "frequency_hz": 120,
                                     "q": 0.707, "gain_db": 0}]},
    }),
    ("two_section", {
        "fl": {"gain_db": 0, "eq": [
            {"type": "highpass", "frequency_hz": 80, "q": 0.707, "gain_db": 0},
            {"type": "peaking", "frequency_hz": 2200, "q": 1.2, "gain_db": 3}]},
        "fr": {"gain_db": 0, "eq": [
            {"type": "highpass", "frequency_hz": 80, "q": 0.707, "gain_db": 0},
            {"type": "peaking", "frequency_hz": 2200, "q": 1.2, "gain_db": 3}]},
    }),
    ("gain_only", {
        "fl": {"gain_db": -6, "eq": []},
        "fr": {"gain_db": 0, "eq": []},
    }),
]

LIMITER_LEVELS_DBFS = [-12, -6, -3, -1, -0.5, 0]


def main():
    output = sys.argv[1]
    os.makedirs(output, exist_ok=True)
    original = api_get("/api/dsp/config")
    with open(os.path.join(output, "original-config.json"), "w") as handle:
        json.dump(original, handle, indent=1)

    block = multitone()
    stimulus = os.path.join(output, "multitone.wav")
    write_wav(stimulus, block, REPEATS)
    results = {"tones_hz": TONE_HZ, "rounds": {}}

    def set_channels(document, channels, limiter_enabled):
        for name in document["input_channels"]:
            document["input_channels"][name] = {"gain_db": 0, "eq": []}
        for name, spec in channels.items():
            document["input_channels"][name] = spec
        document["limiter"]["enabled"] = limiter_enabled

    # Round A: flat reference, limiter off.
    generation = apply_config(lambda d: set_channels(d, {}, False))
    player = subprocess.Popen(["aplay", "-D", DEVICE, "-q", stimulus],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    wait_applied(generation)
    player.wait()
    capture = os.path.join(output, "capture-flat.raw")
    play_and_capture(stimulus, capture)
    reference = tone_spectrum(capture)
    results["rounds"]["flat"] = {
        "levels_dbfs": {
            channel: {str(k): 20 * math.log10(abs(v) + 1e-12)
                      for k, v in spectrum.items()}
            for channel, spectrum in reference.items()},
    }
    print("flat reference captured", flush=True)

    # EQ rounds, relative to the flat reference.
    for name, channels in CURVES:
        generation = apply_config(
            lambda d, c=channels: set_channels(d, c, False))
        player = subprocess.Popen(["aplay", "-D", DEVICE, "-q", stimulus],
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        wait_applied(generation)
        player.wait()
        capture = os.path.join(output, f"capture-{name}.raw")
        play_and_capture(stimulus, capture)
        spectrum = tone_spectrum(capture)
        round_out = {}
        for channel, source in (("L", "fl"), ("R", "fr")):
            spec = channels.get(source, {"gain_db": 0, "eq": []})
            expected = rbj_response(spec["eq"], spec["gain_db"], TONE_HZ)
            measured = np.array([
                20 * math.log10(abs(spectrum[channel][k]) /
                                (abs(reference[channel][k]) + 1e-12) + 1e-12)
                for k in TONE_BINS])
            error = measured - expected
            round_out[channel] = {
                "expected_db": [round(x, 3) for x in expected],
                "measured_db": [round(float(x), 3) for x in measured],
                "error_db": [round(float(x), 3) for x in error],
                "max_abs_error_db": round(float(np.max(np.abs(error))), 3),
                "rms_error_db": round(
                    float(np.sqrt(np.mean(error ** 2))), 3),
            }
        results["rounds"][name] = round_out
        print(f"{name}: L max|err| {round_out['L']['max_abs_error_db']} dB, "
              f"R max|err| {round_out['R']['max_abs_error_db']} dB",
              flush=True)

    # Limiter round: flat EQ, original limiter parameters restored.
    generation = apply_config(
        lambda d: set_channels(d, {}, original["limiter"]["enabled"]))
    tone_bin = bin_center(1000)[0]
    ladder = []
    first = True
    for level in LIMITER_LEVELS_DBFS:
        tone_path = os.path.join(output, f"tone-{level}.wav")
        tone_wav(tone_path, tone_bin, level, 8.0)
        if first:
            player = subprocess.Popen(["aplay", "-D", DEVICE, "-q", tone_path],
                                      stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL)
            wait_applied(generation)
            player.wait()
            first = False
        capture = os.path.join(output, f"capture-lim{level}.raw")
        meters = play_and_capture(tone_path, capture, meters_probe=True)
        raw = np.fromfile(capture, dtype="<i2").reshape(-1, 2) / 32767.0
        segment = raw[N:2 * N]
        rms_dbfs = [
            round(20 * math.log10(
                float(np.sqrt(np.mean(segment[:, ch] ** 2))) * math.sqrt(2)
                + 1e-12), 2)
            for ch in (0, 1)]
        entry = {"in_dbfs": level, "out_peak_equiv_dbfs": rms_dbfs}
        if meters:
            entry["meters_limiter_gr_db"] = (
                meters.get("limiter_gr_db")
                if isinstance(meters, dict) else None)
        ladder.append(entry)
        print(f"limiter {level} dBFS in -> {rms_dbfs} out", flush=True)
    results["rounds"]["limiter"] = ladder

    # Restore the original configuration verbatim (fresh generation pair).
    current = api_get("/api/dsp/config")
    document = json.loads(json.dumps(original))
    document["config_generation"] = current["config_generation"]
    status = api_put_config(put_body(document,
                                     current["config_generation"]))
    print(f"restore PUT: {status}", flush=True)

    with open(os.path.join(output, "results.json"), "w") as handle:
        json.dump(results, handle, indent=1)
    print("battery complete", flush=True)


if __name__ == "__main__":
    main()
