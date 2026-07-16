---
title: Aius P1 evidence topology and cost implementation
type: plans
scope: aius
status: in-progress
tags: [aius, evidence, topology, cost, hardware]
updated: 2026-07-16
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
images, and PDF or MHTML page captures live under
`docs/reference/datasheets.md<source-id>/` and use Git LFS. Reviewable HTML and reStructuredText
source snapshots are text snapshots, not page captures.
Reviewable source snapshots live under `evidence/source-snapshots/<source-id>/`
and remain normal Git. Authored schemas, scripts, Markdown, JSON, CSV, KiCad
sources, and patches remain normal Git.

Publicly retrievable evidence may be retained in this private checkout solely
for the user's personal, noncommercial research even when no redistribution
licence is stated. Its `redistribution_note` must say that no permission is
inferred and the bytes must not be republished. Evidence behind access controls
or an express retention prohibition is inspected but not committed; its
manifest artifact uses `storage: external-reference`, a null path, the final
URL, retrieval time, locally observed byte count and SHA-256 when available,
and a normalized observation containing only the facts used. The validator
never claims external-reference evidence is reproducible from the checkout.

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

`evidence/manifest/clone-boundary.json` classifies exactly the 15 non-dynamic
seed sources as official reuse, independently re-derived, comparative-only, or
unresolved. Future non-seed, non-dynamic sources use the same row contract in
`evidence/manifest/supplemental-clone-boundary.json`; P1D creates that file when
the first such source is approved. Together the two files classify every
external design source without weakening the immutable seed oracle.
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
`scripts/test_evidence_policy.py`.

`clone-boundary.json` is a top-level array sorted by `source_id`, with unique
IDs and exactly the 15 non-dynamic seed sources. Each row rejects additional
properties and contains `source_id`, `classification`, `reused_material`,
`derived_claims`, `prohibited_use`, and `notes`. The three list fields contain
unique non-empty strings; `derived_claims` and `prohibited_use` are non-empty,
while `reused_material` may be empty. `notes` is a string. Classification is
`official-reuse`, `independently-re-derived`, `comparative-only`, or
`unresolved`. An `official-reuse` row has non-empty `reused_material`; a
`comparative-only` row has an empty `reused_material` list.

The exact classification oracle is:

- `official-reuse`: `CLIFF-ORJ5`, `CLIFF-OTJ5`,
  `HAMMOND-1455N1201BK-DRAWING`, `HAMMOND-1455N1201BK-STEP`,
  `PJRC-ETHERNET-KIT`, `PJRC-IMXRT1060-CEC`, `PJRC-IMXRT1060-RM`,
  `PJRC-TEENSY41-PAGE`, `PJRC-TEENSY41-PINOUT`,
  `PJRC-TEENSY41-SCHEMATIC`, and `PLATFORMIO-TEENSY41`;
- `comparative-only`: `PJRC-SPDIF-RX-SOURCE`, `PJRC-SPDIF-TX-SOURCE`,
  `VKSDR-TOSLINK-KICAD`, and `VKSDR-TOSLINK-SCHEMATICS`.

No seed source is independently re-derived or unresolved. Those enum values are
reserved for `supplemental-clone-boundary.json` and must not appear in the P1A3
file.

The README has exactly these H2 headings, in order:

1. `Directory boundary`
2. `Retrieval workflow`
3. `External-reference workflow`
4. `Hash and media validation`
5. `Git LFS verification`
6. `Supersession`
7. `Failure recording`
8. `Runnable validation`
9. `Future validation`

The directory section contains these exact machine-checked policy statements:

- `**Binary evidence:** Store redistributable binary evidence under \`docs/reference/datasheets.md<source-id>/\`; Git LFS must report \`filter=lfs\`.`
- `**Reviewable source snapshots:** Store source snapshots under \`evidence/source-snapshots/<source-id>/\` as normal Git text.`
- `**External references:** Do not commit non-redistributable bytes; record \`storage: external-reference\` with a null path.`
- `**Text contracts:** Keep schemas, manifests, observations, scripts, Markdown, CSV, JSON, KiCad sources, and patches in normal Git.`

`Runnable validation` contains exactly the three commands available at P1A3:

```bash
python3 scripts/test_source_schema.py
python3 scripts/test_selection_schemas.py
python3 scripts/test_evidence_policy.py
```

`Future validation` labels
`python3 scripts/validate_evidence.py` as unavailable until
P1A4. The test embeds the clone-boundary schema and classification oracle,
asserts the exact H2 order, statements, and command blocks, and verifies that
the committed seed classes and clone-boundary coverage agree.

```yaml
phase: P1A3
status: completed
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

Completed 2026-07-15. The focused suite passes 12 tests; all 45 prior schema
tests remain green. The clean-room delivery audit passed.

### P1A4 — Evidence validator

P1A4 is two committed tasks. P1A4a migrates the P1A2/P1A3 contracts; P1A4b
implements the validator only after that green baseline is pushed.

#### P1A4a — Validator contract migration

Modify exactly `evidence/manifest/assumption-schema.json`,
`scripts/test_selection_schemas.py`, and `scripts/test_evidence_policy.py`. The
policy test removes fixed array-length
keywords from its reusable clone-boundary row schema and exports
`validate_clone_boundary_rows(boundary)`, which validates row shape, sorted
unique IDs, and classification invariants. The existing
`validate_clone_boundary(boundary, seeds)` calls that helper and still enforces
the exact 15-row seed oracle. The P1A3 test adds passing empty and supplemental
row fixtures plus malformed, reordered, and duplicate supplemental fixtures for
the reusable helper; no supplemental manifest file is created in P1A4a.

Every assumption additionally requires `source_ids` and `claim_scope` arrays.
`source_ids` contains unique P1A1 identifiers; `claim_scope` contains unique
non-empty strings. Both arrays may be empty. An assumption `evidence_ids` entry
must resolve to a source whose status is not `unavailable`; a `source_ids` entry
must resolve to any source. An unavailable comparative or dynamic source passes
only when exactly one open assumption has `source_ids` equal to `[source.id]` and
`claim_scope` exactly equal, including order, to the source's `claim_scope`.
Every current independently-derived source ID must occur in `evidence_ids` of at
least one assumption. These rules define the evidence ID as a source-record ID;
there is no free-text join.

```yaml
phase: P1A4a
status: completed
unit: validator-contract-migration
seams: [structured assumptions, reusable clone-boundary rows]
contract-artifact: both
fan-out:
  - name: validator-contract-migration
    owned-paths:
      - evidence/manifest/assumption-schema.json
      - scripts/test_evidence_policy.py
      - scripts/test_selection_schemas.py
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_source_schema.py
    - python3 scripts/test_selection_schemas.py
    - python3 scripts/test_evidence_policy.py
    - the repo tooling tool doc-index -- --check
  buildable: [machine-checkable validator input contracts]
  parent-commit: "docs(aius): prepare evidence validator contracts"
```

Completed 2026-07-15. The P1A1, P1A2, and P1A3 suites pass 23, 24, and 14
tests respectively. The clean-room delivery audit passed with no discrepancies.

#### P1A4b — Evidence validator implementation

Create exactly `scripts/validate_evidence.py` and
`scripts/test_validate_evidence.py` against the committed P1A4a contracts, then
update `evidence/manifest/README.md` and `scripts/test_evidence_policy.py` in the
same checkpoint so every documented command exists.

The README retains the P1A3 headings. `Runnable validation` contains, in order,
the three P1A3 commands followed by:

```bash
python3 scripts/test_validate_evidence.py
python3 scripts/validate_evidence.py --self-test
```

`Future validation` says that the following repository-content check is
available but expected to report missing required golden sources until P1B:

```bash
python3 scripts/validate_evidence.py
```

The validator accepts `--root <aius-root>`, optional
`--baseline <sources.json>`, and `--self-test`:

- `--root` is the Aius project root containing `evidence/` and `scripts/`; it
  defaults to the parent of the validator's `scripts/` directory and is resolved
  without following a manifest-provided path;
- a relative explicit baseline is resolved beneath `--root`; an absolute
  baseline is accepted; it must be a regular JSON file. An explicit baseline
  always takes precedence over Git;
- without an explicit baseline, the validator finds the enclosing worktree with
  `git -C <aius-root> rev-parse --show-toplevel` and reads
  `HEAD:evidence/manifest/sources.json`. A missing path is an
  empty baseline; any other Git failure is an environment error;
- every baseline ID must remain present with the identical
  `id,class,authority,landing_url` tuple. New IDs are allowed. Seed identities
  are always compared to `seed-sources.json`;
- `--self-test` is mutually exclusive with `--root` and `--baseline`, runs the
  sibling `test_validate_evidence.py` with the active Python interpreter, and
  returns that process's status;
- exit `0` means valid. Exit `1` covers schema and cross-record violations in
  any syntactically valid JSON, including a wrong top-level type, plus
  artifact-content violations. Exit `2` covers only invalid invocation,
  control-file filesystem I/O, control-file UTF-8 decoding or JSON-syntax
  failure, or an unusable Git environment. A control file is a schema, manifest,
  clone-boundary file, assumption file, or explicit baseline. Artifact-payload
  decoding and signature failures are validation failures with exit `1`.
  Validation diagnostics are emitted to stderr as
  `ERROR <code> <subject>: <message>`, sorted by `(code, subject, message)`.

The complete validation-code and subject vocabulary is:

| Code | Validation class | Canonical subject |
| --- | --- | --- |
| `MANIFEST_SCHEMA` | schema meta-check or instance-schema failure, including any syntactically valid baseline shape failure | control-file path relative to `--root`; literal `baseline` for every explicit or Git-loaded baseline |
| `SOURCE_COLLECTION` | imported P1A1 sortedness, uniqueness, artifact-name, or supersession failure | `evidence/manifest/sources.json` |
| `BASELINE` | schema-valid baseline collection invariant, deleted baseline ID, or changed immutable tuple | literal `baseline` for collection-wide failures; source ID for deletion or identity failure |
| `SEED_IDENTITY` | source tuple differs from the matching immutable seed tuple | source ID |
| `REQUIRED_SOURCE` | required golden source is missing or not current | source ID |
| `ASSUMPTION_REFERENCE` | unresolved `source_ids`/`evidence_ids` entry or evidence names an unavailable source | `assumptions[<index>].<field>[<index>]` |
| `SOURCE_ASSUMPTION` | unavailable comparative/dynamic or unresolved supplemental source lacks its exact open assumption | source ID |
| `DERIVED_EVIDENCE` | current independently-derived source is absent from every assumption `evidence_ids` | source ID |
| `CLONE_BOUNDARY` | boundary row schema/order/uniqueness, coverage, overlap, class, or presence failure | boundary path relative to `--root` for file-wide failures; source ID for row/coverage failures |
| `ARTIFACT_PATH` | missing, non-regular, symlinked, escaping, or wrong-directory artifact path | `sources[<source-index>].artifacts[<artifact-index>]` |
| `ARTIFACT_STORAGE` | suffix requires a different `git`/`lfs` storage mode | artifact index subject above |
| `ARTIFACT_BYTES` | byte-count mismatch | artifact index subject above |
| `ARTIFACT_SHA256` | SHA-256 mismatch | artifact index subject above |
| `ARTIFACT_MEDIA_TYPE` | unknown suffix or declared media-type mismatch | artifact index subject above |
| `ARTIFACT_SIGNATURE` | byte/text signature, UTF-8 payload, JSON, or CSV content failure | artifact index subject above |
| `GIT_ATTRIBUTE` | parsed filter value differs from the required value | artifact index subject above |
| `LFS_POINTER` | checked-out LFS artifact is pointer text | artifact index subject above |

Imported `SourceCollectionError.errors` entries each become one
`SOURCE_COLLECTION` diagnostic with the error string unchanged. Imported
clone-boundary `ValidationError` or `AssertionError` becomes one
`CLONE_BOUNDARY` diagnostic for that file; explicit coverage and classification
checks use the affected source ID. The baseline is first validated against the
P1A1 source-collection schema; its syntactically valid shape failures use
`MANIFEST_SCHEMA`/`baseline`. Its imported collection errors use
`BASELINE`/`baseline` with the error string unchanged. All other checks emit one
diagnostic per failed predicate. Exact duplicate `(code, subject, message)`
triples are deduplicated before the required sort; distinct messages for the
same code and subject are retained.

Validation loads and validates `source-schema.json`, `seed-sources.json`,
`sources.json`, `assumption-schema.json`, `assumptions.json`, and
`clone-boundary.json`. It imports `validate_source_collection` from
`test_source_schema.py` and `validate_clone_boundary` from
`test_evidence_policy.py` as the existing cross-record entry points. The seed
clone boundary must cover exactly the 15 non-dynamic seed records.

If there are non-seed, non-dynamic source records, the validator also requires
`supplemental-clone-boundary.json`, validates it with
`validate_clone_boundary_rows`, and requires its IDs to equal exactly that
supplemental source set. The file must be absent or an empty valid array when no
such source exists. A missing file when the supplemental set is non-empty is a
`CLONE_BOUNDARY` validation failure with exit `1`, not a control-file I/O
failure. Supplemental IDs may not overlap seeds. A golden,
comparative, or independently-derived source respectively requires
`official-reuse`, `comparative-only`, or `independently-re-derived`.
`unresolved` is allowed instead only when exactly one open assumption has
`source_ids` equal to `[source.id]` and `claim_scope` exactly equal to the
source's claim scope. Dynamic sources appear in neither boundary file.

Validation also checks:

- every seed with `required: true` and `class: golden` has a matching current
  source; an unavailable required golden source fails;
- the structured assumption and independently-derived evidence joins above;
- every local artifact with `storage` `git` or `lfs` and
  `retrieval_status: current` is a non-symlink regular file. No path component
  may be a symlink, and the resolved path must remain beneath `--root`;
- current local artifacts match their recorded byte count and SHA-256. External
  references are never dereferenced, including when their locally observed
  digest and size are recorded;
- `lfs` artifacts live below `docs/reference/datasheets.md<source-id>/`; `git` artifacts
  live below `evidence/source-snapshots/<source-id>/`;
- `git -C <worktree> check-attr filter -- <worktree-relative-path>` reports the
  exact value `lfs` for `storage: lfs` and `unspecified` for `storage: git`.
  Missing Git, a non-worktree root, an unparseable response, or command failure
  is an environment error. The Git LFS executable is not invoked. An LFS file
  whose bytes start exactly with
  `version https://git-lfs.github.com/spec/v1\n` is a pointer and fails;
