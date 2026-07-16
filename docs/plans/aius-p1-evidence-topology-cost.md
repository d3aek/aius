---
title: Aius P1 evidence topology and cost implementation
type: plans
scope: aius
status: in-progress
tags: [aius, evidence, topology, cost, hardware]
updated: 2026-07-15
toc: true
---

# Aius P1 evidence, topology, and cost implementation

This is the normative implementation contract for capability phase P1 and
hardware phase H0. It replaces the original broad P1/H0 fan-out while retaining
their product topology, evidence policy, and cost boundary.

P1 is complete only after P1A1 through P1A5 and P1B through P1E are committed
and pushed. Completion also completes H0. H1 remains blocked until capability
phase P7 passes.

## Evidence representation

Binary datasheets, drawings, CAD, reference-design archives, saved quotations,
and page captures live under `docs/reference/datasheets.md<source-id>/` and use Git LFS.
Reviewable source snapshots live under `evidence/source-snapshots/<source-id>/`
and remain normal Git. Authored schemas, scripts, Markdown, JSON, CSV, KiCad
sources, and patches remain normal Git.

Retrievable evidence without redistribution permission is inspected but not
committed. Its manifest artifact uses `storage: external-reference`, a null
`path`, the final URL, retrieval time, locally observed byte count and SHA-256
when available, and a normalized observation containing only the facts used.
The validator never claims such evidence is reproducible from the checkout.

Each record in `evidence/manifest/sources.json` has:

- immutable `id`, `class`, `title`, `authority`, `revision`, `purpose`, and
  `claim_scope`;
- `landing_url`, `retrieved_at`, `status`, `supersedes`, and
  `redistribution_note`;
- an `artifacts` array, allowing zero, one, or several artifacts;
- optional normalized `observations` for dynamic or external-reference facts.

Each artifact has `name`, `final_url`, `storage`, `path`, `sha256`, `bytes`,
`media_type`, `retrieval_status`, and `note`. `storage` is `lfs`, `git`, or
`external-reference`. `retrieval_status` is `current`, `superseded`, or
`unavailable`. A revision or distributor fallback receives a new source ID and
uses `supersedes`; an existing ID never changes authority or meaning.

`evidence/manifest/clone-boundary.md` classifies every external design as
official reuse, independently re-derived, comparative-only, or unresolved.
`evidence/manifest/assumptions.json` records unresolved claim, consequence,
owner, validation gate, evidence IDs, and status. These are checkpoint outputs,
not prose hidden in source records.

## Seed source matrix

P1B may acquire only these seed sources. Document links may be resolved from a
fixed landing page, but authority may not be substituted.

| Source ID | Class | Required object | Fixed URL |
|---|---|---|---|
| `PJRC-TEENSY41-PAGE` | golden | product page | `https://www.pjrc.com/store/teensy41.html` |
| `PJRC-TEENSY41-SCHEMATIC` | golden | schematic linked by the product page | `https://www.pjrc.com/store/teensy41.html` |
| `PJRC-TEENSY41-PINOUT` | golden | two-sided pinout revision 4 | `https://www.pjrc.com/teensy/card11b_rev4_web.pdf` |
| `PJRC-IMXRT1060-RM` | golden | annotated reference manual revision 3 | `https://www.pjrc.com/teensy/IMXRT1060RM_rev3_annotations.pdf` |
| `PJRC-IMXRT1060-CEC` | golden | electrical datasheet revision 4 | `https://www.pjrc.com/teensy/IMXRT1060CEC_rev4.pdf` |
| `PJRC-ETHERNET-KIT` | golden | Ethernet kit assembly guide | `https://media.digikey.com/pdf/Data%20Sheets/Sparkfun%20PDFs/Ethernet_Kit_for_Teensy_4.1_Web.pdf` |
| `CLIFF-OTJ5` | golden | FCR684205T version 2.1 datasheet | `https://www.cliffuk.co.uk/products/optical/FCR684205T.pdf` |
| `CLIFF-ORJ5` | golden | FCR684205R version 2.0 datasheet | `https://www.cliffuk.co.uk/products/optical/FCR684205R.pdf` |
| `VKSDR-TOSLINK-SCHEMATICS` | comparative | published schematic archive | `https://vksdr.com/pmod/` |
| `VKSDR-TOSLINK-KICAD` | comparative | published KiCad archive | `https://vksdr.com/pmod/` |
| `HAMMOND-1455N1201BK-DRAWING` | golden | enclosure drawing | `https://www.hammfg.com/files/parts/pdf/1455N1201BK.pdf` |
| `HAMMOND-1455N1201BK-STEP` | golden | STEP file linked by the part page | `https://www.hammfg.com/part/1455N1201BK` |
| `PLATFORMIO-TEENSY41` | golden | board documentation page | `https://docs.platformio.org/en/latest/boards/teensy/teensy41.html` |
| `PJRC-SPDIF-RX-SOURCE` | comparative | pinned upstream source snapshot | `https://github.com/PaulStoffregen/Audio/blob/master/input_spdif3.cpp` |
| `PJRC-SPDIF-TX-SOURCE` | comparative | pinned upstream source snapshot | `https://github.com/PaulStoffregen/Audio/blob/master/output_spdif3.cpp` |
| `FARNELL-1455N1201BK` | dynamic | normalized price and stock observation | `https://uk.farnell.com/hammond/1455n1201bk/box-black-aluminium-end-plate/dp/9287876` |
| `CPC-CLIFF-OTJ5` | dynamic | normalized price and stock observation | `https://cpc.farnell.com/cliff-electronic-components/fcr684205t/optical-jack-transmitter-otj-5/dp/CN20715` |
| `DIGIKEY-CLIFF-ORJ5` | dynamic | normalized price and stock observation | `https://www.digikey.co.uk/en/products/detail/cliff-electronic-components-ltd/FCR684205R/15283094` |
| `DIGIKEY-TEENSY41` | dynamic | normalized optional-module observation | `https://www.digikey.co.uk/en/products/filter/evaluation-boards/embedded-mcu-dsp-evaluation-boards/teensy-4-1/786` |
| `PCBWAY-2L-100` | dynamic | normalized price observation | `https://www.pcbway.com/pcb_prototype/2_Layer_pcb/100x100mm.html` |
| `PCBWAY-4L-100` | dynamic | normalized price observation | `https://www.pcbway.com/pcb_prototype/4_Layer_pcb/100x100mm.html` |
| `BOE-GBP-DAILY-FX` | dynamic | official daily GBP exchange-rate observation | `https://www.bankofengland.co.uk/boeapps/database/Rates.asp?into=GBP` |

