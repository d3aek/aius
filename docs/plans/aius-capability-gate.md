---
title: Aius DTS capability gate
type: plans
scope: aius
status: draft
tags: [aius, hardware, teensy, dts, spdif, platformio]
updated: 2026-07-15
toc: true
---

# Aius DTS capability gate

Prove that a stock Teensy 4.1 can parse IEC 61937, decode DTS Core 5.1, and retain enough deterministic compute and memory headroom for the later downmix, EQ, control, and network workloads. This milestone uses only the build server, the user's Teensy 4.1, and USB. It does not claim that optical S/PDIF reception or transmission works.

The companion [hardware follow-on plan](aius-hardware-follow-on.md) remains committed even if this gate fails. The [cost model](aius-cost-model.md) tracks the provisional carrier cost independently of the software result.

## Gate outcome

A positive result authorizes optical bench-chain development. It does not authorize PCB manufacture. A negative result is also a completed milestone when its reproducible evidence identifies correctness, CPU, memory, or toolchain as the limiting factor.

The first decoder candidate is a minimal DTS Core extraction from `dcadec`. If that fails only portability, allocation, correctness, or performance requirements, open a new reviewed phase to evaluate a minimal FFmpeg native DTS Core extraction behind the same interfaces. Do not import all of `libavcodec`, relax thresholds, or silently change the processor.

## Locked capability contract

### Target and tooling

- Teensy 4.1 at its production 600 MHz clock.
- Stock internal memory only. PSRAM cannot make the gate pass.
- PlatformIO Core 6.1.19 with `platformio/teensy@5.1.0`, `board = teensy41`, `framework = arduino`, and `upload_protocol = teensy-cli`.
- Project root `firmware/`, directly openable by the VS Code PlatformIO extension.
- Arduino and Teensyduino provide startup, USB loading, USB Serial, and retained `CrashReport` diagnostics only. Parser, decoder, DSP-facing contracts, and test fixtures remain framework-neutral.
- Persisted build scripts are the build-server interface. Their container invocations are ephemeral. A named PlatformIO package cache may persist.
- A resolved package manifest records framework, compiler, linker, uploader, and tool archives with versions and hashes. A clean offline rebuild must reproduce the release binary.
- `scripts/test.sh Aius.Firmware` is the repository test entry point once the target exists.

USB Serial and retained crash diagnostics are sufficient for this gate. Do not buy an offboard debugger. Reconsider J-Link only if repeated target failures cannot yield a retained crash record, serial event, or bounded fault location.

### Input and output scope

- IEC 61937 DTS Core at 48 kHz, including burst types I through III required by the test corpus.
- DTS Core representations encountered as 16-bit or 14-bit, big-endian or little-endian data.
- Up to 5.1 channels including LFE at the maximum ordinary S/PDIF DTS Core bitrate.
- Native decoded channels as planar signed 32-bit PCM with explicit sample count, sample rate, and canonical channel mask/order.
- Stereo PCM at 48 kHz is part of the later S/PDIF product topology, not this decoder benchmark.
- DTS-HD, DTS Express, unrelated compressed codecs, and sample-rate conversion are out of scope.
- Unsupported, malformed, or truncated input returns a typed status and never emits stale PCM.

### Portable interfaces

Expose versioned C-compatible interfaces suitable for later C++, bare-metal, or Rust callers:

- `AiusIec61937Parser` accepts 16-bit S/PDIF payload words and emits complete typed bursts.
- `AiusDtsDecoder` accepts one normalized DTS Core access unit and emits native planar PCM.
- `AiusArena` supplies caller-owned, aligned storage with a size-query operation and high-water reporting.
- `AiusMetrics` reports cycles, frame-deadline ratio, stack and arena high-water, allocation count, and decoder status.
- `AiusPcmSink` consumes decoded frames without depending on Arduino audio objects.

The ABI specifies ownership, alignment, buffer capacity, backpressure, reset, flush, format change, overflow, error recovery, and undefined-behaviour policy. A decoder instance is not reentrant. It may allocate only from its arena during initialization or a declared stream-format change. Ordinary frame decoding performs no heap allocation.

### USB proof protocol

Use `USB_SERIAL`, not USB Audio. The versioned binary protocol has:

