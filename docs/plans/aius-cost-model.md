---
title: Aius prototype cost model
type: plans
scope: component
component: aius
status: in-progress
tags: [aius, hardware, pcb, cost, prototype]
updated: 2026-09-03
toc: true
---

# Aius prototype cost model

Estimate the landed cost of three assembled Aius carrier boards before the design is committed to manufacture. This is a should-cost model, not a supplier quotation. Refresh it first from the costed functional BOM, then from the completed PCB files and an uploaded PCBWay quotation.

**Status 2026-09-03: a living model, so `in-progress` is its steady state** —
it is refreshed at each gate in §Confidence and refresh gates and never
"ships"; the optical-jack anchor rows now name the purchased RS PRO parts.

The estimate covers the carrier assembly, Hammond enclosure, internal cable, hardware, freight, VAT, duty, and prototype contingency. It excludes Teensy modules, the existing DAC, external cables, speakers, engineering time, panel machining, compliance testing, fixtures, CNC time, and re-spins.

## Planning estimate

| Item | Low per assembled unit | Central per assembled unit | High per assembled unit | Treatment |
|---|---:|---:|---:|---|
| Four-layer PCB, assembly setup, component procurement, and placement | £42 | £59 | £77 | 4-layer per the H2 verdict; the bare-board surcharge over 2-layer is small (a few £ across the batch) and sits well inside contingency, since assembly/setup dominates this line |
| Hammond 1455N1201BK enclosure | £28 | £29 | £30 | Stock enclosure; rear machining excluded |
| Internal USB cable and mechanical hardware | £5 | £7 | £10 | Includes approximately 100 mm USB-A to Micro-B cable and retention hardware |
| Freight, VAT, duty, and prototype contingency | £20 | £32 | £40 | Allocation remains provisional until a landed quote exists |
| **Carrier-only landed total** | **£95** | **£125** | **£155** | Rounded planning envelope |

The line-item central values contain deliberate rounding and do not arithmetically force the headline total; the detailed BOM must replace them before quotation.

| Batch | Low | Central | High |
|---|---:|---:|---:|
| Three assembled carriers | £285 | £375 | £465 |

An optional Teensy 4.1 is shown separately at approximately £29 per unit including VAT. The user's existing Teensy is sufficient for the capability milestone and is excluded from the headline estimate.

## Quantity model

- Quote assembly and component procurement for three carrier boards.
- Accept five unassembled bare PCBs if that is the fabricator's minimum.
- Treat surplus bare boards as unpopulated spares, not as completed units.
- Allocate fixed fabrication, stencil, programming, setup, and freight charges across the three assembled units.
- Preserve the supplier's raw quotation and a normalized landed-cost worksheet so changing the quantity does not conceal fixed costs.

The preferred quantity is three because it supplies a rework unit, a regression/A-B unit, and a spare or destructive-test unit. A lower assembled quantity is acceptable only if the quotation exposes setup charges and does not increase the total landed spend or remove the required spare strategy.

## Two-layer versus four-layer decision