Missing golden evidence blocks its dependent claim. Missing comparative evidence
does not block a claim re-derived from golden evidence. Dynamic failures remain
explicitly unavailable and force a low-confidence allowance or a new named UK
authorized-distributor source ID.

## P1A — Evidence contract and validators

P1A is implemented as five independently committed subphases. The original
combined P1A outline later in this section is non-normative.

### P1A1 — Source-record contract

Create exactly:

- `evidence/manifest/source-schema.json`;
- `evidence/manifest/seed-sources.json`;
- `evidence/manifest/sources.json` as an empty array;
- `scripts/jsonschema_subset.py`;
- `scripts/test_source_schema.py`.

The schema uses JSON Schema Draft 2020-12 and `additionalProperties: false` at
every object level. `jsonschema_subset.py` is a standard-library-only evaluator
for the exact committed keyword set: `$schema`, `$id`, `$defs`, `$ref`, `type`,
`required`, `properties`, `additionalProperties`, `enum`, `const`, `pattern`,
`format`, `items`, `minItems`, `maxItems`, `uniqueItems`, `minimum`, `anyOf`,
`allOf`, `contains`, `minContains`, `maxContains`, `if`, `then`, and `else`.
It rejects any unrecognized schema keyword.
`test_source_schema.py` meta-checks the schema with that evaluator and exercises
valid and invalid source/seed examples.

A source record requires `id`, `class`, `title`, `authority`, `revision`,
`purpose`, `claim_scope`, `landing_url`, `retrieved_at`, `status`, `supersedes`,
`redistribution_note`, `artifacts`, and `observations`. `sources.json` is a
top-level array sorted by `id` with unique IDs. IDs match
`^[A-Z0-9]+(?:-[A-Z0-9]+)*$`. `class` is `golden`, `comparative`,
`independently-derived`, or `dynamic`; `status` is `current`, `superseded`, or
`unavailable`. Titles, authorities, revisions, purposes, and redistribution
notes are non-empty strings. `claim_scope` is a non-empty array of unique,
non-empty strings. URLs are HTTPS. Timestamps use UTC RFC 3339 seconds.

An artifact requires `name`, `final_url`, `storage`, `path`, `sha256`, `bytes`,
`media_type`, `retrieval_status`, and `note`. `storage` is `lfs`, `git`, or
`external-reference`; `retrieval_status` is `current`, `superseded`, or
`unavailable`.

- Current `lfs` or `git` artifacts require non-null URL, project-relative path,
  lowercase 64-hex SHA-256, positive byte count, and media type.
- `external-reference` requires a null path. Hash and size are both null or
  both present; no checkout-reproducibility claim is made.
- `unavailable` requires `external-reference`, null path/hash/size/media type,
  and a non-empty note.
- `supersedes` has type string or null. A current record may name one prior ID;
  a superseded or unavailable record requires null. A named prior record must
  exist with `status: superseded`. Chains are acyclic. A superseded source has
  no current artifacts.
- A source has at least one artifact or one observation. An unavailable source
  has an unavailable artifact and no factual observation.

An observation requires `key`, scalar `value`, nullable `unit`, non-empty
`basis`, `observed_at`, and HTTPS `source_url`.

