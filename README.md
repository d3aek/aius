# Aius

Aius is a personal, non-commercial Teensy 4.1 audio processor. It receives
optical S/PDIF from a television, classifies stereo PCM or IEC 61937 DTS Core,
decodes DTS Core when required, applies a custom 5.1-to-stereo downmix and EQ,
and emits 48 kHz stereo PCM over TOSLINK to an existing offboard DAC.

The project is still in its evidence and architecture milestone. The repository
contains reviewed plans, source evidence, machine-checked topology and component
contracts, acquisition tooling, and a reproducible KiCad environment. It does
not yet contain the PlatformIO firmware project, a manufacturing schematic, a
routed PCB, or a released manufacturing package.

No PCB order is authorized by the current state of this repository.

## Product topology

The locked signal path is:

`TOSLINK RX -> S/PDIF capture -> PCM/DTS classification -> DTS Core decode -> native-channel PCM -> custom downmix/EQ -> 48 kHz stereo PCM S/PDIF -> gated TOSLINK TX`

The carrier is planned around:

- a socketed Teensy 4.1;
- integrated Cliff-class TOSLINK receiver and transmitter;
- a reset-safe transmitter gate and diagnostic raw S/PDIF bypass mux;
- native Teensy Ethernet and an integrated magjack;
- rear USB-C for the sole 5 V input and USB device connection;
- an internal USB-A to Micro-B cable feeding the Teensy;
- keyed expansion for I2C controls, SPI, UART/debug, I2S/TDM, USB host, and
  GPIO/ADC;
- a stock Hammond 1455N1201BK enclosure;
- a two-layer PCB as a hypothesis, with four layers required only if the H2
  electrical feasibility gate proves two layers inadequate.

The generated [functional topology](docs/reference/functional-topology.md) and
[interface/state tables](docs/reference/interfaces-and-states.md) are the best
compact descriptions of the current architecture.

## Current project state

Completion notes inside the linked plans are authoritative. This summary is an
orientation aid.

| Milestone | State | Result |
| --- | --- | --- |
| P0 | Complete | Capability, hardware, cost, evidence, and process contracts are committed. |
| P1A | Complete | Evidence schemas, policies, validators, and the pinned KiCad runner exist. |
| P1B | Complete | Seed evidence was acquired, hashed, classified, and assembled. |
| P1C | Complete | Functional topology, interface/state contracts, provisional components, and the supplemental handoff are machine checked. |
| P1D0 | Complete | Per-request manufacturer/UK acquisition, unavailable publication, assembly, and lifecycle validation are implemented. |
| P1D1 | In progress | Supplemental evidence is landing as independently audited, serial acquisition waves. |
| P1D2 to P1E | Pending | Aggregate closure, electrical reference package, schematic reference, functional BOM, cost, and H0 release verdict. |
| Software P2 to P8 | Pending | Portable parser/decoder, PlatformIO target, USB proof, Teensy benchmark, and soak. |
| Hardware H1 to H5 | Pending | Manufacturing schematic, layer feasibility, PCB/quote, three-unit EVT, and full integration. |

The software capability gate is intentionally separate from PCB manufacture. A
positive Teensy result permits optical bench work, not a board order. A negative
result completes the capability milestone when it identifies the limiting
correctness, memory, compute, or toolchain constraint.

## What the completed work established

The work so far is more than a collection of links. It has converted the product
idea into machine-checked boundaries:

- The evidence system distinguishes golden manufacturer material, comparative
  references, normalized distributor observations, and unresolved assumptions.
  Every retained object is bound to its URL, retrieval time, media type, byte
  count, SHA-256, retention rule, and dependent claim.
- The topology defines processed PCM, processed DTS, diagnostic raw bypass,
  unsupported-muted, and fault-muted modes. Raw DTS cannot become the default
  output to the stereo DAC.
- Reset and failure behavior require the optical transmitter to be disabled and
  the processed path selected by hardware defaults until firmware takes control.
- Power is constrained to one rear USB-C-derived 5 V source. A second VIN/VUSB
  feed and uncontrolled expansion back-powering are forbidden.
- Clock domains, DMA/buffer ownership, overflow policy, connector roles,
  expansion behavior, and deferred latency measurements are explicit contracts.
- Provisional component candidates and their manufacturer/UK evidence requests
  are deterministic records rather than an informal BOM.
