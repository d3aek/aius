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

## Pre-build gate status

The first clean-room review of commit `eedf99281` returned `BLOCKED`. It found
schema-incompatible artifact-kind placement, an incomplete realization oracle,
P1C power/request drift, no committable manufacturer-unavailable branch,
underspecified P1D3 schemas, oversized tasks, and false stub declarations. This
revision incorporates those findings. Implementation remains blocked until a
fresh clean-room review passes the amended plans, literal realization oracle and
current code.

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

The unavailable assumption is exact: ID `ASM-UNAVAILABLE-<source-id>`, claim
`Supplemental request <request-id> source <source-id> is unavailable for:
<requirements joined by "; ">`, consequence `The selected component remains
provisional until <request-id> is replaced with current evidence.`, owner `P1D`,
validation gate `Replace unavailable supplemental evidence`, empty evidence IDs,
one source ID, the request requirements as claim scope, status `open`, and null
resolution. Manufacturer unavailability is a design-gate blocker; UK
unavailability is not.

Reviewable HTML product-page snapshots are normal Git files under
`evidence/source-snapshots/<source-id>/`. Datasheets, drawings, CAD archives and
non-reviewable page captures are LFS files under
`docs/reference/datasheets.md<source-id>/`. `artifact_kind` belongs to the supplemental
realization control row, not the source artifact object; the existing
`source-schema.json` and all seed records remain unchanged. For a successful
manufacturer source, artifact array position binds each retained source artifact
to its realization artifact and the ordered unique realization kinds equal the
request's `artifact_kinds`. Repeated files of one kind are permitted. Dynamic
success has no artifacts. The canonical unavailable external-reference artifact
is structural failure metadata and is exempt from request-kind equality.

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

The profile object has exact keys `name`, then `fields`. Each field descriptor
has exact keys `key`, `value_type`, `nullable`, `constraint`, `unit`,
`source_role`, and `basis`. `source_role` is `product` or `authorization` and
selects the corresponding final URL for the generated observation's
`source_url`. A constraint has exact keys `kind`, then `value`; supported kinds
are `const`, `enum`, `minimum`, `exclusive-minimum`, and `pattern`.

The MPN equals the request MPN. URLs are HTTPS. `currency` is `GBP` and
`price_includes_vat` is `false`; an inclusive-only price is unavailable unless
separate evidence supports the ex-VAT normalization. `stock_state` is
`in-stock`, `out-of-stock`, `backorder` or `unknown`. Quantity is a non-negative
integer or null under the state rules; price quantity, MOQ and order multiple
are positive piece counts; the selected quantity must be orderable. The common
basis is `P1D uk-distributor-price-stock normalized field <key> from the approved
supplemental request, realization, and live page.`

Identity, distributor and URL fields are non-null strings. MPN, distributor,
product initial URL, authorization authority/method/initial URL, currency and
VAT flag use `const`. Supplier SKU and final URLs use non-empty/HTTPS patterns.
Stock state uses the four-value enum. Stock quantity is a nullable integer with
minimum zero; price quantity, MOQ and order multiple are integers with minimum
one; unit price is a number with minimum zero. Cross-field validation enforces
the state/quantity matrix, `price_quantity >= minimum_order_quantity`, and
`price_quantity % order_multiple == 0`.

Authorization is independently bound to the distributor. Tag-Connect is the
only permitted `ecia-trustedparts-exception`, using
`https://www.trustedparts.com/en/manufacturers/tag-connect`; all other rows use
the selected manufacturer's page or authorized-distributor locator. Product
commerce and authorization URLs remain separate facts. Authorization has status
`confirmed` or `unresolved`. An unresolved committed tuple is still exact, but
its acquisition task may only publish the canonical unavailable outcome; it may
not record commerce observations. P1D0a locks Pulse/DigiKey UK and
StarTech/RS UK as unresolved because the reviewed official resources did not
establish the required regional/product authorization.

A realization authorization object has exact keys `status`, `authority`,
`method`, `initial_url`, then `basis`. `method` is `manufacturer-page`,
`manufacturer-locator`, or `ecia-trustedparts-exception`; the final token is
permitted only for Tag-Connect. A manufacturer realization uses null
authorization.

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

The P1C0a base fixtures remain hash-locked historical inputs. Corrections are
new explicit migrations in the existing builder and are proved by exact builder
tests; no task rewrites an immutable base.

### P1C5a — Topology schema and migration correction