- little-endian fields;
- `AIUS` magic, version, message type, sequence, payload length, and CRC-32/ISO-HDLC;
- a 64 KiB maximum message payload;
- length-based framing with bounded magic scanning after corruption;
- ACK, NACK, and typed error responses;
- duplicate-sequence rejection without re-execution;
- handling for partial and coalesced transfers;
- a two-second command timeout;
- commands to identify, reset metrics, submit a frame, request PCM, run a corpus, and run a soak;
- responses for capabilities, per-frame results, PCM data, structured diagnostics, and final summaries;
- golden host/device protocol vectors.

Decoder timing excludes USB transfer time. A retained `CrashReport` is the first structured diagnostic after reboot. Disconnect, reboot, missing sequence, CRC failure, or unexplained timeout fails the test rather than skipping it.

## P0: Commit the reviewed contracts

Commit this plan, the hardware follow-on, the cost model, Aius subtree instructions, glossary terms, and Aius evidence/LFS policy before implementation starts.

> **Status: complete 2026-07-15.** The contract artifacts, subtree conventions, glossary entry, and LFS policy passed the clean-room delivery audit and landed together at this checkpoint.

```yaml
phase: P0
unit: documentation-contract
seams: [capability gate, hardware follow-on, cost model, evidence policy]
contract-artifact: spec-only
fan-out:
  - name: capability-plan
    owned-paths: [docs/plans/aius-capability-gate.md]
  - name: hardware-plan
    owned-paths: [docs/plans/aius-hardware-follow-on.md]
  - name: cost-plan
    owned-paths: [docs/plans/aius-cost-model.md]
  - name: subtree-conventions
    owned-paths:
      - CLAUDE.md
      - docs/reference/glossary.md
      - .gitattributes
review-intensity: active
commit-checkpoint:
  green: [doc-index validation, LFS attribute verification, clean-room contract audit]
  buildable: [decision-complete capability and hardware contracts]
  parent-commit: "docs(aius): establish capability and hardware plans"
```

## P1: Evidence, topology, and functional BOM

Archive the authoritative datasheets, errata, reference schematics, source-design packages, mechanical files, and dynamic supplier evidence below `docs/reference/datasheets.md`. Binary artifacts and generated binary fixtures use Git LFS. Text manifests, scripts, Markdown, KiCad sources, CSV, JSON, and patches remain normal Git text.

Every saved artifact receives a manifest record containing its project-relative path, description and use, source URL, manufacturer and revision, retrieval time, SHA-256, file size, licence or redistribution note, and current or superseded status.

Produce:

- source and clone-boundary manifests distinguishing official reuse, independently re-derived circuitry, comparative evidence, and unresolved assumptions;
- annotated reference schematics and electrical tables for Teensy, Cliff optical modules, VKSDR comparative designs, PJRC Ethernet, USB-C, S/PDIF muxing, and transmitter gating;
- signal, channel, clock, DMA/buffering, power, and control topology;
- preliminary Teensy pin and peripheral allocation;
- two-layer placement feasibility sketch;
- costed functional BOM and risk register.

Cliff datasheets are the golden optical source. VKSDR is comparative evidence. Official PJRC Teensy 4.1 and Ethernet-kit documents are the Teensy and Ethernet golden sources. Licensing is retained as a provenance footnote for this personal, non-commercial project and is not a capability gate. Existing notices remain preserved.

```yaml
phase: P1
unit: evidence-components
seams: [evidence manifest, topology, functional BOM, risk register]
contract-artifact: spec-only
fan-out:
  - name: source-archive
    owned-paths:
      - docs/reference/datasheets.md
      - evidence/manifest
  - name: functional-topology
    owned-paths: [docs/reference]
  - name: should-cost
    owned-paths: [evidence/cost]
review-intensity: active
commit-checkpoint:
  green: [source hash verification, LFS verification, provenance audit, cost arithmetic]
  buildable: [auditable evidence and topology package]
  parent-commit: "docs(aius): establish evidence topology and should-cost"
```

An independent reviewer checks every reference-schematic pin and electrical claim against the named source before the phase lands.

## P2: Portable codec contracts and deterministic PCM sources

