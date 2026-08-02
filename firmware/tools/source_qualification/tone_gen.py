#!/usr/bin/env python3
"""Generate the 997 Hz +/-410 identical-channel S16 stereo bracket tone.

Matches the CM6206 direct-loop bracket source (peak_abs 410, identical
channels) so results from any qualified source are directly comparable
with the characterization evidence table.
"""

from __future__ import annotations

import argparse
import math
import wave
from pathlib import Path

import numpy as np


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rate", type=int, required=True)
    parser.add_argument("--seconds", type=int, default=165)
    parser.add_argument("--freq", type=float, default=997.0)
    parser.add_argument("--amplitude", type=int, default=410)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    frames = args.rate * args.seconds
    n = np.arange(frames, dtype=np.float64)
    mono = np.rint(
        args.amplitude * np.sin(2 * math.pi * args.freq * n / args.rate)
    ).astype("<i2")
    stereo = np.repeat(mono[:, None], 2, axis=1)
    with wave.open(str(args.output), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(args.rate)
        out.writeframes(stereo.tobytes())
    print(f"{args.output}: {frames} frames at {args.rate} Hz, "
          f"peak {int(np.abs(stereo).max())}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