- Dynamic supplier data is accepted only when exact pages directly support it.
  Access failures produce explicit unavailable outcomes instead of guessed or
  substituted facts.
- KiCad and evidence workflows are persisted and tested. Independent adversarial
  review and hash-stable regression run at each acquisition-wave boundary.
- The enclosure, three-unit strategy, two-layer hypothesis, and initial landed
  cost envelope are committed without pretending that a quotation or PCB release
  exists.

## Start here

Read these in order when resuming the project:

1. [DTS capability gate](docs/plans/aius-capability-gate.md): the locked Teensy,
   decoder, PlatformIO, USB protocol, benchmark, and soak contract.
2. [Functional topology](docs/reference/functional-topology.md): generated
   operating modes, blocks, flows, clocks, buffers, and latency boundaries.
3. [Interfaces and states](docs/reference/interfaces-and-states.md): generated
   connector, control, power, fault, and transition truth tables.
4. [P1 evidence, topology, and cost plan](docs/plans/aius-p1-evidence-topology-cost.md):
   the implemented P1A through P1C history and the full H0 path.
5. [P1D supplemental reference plan](docs/plans/aius-p1d-supplemental-reference.md):
   the current execution plan, acquisition-wave completion notes, and pending
   electrical reference work.
6. [Hardware follow-on](docs/plans/aius-hardware-follow-on.md): the schematic,
   layer-count, DFM, quotation, EVT, and release gates.
7. [Prototype cost model](docs/plans/aius-cost-model.md): the current should-cost
   envelope and quote-refresh rules.

The cost model is not a quotation. Its current carrier-only planning envelope is
£95 to £155 per assembled unit, or £285 to £465 for three units. It excludes the
Teensy, existing DAC, speakers, external cables, panel machining, engineering,
fixtures, certification, and re-spins.

## Repository map

| Path | Purpose |
| --- | --- |
| `CLAUDE.md` | Aius-specific evidence, tooling, process, and release rules. |
| `docs/plans/` | Decision records, phase contracts, gates, rationale, and completion notes. |
| `docs/reference/` | Deterministically generated human-readable architecture views. |
| `evidence/design/` | Canonical topology, interfaces, component records, schemas, and contract copies. |
| `evidence/manifest/` | Source truth, requests, assumptions, acquisition oracle, and evidence policy. |
| `docs/reference/datasheets.md` | Retained binary manufacturer evidence tracked with Git LFS. |
| `evidence/source-snapshots/` | Byte-exact reviewable upstream text retained in normal Git. |
| `scripts/` | Acquisition, validation, rendering, contract builders, tests, and KiCad wrapper. |
| `compose.yaml` | Digest-pinned, network-isolated KiCad 10 CLI environment and cache volume. |
| `firmware/` | Planned PlatformIO project root; it does not exist yet. |
| `electronics/` | Planned KiCad source/package root; it does not exist yet. |

Generated files say so in their header. Edit their canonical JSON inputs and run
the renderer instead of editing generated Markdown or CSV directly.

## Checkout prerequisites

Use the repository root for all commands. Install:

- Git LFS;
- Python 3;
- Docker with the Compose plugin for the pinned KiCad CLI;
- VS Code and the PlatformIO extension before firmware work begins.

Materialize the retained binary evidence after cloning:

```bash
git lfs install
# datasheets are not vendored in the public mirror; see docs/reference/datasheets.md
```

The checked-out artifact bytes, not the LFS pointer text, must match the size and
SHA-256 recorded in their source manifests.

## Validation

The checks that are expected to pass during the active P1D1 collection are:

```bash
python3 scripts/validate_evidence.py --self-test
python3 scripts/test_supplemental_evidence.py
python3 scripts/validate_p1.py topology --stage handoff
```

The supplemental suite currently exercises the acquisition, unavailable,
assembly, and lifecycle contracts and takes several minutes.

Full checkout validation is intentionally not a P1D1 quick check. It remains
red until every supplemental request has a durable outcome and P1D2 assembles
the aggregate sources, assumptions, and clone boundaries. P1D2 then runs:

```bash
python3 scripts/validate_evidence.py
python3 scripts/validate_p1.py topology --stage supplemental-lifecycle
python3 scripts/validate_p1.py topology --stage handoff
```

