---
title: Aius P1D supplemental evidence and electrical reference
type: plans
scope: aius
status: in-progress
tags: [aius, evidence, electrical, kicad, hardware]
updated: 2026-07-16
toc: true
---

# Aius P1D supplemental evidence and electrical reference

This document supersedes the P1D block in
[`aius-p1-evidence-topology-cost.md`](aius-p1-evidence-topology-cost.md). It is
the normative contract from the P1C handoff through the audited,
non-manufacturing electrical reference package. No evidence download, live UK
observation, electrical-reference authoring or KiCad authoring starts until the
P1D plan-wide clean-room pre-build gate passes against this document and the
current tree.

## Execution constraints

- An agent may signal, stop, restart or otherwise interfere only with a process
  that exact agent spawned. A foreign process conflict is reported to its owner;
  broad process matching and shared-service lifecycle commands are forbidden.
- Sub-agents never mutate Git. The parent fetches, reviews, stages exact paths,
  commits and pushes at every checkpoint.
- Acquisition tasks receive one request only. At most three tasks run at once.
  Each wave is tested, committed and pushed before the next wave starts.
- All network activity is foreground, bounded by the committed acquisition
  protocol and attributable to one request. No acquisition task starts a daemon
  or background service.
- Seed evidence remains reproducible byte-for-byte. Supplemental records use
  separate directories and are merged only by the deterministic assembler.
- The final package is a reference design, not manufacturing data. H1 remains
  blocked until the Teensy capability proof passes P7.

## Locked evidence model

### Directory and source rules

Supplemental source fragments live in
`evidence/manifest/supplemental-fragments/<source-id>.json`. An unavailable
supplemental source may also create
`evidence/manifest/supplemental-assumption-fragments/<source-id>.json`. The seed
directories `manifest/fragments/` and `manifest/assumption-fragments/` retain
their exact P1B membership and are never widened.

Observations are embedded in the source fragment. There is no standalone
`evidence/observations/components/` output. This preserves the P1B source model
and prevents two factual copies from drifting.

Manufacturer evidence uses source class `golden`. UK availability evidence uses
source class `dynamic`. `supplemental` is not a source class and is removed from
validator-only allowances. A successful dynamic source has no retained page
bytes, `artifacts: []`, revision `observed:<YYYY-MM-DD>`, and redistribution note
`Normalized observation only; no source page is retained.`

An unavailable source has no factual observations and exactly one metadata-only
`external-reference` artifact named `unavailable`, as required by the existing
source schema. Its matching open assumption is generated deterministically. A
worker never substitutes a different manufacturer, part, distributor or URL.

Reviewable HTML product-page snapshots are normal Git files under
`evidence/source-snapshots/<source-id>/`. Datasheets, drawings, CAD archives and
non-reviewable page captures are LFS files under
`docs/reference/datasheets.md<source-id>/`. Every retained artifact carries an
`artifact_kind`; the ordered unique kinds for a source must equal its request's
`artifact_kinds`. Repeated files of one kind are permitted.

### UK observation profile

The profile is `uk-distributor-price-stock`. Every observation in one acquisition
wave uses the real UTC timestamp allocated by the parent for that wave. Generated
observations occur in this order:

1. `manufacturer_part_number`
2. `supplier_sku`
3. `distributor`
4. `product_initial_url`
5. `product_final_url`
6. `authorization_authority`
7. `authorization_method`
8. `authorization_initial_url`
9. `authorization_final_url`
10. `stock_state`
11. `stock_quantity`
12. `price_quantity`
13. `unit_price_ex_vat`
14. `currency`
15. `minimum_order_quantity`
16. `order_multiple`
17. `price_includes_vat`

The MPN equals the request MPN. URLs are HTTPS. `currency` is `GBP` and
`price_includes_vat` is `false`; an inclusive-only price is unavailable unless
separate evidence supports the ex-VAT normalization. `stock_state` is
`in-stock`, `out-of-stock`, `backorder` or `unknown`. Quantity is a non-negative
integer or null under the state rules; price quantity, MOQ and order multiple
are positive piece counts; the selected quantity must be orderable. The common
basis is `P1D uk-distributor-price-stock normalized field <key> from the approved
supplemental request, realization, and live page.`

