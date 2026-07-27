---
title: Aius H1 schematic capture
type: plans
scope: aius
subtype: design
status: shipped
tags: [aius, hardware, kicad, schematic, erc]
updated: 2026-07-19
toc: true
---

# Aius H1 schematic capture

Capture the full electrical connectivity of the Aius carrier as an ERC-clean,
renderable KiCad 10 schematic. This is the first manufacturing-track engineering
artifact; it feeds H2 (two-layer feasibility) and H3 (layout). It does not
authorize any order. Inputs: the [hardware spec](aius-hardware-spec.md), the
[interfaces/states](../reference/interfaces-and-states.md) and
[topology](../reference/functional-topology.md) references, the
[`provisional-components.csv`](../../evidence/design/provisional-components.csv)
BOM, and the datasheets under `docs/reference/datasheets.md`.

## Approach — generated, not GUI-drawn

**The schematic is generated from a checked-in connectivity spec, built and
ERC-checked in the pinned KiCad 10 container, and rendered to PDF/SVG.** No
GUI-only step is the sole way to reproduce it (subtree `CLAUDE.md`). Rationale:

- **Connectivity by net label, not drawn wires.** Each component is placed on a
  grid; every pin gets a short stub + a net label. Connectivity is by label name,
  which is ERC-valid and sidesteps intractable programmatic wire-routing. The
  result renders as a readable, reviewable schematic.
- **The spec is the review surface.** A structured `connectivity.json` (or CSV)
  lists every component (ref, KiCad symbol, footprint, value/MPN) and every net
  (name → the pins on it). It diffs cleanly and is the pin-by-pin review artifact.
- **Symbols:** standard KiCad libraries (`Device`, `Connector*`, `Interface_USB`,
  `Regulator_*`, `power`) where they exist; a small custom `aius.kicad_sym` for
  the parts KiCad does not ship — Teensy 4.1 (as its header pinout), Cliff
  ORJ5/OTJ5, the Pulse magjack, and the JST GH headers.
- **Tooling:** `scripts/kicad.sh` (KiCad 10.0.4, digest-pinned). `kicad-cli sch
  erc` gates connectivity; `kicad-cli sch export pdf/svg` renders. A generator
  script owns spec → `.kicad_sch`; ERC + render are persisted commands.

## Sheets

Four hierarchical sheets, each a self-contained review unit:

1. **power-usb** — rear USB-C (CC1/CC2 5.1 kΩ pulldowns, VBUS, D+/D-, shield),
   USB ESD (`TPD2EUSB30A`), 5 V load-switch/protection (`TPS2553`), internal
   USB-A receptacle, the 90 Ω D+/D- net to USB-A, and the single protected-5 V
   rail. Sole power entry; no second VIN/VUSB.
2. **optical-spdif** — Cliff ORJ5 receiver → S/PDIF-RX to Teensy pin 15; Teensy
   S/PDIF-TX (pin 14) and the raw RX bypass into the mux (`SN74LVC1G3157`),
   through the TX gate (`SN74LVC1G125`), to the Cliff OTJ5 transmitter. Hardware
   defaults: processed path selected, TX held disabled. Decoupling.
3. **ethernet** — the Pulse magjack wired to the Teensy's post-PHY 6-pin MDI
   header (RX±/TX±/LED/GND) plus the 100 nF centre-tap cap. The DP83825 PHY is on
   the Teensy, not the carrier (see below), so this sheet is small.
4. **teensy-expansion** — the socketed Teensy 4.1, its pin allocation
   (RX=15, TX=14, reserved I2C/SPI/UART/I2S/USB-host/Program/LED/SDIO), the
   keyed expansion headers (I2C + `TCA9517A` isolator, SPI, UART, I2S/TDM,
   USB-host, GPIO/ADC), the Program/RTC service header, and the Tag-Connect
   bring-up point. Generic passives per the BOM.

## Design gaps H1 must resolve

H1 is real engineering, not mechanical generation; drawing the nets surfaces
questions the paper spec left open. Resolve each against the datasheet and record
the decision:

