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
unit: single-file-implementation
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

The normative P1 contract is [Aius P1 evidence, topology, and cost
implementation](aius-p1-evidence-topology-cost.md). It splits this original
outcome brief into committed P1A through P1E checkpoints, resolves evidence and
Git LFS representation, and defines the validator commands. Where this original
brief conflicts with that contract, the P1 implementation plan governs.

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

### Original P1 outcome brief (superseded)

This subsection is retained only as decision history. It is not an executable
phase contract.

P1 runs in two waves. `source-archive` lands the manifest and artifacts in the
shared tree first. `functional-topology` and `should-cost` then run in parallel
and may cite only the immutable source IDs from that manifest. The phase remains
one commit checkpoint because no intermediate artifact is independently useful.

The source matrix is finite. A retrieval may follow a document link from the
named landing page, but it may not substitute another authority. The manifest
records both the specified landing URL and the final artifact URL after
redirects.

| Source ID | Class | Required artifact | Fixed landing or artifact URL | P1 use |
|---|---|---|---|---|
| `PJRC-TEENSY41-PAGE` | golden | saved product page | `https://www.pjrc.com/store/teensy41.html` | board limits, power, dimensions, interfaces, schematic links |
| `PJRC-TEENSY41-PINOUT` | golden | current two-sided pinout PDF | `https://www.pjrc.com/teensy/card11b_rev4_web.pdf` | pin and peripheral allocation |
| `PJRC-IMXRT1060-RM` | golden | annotated reference manual, revision 3 | `https://www.pjrc.com/teensy/IMXRT1060RM_rev3_annotations.pdf` | S/PDIF, DMA, clock and memory claims |
| `PJRC-IMXRT1060-CEC` | golden | electrical datasheet, revision 4 | `https://www.pjrc.com/teensy/IMXRT1060CEC_rev4.pdf` | electrical limits |
| `PJRC-ETHERNET-KIT` | golden | assembly guide PDF linked from the page | `https://www.pjrc.com/store/ethernet_kit.html` | native-Ethernet connector circuit and parts |
| `CLIFF-OTJ5` | golden | OTJ-5/FCR684205T manufacturer datasheet, version 2.1 | `https://www.cliffuk.co.uk/products/optical/FCR684205T.pdf` | optical transmitter circuit and limits |
| `CLIFF-ORJ5` | golden | ORJ-5/FCR684205R manufacturer datasheet, version 2.0 | `https://www.cliffuk.co.uk/products/optical/FCR684205R.pdf` | optical receiver circuit and limits |
| `VKSDR-TOSLINK-SCHEMATICS` | comparative | published schematic archive | `https://vksdr.com/pmod/` | independent comparison of Cliff support circuitry |
| `VKSDR-TOSLINK-KICAD` | comparative | published KiCad archive | `https://vksdr.com/pmod/` | footprint and placement comparison only |
| `HAMMOND-1455N1201BK` | golden | dimension drawing and available STEP model | `https://www.hammfg.com/part/1455N1201BK` | enclosure and board envelope |
| `PLATFORMIO-TEENSY41` | golden | saved board documentation page | `https://docs.platformio.org/en/latest/boards/teensy/teensy41.html` | target and upload configuration |
| `PJRC-SPDIF-RX-SOURCE` | comparative | exact upstream source snapshot | `https://raw.githubusercontent.com/PaulStoffregen/Audio/master/input_spdif3.cpp` | existing receive/DMA implementation comparison |
| `PJRC-SPDIF-TX-SOURCE` | comparative | exact upstream source snapshot | `https://raw.githubusercontent.com/PaulStoffregen/Audio/master/output_spdif3.cpp` | existing transmit/DMA implementation comparison |
| `FARNELL-1455N1201BK` | dynamic | saved quote evidence | `https://uk.farnell.com/hammond/1455n1201bk/box-black-aluminium-end-plate/dp/9287876` | enclosure price and stock |
| `CPC-CLIFF-OTJ5` | dynamic | saved quote evidence | `https://cpc.farnell.com/cliff-electronic-components/fcr684205t/optical-jack-transmitter-otj-5/dp/CN20715` | transmitter price and stock |
| `DIGIKEY-CLIFF-ORJ5` | dynamic | saved quote evidence | `https://www.digikey.co.uk/en/products/detail/cliff-electronic-components-ltd/FCR684205R/15283094` | receiver price and stock |
| `PCBWAY-2L-100` | dynamic | saved quote or price-page evidence | `https://www.pcbway.com/pcb_prototype/2_Layer_pcb/100x100mm.html` | two-layer prototype fabrication range |
| `PCBWAY-4L-100` | dynamic | saved quote or price-page evidence | `https://www.pcbway.com/pcb_prototype/4_Layer_pcb/100x100mm.html` | automatic fallback cost delta |

