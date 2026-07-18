# Aius

Aius is a personal, non-commercial Teensy 4.1 audio processor. It receives
optical S/PDIF from a television, classifies stereo PCM or IEC 61937 DTS Core,
decodes DTS Core, applies a custom 5.1-to-stereo downmix and EQ, and emits
48 kHz stereo PCM over TOSLINK to an existing offboard DAC.

The project is at the specify-and-experiment stage. The architecture, connector
set, state machine, and a functional BOM are locked; the firmware and KiCad
schematic are the active work. No PCB order is authorized by the current state of
this repository.

## Signal path

```
TOSLINK RX → S/PDIF capture → PCM/DTS classification → DTS Core decode
  → native-channel PCM → downmix/EQ → 48 kHz stereo PCM → gated TOSLINK TX
```

Carried on a socketed Teensy 4.1 with integrated Cliff TOSLINK RX/TX, a reset-safe
transmitter gate and diagnostic raw-bypass mux, native Teensy Ethernet + magjack,
a single rear USB-C 5 V input, keyed expansion headers, and a stock Hammond
1455N1201BK enclosure. Two-layer PCB is the hypothesis; four layers only if a
recorded electrical constraint forces it.

## Start here

1. [Hardware spec](docs/plans/aius-hardware-spec.md) — the buildable spec: locked
   electrical/safety decisions, what is on the board, and the lean build gates.
2. [Functional topology](docs/reference/functional-topology.md) — modes, blocks,
   flows, clocks, buffers, latency boundaries.
3. [Interfaces and states](docs/reference/interfaces-and-states.md) — connectors,
   controls, power domains, operating states, faults, transitions (the FMEA
   contract).
4. [DTS capability gate](docs/plans/aius-capability-gate.md) — the software-first
   experiment proving the Teensy can parse IEC 61937 and decode DTS Core within
   budget. This is the active near-term work.
5. [Cost model](docs/plans/aius-cost-model.md) — provisional should-cost envelope
   (~£95–155 per assembled unit); not a quotation.

The functional BOM is
[`evidence/design/provisional-components.csv`](evidence/design/provisional-components.csv);
the requirements each part satisfies are in
[`evidence/design/selection-policy.json`](evidence/design/selection-policy.json).

## Repository map

| Path | Purpose |
| --- | --- |
| `CLAUDE.md` | Aius-specific hardware/firmware conventions and release gates. |
| `docs/plans/` | Hardware spec, capability gate, cost model. |
| `docs/reference/` | Architecture views: topology and interfaces/states. |
| `evidence/design/` | Functional BOM (`provisional-components.csv`) and requirements policy. |
| `docs/reference/datasheets.md` | Datasheets, reference schematics, enclosure STEP — Git LFS. |
| `evidence/source-snapshots/` | Reviewable upstream text (PJRC S/PDIF source, Teensy pages, socket datasheets). |
| `firmware/` | PlatformIO firmware project (capability-gate work). |
| `electronics/` | KiCad schematic/PCB (created at the schematic gate). |
| `scripts/kicad.sh` | Wrapper for the pinned KiCad 10 CLI container. |
| `compose.yaml` | Digest-pinned, network-isolated KiCad 10 CLI environment. |

## Checkout prerequisites

Run all commands from the repository root. Install Git LFS, Python 3, Docker with
the Compose plugin (for the pinned KiCad CLI), and VS Code + the PlatformIO
extension for firmware work. Materialize the retained binary evidence:

```bash
git lfs install
# datasheets are not vendored in the public mirror; see docs/reference/datasheets.md
```

## KiCad environment

KiCad 10.0.4 is pinned by image digest and exposed through a persisted wrapper.
Every invocation uses an ephemeral `run --rm` container; the named cache volume
survives for performance. Confirm it from any working directory:

```bash
scripts/kicad.sh version
```

ERC and DRC commands are added to persisted scripts as the schematic and PCB land —
no GUI-only step is the sole way to reproduce a manufacturing result.

## Evidence

Datasheets and reference material live under `docs/reference/datasheets.md` (Git LFS) with
reviewable upstream text under `evidence/source-snapshots/`. These are public
material retained for personal, non-commercial research; do not republish the
bytes. This is a plain reference library — the heavyweight provenance/validation
machinery a previous iteration built around it was removed on 2026-07-18 (see the
[hardware spec provenance note](docs/plans/aius-hardware-spec.md#provenance-of-this-spec)).

## Working safely

- Never release or order manufacturing files because a reference schematic or
  should-cost estimate exists. Manufacturing stays gated behind the software
  capability proof, the schematic + fault-safe review, and two-layer feasibility.
- Treat the VKSDR designs as comparative evidence, not golden authority; Cliff and
  PJRC material define the optical and Teensy boundaries.
- Never reformat byte-exact source snapshots or modify binary evidence casually.
- Never stop, restart, or signal a process you did not spawn.
- Preserve unrelated work in the shared repository and use exact pathspecs for
  Aius commits.
