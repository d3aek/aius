# Aius

Aius is a personal, non-commercial Teensy 4.1 audio processor. It receives
optical S/PDIF from a television, classifies stereo PCM or IEC 61937 bursts,
decodes DTS Core, AC-3 and E-AC-3 (DD+), applies a custom 5.1-to-stereo downmix
and EQ, and re-emits stereo PCM over TOSLINK to an existing offboard DAC.

The firmware runs on bench hardware: the socketed Teensy 4.1 is wired to the
purchased RS PRO TOSLINK jacks on a proto board
([wiring guide](docs/guides/proto-optical-wiring.md)), taking a real optical
source and driving a real optical sink. The DTS capability gate passed on the
device ([P6 record](evidence/capability-results/p6-hidraw-validated.md)). The
carrier-board KiCad schematic is captured and ERC-clean
([H1 shipped](docs/plans/aius-h1-schematic.md)); two-layer feasibility (H2) and
layout (H3) have not started. No PCB order is authorized by the current state of
this repository.

## Signal path

```text
TOSLINK RX → S/PDIF capture → PCM / IEC 61937 classification
  → DTS Core / AC-3 / E-AC-3 decode → native-channel PCM → downmix/EQ
  → stereo PCM → gated TOSLINK TX
```

Live output follows the source rate (32 / 44.1 / 48 kHz); replay is exactly
48 kHz. Around the audio path the `teensy41-optical` image serves plain HTTP on
port 80 (no TLS) and a UDP control socket on 4242, a USB CDC control protocol,
an SD-hosted control surface (status, DSP editor, replay manager, upgrade page),
Ed25519-signed staged OTA upgrade, and a PSRAM audio flight recorder — all
inside a watchdog-fed bounded epoch runtime with a sticky recovery-safe mode.
[`firmware/README.md`](firmware/README.md) is the firmware map.

The carrier board design: a socketed Teensy 4.1 with Cliff-class TOSLINK RX/TX
(the purchased parts are RS PRO ORJ-5/OTJ-5), a reset-safe transmitter gate and
diagnostic raw-bypass mux, native Teensy Ethernet + magjack, a single rear
USB-C 5 V input, keyed expansion headers, and a stock Hammond 1455N1201BK
enclosure. Two-layer PCB is the hypothesis; four layers only if a recorded
electrical constraint forces it.

## Start here

1. [Hardware spec](docs/plans/aius-hardware-spec.md) — the buildable spec:
   locked electrical/safety decisions, what is on the board, and the build-gate
   ladder (software capability and H1 done, H2 next).
2. [Signal path](docs/reference/signal-path.md) — what the running image's
   audio path is, stage by stage, with cited buffers and budgets.
3. [Firmware I/O reference](docs/reference/firmware-io.md) — every I/O the
   image performs and the argument it cannot disturb the audio path.
4. [Interfaces and states](docs/reference/interfaces-and-states.md) —
   connectors, controls, power domains, operating states, faults, transitions
   (the FMEA contract).
5. [Open-work register](docs/plans/aius-open-work-register.md) — every open
   item and defect in one place.
6. [Cost model](docs/plans/aius-cost-model.md) — provisional should-cost
   envelope (~£95–155 per assembled unit); not a quotation.

The functional BOM is
[`evidence/design/provisional-components.csv`](evidence/design/provisional-components.csv);
the requirements each part satisfies are in
[`evidence/design/selection-policy.json`](evidence/design/selection-policy.json).

## Repository map

| Path | Purpose |
| --- | --- |
| `CLAUDE.md` | Aius-specific hardware/firmware conventions and release gates. |
| `docs/plans/` | Hardware spec, capability gate, cost model, plus the firmware feature and defect-campaign plans (OTA, AC-3/E-AC-3, latency, sample-rate following, network hardening, open-work register). |
| `docs/reference/` | Architecture and firmware references: topology, interfaces/states, signal path, firmware I/O, USB control protocol, board floorplan, CM6206 characterization. |
| `docs/guides/` | Bench guides (proto-board optical wiring). |
| `evidence/design/` | Functional BOM (`provisional-components.csv`) and requirements policy. |
| `docs/reference/datasheets.md` | Datasheets, reference schematics, enclosure STEP — Git LFS. |
| `evidence/source-snapshots/` | Reviewable upstream text (PJRC S/PDIF source, Teensy pages, socket datasheets). |
| `evidence/capability-results/` | Capability-gate pass records (P6 RawHID validation). |
| `firmware/` | PlatformIO firmware: portable codec/DSP/runtime libs, the `teensy41-optical` image, host test suites, bench tools. |
| `electronics/` | Generated KiCad schematic project (H1, ERC-clean) + connectivity specs. |
| `test/sd-editor/` | Vitest regressions for the SD-hosted web pages in `firmware/sd/www/`. |
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

ERC and render are persisted commands (`electronics/scripts/erc.sh`,
`electronics/scripts/render.sh`); DRC joins them when the PCB lands. No GUI-only
step is the sole way to reproduce a manufacturing result.

## Evidence

Datasheets and reference material live under `docs/reference/datasheets.md` (Git LFS) with
reviewable upstream text under `evidence/source-snapshots/`. These are public
material retained for personal, non-commercial research; do not republish the
bytes. This is a plain reference library — the heavyweight provenance/validation
machinery a previous iteration built around it was removed on 2026-07-18 (see the
[hardware spec provenance note](docs/plans/aius-hardware-spec.md#provenance-of-this-spec)).

## Working safely

- Never release or order manufacturing files because a reference schematic or
  should-cost estimate exists. Manufacturing stays gated behind the remaining
  hardware gates: two-layer feasibility (H2, including validating the footprints
  against the RS PRO optical-jack drawings) and a routed, DRC-clean package with
  a real quote (H3).
- Treat the VKSDR designs as comparative evidence, not golden authority; Cliff and
  PJRC material define the optical and Teensy boundaries.
- Never reformat byte-exact source snapshots or modify binary evidence casually.
- Never stop, restart, or signal a process you did not spawn.
- Preserve unrelated work in the shared repository and use exact pathspecs for
  Aius commits.
