---
title: Aius hardware spec
type: plans
scope: component
component: aius
subtype: design
status: in-progress
tags: [aius, hardware, teensy, spdif, pcb, kicad]
updated: 2026-08-04
toc: true
---

# Aius hardware spec

The buildable specification for the Aius carrier board: a socketed Teensy 4.1 that
receives optical S/PDIF, classifies PCM vs IEC 61937 bursts, decodes DTS Core /
AC-3 / E-AC-3, downmixes 5.1 to stereo with EQ, and re-emits stereo over optical
TOSLINK.

This document is the single hardware source of truth. It supersedes the retired
evidence/audit plans (`aius-p1-evidence-topology-cost`,
`aius-p1d-supplemental-reference`, `aius-hardware-follow-on`,
`aius-critical-receptacle-replacements`) — see [Provenance](#provenance-of-this-spec).
It draws the concrete architecture, connectors, and state machine from the two
generated-then-frozen reference docs and the BOM, all of which remain checked in:

- [functional topology](../reference/functional-topology.md) — modes, blocks, data/control/power flows, clock domains, DMA buffers, latency boundaries.
- [interfaces and states](../reference/interfaces-and-states.md) — connectors, controls, power domains, operating states, fault scenarios, transitions.
- [`provisional-components.csv`](../../evidence/design/provisional-components.csv) — the functional BOM (MPN, datasheet URL, requirement mapping, generic passive values).
- [`selection-policy.json`](../../evidence/design/selection-policy.json) — the engineering policies and per-family requirements the BOM parts must satisfy.

Downloaded datasheets live under `docs/reference/datasheets.md` (Git LFS); reviewable
source (PJRC `input_spdif3.cpp` / `output_spdif3.cpp`, Teensy pages, socket
datasheets) under `evidence/source-snapshots/`.

## Signal path

```text
TOSLINK RX → S/PDIF capture → PCM/IEC 61937 classification
  → DTS Core / AC-3 / E-AC-3 decode → native-channel PCM → downmix/EQ
  → stereo PCM → gated TOSLINK TX
```

Five operating modes: `pcm-processed`, `dts-processed`, `diagnostic-raw-bypass`,
`unsupported-muted`, `fault-muted`. Raw compressed data never reaches the stereo
DAC — the bypass mux defaults to the processed path and is a diagnostic aid only.
(The decoder set outgrew the original DTS-only scope: the running firmware also
decodes AC-3 and E-AC-3 — [`aius-ac3-support.md`](aius-ac3-support.md),
[`aius-eac3-support.md`](aius-eac3-support.md) — and live output follows the
source rate at 32/44.1/48 kHz. The reference tables' mode ids keep their
original DTS-era names.)

## Locked architecture decisions

These are the electrical and safety choices the schematic must honour. Each maps
to an `EP-*` policy or `REQT-*` requirement in `selection-policy.json`.

- **Single power entry.** Rear USB-C VBUS is the sole normal 5 V source. It passes
  unchanged through the internal USB-A→Micro-B cable to the Teensy. No second
  VIN/VUSB feed; no expansion rail may back-power the core.
  (`EP-PRODUCT-SINGLE-POWER-ENTRY`.)
- **Power topology.** `teensy-3v3` feeds native Ethernet and the *input* of the
  expansion current-limit / reverse-blocking protection. `expansion-3v3` is the
  protected *output* derived from `teensy-3v3`, not a rail off `protected-5v`.
  RTC backup is isolated and cannot power the core.
- **Optical logic domain.** Receiver, mux, gate, transmitter run at 3.3 V logic.
  Account for ORJ5 min V_OH 2.4 V and OTJ5 min V_IH 2.0 V; require ≥16 Mbit/s
  logic bandwidth on the S/PDIF path.
- **Safe optical defaults.** Hardware pulls hold the TX gate *disabled* and select
  the *processed* path through absence, reset, boot, switching, brownout, and
  watchdog recovery — before firmware takes control. (`EP-SAFE-OPTICAL-DEFAULTS`.)
- **Safe unpowered expansion.** An unpowered expansion sustains no rail; exported
  signals are high-impedance or current-limited. (`EP-SAFE-EXPANSION-UNPOWERED`.)
- **Pin allocation.** S/PDIF RX = Teensy pin 15, TX = pin 14. Keep usable: the
  native-Ethernet header, Program pin, pin-13 LED, USB host, SDIO, one I2C, one
  SPI, one UART pair, one I2S/TDM.
- **Two-layer hypothesis.** Route on two layers with 90 Ω differential USB and
  100 Ω differential Ethernet against the fabricator's real stackup; USB/Ethernet
  pairs on top with no vias, continuous bottom ground under both pairs. Escalate
  to four layers only on a recorded electrical failure that four layers resolves —
  a mechanical failure never clears via layer count.
- **Mechanical.** Board ≤ 100 × 100 mm, FR-4, 1.6 mm, 1 oz copper, inside a stock
  Hammond 1455N1201BK extruded enclosure. Rear-panel machining is done by hand /
  external CNC, excluded from board fab. Rear-adjacent optical, Ethernet, and
  USB-C connectors.

## What is on the board

Full net-level detail is in [interfaces and states](../reference/interfaces-and-states.md);
the selected parts are in the BOM. Summary:

- Socketed Teensy 4.1 (Samtec `SSW-124-02-G-S` side sockets, `MMS-103-01-L-DV`
  Ethernet header socket, `SSW-105-02-G-S` USB-host socket).
- Optical: RS PRO ORJ-5 receiver (RS `805-1671`) and OTJ-5 transmitter (RS
  `805-1680`) — Cliff `FCR684205R`/`FCR684205T` class, sourced from RS PRO for
  availability (2026-07-27); pin-compatible with the schematic symbols, same
  EIAJ CP-1201 / 16 Mbps / V_OH 2.4 / V_IH 2.0 figures. Gerber must be
  validated against the RS PRO footprint drawings before order
  ([sourcing record](aius-optical-prototype.md#hardware-prototype-scope)).
- S/PDIF **bypass mux** TI `SN74LVC1G3157DBVR`; **TX gate** TI `SN74LVC1G126DBVR`
  (active-high OE, so `TX_ENABLE` default-low = TX disabled with no inverter — set at H1).
- Rear USB-C receptacle GCT `USB4105-GF-A` (captured at H1; see
  [open hardware questions](#open-hardware-questions)),
  independent CC1/CC2 5.1 kΩ pulldowns, USB ESD TI `TPD2EUSB30ADRTR`,
  power protection TI `TPS2553DBVR`, 90 Ω D+/D- to an internal USB-A receptacle
  (candidate TE `292303-1`), StarTech `UUSBHAUB6IN` internal cable to the Teensy.
- Native Ethernet magjack Pulse `J0011D21BNL`, 100 nF centre-tap cap.
- Expansion (keyed JST GH `BM0xB-GHS-TBT`): I2C control (+ TI `TCA9517A` isolator),
  SPI (6-circuit), UART/debug, I2S/TDM, USB host, GPIO/ADC. A trigger/relay output
  is an allocation of the GPIO/ADC header, not a separate connector.
- Service: microSD access, remote Program/RTC header, status LED, Tag-Connect
  `TC2030-IDC` test/bring-up point, HellermannTyton `151-30300` cable retainer.
- Generic passives (provisional, 0402): CC pulldown 5.1 kΩ 1%, logic decoupling
  100 nF 10%, control pulldown 10 kΩ 1%, Ethernet centre-tap 100 nF 10%, expansion
  series resistor 1 kΩ 1%.

## Reset and fault behaviour

The [interfaces and states](../reference/interfaces-and-states.md) operating-state,
fault-scenario, and transition tables are the FMEA contract for the schematic.
Load-bearing rule: hardware pulls establish safe outputs (TX disabled, processed
path, expansion safe) with the Teensy absent, held in reset, booting, or crashed;
firmware arms the watchdog *before* TX may be enabled; path selection changes only
while TX is muted.

## Build gates (lean)

Manufacturing stays gated, but without the retired evidence/audit apparatus. The
order is engineering-driven:

1. **Software capability — *passed* (P6 on-device validation 2026-07-21).** The
   Teensy parses IEC 61937 and decodes DTS Core within budget; record at
   [`evidence/capability-results/p6-hidraw-validated.md`](../../evidence/capability-results/p6-hidraw-validated.md),
   plan at [capability gate](aius-capability-gate.md). The authorised optical
   bench work is now running ([optical prototype](aius-optical-prototype.md)).
2. **Schematic (H1) — *done* (2026-07-19,
   [plan shipped](aius-h1-schematic.md)).** `electronics/aius-carrier.kicad_sch`
   captured as a generated four-sheet hierarchical project in the pinned KiCad
   container, whole-project ERC 0/0, power-path and fault-safe review recorded.
3. **Two-layer feasibility (H2)** — obtain the fabricator's stackup, compute
   90/100 Ω geometry, prove continuous return paths and placement inside the
   enclosure. Record the two-vs-four-layer verdict.
4. **PCB + quote (H3)** — route, ERC/DRC/DFM clean, generate the manufacturing
   package, get a real quote, refresh the [cost model](aius-cost-model.md).
5. **EVT (H4)** — build three units; bring-up in the order visual/continuity →
   power/rails/thermal → reset-safe states → USB → Ethernet → optical loopback →
   raw bypass → simultaneous S/PDIF RX/TX → recovery.
6. **Integration (H5)** — full-path audio: word integrity, clock recovery/drift,
   buffering, underrun/overrun, end-to-end latency, carrier gating, jitter.

Each gate is a normal engineering review (green ERC/DRC, a diff review, a bring-up
checklist) — not a hash-bound verdict schema. Don't order a board because a
reference schematic or should-cost estimate exists; order after H2 + a real H3
quote.

## Open hardware questions

All three were resolved as planned:

- **USB-A / USB-C receptacles — resolved at H1.** The captured schematic uses
  the original candidates: GCT `USB4105-GF-A` for USB-C and TE `292303-1` for
  the internal USB-A (`electronics/connectivity/power-usb.json`); the Würth
  `629722000214` alternative was not adopted.
- **DTS decoder feasibility — proven** by the software gate (P6, 2026-07-21).
- **Latency and memory budgets** are measured on the running bench prototype,
  not locked in this spec: per-bank memory reports gate every build
  (`firmware/scripts/build.sh`), and end-to-end latency is being measured under
  [`aius-e2e-latency.md`](aius-e2e-latency.md).

## Provenance of this spec

This spec is a deliberate de-ceremony consolidation (2026-07-18). The prior agent
built a ~25 k-line Python evidence-validation system (`seed_evidence`,
`validate_evidence`, `validate_p1`, `build_p1c_*`, `render_p1`) plus 726 fixtures
and ~9.6 k lines of plan docs, ~80 % of which was process ceremony (SHA-256
manifests, golden/dynamic source taxonomy, clone-boundary prose, 16 audited
acquisition waves, one-way document lifecycles, hash-locked contracts,
purchase-authorization rituals). That machinery blocked the project on two
freely-available USB-connector datasheet URLs before any schematic or firmware
existed.

Removed: the validation/provenance machinery and the four ceremony plans.
Preserved: the datasheets, the BOM, the two architecture reference docs (now
hand-authored), the requirements policy, and the reproducible KiCad tooling —
i.e. every artifact with forward value for actually building the board.
