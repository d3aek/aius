# Contributing to Aius

How to check out, build, test, flash and document the Aius Teensy 4.1
surround-to-stereo downmixer. [`README.md`](README.md) is the introduction;
this file is the operational reference. The subtree's standing conventions are
in [`CLAUDE.md`](CLAUDE.md).

## Prerequisites and checkout

**Run every command in this file from this repository's root** (``
when it is checked out as the upstream submodule). The Aius scripts resolve their
own roots from their own paths, so they work from any working directory, but the
paths quoted here are relative to this repository.

- **Git LFS** — the durable binary evidence (datasheets, reference schematics,
  enclosure STEP, firmware test fixtures) is stored as LFS objects. Materialize
  it once:

  ```bash
  git lfs install
  # datasheets are not vendored in the public mirror; see docs/reference/datasheets.md
  ```

- **Python 3** — the schematic generator, the per-bank memory report and the
  bench/flash runners are Python.
- **Docker with the Compose plugin** — the pinned KiCad CLI container
  ([`compose.yaml`](compose.yaml)) is the only supported KiCad.
- **VS Code plus the PlatformIO extension** — PlatformIO is the firmware project
  interface, and [`firmware/`](firmware/) opens directly in it.
- **`gcc`** — the host test suites compile with the system compiler; `CC` / `CXX`
  override it.

PlatformIO Core is **not** vendored. Provision it pinned (6.1.16) into a venv and
point `PIO` at that executable; the exact recipe is in the header of
[`firmware/scripts/build.sh`](firmware/scripts/build.sh). The first cross-build
downloads the `teensy@5.2.0` platform and the `arm-none-eabi` toolchain into
`PLATFORMIO_CORE_DIR`.

## Firmware

### Host test suites

**The host suites are the primary loop and need no hardware.** They compile the
portable C code with plain `gcc` under `-Wall -Wextra -Werror` plus
ASan/UBSan, so most firmware work is provable on a laptop:

```bash
firmware/scripts/test.sh --environment native --suite http
```

**Both flags are required.** [`firmware/scripts/test.sh`](firmware/scripts/test.sh)
prints `usage: test.sh --environment native --suite <suite>` and exits 2 for
anything else — there is no default suite and no bare invocation. Exit 3 means a
suite's prerequisite artifact or tool is absent; any other non-zero status is the
suite's own.

Discover suite names from the `# Suites:` block in that script's header or the
suite table in [`firmware/README.md`](firmware/README.md). The `registry` suite
fails when those two and the script's `case` arms disagree with the directories
under `firmware/test/`.

Every suite is host-only and self-contained except `itcm-placement`, which
inspects the linked objects of an optical image; without a current cross-build it
exits 3 and prints the build command to run.

The SD-hosted web pages (status, DSP editor, replay manager, upgrade) are host JS
covered by vitest in [`test/sd-editor/`](test/sd-editor/). This repository is its
own vitest root (the root `vitest.config.js`); the suite runner resolves the
superproject for `pnpm` when this repo is a submodule:

```bash
firmware/scripts/test.sh --environment native --suite sd-editor
```

`pnpm exec vitest run --root git_modules/aius` from the repository root is the same
run. The upstream repository's own vitest include is that repository's graph anchor and does
not list this tree, so nothing else runs these suites.

### Cross-building for the Teensy

[`firmware/scripts/build.sh`](firmware/scripts/build.sh) wraps `pio run` and, on a
successful link, prints the toolchain version and the per-bank memory report:

```bash
firmware/scripts/build.sh                       # teensy41-release
firmware/scripts/build.sh teensy41-optical
firmware/scripts/build.sh teensy41-optical-fault 3
```

**Read the per-bank report, not just the exit code.** It breaks the image down
into FLASH, ITCM, DTCM and OCRAM — the four banks the design is budgeted
against — and annotates the DTCM decoder arena read straight from the ELF. FLASH
is roomy; ITCM and DTCM are not, and a change that links today can leave no
stack tomorrow. The script exits 2 when a bank is over capacity, 1 on a build
failure, 3 on a missing tool, 4 on a usage error, and 75 when another session
holds the optical build lock (optical builds are serialized by a `flock` so two
agents sharing this tree cannot clobber `.pio/build/<env>`).

