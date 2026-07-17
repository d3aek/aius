---
title: Aius P1D supplemental evidence and electrical reference
type: plans
scope: aius
status: in-progress
tags: [aius, evidence, electrical, kicad, hardware]
updated: 2026-07-17
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

Clean-room Gate 12 at commit `a4b724316` returned `PASS` with no findings. It
confirmed the amended plans, literal realization oracle, current code, phase
sizing and deterministic checkpoints, and authorized P1C5a0 implementation.

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

Every manufacturer realization also contains `clone_boundary` with exact keys
`current`, then `unavailable`; a UK realization contains null. Each nested row
uses the existing order `source_id`, `classification`, `reused_material`,
`derived_claims`, `prohibited_use`, `notes`. The current row is literal
`official-reuse` with reused material `Retained primary manufacturer evidence.`,
derived claim `Aius requirements are bounded by the retained primary
manufacturer evidence.`, prohibited use `Do not treat the manufacturer material
as a complete Aius design.`, and empty notes. The unavailable row is literal
`unresolved`, has no reused material, derived claim `The manufacturer claim
remains unresolved.`, prohibited use `Do not select or release the component
until current manufacturer evidence replaces the unavailable source.`, and note
`The fixed manufacturer source is unavailable.` Both rows use the realization's
exact `source_id`; the assembler selects and copies one row without synthesizing
prose.

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

Identity, distributor and URL fields are non-null strings. The shared profile
uses the literal pattern/enum descriptors committed in the oracle; per-row
validation separately requires MPN equal request `selected_mpn`, distributor
equal realization `authority`, product initial URL equal realization
`observation_url`, and authorization authority/method/initial URL equal the
realization tuple. Only currency `GBP` and VAT flag `false` are profile-level
constants. Supplier SKU and final URLs use non-empty/HTTPS patterns. Stock state
uses the four-value enum. Stock quantity is a nullable integer with
minimum zero; price quantity, MOQ and order multiple are integers with minimum
one; unit price is a number with minimum zero. Cross-field validation enforces
the state/quantity matrix: `in-stock` requires a non-null integer greater than
or equal to one; `out-of-stock` requires literal zero; `backorder` and `unknown`
require null. It also enforces `price_quantity >= minimum_order_quantity` and
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

The power migration values are literal. `teensy-3v3.consumers` becomes, in
order, `optical-rx`, `raw-bypass-mux`, `tx-gate`, `optical-tx`, `ethernet`,
`expansion-power-protection`. `expansion-3v3.source` becomes
`expansion-power-protection`; its sole consumer remains `expansion`.
`protected-5v` remains unchanged by P1C5. The functional `expansion-power` row
retains its source/sink and changes only payload from `teensy-3v3` to
`expansion-3v3`.

The P1C0a base fixtures remain hash-locked historical inputs. Corrections are
new explicit migrations in the existing builder and are proved by exact builder
tests; no task rewrites an immutable base.

### P1C5a0 — Component-family power-reference validation

The handoff validator must accept the one selected component family that is a
topology power-path endpoint before generated topology begins using it. The
reference universe adds only `expansion-power-protection`. It is legal only as
the final ordered consumer of `teensy-3v3` and as the source of
`expansion-3v3`; it remains illegal as any other rail source/consumer and no
other component-family ID becomes a power reference. Unknown families still
emit the existing sorted `REFERENCE` diagnostic. A known family in a forbidden
rail position emits `CONTRACT`.

```yaml
phase: P1C5a0
unit: test-surface
seams: [power-reference universe, selected component-family endpoint, handoff diagnostics]
contract-artifact: spec-only
fan-out:
  - name: p1c5-power-reference-validator
    owned-paths:
      - scripts/validate_p1.py
      - scripts/test_validate_p1.py
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_validate_p1.py
    - python3 scripts/validate_p1.py topology --stage handoff
  buildable: [current topology remains valid and focused future component-family references are fixture-proven]
  parent-commit: "feat(aius): validate component power endpoints"
```

Focused tests construct the future exact source/consumer pair and require a
clean reference pass, then independently prove rejection of an unknown family,
the known family on an unrelated power domain, a missing Teensy consumer, and a
mismatched expansion source. P1C5a0 changes no generated topology bytes.

The executable oracle is a new public CLI surface:
`python3 scripts/validate_p1.py power-references --interfaces
<path>`. `--interfaces` is required, is resolved against the Aius root when
relative, and may be absolute only when it remains beneath that root. This
surface parses only the supplied interfaces document and calls the same public
`check_power_references` helper used by `topology`; it deliberately skips schema,
normative-contract and rendered-byte parity so the future pair can be proved
before P1C5a materializes it. Parse/missing failures retain the existing
diagnostics and exit 1; argument errors exit 2. Success prints exactly
`OK power-references <Aius-relative-path>` and exits 0.

The helper applies this closed decision table. Pair completeness is evaluated
only when at least one of the two allowed endpoints is present, so the current
absent/absent topology remains valid.

| Input condition | Exact result |
|---|---|
| family absent from both allowed endpoints and every other power position | pass |
| family is final `teensy-3v3.consumers` member and exact `expansion-3v3.source` | pass |
| exact source present but final consumer absent | `CONTRACT`; detail `power-domain endpoint pair is incomplete: teensy-3v3.consumers must end with 'expansion-power-protection'` |
| exact final consumer present but source differs | `CONTRACT`; detail `power-domain endpoint pair is incomplete: expansion-3v3.source must be 'expansion-power-protection'` |
| `expansion-power-protection` occurs in any additional source or consumer position | one `CONTRACT` per occurrence; detail `<field> forbids component family 'expansion-power-protection'` |
| an ID that occurs only in the validator's exact closed `FAMILIES` tuple occurs in a power source or consumer | one `CONTRACT` per occurrence; detail `<field> forbids component family '<id>'` |
| a token outside blocks, connectors, expansion interfaces, power domains, `carrier`, `enclosure` and `FAMILIES` occurs | the existing `REFERENCE`; detail `<field> does not resolve: '<token>'` |

Here `<field>` is exactly `power_domains[<document-index>].source` or
`power_domains[<document-index>].consumers`. Diagnostics are sorted by the
existing `Diagnostic` ordering. Tests cover absent/absent, the exact pair, both
partial pairs, each allowed-endpoint value in a forbidden domain, an exact pair
plus an extra occurrence, one different known family as source and consumer,
and one unknown source and consumer. The topology handoff test proves the
public stage and normal handoff call the same helper.

Topology-target identity takes precedence when an ID occurs in both `FAMILIES`
and the current blocks, connectors, expansion interfaces or power domains. The
family-only forbidden set is therefore `FAMILIES - topology targets`; for
example, current connector or expansion-interface references remain valid even
when their IDs also classify a component family.

P1C5a0 completed on 2026-07-16. The public oracle, shared helper, closed
decision table and occurrence-preserving diagnostics are implemented. The
clean-room delivery re-audit returned `PASS` with 87 full-suite tests, 16
focused tests and the handoff command green.

### P1C5a — Topology schema and migration correction

```yaml
phase: P1C5a
unit: refactor-target
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
      - scripts/test_validate_p1.py
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
    - python3 scripts/test_validate_p1.py
    - python3 scripts/build_p1c_architecture_contracts.py --check
    - python3 scripts/render_p1.py functional-topology --check
    - python3 scripts/render_p1.py interfaces-and-states --check
    - python3 scripts/validate_p1.py topology --stage handoff
  buildable: [schema-valid internally consistent power topology]
  parent-commit: "docs(aius): reconcile expansion power topology"
```

P1C5a deepens the builder output map so one `--write` covers both contract JSON
files, both valid fixtures and both canonical review JSON mirrors; each
contract/mirror pair is byte-identical. Replacement remains atomically safe per
file, matching the existing builder: it writes and `fsync`s one same-directory
temporary, calls `os.replace`, then advances in output-map order. There is no
cross-file transaction. A failed replacement emits the existing sorted
`AIUS-P1C-CONTRACT-WRITE` diagnostic, returns 1, leaves already replaced files
at their new bytes and later files untouched, and removes its temporary. A
rerun converges safely; `--check` reports every missing or stale member of a
partial six-output set. `--check` verifies all six.
The parent-generated exact paths are
`evidence/design/contracts/functional-topology.json`,
`evidence/design/contracts/interfaces-and-states.json`,
`scripts/fixtures/p1c-schema/functional-topology/valid.json`,
`scripts/fixtures/p1c-schema/interfaces-and-states/valid.json`,
`evidence/design/functional-topology.json`,
`evidence/design/interfaces-and-states.json`,
`docs/reference/functional-topology.md`, and
`docs/reference/interfaces-and-states.md`. The parent alone runs the committed
builder and renderers; no manual copy step exists.

The P1C5a delivery audit found that materializing the previously future endpoint
pair invalidates P1C5a0 test constructors that assumed the live fixture omitted
it. P1C5a therefore owns the compatibility-only fixture-constructor update in
`test_validate_p1.py`; production P1C5a0 behavior remains unchanged, and the
full prior validator suite is an explicit checkpoint.

P1C5a completed on 2026-07-16. The clean-room delivery re-audit returned
`PASS`; builder, schema and validator suites passed 23, 18, 18 and 87 tests,
respectively, and all six JSON outputs, both rendered reviews, byte checks and
the topology handoff were green.

### P1C5b — USB-C request-kind correction

```yaml
phase: P1C5b
unit: single-file-implementation
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

P1C5b completed on 2026-07-16. The GCT request and regenerated aggregate retain
`product-page` after `datasheet` and `drawing`; the aggregate SHA-256 is
`b675f0c6652b862cbe343534c04e16a1ad56ebae2e3395dc32a8696252c43f6a`.
The clean-room delivery audit returned `PASS`; 24 renderer tests, aggregate
parity, topology handoff and exact-file formatting were green.

The one-CS and layer-policy text corrections are committed in the plan amendment
before P1C5a dispatch. The parent records checkpoint completion in both normative
plans at each commit.

## P1D0 — Supplemental evidence contract and tooling

### P1D0a — Realization schema around the precommitted oracle

`supplemental-realization.json` has `schema_version`, `owned_path_root`,
`observation_profiles`, then `realizations`. `owned_path_root` is the literal
`src/hardware/aius`. It contains exactly the 46 non-satisfied requests, sorted by
`request_id`. Every row binds `request_id`, literal `task_name`, exact
`owned_paths`, `source_id`, `authority`, `revision_strategy`, `revision_value`,
`observation_url`, `authorization`, `clone_boundary`, `artifacts`, `observation_profile`,
`failure_policy`, `retention`, and `notes`.
The schema rejects templates, substitutions and a path that does not contain the
row's exact source ID. Dispatch ownership is the confinement-checked join of the
literal root and each literal row path; no other prefix or derivation is allowed.

A manufacturer row has `sha256` revision strategy, null revision value, null
observation fields and one or more artifacts. Each artifact binds
`artifact_kind`, safe basename, initial URL, expected final URL,
project-relative path, `lfs` or `git` storage, and media type.
At least one initial URL equals the request authority URL or the row retains that
authority page as `product-page`. It has both literal clone-boundary outcomes
defined above. A UK row has `observation-date` revision
strategy, null revision value, no artifacts, the exact request commerce URL,
the separate authorization tuple, null clone boundary, and the locked profile.

The oracle also defines the source constructor. ID is request `source_id`;
authority is realization `authority`; requirements become `claim_scope`;
landing URL is request `authority_url`; supersedes is null. For a current
manufacturer source, title is `<manufacturer> <MPN> primary manufacturer
evidence`, purpose is `Satisfy <request-id> for <family>.`, class is `golden`,
status is `current`, and revision is `sha256:<digest>`. With one retained
artifact, `<digest>` is that artifact's lowercase SHA-256. With multiple
artifacts, it is SHA-256 over the ASCII concatenation of their lowercase
SHA-256 values in realization order. The redistribution note is `Retained for
personal noncommercial research; no republication permission is inferred.` It
contains one embedded MPN observation with key
`manufacturer_part_number`, request MPN value, null unit, basis
`Selected MPN identified by the retained primary manufacturer evidence for
<request-id>.`, acquisition timestamp, and request authority URL.

For a current UK source, title is `normalized UK distributor price and stock
observation`, purpose is `Record a UK authorized-distributor price and stock
observation for <manufacturer> <MPN>.`, class is `dynamic`, status is `current`,
revision is `observed:<YYYY-MM-DD>`, and the redistribution note is the locked
normalized observation note above.

Both current constructors emit the 14 source keys in the exact source-schema
order listed below; outcome selects status, revision, artifacts, observations
and redistribution note before validation, so no unconditional current value is
applied to an unavailable record.

For either class, `supplemental unavailable` emits one literal source object in
existing source-schema key order: `id`, `class`, `title`, `authority`,
`revision`, `purpose`, `claim_scope`, `landing_url`, `retrieved_at`, `status`,
`supersedes`, `redistribution_note`, `artifacts`, `observations`. It preserves
the class-specific title and purpose above; sets class to `golden` for a
manufacturer or `dynamic` for UK; sets revision to
`unavailable:<retrieved-at UTC date>`; uses the command's exact UTC timestamp;
sets status `unavailable`, supersedes null, redistribution note `No local copy
is retained; no republication permission is inferred.`, and observations to an
empty array. Its sole artifact has exact key order `name`, `final_url`,
`storage`, `path`, `sha256`, `bytes`, `media_type`, `retrieval_status`, `note`
and literal values `unavailable`, the request `authority_url`,
`external-reference`, null, null, null, null, `unavailable`, and the non-empty
CLI note. Manufacturer and UK unavailable records differ only in their locked
class, title and purpose. No current-only field or observation survives this
outcome.

The literal 46-row oracle is committed with this plan at
`evidence/manifest/supplemental-realization.json`, SHA-256
`ffbb364de79d734c4ba50885095f58319a56687b0bda93669e4753ba689d493d`.
Its 35 retained artifacts and all 24 authorization tuples were selected by
the contract review, not by the schema implementer. P1C5b changes the one known
predecessor mismatch, the GCT request's missing `product-page` kind, before this
phase dispatches.

```yaml
phase: P1D0a
unit: test-surface
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

P1D0a completed on 2026-07-16. The immutable 46-row oracle retains SHA-256
`ffbb364de79d734c4ba50885095f58319a56687b0bda93669e4753ba689d493d`.
The final clean-room audit returned `PASS` after closing synchronized artifact
source-directory and template/substitution bypasses; 33 realization tests, 15
evidence-policy compatibility tests, exact formatting and topology handoff were
green. The unchanged `test_source_schema.py` empty-source expectation remains a
known baseline failure outside this phase; its test, schema and source manifest
have no P1D0a diff.

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

The constructor mapping has the 14 keys in source-schema order before
validation. Publication then passes that mapping through the unchanged shared
renderer, whose compact input and formatted output sort every object key
alphabetically. Persisted fragment bytes therefore use recursively alphabetic
key order; constructor order and published byte order are separate locked
contracts. Supplemental code must not add a second renderer or weaken the seed
renderer parity checks.

No command bypasses an access control or substitutes browser-captured bytes.
The pre-recorded GCT and TE access hazards therefore use the ordinary
golden-block unavailable path if exact retrieval fails. A UK row whose
authorization status is unresolved rejects `record` and accepts only
`unavailable`. Exit 0 means a successful current publication, UK-unavailable
publication, or check. Exit 1 means acquisition/record failed without publishing
new output and permits the caller's explicit unavailable fallback. Exit 2 means
CLI usage. Exit 3 means a durably published manufacturer-unavailable design
blocker. Publication I/O, invariant or rollback failure is exit 1 and never
leaves partial new output.

P1D0b is three serial, independently green checkpoints because all CLI concerns
deepen the same engine. No two agents edit these shared paths concurrently.

```yaml
phase: P1D0b1
unit: component-behind-interface
seams: [existing acquisition engine, manufacturer realization, supplemental source publication]
contract-artifact: spec-only
fan-out:
  - name: supplemental-manufacturer-acquisition
    owned-paths:
      - scripts/seed_evidence.py
      - scripts/test_seed_evidence.py
      - scripts/test_supplemental_evidence.py
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_seed_evidence.py
    - python3 scripts/test_supplemental_evidence.py
    - python3 scripts/test_validate_evidence.py
  buildable: [seed-compatible fixture-proven manufacturer acquisition and source publication]
  parent-commit: "feat(aius): add supplemental manufacturer acquisition"
```

