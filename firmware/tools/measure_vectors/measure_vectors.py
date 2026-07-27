#!/usr/bin/env python3
"""PCM measurement-vector generator + validator for the measurement loopback.

The item 3 stimulus (docs/plans/aius-dsp-config-api.md, "Item 3 design"): bare
16-bit IEC words with NO IEC 61937 preamble, so the pipeline classifies
AIUS_PIPELINE_PCM and passes the samples through bit-exactly at 24-bit scale.
The DTS vectors cannot support the figures (the dca encode carries a -78.7 dB
floor of its own and loops with a phase seam); these are the seam-free
replacement, generated loop-coherent so the replayed stream is a continuous
tone with no discontinuity anywhere -- which is what makes any 8192-sample
capture window seam-free by construction.

THE TWO GENERATOR OBLIGATIONS, asserted at generation time rather than assumed
of tone data, and re-assertable against the committed bytes (the `validate`
subcommand, which the `measure` suite runs):

  1. The first AIUS_PIPELINE_CLASSIFY_MAX_WORDS (512) words are VERIFIED free
     of the IEC 61937 Pa/Pb preamble pair in either wire byte order -- a tone
     sample stream can produce it by coincidence. (Validated over the whole
     file here: classification runs only over the head, but the check is free
     and a preamble anywhere in tone data means the generator misbehaved.)
  2. Coherence is per VECTOR, not per tone: every tone must complete an
     integer cycle count over the file, so 440 Hz alone is coherent over
     multiples of 1,200 samples but an L 440 / R 660 split is coherent only
     over multiples of 2,400 (the lcm). Every vector here is 12,000 frames,
     a common multiple of every tone period used, and every tone frequency is
     a multiple of 4 Hz (= 48,000 / 12,000).

File format: interleaved L,R int16 little-endian words at 48 kHz -- exactly
what PUT /sd/replays/<id> stores and the replay service feeds the S/PDIF
transmit path. Levels are dBFS re the 16-bit full scale (amplitude 32768
convention); the pipeline's left-align shift preserves them at 24-bit scale
to within 0.0003 dB.

Regeneration is deterministic on one host but sin() is libm's, so committed
bytes are the provenance record and validation checks OBLIGATIONS, not byte
equality (a 0.5-ulp libm difference could flip a rounding-boundary sample on
another host without violating any obligation).

Usage:
  tools/measure_vectors/measure_vectors.py generate   # rewrite ./vectors
  tools/measure_vectors/measure_vectors.py validate   # check committed bytes

Exit: 0 ok; 1 an obligation or tolerance failed; 2 usage.
"""

import cmath
import math
import struct
import sys
from pathlib import Path

FS = 48000
FRAMES = 12000  # 250 ms per loop; every tone below is a multiple of 4 Hz
CLASSIFY_WORDS = 512  # AIUS_PIPELINE_CLASSIFY_MAX_WORDS
PA, PB = 0xF872, 0x4E1F
PA_SW, PB_SW = 0x72F8, 0x1F4E  # byte-swapped wire order
FULL = 32768.0  # 16-bit dBFS reference amplitude
INT16_MAX = 32767

# name -> (per-channel tone lists [(freq_hz, amplitude)], detail)
# Amplitudes: 16384 = -6.02 dBFS, 8192 = -12.04, 4096 = -18.06, 32767 = -0.0003.
_M6 = [(440, 16384.0)]
VECTORS = {
    "pcm440_m6": (
        {"l": _M6, "r": _M6},
        "the baseline THD+N/SNR vector; 110 cycles per loop",
    ),
    "pcm440_full": (
        {"l": [(440, 32767.0)], "r": [(440, 32767.0)]},
        "the limiter-hot vector -- pair with a config that opens the limiter "
        "(shipped headroom 0.5 alone leaves the tap at -6 dBFS, below the "
        "-1 dBFS threshold)",
    ),
    "pcm440_660_split": (
        {"l": _M6, "r": [(660, 8192.0)]},
        "per-channel fit split; loop-coherent at lcm(1200, 800) = 2400",
    ),
    "pcm_fr_multitone": (
        {
            "l": [(f, 4096.0) for f in (40, 100, 440, 1000, 4000, 10000, 16000)],
            "r": [(f, 4096.0) for f in (40, 100, 440, 1000, 4000, 10000, 16000)],
        },
        "frequency-response probes at -18 dBFS each; worst-case sum 28,672",
    ),
}


def synth_channel(tones):
    """Round-half-away-from-zero synthesis, matching aius_quantize's tie rule."""
    samples = []
    for i in range(FRAMES):
        x = sum(a * math.sin(2.0 * math.pi * f * i / FS) for f, a in tones)
        r = x - 0.5 if x < 0.0 else x + 0.5
        samples.append(max(-32768, min(INT16_MAX, int(r))))
    return samples


