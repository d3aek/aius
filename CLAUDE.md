# Aius — hardware and firmware conventions

Aius is a personal, non-commercial Teensy 4.1 surround-to-stereo downmixer.
Keep its design evidence, reproducible tooling and release gates inside this
tree.

## Evidence and provenance

**Cite where hardware facts come from.** When a doc relies on a datasheet,
drawing or CAD model, record the vendor source (part number + URL). Third-party
datasheets are not vendored in this repository; the source index is
[`docs/reference/datasheets.md`](docs/reference/datasheets.md). Keep a plain
reference habit — do not build SHA manifests, acquisition-wave audits, or
hash-locked evidence contracts for this personal, non-commercial build.

**Keep authored and reviewable sources in normal Git.** Markdown, CSV, JSON,
scripts, source code, KiCad sources and patches are normal Git. Reviewable
upstream text lives under `evidence/source-snapshots/`; firmware binary fixtures
live under `firmware/test/fixtures/` and are Git LFS tracked.

## Reproducible tooling

**Persist every PlatformIO, firmware-test, KiCad and manufacturing-package
workflow in scripts or checked-in job definitions.** Interactive commands may
diagnose a problem, but repeatable work and acceptance evidence must be
reproducible from committed entry points.

**PlatformIO is the firmware project interface.** Keep the firmware directly
openable in the VS Code PlatformIO extension. Arduino/Teensyduino is a temporary
USB and bring-up target; codec, parser, DSP and measurement code stays behind
portable C-compatible interfaces without Arduino dependencies.

**The SD-card web UI regressions run from this repo as the vitest root:**
`firmware/scripts/test.sh --environment native --suite sd-editor`, or
`pnpm exec vitest run` from the repository root.

**OTA build / sign / flash** entry points live under `firmware/scripts/`
(`build_ab.sh`, `flash_optical.sh`) and `firmware/tools/ota/`
(`aius_ota_sign.py`, `aius_ota_flash.py`). Images are Ed25519-verified on the
device. The private signing key is kept offline and is never committed; the
public repository ships only the matching public key.

## Process ownership

**Never signal, stop, restart or otherwise interfere with a process unless you
spawned that exact process.** Do not use broad process matching or
shared-service lifecycle commands. If a foreign process blocks Aius work, report
the conflict and wait for its owner.

## Hardware release gates

**Do not release or order a manufacturing package before its recorded gates
pass.** The Teensy capability proof precedes the manufacturing schematic;
electrical and failure-mode review precedes layout; two-layer signal-integrity
feasibility precedes routing; and ERC, DRC, mechanical, DFM and independent
design review precede a prototype order. Quotations and should-cost estimates
may be collected before design release.

**Treat the two-layer board as a hypothesis, not a requirement.** Escalate to
four layers only when a documented USB, Ethernet, return-path, impedance,
routing, or vendor-stackup electrical constraint fails and four layers resolves
it. Placement, outline, panel, cable, access, collision, and other mechanical
failures block redesign; changing layer count does not clear them. Record every
technical and cost delta.
