# Third-party components

Aius is licensed under [GPL-3.0-or-later](LICENSE). This file records every
third-party component whose code reaches the shipped `teensy41-optical` firmware
image, its licence, and where its own notice lives.

Surveyed 2026-08-05 against the `[env:teensy41-optical]` build in
[`firmware/platformio.ini`](firmware/platformio.ini), cross-checked against the
symbols actually present in the linked `firmware.elf` rather than the build
configuration alone.

## Why the outbound licence is GPL-3.0-or-later

Two components decide it between them.

**The AC-3 decoder is copyleft.** [`firmware/lib/aius_a52/a52/`](firmware/lib/aius_a52/a52/)
is a fork of liba52 (a52dec 0.7.4), and every file carries "either version 2 of
the License, or (at your option) any later version". It is statically linked
into the image, so the whole combined work must be GPL. Note that this one lives
under `lib/` rather than `vendor/`.

**The TCP/IP stack rules out GPL-2.** FNET is Apache-2.0 and is linked in.
Apache-2.0 is incompatible with GPL-2.0 but compatible with GPL-3.0, so GPL-2.0
is unavailable to this project. liba52's "or later" is what makes GPL-3.0
reachable; had it been GPL-2-only, the two could not be distributed together.

`SPI` and `NativeW5100.h` are dual "GPL v2 or LGPL v2.1" with no "or later". The
LGPL-2.1 branch is taken, and LGPL-2.1 §3 permits converting to the ordinary GPL
at a version newer than 2.

## Components

| Component | Path | Licence | Modified |
|---|---|---|---|
| liba52 / a52dec 0.7.4 | `firmware/lib/aius_a52/a52/` | GPL-2.0-or-later | Yes |
| dcadec (DTS Core) | `firmware/vendor/dcadec-core/` | LGPL-2.1-or-later | No |
| dcadec `ta` allocator | `firmware/vendor/dcadec-core/ta.[ch]` | ISC-style | No |
| FFmpeg 7.1.5 (E-AC-3) | `firmware/vendor/ffmpeg-eac3/` | LGPL-2.1-or-later | Yes |
| Boost `erf` fragment | `firmware/vendor/ffmpeg-eac3/libavutil/` | BSL-1.0 | No |
| MIPS fixed-DSP header | `firmware/vendor/ffmpeg-eac3/libavutil/fixed_dsp.h` | BSD-3-Clause | No |
| TweetNaCl 20140427 | `firmware/vendor/tweetnacl/` | Public domain (see caveat) | No |
| NativeEthernet | `firmware/vendor/NativeEthernet/` | MIT | Yes |
| └ `NativeDns.*` | same tree | Apache-2.0 | No |
| └ `utility/NativeW5100.h` | same tree | GPL-2.0 or LGPL-2.1 | No |
| PJRC S/PDIF RX / TX units | `evidence/source-snapshots/PJRC-SPDIF-*/` | MIT + funding notice | No |
| FNET 4.6.4 fork | Teensy framework package | Apache-2.0 | Upstream fork |
| Teensyduino core | Teensy framework package | MIT + target-device clause | No |
| `memcpy-armv7m.S`, `util/crc16.h` | Teensy framework package | BSD-3-Clause | No |
| Audio library headers | Teensy framework package | MIT + funding notice | No |
| SD wrapper | Teensy framework package | MIT + funding notice | No |
| SdFat 2.1.2 | Teensy framework package | MIT | Teensy's own copy |
| SPI | Teensy framework package | GPL-2.0 or LGPL-2.1 | No |

Each vendored tree keeps its upstream licence file and a `NOTICE.md` recording
its provenance, pinned upstream revision and every divergence.

## Obligations this project carries

- **Modified files state that they were changed.** GPL §2(a) requires this of the
  liba52 fork and LGPL §2(b) of the FFmpeg fork. Every touched file in both now
  carries a dated change notice above the upstream header, pointing at the
  `NOTICE.md` that lists the divergences, and each divergence is also marked at
  its site in the code. NativeEthernet is MIT and carries no such obligation;
  its five patched files carry the same notice as attribution hygiene. Nothing
  under an Apache licence is modified here.
- **Complete corresponding source** is this repository. Anyone distributing a
  built image separately from it must satisfy GPL §3 themselves, by shipping the
  source alongside or by a written offer.
- **Licence texts travel with the work.** LGPL §6 and Apache §4(a) both require
  supplying the licence; the vendored trees carry theirs.
- **PJRC's funding notice is preserved** verbatim in the S/PDIF, SD and Audio
  files, as their licence requires alongside the copyright notice.

## Caveats, stated rather than resolved

Neither of these is settled, and neither is a matter this project can settle by
asserting something.

**TweetNaCl carries no licence statement.** `tweetnacl.c` and `tweetnacl.h`
contain no copyright line and no grant. The public-domain status is the authors'
well-known dedication for NaCl and TweetNaCl, but in this checkout it is asserted
only by a `NOTICE.md` written here, with no upstream statement to point at.

**The Teensy core carries a clause GPL does not obviously accommodate.** Beyond
the MIT grant, it requires that a build system offering a choice of target
devices must include comparable PJRC devices in that list. That is a
product-inclusion requirement rather than the preservation of a legal notice, so
it does not clearly fall within the additional terms GPL-3.0 §7 permits. It is
recorded here because publishing should not depend on nobody noticing it.