Five environments are defined in
[`firmware/platformio.ini`](firmware/platformio.ini), whose comments record why
each flag is set — read them before changing one:

- **`teensy41-release`** — the measurement-clean baseline image.
- **`teensy41-instrument`** — the same footprint plus `-g` and `AIUS_INSTRUMENT`,
  kept as a distinct binary so the release `.elf` stays measurement-clean.
- **`teensy41-capability`** — the RawHID transport used by the P6 capability gate.
- **`teensy41-optical`** — **the live runtime image.** Audio path, Ethernet, plain
  HTTP, the SD control surface, signed OTA and the flight recorder; it carries
  the custom ldscript.
- **`teensy41-optical-fault`** — the production image plus one injectable stall
  checkpoint. Select it as `build.sh teensy41-optical-fault <0..9>`; the script
  refuses to run if you export `AIUS_FAULT_CHECKPOINT` yourself.

### Flashing and bench work

- **[`firmware/scripts/flash_optical.sh`](firmware/scripts/flash_optical.sh)** —
  the button-free route for the runtime image. With no argument it flashes
  `firmware/.pio/build/teensy41-optical/firmware.hex`. It enters HalfKay over the
  CDC control protocol and verifies the image identity after the write.
  `--via rawhid` is the legacy handoff and **cannot verify what it wrote**. CDC is
  the default. `--port PORT` names the serial port; `TEENSY_LOADER` points at
  `teensy_loader_cli`. When the group login is stale, wrap it:

  ```bash
  sg dialout -c "sg plugdev -c 'firmware/scripts/flash_optical.sh'"
  ```

- **[`firmware/scripts/flash_and_run.sh`](firmware/scripts/flash_and_run.sh)** —
  `flash_and_run.sh firmware.hex vectors-dir [audit-file]`. The capability-image
  path: it reboots the board through RawHID resolved by USB identity, flashes,
  waits for re-enumeration, then runs the P6 proof against the vector directory.
- **[`firmware/scripts/run_recovery_smoke.sh`](firmware/scripts/run_recovery_smoke.sh)** —
  the live recovery smoke against one deployed board:
  `--target HOST [--serial PATH] [--gates reboots,stalls,soak,dsp] [--allow-mutation] [--dry-run]`.
  **Without `--allow-mutation` the run is read-only**: reboot cycles, fault images
  and the DSP round-trip are reported unresolved (exit 3), not a pass.
  The stall gate builds and flashes fault images, so it needs the build and flash
  tools.

Bench wiring for the proto board is in
[`docs/guides/proto-optical-wiring.md`](docs/guides/proto-optical-wiring.md).

**A build that fails on a bank `ASSERT` is intended behaviour.** The next section
says why.

## Memory budgets are enforced at link time

**The linker rejects an image that would misbehave on the board.**
[`firmware/link/aius_teensy41.ld`](firmware/link/aius_teensy41.ld) is the
framework script plus a carve-out and a block of `ASSERT`s. Each one exists
because the failure it prevents already happened once, and each failure message
names the constraint. The three that constrain everyday work:

- **The OCRAM heap floor — 128 KiB.** NativeEthernet mallocs roughly 96 KiB at
  network bring-up (a 64 KiB FNET stack heap plus eight 4 KiB socket buffers). A
  heap squeezed below that made the allocation fail, which left the network
  service aborting every epoch unfed and the watchdog dropping the board into
  recovery-safe mode on **every boot**. The assert turns that into a link error.
- **The ITCM six-block ceiling.** ITCM claims FlexRAM in 32 KiB blocks and moves
  `_estack` down by one block for each. Six blocks leave the DTCM stack/locals
  zone intact; a seventh would take 32 KiB straight out of the stack shared by
  the main loop and the audio ISR.