```yaml
phase: P1C5a
unit: topology-contract-migration
seams: [functional payload vocabulary, deterministic P1C builder, power-domain migration]
contract-artifact: spec-only
fan-out:
  - name: p1c5-topology-contract
    owned-paths:
      - evidence/design/functional-topology-schema.json
      - scripts/test_functional_topology_schema.py
      - scripts/fixtures/p1c-schema/functional-topology/invalid
      - scripts/build_p1c_architecture_contracts.py
      - scripts/test_build_p1c_architecture_contracts.py
coordination: parent runs committed builders and renderers to update generated contracts, valid fixtures, JSON mirrors, and Markdown; immutable base fixtures are untouched
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/build_p1c_architecture_contracts.py --write
    - python3 scripts/render_p1.py functional-topology --write
    - python3 scripts/render_p1.py interfaces-and-states --write
    - python3 scripts/test_build_p1c_architecture_contracts.py
    - python3 scripts/test_functional_topology_schema.py
    - python3 scripts/test_interfaces_states_schema.py
    - python3 scripts/build_p1c_architecture_contracts.py --check
    - python3 scripts/render_p1.py functional-topology --check
    - python3 scripts/render_p1.py interfaces-and-states --check
    - python3 scripts/validate_p1.py topology --stage handoff
  buildable: [schema-valid internally consistent power topology]
  parent-commit: "docs(aius): reconcile expansion power topology"
```

The parent-generated exact paths are the two generated contract JSON files, the
two valid fixtures, the two review JSON mirrors, and the two reference Markdown
files already owned by their committed generators.

### P1C5b — USB-C request-kind correction

```yaml
phase: P1C5b
unit: single-request-correction
seams: [selected GCT identity, retained artifact kinds, request aggregate]
contract-artifact: spec-only
fan-out:
  - name: p1c5-usb-c-request
    owned-paths: [evidence/manifest/requests/REQ-USB-C-RECEPTACLE-MFR.json]
coordination: parent runs the committed supplemental-request renderer; no agent edits the shared aggregate
review-intensity: observer
commit-checkpoint:
  green:
    - python3 scripts/render_p1.py supplemental-requests --write
    - python3 scripts/render_p1.py supplemental-requests --check
    - python3 scripts/test_render_p1.py
    - python3 scripts/validate_p1.py topology --stage handoff
    - node_modules/.bin/prettier --check evidence/manifest/requests/REQ-USB-C-RECEPTACLE-MFR.json evidence/manifest/supplemental-requests.json
  buildable: [exact auditable USB-C manufacturer evidence request]
  parent-commit: "docs(aius): retain USB-C manufacturer identity page"
```

The one-CS and layer-policy text corrections are committed in the plan amendment
before P1C5a dispatch. The parent records checkpoint completion in both normative
plans at each commit.

## P1D0 — Supplemental evidence contract and tooling

### P1D0a — Realization schema around the precommitted oracle

`supplemental-realization.json` has `schema_version`, `observation_profiles`, then
`realizations`. It contains exactly the 46 non-satisfied requests, sorted by
`request_id`. Every row binds `request_id`, literal `task_name`, exact
`owned_paths`, `source_id`, `authority`, `revision_strategy`, `revision_value`,
`observation_url`, `authorization`, `artifacts`, `observation_profile`,
`failure_policy`, `retention`, and `notes`.
The schema expands every path; it rejects templates, substitutions and a path
that does not contain the row's exact source ID.

A manufacturer row has `sha256` revision strategy, null revision value, null
observation fields and one or more artifacts. Each artifact binds
`artifact_kind`, safe basename, initial URL, expected final URL,
project-relative path, `lfs` or `git` storage, and media type.
At least one initial URL equals the request authority URL or the row retains that
authority page as `product-page`. A UK row has `observation-date` revision
strategy, null revision value, no artifacts, the exact request commerce URL,
the separate authorization tuple, and the locked profile.

The oracle also defines the source constructor. ID is request `source_id`;
authority is realization `authority`; requirements become `claim_scope`;
landing URL is request `authority_url`; status is `current`; supersedes is null.
Manufacturer title is `<manufacturer> <MPN> primary manufacturer evidence`,
purpose is `Satisfy <request-id> for <family>.`, class is `golden`, revision is
`sha256:<digest>`, where digest is SHA-256 over the ASCII concatenation of each
lowercase artifact SHA-256 hex value in realization order, and redistribution note is
`Retained for personal noncommercial research; no republication permission is
inferred.` It contains one embedded MPN observation with key
`manufacturer_part_number`, request MPN value, null unit, basis
`Selected MPN identified by the retained primary manufacturer evidence for
<request-id>.`, acquisition timestamp, and request authority URL.