P1D0b1 completed on 2026-07-16. Manufacturer acquisition reuses the seed
retrieval, validation, renderer and transaction engine for all 22 manufacturer
rows and 35 artifacts; later serial commands remain deterministic stubs. The
clean-room delivery audit returned `PASS`; 33 realization, 13 supplemental
acquisition, 32 seed compatibility and 34 evidence-validator tests were green.

```yaml
phase: P1D0b2
unit: component-behind-interface
seams: [UK realization, locked observation profile, authorization tuple]
contract-artifact: stubs-committed
fan-out:
  - name: supplemental-uk-observation
    owned-paths:
      - scripts/seed_evidence.py
      - scripts/test_seed_evidence.py
      - scripts/test_supplemental_evidence.py
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_seed_evidence.py
    - python3 scripts/test_supplemental_evidence.py
    - python3 scripts/test_validate_evidence.py
  buildable: [fixture-proven UK recording and per-row profile validation]
  parent-commit: "feat(aius): add supplemental UK observations"
```

P1D0b2 completed on 2026-07-16. `supplemental record` publishes exact normalized
dynamic sources for confirmed UK rows without network access and rejects both
unresolved authorizations. The clean-room delivery audit returned `PASS`; 33
realization, 26 supplemental, 32 seed and 34 evidence-validator tests were green.

```yaml
phase: P1D0b3
unit: component-behind-interface
seams: [literal unavailable source, optional assumption, isolated fragment check]
contract-artifact: stubs-committed
fan-out:
  - name: supplemental-unavailable-and-check
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
  buildable: [fixture-proven unavailable outcomes exit contracts and isolated checks]
  parent-commit: "feat(aius): close supplemental failure paths"
```

P1D0b3 completed on 2026-07-17. Canonical unavailable publication now closes
all 46 realization rows, emits exact UK assumptions, preserves manufacturer
golden blockers, and provides isolated current-or-unavailable fragment checks.
The clean-room delivery audit returned `PASS`; 35 supplemental, 32 seed, 34
evidence-validator, 87 P1-validator and 15 evidence-policy tests were green,
along with the repository evidence validator and scoped diff checks.

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

Lifecycle projection copies every request byte-semantically except its outcome
fields. A current source sets request status `satisfied`, evidence IDs to the
single request `source_id`, and notes to empty. An unavailable source sets status
`unavailable`, evidence IDs empty, and notes equal to its canonical unavailable
artifact note. A current/unavailable manufacturer outcome changes only the
matching component `status` to `selected`/`candidate-evidence-unavailable`;
every other component field is byte-semantically preserved. UK outcomes never
change a component field. The two already-satisfied Cliff manufacturer requests
and their components remain byte-for-byte unchanged.

For each manufacturer source, the assembler selects `clone_boundary.current` or
`clone_boundary.unavailable` according to source status and copies that literal
row. UK nulls produce no row. `supplemental-clone-boundary.json` is sorted by
`source_id`; no classification or boundary prose is synthesized.

- `manifest/sources.json` and `manifest/assumptions.json`;
- `manifest/supplemental-clone-boundary.json` for non-dynamic supplemental
  sources only;
- all 46 request fragments and the 22 affected component fragments;
- `manifest/supplemental-requests.json` and
  `design/provisional-components.csv`.

Rendering is exact by output class. Sources and assumptions retain the existing
shared `_render_json` default, including recursively alphabetical object keys.
That same shared renderer gains one opt-in order-preserving mode; its default,
strict JSON checks, pinned Prettier invocation and byte checks remain unchanged.
Request and component fragments use the order-preserving mode after copying the
decoded input mappings and replacing existing outcome fields in place, so every
object retains its input key order recursively. The supplemental clone-boundary
array also uses the order-preserving mode and contains the realization's literal
nested rows in their committed key order. This is one renderer with two explicit
ordering modes, not a second JSON renderer. The two review aggregates use the
existing pure `render_p1.render_supplemental_requests` and
`render_p1.render_components` functions, respectively.

P1D0c validation is assembler-local. Before rendering or replacing any output,
it validates every seed and supplemental source and assumption fragment with
the existing per-fragment validators; validates the combined source and
assumption arrays with their schemas and `validate_source_collection`; validates
every projected request and component with `source-request-schema.json` and
`component-schema.json`; applies the reusable `validate_clone_boundary_rows`
check; and enforces the assembler-owned exact fileset, ID join, assumption,
lifecycle and status-only mutation invariants in this section. It does not call
`validate_evidence.validate_root` or `validate_p1`; their supplemental
collection and lifecycle integration remains P1D0d and P1D0e.

All 73 output byte streams are constructed and validated before the first
replacement. The transaction orders targets lexically by Aius-relative path,
stages and fsyncs every byte stream, fsyncs each replacement directory, and on
handled failure restores every prior byte stream or prior absence and removes
every temporary. `--check` renders and compares the same 73 byte streams but
never calls the replacement seam and performs no write.

```yaml
phase: P1D0c
unit: component-behind-interface
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

The P1D0c regression matrix is explicit. Parameterized tests cover each missing,
extra, unsafe and colliding seed, supplemental and assumption fragment boundary;
missing, extra and duplicate record IDs; missing or extra unavailable
assumptions and assumptions beside current sources; all current and unavailable
manufacturer and UK lifecycle branches across 48 requests; exact status-only
projection across all 22 components; byte-identical preservation of both
already-satisfied Cliff manufacturer requests and components; UK component
non-mutation; and literal clone-row selection and ordering. Tests inject a
failure before publication and at each of the 73 sorted replacement indices,
with mixed prior-present and prior-absent targets, and require full restoration,
directory fsync coverage and no temporary residue. A write spy proves every
successful and failing `--check` path performs no replacement. Renderer tests
lock every output class and both ordering modes. Existing seed and P1D0b suites
remain unchanged and green.

P1D0c completed on 2026-07-17. The assembler closes the exact 48-request and
22-component lifecycle, selects literal manufacturer clone rows, and constructs
the 73 sorted outputs behind the existing aggregate transaction. The final
clean-room delivery audit returned `PASS` after adding all-current-capable
lifecycle coverage and production-format fidelity for every persistent fixture
input. Forty-five supplemental, 32 seed, 34 evidence-validator, 87 P1-validator,
33 realization and 15 evidence-policy tests were green, together with the
standalone production fixture check and scoped diff checks.

### P1D0d — Evidence collection validation

The evidence validator learns the separate supplemental directories, exact
source/assumption set rules, realization-to-source artifact-position binding,
and supplemental clone-boundary membership. It accepts exactly two collection
states.

In **seed-only** state, `fragments/` contains exactly the 22 seed source IDs and
`assumption-fragments/` contains exactly the assumptions derived from those
validated seed source statuses and failure policies. Both supplemental fragment
directories are absent. `supplemental-clone-boundary.json` is absent or the
literal empty array. `sources.json` and `assumptions.json` deep-equal their
respective fragment records sorted by record ID.

In **complete** state, the seed directories retain those exact sets,
`supplemental-fragments/` contains exactly all 46 realization source IDs, and
`supplemental-assumption-fragments/` contains exactly the status-derived UK
assumption set. The latter directory may be absent only when that expected set
is empty. Complete mode activates when either supplemental fragment directory
exists, the supplemental boundary contains a row, or either aggregate contains
a realization source or assumption ID. Any activation signal requires every
complete-mode member. `sources.json` and `assumptions.json` deep-equal the
record-ID-sorted concatenation of the seed and supplemental fragments.

Every present fragment directory is a regular non-symlink directory containing
only safe canonical JSON names. A source fragment filename is its source-record
`id`. An assumption fragment filename is its exact single cited `source_ids[0]`;
the assumption record `id` remains its canonical `ASM-...` value. Seed and
supplemental source IDs, assumption IDs and owned filenames are pairwise
collision-free. Missing, extra, unsafe, non-object, filename-identity and
aggregate-parity failures are validation exit 1. Unreadable fragment bytes,
non-UTF-8 or malformed JSON remain control-file exit 2 under the existing
global rule.

For each current manufacturer source, artifact count and every position's
`name`, `final_url`, `path`, `storage` and `media_type` exactly equal the
realization's `name`, `expected_final_url`, `path`, `storage` and `media_type`.
The ordered unique realization `artifact_kind` values equal the request's
`artifact_kinds`. Recorded byte count and SHA-256 remain bound to the payload by
the existing artifact diagnostics. A current UK source has no artifacts. An
unavailable source instead has the exact canonical single metadata-only
external-reference artifact and is exempt from current positional equality.

The supplemental boundary has exactly one row for each of the 22 manufacturer
sources and none for UK. Rows sort by `source_id` and each deep-equals the
realization's literal `clone_boundary.current` or `.unavailable` row selected by
the validated source status. No boundary field or prose is synthesized.

New syntactically valid failures have these exact diagnostics:

| Predicate | Code and subject | Canonical message |
| --- | --- | --- |
| required directory absent | `FRAGMENT_COLLECTION`, directory path | `required fragment directory is missing` |
| present directory symlink or non-directory | `FRAGMENT_COLLECTION`, directory path | `fragment path is not a regular non-symlink directory` |
| unsafe or non-JSON entry | `FRAGMENT_COLLECTION`, entry path | `unexpected or unsafe fragment entry` |
| exact ID set differs | `FRAGMENT_COLLECTION`, directory path | `fragment IDs differ; missing=<sorted repr>; extra=<sorted repr>` |
| source filename and record differ | `FRAGMENT_COLLECTION`, fragment path | `filename source ID <repr> does not match record ID <repr>` |
| assumption filename and citation differ | `FRAGMENT_COLLECTION`, fragment path | `filename source ID <repr> does not match canonical assumption source <repr>` |
| duplicate fragment record ID | `FRAGMENT_COLLECTION`, duplicate record ID | `record ID appears in more than one fragment` |
| seed/supplemental ID collision | `FRAGMENT_COLLECTION`, colliding ID | `ID appears in both seed and supplemental fragments` |
| aggregate differs from fragments | `FRAGMENT_COLLECTION`, aggregate path | `aggregate differs from sorted fragment collection` |
| current artifact count differs | `REALIZATION_BINDING`, source ID | `current artifact count differs from realization` |
| current artifact field differs | `REALIZATION_BINDING`, source ID | `artifact[<index>] field <field repr> differs from realization` |
| ordered artifact kinds differ | `REALIZATION_BINDING`, source ID | `ordered artifact kinds differ from request` |
| current UK source has artifacts | `REALIZATION_BINDING`, source ID | `current UK source must not contain artifacts` |
| unavailable artifact is noncanonical | `REALIZATION_BINDING`, source ID | `unavailable source artifact differs from canonical metadata` |
| boundary ID set/order differs | `CLONE_BOUNDARY`, supplemental boundary path | `supplemental boundary source IDs differ; missing=<sorted repr>; extra=<sorted repr>` or `supplemental boundary source IDs are not sorted` |
| boundary row differs from selected realization outcome | `CLONE_BOUNDARY`, source ID | `supplemental boundary row differs from realization outcome <status repr>` |
| UK row appears in boundary | `CLONE_BOUNDARY`, source ID | `UK source must not appear in supplemental clone boundary` |

The existing schema, source-collection, assumption, payload, Git/LFS and
baseline diagnostics remain unchanged. Each distinct failed predicate emits one
deduplicated sorted diagnostic.

```yaml
phase: P1D0d
unit: test-surface
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
    - python3 scripts/test_supplemental_evidence.py
    - python3 scripts/seed_evidence.py --root scripts/fixtures/supplemental-assembly/valid supplemental assemble --check
    - python3 scripts/validate_evidence.py --root scripts/fixtures/supplemental-assembly/valid --baseline evidence/manifest/sources.json
  buildable: [seed-compatible supplemental evidence validator]
  parent-commit: "feat(aius): validate supplemental evidence collections"
```

Parameterized tests lock seed-only and complete acceptance; every activation
signal; every missing, extra, unsafe, colliding, malformed and identity boundary
across all four directories, including duplicate record IDs within either
assumption directory; both aggregate parity failures; every current and
unavailable realization artifact binding field and position; unavailable
exemption; payload hash/size binding; exact boundary membership, ordering,
literal outcome equality and UK exclusion; and exact code, subject, message and
exit class. The complete fixture adds the immutable seed
`clone-boundary.json`, passes the direct validator command above and remains
green through the P1D0c assembler check. Its generator copies that seed boundary
byte-for-byte and sets only the fixture copy of the 11 required golden seed
controls to `required: false`, because the synthetic complete fixture records
all 22 seed outcomes as unavailable. Regression tests prove regeneration retains
both adaptations and that the production controls remain 11 required rows.
Existing seed validation and P1D0c assembly behavior remain unchanged.

P1D0d completed on 2026-07-17. The validator now accepts only the exact
seed-only or complete collection state, binds current and unavailable
supplemental sources to their realization and immutable request authority,
checks literal supplemental clone-boundary rows, and rejects duplicate record
identities across fragment files. The persistent complete fixture is fully
regenerable while keeping the production seed boundary byte-identical and the
11 required-seed relaxations confined to the synthetic fixture. The final
clean-room delivery audit returned `PASS` after adding correlated unavailable
URL tamper coverage and a positive current-UK/status-derived-assumption branch.
Forty-seven validator, 32 seed and 45 supplemental tests were green, together
with the production validator, generator, persistent assembler check and direct
complete-fixture validator.

### P1D0e — P1 lifecycle validation

The P1 runner is cumulative only through `selection`. Its explicit dependency
graph is:

```text
contract -> architecture-base -> architecture -> selection-fragments -> selection
                                                                        |-> handoff
                                                                        `-> supplemental-lifecycle
```

Both leaf stages therefore run the contract, architecture, policy, complete
selection-fragment, component-CSV and request-aggregate parity checks.
`handoff` then applies the design-eligibility gate but does not require requests
to be closed; this keeps the current 46-pending production handoff valid.
`supplemental-lifecycle` instead requires lifecycle closure but does not apply
the design-eligibility gate, so a structurally correct unavailable critical
manufacturer outcome is committable. The existing critical `unresolved` or
`candidate-evidence-unavailable` rejection moves out of shared
`selection-fragments` and runs only in `handoff`. The two leaf stages are
siblings; neither invokes the other.

`topology --stage selection-fragments --family <family>` remains the isolated
component and two-request check. It gains the satisfied-source class,
realization and MPN rules below but does not require any sibling family to be
closed. `--family` remains invalid for every other stage. Full
`topology --stage supplemental-lifecycle` accepts no family argument and emits
`OK topology supplemental-lifecycle` only after all 48 requests pass.

The validator loads the immutable 46-row `supplemental-realization.json` oracle.
A pre-index identity check derives the exact expected `(request_id, source_id)`
set from the selection-policy pairs for every UK request and every non-Cliff
manufacturer request. The loaded rows must contain each of those 46 identities
exactly once and no other identity. Duplicate identities are diagnosed before
assignment and never overwrite one another. This oracle identity check runs for
both isolated `selection-fragments` and full lifecycle validation.
A satisfied supplemental manufacturer request requires its exact current
`golden` source, artifact count and each position's `name`, `final_url`, `path`,
`storage` and `media_type` equal to the realization, ordered unique realization
`artifact_kind` values equal to the request `artifact_kinds`, every artifact be
current and retained in `git` or `lfs`, and an observation with key
`manufacturer_part_number` and value equal to `selected_mpn`. `comparative`,
`independently-derived`, `dynamic` and the schema-invalid `supplemental` class
never satisfy a manufacturer request.

The exact locked seed pairs `REQ-CLIFF-RX-MFR` / `CLIFF-ORJ5` / `ORJ5` and
`REQ-CLIFF-TX-MFR` / `CLIFF-OTJ5` / `OTJ5` are the only realization and embedded
MPN-observation exceptions. Each still requires a current `golden` source and at
least one current retained `git` or `lfs` artifact. No other source ID, request
or MPN receives this exception.