- **The DTCM stack floor.** Nothing in the framework script bounds DTCM statics —
  the bank is declared 512K, so an over-claim links clean and hands the stack a
  sliver. The assert pins it to the smallest zone this board has demonstrably run
  with.

**As of 2026-09-02 the optical image links with 5,844 bytes of ITCM headroom
(190,764 of 196,608 used), so any new function on a hot path must be placed
deliberately.** It was 1,108 bytes before the recorder, config and auth
libraries went `FLASHMEM`, and 148 on 2026-08-05. Assume a new hot-path function
does not fit until the build says otherwise, and budget for it by pinning
something cold to flash. The remaining ITCM is dominated by code no annotation
reaches — newlib's printf family (~25 KB), FNET + NativeEthernet (~43 KB), SdFat
(~19 KB) and the vendored decoders — which only a linker-script placement rule
can move.

The remedy is placement:

- **Put `FLASHMEM` on the definition of every cold function, never only on a
  forward declaration.** GCC does not merge a `section` attribute from a
  prototype, so annotating the declaration compiles silently and leaves the
  function in ITCM anyway. Portable sources define a no-op `FLASHMEM` fallback so
  the host suites still compile Arduino-free.
- **Large buffers go in `DMAMEM` (OCRAM), not DTCM.** The HTTP request/response
  buffers, the DSP models, the replay service and the UDP buffers are all placed
  that way; a DTCM instance of the composed runtime would not fit.
- **Five post-link ITCM guards run on every optical build**
  ([`firmware/test/itcm-placement/verify_target.py`](firmware/test/itcm-placement/verify_target.py)).
  They reject `.text` from declared-cold objects, pin the mixed ingress library
  and `optical_main.cpp` to their named hot entry points, require any LDF library
  emitting `.text` to be allow-listed, and refuse an environment built with
  `-flto`. A new object under a cold engine tree fails by default, so "this is
  legitimately hot" stays a reviewable claim. Re-run them over an existing build
  with `test.sh --environment native --suite itcm-placement`.

The measured budgets and their provenance are in
[`docs/reference/firmware-io.md`](docs/reference/firmware-io.md); the audio path
they protect is in [`docs/reference/signal-path.md`](docs/reference/signal-path.md).

## Electronics

**KiCad runs only in the digest-pinned container.** KiCad 10.0.4 is pinned by
image digest in [`compose.yaml`](compose.yaml), runs network-isolated as uid 1000,
and every invocation is an ephemeral `run --rm` against a named cache volume.
[`scripts/kicad.sh`](scripts/kicad.sh) is the wrapper:

```bash
scripts/kicad.sh version
```

**The schematic is generated from a checked-in connectivity spec, not hand-drawn
in the GUI.** [`electronics/generate.py`](electronics/generate.py) reads the JSON
under [`electronics/connectivity/`](electronics/connectivity) and emits the
hierarchical root plus one sub-sheet per functional area. Edit the spec and
regenerate; a GUI edit is not reproducible and the next generate overwrites it.

```bash
electronics/generate.py --project \
  electronics/connectivity/aius-carrier.json \
  electronics
electronics/scripts/erc.sh          # exit 0 = no ERC errors
electronics/scripts/erc.sh --all    # include warnings
electronics/scripts/render.sh       # SVG per page + PDF
```

[`electronics/scripts/erc.sh`](electronics/scripts/erc.sh) is the gate: exit 0
means no errors. [`electronics/scripts/render.sh`](electronics/scripts/render.sh)
writes `aius-carrier.svg` and `aius-carrier.pdf` into `electronics/`, which must
be writable by the container uid (a normal checkout is). DRC joins the same
pattern when the PCB lands. **No GUI-only step may be the sole way to reproduce a
manufacturing result** — the same rule applies to every PlatformIO, firmware-test
and manufacturing-package workflow: persist it in a script or a checked-in job
definition. Interactive commands may diagnose a problem; acceptance evidence must
come from a committed entry point.

Pipeline details, the symbol libraries and the current ERC status are in
[`electronics/README.md`](electronics/README.md).

## Evidence handling