Authorization is independently bound to the distributor. Tag-Connect is the
only permitted `ecia-trustedparts-exception`, using
`https://www.trustedparts.com/en/manufacturers/tag-connect`; all other rows use
the selected manufacturer's page or authorized-distributor locator. Product
commerce and authorization URLs remain separate facts.

### Lifecycle closure

At successful P1D2 completion no request is `pending`:

| Evidence result | Request result | Component result |
|---|---|---|
| manufacturer current | `satisfied`, one evidence ID, empty notes | `selected` |
| manufacturer unavailable | `unavailable`, no evidence ID, acquisition note | `candidate-evidence-unavailable` |
| UK current | `satisfied`, one evidence ID, empty notes | unchanged |
| UK unavailable | `unavailable`, no evidence ID, acquisition note | unchanged |

The two already-satisfied Cliff manufacturer requests remain byte-for-byte
unchanged. A critical unavailable manufacturer source durably closes lifecycle
records but blocks P1D3. UK unavailability remains an explicit P1E sourcing risk
and does not by itself block electrical-reference work.

## P1C5 — Pre-acquisition contract corrections

The committed P1C topology contains two inconsistent power statements and one
stale expansion statement. The correction is exact:

- the Teensy 3.3 V rail supplies native Ethernet and the input of the expansion
  current-limit/reverse-blocking protection path;
- `expansion-3v3` is the protected output derived from `teensy-3v3`, not a rail
  derived directly from `protected-5v`;
- the functional `expansion-power` flow payload is `expansion-3v3`;
- the provisional `TPS2553DBVR` is evaluated as a 3.3 V-input protection switch,
  not as a 5 V-to-3.3 V regulator;
- expansion SPI has one chip select and six circuits: SCK, MOSI, MISO, CS, 3.3 V
  and ground;
- native Ethernet, USB host, Program, RTC and SDIO resources are identified from
  retained PJRC evidence. Only S/PDIF RX pin 15, S/PDIF TX pin 14 and status LED
  pin 13 are locked before that evidence-derived map;
- an electrical return-path, impedance or routing failure may select four layers;
  an outline, panel, cable, access or collision failure blocks mechanical redesign
  and is never reported as fixed merely by selecting four layers;
- `REQ-USB-C-RECEPTACLE-MFR` adds `product-page` so the GCT identity page used to
  resolve its datasheet and drawing is retained and auditable.

```yaml
phase: P1C5
unit: contract-correction
seams: [functional power flow, power domains, expansion connector contract, layer-decision policy]
contract-artifact: spec-only
fan-out:
  - name: p1c5-power-contract
    owned-paths:
      - scripts/fixtures/p1c-schema/functional-topology/base-v1.json
      - scripts/fixtures/p1c-schema/interfaces-and-states/base-v1.json
      - scripts/build_p1c_architecture_contracts.py
      - scripts/test_build_p1c_architecture_contracts.py
      - evidence/design/functional-topology.json
      - evidence/design/contracts/functional-topology.json
      - evidence/design/interfaces-and-states.json
      - evidence/design/contracts/interfaces-and-states.json
      - docs/reference/functional-topology.md
      - docs/reference/interfaces-and-states.md
      - evidence/manifest/requests/REQ-USB-C-RECEPTACLE-MFR.json
      - evidence/manifest/supplemental-requests.json
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_build_p1c_architecture_contracts.py
    - python3 scripts/test_functional_topology_schema.py
    - python3 scripts/test_interfaces_states_schema.py
    - python3 scripts/build_p1c_architecture_contracts.py --check
    - python3 scripts/render_p1.py functional-topology --check
    - python3 scripts/render_p1.py interfaces-and-states --check
    - python3 scripts/render_p1.py supplemental-requests --check
    - python3 scripts/validate_p1.py topology --stage handoff
    - node_modules/.bin/prettier --check evidence/manifest/supplemental-requests.json
  buildable: [one internally consistent topology and request handoff]
  parent-commit: "docs(aius): reconcile pre-acquisition contracts"
```

The parent makes the matching one-CS and layer-policy wording corrections in
`aius-hardware-follow-on.md` and records the completed checkpoint in both plan
documents before the P1C5 commit.