`seed-sources.json` is a top-level array sorted by `id` with unique IDs and
exactly 22 records. Each record contains only the immutable tuple `id`, `class`,
`authority`, and HTTPS `landing_url`, plus non-empty `required_object` and
`purpose`, non-empty unique-string-array `claim_scope`, and Boolean `required`.
`test_source_schema.py` contains the 22 expected immutable tuples copied from
the matrix above and fails on a missing, extra, reordered, or changed tuple.

JSON Schema enforces record-local rules. `test_source_schema.py` also exposes
`validate_source_collection(sources, seeds)` for sortedness, uniqueness,
artifact-name uniqueness, supersession references, and cycle detection. P1A4
imports this function as the reusable cross-record validation entry point; it
does not reimplement those rules.

```yaml
phase: P1A1
status: completed
unit: source-schema
seams: [source records, seed identities]
contract-artifact: both
fan-out:
  - name: source-record-contract
    owned-paths:
      - evidence/manifest/source-schema.json
      - evidence/manifest/seed-sources.json
      - evidence/manifest/sources.json
      - scripts/jsonschema_subset.py
      - scripts/test_source_schema.py
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_source_schema.py
    - the repo tooling tool doc-index -- --check
  buildable: [source record contract]
  parent-commit: "feat(aius): define evidence source schema"
```

Completed 2026-07-15. The focused suite passes 23 tests, including malformed
URI, unbounded-integer, unsatisfiable-bound, conditional, artifact, seed-oracle,
and supersession cases. The clean-room delivery audit passed.

### P1A2 — Request, component, and assumption contracts

Create exactly:

- `evidence/manifest/source-request-schema.json`;
- `evidence/design/component-schema.json`;
- `evidence/manifest/assumption-schema.json`;
- `evidence/manifest/assumptions.json` as an empty array;
- `scripts/test_selection_schemas.py`.

All schemas use Draft 2020-12 and reject additional properties. Every array
called out as non-empty contains unique non-empty strings. Evidence and source
IDs use the P1A1 identifier pattern. Family and block slugs match
`^[a-z0-9]+(?:-[a-z0-9]+)*$`; request IDs match
`^REQ-[A-Z0-9]+(?:-[A-Z0-9]+)*$`; assumption IDs match
`^ASM-[A-Z0-9]+(?:-[A-Z0-9]+)*$`. URLs are HTTPS.

A source request requires `request_id`, `family`, non-empty `requirements`,
`authority_url`, `selected_manufacturer`, `selected_mpn`, `source_id`, non-empty
`artifact_kinds`, `status`, `evidence_ids`, and `notes`. The four selected
fields are non-empty strings except the HTTPS URL. `artifact_kinds` uses only
`datasheet`, `drawing`, `cad`, `product-page`, `price-observation`,
`source-snapshot`, or `reference-design`.

- `pending` means the component and authority are selected but the artifact has
  not been acquired; it requires all selected fields and zero evidence IDs.
- `satisfied` requires all selected fields and at least one evidence ID.
- `unavailable` requires all selected fields, zero evidence IDs, and non-empty
  notes describing the failed acquisition.

A component requires `block`, non-empty `requirements`, nullable `manufacturer`
and `mpn`, `alternatives`, nullable `selection_rationale`, `authority_url`,
`source_request_id`, `generic_policy`, `status`, and string `notes`.
`authority_url`, `source_request_id`, and `generic_policy` are nullable.
`alternatives` has zero to two objects, each requiring non-empty `manufacturer`,
`mpn`, `selection_rationale`, and HTTPS `authority_url`, with no extra fields.

- `selected` requires non-empty manufacturer, MPN, rationale, HTTPS authority,
  and request ID; `generic_policy` is null.
- `generic-allowance` requires null manufacturer, MPN, authority, and request
  ID; non-empty rationale; and a `generic_policy` object with exactly the three
  non-empty strings `value`, `package`, and `tolerance`.
- `unresolved` requires null manufacturer, MPN, authority, request ID, and
  generic policy, plus non-empty rationale explaining what remains unresolved.

An assumption requires `id`, non-empty `claim`, `consequence`, `owner`, and
`validation_gate`, unique `evidence_ids`, `status`, and nullable `resolution`.
Status is `open` or `resolved`. `open` requires null resolution and permits zero
or more evidence IDs. `resolved` requires non-empty resolution and at least one
evidence ID.

`test_selection_schemas.py` meta-checks all three schemas, validates the empty
assumption array, covers every status with a valid record, and rejects invalid
identifiers, empty/duplicate arrays, additional properties, a third
alternative, every missing conditional field, every forbidden non-null field,
and incomplete generic policies.

