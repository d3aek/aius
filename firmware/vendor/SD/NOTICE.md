# SD wrapper provenance and Aius patches

This directory is a source snapshot of the MIT-licensed Teensy `SD`
compatibility wrapper for SdFat (version 2.0.0) shipped in PlatformIO package
`framework-arduinoteensy` version
`1.162.0`, selected by `teensy@5.2.0`. The license text is the header of
each source file; upstream ships no separate license file.

Snapshot source: `libraries/SD/{src,library.properties}`, copied from the
locally installed immutable PlatformIO package on 2026-10-07. Examples and
`keywords.txt` are omitted. `UPSTREAM_SHA256` records every copied file's
pristine bytes; `PATCHED_SHA256` records every intentional Aius patch, checked
by the `sdfat-vendor` source-contract suite. `library.json` and
`.gitattributes` are Aius additions. The only files intentionally different
from upstream are:

- `src/SD.h`
- `src/SD.cpp`
- `library.properties`, whose library name becomes `AiusSD` (as does the
  added `library.json`) so PlatformIO can ignore the framework copy by name.

It builds against the vendored `firmware/vendor/SdFat` (`AiusSdFat`).

The patches (Phase 4e of `docs/plans/aius-sample-rate-following.md`):

- `SDClass::begin(uint8_t csPin = 10, bool dma = false)` mounts the built-in
  card in FIFO or DMA mode in one call; `sdfs.restart()` keeps the stored mode,
  as upstream already does.
- `SDClass::cardDetected()` reads DAT3 (pin 46 on Teensy 4.1), the test
  upstream `mediaPresent()` makes before a restart, without the restart. A
  failed `begin` already leaves DAT3 `INPUT_PULLDOWN`.
- `SDClass::clearError()`, `SDClass::sdErrorCode()` and
  `SDClass::busyTimeouts()` forward to the SdioCard error reset, the card's
  error code and the busy-timeout counter; each is safe before the first
  mount (the SdioCard state is file-static; `sdErrorCode()` is 0 with no card
  object).