If an exact URL is unavailable or redirects to unrelated content, the agent
records `unavailable` and does not replace it. A missing golden artifact blocks
P1. A missing comparative artifact blocks only claims that cannot be re-derived
from golden evidence. Dynamic prices may be replaced only by another named UK
authorized distributor for the identical manufacturer part, with the deviation
recorded in the cost README.

The source archive has these exact text outputs:

- `evidence/manifest/schema.json`: JSON Schema for source records;
- `evidence/manifest/sources.json`: records in source-ID order;
- `evidence/manifest/README.md`: retrieval and validation procedure;
- `docs/reference/datasheets.md<source-id>/<retrieved-file>`: immutable downloaded bytes.

Each source record contains `id`, `class`, `title`, `authority`, `revision`,
`landing_url`, `artifact_url`, `retrieved_at`, `artifact_path`, `sha256`,
`bytes`, `media_type`, `redistribution_note`, `status`, and `supersedes`.
`class` is one of `golden`, `comparative`, `independently-derived`, or
`dynamic`. `status` is one of `current`, `superseded`, or `unavailable`.
Source IDs are immutable. A revision gets a new ID and names the prior ID in
`supersedes`; records are never silently overwritten. Unavailable records use
null artifact metadata and explain the retrieval failure in
`redistribution_note`.

Archive validation recalculates every artifact byte count and SHA-256, checks
its media type is plausible, and confirms `git check-attr filter` reports
`lfs`. It also rejects an artifact that is already an LFS pointer instead of
the downloaded content. `sources.json`, its schema, Markdown, source snapshots,
and generated CSV remain ordinary Git text.

The topology wave has these exact outputs:

- `docs/reference/functional-topology.md`: input modes, output modes, block and
  channel flow, data/control/power paths, clock domains, buffering/DMA
  ownership, and mode/fault state table;
- `docs/reference/electrical-reference.md`: connector and signal tables for
  optical RX/TX, USB, Ethernet, raw S/PDIF bypass/mux, and TX gating, with every
  limit cited by source ID and unverified values labelled;
- `docs/reference/pin-resource-map.md`: a conflict-checked Teensy allocation;
- `docs/reference/two-layer-placement.md`: non-routing placement proof and the
  explicit triggers for the four-layer fallback.

The resource map fixes S/PDIF RX to Teensy pin 15 and S/PDIF TX to pin 14,
reserves the Teensy 4.1 six-pin native-Ethernet header, and routes external USB
only to the Teensy Micro-B connector through the internal cable. It reserves
Program, pin 13 LED, USB host, SDIO, one complete I2C bus, one complete SPI bus,
one UART pair, and one I2S/TDM interface until H1. Any conflict is a P1 failure,
not a deferred layout choice.

The placement proof uses the Hammond internal envelope, a board no larger than
100 mm by 100 mm, rear-adjacent optical/Ethernet/USB connectors, no vias in the
USB or Ethernet differential pairs, and a mostly continuous bottom ground
plane. It states numerical preliminary length budgets, cable-bend volume,
ground-plane interruptions, connector access, and every item still requiring
H2 field-solver or layout validation. It is explicitly not manufacturing data.

The cost wave has these exact outputs:

- `evidence/cost/functional-bom.csv` using `group`, `manufacturer`, `mpn`,
  `description`, `quantity_per_unit`, `supplier`, `supplier_sku`,
  `price_break`, `unit_price_gbp_ex_vat`, `extended_price_gbp_ex_vat`,
  `evidence_ids`, `confidence`, and `notes`;
- `evidence/cost/should-cost.csv` using `scenario`, `group`, `low_gbp_ex_vat`,
  `central_gbp_ex_vat`, `high_gbp_ex_vat`, `vat_gbp`, `freight_duty_gbp`,
  `landed_gbp`, `evidence_ids`, `confidence`, and `assumptions`;
- `evidence/cost/risk-register.csv` using `risk_id`, `category`, `description`,
  `cause`, `consequence`, `likelihood_1_5`, `impact_1_5`, `score`,
  `evidence_ids`, `mitigation`, `validation_gate`, `residual_likelihood`,
  `residual_impact`, and `status`;
- `evidence/cost/README.md`: arithmetic, currency, VAT, freight allocation,
  confidence, and risk-scoring rules.

All money is GBP. Foreign prices use the Bank of England daily rate for the
retrieval date and preserve the original amount and currency in `notes`.
UK VAT is 20%; landed totals state whether VAT is recoverable. Freight and any
duty are separate and allocated over three assembled prototypes. Low, central,
and high values are independently arithmetically reproducible. `confidence` is
`high` for current identical-MPN supplier evidence, `medium` for current
comparable evidence or a stated quantity extrapolation, and `low` for an
engineering allowance.

