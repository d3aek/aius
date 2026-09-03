"""Unit tests for the gate-5 stimulus generator.

Drives the public functions of tools/ac3_vectors/gen_iec_stream.py: the WAV
wrapping at both link rates, the IEC 61937 burst scan over synthetic periods,
the refusals that keep a silently-wrong stream off the U24 (odd byte count,
truncated final period, wrong data type), and the CLI exit codes. The one
FFmpeg-backed case generates a 1 s 44.1 kHz stream and checks its burst count
and period; it skips cleanly when ffmpeg is absent so the suite stays runnable
on a host without it.
"""

import contextlib
import importlib.util
import io
import math
import pathlib
import shutil
import struct
import sys
import tempfile
import unittest
import wave

ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE = ROOT / "tools" / "ac3_vectors" / "gen_iec_stream.py"
SPEC = importlib.util.spec_from_file_location("gen_iec_stream", MODULE)
GEN = importlib.util.module_from_spec(SPEC)
# Registered before execution: the module's dataclasses resolve their deferred
# (PEP 563) annotations through sys.modules[__module__].
sys.modules[SPEC.name] = GEN
SPEC.loader.exec_module(GEN)

FFMPEG = shutil.which("ffmpeg")


def synthetic_burst(data_type=GEN.AC3_DATA_TYPE, payload_bytes=1792):
    """One repetition period: Pa Pb Pc Pd then payload then zero stuffing."""
    header = struct.pack("<HHHH", GEN.PA, GEN.PB, data_type, payload_bytes * 8)
    body = bytes((index * 7 + 3) & 0xFF for index in range(payload_bytes))
    period = header + body
    return period + bytes(GEN.BURST_BYTES - len(period))


def synthetic_stream(bursts, **kwargs):
    return b"".join(synthetic_burst(**kwargs) for _ in range(bursts))


class WavWrapTests(unittest.TestCase):
    def check_header(self, rate):
        payload = synthetic_stream(3)
        with tempfile.TemporaryDirectory() as scratch:
            path = pathlib.Path(scratch) / f"stream-{rate}.wav"
            GEN.write_wav(path, rate, payload)
            raw = path.read_bytes()
            with wave.open(str(path), "rb") as wav:
                self.assertEqual(wav.getnchannels(), 2)
                self.assertEqual(wav.getsampwidth(), 2)
                self.assertEqual(wav.getframerate(), rate)
                self.assertEqual(wav.getnframes(), 3 * GEN.AU_FRAMES)
            read_rate, read_payload = GEN.read_wav(path)
        self.assertEqual(raw[:4], b"RIFF")
        self.assertEqual(raw[8:12], b"WAVE")
        # fmt chunk: PCM (1), 2 channels, rate, byte rate, block align 4, 16 bits.
        self.assertEqual(struct.unpack("<HHIIHH", raw[20:36]),
                         (1, 2, rate, rate * 4, 4, 16))
        self.assertEqual(read_rate, rate)
        # The wire words ride unchanged: this is what makes aplay bit-exact.
        self.assertEqual(read_payload, payload)

    def test_write_wav_44100_header_fields(self):
        self.check_header(44100)

    def test_write_wav_48000_header_fields(self):
        self.check_header(48000)

    def test_write_wav_refuses_odd_byte_count(self):
        payload = synthetic_stream(2) + b"\x00"
        with tempfile.TemporaryDirectory() as scratch:
            path = pathlib.Path(scratch) / "odd.wav"
            with self.assertRaises(GEN.StreamError) as context:
                GEN.write_wav(path, 44100, payload)
            self.assertIn("odd", str(context.exception))
            self.assertFalse(path.exists())

    def test_write_wav_refuses_truncated_final_period(self):
        payload = synthetic_stream(2)[:-2]
        with tempfile.TemporaryDirectory() as scratch:
            path = pathlib.Path(scratch) / "short.wav"
            with self.assertRaises(GEN.StreamError) as context:
                GEN.write_wav(path, 48000, payload)
            self.assertIn("truncated", str(context.exception))
            self.assertFalse(path.exists())

    def test_check_stream_refuses_empty(self):
        with self.assertRaises(GEN.StreamError):
            GEN.check_stream(b"")


