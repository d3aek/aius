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
| `lib/aius_codec_api/src/` | Portable implementations (arena today; parser/decoder land per phase). |
| `test/` | Host test suites (one directory per suite). |
| `scripts/test.sh` | Native (host, gcc) test runner. |
| `platformio.ini` | Teensy 4.1 cross-build environment. |

## Running host tests

Requires `gcc`. From anywhere:

```bash
firmware/scripts/test.sh --environment native --suite arena
firmware/scripts/test.sh --environment native --suite codec-api-contract
```

Suites build with `-Wall -Wextra -Werror` and ASan/UBSan. `scripts/test.sh
Aius.Firmware` becomes the repository entry point once the Teensy target build
lands (capability-gate P4).

## Status

Foundation only (capability-gate P1): the portable ABI headers, a tested arena
allocator, and the native harness. Next: the IEC 61937 parser (P2) and the
`dcadec` DTS Core decoder (P3).
