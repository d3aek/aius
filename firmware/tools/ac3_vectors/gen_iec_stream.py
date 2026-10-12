#!/usr/bin/env python3
"""Generate a WAV-wrapped IEC 61937 AC-3 stream for the gate-5 encoded-input run.

Gate 5 of aius-sample-rate-following.md (lines 596-598) feeds encoded 44.1 kHz
AC-3 INTO the board's S/PDIF receiver and requires carrier, decoder and output
nominal rates to agree with no mismatch or refusal. The board only produces
output, so the stimulus has to come from the host: this script encodes
a known tone programme with FFmpeg's native `ac3` encoder, muxes it with
FFmpeg's `spdif` muxer exactly as tools/latency/bench.py does (`-c:a ac3`, then
`-f spdif`), and wraps the resulting 16-bit little-endian wire words as a
2-channel 16-bit PCM WAV at the chosen rate. IEC 61937 rides a PCM link as
ordinary sample words, so `aplay -D hw:0,0 <wav>` on the ESI U24 XL emits the
bitstream bit-exactly. The AC-3 corpus's `generate.sh` is the model for the
tone recipes (explicit `join ... map=` on every multi-channel recipe).

UNITY GAIN IS A PRECONDITION. The U24 XL resets its PCM playback volume to
70/70 (-20 dB) on every USB re-enumeration, and a -20 dB digital output
silently destroys IEC 61937 sync arithmetic while passing every
amplitude-normalized PCM check; the failure then looks like a
codec/classification fault (tools/source_qualification/README.md:37-42).
Re-apply `amixer -c 0 cset numid=5 110,110` before every playback of this
stream, and re-verify it after any cable blink.

Usage:
    gen_iec_stream.py --rate 44100 --output gate5-ac3-44100.wav
    gen_iec_stream.py --rate 48000 --seconds 10 --program surround51_ident \\
        --output smoke-48000.wav --keep-intermediates
    gen_iec_stream.py --inspect gate5-ac3-44100.wav

Env:
    FFMPEG    ffmpeg binary (default: ffmpeg; `--ffmpeg` overrides)

Exit codes: 0 success; 1 ffmpeg ran and failed; 2 the produced stream is
refused (odd byte count, not whole 6144-byte bursts, or no AC-3 bursts found);
3 ffmpeg missing; 4 usage error.
"""

from __future__ import annotations

import argparse
import dataclasses
import math
import os
import shutil
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

RATES = (44100, 48000)
PROGRAMS = ("tone", "surround51_ident")

# One AC-3 access unit is 1536 frames; on a 16-bit stereo link that is 1536 x 4
# bytes per IEC 61937 repetition period at every AC-3 sample rate.
AU_FRAMES = 1536
LINK_BYTES_PER_FRAME = 4
BURST_BYTES = AU_FRAMES * LINK_BYTES_PER_FRAME
PA = 0xF872
PB = 0x4E1F
AC3_DATA_TYPE = 1

# Bitrates pinned as in generate.sh (stereo) and bench.py / iec_fixtures
# (5.1) so the stream does not drift with FFmpeg's defaults.
BITRATE = {"tone": "192k", "surround51_ident": "448k"}
# The channel-identity programme of the corpus's surround51_ident vector: FL
# 300, FR 600, FC 900, LFE 60, BL 1200, BR 1500 Hz, each on the channel it
# names.
IDENT_HZ = (300, 600, 900, 60, 1200, 1500)
IDENT_MAP = "0.0-FL|1.0-FR|2.0-FC|3.0-LFE|4.0-BL|5.0-BR"
VOLUME = 0.6

EXIT_FFMPEG_FAILED = 1
EXIT_STREAM_REFUSED = 2
EXIT_FFMPEG_MISSING = 3
EXIT_USAGE = 4


class StreamError(Exception):
    """The produced byte stream is not a playable IEC 61937 AC-3 stream."""


@dataclasses.dataclass(frozen=True)
class Burst:
    offset: int
    data_type: int
    length_code: int


@dataclasses.dataclass(frozen=True)
class Summary:
    total_bytes: int
    bursts: int
    periods: tuple[int, ...]
    data_types: tuple[int, ...]