- supersession references exist and are acyclic through the imported P1A1
  validator.

Media validation uses the artifact path's case-sensitive lowercase suffix. A
suffix not in this exact table, a different declared media type, or a failed
content check is a validation error:

| Suffix | Media type | Content check |
| --- | --- | --- |
| `.pdf` | `application/pdf` | starts with `%PDF-` |
| `.png` | `image/png` | starts with bytes `89 50 4e 47 0d 0a 1a 0a` |
| `.zip` | `application/zip` | starts with `PK\\x03\\x04`, `PK\\x05\\x06`, or `PK\\x07\\x08` |
| `.html`, `.htm` | `text/html` | UTF-8; after optional BOM and leading ASCII whitespace, first 1024 characters contain case-insensitive `<!doctype html` or `<html` |
| `.mhtml` | `multipart/related` | first 4096 ASCII bytes contain case-insensitive `MIME-Version:` and `Content-Type: multipart/related` headers |
| `.step`, `.stp` | `model/step` | UTF-8; trimmed text starts `ISO-10303-21;` and ends `END-ISO-10303-21;` |
| `.kicad_sch` | `application/x-kicad-schematic` | UTF-8; trimmed text starts `(kicad_sch` |
| `.kicad_pcb` | `application/x-kicad-pcb` | UTF-8; trimmed text starts `(kicad_pcb` |
| `.kicad_sym` | `application/x-kicad-symbol-library` | UTF-8; trimmed text starts `(kicad_symbol_lib` |
| `.kicad_mod` | `application/x-kicad-footprint` | UTF-8; trimmed text starts `(footprint` |
| `.kicad_pro` | `application/json` | UTF-8 JSON object |
| `.json` | `application/json` | valid UTF-8 JSON |
| `.csv` | `text/csv` | UTF-8 parsed by `csv.reader` with at least one non-empty row |
| `.c`, `.h` | `text/x-c` | UTF-8 with no NUL |
| `.cpp`, `.cc`, `.cxx`, `.hpp`, `.hh`, `.hxx` | `text/x-c++` | UTF-8 with no NUL |
| `.txt` | `text/plain` | UTF-8 with no NUL |
| `.md` | `text/markdown` | UTF-8 with no NUL |
| `.rst` | `text/x-rst` | UTF-8 with no NUL |

All UTF-8 checks accept one leading UTF-8 BOM. Binary signatures are byte
prefixes. PDF, PNG, ZIP, MHTML, and STEP require `storage: lfs`; every reviewable
HTML, reStructuredText, text, source, JSON, CSV, and KiCad suffix requires
`storage: git`.

The test script creates isolated temporary Aius trees inside temporary Git
worktrees with explicit baselines and `.gitattributes`. It covers one passing
multi-artifact collection plus every rule and exit class above, external
references with and without local digests, unavailable artifacts, changed and
deleted immutable identities, new IDs, a valid supersession chain, deterministic
diagnostic ordering, default-root and explicit-root behavior, the missing-HEAD
baseline case, seed/supplemental boundary coverage and classification, and
self-test dispatch. The exit matrix includes a syntactically valid wrong
top-level type and requires exit `1`. A diagnostic test asserts the exact code
and subject; the script exits non-zero if an expected diagnostic is absent.

```yaml
phase: P1A4b
status: completed
unit: evidence-validator
seams: [checkout validation, immutable baseline]
contract-artifact: both
fan-out:
  - name: evidence-validator
    owned-paths:
      - evidence/manifest/README.md
      - scripts/validate_evidence.py
      - scripts/test_validate_evidence.py
      - scripts/test_evidence_policy.py
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_source_schema.py
    - python3 scripts/test_selection_schemas.py
    - python3 scripts/test_evidence_policy.py
    - python3 scripts/test_validate_evidence.py
    - python3 scripts/validate_evidence.py --self-test
    - the repo tooling tool doc-index -- --check
  buildable: [deterministic evidence validator]
  parent-commit: "feat(aius): validate evidence repository"
```

Completed 2026-07-15. The four contract suites pass 23, 24, 14, and 30 tests;
`validate_evidence.py --self-test` also passes all 30 validator fixtures. The
default repository check reports only the 11 intentionally unacquired required
golden sources. The clean-room delivery audit passed with no discrepancies.

### P1A5 — Reproducible KiCad runner

Create exactly `compose.yaml`, `scripts/kicad.sh`, and
`scripts/test_kicad_wrapper.sh`.

The Compose service uses
`kicad/kicad:10.0.4@sha256:770e2fa006695077b5ba218742d5c3137896af91257b2ca971243fa56b01bb86`,
fixed project name `aius-kicad`, network mode `none`, repository bind mount at
`/workspace`, working directory `/workspace/src/hardware/aius`, and named volume
`aius-kicad-cache` at `/home/kicad`. The pinned image provides that directory as
UID/GID 1000 mode `0700`; Docker's empty-volume copy initialization therefore
preserves writable ownership while persisting KiCad cache and user state. The
service remains `user: 1000:1000`; no root bootstrap or privilege drop is
permitted. No container is kept resident.

`kicad.sh` resolves the repository from its own checked-in location, exports the
absolute bind path only for Compose interpolation, and invokes exactly
`docker compose --project-name aius-kicad -f <absolute-compose-path> run --rm
kicad kicad-cli "$@"`. The explicit CLI project name defeats any ambient
`COMPOSE_PROJECT_NAME`. It works from any current directory and supports
`AIUS_DOCKER_BIN` only for its wrapper test.

The test uses a fake Docker executable to assert the absolute Compose path,
explicit project-name option, `run --rm`, service, forwarded arguments, and
identical behavior from the repo root and an unrelated directory. It sets a
hostile ambient `COMPOSE_PROJECT_NAME` and proves the exact argv still pins
`aius-kicad`. It also parses `docker compose config --format json` to assert
image digest, project name, network, bind target, `/home/kicad` volume target,
`1000:1000` user, and working directory. The real `kicad.sh version` checkpoint
must exit `0`, print version `10.0.4`, and contain no cache permission warning.

```yaml
phase: P1A5
status: completed
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

Completed 2026-07-15. Compose rendering, ShellCheck, the hostile-environment
wrapper suite, and the real digest-pinned KiCad run pass. The real run reports
`10.0.4` without cache permission warnings and leaves no project container. The
clean-room delivery audit passed with no discrepancies.

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

P1B is complete only after P1B1a through P1B1c, P1B2a through P1B2e, P1B3a
through P1B3c, and P1B4 are committed and pushed. Each acquisition agent owns
one source ID. Every wave is independently checked, audited, committed, and
pushed before the next wave begins; the manifest coordinator runs last and does
not retrieve or reinterpret evidence.

### P1B1a — Acquisition policy and media migration

Modify exactly `evidence/manifest/README.md`, `scripts/test_evidence_policy.py`,
`scripts/validate_evidence.py`, and `scripts/test_validate_evidence.py`.

The README keeps its existing headings and replaces two exact directory-policy
statements with:

- `**Binary evidence:** Store public, directly retrievable binary evidence retained for personal noncommercial research under \`docs/reference/datasheets.md<source-id>/\`; Git LFS must report \`filter=lfs\`.`
- `**External references:** Do not commit bytes behind access controls or an express retention prohibition; record \`storage: external-reference\` with a null path.`

The external-reference workflow states that public retrieval does not infer
permission to republish and requires that limitation in `redistribution_note`.
The validator adds exactly two media rules to the P1A4 table:

- `.png`, `image/png`, `storage: lfs`, with the eight-byte PNG signature
  `89 50 4e 47 0d 0a 1a 0a`;
- `.rst`, `text/x-rst`, `storage: git`, valid UTF-8 with no NUL.

The validator fixtures add valid and invalid cases for both suffixes. All prior
policy statements, commands, diagnostic behavior, and media rules remain exact.

```yaml
phase: P1B1a
status: completed
unit: acquisition-policy-media
seams: [private research retention, evidence media]
contract-artifact: both
fan-out:
  - name: acquisition-policy-media
    owned-paths:
      - evidence/manifest/README.md
      - scripts/test_evidence_policy.py
      - scripts/validate_evidence.py
      - scripts/test_validate_evidence.py
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_source_schema.py
    - python3 scripts/test_selection_schemas.py
    - python3 scripts/test_evidence_policy.py
    - python3 scripts/test_validate_evidence.py
  buildable: [explicit private-research evidence policy and complete seed media map]
  parent-commit: "feat(aius): prepare seed evidence media"
```

Completed 2026-07-15. The four contract suites pass 23, 24, 15, and 30 tests.
The clean-room delivery audit passed with no discrepancies.

### P1B1b — Exact seed realization contract

Create exactly `evidence/manifest/seed-realization-schema.json`,
`evidence/manifest/seed-realization.json`, and
`scripts/test_seed_realization.py`. The schema is Draft 2020-12, rejects extra
properties, and uses the P1A1 identifier, path, URL, SHA, and string contracts.
The realization file is a top-level array sorted by `source_id` with exactly the
22 seed IDs and no duplicates.

Each row requires `source_id`, `mode`, `revision_strategy`, `revision_value`,
`failure_policy`, `retention`, `artifacts`, and `observation_profile`.
`mode` is `artifact`, `snapshot`, or `dynamic`; `failure_policy` is
`golden-block` or `assumption`; retention is exactly
`personal-noncommercial-no-republication` for local evidence and
`normalized-observation-only` for dynamic evidence. An artifact entry requires
`name`, `retrieval_url`, `expected_final_url`, `path`, `storage`, and
`media_type`; name equals the path basename. Artifact paths are exact
project-relative paths and storage is `lfs` or `git`. Revision strategy is
`fixed`, `sha256`, `commit`, or `observation-date`; fixed and commit strategies
require a non-empty value, while sha256 and observation-date require null. A
non-dynamic row has one or more artifacts and null observation profile. A
dynamic row has no artifacts, `revision_strategy: observation-date`, null
revision value, and one of the profile descriptors below. Golden rows use
`golden-block`; comparative and dynamic rows use `assumption`.

An observation profile descriptor has exactly `name` and `fields`. `fields` is
an ordered, non-empty array whose entries have exactly `key`, `value_type`,
`nullable`, `constraint`, `unit`, and `basis`. Keys are unique within the
descriptor. `value_type` is `string`, `integer`, `number`, or `boolean` and
means the corresponding JSON scalar type; Boolean never satisfies integer or
number. `unit` is a non-empty string or null. `basis` is exactly
`P1B <profile-name> normalized field <key> from the fixed seed landing page.`
The constraint has exactly `kind` and `value`, with these machine-readable
semantics:

| Kind | Value | Rule |
| --- | --- | --- |
| `none` | null | any scalar of `value_type` |
| `non-empty` | null | a string containing at least one non-whitespace character |
| `enum` | non-empty array of unique scalars | exact membership, with Boolean distinct from integer/number |
| `minimum` | number | numeric value greater than or equal to `value` |
| `exclusive-minimum` | number | numeric value greater than `value` |
| `const` | scalar | exact equality, with Boolean distinct from integer/number |
| `pattern` | non-empty anchored regular-expression string | string matches the expression; P1B1c uses Python `re.fullmatch` and rejects invalid expressions |

`nullable: true` additionally permits null before the constraint is applied;
`nullable: false` rejects null. The schema validates the descriptor structure
and constraint value shape using only the supported Draft 2020-12 subset. The
realization contract test owns the exact profile oracle and proves that each
table below maps to the descriptor rules. P1B1c consumes these descriptors as
the sole source of field order, type, nullability, constraint, unit, and basis;
it must not carry a second hard-coded profile-rule table.

The exact non-dynamic realization oracle is:

| Source ID | Revision | Exact artifact realization |
| --- | --- | --- |
| `PJRC-TEENSY41-PAGE` | `sha256` | Git `evidence/source-snapshots/PJRC-TEENSY41-PAGE/teensy41.html`, `text/html`, from/final `https://www.pjrc.com/store/teensy41.html` |
| `PJRC-TEENSY41-SCHEMATIC` | `sha256` | LFS `https://www.pjrc.com/teensy/schematic.html`, `image/png`, from/final `https://www.pjrc.com/teensy/schematic41.png` |
| `PJRC-TEENSY41-PINOUT` | fixed `rev4` | LFS `card11a_rev4_web.pdf` from/final `https://www.pjrc.com/teensy/card11a_rev4_web.pdf` and LFS `card11b_rev4_web.pdf` from/final `https://www.pjrc.com/teensy/card11b_rev4_web.pdf`, both `application/pdf` under `https://www.pjrc.com/teensy/pinout.html` |
| `PJRC-IMXRT1060-RM` | fixed `rev3` | LFS `https://www.pjrc.com/teensy/IMXRT1060RM_rev3_annotations.pdf`, `application/pdf`, from/final `https://www.pjrc.com/teensy/IMXRT1060RM_rev3_annotations.pdf` |
| `PJRC-IMXRT1060-CEC` | fixed `rev4` | LFS `https://www.pjrc.com/teensy/IMXRT1060CEC_rev4.pdf`, `application/pdf`, from/final `https://www.pjrc.com/teensy/IMXRT1060CEC_rev4.pdf` |
| `PJRC-ETHERNET-KIT` | `sha256` | LFS `https://www.pjrc.com/store/ethernet_kit.html`, `application/pdf`, from/final `https://media.digikey.com/pdf/Data%20Sheets/Sparkfun%20PDFs/Ethernet_Kit_for_Teensy_4.1_Web.pdf`; note that the document is PJRC-authored but mirror-hosted |
| `CLIFF-ORJ5` | fixed `2.0` | LFS `https://www.cliffuk.co.uk/products/optical/FCR684205R.pdf`, `application/pdf`, from/final `https://www.cliffuk.co.uk/products/optical/FCR684205R.pdf` |
| `CLIFF-OTJ5` | fixed `2.1` | LFS `https://www.cliffuk.co.uk/products/optical/FCR684205T.pdf`, `application/pdf`, from/final `https://www.cliffuk.co.uk/products/optical/FCR684205T.pdf` |
| `HAMMOND-1455N1201BK-DRAWING` | `sha256` | LFS `https://www.hammfg.com/part/1455N1201BK`, `application/pdf`, from/final `https://www.hammfg.com/files/parts/pdf/1455N1201BK.pdf` |
| `HAMMOND-1455N1201BK-STEP` | fixed `archive-v1697661986` | LFS `https://www.hammfg.com/part/1455N1201BK`, `application/zip`, from/final `https://www.hammfg.com/files/parts/stp/1455N1201BK.zip?v=1697661986` |
| `PLATFORMIO-TEENSY41` | commit `54a797fb5440a069d4790d495499552e5a7edb35` | Git `evidence/source-snapshots/PLATFORMIO-TEENSY41/teensy41.rst`, `text/x-rst`, from/final `https://raw.githubusercontent.com/platformio/platformio-docs/54a797fb5440a069d4790d495499552e5a7edb35/boards/teensy/teensy41.rst` |
| `PJRC-SPDIF-RX-SOURCE` | commit `3039be2773e86daf1f381a1e8bdc1e6a55ed11f1` | Git `evidence/source-snapshots/PJRC-SPDIF-RX-SOURCE/input_spdif3.cpp`, `text/x-c++`, from/final `https://raw.githubusercontent.com/PaulStoffregen/Audio/3039be2773e86daf1f381a1e8bdc1e6a55ed11f1/input_spdif3.cpp` |
| `PJRC-SPDIF-TX-SOURCE` | commit `3039be2773e86daf1f381a1e8bdc1e6a55ed11f1` | Git `evidence/source-snapshots/PJRC-SPDIF-TX-SOURCE/output_spdif3.cpp`, `text/x-c++`, from/final `https://raw.githubusercontent.com/PaulStoffregen/Audio/3039be2773e86daf1f381a1e8bdc1e6a55ed11f1/output_spdif3.cpp` |
| `VKSDR-TOSLINK-SCHEMATICS` | `sha256` | LFS `docs/reference/datasheets.mdVKSDR-TOSLINK-SCHEMATICS/toslink_schematics.zip`, `application/zip`, retrieve `https://vksdr.com/content/files/2022/11/toslink_schematics.zip`, expect final `https://storage.ghost.io/c/e5/f8/e5f82b1b-0848-44d9-95b4-b4a61ecb3633/content/files/2022/11/toslink_schematics.zip` |
| `VKSDR-TOSLINK-KICAD` | `sha256` | LFS `docs/reference/datasheets.mdVKSDR-TOSLINK-KICAD/toslink_kicad.zip`, `application/zip`, retrieve `https://vksdr.com/content/files/2022/11/toslink_kicad.zip`, expect final `https://storage.ghost.io/c/e5/f8/e5f82b1b-0848-44d9-95b4-b4a61ecb3633/content/files/2022/11/toslink_kicad.zip` |

The pinout's front-side URL is an explicit same-authority companion linked by
the Teensy 4.1 product page; it is the only artifact not reachable from the
seeded back-side PDF itself. The PJRC Audio commit was resolved once from
`https://github.com/PaulStoffregen/Audio.git` default branch `master`; both
snapshots must record that same commit in `revision`.

The exact dynamic oracle is:

| Source IDs | Observation profile |
| --- | --- |
| `FARNELL-1455N1201BK`, `CPC-CLIFF-OTJ5`, `DIGIKEY-CLIFF-ORJ5`, `DIGIKEY-TEENSY41` | `uk-distributor-price-stock` |
| `PCBWAY-2L-100`, `PCBWAY-4L-100` | `pcbway-prototype-quote` |
| `BOE-GBP-DAILY-FX` | `boe-gbp-fx` |

`uk-distributor-price-stock` observations occur in this exact key order:
`manufacturer_part_number`, `supplier_sku`, `stock_state`, `stock_quantity`,
`price_quantity`, `unit_price_ex_vat`, `minimum_order_quantity`,
`order_multiple`, and `price_includes_vat`, with these exact value/unit rules:

| Key | Value | Unit |
| --- | --- | --- |
| `manufacturer_part_number`, `supplier_sku` | non-empty string | null |
| `stock_state` | `in-stock`, `out-of-stock`, `backorder`, or `unknown` | null |
| `stock_quantity` | non-negative integer or null | `pieces` |
| `price_quantity`, `minimum_order_quantity`, `order_multiple` | positive integer | `pieces` |
| `unit_price_ex_vat` | non-negative number | `GBP` |
| `price_includes_vat` | Boolean | null |

`pcbway-prototype-quote` records exactly `layer_count`, `board_width_mm`,
`board_height_mm`, `quantity`, `material`, `finished_thickness_mm`,
`copper_weight_oz`, `solder_mask`, `silkscreen`, `surface_finish`,
`fabrication_total`, `shipping_included`, `tax_included`, and
`lead_time_days`. Locked inputs are 100 by 100 mm, quantity 5, FR-4, 1.6 mm,
1 oz, green mask, white silkscreen, and lead-free HASL; layer count is 2 or 4
according to source ID. Exact value/unit rules are:

| Key | Value | Unit |
| --- | --- | --- |
| `layer_count` | integer 2 or 4, matching source ID | `layers` |
| `board_width_mm`, `board_height_mm` | number exactly 100 | `mm` |
| `quantity` | integer exactly 5 | `pieces` |
| `material` | string `FR-4` | null |
| `finished_thickness_mm` | number exactly 1.6 | `mm` |
| `copper_weight_oz` | number exactly 1 | `oz/ft^2` |
| `solder_mask` | string `green` | null |
| `silkscreen` | string `white` | null |
| `surface_finish` | string `lead-free HASL` | null |
| `fabrication_total` | non-negative number | `USD` |
| `shipping_included`, `tax_included` | Boolean | null |
| `lead_time_days` | positive integer or null | `days` |

`boe-gbp-fx` records exactly `rate_date`, `gbp_per_usd`, and `gbp_per_eur`.
`rate_date` is a non-null string with null unit and exact pattern
`^[0-9]{4}-(?:0[1-9]|1[0-2])-(?:0[1-9]|[12][0-9]|3[01])$`. Rates are positive
numbers with units `GBP/USD` and `GBP/EUR` respectively.

The values file is a top-level scalar map with exactly the descriptor keys; it
never contains observation metadata. `record` requires one
`--observed-at YYYY-MM-DDTHH:MM:SSZ` argument. Every generated observation uses
that timestamp, the immutable seed `landing_url` as `source_url`, and the
descriptor's exact basis. Source `retrieved_at` equals the shared timestamp and
revision uses its UTC date. The P1B1b test embeds the 22-row oracle, exact
artifact tuples, revision values, profile mapping, and every descriptor
key/type/nullability/constraint/unit/basis rule. Generated observation value,
timestamp, source-URL, and descriptor-consumption assertions belong to
`scripts/test_seed_evidence.py` after the P1B1c generator exists.

The P1B1b clean-room gate first returned `BLOCKED` on 2026-07-15 because a
profile name alone left P1B1c without machine-readable field rules and the
contract assigned generator assertions before that generator existed. The
descriptor contract and test ownership above resolve both findings; a fresh
clean-room verdict remains required before implementation. Implementation then
found that the original descriptor vocabulary could not encode the required
`rate_date` shape without duplicating policy in P1B1c. The `pattern` constraint
above resolves that gap and requires another fresh clean-room verdict.

```yaml
phase: P1B1b
status: completed
unit: seed-realization-contract
seams: [seed identity, artifact realization, dynamic observation profile]
contract-artifact: both
fan-out:
  - name: seed-realization-contract
    owned-paths:
      - evidence/manifest/seed-realization-schema.json
      - evidence/manifest/seed-realization.json
      - scripts/test_seed_realization.py
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_seed_realization.py
  buildable: [machine-checkable realization for every seed source]
  parent-commit: "feat(aius): define seed evidence realization"
```

Completed 2026-07-15. The exact realization checkpoint passes 13 tests and the
existing source/seed schema suite passes 23 tests. The clean-room delivery audit
passed with no contract discrepancies. Full evidence validation remains
intentionally red on missing golden sources until P1B acquisition populates the
source fragments and aggregate manifest.

### P1B1c — Reproducible acquisition and assembly

Create `scripts/seed_evidence.py`, `scripts/test_seed_evidence.py`, and
`scripts/fixtures/seed-renderer.json`, and extend
`scripts/validate_evidence.py` plus `scripts/test_validate_evidence.py` with the
public artifact-payload seam defined below. The Python acquisition, validation,
network, and transaction logic uses only the standard library; canonical JSON
rendering deliberately uses the repository's pinned Prettier 3.8.3 executable
as the single formatting authority. The CLI accepts `--root` with the same
default and resolution semantics as `validate_evidence.py` and provides:

- `acquire <source-id> --retrieved-at <UTC>` for non-dynamic rows: retrieve only
  the realization URLs, enforce the redirect contract below, stage every exact
  artifact, calculate bytes/SHA-256, require media and Git placement through the
  public validator seam, and transactionally publish the artifacts plus one
  schema-valid source object at
  `evidence/manifest/fragments/<ID>.json`;
- `record <source-id> --values <json> --observed-at <UTC>` for dynamic rows:
  accept exactly the scalar-map descriptor keys and types, generate every unit,
  basis, timestamp, and source URL from the descriptor and seed, embed
  observations in the source fragment, and never create a standalone
  observation file;
- `unavailable <source-id> --note <text> --retrieved-at <UTC>`: write an
  unavailable source fragment. For comparative/dynamic sources it also writes
  exactly one open assumption to
  `evidence/manifest/assumption-fragments/<ID>.json`, with ID
  `ASM-UNAVAILABLE-<ID>`, exact source ID and claim scope, no evidence IDs, owner
  `P1B`, and validation gate `Replace unavailable seed evidence`. Golden use
  writes the fragment but exits `3`, which blocks the wave and all later phases;
- `check-fragment <ID>...`: validate each named source object, immutable seed
  tuple, realization, local bytes/media/Git placement, observation descriptor,
  and required failure assumption without requiring other fragments;
- `assemble --write`: require exactly the 22 fragment IDs and no extras, reject
  duplicate record IDs, validate every fragment, require exactly the permitted
  assumption fragments and no extras, sort both arrays by ID, and
  transactionally render `sources.json` and `assumptions.json`;
- `assemble --check`: render in memory and require byte equality with both
  committed aggregate files.

