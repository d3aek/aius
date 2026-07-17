---
title: Aius critical receptacle replacement search
type: plans
scope: aius
status: in-progress
tags: [aius, evidence, hardware, sourcing, usb]
updated: 2026-07-17
toc: true
---

# Aius critical receptacle replacement search

## Purpose and authorization

P1D2 closed the supplemental lifecycle but blocked P1D3 because primary
manufacturer evidence remained unavailable for the selected internal USB-A and
rear USB-C receptacles. The user authorized a replacement search on 2026-07-17
with this strict UK supplier preference:

1. RS UK;
2. Mouser UK;
3. DigiKey UK;
4. Farnell UK.

This order supersedes the historical P1C2 distributor order only for these two
replacement searches. CPC Farnell is outside this search unless separately
authorized.

P1C6a is research-only. It changes no component, request, realization, source,
assumption or lifecycle record. It retains no source bytes. A later reviewed
P1C6b checkpoint must authorize and apply any replacement; a later P1D
checkpoint must retain its primary artifacts under the existing Git LFS rules.

## Eligibility and ranking

A candidate is eligible only when all of these conditions hold:

1. It is one exact, currently orderable MPN identified by a reachable primary
   manufacturer HTTPS page.
2. Reachable primary manufacturer material identifies that exact MPN and links
   or provides the current datasheet and mechanical drawing or footprint needed
   to make every assigned P1D, H1 and H2 verification actionable.
3. The manufacturer material does not mark the part obsolete, discontinued,
   NRND or otherwise unsuitable for a new design.
4. A reachable exact-MPN product page exists at an authorized UK distributor.
   Distributor authorization must be supported by manufacturer material.
5. The candidate does not contradict any requirement already assigned to its
   family. Research records actionability, not a passed electrical, mechanical
   or manufacturing gate.

Search distributors in the locked preference order. Within each distributor,
rank qualifying candidates by requirement coverage, stable direct primary
documentation, assembly and retention practicality for a small-run carrier,
then lexically by normalized manufacturer and MPN. Record up to three eligible
candidates per family plus every serious candidate rejected before them. Do not
substitute a nearby suffix, series or distributor-only identity.

The internal USB-A candidate must be a PCB-mounted USB 2.0 Type-A receptacle
carrying D+, D-, protected VBUS and ground, accepting the internal cable, and
providing documented footprint, shell, mating and enclosure dimensions.

The rear USB-C candidate must be a PCB-mounted USB Type-C receptacle exposing
USB 2.0 D+/D-, independent CC1 and CC2, VBUS, ground and shield, with documented
footprint, shell and mating geometry. The system remains a sink-only USB 2.0
device with no source role or Power Delivery.

## Required research record

For every considered exact MPN, record:

- family, manufacturer and exact MPN;
- lifecycle statement and its primary basis;
- manufacturer product-page, datasheet and drawing URLs;
- whether the primary material identifies the exact suffix;
- distributor, exact UK product URL and supplier SKU;
- manufacturer authorization URL and basis;
- stock state, stock quantity, MOQ, order multiple and one-unit ex-VAT GBP
  price only when the exact live page directly supports them;
- requirement, assembly and mechanical-screening notes;
- `eligible` or `rejected` with an exact reason;
- retrieval timestamp.

Unknown commerce fields remain unknown. Search snippets, indexed values and a
manufacturer's distributor authorization do not substitute for facts missing
from the exact distributor page.

```yaml
phase: P1C6a
unit: read-only-family-research
seams: [critical component family, exact manufacturer MPN, authorized UK distributor page]
contract-artifact: spec-only
depends-on:
  - evidence/design/selection-policy.json
  - evidence/design/components/usb-a-receptacle.json
  - evidence/design/components/usb-c-receptacle.json
  - evidence/manifest/requests/REQ-USB-A-RECEPTACLE-MFR.json
  - evidence/manifest/requests/REQ-USB-C-RECEPTACLE-MFR.json
  - docs/plans/aius-p1-evidence-topology-cost.md
  - docs/plans/aius-p1d-supplemental-reference.md
fan-out:
  - name: research-usb-a-receptacle-replacement
    owned-paths: []
  - name: research-usb-c-receptacle-replacement
    owned-paths: []
coordination: both tasks are read-only and return source matrices; the parent alone records integrated findings in this plan
review-intensity: active
commit-checkpoint:
  green:
    - the repo tooling tool doc-index -- --validate-only
  buildable: [audited ranked replacement shortlist with exact primary and authorized-distributor URLs]
  parent-commit: "docs(aius): shortlist critical receptacle replacements"
```

