# Aius firmware

The Teensy 4.1 capability firmware and its portable, host-testable codec code.
See the [capability gate](../docs/plans/aius-capability-gate.md) for the phased
plan and the measured pass/fail budgets.

Open this directory in the VS Code PlatformIO extension. Codec, parser, DSP, and
measurement code stays behind portable C-compatible interfaces with no Arduino
dependency, so the identical source runs in host tests and on the Teensy.

## Layout

| Path | Purpose |
| --- | --- |
| `lib/aius_codec_api/include/` | Portable C ABI: `aius_arena.h`, `aius_codec.h`. |
| `lib/aius_codec_api/src/` | Portable implementations (arena; parser lands at P2). |
| `lib/aius_dts/` | `AiusDtsDecoder` adapter over the vendored DTS Core. |
| `vendor/dcadec-core/` | Minimal `dcadec` fixed-point DTS Core (LGPL 2.1, verbatim; see its `NOTICE.md`). |
| `tools/dts_vectors/` | Deterministic DTS Core vector generator (ffmpeg) + committed corpus. |
| `test/` | Host test suites (one directory per suite). |
| `scripts/test.sh` | Native (host, gcc) test runner. |
| `platformio.ini` | Teensy 4.1 cross-build environment. |

## Running host tests

Requires `gcc`. From anywhere:

```bash
firmware/scripts/test.sh --environment native --suite arena
firmware/scripts/test.sh --environment native --suite codec-api-contract
firmware/scripts/test.sh --environment native --suite dts
```

The `dts` suite is a differential test: it decodes the committed DTS Core corpus
with `AiusDtsDecoder` and compares against FFmpeg's decode of the same bitstream.
Regenerating the corpus (`tools/dts_vectors/generate.sh`) needs `ffmpeg`; running
the suite does not.

Suites build with `-Wall -Wextra -Werror` and ASan/UBSan. `scripts/test.sh
Aius.Firmware` becomes the repository entry point once the Teensy target build
lands (capability-gate P4).

## Status

Capability-gate P1 + first P3 milestone. Landed: the portable ABI headers, a
tested arena allocator, the native harness, and the `dcadec` DTS Core decoder
decoding real bitstreams on the host, bit-exact to FFmpeg on every full-band
channel (the LFE, which each decoder reconstructs with its own interpolation FIR,
matches within a looser documented bound). Next: route decode allocation through
`AiusArena` (no-heap path), the IEC 61937 parser (P2), malformed/fuzz corpora,
and the reproducible Teensy target build (P4).