- **Ethernet PHY — CORRECTED (2026-07-19): no PHY on the carrier.** This was
  first mis-flagged as a gap (and a wrong "add the DP83825 on-board" decision was
  briefly recorded). The evidence is unambiguous: the Teensy 4.1 itself contains
  the Ethernet controller **and** the DP83825 PHY (PJRC teensy41 page: "Teensy 4.1
  contains an Ethernet controller and Ethernet PHY chip"; the spec table lists the
  DP83825 PHY under Teensy 4.1). Its 6-pin Ethernet header carries the **post-PHY
  MDI** signals (RX±, TX±, LED, GND); PJRC's Ethernet Kit is just a magjack + cap
  + cable, no PHY. So the carrier needs **only the magjack** (Pulse `J0011D21BNL`)
  + a 100 nF centre-tap cap — exactly the original BOM. The ethernet sheet wires
  the magjack to the Teensy MDI globals (`ETH_RXP/RXN/TXP/TXN/LED`); no on-carrier
  PHY, no RMII routing. (Confirm the user's Teensy 4.1 is an Ethernet-populated
  board.)
- **Teensy 4.1 pin map.** Fix the exact pad each net uses (the native-Ethernet
  6-pin header signals, I2S/TDM, SPI, I2C, UART, USB-host, Program, LED, SDIO)
  from the PJRC pinout card, conflict-checked.
- **USB-C pinout** for the selected receptacle (D+/D- pairs tied, CC1/CC2
  independent 5.1 kΩ, VBUS, GND, shield); confirm the final USB-C MPN (the BOM
  candidate vs the Würth `629722000214` from the receptacle search).
- **Optical logic levels/thresholds** already captured in the spec (ORJ5 2.4 V
  V_OH, OTJ5 2.0 V V_IH); confirm the mux/gate supply and any series/pullups.

## Milestones

- **H1a — pipeline proof. *Done.*** Custom symbol library + generator + the
  `power-usb` sheet, ERC 0/0, rendered. Proved spec → `.kicad_sch` → ERC → render.
- **H1b — remaining sheets. *Done.*** teensy-expansion (Teensy symbol + the
  conflict-checked pin map + shared-net convention), optical-spdif, and ethernet.
  Whole-project ERC 0/0. TX gate simplified to a single `SN74LVC1G126`
  (active-high OE) during review — no inverter.
- **H1c — full review. *Done (2026-07-19).*** Whole-schematic ERC 0/0. Fault-safe
  review confirmed: single USB-C power entry (no second VIN/VUSB; shield on its
  own CHASSIS net); the reset defaults hold in hardware before firmware runs —
  `PATH_SELECT` 10k pulldown selects the processed mux path and `TX_ENABLE` 10k
  pulldown drives the `1G126` gate OE low → TX high-Z → OTJ5 idle; pin map
  conflict-free (SPI on SPI1 to avoid the pin-13 LED/SCK0 clash). Rendered SVGs
  committed. **H1 complete — ready for H2.**

Remaining for H2 (not H1): footprint/land-pattern verification (the provisional
footprints are the only full-severity ERC warnings), placement, impedance
(90 Ω USB / 100 Ω Ethernet), routing, DRC. Open item to confirm with the user:
the Teensy 4.1 is an Ethernet-populated board.

**Optical-jack footprint gate (added 2026-07-27):** the purchased optical
modules are the RS PRO ORJ-5 (`805-1671`) / OTJ-5 (`805-1680`), not the Cliff
parts the `aius:Cliff_ORJ5_TOSLINK` / `aius:Cliff_OTJ5_TOSLINK` footprints were
drawn from. Before any board order, validate the Gerber against the RS PRO PCB
layout drawings (page 4 of each datasheet in
`docs/reference/datasheets.mdAIUS-OPTICAL-{ORJ5,OTJ5}-RS/`): 3 × Ø0.8 mm signal holes on
2.54 mm pitch, 3 × Ø2.0 mm peg holes, 9.20 × 6.00 × 3.90 mm geometry, 1.6 mm
substrate assumed. Sourcing record:
[aius-optical-prototype.md](aius-optical-prototype.md#hardware-prototype-scope).

## Not in H1

Footprint/land-pattern verification depth, placement, routing, impedance, and
DRC are H2/H3. H1 assigns a footprint per part (for the later netlist) but does
not lay out or route.