def check_stream(payload: bytes) -> None:
    """Refuse a stream that would play wrong without erroring."""
    if not payload:
        raise StreamError("stream is empty")
    if len(payload) % 2:
        raise StreamError(f"stream is {len(payload)} bytes, an odd count: "
                          "the 16-bit link words would be misaligned")
    if len(payload) % BURST_BYTES:
        raise StreamError(f"stream is {len(payload)} bytes, not whole "
                          f"{BURST_BYTES}-byte bursts: the final period is "
                          "truncated")


def find_bursts(payload: bytes) -> list[Burst]:
    """Every Pa/Pb preamble on a 16-bit word boundary, with its Pc type and Pd."""
    bursts = []
    limit = len(payload) - 8
    offset = 0
    while offset <= limit:
        if (int.from_bytes(payload[offset:offset + 2], "little") == PA and
                int.from_bytes(payload[offset + 2:offset + 4], "little") == PB):
            pc = int.from_bytes(payload[offset + 4:offset + 6], "little")
            pd = int.from_bytes(payload[offset + 6:offset + 8], "little")
            bursts.append(Burst(offset, pc & 0x1F, pd))
            offset += 8
            continue
        offset += 2
    return bursts


def summarize(payload: bytes) -> Summary:
    bursts = find_bursts(payload)
    periods = tuple(sorted({later.offset - earlier.offset
                            for earlier, later in zip(bursts, bursts[1:])}))
    return Summary(len(payload), len(bursts), periods,
                   tuple(sorted({burst.data_type for burst in bursts})))


def check_ac3_bursts(summary: Summary) -> None:
    if summary.bursts == 0:
        raise StreamError("no IEC 61937 preamble found")
    if summary.data_types != (AC3_DATA_TYPE,):
        raise StreamError(f"burst data types {summary.data_types}, expected "
                          f"({AC3_DATA_TYPE},)")
    if summary.bursts > 1 and summary.periods != (BURST_BYTES,):
        raise StreamError(f"burst periods {summary.periods}, expected "
                          f"({BURST_BYTES},)")
    if summary.bursts != summary.total_bytes // BURST_BYTES:
        raise StreamError(f"{summary.bursts} bursts in "
                          f"{summary.total_bytes // BURST_BYTES} periods")


def write_wav(path: Path, rate: int, payload: bytes) -> None:
    """Wrap the wire words as 2-channel 16-bit PCM; refuses a misaligned stream."""
    check_stream(payload)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(payload)


def read_wav(path: Path) -> tuple[int, bytes]:
    """The WAV's rate and its raw data bytes; refuses a non-link-shaped WAV."""
    with wave.open(str(path), "rb") as source:
        if source.getnchannels() != 2 or source.getsampwidth() != 2:
            raise StreamError(f"{path}: not 2-channel 16-bit PCM")
        return source.getframerate(), source.readframes(source.getnframes())


def encode_command(ffmpeg: str, rate: int, seconds: float, program: str,
                   tone_hz: float, ac3_path: Path) -> list[str]:
    """The FFmpeg encode, mirroring generate.sh's recipes at the chosen rate."""
    if program == "tone":
        frequencies = (tone_hz, tone_hz)
        layout, channel_map = "stereo", "0.0-FL|1.0-FR"
    else:
        frequencies = IDENT_HZ
        layout, channel_map = "5.1", IDENT_MAP
    command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y"]
    for frequency in frequencies:
        command += ["-f", "lavfi", "-i",
                    f"sine=frequency={frequency}:duration={seconds}"
                    f":sample_rate={rate}"]
    labels = "".join(f"[{index}]" for index in range(len(frequencies)))
    command += [
        "-filter_complex",
        f"{labels}join=inputs={len(frequencies)}:channel_layout={layout}"
        f":map={channel_map},volume={VOLUME}[a]",
        "-map", "[a]", "-ar", str(rate), "-c:a", "ac3", "-b:a", BITRATE[program],
        "-f", "ac3", str(ac3_path),
    ]
    return command


def wrap_command(ffmpeg: str, ac3_path: Path, spdif_path: Path) -> list[str]:
    """The spdif mux, verbatim from tools/latency/bench.py."""
    return [ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
            "-i", str(ac3_path), "-map", "0:a", "-c:a", "copy",
            "-f", "spdif", str(spdif_path)]


def run_ffmpeg(command: list[str]) -> None:
    completed = subprocess.run(command, check=False,
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, text=True)
    if completed.returncode != 0:
        raise RuntimeError(f"{' '.join(command[:1])} exited "
                           f"{completed.returncode}: {completed.stderr.strip()}")