All timestamp arguments must be real calendar times in exact
`YYYY-MM-DDTHH:MM:SSZ` form. The supplied timestamp is the source
`retrieved_at`; no command consults the wall clock. Fixed revisions are exactly
the realization value; commit revisions are
`commit:<lowercase-realization-value>`; `sha256` revisions are
`sha256:<artifact-sha256>` for one artifact or
`sha256:<sha256-of-concatenated-lowercase-artifact-digests>` for several, with
digests concatenated in realization artifact order; dynamic revisions are
`observed:<YYYY-MM-DD>`. Every generated source copies `id`, `class`,
`authority`, `landing_url`, `purpose`, and `claim_scope` from its seed, uses the
seed `required_object` as title, and has null `supersedes`. Acquired and recorded
sources are current. Acquired artifacts remain in realization order and have no
observations; recorded sources have no artifacts and observations in descriptor
order. Every acquired artifact note and its source `redistribution_note` use
exact text
`Retained for personal noncommercial research; no republication permission is inferred.`
A recorded dynamic source uses exact redistribution note
`Normalized observation only; no source page is retained.` An unavailable
source uses exact redistribution note
`No local copy is retained; no republication permission is inferred.`

The acquisition command sends user agent `Aius-evidence/1.0`, uses a 30-second
open/read timeout, requests identity encoding, permits at most five redirects,
and accepts redirect status only 301, 302, 303, 307, or 308. The initial URL,
every redirect target, and the response final URL must be HTTPS. The terminal
status must be 200, `Content-Encoding` must be absent or `identity`, and the
payload must not exceed 256 MiB by header or streamed bytes. The response
`geturl()` string must equal `expected_final_url` byte for byte; there is no URL
normalization, environment override, authority fallback, credential prompt, or
proxy-specific alternate URL.

Each source fragment is one source-record object, not a one-element array. Each
assumption fragment is one assumption-record object. Rendering first creates
strict compact UTF-8 JSON with sorted object keys, `ensure_ascii=False`, and
`allow_nan=False`, then sends those bytes to the exact worktree
`node_modules/.bin/prettier --parser json --config <git-root>/.prettierrc.json`
process. Production runs
`git -C <resolved-aius-root> rev-parse --show-toplevel`, requires exactly one
absolute UTF-8 path resolving to a directory, and selects only
`<git-root>/node_modules/.bin/prettier` and `<git-root>/.prettierrc.json`.
Missing, unusable, or wrong-root controls fail as exit `2` before publish and
are covered with isolated-Git regressions. Production requires Prettier version
exactly `3.8.3`, uses a 30-second subprocess timeout, accepts only exit `0`,
requires UTF-8 stdout containing the same JSON value, one terminal LF and no
other trailing bytes, and treats missing/wrong-version/timeout/nonzero/invalid
output as control exit `2` before any publish. Object-key sorting therefore
remains deterministic while Prettier alone owns width, escaping, Unicode,
numeric, and nested-shape layout.

The render function accepts a formatter dependency. Broad isolated CLI tests
use an in-process deterministic test formatter; a dedicated production test
uses the real pinned executable and persistent
`scripts/fixtures/seed-renderer.json`. The fixture contains exact 119, 120, and
121-column cases; quotes, backslashes, controls, BMP and non-BMP Unicode, wide
characters; integers, decimals, exponents, and negative zero; empty arrays and
objects, scalar arrays, object arrays, nested arrays, and representative source,
observation, assumption, and top-level aggregate shapes. The test requires byte
equality among the committed fixture, production rendering of its parsed value,
and Prettier output, then rerenders the result to prove idempotence. Every
present regular `evidence/manifest/fragments/*.json` file, whether tracked,
staged, or untracked, is also required to equal production rendering of its
parsed value. The checkpoint runs the pinned Prettier `--check` command on the
fixture and every present fragment. Input JSON rejects `NaN`, `Infinity`, and
`-Infinity`. `record` validates values solely by walking its realization
descriptor in order; it has no profile-name branch or second field-rule table.

Before reading or writing, every path is lexically confined beneath the
resolved Aius root. An explicit `--root` final path component is checked with
`lstat` before resolution and is rejected if it is a symlink. Every existing
component beneath the root is checked with `lstat`; symlinks are always
rejected, intermediate components must be directories, and
input/aggregate files must be regular files. Per-source commands refuse to
overwrite any artifact, source fragment, or assumption fragment, including a
symlink or non-regular entry. Fragment-directory scans inspect every direct
entry: `fragments/` must exist and contain exactly the 22 regular, non-symlink
`<ID>.json` files; a missing `assumption-fragments/` means empty, otherwise every
direct entry must be a regular, non-symlink `<ID>.json` permitted by an
unavailable comparative/dynamic source. Hidden, temporary, nested, and other
entries are errors. Each fragment object's `id` must match its filename ID;
swapping otherwise-valid source objects between filenames is an error.

Writes stage unique adjacent temporary regular files with exclusive creation,
flush and `fsync` their contents, re-check parent/target safety, publish with
`os.replace`, `fsync` the affected directories, and remove staged files on any
handled failure. Acquisition stages every artifact and its fragment before any
publish; unavailable comparative/dynamic handling stages source and assumption
together. A handled publish failure rolls back all outputs from that command,
leaving no new target. `assemble --write` is allowed to replace only the two
regular aggregate files; a handled failure restores both prior byte streams (or
their prior absence) and removes backups/temps. These guarantees are
process-failure transactional, not power-loss atomic across multiple files.
Tests inject failures at each publish boundary and require rollback plus cleanup.
A cleanup or rollback failure is itself reported as a transaction failure and is
never silently discarded.

`validate_evidence.py` exposes
`validate_artifact_payload(root, relative_path, payload, media_type, storage)`.
`payload` is bytes when content is readable and `None` when the aggregate reader
cannot supply content. The function always performs the existing
suffix/media/storage and Git-attribute checks against the final
project-relative path; with bytes it additionally performs the LFS-pointer,
signature, and UTF-8/content checks. It returns a sorted tuple of `Diagnostic`
without printing or requiring aggregate sources and raises `ControlError` only
for an unusable root/Git environment. The aggregate validator, including its
unreadable-payload branch, and the new fragment adapter both use this public
function; neither duplicates the media table, storage, Git-attribute, or
signature rules. P1B1c first
validates source/assumption objects with the existing schemas and collection
helper, then applies immutable seed, realization, descriptor, secure local-byte,
hash/size, and public artifact-payload checks.

The internal retrieval function accepts an opener dependency whose `open` does
not auto-follow redirects. Production constructs a standard-library opener with
automatic redirects and environment proxy discovery disabled; the retriever
resolves each `Location` with `urljoin`, validates it, and performs the next
request itself. No environment value can override URLs or expected redirects.
Test-only transaction helpers also accept a replace-function dependency so
every rollback edge is deterministically reachable.

An unavailable fragment has `revision: unavailable:<YYYY-MM-DD>` from the
required timestamp, source status `unavailable`, no observations, and exactly
one artifact named `unavailable` with the seed landing URL,
`storage: external-reference`, null path/hash/bytes/media type, retrieval status
`unavailable`, and the non-whitespace CLI note. The generated assumption claim
is `Seed source <ID> is unavailable for: <claim_scope joined by "; ">`;
consequence is
`Dependent P1 claims remain provisional until the source is replaced.`; status
is `open` and resolution is null.

Exit status is `0` for a silent success, `1` for acquisition, record,
validation, transaction, aggregate-drift, or unavailable comparative/dynamic
failure, `2` for argparse usage, invalid control/schema/root/Git state, or an
unexpected internal exception, and `3` only after a golden unavailable fragment
has been durably written. Errors go to stderr with one deterministic `ERROR:`
line per failure and no traceback; data-validation diagnostics are sorted.

Tests use an in-process fake HTTPS opener with deterministic redirect responses
and isolated Git trees to cover all CLI commands, exact timestamps and exit
codes, HTTPS/status/redirect/encoding/size limits, rollback at every publish
boundary, target and ancestor symlinks, non-regular and temporary entries, exact
final-URL rejection, streamed-read failure as acquisition exit `1`, artifact and
snapshot storage, fixed/hash/commit revisions, filename/object identity,
descriptor-only validation of every dynamic profile, invalid JSON constants,
golden exit `3`, comparative/dynamic assumptions, missing/extra/duplicate
fragments, canonical sorting/rendering, and check drift. The validator test
covers both public artifact-payload callers, null-payload behavior, and parity
with aggregate media diagnostics. Transaction tests inject at least one real
rollback-operation failure, not only a publish or cleanup failure, and require
its deterministic transaction diagnostic. The golden-block assembly regression
provides all 22 valid fragment filenames so execution reaches the dedicated
golden-unavailable branch before failing. Separate regressions cover unavailable
comparative and dynamic assumptions; a 21-of-22 regular fragment directory with
no other invalid entry reaches the missing-ID diagnostic; and a complete
aggregate containing at least two unavailable assumption-policy sources proves
non-empty assumption validation, ID sorting, canonical rendering, plus isolated
missing and extra assumption-fragment diagnostics.

The P1B1c clean-room gate returned `BLOCKED` on 2026-07-15 because retrieval
time, revision rendering, transactional boundaries, symlink safety, redirect
limits, fragment rendering, isolated artifact validation, and exit behavior were
not complete contracts. The rules above resolve those findings; a fresh
clean-room verdict remains required before implementation. The first delivery
audit then returned `BLOCKED` because streamed response-read failures escaped as
internal exit `2`; the explicit regression above closes that classification
gap. The second delivery audit returned `BLOCKED` because the aggregate
unreadable branch still duplicated validator rules, rollback-failure reporting
was not directly exercised, and the golden-block test failed earlier on missing
fragments. The nullable-payload seam and reachability requirements above close
those findings. The third delivery audit returned `BLOCKED` because comparative
unavailability, an isolated missing-fragment set, and non-empty assumption
aggregation were not actually exercised, while a symlink supplied directly as
`--root` was resolved before inspection. The explicit root check and separate
reachable regressions above close those findings.

```yaml
phase: P1B1c
status: completed
unit: seed-evidence-orchestration
seams: [authority retrieval, source fragment, assumption fragment, aggregate]
contract-artifact: both
fan-out:
  - name: seed-evidence-orchestration
    owned-paths:
      - scripts/seed_evidence.py
      - scripts/test_seed_evidence.py
      - scripts/fixtures/seed-renderer.json
      - scripts/validate_evidence.py
      - scripts/test_validate_evidence.py
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_seed_evidence.py
    - python3 scripts/test_seed_realization.py
    - python3 scripts/test_validate_evidence.py
    - pnpm exec prettier --check scripts/fixtures/seed-renderer.json "evidence/manifest/fragments/*.json"
  buildable: [reproducible per-source acquisition and deterministic aggregation]
  parent-commit: "feat(aius): orchestrate seed evidence acquisition"
```

Completed 2026-07-15. The orchestration, realization, and validator checkpoints
pass 28, 13, and 34 tests. Successive delivery audits exposed and closed normal
stream-read classification, fragment identity, cleanup/rollback proof,
nullable-payload validator reuse, golden-block reachability, assumption
aggregation, missing-fragment isolation, and root-symlink gaps. The final
clean-room acceptance audit passed with no contract discrepancies.

Reopened 2026-07-16 when the first real fragments showed that Python's default
indented scalar-array layout fails the repository's mandatory Prettier hook.
P1B2a evidence remains uncommitted until the renderer and its exact-byte
regressions satisfy both the Python orchestration and repository-format contracts.
The attempted custom-printer contract was rejected by its clean-room gate
because it would duplicate Prettier's width, Unicode, numeric, and nested-shape
semantics. The pinned subprocess seam above makes Prettier the only printer.

Renderer hardening completed 2026-07-16. The orchestration/renderer,
realization, and validator suites pass 32, 13, and 34 tests; the pinned Prettier
checkpoint passes the persistent fixture and every present P1B2a fragment. The
clean-room renderer delivery audit passed with no discrepancies.

### P1B2 — Non-dynamic acquisition waves

Immediately before each P1B2 wave spawns, the coordinator captures one actual
UTC-seconds timestamp and passes the exact same value to every agent in that
wave. Each agent runs
`seed_evidence.py acquire <ID> --retrieved-at <wave-UTC>` and then
`seed_evidence.py check-fragment <ID>`; agents never consult or choose a wall
clock. The phase completion note records the shared timestamp.

Agents write only the exact realization directory and fragment below.
Comparative agents additionally own their assumption-fragment path solely for
the `unavailable` branch. If `acquire` exits `1`, the same agent records the
failure with
`seed_evidence.py unavailable <ID> --note "Automated acquisition from the fixed realization URL failed during <phase>." --retrieved-at <wave-UTC>`,
accepts the policy exit `1` for comparative or `3` for golden, and runs
`check-fragment`. It does not retry through a different URL, user agent, tool,
or authority. A golden failure is recorded, audited, committed, and stops P1B;
it is never substituted. Any other exit is a control defect and blocks the wave
without writing an unavailable claim.

