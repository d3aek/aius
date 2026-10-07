# SdFat provenance and Aius patches

This directory is a source snapshot of the MIT-licensed `SdFat` library
(version 2.1.2, Teensy-modified) shipped in PlatformIO package
`framework-arduinoteensy` version
`1.162.0`, selected by `teensy@5.2.0`.

Snapshot source: `libraries/SdFat/{src,LICENSE.md,library.properties}`,
copied from the locally installed immutable PlatformIO package on 2026-10-07.
Examples, `doc` and `extras` are omitted because the firmware build does not
consume them. `LICENSE.md` is the unmodified upstream license.
`UPSTREAM_SHA256` records every copied file's pristine 1.162.0 bytes, so the
unmodified tree is reproducible; `PATCHED_SHA256` records the expected result
of every intentional Aius patch, so a later edit fails the `sdfat-vendor`
source-contract suite until reviewed. `library.json` and `.gitattributes` are
Aius additions. The only files intentionally different from upstream are:

- `src/SdCard/SdioTeensy.cpp`
- `src/SdCard/SdioCard.h`
- `src/FatLib/FatPartition.cpp`
- `src/FatLib/FatPartition.h`
- `src/FsLib/FsVolume.h`
- `library.properties`, whose library name becomes `AiusSdFat` (as does the
  added `library.json`) so PlatformIO can ignore the framework copy by name.

The patches (Phase 4e of `docs/plans/aius-sample-rate-following.md`, calls
E1, E2, E6, E7 and CR5-S4):

- **Wait hook (E1, E6).** `extern "C" void aius_sdio_wait_hook(void)` is
  declared in `SdioCard.h` and defined weak and empty in `SdioTeensy.cpp`; the
  firmware supplies the strong definition. It is called once per
  `yieldTimeout` iteration (after upstream's `yield()`) and once per ACMD41
  iteration in `SdioCard::begin`, and never in `waitTimeout`, `cardCommand`,
  `readData` or `writeData`, so no pass runs between the `SDHC_XFERTYP` write
  and the command-complete observation.
- **DMA only into DTCM (E2).** `aiusDmaBufferOk()` accepts a 4-byte-aligned
  buffer lying wholly inside DTCM on the IMXRT1062, from `0x20000000` up to
  the linker symbol `_estack` (DTCM shrinks as ITCM blocks grow; `0x20050000`
  under the six-block link), never a constant bound. `readSector` and `writeSector` bounce every other buffer through
  their 512 B stack buffer (now explicitly 4-byte aligned); `readSectors` and
  `writeSectors` take the per-sector path for it; `rdWrSectors` and `cardCMD6`
  refuse it with `SD_CARD_ERROR_DMA`. OCRAM and PSRAM buffers never meet the
  controller, so no path needs dcache maintenance. `begin`'s CMD6 status
  buffer is a 4-byte-aligned stack array.
- **Busy-timeout counter (E7).** Every `BUSY_TIMEOUT_MICROS` expiry
  (`yieldTimeout`, `waitTimeout`, the ACMD41 deadline) increments a lifetime
  counter read by `static uint32_t SdioCard::busyTimeouts()`. The ACMD41
  condition is split so a command failure is not also counted as an expiry.
- **Error reset.** `static void SdioCard::clearError()` clears the file-static
  error code and line, which upstream clears only in `begin`.
- **Volume serial (CR5-S4).** `FatPartition::init` (both overloads) stores the
  boot sector's `volumeSerialNumber` (FAT12/16 or FAT32 BPB offset by FAT
  type) in a new 4 B member from the boot sector it already reads; the member
  is zero at the start of `init`, after `end()`, and on any non-FAT volume.
  `FatPartition::volumeSerial()` and the `FsVolume::volumeSerial()` forwarder
  return the stored value with no card read.