## P1D0 — Supplemental evidence contract and tooling

### P1D0a — Realization schema and exact oracle

`supplemental-realization.json` has `schema_version`, `observation_profiles`, then
`realizations`. It contains exactly the 46 non-satisfied requests, sorted by
`request_id`. Every row binds `request_id`, literal `task_name`, exact
`owned_paths`, `source_id`, `authority`, `observation_url`, `authorization`,
`artifacts`, `observation_profile`, `failure_policy`, `retention`, and `notes`.
The schema expands every path; it rejects templates, substitutions and a path
that does not contain the row's exact source ID.

A manufacturer row has null observation fields and one or more artifacts. Each
artifact binds `artifact_kind`, safe basename, initial URL, expected final URL,
project-relative path, `lfs` or `git` storage, media type and revision strategy.
At least one initial URL equals the request authority URL or the row retains that
authority page as `product-page`. A UK row has no artifacts, the exact request
commerce URL, the separate authorization tuple, and the locked profile.

The oracle also defines the source constructor. Manufacturer sources are
`golden`, dynamic sources are `dynamic`, requirements become `claim_scope`, and
request/source/MPN identity is exact. Manufacturer sources include an embedded
`manufacturer_part_number` observation derived from retained evidence.

```yaml
phase: P1D0a
unit: schema-and-oracle
seams: [supplemental requests, fixed realizations, source constructor]
contract-artifact: both
fan-out:
  - name: supplemental-realization-contract
    owned-paths:
      - evidence/manifest/supplemental-realization-schema.json
      - evidence/manifest/supplemental-realization.json
      - scripts/fixtures/supplemental-realization
      - scripts/test_supplemental_realization.py
      - evidence/manifest/README.md
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_supplemental_realization.py
    - python3 scripts/validate_p1.py topology --stage handoff
    - node_modules/.bin/prettier --check evidence/manifest/supplemental-realization-schema.json evidence/manifest/supplemental-realization.json
  buildable: [offline-validated exact 46-request acquisition oracle]
  parent-commit: "docs(aius): lock supplemental evidence realizations"
```

### P1D0b — Per-request acquisition and validation

Deepen `seed_evidence.py`; do not create a parallel retrieval engine. Preserve
every seed command and add:

```text
supplemental acquire <request-id> --retrieved-at <UTC>
supplemental record <request-id> --values <strict-json> --product-final-url <HTTPS> --authorization-final-url <HTTPS> --observed-at <UTC>
supplemental unavailable <request-id> --note <text> --retrieved-at <UTC>
supplemental check-fragment <request-id>
```

The existing HTTPS-only, redirect, timeout, size, signature, media, confinement,
hash, renderer and transactional-publication rules remain one shared
implementation. `acquire` accepts manufacturer requests. `record` accepts UK
requests, performs no page download and embeds the normalized observations in
the source fragment. `unavailable` writes the canonical source and optional
assumption fragment. `check-fragment` validates exactly one request without
requiring siblings.

```yaml
phase: P1D0b
unit: supplemental-cli-adapter
seams: [existing acquisition engine, one request, supplemental fragment]
contract-artifact: stubs-committed
fan-out:
  - name: supplemental-acquisition-cli
    owned-paths:
      - scripts/seed_evidence.py
      - scripts/test_seed_evidence.py
      - scripts/test_supplemental_evidence.py
      - evidence/manifest/README.md
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_seed_evidence.py
    - python3 scripts/test_supplemental_evidence.py
    - python3 scripts/test_validate_evidence.py
    - python3 scripts/test_validate_p1.py
  buildable: [seed-compatible, fixture-proven per-request supplemental CLI]
  parent-commit: "feat(aius): add supplemental evidence acquisition"
```

Tests cover all realization rows, property order, artifact-kind binding,
repeated kinds, Git/LFS media, exact URLs, MPN identity, UK field ranges,
authorization, unavailable branches, symlinks, payload tampering, every publish
rollback boundary, temporary-file cleanup, exit codes and real pinned-Prettier
parity. Production-network tests are not part of this phase.

### P1D0c — Deterministic assembly and lifecycle projection