**Resolved 2026-08-15: four layers.** The H2a impedance feasibility gate showed
2-layer 1.6 mm cannot hold manufacturable 90 Ω USB / 100 Ω Ethernet geometry
(needs ~0.9–1.15 mm traces; a routable pair reads ~163 Ω, +63–83 % off target)
and offers no dedicated return plane — a documented electrical escalation, not a
mechanical one. The owner escalated to 4-layer; the design routes against the
JLC04161H-7628 / PCBWay 4-layer 1.6 mm class. See
[aius-h2-h3-manufacturing.md §H2d](aius-h2-h3-manufacturing.md#phase-h2d--layer-verdict-and-cost-refresh-owner-gate).
Two layers were the original cost baseline; the paragraphs below are retained for
the escalation record.

Planning price anchors from PCBWay's published prototype pages are approximately:

| Bare-board case | Two layers | Four layers | Interpretation |
|---|---:|---:|---|
| Up to 100 x 100 mm, five boards | US$5 | US$48 | Advertised entry pricing before options and shipping |
| Approximately 150 x 100 mm, five boards | US$41 | US$116 | Indicates the cost of crossing the preferred size tier |

Sources: [PCBWay 100 x 100 mm two-layer prototype pricing](https://www.pcbway.com/pcb_prototype/2_Layer_pcb/100x100mm.html) and [PCBWay 100 x 100 mm four-layer prototype pricing](https://www.pcbway.com/pcb_prototype/4_Layer_pcb/100x100mm.html). These dynamic marketing values are planning anchors only. Archive dated evidence and replace them with a project quotation.

Four layers become mandatory only when the H2 gate demonstrates that two layers cannot provide manufacturable 90 ohm USB and 100 ohm Ethernet geometry, uninterrupted return paths, acceptable placement, and a complete route within the enclosure and 100 x 100 mm target. A layer escalation updates:

- bare-board and assembly price;
- stackup and impedance surcharge;
- stencil/setup cost;
- shipping weight where material;
- central and high landed estimates;
- the recorded technical reason for escalation.

## Initial component anchors

These are provisional catalogue anchors, not a released BOM:

| Function | Planning part/source | Provisional price evidence | Cost-model treatment |
|---|---|---:|---|
| Enclosure | Hammond 1455N1201BK, [Farnell](https://uk.farnell.com/hammond/1455n1201bk/box-black-aluminium-end-plate/dp/9287876) | £22.27 ex VAT at quantity five | Model £28-£30 delivered/VAT-inclusive |
| Optical transmitter | RS PRO OTJ-5, [RS `805-1680`](https://uk.rs-online.com/web/p/fibre-optic-connectors/8051680) (Cliff FCR684205T class, purchased 2026-07-27) | RS PRO price not re-quoted in-repo; Cliff-class anchor £1.23 ex VAT at quantity one (CPC, 2026-07) carried as the planning figure | Included in PCBA line |
| Optical receiver | RS PRO ORJ-5, [RS `805-1671`](https://uk.rs-online.com/web/p/fibre-optic-connectors/8051671) (Cliff FCR684205R class, purchased 2026-07-27) | RS PRO price not re-quoted in-repo; Cliff-class anchor £3.67 ex VAT at quantity one (DigiKey, 2026-07) carried as the planning figure | Included in PCBA line |
| Optional processor module | Teensy 4.1, [DigiKey](https://www.digikey.co.uk/en/products/filter/evaluation-boards/embedded-mcu-dsp-evaluation-boards/teensy-4-1/786) | £23.88 ex VAT | Separate approximately £29 inc-VAT option |

**Optical-part substitution (2026-07-27, rows updated 2026-09-03):** the
purchased modules are the RS PRO ORJ-5 (RS `805-1671`) and OTJ-5 (RS
`805-1680`), chosen for availability over the unavailable Cliff parts
([sourcing record](aius-optical-prototype.md#objective); datasheets under
`https://uk.rs-online.com/web/p/fibre-optic-connectors/8051671` and `AIUS-OPTICAL-OTJ5-RS/`). The
RS PRO unit price is not recorded in this repository and RS refuses automated
page fetches (HTTP 403, 2026-09-03), so the rows carry the Cliff-class figures
as planning anchors; read the RS PRO price off the purchase record or the
product pages at the next refresh gate.

Archive current supplier pages or quotations under `docs/reference/datasheets.md` before using them as a release decision. Record stock state, currency, price breaks, retrieval time, VAT basis, and source URL in the doc that cites them.

## Costed functional BOM requirements

The functional BOM lives in
[`provisional-components.csv`](../../evidence/design/provisional-components.csv).
Costing is plain arithmetic — GBP, Bank of England rate at the retrieval date for
foreign prices, VAT 20%, freight/duty allocated over three assembled units — not a
machine-validated schema. Every BOM line records:

- functional block and provisional or selected manufacturer part number;
- approved substitute policy;
- quantity per carrier;
- supplier URL and stock state;
- retrieval date and currency;
- quantity-one, quantity-three, and vendor price-break prices;
- minimum order quantity and order multiple;
- fixed NRE/setup versus recurring cost;
- low, central, and high assumptions;
- VAT, duty, and freight treatment;
- confidence level and refresh trigger;
- customer-supplied and optional status;
- evidence artifact where the source is saved.

Functional groups include:

- bare PCB and stackup;
- stencil, assembly setup, placement, and inspection;
- socketed Teensy headers and retention;
- USB-C, internal USB-A, CC resistors, ESD, and power protection;
- optical receiver, transmitter, filtering, buffer, and diagnostic mux;
- Ethernet magjack, support passives, LEDs, and shield termination;
- expansion connectors, test points, and fixture interface;
- passives, regulators or protection, indicators, and hardware;
- enclosure, internal cable, and fasteners;
- packaging, freight, import duty, and VAT.

## Confidence and refresh gates

| Estimate state | Required evidence | Decision it supports |
|---|---|---|
| Initial should-cost | This planning envelope and catalogue anchors | Budget discussion only |
| Functional-BOM estimate | Topology, provisional MPNs, supplier evidence, and fixed-cost assumptions | Continue or simplify before schematic |
| Pre-layout estimate | Reviewed schematic, placement, board size, layer verdict, and current stock | Continue detailed routing |
| Quoted estimate | Released PCB files, assembly BOM/CPL, approved substitutions, shipping, VAT, and duty | Decide whether to order three prototypes |
| Actual EVT cost | Invoice, yield, rework, shortages, freight, enclosure, and hardware | Re-spin and future-unit decisions |

Refresh the model when:

- a functional block or selected MPN changes;
- the board crosses 100 x 100 mm;
- the design changes from two to four layers;
- assembly quantity changes;
- a supplier price is more than 30 days old at quotation time;
- a quoted component is unavailable or substituted;
- freight, VAT, or duty treatment changes;
- EVT reveals rework, yield, fixture, or mechanical costs.

## Quote acceptance report

Before ordering, the cost report shows:

- carrier-only low, central, high, and quoted landed totals;
- total cash outlay for three assembled carriers plus any surplus bare boards;
- optional Teensy price separately;
- two-layer and equivalent four-layer comparison;
- all fixed charges and their per-unit allocation;
- customer-supplied machining as zero supplier cost, not omitted scope;
- excluded engineering, certification, fixtures, and re-spin costs;
- currency conversion source and date;
- contingency remaining after quotation;
- variance from the £95-£155 per-unit envelope.

A quote outside the envelope is not automatically rejected. It requires an explicit cost-model update identifying whether the cause is fixed prototype setup, component selection, layer escalation, board size, logistics, or an inaccurate initial assumption.

## Assumptions

- Delivery destination is M20 5PG, United Kingdom.
- Three assembled carriers are the target; vendor-minimum surplus boards are bare.
- Rear-panel machining and CNC work are customer-supplied and excluded.
- Teensy modules are optional and excluded from the headline carrier total.
- Four layers (H2 verdict, 2026-08-15) and a board no larger than 100 x 100 mm are the planning baseline.
- Licensing work is documentation-only for this personal, non-commercial project.
- No manufacturing order occurs from this should-cost estimate alone.
