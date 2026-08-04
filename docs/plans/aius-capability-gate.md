---
title: Aius DTS capability gate
type: plans
scope: component
component: aius
status: shipped
tags: [aius, hardware, teensy, dts, spdif, platformio]
updated: 2026-08-04
toc: true
---

# Aius DTS capability gate

Prove that a stock Teensy 4.1 can parse IEC 61937 and decode DTS Core 5.1 within
enough time and memory headroom to leave room for the later downmix, EQ, control,
and network work. This milestone uses only the build server, the user's Teensy
4.1, and USB — no optical hardware, no custom PCB.

**A pass authorises optical bench work, not a board order.** A clean negative is
also a completed result when its measurements name the limiting factor
(correctness, CPU, memory, or toolchain). This is the software-first track; the
board is specified in the [hardware spec](aius-hardware-spec.md).

This plan was rewritten lean on 2026-07-18: the prior version's P6b/P7
closed-schema, gate-ID verdict machinery, nullability matrices, and hash-bound
evidence contracts were process ceremony and are removed. What remains is the
engineering: build it, decode correctly, measure it honestly.

## Locked target

- Teensy 4.1 at its production 600 MHz clock, stock internal memory only (PSRAM
  cannot make the gate pass).
- PlatformIO Core with `platformio/teensy`, `board = teensy41`,
  `framework = arduino`, `upload_protocol = teensy-cli`.
- Project root `firmware/`, openable directly in the VS Code
  PlatformIO extension.
- Arduino/Teensyduino provides only startup, USB loading, USB Serial, and retained
  `CrashReport`. Parser, decoder, arena, metrics, and PCM-sink code stay behind
  portable C-compatible interfaces with no Arduino dependency, so the same code
  runs in host tests.
- `scripts/test.sh Aius.Firmware` is the repository test entry point once the
  firmware target exists; it wraps the firmware's own `scripts/test.sh`.

USB Serial plus retained `CrashReport` are sufficient. Do not buy an offboard
debugger unless repeated target failures cannot produce a retained crash record,
serial event, or bounded fault location.

## Scope

- **In:** IEC 61937 DTS Core at 48 kHz (burst types I–III), 16/14-bit and
  big/little-endian representations, up to 5.1 channels incl. LFE at the ordinary
  S/PDIF DTS Core bitrate, decoded to planar signed-32-bit PCM with explicit
  sample count/rate and canonical channel mask.
- **Out:** DTS-HD, DTS Express, other codecs, sample-rate conversion, and the
  stereo downmix itself (that is product DSP, not this decoder benchmark). Optical
  RX/TX, clock recovery, and carrier gating belong to hardware integration.
- Unsupported, malformed, or truncated input returns a typed status and never
  emits stale PCM.

## Portable interfaces

Versioned C-compatible interfaces, host- and target-callable:

- `AiusIec61937Parser` — accepts S/PDIF payload words, emits complete typed bursts.
- `AiusDtsDecoder` — accepts one normalized DTS Core access unit, emits native
  planar PCM.
- `AiusArena` — caller-owned aligned storage with size-query and high-water report.
- `AiusMetrics` — cycles, frame-deadline ratio, stack/arena high-water, allocation
  count, decoder status.
- `AiusPcmSink` — consumes decoded frames without Arduino audio objects.

A decoder instance is not reentrant. It may allocate from its arena only during
init or a declared stream-format change; ordinary frame decode does no heap
allocation.

## Decoder choice

First candidate: a minimal DTS Core extraction from `dcadec` — vendor only the
required core files, notices, and exact upstream commit, behind a reviewable patch
series. Strip desktop I/O, DTS-HD/LBR/XLL, and uncontrolled allocation. If it fails
portability, allocation, correctness, or performance, open a reviewed phase to
evaluate a minimal FFmpeg native DTS Core extraction behind the same interfaces.
Do not import all of `libavcodec` or relax the budgets.

FFmpeg is a common-lineage differential reference, not an independent oracle:
compare with exact sample count and channel mapping, documented delay alignment,
per-channel correlation > 0.9999, and error below −80 dBFS. Investigate mismatches
rather than blaming either side. The same decoder build must later be bit-exact
between host and Teensy.

## Budgets

Measured, not asserted. The gate passes when, at 600 MHz release build:

- decoder output is bit-exact between the host and Teensy builds of the same code;
- FFmpeg differential thresholds hold;
- decode time (DWT cycle count, excluding USB transfer) p99 ≤ 50 % of frame
  presentation time, worst valid frame ≤ 65 %;
- every relevant internal-memory bank (ITCM, DTCM, OCRAM) keeps ≥ 25 % headroom —
  reported per bank, not as a combined percentage;
- no allocation on the ordinary decode path;
- malformed input recovers at the next valid burst without a reset;
- every failure yields a typed result or a retained crash report.

## Phases

Each phase is a normal commit: green host tests (and, from P5, a reproducible
target build) plus a diff review. No verdict schemas. **P1–P6 are done** (P6
physical validation 2026-07-21, evidence
[`p6-hidraw-validated.md`](../../evidence/capability-results/p6-hidraw-validated.md));
**P7's USB-harness soak was overtaken — closed 2026-08-04**, see the phase
entry. The
product signal chain beyond this gate — PCM/DTS classifier + end-to-end pipeline
(`lib/aius_pipeline`) and the 5.1→stereo downmix + EQ (`lib/aius_dsp`) — also
landed host-tested during the board-free window.