```yaml
phase: P1A2
status: completed
unit: selection-schemas
seams: [source request, component choice, unresolved assumption]
contract-artifact: both
fan-out:
  - name: selection-contracts
    owned-paths:
      - evidence/manifest/source-request-schema.json
      - evidence/design/component-schema.json
      - evidence/manifest/assumption-schema.json
      - evidence/manifest/assumptions.json
      - scripts/test_selection_schemas.py
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_selection_schemas.py
  buildable: [selection and assumption contracts]
  parent-commit: "feat(aius): define evidence selection schemas"
```

Completed 2026-07-15. The focused suite passes 22 tests and the P1A1 suite
remains green at 23 tests. The clean-room delivery audit passed.

### P1A3 — Evidence policy artifacts

Create exactly `evidence/manifest/README.md`,
`evidence/manifest/clone-boundary.json`, and
`scripts/test_evidence_policy.py`. The clone-boundary file contains one
row per non-dynamic seed source with `source_id`, `classification`,
`reused_material`, `derived_claims`, `prohibited_use`, and `notes`.
Classification is `official-reuse`, `independently-re-derived`,
`comparative-only`, or `unresolved`. The README documents retrieval, temporary
inspection of external references, hashing, LFS placement, supersession,
failure recording, and validator commands.

```yaml
phase: P1A3
unit: evidence-policy
seams: [retrieval policy, clone boundary]
contract-artifact: both
fan-out:
  - name: evidence-policy
    owned-paths:
      - evidence/manifest/README.md
      - evidence/manifest/clone-boundary.json
      - scripts/test_evidence_policy.py
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_evidence_policy.py
    - the repo tooling tool doc-index -- --check
  buildable: [documented evidence policy]
  parent-commit: "docs(aius): define evidence handling policy"
```

### P1A4 — Evidence validator

Create exactly `scripts/validate_evidence.py` and
`scripts/test_validate_evidence.py`. The validator accepts
`--root <checkout>`, optional `--baseline <sources.json>`, and `--self-test`.
Without `--baseline`, it reads the previous committed `sources.json` with
`git show HEAD:<path>` when present. It compares the immutable tuple
`id,class,authority,landing_url`; a changed tuple fails and a new revision needs
a new ID. Seed identities are always compared to `seed-sources.json`.

Validation deterministically checks both the schema and its cross-record rules:

- every required golden seed is current; an unavailable golden source fails;
- unavailable comparative or dynamic sources pass only with an open assumption
  naming their claim scope; independently-derived sources require an evidence ID;
- current checkout artifacts match byte count and SHA-256;
- extension and signature agree for PDF, ZIP, HTML, STEP, KiCad, JSON, CSV, C,
  C++, and plain text; unlisted media types fail until the map is amended;
- `lfs` paths report `filter=lfs`, `git` paths do not, and LFS artifacts are not
  pointer text in the working tree;
- every non-dynamic source has exactly one clone-boundary row and no unknown row
  exists;
- supersession references exist and are acyclic.

The test script creates isolated fixture trees and covers one passing record plus
each failure above, multi-artifact records, external references with and without
local digests, unavailable artifacts, changed immutable identity, and a valid
supersession chain. It exits non-zero if any expected diagnostic is absent.

```yaml
phase: P1A4
unit: evidence-validator
seams: [checkout validation, immutable baseline]
contract-artifact: both
fan-out:
  - name: evidence-validator
    owned-paths:
      - scripts/validate_evidence.py
      - scripts/test_validate_evidence.py
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_validate_evidence.py
    - python3 scripts/validate_evidence.py --self-test
  buildable: [deterministic evidence validator]
  parent-commit: "feat(aius): validate evidence repository"
```

### P1A5 — Reproducible KiCad runner

Create exactly `compose.yaml`, `scripts/kicad.sh`, and
`scripts/test_kicad_wrapper.sh`.

The Compose service uses
`kicad/kicad:10.0.4@sha256:770e2fa006695077b5ba218742d5c3137896af91257b2ca971243fa56b01bb86`,
fixed project name `aius-kicad`, network mode `none`, repository bind mount at
`/workspace`, working directory `/workspace/src/hardware/aius`, and named volume
`aius-kicad-cache` at `/home/kicad/.cache`. The image runs as its built-in
`kicad` UID/GID 1000 user. No container is kept resident.

`kicad.sh` resolves the repository from its own checked-in location, exports the
absolute bind path only for Compose interpolation, and invokes exactly
`docker compose -f <absolute-compose-path> run --rm kicad kicad-cli "$@"`.
It works from any current directory and supports `AIUS_DOCKER_BIN` only for its
wrapper test.

The test uses a fake Docker executable to assert the absolute Compose path,
`run --rm`, service, forwarded arguments, and identical behavior from the repo
root and an unrelated directory. It also parses `docker compose config --format
json` to assert image digest, project name, network, bind target, cache volume,
and working directory.