Add `supplemental assemble --write|--check`. The assembler requires exactly 22
seed and 46 supplemental source fragments, rejects every collision or extra,
derives the exact unavailable-assumption set, and constructs all outputs in
memory before a sorted transactional publish:

- `manifest/sources.json` and `manifest/assumptions.json`;
- `manifest/supplemental-clone-boundary.json` for non-dynamic supplemental
  sources only;
- all 46 request fragments and the 22 affected component fragments;
- `manifest/supplemental-requests.json` and
  `design/provisional-components.csv`.

It preserves key order, validates the proposed complete collection before the
first replacement, fsyncs, and restores every prior byte stream or prior absence
after handled failure. `--check` performs no writes. `validate_p1` rejects the
non-schema `supplemental` class and adds isolated supplemental-fragment checks.

```yaml
phase: P1D0c
unit: deterministic-assembler
seams: [seed aggregate, supplemental fragments, lifecycle state, review aggregates]
contract-artifact: both
fan-out:
  - name: supplemental-assembler
    owned-paths:
      - scripts/seed_evidence.py
      - scripts/test_seed_evidence.py
      - scripts/test_supplemental_evidence.py
      - scripts/validate_evidence.py
      - scripts/test_validate_evidence.py
      - scripts/validate_p1.py
      - scripts/test_validate_p1.py
      - scripts/fixtures/supplemental-assembly
      - scripts/fixtures/validate-p1
      - evidence/manifest/README.md
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_seed_evidence.py
    - python3 scripts/test_supplemental_evidence.py
    - python3 scripts/test_validate_evidence.py
    - python3 scripts/test_validate_p1.py
    - python3 scripts/seed_evidence.py --root scripts/fixtures/supplemental-assembly/valid supplemental assemble --check
  buildable: [fixture-proven atomic supplemental lifecycle closure]
  parent-commit: "feat(aius): assemble supplemental evidence lifecycle"
```

## P1D1 — Supplemental acquisition waves

Each row is one short-lived agent and one request. Owned paths are the exact
supplemental source and optional assumption fragment shown below. Manufacturer
rows additionally own both retained roots shown; the realization determines
which exact files are created. UK rows retain no page bytes. Each wave uses one
parent-issued UTC timestamp, runs all scoped `check-fragment` commands, then
lands one commit before the next wave.