**Gate verdict (recorded 2026-08-04): PASS.** The pass authorised optical
bench work, and that work shipped: the Phase 14 optical cutover made
`teensy41-optical` the live runtime, and the decoder this gate benchmarked
now runs in production well inside its budgets — 936,466 cycles per DTS AU
≈ 1.56 ms against the 8 ms AUDIO service budget on the E-AC-3-era image
([`aius-eac3-support.md`](aius-eac3-support.md) Phase 4 acceptance),
steady-state no-heap decode from the 128 KB DTCM arena proven on hardware
(62.5 KB peak; register defects 14/29).

1. **P1 — Portable codec contracts.** *Done.* The C headers (`aius_codec.h`,
   `aius_arena.h`), a tested arena allocator, and the native harness
   (`firmware/scripts/test.sh --environment native`). Deviation: independent
   deterministic PCM generation was folded into P3, which instead uses
   FFmpeg-encoded DTS Core vectors + an FFmpeg differential reference.
2. **P2 — IEC 61937 parser.** *Done.* Pa/Pb/Pc/Pd interpretation, 14/16-bit +
   endian normalization, arbitrary word-boundary chunking, typed
   malformed/truncated/unsupported rejection, bounded resync. Host corpus + 200k
   fuzz (`lib/aius_iec61937`).
3. **P3 — DTS Core decoder.** *Done.* `dcadec` fixed-point core vendored verbatim
   behind `AiusDtsDecoder`; host decode bit-exact to FFmpeg on every full-band
   channel (LFE matches within a looser bound — each decoder uses its own LFE
   reconstruction FIR); no-heap arena path (0 steady-state allocations, 60.9 KiB
   high-water for 5.1); 1M-iteration robustness fuzz. LFE since confirmed
   bit-exact to a full dcadec build (the FFmpeg gap was its differing LFE FIR) —
   `dts-lfe` suite.
4. **P4 — Reproducible PlatformIO target.** *Done.* `teensy41-release` +
   `teensy41-instrument` at 600 MHz; per-bank memory report (all banks fit);
   `firmware.hex` byte-identical across rebuilds. The firmware's own
   `firmware/scripts/test.sh` is the native-test entry point — the repo-root
   `scripts/test.sh` is a .NET/vitest/pytest discovery runner with no C-suite
   slot, so it is deliberately not modified.
5. **P5 — USB capability harness.** *Done.* The versioned USB-Serial
   protocol (`AIUS` magic, length-framed, CRC-32/ISO-HDLC, ACK/NACK; identify,
   submit-frame, request-PCM, run-corpus, run-soak), an arena-driven device
   handler over `AiusDtsDecoder`, a simulated device + host runner over an
   `AiusHostTransport` seam, and golden wire vectors — all host-tested
   (`lib/aius_capability`, `tools/device`).
6. **P6 — Teensy proof.** *Done.* The
   `AiusHostTransport` seam is implemented over the Teensy RawHID reports exposed
   as `/dev/hidraw*`; the runner, framer, and protocol remain unchanged from the
   P5 sim. The `teensy41-capability` target provides the matching firmware entry
   point and uses acknowledged 32-byte command fragments. The physical corpus
   passed 40/40 vectors with zero allocation violations; raw results and a summary
   are under `evidence/capability-results/`. The USB path must pass through a
   dedicated USB controller (or provide equivalent host-side reattachment);
   individual VID/PID hostdevs and hub passthrough do not follow Teensy
   re-enumeration. `scripts/flash_and_run.sh` supplies bounded loader and
   application-interface discovery once that topology is in place.
7. **P7 — Soak.** *Closed as overtaken 2026-08-04.* As specified — an
   eight-hour run over the P6 USB harness — it never ran: the first long
   RawHID run was invalidated by a transport stall, the fragment-retry fix
   was validated at 100 iterations / 4,000 frames, and the runtime then cut
   over to the optical image
   ([`aius-optical-prototype.md`](aius-optical-prototype.md) §RawHID soak
   investigation), retiring the USB harness as a bring-up tool. The
   sustained-decode question P7 existed to answer was settled on the
   production lineage with strictly stronger evidence: a 1,857 s live
   optical DTS soak under recorder/HTTP load with zero drops or resets
   (defect-31 closure, [`aius-ac3-support.md`](aius-ac3-support.md) Slice
   2b), and on 2026-08-04 a 10.0 h loaded watch with ~8 h of continuous
   production DTS decode — 2,842+ passes, zero failures, zero resets
   (`6950202b7`; [`aius-open-work-register.md`](aius-open-work-register.md)
   periodic-WDOG entry, 2026-08-04 bullet). Any reset,
   protocol error, PCM mismatch, deadline miss, allocation, memory high-water
   growth, or crash would have failed it; none occurred.

## Deferred

- Optical S/PDIF integrity, simultaneous 48 kHz RX/TX, clock recovery, drift,
  latency, and carrier gating → hardware integration.
- Embedded Rust is evaluated after P6 against the stable C ABI and the real
  i.MX RT1062 USB/Ethernet/DMA/S/PDIF gaps. No Rust migration inside this gate.
- Licensing/certification stay out of scope for this personal build; preserve
  third-party notices and record provenance.