def generate(ffmpeg: str, rate: int, seconds: float, program: str,
             tone_hz: float, output: Path,
             keep_intermediates: bool) -> Summary:
    """Encode, mux and wrap; returns the burst summary of the written stream."""
    with tempfile.TemporaryDirectory(prefix="gen_iec_stream.") as scratch:
        ac3_path = Path(scratch) / "stream.ac3"
        spdif_path = Path(scratch) / "stream.iec"
        run_ffmpeg(encode_command(ffmpeg, rate, seconds, program, tone_hz,
                                  ac3_path))
        run_ffmpeg(wrap_command(ffmpeg, ac3_path, spdif_path))
        payload = spdif_path.read_bytes()
        summary = summarize(payload)
        check_ac3_bursts(summary)
        write_wav(output, rate, payload)
        if keep_intermediates:
            shutil.copyfile(ac3_path, output.with_suffix(".ac3"))
            shutil.copyfile(spdif_path, output.with_suffix(".iec"))
    return summary


def describe(summary: Summary, rate: int) -> str:
    seconds = summary.bursts * AU_FRAMES / rate
    period = "/".join(str(value) for value in summary.periods) or "n/a"
    return (f"{summary.total_bytes} bytes, {summary.bursts} bursts of type "
            f"{'/'.join(str(value) for value in summary.data_types) or 'none'}, "
            f"period {period} bytes, {seconds:.3f} s at {rate} Hz")


class Parser(argparse.ArgumentParser):
    def error(self, message: str) -> None:  # noqa: D401 - argparse hook
        self.print_usage(sys.stderr)
        print(f"{self.prog}: error: {message}", file=sys.stderr)
        raise SystemExit(EXIT_USAGE)


def make_parser() -> Parser:
    parser = Parser(description="WAV-wrapped IEC 61937 AC-3 stimulus generator")
    parser.add_argument("--rate", type=int, choices=RATES, default=44100,
                        help="link and AC-3 sample rate (default 44100)")
    parser.add_argument("--seconds", type=float, default=130.0,
                        help="programme length; the encoder pads to whole "
                             "access units (default 130)")
    parser.add_argument("--program", choices=PROGRAMS, default="tone",
                        help="stereo tone, or the corpus's 5.1 channel-identity "
                             "programme (default tone)")
    parser.add_argument("--tone-hz", type=float, default=997.0,
                        help="stereo tone frequency (default 997)")
    parser.add_argument("--output", type=Path,
                        help="WAV to write (required unless --inspect)")
    parser.add_argument("--keep-intermediates", action="store_true",
                        help="also write <output>.ac3 (raw AC-3) and "
                             "<output>.iec (raw spdif words) beside the WAV")
    parser.add_argument("--inspect", type=Path, metavar="WAV",
                        help="summarize an existing stream instead of generating")
    parser.add_argument("--ffmpeg", default=os.environ.get("FFMPEG", "ffmpeg"))
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = make_parser()
    args = parser.parse_args(argv)
    if args.inspect is not None:
        try:
            rate, payload = read_wav(args.inspect)
            check_stream(payload)
            summary = summarize(payload)
            check_ac3_bursts(summary)
        except (StreamError, wave.Error, OSError) as error:
            print(f"error: {error}", file=sys.stderr)
            return EXIT_STREAM_REFUSED
        print(f"{args.inspect}: {describe(summary, rate)}")
        return 0
    if args.output is None:
        parser.error("--output is required")
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    if shutil.which(args.ffmpeg) is None:
        print(f"error: ffmpeg not found at {args.ffmpeg!r} (set FFMPEG=... or "
              "--ffmpeg)", file=sys.stderr)
        return EXIT_FFMPEG_MISSING
    try:
        summary = generate(args.ffmpeg, args.rate, args.seconds, args.program,
                           args.tone_hz, args.output, args.keep_intermediates)
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        return EXIT_FFMPEG_FAILED
    except StreamError as error:
        print(f"error: refusing to write {args.output}: {error}",
              file=sys.stderr)
        return EXIT_STREAM_REFUSED
    expected = math.ceil(args.seconds * args.rate / AU_FRAMES)
    print(f"{args.output}: {describe(summary, args.rate)} "
          f"(requested {args.seconds} s ~ {expected} access units)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