| Wave | Request | Source | Type |
|---:|---|---|---|
| 01 | `REQ-CLIFF-RX-UK` | `AIUS-CLIFF-RX-UK` | UK |
| 01 | `REQ-CLIFF-TX-UK` | `AIUS-CLIFF-TX-UK` | UK |
| 01 | `REQ-ETHERNET-MAGJACK-MFR` | `AIUS-ETHERNET-MAGJACK-MFR` | manufacturer |
| 02 | `REQ-ETHERNET-MAGJACK-UK` | `AIUS-ETHERNET-MAGJACK-UK` | UK |
| 02 | `REQ-EXPANSION-GPIO-ADC-MFR` | `AIUS-EXPANSION-GPIO-ADC-MFR` | manufacturer |
| 02 | `REQ-EXPANSION-GPIO-ADC-UK` | `AIUS-EXPANSION-GPIO-ADC-UK` | UK |
| 03 | `REQ-EXPANSION-I2C-CONTROL-MFR` | `AIUS-EXPANSION-I2C-CONTROL-MFR` | manufacturer |
| 03 | `REQ-EXPANSION-I2C-CONTROL-UK` | `AIUS-EXPANSION-I2C-CONTROL-UK` | UK |
| 03 | `REQ-EXPANSION-I2C-ISOLATOR-MFR` | `AIUS-EXPANSION-I2C-ISOLATOR-MFR` | manufacturer |
| 04 | `REQ-EXPANSION-I2C-ISOLATOR-UK` | `AIUS-EXPANSION-I2C-ISOLATOR-UK` | UK |
| 04 | `REQ-EXPANSION-I2S-TDM-MFR` | `AIUS-EXPANSION-I2S-TDM-MFR` | manufacturer |
| 04 | `REQ-EXPANSION-I2S-TDM-UK` | `AIUS-EXPANSION-I2S-TDM-UK` | UK |
| 05 | `REQ-EXPANSION-POWER-PROTECTION-MFR` | `AIUS-EXPANSION-POWER-PROTECTION-MFR` | manufacturer |
| 05 | `REQ-EXPANSION-POWER-PROTECTION-UK` | `AIUS-EXPANSION-POWER-PROTECTION-UK` | UK |
| 05 | `REQ-EXPANSION-SPI-MFR` | `AIUS-EXPANSION-SPI-MFR` | manufacturer |
| 06 | `REQ-EXPANSION-SPI-UK` | `AIUS-EXPANSION-SPI-UK` | UK |
| 06 | `REQ-EXPANSION-UART-DEBUG-MFR` | `AIUS-EXPANSION-UART-DEBUG-MFR` | manufacturer |
| 06 | `REQ-EXPANSION-UART-DEBUG-UK` | `AIUS-EXPANSION-UART-DEBUG-UK` | UK |
| 07 | `REQ-EXPANSION-USB-HOST-MFR` | `AIUS-EXPANSION-USB-HOST-MFR` | manufacturer |
| 07 | `REQ-EXPANSION-USB-HOST-UK` | `AIUS-EXPANSION-USB-HOST-UK` | UK |
| 07 | `REQ-INTERNAL-USB-CABLE-MFR` | `AIUS-INTERNAL-USB-CABLE-MFR` | manufacturer |
| 08 | `REQ-INTERNAL-USB-CABLE-UK` | `AIUS-INTERNAL-USB-CABLE-UK` | UK |
| 08 | `REQ-MECHANICAL-CABLE-RETAINER-MFR` | `AIUS-MECHANICAL-CABLE-RETAINER-MFR` | manufacturer |
| 08 | `REQ-MECHANICAL-CABLE-RETAINER-UK` | `AIUS-MECHANICAL-CABLE-RETAINER-UK` | UK |
| 09 | `REQ-REMOTE-PROGRAM-RTC-MFR` | `AIUS-REMOTE-PROGRAM-RTC-MFR` | manufacturer |
| 09 | `REQ-REMOTE-PROGRAM-RTC-UK` | `AIUS-REMOTE-PROGRAM-RTC-UK` | UK |
| 09 | `REQ-SPDIF-MUX-MFR` | `AIUS-SPDIF-MUX-MFR` | manufacturer |
| 10 | `REQ-SPDIF-MUX-UK` | `AIUS-SPDIF-MUX-UK` | UK |
| 10 | `REQ-SPDIF-TX-GATE-MFR` | `AIUS-SPDIF-TX-GATE-MFR` | manufacturer |
| 10 | `REQ-SPDIF-TX-GATE-UK` | `AIUS-SPDIF-TX-GATE-UK` | UK |
| 11 | `REQ-TEENSY-ETHERNET-SOCKET-MFR` | `AIUS-TEENSY-ETHERNET-SOCKET-MFR` | manufacturer |
| 11 | `REQ-TEENSY-ETHERNET-SOCKET-UK` | `AIUS-TEENSY-ETHERNET-SOCKET-UK` | UK |
| 11 | `REQ-TEENSY-SIDE-SOCKET-MFR` | `AIUS-TEENSY-SIDE-SOCKET-MFR` | manufacturer |
| 12 | `REQ-TEENSY-SIDE-SOCKET-UK` | `AIUS-TEENSY-SIDE-SOCKET-UK` | UK |
| 12 | `REQ-TEENSY-USB-HOST-SOCKET-MFR` | `AIUS-TEENSY-USB-HOST-SOCKET-MFR` | manufacturer |
| 12 | `REQ-TEENSY-USB-HOST-SOCKET-UK` | `AIUS-TEENSY-USB-HOST-SOCKET-UK` | UK |
| 13 | `REQ-TEST-FIXTURE-HEADER-MFR` | `AIUS-TEST-FIXTURE-HEADER-MFR` | manufacturer |
| 13 | `REQ-TEST-FIXTURE-HEADER-UK` | `AIUS-TEST-FIXTURE-HEADER-UK` | UK |
| 13 | `REQ-USB-A-RECEPTACLE-MFR` | `AIUS-USB-A-RECEPTACLE-MFR` | manufacturer |
| 14 | `REQ-USB-A-RECEPTACLE-UK` | `AIUS-USB-A-RECEPTACLE-UK` | UK |
| 14 | `REQ-USB-C-RECEPTACLE-MFR` | `AIUS-USB-C-RECEPTACLE-MFR` | manufacturer |
| 14 | `REQ-USB-C-RECEPTACLE-UK` | `AIUS-USB-C-RECEPTACLE-UK` | UK |
| 15 | `REQ-USB-ESD-MFR` | `AIUS-USB-ESD-MFR` | manufacturer |
| 15 | `REQ-USB-ESD-UK` | `AIUS-USB-ESD-UK` | UK |
| 15 | `REQ-USB-POWER-PROTECTION-MFR` | `AIUS-USB-POWER-PROTECTION-MFR` | manufacturer |
| 16 | `REQ-USB-POWER-PROTECTION-UK` | `AIUS-USB-POWER-PROTECTION-UK` | UK |