```yaml
phase: P1B2a
status: completed
unit: single-seed-source
seams: [public authority, validated fragment]
contract-artifact: both
fan-out:
  - name: acquire-pjrc-teensy41-page
    owned-paths: [evidence/source-snapshots/PJRC-TEENSY41-PAGE, evidence/manifest/fragments/PJRC-TEENSY41-PAGE.json]
  - name: acquire-pjrc-teensy41-schematic
    owned-paths: [docs/reference/datasheets.mdPJRC-TEENSY41-SCHEMATIC, evidence/manifest/fragments/PJRC-TEENSY41-SCHEMATIC.json]
  - name: acquire-pjrc-teensy41-pinout
    owned-paths: [docs/reference/datasheets.mdPJRC-TEENSY41-PINOUT, evidence/manifest/fragments/PJRC-TEENSY41-PINOUT.json]
review-intensity: active
commit-checkpoint:
  green: [python3 scripts/seed_evidence.py check-fragment PJRC-TEENSY41-PAGE PJRC-TEENSY41-SCHEMATIC PJRC-TEENSY41-PINOUT]
  buildable: [validated Teensy board evidence fragments]
  parent-commit: "docs(aius): archive Teensy board evidence"
```

Completed 2026-07-16 with shared retrieval timestamp
`2026-07-16T04:46:15Z`. The product-page HTML, schematic PNG, and both revision
4 pinout PDFs were acquired from their exact realization URLs as current golden
evidence. The combined fragment checkpoint and clean-room delivery audit passed;
Git/LFS placement and pinned-Prettier byte parity are correct.

```yaml
phase: P1B2b
status: completed
unit: single-seed-source
seams: [public authority, validated fragment]
contract-artifact: both
fan-out:
  - name: acquire-pjrc-imxrt-rm
    owned-paths: [docs/reference/datasheets.mdPJRC-IMXRT1060-RM, evidence/manifest/fragments/PJRC-IMXRT1060-RM.json]
  - name: acquire-pjrc-imxrt-cec
    owned-paths: [docs/reference/datasheets.mdPJRC-IMXRT1060-CEC, evidence/manifest/fragments/PJRC-IMXRT1060-CEC.json]
  - name: acquire-pjrc-ethernet-kit
    owned-paths: [docs/reference/datasheets.mdPJRC-ETHERNET-KIT, evidence/manifest/fragments/PJRC-ETHERNET-KIT.json]
review-intensity: active
commit-checkpoint:
  green: [python3 scripts/seed_evidence.py check-fragment PJRC-IMXRT1060-RM PJRC-IMXRT1060-CEC PJRC-ETHERNET-KIT]
  buildable: [validated processor and Ethernet evidence fragments]
  parent-commit: "docs(aius): archive processor and Ethernet evidence"
```

Completed 2026-07-16 with shared retrieval timestamp
`2026-07-16T05:28:48Z`. The revision 3 reference manual, revision 4 electrical
datasheet, and PJRC-authored DigiKey-mirror Ethernet guide were acquired as
current golden evidence. The combined checkpoint, pinned-Prettier parity, LFS
placement, and clean-room delivery audit passed.

```yaml
phase: P1B2c
status: completed
unit: single-seed-source
seams: [public authority, validated fragment]
contract-artifact: both
fan-out:
  - name: acquire-cliff-orj5
    owned-paths: [docs/reference/datasheets.mdCLIFF-ORJ5, evidence/manifest/fragments/CLIFF-ORJ5.json]
  - name: acquire-cliff-otj5
    owned-paths: [docs/reference/datasheets.mdCLIFF-OTJ5, evidence/manifest/fragments/CLIFF-OTJ5.json]
  - name: acquire-hammond-drawing
    owned-paths: [docs/reference/datasheets.mdHAMMOND-1455N1201BK-DRAWING, evidence/manifest/fragments/HAMMOND-1455N1201BK-DRAWING.json]
review-intensity: active
commit-checkpoint:
  green: [python3 scripts/seed_evidence.py check-fragment CLIFF-ORJ5 CLIFF-OTJ5 HAMMOND-1455N1201BK-DRAWING]
  buildable: [validated optical and enclosure-drawing fragments]
  parent-commit: "docs(aius): archive optical and enclosure evidence"
```

Completed 2026-07-16 with shared retrieval timestamp
`2026-07-16T05:36:51Z`. Cliff receiver v2.0, transmitter v2.1, and the Hammond
enclosure drawing were acquired as current golden PDF evidence. The combined
checkpoint, pinned-Prettier parity, LFS placement, and clean-room delivery audit
passed.

```yaml
phase: P1B2d
status: completed
unit: single-seed-source
seams: [public authority, validated fragment]
contract-artifact: both
fan-out:
  - name: acquire-hammond-step
    owned-paths: [docs/reference/datasheets.mdHAMMOND-1455N1201BK-STEP, evidence/manifest/fragments/HAMMOND-1455N1201BK-STEP.json]
  - name: acquire-platformio-teensy41
    owned-paths: [evidence/source-snapshots/PLATFORMIO-TEENSY41, evidence/manifest/fragments/PLATFORMIO-TEENSY41.json]
  - name: acquire-pjrc-spdif-rx
    owned-paths: [evidence/source-snapshots/PJRC-SPDIF-RX-SOURCE, evidence/manifest/fragments/PJRC-SPDIF-RX-SOURCE.json, evidence/manifest/assumption-fragments/PJRC-SPDIF-RX-SOURCE.json]
review-intensity: active
commit-checkpoint:
  green: [python3 scripts/seed_evidence.py check-fragment HAMMOND-1455N1201BK-STEP PLATFORMIO-TEENSY41 PJRC-SPDIF-RX-SOURCE]
  buildable: [validated mechanical, PlatformIO, and receive-source fragments]
  parent-commit: "docs(aius): archive mechanical and software evidence"
```

Completed 2026-07-16 with shared retrieval timestamp
`2026-07-16T05:45:03Z`. The Hammond STEP archive, pinned PlatformIO Teensy 4.1
board definition, and pinned PJRC S/PDIF receive source were acquired as current
evidence. The combined checkpoint, pinned-Prettier parity, storage placement,
artifact integrity, and clean-room delivery audit passed.

```yaml
phase: P1B2e
status: completed
unit: single-seed-source
seams: [public authority, validated fragment]
contract-artifact: both
fan-out:
  - name: acquire-pjrc-spdif-tx
    owned-paths: [evidence/source-snapshots/PJRC-SPDIF-TX-SOURCE, evidence/manifest/fragments/PJRC-SPDIF-TX-SOURCE.json, evidence/manifest/assumption-fragments/PJRC-SPDIF-TX-SOURCE.json]
  - name: acquire-vksdr-schematics
    owned-paths: [docs/reference/datasheets.mdVKSDR-TOSLINK-SCHEMATICS, evidence/manifest/fragments/VKSDR-TOSLINK-SCHEMATICS.json, evidence/manifest/assumption-fragments/VKSDR-TOSLINK-SCHEMATICS.json]
  - name: acquire-vksdr-kicad
    owned-paths: [docs/reference/datasheets.mdVKSDR-TOSLINK-KICAD, evidence/manifest/fragments/VKSDR-TOSLINK-KICAD.json, evidence/manifest/assumption-fragments/VKSDR-TOSLINK-KICAD.json]
review-intensity: active
commit-checkpoint:
  green: [python3 scripts/seed_evidence.py check-fragment PJRC-SPDIF-TX-SOURCE VKSDR-TOSLINK-SCHEMATICS VKSDR-TOSLINK-KICAD]
  buildable: [validated transmit-source and comparative optical fragments]
  parent-commit: "docs(aius): archive comparative optical evidence"
```

Completed 2026-07-16 with shared retrieval timestamp
`2026-07-16T05:57:35Z`. The pinned PJRC S/PDIF transmit source and both VKSDR
comparative optical archives were acquired as current evidence. The combined
checkpoint, pinned-Prettier parity, source and archive integrity, LFS placement,
and clean-room delivery audit passed.

### P1B3 — Dynamic observation waves

Each agent reads only its fixed landing page, creates a scratch scalar-map JSON
with the exact profile fields, runs `seed_evidence.py record --observed-at` with
one explicit UTC timestamp, and then runs `check-fragment`. If the page cannot
yield the locked facts, it runs
`unavailable`; the generated open assumption is committed with the fragment.
No standalone observation file is created.

```yaml
phase: P1B3a
status: completed
unit: single-dynamic-source
seams: [live page, normalized observations]
contract-artifact: both
fan-out:
  - name: observe-farnell-enclosure
    owned-paths: [evidence/manifest/fragments/FARNELL-1455N1201BK.json, evidence/manifest/assumption-fragments/FARNELL-1455N1201BK.json]
  - name: observe-cpc-otj5
    owned-paths: [evidence/manifest/fragments/CPC-CLIFF-OTJ5.json, evidence/manifest/assumption-fragments/CPC-CLIFF-OTJ5.json]
  - name: observe-digikey-orj5
    owned-paths: [evidence/manifest/fragments/DIGIKEY-CLIFF-ORJ5.json, evidence/manifest/assumption-fragments/DIGIKEY-CLIFF-ORJ5.json]
review-intensity: active
commit-checkpoint:
  green: [python3 scripts/seed_evidence.py check-fragment FARNELL-1455N1201BK CPC-CLIFF-OTJ5 DIGIKEY-CLIFF-ORJ5]
  buildable: [validated enclosure and optical price observations]
  parent-commit: "docs(aius): record enclosure and optical prices"
```

Completed 2026-07-16 with shared observation timestamp
`2026-07-16T06:25:44Z`. Farnell yielded a complete current enclosure price and
stock observation. CPC and DigiKey returned access-denied responses from their
fixed pages, so their canonical unavailable records and open assumptions were
retained. An earlier wave at `2026-07-16T06:15:53Z` was rejected after delivery
audit found live-data drift and a false unavailable result; none of its values
were retained. The replacement combined checkpoint, pinned-Prettier parity,
owned-path audit, and clean-room delivery audit passed.

```yaml
phase: P1B3b
status: completed
unit: single-dynamic-source
seams: [live page, normalized observations]
contract-artifact: both
fan-out:
  - name: observe-digikey-teensy41
    owned-paths: [evidence/manifest/fragments/DIGIKEY-TEENSY41.json, evidence/manifest/assumption-fragments/DIGIKEY-TEENSY41.json]
  - name: observe-pcbway-2l
    owned-paths: [evidence/manifest/fragments/PCBWAY-2L-100.json, evidence/manifest/assumption-fragments/PCBWAY-2L-100.json]
  - name: observe-pcbway-4l
    owned-paths: [evidence/manifest/fragments/PCBWAY-4L-100.json, evidence/manifest/assumption-fragments/PCBWAY-4L-100.json]
review-intensity: active
commit-checkpoint:
  green: [python3 scripts/seed_evidence.py check-fragment DIGIKEY-TEENSY41 PCBWAY-2L-100 PCBWAY-4L-100]
  buildable: [validated Teensy and PCB quote observations]
  parent-commit: "docs(aius): record Teensy and PCB prices"
```

Completed 2026-07-16 with shared observation timestamp
`2026-07-16T06:39:46Z`. DigiKey returned an access-denied response. Both PCBWay
landing pages were readable but exposed default quantity-10 prices rather than
complete quotes for the locked quantity-5 inputs, and did not bind all required
inclusion fields. All three sources therefore retain canonical unavailable
records and open assumptions. The combined checkpoint, pinned-Prettier parity,
owned-path audit, and clean-room delivery audit passed.

```yaml
phase: P1B3c
status: completed
unit: single-dynamic-source
seams: [official series, normalized rates]
contract-artifact: both
execution-parameters:
  observed-at: "2026-07-16T06:53:36Z"
  unavailable-note: "Automated observation from the fixed landing page failed during P1B3c."
fan-out:
  - name: observe-boe-fx
    owned-paths: [evidence/manifest/fragments/BOE-GBP-DAILY-FX.json, evidence/manifest/assumption-fragments/BOE-GBP-DAILY-FX.json]
review-intensity: active
commit-checkpoint:
  green: [python3 scripts/seed_evidence.py check-fragment BOE-GBP-DAILY-FX]
  buildable: [validated official FX observations]
  parent-commit: "docs(aius): record official GBP rates"
```

Completed 2026-07-16 with observation timestamp `2026-07-16T06:53:36Z`.
The fixed Bank of England page quoted EUR and USD per pound, opposite the locked
GBP-per-currency fields. Because recording those fields would require an
unapproved reciprocal calculation, the canonical unavailable record and open
assumption were retained. The checkpoint, pinned-Prettier parity, owned-path
audit, and clean-room delivery audit passed.

### P1B4 — Aggregate seed manifest

The coordinator runs only after every prior wave is committed. It owns only the
two aggregate files, runs `seed_evidence.py assemble --write`, then the check and
full validator. It does not manually edit records or assumptions.

```yaml
phase: P1B4
status: completed
unit: seed-manifest-coordinator
seams: [validated fragments, aggregate manifests]
contract-artifact: both
fan-out:
  - name: seed-manifest-coordinator
    owned-paths:
      - evidence/manifest/sources.json
      - evidence/manifest/assumptions.json
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/seed_evidence.py assemble --check
    - python3 scripts/validate_evidence.py
    - the repo tooling tool doc-index -- --check
  buildable: [auditable complete seed evidence checkout]
  parent-commit: "docs(aius): archive seed design evidence"
```