The full lifecycle stage requires exactly the 48 request IDs selected by the 24
non-generic components. It applies this matrix after the existing schema,
linkage and aggregate-parity checks:

| Request | Source outcome/class | Request outcome | Exact `evidence_ids` | Exact notes | Component outcome |
| --- | --- | --- | --- | --- | --- |
| manufacturer | `current` / `golden` | `satisfied` | `[source_id]` | empty | `selected` |
| manufacturer | `unavailable` / `golden` | `unavailable` | `[]` | sole unavailable artifact note | `candidate-evidence-unavailable` |
| UK | `current` / `dynamic` | `satisfied` | `[source_id]` | empty | unchanged |
| UK | `unavailable` / `dynamic` | `unavailable` | `[]` | sole unavailable artifact note | unchanged |

Absent and superseded sources, pending requests, any other class/status pair,
and component/manufacturer-request disagreement fail lifecycle validation. A
satisfied UK source additionally requires a matching-MPN observation. The
evidence validator remains responsible for payload bytes, unavailable artifact
shape and authorization fields; P1 consumes its validated source outcome and
rechecks the lifecycle joins above.

New failures have these exact diagnostics; existing schema, linkage, parity and
source-presence diagnostics remain unchanged and sorted:

| Predicate | Code and subject | Canonical message |
| --- | --- | --- |
| realization identity set differs | `LIFECYCLE`, `evidence/manifest/supplemental-realization.json` | `realization identities differ; missing=<sorted tuple repr>; extra=<sorted tuple repr>` |
| realization identity repeats | `LIFECYCLE`, `evidence/manifest/supplemental-realization.json` | `realization identity <tuple repr> appears more than once` |
| full request membership differs | `LIFECYCLE`, `evidence/manifest/requests` | `request IDs differ; missing=<sorted repr>; extra=<sorted repr>` |
| request is pending | `LIFECYCLE`, request fragment path | `request is not closed` |
| source is absent | `LIFECYCLE`, request fragment path | `source_id <repr> is absent from sources` |
| request/source outcome differs | `LIFECYCLE`, request fragment path | `request status <repr> does not match source status <repr>` |
| outcome evidence differs | `LIFECYCLE`, request fragment path | `evidence_ids differ from lifecycle outcome <status repr>` |
| outcome notes differ | `LIFECYCLE`, request fragment path | `notes differ from lifecycle outcome <status repr>` |
| source class differs | `LIFECYCLE`, request fragment path | `source class <actual repr> must be <expected repr>` |
| supplemental manufacturer realization absent | `LIFECYCLE`, request fragment path | `supplemental manufacturer realization is absent` |
| artifact count differs | `LIFECYCLE`, request fragment path | `artifact count differs from realization` |
| artifact position differs | `LIFECYCLE`, request fragment path | `artifact[<index>] field <field repr> differs from realization` |
| artifact is not retained current | `LIFECYCLE`, request fragment path | `artifact[<index>] is not a current retained artifact` |
| artifact-kind order differs | `LIFECYCLE`, request fragment path | `ordered artifact kinds differ from request` |
| MPN observation differs | `LIFECYCLE`, request fragment path | `source is not bound to selected_mpn` |
| component outcome differs | `LIFECYCLE`, component fragment path | `component status <repr> does not match manufacturer request status <repr>` |
| critical design blocker at handoff | `COMPONENT`, component fragment path | `critical family is blocked by status <status repr>` |

The persistent runnable fixture lives below
`scripts/fixtures/validate-p1/supplemental-lifecycle/` with `eligible/` and
`blocked/` roots plus a deterministic `generate.py --write|--check`. The
generator combines the production P1 schemas, base contracts, topology,
rendered documents and current seed fragments with the existing assembly test
engine. `eligible/` makes all 22 supplemental manufacturers current, all 22
authorization-confirmed UK rows current, and the exact unresolved
`AIUS-ETHERNET-MAGJACK-UK` and `AIUS-INTERNAL-USB-CABLE-UK` rows unavailable.
`blocked/` differs only by making the critical
`AIUS-ETHERNET-MAGJACK-MFR` outcome unavailable. Both roots pass
`supplemental-lifecycle`; only `eligible/` passes `handoff`, while `blocked/`
emits exactly the one canonical Ethernet-magjack critical diagnostic. Fixture
generation uses deterministic synthetic retained payloads and never copies
production evidence bytes.

```yaml
phase: P1D0e
unit: test-surface
seams: [request state, component state, source class, supplemental realization oracle, deterministic design gate]
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
    - python3 scripts/fixtures/validate-p1/supplemental-lifecycle/generate.py --check
    - python3 scripts/validate_p1.py --root scripts/fixtures/validate-p1/supplemental-lifecycle/eligible topology --stage supplemental-lifecycle
    - python3 scripts/validate_p1.py --root scripts/fixtures/validate-p1/supplemental-lifecycle/eligible topology --stage handoff
    - python3 scripts/validate_p1.py --root scripts/fixtures/validate-p1/supplemental-lifecycle/blocked topology --stage supplemental-lifecycle
    - python3 scripts/test_validate_p1.py PersistentLifecycleFixtureTests.test_blocked_handoff_exact_diagnostic
    - python3 scripts/test_validate_p1.py
    - python3 scripts/validate_p1.py topology --stage handoff
    - python3 scripts/test_validate_evidence.py
  buildable: [seed-compatible lifecycle and design-gate validator]
  parent-commit: "feat(aius): validate supplemental evidence lifecycle"
```

The regression matrix adds the non-linear leaf-stage dispatch, production
pending-handoff compatibility, exact eligible and blocked fixture commands, all
four matrix rows, both Cliff seed exceptions, every non-golden manufacturer
class including `comparative`, missing/superseded sources, closure membership,
evidence and notes cardinality, missing/mismatched/extra realization identities,
duplicate tuple and replacement-collision masking attempts, artifact count/
position/kind/current-retention mutations, missing/wrong MPN observations,
component outcome disagreement, diagnostic sorting, and the exact single
blocked-handoff diagnostic. The manifest README records the isolated-family,
lifecycle-closure and design-handoff commands and the required P1D2 order:
`validate_evidence`, `supplemental-lifecycle`, then `handoff`.

P1D0e completed on 2026-07-17. The P1 runner now has explicit sibling
`supplemental-lifecycle` and `handoff` leaves, with lifecycle-valid unavailable
outcomes separated from design eligibility. Exact policy-derived realization
identities, duplicate-tuple rejection, source classes, Cliff exceptions,
artifact positions, MPN observations and all four request/source/component
outcomes are enforced through the public CLI. Deterministic eligible and
blocked roots retain only synthetic fixture payloads and reproduce under the
committed generator. The final clean-room delivery audit returned `PASS` after
closing source-ID and duplicate-tuple realization masking probes and confirming
the schema-owned unavailable-evidence boundary. One hundred five P1 and 47
evidence-validator tests were green, together with every direct fixture command,
production pending handoff and scoped diff checks.

## P1D1 — Supplemental acquisition waves

Each row is one short-lived agent and one request. Oracle
`realization.owned_paths` values are relative to literal `owned_path_root` and
contain the source fragment, assumption fragment, then every `artifacts[].path`
in artifact order. Every phase `fan-out[].owned-paths` entry and spawn prompt
contains the repository-relative confinement-checked join
`<realization-path>`; the joined declarations are mechanically
checked against the oracle and are not literal copies of
`realization.owned_paths`. No task owns a directory root, unused storage root or
derived filename. UK rows therefore own exactly two files. Each wave uses one
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

The oracle is the sole ownership authority. Dispatch reads literal `task_name`,
`owned_path_root` and `owned_paths`, verifies every repository-relative declared
path against the joined oracle set, and copies those declarations verbatim into
spawn prompts.

The wave procedure is exact:

1. Manufacturer: run `supplemental acquire`. Exit 0 is current. On exit 1,
   run `supplemental unavailable` with note `Automated acquisition from the
   fixed realization URL failed during P1D1.NN.` and the wave timestamp; that
   command must exit 3. Exit 3 directly from acquire is also an already-durable
   blocker. Any other exit blocks the wave.
2. Confirmed UK: run `supplemental record` with the observed strict JSON and
   final URLs. Exit 0 is current. Inability to assemble the observation or exit
   1 invokes `supplemental unavailable` with the same literal wave note and
   timestamp; that command must exit 0. Any other exit blocks the wave.
3. Unresolved UK: skip record and run `supplemental unavailable` directly with
   the same literal wave note and timestamp; it must exit 0.
4. Run the row's `supplemental check-fragment`; only exit 0 completes the task.
   A current or lifecycle-valid unavailable outcome may commit. Access-control
   bypass, part/URL substitution and unretained browser bytes remain forbidden.

Each wave below is a separate checkpoint. There is no multi-wave uncommitted
phase and no scalar fan-out shorthand.

```yaml
phase: P1D1.01
unit: single-file-implementation
seams: [fixed realization row, bounded acquisition or normalized observation, isolated fragment check]
contract-artifact: both
fan-out:
  - name: component-evidence-req-cliff-rx-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-CLIFF-RX-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-CLIFF-RX-UK.json
  - name: component-evidence-req-cliff-tx-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-CLIFF-TX-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-CLIFF-TX-UK.json
  - name: component-evidence-req-ethernet-magjack-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-ETHERNET-MAGJACK-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-ETHERNET-MAGJACK-MFR.json
      - https://productfinder.pulseeng.com/products/datasheets/J403.pdf
coordination: maximum three concurrent; parent allocates one UTC timestamp; every worker follows the locked current/unavailable procedure; parent commits only after all listed fragment checks pass
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-CLIFF-RX-UK
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-CLIFF-TX-UK
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-ETHERNET-MAGJACK-MFR
    - python3 scripts/test_supplemental_evidence.py
  buildable: [durable independently validated source outcomes for this wave]
  parent-commit: "docs(aius): archive supplemental evidence wave 01"
```

```yaml
phase: P1D1.02
unit: single-file-implementation
seams: [fixed realization row, bounded acquisition or normalized observation, isolated fragment check]
contract-artifact: both
fan-out:
  - name: component-evidence-req-ethernet-magjack-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-ETHERNET-MAGJACK-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-ETHERNET-MAGJACK-UK.json
  - name: component-evidence-req-expansion-gpio-adc-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-EXPANSION-GPIO-ADC-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-EXPANSION-GPIO-ADC-MFR.json
      - https://www.jst-mfg.com/product/pdf/eng/eGH.pdf
  - name: component-evidence-req-expansion-gpio-adc-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-EXPANSION-GPIO-ADC-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-EXPANSION-GPIO-ADC-UK.json
coordination: maximum three concurrent; parent allocates one UTC timestamp; every worker follows the locked current/unavailable procedure; parent commits only after all listed fragment checks pass
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-ETHERNET-MAGJACK-UK
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-EXPANSION-GPIO-ADC-MFR
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-EXPANSION-GPIO-ADC-UK
    - python3 scripts/test_supplemental_evidence.py
  buildable: [durable independently validated source outcomes for this wave]
  parent-commit: "docs(aius): archive supplemental evidence wave 02"
```

```yaml
phase: P1D1.03
unit: single-file-implementation
seams: [fixed realization row, bounded acquisition or normalized observation, isolated fragment check]
contract-artifact: both
fan-out:
  - name: component-evidence-req-expansion-i2c-control-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-EXPANSION-I2C-CONTROL-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-EXPANSION-I2C-CONTROL-MFR.json
      - https://www.jst-mfg.com/product/pdf/eng/eGH.pdf
  - name: component-evidence-req-expansion-i2c-control-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-EXPANSION-I2C-CONTROL-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-EXPANSION-I2C-CONTROL-UK.json
  - name: component-evidence-req-expansion-i2c-isolator-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-EXPANSION-I2C-ISOLATOR-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-EXPANSION-I2C-ISOLATOR-MFR.json
      - https://www.ti.com/lit/ds/symlink/tca9517a.pdf
coordination: maximum three concurrent; parent allocates one UTC timestamp; every worker follows the locked current/unavailable procedure; parent commits only after all listed fragment checks pass
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-EXPANSION-I2C-CONTROL-MFR
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-EXPANSION-I2C-CONTROL-UK
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-EXPANSION-I2C-ISOLATOR-MFR
    - python3 scripts/test_supplemental_evidence.py
  buildable: [durable independently validated source outcomes for this wave]
  parent-commit: "docs(aius): archive supplemental evidence wave 03"
```

```yaml
phase: P1D1.04
unit: single-file-implementation
seams: [fixed realization row, bounded acquisition or normalized observation, isolated fragment check]
contract-artifact: both
fan-out:
  - name: component-evidence-req-expansion-i2c-isolator-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-EXPANSION-I2C-ISOLATOR-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-EXPANSION-I2C-ISOLATOR-UK.json
  - name: component-evidence-req-expansion-i2s-tdm-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-EXPANSION-I2S-TDM-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-EXPANSION-I2S-TDM-MFR.json
      - https://www.jst-mfg.com/product/pdf/eng/eGH.pdf
  - name: component-evidence-req-expansion-i2s-tdm-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-EXPANSION-I2S-TDM-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-EXPANSION-I2S-TDM-UK.json
coordination: maximum three concurrent; parent allocates one UTC timestamp; every worker follows the locked current/unavailable procedure; parent commits only after all listed fragment checks pass
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-EXPANSION-I2C-ISOLATOR-UK
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-EXPANSION-I2S-TDM-MFR
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-EXPANSION-I2S-TDM-UK
    - python3 scripts/test_supplemental_evidence.py
  buildable: [durable independently validated source outcomes for this wave]
  parent-commit: "docs(aius): archive supplemental evidence wave 04"
```

```yaml
phase: P1D1.05
unit: single-file-implementation
seams: [fixed realization row, bounded acquisition or normalized observation, isolated fragment check]
contract-artifact: both
fan-out:
  - name: component-evidence-req-expansion-power-protection-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-EXPANSION-POWER-PROTECTION-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-EXPANSION-POWER-PROTECTION-MFR.json
      - https://www.ti.com/lit/ds/symlink/tps2553.pdf
  - name: component-evidence-req-expansion-power-protection-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-EXPANSION-POWER-PROTECTION-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-EXPANSION-POWER-PROTECTION-UK.json
  - name: component-evidence-req-expansion-spi-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-EXPANSION-SPI-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-EXPANSION-SPI-MFR.json
      - https://www.jst-mfg.com/product/pdf/eng/eGH.pdf
coordination: maximum three concurrent; parent allocates one UTC timestamp; every worker follows the locked current/unavailable procedure; parent commits only after all listed fragment checks pass
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-EXPANSION-POWER-PROTECTION-MFR
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-EXPANSION-POWER-PROTECTION-UK
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-EXPANSION-SPI-MFR
    - python3 scripts/test_supplemental_evidence.py
  buildable: [durable independently validated source outcomes for this wave]
  parent-commit: "docs(aius): archive supplemental evidence wave 05"
```

```yaml
phase: P1D1.06
unit: single-file-implementation
seams: [fixed realization row, bounded acquisition or normalized observation, isolated fragment check]
contract-artifact: both
fan-out:
  - name: component-evidence-req-expansion-spi-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-EXPANSION-SPI-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-EXPANSION-SPI-UK.json
  - name: component-evidence-req-expansion-uart-debug-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-EXPANSION-UART-DEBUG-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-EXPANSION-UART-DEBUG-MFR.json
      - https://www.jst-mfg.com/product/pdf/eng/eGH.pdf
  - name: component-evidence-req-expansion-uart-debug-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-EXPANSION-UART-DEBUG-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-EXPANSION-UART-DEBUG-UK.json
coordination: maximum three concurrent; parent allocates one UTC timestamp; every worker follows the locked current/unavailable procedure; parent commits only after all listed fragment checks pass
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-EXPANSION-SPI-UK
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-EXPANSION-UART-DEBUG-MFR
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-EXPANSION-UART-DEBUG-UK
    - python3 scripts/test_supplemental_evidence.py
  buildable: [durable independently validated source outcomes for this wave]
  parent-commit: "docs(aius): archive supplemental evidence wave 06"
```