For source `S`, every task owns exactly
`evidence/manifest/supplemental-fragments/S.json` and
`evidence/manifest/supplemental-assumption-fragments/S.json`. A manufacturer
task also owns `docs/reference/datasheets.mdS/` and `evidence/source-snapshots/S/`.
These four roots are the only formula in the plan; the 46 literal request/source
pairs above are the finite review oracle. P1D0a expands the formula into each
row's literal `task_name` and `owned_paths`; P1D1 dispatch reads those committed
values and performs no path derivation.

Each wave is a separate checkpoint phase named `P1D1.01` through `P1D1.16`.
There is no 16-wave uncommitted phase.

```yaml
phase: P1D1.01-P1D1.16
unit: single-supplemental-request
seams: [fixed realization row, bounded acquisition or normalized observation, isolated fragment check]
contract-artifact: both
fan-out: 46 literal tasks in the 16-wave table, maximum 3 concurrent
coordination: no shared authored files; parent allocates timestamp and commits each wave
review-intensity: active
commit-checkpoint:
  green:
    - scoped supplemental check-fragment for every row in the wave
    - python3 scripts/test_supplemental_evidence.py
  buildable: [durable independently validated source outcomes for the completed wave]
  parent-commit: "docs(aius): archive supplemental evidence wave NN"
```

## P1D2 — Serial integration and lifecycle closure

P1D2 starts only after all 46 isolated checks pass. It runs the deterministic
assembler, closes every request and component lifecycle, regenerates both review
aggregates, and records any manufacturer blocker. No design task starts if a
critical manufacturer source is unavailable.

```yaml
phase: P1D2
unit: parent-only
seams: [validated supplemental fragments, immutable seed set, lifecycle projection]
contract-artifact: both
fan-out:
  - name: p1d2-integration
    owned-paths:
      - evidence/manifest/sources.json
      - evidence/manifest/assumptions.json
      - evidence/manifest/supplemental-clone-boundary.json
      - evidence/manifest/requests
      - evidence/design/components
      - evidence/manifest/supplemental-requests.json
      - evidence/design/provisional-components.csv
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py supplemental assemble --write
    - python3 scripts/seed_evidence.py supplemental assemble --check
    - python3 scripts/validate_evidence.py
    - python3 scripts/render_p1.py supplemental-requests --check
    - python3 scripts/render_p1.py components --check
    - python3 scripts/validate_p1.py topology --stage handoff
    - exact-path pinned Prettier checks
  buildable: [closed supplemental source, request and component lifecycle]
  parent-commit: "docs(aius): assemble supplemental component evidence"
```

## P1D3 — Electrical reference contract and package

### P1D3a1 — Closed design schemas and base fixtures

Create four closed schemas and disjoint valid/invalid fixtures before design
facts or shared generator code are authored:

- `electrical-reference-schema.json`;
- `pin-resource-map-schema.json`;
- `two-layer-placement-schema.json`;
- `reference-schematic-contract-schema.json`.

Electrical signal rows use integer engineering units and fields
`signal_id`, `parent_signal_id`, `kind`, `source`, `sinks`, `direction`,
`voltage_domain`, `source_vol_max_mv`, `source_voh_min_mv`, `sink_vil_max_mv`,
`sink_vih_min_mv`, `low_margin_mv`, `high_margin_mv`,
`max_source_current_ua`, `max_sink_current_ua`, `required_bandwidth_kbps`,
`supported_bandwidth_kbps`, `default_state`, `support_circuit_ids`,
`requirement_ids`, `evidence_ids`, `status`, and `notes`. Margins are
non-negative and supported bandwidth covers required bandwidth. Power rows bind
min/nominal/max millivolts, continuous/peak/source-limit milliamps, reverse
leakage microamps, support circuits, requirements, evidence and status.