Completed 2026-07-16. Deterministic assembly produced 22 canonical source
records and six canonical open assumptions. All 11 golden sources are current;
the six unavailable sources are dynamic only. Aggregate parity, the full
validator, 32 seed tests, 34 validator tests, pinned-Prettier parity, owned-path
scope, and clean-room delivery audit passed. The documentation index check
reported an already-dirty stale generated `docs/README.md`; repository policy
classifies that foreign generated-index condition as nonblocking and excludes
it from this authored commit.

## P1C — Functional topology and bounded component selection

The first P1C clean-room gate returned `BLOCKED` on 2026-07-16. The original
combined phase referenced a nonexistent validator, had no topology/state
schemas or deterministic CSV renderer, ran selectors before their architecture
inputs, combined multiple MPNs into single records, and allowed free-text
requirements without evidence or policy bases. It also exposed a stale P1A test
that still required the now-populated assumptions aggregate to be empty. The
P1C0 through P1C4 contract below replaces that combined outline.

### Locked P1C architecture and safety decisions

The canonical architecture is JSON; Markdown and CSV are generated review
views. The functional topology must contain these block IDs:
`power-entry`, `usb-bridge`, `teensy`, `optical-rx`, `spdif-capture`,
`input-classifier`, `dts-decoder`, `pcm-normalizer`, `downmix-eq`,
`spdif-transmit`, `raw-bypass-mux`, `tx-gate`, `optical-tx`, `ethernet`, and
`expansion`. It must contain modes `pcm-processed`, `dts-processed`,
`diagnostic-raw-bypass`, `unsupported-muted`, and `fault-muted`; data, control,
and sole-source power flows; recovered-S/PDIF, processing, transmit, USB, and
Ethernet clock domains; DMA/buffer owners; and latency boundaries whose numeric
budgets remain explicitly deferred to P6/P7 rather than invented in P1C.
Exact clock-domain IDs are `spdif-recovered`, `audio-processing`,
`spdif-transmit-clock`, `usb-device-clock`, and `ethernet-rmii-clock`. Exact
buffer IDs are `spdif-rx-dma`, `compressed-burst-ring`, `decoded-pcm-pool`,
`dsp-output-ring`, and `spdif-tx-dma`. Exact latency-boundary IDs are
`capture`, `burst-assembly`, `decode`, `dsp`, and `transmit-queue`.
`schema_version` is the constant string `1.0.0`. Every object rejects additional
properties. Array order is canonical in the order declared by the plan or, for
non-enumerated rows, ascending by ID.

Functional member contracts are:

| Record | Required members and value domains |
| --- | --- |
| mode | `id` from the five locked mode IDs; `input_formats` as a unique non-empty array of `pcm-stereo`, `iec61937-dts-core`, or `none`; `output_format` as `pcm-stereo-48k`, `raw-spdif`, or `none`; unique `active_blocks`; `error_mode` as `unsupported-muted` or `fault-muted`; unique `evidence_ids`; string `notes` |
| block | locked `id`; `kind` as `hardware`, `firmware`, `software`, or `boundary`; `owner` as `carrier`, `teensy-hardware`, `teensy-firmware`, or `external`; nullable locked `power_domain`; nullable locked `clock_domain`; unique `evidence_ids`; string `notes` |
| flow | locked `id`; `kind` as `data`, `control`, or `power`; cross-referenced `source_id` and `sink_id`; `payload` from the locked payload tokens; unique `enabled_modes`; nullable locked `control_id`; unique `evidence_ids`; string `notes` |
| clock domain | locked `id`; cross-referenced `source_id`; unique non-empty `consumer_ids`; unique `synchronous_with`; `frequency_status` as `evidence-locked`, `software-derived`, or `deferred`; unique `evidence_ids`; string `notes` |
| buffer | locked `id`; block IDs in `owner`, `producer`, and `consumer`; `storage_class` as `dma-ring`, `ring-buffer`, or `block-pool`; `overflow_policy` as `drop-and-mute`, `reset-pipeline-and-mute`, or `backpressure`; `measurement_gate` from P3 through P7; string `notes` |
| latency boundary | locked `id`; block IDs in `source` and `sink`; `budget_status` constant `deferred`; `measurement_gate` as P6 or P7; string `notes` |

Exact flow IDs are `optical-to-capture`, `capture-to-classifier`,
`classifier-to-dts`, `classifier-to-pcm`, `dts-to-normalizer`,
`pcm-to-normalizer`, `normalizer-to-dsp`, `dsp-to-transmit`,
`transmit-to-mux`, `optical-to-raw-mux`, `mux-to-gate`, `gate-to-optical`,
`path-select-control`, `tx-enable-control`, `watchdog-reset-control`,
`usb-vbus-entry`, `protected-vbus`, `teensy-power`, `carrier-3v3`, and
`expansion-power`. Exact payload tokens are `optical-spdif`, `logic-spdif`,
`iec61937-dts-core`, `pcm-native`, `pcm-stereo-48k`, `path-select`,
`tx-enable`, `reset`, `usb-vbus`, `protected-5v`, and `teensy-3v3`.

The functional JSON root fields, in order, are `schema_version`, `modes`,
`blocks`, `flows`, `clock_domains`, `buffers`, `latency_boundaries`, and
`evidence_ids`. Mode rows contain `id`, input/output formats, active block IDs,
error mode, evidence IDs, and notes. Block rows contain `id`, kind, owner,
power-domain ID, nullable clock-domain ID, evidence IDs, and notes. Flow rows
contain `id`, `kind` (`data`, `control`, or `power`), source and sink IDs,
payload, enabled mode IDs, nullable controlling control ID, evidence IDs, and
notes. Clock rows identify source, consumers, and synchronous relationships.
Buffer rows identify owner, producer, consumer, storage class, overflow policy,
and measurement gate. Latency rows identify endpoints, `deferred` budget
status, P6/P7 measurement gate, and notes.

The interface/state JSON root fields, in order, are `schema_version`,
`connectors`, `controls`, `expansion_interfaces`, `power_domains`,
`operating_states`, `fault_scenarios`, `transitions`, and `evidence_ids`.
Connector rows contain ID, location, role, signals, power-domain IDs, mating
constraint, evidence IDs, and notes. Control rows contain ID, owner, active
level, reset default, safe value, evidence IDs, and notes. Expansion rows add
direction, powered and unpowered behavior, current-limit or isolation method,
and backfeed rule. Power-domain rows contain ID, source, consumers, and
backfeed rule. State and fault rows contain entry condition, path select,
TX state, output payload, expansion state, recovery/exit condition, nullable
recovery measurement gate, and notes. Transition rows contain ID, source and
sink states, preconditions, ordered actions, and postconditions.

The exact interface inventory IDs are `rear-usb-c`, `internal-usb-a`,
`teensy-micro-b`, `toslink-rx`, `toslink-tx`, `ethernet-rj45`,
`microsd-access`, `program-control`, `status-led`, `remote-program-rtc`,
`expansion-i2c-control`, `expansion-spi`, `expansion-uart-debug`,
`expansion-i2s-tdm`, `expansion-usb-host`, `expansion-gpio-adc`, and
`test-fixture`. Control IDs are `spdif-path-select`, `optical-tx-enable`,
`teensy-reset`, `teensy-watchdog`, `program-control`, and `status-led`.
Power-domain IDs are `usb-vbus-raw`, `protected-5v`, `teensy-3v3`,
`digital-ground`, `chassis-shield`, `expansion-3v3`, and
`expansion-usb-host-vbus`. Operating-state IDs are `power-off`,
`powered-reset`, `boot-muted`, `waiting-for-carrier`, `pcm-active`,
`dts-active`, `unsupported-muted`, `diagnostic-bypass`, and `fault-muted`.

ID partitions are strict: connector IDs are `rear-usb-c`, `internal-usb-a`,
`teensy-micro-b`, `toslink-rx`, `toslink-tx`, `ethernet-rj45`,
`microsd-access`, `remote-program-rtc`, and `test-fixture`; expansion IDs are
the six `expansion-*` IDs; control IDs occur only in `controls`. Interface/state
member contracts are:

| Record | Required members and value domains |
| --- | --- |
| connector | locked `id`; `location` as `rear`, `internal`, `module`, or `user-access`; `role` as `power-data`, `data`, `optical`, `network`, `storage-access`, `control`, or `test`; unique non-empty `signals`; unique `power_domains`; non-empty `mating_constraint`; unique `evidence_ids`; string `notes` |
| control | locked `id`; cross-referenced `owner`; `active_level` as `high`, `low`, or `not-applicable`; `reset_default` and `safe_value` from the safe-value tokens; unique `evidence_ids`; string `notes` |
| expansion interface | locked `id`; `direction` as `input`, `output`, or `bidirectional`; unique non-empty `signals`; unique `power_domains`; non-empty `powered_behavior`, `unpowered_behavior`, `protection_method`, and `backfeed_rule`; unique `evidence_ids`; string `notes` |
| power domain | locked `id`; cross-referenced `source`; unique `consumers`; non-empty `backfeed_rule`; unique `evidence_ids`; string `notes` |
| operating state | locked `id`; non-empty `entry_condition`; `path_select`, `tx_state`, `output_payload`, and `expansion_state` from locked tokens; non-empty `exit_condition`; string `notes` |
| fault scenario | locked `id`; non-empty `trigger`; four locked outcome tokens; Boolean `hardware_pull_required` and `watchdog_required`; nullable `debounce_measurement_gate` as P6 or P7; nullable `residual_interval_measurement_gate` as P7; ordered unique `actions`; locked `recovery_state`; string `notes` |
| transition | locked `id`, `from_state`, and `to_state`; unique non-empty locked `triggers`; unique `preconditions`; ordered non-empty `actions`; unique non-empty `postconditions`; string `notes` |

Outcome tokens are: path `processed` or `raw`; TX `disabled`, `enabled-pcm`,
`enabled-raw`, or `unchanged`; payload `none`, `pcm-silence-48k`,
`processed-pcm-48k`, `raw-spdif`, or `unchanged`; expansion
`high-impedance`, `current-limited`, `active`, or `unchanged`. Safe-value tokens
are those outcome tokens plus `asserted`, `deasserted`, `low`, and `high`.
Action tokens are `disable-tx`, `wait-muted`, `select-processed`, `select-raw`,
`clear-pipeline`, `prime-pcm-silence`, `enable-tx-pcm`, `enable-tx-raw`,
`assert-reset`, and `hold-expansion-safe`. Precondition tokens are
`rails-valid`, `watchdog-armed`, `pipeline-primed`, `carrier-present`,
`format-supported`, `output-muted`, `tx-disabled`, and
`diagnostic-authorized`. Postcondition tokens are `processed-selected`,
`raw-selected`, `tx-disabled`, `tx-pcm-enabled`, `tx-raw-enabled`, and
`expansion-safe`.

Exact transition IDs are `power-on-to-reset`, `reset-to-boot`,
`boot-to-waiting`, `waiting-to-pcm`, `waiting-to-dts`,
`active-to-carrier-loss`, `active-to-unsupported`,
`unsupported-to-active`, `active-to-diagnostic-bypass`,
`diagnostic-bypass-to-waiting`, `watchdog-to-reset`, and
`usb-unplug-to-power-off`. Their exact trigger mapping is:

| Transition ID | Trigger tokens |
| --- | --- |
| `power-on-to-reset` | `power-applied` |
| `reset-to-boot` | `reset-released` |
| `boot-to-waiting` | `pipeline-ready` |
| `waiting-to-pcm` | `pcm-detected` |
| `waiting-to-dts` | `dts-detected` |
| `active-to-carrier-loss` | `optical-carrier-loss` |
| `active-to-unsupported` | `unsupported-burst`, `malformed-input` |
| `unsupported-to-active` | `supported-format-detected` |
| `active-to-diagnostic-bypass` | `raw-bypass-request` |
| `diagnostic-bypass-to-waiting` | `raw-bypass-release` |
| `watchdog-to-reset` | `watchdog-expired` |
| `usb-unplug-to-power-off` | `usb-unplug` |

No other transition trigger token is schema-valid.

The following exact fault/scenario IDs are mandatory and have these safe outcomes:

| Scenario ID | Path select | Optical TX | Expansion | Additional contract |
| --- | --- | --- | --- | --- |
| `teensy-absent` | processed | disabled | high impedance or current-limited | Hardware pulls establish the state without firmware. |
| `boot` | processed | disabled until watchdog and output pipeline are ready | high impedance or current-limited | Firmware arms the watchdog before it may enable TX. |
| `reset` | processed | disabled | high impedance or current-limited | Hardware pulls establish the state without firmware. |
| `brownout` | processed | disabled after reset assertion | high impedance or current-limited | Recovery requires a clean reset and boot sequence. |
| `firmware-crash` | processed after watchdog reset | disabled after watchdog reset | high impedance or current-limited after reset | Record the bounded pre-reset residual interval. |
| `optical-carrier-loss` | processed | disabled after debounce | unchanged safe configuration | Preserve the downstream DAC's optical-carrier power trigger. |
| `unsupported-burst` | processed | valid 48 kHz PCM silence only | unchanged safe configuration | Raw compressed data never reaches the DAC. |
| `malformed-input` | processed | valid 48 kHz PCM silence only | unchanged safe configuration | Raw malformed data never reaches the DAC. |
| `usb-unplug` | processed | disabled as rails collapse | high impedance or current-limited | No secondary supply or backfeed may sustain a rail. |
| `raw-bypass-request` | raw only after an explicit diagnostic request | disabled while switching, then explicitly enabled | unchanged safe configuration | Switch only while muted; reset returns to processed and disabled. |
| `unpowered-expansion` | processed | normal core policy | high impedance or current-limited | No expansion power-rail backfeed; signal clamp current is bounded. |

