# Aius evidence manifest policy

This directory defines the durable evidence records, acquisition controls, and
validation commands used by Aius hardware decisions.

## Directory boundary

**Binary evidence:** Store public, directly retrievable binary evidence retained for personal noncommercial research under `docs/reference/datasheets.md<source-id>/`; Git LFS must report `filter=lfs`.

**Reviewable source snapshots:** Store source snapshots under `evidence/source-snapshots/<source-id>/` as normal Git text.

**External references:** Do not commit bytes behind access controls or an express retention prohibition; record `storage: external-reference` with a null path.

**Text contracts:** Keep schemas, manifests, observations, scripts, Markdown, CSV, JSON, KiCad sources, and patches in normal Git.

Paths recorded by manifests are relative to the Aius project root. Evidence bytes,
source snapshots, normalized observations, and authored contracts must remain in
their respective directories.

## Retrieval workflow

Start from the immutable seed or approved source-request identity. Resolve the
required object from its fixed landing page without substituting another
authority. Record the final HTTPS URL, retrieval time, revision, byte count,
SHA-256, media type, redistribution decision, and every artifact outcome.

Store redistributable binary objects under the source ID in `docs/reference/datasheets.md`.
Store reviewable upstream text under the source ID in
`evidence/source-snapshots/`. Record normalized dynamic facts as observations;
do not save a distributor page as a reproducible artifact.

`supplemental-realization.json` is the immutable, sorted acquisition oracle for
the 46 non-satisfied supplemental requests. Its schema fixes the Aius-owned
dispatch root, manufacturer and UK row shapes, artifact controls, authorization
tuples, clone-boundary outcomes, and the ordered UK observation profile.

Every owned path is relative to the literal `src/hardware/aius` root and must
contain its row's exact source ID. Manufacturer rows retain primary evidence;
UK rows retain normalized observations only. The constructor fixtures under
`scripts/fixtures/supplemental-realization/` lock the exact current and
unavailable source-record shapes for both classes.

Run one supplemental request through the shared evidence engine with
`seed_evidence.py supplemental acquire`, `record`, `unavailable`, or
`check-fragment`. Each command takes the immutable supplemental request ID;
acquisition and unavailable recording also require an exact UTC timestamp,
while UK recording requires the strict observation JSON and both final HTTPS
URLs.

`check-fragment` validates only the named request and does not require sibling
supplemental fragments. A current source or UK-unavailable publication exits
zero. A durably published manufacturer-unavailable source exits three because
it is a design blocker. An acquisition, recording, validation, or transactional
publication failure exits one without publishing a new partial result. CLI
usage errors exit two.

## External-reference workflow

Public retrieval does not infer permission to republish. Publicly retrievable
evidence retained in this private checkout is solely for personal,
noncommercial research, and its `redistribution_note` must state that no
permission is inferred and the bytes must not be republished.

Inspect material behind access controls or an express retention prohibition
locally without adding its bytes to the checkout. Record the final URL with
`storage: external-reference` and a null path. Record the locally observed
SHA-256 and byte count together when they are available, or leave both null.
Commit only the normalized facts needed by an Aius claim and state the access
or retention restriction.

An external reference describes what was inspected. It does not claim that a
fresh checkout can reproduce those bytes.

## Hash and media validation

Compute SHA-256 over the exact retrieved bytes before transforming or unpacking
them. Record a lowercase 64-hex digest and the byte count of that same object.
Confirm that the declared media type, filename extension, and file signature
agree. An archive and each extracted artifact are distinct manifest objects
when both support a claim.

## Git LFS verification

Before committing binary evidence, use `git check-attr filter -- <path>` and
require `filter: lfs`. Reviewable snapshots and text contracts must not report
the LFS filter. After checkout, the working file must contain the evidence
bytes rather than Git LFS pointer text, and its size and digest must match the
manifest.

## Supersession

Never change the authority or meaning of an existing source ID. Assign a new ID
to a replacement revision or distributor fallback and make the current record
name the prior, superseded record. Keep the chain acyclic. Superseded records
remain reviewable but contain no current artifacts.

## Failure recording

Record a failed retrieval as `status: unavailable` with an unavailable
`external-reference` artifact, null path, digest, size, and media type, plus a
specific failure note. Do not infer facts from an unavailable source. Missing
golden evidence blocks its dependent claim; missing comparative or dynamic
evidence follows the assumption and confidence rules in the source contract.

A supplemental UK-unavailable source also writes its exact open P1D assumption
under `evidence/manifest/supplemental-assumption-fragments/`. A supplemental
manufacturer-unavailable source writes no assumption because its exit-three
golden blocker is explicit.

## Runnable validation

Run all policy contract tests from the repository root:

```bash
python3 scripts/test_source_schema.py
python3 scripts/test_selection_schemas.py
python3 scripts/test_evidence_policy.py
python3 scripts/test_validate_evidence.py
python3 scripts/validate_evidence.py --self-test
```

## P1 selection and lifecycle validation

Validate one component and its two request fragments in isolation while working
on an acquisition outcome:

```bash
python3 scripts/validate_p1.py topology --stage selection-fragments --family ethernet-magjack
```

Before P1D2, validate the completed repository in this order:

```bash
python3 scripts/validate_evidence.py
python3 scripts/validate_p1.py topology --stage supplemental-lifecycle
python3 scripts/validate_p1.py topology --stage handoff
```

The evidence check validates retained payloads and source records. The lifecycle
stage then requires all 48 selected-component requests to be closed and joined
to their source and component outcomes. The handoff stage is the separate design
eligibility gate; it may pass while requests are pending, but rejects a critical
component whose manufacturer evidence is unavailable.