UK title is `normalized UK distributor price and stock observation`, purpose is
`Record a UK authorized-distributor price and stock observation for
<manufacturer> <MPN>.`, class is `dynamic`, revision is
`observed:<YYYY-MM-DD>`, and its redistribution note is the locked normalized
observation note above. All generated source objects otherwise follow the
existing source-schema key order.

The literal 46-row oracle is committed with this plan at
`evidence/manifest/supplemental-realization.json`, SHA-256
`2c77f910ee5254441ecca2abf7d8a312fd255777ba662b45560524f5f531e581`.
Its 35 retained artifacts and all 24 authorization tuples were selected by
the contract review, not by the schema implementer. P1C5b changes the one known
predecessor mismatch, the GCT request's missing `product-page` kind, before this
phase dispatches.

```yaml
phase: P1D0a
unit: realization-schema-test-surface
seams: [supplemental requests, fixed realizations, source constructor]
contract-artifact: spec-only
fan-out:
  - name: supplemental-realization-contract
    owned-paths:
      - evidence/manifest/supplemental-realization-schema.json
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

No command bypasses an access control or substitutes browser-captured bytes.
The pre-recorded GCT and TE access hazards therefore use the ordinary
golden-block unavailable path if exact retrieval fails. A UK row whose
authorization status is unresolved rejects `record` and accepts only
`unavailable`. Exit 0 means successful publication/check, exit 1 means handled
acquisition/record/UK-unavailable failure, exit 2 means CLI usage, and exit 3
means a durably published manufacturer-unavailable design blocker.

```yaml
phase: P1D0b
unit: supplemental-cli-adapter
seams: [existing acquisition engine, one request, supplemental fragment]
contract-artifact: spec-only
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
after handled failure. `--check` performs no writes. This phase changes the deep
evidence engine and its tests only; validator integration follows in separate
checkpoints.

```yaml
phase: P1D0c
unit: deterministic-assembler
seams: [seed aggregate, supplemental fragments, lifecycle state, review aggregates]
contract-artifact: spec-only
fan-out:
  - name: supplemental-assembler
    owned-paths:
      - scripts/seed_evidence.py
      - scripts/test_seed_evidence.py
      - scripts/test_supplemental_evidence.py
      - scripts/fixtures/supplemental-assembly
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_seed_evidence.py
    - python3 scripts/test_supplemental_evidence.py
    - python3 scripts/seed_evidence.py --root scripts/fixtures/supplemental-assembly/valid supplemental assemble --check
  buildable: [fixture-proven atomic supplemental lifecycle closure]
  parent-commit: "feat(aius): assemble supplemental evidence lifecycle"
```

### P1D0d — Evidence collection validation

The evidence validator learns the separate supplemental directories, exact
source/assumption set rules, realization-to-source artifact-position binding,
and supplemental clone-boundary membership. It accepts the current seed-only
tree and a complete fixture tree, and rejects partial or colliding collections.

```yaml
phase: P1D0d
unit: evidence-validator-surface
seams: [source schema, seed fragments, supplemental fragments, clone boundaries]
contract-artifact: spec-only
fan-out:
  - name: supplemental-evidence-validator
    owned-paths:
      - scripts/validate_evidence.py
      - scripts/test_validate_evidence.py
      - scripts/fixtures/supplemental-assembly
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_validate_evidence.py
    - python3 scripts/validate_evidence.py
    - python3 scripts/test_seed_evidence.py
  buildable: [seed-compatible supplemental evidence validator]
  parent-commit: "feat(aius): validate supplemental evidence collections"
```

### P1D0e — P1 lifecycle validation

The P1 validator adds isolated supplemental-fragment and lifecycle-valid checks,
then removes the schema-invalid `supplemental` class allowance. A satisfied
manufacturer request requires exactly a current `golden` source, a retained
artifact set position-bound to the realization, and an embedded matching-MPN
observation. `comparative` never satisfies it. New stage
`topology --stage supplemental-lifecycle` requires all 48 requests to be closed,
enforces the lifecycle matrix, and exits zero for a structurally correct
unavailable manufacturer outcome. The existing cumulative `handoff` stage
remains the separate design gate and rejects a critical
`candidate-evidence-unavailable` component with its exact diagnostic.