Commit the C ABI stubs before implementation fan-out. Generate deterministic, redistributable multichannel PCM sources independently of DTS tooling, including independent tones, impulses, silence, phase inversions, and channel-identification sequences. Record generator command, tool revision, input and output hashes, expected channel metadata, and redistribution status.

```yaml
phase: P2
unit: interface-and-fixtures
seams: [versioned codec ABI, arena contract, deterministic PCM manifest]
contract-artifact: stubs-committed
fan-out:
  - name: codec-contract
    owned-paths: [firmware/lib/aius_codec_api]
  - name: pcm-generator
    owned-paths: [firmware/tools/pcm_corpus]
review-intensity: active
commit-checkpoint:
  green: [ABI compile tests, arena boundary tests, PCM checksum regeneration]
  buildable: [portable ABI and source corpus]
  parent-commit: "feat(aius): define portable codec contract"
```

## P3: IEC 61937 parser and corpus

Implement Pa, Pb, Pc, and Pd interpretation, data type and payload bit length, padding and burst period, maximum burst size, arbitrary word-boundary chunking, malformed and truncated burst rejection, and bounded recovery into the next valid preamble.

The corpus covers valid type I through III bursts, 14/16-bit and endian normalization, one-word calls, split and coalesced reads, corrupt sync, invalid lengths, unsupported burst types, truncation, padding, and recovery into a subsequent valid burst. Use known-answer vectors with explicit redistribution rights where possible. Otherwise record the residual standards-conformance uncertainty.

```yaml
phase: P3
unit: parser-component
seams: [AiusIec61937Parser, IEC fixture manifest]
contract-artifact: both
fan-out:
  - name: parser
    owned-paths: [firmware/lib/aius_iec61937]
  - name: parser-corpus
    owned-paths: [firmware/tools/iec_corpus]
  - name: parser-tests
    owned-paths: [firmware/test/iec61937]
review-intensity: active
commit-checkpoint:
  green: [unit tests, chunk-boundary properties, malformed-input fuzzing]
  buildable: [host IEC 61937 parser]
  parent-commit: "feat(aius): add IEC 61937 DTS parser"
```

## P4: Minimal DTS Core decoder

Vendor only the required `dcadec` DTS Core files, notices, exact upstream commit, and a reviewable patch series. Remove desktop I/O, DTS-HD/LBR/XLL code, and uncontrolled allocation. Normalize supported wire representations before decode and emit native channels without an embedded stereo downmix.

Host tests cover the valid corpus, malformed and truncated frames, channel order and LFE, sample count, repeated initialization, format changes, arena exhaustion, misaligned input, sanitizers, undefined behaviour, and fuzzing.

FFmpeg is a common-lineage differential implementation, not an independent oracle. Comparison requires exact sample count and channel mapping, documented delay alignment, per-channel correlation above 0.9999, and error below -80 dBFS. A mismatch is investigated and not automatically attributed to either implementation. The same decoder build must later be bit-exact between host and Teensy.

```yaml
phase: P4
unit: decoder-component
seams: [AiusDtsDecoder, AiusArena]
contract-artifact: stubs-committed
fan-out:
  - name: source-import
    owned-paths: [firmware/vendor/dcadec-core]
  - name: decoder-adapter
    owned-paths: [firmware/lib/aius_dts]
  - name: decoder-tests
    owned-paths: [firmware/test/dts]
  - name: differential-runner
    owned-paths: [firmware/tools/differential]
review-intensity: active
commit-checkpoint:
  green: [host tests, sanitizers, fuzzing, differential report]
  buildable: [portable DTS Core decoder]
  parent-commit: "feat(aius): add portable DTS Core decoder"
```

## P5: Reproducible PlatformIO target

Create the VS Code PlatformIO project and pinned build-server wrapper. Produce release and instrumentation environments at 600 MHz. Record framework, compiler, linker, uploader, and all transitive package versions and archive hashes. Prove that a clean offline rebuild using only the pinned container and persisted package cache produces the same binary.

Report ITCM code, DTCM static and stack use, OCRAM/RAM2 and DMA buffers, USB buffers, arena peak, and unused headroom separately. A combined-RAM percentage cannot conceal pressure in a required bank. Static plus reserved runtime memory must leave at least 25% justified headroom in every relevant bank before hardware testing.