The complete policy-test list and closure order are in the
[evidence manifest policy](evidence/manifest/README.md#runnable-validation).

Check deterministic review artifacts without rewriting them:

```bash
python3 scripts/render_p1.py functional-topology --check
python3 scripts/render_p1.py interfaces-and-states --check
python3 scripts/render_p1.py components --check
python3 scripts/render_p1.py supplemental-requests --check
```

Validate repository documentation with:

```bash
the repo tooling tool doc-index -- --validate-only
```

## Evidence workflow

Do not manually invent or substitute evidence outcomes. The immutable
[`supplemental-realization.json`](evidence/manifest/supplemental-realization.json)
defines each request's authority, exact URL, owned paths, artifact retention,
authorization, failure policy, and UK observation profile.

The shared engine is:

```bash
python3 scripts/seed_evidence.py supplemental --help
```

Its relevant operations are `acquire`, `record`, `unavailable`,
`check-fragment`, and `assemble`.

- Manufacturer evidence retains exact primary-source bytes. Binary files use
  Git LFS; reviewable HTML snapshots use normal Git.
- UK distributor pages are reduced to ordered normalized observations. Raw
  distributor page bytes are not retained.
- A current source means the fixed authority directly supported the recorded
  facts at the recorded UTC time.
- An unavailable UK source publishes a canonical open assumption and no
  commerce facts.
- Unavailable manufacturer evidence is a golden-source design blocker.
- Access-control bypasses, alternate product IDs, search-result inference, and
  part substitution are forbidden.

Source snapshots are hashed evidence. Preserve them byte-for-byte, including
upstream whitespace; do not run a formatter over them.

## KiCad environment

KiCad is free and open source. Aius pins KiCad 10.0.4 by image digest and exposes
it through a persisted wrapper. Every invocation uses an ephemeral Compose
container via `run --rm`; the named cache volume survives for performance.

Confirm the environment from any working directory:

```bash
scripts/kicad.sh version
```

Later ERC and DRC commands must also be added to persisted scripts. Do not make
an interactive GUI-only process the sole way to reproduce a manufacturing
result.

## Firmware capability milestone

The future firmware project root is `firmware/`, directly
openable with the VS Code PlatformIO extension. The locked target is a stock
Teensy 4.1 at 600 MHz with PlatformIO Core 6.1.19,
`platformio/teensy@5.1.0`, Arduino startup/upload support, and portable
C-compatible parser, decoder, arena, metrics, and PCM-sink interfaces.

The first decoder candidate is a minimal DTS Core extraction from `dcadec`. A
minimal FFmpeg native DTS Core extraction is a reviewed fallback only if the
first candidate fails portability, allocation, correctness, or performance. The
project must not import all of `libavcodec` or relax the gate thresholds.

USB Serial plus retained Teensy `CrashReport` diagnostics are sufficient for
the initial proof. Do not buy an offboard debugger unless repeated target
failures cannot produce a retained crash record, serial event, or bounded fault
location.

## Hardware release path

Manufacturing work remains gated in this order:

1. P1D and P1E close H0 with complete evidence, an electrical reference
   package, a costed functional BOM, and an eligible release verdict.
2. Software P7 proves the Teensy parser/decoder capability over USB.
3. H1 creates and adversarially reviews the manufacturing schematic.
4. H2 proves placement, return paths, routing, and 90 ohm USB / 100 ohm
   Ethernet feasibility on two layers, or records why four layers are required.
5. H3 completes PCB, ERC/DRC, mechanical, DFM, manufacturing outputs, and a
   current landed quotation.
6. H4 builds three EVT units: rework/bring-up, regression/A-B, and untouched
   spare or destructive-test unit.
7. H5 integrates optical I/O, DTS decode, DSP, control, Ethernet, enclosure,
   and release evidence.

Rear-panel machining is intentionally excluded from supplier work. The user can
machine it manually or use available CNC help. A vendor minimum of five bare
PCBs is acceptable, but only three assembled carriers are planned.

## Working safely

- Never release or order manufacturing files because a reference schematic or
  should-cost estimate exists.
- Never treat the VKSDR designs as golden authority; they are comparative
  evidence. Cliff and official manufacturer/PJRC material define the golden
  boundaries recorded by the manifests.
- Never reformat byte-exact source snapshots or modify binary evidence outside
  the acquisition workflow.
- Never stop, restart, signal, or otherwise interfere with a process unless you
  spawned that exact process.
- Preserve unrelated work in the shared repository and use exact pathspecs for
  Aius commits.