```yaml
phase: P1D0e
unit: p1-lifecycle-validator-surface
seams: [request state, component state, source class, deterministic design gate]
contract-artifact: spec-only
fan-out:
  - name: supplemental-p1-validator
    owned-paths:
      - scripts/validate_p1.py
      - scripts/test_validate_p1.py
      - scripts/fixtures/validate-p1
      - evidence/manifest/README.md
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_validate_p1.py
    - python3 scripts/validate_p1.py topology --stage handoff
    - python3 scripts/test_validate_evidence.py
  buildable: [seed-compatible lifecycle and design-gate validator]
  parent-commit: "feat(aius): validate supplemental evidence lifecycle"
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
    - python3 scripts/validate_p1.py topology --stage supplemental-lifecycle
    - python3 scripts/render_p1.py supplemental-requests --check
    - python3 scripts/render_p1.py components --check
    - exact-path pinned Prettier checks
  buildable: [closed supplemental source, request and component lifecycle in either design-gate branch]
  parent-commit: "docs(aius): assemble supplemental component evidence"
```

P1D2 has two explicit committable branches after the common green set:

- **eligible:** no critical manufacturer is unavailable;
  `topology --stage handoff` exits zero and P1D3 may dispatch;
- **blocked:** one or more critical manufacturers are unavailable; the parent
  runs `handoff`, records the exact expected critical-selection diagnostic in
  the phase note, commits the lifecycle-valid state, and does not dispatch P1D3.
  Any additional diagnostic is a failure. Replacement selection returns to a
  new reviewed P1C candidate checkpoint before P1D1 resumes.

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
`requirement_ids`, `evidence_ids`, `status`, and `notes`. Margins are signed so
an incompatibility can be retained; a passing row requires both applicable
margins non-negative and supported bandwidth covers required bandwidth. Power rows bind
min/nominal/max millivolts, continuous/peak/source-limit milliamps, reverse
leakage microamps, support circuits, requirements, evidence and status.

The pin JSON preserves the 14 locked CSV fields. Physical pins are unique
unless rows share one non-empty bus and an identical mux/peripheral/DMA/clock
tuple. Dedicated Ethernet, USB-host, Program, RTC and SDIO pad identities come
from retained PJRC evidence, not memory.

Placement JSON records board datum and outline, AABB placements and keepouts,
route budgets, ground corridors, cable volumes, collision checks and criteria.
Electrical criteria may select four layers. Mechanical failure sets release
blocked and requires redesign.

#### Common closed-schema rules

All four roots begin with `schema_version`, constant `1.0.0`, then
`document_state`, enum `placeholder` or `authored`. Every object rejects
additional properties. Internal IDs match
`^[a-z0-9]+(?:-[a-z0-9]+)*$`, evidence IDs match
`^[A-Z0-9]+(?:-[A-Z0-9]+)*$`, and requirements match
`^REQT-[A-Z0-9]+(?:-[A-Z0-9]+)*$`. Identifier arrays are unique and sorted;
record arrays sort by their first ID field. Root requirement/evidence arrays are
the exact sorted nested unions and resolve against selection policy and sources.
Cross-row order, uniqueness, arithmetic and references are enforced by
`test_p1d_schemas.py` because the repository subset validator does not implement
them. Engineering row status is `pass` or `fail`; a failure has non-empty notes
and blocks P1D3/P1D4 release.

Placeholders have empty factual arrays, empty requirement/evidence arrays and
empty notes. Nullable root objects are null. An authored document contains the
complete evidence-derived facts; it never uses placeholder values.

#### Electrical-reference contract

Root key order is `schema_version`, `document_state`, `support_circuits`,
`signals`, `power_rails`, `requirement_ids`, `evidence_ids`, `notes`.

A support-circuit row keys `circuit_id`, `kind`, `description`,
`requirement_ids`, `evidence_ids`, `status`, `notes`; kind is `power`,
`protection`, `interface`, `logic`, `isolation`, `passive-network`, `module` or
`test`. Description and evidence are non-empty; requirements may be empty.

A signal row has the 23 keys listed above in that exact order. `kind` is
`single-ended-digital`, `differential-pair`, `differential-leg`, `control` or
`clock`; direction is `input-to-carrier`, `output-from-carrier`, `internal` or
`bidirectional`; voltage domain is a committed interface-state domain or
`usb2-phy`/`ethernet-mdi`. Source is non-empty or null only for bidirectional;
sinks are non-empty and bidirectional has at least two. Threshold fields are
non-negative integer millivolts or null; margins are signed integer millivolts
or null; currents and bandwidths are non-negative integers. Default state is
`low`, `high`, `high-impedance`, `disabled`, `processed-path`, `input`,
`protocol-defined` or `not-applicable`. Support circuits and evidence are
non-empty.