The pin JSON preserves the 14 committed CSV fields. Physical pins are unique
unless rows share one non-empty bus and an identical mux/peripheral/DMA/clock
tuple. Dedicated Ethernet, USB-host, Program, RTC and SDIO pad identities come
from retained PJRC evidence, not memory.

Placement JSON records board datum and outline, AABB placements and keepouts,
route budgets, ground corridors, cable volumes, collision checks and criteria.
Electrical criteria may select four layers. Mechanical failure sets release
blocked and requires redesign.

```yaml
phase: P1D3a1
unit: schema-test-surface
seams: [electrical rows, pin rows, placement geometry, schematic acceptance]
contract-artifact: both
fan-out:
  - name: electrical-schema
    owned-paths:
      - evidence/design/electrical-reference-schema.json
      - scripts/fixtures/p1d-schema/electrical-reference
  - name: pin-resource-schema
    owned-paths:
      - evidence/design/pin-resource-map-schema.json
      - scripts/fixtures/p1d-schema/pin-resource-map
  - name: placement-schema
    owned-paths:
      - evidence/design/two-layer-placement-schema.json
      - scripts/fixtures/p1d-schema/two-layer-placement
  - name: schematic-contract-schema
    owned-paths:
      - evidence/design/reference-schematic-contract-schema.json
      - scripts/fixtures/p1d-schema/reference-schematic-contract
  - name: p1d3-schema-test-coordinator
    owned-paths: [scripts/test_p1d_schemas.py]
coordination: four disjoint schema tasks run with maximum three concurrent; test coordinator runs last
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_p1d_schemas.py
    - node_modules/.bin/prettier --check evidence/design/electrical-reference-schema.json evidence/design/pin-resource-map-schema.json evidence/design/two-layer-placement-schema.json evidence/design/reference-schematic-contract-schema.json
  buildable: [closed independently fixture-proven design schemas]
  parent-commit: "feat(aius): define electrical reference schemas"
```

One coordinator owns `scripts/test_p1d_schemas.py` after the four disjoint schema
tasks return. It registers all fixture directories without modifying their
owned files.

### P1D3a2 — Contract builder and deterministic renderers

The serial coordinator creates the valid base package, validates canonical JSON
and emits electrical Markdown, pin CSV plus Markdown, and placement Markdown.
Renderer fixtures cover every escaping and ordering boundary. `render_p1.py`
adds exact subcommands `electrical-reference`, `pin-resource-map`, and
`two-layer-placement`, each with `--write` and `--check`.

```yaml
phase: P1D3a2
unit: design-contract-generator
seams: [closed schemas, canonical package, deterministic review artifacts]
contract-artifact: stubs-committed
fan-out:
  - name: p1d3-contract-generator
    owned-paths:
      - evidence/design/contracts/p1d-reference-package.json
      - evidence/design/electrical-reference.json
      - evidence/design/pin-resource-map.json
      - evidence/design/two-layer-placement.json
      - electronics/reference/reference-schematic-contract.json
      - scripts/build_p1d_reference_contract.py
      - scripts/test_build_p1d_reference_contract.py
      - scripts/render_p1.py
      - scripts/test_render_p1.py
      - scripts/fixtures/render-p1/p1d-reference
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_build_p1d_reference_contract.py
    - python3 scripts/test_render_p1.py
    - python3 scripts/build_p1d_reference_contract.py --check
  buildable: [fixture-proven closed design contracts and deterministic renderers]
  parent-commit: "feat(aius): define electrical reference contracts"
```

### P1D3b — Evidence-derived design records

Three disjoint agents author canonical JSON against the committed schemas. The
renderers are already committed, so agents do not share code files.

