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
| `lib/aius_codec_api/` | Portable C ABI (`aius_arena.h`, `aius_codec.h`) + the arena allocator. |
| `lib/aius_iec61937/` | IEC 61937 data-burst parser. |
| `lib/aius_dts/` | `AiusDtsDecoder` adapter over the vendored DTS Core + the arena alloc shim. |
| `lib/aius_pipeline/` | PCM/DTS classifier + parser→decoder end-to-end chaining. |
| `lib/aius_dsp/` | 5.1→stereo downmix + biquad EQ output DSP. |
| `lib/aius_capability/` | USB capability wire protocol + arena-driven device handler. |
| `vendor/dcadec-core/` | Minimal `dcadec` fixed-point DTS Core (LGPL 2.1, verbatim; see its `NOTICE.md`). |
| `src/` | Teensy entry point (`main.cpp`) — the P4 decode-footprint harness. |
| `tools/device/` | Simulated device + host runner, including the P6 hidraw transport. |
| `tools/dts_vectors/`, `tools/iec_corpus/` | Committed DTS/IEC test corpora + generators. |
| `tools/dcadec_ref/` | dcadec-reference generator for the LFE bit-exactness suite. |
| `tools/memory/` | Linker-map → per-bank memory report. |
| `test/` | Host test suites (one directory per suite). |
| `scripts/test.sh` | Native (host, gcc) test runner — the firmware's test entry point. |
| `scripts/build.sh` | Teensy 4.1 cross-build (`pio run`) + per-bank memory report. |
| `scripts/run_hid.sh` | Host P6 proof over Teensy RawHID (`/dev/hidrawN`). |
| `scripts/flash_and_run.sh` | Bounded flash, re-enumeration wait, and P6 proof. |
| `platformio.ini` | Teensy 4.1 cross-build environments. |

## Running host tests

Requires `gcc`. From anywhere, `scripts/test.sh --environment native --suite <name>`.
Suites (all build with `-Wall -Wextra -Werror` + ASan/UBSan):

| Suite | Covers |
| --- | --- |
| `arena`, `codec-api-contract` | The allocator and ABI contract. |
| `dts` | DTS Core decode differential vs FFmpeg (committed corpus). |
| `dts-arena` | No-heap arena decode path + allocation metrics. |
| `dts-fuzz` | DTS decoder robustness fuzz. |
| `dts-lfe` | LFE bit-exactness vs a full dcadec build. |
| `iec61937`, `iec61937-fuzz` | IEC 61937 parser known-answer + fuzz. |
| `pipeline` | Classifier + parser→decoder end-to-end. |
| `capability-protocol`, `capability` | USB protocol goldens + sim device round-trip. |
| `dsp` | Downmix + EQ known-answer. |

The `dts`/`dts-lfe` suites run against committed reference PCM, so `ffmpeg` /
`dcadec` are needed only to regenerate the corpora, not to run the suites. This
firmware's own `scripts/test.sh` is the test entry point; the repo-root
`scripts/test.sh` is a .NET/vitest/pytest discovery runner and does not drive the
C suites.

Cross-build for the Teensy with `scripts/build.sh` (needs PlatformIO; see its
header for the pinned-venv recipe).

## Status

Everything that can be done without the physical Teensy is done and green on the
host: the portable ABI + arena, the IEC 61937 parser (P2), the `dcadec` DTS Core
decoder bit-exact to a full dcadec build including LFE (P3, no-heap arena path),
the reproducible Teensy cross-build + per-bank memory report (P4, all banks fit),
the USB capability protocol + simulated device + host runner (P5), the PCM/DTS
classifier + end-to-end pipeline, and the 5.1→stereo downmix + EQ output DSP.

The P6 target entry point is `teensy41-capability`, using Teensy RawHID with the
same framed protocol and device handler as the host simulator. Build it with
`scripts/build.sh teensy41-capability`, flash it with PlatformIO/Teensy loader,
then run `scripts/flash_and_run.sh .pio/build/teensy41-capability/firmware.hex \
tools/dts_vectors/vectors`. The wrapper requests the running image to enter
HalfKay, flashes it, resolves the new application interface by VID/PID and
interface number, and writes a raw transcript unless an audit path is supplied.
Large RawHID commands use acknowledged 32-byte fragments because this USB
extender corrupts longer logical reports. The physical P6 corpus passes all 40
vectors; P7 soak remains.

Set `AIUS_HID_ITERATIONS` when invoking `scripts/run_hid.sh` to repeat the full
corpus in one device session; use `/dev/null` as the audit path for a long soak.