The six threshold/margin fields are all integers or all null and are required
for single-ended digital, control and clock rows. Exact formulae are
`low_margin_mv = sink_vil_max_mv - source_vol_max_mv` and
`high_margin_mv = source_voh_min_mv - sink_vih_min_mv`. Pass additionally
requires source current at least sink current and supported bandwidth at least
required bandwidth. Parent references are acyclic; a differential leg has one
differential-pair parent and every pair has exactly two legs.

A power row keys `rail_id`, `source`, `consumers`, `minimum_mv`, `nominal_mv`,
`maximum_mv`, `continuous_load_ma`, `peak_load_ma`, `source_limit_ma`,
`reverse_leakage_ua`, `default_state`, `support_circuit_ids`, `requirement_ids`,
`evidence_ids`, `status`, `notes`. Rail ID is one of `usb-vbus-raw`,
`protected-5v`, `teensy-3v3`, `rtc-backup`, `expansion-3v3`, or
`expansion-usb-host-vbus`; numeric values are non-negative integers; default is
`off`, `on`, `current-limited` or `battery-backed`. Pass requires
`minimum_mv <= nominal_mv <= maximum_mv` and
`continuous_load_ma <= peak_load_ma <= source_limit_ma`. Authored state has
exactly all six rails and covers every `P1D-electrical` requirement.

#### Pin-resource contract

Root key order is `schema_version`, `document_state`, `pins`, `evidence_ids`,
`notes`. A pin row has exactly the CSV fields in order: `signal_id`,
`physical_pin`, `pad_mux`, `peripheral`, `instance_signal`, `direction`,
`voltage_domain`, `dma_request`, `clock_root`, `shared_bus`,
`reservation_owner`, `status`, `evidence_ids`, `notes`.

Physical pin is a non-empty string; numbered Teensy pins use decimal spelling.
Mux/peripheral/instance/clock/bus are non-empty or null. Direction is
`input-to-teensy`, `output-from-teensy`, `bidirectional`, `power` or `ground`;
voltage domain is one of the eight interface-state domains; DMA is a
non-negative integer or null. Owner is null or `spdif-rx`, `spdif-tx`,
`status-led`, `native-ethernet`, `usb-host`, `program`, `rtc`, `sdio`,
`expansion-i2c-control`, `expansion-spi`, `expansion-uart-debug`,
`expansion-i2s-tdm`, `expansion-gpio-adc`, `test-fixture`, or
`general-purpose-reserve`. Status is `allocated`, `reserved`, `available` or
`conflict`; available alone has null owner and conflict has non-empty notes.

Duplicate physical pins require one identical non-null shared bus and identical
mux, peripheral, instance signal, DMA and clock tuple. S/PDIF RX/TX are exactly
pins `15`/`14`; status LED reserves `13`. Authored owner coverage includes the
evidence-derived dedicated Ethernet, USB-host, Program, RTC and SDIO pads plus
one I2C, one one-CS SPI, one UART pair, one I2S/TDM, GPIO/ADC and both S/PDIF
signals. CSV and Markdown are projections only.

#### Two-layer-placement contract

Root key order is `schema_version`, `document_state`, `board`, `placements`,
`keepouts`, `routes`, `ground_corridors`, `cables`, `collision_checks`,
`criteria`, `layer_decision`, `requirement_ids`, `evidence_ids`, `notes`.
Geometry is integer micrometres, impedance integer milliohms and rotation integer
millidegrees.

Board keys `board_id`, `enclosure_mpn`, `datum`, `outline_width_um`,
`outline_height_um`, `thickness_um`, `guide_clearance_um`, `evidence_ids`,
`status`, `notes`; constants are `aius-carrier-reference-envelope`,
`1455N1201BK`, and `rear-left-board-bottom`, and pass outline dimensions are at
most 100000.

Placement keys `placement_id`, `item_id`, `kind`, `side`, `x_min_um`,
`y_min_um`, `z_min_um`, `x_max_um`, `y_max_um`, `z_max_um`, `rotation_mdeg`,
`panel_datum`, `connector_face_setback_um`, `evidence_ids`, `status`, `notes`.
Kind is `module`, `connector`, `integrated-circuit`, `passive`, `mechanical` or
`test-interface`; side is `top`, `bottom` or `through`; rotation is 0, 90000,
180000 or 270000. Panel datum is null or `rear-interior`, with a setback iff
non-null; rear pass is at most 5000. Extents are ordered and within the board.

Keepout keys `keepout_id`, `owner_id`, `kind`, `x_min_um`, `y_min_um`,
`z_min_um`, `x_max_um`, `y_max_um`, `z_max_um`, `applies_to`, `evidence_ids`,
`status`, `notes`. Kind is `component-body`,
`connector-mating`, `cable-bend`, `tool-access`, `enclosure`, `routing` or
`test-access`; applies-to values are `placement`, `copper`, `via`, `component`,
`cable` or `tool`.

