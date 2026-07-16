# Aius evidence manifest policy

## Directory boundary

**Binary evidence:** Store redistributable binary evidence under `docs/reference/datasheets.md<source-id>/`; Git LFS must report `filter=lfs`.

**Reviewable source snapshots:** Store source snapshots under `evidence/source-snapshots/<source-id>/` as normal Git text.

**External references:** Do not commit non-redistributable bytes; record `storage: external-reference` with a null path.

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

## External-reference workflow

Inspect non-redistributable material locally without adding its bytes to the
checkout. Record the final URL with `storage: external-reference` and a null
path. Record the locally observed SHA-256 and byte count together when they are
available, or leave both null. Commit only the normalized facts needed by an
Aius claim and state the redistribution restriction.

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

## Runnable validation

Run all policy contract tests from the repository root:

```bash
python3 scripts/test_source_schema.py
python3 scripts/test_selection_schemas.py
python3 scripts/test_evidence_policy.py
```

## Future validation

The following command is unavailable until P1A4:

```bash
python3 scripts/validate_evidence.py
```