```yaml
phase: P1A5
unit: kicad-runner
seams: [ephemeral KiCad CLI, persistent cache]
contract-artifact: both
fan-out:
  - name: kicad-runner
    owned-paths:
      - compose.yaml
      - scripts/kicad.sh
      - scripts/test_kicad_wrapper.sh
review-intensity: active
commit-checkpoint:
  green:
    - docker compose -f compose.yaml config --quiet
    - shellcheck scripts/kicad.sh scripts/test_kicad_wrapper.sh
    - scripts/test_kicad_wrapper.sh
    - scripts/kicad.sh version
  buildable: [digest-pinned ephemeral KiCad CLI from any working directory]
  parent-commit: "build(aius): add reproducible KiCad runner"
```

### Original combined P1A outline (superseded)

Create:

- `evidence/manifest/schema.json`, `source-request-schema.json`, and
  `evidence/design/component-schema.json`;
- `evidence/manifest/seed-sources.json` containing the matrix above;
- empty, schema-valid `sources.json` and `assumptions.json`;
- `evidence/manifest/clone-boundary.md` and `README.md`;
- `scripts/validate_p1.py` with `evidence`, `topology`, and `cost` stages;
- validator fixtures proving hash, byte-count, LFS/Git placement, multi-artifact,
  external-reference, immutable-ID, schema, and unavailable-source failures.
- `compose.yaml` using a digest-pinned official `kicad/kicad` image and a named
  cache volume, plus `scripts/kicad.sh`, which always invokes
  `docker compose run --rm` from a checkout-independent working directory.

The evidence stage verifies JSON Schema, unique and sorted IDs, complete seed
coverage, artifact hashes and byte counts, plausible media types, correct
`git check-attr filter`, non-pointer downloaded content, source-snapshot Git
placement, and clone-boundary/assumption coverage.

```yaml
phase: P1A
unit: evidence-contract
seams: [manifest schema, staged validator]
contract-artifact: both
fan-out:
  - name: evidence-contract
    owned-paths:
      - evidence/manifest
      - evidence/design/component-schema.json
      - scripts/validate_p1.py
      - scripts/test
      - compose.yaml
      - scripts/kicad.sh
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_p1.py evidence --self-test
    - scripts/kicad.sh version
    - the repo tooling tool doc-index -- --check
  buildable: [empty schema-valid evidence repository and executable validator]
  parent-commit: "feat(aius): establish evidence validation contract"
```

## P1B — Seed evidence acquisition

Acquisition agents write only their source directories and one record per file
under `evidence/manifest/fragments/`. `manifest-coordinator` runs after all
acquisition agents and alone writes `sources.json`; it does not retrieve or
reinterpret evidence.

```yaml
phase: P1B
unit: bounded-source-family
seams: [manifest fragments, immutable source IDs]
contract-artifact: both
fan-out:
  - name: teensy-board-evidence
    owned-paths:
      - docs/reference/datasheets.mdPJRC-TEENSY41-PAGE
      - docs/reference/datasheets.mdPJRC-TEENSY41-SCHEMATIC
      - docs/reference/datasheets.mdPJRC-TEENSY41-PINOUT
      - evidence/manifest/fragments/PJRC-TEENSY41-PAGE.json
      - evidence/manifest/fragments/PJRC-TEENSY41-SCHEMATIC.json
      - evidence/manifest/fragments/PJRC-TEENSY41-PINOUT.json
  - name: imxrt-evidence
    owned-paths:
      - docs/reference/datasheets.mdPJRC-IMXRT1060-RM
      - docs/reference/datasheets.mdPJRC-IMXRT1060-CEC
      - evidence/manifest/fragments/PJRC-IMXRT1060-RM.json
      - evidence/manifest/fragments/PJRC-IMXRT1060-CEC.json
  - name: ethernet-kit-evidence
    owned-paths:
      - docs/reference/datasheets.mdPJRC-ETHERNET-KIT
      - evidence/manifest/fragments/PJRC-ETHERNET-KIT.json
  - name: cliff-optical-evidence
    owned-paths:
      - docs/reference/datasheets.mdCLIFF-OTJ5
      - docs/reference/datasheets.mdCLIFF-ORJ5
      - evidence/manifest/fragments/CLIFF-OTJ5.json
      - evidence/manifest/fragments/CLIFF-ORJ5.json
  - name: vksdr-comparative-evidence
    owned-paths:
      - docs/reference/datasheets.mdVKSDR-TOSLINK-SCHEMATICS
      - docs/reference/datasheets.mdVKSDR-TOSLINK-KICAD
      - evidence/manifest/fragments/VKSDR-TOSLINK-SCHEMATICS.json
      - evidence/manifest/fragments/VKSDR-TOSLINK-KICAD.json
  - name: mechanical-evidence
    owned-paths:
      - docs/reference/datasheets.mdHAMMOND-1455N1201BK-DRAWING
      - docs/reference/datasheets.mdHAMMOND-1455N1201BK-STEP
      - evidence/manifest/fragments/HAMMOND-1455N1201BK-DRAWING.json
      - evidence/manifest/fragments/HAMMOND-1455N1201BK-STEP.json
  - name: platformio-evidence
    owned-paths:
      - docs/reference/datasheets.mdPLATFORMIO-TEENSY41
      - evidence/manifest/fragments/PLATFORMIO-TEENSY41.json
  - name: source-snapshots
    owned-paths:
      - evidence/source-snapshots/PJRC-SPDIF-RX-SOURCE
      - evidence/source-snapshots/PJRC-SPDIF-TX-SOURCE
      - evidence/manifest/fragments/PJRC-SPDIF-RX-SOURCE.json
      - evidence/manifest/fragments/PJRC-SPDIF-TX-SOURCE.json
  - name: enclosure-price-observation
    owned-paths:
      - evidence/observations/seed/FARNELL-1455N1201BK.json
      - evidence/manifest/fragments/FARNELL-1455N1201BK.json
  - name: optical-price-observations
    owned-paths:
      - evidence/observations/seed/CPC-CLIFF-OTJ5.json
      - evidence/observations/seed/DIGIKEY-CLIFF-ORJ5.json
      - evidence/manifest/fragments/CPC-CLIFF-OTJ5.json
      - evidence/manifest/fragments/DIGIKEY-CLIFF-ORJ5.json
  - name: teensy-price-observation
    owned-paths:
      - evidence/observations/seed/DIGIKEY-TEENSY41.json
      - evidence/manifest/fragments/DIGIKEY-TEENSY41.json
  - name: pcb-price-observations
    owned-paths:
      - evidence/observations/seed/PCBWAY-2L-100.json
      - evidence/observations/seed/PCBWAY-4L-100.json
      - evidence/manifest/fragments/PCBWAY-2L-100.json
      - evidence/manifest/fragments/PCBWAY-4L-100.json
  - name: fx-observation
    owned-paths:
      - evidence/observations/seed/BOE-GBP-DAILY-FX.json
      - evidence/manifest/fragments/BOE-GBP-DAILY-FX.json
  - name: manifest-coordinator
    owned-paths: [evidence/manifest/sources.json]
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_evidence.py
  buildable: [auditable seed evidence checkout]
  parent-commit: "docs(aius): archive seed design evidence"
```