Route keys `route_id`, `interface`, `source_placement_id`, `sink_placement_id`,
`maximum_length_um`, `estimated_length_um`, `layer`, `via_count`,
`target_diff_impedance_mohm`, `impedance_tolerance_mohm`,
`ground_corridor_id`, `evidence_ids`, `status`, `notes`. Interface is
`usb2-device` or `ethernet-100base-tx`; layer is `top`; pass has no vias and
estimated length within maximum. USB constants are 30000/90000 for maximum
length/target impedance; Ethernet constants are 50000/100000.

Ground-corridor keys `corridor_id`, `route_ids`, `reference_layer`,
`continuous`, `minimum_width_um`, `split_count`, `neck_count`,
`via_transition_count`, `evidence_ids`, `status`, `notes`. Pass uses bottom,
continuous, zero splits and zero transitions; a non-zero neck fails unless its
adequacy is separately evidenced in notes.

Cable keys `cable_id`, `component_id`, `source_placement_id`,
`sink_placement_id`, `x_min_um`, `y_min_um`, `z_min_um`, `x_max_um`,
`y_max_um`, `z_max_um`, `minimum_bend_radius_um`,
`service_slack_um`, `insertion_axis`, `retainer_placement_id`, `evidence_ids`,
`status`, `notes`; insertion axis is signed X/Y/Z. Collision keys `check_id`,
`subject_id`, `obstacle_id`, `required_clearance_um`,
`measured_clearance_um`, `evidence_ids`, `status`, `notes`; pass requires
measured at least required.

Criterion keys `criterion_id`, `category`, `metric`, `actual_value`,
`minimum_value`, `maximum_value`, `unit`, `status`, `failure_action`,
`requirement_ids`, `evidence_ids`, `notes`. Category is electrical/mechanical;
metric is `board-outline`, `guide-fit`, `rear-face-setback`, `route-length`,
`via-count`, `differential-impedance`, `ground-continuity`, `cable-clearance`,
`insertion-access` or `collision-clearance`; unit is `um`, `count`, `mohm` or
`boolean`. At least one bound is non-null. Electrical failure action is
`select-four-layer`; mechanical is `block-mechanical-redesign`.

Layer-decision keys `evaluated_layer_count`, `selected_layer_count`,
`electrical_failure_ids`, `mechanical_failure_ids`, `release_state`, `decision`,
`rationale`, `evidence_ids`, `notes`. Evaluated count is 2. No failures selects
2/proceed/`two-layer-pass`; electrical-only failures select
4/proceed/`four-layer-required`; any mechanical failure selects
null/blocked/`mechanical-redesign-required`. Failure arrays exactly equal failed
criterion IDs and mechanical dominates.

#### Reference-schematic contract

Root key order is `schema_version`, `document_state`, `root_schematic`, `sheets`,
`symbol_libraries`, `net_bindings`, `test_points`, `erc_policy`,
`requirement_ids`, `evidence_ids`, `notes`.

Root-schematic keys `path`, `title`, `hierarchical_sheet_ids`,
`symbol_library_ids`, `evidence_ids`, `status`, `notes`; exact path is
`electronics/reference/aius-reference.kicad_sch` and sheet order is
`power-usb`, `optical-spdif`, `ethernet`, `teensy-expansion`.

Sheet keys `sheet_id`, `path`, `title`, `support_circuit_ids`, `component_ids`,
`signal_ids`, `power_rail_ids`, `requirement_ids`, `evidence_ids`, `status`,
`notes`. Exact ID/path pairs are `power-usb`/
`electronics/reference/power-usb.kicad_sch`, `optical-spdif`/
`electronics/reference/optical-spdif.kicad_sch`, `ethernet`/
`electronics/reference/ethernet.kicad_sch`, and `teensy-expansion`/
`electronics/reference/teensy-expansion.kicad_sch`. All references resolve
against authored design and P1 aggregate records.

Symbol-library keys `library_id`, `kind`, `path`, `symbol_ids`, `evidence_ids`,
`status`, `notes`; kind is `local` or `kicad-standard`, with a `.kicad_sym` path
only for local. Net-binding keys `net_id`, `sheet_id`, `kind`, `signal_id`,
`power_rail_id`, `endpoint_refs`, `safe_default`, `test_point_ids`,
`evidence_ids`, `status`, `notes`; kind is `signal`, `power`, `ground` or
`shield`, exactly one signal/rail reference is non-null, endpoints have at least
two entries, and safe default is null or `processed-path`, `disabled`,
`asserted`, `deasserted`, `high-impedance`, `not-applicable`. Every authored
signal/rail is bound and Teensy signals agree with the pin map.