Risk score is likelihood times impact. The README defines each 1-through-5
level. A score of 15 or more blocks the next phase; 8 through 12 requires a
named mitigation and validation gate; 6 or less remains tracked. Residual
fields are filled only after the mitigation has evidence. The register must
cover DTS feasibility, real-time budget, memory-bank pressure, S/PDIF recovery,
USB servicing, two-layer signal integrity, optical electrical limits, native
Ethernet routing, power/backfeed, enclosure fit, assembly yield, sourcing, and
cost volatility.

```yaml
phase: P1
unit: component-behind-interface
seams: [evidence manifest, topology, functional BOM, risk register]
contract-artifact: spec-only
coordination: source-archive first; functional-topology and should-cost parallel after manifest validation
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
unit: test-surface
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
unit: component-behind-interface
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
unit: component-behind-interface
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
unit: phase-task
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
unit: component-behind-interface
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
unit: test-surface
seams: [USB capability protocol, target result manifest]
contract-artifact: both
fan-out:
  - name: target-runner
    owned-paths:
      - evidence/capability-results/p7-run.json
      - evidence/capability-results/p7-raw-results.jsonl
      - evidence/capability-results/p7-performance.csv
      - evidence/capability-results/p7-memory.json
      - evidence/capability-results/p7-crash-report.json
      - evidence/capability-results/p7-verdict.json
review-intensity: active
commit-checkpoint:
  green: [structurally complete full valid/malformed corpus and performance/memory records]
  branches:
    pass:
      command: python3 firmware/tools/device/run_capability.py --stage p7-gate --record evidence/capability-results/p7-verdict.json
      expected-exit: 0
      buildable: [measured passing Teensy capability report]
      parent-commit: "test(aius): approve Teensy DTS capability result"
    blocked:
      command: python3 firmware/tools/device/run_capability.py --stage p7-gate --record evidence/capability-results/p7-verdict.json
      expected-exit: 3
      expected-diagnostics: sorted exact AIUS-P7-BLOCKED lines only
      buildable: [complete measured Teensy capability report requiring software work]
      parent-commit: "test(aius): record blocked Teensy DTS capability result"
```

A clean-room delivery audit compares raw result artifacts against every threshold
before declaring a pass. `p7-verdict.json` has ordered keys `schema_version`,
`verdict`, `firmware_sha256`, `corpus_sha256`, `failed_gate_ids`, `notes`; its
verdict is `pass` or `blocked`, failed IDs are sorted and empty only for pass,
and it contains no timestamp. H1 accepts only a committed pass whose hashes
match the recorded run.

## P8: Dedicated eight-hour soak

Run on the recorded Teensy, USB cable, build-server port, firmware hash, and host-runner hash. Persist periodic telemetry and raw results. Any reset, missing sequence, CRC or protocol error, PCM mismatch, deadline miss, allocation, memory high-water growth, or unexpected crash report fails the soak. Automatic retry cannot convert a failure to green.

```yaml
phase: P8
unit: test-surface
seams: [soak manifest, periodic telemetry, final verdict]
contract-artifact: both
fan-out:
  - name: soak-runner
    owned-paths:
      - evidence/capability-results/p8-run.json
      - evidence/capability-results/p8-telemetry.csv
      - evidence/capability-results/p8-verdict.json
review-intensity: active
commit-checkpoint:
  green: [structurally complete eight-hour telemetry and clean-room result audit]
  branches:
    pass:
      command: python3 firmware/tools/device/run_capability.py --stage p8-gate --record evidence/capability-results/p8-verdict.json
      expected-exit: 0
      buildable: [passing final DTS capability decision]
      parent-commit: "test(aius): complete Teensy DTS capability gate"
    blocked:
      command: python3 firmware/tools/device/run_capability.py --stage p8-gate --record evidence/capability-results/p8-verdict.json
      expected-exit: 3
      expected-diagnostics: sorted exact AIUS-P8-BLOCKED lines only
      buildable: [complete soak evidence requiring software work]
      parent-commit: "test(aius): record blocked Teensy DTS soak"
```

## Deferred decisions

- Optical S/PDIF word integrity, simultaneous 48 kHz receive/transmit operation, clock recovery, drift, latency, and carrier gating belong to the hardware follow-on.
- Embedded Rust is evaluated after P7 against the stable C ABI and the actual i.MX RT1062 USB, Ethernet, DMA, and S/PDIF support gaps. No Rust migration occurs inside this gate.
- Product licensing, certification, and commercial distribution remain out of scope for this personal build. Preserve third-party notices and record provenance.