```yaml
phase: P5
unit: target-build
seams: [PlatformIO environment, resolved package lock, bank-level memory report]
contract-artifact: both
fan-out:
  - name: platformio-project
    owned-paths:
      - firmware/platformio.ini
      - firmware/src
  - name: build-orchestration
    owned-paths:
      - firmware/build
      - firmware/scripts
      - scripts/test.sh
  - name: memory-report
    owned-paths: [firmware/tools/memory]
review-intensity: active
commit-checkpoint:
  green: [native tests, online cross-build, offline rebuild, checksum parity, memory gate]
  buildable: [flashable Teensy capability firmware]
  parent-commit: "build(aius): add reproducible Teensy target"
```

## P6: USB capability harness

Implement the locked USB protocol, retained crash-report path, simulated device, host controller, and golden byte vectors. The host runner persists raw commands, results, version identity, build hash, metrics, and errors so a test can be audited without rerunning it.

```yaml
phase: P6
unit: protocol-adapters
seams: [capability protocol, structured diagnostic stream]
contract-artifact: stubs-committed
fan-out:
  - name: device-protocol
    owned-paths: [firmware/src/capability]
  - name: host-controller
    owned-paths: [firmware/tools/device]
  - name: protocol-tests
    owned-paths: [firmware/test/protocol]
review-intensity: active
commit-checkpoint:
  green: [simulated-device tests, golden protocol vectors, Teensy build]
  buildable: [USB capability firmware and host runner]
  parent-commit: "feat(aius): add USB capability harness"
```

## P7: USB-only Teensy proof

This phase starts when the user connects the stock Teensy 4.1 directly to the build server with a known data-capable USB cable. Nothing else connects to the board. The Program button remains physically accessible.

The persisted runner flashes the identified release build, streams the complete corpus, and records raw results. Acceptance requires:

- target PCM bit-exact with the host build of the same decoder;
- differential thresholds against pinned FFmpeg remain satisfied;
- DWT timing around normalization, parsing, and decoding separately, excluding USB transfer;
- optimized 600 MHz release measurements for cold and warmed cache cases;
- decoder p99 no more than 50% of each frame's presentation time;
- worst valid frame no more than 65% of its presentation time;
- at least 25% justified headroom in every relevant internal-memory bank;
- no ordinary decode-path allocation;
- malformed inputs recover at the next valid burst without reset;
- every failure produces a typed result or retained crash report.

```yaml
phase: P7
unit: hardware-test-surface
seams: [USB capability protocol, target result manifest]
contract-artifact: both
fan-out:
  - name: target-runner
    owned-paths: [evidence/capability-results]
review-intensity: active
commit-checkpoint:
  green: [full valid corpus, malformed corpus, performance and memory gates]
  buildable: [measured Teensy capability report]
  parent-commit: "test(aius): record Teensy DTS capability result"
```

A clean-room delivery audit compares raw result artifacts against every threshold before declaring a pass.

## P8: Dedicated eight-hour soak

Run on the recorded Teensy, USB cable, build-server port, firmware hash, and host-runner hash. Persist periodic telemetry and raw results. Any reset, missing sequence, CRC or protocol error, PCM mismatch, deadline miss, allocation, memory high-water growth, or unexpected crash report fails the soak. Automatic retry cannot convert a failure to green.

```yaml
phase: P8
unit: test-surface
seams: [soak manifest, periodic telemetry, final verdict]
contract-artifact: both
fan-out:
  - name: soak-runner
    owned-paths: [evidence/capability-results]
review-intensity: active
commit-checkpoint:
  green: [eight-hour zero-failure soak, clean-room result audit]
  buildable: [final DTS capability decision]
  parent-commit: "test(aius): complete Teensy DTS capability gate"
```

## Deferred decisions

- Optical S/PDIF word integrity, simultaneous 48 kHz receive/transmit operation, clock recovery, drift, latency, and carrier gating belong to the hardware follow-on.
- Embedded Rust is evaluated after P7 against the stable C ABI and the actual i.MX RT1062 USB, Ethernet, DMA, and S/PDIF support gaps. No Rust migration occurs inside this gate.
- Product licensing, certification, and commercial distribution remain out of scope for this personal build. Preserve third-party notices and record provenance.