Test-point keys `test_point_id`, `sheet_id`, `net_id`, `purpose`, `required`,
`requirement_ids`, `evidence_ids`, `status`, `notes`; purpose is `rail`, `reset`,
`optical-data`, `path-select`, `tx-enable`, `usb`, `ethernet`, `expansion` or
`ground`. Required coverage includes every rail and reset, optical RX/TX logic,
mux state, TX enable, USB and Ethernet bring-up.

ERC-policy keys `command`, `project_file_allowed`, `waiver_count`,
`ignored_violation_count`, `accepted_error_count`, `accepted_warning_count`,
`status`, `notes`; authored constants are command
`scripts/kicad.sh sch erc electronics/reference/aius-reference.kicad_sch`, false,
four zero counts and pass. Validation rejects `.kicad_pro`, unlisted schematics
and missing listed files.

#### One-way document lifecycle

`scripts/fixtures/p1d-schema/reference-package/base-v1.json` contains all four
exact placeholders. Builder operations are
`bootstrap --write|--check` and
`validate --stage placeholders|design-records|schematic`. Bootstrap creates or
byte-checks placeholders and refuses to overwrite authored state. Placeholder
stage requires four exact placeholders; design-records requires electrical,
pin and placement authored with schematic still exact placeholder; schematic
requires all authored. Renderers reject placeholders. No authored-to-placeholder
transition exists; tests cover every legal transition and downgrade refusal.

```yaml
phase: P1D3a1
unit: schema-test-surface
seams: [electrical rows, pin rows, placement geometry, schematic acceptance]
contract-artifact: spec-only
fan-out:
  - name: electrical-schema
    owned-paths:
      - evidence/design/electrical-reference-schema.json
      - evidence/design/electrical-reference.json
      - scripts/fixtures/p1d-schema/electrical-reference
  - name: pin-resource-schema
    owned-paths:
      - evidence/design/pin-resource-map-schema.json
      - evidence/design/pin-resource-map.json
      - scripts/fixtures/p1d-schema/pin-resource-map
  - name: placement-schema
    owned-paths:
      - evidence/design/two-layer-placement-schema.json
      - evidence/design/two-layer-placement.json
      - scripts/fixtures/p1d-schema/two-layer-placement
  - name: schematic-contract-schema
    owned-paths:
      - evidence/design/reference-schematic-contract-schema.json
      - electronics/reference/reference-schematic-contract.json
      - scripts/fixtures/p1d-schema/reference-schematic-contract
  - name: p1d3-schema-test-coordinator
    owned-paths:
      - scripts/test_p1d_schemas.py
      - scripts/fixtures/p1d-schema/reference-package
coordination: four disjoint schema tasks run with maximum three concurrent; test coordinator runs last
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_p1d_schemas.py
    - node_modules/.bin/prettier --check evidence/design/electrical-reference-schema.json evidence/design/electrical-reference.json evidence/design/pin-resource-map-schema.json evidence/design/pin-resource-map.json evidence/design/two-layer-placement-schema.json evidence/design/two-layer-placement.json evidence/design/reference-schematic-contract-schema.json electronics/reference/reference-schematic-contract.json
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
  - name: p1d3-contract-builder
    owned-paths:
      - evidence/design/contracts/p1d-reference-package.json
      - scripts/build_p1d_reference_contract.py
      - scripts/test_build_p1d_reference_contract.py
  - name: p1d3-review-renderers
    owned-paths:
      - scripts/render_p1.py
      - scripts/test_render_p1.py
      - scripts/fixtures/render-p1/p1d-reference
coordination: two disjoint agents consume the committed schema stubs; neither edits canonical design JSON
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_build_p1d_reference_contract.py
    - python3 scripts/test_render_p1.py
    - python3 scripts/build_p1d_reference_contract.py bootstrap --check
    - python3 scripts/build_p1d_reference_contract.py validate --stage placeholders
  buildable: [fixture-proven closed design contracts and deterministic renderers]
  parent-commit: "feat(aius): define electrical reference contracts"
```

### P1D3b — Evidence-derived design records

Three disjoint agents replace one placeholder canonical JSON document with an
authored document against the committed schemas. The parent runs the committed
renderers after all three return, so agents share neither code nor generated
review documents.