def synth(tones_by_ch):
    left = synth_channel(tones_by_ch["l"])
    right = synth_channel(tones_by_ch["r"])
    words = [w for pair in zip(left, right) for w in pair]
    return struct.pack(f"<{len(words)}h", *words)


def check(name, cond, message):
    if not cond:
        print(f"FAIL {name}: {message}", file=sys.stderr)
        return 1
    return 0


def validate_vector(name, data, tones_by_ch, quiet=False):
    """Assert both generator obligations plus tone content on raw file bytes."""
    failures = 0
    failures += check(name, len(data) == FRAMES * 4,
                      f"length {len(data)} != {FRAMES} frames x 4 bytes")
    if failures:
        return failures

    words = struct.unpack(f"<{FRAMES * 2}H", data)
    # Obligation 1: no Pa/Pb pair, either byte order, anywhere (the classifier
    # only reads the first CLASSIFY_WORDS, but tone data has no business
    # containing one at all).
    for i in range(len(words) - 1):
        pair = (words[i], words[i + 1])
        if pair in ((PA, PB), (PA_SW, PB_SW)):
            failures += check(
                name, False,
                f"IEC 61937 preamble pair at word {i}"
                + (" (inside the classify window!)" if i + 1 < CLASSIFY_WORDS
                   else ""))
            break

    signed = struct.unpack(f"<{FRAMES * 2}h", data)
    for ch, offset in (("l", 0), ("r", 1)):
        tones = tones_by_ch[ch]
        chan = signed[offset::2]
        # Obligation 2: per-vector coherence -- integer cycles for every tone.
        for f, _ in tones:
            cycles = f * FRAMES / FS
            failures += check(name, cycles == int(cycles),
                              f"{ch}: {f} Hz is not loop-coherent over "
                              f"{FRAMES} frames ({cycles} cycles)")

        # Tone content: coherent rectangular DFT at an exact bin is exact, so
        # each declared tone must read back within 0.05 dB and the residual
        # after removing declared tones must be 16-bit quantization only
        # (< -90 dB re full scale; the floor itself sits near -98).
        total_power = sum(s * s for s in chan) / FRAMES
        tone_power = 0.0
        for f, amp in tones:
            k = f * FRAMES // FS
            x = sum(s * cmath.exp(-2j * math.pi * k * i / FRAMES)
                    for i, s in enumerate(chan))
            est = 2.0 * abs(x) / FRAMES
            err_db = abs(20.0 * math.log10(est / amp))
            failures += check(name, err_db <= 0.05,
                              f"{ch}: {f} Hz reads {est:.1f} vs declared "
                              f"{amp:.1f} ({err_db:.3f} dB off)")
            tone_power += est * est / 2.0
        dc = sum(chan) / FRAMES
        residual = max(total_power - tone_power - dc * dc, 0.0)
        residual_db = (10.0 * math.log10(residual / (FULL * FULL / 2.0))
                       if residual > 0.0 else -200.0)
        failures += check(name, residual_db < -90.0,
                          f"{ch}: non-tone residual {residual_db:.1f} dB "
                          "re full scale -- not a pure declared-tone vector")
    if not failures and not quiet:
        print(f"ok {name}")
    return failures


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("generate", "validate"):
        print(__doc__.strip().splitlines()[-4].strip(), file=sys.stderr)
        print(__doc__.strip().splitlines()[-3].strip(), file=sys.stderr)
        return 2
    out_dir = Path(__file__).resolve().parent / "vectors"
    failures = 0

    if sys.argv[1] == "generate":
        out_dir.mkdir(exist_ok=True)
        lines = [
            "PCM measurement-vector corpus for the measurement loopback.",
            "Columns: name | frames | tones (ch freq@amp) | detail",
            "(regenerate with tools/measure_vectors/measure_vectors.py; levels",
            " are amplitudes re 16-bit full scale 32768)",
            "",
        ]
        for name, (tones_by_ch, detail) in VECTORS.items():
            data = synth(tones_by_ch)
            failures += validate_vector(name, data, tones_by_ch, quiet=True)
            if failures:
                return 1  # never write a vector that violates an obligation
            (out_dir / f"{name}.iec").write_bytes(data)
            tones = "; ".join(
                f"{ch} " + ",".join(f"{f}@{a:.0f}" for f, a in t)
                for ch, t in tones_by_ch.items())
            lines.append(f"{name} | {FRAMES} | {tones} | {detail}")
            print(f"wrote {name}.iec")
        (out_dir / "manifest.txt").write_text("\n".join(lines) + "\n")
        print(f"wrote manifest.txt ({len(VECTORS)} vectors)")
        return 0

    for name, (tones_by_ch, _) in VECTORS.items():
        path = out_dir / f"{name}.iec"
        if not path.is_file():
            print(f"FAIL {name}: missing {path}", file=sys.stderr)
            failures += 1
            continue
        failures += validate_vector(name, path.read_bytes(), tones_by_ch)
    if failures:
        return 1
    print(f"measure vectors: all {len(VECTORS)} validated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