class BurstScanTests(unittest.TestCase):
    def test_find_bursts_reads_type_and_length_code(self):
        bursts = GEN.find_bursts(synthetic_stream(4, payload_bytes=1394))
        self.assertEqual([burst.offset for burst in bursts],
                         [index * GEN.BURST_BYTES for index in range(4)])
        self.assertEqual({burst.data_type for burst in bursts}, {1})
        self.assertEqual({burst.length_code for burst in bursts}, {1394 * 8})

    def test_summarize_reports_single_period(self):
        summary = GEN.summarize(synthetic_stream(5))
        self.assertEqual(summary.bursts, 5)
        self.assertEqual(summary.periods, (GEN.BURST_BYTES,))
        self.assertEqual(summary.data_types, (1,))
        GEN.check_ac3_bursts(summary)

    def test_check_ac3_bursts_refuses_wrong_data_type(self):
        summary = GEN.summarize(synthetic_stream(3, data_type=11))
        with self.assertRaises(GEN.StreamError) as context:
            GEN.check_ac3_bursts(summary)
        self.assertIn("data types", str(context.exception))

    def test_check_ac3_bursts_refuses_missing_burst(self):
        # A period whose preamble was zeroed: one fewer burst than periods.
        stream = synthetic_stream(3)
        blanked = stream[:GEN.BURST_BYTES] + bytes(8) + stream[GEN.BURST_BYTES + 8:]
        with self.assertRaises(GEN.StreamError):
            GEN.check_ac3_bursts(GEN.summarize(blanked))

    def test_check_ac3_bursts_refuses_no_preamble(self):
        with self.assertRaises(GEN.StreamError) as context:
            GEN.check_ac3_bursts(GEN.summarize(bytes(GEN.BURST_BYTES)))
        self.assertIn("no IEC 61937 preamble", str(context.exception))


def quiet_main(argv):
    """Run the CLI with its stdout/stderr captured; returns (code, stderr)."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = GEN.main(argv)
    return code, err.getvalue()


class CliTests(unittest.TestCase):
    def test_usage_error_exits_4(self):
        for argv in (["--rate", "22050", "--output", "x.wav"],
                     ["--rate", "44100"],
                     ["--seconds", "0", "--output", "x.wav"]):
            with self.subTest(argv=argv), \
                    contextlib.redirect_stderr(io.StringIO()), \
                    self.assertRaises(SystemExit) as context:
                GEN.main(argv)
            self.assertEqual(context.exception.code, GEN.EXIT_USAGE)

    def test_missing_ffmpeg_exits_3(self):
        with tempfile.TemporaryDirectory() as scratch:
            output = pathlib.Path(scratch) / "never.wav"
            code, err = quiet_main(
                ["--output", str(output), "--ffmpeg",
                 str(pathlib.Path(scratch) / "no-such-ffmpeg")])
            self.assertEqual(code, GEN.EXIT_FFMPEG_MISSING)
            self.assertIn("ffmpeg not found", err)
            self.assertFalse(output.exists())

    def test_inspect_refuses_non_link_wav(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = pathlib.Path(scratch) / "mono.wav"
            with wave.open(str(path), "wb") as out:
                out.setnchannels(1)
                out.setsampwidth(2)
                out.setframerate(48000)
                out.writeframes(bytes(GEN.BURST_BYTES))
            code, err = quiet_main(["--inspect", str(path)])
        self.assertEqual(code, GEN.EXIT_STREAM_REFUSED)
        self.assertIn("not 2-channel 16-bit PCM", err)

    def test_inspect_accepts_synthetic_stream(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = pathlib.Path(scratch) / "ok.wav"
            GEN.write_wav(path, 44100, synthetic_stream(2))
            code, err = quiet_main(["--inspect", str(path)])
        self.assertEqual((code, err), (0, ""))


@unittest.skipUnless(FFMPEG, "ffmpeg not on PATH")
class FfmpegTests(unittest.TestCase):
    def test_generated_1s_44100_stream_bursts_and_period(self):
        with tempfile.TemporaryDirectory() as scratch:
            output = pathlib.Path(scratch) / "one-second.wav"
            code, err = quiet_main(["--rate", "44100", "--seconds", "1",
                                    "--output", str(output),
                                    "--keep-intermediates", "--ffmpeg", FFMPEG])
            self.assertEqual((code, err), (0, ""))
            rate, payload = GEN.read_wav(output)
            raw_ac3 = output.with_suffix(".ac3").read_bytes()
            self.assertEqual(output.with_suffix(".iec").read_bytes(), payload)
        self.assertEqual(rate, 44100)
        summary = GEN.summarize(payload)
        expected = math.ceil(44100 / GEN.AU_FRAMES)
        self.assertLessEqual(abs(summary.bursts - expected), 1)
        self.assertEqual(summary.bursts, len(payload) // GEN.BURST_BYTES)
        self.assertEqual(summary.periods, (GEN.BURST_BYTES,))
        self.assertEqual(summary.data_types, (GEN.AC3_DATA_TYPE,))
        # Pd is the payload length in bits; the bursts carry the whole raw
        # elementary stream and nothing else.
        self.assertEqual(sum(burst.length_code for burst in
                             GEN.find_bursts(payload)), len(raw_ac3) * 8)
        self.assertEqual(raw_ac3[:2], b"\x0b\x77")


if __name__ == "__main__":
    unittest.main()