- **Durable binary research artifacts go under
  [`docs/reference/datasheets.md`](docs/reference/datasheets.md) as Git LFS objects** — datasheets,
  errata, reference schematics, CAD models, vendor stackups, binary test vectors.
  Firmware binary fixtures belong under `firmware/test/fixtures/`, also LFS.
- **Reviewable upstream text goes under
  [`evidence/source-snapshots/`](evidence/source-snapshots)** in normal Git, so it
  diffs. Never reformat a byte-exact snapshot.
- **Authored and reviewable sources stay in normal Git** — Markdown, CSV, JSON,
  scripts, source code, KiCad sources and patches do not belong in the LFS-only
  artifact directory.
- **Note where each item came from — source URL, part revision — in the doc that
  cites it.** That citation is the provenance record.

**This is a plain reference library, not a validated manifest system.** A previous
iteration built a heavyweight provenance apparatus around this directory; SHA
manifests, acquisition-wave audits and hash-locked evidence contracts were
deliberately removed on 2026-07-18 and **must not be reintroduced** for this
personal, non-commercial build. The rationale is recorded in
[`docs/plans/aius-hardware-spec.md`](docs/plans/aius-hardware-spec.md).

The retained material is public reference kept for personal, non-commercial
research: do not republish the bytes.

## Release gates

**No manufacturing package is released or ordered before its recorded gates
pass.** The ladder is ordered, and no later step substitutes for an earlier one:
the Teensy capability proof precedes the manufacturing schematic; electrical and
failure-mode review precedes layout; two-layer signal-integrity feasibility
precedes routing; and ERC, DRC, mechanical, DFM and independent design review
precede a prototype order. Quotations and should-cost estimates may be collected
before design release — a cost model is not an authorization.

**The two-layer board is a hypothesis, not a requirement.** Escalate to four
layers only when a documented USB, Ethernet, return-path, impedance, routing or
vendor-stackup electrical constraint fails and four layers resolves it, and record
the technical and cost delta when you do. **Placement, outline, panel, cable,
access and collision failures are mechanical: they block redesign, and changing
the layer count does not clear them.**

The gate ladder and the current state are in
[`docs/plans/aius-hardware-spec.md`](docs/plans/aius-hardware-spec.md); every open
item and defect is in
[`docs/plans/aius-open-work-register.md`](docs/plans/aius-open-work-register.md).

## Documentation conventions

- **[`docs/plans/`](docs/plans)** — future and in-flight work: the hardware spec,
  feature and defect campaigns, the cost model, the open-work register.
- **[`docs/reference/`](docs/reference)** — current-state architecture and
  firmware references: topology, interfaces and states, signal path, firmware I/O,
  the USB control protocol.
- **[`docs/guides/`](docs/guides)** — task how-tos, such as the bench wiring guide.

**Every indexed doc under `docs/` carries YAML front-matter** (`title`, `type`,
`scope`, `component: aius`, `status`, optional `subtype` / `tags` / `updated`);
placement and front-matter are validated by the repo's doc-index tool.
`README.md`, `CLAUDE.md` and this file are skipped by that validator and carry no
front-matter.

**A plan's `status` states implementation state, not document authority** —
`draft`, `in-progress`, `shipped`, `superseded` or `abandoned` — and it must be
updated in the commit that ships the work. A plan still reading `in-progress`
after its phase lands sends the next reader to redo finished work.

## Working safely in a shared tree

**Never signal, stop or restart a process you did not start yourself.** No broad
process matching, no shared-service lifecycle commands. A bench board, a soak
run, a long build or a test host may belong to someone else's session; if a
foreign process blocks you — including an optical build lock that exits 75 —
report the conflict and wait for its owner rather than clearing it.

**Commit with exact pathspecs.** This tree is shared and the Git index is shared
with it, so `git add -A` and a bare `git commit -a` sweep up work that is not
yours. Stage the explicit paths you own, inspect that snapshot, and leave every
foreign staged, unstaged or untracked path untouched. Never discard someone
else's uncommitted changes to unblock your own.