```yaml
phase: P1D3b
unit: single-design-record
seams: [supplemental evidence, closed design schemas, deterministic renderers]
contract-artifact: stubs-committed
fan-out:
  - name: electrical-reference
    owned-paths: [evidence/design/electrical-reference.json]
  - name: pin-resource-map
    owned-paths: [evidence/design/pin-resource-map.json]
  - name: two-layer-placement
    owned-paths: [evidence/design/two-layer-placement.json]
coordination: parent validates design-records stage, writes four generated review files, then checks parity
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/build_p1d_reference_contract.py validate --stage design-records
    - python3 scripts/render_p1.py electrical-reference --check
    - python3 scripts/render_p1.py pin-resource-map --check
    - python3 scripts/render_p1.py two-layer-placement --check
  buildable: [evidence-closed electrical, pin and placement records]
  parent-commit: "docs(aius): establish electrical design records"
```

### P1D3c1 — ERC-clean schematic shell

This phase starts after P1D3b. The root shell predeclares all hierarchical pins
and every local symbol needed by later sheets; sheet agents create interface-only
stubs and never edit the shared symbol library. No `.kicad_pro` is created.

```yaml
phase: P1D3c1
unit: single-schematic-shell-file
seams: [hierarchical pins, predeclared symbols, four sheet interfaces]
contract-artifact: spec-only
fan-out:
  - name: schematic-root-shell
    owned-paths:
      - electronics/reference/aius-reference.kicad_sch
      - electronics/reference/aius-reference.kicad_sym
      - electronics/reference/README.md
  - name: power-usb-sheet-stub
    owned-paths: [electronics/reference/power-usb.kicad_sch]
  - name: optical-spdif-sheet-stub
    owned-paths: [electronics/reference/optical-spdif.kicad_sch]
  - name: ethernet-sheet-stub
    owned-paths: [electronics/reference/ethernet.kicad_sch]
  - name: teensy-expansion-sheet-stub
    owned-paths: [electronics/reference/teensy-expansion.kicad_sch]
coordination: maximum three concurrent; root shell contract is fixed in the plan and sheet agents do not edit root or library
review-intensity: active
commit-checkpoint:
  green:
    - scripts/kicad.sh sch erc electronics/reference/aius-reference.kicad_sch
    - python3 scripts/build_p1d_reference_contract.py validate --stage design-records
  buildable: [interface-complete zero-warning ERC-clean schematic shell]
  parent-commit: "design(aius): establish reference schematic shell"
```

### P1D3c2 — Hierarchical sheet implementation

Four agents replace one committed sheet stub each. They use only predeclared
symbols and preserve every hierarchical pin, so no shared root/library edit is
needed.

```yaml
phase: P1D3c2
unit: single-hierarchical-sheet
seams: [committed sheet interface, authored design records, predeclared symbol library]
contract-artifact: stubs-committed
fan-out:
  - name: power-usb-sheet
    owned-paths: [electronics/reference/power-usb.kicad_sch]
  - name: optical-spdif-sheet
    owned-paths: [electronics/reference/optical-spdif.kicad_sch]
  - name: ethernet-sheet
    owned-paths: [electronics/reference/ethernet.kicad_sch]
  - name: teensy-expansion-sheet
    owned-paths: [electronics/reference/teensy-expansion.kicad_sch]
coordination: maximum three concurrent; no agent edits the root schematic, symbol library, README, or contract JSON
review-intensity: active
commit-checkpoint:
  green:
    - scripts/kicad.sh sch erc electronics/reference/aius-reference.kicad_sch
    - python3 scripts/build_p1d_reference_contract.py validate --stage design-records
  buildable: [complete zero-warning ERC-clean hierarchical sheets]
  parent-commit: "design(aius): implement reference schematic sheets"
```

### P1D3c3 — Schematic contract closure

One agent replaces the exact schematic-contract placeholder after inspecting the
finished KiCad files. It does not edit KiCad sources.

```yaml
phase: P1D3c3
unit: single-contract-document
seams: [finished schematic files, electrical/pin/placement references, ERC policy]
contract-artifact: stubs-committed
fan-out:
  - name: schematic-contract-closure
    owned-paths: [electronics/reference/reference-schematic-contract.json]
review-intensity: active
commit-checkpoint:
  green:
    - scripts/kicad.sh sch erc electronics/reference/aius-reference.kicad_sch
    - python3 scripts/build_p1d_reference_contract.py validate --stage schematic
  buildable: [annotated zero-warning ERC-clean non-manufacturing reference schematic]
  parent-commit: "design(aius): close reference schematic contract"
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
contract-artifact: spec-only
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
    - python3 scripts/build_p1d_reference_contract.py validate --stage schematic
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
