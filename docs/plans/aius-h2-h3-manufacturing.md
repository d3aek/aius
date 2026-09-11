---
title: Aius H2/H3 manufacturing readiness
type: plans
scope: component
component: aius
subtype: design
status: in-progress
tags: [aius, hardware, kicad, pcb, layout, impedance, dfm, manufacturing]
updated: 2026-09-11
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

### Phase H2c — mechanical and placement feasibility ✓ SHIPPED 2026-08-15

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

**Result (2026-08-15).** From the filed Hammond drawing
(`https://www.hammfg.com/part/1455N1201BK`) and STEP:

- **Extrusion** 100.00 × 53.00 mm external, ~49.70 mm internal height, **designed
  for a 100 mm-wide PC board** that slides into the side slots; 120.00 mm long.
- **Board outline: 100 mm wide (slot dimension) × ≤100 mm long.** The width is
  fixed by the extrusion slots; length runs along the 120 mm barrel with ~20 mm to
  spare and stays within the spec's ≤100×100 cap and PCBWay's ≤100×100 price tier.
  Exact width trim (typ. ~99.5 mm to clear the slots) and the slot height are
  finalised at H3b against the STEP.
- **Rear-panel window 97.10 × 48.70 mm** (bezel opening) is the connector budget.
  The rear row — 2× TOSLINK (~14 mm each), RJ45 magjack (~16 mm), USB-C (~9 mm),
  6-pin Program header (~8 mm) — sums to ~75–85 mm with gaps, inside the 97.10 mm
  width; every connector height is well inside 48.70 mm. Fits with margin.
- **Height stack:** placing the carrier on a lower extrusion slot leaves ~40 mm
  above for the socketed-Teensy stack (~10 mm) and the magjack, clear of the lid.
  microSD access and the front-edge expansion headers + Tag-Connect have reach.

No connector-to-wall or module-to-lid collision at this granularity. Failure
modes that would block regardless of layer count (outline, panel alignment,
collision) are cleared; exact cutout coordinates and slot selection are set at
H3b placement against the STEP assembly.

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
Design-target impedance geometry (from H2a, JLCPCB 0.2104 mm; corrected against
the calculator on 2026-09-09): **USB 90 Ω** at w ≈ 0.25 mm / s ≈ 0.15 mm;
**Ethernet 100 Ω** at w ≈ 0.20 mm / s ≈ 0.15 mm. The
two fabs' prepreg differs by ~0.01 mm, well inside tolerance, so one routed
geometry quotes on both and is re-confirmed against each fab's controlled-impedance
service at H3c. Cost model refreshed to the 4-layer pre-layout estimate on both
fabs ([aius-cost-model.md](aius-cost-model.md)).

## Phase H3 — layout, package, quote (order-ready)

Only starts once H2d fixes the stackup and layer count.

### Phase H3a — footprint library ✓ SHIPPED 2026-08-15

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

**Result (2026-08-15): full-severity ERC 0 violations — the schematic is
footprint-clean.** Driven from 13 → 0:

- **Map-to-standard (done).** The four expansion JST GH headers → stock
  `Connector_JST:JST_GH_BM0NB-GHS-TBT_1x0N-1MP_P1.25mm_Vertical`; the Tag-Connect
  → `Connector:Tag-Connect_TC2030-IDC-NL_2x03_P1.27mm_Vertical`. The GCT `USB4105`
  USB-C resolves in the pinned 10.0.4 container (only kicad-footprints master
  lacked it), so it needs no custom build.
- **USB-A re-selected (done).** The internal USB-A moved from TE `292303-1` (no
  KiCad footprint, no filed datasheet) to the pin-identical `292303-7` variant of
  the same series, KiCad-native as `Connector_USB:USB_A_TE_292303-7_Horizontal`.