```yaml
phase: P1D1.07
unit: single-file-implementation
seams: [fixed realization row, bounded acquisition or normalized observation, isolated fragment check]
contract-artifact: both
fan-out:
  - name: component-evidence-req-expansion-usb-host-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-EXPANSION-USB-HOST-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-EXPANSION-USB-HOST-MFR.json
      - https://www.jst-mfg.com/product/pdf/eng/eGH.pdf
  - name: component-evidence-req-expansion-usb-host-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-EXPANSION-USB-HOST-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-EXPANSION-USB-HOST-UK.json
  - name: component-evidence-req-internal-usb-cable-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-INTERNAL-USB-CABLE-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-INTERNAL-USB-CABLE-MFR.json
      - https://www.startech.com/en-us/cables/uusbhaub6in
coordination: maximum three concurrent; parent allocates one UTC timestamp; every worker follows the locked current/unavailable procedure; parent commits only after all listed fragment checks pass
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-EXPANSION-USB-HOST-MFR
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-EXPANSION-USB-HOST-UK
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-INTERNAL-USB-CABLE-MFR
    - python3 scripts/test_supplemental_evidence.py
  buildable: [durable independently validated source outcomes for this wave]
  parent-commit: "docs(aius): archive supplemental evidence wave 07"
```

```yaml
phase: P1D1.08
unit: single-file-implementation
seams: [fixed realization row, bounded acquisition or normalized observation, isolated fragment check]
contract-artifact: both
fan-out:
  - name: component-evidence-req-internal-usb-cable-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-INTERNAL-USB-CABLE-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-INTERNAL-USB-CABLE-UK.json
  - name: component-evidence-req-mechanical-cable-retainer-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-MECHANICAL-CABLE-RETAINER-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-MECHANICAL-CABLE-RETAINER-MFR.json
      - https://www.hellermanntyton.com/products/cable-tie-mounts/ctm0/151-30300
      - evidence/source-snapshots/AIUS-MECHANICAL-CABLE-RETAINER-MFR/151-30300.html
  - name: component-evidence-req-mechanical-cable-retainer-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-MECHANICAL-CABLE-RETAINER-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-MECHANICAL-CABLE-RETAINER-UK.json
coordination: maximum three concurrent; parent allocates one UTC timestamp; every worker follows the locked current/unavailable procedure; parent commits only after all listed fragment checks pass
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-INTERNAL-USB-CABLE-UK
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-MECHANICAL-CABLE-RETAINER-MFR
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-MECHANICAL-CABLE-RETAINER-UK
    - python3 scripts/test_supplemental_evidence.py
  buildable: [durable independently validated source outcomes for this wave]
  parent-commit: "docs(aius): archive supplemental evidence wave 08"
```

```yaml
phase: P1D1.09
unit: single-file-implementation
seams: [fixed realization row, bounded acquisition or normalized observation, isolated fragment check]
contract-artifact: both
fan-out:
  - name: component-evidence-req-remote-program-rtc-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-REMOTE-PROGRAM-RTC-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-REMOTE-PROGRAM-RTC-MFR.json
      - https://www.jst-mfg.com/product/pdf/eng/eGH.pdf
  - name: component-evidence-req-remote-program-rtc-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-REMOTE-PROGRAM-RTC-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-REMOTE-PROGRAM-RTC-UK.json
  - name: component-evidence-req-spdif-mux-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-SPDIF-MUX-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-SPDIF-MUX-MFR.json
      - https://www.ti.com/lit/ds/symlink/sn74lvc1g3157.pdf
coordination: maximum three concurrent; parent allocates one UTC timestamp; every worker follows the locked current/unavailable procedure; parent commits only after all listed fragment checks pass
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-REMOTE-PROGRAM-RTC-MFR
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-REMOTE-PROGRAM-RTC-UK
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-SPDIF-MUX-MFR
    - python3 scripts/test_supplemental_evidence.py
  buildable: [durable independently validated source outcomes for this wave]
  parent-commit: "docs(aius): archive supplemental evidence wave 09"
```

```yaml
phase: P1D1.10
unit: single-file-implementation
seams: [fixed realization row, bounded acquisition or normalized observation, isolated fragment check]
contract-artifact: both
fan-out:
  - name: component-evidence-req-spdif-mux-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-SPDIF-MUX-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-SPDIF-MUX-UK.json
  - name: component-evidence-req-spdif-tx-gate-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-SPDIF-TX-GATE-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-SPDIF-TX-GATE-MFR.json
      - https://www.ti.com/lit/ds/symlink/sn74lvc1g125.pdf
  - name: component-evidence-req-spdif-tx-gate-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-SPDIF-TX-GATE-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-SPDIF-TX-GATE-UK.json
coordination: maximum three concurrent; parent allocates one UTC timestamp; every worker follows the locked current/unavailable procedure; parent commits only after all listed fragment checks pass
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-SPDIF-MUX-UK
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-SPDIF-TX-GATE-MFR
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-SPDIF-TX-GATE-UK
    - python3 scripts/test_supplemental_evidence.py
  buildable: [durable independently validated source outcomes for this wave]
  parent-commit: "docs(aius): archive supplemental evidence wave 10"
```

```yaml
phase: P1D1.11
unit: single-file-implementation
seams: [fixed realization row, bounded acquisition or normalized observation, isolated fragment check]
contract-artifact: both
fan-out:
  - name: component-evidence-req-teensy-ethernet-socket-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-TEENSY-ETHERNET-SOCKET-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-TEENSY-ETHERNET-SOCKET-MFR.json
      - https://www.samtec.com/products/tmm
      - https://www.samtec.com/products/mms
      - https://www.samtec.com/products/mms-103-01-l-dv
      - https://www.samtec.com/products/mms-103-01-l-dv
      - evidence/source-snapshots/AIUS-TEENSY-ETHERNET-SOCKET-MFR/mms-103-01-l-dv.html
  - name: component-evidence-req-teensy-ethernet-socket-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-TEENSY-ETHERNET-SOCKET-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-TEENSY-ETHERNET-SOCKET-UK.json
  - name: component-evidence-req-teensy-side-socket-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-TEENSY-SIDE-SOCKET-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-TEENSY-SIDE-SOCKET-MFR.json
      - https://www.samtec.com/products/tsw
      - https://www.samtec.com/products/ssw
      - https://www.samtec.com/products/ssw-124-02-g-s
      - evidence/source-snapshots/AIUS-TEENSY-SIDE-SOCKET-MFR/ssw-124-02-g-s.html
coordination: maximum three concurrent; parent allocates one UTC timestamp; every worker follows the locked current/unavailable procedure; parent commits only after all listed fragment checks pass
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-TEENSY-ETHERNET-SOCKET-MFR
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-TEENSY-ETHERNET-SOCKET-UK
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-TEENSY-SIDE-SOCKET-MFR
    - python3 scripts/test_supplemental_evidence.py
  buildable: [durable independently validated source outcomes for this wave]
  parent-commit: "docs(aius): archive supplemental evidence wave 11"
```

```yaml
phase: P1D1.12
unit: single-file-implementation
seams: [fixed realization row, bounded acquisition or normalized observation, isolated fragment check]
contract-artifact: both
fan-out:
  - name: component-evidence-req-teensy-side-socket-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-TEENSY-SIDE-SOCKET-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-TEENSY-SIDE-SOCKET-UK.json
  - name: component-evidence-req-teensy-usb-host-socket-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-TEENSY-USB-HOST-SOCKET-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-TEENSY-USB-HOST-SOCKET-MFR.json
      - https://www.samtec.com/products/tsw
      - https://www.samtec.com/products/ssw
      - https://www.samtec.com/products/ssw-105-02-g-s
      - evidence/source-snapshots/AIUS-TEENSY-USB-HOST-SOCKET-MFR/ssw-105-02-g-s.html
  - name: component-evidence-req-teensy-usb-host-socket-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-TEENSY-USB-HOST-SOCKET-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-TEENSY-USB-HOST-SOCKET-UK.json
coordination: maximum three concurrent; parent allocates one UTC timestamp; every worker follows the locked current/unavailable procedure; parent commits only after all listed fragment checks pass
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-TEENSY-SIDE-SOCKET-UK
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-TEENSY-USB-HOST-SOCKET-MFR
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-TEENSY-USB-HOST-SOCKET-UK
    - python3 scripts/test_supplemental_evidence.py
  buildable: [durable independently validated source outcomes for this wave]
  parent-commit: "docs(aius): archive supplemental evidence wave 12"
```

```yaml
phase: P1D1.13
unit: single-file-implementation
seams: [fixed realization row, bounded acquisition or normalized observation, isolated fragment check]
contract-artifact: both
fan-out:
  - name: component-evidence-req-test-fixture-header-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-TEST-FIXTURE-HEADER-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-TEST-FIXTURE-HEADER-MFR.json
      - https://www.tag-connect.com/product/tc2030-idc-6-pin-no-legs
  - name: component-evidence-req-test-fixture-header-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-TEST-FIXTURE-HEADER-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-TEST-FIXTURE-HEADER-UK.json
  - name: component-evidence-req-usb-a-receptacle-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-USB-A-RECEPTACLE-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-USB-A-RECEPTACLE-MFR.json
      - docs/reference/datasheets.mdAIUS-USB-A-RECEPTACLE-MFR/292303-1.datasheet.pdf
coordination: maximum three concurrent; parent allocates one UTC timestamp; every worker follows the locked current/unavailable procedure; parent commits only after all listed fragment checks pass
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-TEST-FIXTURE-HEADER-MFR
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-TEST-FIXTURE-HEADER-UK
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-USB-A-RECEPTACLE-MFR
    - python3 scripts/test_supplemental_evidence.py
  buildable: [durable independently validated source outcomes for this wave]
  parent-commit: "docs(aius): archive supplemental evidence wave 13"
```

```yaml
phase: P1D1.14
unit: single-file-implementation
seams: [fixed realization row, bounded acquisition or normalized observation, isolated fragment check]
contract-artifact: both
fan-out:
  - name: component-evidence-req-usb-a-receptacle-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-USB-A-RECEPTACLE-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-USB-A-RECEPTACLE-UK.json
  - name: component-evidence-req-usb-c-receptacle-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-USB-C-RECEPTACLE-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-USB-C-RECEPTACLE-MFR.json
      - docs/reference/datasheets.mdAIUS-USB-C-RECEPTACLE-MFR/usb4105-spec.pdf
      - docs/reference/datasheets.mdAIUS-USB-C-RECEPTACLE-MFR/usb4105.pdf
      - evidence/source-snapshots/AIUS-USB-C-RECEPTACLE-MFR/usb4105.html
  - name: component-evidence-req-usb-c-receptacle-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-USB-C-RECEPTACLE-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-USB-C-RECEPTACLE-UK.json
coordination: maximum three concurrent; parent allocates one UTC timestamp; every worker follows the locked current/unavailable procedure; parent commits only after all listed fragment checks pass
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-USB-A-RECEPTACLE-UK
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-USB-C-RECEPTACLE-MFR
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-USB-C-RECEPTACLE-UK
    - python3 scripts/test_supplemental_evidence.py
  buildable: [durable independently validated source outcomes for this wave]
  parent-commit: "docs(aius): archive supplemental evidence wave 14"
```

```yaml
phase: P1D1.15
unit: single-file-implementation
seams: [fixed realization row, bounded acquisition or normalized observation, isolated fragment check]
contract-artifact: both
fan-out:
  - name: component-evidence-req-usb-esd-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-USB-ESD-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-USB-ESD-MFR.json
      - https://www.ti.com/lit/ds/symlink/tpd2eusb30a.pdf
  - name: component-evidence-req-usb-esd-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-USB-ESD-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-USB-ESD-UK.json
  - name: component-evidence-req-usb-power-protection-mfr
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-USB-POWER-PROTECTION-MFR.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-USB-POWER-PROTECTION-MFR.json
      - https://www.ti.com/lit/ds/symlink/tps2553.pdf
coordination: maximum three concurrent; parent allocates one UTC timestamp; every worker follows the locked current/unavailable procedure; parent commits only after all listed fragment checks pass
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-USB-ESD-MFR
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-USB-ESD-UK
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-USB-POWER-PROTECTION-MFR
    - python3 scripts/test_supplemental_evidence.py
  buildable: [durable independently validated source outcomes for this wave]
  parent-commit: "docs(aius): archive supplemental evidence wave 15"
```

```yaml
phase: P1D1.16
unit: single-file-implementation
seams: [fixed realization row, bounded acquisition or normalized observation, isolated fragment check]
contract-artifact: both
fan-out:
  - name: component-evidence-req-usb-power-protection-uk
    owned-paths:
      - evidence/manifest/supplemental-fragments/AIUS-USB-POWER-PROTECTION-UK.json
      - evidence/manifest/supplemental-assumption-fragments/AIUS-USB-POWER-PROTECTION-UK.json
coordination: maximum three concurrent; parent allocates one UTC timestamp; every worker follows the locked current/unavailable procedure; parent commits only after all listed fragment checks pass
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py supplemental check-fragment REQ-USB-POWER-PROTECTION-UK
    - python3 scripts/test_supplemental_evidence.py
  buildable: [durable independently validated source outcomes for this wave]
  parent-commit: "docs(aius): archive supplemental evidence wave 16"
```

## P1D2 — Serial integration and lifecycle closure

P1D2 starts only after all 46 isolated checks pass. It runs the deterministic
assembler, closes every request and component lifecycle, regenerates both review
aggregates, and records any manufacturer blocker. No design task starts if a
critical manufacturer source is unavailable.

```yaml
phase: P1D2a
unit: test-surface
seams: [closed supplemental realization, selected family set, exact formatter argv]
contract-artifact: spec-only
depends-on:
  - evidence/manifest/supplemental-realization.json
  - scripts/validate_p1.py
fan-out:
  - name: p1d2-format-checker
    owned-paths:
      - scripts/check_p1d2_format.py
      - scripts/test_check_p1d2_format.py
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_check_p1d2_format.py
  buildable: [fixture-proven exact supplemental lifecycle formatting command]
  parent-commit: "test(aius): pin supplemental formatting inputs"
```

`check_p1d2_format.py` accepts no arguments, resolves the Aius and repository
roots from its own path, and invokes the repository-pinned
`node_modules/.bin/prettier --check` once. Its closed sorted argv contains
`sources.json`, `assumptions.json`, `supplemental-clone-boundary.json`, and
`supplemental-requests.json`; one request path for every unique request ID in
the 46-row realization; and one component path for every exact family in the
validator's closed `FAMILIES` tuple. It rejects duplicate realization request
IDs, a realization count other than 46, a missing expected input, an extra
request/component JSON member, or any path escape before invoking Prettier.
The test command-shims Prettier and proves exact argv, lexical ordering, CWD
independence, exit propagation and every rejection branch.