The orchestrator quotes the exact source IDs and disjoint paths above in every
spawn. Agents run in waves of at most three. Source snapshots resolve the PJRC
Audio repository HEAD once, record the commit SHA, and use immutable raw URLs.

## P1C — Functional topology and bounded component selection

Create three independent contract artifacts:

- `docs/reference/functional-topology.md`: source/sink modes, blocks, channel
  flow, data/control/power paths, clock domains, DMA/buffer ownership, and
  latency boundaries;
- `docs/reference/interfaces-and-states.md`: connector, control, expansion,
  power-domain, operating-state, and fault-state inventories with safe outputs;
- one schema-valid record in `evidence/design/components/` and one source request
  in `evidence/manifest/requests/` for each selected electrical component
  family, followed by coordinator-generated
  `evidence/design/provisional-components.csv`;

Selection is bounded to USB-C receptacle, internal USB-A receptacle, internal
cable, USB ESD, 5 V protection/backfeed control, Cliff RX/TX, S/PDIF mux,
transmitter gate/buffer, native-Ethernet magjack, Ethernet termination parts,
socket headers, keyed expansion headers, and mechanical hardware. Each row has
`block`, `requirements`, selected manufacturer and MPN, at most two alternatives,
selection rationale, authority URL, source-request ID, status, and notes.
Unselected safety- or signal-critical families block P1C. Generic passives may
use a stated value/package/tolerance policy and an engineering allowance.