## Findings

USB-A:

- Eligible and preferred: TE Connectivity `292303-1`.
  - Manufacturer page: <https://www.te.com/en/product-292303-1.html>
  - Status: Active.
  - Exact match: USB 2.0, Type A, receptacle, 4 position, right angle, through-hole.
  - Primary docs: product drawing `ENG_CD_292303_G.pdf`, application spec `ENG_SS_114-40054_B.pdf`, product spec `ENG_SS_108-60034_A.pdf`.
  - RS UK page: <https://uk.rs-online.com/web/p/usb-connectors/5298195>
  - RS stock and price evidence on the crawled page: 25 in stock, 115 more from another location, 1,560 shipping from 2026-06-05, pack of 5, £8.58 ex VAT, £1.716/unit.
  - Why retained: it exactly satisfies the internal USB-A screen and the earlier failure was evidence selection, not a bad MPN.

- Rejected: Würth Elektronik `614004134726`.
  - Manufacturer catalogue and datasheet exist.
  - Rejected on mechanical practicality for the likely carrier: right-angle THT with ~19.3 mm depth and a 1.57 mm PCB recommendation mismatch versus the design target.

- Rejected: Würth Elektronik `629104190121`.
  - Manufacturer catalogue and datasheet exist.
  - Rejected as an SMT USB-A option: the footprint/retention tradeoff is worse than the incumbent THT TE part for a small-run carrier.

- Rejected: Würth Elektronik `614004184726`.
  - Manufacturer catalogue and datasheet exist.
  - Rejected because the manufacturer-recommended PCB thickness is 1.20 ± 0.05 mm, which is a poor fit for the likely 1.6 mm board.

- Rejected: Molex `67643-3911` and `89485-8000`.
  - RS pages exist, but the manufacturer evidence chain remained too weakly published to satisfy the exact-MPN rule.

USB-C:

- Eligible and preferred: Würth Elektronik `629722000214`.
  - Manufacturer catalogue: <https://www.we-online.com/en/components/products/INPUT_OUTPUT_WR_USB_TYPE_C>
  - Datasheet: `629722000214 Datasheet (PDF)` on the manufacturer page and the dedicated PDF from the prior audit.
  - Status: Active, production active, expected lifetime >10 years.
  - Exact match: USB 2.0, Type C, horizontal, receptacle, 16 pins, SMT, 0.8 mm PCB, 5 A, 48 V.
  - Documented CAD outputs: STEP, IGES, and EDA libraries are published on the manufacturer catalog page.
  - RS UK page: <https://uk.rs-online.com/web/p/usb-connectors/0736097>
  - RS evidence on the crawled page: RS stock no. 736-097, tape of 10, £9.50 ex VAT, preorder, shipping from 2026-07-20.
  - Mouser and DigiKey also carry the exact MPN with current stock; those pages corroborate the part but do not change the RS-first recommendation.

- Rejected: TE Connectivity `1-2295018-2`.
  - Primary evidence exists and the MPN is exact.
  - Rejected because the mid-mount construction makes routing and assembly materially harder than the Würth horizontal part without improving the current requirement coverage.

- Rejected: Hirose `CX90M-16P`.
  - Rejected because the authority/lifecycle chain was not explicit enough for a clean exact-MPN shortlist.

- Rejected: Würth Elektronik `632723300011`.
  - Rejected because the pin naming and interface details do not match the independent CC1/CC2 requirement cleanly enough for this design.

- Rejected: Molex `217179-0001`.
  - Rejected because the published manufacturer chain was incomplete for the exact-MPN rule.

- Rejected: TE Connectivity `2480790-1`.
  - Rejected because it is a charge-only connector and does not expose D+/D-.

Conclusion:

- The search did not displace the USB-A incumbent.
- The USB-C replacement candidate is `629722000214`, with the main unresolved implementation risk being the 0.8 mm PCB thickness recommendation. That belongs in P1C6b selection and H2 validation, not in P1C6a.

## Follow-on gate

P1C6b remains unauthorized. It requires an adversarial review of the complete
research matrices, a deterministic selected MPN for each family, exact request
and realization migration ownership, supersession handling for old evidence,
and a fresh acquisition/assembly lifecycle before P1D3 can reopen.