```yaml
phase: P1D2
unit: parent-only
seams: [validated supplemental fragments, immutable seed set, lifecycle projection]
contract-artifact: both
depends-on:
  - scripts/check_p1d2_format.py
fan-out:
  - name: p1d2-integration
    owned-paths:
      - evidence/manifest/sources.json
      - evidence/manifest/assumptions.json
      - evidence/manifest/supplemental-clone-boundary.json
      - evidence/manifest/requests/REQ-CLIFF-RX-UK.json
      - evidence/manifest/requests/REQ-CLIFF-TX-UK.json
      - evidence/manifest/requests/REQ-ETHERNET-MAGJACK-MFR.json
      - evidence/manifest/requests/REQ-ETHERNET-MAGJACK-UK.json
      - evidence/manifest/requests/REQ-EXPANSION-GPIO-ADC-MFR.json
      - evidence/manifest/requests/REQ-EXPANSION-GPIO-ADC-UK.json
      - evidence/manifest/requests/REQ-EXPANSION-I2C-CONTROL-MFR.json
      - evidence/manifest/requests/REQ-EXPANSION-I2C-CONTROL-UK.json
      - evidence/manifest/requests/REQ-EXPANSION-I2C-ISOLATOR-MFR.json
      - evidence/manifest/requests/REQ-EXPANSION-I2C-ISOLATOR-UK.json
      - evidence/manifest/requests/REQ-EXPANSION-I2S-TDM-MFR.json
      - evidence/manifest/requests/REQ-EXPANSION-I2S-TDM-UK.json
      - evidence/manifest/requests/REQ-EXPANSION-POWER-PROTECTION-MFR.json
      - evidence/manifest/requests/REQ-EXPANSION-POWER-PROTECTION-UK.json
      - evidence/manifest/requests/REQ-EXPANSION-SPI-MFR.json
      - evidence/manifest/requests/REQ-EXPANSION-SPI-UK.json
      - evidence/manifest/requests/REQ-EXPANSION-UART-DEBUG-MFR.json
      - evidence/manifest/requests/REQ-EXPANSION-UART-DEBUG-UK.json
      - evidence/manifest/requests/REQ-EXPANSION-USB-HOST-MFR.json
      - evidence/manifest/requests/REQ-EXPANSION-USB-HOST-UK.json
      - evidence/manifest/requests/REQ-INTERNAL-USB-CABLE-MFR.json
      - evidence/manifest/requests/REQ-INTERNAL-USB-CABLE-UK.json
      - evidence/manifest/requests/REQ-MECHANICAL-CABLE-RETAINER-MFR.json
      - evidence/manifest/requests/REQ-MECHANICAL-CABLE-RETAINER-UK.json
      - evidence/manifest/requests/REQ-REMOTE-PROGRAM-RTC-MFR.json
      - evidence/manifest/requests/REQ-REMOTE-PROGRAM-RTC-UK.json
      - evidence/manifest/requests/REQ-SPDIF-MUX-MFR.json
      - evidence/manifest/requests/REQ-SPDIF-MUX-UK.json
      - evidence/manifest/requests/REQ-SPDIF-TX-GATE-MFR.json
      - evidence/manifest/requests/REQ-SPDIF-TX-GATE-UK.json
      - evidence/manifest/requests/REQ-TEENSY-ETHERNET-SOCKET-MFR.json
      - evidence/manifest/requests/REQ-TEENSY-ETHERNET-SOCKET-UK.json
      - evidence/manifest/requests/REQ-TEENSY-SIDE-SOCKET-MFR.json
      - evidence/manifest/requests/REQ-TEENSY-SIDE-SOCKET-UK.json
      - evidence/manifest/requests/REQ-TEENSY-USB-HOST-SOCKET-MFR.json
      - evidence/manifest/requests/REQ-TEENSY-USB-HOST-SOCKET-UK.json
      - evidence/manifest/requests/REQ-TEST-FIXTURE-HEADER-MFR.json
      - evidence/manifest/requests/REQ-TEST-FIXTURE-HEADER-UK.json
      - evidence/manifest/requests/REQ-USB-A-RECEPTACLE-MFR.json
      - evidence/manifest/requests/REQ-USB-A-RECEPTACLE-UK.json
      - evidence/manifest/requests/REQ-USB-C-RECEPTACLE-MFR.json
      - evidence/manifest/requests/REQ-USB-C-RECEPTACLE-UK.json
      - evidence/manifest/requests/REQ-USB-ESD-MFR.json
      - evidence/manifest/requests/REQ-USB-ESD-UK.json
      - evidence/manifest/requests/REQ-USB-POWER-PROTECTION-MFR.json
      - evidence/manifest/requests/REQ-USB-POWER-PROTECTION-UK.json
      - evidence/design/components/ethernet-magjack.json
      - evidence/design/components/expansion-gpio-adc.json
      - evidence/design/components/expansion-i2c-control.json
      - evidence/design/components/expansion-i2c-isolator.json
      - evidence/design/components/expansion-i2s-tdm.json
      - evidence/design/components/expansion-power-protection.json
      - evidence/design/components/expansion-spi.json
      - evidence/design/components/expansion-uart-debug.json
      - evidence/design/components/expansion-usb-host.json
      - evidence/design/components/internal-usb-cable.json
      - evidence/design/components/mechanical-cable-retainer.json
      - evidence/design/components/remote-program-rtc.json
      - evidence/design/components/spdif-mux.json
      - evidence/design/components/spdif-tx-gate.json
      - evidence/design/components/teensy-ethernet-socket.json
      - evidence/design/components/teensy-side-socket.json
      - evidence/design/components/teensy-usb-host-socket.json
      - evidence/design/components/test-fixture-header.json
      - evidence/design/components/usb-a-receptacle.json
      - evidence/design/components/usb-c-receptacle.json
      - evidence/design/components/usb-esd.json
      - evidence/design/components/usb-power-protection.json
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
    - python3 scripts/check_p1d2_format.py
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

Create six closed schemas and disjoint valid/invalid fixtures before design
facts or shared generator code are authored:

- `electrical-reference-schema.json`;
- `pin-resource-map-schema.json`;
- `two-layer-placement-schema.json`;
- `reference-schematic-contract-schema.json`;
- `design-basis-schema.json`;
- `design-basis-fragment-schema.json`.

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
`^REQT-[A-Z0-9]+(?:-[A-Z0-9]+)*$`. Identifier arrays are unique and sorted
unless an explicitly listed semantic order overrides lexical order; record
arrays sort by their first ID field. Root requirement/evidence arrays are
the exact sorted nested unions and resolve against selection policy and sources.
Cross-row order, uniqueness, arithmetic and references are enforced by
`test_p1d_schemas.py` because the repository subset validator does not implement
them. Except for the pin-specific allocation enum defined below, engineering-row
status is `pass` or `fail`; every failure and every pin `conflict` has non-empty
notes. A failed support circuit, signal, power rail, board, placement, keepout,
cable, collision check or schematic row, or a conflicting pin, blocks
P1D3/P1D4. A failed route or ground corridor is permitted only when every such
failure is covered by failed electrical criteria, `mechanical_failure_ids` is
empty, and the layer decision is exactly
4/proceed/`four-layer-required`; otherwise it blocks. Any mechanical criterion
or mechanical-row failure blocks redesign.

Every key listed for a root or row is required and appears in the listed order.
Unless a field is explicitly described as nullable, it is non-null. Every
`notes` field is a string, every status is the stated enum, every ID/reference
field is a string, and every plural `*_ids`/`sinks`/`consumers`/`endpoint_refs`
field is a sorted unique string array. Every non-null internal reference must
resolve in the same package or the named P1 aggregate.

Placeholders have empty factual arrays, empty requirement/evidence arrays and
empty notes. The only nullable root objects are placement `board`, placement
`layer_decision`, schematic `root_schematic`, and schematic `erc_policy`; each
is null in its placeholder. Electrical has empty `support_circuits`, `signals`
and `power_rails`; pin has empty `pins`; placement has empty `placements`,
`keepouts`, `routes`, `ground_corridors`, `cables`, `collision_checks` and
`criteria`; schematic has empty `sheets`, `symbol_libraries`, `net_bindings` and
`test_points`. An authored document contains the complete evidence-derived
facts, makes all four nullable root objects non-null in their owning document,
and never uses placeholder values.

#### Electrical-reference contract

Root key order is `schema_version`, `document_state`, `support_circuits`,
`signals`, `power_rails`, `requirement_ids`, `evidence_ids`, `notes`.

A support-circuit row keys `circuit_id`, `kind`, `description`,
`requirement_ids`, `evidence_ids`, `status`, `notes`; kind is `power`,
`protection`, `interface`, `logic`, `isolation`, `passive-network`, `module` or
`test`. Description and evidence are non-empty; requirements may be empty.

Electrical field types and nullability are closed as follows:

| Object | Fields | Type and nullability |
|---|---|---|
| support circuit | `circuit_id`, `kind`, `description`, `status`, `notes` | non-null string |
| support circuit | `requirement_ids`, `evidence_ids` | string array; evidence non-empty |
| signal | `signal_id`, `kind`, `direction`, `voltage_domain`, `default_state`, `status`, `notes` | non-null string |
| signal | `parent_signal_id` | internal-ID string or null; null except differential legs |
| signal | `source` | non-empty string or null; null if and only if direction is bidirectional |
| signal | `sinks` | non-empty string array; at least two for bidirectional |
| signal | `source_vol_max_mv`, `source_voh_min_mv`, `sink_vil_max_mv`, `sink_vih_min_mv`, `low_margin_mv`, `high_margin_mv` | integer or null under the all-six rule below |
| signal | `max_source_current_ua`, `max_sink_current_ua`, `required_bandwidth_kbps`, `supported_bandwidth_kbps` | non-negative integer, never null |
| signal | `support_circuit_ids`, `requirement_ids`, `evidence_ids` | string array; support circuits and evidence non-empty |
| power rail | `rail_id`, `source`, `default_state`, `status`, `notes` | non-null string; source is non-empty |
| power rail | `consumers` | non-empty string array |
| power rail | `minimum_mv`, `nominal_mv`, `maximum_mv`, `continuous_load_ma`, `peak_load_ma`, `source_limit_ma`, `reverse_leakage_ua` | non-negative integer, never null |
| power rail | `support_circuit_ids`, `requirement_ids`, `evidence_ids` | string array; support circuits and evidence non-empty |

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

Pin field types are: `signal_id`, `physical_pin`, `direction`,
`voltage_domain`, `status`, and `notes` are non-null strings; `pad_mux`,
`peripheral`, `instance_signal`, `clock_root`, `shared_bus`, and
`reservation_owner` are non-empty strings or null; `dma_request` is a
non-negative integer or null; and `evidence_ids` is a non-empty string array.

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
millidegrees. At `rear-left-board-bottom`, X increases to the right, Y toward
the enclosure front and Z upward; board copper spans Z 0 through thickness, so
bottom-side bodies may have negative Z.

Placement field types and nullability are closed as follows:

| Object | Fields | Type and nullability |
|---|---|---|
| board | `board_id`, `enclosure_mpn`, `datum`, `status`, `notes` | non-null string |
| board | `outline_width_um`, `outline_height_um`, `thickness_um`, `guide_clearance_um` | non-negative integer |
| board | `evidence_ids` | non-empty string array |
| placement | `placement_id`, `item_id`, `kind`, `side`, `status`, `notes` | non-null string; item resolves to a component/module or named mechanical item |
| placement | `x_min_um`, `y_min_um`, `x_max_um`, `y_max_um` | non-negative integer |
| placement | `z_min_um`, `z_max_um`, `rotation_mdeg` | signed integer; extents remain ordered |
| placement | `panel_datum` | `rear-interior` or null |
| placement | `connector_face_setback_um` | non-negative integer or null; non-null iff panel datum is non-null |
| placement | `evidence_ids` | non-empty string array |
| keepout | `keepout_id`, `owner_id`, `kind`, `status`, `notes` | non-null string; owner resolves to a placement, route, cable or board |
| keepout | `x_min_um`, `y_min_um`, `x_max_um`, `y_max_um` | non-negative integer |
| keepout | `z_min_um`, `z_max_um` | signed integer; extents remain ordered |
| keepout | `applies_to`, `evidence_ids` | non-empty string array |
| route | `route_id`, `interface`, `source_placement_id`, `sink_placement_id`, `layer`, `ground_corridor_id`, `status`, `notes` | non-null string; placement and corridor references resolve |
| route | `maximum_length_um`, `estimated_length_um`, `via_count`, `target_diff_impedance_mohm`, `impedance_tolerance_mohm` | non-negative integer |
| route | `evidence_ids` | non-empty string array |
| ground corridor | `corridor_id`, `reference_layer`, `status`, `notes` | non-null string |
| ground corridor | `continuous` | boolean |
| ground corridor | `minimum_width_um`, `split_count`, `neck_count`, `via_transition_count` | non-negative integer |
| ground corridor | `route_ids`, `evidence_ids` | non-empty string array; routes resolve |
| cable | `cable_id`, `component_id`, `source_placement_id`, `sink_placement_id`, `insertion_axis`, `status`, `notes` | non-null string; component and placements resolve |
| cable | `x_min_um`, `y_min_um`, `x_max_um`, `y_max_um`, `minimum_bend_radius_um`, `service_slack_um` | non-negative integer |
| cable | `z_min_um`, `z_max_um` | signed integer; extents remain ordered |
| cable | `retainer_placement_id` | internal-ID string or null; resolves to a mechanical placement when non-null |
| cable | `evidence_ids` | non-empty string array |
| collision check | `check_id`, `subject_id`, `obstacle_id`, `status`, `notes` | non-null string; subject/obstacle resolve |
| collision check | `required_clearance_um`, `measured_clearance_um` | non-negative integer |
| collision check | `evidence_ids` | non-empty string array |
| criterion | `criterion_id`, `category`, `metric`, `unit`, `status`, `failure_action`, `notes` | non-null string |
| criterion | `actual_value` | integer for `um`/`count`/`mohm`, boolean for `boolean`; never null |
| criterion | `minimum_value`, `maximum_value` | same scalar type as actual or null; at least one non-null |
| criterion | `affected_row_ids`, `requirement_ids`, `evidence_ids` | non-empty string arrays; internal rows and evidence resolve |
| layer decision | `evaluated_layer_count` | integer constant 2 |
| layer decision | `selected_layer_count` | integer 2 or 4, or null only for blocked mechanical result |
| layer decision | `electrical_failure_ids`, `mechanical_failure_ids` | string arrays exactly projecting failed criteria |
| layer decision | `release_state`, `decision`, `rationale`, `notes` | non-null string; rationale non-empty |
| layer decision | `evidence_ids` | non-empty string array |

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
`status`, `notes`; insertion axis is exactly `positive-x`, `negative-x`,
`positive-y`, `negative-y`, `positive-z` or `negative-z`. Collision keys `check_id`,
`subject_id`, `obstacle_id`, `required_clearance_um`,
`measured_clearance_um`, `evidence_ids`, `status`, `notes`; pass requires
measured at least required.

Criterion keys `criterion_id`, `category`, `metric`, `actual_value`,
`minimum_value`, `maximum_value`, `unit`, `status`, `failure_action`,
`affected_row_ids`, `requirement_ids`, `evidence_ids`, `notes`. Category is
electrical/mechanical;
metric is `board-outline`, `guide-fit`, `rear-face-setback`, `route-length`,
`via-count`, `differential-impedance`, `ground-continuity`, `cable-clearance`,
`insertion-access` or `collision-clearance`; unit is `um`, `count`, `mohm` or
`boolean`. At least one bound is non-null. Electrical failure action is
`select-four-layer`; mechanical is `block-mechanical-redesign`.
For numeric units, pass means actual is at least each non-null minimum and at
most each non-null maximum. For boolean, every non-null bound equals actual; if
both bounds are present they must equal each other. Electrical criteria have
non-empty `affected_row_ids` resolving only to route or ground-corridor rows;
mechanical criteria have non-empty `affected_row_ids` resolving only to board,
placement, keepout, cable or collision-check rows.

Layer-decision keys `evaluated_layer_count`, `selected_layer_count`,
`electrical_failure_ids`, `mechanical_failure_ids`, `release_state`, `decision`,
`rationale`, `evidence_ids`, `notes`. Evaluated count is 2. No failures selects
2/proceed/`two-layer-pass`; electrical-only failures select
4/proceed/`four-layer-required`; any mechanical failure selects
null/blocked/`mechanical-redesign-required`. Failure arrays exactly equal failed
criterion IDs and mechanical dominates. The set of failed route and
ground-corridor row IDs must equal the union of `affected_row_ids` across failed
electrical criteria. Each failed route or ground-corridor row is therefore
referenced by at least one failed electrical criterion, and each failed
electrical criterion references at least one failed route or ground-corridor
row. This bidirectional projection, an empty `mechanical_failure_ids` array and
the exact 4/proceed/`four-layer-required` decision are all required to permit
those row failures; any mismatch blocks P1D3/P1D4.

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
`power_rail_id`, `reference_domain_id`, `endpoint_refs`, `safe_default`, `test_point_ids`,
`evidence_ids`, `status`, `notes`; kind is `signal`, `power`, `ground` or
`shield`. Its reference matrix is exact:

| `kind` | `signal_id` | `power_rail_id` | `reference_domain_id` |
|---|---|---|---|
| `signal` | non-null, resolves to signal | null | null |
| `power` | null | non-null, resolves to power rail | null |
| `ground` | null | null | `digital-ground` |
| `shield` | null | null | `chassis-shield` |

Exactly one of the three reference fields is non-null according to this matrix.
Endpoints have at least two entries, and safe default is null or `processed-path`, `disabled`,
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

Schematic field types and nullability are closed as follows:

| Object | Fields | Type and nullability |
|---|---|---|
| root schematic | `path`, `title`, `status`, `notes` | non-null string |
| root schematic | `hierarchical_sheet_ids`, `symbol_library_ids`, `evidence_ids` | non-empty string array; every ID resolves; sheet IDs use the stated semantic order |
| sheet | `sheet_id`, `path`, `title`, `status`, `notes` | non-null string |
| sheet | `support_circuit_ids`, `component_ids`, `signal_ids`, `power_rail_ids`, `requirement_ids`, `evidence_ids` | non-empty string array; every ID resolves |
| symbol library | `library_id`, `kind`, `status`, `notes` | non-null string |
| symbol library | `path` | project-relative `.kicad_sym` string for local; null for standard |
| symbol library | `symbol_ids`, `evidence_ids` | non-empty string array |
| net binding | `net_id`, `sheet_id`, `kind`, `status`, `notes` | non-null string; sheet resolves |
| net binding | `signal_id`, `power_rail_id`, `reference_domain_id` | string or null exactly as the kind matrix requires |
| net binding | `endpoint_refs` | string array with at least two entries |
| net binding | `safe_default` | listed string or null |
| net binding | `test_point_ids`, `evidence_ids` | string array; evidence non-empty and references resolve |
| test point | `test_point_id`, `sheet_id`, `net_id`, `purpose`, `status`, `notes` | non-null string; sheet/net references resolve |
| test point | `required` | boolean |
| test point | `requirement_ids`, `evidence_ids` | string array; evidence non-empty |
| ERC policy | `command`, `status`, `notes` | non-null string |
| ERC policy | `project_file_allowed` | boolean constant false |
| ERC policy | `waiver_count`, `ignored_violation_count`, `accepted_error_count`, `accepted_warning_count` | integer constant zero |

#### Normative design-basis contract

Engineering choices are not made implicitly while projecting the three authored
design documents. Each document first receives one committed normative basis at
`evidence/design/basis/<document>-basis.json`, validated by
`design-basis-schema.json`. Root keys are `schema_version`, `basis_id`,
`target_path`, `target_sha256`, `target_document`, `field_derivations`,
`evidence_ids`, `notes`. The schema version is `1.0.0`; basis IDs are exactly
`electrical-reference-basis`, `pin-resource-map-basis` and
`two-layer-placement-basis`; target paths are their three canonical design JSON
paths. `target_document` is a complete schema-valid authored target, not a
partial overlay, and `target_sha256` is the lowercase SHA-256 of its canonical
Prettier bytes.

A field-derivation row keys `json_pointer`, `method`, `expression`,
`input_refs`, `evidence_ids`, `rationale`. Rows are sorted by JSON pointer and
cover every scalar leaf in `target_document` exactly once, including every row
ID, ordinal-bearing array position, fixed engineering value, status and note.
`method` is `literal`, `copy`, `formula`, `measurement` or
`engineering-choice`; `expression` and `rationale` are non-empty. `input_refs`
is a sorted unique array of repository-relative path plus JSON Pointer strings;
it is non-empty except for a literal. Evidence IDs are sorted, resolve to the
closed source manifest, and are non-empty for copy, formula and measurement.
An engineering choice names its accepted requirement or engineering-policy ID
in `input_refs` and explains the bounded decision in `rationale`. Every target
row therefore has an exact ID, order, value or formula, and evidence basis before
canonical materialization begins.

Large concerns are authored as fragments validated by
`design-basis-fragment-schema.json`. Fragment root keys are `schema_version`,
`fragment_id`, `target_document`, `target_sections`, `payload`,
`field_derivations`, `evidence_ids`, `notes`. `target_sections` is a non-empty
ordered subset of the literal assembly table below; payload keys and order match
that subset exactly, and derivations cover every scalar payload leaf. Fragment
IDs and ownership are closed:

| Fragment | Target | Exact target sections |
|---|---|---|
| `electrical-support-circuits` | electrical | `support_circuits` |
| `electrical-signals` | electrical | `signals` |
| `electrical-power-rails` | electrical | `power_rails` |
| `pin-allocations` | pin | `pins` |
| `placement-envelope` | placement | `board`, `placements`, `keepouts` |
| `placement-routing` | placement | `routes`, `ground_corridors`, `electrical_criteria` |
| `placement-mechanical-verification` | placement | `cables`, `collision_checks`, `mechanical_criteria` |
| `placement-layer-decision` | placement | `layer_decision` |

The assembler rejects missing/extra fragments and overlapping sections. It
forms `criteria` by concatenating electrical then mechanical criteria and sorting
by `criterion_id`; it copies the other sections literally; it computes each
target root requirement/evidence array as the sorted nested union and sets root
notes empty. It then emits the complete basis wrapper and target hash. Thus
every agent owns one bounded engineering concern and the parent owns only a
fixed, tested assembly.

The target byte contract is exact: canonicalizing `target_document` with the
repository Prettier version must produce `target_sha256`; the materializer writes
those bytes without adding or deriving fields. Cross-basis references are
serial: the pin basis may reference the committed electrical basis, and the
placement basis may reference both earlier bases. No later projector may alter
an engineering value.

#### One-way document lifecycle

`scripts/fixtures/p1d-schema/reference-package/base-v1.json` contains all four
exact placeholders. Builder operations are
`bootstrap --write|--check`,
`validate-basis-fragment --input <path>`,
`assemble-basis <electrical-reference|pin-resource-map|two-layer-placement> --write|--check`,
`validate-basis <electrical-reference|pin-resource-map|two-layer-placement> --input <path>`,
`materialize-design-records --write|--check`, and
`validate --stage placeholders|design-records-structural|design-records|schematic-stubs-unlocked|schematic-stubs|schematic-sheets|schematic`,
`validate-document <electrical-reference|pin-resource-map|two-layer-placement|reference-schematic-contract>
--input <path> --placeholder-package <path>`, plus
`schematic-interface lock --write|--check`. `validate-document` is read-only: it
substitutes the named authored input into the exact locked placeholder package,
validates its schema, local invariants and P1 external references, and defers
only cross-design-document references to the parent design-record stages.
`validate-basis-fragment` and `assemble-basis` enforce the closed fragment table,
exact leaf coverage and deterministic assembly. `validate-basis` checks
input/evidence resolution, target schema, hash and serial cross-basis references.
`materialize-design-records`
requires all three valid bases and writes or checks only their byte-locked target
documents. Bootstrap creates or byte-checks placeholders and refuses to
overwrite authored state.

The six validation states are one-way:

1. `placeholders` requires four exact JSON placeholders and no reference KiCad
   files.
2. `design-records-structural` requires three valid bases, their byte-identical
   electrical, pin and placement targets, the schematic contract still the exact
   placeholder, no reference KiCad files, and all cross-document references and
   arithmetic valid. It accepts recorded engineering failures.
   `design-records` adds the release gate: it exits 0 only for an eligible design
   and exits 3 for a structurally valid blocked design.
3. `schematic-stubs-unlocked` adds the root, local symbol library, README and
   four interface-only sheets. It validates every ordered hierarchical pin,
   every local symbol and every KiCad-standard symbol against the precommitted
   interface contract, rejects symbols outside their exact union, requires the
   schematic contract placeholder, and requires no stub lock.
4. `schematic-stubs` additionally requires
   `scripts/fixtures/p1d-schema/reference-package/schematic-stub-lock-v1.json`
   with exact SHA-256 values for those seven files, exact locked interface
   tuples and both ordered symbol inventories. The parent creates it only after
   ERC and the unlocked stage pass.
5. `schematic-sheets` requires the root, library and README to retain their
   locked hashes, every one of the four sheet hashes to differ from its locked
   stub hash, every sheet to preserve its locked hierarchical interface and use
   only the locked symbol union, and the schematic contract to remain the
   placeholder.
6. `schematic` requires the `schematic-sheets` conditions and all four JSON
   documents authored.

`schematic-interface lock --check` never writes. Bootstrap refuses when any
reference KiCad file or lock exists, so it cannot downgrade a schematic state;
the lock command cannot relock implemented sheets. `lock --write` first performs
the complete unlocked structural validation and publishes atomically only if it
passes. Renderers reject placeholders. Tests cover every legal transition,
partial sheet replacement, root/library/interface drift, failed pre-lock proof,
attempted relock, and every downgrade refusal.

```yaml
phase: P1D3a1
unit: test-surface
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
  - name: design-basis-schema
    owned-paths:
      - evidence/design/design-basis-schema.json
      - evidence/design/design-basis-fragment-schema.json
      - scripts/fixtures/p1d-schema/design-basis
  - name: p1d3-schema-test-coordinator
    owned-paths:
      - scripts/test_p1d_schemas.py
      - scripts/fixtures/p1d-schema/reference-package