```yaml
phase: P1C
unit: architecture-contract
seams: [functional flow, interface states, component source requests]
contract-artifact: both
fan-out:
  - name: functional-flow
    owned-paths: [docs/reference/functional-topology.md]
  - name: interfaces-states
    owned-paths: [docs/reference/interfaces-and-states.md]
  - name: select-usb-c-receptacle
    owned-paths:
      - evidence/design/components/usb-c-receptacle.json
      - evidence/manifest/requests/REQ-USB-C-RECEPTACLE.json
  - name: select-usb-a-receptacle
    owned-paths:
      - evidence/design/components/usb-a-receptacle.json
      - evidence/manifest/requests/REQ-USB-A-RECEPTACLE.json
  - name: select-internal-usb-cable
    owned-paths:
      - evidence/design/components/internal-usb-cable.json
      - evidence/manifest/requests/REQ-INTERNAL-USB-CABLE.json
  - name: select-usb-esd
    owned-paths:
      - evidence/design/components/usb-esd.json
      - evidence/manifest/requests/REQ-USB-ESD.json
  - name: select-usb-power-protection
    owned-paths:
      - evidence/design/components/usb-power-protection.json
      - evidence/manifest/requests/REQ-USB-POWER-PROTECTION.json
  - name: select-cliff-optics
    owned-paths:
      - evidence/design/components/cliff-optics.json
      - evidence/manifest/requests/REQ-CLIFF-OPTICS.json
  - name: select-spdif-mux
    owned-paths:
      - evidence/design/components/spdif-mux.json
      - evidence/manifest/requests/REQ-SPDIF-MUX.json
  - name: select-spdif-tx-gate
    owned-paths:
      - evidence/design/components/spdif-tx-gate.json
      - evidence/manifest/requests/REQ-SPDIF-TX-GATE.json
  - name: select-ethernet-magjack
    owned-paths:
      - evidence/design/components/ethernet-magjack.json
      - evidence/manifest/requests/REQ-ETHERNET-MAGJACK.json
  - name: select-ethernet-termination
    owned-paths:
      - evidence/design/components/ethernet-termination.json
      - evidence/manifest/requests/REQ-ETHERNET-TERMINATION.json
  - name: select-teensy-sockets
    owned-paths:
      - evidence/design/components/teensy-sockets.json
      - evidence/manifest/requests/REQ-TEENSY-SOCKETS.json
  - name: select-expansion-connectors
    owned-paths:
      - evidence/design/components/expansion-connectors.json
      - evidence/manifest/requests/REQ-EXPANSION-CONNECTORS.json
  - name: select-mechanical-hardware
    owned-paths:
      - evidence/design/components/mechanical-hardware.json
      - evidence/manifest/requests/REQ-MECHANICAL-HARDWARE.json
  - name: component-selection-coordinator
    owned-paths: [evidence/design/provisional-components.csv]
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_p1.py topology --stage selection
    - the repo tooling tool doc-index -- --check
  buildable: [locked functional topology and finite supplemental source queue]
  parent-commit: "docs(aius): lock functional topology and component candidates"
```

## P1D — Supplemental evidence and electrical reference

Component-family acquisition agents archive only the approved source requests
and write disjoint fragments. The manifest coordinator appends validated records
without rewriting seed IDs. Four design agents then produce:

- `docs/reference/electrical-reference.md`, with per-signal voltage, current,
  timing, default state, drive direction, support circuit, and evidence IDs;
- `electronics/reference/aius-reference.kicad_sch` and README, an annotated
  non-manufacturing reference schematic for optical, S/PDIF mux/gate, USB,
  Ethernet, power, Teensy, and expansion boundaries;
- `docs/reference/pin-resource-map.csv` and `.md`;
- `docs/reference/two-layer-placement.md`, including the measured placement
  table and automatic four-layer triggers.

Pin-map CSV fields are `signal_id`, `physical_pin`, `pad_mux`, `peripheral`,
`instance_signal`, `direction`, `voltage_domain`, `dma_request`, `clock_root`,
`shared_bus`, `reservation_owner`, `status`, `evidence_ids`, and `notes`.
Physical pins are unique unless `shared_bus` names the same bus; incompatible
pad muxes, peripheral instances, DMA requests, or clock ownership fail. S/PDIF
RX is pin 15 and TX is pin 14. The native-Ethernet header, Program, pin 13 LED,
USB host, SDIO, one I2C bus, one SPI bus, one UART pair, and one I2S/TDM
interface must remain usable and source-cited.

The placement table records X/Y extents, connector-to-panel setback, route
budget, cable clearance, and ground-corridor status. Pass criteria are:

- board outline at most 100 mm by 100 mm and within the Hammond guide envelope;
- rear connector faces within 5 mm of the rear-panel interior datum;
- USB-C to internal USB-A routed length budget at most 30 mm, top only, no vias;
- Ethernet header to magjack routed length budget at most 50 mm, top only, no vias;
- an uninterrupted bottom-ground corridor under both pair budgets, with no
  planned split, slot, neck, or connector-return discontinuity;
- 100 mm nominal internal cable with at least five cable diameters bend radius
  and 15 mm insertion/removal clearance;
- no connector, Teensy, cable, SD, Program, expansion, or test-access collision.

Failure of any criterion automatically selects four layers for H2; it is not a
reviewer preference. The placement is a measured feasibility artifact, not PCB
manufacturing data.

P1C updates the P1D fan-out in this plan with one exact source ID, artifact
directory, and fragment path per approved request before its checkpoint lands.
Each P1D acquisition agent receives one request only. The P1D clean-room gate
fails if any `<request-id>` placeholder remains; source acquisition and design
work do not start in the same wave.