USB-C is a sink-only USB 2.0 device entry with one 5 V source, separate CC1/CC2
pull-downs, no source role, no Power Delivery, and no second VIN/VUSB feed. The
power candidate must bound inrush, overload, thermal fault, input loss, and
reverse-current/backfeed behavior. USB ESD must be appropriate for high-speed
D+/D- and the two-layer 90-ohm route. P1C records the preliminary current
envelope and defers final USB power compliance arithmetic to P1D.

Expansion 3.3 V and USB-host VBUS use a selected protection device that blocks
reverse power flow when either side is unpowered. I2C uses a selected isolating
or powered-off-high-impedance device. Remaining exported single-ended CMOS
outputs use the `expansion-io-series-resistor` allowance to bound clamp current;
inputs must be high impedance while unconfigured. P1D must validate the actual
powered-off leakage and clamp-current limits before these candidates become
selected.

The optical receiver, mux, gate, and transmitter use a 3.3 V logic domain. The
selection policy must account for ORJ5's 2.4 V minimum high output and OTJ5's
2.0 V minimum high input, require at least 16 Mbit/s logic bandwidth, prevent
driver contention, pull the mux to the processed path, pull TX enable inactive,
and permit selection changes only while TX is disabled. Ethernet selection is
for the Teensy 4.1 native DP83825 interface and must bind magjack turns ratio,
pinout, centre taps, isolation, LEDs, shield, and the PJRC reference's 0.1 uF
centre-tap capacitor. Connector requirements include lifecycle, manufacturer
authority, UK authorized-distributor availability to be verified in P1D, MOQ,
mating geometry, and Hammond-derived enclosure/cable clearance.

Functional Markdown section order is `Modes`, `Blocks`, `Data flows`,
`Control flows`, `Power flows`, `Clock domains`, `Buffers and DMA ownership`,
`Latency boundaries`, and `Evidence`. Interface/state Markdown section order
is `Connectors`, `Controls`, `Expansion interfaces`, `Power domains`,
`Operating states`, `Fault scenarios`, `Transitions`, and `Evidence`.

Every requirement is a stable requirement ID matching
`^REQT-[A-Z0-9]+(?:-[A-Z0-9]+)*$` and declared in
`evidence/design/selection-policy.json`. Each requirement cites at least one
current seed evidence ID or a declared engineering-policy ID. Component and
source-request records repeat only those IDs; cross-validation resolves the
text and rejects unsupported additions. Engineering-policy IDs match
`^EP-[A-Z0-9]+(?:-[A-Z0-9]+)*$` and have scope `product-topology`,
`safe-default`, `manufacturability`, `sourcing`, or
`provisional-design-choice`, plus an explicit later validation gate.
Engineering policies may state project choices; they may not assert external
component capability, standards compliance, safety rating, lifecycle, price,
or availability. A provisional numeric design choice must use the
`provisional-design-choice` scope and remain blocked on its P1D validation gate.
The selection-policy JSON root fields, in order, are `schema_version`,
`engineering_policies`, `requirements`, and `families`. Policy rows contain
`id`, `scope`, `rule`, `validation_gate`, and notes. Requirement rows contain
`id`, statement, non-empty basis IDs, `verification_stage`, and notes. Family
rows contain slug, critical flag, allowed status set, exact requirement IDs,
manufacturer and availability request IDs when applicable, generic eligibility,
dependent feature, and notes. Basis IDs must resolve to a current seed source or
a declared engineering policy; source and policy namespaces may not overlap.
`verification_stage` is one of `P1D-electrical`, `P1D-mechanical`,
`P1E-sourcing`, `H1-schematic`, or `H2-placement`; dependent features are
non-empty slugs and are instantiated by P1C1b.

The ordered component key set is `block`, `requirements`, `manufacturer`,
`mpn`, `alternatives`, `selection_rationale`, `authority_url`,
`manufacturer_request_id`, `availability_request_id`, `generic_policy`,
`status`, and `notes`. `block` is a slug; `requirements` is a unique non-empty
array of requirement IDs; manufacturer, MPN, rationale, HTTPS authority, and
both request IDs are nullable non-empty values governed by the status matrix;
alternatives retain the P1A2 object shape but are cross-checked for uniqueness;
generic policy is nullable and otherwise contains exactly non-empty `value`,
`package`, and `tolerance`; status is one of the five statuses in the matrix;
notes is a string. Additional properties are rejected.

The ordered selection-policy family key set is `family`, `component_file`,
`critical`, `generic_eligible`, `allowed_statuses`, `requirements`,
`manufacturer_request_id`, `availability_request_id`,
`manufacturer_source_id`, `availability_source_id`, `dependent_feature`, and
`notes`. Family and dependent feature are slugs; `component_file` is exactly
`components/<family>.json`; the two flags are Boolean; statuses and requirements
are unique non-empty arrays of their locked IDs; request/source fields are
nullable identifier strings; notes is a string. When `generic_eligible` is
true, all four request/source fields are null and allowed statuses is exactly
`generic-allowance`. Otherwise all four request/source fields are non-null and
use the deterministic conventions below. Critical families exclude
`unresolved` and treat `candidate-evidence-unavailable` as blocking;
noncritical selected families may include `unresolved`.

Each selected candidate has one manufacturer-evidence request and one separate
UK-availability request. Requests add `request_kind` with value
`manufacturer-evidence` or `uk-availability`. The component record stores
`manufacturer_request_id` and `availability_request_id`; its authority URL
equals the manufacturer request URL. Exact status mapping is:

| Component status | Manufacturer request | Required component fields | P1C/P1D meaning |
| --- | --- | --- | --- |
| `candidate-pending-evidence` | `pending` | manufacturer, MPN, rationale, authority, both request IDs; generic policy null | Provisional candidate; P1D must acquire manufacturer evidence. |
| `selected` | `satisfied` with at least one current evidence ID | same selected fields; generic policy null | Electrical candidate is supported by archived manufacturer evidence. |
| `candidate-evidence-unavailable` | `unavailable` with a non-empty note | same selected fields; generic policy null | Critical family blocks; noncritical family remains an explicit risk. |
| `generic-allowance` | no requests | selected fields and request IDs null; one complete generic policy | Uniform engineering allowance only. |
| `unresolved` | no requests | selected fields, request IDs, and generic policy null; non-empty rationale | Allowed only for a noncritical family and blocks its dependent feature. |

The UK-availability request is independently `pending`, `satisfied`, or
`unavailable` and never promotes or demotes electrical selection status. It is
consumed by P1D/P1E sourcing and confidence gates. Cliff RX and TX are the only
P1C2 records initially `selected`: their manufacturer requests are satisfied by
`CLIFF-ORJ5` and `CLIFF-OTJ5` respectively, while their new UK-availability
requests remain pending. All other selected candidates start
`candidate-pending-evidence` with both requests pending. Generic allowances
have one uniform value/package/tolerance and no requests. An unresolved or
manufacturer-unavailable critical family blocks P1C. Alternatives are unique
and must not repeat the selected manufacturer/MPN.

All source-request fields are required: `request_id`, `request_kind`, `family`,
`requirements`, `authority_url`, `selected_manufacturer`, `selected_mpn`,
`source_id`, `artifact_kinds`, `retention`, `status`, `evidence_ids`, and
`notes`. A manufacturer request uses a primary manufacturer HTTPS URL,
`retention: archive`, and artifact kinds drawn only from `datasheet`, `drawing`,
`cad`, `product-page`, and `reference-design`. Its source ID is
`AIUS-<UPPERCASE-FAMILY>-MFR`. A UK-availability request uses a UK authorized
distributor HTTPS URL, `retention: normalized-observation-only`, exact artifact
kind `price-observation`, and source ID `AIUS-<UPPERCASE-FAMILY>-UK`.
The two satisfied Cliff manufacturer requests are the only source-ID exceptions:
they use existing current seed IDs `CLIFF-ORJ5` and `CLIFF-OTJ5`.

For either request kind, `pending` has no evidence IDs; `satisfied` has at least
one current evidence ID matching its source ID and selected MPN; `unavailable`
has no evidence IDs and a non-empty failure note. Manufacturer evidence may be
golden, comparative, or supplemental but must archive a manufacturer artifact.
UK evidence is a current dynamic normalized observation and never archives the
page. Request IDs are exactly `REQ-<UPPERCASE-FAMILY>-MFR` and
`REQ-<UPPERCASE-FAMILY>-UK`; component and filename cross-validation is exact.

### P1C0a — Schema and policy vocabulary

P1C0a is an intentional forward schema migration. It supersedes the original
P1A2 source-request and component field/status contract; the P1A2 section
remains historical evidence of its completed checkpoint, not the post-P1C
schema authority. Existing assumption records remain schema-compatible.

```yaml
phase: P1C0a
status: completed
unit: schema-contract
seams: [canonical architecture JSON, atomic selection records]
contract-artifact: both
fan-out:
  - name: functional-topology-schema
    owned-paths:
      - evidence/design/functional-topology-schema.json
      - scripts/fixtures/p1c-schema/functional-topology
      - scripts/test_functional_topology_schema.py
  - name: interfaces-states-schema
    owned-paths:
      - evidence/design/interfaces-and-states-schema.json
      - scripts/fixtures/p1c-schema/interfaces-and-states
      - scripts/test_interfaces_states_schema.py
  - name: selection-status-schema
    owned-paths:
      - evidence/design/component-schema.json
      - evidence/design/selection-policy-schema.json
      - evidence/manifest/source-request-schema.json
      - scripts/fixtures/p1c-schema/selection
      - scripts/test_selection_schemas.py
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_functional_topology_schema.py
    - python3 scripts/test_interfaces_states_schema.py
    - python3 scripts/test_selection_schemas.py
    - node_modules/.bin/prettier --check "evidence/**/*.json" "scripts/fixtures/p1c-schema/**/*.json"
  buildable: [machine-readable P1C artifact and status vocabulary]
  parent-commit: "feat(aius): define P1C architecture contracts"
```

The stale empty-assumptions assertion is replaced by schema validation plus the
same aggregate-to-fragment parity invariant used by P1B. The new schemas require
the mandatory IDs and scenario coverage above. The selection-policy schema owns
the fully named structural fields, ID patterns, status/request rules, basis
resolution types, and additional-property rejection. It does not invent the
policy instance. P1C1b owns and commits the exact requirement and engineering
policy registries, per-family mappings, allowed-status sets, dependent-feature
values, request/source IDs, and file slugs. The `architecture` validator stage
then enforces that instance against the exact family list in this plan.

Completed 2026-07-16. The three disjoint schema owners delivered the canonical
functional topology, interface/state, component, source-request, and selection
policy contracts plus focused fixtures. Sixteen functional, 15 interface/state,
and 31 selection/status tests pass; the current six assumptions exactly match
their fragments; broad pinned-Prettier parity and the clean-room delivery audit
passed.

### P1C0b — Validator and deterministic renderers

```yaml
phase: P1C0b
unit: enforcement-tooling
seams: [canonical JSON, generated review views]
contract-artifact: both
fan-out:
  - name: p1-validator
    owned-paths:
      - scripts/validate_p1.py
      - scripts/test_validate_p1.py
      - scripts/fixtures/validate-p1
  - name: p1-renderer
    owned-paths:
      - scripts/render_p1.py
      - scripts/test_render_p1.py
      - scripts/fixtures/render-p1
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_validate_p1.py
    - python3 scripts/test_render_p1.py
    - python3 scripts/validate_p1.py topology --stage contract
  buildable: [executable P1C contract and reproducible review-view generation]
  parent-commit: "feat(aius): enforce P1C architecture contracts"
```

`validate_p1.py` supports topology stages `contract`, `architecture-base`,
`architecture`, `selection-fragments`, `selection`, and `handoff`. It validates schemas,
mandatory IDs, exact expected file sets, citations, cross-document references,
component/request equality, status transitions, critical-family resolution,
and generated-view parity. Negative fixtures cover every invariant.

`render_p1.py` has independent `functional-topology`,
`interfaces-and-states`, `components`, and `supplemental-requests` write/check
commands. It is the only
writer for the two Markdown views and CSV. Markdown sections and tables have
stable schema-defined ordering. The component CSV is UTF-8 RFC 4180 with LF,
header order `block,status,critical,manufacturer,mpn,requirements,alternatives,selection_rationale,authority_url,manufacturer_request_id,availability_request_id,generic_value,generic_package,generic_tolerance,notes`, rows sorted by `block`, compact sorted JSON for array/object cells, and empty strings for nulls. Tests include golden fixtures, escaping/newline cases, semantic parity, and stale-output rejection.

### P1C1a — Functional topology and interface/state truth tables