coordination: five disjoint schema tasks run with maximum three concurrent; test coordinator runs last
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_p1d_schemas.py
    - node_modules/.bin/prettier --check evidence/design/electrical-reference-schema.json evidence/design/electrical-reference.json evidence/design/pin-resource-map-schema.json evidence/design/pin-resource-map.json evidence/design/two-layer-placement-schema.json evidence/design/two-layer-placement.json evidence/design/reference-schematic-contract-schema.json evidence/design/design-basis-schema.json evidence/design/design-basis-fragment-schema.json electronics/reference/reference-schematic-contract.json
  buildable: [closed independently fixture-proven design schemas]
  parent-commit: "feat(aius): define electrical reference schemas"
```

One coordinator owns `scripts/test_p1d_schemas.py` after the five disjoint schema
tasks return. It registers all fixture directories without modifying their
owned files.

### P1D3a2 — Contract builder

The builder creates the valid base package and enforces the one-way lifecycle.

```yaml
phase: P1D3a2
unit: component-behind-interface
seams: [closed schemas, canonical package, one-way document lifecycle]
contract-artifact: stubs-committed
fan-out:
  - name: p1d3-contract-builder
    owned-paths:
      - scripts/build_p1d_reference_contract.py
      - scripts/test_build_p1d_reference_contract.py
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_build_p1d_reference_contract.py
    - python3 scripts/build_p1d_reference_contract.py bootstrap --check
    - python3 scripts/build_p1d_reference_contract.py validate --stage placeholders
  buildable: [fixture-proven closed design package and lifecycle]
  parent-commit: "feat(aius): build electrical reference contracts"
```

Every `build_p1d_reference_contract.py` option that accepts a path uses one
locked rule: an absolute path is accepted only beneath the resolved Aius root;
a relative path is interpreted as Aius-root-relative, never caller-CWD-relative.
Output and placeholder-package paths use the same rule. Diagnostics render the
normalized Aius-relative path. `test_build_p1d_reference_contract.py` invokes
every path-bearing subcommand from the repository root and an unrelated nested
working directory, then proves identical bytes/diagnostics; it also proves
absolute in-root acceptance and rejects `..`, absolute escape, symlink escape,
missing and directory-valued inputs with usage exit 2. Thus commands such as
`--input evidence/design/...` below are executable from any working directory.

The three renderer concerns land serially because each deepens the existing
`render_p1.py`/`test_render_p1.py` pair. Before each renderer dispatch, a separate
checkpoint commits its authored input and adversarial expected bytes. The
renderer worker may not edit those golden files. Each renderer has `--write` and
`--check`; neither mode accepts a placeholder.

The four exact outputs are:

- `docs/reference/electrical-reference.md`;
- `docs/reference/pin-resource-map.csv`;
- `docs/reference/pin-resource-map.md`;
- `docs/reference/two-layer-placement.md`.

Markdown reuses the committed `GENERATED_NOTICE`, `_scalar_text`,
`_markdown_escape`, `_markdown_scalar`, `_markdown_cell`, `_table`, and
`_render_markdown` rules byte-for-byte. First lines are exactly `# Aius
electrical reference`, `# Aius pin resource map`, or `# Aius two-layer
placement`, then a blank line and `<!-- Generated by scripts/render_p1.py; do not
edit. -->`. Every section is preceded by one blank line, `## <heading>`, one
blank line, then `| <raw key> |` headers, `| --- |` separators and data rows; an
empty collection still emits header and separator. Headers are never wrapped in
backticks, and the exact backtick-wrapped field set is empty for every table.

Cells reuse the existing rules: null is empty, Boolean is lowercase, strings are
literal scalar text, numbers use compact JSON, a list joins individually rendered
items with `<br>`, and a nested object uses sorted compact JSON. Backslash becomes
`\\`, pipe becomes `\|`, and CRLF, bare CR and LF each become `<br>`. Output is
UTF-8 with LF line endings and exactly one final LF.

Electrical sections and columns are exact: Metadata has `schema_version`,
`document_state`; Support circuits, Signals and Power rails use each row's full
locked key order above; Coverage has `requirement_ids`, `evidence_ids` from one
root row; Notes has `notes` from one root row. Pin sections are Metadata with the
same two keys; Pin resources with all 14 locked keys; Reservation coverage with
`reservation_owner`, `signal_ids`, one row per non-null owner sorted by owner and
lexically sorted signal IDs; Evidence with `evidence_id`, one row per root ID;
and Notes with root `notes`. Placement sections are Metadata; Board; Placements;
Keepouts; Routes; Ground corridors; Cables; Collision checks; Criteria; Layer
decision; Coverage; Notes. Metadata/Coverage/Notes use the same keys as above;
every factual section uses its full locked object key order, and a null Board or
Layer decision produces no data row.

The pin CSV header is the 14 pin fields in locked order. It reuses `_csv_cell`
and Python `csv.writer(dialect="excel", lineterminator="\\n",
quoting=csv.QUOTE_MINIMAL)`: null is empty, Boolean is lowercase, dict/list is
sorted compact JSON, every other scalar is `str(value)`, quote is doubled, and
comma/quote/CR/LF triggers quoting. Goldens cover empty/null, delimiter, quote,
all line endings, backslash, pipe, backtick, Unicode, list/object, ordering and
final-newline boundaries.

Golden directory membership is exact: `electrical-reference/` contains
`input.json`, `expected.md`; `pin-resource-map/` contains `input.json`,
`expected.csv`, `expected.md`; `two-layer-placement/` contains `input.json`,
`expected.md`. Each input is one complete schema-valid authored document that
contains every adversarial scalar boundary applicable to that document. Expected
files are literal final bytes, not templates or hashes.

```yaml
phase: P1D3a3g
unit: test-surface
seams: [authored electrical fixture, exact Markdown bytes]
contract-artifact: spec-only
fan-out:
  - name: electrical-renderer-golden
    owned-paths: [scripts/fixtures/render-p1/electrical-reference]
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/build_p1d_reference_contract.py validate-document electrical-reference --input scripts/fixtures/render-p1/electrical-reference/input.json --placeholder-package scripts/fixtures/p1d-schema/reference-package/base-v1.json
  buildable: [committed complete electrical renderer input and exact expected Markdown]
  parent-commit: "test(aius): lock electrical renderer bytes"
```

```yaml
phase: P1D3a3
unit: component-behind-interface
seams: [authored electrical JSON, deterministic Markdown]
contract-artifact: stubs-committed
fan-out:
  - name: electrical-reference-renderer
    owned-paths:
      - scripts/render_p1.py
      - scripts/test_render_p1.py
review-intensity: active
commit-checkpoint:
  green: [python3 scripts/test_render_p1.py]
  buildable: [fixture-proven electrical Markdown renderer]
  parent-commit: "feat(aius): render electrical reference"
```