```yaml
phase: P1D
unit: electrical-reference-component
seams: [supplemental evidence, reference schematic, pin resources, placement]
contract-artifact: both
fan-out:
  - name: component-evidence-<request-id>
    owned-paths:
      - docs/reference/datasheets.md<exact-source-id>
      - evidence/observations/components/<exact-source-id>.json
      - evidence/manifest/fragments/<exact-source-id>.json
  - name: supplemental-manifest-coordinator
    owned-paths: [evidence/manifest/sources.json]
  - name: electrical-tables
    owned-paths: [docs/reference/electrical-reference.md]
  - name: reference-schematic
    owned-paths: [electronics/reference]
  - name: pin-resources
    owned-paths:
      - docs/reference/pin-resource-map.csv
      - docs/reference/pin-resource-map.md
  - name: placement-proof
    owned-paths: [docs/reference/two-layer-placement.md]
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_evidence.py
    - python3 scripts/validate_p1.py topology --stage final
    - scripts/kicad.sh sch erc electronics/reference/aius-reference.kicad_sch
    - the repo tooling tool doc-index -- --check
  buildable: [audited non-manufacturing electrical reference package]
  parent-commit: "docs(aius): establish electrical reference package"
```

## P1E — Functional BOM, should-cost, and risk

`functional-bom.csv` fields are:

`bom_id`, `group`, `fixed_or_recurring`, `customer_supplied`, `optional`,
`manufacturer`, `mpn`, `substitute_policy`, `description`, `quantity_per_unit`,
`batch_quantity`, `supplier`, `supplier_sku`, `supplier_url`, `stock_state`,
`retrieved_at`, `currency`, `fx_rate_to_gbp`, `fx_source_id`, `price_qty_1`,
`price_qty_3`, `price_break_qty`, `price_break_unit`, `moq`, `order_multiple`,
`unit_price_gbp_ex_vat`, `extended_gbp_ex_vat`, `low_gbp_ex_vat`,
`central_gbp_ex_vat`, `high_gbp_ex_vat`, `vat_treatment`, `duty_treatment`,
`freight_treatment`, `scenario_assumptions`, `evidence_ids`,
`artifact_sha256`, `confidence`, `refresh_trigger`, and `notes`.
It contains every functional group listed in the cost model. An electrical or
safety-critical line may not use `UNSELECTED`; generic passives, assembly, and
logistics may use a documented low-confidence engineering allowance.

`should-cost.csv` is tidy, with one estimate per row: `scenario`, `scope`,
`assembled_units`, `group`, `basis_quantity`, `recurring_gbp_ex_vat`,
`fixed_gbp_ex_vat`, `vat_gbp`, `freight_gbp`, `duty_gbp`, `contingency_gbp`,
`landed_gbp`, `evidence_ids`, `confidence`, and `assumptions`. `scenario` is
`low`, `central`, or `high`; `scope` is `batch` or `per_unit`. Landed cost is
the sum of the six monetary inputs on that row. Per-unit fixed, freight, and
duty costs divide the three assembled units, not surplus bare boards.

`risk-register.csv` fields are `risk_id`, `category`, `owner`, `description`,
`cause`, `consequence`, `likelihood_1_5`, `impact_1_5`, `score`, `evidence_ids`,
`mitigation`, `validation_gate`, `mitigation_evidence_ids`,
`residual_likelihood`, `residual_impact`, `residual_score`, and `status`.
Score is likelihood times impact. Residual score is null until mitigation
evidence exists, then equals residual likelihood times residual impact. A score
of 15 or more blocks; 8 through 12 requires mitigation and gate; 6 or less is
tracked. The README defines all five likelihood and impact levels.
The register has at least one row for DTS feasibility, real-time budget,
memory-bank pressure, S/PDIF recovery, USB servicing, two-layer signal
integrity, optical electrical limits, native-Ethernet routing, power/backfeed,
enclosure fit, assembly yield, sourcing, and cost volatility.

All money is GBP. Foreign prices retain original currency and use the Bank of
England rate observed on `retrieved_at`. If no rate exists that day, use the
most recent prior published rate and record its date. Calculations use decimal
arithmetic, round half up to £0.01 per line, then sum rounded lines. VAT is 20%
unless evidence says a line is outside scope. Freight, duty, VAT, and
contingency remain separate. The batch and per-unit low/central/high totals must
reconcile exactly.

```yaml
phase: P1E
unit: commercial-contract
seams: [functional BOM, scenario cost, risk register]
contract-artifact: both
fan-out:
  - name: functional-bom
    owned-paths: [evidence/cost/functional-bom.csv]
  - name: should-cost
    owned-paths: [evidence/cost/should-cost.csv]
  - name: risk-register
    owned-paths: [evidence/cost/risk-register.csv]
  - name: cost-rules
    owned-paths: [evidence/cost/README.md]
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_evidence.py
    - python3 scripts/validate_p1.py topology --stage final
    - python3 scripts/validate_p1.py cost
    - the repo tooling tool doc-index -- --check
    - clean-room pin-by-pin electrical and P1 promise audit
  buildable: [completed hardware H0 architecture and auditable should-cost]
  parent-commit: "docs(aius): complete H0 architecture and should-cost"
```

The P1E delivery audit sees only this contract and the checkout. It checks every
electrical claim against a named source, every P1 output against its schema, all
automatic layer triggers, and the P1-to-H0 handoff before the phase lands.