- **4 custom land patterns (built).** `lib/aius.pretty/` + `fp-lib-table`:
  `Cliff_ORJ5_TOSLINK` / `Cliff_OTJ5_TOSLINK` (RS PRO ORJ-5/OTJ-5, 3×Ø0.8 signal
  @ 2.54 mm + 3×Ø2.0 NPTH pegs — verified NOT compatible with KiCad's Toshiba
  TORX170 4-pin/2-post footprint), `Pulse_J0011D21BNL_TabDown` (8×Ø0.89 staggered
  signal + 4×Ø1.02 LED + 2×Ø1.60 shield + 2×Ø3.20 NPTH posts), and the mixed-pitch
  `Teensy41_Sockets_SSW-124_MMS-103_SSW-105` (2×24 @ 2.54 mm + 6-pin @ 2.00 mm MMS
  Ethernet + 5-pin @ 2.54 mm USB host, generated from a reviewable pad table by
  `lib/gen_teensy_footprint.py`, pads named to the Teensy pin ids so the partial
  Teensy41 symbol maps by name). Signal-pad geometry is datasheet-exact and the
  Teensy main-row order is exact from PJRC card11a; the optical peg / magjack
  staggered / Teensy underside-cluster XY are datasheet best-estimates flagged for
  pad-by-pad verification against the physical parts (on the bench) at the recorded
  pre-order footprint gate ([aius-h1-schematic.md](aius-h1-schematic.md#milestones)).

**Residual for H3d/pre-order:** the peg/staggered/underside XY verification above
does not block H3b/H3c (it affects physical seating, not the netlist or routing
topology), but it is a hard pre-order gate item.

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

**Progress (2026-08-16): board scaffolded.** `electronics/scripts/scaffold_pcb.py`
(run in the pinned container via the `pcbnew` Python API) builds
`electronics/aius-carrier.kicad_pcb` from the schematic netlist: 4 copper layers
(L1 sig / L2 GND / L3 GND+power / L4 sig), the 96 × 95 mm Edge.Cuts outline
(H2c; final Hammond-slot trim is a pre-order mechanical-review item), all 62
footprints loaded and net-assigned (69 nets, 169 ratsnest), initial placement by
functional zone (optical/USB-C/magjack on the rear edge, Teensy central with its
Ethernet 2×3 + USB-host clusters, the eight expansion headers + Tag-Connect on the
front edge, passives gridded), and the L2/L3 inner GND planes (H2b return plane;
left unfilled — KiCad fills on open, H3c refills after routing). DRC: 0 shorts,
169 expected ratsnest, 46 placement-refinement items (courtyard/hole clearance,
silk) that resolve during H3c placement refinement + routing. The board is
regenerable from the netlist; from H3c on it is hand-routed and no longer
regenerated. **Remaining in H3b:** collision-free placement refinement (the 46
items) is done together with routing in H3c.

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

**Progress (2026-08-16): board fully routed by Freerouting.** Toolchain (all
reproducible, in `electronics/scripts/` + the `freerouting` service in
`compose.yaml`): `route_export_dsn.py` sets the net class (0.20 mm track / 0.15 mm
clearance / 0.60/0.30 mm via) and exports Specctra DSN → Freerouting v2.3.0
(`ghcr.io/freerouting/freerouting`, 20 passes, headless) → `route_import_ses.py`
imports the session, applies fab-capable DRC minimums, and fills the L2/L3 GND
planes; the persisted net class lives in `aius-carrier.kicad_pro`.

Result: **160/160 nets routed** (0 unrouted), 880 track segments, both GND planes
filled. DRC against the fab rule set fell 607 → 22, then a bounded post-route
cleanup pass (`route_cleanup.py`: widen sub-0.15 mm necked tracks to 0.20 mm; set
the GND planes to a solid pad connection; refill) brought it to **15 violations +
3 GND-stitch unconnected**. That is the deliberate stopping point for the scripted
grind (owner guidance: bounded iterations, stop at diminishing returns, do not
fixate on 0). The residual is interactive-judgment cleanup for the H3e pass:

- 4 `clearance` + 4 `courtyards_overlap` + 4 `hole_clearance` — congestion in the
  USB cluster (J1/J2/U1/U2) and the dense Teensy area (U10); needs a placement
  nudge + local reroute, best done interactively (a scripted reroute re-rolls the
  whole board and risks whack-a-mole against the fixes above).
- 3 GND unconnected + 2 `via_dangling` — a few Freerouting GND stubs on an outer
  layer not tied to the plane; add stitching vias / drop the dangling ends.
- 1 `silk_over_copper` — trivial.

A footprint bug surfaced and was fixed here: the magjack (J20) estimated LED/post/
shield positions self-overlapped (70 proximity items) — respaced so no holes
collide (signal pitches kept datasheet-exact; secondary XY still a pre-order
verify item). Diff-pair coupling/length-matching is an H3e review refinement (the
autorouter routes pairs single-ended at class width over the solid L2 plane).
**Remaining in H3c:** clear the 22+3 punch-list (placement/GND polish) — the
interactive cleanup, done with the H3e review.

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

**Progress (2026-08-16): export tooling ready.** `electronics/scripts/export_fab.sh`
runs DRC then exports the full package — Gerbers (all 4 copper layers incl. the
In1/In2 GND planes), drill, BOM, CPL, STEP — via `kicad-cli` into
`electronics/manufacturing/` (regenerable, not committed). Proven end to end on
the routed board. **Gate not yet met:** the package is a review/quote snapshot,
not order-ready, until the H3c punch-list (22+3) is cleared to DRC 0 and DFM is
checked against each fab's rules.

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

**First review pass (2026-08-16): NOT order-ready — findings, most-severe first.**
An adversarial independent review of the routed board found one systemic routing
defect that resets H3c to a re-route, plus deferred verification gates that are
real order-blockers. It also *cleared* the items previously flagged as highest
risk. Consolidated punch-list (supersedes the earlier 15+3 DRC list):

- **B1 (blocker) — signals routed through the inner GND planes.** All four copper
  layers are declared `signal` and the board carries no `stackup` block, so
  Freerouting routed hundreds of segments on In1.Cu/In2.Cu (the L2/L3 GND pours)
  and split diff pairs across layers (ETH_TXP on In2.Cu, ETH_TXN on F.Cu). The
  return-path/impedance guarantees are void as routed. Fix: set In1/In2 as plane
  layers + a no-route keepout, then re-route with diff pairs coupled on one outer
  layer over solid L2 GND. A full re-route, not a cleanup.
- **B2 (blocker) — no board stackup.** Add the JLC04161H-7628 / PCBWay 4-layer
  1.6 mm stackup to the board; select each fab's controlled-impedance service at
  quote (a priced add-on).
- **B3 (blocker) — rear connectors mis-placed.** Optical modules are at 180° while
  USB-C/magjack are at 0° (opposite facings); bodies sit 8–23 mm inside the edge,
  not bezel-aligned; and the board-width vs Hammond slot/insertion axis is
  unresolved (96 mm width vs the end-plate 95.02 mm datum). Re-orient consistent,
  edge-align, confirm the slot width.
- **B4 (blocker) — magjack + USB-C pinouts unverified.** `J403.pdf` has no internal
  schematic, so the MDI pin mapping in `ethernet.json` is unconfirmed and magjack
  pin 7 floats (`unconnected-(J20-NC-Pad7)`); the GCT USB4105 USB-C has no filed
  datasheet. Obtain the Pulse J0011D21BNL + GCT schematics and confirm before
  order (needs external evidence — operator or datasheet fetch).
- **M1** Teensy footprint self-clearance (VBAT vs main-row pad 30, 0.14 mm) — fix
  the service-cluster spacing in the footprint. **M2** power traces undersized
  (0.2 mm for ~1 A VBUS) — widen VBUS_RAW/VBUS_5V/EXP_USBH_VBUS to ≥0.5 mm.
  **M3** CHASSIS↔GND single-point tie asserted in `nets.md` but not implemented —
  add the net-tie (or document deliberate isolation). **M4** magjack + Teensy
  underside-cluster XY still bench-verify. **M5** placement congestion + GND stubs
  (resolve in the re-route).
- **Verified correct (were the top worries):** Teensy main-row pin order (no
  transposition), optical footprint geometry vs datasheets, S/PDIF reset-safe
  default path. DFM otherwise comfortable at both fabs except controlled impedance.

The scripted placement-nudge iteration (owner option 1) was tried and bottomed
out (reroll → 17+3 with a newly unrouted net, worse than 15+3), confirming the
residual is not a scripted-placement problem — consistent with B1/B3 being the
real issues. Rework sequence: B4/M3 schematic → B3/M1 placement+footprint →
B1/B2 planes+stackup → re-route → M2 power widen → DRC/DFM → re-review.

## Recovery audit (2026-09-09)

The owner resumed layout completion. The first fresh KiCad 10.0.4 baseline
reproduces **15 violations and 3 unconnected items**. This does not clear the
structural blockers above. Several earlier review assurances require correction:

- **Tooling repaired:** both container services now bind the Aius checkout at
  `/workspace/aius`, independent of its upstream repository location. The CLI/Python wrapper
  preserves the invoking UID/GID with host user-namespace mapping, so reports
  actually persist on this Docker host. KiCad had printed "Saved DRC Report"
  even when the old UID could not write it. Wrapper tests and a readable JSON
  DRC artifact establish the repaired feedback loop. Evidence LFS attributes
  lost during submodule extraction are restored. Project library tables now
  declare their stock dependencies explicitly after a fresh profile exposed
  105 library-resolution warnings.
- **Library/schematic corrections delivered:** the selected TPD2EUSB30ADRTR is a
  TI DRT package, not the assigned SOT-23-3. PJRC's component-side dimensioned
  drawing contradicts the Teensy main-row handedness and guessed auxiliary
  pad coordinates. The optical rear peg pair should be 2.10 mm from the signal
  row: 6.00 mm is the separation between peg rows, not the signal-to-peg distance.
  These invalidate the earlier "verified correct" claims. The library now fixes
  these coordinates, optical pin1/3 handedness, the Pulse stagger/LED/post/shield
  coordinates, USB metal courtyard, and per-socket drill sizes. U1 uses Texas_DRT-3;
  magjack pin8 returns to PCB GND per Pulse J403.L, and its unused right LED pin
  names are corrected. The schematic is regenerated and full ERC remains zero.
  The library checker covers 112 coordinate/drill assertions; its board mode
  rejects the unchanged old PCB as intended. Coordinate provenance and limitations:
  [`carrier-footprint-review.md`](../reference/carrier-footprint-review.md).
- **Two suspected electrical defects were cleared:** KiCad's exported netlist
  correctly expands the USB-C symbol's stacked power/ground pins; all PCB pads
  carry their intended nets. PJRC already puts 150 ohms in series with the
  exposed Ethernet LED signal. The claim that the magjack has internal LED
  resistors is incorrect, but another carrier resistor is not required on that
  evidence alone.

**Assembly blocker discovered in delivery review:** the selected SSW and MMS
socket bodies overlap nominally by 0.035 mm and differ in height (8.51 vs 4.45 mm).
Neither moving the Teensy holes nor routing around them resolves the mismatch.
Socket selection, mating pin lengths and physical seating require resolution;
the owner approved a fresh matched pin-and-socket set on 2026-09-09, assuming
a bare shop-new Teensy. Existing headers need not be retained. The library
coordinate correction does not claim to clear this assembly gate. Delivery
review also identifies the full SSW-105 service-row body's overlap with both
main-row sockets; the replacement selection must cover the service row too.
The owner rejected custom spacers and requires off-the-shelf components.
The discrete Mill-Max candidate is therefore rejected. The 2026-09-11 revision
retains main/optional-host SSW sockets with matched HTSW-xxx-07 headers and
moves Ethernet/service to Teensy top headers cabled to carrier J18/J19.
This removes the colliding housings and establishes an 11.05 mm nominal hard
stop without a custom spacer. U10 now has 53 underside pads; its symbol and
the exported netlist use J18/J19 for the moved interfaces. Stock parts,
unkeyed-cable mapping and remaining physical checks are in the footprint review.

**Placement recovery contract (prebuild review):** preserve the 100 × 95 mm
board envelope and place the Teensy with microSD facing the rear panel, between
USB-C and Ethernet, rather than claiming inaccessible central placement meets
the card-access requirement. Rear optical/USB-C/magjack bodies must meet a common
panel datum; the internal USB cable gets an explicit insertion/bend envelope.
Reserve and hand-route all four differential pairs (device USB, host USB, TX
MDI, RX MDI) before autorouting other signals on F.Cu/B.Cu only. L2/L3 remain
ground reference planes. Acceptance requires corrected embedded footprints,
zero placement/hole collisions, panel/slot/cable clearance, and reviewed power
connectivity before routing. Matched socket selection and expansion isolation
are still in prebuild review; this is not placement acceptance.

**USB-host power correction (2026-09-10):** prebuild review found carrier U12 OUT
paralleled with the native Teensy TPD3S014 OUT. The incorrect `power_in` type on
UH5V hid this from ERC. UH5V is now `power_out` and the sole J14 VBUS source;
U12, R14/R15/R16, C12 and orphan control nets are removed. Native firmware power
control and the 500 mA service budget are retained. The regression reproduced
the old parallel source, then passed against corrected JSON and KiCad's exported
XML netlist; four CLI regressions cover valid, parallel, mistyped and missing
sources. Regenerated full-severity ERC is zero. Sources and fault limitations are
in [`connectivity/nets.md`](../../electronics/connectivity/nets.md).

**Separate electrical release blocker:** expansion 3.3 V is connected directly
to Teensy 3.3 V despite the interface's reverse-isolation requirement. Signal
series resistors do not isolate that rail. Correct rail and signal powered-off
paths before freezing routing; no isolation waiver is implied by clean ERC.
The owner selected an independent-power option, with no concrete expansion plans
and permission to leave pads empty. **First-build J10–J15 are DNP**, enforced in
JSON, generated schematic and exported-netlist checks. This is an unpopulated
variant, not an isolation fix: do not populate these headers or attach external
power until the rail/signal isolation circuit and population are reviewed.
The core and Tag-Connect remain wired. I2S still has no power/sense pin; any
future independent-power assembly must resolve that interface explicitly.

**Stock-interface delivery proof:** 124/124 library coordinate/drill checks;
cable/DNP and native-host-power checks pass against the actual KiCad XML export;
seven assembly/generator regressions pass. Full ERC is zero. The obsolete
65-hole footprint is removed, recoverable from Git. The PCB is unchanged and
still rejected; this delivery is schematic/assembly preparation, not H3b closure.
Focused independent delivery review and clean-room confirmation pass for the
changed mapping/DNP contract. Physical cable continuity, seating, retention and
enclosure checks remain open; neither review approves the old PCB.

The existing impedance calculator also contradicts H2d's former claim that
0.20/0.15 mm geometry supplies both targets: at the recorded JLCPCB stackup,
90 ohms requires width 0.251 mm and 100 ohms 0.201 mm at 0.15 mm edge spacing.
H2d's routing targets above are corrected. These remain feasibility estimates,
not fabricator-controlled-impedance approval.

Documentation validation remains diagnostic: `the repo tooling tool doc-index --
--repo-root git_modules/aius --validate-only --quiet` rejects the extracted
submodule's `docs/` placement against its former upstream repository component-path rule.
This is not an ERC/footprint failure; adapting the shared validator is outside
this hardware repair.

Recovery remains within H3: resolve the socket assembly, establish actual
connector/panel datums, rebuild placement and constrained routing, then apply
ERC/DRC, pair/plane, mechanical and independent review gates. No manufacturing
release or order is authorised by this recovery note.

## Not in this plan

EVT bring-up (H4) and full-path integration (H5) are separate gates in the
[hardware spec](aius-hardware-spec.md#build-gates-lean). Placing the physical
order — fabricator account, package upload, payment — is an operator action taken
after H3e, not a step this plan executes.

H4 also owns the raw-bypass test rows re-homed from
[`aius-optical-prototype.md`](aius-optical-prototype.md) §Representative test
matrix row 5 (2026-09-03): the `SN74LVC1G3157` bypass mux exists only on the
manufactured carrier, so "select bypass while muted; source frames reach the
sink without the processed path" runs at H4's `raw bypass` bring-up step.