```yaml
phase: P1D3a4g
unit: test-surface
seams: [authored pin fixture, exact CSV and Markdown bytes]
contract-artifact: spec-only
fan-out:
  - name: pin-renderer-golden
    owned-paths: [scripts/fixtures/render-p1/pin-resource-map]
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/build_p1d_reference_contract.py validate-document pin-resource-map --input scripts/fixtures/render-p1/pin-resource-map/input.json --placeholder-package scripts/fixtures/p1d-schema/reference-package/base-v1.json
  buildable: [committed complete pin renderer input and exact expected CSV and Markdown]
  parent-commit: "test(aius): lock pin renderer bytes"
```

```yaml
phase: P1D3a4
unit: component-behind-interface
seams: [authored pin JSON, deterministic CSV and Markdown]
contract-artifact: stubs-committed
fan-out:
  - name: pin-resource-renderer
    owned-paths:
      - scripts/render_p1.py
      - scripts/test_render_p1.py
review-intensity: active
commit-checkpoint:
  green: [python3 scripts/test_render_p1.py]
  buildable: [fixture-proven pin CSV and Markdown renderer]
  parent-commit: "feat(aius): render pin resource map"
```

```yaml
phase: P1D3a5g
unit: test-surface
seams: [authored placement fixture, exact Markdown bytes]
contract-artifact: spec-only
fan-out:
  - name: placement-renderer-golden
    owned-paths: [scripts/fixtures/render-p1/two-layer-placement]
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/build_p1d_reference_contract.py validate-document two-layer-placement --input scripts/fixtures/render-p1/two-layer-placement/input.json --placeholder-package scripts/fixtures/p1d-schema/reference-package/base-v1.json
  buildable: [committed complete placement renderer input and exact expected Markdown]
  parent-commit: "test(aius): lock placement renderer bytes"
```

```yaml
phase: P1D3a5
unit: component-behind-interface
seams: [authored placement JSON, deterministic Markdown]
contract-artifact: stubs-committed
fan-out:
  - name: two-layer-placement-renderer
    owned-paths:
      - scripts/render_p1.py
      - scripts/test_render_p1.py
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_render_p1.py
    - python3 scripts/build_p1d_reference_contract.py validate --stage placeholders
  buildable: [all fixture-proven deterministic P1D renderers]
  parent-commit: "feat(aius): render two-layer placement"
```

### P1D3b0 — Evidence-derived normative design bases

The substantive engineering synthesis is seven serial checkpoints. Each worker
authors one bounded fragment; the parent runs only the committed fixed assembler.
The ordering makes every cross-concern dependency a committed input.

```yaml
phase: P1D3b0a
unit: single-file-implementation
seams: [closed source manifest, selected components, support-circuit inventory]
contract-artifact: spec-only
depends-on:
  - evidence/manifest/sources.json
  - evidence/design/selection-policy.json
  - evidence/design/contracts/functional-topology.json
  - evidence/design/contracts/interfaces-and-states.json
  - evidence/design/design-basis-fragment-schema.json
fan-out:
  - name: electrical-support-circuit-basis
    owned-paths: [evidence/design/basis/fragments/electrical-support-circuits.json]
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/build_p1d_reference_contract.py validate-basis-fragment --input evidence/design/basis/fragments/electrical-support-circuits.json
  review-focus: [support-circuit topology, component values, retained evidence]
  buildable: [closed support-circuit inventory and values]
  parent-commit: "design(aius): establish support-circuit basis"
```

```yaml
phase: P1D3b0b
unit: single-file-implementation
seams: [committed support circuits, signal compatibility, power budgets]
contract-artifact: spec-only
depends-on:
  - evidence/design/basis/fragments/electrical-support-circuits.json
  - evidence/design/contracts/functional-topology.json
  - evidence/design/contracts/interfaces-and-states.json
fan-out:
  - name: electrical-signal-basis
    owned-paths: [evidence/design/basis/fragments/electrical-signals.json]
  - name: electrical-power-basis
    owned-paths: [evidence/design/basis/fragments/electrical-power-rails.json]
coordination-owned-paths:
  - evidence/design/basis/electrical-reference-basis.json
coordination: maximum two disjoint fragment agents; parent validates both then runs assemble-basis electrical-reference --write and --check
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/build_p1d_reference_contract.py validate-basis-fragment --input evidence/design/basis/fragments/electrical-signals.json
    - python3 scripts/build_p1d_reference_contract.py validate-basis-fragment --input evidence/design/basis/fragments/electrical-power-rails.json
    - python3 scripts/build_p1d_reference_contract.py assemble-basis electrical-reference --write
    - python3 scripts/build_p1d_reference_contract.py assemble-basis electrical-reference --check
    - python3 scripts/build_p1d_reference_contract.py validate-basis electrical-reference --input evidence/design/basis/electrical-reference-basis.json
  review-focus: [signal thresholds, interface margins, power budgets]
  buildable: [complete field-by-field electrical design basis]
  parent-commit: "design(aius): establish electrical design basis"
```

```yaml
phase: P1D3b0c
unit: single-file-implementation
seams: [committed electrical basis, PJRC pin evidence, pin-allocation basis]
contract-artifact: spec-only
depends-on:
  - evidence/design/basis/electrical-reference-basis.json
  - https://www.pjrc.com/teensy/card11a_rev4_web.pdf
  - https://www.pjrc.com/teensy/card11b_rev4_web.pdf
  - https://www.pjrc.com/teensy/IMXRT1060RM_rev3_annotations.pdf
fan-out:
  - name: pin-resource-design-basis
    owned-paths: [evidence/design/basis/fragments/pin-allocations.json]
coordination-owned-paths:
  - evidence/design/basis/pin-resource-map-basis.json
coordination: parent validates the fragment then runs assemble-basis pin-resource-map --write and --check
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/build_p1d_reference_contract.py validate-basis-fragment --input evidence/design/basis/fragments/pin-allocations.json
    - python3 scripts/build_p1d_reference_contract.py assemble-basis pin-resource-map --write
    - python3 scripts/build_p1d_reference_contract.py assemble-basis pin-resource-map --check
    - python3 scripts/build_p1d_reference_contract.py validate-basis pin-resource-map --input evidence/design/basis/pin-resource-map-basis.json
  review-focus: [pin mux, DMA ownership, peripheral and reservation conflicts]
  buildable: [complete field-by-field pin allocation basis]
  parent-commit: "design(aius): establish pin allocation basis"
```

```yaml
phase: P1D3b0d
unit: single-file-implementation
seams: [enclosure guide, component geometry, board placement and keepouts]
contract-artifact: spec-only
depends-on:
  - evidence/design/basis/electrical-reference-basis.json
  - evidence/design/basis/pin-resource-map-basis.json
  - https://www.hammfg.com/part/1455N1201BK
  - https://www.hammfg.com/part/1455N1201BK
fan-out:
  - name: placement-envelope-basis
    owned-paths: [evidence/design/basis/fragments/placement-envelope.json]
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/build_p1d_reference_contract.py validate-basis-fragment --input evidence/design/basis/fragments/placement-envelope.json
  review-focus: [enclosure datum, placement envelope, keepouts]
  buildable: [exact board placement and keepout basis]
  parent-commit: "design(aius): establish placement envelope basis"
```

```yaml
phase: P1D3b0e
unit: single-file-implementation
seams: [committed placement envelope, USB and Ethernet routing, return path]
contract-artifact: spec-only
depends-on:
  - evidence/design/basis/fragments/placement-envelope.json
  - evidence/design/basis/electrical-reference-basis.json
fan-out:
  - name: placement-routing-basis
    owned-paths: [evidence/design/basis/fragments/placement-routing.json]
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/build_p1d_reference_contract.py validate-basis-fragment --input evidence/design/basis/fragments/placement-routing.json
  review-focus: [impedance, length, via budget, return path]
  buildable: [exact route corridor and electrical-criterion basis]
  parent-commit: "design(aius): establish routing feasibility basis"
```

```yaml
phase: P1D3b0f
unit: single-file-implementation
seams: [committed placement envelope, cable geometry, collision and access criteria]
contract-artifact: spec-only
depends-on:
  - evidence/design/basis/fragments/placement-envelope.json
  - evidence/design/components/internal-usb-cable.json
  - evidence/design/components/mechanical-cable-retainer.json
fan-out:
  - name: placement-mechanical-verification-basis
    owned-paths: [evidence/design/basis/fragments/placement-mechanical-verification.json]
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/build_p1d_reference_contract.py validate-basis-fragment --input evidence/design/basis/fragments/placement-mechanical-verification.json
  review-focus: [cable collision, insertion access, service access]
  buildable: [exact cable collision and mechanical-criterion basis]
  parent-commit: "design(aius): establish mechanical verification basis"
```

```yaml
phase: P1D3b0g
unit: single-file-implementation
seams: [committed electrical and mechanical criteria, deterministic layer decision]
contract-artifact: spec-only
depends-on:
  - evidence/design/basis/fragments/placement-envelope.json
  - evidence/design/basis/fragments/placement-routing.json
  - evidence/design/basis/fragments/placement-mechanical-verification.json
fan-out:
  - name: placement-layer-decision-basis
    owned-paths: [evidence/design/basis/fragments/placement-layer-decision.json]
coordination-owned-paths:
  - evidence/design/basis/two-layer-placement-basis.json
coordination: parent validates the layer fragment then runs assemble-basis two-layer-placement --write and --check
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/build_p1d_reference_contract.py validate-basis-fragment --input evidence/design/basis/fragments/placement-layer-decision.json
    - python3 scripts/build_p1d_reference_contract.py assemble-basis two-layer-placement --write
    - python3 scripts/build_p1d_reference_contract.py assemble-basis two-layer-placement --check
    - python3 scripts/build_p1d_reference_contract.py validate-basis two-layer-placement --input evidence/design/basis/two-layer-placement-basis.json
  review-focus: [electrical versus mechanical failure classification, layer decision]
  buildable: [complete field-by-field two-layer placement basis]
  parent-commit: "design(aius): establish placement design basis"
```

`review-focus` is a non-executable clean-room delivery-audit requirement, not a
green command. At each P1D3b0 boundary the parent supplies only this plan, the
owned fragment, its committed dependencies and current validators to a reviewer.
The reviewer returns PASS or numbered findings against the named focus; findings
are fixed or recorded before the exact executable commands can form a commit
boundary. The phase note records the review result, while process exit status is
derived only from the listed commands.

### P1D3b — Materialized design records and verdict

One bounded integration worker performs the mechanical projection only after all
three basis commits. It materializes byte-locked canonical JSON, renders the
four review outputs and records one of two committable outcomes. It does not
invent or change a target value during this phase.

```yaml
phase: P1D3b
unit: phase-task
seams: [three committed normative bases, byte-locked design records, explicit design verdict]
contract-artifact: both
depends-on:
  - evidence/design/basis/electrical-reference-basis.json
  - evidence/design/basis/pin-resource-map-basis.json
  - evidence/design/basis/two-layer-placement-basis.json
fan-out:
  - name: design-record-materialization
    owned-paths:
      - evidence/design/electrical-reference.json
      - evidence/design/pin-resource-map.json
      - evidence/design/two-layer-placement.json
      - docs/reference/electrical-reference.md
      - docs/reference/pin-resource-map.csv
      - docs/reference/pin-resource-map.md
      - docs/reference/two-layer-placement.md
coordination: one bounded integration worker runs only the committed materializer, renderers and verdict gate; basis agents never edit target or generated paths
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/build_p1d_reference_contract.py materialize-design-records --write
    - python3 scripts/build_p1d_reference_contract.py materialize-design-records --check
    - python3 scripts/build_p1d_reference_contract.py validate --stage design-records-structural
    - python3 scripts/render_p1.py electrical-reference --write
    - python3 scripts/render_p1.py pin-resource-map --write
    - python3 scripts/render_p1.py two-layer-placement --write
    - python3 scripts/render_p1.py electrical-reference --check
    - python3 scripts/render_p1.py pin-resource-map --check
    - python3 scripts/render_p1.py two-layer-placement --check
  branches:
    eligible:
      command: python3 scripts/build_p1d_reference_contract.py validate --stage design-records
      expected-exit: 0
      buildable: [evidence-closed electrical pin and placement records eligible for P1D3c]
      parent-commit: "design(aius): approve electrical design records"
    blocked:
      command: python3 scripts/build_p1d_reference_contract.py validate --stage design-records
      expected-exit: 3
      expected-diagnostics: sorted exact AIUS-P1D-DESIGN-BLOCKED lines only
      buildable: [structurally complete evidence-closed blocked design records]
      parent-commit: "design(aius): record blocked electrical design"
```

Each blocked diagnostic is exactly
`AIUS-P1D-DESIGN-BLOCKED:<document>:<row-kind>:<row-id>:<status>`. The set is the
sorted unique projection of every blocking support-circuit, signal, rail, pin,
mechanical row or layer decision under the rules above. An eligible
four-layer-required electrical route is not blocking. Any other diagnostic or
exit status fails the checkpoint. The blocked branch commits the bases,
canonical records and rendered reviews, records the expected diagnostics in the
phase note, and stops before P1D3c0.

Document tokens are exactly `electrical-reference`, `pin-resource-map`, and
`two-layer-placement`. Row-kind/ID/status projections are exactly:
`support-circuit`/`circuit_id`/`fail`, `signal`/`signal_id`/`fail`,
`power-rail`/`rail_id`/`fail`, `pin`/`signal_id`/`conflict`,
`board`/`board_id`/`fail`, `placement`/`placement_id`/`fail`,
`keepout`/`keepout_id`/`fail`, `route`/`route_id`/`fail`,
`ground-corridor`/`corridor_id`/`fail`, `cable`/`cable_id`/`fail`,
`collision-check`/`check_id`/`fail`, and
`criterion`/`criterion_id`/`fail`. A mechanically blocked layer-decision object
adds the synthetic projection
`two-layer-placement:layer-decision:layer-decision:blocked`; no field is read as
an ID or status from that object. Duplicate projections collapse before lexical
sorting.

### P1D3c0 — Committed schematic interface

`electronics/reference/schematic-interface-v1.json` is a closed object with
keys `schema_version`, `sheets`, `local_symbol_ids`, `standard_symbol_ids`,
`symbol_bindings`. A sheet has `sheet_id`, `path`, `pins`; a pin has `name`,
`direction`, `electrical_type`, `binding`. A symbol binding keys `symbol_id`,
`component_ids`, `source_ids`, `support_circuit_ids`, `pin_count`,
`evidence_ids`. The arrays are sorted unique; component/source/evidence
references resolve; at least one component or source resolves; support circuits
resolve to the committed electrical basis; pin count is a positive integer
proved by the retained component evidence. Binding rows are in local-symbol
inventory order and cover that inventory exactly once.
Sheet order is power-usb, optical-spdif, ethernet, teensy-expansion. Direction is
`into-sheet`, `out-of-sheet` or `bidirectional`; electrical type is `input`,
`output`, `bidirectional`, `passive`, `power-input` or `power-output`. Binding is
exactly `signal:<signal-id>`, `rail:<rail-id>` or `domain:<domain-id>`.

The exact ordered pin tuples are:

| Sheet | Name | Direction | Electrical type | Binding |
|---|---|---|---|---|
| power-usb | `USB_DEV_DP` | bidirectional | bidirectional | `signal:usb-device-dp` |
| power-usb | `USB_DEV_DM` | bidirectional | bidirectional | `signal:usb-device-dm` |
| power-usb | `PROTECTED_5V` | out-of-sheet | power-output | `rail:protected-5v` |
| power-usb | `DIGITAL_GROUND` | bidirectional | passive | `domain:digital-ground` |
| power-usb | `CHASSIS_SHIELD` | bidirectional | passive | `domain:chassis-shield` |
| optical-spdif | `SPDIF_RX_LOGIC` | out-of-sheet | output | `signal:spdif-rx-logic` |
| optical-spdif | `SPDIF_TX_LOGIC` | into-sheet | input | `signal:spdif-tx-logic` |
| optical-spdif | `SPDIF_PATH_SELECT` | into-sheet | input | `signal:spdif-path-select` |
| optical-spdif | `OPTICAL_TX_ENABLE` | into-sheet | input | `signal:optical-tx-enable` |
| optical-spdif | `TEENSY_3V3` | into-sheet | power-input | `rail:teensy-3v3` |
| optical-spdif | `DIGITAL_GROUND` | bidirectional | passive | `domain:digital-ground` |
| ethernet | `ETH_TX_P` | into-sheet | input | `signal:ethernet-tx-p` |
| ethernet | `ETH_TX_N` | into-sheet | input | `signal:ethernet-tx-n` |
| ethernet | `ETH_RX_P` | out-of-sheet | output | `signal:ethernet-rx-p` |
| ethernet | `ETH_RX_N` | out-of-sheet | output | `signal:ethernet-rx-n` |
| ethernet | `ETH_LED_LINK` | into-sheet | input | `signal:ethernet-led-link` |
| ethernet | `ETH_LED_ACTIVITY` | into-sheet | input | `signal:ethernet-led-activity` |
| ethernet | `TEENSY_3V3` | into-sheet | power-input | `rail:teensy-3v3` |
| ethernet | `DIGITAL_GROUND` | bidirectional | passive | `domain:digital-ground` |
| ethernet | `CHASSIS_SHIELD` | bidirectional | passive | `domain:chassis-shield` |
| teensy-expansion | `USB_DEV_DP` | bidirectional | bidirectional | `signal:usb-device-dp` |
| teensy-expansion | `USB_DEV_DM` | bidirectional | bidirectional | `signal:usb-device-dm` |
| teensy-expansion | `PROTECTED_5V` | into-sheet | power-input | `rail:protected-5v` |
| teensy-expansion | `SPDIF_RX_LOGIC` | into-sheet | input | `signal:spdif-rx-logic` |
| teensy-expansion | `SPDIF_TX_LOGIC` | out-of-sheet | output | `signal:spdif-tx-logic` |
| teensy-expansion | `SPDIF_PATH_SELECT` | out-of-sheet | output | `signal:spdif-path-select` |
| teensy-expansion | `OPTICAL_TX_ENABLE` | out-of-sheet | output | `signal:optical-tx-enable` |
| teensy-expansion | `ETH_TX_P` | out-of-sheet | output | `signal:ethernet-tx-p` |
| teensy-expansion | `ETH_TX_N` | out-of-sheet | output | `signal:ethernet-tx-n` |
| teensy-expansion | `ETH_RX_P` | into-sheet | input | `signal:ethernet-rx-p` |
| teensy-expansion | `ETH_RX_N` | into-sheet | input | `signal:ethernet-rx-n` |
| teensy-expansion | `ETH_LED_LINK` | out-of-sheet | output | `signal:ethernet-led-link` |
| teensy-expansion | `ETH_LED_ACTIVITY` | out-of-sheet | output | `signal:ethernet-led-activity` |
| teensy-expansion | `TEENSY_3V3` | out-of-sheet | power-output | `rail:teensy-3v3` |
| teensy-expansion | `DIGITAL_GROUND` | bidirectional | passive | `domain:digital-ground` |

The exact lexically ordered local-symbol inventory is
`selected-cliff-rx`, `selected-cliff-tx`, `selected-ethernet-magjack`,
`selected-ethernet-socket`,
`selected-expansion-gpio-adc`, `selected-expansion-i2c-control`,
`selected-expansion-i2c-isolator`, `selected-expansion-i2s-tdm`,
`selected-expansion-power-protection`, `selected-expansion-spi`,
`selected-expansion-uart-debug`, `selected-expansion-usb-host`,
`selected-internal-usb-a-receptacle`, `selected-remote-program-rtc-header`,
`selected-side-socket`, `selected-spdif-bypass-mux`,
`selected-spdif-tx-gate`, `selected-test-fixture-header`,
`selected-usb-c-receptacle`, `selected-usb-esd`,
`selected-usb-host-socket`, `selected-usb-power-protection`,
`teensy-4-1-module`.

The component/support-circuit binding is closed before any KiCad worker starts:

| Local symbol | Component or source | Required support circuit |
|---|---|---|
| `selected-cliff-rx` | component `cliff-rx` / MPN `ORJ5` | `optical-rx-interface` |
| `selected-cliff-tx` | component `cliff-tx` / MPN `OTJ5` | `optical-tx-interface` |
| `selected-ethernet-magjack` | component `ethernet-magjack` | `ethernet-interface` |
| `selected-ethernet-socket` | component `teensy-ethernet-socket` | `ethernet-interface` |
| `selected-expansion-gpio-adc` | component `expansion-gpio-adc` | `expansion-gpio-adc-interface` |
| `selected-expansion-i2c-control` | component `expansion-i2c-control` | `expansion-i2c-control-interface` |
| `selected-expansion-i2c-isolator` | component `expansion-i2c-isolator` | `expansion-i2c-isolation` |
| `selected-expansion-i2s-tdm` | component `expansion-i2s-tdm` | `expansion-i2s-tdm-interface` |
| `selected-expansion-power-protection` | component `expansion-power-protection` | `expansion-power-protection` |
| `selected-expansion-spi` | component `expansion-spi` | `expansion-spi-interface` |
| `selected-expansion-uart-debug` | component `expansion-uart-debug` | `expansion-uart-debug-interface` |
| `selected-expansion-usb-host` | component `expansion-usb-host` | `expansion-usb-host-interface` |
| `selected-internal-usb-a-receptacle` | component `usb-a-receptacle` | `internal-usb-service` |
| `selected-remote-program-rtc-header` | component `remote-program-rtc` | `remote-program-rtc-interface` |
| `selected-side-socket` | component `teensy-side-socket` | `teensy-module-interface` |
| `selected-spdif-bypass-mux` | component `spdif-mux` | `spdif-bypass-selection` |
| `selected-spdif-tx-gate` | component `spdif-tx-gate` | `spdif-transmit-gate` |
| `selected-test-fixture-header` | component `test-fixture-header` | `test-fixture-interface` |
| `selected-usb-c-receptacle` | component `usb-c-receptacle` | `usb-device-entry` |
| `selected-usb-esd` | component `usb-esd` | `usb-device-protection` |
| `selected-usb-host-socket` | component `teensy-usb-host-socket` | `teensy-usb-host-interface` |
| `selected-usb-power-protection` | component `usb-power-protection` | `usb-power-protection` |
| `teensy-4-1-module` | source `PJRC-TEENSY41-PAGE` | `teensy-module-interface` |

The `cliff-rx`/`ORJ5` and `cliff-tx`/`OTJ5` rows are the selected optical parts;
VKSDR is retained only as circuit evidence and is not represented as a fitted
module. The electrical basis must contain every required support-circuit ID in
this table. P1D3c0 copies the component/source IDs and exact evidence-derived pin
counts into `symbol_bindings`; it does not infer a module abstraction.

The expansion symbols bind one-to-one to their committed component-selection
records and evidence-derived connector pin counts: `selected-expansion-gpio-adc`
to `expansion-gpio-adc`/`BM04B-GHS-TBT`/4,
`selected-expansion-i2c-control` to
`expansion-i2c-control`/`BM04B-GHS-TBT`/4,
`selected-expansion-i2s-tdm` to
`expansion-i2s-tdm`/`BM05B-GHS-TBT`/5, `selected-expansion-spi` to
`expansion-spi`/`BM06B-GHS-TBT`/6, `selected-expansion-uart-debug` to
`expansion-uart-debug`/`BM04B-GHS-TBT`/4, and
`selected-expansion-usb-host` to
`expansion-usb-host`/`BM04B-GHS-TBT`/4. The P1D4 builder verifies each component
ID, selected MPN and pin count against the closed P1C3 selection and P1D1
manufacturer evidence before it may emit the corresponding local symbol.

The exact lexically ordered KiCad-standard inventory is
`Connector:TestPoint`, `Device:C`, `Device:C_Polarized`, `Device:D_TVS`,
`Device:Ferrite_Bead`, `Device:LED`, `Device:Net-Tie_2`, `Device:Polyfuse`,
`Device:R`, `power:+3V3`, `power:+5V`, `power:Earth_Protective`, `power:GND`,
`power:PWR_FLAG`. P1D3c1 and P1D3c2 reject every symbol outside the union and
require every actual symbol/library reference to use the exact spelling. Both
ordered inventories are copied into the immutable stub lock; the schematic
placeholder is never used as an inventory source.

```yaml
phase: P1D3c0
unit: test-surface
seams: [four hierarchical interfaces, local symbol inventory]
contract-artifact: spec-only
fan-out:
  - name: schematic-interface-contract
    owned-paths:
      - electronics/reference/schematic-interface-v1.json
      - electronics/reference/schematic-interface-schema.json
      - scripts/fixtures/p1d-schema/schematic-interface
      - scripts/test_p1d_schematic_interface.py
review-intensity: active
commit-checkpoint:
  green: [python3 scripts/test_p1d_schematic_interface.py]
  buildable: [closed fixture-proven sheet and symbol interface]
  parent-commit: "design(aius): lock reference schematic interfaces"
```

### P1D3c1 — ERC-clean schematic shell

This phase starts after P1D3b and P1D3c0. The root shell and every stub consume
the committed ordered interface and both symbol inventories. The root shell
predeclares every local symbol; sheet agents create interface-only stubs and
never edit the shared symbol library. No `.kicad_pro` is created.

```yaml
phase: P1D3c1
unit: single-file-implementation
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
coordination-owned-paths:
  - scripts/fixtures/p1d-schema/reference-package/schematic-stub-lock-v1.json
coordination: maximum three concurrent; every worker consumes schematic-interface-v1.json and sheet agents do not edit root or library; after all return the parent proves ERC and the unlocked stage before atomically locking the seven file hashes
review-intensity: active
commit-checkpoint:
  green:
    - scripts/kicad.sh sch erc electronics/reference/aius-reference.kicad_sch
    - python3 scripts/build_p1d_reference_contract.py validate --stage schematic-stubs-unlocked
    - python3 scripts/build_p1d_reference_contract.py schematic-interface lock --write
    - python3 scripts/build_p1d_reference_contract.py schematic-interface lock --check
    - python3 scripts/build_p1d_reference_contract.py validate --stage schematic-stubs
  buildable: [interface-complete zero-warning ERC-clean schematic shell]
  parent-commit: "design(aius): establish reference schematic shell"
```

### P1D3c2 — Hierarchical sheet implementation

Four agents replace one committed sheet stub each. They use only predeclared
symbols and preserve every hierarchical pin, so no shared root/library edit is
needed.

```yaml
phase: P1D3c2
unit: single-file-implementation
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
    - python3 scripts/build_p1d_reference_contract.py validate --stage schematic-sheets
  buildable: [complete zero-warning ERC-clean hierarchical sheets]
  parent-commit: "design(aius): implement reference schematic sheets"
```

### P1D3c3 — Schematic contract closure

One agent replaces the exact schematic-contract placeholder after inspecting the
finished KiCad files. It does not edit KiCad sources.

```yaml
phase: P1D3c3
unit: single-file-implementation
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

P1D4 is two serial checkpoints so the cumulative validator and operator-facing
release surface remain separate concerns.

```yaml
phase: P1D4a
unit: test-surface
seams: [complete P1D package, cumulative topology stage]
contract-artifact: spec-only
fan-out:
  - name: p1d-final-validator
    owned-paths:
      - scripts/validate_p1.py
      - scripts/test_validate_p1.py
      - scripts/fixtures/validate-p1/final
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_validate_p1.py
    - python3 scripts/validate_evidence.py
    - python3 scripts/validate_p1.py topology --stage final
  buildable: [fixture-proven cumulative final validator]
  parent-commit: "feat(aius): validate final reference package"
```

```yaml
phase: P1D4b
unit: component-behind-interface
seams: [final validator, deterministic projections, external ERC gate]
contract-artifact: stubs-committed
fan-out:
  - name: p1d-release-wrapper
    owned-paths:
      - scripts/validate_p1d_release.sh
      - scripts/test_validate_p1d_release.py
      - evidence/manifest/README.md
      - evidence/design/p1d-release.json
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_validate_p1d_release.py
    - shellcheck scripts/validate_p1d_release.sh
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
    - scripts/validate_p1d_release.sh
  buildable: [audited non-manufacturing electrical reference package]
  parent-commit: "docs(aius): release electrical reference package"
```

The release wrapper accepts zero arguments and returns usage exit 2 otherwise.
It resolves the Aius root from its own path, so execution is independent of the
caller working directory. It is foreground-only, network-free, uses strict
fail-fast shell semantics, and runs once in exact checkpoint order from
`validate_evidence.py` through the KiCad ERC check. The repository-wide
doc-index check remains an outer documentation proof and is not a release input;
the Python wrapper test, ShellCheck and wrapper invocation are also outer proof
commands and are not called by the wrapper. It stops at the first non-zero
command and propagates that status.
Only after every check passes it atomically writes `p1d-release.json`, with exact
ordered keys `schema_version`, `release_state`, `input_sha256`, `design_record_sha256`,
`schematic_contract_sha256`, `interface_sha256`, `schematic_file_sha256`,
`notes`; values are `1.0.0`,
`eligible`, the closed release-input map below, a lexically keyed object containing the SHA-256 of the three
canonical design records under exact keys `electrical-reference`,
`pin-resource-map`, and `two-layer-placement`, the schematic-contract SHA-256, the
schematic-interface SHA-256, a closed lexically keyed file-hash object, and an
empty note. `schematic_file_sha256` has exactly these Aius-relative keys:
`electronics/reference/aius-reference.kicad_sch`,
`electronics/reference/aius-reference.kicad_sym`,
`electronics/reference/ethernet.kicad_sch`,
`electronics/reference/optical-spdif.kicad_sch`,
`electronics/reference/power-usb.kicad_sch`, and
`electronics/reference/teensy-expansion.kicad_sch`. Each value is the
lowercase SHA-256 of the committed bytes after the successful ERC check. The
wrapper test mutates each file in turn and proves that regeneration changes
only its keyed hash and that every absent, extra, stale or non-lowercase entry
is rejected. H1 verifies this object transitively before projecting its bridge.

`input_sha256` is a closed lexically keyed map from repository-relative path to the
lowercase SHA-256 of current bytes. Its key set is every regular non-symlink file
under these exact directory roots, recursively:
`evidence/manifest`, `evidence/design`
except `evidence/design/p1d-release.json`,
`docs/reference/datasheets.md`,
`evidence/source-snapshots`,
`docs/reference`, `electronics/reference`,
`scripts/fixtures/p1c-schema`,
`scripts/fixtures/p1d-schema`, and
`scripts/fixtures/render-p1`; plus these exact files:
`compose.yaml`,
`scripts/jsonschema_subset.py`,
`scripts/build_p1c_architecture_contracts.py`,
`scripts/build_p1c_selection_policy_contract.py`,
`scripts/build_p1d_reference_contract.py`,
`scripts/kicad.sh`,
`scripts/render_p1.py`,
`scripts/validate_evidence.py`,
`scripts/validate_p1.py`,
`scripts/validate_p1d_release.sh`, `.gitattributes`,
`package.json`, and `pnpm-lock.yaml`. A missing root/file, symlink,
non-regular entry, path escape,
duplicate normalized key, uppercase digest, absent current key, or extra current
key fails release generation. The wrapper hashes hydrated file bytes, not an LFS
pointer obtained through Git. Its test enumerates the computed set, mutates each
member in turn, adds an extra file beneath each recursive root, and proves the
closed map changes or rejects exactly as specified. H1 independently recomputes
this complete map before accepting the P1D release; it never trusts stored hashes
without reading the bound inputs. This deliberately binds all evidence,
lifecycle/request/component aggregates, normative bases, rendered reviews,
schematic inputs, fixtures, validators and locked tool configuration consumed by
the release checks.

The release contains no timestamp or Git
identity, so identical inputs produce identical bytes. Any failed command leaves
the verdict absent: the wrapper removes a prior verdict before its first check
and creates no replacement until the complete sequence passes.
Its Python test uses command shims to prove order, CWD independence, argument
rejection and every fail-fast boundary; an empty or self-recursive wrapper fails.

After the P1D4b worker returns and these checks pass, one clean-room delivery
reviewer receives only this plan and the current code. Missing promises are
fixed; intentional divergence is recorded in this plan's phase notes. The
parent then lands and pushes P1D4b.
