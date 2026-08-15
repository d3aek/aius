---
title: Aius H2/H3 manufacturing readiness
type: plans
scope: component
component: aius
subtype: design
status: in-progress
tags: [aius, hardware, kicad, pcb, layout, impedance, dfm, manufacturing]
updated: 2026-08-15
toc: true
---

# Aius H2/H3 manufacturing readiness

Take the carrier from an ERC-clean schematic (H1, shipped) to an **order-ready
manufacturing package**: feasibility proven (H2), then footprints, layout,
impedance-controlled routing, DRC/DFM-clean Gerbers, an independent review, and a
real fabricator quote (H3). This plan authorises no order by itself — ordering is
an operator action after this plan's gates pass, per
[hardware spec §Build gates](aius-hardware-spec.md#build-gates-lean) and the
subtree [`CLAUDE.md`](../../CLAUDE.md) hardware release gates.

Inputs: the [hardware spec](aius-hardware-spec.md) (source of truth), the
[H1 schematic plan](aius-h1-schematic.md) (shipped), the ERC-clean project under
[`electronics/`](../../electronics/README.md), the
[connectivity spec](../../electronics/connectivity/nets.md), the
[board floorplan](../reference/board-floorplan.md) concept, the filed datasheets
and STEP models under `docs/reference/datasheets.md`, and the
[cost model](aius-cost-model.md).

## Operator decisions (2026-08-15)

Recorded from the session that opened this plan; they set scope and the stackup
inputs:

- **Fabricator: quote-compare PCBWay and JLCPCB.** H2 computes impedance geometry
  against both fabs' documented 1.6 mm stackups; H3 prepares a manufacturing
  package for the chosen fab and a real quote from each. The cost model is
  re-anchored to whichever wins.
- **Layer count: decided after H2 numbers.** The spec carries a two-layer
  hypothesis; H2a computes the real 90 Ω USB / 100 Ω Ethernet geometry for 2- and
  4-layer stackups and presents the manufacturability + signal-integrity verdict.
  The 2-vs-4 decision is an explicit gate at the end of H2 (H2d), owner-made with
  the numbers in hand.
- **Layout scope: full manufacturing package.** Build the footprint library,
  place, route to impedance-controlled DRC/DFM-clean, generate Gerber/drill/BOM/CPL
  + STEP/PDF, obtain an independent design review, and reach order-ready. The
  physical order (account, upload, payment) stays with the operator.

## Gate ladder

H2 and H3 sit between H1 (done) and EVT (H4). This plan owns H2 + H3.

| Gate | State | Owned here |
|---|---|---|
| Software capability | passed (P6) | no |
| H1 schematic | shipped | no |
| **H2 feasibility** | this plan | yes |
| **H3 layout + package + quote** | this plan | yes |
| Independent design review | H3e | yes |
| Order | operator, post-H3 | no |
| H4 EVT / H5 integration | future | no |

## Phase H2 — feasibility (authorises no order)

Prove the board is buildable on a real stackup inside the enclosure before any
copper is drawn. No `.kicad_pcb` is routed in H2.

### Phase H2a — impedance and stackup feasibility ✓ SHIPPED 2026-08-15

```yaml
outcome: A checked-in impedance calculator and fab stackup inputs produce the
  trace-width/spacing geometry needed for 90 Ω differential USB and 100 Ω
  differential Ethernet on PCBWay and JLCPCB, for both 2-layer and 4-layer 1.6 mm
  stackups, with a manufacturability assessment against each fab's minimum
  trace/space rules.
seams: electronics/feasibility/ (new) — stackups.json inputs + impedance.py
  calculator; no change to the schematic or connectivity spec.
targeted-proof: impedance.py reproduces a published controlled-impedance stackup's
  rated differential impedance within a stated tolerance (sanity anchor), and the
  emitted geometry table is cross-checked against each fab's own impedance
  guidance; unit values verified against an independent closed-form reference.
unit: parent-only
```

The load-bearing computation: on 1.6 mm FR-4 the 2-layer reference plane sits
~1.5 mm below the signal, so controlled-impedance differential geometry is
expected to be impractically wide — H2a produces the actual numbers rather than
asserting it. Uses IPC-2141 / Wadell closed-form differential-microstrip
equations (feasibility grade; final impedance confirmed against the fab's
controlled-impedance service at H3c).

**Result (2026-08-15).** The checked-in calculator
([`electronics/feasibility/impedance.py`](../../electronics/feasibility/impedance.py)
over [`stackups.json`](../../electronics/feasibility/stackups.json); self-test
anchor passes at 106.8 Ω vs JLCPCB's rated 100 Ω) gives an unambiguous, fab-
independent verdict:

| Stackup | 90 Ω USB geometry | 100 Ω Ethernet geometry | Verdict |
|---|---|---|---|
| PCBWay 2-layer 1.6 mm | w ≈ 1.15 mm | w ≈ 0.89 mm | **not feasible** |
| JLCPCB 2-layer 1.6 mm | w ≈ 1.13 mm | w ≈ 0.87 mm | **not feasible** |
| PCBWay 4-layer 1.6 mm | w ≈ 0.20–0.28 mm | w ≈ 0.16–0.23 mm | **feasible** |
| JLCPCB 4-layer 1.6 mm (JLC04161H-7628) | w ≈ 0.20–0.28 mm | w ≈ 0.16–0.23 mm | **feasible** |

On **2-layer** the only reference plane is the opposite outer layer ~1.53 mm
away, so controlled-impedance pairs need ~0.9–1.15 mm traces — impractical on a
100 × 100 mm board carrying a Teensy. A narrow, routable 0.15/0.15 mm pair
measures ~163–164 Ω differential, **+63 % to +83 %** off target: uncontrolled.
On **4-layer** the L2 ground plane sits ~0.2 mm below L1, giving standard,
routable 0.16–0.28 mm geometry with margin on both fabs' standard process. This
is a documented **electrical** failure of the two-layer hypothesis (impedance),
the exact escalation trigger the spec names. The 2-vs-4 decision is the owner gate
at H2d; the electrical evidence points to **4-layer**.

### Phase H2b — return-path and signal-integrity feasibility ✓ SHIPPED 2026-08-15

```yaml
outcome: For each viable stackup, a recorded analysis that the USB and Ethernet
  differential pairs can be routed over a continuous, unbroken reference plane
  with no plane splits or via stitching gaps under the pairs, plus decoupling
  adequacy and the S/PDIF ≥16 Mbit/s logic-path check.
seams: docs — this plan's H2b section + the feasibility inputs from H2a; no source
  change.
targeted-proof: a documented reference-plane continuity argument per pair against
  the chosen stackup and the H2c placement, with the specific failure modes
  (plane split, connector-transition discontinuity) enumerated and cleared.
unit: parent-only
```

**Result (2026-08-15).** With the 4-layer stackup fixed at H2d, return paths are
continuous **by construction**: layer assignment is **L1 signal / L2 solid GND /
L3 GND+power islands / L4 signal**. Both differential pairs route on an outer
signal layer directly over the adjacent solid GND plane (USB D+/D- and Ethernet
MDI on L1 over L2), so every pair has an unbroken image plane ~0.2 mm beneath it
and no pair crosses a plane split. Failure modes cleared: (a) **plane split under
a pair** — none, L2 is a single unbroken GND pour and power lives on L3 islands
away from the pair footprints; (b) **connector-transition discontinuity** — the
USB-C and magjack pads sit at the board edge with GND stitching vias flanking each
pair's break-in, keeping the return local across the pad transition. Decoupling is
the BOM's 100 nF-per-IC set plus the magjack centre-tap 100 nF; the single-ended
S/PDIF path (3.3 V logic, ≥16 Mbit/s) is a short L1 run over L2 GND, far inside
the FR-4 bandwidth. This is a feasibility argument; final continuity is enforced
by DRC/pour review at H3c.

### Phase H2c — mechanical and placement feasibility

```yaml
outcome: A dimensioned board outline (≤100×100 mm) and connector/module placement
  that fits the Hammond 1455N1201BK extruded enclosure — rear-panel optical/USB-C/
  Ethernet/Program alignment, Teensy socket stack height and microSD access
  clearance, expansion-header and Tag-Connect reach — validated against the Hammond
  mechanical drawing and the filed AP214 STEP models.
seams: docs/reference — refine board-floorplan into a mechanically-constrained
  outline; docs/reference/datasheets.md for the Hammond drawing.
targeted-proof: outline + connector datums checked against the enclosure internal
  dimensions and rail/slot geometry; no connector-to-wall or module-to-lid
  collision; rear-panel cutout positions consistent across the three rear parts.
unit: parent-only
```

### Phase H2d — layer verdict and cost refresh (owner gate) ✓ SHIPPED 2026-08-15

```yaml
outcome: A consolidated 2-vs-4-layer recommendation with the recorded electrical
  reason (per the escalation rule), presented to the owner for the decision; the
  cost model refreshed with the pre-layout estimate for the chosen path on both
  fabs.
seams: docs/plans — this plan's verdict + aius-cost-model.md pre-layout row.
targeted-proof: the verdict cites H2a geometry and H2b/H2c constraints; the cost
  refresh reflects the layer choice and both fabs' current stackup pricing.
unit: parent-only
```

**Verdict (2026-08-15): 4-layer.** The owner escalated from the two-layer
hypothesis to 4-layer on the H2a electrical evidence — 2-layer 1.6 mm cannot
provide manufacturable 90 Ω USB / 100 Ω Ethernet geometry (needs ~0.9–1.15 mm
traces; a routable pair reads ~163 Ω, +63–83 % off) and offers no dedicated
return plane, both independent electrical failures that 4-layer resolves. Per the
escalation rule this is a recorded electrical reason, not a mechanical one.

**Stackup lock.** Route against the JLC04161H-7628 / PCBWay 4-layer 1.6 mm class:
L1 signal / L2 solid GND / L3 GND+power / L4 signal, L1→L2 ≈ 0.20–0.21 mm prepreg.
Design-target impedance geometry (from H2a, JLCPCB 0.2104 mm): **USB 90 Ω** at
w ≈ 0.20 mm / s ≈ 0.15 mm; **Ethernet 100 Ω** at w ≈ 0.20 mm / s ≈ 0.15 mm. The
two fabs' prepreg differs by ~0.01 mm, well inside tolerance, so one routed
geometry quotes on both and is re-confirmed against each fab's controlled-impedance
service at H3c. Cost model refreshed to the 4-layer pre-layout estimate on both
fabs ([aius-cost-model.md](aius-cost-model.md)).

## Phase H3 — layout, package, quote (order-ready)

Only starts once H2d fixes the stackup and layer count.

### Phase H3a — footprint library

```yaml
outcome: A complete, ERC-clean aius footprint library — .pretty with a .kicad_mod
  per part and an fp-lib-table — resolving all 13 footprint_link_issues to 0. The
  RS PRO ORJ-5/OTJ-5 land patterns are built and validated against the datasheet
  PCB drawings; the two open receptacle MPNs (USB-A TE 292303-1, USB-C GCT
  USB4105-GF-A) are confirmed and footprinted.
seams: electronics/lib/aius.pretty (new) + fp-lib-table (new); footprint strings
  already assigned in the schematic; provisional-components.csv status updates.
targeted-proof: full-severity ERC (scripts/erc.sh --all) reports 0
  footprint_link_issues; each custom land pattern's pad/hole/courtyard dimensions
  match its datasheet (optical: 3×Ø0.8 mm signal on 2.54 mm pitch, 3×Ø2.0 mm pegs,
  9.20×6.00×3.90 mm), verified pad-by-pad.
unit: parent-only
```

### Phase H3b — netlist and placement

```yaml
outcome: A .kicad_pcb with the netlist imported, board outline + mounting per H2c,
  and every component placed per the floorplan with clean courtyards and no
  collisions; keepouts and rear-panel datums set.
seams: electronics/aius-carrier.kicad_pcb (new); netlist export from the schematic.
targeted-proof: netlist imports with no unconnected/extra references; courtyard
  DRC clean; placement matches H2c datums within tolerance.
unit: parent-only
```

### Phase H3c — impedance-controlled routing

```yaml
outcome: Fully routed board on the H2d stackup — USB 90 Ω and Ethernet 100 Ω pairs
  on the geometry from H2a with continuous return paths, length/skew matched;
  copper pours and stitching; all nets routed.
seams: electronics/aius-carrier.kicad_pcb.
targeted-proof: pair geometry matches the H2a table on the chosen stackup;
  reference-plane continuity under every pair confirmed; no unrouted nets.
unit: parent-only
```

### Phase H3d — DRC/DFM clean + manufacturing package

```yaml
outcome: DRC 0 against the fab rule set, DFM checks cleared, and a complete
  manufacturing package exported — Gerbers, drill, BOM, CPL, STEP, and fabrication
  drawing — for the chosen fab (and the compare fab where quote-compare needs it).
seams: electronics/scripts (new gerber/DRC runners via kicad-cli) +
  electronics/manufacturing/ output; reproducible per subtree CLAUDE.md.
targeted-proof: kicad-cli DRC exits clean; the package passes the fab's DFM
  rules (min trace/space/annular, silkscreen, solder-mask sliver); Gerbers render
  and match the board.
unit: parent-only
```

### Phase H3e — independent review, quote, order-ready

```yaml
outcome: An independent design review (fresh reviewer against the changed layout
  contract) passes; real quotes obtained from PCBWay and JLCPCB; the cost model
  refreshed to the quoted estimate. The design is order-ready; the order itself is
  the operator's action.
seams: docs/plans — cost-model quoted row + this plan's closeout; the operator
  uploads the package for the live quote.
targeted-proof: the review finds no order-blocking defect (or all are resolved);
  the quotes land inside or explain the £95–£155/unit envelope; the quote
  acceptance report items in aius-cost-model.md §Quote acceptance report are filled.
unit: parent-only
```

## Not in this plan

EVT bring-up (H4) and full-path integration (H5) are separate gates in the
[hardware spec](aius-hardware-spec.md#build-gates-lean). Placing the physical
order — fabricator account, package upload, payment — is an operator action taken
after H3e, not a step this plan executes.