```yaml
phase: P1D3b
unit: single-design-record
seams: [supplemental evidence, closed design schemas, deterministic renderers]
contract-artifact: stubs-committed
fan-out:
  - name: electrical-reference
    owned-paths:
      - evidence/design/electrical-reference.json
      - docs/reference/electrical-reference.md
  - name: pin-resource-map
    owned-paths:
      - evidence/design/pin-resource-map.json
      - docs/reference/pin-resource-map.csv
      - docs/reference/pin-resource-map.md
  - name: two-layer-placement
    owned-paths:
      - evidence/design/two-layer-placement.json
      - docs/reference/two-layer-placement.md
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/build_p1d_reference_contract.py --check
    - python3 scripts/render_p1.py electrical-reference --check
    - python3 scripts/render_p1.py pin-resource-map --check
    - python3 scripts/render_p1.py two-layer-placement --check
  buildable: [evidence-closed electrical, pin and placement records]
  parent-commit: "docs(aius): establish electrical design records"
```

### P1D3c — Non-manufacturing reference schematic

This task is serial and starts after P1D3b is committed. It creates a root
`aius-reference.kicad_sch` with hierarchical sheets `power-usb.kicad_sch`,
`optical-spdif.kicad_sch`, `ethernet.kicad_sch`, and
`teensy-expansion.kicad_sch`, a local symbol library, README and the canonical
schematic contract. There is no `.kicad_pro`. ERC has zero waivers or ignored
violations.

```yaml
phase: P1D3c
unit: reference-schematic
seams: [electrical record, pin record, placement record, KiCad ERC]
contract-artifact: stubs-committed
fan-out:
  - name: reference-schematic
    owned-paths: [electronics/reference]
review-intensity: active
commit-checkpoint:
  green:
    - scripts/kicad.sh sch erc electronics/reference/aius-reference.kicad_sch
    - python3 scripts/build_p1d_reference_contract.py --check
  buildable: [annotated ERC-clean non-manufacturing reference schematic]
  parent-commit: "design(aius): add electrical reference schematic"
```

## P1D4 — Serial final release gate

Extend `validate_p1.py topology --stage final` only after all package artifacts
exist. The validator does not invoke Docker or KiCad; the release script runs
those external checks explicitly. The stage cumulatively requires `handoff`,
exact schemas and JSON/CSV/Markdown parity, signal and power requirement
coverage, pin uniqueness, evidence closure, layer/mechanical verdict semantics,
schematic contract membership and supplemental lifecycle closure.

```yaml
phase: P1D4
unit: final-validation-surface
seams: [complete P1D package, cumulative validator, external ERC gate]
contract-artifact: both
fan-out:
  - name: p1d-final-validator
    owned-paths:
      - scripts/validate_p1.py
      - scripts/test_validate_p1.py
      - scripts/fixtures/validate-p1/final
      - scripts/validate_p1d_release.sh
      - evidence/manifest/README.md
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_validate_p1.py
    - python3 scripts/validate_evidence.py
    - python3 scripts/validate_p1.py topology --stage final
    - python3 scripts/build_p1d_reference_contract.py --check
    - python3 scripts/render_p1.py functional-topology --check
    - python3 scripts/render_p1.py interfaces-and-states --check
    - python3 scripts/render_p1.py components --check
    - python3 scripts/render_p1.py supplemental-requests --check
    - python3 scripts/render_p1.py electrical-reference --check
    - python3 scripts/render_p1.py pin-resource-map --check
    - python3 scripts/render_p1.py two-layer-placement --check
    - scripts/kicad.sh sch erc electronics/reference/aius-reference.kicad_sch
    - node_modules/.bin/prettier --check evidence/design/electrical-reference-schema.json evidence/design/electrical-reference.json evidence/design/pin-resource-map-schema.json evidence/design/pin-resource-map.json evidence/design/two-layer-placement-schema.json evidence/design/two-layer-placement.json evidence/design/reference-schematic-contract-schema.json electronics/reference/reference-schematic-contract.json
    - the repo tooling tool doc-index -- --check
  buildable: [audited non-manufacturing electrical reference package]
  parent-commit: "docs(aius): release electrical reference package"
```

After these checks pass, one clean-room delivery reviewer receives only this
plan and the current code. Missing promises are fixed; intentional divergence
is recorded in this plan's phase notes. The parent then lands and pushes P1D4.
