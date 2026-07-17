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

Research pending.

## Follow-on gate

P1C6b remains unauthorized. It requires an adversarial review of the complete
research matrices, a deterministic selected MPN for each family, exact request
and realization migration ownership, supersession handling for old evidence,
and a fresh acquisition/assembly lifecycle before P1D3 can reopen.