```yaml
phase: P1C1a
unit: architecture-instance
seams: [functional flow, failure-state outputs]
contract-artifact: both
fan-out:
  - name: functional-flow
    owned-paths:
      - evidence/design/functional-topology.json
      - docs/reference/functional-topology.md
  - name: interfaces-states
    owned-paths:
      - evidence/design/interfaces-and-states.json
      - docs/reference/interfaces-and-states.md
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/render_p1.py functional-topology --check
    - python3 scripts/render_p1.py interfaces-and-states --check
    - python3 scripts/validate_p1.py topology --stage architecture-base
  buildable: [locked architecture IDs, flow ownership, and safe-state truth tables]
  parent-commit: "docs(aius): lock functional and failure-state topology"
```

### P1C1b — Requirement and expected-family policy

```yaml
phase: P1C1b
unit: selection-policy
seams: [locked architecture, atomic selector inputs]
contract-artifact: both
fan-out:
  - name: selection-policy
    owned-paths: [evidence/design/selection-policy.json]
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_p1.py topology --stage architecture
  buildable: [finite evidence-backed atomic selection queue]
  parent-commit: "docs(aius): bind component selection requirements"
```

The exact P1C2 family slugs are:

- selected critical candidates: `usb-c-receptacle`, `usb-a-receptacle`,
  `internal-usb-cable`, `usb-esd`, `usb-power-protection`, `cliff-rx`,
  `cliff-tx`, `spdif-mux`, `spdif-tx-gate`, `ethernet-magjack`,
  `teensy-side-socket`, `teensy-ethernet-socket`,
  `teensy-usb-host-socket`, `expansion-power-protection`, and
  `expansion-i2c-isolator`;
- selected noncritical candidates: `expansion-i2c-control`, `expansion-spi`,
  `expansion-uart-debug`, `expansion-i2s-tdm`, `expansion-usb-host`,
  `expansion-gpio-adc`, `remote-program-rtc`, `test-fixture-header`, and
  `mechanical-cable-retainer`;
- generic allowances: `usb-c-cc-pulldown`, `logic-decoupling-capacitor`,
  `logic-control-pulldown`, `ethernet-center-tap-capacitor`, and
  `expansion-io-series-resistor`.

P1C1b must use exactly this set. Changing it requires a reviewed plan amendment,
not an implementer-local split or merge.

### P1C2 — Atomic component candidates and source requests

Each selected-candidate task owns
`evidence/design/components/<family>.json` and
`evidence/manifest/requests/REQ-<FAMILY>-MFR.json` plus
`evidence/manifest/requests/REQ-<FAMILY>-UK.json`. Each generic task owns only
its component record. The coordinator spawns one bounded task per slug from the
locked list above; no task selects more than one MPN. At most three disjoint
tasks run concurrently. Each task runs
`validate_p1.py topology --stage selection-fragments --family <slug>` before
handoff. Primary manufacturer material is required for the authority URL;
current distributor availability is recorded as a P1D request, not inferred.

```yaml
phase: P1C2
unit: single-atomic-family
seams: [selection-policy family, component/request pair]
contract-artifact: both
fan-out:
  - name: select-usb-c-receptacle
    owned-paths: [evidence/design/components/usb-c-receptacle.json, evidence/manifest/requests/REQ-USB-C-RECEPTACLE-MFR.json, evidence/manifest/requests/REQ-USB-C-RECEPTACLE-UK.json]
  - name: select-usb-a-receptacle
    owned-paths: [evidence/design/components/usb-a-receptacle.json, evidence/manifest/requests/REQ-USB-A-RECEPTACLE-MFR.json, evidence/manifest/requests/REQ-USB-A-RECEPTACLE-UK.json]
  - name: select-internal-usb-cable
    owned-paths: [evidence/design/components/internal-usb-cable.json, evidence/manifest/requests/REQ-INTERNAL-USB-CABLE-MFR.json, evidence/manifest/requests/REQ-INTERNAL-USB-CABLE-UK.json]
  - name: select-usb-esd
    owned-paths: [evidence/design/components/usb-esd.json, evidence/manifest/requests/REQ-USB-ESD-MFR.json, evidence/manifest/requests/REQ-USB-ESD-UK.json]
  - name: select-usb-power-protection
    owned-paths: [evidence/design/components/usb-power-protection.json, evidence/manifest/requests/REQ-USB-POWER-PROTECTION-MFR.json, evidence/manifest/requests/REQ-USB-POWER-PROTECTION-UK.json]
  - name: select-cliff-rx
    owned-paths: [evidence/design/components/cliff-rx.json, evidence/manifest/requests/REQ-CLIFF-RX-MFR.json, evidence/manifest/requests/REQ-CLIFF-RX-UK.json]
  - name: select-cliff-tx
    owned-paths: [evidence/design/components/cliff-tx.json, evidence/manifest/requests/REQ-CLIFF-TX-MFR.json, evidence/manifest/requests/REQ-CLIFF-TX-UK.json]
  - name: select-spdif-mux
    owned-paths: [evidence/design/components/spdif-mux.json, evidence/manifest/requests/REQ-SPDIF-MUX-MFR.json, evidence/manifest/requests/REQ-SPDIF-MUX-UK.json]
  - name: select-spdif-tx-gate
    owned-paths: [evidence/design/components/spdif-tx-gate.json, evidence/manifest/requests/REQ-SPDIF-TX-GATE-MFR.json, evidence/manifest/requests/REQ-SPDIF-TX-GATE-UK.json]
  - name: select-ethernet-magjack
    owned-paths: [evidence/design/components/ethernet-magjack.json, evidence/manifest/requests/REQ-ETHERNET-MAGJACK-MFR.json, evidence/manifest/requests/REQ-ETHERNET-MAGJACK-UK.json]
  - name: select-teensy-side-socket
    owned-paths: [evidence/design/components/teensy-side-socket.json, evidence/manifest/requests/REQ-TEENSY-SIDE-SOCKET-MFR.json, evidence/manifest/requests/REQ-TEENSY-SIDE-SOCKET-UK.json]
  - name: select-teensy-ethernet-socket
    owned-paths: [evidence/design/components/teensy-ethernet-socket.json, evidence/manifest/requests/REQ-TEENSY-ETHERNET-SOCKET-MFR.json, evidence/manifest/requests/REQ-TEENSY-ETHERNET-SOCKET-UK.json]
  - name: select-teensy-usb-host-socket
    owned-paths: [evidence/design/components/teensy-usb-host-socket.json, evidence/manifest/requests/REQ-TEENSY-USB-HOST-SOCKET-MFR.json, evidence/manifest/requests/REQ-TEENSY-USB-HOST-SOCKET-UK.json]
  - name: select-expansion-power-protection
    owned-paths: [evidence/design/components/expansion-power-protection.json, evidence/manifest/requests/REQ-EXPANSION-POWER-PROTECTION-MFR.json, evidence/manifest/requests/REQ-EXPANSION-POWER-PROTECTION-UK.json]
  - name: select-expansion-i2c-isolator
    owned-paths: [evidence/design/components/expansion-i2c-isolator.json, evidence/manifest/requests/REQ-EXPANSION-I2C-ISOLATOR-MFR.json, evidence/manifest/requests/REQ-EXPANSION-I2C-ISOLATOR-UK.json]
  - name: select-expansion-i2c-control
    owned-paths: [evidence/design/components/expansion-i2c-control.json, evidence/manifest/requests/REQ-EXPANSION-I2C-CONTROL-MFR.json, evidence/manifest/requests/REQ-EXPANSION-I2C-CONTROL-UK.json]
  - name: select-expansion-spi
    owned-paths: [evidence/design/components/expansion-spi.json, evidence/manifest/requests/REQ-EXPANSION-SPI-MFR.json, evidence/manifest/requests/REQ-EXPANSION-SPI-UK.json]
  - name: select-expansion-uart-debug
    owned-paths: [evidence/design/components/expansion-uart-debug.json, evidence/manifest/requests/REQ-EXPANSION-UART-DEBUG-MFR.json, evidence/manifest/requests/REQ-EXPANSION-UART-DEBUG-UK.json]
  - name: select-expansion-i2s-tdm
    owned-paths: [evidence/design/components/expansion-i2s-tdm.json, evidence/manifest/requests/REQ-EXPANSION-I2S-TDM-MFR.json, evidence/manifest/requests/REQ-EXPANSION-I2S-TDM-UK.json]
  - name: select-expansion-usb-host
    owned-paths: [evidence/design/components/expansion-usb-host.json, evidence/manifest/requests/REQ-EXPANSION-USB-HOST-MFR.json, evidence/manifest/requests/REQ-EXPANSION-USB-HOST-UK.json]
  - name: select-expansion-gpio-adc
    owned-paths: [evidence/design/components/expansion-gpio-adc.json, evidence/manifest/requests/REQ-EXPANSION-GPIO-ADC-MFR.json, evidence/manifest/requests/REQ-EXPANSION-GPIO-ADC-UK.json]
  - name: select-remote-program-rtc
    owned-paths: [evidence/design/components/remote-program-rtc.json, evidence/manifest/requests/REQ-REMOTE-PROGRAM-RTC-MFR.json, evidence/manifest/requests/REQ-REMOTE-PROGRAM-RTC-UK.json]
  - name: select-test-fixture-header
    owned-paths: [evidence/design/components/test-fixture-header.json, evidence/manifest/requests/REQ-TEST-FIXTURE-HEADER-MFR.json, evidence/manifest/requests/REQ-TEST-FIXTURE-HEADER-UK.json]
  - name: select-mechanical-cable-retainer
    owned-paths: [evidence/design/components/mechanical-cable-retainer.json, evidence/manifest/requests/REQ-MECHANICAL-CABLE-RETAINER-MFR.json, evidence/manifest/requests/REQ-MECHANICAL-CABLE-RETAINER-UK.json]
  - name: allow-usb-c-cc-pulldown
    owned-paths: [evidence/design/components/usb-c-cc-pulldown.json]
  - name: allow-logic-decoupling-capacitor
    owned-paths: [evidence/design/components/logic-decoupling-capacitor.json]
  - name: allow-logic-control-pulldown
    owned-paths: [evidence/design/components/logic-control-pulldown.json]
  - name: allow-ethernet-center-tap-capacitor
    owned-paths: [evidence/design/components/ethernet-center-tap-capacitor.json]
  - name: allow-expansion-io-series-resistor
    owned-paths: [evidence/design/components/expansion-io-series-resistor.json]
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_p1.py topology --stage selection-fragments
  buildable: [complete atomic candidate set and finite supplemental source queue]
  parent-commit: "docs(aius): select provisional component candidates"
```

### P1C3 — Deterministic provisional component view

```yaml
phase: P1C3
unit: component-selection-coordinator
seams: [validated atomic records, generated CSV]
contract-artifact: both
fan-out:
  - name: component-selection-coordinator
    owned-paths: [evidence/design/provisional-components.csv]
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/render_p1.py components --write
    - python3 scripts/render_p1.py components --check
    - python3 scripts/validate_p1.py topology --stage selection
    - the repo tooling tool doc-index -- --check
  buildable: [deterministic provisional component review view]
  parent-commit: "docs(aius): lock functional topology and component candidates"
```

P1C3 runs only after every P1C2 handoff is committed. An already-dirty or stale
foreign generated documentation index remains nonblocking under repository
policy and is recorded in the phase note rather than joined to the commit.

### P1C4 — Exact P1D acquisition handoff

```yaml
phase: P1C4
unit: supplemental-request-coordinator
seams: [validated requests, explicit P1D acquisition fan-out]
contract-artifact: both
fan-out:
  - name: p1d-handoff-coordinator
    owned-paths:
      - evidence/manifest/supplemental-requests.json
      - docs/plans/aius-p1-evidence-topology-cost.md
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/render_p1.py supplemental-requests --write
    - python3 scripts/render_p1.py supplemental-requests --check
    - python3 scripts/validate_p1.py topology --stage handoff
    - the repo tooling tool doc-index -- --check
  buildable: [exact manufacturer and UK-availability acquisition queue for P1D]
  parent-commit: "docs(aius): bind supplemental evidence acquisition"
```

P1C4 runs only after P1C3 is committed. It generates the sorted request
aggregate, then replaces the P1D placeholder fan-out below with one explicit
task and exact owned paths per non-satisfied request. Satisfied Cliff
manufacturer requests are absent from acquisition fan-out but remain in the
aggregate. The plan file is an explicit owned path for this integration phase;
no later implementer is allowed to derive or silently widen the queue.

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

P1C4 updates the P1D fan-out in this plan with one exact source ID, artifact
directory, and fragment path per approved request before its checkpoint lands.
Each P1D acquisition agent receives one request only. The P1D clean-room gate
fails if any `<request-id>` placeholder remains; source acquisition and design
work do not start in the same wave. The supplemental manifest coordinator also
creates `supplemental-clone-boundary.json` with exactly every non-seed,
non-dynamic source added by those requests, using the reusable P1A4a row
contract and the P1A4b classification rules.

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
    owned-paths:
      - evidence/manifest/sources.json
      - evidence/manifest/supplemental-clone-boundary.json
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
