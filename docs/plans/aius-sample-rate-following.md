---
title: Aius sample-rate following
type: plans
scope: component
component: aius
status: in-progress
subtype: implementation
tags: [aius, firmware, teensy, audio, clocking, spdif]
updated: 2026-10-07
toc: true
---

# Aius sample-rate following

Make the `teensy41-optical` output clock follow a live optical source at
32, 44.1 or 48 kHz, including the source oscillator's same-nominal ppm error.
Replay remains explicitly 48 kHz. The selected production mechanism is:

- a whole-rate PLL4 reconfiguration when the nominal rate changes;
- a live `CCM_ANALOG_PLL_AUDIO_NUM` trim when the nominal rate and structural
  plan fields are unchanged;
- the optical input remains the graph master for live input; and
- the output remains the graph master only for 48 kHz replay without a carrier.

This is literal clock following. It adds no resampler, no persistent audio
buffer and no intermediate DSP quantisation.

## Current state

Handover 2026-10-07 (orchestration run, session 170d7fc3: 4e and 4f
built); replace wholesale on the next handover.

- **Status.** Phases 1, 2, 3 DONE; 4a, 4c, 4d DONE. Phase 4 BUILT (gate 7
  at fill-write cadence on the U24 bench is its last proof, owner
  needed). **4e BUILT** (`21c8f52`), **4f BUILT** (`9c9bd88`) and **4g
  BUILT** (2026-10-10): host suites green, optical images with every
  post-link guard and OCRAM at each env's floor, delivery audits and
  closures passed; their device runs are the owner's and none has run.
  4h PLANNED: its card-swap hold narrows to accidental disconnect (owner,
  2026-10-10), so its contract is revised and takes one focused prebuild
  review before dispatch. 4b (acceptance umbrella) PLANNED: it closes
  after 4e to 4h's device runs. The owner holds two gates (below).
- **Board.** Runs `df966f17b370` (Phase 4a) in slot 1, deployed by OTA
  2026-10-06, healthy-boot self-commit. It is the owner's **fail-safe
  rollback candidate** (2026-10-07): tag `fw-rollback-df966f17b370`,
  signed container and USB `recovery.hex` in LFS with steps in
  `firmware/releases/df966f17b370/README.md`.
- **Implementation started** (owner "start", 2026-10-07), 4e first.
  **No OTA deploy until the owner's go-ahead** (owner, 2026-10-07):
  another bench round may be needed first, not on 2026-10-07. Builds,
  signing and host suites proceed; staging, rehearsal and activation
  wait.
- **Orchestration posture: checkpoint** (owner, 2026-10-10; was
  autonomous from 2026-10-06): surface each decision point and
  human-review gate as it arises and wait for the owner; give each call
  its own line, never a packed digest. Every device session (OTAs and
  source-off checks included) and anything destructive goes to the owner
  (ruling 2026-10-07).
- **Owner gates (not questions).** (1) No OTA deploy, stage, rehearsal
  or activation until the owner's go-ahead; another bench round may come
  first. (2) Every device session is the owner's: 4e runs 1 and 2 (run 1
  ends with an OTA stage and rehearsal) and its E7 verdict, then 4f's
  source-off and AC-3 runs, then 4g's run 1 (source off, after a full
  sync that uploads the bundles) and run 2 (AC-3 page loads). An E7 verdict of B2 re-plans 4f to 4h before
  their device runs (4f's body records this).
- **Next action.** Re-enter `orchestrate-plan` on this doc for Phase 4h:
  first revise its contract per "Carried in from 4g" (card-swap hold
  narrowed to accidental disconnect; memory figures from 4g's link;
  three tooling nits), take one focused prebuild review, then dispatch.
  4g's device runs 1 and 2 join the owner's queue with 4e's and 4f's.
- **What landed this run** (aius commits; each followed by a upstream repository
  pointer bump; foreign commits from the mixer session omitted):

| Commit | Contents | Proof |
| --- | --- | --- |
| `0cca005` | Phase 4a delivery-audit fixes (F3/R3 pending-open pad, N5 to N11, R4, R7, R9; bench tools R1, R2, R6, R10 to R15) | 10 firmware suites, both images, tool tests 93/71/119; delivery review FAIL, closure PASS, clean-room PASS |
| `a2c4e8c` | Phase 4c `test.sh` runs every `--suite` | registry, multi-suite exit codes, shellcheck |
| `df966f1` | owner confirms R2, R3, R13 | none needed |
| `bdfa916`, `961ac4c` | deploy record `df966f17b370`; reducer splits on pending pad; short device session | reducer `--self-test` ok; OTA self-commit |
| `5d7c05d`, `3ce0129` | aius CLAUDE.md: auth-file tools, recorder keys, glitch stamps; Phase 4d fill counters and signing form | promote diff matched B1 |
| `46456cf` | full device session; 4b widened (owner: audio hot path always has priority; OTA excluded) | device evidence 2026-10-07 |
| `a974465` to `a480243` | 4b B2 design, prebuild round 1, three investigations (network/SD, write path, SdFat DMA), owner decisions | read-only reviews |
| `9d62deb` | rollback candidate images and README, `*.ota` LFS, tag `fw-rollback-df966f17b370` | sha256 checked against the deploy record |
| `bdf9629` | re-plan: 4b umbrella, phases 4e to 4h | yaml fields checked by the author |
| `ef2c57f` | prebuild round 2 folded into 4b, 4e to 4h and Phase 5; 4e interrogate step | none |
| `6601355` | 4b and 4e to 4h contracts revised against round 2; Phase 5 gains E5 to E9, F5, F6, G6, G7, H7 to H10 | none |
| `881f57a` | prebuild round 3 (NOT READY) folded and resolved; Phase 5 gains G3, E10, E11, F7, F8 and nine corrections | reviewer citations checked against source by the reviser |
| `17e0474` | prebuild round 4 (closure; 4f READY) folded and resolved; Phase 5 gains E12, F9, F10, G9, H12, H13 | reviser checked citations against source |
| `e54ee82` | prebuild round 5 (closure, no blocker; 4f, 4g READY) written in: R4-N1 to N3, R5-S1 to S3; Phase 5 gains E13 | reviser checked citations against source |
| `6e4eff8` | clean-room round (HOLD, CR-B1 to B3, CR-S1 to S12) folded; Phase 5 gains E14, F11 to F13, G10, H14, H15 and four corrections | edited yaml blocks parse; citations checked by the reviser |
| `15edf92` | clean-room closure (HOLD, CR2-B1 E-AC-3 log compiled out, CR2-S1 FAT check from the job's own BEGIN) folded; E14, H14 amended | all yaml blocks parse; citations checked by the reviser |
| `9c12136` | clean-room round 3 (HOLD, CR3-B1 to B4, CR3-S1 to S5) folded; isolation walk rewritten as W1 to W8; Phase 5 gains E15, F14, H16; E3, H4 corrected | prototype walk over the Oct 6 optical ELF: no forbidden symbol reached with declared targets; yaml parses |
| `6c327f4` | round 3 closure (HOLD, CR4-B1 to B3, CR4-S1 to S4) folded; W5 (2) forward, per-row counts, relocation-based address-taken, chain stack bound; Phase 5 gains E16, E17, H17; H16 corrected | per-row prototype walk (`walk_cr4.py`): 447 nodes, 74 sites, 39 leaf pairs; yaml parses |
| `e520fc4` | second closure READY (4b, 4e to 4h); CR5-S1 to S4 folded (dependency-file wording, libc row 77 nodes and 12 sites in 7 functions, SHF_ALLOC relocations, FAT serial stored at mount); `prebuild-review: ready` | yaml parses; figures from `walk7.txt`, `walk8.txt` |
| `a0dcb55` | owner answers Phase 5: confirmed (E2 deferred), H4/H10/H14 overturned; 4h rewritten as copy-before-write (journal, RECOVER, unlink patch, FAT-only gate removed); confirmations recorded per phase; Phase 5 deleted | yaml parses; citations checked by the reviser |
| `4d8c36c` | focused 4h review (NOT READY, FR-B1, FR-S1 to S13) folded: safe-copy ordering, in-place overwrite with sector 0 last, 3N+17 primitives (89, 90 epochs for dsp.json); owner scopes the rename-gap fix to settings files | 4h yaml parses; parser trailing-byte rejection checked in source |
| `368cecb` | 4h closure READY; FR2-S1 to S3 folded (put keeps the safe copy; 0.78 s; first-save fault over-count accepted); all of 4b, 4e to 4h READY | reviewer walked every cut in both orders and both probes against SdFat source |
| `d6f4f58` | owner "start"; no OTA deploy without the owner's go-ahead | none |
| `21c8f52` | Phase 4e BUILT: vendored SdFat and SD (DMA, card-wait hook, DTCM bounce), producer-yield library, adapter fault split and mode route, windows and yield points, per-env OCRAM floors, producer-isolation and stack-bound suites, delivery-audit fixes (D-B1 MPU trap, D-S1 to D-S4) | 22 host suites (verifier run), all four images and every post-link guard, delivery audit closure PASS |
| `9c9bd88` | Phase 4f BUILT: link-time admission (route and SD-start shedding, 503 audio_busy with Retry-After), live_owns_ring, time-aged bucketed cost windows, FIFO start-gate deferral with bring-up latch, render and storage-diag yields, /api/status/brief, shared web poller with 503 skip and GET timeout, capture.py 503 skips, dsp_model_pad.py, D-S7, delivery-audit fixes (F-B1, F-B2, F-S1 to F-S6) | 14 host suites (verifier run), all four images, OCRAM at every floor, delivery audit closure PASS |
| this commit | handover: this state; 4g gains starting figures and three carried-in items | none needed |

- **Uncommitted tree.** Nothing of this session's. Foreign and left
  alone: `docs/plans/aius-sd-mixer-dashboard.md` and
  `firmware/sd/www/mixer.js` (the mixer session's, uncommitted).
- **Hook item.** `stop-guard.action-reverify` is item 11 of
  `src/tools/hook/docs/plans/hook-daemon-feature-backlog.md` (owner:
  plan, do not dispatch).
- **Evidence in scratch** (reaped about 2026-10-14): full session
  `.scratch/63e414f8-e0a8-416d-8629-cda946de784a/session-4a-full/`, the
  underrun capture `underrun-0811/`, the B2 disassembly `optical.dis`;
  and the prototype isolation walks behind 4e W1 to W8
  (`walk_cr3.py`, `walk_declared.py`, `walk_sdroots.py`, `walk_scc.py`,
  disassembly `opt.dis`) in
  `.scratch/170d7fc3-409e-4126-8b10-6d8f8a95620b/`, with the 4e and 4f
  worker handoff notes (`4e-handoff.md`, `4f-handoff.md`,
  `4f-audit-findings.md`), build logs and verifier logs; everything
  durable from them is in 4e and 4f's bodies.
  Everything durable is in Phase 4 §Device evidence and Phase 4b.
- **Editor diagnostics.** clangd reports missing `Arduino.h` and library
  headers under `firmware/`: an editor include-path gap only;
  `firmware/scripts/test.sh` and the PlatformIO builds are the truth.
- **Traps.**
  - A firmware change that no host suite runs (boot, `setup()`, linker
    layout) can pass every suite and still fail the board: 4e's stack
    paint wrote the core's MPU trap at `_ebss` and would have boot-looped
    (4e §Memory). Read the delivery audit's ELF-level checks, not just
    suite results.
  - Rebuilding images overwrites `firmware/.pio/build/*`; the
    producer-isolation and stack-bound real-ELF cases read whatever was
    last built, so never rebuild while an agent is reading those ELFs.
  - The path-ownership hook can hold a file another session wrote even
    when it is clean against HEAD (`test/sd-editor/mixer.test.js` was held
    by session 74c6ef4f until released); retry after the owner finishes,
    never write around it.
  - A verifier quoting "Ran 25 tests" for producer-isolation is only the
    fixture half; the arm runs `test_real_elf.py` too (7 more). Read the
    log's every "Ran N" line before trusting a count.
  - `firmware/scripts/test.sh` needs `--environment native`; several
    `--suite` flags now each run (4c). Python tool tests run with
    `python3 -m unittest` (pytest is absent); `unittest discover` on
    `firmware/test/rate-runner` also picks up `test_recorder_pull.py`.
  - A raw `git status`/`git diff` on the working tree is hook-denied; use
    `the repo tooling git -C git_modules/aius status --short`.
  - "Unpolled" is only unpolled if no browser has the web UI open: the
    editor polls meters about 2.6/s and status 1/s. Check
    `http_requests` per poll before trusting a quiet segment.
  - A poller job longer than about 25 min is killed with its background
    job; launch the next segment immediately on the completion notice.
  - A sub-agent's leftover Monitor keeps re-delivering its report; ask it
    to stop its own monitor rather than killing it.
  - The host clock is UTC; the owner speaks UK local time (BST in
    October). Map board `millis()` stamps to wall time from one bracketed
    `/api/status` read (`dark_open_start_ms + dark_open_ms` is "now" while
    the carrier is down).
  - Switching the source off freezes zero-frame `carrier_loss` captures that
    evict real ones. Pull with `aius-recorder-pull.py --auth-file
    .secrets/aius-sd-auth` before switch-off; `--slot 99` writes the bank
    header (`--header-json`) and exits without pulling audio.
  - The SD token never goes on a command line, nor through `$(cat …)` (the
    bash guard denies command substitution); use the tools' `--auth-file`.
  - A streaming follow (`tail -f`) is hook-denied; a Monitor over the
    poller's change log uses a `wc -l` polling loop.
  - `clock.trim_updates` 0 through a whole session is expected inside the
    500 mHz deadband, not a dead tracker; that is the defect Phase 4 fixes.
  - The recorder taps the stream before the TX ring, so a capture does not
    contain a TX underrun's gap, only the programme around it.
  - `CLAUDE.md` is protected: change it only via `the maintainer
    promote` (4b's ship step does).
  - Polling `/api/status` is not neutral: a render delays the AC-3
    producer and can trigger the underrun being measured. Note each poll
    instant against `underrun_episodes[].start_ms` when reading a capture.
  - A fill window that contains an underrun blends pre- and post-step
    minima (+8.5 instead of +127). Read the step across the next window.
  - An AV-chain wake or CEC hypothesis does not apply: the owner confirms
    no HDMI-CEC link and no optical carrier when idle. False replay carrier
    stops are on-board (output-jitter Phase K).

## Why this plan was rebased

The first draft selected a fixed 48 kHz carrier plus an approximately 88 KiB
polyphase ASRC. That design is not implementable against the production image
that now exists.

The exact running `bc0523288` image reports:

| Bank | Used | Capacity | Free |
|---|---:|---:|---:|
| ITCM | 191,100 B | 196,608 B | 5,508 B |
| DTCM | 289,824 B | 327,680 B | 37,856 B |
| OCRAM | 392,832 B | 524,288 B | 131,456 B |

`test/memory/verify_target.py` enforces at least 131,424 B of OCRAM heap for
NativeEthernet. The production image therefore has **32 B of spendable internal
OCRAM**, not the approximately 310 KiB the draft costed. The draft's
coefficient table, delay and staging buffers cannot land there.

The DSP contract changed as well. The delivered chain stays in `double` through
input EQ, downmix, output EQ and limiter, then quantises to the 24-bit integer
domain once. A Q31 resampler between downmix and output EQ would reintroduce an
intermediate quantisation the single-quantise work deliberately removed.

Two more draft assumptions became false when the 24-bit transmit path landed:

- transmit underruns and the entire output queue are already exactly countable;
- the transmit DMA drains the 24-bit ring independently of
  `AudioStream::update_all()`, so the input may remain graph master while the
  output consumes at its own rate.

The last point removes the draft's claimed need for permanent output-master
polling. That inversion would now be harmful: a normal 44.1 kHz receive block
not yet filled when polled at 48 kHz is currently recorded as
`MISSING_LIVE`, which increments ingress generation and resets the pipeline.
Keeping the input as live master avoids the false gap rather than teaching
every downstream service to distinguish one.

## Reproduced defect and selected actuator

The CM6206 optical loop reports about 48,021 Hz while the Teensy output is fixed
at 48,000 Hz. During continuous DTS decode, the compressed ingress ring grew by
about 1,280 words every 30 seconds, 42.7 words/s, until it overflowed after
roughly 137 seconds.

A tagged +438 ppm `PLL_AUDIO_NUM` write then held the same live loop for 1,857
seconds:

- no ingress drop or generation reset;
- ingress high-water fixed at 442 words;
- no output drop, AU abandon, budget overrun or watchdog reset;
- AUDIO max 2.401 ms; and
- carrier remained locked.

That experiment proves the live numerator actuator when the old and new plans
have the same structural fields. It did not capture the exact waveform at the
write boundary, so production marks one block invalid while a trim lands.

Whole-rate PLL reconfiguration is the corresponding nominal-rate actuator.
The vendor `config_spdif3()` already derives valid PLL and transmit-divider
settings from `fs`; production factors that arithmetic into a pure clock plan
and a bounded target apply operation. A nominal change may interrupt the
outgoing carrier while PLL4 relocks. Source changes already require
classification reset and downstream reacquisition, so preserving a fixed
carrier is not worth violating the memory and DSP contracts above.

## Runtime rate model

There are four distinct values. They must not be collapsed again:

| Value | Meaning |
|---|---|
| configured rate | the default model/replay rate, 48,000 Hz |
| carrier measurement | the receiver DPLL's measured physical input rate |
| nominal stream rate | 32,000, 44,100 or 48,000 Hz |
| output target | the physical transmit rate requested from PLL4 |

The DSP designs coefficients from the **nominal stream rate**. PLL4 follows the
**physical carrier measurement**. A 48,021 Hz physical carrier still carries a
nominal 48 kHz programme; EQ at 48,021 would be semantically wrong.

### Decoder and passthrough contract

- PCM passthrough is stamped with the accepted nominal carrier rate.
  `AIUS_PIPELINE_PCM_SAMPLE_RATE_HZ` is deleted.
  The runtime value defaults to 48 kHz and survives pipeline resets, matching
  the tracker's hold-last-rate carrier-loss contract.
- DTS already publishes its bitstream header rate. A DTS block whose rate does
  not equal the accepted nominal carrier is refused.
- Phase 1 widened AC-3 to 32, 44.1 and 48 kHz. The adapter retains the
  accepted `a52_syncinfo()` rate and publishes it on all six blocks.
- Replay has no embedded rate metadata and remains 48 kHz by contract.

At `enqueue()`, immediately before any staged config or DSP state advances:

1. block rate equals the active nominal output rate: process normally;
2. block rate is a supported rate but differs: count mismatch, refuse the block
   and keep the transmit gate closed;
3. block rate is unsupported: count, emit one rate-unsupported event per
   episode, refuse and keep the gate closed.

A refused block advances no config, DSP, meter, recorder, measurement or output
state. The unity path at 48 kHz remains sample-identical to the current image.

### DSP configuration

`AiusOpticalConfig.sample_rate_hz` remains the generated default and replay
rate. `apply_config()` additionally receives the active nominal stream rate and
uses it for:

- input EQ;
- output EQ;
- LFE fold;
- limiter state;
- meter rate/window; and
- microseconds per frame.

The limiter already receives `block->sample_rate_hz`; this change removes the
remaining config-rate sources. A rate change applies through the existing
block-boundary seam and clears rate-dependent filter state.

## Stable-rate tracker

The tracker is portable C with no register access. It consumes:

- raw carrier lock;
- carrier rate in millihertz;
- replay/live source state; and
- monotonic milliseconds.

The target adapter reads millihertz directly from `SPDIF_SRFM`:

```text
fs_millihz = F_BUS_ACTUAL * SPDIF_SRFM * 1000 / (2^27 * DPLL_GAIN)
```

With the shipped gain of 8, one `SRFM` LSB is about 0.14 Hz. Integer
millihertz preserves that information instead of rounding it to the existing
`sampleRate()` integer.

### Detection

- Snap to the nearest of `{32000, 44100, 48000}` within ±2%.
- Require at least eight consecutive locked samples with the same nominal
  result and, for a supported rate, a complete 1,000 ms interval whose physical
  measurements span no more than 1,000 millihertz. A wider span restarts the
  supported candidate at the newest sample. Count-only debounce remains for an
  unsupported episode.
- On the **first** locked sample that snaps to a different nominal rate, or
  cannot snap to a supported rate, immediately set `carrier_ready = false` and
  request physical quiescence. Debounce delays acceptance and apply; it never
  keeps the old plan ready during an in-lock rate change.
- Hold the last applied rate on carrier loss but make the live source not ready.
- Accept at most one nominal-rate change per second.
- After initial acceptance, average every full one-second measurement window.
  Request a same-nominal target update when the mean differs from the applied
  target by at least 50 millihertz. The planner, not the nominal label, then
  decides whether that update is a live numerator trim or a structural replan.
- Unsupported locked samples form their own debounced episode and never open
  the transmit gate.

Candidate stability may outlive every bounded audio ring. While a live
supported candidate is unaccepted, the AUDIO producer releases incoming
`AudioStream` blocks without publishing them and without recording a gap. The
first locked changed/unsupported-rate sample closes the physical and producer
gates and requests one audio reset; acceptance of that same nominal consumes the
reset credit instead of resetting classification twice. Carrier loss does not
claim that credit: the existing debounced relock/replay paths remain its sole
reset owners. Producer ingress reopens only after the desired plan and its
dark-transition boundary are complete. A same-nominal structural replan closes
only the physical gate and continues buffering live input, because it preserves
decoder generation. A persistent clock-apply fault also closes producer
admission and owns one coalesced audio reset for the fault episode (or consumes
the reset already requested by nominal acceptance) before recovery can reopen
it. Replay ingress is independent of this live-only latch.

The tracker returns decisions; it never writes a register. Golden tests cover
lock flap, unsupported input, all three rates, the ±2% boundary, integer wrap,
one-second rate limiting, millihertz averaging and trim hysteresis.
The in-lock 48→44.1 and supported→unsupported goldens prove readiness falls on
sample 1. A supported rate cannot return before the one-second stable interval
plus successful plan apply/reset and new stream-rate agreement; unsupported
reporting retains the eight-sample boundary.

Every definition in `lib/aius_clock/src` is `FLASHMEM` on the Teensy build and
empty-annotated on the host, following the existing portable-C cold-library
pattern. `test/itcm-placement/verify_target.py` adds `aius_clock` to its
`COLD_TREES`; a green target build must prove the new object contributes zero
ordinary `.text` to the 5,508-byte-free ITCM bank.

## PLL clock plan

The pure clock planner accepts nominal Hz plus target millihertz and produces:

- PLL4 integer divider;
- numerator and denominator;
- `SPDIF0_CLK_ROOT` pre/post dividers;
- `SPDIF_STC[TxClk_DF]`; and
- IEC 60958 consumer channel-status sample-rate bits.

Use a denominator of 1,000,000. For the three nominal rates, the existing PJRC
selection yields an exact structural relation:

| Nominal | multiplier `k` | TX divider |
|---:|---:|---:|
| 48,000 | 14 | 28 |
| 44,100 | 15 | 30 |
| 32,000 | 21 | 42 |

```text
PLL4 = target_fs * 256 * 4 * k
SPDIF0_CLK_ROOT = PLL4 / 8
TX bit clock = SPDIF0_CLK_ROOT / (2 * k) = target_fs * 64
```

The 32 kHz multiplier deliberately differs from the vendor helper's nominal
`k = 20`: at the accepted -2% edge, 20 would request 642.2528 MHz and violate
PLL4's 648 MHz lower bound. `k = 21` keeps the entire accepted 32 kHz band
legal. The planner uses checked 64-bit integer arithmetic and nearest rounding.
Tests assert all three rates at both ±2% edges, the 648–1,296 MHz PLL range,
numerator bounds, millihertz reconstruction error and channel-status codes.
They also straddle every integer-divider crossing inside the accepted bands:
43,750 Hz for 44.1 kHz, about 48,549.107 Hz for 48 kHz, and about 32,366.071
Hz for 32 kHz. The below/above plans must reconstruct their targets and must
compare structurally unequal.

### Apply policy

- The first accepted live rate performs a full apply. This replaces the
  boot-time denominator of 10,000 with the production denominator and happens
  while the physical transmit gate is still closed.
- A nominal-rate change first closes the physical gate and sets S/PDIF
  validity invalid, then flushes the
  24-bit transmit ring **without clearing its lifetime underrun counter**,
  requests the existing audio-generation reset, forces PLL4 through the
  complete plan, and arms the exact 512-frame invalid hold below.
- A same-nominal target update compares `DIV_SELECT`, denominator, root
  dividers and TX divider with the applied plan. If all are unchanged, it
  **keeps the physical pad carrying the continuous carrier and preserves the
  ring**. The checkpoint queues the numerator, and the TX DMA ISR consumes
  that request at its next 128-frame period boundary: it marks IEC validity
  invalid, writes `CCM_ANALOG_PLL_AUDIO_NUM`, and starts the exact four-period
  invalid hold below. It does not reset classification or audio generation.
- If any structural field differs, including an integer-divider crossing
  inside one nominal band, the update uses the bounded dark full-plan path.
  It preserves the ring and decoder generation because the nominal stream rate
  did not change, but reopens the physical gate only after lock, channel-status
  agreement and the four-period invalid hold.
- Same-nominal target updates are rate-limited to one per 10 seconds after
  initial acquisition. `trim_updates`, `structural_replans` and
  `invalid_windows` are separate counters so status can prove the chosen path,
  cadence and every declared validity interval.
- A 48 kHz replay request restores the exact 48,000,000 millihertz plan before
  the output takes graph-master responsibility.
- The live optical input retains graph-master responsibility. The replay
  ownership reconciler remains the only writer of the master flags.

No operation touches OCOTP. No vendor snapshot is edited.

The production full-PLL apply does **not** call the retained helper's unbounded
`while (!LOCK) {}` loop. It polls for at most 1,000 microseconds, over twice the
RM's <469 microsecond maximum, using a wrap-safe cycle deadline. A timeout
returns `AIUS_CLOCK_APPLY_LOCK_TIMEOUT`, leaves PLL4 bypassed, validity invalid
and the physical pad dark, increments one fault episode and emits
`AIUS_EVENT_CLOCK_FAULT`. The epoch continues so USB/network diagnostics remain
reachable; it never waits for the watchdog. The target-adapter host suite drives
a fake register/clock trace through immediate lock, last-cycle lock and
never-lock outcomes.

“One block invalid” means **four complete 128-frame TX DMA periods (512
transmitted frames)**, not one TX DMA period and not AC-3's 256-frame emitted
slice. An asynchronous checkpoint counter cannot prove that interval: the
first interrupt may already be pending and each ISR fills the inactive half.
The adapter therefore owns a volatile boundary request plus
`invalid_periods_remaining`. At an ISR entry, before filling the now-inactive
half, it first acknowledges the period completed by any existing hold. It then
consumes a new request: sets `VALCTRL` invalid, performs the queued live
numerator write when present, and arms four periods **without decrementing at
that same boundary**. Each of the next four ISR entries acknowledges one
completed 128-frame period; the fourth clears `VALCTRL`. A nominal full apply
queues a hold-only boundary request after its successful lock. A live trim
queues the numerator itself, so validity becomes invalid before the write at
the same TX boundary. Failed full applies leave validity invalid and queue no
clearable hold. The checkpoint reads the resulting `invalid_active`
acknowledgement; it does not estimate frame time from epoch cadence. On a
**nominal or structural-plan change**, the physical gate and
`SPDIF_SCR[VALCTRL]` are both closed/invalid before any NUM, DENOM, divider or
full-PLL write, and the pad still waits for tracker/plan/stream agreement after
the ISR acknowledges the 512 frames. On a **same-nominal trim**, only
`VALCTRL` changes: pin 14 stays muxed to S/PDIF, the ring continues draining
and carrier continuity is preserved.

### Correction 2026-09-07 — validity polarity, and no hold on a trim

**The paragraphs above describe the design as it was built; two of their claims were
wrong on the wire, and the firmware now behaves differently.** They are left standing
because they record what was decided and shipped.

**`SPDIF_SCR[VALCTRL]` was read inverted.** The i.MX RT1060 field means `0` =
"outgoing validity always *set*" (`V = 1`, IEC 60958 "not suitable for conversion") and
`1` = "outgoing validity always *clear*" (`V = 0`, reliable). PJRC's boot configuration
sets the bit and labels it correctly
(`evidence/source-snapshots/PJRC-SPDIF-TX-SOURCE/output_spdif3.cpp:265`), but their
`mute_PCM(true)` sets the same bit to *mute* (`:211`), and this firmware inherited that
second reading: the constant was named `AIUS_AUDIO_CLOCK_VALIDITY_INVALID_BIT`, a
transition **set** it and the hold's last period **cleared** it. The wire state that
produced: from the first full apply onward the board transmitted `V = 1` **permanently**,
with a 10.67 ms `V = 0` window per trim — the exact inverse of the intent, and a
plausible cause of dropouts on any DAC that honours validity. The constant is now
`AIUS_AUDIO_CLOCK_VALCTRL_VALID_BIT`, "invalid" clears it, "valid" sets it, and the
`clock-target` suite starts its fake register at the real boot value and asserts the
steady-state register rather than only the writes.

**A same-nominal trim no longer opens a validity window at all.** The hold exists to
warn a receiver about a discontinuous clock change, and a numerator step is not one: the
+438 ppm experiment recorded above proves the step is carrier-continuous, the tracker's
corrections are ~10 ppm, and every sample either side is perfectly convertible. The hold
therefore bought nothing and cost at least four TX periods (10.67 ms) of possible DAC
mute per trim. `aius_audio_clock_tx_boundary()` now writes the queued numerator for a
`TRIM` request and touches neither `VALCTRL` nor `invalid_periods_remaining`; the dark
full-apply and structural-replan paths keep the hold unchanged, because there the pad is
closed and the discontinuity is real. Consequences: `clock.invalid_windows` counts dark
applies only, a trim is no longer refused for four periods after its predecessor, and
the trim's own timing is served as `clock.trim_last_at_ms` (the request) and
`clock.trim_applied_at_ms` (the boundary that wrote the numerator).

The transition flush is a new `aius_spdif_tx24_flush_preserve_diag()` operation,
not `aius_spdif_tx24_reset()`. It makes the ring empty by setting
`consumed = written` and leaves `underruns` unchanged. The checkpoint-7 caller
runs after the prior epoch's AUDIO producer and before the current epoch's
AUDIO checkpoint. Epoch-thread work therefore cannot overlap the flush; the
caller additionally masks the TX interrupt across it so
`aius_spdif_tx24_fill()` cannot observe a half-rebased cursor pair. The
boot-only reset retains its existing semantics and remains the only operation
that clears all three counters.

## Gate and transition semantics

The optical transmitter opens for live input only when all are true:

- receiver DPLL locked;
- the tracker has accepted a supported nominal rate;
- the matching clock plan was applied;
- no physical-gate transition is in flight: nominal changes and same-nominal
  structural replans both wait for their four-period hold acknowledgement; and
- the last decoded/passthrough stream rate matches the accepted nominal rate.

The same-nominal trim-invalid interval is deliberately not a physical-gate
condition. It changes IEC validity only, preserving the carrier-continuous
property proven by the +438 ppm experiment.

Checkpoint 7 orders the moving parts explicitly:

1. SD/replay refill may promote `SOURCE_REPLAY`;
2. carrier displacement settles the replay source first;
3. the sample-rate service applies and verifies the desired live plan, or the
   exact 48,000,000 millihertz replay plan;
4. the master reconciler gives output graph ownership only when replay is the
   settled source, no carrier is locked and the verified 48 kHz replay plan is
   ready; and
5. the physical gate is evaluated last.

On any replay clock-apply failure, output remains non-master and the pad remains
dark. Replay data is retained rather than consumed at the last live rate.

Carrier arrival still displaces replay. Carrier loss closes the physical gate,
holds the last rate and resets the tracker candidate. A later replay restores
48 kHz before it becomes output-driven.

An accepted rate change resets classification and any partial decode unit
through the existing audio reset sequence. Old-rate words and committed output
cannot cross the change.

## Observability

`GET /api/status` gains one authoritative `clock` object:

```json
{
  "carrier_locked": true,
  "carrier_ready": true,
  "carrier_rate_millihz": 48021000,
  "nominal_rate_hz": 48000,
  "stream_rate_hz": 48000,
  "output_rate_millihz": 48021000,
  "trim_ppm": 437,
  "master": "input",
  "state": "tracking",
  "rate_changes": 1,
  "trim_updates": 2,
  "structural_replans": 0,
  "invalid_windows": 2,
  "clock_faults": 0,
  "last_clock_fault": 0,
  "mismatch_blocks": 0,
  "unsupported_blocks": 0,
  "unsupported_episodes": 0,
  "tx_level_frames": 512,
  "tx_underrun_frames": 0,
  "ingress_level_words": 256,
  "ingress_high_water_words": 442
}
```

The existing top-level integer `sample_rate` remains for compatibility and
continues to mean the rounded carrier measurement. HTTP is the authoritative
new surface; this phase does not consume the UDP payload's reserved bytes or
change its version.

Append event values without renumbering existing entries:

- `AIUS_EVENT_RATE_CHANGE = 11`;
- `AIUS_EVENT_RATE_UNSUPPORTED = 12`;
- `AIUS_EVENT_CLOCK_FAULT = 13`.

Rate/mismatch counters participate in the existing clear operation. Events are
episode-based, not per block.

## Phases

### Phase 1: runtime rate, refusal and observability

```yaml
phase: 1
title: Runtime rate contract
status: complete
unit: component-behind-interface
seams:
  - AiusRateTracker and AiusSpdifClockPlan
  - aius_pipeline_set_pcm_sample_rate
  - AiusPcmBlock.sample_rate_hz
  - GET /api/status clock
contract-artifact: spec-only
fan-out:
  - name: rate-core
    owned-paths:
      - firmware/lib/aius_clock/**
      - firmware/test/clock/**
      - firmware/test/itcm-placement/verify_target.py
  - name: codec-rates
    owned-paths:
      - firmware/lib/aius_pipeline/**
      - firmware/lib/aius_a52/**
      - firmware/test/pipeline/**
      - firmware/test/a52/**
      - firmware/tools/ac3_vectors/**
review-intensity: active
commit-checkpoint:
  green: [clock, pipeline, a52, dsp, optical-cutover, registry]
  buildable: [teensy41-optical]
  parent-commit: "feat(aius): make optical rate runtime data"
```

The parent owns the composition root, status/events, test registry,
`platformio.ini`, README and this plan. Phase 1 recognizes 32/44.1/48, preserves
decoder rates, applies DSP coefficients from stream rate, and cleanly refuses a
mismatch. It does not yet alter PLL4.

Phase 1 keeps two values explicit: accepted carrier nominal may become 44.1 or
32 kHz, but **active output nominal remains 48 kHz** until a clock plan has been
successfully applied. Therefore no non-48 carrier can satisfy
`clock_plan_applied` or live gate readiness in Phase 1; its blocks are refused
before staged config/DSP/output state advances and pin 14 stays dark. A target
structural/host integration test pins that 44.1 kHz safe behavior.

The prebuild review attacks:

- a block advancing state before the mismatch check;
- Phase 1 treating an accepted non-48 carrier as an applied output plan;
- an AC-3 block changing rate between its six output blocks;
- config and meter state still using the generated rate;
- event floods;
- status buffer overflow; and
- portable clock code leaking into ITCM; and
- a lock timeout becoming an unbounded checkpoint or event storm; and
- the 48 kHz unity path changing a sample.

### Phase 2: target clock following

```yaml
phase: 2
title: Whole-rate PLL following and live trim
status: shipped # 2026-09-04: gate 1 accepted on the D4 (b) rebased criterion (65bb582 re-run); gates 2, 4, 5, 6, 7 pass on 92aae16706f5
implementation: complete
delivery-audit: complete
clean-room-audit: complete
unit: component-behind-interface
seams:
  - AiusSpdifClockPlan target apply
  - sample-rate service at checkpoint 7
  - replay/live graph-master reconciler
contract-artifact: both
fan-out:
  - name: clock-adapter
    owned-paths:
      - firmware/lib/aius_audio/**
      - firmware/test/clock-target/**
      - firmware/test/itcm-placement/verify_target.py
  - name: rate-bench
    owned-paths:
      - firmware/tools/net/aius_rate_bench.py
      - firmware/test/rate-runner/**
review-intensity: active
commit-checkpoint:
  green: [clock, clock-target, rate-runner, tx-gate, pipeline, a52, dsp, optical-cutover, memory, itcm-placement, registry, fault-target]
  buildable: [teensy41-optical, teensy41-optical-fault-checkpoint-9]
  parent-commit: "feat(aius): follow the optical source clock"
```

The parent owns composition, transition ordering, exact-image flash, hardware
evidence, and the authoritative reference updates in
`docs/reference/firmware-io.md` and `docs/reference/signal-path.md`. The Phase 2
commit replaces their fixed-48-kHz transmit claims and diagrams with the
runtime PLL/graph-master behavior and 32/44.1/48 channel-status mapping, while
retaining 48 kHz only as the replay default and generated-config/static-assert
role. Phase 2 is software completion. The optional recovered-clock board trace
below is not a completion dependency.

The implementation, host proof, production build and checkpoint-9 fault build
are complete. Phase status is `shipped` as of 2026-09-04: the exact committed
production image cleared every in-scope device gate in
[§Acceptance](#acceptance) (gate 3 and gate 4's 32 kHz leg deferred 2026-07-31
to the 32 kHz long tail by owner decision). Gates 2, 4, 5, 6 and 7 pass on the
single image `92aae16706f5+teensy41-optical-slot-a` (§Device evidence —
2026-09-03, one-image acceptance re-run); gate 1 is accepted on the D4 (b)
rebased criterion after the 500 mHz re-run on `65bb5823c835` (§Device evidence —
2026-09-04) — its post-settle trim count is recorded but no longer gates, the
count being a proxy the source's estimate quantum cannot meet with the output
provably unaffected.

The full-PLL target apply and checkpoint-side request functions live in
`lib/aius_audio/src/aius_audio_clock.cpp`, carry `FLASHMEM`, and that exact
object is added to the ITCM verifier's `COLD_OBJECTS`. The mixed
`aius_spdif_sources.cpp` object retains the volatile boundary/invalid-period
state and the deliberately hot ISR sequence: acknowledge an old period, set
validity invalid, perform a queued numerator write, and arm a new hold. No
checkpoint helper is added to that mixed object. `clock-target` uses a fake
register trace to prove that boundary ordering and that the arm boundary is
not counted as one of the four completed periods.

The prebuild review attacks:

- reprogramming PLL4 while an ISR observes a half-applied plan;
- marking validity or closing the physical gate only after a clock write;
- routing the physical pad dark for a same-nominal NUM trim;
- reopening the gate before rate/classification agreement;
- replay starting at the last live rate;
- granting replay graph ownership after a failed 48 kHz apply;
- stale channel-status bits;
- an old-rate ring or partial access unit crossing a change;
- a transition flush clearing lifetime diagnostics or racing the TX ISR;
- trimming with the boot denominator still active;
- confusing the 128-frame TX period, 256-frame AC-3 slice and 512-frame
  transition hold;
- millihertz overflow/rounding; and
- calling the full PLL path for every one-second trim.

### Phase 3 Phase-corrected fill sensor and noise capture ✓ SHIPPED 2026-10-05

```yaml
phase: 3
title: Phase-corrected transmit-ring fill statistic, observed only
status: shipped # 2026-10-05: deployed 443a0ae then 0af7a82; capture recorded in §Measured below
prebuild-review: ready # clean-room 2026-10-05: text-only findings folded (DMA-position sensor, statistic spec, status limit, device criteria)
outcome: >-
  GET /api/status serves a transmit-ring fill statistic with about one-frame
  resolution that reads the same floor for PCM and AC-3 (DTS by host proof
  only), and one listening-room session has measured its window-to-window noise
  and drift, the inputs Phase 4 derives its gains from. No audio behaviour
  changes and no ISR change.
unit: component-behind-interface
seams:
  - TX DMA read position (AiusSpdifTx24Hook accessor, aius_spdif_sources.cpp)
  - fill statistic at the governor tap (optical_main.cpp)
  - GET /api/status clock and firmware/sd/www/openapi.json
targeted-proof:
  - host: tx-fill-sensor suite (cases below)
  - target: tx-gate, tx-governor, clock-target, itcm-placement, registry, sd-editor green
  - device: one listening-room session of 45 min or more spanning a PCM to AC-3 change, polled and unpolled segments, criteria below
contract-artifact: spec-only
fan-out:
  - name: fill-sensor
    owned-paths:
      - firmware/lib/aius_audio/src/aius_tx_fill_sensor.h
      - firmware/test/tx-fill-sensor/**
review-intensity: active
commit-checkpoint:
  green: [tx-fill-sensor, tx-gate, tx-governor, clock-target, itcm-placement, registry, sd-editor, optical-cutover]
  buildable: [teensy41-optical, teensy41-optical-slot-a]
  parent-commit: "feat(aius): phase-corrected transmit-ring fill statistic"
```

**Owner decision (2026-10-05): build now, deploy over OTA when the board is
idle** (no carrier, nothing playing; check `/api/status` `carrier` 0 first),
then the capture session at the owner's next viewing.

**Remaining steps, in order:**

1. ~~OTA the committed image when the board is idle.~~ Done 2026-10-05
   08:19 UTC: `443a0ae5cb26+teensy41-optical-slot-a`, slot 2 to 1, `ab.sequence`
   13 after the healthy-boot self-commit, `rollback_observed` false. Idle
   first read: fill keys present, `fill_level_q4` null and `fill_windows` 0
   (gate closed, as designed), `status_body_peak_bytes` 8,592 with no carrier.
2. ~~Run the capture session and reduce it.~~ Done 2026-10-05 on
   `0af7a8244081`: 20:20:45 to 21:07:21 UTC (35 min at 1 Hz, 11.6 min at
   30 s) plus an 8 min 1 Hz PCM top-up (21:25:04 to 21:33:03).
3. ~~Record the inputs and ship.~~ See §Measured below.

**Delivery decisions (2026-10-05).**

- **Governor replay target: real.** Code read confirms the governor trims a
  clean replay from its 768-frame floor toward 128 about 4 s in. Filed in the
  [register](aius-open-work-register.md#open-defects--runtime), fix planned as
  [`aius-output-jitter.md`](aius-output-jitter.md) Phase J. Not fixed here.
- **Invalid window serves `null`** (owner): a window that closes with fewer
  than 8 valid sub-windows clears `fill_level_q4` until the next valid window,
  so a 1 Hz reader never records a stale value. "The last valid window
  statistic" below means the latest window, when valid.
- **Status size** (owner): top-level `status_body_peak_bytes` serves the
  largest body of the previous renders this boot (a body cannot state its
  own length, so the first render reads 0); the build-time bound moved to
  Phase 4. The fill keys add at most 162 B and the peak key about 35 B, to a
  live body of 10,488 B (older image).
- **Stall unit per class** follows the IEC 61937 repetition period from
  `pipeline_diag.codec_pc`: PCM the block, AC-3 1,536, E-AC-3 6,144, DTS
  512, 1,024 or 2,048 by Pc. The mapping lives in the pure header so the host
  suite exercises the firmware's own table. Known limits: E-AC-3 and
  unknown-compressed units make the stall test lenient (up to 138 ms) for
  sources sending shorter units. The tap's choice of class (it reads
  `codec_pc`, which can be stale from an earlier session at AC-3 onset) and the
  held-block skip of the post-commit call are untested on the host. A wrong
  exclusion shows in the capture's per-class exclusion rate; check it there.
- **Gate close resets the sensor** at `tx_gate_actuate(false)` as well as at
  the next held block, because a closed gate may never deliver one.
- **A governor trim empties the window but keeps the fault latches**, so a
  fault between the trim block and the next still excludes.
- **The plan-apply ring flush needs no sensor reset of its own.**
  `clock_flush_ring_preserve_diag()` (one caller, `clock_apply_desired_plan`)
  always runs after `close_stream_rate_gate()`, whose gate close resets the
  sensor. Sampling resumes only at the next gate open.
- **The reducer classes by codec**, not just PCM versus compressed: `ac3`,
  `eac3`, `dts` from `codec_pc`, `compressed-unknown` before the first burst.
- **`optical-cutover` refreshed:** its clock key set and event table were
  stale before this phase (25 keys, events 14 to 18); now green and in this
  phase's green list.

**Defect.** The tracker retargets the output only when the one-second carrier
estimate leaves a 500 mHz band around the applied target
(`firmware/lib/aius_clock/src/aius_clock.c:258`). A true source offset inside
the band is never corrected, and the ring integrates it without bound: the
deadband comment's "bounded by the 10 s cadence" holds only while trims
happen. The governor cannot help; it trims an elevated floor down
(`firmware/lib/aius_audio/src/aius_tx_governor.h`) and an underrun merely
voids its window. listening-room evidence, 2026-10-04 on `cd7ad191ac77`: carrier
settled 280 mHz below the output, `trim_updates` 0 for 49 min, the ring floor
stepped 256, 128, 0, and 128-frame underruns followed every 489 to 567 s
([register entry](aius-open-work-register.md#open-defects--runtime)). This is
the residual risk below ("`SRFM` jitter may require ingress-slope feedback")
arriving from production evidence, on the transmit side.

**Prebuild review, round 1 (2026-10-04): HOLD.** The first design (window
minimum of the pre-commit level, proportional 500 mHz per 128 frames, 256-frame
setpoint) was refuted on four blocking counts, all verified against the code:

- the ring level only moves in 128-frame TX periods
  (`firmware/lib/aius_audio/src/aius_spdif_tx24.h:293-320`), so a
  proportional law at that gain is a ±500 mHz relay that limit-cycles and
  never converges;
- it had no authority past ±500 mHz, where the governor (reading 512 > 384)
  or an underrun takes over;
- the tracker's `applied_target_millihz` and cadence timer are refreshed from
  the applied PLL target (`firmware/src/optical_main.cpp:8556-8564`,
  `firmware/lib/aius_clock/src/aius_clock.c:344-345`), so a composed target
  would trip the 500 mHz hysteresis every 10 s and starve centre retargets;
- the -500 mHz opening transient from a 128-frame gate-open floor breaches
  gate 1's 1,000 mHz follow-error bound.

The should-fix findings (event flood through the `target_update` branch,
unhandled trim refusals, a reset leaving the offset on the wire, replay
emission dropping `g_clock_replay_ready`, a composed target outside the plan
band faulting, a host proof blind to all of these, an incomplete green list,
the 256 setpoint's +2.67 ms latency) are folded in below.

**Prebuild review, round 2 (2026-10-04): HOLD on one count.** The round-1
revision used a window MEAN of phase-corrected samples against a setpoint
learned at gate open. The mean depends on decoder class and scheduling: an
AC-3 access unit yields six 256-frame blocks, one per service call
(`firmware/lib/aius_pipeline/src/aius_pipeline.c:725-726`), committed in quick
succession (the capture confirms how quick), so its samples
read F, F+256, ..., F+1280 (mean about F+640), while PCM reads about F. A
PCM-to-AC-3 change inside one lock does not close the gate, so a PCM-learned
setpoint would drive the output to the clamp and pull the real floor down by
about 640 frames. The governor header rejects a mean for the same reason
(`firmware/lib/aius_audio/src/aius_tx_governor.h:24-28`). Resolved below with
a phase-corrected minimum and a constant setpoint. Round 2 also showed the PI
targets depend on the statistic's noise, which no one has measured; hence
this phase measures before Phase 4 actuates.

**Prebuild review, round 3 and clean-room (2026-10-04 and 05).** Round 3 found
no blocking issue; its should-fix items (per-relock occupancy spread,
class-aware stall test, empty-ring reading, trough-count bias, the ISR's
non-TRIM-is-HOLD branch) are folded below and into Phase 4. The clean-room
confirmation held Phase 3 on text only: an ISR stamp needs a period
measurement that cannot be built as written, the status limit was the wrong
constant, and the statistic was underspecified in six places. It held Phase 4
on two counts: a constant setpoint against a random gate-open occupancy pins
the output at the clamp after every relock, and "commit on request success"
was ambiguous between controller and emission state. All are resolved below.

**Sensor.** The ring level is the integral of the rate error. Plant: a rate
error of `u` mHz moves the ring by `u / 1000` frames/s.

- **Played fraction from the DMA position, no ISR change.** The TX DMA's
  current source address (`dma.TCD->SADDR`) says exactly how far it has read
  into the half it is playing; the ISR already uses it to pick the half to
  fill (`aius_spdif_sources.cpp:234-239`). A static accessor on the existing
  access shim (`AiusSpdifTx24Hook`, `:209-214`) returns that offset; `phi` is
  the offset within the current half over the half's 128 frames. No stamp,
  no period measurement, no ITCM bytes. The accessor is `FLASHMEM` (it runs
  per producer block from the flash-resident `enqueue`); the map file is
  checked so neither it nor the reduction is emitted into `optical_main`'s
  default `.text` (ITCM).
- **Read.** At the governor tap (`firmware/src/optical_main.cpp:2059`) read
  `ring->fills`, the level, the DMA offset, then `ring->fills` again; retry if
  it moved, three attempts in all. A sample whose three attempts all tear is
  dropped and counted (`fill_torn_reads`). The one tear this cannot see (DMA
  already in the next half, ISR not yet run) reads 128 high, which a
  minimum ignores.
- **Correction.** `fill()` hands each DMA half one period ahead
  (`aius_spdif_sources.cpp:230-269`), so the unsent frames are the ring level,
  128 in the next half and `(1 - phi) * 128` still being read: occupancy is
  `level + 256 - phi * 128`. `fill()` pads an underrun with silence and
  advances `consumed` only by what it took (`aius_spdif_tx24.h:296-320`), so
  an empty ring reads `256 - phi * 128` (128 to 256); the arithmetic is
  signed as a defence only. Measured 2026-10-05: healthy AC-3 troughs read
  134 to 174, inside that band, so an empty ring at a pre-commit sample is
  normal for AC-3 and not a fault. The padded silence was played from no
  input, so once the producer resumes the occupancy stands **128 higher and
  stays there**: each underrun is a +128-frame step, not a low reading.
- **Statistic.** Its own 192,000-frame window of committed frames, with its
  own boundaries (not the governor's, which voids a whole window on an
  underrun, `aius_tx_governor.h:73-75`), split into 16 sub-windows of 12,000
  frames. Each sub-window's value is the minimum occupancy over its pre-commit
  samples. The window statistic is the mean of the non-excluded sub-window
  minima, exact in 1/16 frame; a window with fewer than 8 valid sub-windows
  is invalid (served as such, and Phase 4 treats it per its all-excluded
  rule). A minimum reads the floor for every class; averaging minima recovers
  resolution.
- **Exclusion** (per sub-window, counted): an underrun, a missed fill, or a
  production stall. The stall test: a gap between accepted blocks longer than
  the duration of the last accepted block's frames at the stream rate (PCM's
  512-frame block, an AC-3 burst's 1,536, the pipeline's unit for E-AC-3 and
  DTS) plus a fixed 10 ms allowance. Not the 40 ms `AIUS_INPUT_GAP_ALERT_US`
  alert (`firmware/src/optical_main.cpp:1539-1540`), which normal DTS-2048
  and long-burst E-AC-3 cadence exceeds every unit and AC-3 (32 ms plus
  service lag) exceeds intermittently.
- **Reset** (window emptied, statistic invalid until the next full window):
  gate close, governor trim (the level jumps, so it is a reset, not an
  exclusion), carrier loss and any replay block.
- **Bias to measure.** A fixed frame count holds different numbers of troughs
  per class (about 23 for PCM, 7.8 for AC-3, 2 to 8 for E-AC-3), and the
  minimum of fewer troughs sits slightly higher; the capture measures it.
- **Placement.** The reduction is a pure header (`aius_tx_fill_sensor.h`, in
  the governor's style), `FLASHMEM` where compiled into a cold object.
- **Observability.** `clock` gains `fill_level_q4` (signed, the last valid
  window statistic in 1/16 frame; `null` before the first valid window and
  after a reset), `fill_subwindow_spread_frames` (max minus min of its
  minima), `fill_windows`, `fill_subwindows_excluded` and `fill_torn_reads`.
  The keys land in `firmware/sd/www/openapi.json` (the `sd-editor` drift
  gate). The live status is already about 10.5 KB, past the 8,192-byte
  `AIUS_HTTP_MAX_RESPONSE` and streamed; the binding limit is
  `AIUS_RUNTIME_TEENSY_APP_BODY`, 12,288 bytes
  (`firmware/src/aius_runtime_teensy.h:48`), about 1.8 KB of headroom for
  Phases 3 and 4 together. Phase 3 serves the boot's peak body size; the
  build-time check is Phase 4's (see Delivery decisions).
- **Tooling.** The capture's derived numbers come from a committed analysis
  script over the poller's JSONL (`firmware/tools/device/`, parent-owned), not
  ad-hoc queries. The parent also owns registering the `tx-fill-sensor` suite
  (`firmware/scripts/test.sh` header list and case arm, the README suites
  table), which the `registry` suite checks.

**Host proof (tx-fill-sensor suite).** A torn read retries, and three tears
drop and count the sample; the DMA offset maps to `phi` at both half
boundaries and mid-half; an empty ring reads `256 - phi * 128`, never wrapped;
PCM 512-frame commits and AC-3 6 × 256 bursts at the same true floor, with
trough-timing jitter injected, give statistics whose difference matches the
trough-count bias; normal AC-3, DTS-2048 and long-burst E-AC-3 cadence
excludes nothing, a real stall is excluded; fewer than 8 valid sub-windows
make the window invalid; every reset path empties the window and serves
`null`.

**Device proof and noise capture.** One listening-room session of 45 min or more
that spans a PCM-to-AC-3 change (menus then programme) and several gate opens
(relocks), polled at 1 Hz with `firmware/tools/device/aius_status_poll.py`,
plus an unpolled segment of 10 min or more (`--interval 30`), because the
~10.5 KB status serialisation lengthens epochs and may bias the trough
samples. Pass criteria:

- **Resolution is real:** across each 128-frame step of the `tx_level_frames`
  floor (the slow precession; on 2026-10-04 it stepped at 20:53 and 21:04 UTC),
  `fill_level_q4` stays continuous within a few frames, and its slope matches
  the underrun cadence (about 0.23 frames/s on this source). A `phi`
  inversion or half-period error shows here as a 128-frame sawtooth.
- **No audio change:** `underrun_episode_count`, `governor_trims` and every
  harmful counter behave as on the previous image.

Recorded here, as Phase 4's inputs (by the committed analysis script):

- the steady `fill_level_q4` after each gate open, and their spread: the
  gate-open trim sets the level at an instant unrelated to the TX DMA, so
  occupancy opens anywhere in 256 to 384 and differs per relock
  (`firmware/src/optical_main.cpp:2524-2536`);
- its window-to-window standard deviation in steady state (de-duplicated by
  `fill_windows`), polled and unpolled, and the sub-window spread (the
  trough-timing jitter);
- the PCM and AC-3 floors and their difference, and the shape of the
  PCM-to-AC-3 transition (Phase 4's host model of that switch is built from
  it);
- the exclusion and torn-read rates per class.

**Measured, 2026-10-05** (`0af7a8244081`; reduced by
`firmware/tools/device/aius_fill_capture_analysis.py`; raw JSONL and reports
in `.scratch/2682a96a-3760-4d00-b9e0-f71f66b336ba/`, 7-day GC; owner-accepted
plan: main session plus a PCM top-up).

- **Resolution: pass.** Floor steps clear of an underrun read continuous
  within window noise: 20:37 −2.2 frames, 20:32 +4.06 (the reducer's 4-frame
  tolerance is tighter than the AC-3 σ of 4.3). Steps that straddle an
  underrun carry the real +128 step and get no verdict. Every episode-free
  drain slope matches the underrun cadence. No 128-frame sawtooth.
- **No audio change: pass.** `governor_trims`, `tx_missed_fills`,
  `clock_faults`, CRC rejects, overruns and torn reads all 0. Five 128-frame
  underruns (20:47:16, 20:58:51, 21:04:51, one between 21:07 and 21:25
  outside the captures, and 21:25:16) are the pre-existing drain defect this
  plan fixes; the first three were pulled from the recorder.
- **Underrun step:** +126.9 and +126.8 frames at window resolution (the
  window containing the episode blends to +8.5; ignore it). 30 s samples
  read +119 and +122 (the step less 30 s of drain).
- **Status polls trigger the underruns** (measured): every episode starts 29
  ms after the producer's last commit before a status render
  (`input_diag.dark_open_start_ms`). The ~10 KB render delays the next AC-3
  burst; once the drift has thinned the margin, a poll empties the ring.
  Polling therefore brings each underrun forward and is not neutral.
- **Drain:** AC-3 programme −0.157 to −0.275 frames/s between underruns
  (mean −0.186), steepening; output 48,001,902 against carrier 48,001,623
  mHz at the end (279 mHz, 5.8 ppm), matching the late slope. The plant
  model (u mHz moves u/1000 frames/s) holds to about 10 percent. The PCM
  top-up's reported +0.38 frames/s rise is a reducer artefact (Phase 4
  entry-gate design pass, 2026-10-06): `episode_free_slope`
  (`firmware/tools/device/aius_fill_capture_analysis.py:344-366`) does not
  split a span at a gate close, so its fit spanned the 21:27:59 relock's
  +146-frame re-base. Per segment the source is 48,001,666 mHz (AC-3, before,
  −0.236 frames/s) and 48,001,656 mHz (PCM, after, +0.033 frames/s): one
  clock. The relock's dark full apply re-centred the output on the estimate
  (48,001,902 to 48,001,623, `trim_updates` 0), about 33 mHz below the
  source. Earlier the source warmed from about 48,001,866 (20:31) to
  48,001,650 (21:00), steepest about −14 mHz/min.
- **Steady fill after a gate open:** 292.6 (AC-3, 20:27:25), 306 to 310
  (PCM), 328 to 339 (AC-3), 379 to 382 (PCM) frames over four real relocks:
  range 90 frames, inside the predicted 256 to 384. The reducer's
  "21:25:04 ac3 244.62 gate open" is the top-up file's start
  (`gate_open_padded_frames` unchanged), not a relock.
- **Noise (lag-1 window-to-window σ, polled):** AC-3 1.17 frames (412
  pairs, P plus top-up, windows around an underrun excluded; the reducer's
  4.33 included two underrun-step pairs), PCM 1.42 (61 pairs). 16 of 412
  clean AC-3 pairs exceed 4 frames, the largest 7.06 (one render-dip
  window). Sub-window spread median and p95: AC-3 7 and
  47, PCM 13.5 and 54 frames. Unpolled AC-3 (lag 7 to 8): white-model σ
  0.88, random-walk-model σ 0.29.
- **Exclusions and tears:** AC-3 4 of 625 windows (0.0016 per s), PCM 0 of
  91; torn reads 0.
- **Class floors and difference:** every PCM and AC-3 change on this source
  closed the gate and relocked, so the gate-open trim reset the fill at each
  switch; no in-lock class switch occurred and no drift-free class
  difference is derivable. Cross-time medians (PCM 384.8, AC-3 255.8) are
  drift-confounded and not a class bias.

### Phase 4 Fill-referenced rate trim

```yaml
phase: 4
title: Close the output-rate loop on transmit-ring fill
status: built # 4a shipped and deployed (df966f17b370); device fill-loop criteria passed 2026-10-07; gate 7 at fill-write cadence outstanding
depends-on: phase 3 noise capture (setpoint spread across gate opens, PCM-versus-AC-3 floor bias)
prebuild-review: ready # 2026-10-06: rounds 4 to 6 NOT READY and folded; round 7 READY; clean-room NOT READY (1 blocker) folded, closure READY with N1 to N3 folded. Delivery audit adds: the parent's arm counter and unit_frames input, and the apply-started note flag (host-untested)
outcome: >-
  A live source offset inside the 500 mHz target deadband no longer drains
  (or fills) the transmit ring: the output rate converges on the source's
  true rate, so a steady-state session shows no periodic 128-frame underrun,
  at unchanged mean standing latency (per-session setpoint, mean 320 frames).
unit: component-behind-interface
seams:
  - AiusRateTracker (centre and fill integral as separate state; returns one composed request; told the centre only)
  - clock_refresh_applied -> aius_rate_tracker_note_target_applied (optical_main.cpp:8751-8759; a FILL readback never notes a target)
  - same-nominal NUM trim request with a stamp-kind argument (aius_audio_clock_request_trim)
  - fill sensor closed-window sequence number and exclusion-reason bits (aius_tx_fill_sensor.h)
  - TX gate open reads the opening occupancy in the masked section and opens only at a burst start (tx_gate_execute_open, optical_main.cpp)
  - GET /api/status clock and firmware/sd/www/openapi.json
targeted-proof:
  - host: clock suite plant model (cases below)
  - target: clock-target, tx-gate, tx-governor, tx-fill-sensor, itcm-placement, registry, sd-editor green; ISR ITCM bytes recorded
  - device: one listening-room AC-3 session with criteria below; bench gates 1 and 7 and the stable-phase transition gates
contract-artifact: spec-only
fan-out: # host-only slices against §Slice interfaces: fill-sensor-window, clock-kind, unit-start concurrently; fill-loop-core once fill-sensor-window's pure opening functions land; parent-wiring last; the parent runs every target build
  - name: fill-loop-core
    owned-paths:
      - firmware/lib/aius_clock/**
      - firmware/test/clock/**
  - name: clock-kind
    owned-paths:
      - firmware/lib/aius_audio/include/aius_audio_clock.h
      - firmware/lib/aius_audio/src/aius_audio_clock.cpp
      - firmware/lib/aius_audio/src/aius_audio_clock_internal.h
      - firmware/lib/aius_audio/src/aius_spdif_sources.cpp
      - firmware/lib/aius_audio/src/aius_spdif_tx24.h
      - firmware/test/clock-target/**
      - firmware/test/itcm-placement/verify_target.py
  - name: fill-sensor-window
    owned-paths:
      - firmware/lib/aius_audio/src/aius_tx_fill_sensor.h
      - firmware/test/tx-fill-sensor/**
  - name: unit-start
    owned-paths:
      - firmware/lib/aius_codec_api/include/aius_codec.h
      - firmware/lib/aius_pipeline/**
      - firmware/test/pipeline/**
      - firmware/test/codec-api/**
      - firmware/test/capability/**
      - firmware/test/dsp/test_dsp.c
  - name: parent-wiring # coordinator: gate open, sample_rate_service, status keys, openapi, test.sh, reducer fix
    owned-paths:
      - firmware/src/optical_main.cpp
      - firmware/sd/www/openapi.json
      - firmware/test/optical-cutover/**
      - firmware/test/status-body-bound/**
      - firmware/scripts/test.sh
      - firmware/tools/device/aius_fill_capture_analysis.py
review-intensity: active
commit-checkpoint:
  green: [clock, clock-target, tx-gate, tx-governor, tx-fill-sensor, rate-runner, itcm-placement, registry, sd-editor, optical-cutover, status-body-bound, memory, pipeline, codec-api-contract, capability, dsp, a52, eac3, dts]
  buildable: [teensy41-optical, teensy41-optical-slot-a]
  parent-commit: "feat(aius): trim the output rate on transmit-ring fill"
```

**Mechanism.**

- **Entry gate: decided 2026-10-06 from the Phase 3 capture** (design pass
  `opus`; each decision put to the owner and accepted as recommended).
  Loop figures are closed form (continuous PI, 4 s window, plant u/1000
  frames/s); the host plant model confirms them before the device session.
  1. **Setpoint 320 frames** (6.67 ms at 48 kHz, the centre of today's 256
     to 384 gate-open band, so mean standing latency is unchanged). The
     underrun line is occupancy 128, so the earlier "256 plus 50" floor was
     conservative. Underruns struck at window statistics of 134 to 174; the
     deepest status-render dip below its window was 71 frames (sub-window
     spread p95: AC-3 47, PCM 54). 320, less that dip, a 700 mHz step's
     34.5-frame peak and 3σ of noise, sits 82 frames clear (46 if the 16 KiB
     status body deepens the dip 1.5 times). Alternatives 288 (less
     margin) and 352 (+0.67 ms latency) declined.
     **Amended 2026-10-06 (prebuild review round 4, owner): the setpoint is
     per session, mean 320.** Every fill and commit moves the ring in
     multiples of 128 frames, so the residue `r = level mod 128` is fixed
     for a session; a pad to a constant occupancy spreads `r` over 0 to 127,
     and the delay tolerance at a trough is `O - 128 - r`. At a constant 320
     that is 64 to 192 frames, short of the 109-frame budget (71 dip, 34.5
     step, 3σ) in about 35 percent of sessions; "82 clear" was the best
     phase. So the gate open keeps its level-128 pad (`r` = 0) and the
     setpoint is the occupancy measured at the open, `384 - phi_0 * 128`
     (256 to 384, mean 320, so mean standing latency is unchanged; it varies
     ±1.33 ms between sessions). Worst-phase tolerance is 128 frames, 19
     clear of the 109 budget; with the 16 KiB body's estimated 1.5 times
     dip (about 145) it falls short when `phi_0` > 0.87, about 13 percent
     of sessions, so Phase 4b, not the setpoint, is the real mitigation for
     a deeper dip (round 5 N8). After one underrun the trough level is 256,
     under the governor's 384 trigger (`firmware/lib/aius_audio/src/aius_tx_governor.h:84`), and a
     governor trim (which lands at level 128) lands on the setpoint, also in
     a session whose open saw a pending boundary (F3 pads that open to level
     128 after the late ISR, so its trough is 256 as well). The constant alternative (about 368 frames, about 1 ms more latency)
     declined.
  2. **Class bias: none.** All three class changes closed the gate and
     relocked, so the occupancy pad resets the floor. The budget allows
     3.8 frames (50 mHz over `Kp`); the estimated trough-count bias is 2 to
     3 frames (inferred, unmeasured). The host in-lock switch case injects a
     4-frame bias and a floor re-base and asserts no underrun. A per-class
     setpoint is the fallback if a source is seen switching in lock.
  3. **The PCM top-up's +0.38 frames/s was a reducer artefact** (Phase 3
     §Measured, Drain): one source clock, the relock's full apply re-centred
     on the estimate. No change to gains or authority; the warm-up ramp
     (about −14 mHz/min) leaves a steady tracking error of about 2.7 frames,
     and a 150 mHz post-relock residual with `I` zeroed peaks at about
     7 frames.
  4. **Gains: `Kp` 13 mHz per frame, `Ki` 345 µHz per frame per window**
     (damping 0.70). Statistic unchanged (mean of the 16 sub-window minima,
     4 s window at 48 kHz; 4.35 s at 44.1, 6 s at 32, so slower there).
     Noise: AC-3 1.17 frames, PCM 1.42, unpolled 0.88, 3 to 5 times under the
     4-frame bound. Modulation 15 to 18.5 mHz rms; a single render-dip window
     (up to 7 frames) kicks about 91 mHz for 4 s, accepted. A 700 mHz step
     peaks at 34.5 frames, is inside 16 frames by about 3.5 min and settled
     by 10.3 min. Fixed point: `P = 13 * e_q4 / 16` mHz,
     `I += 345 * e_q4 / 16` µHz, rounded. Kp 8 / Ki 130 µHz (56-frame peak,
     17 min) and Kp 20 to 26 (more modulation and writes) declined.
  5. **Emit threshold 20 mHz** (12 NUM LSBs). Noise moves the composed
     target 21 to 26 mHz rms per window, so writes are not rare: expect 35
     to 45 percent of windows polled (300 to 400 per hour), about 20 percent
     unpolled, at a cost of 1.5 frames of fill error. Hard bound: one FILL
     write per window (900 per hour at 48 kHz, 827 at 44.1, 600 at 32).
     Gate 7 tests carrier continuity at one write per 4 s.
  6. **An underrun is a +128-frame step, rejected by plain PI, not a reset**
     (+126.94 and +126.81 measured; one sub-window excluded each, no trim).
     The window holding it is used as is: it reads the step times the share
     of valid sub-windows after it (+8.50 is 1 of 15), a linear partial
     step. `Kp * 128` is 1,664 mHz, so the output rides the ±900 clamp with
     `I` held. Clipping the error at ±64 frames, and subtracting known
     underrun frames (latency then stays 128 high), declined. Round 4
     correction: the step is `128 - r`, which is 128 once `r` = 0 (always,
     under the item 1 amendment). Recovery depends on `I` (reviewer plant
     sim, +128 step): `I` 0 back within 16 frames in 144 s with 15 frames
     undershoot; `I` +670 mHz 491 s, about 4; `I` −670 mHz 99 s, 26. So
     the worst recovery is about 8 min, not 3 to 4. The undershoot is
     covered by the 128-frame worst-phase tolerance with the measured dip
     (26 + 71 + 3.5, about 100), not with a 1.5 times dip (about 137):
     Phase 4b again.
- **Status body size, checked at build.** Moved here from Phase 3 (owner,
  2026-10-05). No build or test today bounds `/api/status` against
  `AIUS_RUNTIME_TEENSY_APP_BODY` (12,288 B); the only `static_assert`s cover
  the meters and recorder documents. Phase 3 serves `status_body_peak_bytes`
  (the largest earlier body this boot) so the capture measures real headroom. This
  phase adds the worst-case bound with a `static_assert` (or a host render of
  the worst case) before it spends that headroom on its own keys, and adds its
  keys to the `optical-cutover` expected set.
  **Pre-capture slice, 2026-10-05.** `status_render_failures` (top level,
  counted in `app_write_status()`) shipped in the Phase J redeploy, so an
  overflow is visible after the next successful render. The bound is built
  but red: `firmware/test/status-body-bound/` (a host extractor that walks
  every function `http_status_json()` writes through and sums worst-case
  widths, refusing what it cannot bound) puts the worst case at 14,723 B
  against 12,288 B, mostly counters at full 10-digit width (`input_diag`
  3,337 B, `clock` 2,160 B).
  **OCRAM slice: done 2026-10-06** (owner decision 2026-10-05: fund the
  OCRAM, do not lower a floor). OCRAM free was really 131,072 B (32 B over
  the 131,040 B heap floor; the 131,360 `teensy_size` figure omits 288 B of
  `.bss.aius_retained`), and DTCM free 25,472 B against the 24,352 B
  stack floor (`firmware/link/aius_teensy41.ld:285`), so DTCM had to be
  funded first. Owner chose (2026-10-06) PROGMEM format strings over a
  per-object linker rule or a lower DTCM floor:
  1. `http_meters_json()::snapshot` (`AiusMeterSnapshot`, 4,776 B) moved
     from OCRAM `DMAMEM` to DTCM `.bss` (not a DMA buffer, not hot path).
  2. Twelve cold format literals in `firmware/src/optical_main.cpp`
     (5,106 B) became function-local `static const char ...[] PROGMEM`, the
     `http_clock_json` pattern; all in `FLASHMEM` HTTP render paths, none
     read from an ISR or ITCM. Rendered text byte-identical.
  3. `AIUS_RUNTIME_TEENSY_STATUS_BODY` 16,384 B
     (`firmware/src/aius_runtime_teensy.h`) now sizes `app_body`;
     `AIUS_DSP_MODEL_MAX_JSON_BYTES` stays 12,288.
  4. The suite is committed; `test.sh` requires it to pass at 16,384 and to
     refuse a limit equal to the worst case (and 12,288).
  Linked: DTCM free 25,824 B (+1,472 over the floor), OCRAM free 131,776 B
  (+736), ITCM unchanged at 191,804 B (`teensy41-optical`). Status headroom
  is now 1,660 B; Phase 4's own `clock` keys spend part of it, and the
  suite bounds them when they land.
- **`CLAUDE.md` promote.** The commit that writes the fill
  stamps into CLAUDE.md (`the release-promotion step`) also corrects §Device access and
  OTA's signing line: `aius_ota_sign.py sign` needs `--image`, `--output`
  and `--build-revision` (12-char HEAD, at most 32 chars). Today the line
  shows only `--secret`; both 2026-10-05 deploys needed the full form. If
  the file still carries foreign uncommitted edits, wait for their owner.
- **Gate open keeps the level pad; the setpoint is the opening occupancy**
  (entry gate item 1 amendment, which replaced the earlier "pad to a
  constant occupancy" design). `tx_gate_execute_open`
  (`firmware/src/optical_main.cpp:2630-2652`) still pads the ring to level
  128, so `r` = 0. Against a constant setpoint the opening occupancy (256 to
  384) would be a ±64-frame error pinning the output at the clamp after
  every relock, breaking gate 1 and the stable-phase labelling
  (`firmware/tools/net/aius_rate_bench.py:397-424`, `:642-664`); a setpoint
  equal to the opening occupancy has near-zero opening error. Not exactly
  zero (round 5 N9): the statistic, a mean of sub-window minima including
  late commits, reads a few frames below the open sample (about 7 AC-3, 13
  PCM at median spread), a one-off kick of about 90 to 175 mHz after each
  open, which the host cases bound.
  - *Read phi inside the masked section* that sets the level: read outside,
    an ISR between read and set skews it by 127 frames. Inside, the Phase 3
    tear check is blind (`fills` cannot move), so the open detects a pending
    period boundary from ONE TX DMA SADDR read returning `{half, offset}`:
    pending means SADDR lies in `g_aius_tx_fill_witness.last_half` (the DMA
    already reads the half the ISR last filled, and the half it just played
    is stale). The late ISR takes one half on the unmask, so a pending open
    padded only to level 128 would be left 1 to 128 frames. A pending live
    open therefore pads one half more, to level 256
    (`aius_tx_fill_open_level_frames`, Phase 4a F3): after the late ISR the
    level is 128 and the setpoint is `384 - phi' * 128`, the normal case's
    `384 - phi * 128` shape. The extra half counts in
    `gate_open_pending_padded_frames`, so `gate_open_padded_frames` still
    advances by one target per live relock. (Round 5 N1: the round
    4 "add 128" had the sign wrong and would set the setpoint 256 frames
    high, a repeating governor splice. Reading SADDR and the DMA interrupt
    flag separately can straddle a crossing, so the flag is not used, N4.)
    The accessor lives in `aius_spdif_sources.cpp` (slice clock-kind).
  - *Open only at a unit start* (owner, 2026-10-06; mechanism round 5 N3).
    An open armed between the six one-per-call AC-3 blocks executes at block
    k and lands k × 256 frames low; today level 128 at k ≥ 1 underruns at
    the open. The block carries a `first_in_unit` flag set by the
    pipeline/decoder (`AiusPcmBlock`,
    `firmware/lib/aius_codec_api/include/aius_codec.h:76-82` sees no unit
    position today). Unit start per class: AC-3 block 0 of 6
    (`a52_blocks_decoded == 1`); E-AC-3 the first block of each decoded
    access unit (`retained_offset == 0`; the pipeline's unit is one access
    unit of at most 1,536 frames, `aius_eac3.h:20`, so nothing counts a
    6,144-frame unit); DTS the first 512-frame slice of each access unit
    (`retained_offset == 0`; a DTS-2048 unit arrives as four slices,
    `aius_pipeline.c:1277-1294,1334-1340`, so marking every slice would
    reopen k × 512 low, round 6 R1); PCM and replay every block; a CRC
    silence substitution marks its first substituted block of each unit
    (`aius_pipeline.c:1275,1331`). The sensor's 6,144-frame E-AC-3 figure
    (`aius_tx_fill_sensor.h:182`) only makes the cap lenient (128 ms); a
    type-21 burst is one access unit (`aius_eac3.h:17-23`), so the marker is
    right (round 6 R8). The deferral is capped at one unit of committed
    frames since the arm (`aius_tx_fill_unit_frames(pc, block_frames,
    is_pcm)`, `aius_tx_fill_sensor.h:173`), then the open takes any block, so a class or a stale
    `codec_pc` that never marks a start cannot hold the room silent. No
    interaction with the 512-frame invalid hold: the open arms only after
    `transition_complete`.
- **Setpoint: per session, arithmetic.** `384 - phi_0 * 128` in frames from
  the corrected open read, fixed for the lock; a governor trim (level 128)
  leaves it valid. Valid across 48, 44.1 and 32 kHz because the open is in
  frames too (44.1 kHz unproven until a 44.1 source is observed). No
  learning to poison.
- **Invalid window** (fewer than 8 valid sub-windows,
  `firmware/lib/aius_audio/src/aius_tx_fill_sensor.h:257-290`; an invalid
  window serves `fill_level_q4` 0 with `fill_level_valid` 0, and
  `fill_windows` counts valid windows only). The loop gates on
  `fill_level_valid`, never reads q4 alone (e = −setpoint would slew to
  −900). The sensor gains a closed-window sequence number (valid or not)
  and per-window exclusion-reason bits (underrun, stall, missed fill, no
  sample), so the loop sees every close and the window cannot go silent
  (the round-1 governor-void trap). Rule (owner, 2026-10-06): an invalid
  window whose exclusions include an underrun and no stall reads as **128**,
  the true occupancy at an underrun (level 0 at phi 1); the output goes to
  the −900 clamp with `I` held, no special case. A stall, or no underrun:
  hold `I`, emit nothing. The step that follows is read as is. Never
  observed in the capture.
- **Control law: PI.** Per window, `I += Ki * e` and
  `offset = clamp(Kp * e + I)`, with `e` the window statistic minus the
  setpoint. A positive error (ring filling) speeds the output up. `I` is held
  in fixed point (µHz), because `Ki` is below 1 mHz per frame and would
  truncate to a P-only loop in integer mHz. Anti-windup: `I` holds while the
  clamp is active, and is itself clamped to ±900 mHz.
- **Gains.** Derived in the host plant model with the Phase 3 noise. Targets,
  in the order they give way: output modulation from sensor noise within
  ±50 mHz and peak fill excursion ≤ 64 frames for a ±700 mHz step hold;
  settling may relax from 10 min to 30 min. Reference point from review
  round 2: damping 0.7 and a 10 min settle give `Kp` about 13 mHz per frame,
  `Ki` about 0.36 mHz per frame per window and a 34-frame peak, which needs
  statistic noise of about 4 frames or less to meet ±50 mHz; a noisier
  statistic means a slower loop.
- **Authority.** Clamp ±900 mHz: the in-lock offset is at most the 500 mHz
  hysteresis plus the estimate error (about 170 mHz at most on 2026-10-05,
  670 together; the earlier "bias 55 plus wander 280, 835" double-counted),
  plus margin; well inside the plan band (±2 %). The composed target is also
  clamped into the plan band and into the centre's structural cell:
  `request_trim` refuses a plan that differs structurally from the applied
  one (`firmware/lib/aius_audio/src/aius_audio_clock.cpp:573-579`), so a
  target across an integer-divider crossing would be refused forever (at
  48 kHz the crossing is at 48,549 Hz, so this is a guard, not a case). A plan
  failure on a fill request is a no-op, never a fault.
- **Controller state and emission state are separate.**
  - *Controller state* is `centre` (the carrier-estimate target; hysteresis
    and the 10 s cadence judged against it alone, logic unchanged) and `I`.
    It updates every window (`I`) and every centre retarget, whether or not
    anything is written. A retarget is bumpless, `I -= centre_new -
    centre_old` (computed in int64 µHz: a full ±2 % band move is 1.92e9 µHz,
    near `INT32_MAX`), then `I` is re-clamped (any residual self-heals at the
    next retarget, because the centre follows the estimate). A committed
    centre also updates `g_clock_desired_plan`, so the fault-retry full apply
    (`firmware/src/optical_main.cpp:8767`) uses the current centre, with `I`
    zeroed.
  - *Retarget under saturation* (orchestrator calls, round 4 finding 7 and
    round 5 N2, N6). Saturated, `I -= Δ` leaves the offset at the clamp, so
    the composed target jumps by the whole Δcentre (500 mHz or more). A
    retarget whose Δcentre has the **same sign** as the saturated offset
    commits at once: the composed target moves toward what the loop already
    asks for, never past it. One of **opposite sign** is deferred, because
    that saturation resolves itself (the ring drains faster); the deferral
    is capped at 120 windows (8 min, the worst recovery), then commits.
    Deferring every retarget was declined: a source wandering past the
    clamp during an underrun recovery would keep the loop saturated forever
    with the retarget that would end it deferred (N2). The cap counts from
    the first proposal of a deferral episode; `_step` re-proposes every
    10 s while the reference is the old centre (`aius_clock.c:258-267`), and
    a re-proposal refreshes the pending value only, never the count (R5). Slew-limiting the
    composed step was declined: every legitimate retarget would take 25
    windows. `fill_reset(discard_pending = false)` commits a pending centre
    into `g_clock_desired_plan`, and is called **before**
    `aius_audio_clock_apply` at exactly two sites: the same-nominal
    structural full apply (`firmware/src/optical_main.cpp:8903`) and the
    fault retry (`:8767`); the apply is dark, so the bump is inaudible (N6).
    Every other `I`-zeroing event discards it (`discard_pending = true`):
    an accept (`:8848`, which replaces the desired plan with a fresh
    estimate), a replay (its desired plan must stay exactly 48,000,000 mHz,
    `:8744-8749`, `:8924-8937`, or `g_clock_replay_ready` stays false) and
    carrier loss (round 6 R3). `clock_apply_desired_plan` has four call
    sites (`:8767`, `:8848`, `:8903`, `:8936`); carrier loss is a discard
    event, not an apply site (round 7 F3). A pending centre is also dropped
    when a 1 s window mean returns inside the hysteresis of the current
    reference, so a cap or end of saturation never commits a stale value.
  - *Existing centre-path behaviour survives* (round 6 R7). The trim branch
    at `:8851-8900` keeps its invalid-plan fault for a centre target
    (`:8854-8857`), and a committed centre whose plan differs structurally
    takes the structural full apply (`:8900-8905`) with `I` zeroed, never a
    composed target in a cell `request_trim` refuses forever.
  - *The tracker is told the centre only.* `clock_refresh_applied`
    (`firmware/src/optical_main.cpp:8751-8759`) passes the applied target to
    `aius_rate_tracker_note_target_applied`, which sets
    `applied_target_millihz` and resets `last_target_request_ms`
    (`firmware/lib/aius_clock/src/aius_clock.c:344-345`). **The centre and
    the hysteresis reference are one field** (`applied_target_millihz`,
    round 6 R2: two fields left the new centre uninitialised after a
    relock, so the first FILL write was composed on the pre-relock centre,
    and at a nominal change every request was refused at
    `aius_audio_clock.cpp:575`). Accept sets it to the accept target; a
    centre commit sets it; the parent's note sets it once, on the pass
    where `g_clock_dark_transition_pending` goes from true to false (not on
    "applied differs from noted", which would fire on every FILL write),
    keeping today's guards `!replay_selected` and applied nominal equal to
    `g_clock_nominal_rate_hz` (`optical_main.cpp:8751-8753`), so a replay's
    dark transition never notes 48,000,000 as the live centre. "Note iff
    applied equals desired" was declined: after a bumpless retarget the
    composed target almost never equals the centre, so it would never fire
    (interface draft, 2026-10-06). A FILL readback never notes, or every
    fill write restarts the 10 s cadence and moves the hysteresis reference
    onto the composed value (the round-1 starvation blocker through unnamed
    wiring). `accept_candidate` today zeroes `applied_target_millihz`
    (`aius_clock.c:221`), and `observe_measurement_window` returns early
    while it is 0 (`:258`); Phase 4 changes accept to set it to the accept
    target instead (round 7 F1), and the note becomes idempotent,
    re-asserting the full apply's readback (including a fault retry that
    committed a pending centre).
  - *Emission state is the applied-plan readback on every pass*, never a
    value committed on a true return: the applied plan lags a true return by
    up to one 128-frame period, and a queued trim can be superseded by a full
    apply. A true return only spends that window's single write; a refusal
    spends nothing and the next pass retries. The tracker API carries both
    states, rather than `optical_main` wiring.
- **Emission.** One composed request per `sample_rate_service` pass, at most
  once per window, and only when the composed target differs from the
  emission state by more than the emit threshold (20 mHz, entry gate item
  5). At the measured noise writes are not rare: the bound is one FILL write
  per window.
- **Kinds and diagnostics.** A centre commit raises the CENTRE flag. If the
  commit pass needs no write (sub-threshold, bumpless), the flag clears at
  the end of that pass, so a noise write minutes later is not labelled
  CENTRE with a false `trim_last_at_ms`. If it needs a write, CENTRE
  persists until that write is accepted, through refusals and spent
  windows, and a CENTRE write bypasses the one-FILL-per-window budget
  (round 6 R4). The flag also clears on any later pass that needs no write
  and on `fill_reset`, so a needed write that stops being needed cannot
  leave it up for a noise write minutes later (round 7 F2). Every other
  write is FILL. A centre retarget counts `trim_updates`
  and records `AIUS_EVENT_CLOCK_TRIM` when it commits, whether or not a write
  follows, so centre activity stays visible. A CENTRE write stamps
  `trim_last_at_ms` and `trim_applied_at_ms`
  (`aius_spdif_sources.cpp:184`); a FILL write counts `fill_trims` and stamps
  `fill_trim_last_at_ms` and `fill_applied_at_ms`, with no event. The
  glitch-reading stamp list in `CLAUDE.md` gains the two
  fill stamps in the Phase 4 commit (a protected file: `the release-promotion step`).
- **The kind is a separate field, not a new request value.** The boundary
  ISR treats every request other than `NONE` and `TRIM` as a HOLD
  (`aius_spdif_sources.cpp:162-197`, enum at
  `aius_audio_clock_internal.h:8-12`): it marks validity invalid and arms
  the four-period hold without writing the numerator, the 2026-09-07 mute.
  So the request stays `TRIM` and a separate stamp-kind field picks only
  which stamp the ISR writes. The kind is an argument of
  `aius_audio_clock_request_trim`, stored inside its critical section after
  the pending check (`firmware/lib/aius_audio/src/aius_audio_clock.cpp:567-599`),
  never a global set before the call: a refused FILL call must not overwrite
  the kind of a CENTRE trim still pending. `clock-target` builds with
  `-DAIUS_AUDIO_CLOCK_FAKE_TARGET` (`firmware/scripts/test.sh:523`), which
  compiles out the ISR stamp (`aius_spdif_sources.cpp:184-189`), so the stamp
  moves behind a fakeable clock and `clock-target` asserts a FILL write
  writes NUM, leaves `VALCTRL` and `invalid_periods_remaining` untouched,
  and stamps only the fill pair (and a CENTRE write only the centre pair).
  Cost: one load, one compare, one DTCM store in the ITCM-resident ISR,
  recorded against ITCM headroom.
- **Scope and reset.** Emission only for a live source with
  `g_clock_plan_applied`; replay blocks emit nothing, so
  `g_clock_replay_ready`'s exact-48,000,000 check is untouched. `I` is kept
  across a same-nominal gate blip (a real source change drops lock, and the
  re-accept's dark full apply zeroes it) and zeroed on a nominal change, a
  replay, or a dark full apply. The last-applied composed value is read back
  from the applied plan, never assumed.
- **Governor.** Unchanged; with the ring held at the gate-open floor it only
  sees stall backlogs, its intended job.
- **Cleanup.** The `AIUS_CLOCK_TARGET_HYSTERESIS_MILLIHZ` comment
  (`firmware/lib/aius_clock/src/aius_clock.c:18-34`) loses its drift-bound
  claim and points here.

**Observability.** `clock` gains `fill_offset_millihz` (signed, composed
minus centre), `fill_setpoint_frames` (this lock's setpoint),
`fill_open_boundary_pending` (which setpoint formula the open used), `fill_trims`,
`fill_trim_last_at_ms` and `fill_applied_at_ms`; the fill sensor's block
gains the closed-window sequence number and the last window's
exclusion-reason bits. Same openapi check; the body bound is
`AIUS_RUNTIME_TEENSY_STATUS_BODY` (16,384 B, 1,660 B headroom before these
keys), enforced by `status-body-bound`. No new event kind.
`gate_open_target_frames` and the `CLAUDE.md` line "advances
by one `gate_open_target_frames` per live relock" stay true, because the pad
stays constant.

**Slice interfaces** (fixed 2026-10-06 by the interface draft; the parent
holds every slice to them). No slice edits a file another owns. A type that
crosses a slice boundary has a copy in each owner's header with equal
numeric values, and the parent `static_assert`s the match in
`firmware/src/optical_main.cpp`. The tracker never includes
`aius_tx_fill_sensor.h`; `aius_audio_clock.h` never takes the tracker's kind
enum. The decoders do not change: the pipeline already knows each block's
position.

```c
/* aius_clock.h (fill-loop-core) */
#define AIUS_CLOCK_FILL_EXCL_UNDERRUN    UINT8_C(0x01)
#define AIUS_CLOCK_FILL_EXCL_STALL       UINT8_C(0x02)
#define AIUS_CLOCK_FILL_EXCL_MISSED_FILL UINT8_C(0x04)
#define AIUS_CLOCK_FILL_EXCL_NO_SAMPLE   UINT8_C(0x08)
#define AIUS_CLOCK_FILL_RETARGET_DEFER_MAX_WINDOWS 120u
typedef enum { AIUS_RATE_TRACKER_TRIM_NONE = 0, AIUS_RATE_TRACKER_TRIM_CENTRE = 1,
               AIUS_RATE_TRACKER_TRIM_FILL = 2 } AiusRateTrackerTrimKind;
typedef struct {            /* copied from AiusTxFillSnapshot by the parent */
	uint32_t window_seq;    /* closed windows, valid or not; wraps */
	int32_t level_q4;       /* read iff level_valid */
	bool level_valid;
	uint8_t exclusions;     /* AIUS_CLOCK_FILL_EXCL_*, last closed window */
} AiusRateTrackerFillWindow;
typedef struct {
	bool emit_allowed;                /* live, g_clock_plan_applied, gate open */
	bool have_applied;
	uint32_t emission_readback_millihz; /* applied plan readback = emission state;
	                                       never written into the tracker's centre */
	uint32_t setpoint_frames;         /* this lock's; 0 = none yet */
	AiusRateTrackerFillWindow window;
} AiusRateTrackerFillInput;
typedef struct {
	AiusRateTrackerTrimKind emit_kind;  /* NONE: write nothing this pass;
	                                       field below is the centre, never the readback */
	uint32_t composed_target_millihz;   /* clamped to band and centre's cell */
	uint32_t centre_millihz;            /* g_clock_desired_plan target */
	int32_t fill_offset_millihz;        /* composed minus centre */
	int32_t fill_integral_microhz;      /* clamped to +/-900000 */
	bool centre_committed;              /* this pass: trim_updates++, event, desired plan */
	bool retarget_pending;
	uint8_t retarget_pending_windows;   /* 0..120 */
	bool saturated;
} AiusRateTrackerFillResult;
/* AiusRateTracker gains: pending_centre_millihz, pending_centre_windows
 * (counts from the episode's first proposal), fill_integral_microhz,
 * fill_offset_millihz, fill_last_seq, fill_spent_seq, fill_have_seq,
 * fill_spent, fill_saturated, centre_flag (persists until a CENTRE write
 * is accepted). The centre IS applied_target_millihz (one field). The
 * setpoint is input only (in->setpoint_frames governs; no tracker copy). */
AiusRateTrackerFillResult aius_rate_tracker_fill_step(
	AiusRateTracker *tracker, const AiusRateTrackerFillInput *in); /* every pass, after _step */
void aius_rate_tracker_note_emit(AiusRateTracker *tracker, bool accepted); /* true spends the window */
bool aius_rate_tracker_fill_reset(AiusRateTracker *tracker, bool discard_pending,
	uint32_t *committed_centre_millihz); /* zero I, clear the CENTRE flag; true if a
	pending centre committed. On true the parent, before clock_apply_desired_plan,
	counts trim_updates, records AIUS_EVENT_CLOCK_TRIM and rebuilds
	g_clock_desired_plan from the committed centre; if that plan is invalid it
	faults as the centre path does today (clock_record_fault(INVALID_PLAN)
	marks the desired plan invalid, so the following apply returns false;
	orchestrator call 2026-10-06: unreachable in band, as every in-band
	centre has a valid plan, so today's behaviour is kept). */
/* (no tracker setpoint function: aius_tx_fill_open_setpoint_frames is the one) */
/* _step: a proposal goes to pending_centre (target_update still reported);
 * a committed centre sets applied_target_millihz itself; accept_candidate
 * sets it to the accept target and zeroes I. note_target_applied:
 * signature unchanged, called once when g_clock_dark_transition_pending
 * goes true -> false, and only for a transition clock_apply_desired_plan
 * started (clock_record_fault also raises the flag with no apply,
 * optical_main.cpp:8662; that edge never notes). */

/* aius_audio_clock.h (clock-kind) */
typedef enum { AIUS_AUDIO_CLOCK_TRIM_CENTRE = 1, AIUS_AUDIO_CLOCK_TRIM_FILL = 2 }
	AiusAudioClockTrimKind;                 /* 0 or other: refused */
bool aius_audio_clock_request_trim(const AiusSpdifClockPlan *plan,
	AiusAudioClockTrimKind kind);
/* aius_audio_clock_internal.h: AiusAudioClockSharedState gains
 * volatile uint32_t queued_kind (stored after the pending check). */
extern "C" volatile uint32_t g_aius_tx_trim_applied_at_ms; /* defined in FAKE too */
extern "C" volatile uint32_t g_aius_tx_fill_applied_at_ms;
/* aius_audio_clock_internal.h: static inline, so the ITCM ISR never calls
 * into flash; millis() on target, a fake variable under FAKE_TARGET. */
static inline uint32_t aius_audio_clock_now_ms(void);
#if defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)
void aius_audio_clock_fake_set_now_ms(uint32_t now_ms);
#endif
/* aius_spdif_sources.cpp: the CALLER holds the interrupt mask; the accessor
 * never touches PRIMASK (an unconditional interrupts() would reopen the
 * parent's masked section); FASTRUN, not FLASHMEM, not inline (another TU).
 * ONE SADDR read; half 0/1; pending decided by the shared pure predicate. */
extern "C" uint32_t aius_spdif_tx24_dma_read_position(uint32_t *half,
	uint32_t *offset_frames);

/* aius_tx_fill_sensor.h (fill-sensor-window) */
#define AIUS_TX_FILL_EXCL_UNDERRUN 0x01u   /* = AIUS_CLOCK_FILL_EXCL_* */
#define AIUS_TX_FILL_EXCL_STALL 0x02u
#define AIUS_TX_FILL_EXCL_MISSED_FILL 0x04u
#define AIUS_TX_FILL_EXCL_NO_SAMPLE 0x08u  /* sub-window closed with cur_min INT32_MAX */
/* AiusTxFillSnapshot += uint32_t fill_window_seq (every close, lifetime);
 *                       uint32_t fill_window_exclusions (OR over the last window).
 * AiusTxFillSensor += uint32_t cur_reasons, win_reasons; reset zeroes both and
 * the served exclusions and keeps the seq. */
static inline void aius_tx_fill_sensor_exclude(AiusTxFillSensor *s, uint32_t reason);
/* replaces the one-argument exclude, which is removed */
/* Shipped opening rules, pure, tested in tx-fill-sensor and called (not
 * modelled) by the test/clock plant and by optical_main (clean-room 1).
 * Always-inline and WITHOUT AIUS_TX_FILL_SENSOR_COLD (FLASHMEM in both
 * callers, optical_main.cpp:130, aius_spdif_sources.cpp:23-26), so neither
 * the FASTRUN accessor nor the masked gate open runs from flash (N2): */
static inline bool aius_tx_fill_boundary_pending(uint32_t saddr,
	uint32_t ring_base, uint32_t last_half_base);  /* SADDR lies in last_half */
static inline uint32_t aius_tx_fill_open_setpoint_frames(uint32_t level_frames,
	uint32_t offset_frames, bool boundary_pending); /* level + 256 - offset,
	minus 128 if pending (384 - phi*128, or 384 - phi'*128 pending: the pending
	open sets level 256) */
static inline uint32_t aius_tx_fill_open_level_frames(uint32_t target_frames,
	bool boundary_pending); /* target, plus 128 if pending (F3) */
typedef enum { AIUS_TX_FILL_OPEN_WAIT = 0, AIUS_TX_FILL_OPEN_NOW = 1 } AiusTxFillOpenDecision;
static inline AiusTxFillOpenDecision aius_tx_fill_open_decide(bool first_in_unit,
	uint32_t committed_since_arm_frames, uint32_t unit_frames); /* NOW at a unit
	start, or once committed_since_arm_frames >= unit_frames (the cap) */

/* aius_codec.h (unit-start); appended so positional initialisers stay valid;
 * AIUS_CODEC_ABI_VERSION 1 -> 2 */
typedef struct { int32_t *planes[AIUS_MAX_CHANNELS]; uint16_t channels;
	uint32_t frames; uint32_t sample_rate_hz; uint32_t channel_mask;
	uint8_t first_in_unit; /* 1: a gate open may land here */ } AiusPcmBlock;
```

- **Before spawning**, the parent lands `-I$fw_root/lib/aius_audio/src` on
  the clock line at `firmware/scripts/test.sh:517` (done 2026-10-06,
  uncommitted until the fill-loop-core slice first runs the suite; add
  `-lm` there if the plant needs it).
- **fill-loop-core** consumes nothing new. The `test/clock` plant includes
  the Phase 3 sensor header read-only for its arithmetic and derives
  `window_seq` and `exclusions` from its own injected faults (a window
  closes every 192,000 frames). It **calls** the shipped
  `aius_tx_fill_boundary_pending`, `aius_tx_fill_open_setpoint_frames` and
  `aius_tx_fill_open_decide` rather than modelling them, so a sign or
  deferral error in the shipped rule fails the clock suite (clean-room 1).
  Its only other sensor calls are `aius_tx_fill_saddr_offset_frames`
  (`aius_tx_fill_sensor.h:153`), `aius_tx_fill_unit_frames` (`:173`),
  `note_underrun`, `note_missed_fill` and `commit`, whose signatures
  fill-sensor-window must not change; it never calls `exclude` (clean-room
  5). A host case feeds a FILL readback that differs from the centre and
  asserts the centre is unchanged (clean-room 4).
- **clock-kind** updates `test_clock_target.cpp:501,544,612,614,649` to the
  new signature and adds a FILL stamp, a CENTRE stamp and a refused FILL
  keeping a pending CENTRE's kind; `itcm-placement` records the ISR and
  accessor bytes.
- **fill-sensor-window** consumes nothing. It lands the three pure opening
  functions first (the plant depends on them), with tx-fill-sensor cases:
  pending true and false at both halves and at the half edges, setpoint
  `384 - phi' * 128` pending (level 256) and `384 - phi * 128` not, the open
  level (`aius_tx_fill_open_level_frames`), one setpoint case
  at a level other than 128 (N3), and the open
  decision at a unit start, mid-unit below the cap and at the cap.
- **unit-start** sets the flag after `aius_pipeline_slice_pcm_block`, in
  `fill_pcm_descriptor` (`aius_pipeline.c:1124-1133,1281-1294`; a retried
  block keeps its `sink_block`, so the flag survives a retry) and in
  `fill_silence_descriptor` (`:748-775`, which memsets `sink_block` at
  `:767`): a substituted block is marked when its first frame starts a unit
  of the shape `note_substitution_shape` records, so a substitution
  covering several units marks one block per unit (clean-room 3). Pipeline
  suite cases: AC-3 marks block 0 only; E-AC-3 the first slice only;
  DTS-2048 1 of 4 slices; DTS-512 every slice; PCM every block; a retry
  keeps the flag; a multi-unit substitution marks once per unit. It bumps
  `AIUS_CODEC_ABI_VERSION` to 2 (a layout change; orchestrator call) with
  `test_codec_api_contract.c:10` and `test_capability.c:330`. It converts
  the nine positional `AiusPcmBlock` initialisers in
  `firmware/test/dsp/test_dsp.c` (`:594`, `:1304`, … `:3215`) to designated
  initialisers, since the appended field trips
  `-Wmissing-field-initializers` under `-Werror` (`test.sh:77`).
- **parent-wiring** consumes everything: local `extern "C"` declarations
  (the `optical_main.cpp:1440` pattern), the enum and exclusion-bit
  `static_assert`s, the trim branch at `:8851-8900` replaced by `fill_step`,
  plan, `request_trim(kind)`, `note_emit`; `first_in_unit` ignored on replay
  blocks (`:2119`). `tx_gate_execute_open`, inside its masked section,
  calls the accessor (which alone calls `aius_tx_fill_boundary_pending` on
  its one SADDR read; never a second SADDR read in the parent), then
  `aius_tx_fill_open_setpoint_frames` and `aius_tx_fill_open_decide` with
  the accessor's pending result, and publishes `fill_open_boundary_pending`
  with the setpoint (clean-room closure N1). It bounds the new keys in `status-body-bound`, and runs every target build itself, one
  at a time; slices run host suites only.

**Host proof (clock suite).** The tracker API owns the centre/integral
wiring, so the model exercises it end to end. Plant: TX consumes in 128-frame
periods at the output rate; the producer commits 512-frame PCM blocks and
6 × 256-frame AC-3 bursts at the source rate with service jitter; the sensor
is the Phase 3 header with the Phase 3 noise; the carrier estimate is
quantised to 140 mHz bins with 1.2 Hz spread and slow wander (280 mHz over
8 min). Cases, two simulated hours each unless noted:

- offsets against the centre of 0, ±225, ±480, ±550, ±650 and ±800 mHz,
  each with the gate opening at `phi` 0, 0.47 (the worst phase under the old
  design) and 1: zero underruns, the setpoint equals the opening occupancy,
  fill error ≤ 64 frames throughout, no governor trim, modulation within
  ±50 mHz, writes within the threshold's bound, and at steady state the
  offset is carried by `I` (P term near 0);
- the opening kick: the first windows after an open stay within 16 frames
  of the setpoint and 200 mHz of the centre;
- a gate open with a pending period boundary (SADDR in `last_half`): the
  open pads to level 256 (`gate_open_pending_padded_frames` +128), the
  setpoint is `384 - phi' * 128` (not 128 or 256 higher), no underrun at
  the open, no governor trim over the following hour;
- a gate open armed at AC-3 block k = 1 to 5 and at an E-AC-3 mid-unit
  block: deferred to the next unit start, no underrun at the open; a class
  that never marks a unit start opens after one unit duration;
- a source drifting past centre + 900 mHz during an underrun recovery: the
  same-sign retarget commits at once, no deadlock, no governor splice; an
  opposite-sign retarget defers and commits by 120 windows;
- a full apply (relock or fault retry) during a deferral: the pending centre
  lands in the desired plan, `I` zeroed, the readback notes once (dark
  transition ends), and retargets resume after the relock;
- a relock at a different rate (same nominal and across a nominal change):
  the first FILL write is composed on the new centre, never the old;
- a `clock_record_fault` dark transition with no apply: its end does not
  note, and the centre is unchanged;
- a replay or carrier loss during a deferral: the pending centre is
  discarded, the replay plan stays exactly 48,000,000 mHz, replay ready;
- a deferral with re-proposals every 10 s: the retarget commits by window
  120;
- a CENTRE-needing commit whose write is refused, then lands in a spent
  window: the eventual write is CENTRE and stamps the centre pair;
- a CENTRE-needing commit refused, then sub-threshold, then a later noise
  write: that write is FILL; a full apply while the flag is up clears it;
- a deferred centre whose estimate drifts back inside the old centre's
  hysteresis: the pending centre is dropped, nothing commits;
- a committed centre whose plan is invalid faults as today; one that is
  structurally different takes the structural full apply with `I` zeroed;
- a gate open armed at DTS-2048 slice k = 1 to 3: deferred to the next
  access unit's first slice, no underrun at the open;
- one injected underrun at the setpoint with `I` at −670, 0 and +670 mHz:
  no second underrun, no governor trim, worst recovery within 16 frames
  stated (about 8 min expected);
- a centre retarget during clamp saturation (underrun recovery), each sign:
  composed target never steps past the loop's own demand;
- an invalid window with underrun bits: reads 128; with stall bits or no
  underrun: `I` held, nothing emitted; a q4 0 with `fill_level_valid` 0 is
  never read as a level;
- a FILL write's readback does not note a target: `last_target_request_ms`
  and the hysteresis reference unchanged;
- a refused FILL call while a CENTRE trim is pending: the pending kind is
  kept (clock-target);
- ±950 mHz past the clamp, with an injected estimate bias of at least
  450 mHz so the centre stays 950 off (without it the centre retargets within
  10 s): offset pinned at the clamp, and the 50 mHz residual either drains
  the ring to an underrun or fills it to a governor splice about every
  85 min ((384 - 128) / 0.05 s); both signs stated, neither worse than today;
- a mid-lock same-nominal gate blip, re-opening at AC-3 block k ≠ 0: the
  open defers to the burst start, the setpoint is re-read, `I` kept, no
  transient beyond the modulation bound;
- the trough-timing jitter measured in Phase 3, injected into the sensor
  model for every case;
- a PCM-to-AC-3 switch with the gate open, modelled from the transition the
  capture recorded (the gate stays open through a class change: the open
  decision `aius_tx_gate_permitted(carrier, g_clock_live_ready,
  g_clock_replay_ready, source)` at `firmware/src/optical_main.cpp:8973-8977`
  takes no class input; and the real gap may re-base the
  floor): its assertion is set from that capture, not assumed;
- a centre retarget with non-zero `I`: composed target continuous;
- a 2,048-frame stall backlog: the governor trims once, the loop neither
  fights it nor overshoots into an underrun;
- replay with the gate open: no emission, replay readiness kept;
- a same-nominal gate blip keeps `I`; a nominal change, replay or full apply
  zeroes it;
- a trim refusal: emission state uncommitted and retried next pass, while
  controller state keeps advancing; a sub-threshold window still integrates;
- a bumpless centre retarget that writes nothing: committed once, not
  re-fired every 10 s, and the desired plan follows it;
- a composed target outside the plan band or the structural cell: clamped,
  no fault, no permanent refusal;
- a source warm-up ramp of about −14 mHz/min (−250 mHz over 30 min, as
  measured 2026-10-05): tracking error within about 3 frames, no underrun;
- an in-lock class switch with a 4-frame floor bias and a floor re-base:
  no underrun, modulation within the bound;
- every case at 48, 44.1 and 32 kHz window lengths (4, 4.35 and 6 s),
  because `Ki` is per window: no underrun, fill error ≤ 64 frames;
- the closed-form figures in entry gate items 4 to 6 (34.5-frame step peak,
  10.3 min settle, write fraction at 20 mHz, the item 6 recovery times)
  reproduced within 20 percent;
- fixed point: negative `e_q4` rounds symmetrically; `345 * e_q4` and the
  int64 retarget term do not overflow across the plan band.

**Device proof.** One listening-room AC-3 session of 45 min or more polled with
`firmware/tools/device/aius_status_poll.py`, plus an unpolled segment of
10 min or more (`--interval 30`). Pass criteria:

- `underrun_episode_count` delta 0 and `governor_trims` delta 0;
- settled |`fill_offset_millihz`| of at least 100 mHz, or the session is
  marked inconclusive (a source near zero offset proves nothing);
- settled `fill_level_q4` within ±16 frames of `fill_setpoint_frames`, and
  the setpoint matching the formula for the logged
  `fill_open_boundary_pending` (257 to 384 in both cases;
  the "unchanged standing latency" outcome; clean-room 6);
- settled mean |fill error| ≤ 4 frames over the last 10 min (noise σ 1.2,
  threshold 1.5): a P-only regression (`Ki` truncated to 0) holds offsets up
  to 208 mHz inside ±16 frames and would otherwise pass;
- `fill_trims` delta at most the `fill_window_seq` delta (all closes, N10)
  plus 1 (R12), and at most half of it
  in a polled session (entry gate item 5).

Before reading the session, fix the reducer
(`firmware/tools/device/aius_fill_capture_analysis.py:344-366`): split
`episode_free_slope` spans at a gate close, so a relock's re-base is not
fitted as drift (the 2026-10-05 +0.38 frames/s artefact). Proof: its
`--self-test` gains a case with a gate close inside a span. Done: the
split and its case landed with the Phase 4 build (`e019dc0`); 2026-10-06
added a split on `gate_open_pending_padded_frames` (F3) as well
(`PAD_KEYS`), `--self-test` ok.

Bench: gate 1's rebased criterion (`max_follow_error_millihz` should stay
about the estimate's own spread, 798 of 1,000 on 2026-09-04; if it breaches,
the owner rules on the criterion before acceptance), the transition gates
whose stable-phase labelling uses the same bound
(`firmware/tools/net/aius_rate_bench.py:410-424`), and gate 7's
carrier-continuity check at fill-write cadence, since the existing evidence
covers isolated NUM writes only.

**Device evidence 2026-10-06, `df966f17b370`, SHORT.** listening-room AC-3,
11:35:22 to 12:15:36 UTC: 33.7 min polled at 1 Hz (a 7 min unpolled gap
at 12:00 from a late segment launch; counters continuous), 0 min
unpolled (source switched off at 12:15:37 before that segment). Every
criterion that could run passed:

- `underrun_episode_count`, `governor_trims`, `tx_underrun_frames`,
  `tx_missed_fills` deltas 0;
- settled `fill_offset_millihz` about −233 mHz (not inconclusive);
- setpoint 377 throughout (`fill_open_boundary_pending` false, inside 257
  to 384), max |fill error| 8 frames, last-10-min mean 0.99 frames;
- `fill_trims` 184 against `fill_window_seq` 604 (30.5%);
- `centre_writes`, `trim_updates`, `structural_replans`,
  `invalid_windows`, both pad counters: delta 0; window-to-window σ 1.14
  frames;
- polled sub-window spread p95 52 frames (median 7, max 75); no unpolled
  figure, so Phase 4b's comparison is open;
- two owner seeks (11:44 and 11:59 UTC) left no trace: gate open, no
  counter moved, fill level 370 to 379 frames.

Not yet proven: 45 min polled, 10 min unpolled, and gate 7 at fill-write
cadence. Next session: one polled run of 45 min or more and an unpolled
run of 10 min or more, launched back to back (a background job ends at
about 30 min, so two polled jobs with no gap), then gate 7. Raw data:
`.scratch/2682a96a-3760-4d00-b9e0-f71f66b336ba/session-4a/` (reaped about
2026-10-13).

**Device evidence 2026-10-07, `df966f17b370`, full length.** listening-room
AC-3, 07:12:37 to 08:12:50 UTC: 48 min polled at 1 Hz (two segments, 5 s
gap), then 12 min unpolled at 30 s. The polled span passed every
criterion: underrun and governor deltas 0; settled offset −255 mHz;
setpoint 353 (not pending), last-10-min mean |fill error| 1.14 frames,
max 7.75 after the first 600 s (21.4 at 80 s, before settling);
`fill_trims` 221 of 721 windows (30.7%); every other counter delta 0.

- **Intentional source silence is tolerated.** The owner's 25 s episode
  change (08:05:49 to 08:06:14) kept the gate open and the carrier locked;
  the fill level moved ±20 frames and recovered by 08:10:50, no underrun.
  This is now an acceptance criterion: a source silence or pause within a
  lock never underruns.
- **Web UI load underran the ring.** At 08:11:38 the owner opened the SD
  web UI: 108 HTTP requests and 5 SD download jobs in one 30 s window,
  HTTP checkpoint 1.4 ms to 20.9 ms, an epoch budget overrun, ingress
  backlog high-water 2048 words (8 chunks, a 21 ms wait; capacity 8,192). Two 128-frame underruns 0.93 s apart
  (`underrun_episode_count` +2, `tx_underrun_frames` +256). AC-3, fill
  loop, DMA and governor ruled out (no crc, resync, missed fill, governor
  trim, or fill write in the prior 52 s); the recorder's underrun capture
  has continuous programme. Recovery was as designed (entry gate item 6):
  +256 step, offset on the +900 mHz clamp, back near setpoint in about 5
  min, no further underrun. Owner ruling (2026-10-07): loading the SD
  interfaces must never cause audio issues; the audio hot path always has
  priority. Moved to Phase 4b, widened.
- **Phase 4b's question answered.** Polled sub-window spread p95 54
  frames against unpolled 10 (median 8 against 7.5): polling inflates the
  tail, so 4b is needed.

Phase 4's fill-loop criteria are met; the remaining proof is gate 7 at
fill-write cadence (bench, U24). Raw data:
`.scratch/63e414f8-e0a8-416d-8629-cda946de784a/session-4a-full/` and
`underrun-0811/` (reaped about 2026-10-14).

Prebuild review: rounds 1 to 3 and the clean-room confirmation are done
(records in Phase 3). Round 4 (2026-10-06, against the decided entry gate):
NOT READY, two blockers (the constant-occupancy pad's fixed residue; the
open's phi read racing the ISR) and nine contract-text findings, all folded
above; owner settled the setpoint scheme, the invalid-window rule and the
burst-start open, and accepted the orchestrator's recommendations for the
rest of the phase. Round 5 (same reviewer, 2026-10-06): NOT READY, three
blockers the round 4 fixes opened (pending-boundary sign, deferral
deadlock, an unobservable burst start) and four contract-text holes, folded
above with §Slice interfaces. Round 6 (2026-10-06): NOT READY, two
blockers (DTS slices all marked; two centre references after a relock) and
five contract-text holes, folded. Round 7 (2026-10-06): READY; three text
fixes (accept sets the reference, the CENTRE flag's clear paths, four apply
sites and a stale-pending drop) folded. Clean-room (2026-10-06): NOT
READY, one blocker (the opening rules were tested only against the plant's
own copy, so the round 5 sign error or a DTS mis-mark passed every host
case; now three shipped pure functions the plant calls) and nine text
fixes, folded. Closure check (2026-10-06): READY; N1 (the accessor alone
reads SADDR), N2 (opening functions always-inline, not COLD) and N3 (a
non-128 level case) folded. Prebuild review complete. Phase 4's delivery audit re-attacks: the opening
read and setpoint; the controller and emission state split under refusal,
saturation and sub-threshold windows; the stamp-kind field on every write
path; and the fill loop against the governor.

**Build record (2026-10-06).** Slices fill-sensor-window, clock-kind,
unit-start and fill-loop-core landed with their host suites green; the
clock suite carries the full host-proof list. Measured against the entry
gate's closed form: 700 mHz step peak 36.0 frames (34.5), back inside 16
frames in 277 s (the gate text's "about 3.5 min" should read about
4.7 min, as the continuous PI gives 283 s), settled in 405 s (618 s was
the envelope bound), write fraction 0.39, modulation 17.1 mHz rms,
underrun recovery 101 / 145 / 489 s for `I` −670 / 0 / +670 (gate: 99 /
144 / 491), ramp tracking −2.71 frames. Plant readings to know: anti-windup
holds `I` when |P + I_old| > 900; the offset matrix holds the centre fixed
(estimate noise covered separately up to ±480, because ±650 to ±800 plus
the full 1.2 Hz estimate noise exceed the 670 authority budget); only the
offset matrix runs at all three rates. The tracker gained
`fill_proportional_millihz` (the last P, needed to hold the offset at the
clamp across a saturated retarget). Delivery-audit items from the slices:
- **Untested unit-start cases.** DTS-2048 and a multi-unit substitution
  cannot be driven from the pipeline API (the corpus has only DTS-512, and
  every substitution today is one unit); the multi-slice
  `retained_offset == 0` path is covered through E-AC-3. Add a DTS-2048
  vector when one is captured.
- **Cap opens mid-unit.** A class that never marks a unit start opens at
  the cap and took 7 underruns at the open in the plant: the accepted cost
  of the guard, worth confirming against real classes in the audit.
- **Sensor sample between a DMA crossing and its ISR** reads 128 high;
  harmless at real ISR latency, but the plant models latency only at the
  open.

Parent-wiring landed (2026-10-06): both images build; ISR 496 B (+12),
accessor 92 B, ITCM 191,996 B (+192), DTCM free 25,760 B (1,408 over the
floor), OCRAM free 131,776 B; status worst case 14,981 B (1,402 B
headroom). Recorded deviations: `aius_tx_fill_open_decide` runs before the
mask (it reads no ISR state; the setpoint is still computed inside);
`clock_fill_service` runs before `clock_refresh_applied` (on the dark-end
pass emission is blocked anyway); a centre commit now counts
`trim_updates` and records the event at commit, not at an accepted trim;
carrier loss and the rate-change quiesce also call `fill_reset(true)`; the
reducer also splits a span when `gate_open_padded_frames` changes between
two open polls.

Gate 1's 1,000 mHz follow-error bound was set on a bench with no underruns;
an underrun's clamp excursion (entry gate item 6) would breach it, so a
listening-room session reads gate 1 only across underrun-free spans.

### Phase 4a Delivery-audit fixes before the device session ✓ SHIPPED 2026-10-06

```yaml
phase: 4a
title: Close the Phase 4 delivery audit before the listening-room session
status: shipped
depends-on: phase 4 build (committed 2026-10-06, host suites and both images green)
outcome: >-
  The Phase 4 build carries no known path to a periodic underrun, its bench
  tooling can see fill writes, and its stamps and counters say what
  CLAUDE.md tells a reader they say.
unit: component-behind-interface
seams:
  - aius_tx_fill_open_setpoint_frames and tx_gate_execute_open pad (F3)
  - clock_fill_service / clock_refresh_applied order, centre commit gating (N5 to N8)
  - gate 1 and gate 7 bench tooling (F2, N12)
targeted-proof:
  - host: clock (new cases per item), tx-fill-sensor, clock-target, optical-cutover, status-body-bound; both images build
  - bench: aius_rate_bench gate 7 at fill-write cadence with the fixed matcher (moved to the Phase 4 device session; needs the image deployed)
review-intensity: active
```

Shipped on host proof: clock, tx-fill-sensor, tx-gate, tx-governor,
clock-target, optical-cutover, status-body-bound, itcm-placement,
registry and sd-editor green; both images build (ITCM free 4,612 B
plain, 4,340 B canonical); tool tests rate-gate-capture 93,
rate-runner 71, latency-runner 119. Delivery review FAIL, then same-reviewer
closure PASS, then clean-room PASS. B1 moved to Phase 4d. The owner
confirmed the autonomous calls R2, R3 and R13 (2026-10-06).

Delivery audit (2026-10-06, `reviewer` `opus`) of the Phase 4 build:
FAIL on B1 only; no blocking code defect. Each item decided by the
orchestrator under the owner's Phase 4 delegation:

- **B1, CLAUDE.md promote.** Moved to Phase 4d. Add `fill_trim_last_at_ms` and
  `fill_applied_at_ms` to the glitch-reading stamp list and the full
  `aius_ota_sign.py sign --image --output --build-revision` form, via
  `the release-promotion step`. Also add `clock.gate_open_pending_padded_frames`
  (F3 and R3: 0 to 128 per pending live open, 0 when the ring already
  holds 256 or more; by design) to the counters that read as faults and
  are not. Name the write counters: `clock.fill_trims` counts FILL writes,
  `clock.centre_writes` accepted CENTRE writes, and `trim_updates` every
  centre commit since N12, most of which write nothing (bumpless). Blocked while the file
  carries another session's uncommitted edits (re-check `git -C git_modules/aius diff --stat --
  CLAUDE.md` before acting; never from memory).
- **F2, gate 7 cannot see fill writes.** `rate_gate_capture/capture.py:559-576`
  and `net/aius_rate_bench.py:1033-1044` match waveform jumps against
  `trim_updates` only. Match against the two applied-at stamps
  (`trim_applied_at_ms`, `fill_applied_at_ms`) and add a fill-cadence
  assertion.
- **F3, pending-boundary open leaves 1 to 128 frames.** Decided: when
  pending, pad to the level-128 target plus 128, so after the late ISR the
  level is 128 and the setpoint is `384 - phi' * 128` as in the normal
  case; count the extra pad separately (new counter) so the CLAUDE.md
  `gate_open_padded_frames` line stays true. Update
  `aius_tx_fill_open_setpoint_frames` and its tx-fill-sensor cases, the
  plant, and §Mechanism's pending text in the same change.
- **N5.** Run `clock_refresh_applied` before `clock_fill_service`, matching
  the plant (`test/clock/test_clock.c:1543-1573`), so the dark-end note
  never resets a centre committed in the same pass.
- **N6.** Commit a centre only when `in->have_applied`, so a failing
  accept's apply does not spawn full applies outside the 1 s retry throttle.
- **N7.** Validate the plan before `clock_note_centre_commit`; an invalid
  centre neither counts nor stays in the tracker.
- **N8.** The CENTRE flag also clears on a pass with emission disallowed
  and nothing to write.
- **N9.** Carrier loss and quiesce clear `fill_open_boundary_pending` with
  the setpoint.
- **N10.** Device criterion compares `fill_trims` with `fill_window_seq`
  (all closes), not `fill_windows`; add a `centre_writes` counter (accepted
  CENTRE writes) so the one-write-per-window bound is checkable.
- **N11.** The accessor calls `aius_tx_fill_saddr_offset_frames` instead of
  re-implementing the arithmetic. Residual risk, recorded: the parent's
  `clock_fill_service`, `clock_fill_reset`, arm counter and note flag are
  copied into the plant, not called; a wiring divergence passes the host
  suite, so the device session and this audit carry it.
- **N12.** `trim_updates` now counts commits (including no-write and
  structural); gate 1's trim-cadence check (`aius_rate_bench.py:849-854`)
  moves to `centre_writes`. `firmware/tools/latency/linked_probe.py:457`
  was already stale; fix it in the same pass.
- **N4.** Correct the `tx_gate_execute_open` comment: the masked section
  runs inlined in `.flashmem.audio_enqueue` (already true of `set_level`);
  record its duration. Derived from the `teensy41-optical` disassembly,
  not measured: 7 Thumb-2 instructions per padded frame, about 3 to 5 us
  for a live open's worst 256 frames and 9 to 11 us for a replay's 768.
- **Residual risk, recorded (F2).** The bench tools infer writes from
  changes of `trim_applied_at_ms` / `fill_applied_at_ms` between polls, so
  two writes inside one poll interval count as one; the fill-cadence
  assertion therefore bounds from below. `centre_writes` is the exact
  count where a gate needs one.

Delivery review of the 4a build (2026-10-06, `reviewer` `opus`): FAIL on
the bench tooling, no firmware underrun path. Decided (autonomous):

- **R1, gate 7 `invalid-window-count`** compared `invalid_windows` with
  every write, but only a full apply raises it. Compare with full applies
  only. `invalid_windows` rises on every successful apply (nominal
  accept, fault retry, replay apply) and `structural_replans` only on a
  replan, so the bench asserts `invalid_windows` delta at least the
  `structural_replans` delta; equality is not expected.
- **R2, match windows.** A FILL or trim write is a carrier-continuous
  numerator step and must never excuse a waveform jump; only full applies
  (invalid windows) open match windows. Fill writes stay visible to the
  trim-observed check. This supersedes F2's "match against the stamps".
- **R3, F3 zeros.** A pending open with ring level of 256 or more trimmed
  to 128 and then padded 128 zeros, putting 2.67 ms of silence between
  kept audio and the first block. One `set_level` to the final open level;
  `gate_open_pending_padded_frames` and `gate_open_trimmed_frames` count
  only frames actually padded or dropped.
- **R4, N5 incomplete.** The end-of-pass `clock_refresh_applied` (kept for
  the replay apply) can still fire the dark-end note after a same-pass
  centre commit. The note must not overwrite a centre committed in the
  same pass; the plant models both refreshes.
- **R5, openapi text.** Pending setpoint, `fill_trims` and
  `centre_writes` descriptions to post-F3 and post-N12 truth; drop the
  unenforced one-per-window claim for `centre_writes` (a FILL and a CENTRE
  may share a window).
- **R6, fill cadence.** Assert `fill_trims` delta at most `fill_window_seq`
  delta (the one-FILL-per-window rule), not only `centre_writes`.
- **R7, plant ISR order.** The plant ran the late ISR after the block
  commit, so deleting F3's pad passed every host suite. Run it at the
  unmask, before the commit, so that deletion fails.
- **R8, latency tools.** `latency/bench.py` zero-delta also checks
  `fill_trims`; `linked_probe.py` edge mode no longer requires an
  `invalid_windows` step for a CENTRE write.
- **R9, N7 deferral.** An invalid pending centre is dropped at once, never
  deferred under saturation.

Closure re-review (same reviewer): PASS. The R3 straddle (boundary
crossed during the pad after a not-pending read, about 0.05 to 0.2% of
opens) leaves the setpoint correct to one frame; not an underrun path.
Folded before commit:

- **R10.** The latency bench no longer rejects a bracket on a
  `fill_trims` delta (a 20 mHz continuous step; FILL writes land in up to
  half the 4 s windows); it reports it.
- **R11.** openapi `trim_updates` text says it counts every centre commit
  since N12.
- **R12.** Fill cadence bound is `fill_window_seq` delta plus 1: a window
  closed just before the first poll can be written after it.
- **Residual risk, recorded.** The plant skips the R4 note only on
  `centre_committed`, the firmware on every `clock_note_centre_commit`;
  equivalent in every traced case, the same copy-not-call risk as N11.

Clean-room confirmation: PASS, no firmware underrun path; doc errors in
this plan, the gate 7 README and one code comment fixed. Decided:

- **R13, gate 1 trim-cadence advisory in code.** N12 moved the bound to
  `centre_writes`, which is near-vacuous: centre commits are bumpless, so
  an unsaturated commit writes nothing. Bounding commits instead would
  fail every healthy run (D4 (b): the estimate quantum makes at most two
  post-settle commits unmeetable). The check now records post-settle
  commits and `centre_writes` and never fails, completing D4 (b).
- **R14.** The stamp-derived write count falls back to `trim_updates`
  only when no sample carries the stamps, not when one sample lacks one.
- **R15.** `linked_probe.py` edge mode fires on a centre commit
  (`trim_updates` +1), not on `centre_writes`.
- **Coverage gap, recorded.** No host case drives a pre-filled ring
  through `tx_gate_execute_open`'s pending trim and pad; the order is
  checked by reading. The Phase 4 device session's
  `gate_open_pending_padded_frames` readings carry it.

### Phase 4b No HTTP or SD work delays the audio producer

```yaml
phase: 4b
title: "Acceptance: HTTP and SD-card work never delays the live-input audio producer"
status: planned
depends-on: phase-4e, phase-4f, phase-4g, phase-4h
prebuild-review: superseded (round 1 reviewed the B2 design below; each of 4e to 4h carries its own gate) # 2026-10-07: rounds 2 to 5, clean-room, clean-room closure, clean-room round 3 and two closures; second closure READY; CR5-S1 to S4 folded; no further clean-room: the closing reviewer was itself clean-room and implementation carries a delivery audit per phase
outcome: >-
  During live input (optical AC-3 and PCM), no web UI load, file download,
  directory listing, card sync, status or meter poll, API request, DSP
  Apply or preset save causes an underrun or moves the polled sub-window
  spread beyond the unpolled one: HTTP is best-effort and audio never
  waits for a response or for the card. OTA staging, verify, activation
  and rehearsal are excluded (owner, 2026-10-07) and keep their
  state-the-cost-first posture; replay is out of scope (synthetic testing
  only).
unit: parent-only
seams:
  - the Phase 4h image as deployed (no new firmware in this phase unless a proof fails)
  - firmware/tools/rate_gate_capture and the 1 Hz status poller (polled and unpolled segments, content check)
  - CLAUDE.md §Device access and OTA, via the release-promotion step (text below)
targeted-proof:
  - "device (owner, AC-3 playing): ten web UI loads at least 5 s apart (index and mixer pages cold and warm, status, upgrade), one full aius-sd-sync.sh run, three DSP Applies and two preset saves and one preset delete give underrun_episode_count and tx_underrun_frames delta 0"
  - "device (owner): polled (full /api/status at 1 Hz, 10 min or more, at least 95% of polls answered 200 and capture.py's 503 skips under 5%, per 4f F5 and F6) versus unpolled (10 min or more, no browser open, http_requests checked) sub-window spread p95 within 10 frames of each other"
  - "device (POST /api/clear at session start): ingress_high_water_words at most the derived alarm, (8 ms producer budget + D) x 96 words per ms + one 256-word chunk: 1,216 words at D = 2,000 us (the post-4f target), re-derived when 4f's measured DSP Apply parse stretch (4f F7) exceeds 2,000 us and sets D; producer_pend_latency_max_us at most D, with the three Applies inside the measured window; stack_free_min_bytes at least 2,048; sd_mode DMA at the end"
  - "device: fault deltas 0 across the runs, taken outside any card-out interval (the 4h card-swap step when it shares the session; sd_diag read just before each pull and just after the first successful remount): sd_diag driver_faults, protocol_faults and busy_timeouts, sd_flush.failures, www_cache.crc_faults, stall-witness events, pipeline crc rejects, tx_missed_fills (sd_diag.open_misses and absent_mounts are not faults)"
  - "device (owner, PCM through the U24 bench loop per CLAUDE.md's bench-loop note, the mixer at unity, POST /api/clear at start, 10 min or more): full /api/status at 1 Hz with at least 95% answered 200, three DSP Applies each answered 200 with config_generation advancing, and underrun_episode_count and tx_underrun_frames delta 0; PCM has the smallest ring margin (128 to 225 frames, 4f F1), where round 2 B5's shedding lived (CR3-S1)"
  - "device (source off, /api/status tx_gate.open 0, at the session's start or in the card-swap step): a cold page load, 60 s of 1 Hz full status and one preset save: no request answers 503, every request answers its expected status at its first attempt (200, except the browser's GET /favicon.ico, which answers 404: no page links an icon and no route serves one, CR3-B4), and http_shed and sd_fifo_defers delta 0 (4f F11, CR-B1)"
  - "device: content check: every plain (non-bundle) file aius-sd-sync.sh uploads from firmware/sd/www/ downloaded byte-equal to its source (owner presets saved under /sd/www/presets/ have no sync source and are checked against their PUT bodies below, CR3-S4), and every bundle, gzip-decoded, byte-equal to firmware/tools/www_bundle.py's inlined page for the synced sources, as 4g run 1 checks (the decoded bundle is the inlined page, not any one source file, CR-S8); GET /sd/aius/dsp.json and every saved preset under /sd/www/presets/ equal to their PUT bodies, and /api/status showing the PUT public config (config.json has no read route), once sd_flush.dirty is 0 and again after a reboot (read from the card)"
  - "the promoted CLAUDE.md diff carries exactly the replacement paragraph below, with {ALARM} replaced by the ingress alarm this session derived from D, and the 'Two DTCM layout traps' paragraph after it; no foreign hunk reverted"
acceptance-examples:
  - the index page's presets listing (the likely 20.9 ms dispatch of 2026-10-07) is served from the PSRAM manifest and touches no card
  - a 100 ms SD read inside a sync upload leaves producer_pend_latency_max_us under 2,000 (the card wait runs the producer)
  - a DSP Apply is heard at once and its file reaches the card within seconds, with no underrun
  - an OTA verify during playback may still underrun; no proof, counter or CLAUDE.md line claims otherwise
review-intensity: active
```

**Re-plan 2026-10-07: 4b is the acceptance umbrella.** The owner's
second decision block (below) selected A-DMA over B2 and widened the work
to caching, write-behind and lighter polling. The build is phases 4e to
4h; 4b holds the rulings, the investigation records and the acceptance
session. Decision to phase map:

| Owner decision 2026-10-07 | Phase |
| --- | --- |
| A-DMA with a vendored SdFat; B2 only if the DMA device check fails | 4e |
| HTTP best-effort, 503 shedding; lighter polling (compact editor status, meters 4 to 5 Hz, self-scheduled polls) | 4f |
| PSRAM web cache on pad 2, CRC per 512 B chunk, SD fallback; gzip bundling with cache validation | 4g |
| Write-behind flushing within seconds; Apply heard at PSRAM accept; pending writes held on a card swap; the rename-gap fix: 4h's copy-before-write covers the settings files (`dsp.json`, `config.json`, presets); sync uploads keep the gap, a re-sync restores them (owner, 2026-10-07) | 4h |
| OTA excluded; replay out of scope | every phase (no proof claims either) |

**Images and owner time.** 4e needs two OTAs (DMA runtime-armed, then
DMA at boot), 4f, 4g and 4h one each (4f and 4g also a card sync of
`firmware/sd/www/`). Every AC-3 run needs the owner (the source plays
in the listening-room); 4h's card-swap check needs the owner's hands. To
save owner sessions, 4h's AC-3 run and this phase's acceptance may run
as one session on the 4h image.

**CLAUDE.md at ship** (`CLAUDE.md` §Device access and
OTA, via `the release-promotion step`, after the device proofs pass; this text
supersedes the B2 fallback's draft below): replace the paragraph from
"Owner-initiated card traffic is deliberately NOT gated" through "state
its cost first rather than after." with the text below; the bench
sentence that follows ("On the bench there is never audio to
interrupt") stays as it is. `{ALARM}` is the ingress alarm the 4b session
derives from D (1,216 words at D = 2,000 us; re-derived when 4f F7's
measured parse cost sets a larger D), written in at promote (CR-S7).

> **HTTP and SD-card work never costs live-input audio; OTA is the one
> exception.** In DMA mode (the boot default) every SD-card wait between
> card commands runs pending audio production (vendored SdFat with a
> guarded wait hook). In FIFO mode (`POST /api/sd/mode`, the field
> fallback) card data waits spin, so while live input plays a new SD job
> waits for the input to stop and an HTTP SD start answers `503
> audio_busy` after 500 ms, while a job started before the input began
> runs to its end and may cost a dropout.
> Every checkpoint boundary
> and status-render append is a yield point, HTTP work the ring cannot
> absorb during live input is shed with `503 audio_busy` (with no live
> input nothing is shed), the web app is served from a
> PSRAM cache, and settings writes complete into PSRAM and reach the card
> within seconds (aius-sample-rate-following Phases 4b, 4e to 4h). So a
> web UI load, a download, a card sync, a status poll, a DSP Apply or a
> preset save during live playback costs no dropout and needs no cost
> statement. `audio_diag.producer_pend_latency_max_us` is the evidence;
> `ingress_high_water_words` above about {ALARM} means a stretch escaped.
> A `503 audio_busy` during playback, and rises in
> `checkpoint_walltime_us`, `runtime_epoch_budget_overruns` and
> `http_service_overruns` from nested producer passes, are not faults. A
> power cut can lose the last few seconds of settings changes
> (`sd_flush.dirty`); pending writes are held across a card swap until
> the same card returns or `POST /api/sd/pending/discard`. `POST
> /api/reboot` refuses with `409 sd_flush_pending` until the queue is
> drained; a held or repeatedly failing entry needs the discard first.
> Replay is for synthetic testing and outside the guarantee. OTA staging,
> verify, activation and rehearsal stay outside it too (owner,
> 2026-10-07): activation and rehearsal quiesce audio by design and
> refuse with `409 sd_flush_pending` until the queue is drained, and a verify
> measured one 80 ms stall, so prefer silence, and when the owner accepts
> the interruption, state its cost first rather than after.

Also promoted at 4b (found in 4e's build, 2026-10-07), as a new
paragraph in the firmware conventions:

> **Two DTCM layout traps.** The Teensy core maps a 32 B no-access MPU
> region at `_ebss` as a stack-overflow trap (`cores/teensy4/startup.c`
> `configure_cache()`), so anything that walks the stack zone (the
> paint and scan behind `stack_free_min_bytes`) starts at
> `(_ebss & ~31) + 32`; touching the trap faults every boot before the
> watchdog arms. And the core's `_VectorsRam` is `aligned(1024)` in DTCM
> `.data`, so `.data` grows in 1,024 B steps: a few bytes of new DTCM
> `.rodata` (format strings, constant tables) can cost the stack zone a
> kilobyte. Put new strings and tables in flash (`PROGMEM`/`FLASHMEM`)
> and read slot-a's `.data` size and stack zone after every link.

**Owner ruling 2026-10-07:** loading the SD interfaces must never cause
audio issues; the audio hot path always has priority. OTA is the
exception: staging and activation are deliberate, disruptive operations
and stay outside the guarantee. This widens 4b from
the status render to every HTTP and SD service path, and supersedes
`CLAUDE.md`'s "owner-initiated card traffic is
deliberately NOT gated" (that paragraph is updated in this phase; until it
ships, card traffic still costs dropouts). Evidence: Phase 4 §Device
evidence 2026-10-07 (page-load underrun; polled p95 54 against unpolled
10). Design pass done (2026-10-07, now §Fallback design below); the
owner's direction block re-planned the build as phases 4e to 4h.

Found by the Phase 4 entry-gate design pass (2026-10-06): Phase 3 measured
the render delaying production, and Phase 4 only adds margin against it.
The 16 KiB status body makes the render longer (dip estimated 1.5 times).
The status path is settled as per-append yields (B2 under its producer
gate; under A-DMA, Phase 4f's yield points).

#### Fallback design (not selected): the producer runs in its own interrupt (2026-10-07)

Kept as the record and as Phase 4e's fallback: the owner selected A-DMA
(§Owner decisions, direction block below), and B2 is built only if 4e's
DMA device check fails on data integrity or DMA reliability. Its draft
contract, if revived: slices producer-gate (`firmware/lib/aius_producer_gate/**`),
producer-schedule (`firmware/test/producer-schedule/**`), io-windows (the
SD and network adapters with their suites), producer-isolation and
parent-wiring; proofs as in §Slice interfaces and §The bound it
guarantees, amended by prebuild round 1 (B1, B2, S1 to S9). Its CLAUDE.md
draft below is superseded by the umbrella's text above.

**Measured cause.** The producer is `aius_audio_service_run`, called only
by `platform_audio` at the two AUDIO visits of each epoch
(`firmware/lib/aius_runtime/src/aius_runtime.c:114`, `:129`); it ingests
the ingress ring, decodes, runs the DSP and commits 128-frame blocks to
the transmit ring inside `enqueue`. Every other checkpoint runs serially
in `loop()`, so a burst that lands while any one checkpoint runs waits
for that checkpoint's full length. The ring drains 48 frames per ms from
its trough: at setpoint 353 the underrun line (occupancy 128) is 225
frames, 4.69 ms, away; at the worst-phase setpoint 256 it is 128 frames,
2.67 ms. On 2026-10-07 `checkpoint_walltime_us[5]` (HTTP) went from
1,402 µs (its maximum over 48 min of 1 Hz polling, status render
included) to 20,916 µs in the 30 s holding the page load,
`production_gap_max_us` from 30,086 to 35,823, and two 128-frame
underruns followed 0.93 s apart. HTTP checkpoint 5 runs one transaction
step and one action per epoch; the only actions that touch the card
there are the raw scans in dispatch. The index page loads 5 SD files
(`index.html`, `replays.css`, `dsp-editor.css`, `sd-shared.js`,
`dsp-editor.js`, job kind 4 in SD checkpoint 8) and lists
`/sd/www/presets/` (`firmware/sd/www/dsp-editor.js:1832`), which
`app_read_directory` serves with one `SD.open` and up to 129
`openNextFile` calls in a single dispatch
(`firmware/src/optical_main.cpp:6792`). That listing is the likely
20.9 ms (inferred: there is no per-route timing). The second underrun is
unattributed: checkpoint 8's lifetime maximum (582,330 µs, the boot
mount) hides the download primitives. Correction to §Device evidence
2026-10-07: 2,048 words is not the ingress cap
(`AIUS_AUDIO_INGRESS_CAPACITY_WORDS` is 8,192,
`firmware/lib/aius_audio_ingress/include/aius_audio_ingress.h:14`); it is
8 chunks, 21.3 ms of input at 96 words per ms, the backlog of a 20.9 ms
wait. The ingress high-water is therefore a direct reading of the longest
producer wait; its baseline is 616 words.

**Worst-case service time per path today**, each a single stretch the
producer waits behind:

| Path | Where | Worst case per pass | Source |
| --- | --- | --- | --- |
| Socket accept, read, write (512 B), close | HTTP 5, `aius_network_teensy_*_once` | tens of µs, non-blocking FNET calls | code; HTTP max 1,402 µs over 48 min polled |
| Status render | HTTP 5 dispatch, `http_status_json` | 1,402 µs at 9.7 KB; about 1.5 times at the 16 KiB worst body | Phase 4 session; Phase 4 entry gate |
| Directory listing | HTTP 5 dispatch, `app_read_directory` (`/sd/<dir>/`), `http_replay_list_json` (`/api/replays`, no `sd_busy` gate) | card-bound; 20.9 ms observed | this session (attribution inferred) |
| Replay delete | HTTP 5 dispatch, `app_replay_delete` | `SD.exists` plus `SD.remove`, card-bound | code |
| SD job primitive (download, upload, install rename, delete) | SD 8, `aius_sd_teensy_execute` | one SdFat call, card-bound: read access up to 100 ms and write busy up to 250 ms by the SD physical-layer limits; `SD.usedSize()` is a FAT scan; 29,183 µs in the boot fault record | `sd_diag` |
| Card sync | HTTP 5 during `aius-sd-sync.sh` | 1.55 s epoch, 59 ms of underrun | register, 2026-08-16 |
| SD lifecycle, replay refill, audio control, recorder drain | REPLAY 7, `platform_replay` | 1,060 µs this session | `checkpoint_walltime_us[7]` |
| FNET poll | NETWORK 4, `aius_network_teensy_poll` | short; 1,557,632 µs once at boot (stack start) | `checkpoint_walltime_us[4]` |
| OTA verify (outside the guarantee) | HTTP 5 dispatch | 79,952 µs (Ed25519) | register, 2026-08-16 |
| Producer itself | AUDIO 9 | 7,609 µs (budget 8,000), already inside the trough | `checkpoint_walltime_us[9]` |

The SdFat SDIO driver runs in FIFO mode (the framework's `SD.begin`
passes `SdioConfig(FIFO_SDIO)`), whose read waits spin without `yield()`
(`SdioTeensy.cpp` `readData`, `waitTimeout`), and the card, not the
firmware, sets their length. No slicing of card work can bound a single
primitive below 4.7 ms.

**Options.**

| Option | Mechanism | Worst-case producer delay | Web UI | Complexity | ITCM / RAM | Host testability |
| --- | --- | --- | --- | --- | --- | --- |
| A: cooperative budget | AUDIO pass after every checkpoint; render chunked across epochs; directory scans become SD jobs; per-pass HTTP and SD budget | the longest indivisible step, one SdFat call: card-bound, tens to hundreds of ms | slower pages (render and listings spread over epochs) | medium: render cursor, two new SD job kinds | about 1 KiB RAM; no ITCM | budgets yes; card latency no |
| B1: producer interrupt, lock by exception | producer in the lowest-priority interrupt, preempting anywhere; main loop masks it only around shared-state access | lock hold plus higher ISR time, about 0.3 ms | unchanged | high: every main-loop access to producer state must be found and locked; a miss is a silent race | about 32 B; producer stack nests over any main-loop depth | races are not host-testable |
| B2: producer interrupt, lock by default (recommended) | producer in the lowest-priority interrupt; main loop keeps it masked except inside I/O windows (adapter calls, raw scans) and at yield points (checkpoint boundaries, render appends) | longest locked stretch plus higher ISR time, about 0.6 ms, watermarked | unchanged; HTTP and SD lose only the producer's own CPU | medium: one gate module, windows in two adapters and three scans, render yields | about 32 B DTCM, under 64 B ITCM (the `update` pend); producer stack nests only over window depths | gate and schedule model on host; isolation post-link; bound by device watermark |

A `yield()` hook into SdFat (DMA SDIO mode) is A's best case and was
rejected with it: FIFO-mode reads still spin, and the hook re-enters the
producer from every SdFat caller, including the replay refill in the
middle of the REPLAY audio-control sequence.

**Recommendation: B2.** It is the only option whose bound does not depend
on the card or on HTTP: a window around an SdFat or FNET call lets the
producer preempt at any instruction inside it, including a FIFO spin or a
1.55 s sync epoch. Against B1 it keeps every epoch-serial assumption in
`optical_main.cpp` true without an audit (for example "Pipeline state is
AUDIO-owned and HTTP runs serially in the same epoch", `:1696`, and
`app_store_dsp_config` rewriting `g_pending_dsp_model` while an earlier
`g_dsp_apply_pending` may still be set, `:6475`): outside a window the
main loop and the producer stay mutually exclusive exactly as today.
Concurrency is confined to window bodies, which touch only SdFat, FNET,
adapter-private state and request-local buffers, and the post-link check
sees that the producer reaches none of them. B2 fails safe: a locked
stretch that grows shows in `producer_lock_hold_max_us` and
`producer_late_passes`, where B1's failure is a race.

**Mechanism.**

- **Producer interrupt.** A spare NVIC vector (the implementer picks one
  unused by the core and libraries, documented beside `IRQ_SOFTWARE`) at
  priority 240, the lowest implemented level: below the audio software
  interrupt (208, `update`), the transmit DMA interrupt and every other
  ISR, above thread mode. Its FLASHMEM handler takes the pend latency,
  returns if `!g_audio_service_ready`, and runs `aius_audio_service_run`
  once. It never re-pends itself.
- **Pends.** `AiusAudioProcessor::update` pends it after each ingress
  commit (375 Hz at 48 kHz; an inline `note_pend` stamps the first
  outstanding pend). `platform_audio` pends it inside its window, so the
  two AUDIO visits still produce exactly where they do today. A pend
  raised while masked latches and fires at the next window.
- **Lock by default.** `setup()` leaves the vector masked; the main loop
  holds it masked except inside `window_begin` / `window_end` pairs.
  Windows do not nest: `window_begin` inside an open window is a no-op
  that returns false, and `window_end(false)` leaves the window open.
- **Windows.** Around each SD and FNET adapter entry point (hooks set at
  init; null hooks mean no window, so host suites are unchanged); around
  the three raw scans and the two dispatch-time parses; and the AUDIO
  checkpoint. A window body may touch only its library's state, its
  adapter's state and request-local buffers.
- **Yield points** (`window_begin` then `window_end`, letting a pending
  pass run): in `platform_checkpoint` at every checkpoint boundary, after
  each `http_json_append` in the status sections, and between recorder
  drain quanta if the drain exceeds the hold target. Per-append
  consistency of `/api/status` replaces per-epoch consistency; fields
  read inside one append stay consistent.
- **Unchanged.** The epoch order; the watchdog feed (a wedged producer
  starves thread mode, so the epoch never completes and WDOG1 resets as
  today); `aius_audio_service_run` and its 8 ms budget; the fill sensor
  and gate-open masked sections (the DMA interrupt still outranks the
  producer); the REPLAY audio-control block, which runs locked and so
  never interleaves with a production pass. `ota_quiesce` masks the
  vector with the others.
- **Observability.** `audio_diag` gains `producer_passes`,
  `producer_pend_latency_max_us`, `producer_late_passes` (pend latency
  over 1,000 µs), `producer_lock_hold_max_us` (armed after the
  healthy-boot mark, cleared by `clear_counters`) and
  `stack_free_min_bytes` (boot-time paint of the stack region, scanned by
  the 1 Hz METRICS sampler). `checkpoint_walltime_us` and
  `runtime_epoch_budget_overruns` now include producer passes that
  preempt a window; a rise there is not an audio fault.
- **FPU.** The producer's double-precision DSP now runs in an interrupt;
  parent-wiring asserts at boot that `FPU_FPCCR` keeps automatic lazy
  stacking (ASPEN and LSPEN set).

**The bound it guarantees.** Producer start delay after a pend is
`D <= H_lock + H_off + I_hi`: `H_lock` the longest locked main-loop
stretch (target 500 µs, held by the device proof on
`producer_lock_hold_max_us`); `H_off` the longest global interrupt-off
section in steady state (the gate-open pad, 3 to 11 µs per Phase 4a N4,
and the few-µs SdFat and FNET sections); `I_hi` higher-priority ISR time
in that interval (under 100 µs). So `D` is at most about 0.62 ms, 30
frames at 48 kHz: 195 frames inside the 225-frame tolerance at setpoint
353, 98 inside the worst-phase 128, and 2 percent of the 32 ms AC-3 burst
period. No HTTP request, render, listing or card primitive appears in
`D`; their length only delays other main-loop work. The ingress backlog
is the safety signal: under `D` it stays within one 8 ms producer pass
plus a chunk, 1,024 words; a reading above that means a stretch escaped
the gate.

**Slice interfaces.** Parent-wiring builds against these; the host
slices implement them.

```c
/* firmware/lib/aius_producer_gate/include/aius_producer_gate.h */
typedef struct {
	void (*mask)(void *ctx);      /* NVIC_DISABLE_IRQ(producer vector) */
	void (*unmask)(void *ctx);    /* NVIC_ENABLE_IRQ(producer vector) */
	uint32_t (*cycles)(void *ctx);
	void *ctx;
	uint32_t cycles_per_us;
	uint32_t late_us;             /* producer_late_passes threshold, 1000 */
} AiusProducerGateOps;

void aius_producer_gate_init(AiusProducerGate *gate,
                             const AiusProducerGateOps *ops); /* masked, disarmed */
void aius_producer_gate_arm(AiusProducerGate *gate);          /* hold watermark on */
bool aius_producer_gate_window_begin(AiusProducerGate *gate); /* false: already open */
void aius_producer_gate_window_end(AiusProducerGate *gate, bool opened);
void aius_producer_gate_yield(AiusProducerGate *gate);
static inline void aius_producer_gate_note_pend(AiusProducerGate *gate); /* ISR-safe */
uint32_t aius_producer_gate_take_pend_us(AiusProducerGate *gate); /* producer entry */
void aius_producer_gate_get_diag(const AiusProducerGate *gate,
                                 AiusProducerGateDiag *out);
void aius_producer_gate_clear_diag(AiusProducerGate *gate);
```

Each adapter declares its own hook pair, so the adapters and the gate
share no header: `AiusSdTeensyWindow` and `AiusNetworkTeensyWindow`, each
`{bool (*begin)(void *ctx); void (*end)(void *ctx, bool opened); void
*ctx;}`, set by `aius_sd_teensy_set_window` and
`aius_network_teensy_set_window`. Every entry point brackets its library
call and re-closes on every return path, faults included (host cases
assert balance). The schedule model takes its constants from this design
(48 frames per ms, 128-frame DMA periods, 1,536-frame bursts every 32 ms
in 256-word chunks, 1,661 µs decode steps) and calibrates against
§Device evidence 2026-10-07 before asserting the B2 result. The isolation
check walks `arm-none-eabi-objdump` call edges from the producer handler
symbol, in the manner of `firmware/test/itcm-placement/verify_target.py`.

**Technical calls taken here.** Lock by default over lock by exception
(B2 over B1, above). Per-append yields over a snapshot struct for the
status render (no 16 KiB copy, no restructure of the section writers).
Priority 240, below every ISR. A hook pair per adapter over a shared
header (disjoint ownership; null hooks keep host suites unchanged). Hold
target 500 µs (24 frames), inside the polled spread's 10-frame p95 proof
because a hold costs frames only when a pend lands in it at the trough.
Ingress device threshold 1,024 words.

**Open owner decision.** Replay playback from the card: B2 protects the
producer, but a download primitive and a replay refill primitive still
take turns on the card, so a slow card read during a web UI load can
starve the replay source itself. Recommended: 4b's guarantee covers live
input (optical AC-3 and PCM); replay keeps its SD-sized gate-open margin
(`AIUS_TX_GATE_OPEN_REPLAY_TARGET_FRAMES`), and card priority for the
refill becomes later work only if asked for.

**Residual risks.**

- The locked-stretch bound is measured, not proven: a future long
  computation without a yield raises `H_lock`. The watermark, the late
  counter and the device proof catch it; host suites cannot time it.
- The producer stack now nests over window depths; the 2,048-byte floor
  proof covers only the paths the device runs exercise.
- The A/B healthy-boot record (`ab_mark_healthy`, `ab_record_write`)
  programs flash with interrupts off once per boot, possibly during
  playback; this exists today. Record its duration in the device session.
- The 20.9 ms attribution and the second underrun's cause are inferred;
  B2 does not depend on them.
- A bench build with `AIUS_NET_POLL_SINGLE_CONTEXT=0` arms the FNET PIT
  at priority 224, above the producer; the deployed envs set 1
  (`firmware/platformio.ini:304`).
- OTA stays outside: verify (80 ms) and staging may still underrun.
  Under B2 a window around the verify would remove most of that cost
  cheaply; it is not promised.

**CLAUDE.md draft for B2 (superseded by the umbrella's text)**
(`CLAUDE.md` §Device access and
OTA, via `the release-promotion step`): replace the paragraph from "Owner-initiated
card traffic is deliberately NOT gated" through "state its cost first
rather than after." with the text below; the bench sentence that follows
("On the bench there is never audio to interrupt") stays as it is.

> **HTTP and SD-card work never delays the audio producer; OTA is the one
> exception.** The producer runs in its own lowest-priority interrupt and
> the main loop masks it except inside I/O windows and yield points
> (aius-sample-rate-following Phase 4b), so a web UI load, a download, a
> card sync, a status poll or an API request during playback costs no
> dropout and needs no cost statement. `audio_diag.producer_lock_hold_max_us`
> and `producer_pend_latency_max_us` are the evidence;
> `ingress_high_water_words` above about 1,024 means a stretch escaped
> the gate. OTA staging, verify, activation and rehearsal stay outside the
> guarantee (owner, 2026-10-07): activation and rehearsal quiesce audio by
> design and a verify measured one 80 ms stall, so prefer silence, and
> when the owner accepts the interruption, state its cost first rather
> than after.

**Owner rulings 2026-10-07 (after the design).**

- **Replay is out of scope.** Replay exists only for synthetic testing,
  so the guarantee, the acceptance examples and the CLAUDE.md text cover
  live input (optical AC-3 and PCM) only.
- **Hold: discuss B2 before implementation.** The owner asked to discuss
  moving the producer into an interrupt before any slice is dispatched.
  This is 4b's first step (below).

**Investigation 2026-10-07 (owner-requested, read-only).**

- **Requests.** A cold page load is about 8 requests: 5 SD downloads
  (231,937 B raw, 67,869 B as one gzip stream; `dsp-editor.js` alone
  175,913 B), `/api/dsp/config`, the `/sd/www/presets/` listing and a
  favicon 404. The 108 requests in 30 s were polling: the editor polls
  `/api/meters` at a 10 Hz target (about 2.6/s achieved) and
  `/api/status` at 1 Hz; the replays page also lists `/api/replays`, a
  raw card scan, every 5 s. Every response is HTTP/1.0 with
  `Connection: close`, and the listener backlog is 1.
- **The 20.9 ms was the presets listing** (by elimination): one dispatch of
  `SD.open` plus up to 129 `openNextFile`, each directory-sector change a
  card read. Downloads are already spread at one 512 B read per epoch;
  their worst step is the OPEN (one card read latency). Status render
  1.4 ms measured, about 2.1 ms at 16 KiB.
- **Memory.** DTCM and OCRAM have about 1.3 KB and 700 B headroom: no
  room. PSRAM is fitted (16 MB): about 8.35 MB free on pad 2 (flagged
  marginal) or pad 1 if the recorder ring halves to 4 MiB. Flash slot A
  has about 3.44 MB free. The whole www set is about 187 KB gzip plus 145
  KB of fonts, so a cache fits in PSRAM (CRC per chunk, SD fallback) or
  could be embedded in flash (web changes then ship by OTA).
- **HTTP.** `Cache-Control: max-age=86400` on `.js` and `.css` only; no
  gzip, ETag or 304. Bundling saves about 4 requests per cold load and
  70% of its card reads and sends; the request rate is the polling.
- **SdFat 2.1.2.** The firmware uses FIFO mode, whose read latency spins
  with no hook (write-busy waits already call `yield()`). DMA mode yields
  in every transfer and busy wait (only the µs command phase spins); it
  uses the uSDHC's own DMA (no conflict with audio eDMA) but needs dcache
  maintenance on the OCRAM buffers and 4-byte alignment.
- **What caching does not cover.** Every write (DSP Apply, presets, card
  sync, uploads), uncached reads, the replays listing and a remount stay
  single SdFat calls; in FIFO mode their latency cannot be bounded below
  4.7 ms.
- **New option, A-DMA (untested inference).** DMA SDIO mode plus a
  guarded `yield()` override that runs a pending producer pass only
  inside SD adapter windows, plus yield points in the status render: every
  card wait becomes a producer opportunity without an interrupt. Bound is
  the longest non-yielding stretch (command phases, FNET calls, render
  gaps, the 1,060 µs REPLAY checkpoint). Risks: DMA-mode reliability on
  this card, dcache handling, stack nesting over SdFat. Size about 300 to
  600 lines; B2 is medium to high; cache, bundle and lighter polls about
  800 to 1,500 lines and worth doing under any option.

**Owner decisions 2026-10-07 (after the investigation).**

- **PSRAM web cache: approved.** The web app is cached in PSRAM and
  served from RAM; the SD card stays the source (updated by sync).
- **Gzip bundling: approved.** Served gzip'd with cache validation.
- **A UI poll never interrupts audio.** HTTP is best-effort: when serving
  a request would delay the producer, the request waits, is shed or fails;
  a missed status update heals on the next success. Audio never waits for
  a response.
- **Investigate next:** DMA SDIO mode (A-DMA), and whether settings
  writes through the PSRAM cache, or an async flush to SD, remove the
  write paths (DSP Apply, presets, uploads) from the audio's way.

**Write-path investigation 2026-10-07 (read-only).**

- **Write paths during playback.** DSP Apply (`PUT /api/dsp/config`,
  REPLACE `/aius/dsp.json`, up to 36 SdFat calls), public config (about
  13), preset save and import (12 + n/512), preset delete (2), card sync
  (22 files, about 1,612 writes and 290 metadata calls). Each answers
  200 only after the rename commit, and each commit also runs a
  USAGE_REFRESH (a FAT scan, 29 ms seen). Recorder, ledger and fault
  records never touch the SD.
- **FIFO waits.** Card programming busy waits call `yield()`; every read
  wait spins, including the FAT and directory reads inside `rename`,
  `remove`, `close`, `mkdir` and `exists`.
- **Write-behind design.** A PUT completes into a PSRAM overlay (one
  dirty entry per path, CRC per 512 B chunk), the next GET serves the
  overlay, and a flush job replays the REPLACE install from the same
  bytes. Latest write per path wins (slider drags write nothing; Apply
  is the only writer); a DELETE is a tombstone; USAGE_REFRESH coalesced.
  Apply arms at PSRAM accept, not at SD commit. The presets listing comes
  from the overlay (removing the 20.9 ms scan). A volume-ID check stops a
  swapped card being written. `/api/status` gains an `sd_flush` object.
- **When the flush may run.** Margin-gated during playback is not safe
  (one call can spin up to 100 ms). Flushing only with no carrier needs no
  priority mechanism, at the cost of a lost-write window of the whole
  session on a power cut. Flushing during playback needs A-DMA or B2.
- **Load shedding.** Check ring level (`aius_spdif_tx24_level`) or
  ingress backlog at HTTP DISPATCH (`aius_runtime_network_http.cpp`);
  answer an immediate 503 `busy` with Retry-After when the estimated
  cost exceeds the margin. Bounds CPU stretches of known length (status
  render, parse), not card calls. UI: a 503 is a silent skip keeping last
  values; "unreachable" only after several consecutive failures; move
  `status.js` and `replays.js` to self-scheduled polls.
- **Card sync.** PUT bodies stage into PSRAM and become the cache entry
  by pointer swap; staging full answers 503 (the script already retries).
- **PSRAM placement.** About 1.2 MB worst case. OTA staging aliases pad
  1 below the recorder ring's top; pad 2 has 8.35 MB free. Options: P1,
  everything on pad 2 with per-chunk CRC and read-back checks; P2, halve
  the recorder ring to 4 MiB to free pad 1. Either way OTA activation and
  rehearsal drain the dirty queue first or refuse with 409.
- **Existing gap found.** A power cut between REPLACE's two renames
  leaves no target file; there is no boot-time `.bak` recovery.
- **Size.** Write-behind, shedding and UI about 1,000 to 1,500 lines
  plus tests, on top of the cache and bundling.

**DMA investigation 2026-10-07 (read-only; A-DMA feasible with
conditions).**

- **Yields.** In DMA mode every transfer and ready wait yields; only the
  command phase spins (under about 20 µs per op). CMD12 is hardware
  auto-stop. Card init (`SD.begin`, ACMD41 up to 1 s) and error paths do
  not yield: a remount is the remaining long stretch (boot mount 582 ms).
  A failed OPEN of a missing file currently forces a remount; invalidate
  only on `sdErrorCode() != 0`.
- **Mount.** `SD.begin` hardcodes FIFO; mount with `SD.begin` then
  `SD.sdfs.begin(SdioConfig(DMA_SDIO))` (or implement presence and sizes
  on `sdfs`), else the 1 Hz presence probe re-inits every second.
- **yield() callers.** `main` after `loop`, init-only `delay()`, USB
  serial (incl. NativeEthernet debug prints from inside FNET calls, and
  from the PIT ISR when `AIUS_NET_POLL_SINGLE_CONTEXT=0`), the CDC
  adapter, SdFat. Guard: a window flag set only around
  `aius_sd_teensy_execute` and the three raw scans (not replay refill);
  run one pass only if the window is set, not already in the producer,
  IPSR 0, PRIMASK 0, IRQ_SOFTWARE enabled, audio ready, not recovery-safe,
  and a commit is pending. C linkage override of the weak `yield`.
- **Data integrity.** SdFat's caches sit in DTCM (uncached): safe. User
  buffers in OCRAM need dcache flush before WRITE and flush-delete around
  READ, 32 B aligned whole lines; `g_replay_transfer` needs `alignas(32)`;
  the OTA copy into PSRAM must stay FIFO or bounce. No extra DTCM stack
  over FIFO's existing bounce.
- **Prebuild findings.** B1 and S3 apply (same fixes); B2 partly (hook
  returns in recovery-safe; keep the "producer active" flag; the
  post-commit hazard is gone because the window is closed at `main`'s
  yield); S5 applies (producer `enqueue` about 4.37 KB nested over the SD
  chain, about 0.8 to 1 KB over today against 1,408 B headroom; needs a
  `-fstack-usage` bound); S7 largely not (commits stay RX-asynchronous);
  S1 not (no NVIC); S4, S6, S8 apply.
- **Performance.** Single-sector CMD17/CMD24: downloads and sync slower
  but yielding; moot for cached web files.
- **Device check.** One OTA of an image with DMA runtime-armed by an
  authed POST (FIFO at boot keeps self-commit and OTA rollback safe);
  counters `sd_mode`, `sd_window_nonyield_max_us`, yields per op,
  producer passes in windows, pend latency, stack free, remounts, fault
  deltas. Run 1 source off: content check of every `/sd/www` file, one
  sync, listings. Run 2 AC-3 playing: page loads and a sync, underrun 0.
- **Bound.** About 1.1 to 2 ms steady state (REPLAY 1,060 µs, status
  render unless sliced, unmeasured dispatch parses) against B2's 0.62 ms;
  inside the 4.7 ms margin. Residual: remount and error-path waits; DMA
  reliability on this card; stack nesting. Vendoring SdFat would let
  yields reach ACMD41 and `waitTimeout`, closing most of the remount gap.

**Owner decisions 2026-10-07 (direction; supersede B2).**

- **Mechanism: A-DMA with a vendored SdFat.** DMA SDIO mode and a
  guarded `yield()` hook that runs a pending producer pass inside SD
  windows; SdFat vendored (as NativeEthernet already is) so card init,
  `waitTimeout` and error paths yield and cache maintenance sits in the
  driver. B2 is the fallback only if the DMA device check fails.
- **Write-behind, flushing within seconds during playback.** A power cut
  may lose the last few seconds of changes.
- **PSRAM placement: pad 2 with a CRC per 512 B chunk,** SD fallback on
  a mismatch; the recorder ring is unchanged.
- **Apply is heard at PSRAM accept.**
- **Pending writes are held on a card swap,** never written to another
  card, until the original card returns or the owner discards them.
- **Fix the rename gap:** boot-time recovery of the `.bak` left by a power
  cut between REPLACE's two renames. (Mechanism overturned by the owner
  later the same day: settings saves are copy-before-write with a mount
  restore, 4h H4 and H10.)
- **Lighter polling:** the editor uses a compact status, meters at 4 to 5
  Hz, every page self-schedules its polls.

The B2 design above is kept as the fallback record; the work is
re-planned as phases 4e onward.

**Prebuild review round 1 (2026-10-07, `reviewer` `opus`): NOT READY.**
Each finding is plan work for the design revision after the discussion.

- **B1, pend-latency metric.** It counts the producer's own run time (a
  pend raised during a pass waits for the pass), so the device proof
  fails with a perfect gate. Stamp only while the producer is idle; host
  case.
- **B2, recovery-safe.** Recovery-safe epochs never run AUDIO today; a
  pended producer would run there, and a hang after the EPOCH_COMPLETE
  ledger commit scores as a clean reset. The producer returns early in
  recovery-safe, no yield after the commit, and a retained "producer
  active" flag attributes a wedge to AUDIO. Changes no owner-visible
  behaviour if done this way.
- **S1.** Unmask and pend need DSB and ISB, or yields and AUDIO windows
  can be no-ops; device counter of passes per AUDIO window.
- **S2.** The REPLAY checkpoint already holds 1,060 µs locked, over the
  500 µs target; time and yield per stage, and feed measured stretches
  into the schedule model.
- **S3.** The replay refill window runs the producer mid-REPLAY after the
  source promotion; give refill a non-windowed execute (refill only runs
  without a carrier).
- **S4.** The 1,024-word ingress alarm has no margin; derive it from the
  pass length plus the start-delay bound (about 1,152).
- **S5.** Stack headroom (1,408 B over the floor) is unproven at the deep
  window sites; a static `-fstack-usage` bound and those routes in the
  device run.
- **S6.** The isolation check cannot follow indirect calls (`enqueue`
  through the sink pointer); explicit roots, fail on unresolved indirect
  branches.
- **S7.** Fill-sensor sampling becomes RX-locked, so the "sample between
  a DMA crossing and its ISR" artefact arrives in runs; plant case with
  swept phase and a device comparison against the Phase 4 baseline.
- **S8.** Device run adds `sd_diag` driver and protocol fault deltas 0
  and a content check of downloaded and synced files.
- **S9.** Scope the CLAUDE.md text to live input (settled by the replay
  ruling).
- **Nits.** `note_pend` always-inline reading the cycle counter in ITCM,
  gate library FLASHMEM, an owner for `test/itcm-placement`; atomic
  `take_pend_us`; `g_http_service_overruns` and `cw=` join the
  not-a-fault list; no windows inside IRQ_SOFTWARE-masked sections; the
  gate latches parked after `ota_quiesce`; PROGMEM `audio_diag` format
  strings; reconcile the `/api/replays` `sd_busy` table entry with the
  code.

**Prebuild review round 2 (2026-10-07, `reviewer` `opus`): NOT READY.**

- **Verdict.** Overall HOLD. Phase 4b inherits the phase verdicts: 4e NOT
  READY (4 blockers), 4f NOT READY (1), 4g NOT READY (no blocker, S10 to
  S13 contract corrections), 4h NOT READY (1 blocker plus wrong-fact
  proofs). Blockers B1 to B6 are listed under their phases; the contract
  revision is the next session's work.
- **B5 consequence.** The polled-vs-unpolled proof (the 4b proof at
  `:2058` and 4f's at `:2833`) becomes vacuous under 4f as written: most
  1 Hz full-status polls would answer 503, so the spreads compare almost
  nothing. Once the 4f admission cost is fixed, the proof must assert at
  least N% of the 1 Hz polls answer 200 and
  `firmware/tools/rate_gate_capture/capture.py:726-745` (retries 503 three
  times, then raises) needs 503 handling, as a 4f seam.

**Round 2 resolutions (contract revision 2026-10-07; round 3 to confirm).**

- B1 to B4: resolved in 4e's contract (see 4e §Round 2 resolutions).
- B5 resolved in 4f (F1, F5, F6); B5 consequence resolved here: the
  polled-vs-unpolled proof requires at least 95% of polls answered 200
  and capture.py skips under 5%, and the unpolled segment is checked for
  browser polling.
- B6 resolved in 4h (H7).
- S1 to S17: resolved in the phase that files them (4e S1 to S9, 4f S10
  and S11, 4g S12 and S13, 4h S14 to S17; S2, S8 and S11 in every phase
  they name). 4b's own proofs adopt S5 (clear at start, `sd_mode` at end),
  B4's fault split (`busy_timeouts` counted, `open_misses` not) and S14's
  route names in the content check.

**Prebuild review round 3 (2026-10-07, `reviewer` `opus`): NOT READY**
(depends on 4e to 4h; the fault-delta proof hit by R3-B1). Each item is
followed by its resolution; the rest are listed under their phases.

- **R3-B1 (4b part).** A card pull raises `driver_faults` once per probe,
  so 4b's fault deltas fail if 4h's card-swap step shares the session.
  Resolved: fault deltas taken outside every card-out interval, and
  `absent_mounts` named as not a fault (4e E10).
- **R3-S6 (4b part).** The DSP Apply parse (up to 12,288 B, no yields,
  unmeasured) may break `producer_pend_latency_max_us` at most 2,000 with
  three Applies. Resolved: 4f F7 measures it; the 4b latency and ingress
  lines take D from that measurement when it exceeds 2,000 us.

**Prebuild review round 4 (2026-10-07, `reviewer` `opus`, closure
review): NOT READY** only because it depends on 4e to 4h; its R3-B1 and
R3-S6 edits are confirmed closed and it carries no round 4 item of its
own. The round 4 items are listed and resolved under 4e (R4-B2, R4-S7),
4f (R4-S1, R4-S2, R4-S8), 4g (R3-N1) and 4h (R4-B1, R4-S3 to S6, R3-N2
to N4). One bears on 4b's session: 4h's 5 s settings bound holds in DMA
mode only (R4-S2), which 4b's device proof already asserts (`sd_mode`
DMA at the end).

**Clean-room review (2026-10-07, `reviewer` `opus`): NOT READY** only
via 4f's CR-B1; no blocker of its own. Each item is followed by its
resolution.

- **CR-B1 (4b part, from 4f).** With no source the empty ring read as a
  short margin, so source-off parts of the session (the card-swap step,
  a reboot read-back) would see every sheddable route 503. Resolved in
  4f (F11); here a source-off proof line asserts first-attempt 200s with
  `http_shed` and `sd_fifo_defers` delta 0, and the CLAUDE.md text says
  shedding applies during live input only.
- **CR-S7.** The CLAUDE.md text hard-coded "about 1,216" though 4f F7
  may raise D, and "every SD-card wait runs pending audio production"
  lacked the DMA-mode qualifier (FIFO stays the field fallback, its data
  waits spin, and a started HTTP job is never deferred). Resolved: an
  `{ALARM}` placeholder filled at promote from the session's derived
  alarm, the proof line names it, and the text states DMA and FIFO
  behaviour separately.
- **CR-S8.** The content check compared a decoded bundle with "the sync
  source", but a decoded bundle is the inlined page. Resolved: bundles
  compare against `www_bundle.py`'s output for the synced sources, as
  4g run 1 does.

**Clean-room round 3 (2026-10-07): HOLD** (CR3-B4; CR3-S1, CR3-S4; 4b
also depends on 4e to 4h, which carried CR3-B1 to B3). Each item is
followed by its resolution.

- **CR3-B1 to B3 (4b part).** Resolved in 4e (§Walk specification);
  4b's proofs are device proofs and do not change.
- **CR3-B4 (blocker).** The source-off line required every request 200
  at its first attempt, but a cold page load's `/favicon.ico` answers
  404 (no page links an icon, no route serves one; the unknown-route
  404 touches no card). Resolved with the proof text, as in 4f: no 503,
  and every request its expected status, the favicon 404 excepted
  (Phase 5 F14).
- **CR3-S1.** The outcome and the CLAUDE.md text claim PCM, but every
  device run in 4e to 4h and here played AC-3, and PCM has the smallest
  margin. Resolved: a PCM segment through the U24 bench loop (1 Hz full
  status at least 95% 200, three Applies 200, underrun delta 0). Like
  every device step it is an owner session.
- **CR3-S4.** "Every plain /sd/www file equal to the sync source" swept
  in owner presets, which have no sync source. Resolved: the check
  covers the plain files `aius-sd-sync.sh` uploads from
  `firmware/sd/www/`; presets compare with their PUT bodies and bundles
  with `www_bundle.py`'s output, as the line already says.

**Clean-room round 3 second closure (2026-10-07): READY.** No 4b
item. No further clean-room round: the closing reviewer was itself
clean-room, and implementation carries a delivery audit per phase.

- **CR5-S1 to S4 (4b part, from 4e and 4h).** Resolved in 4e and 4h;
  4b's proofs are device proofs and do not change.

### Phase 4e Every SD-card wait runs the live-input producer (vendored SdFat, DMA SDIO)

```yaml
phase: 4e
title: Every SD-card wait and checkpoint boundary runs pending live-input production
status: built # 2026-10-07: host proof green (22 suites), all four images build with every post-link guard, delivery audit FAIL (D-B1 boot fault) then closure PASS; device runs 1 and 2 and the E7 gate are the owner's (no OTA until the owner's go-ahead)
depends-on: phase-4 (device session done 2026-10-07)
prebuild-review: ready # 2026-10-07: rounds 2 to 5, clean-room, clean-room closure, clean-room round 3 and two closures; second closure READY; CR5-S1 to S4 folded; no further clean-room: the closing reviewer was itself clean-room and implementation carries a delivery audit per phase
outcome: >-
  During live input the producer waits behind no card latency: every
  SD-card wait between card commands inside an SD job primitive, a mount
  or remount, or a raw directory scan, and every epoch checkpoint
  boundary, runs a pending production pass, so the longest producer wait
  (D) is the longest non-yielding stretch (one card command, one
  checkpoint body, the status render until 4f), watermarked and held at
  2,500 us (2,000 us once 4f slices the render) whatever the card does.
  No pass runs inside a card command or a FIFO data loop; the controller
  DMAs only into DTCM (every other buffer bounces per sector,
  multi-sector paths included); a missing file neither remounts the card
  nor counts as a driver fault, nor does a mount attempt with no card
  inserted; and a wedge inside a nested pass reads as AUDIO on the next
  boot. Image 1 boots FIFO with the hook only at upstream `yield()` sites
  and the ACMD41 loop (all between commands) and DMA armed at runtime,
  so DMA meets OTA self-commit only on image 2, after both runs pass.
unit: component-behind-interface
seams:
  - "firmware/vendor/SdFat (new snapshot of framework SdFat 2.1.2): SdioTeensy.cpp: weak aius_sdio_wait_hook called once per yieldTimeout iteration and once per ACMD41 iteration in SdioCard::begin, never in waitTimeout, cardCommand, readData or writeData; readSector, writeSector, readSectors and writeSectors bounce every non-DTCM buffer per sector through a 512 B stack buffer, and rdWrSectors refuses a non-DTCM buffer (SD_CARD_ERROR_DMA); a busy-timeout counter (BUSY_TIMEOUT_MICROS expiries); SdioCard::clearError() (the file-static m_errorCode, SdioTeensy.cpp:202, is otherwise cleared only in begin, :647); FAT volume-serial accessor (used by 4h): FatPartition keeps no volume serial (its members, FatPartition.h:191-202), so the patch stores the boot sector's volumeSerialNumber (FsStructs.h:172 for FAT12 and FAT16, :202 for FAT32) in a new 4 B FatPartition member when init() reads the boot sector at mount, zero on a non-FAT volume, and the accessor returns the stored value with no card read (CR5-S4)"
  - "firmware/vendor/SD (new snapshot of the framework SD wrapper): SDClass::begin(csPin, dma) mounts in either mode in one call; restart() keeps the stored mode (as upstream already does, SdFat.h:104-112); cardDetected() reads DAT3, which a failed begin already leaves INPUT_PULLDOWN (SD.cpp:95-98, the test upstream mediaPresent uses, :157-159)"
  - "firmware/lib/aius_producer_yield (new): aius_producer_yield_wait (the hook body), aius_producer_yield_point, aius_producer_yield_note_pend (static inline in the header, read from ITCM update()), the nested-pass ledger wrapper (E5), get_diag and clear_diag"
  - "firmware/src/aius_sd_teensy.cpp: aius_sd_teensy_execute BEGIN (mode-aware, still one primitive per case; a failed mount with cardDetected() false counts absent_mounts, not driver_faults), clearError() before every card primitive, driver_fault split (sdErrorCode() == 0 counts open_misses and keeps the mount; only sdErrorCode() != 0 counts driver_faults and invalidates), new aius_sd_teensy_set_mode"
  - "firmware/src/aius_runtime_storage.cpp: the SD window flag around the storage execute call (aius_runtime_storage.cpp:335, which OTA staging chunks also pass through)"
  - "firmware/src/optical_main.cpp: platform_checkpoint (yield point at every boundary except EPOCH_COMPLETE), app_read_directory, http_replay_list_json and app_replay_delete (windows), the replay refill execute (optical_main.cpp:8294, no window), AiusAudioProcessor::update (pend stamp), the strong aius_sdio_wait_hook (an object file, not an LDF archive, so it beats the weak one), the mode route callback, POST /api/clear resetting the new watermarks, audio_diag and sd_diag keys; no new retained object (E5) and no alignas on g_replay_transfer (E9)"
  - "firmware/test/memory/verify_target.py: the single COMMITTED_OCRAM_FREE_BYTES (stale 131,040, the 0x1FFE0 link floor, aius_teensy41.ld:271) becomes a floor table keyed by PIOENV with one entry per env in OPTICAL_IMAGE_ENVS (verify_target.py:265-273; verify_target passes $PIOENV, the CLI gains --env). Each entry is the OCRAM free measured on that env's link of the unchanged tree at 4e start, before any 4e edit, and recorded in §Memory: the A/B envs carry the 32 B .aius_retained_ab ledger (optical_main.cpp:4521-4537, under #if AIUS_AB_SLOT) in the content-sized .bss.aius_retained (aius_teensy41.ld:151-174), so the plain env's 131,776 B is not their figure. An env below its own floor, or an env with no entry, fails; a floor only ever rises to a new measurement (lowering one needs funding, the file's own rule), so any OCRAM growth in any env fails the gate (R4-B2). In the same edit parent-wiring (which owns firmware/test/memory/**, CR-S12) rewrites the stale comment at verify_target.py:54-57 (\"OCRAM margin is now ZERO\") and the history block through :63, stating the per-env floors and the 0x1FFE0 link floor (R4-N1)"
  - "firmware/lib/aius_http route POST /api/sd/mode, firmware/src/aius_optical_app.cpp dispatch, firmware/sd/www/openapi.json"
  - "firmware/platformio.ini teensy41-optical: lib_deps symlinks to the two snapshots, lib_ignore of the framework SD and SdFat, -fstack-usage, a link map (-Wl,-Map to firmware.map in the build directory; the walk's library and object membership and the link half's provenance, W1, CR3-N1), -Wl,--emit-relocs (relocation sections kept in the ELF, not loaded, so W1 reads address-taken functions from R_ARM_ABS32 relocations applying to SHF_ALLOC sections, CR4-S1, CR5-S3), -MMD (a dependency file beside each object, none written today, for the link half's include check, CR4-B3), -DAIUS_DTS_LOG_STDERR=0 and -DAIUS_AVSHIM_LOG=0 (both inherited by slot-a, slot-b and fault, which extend teensy41-optical's build_flags, platformio.ini:374-393)"
  - "firmware/lib/aius_dts/src/aius_dts.c dca_format_log: its stderr body (aius_dts.c:57-59) compiled only when AIUS_DTS_LOG_STDERR is nonzero, default 1, so the native dts suite and host tools keep the log and the optical images carry an empty sink (E14, CR-B3)"
  - "firmware/vendor/ffmpeg-eac3/shim/aius_avshim.c av_log and avpriv_request_sample: their bodies (the g_log_fn test, the vsnprintf into a 256 B stack buffer and the g_log_fn call, aius_avshim.c:109-133) compiled only when AIUS_AVSHIM_LOG is nonzero, default 1, so the native eac3 suites keep them and the optical images carry two empty functions with no vsnprintf edge and no indirect call; aius_avshim_set_log keeps its definition (no caller in firmware/) (E14, CR2-B1)"
  - "firmware/lib/aius_producer_yield: the window stretch clock behind sd_window_nonyield_max_us (window open and close, hook entry and return stamps; pass time excluded, CR-S1)"
targeted-proof:
  - "host: sdfat-vendor (new): UPSTREAM_SHA256 and PATCHED_SHA256 source contract in the manner of firmware/test/network-transport/test_vendor_source.py; the hook is called only inside the yieldTimeout loop and the ACMD41 loop; no hook call in waitTimeout, cardCommand, readData or writeData, so none between the SDHC_XFERTYP write and the CC observation; readSectors and writeSectors with a non-DTCM buffer take the per-sector bounce, and no rdWrSectors or cardCMD6 call passes a buffer the DTCM predicate has not passed; the volume serial is stored in FatPartition::init from the boot sector it already reads, and the accessor reads only that member (CR5-S4)"
  - "host: sd-target: one primitive per case with the mode-aware BEGIN (test_source_guard.py updated); a not-found OPEN (sdErrorCode 0) keeps the mount and counts open_misses, not driver_faults; a nonzero sdErrorCode counts driver_faults and invalidates; a failed BEGIN with cardDetected() false counts absent_mounts and leaves driver_faults unchanged, with it true counts driver_faults; a stale nonzero error code left by an earlier primitive does not turn a later not-found OPEN into a driver fault (clearError() before each primitive); a mode change invalidates the mount only with no file open"
  - "host: runtime-target (test.sh:1203-1255, which compiles aius_sd_teensy.cpp, aius_runtime_storage.cpp, aius_runtime_network_http.cpp and aius_optical_app.cpp against test/runtime-target/fake/SD.h): the fake gains begin(cs, dma), clearError, cardDetected and the new counters, and the suite stays green"
  - "host: fake-fidelity: both fake SD.h files are compared against firmware/vendor/SD/src/SD.h (the linked copy) instead of the framework libraries/SD/src/SD.h that lib_ignore drops (test_fake_fidelity.py:152, :157)"
  - "host: producer-yield (new): guard truth table over window or point, in-producer, IPSR, PRIMASK, IRQ_SOFTWARE enabled, audio ready, recovery-safe and work pending; the pend stamped only while the producer is idle (round 1 B1) and re-stamped at pass exit when ingress is non-empty (S3); every nested pass records AUDIO and commits the ledger before and restores the saved checkpoint after, on every path (E5); late-pass and latency watermarks against a fake cycle clock; on the same clock a window of 0.2 ms to a hook, a 7.6 ms nested pass, 0.3 ms to a second hook with no pass, and 0.1 ms to the close records sd_window_nonyield_max_us 0.3 ms, never a figure that includes the pass (CR-S1); clear_diag resets them"
  - "host: producer-isolation (new): the post-link walk of §Walk specification (W1 to W8) over teensy41-optical and teensy41-optical-slot-a: graph from direct branches with veneers resolved to their literal target (W1), roots from the producer, yield-root and computed SD-root tables (W2), the W3 leaf table with no edge allowlist, register-indirect sites from the W4 site table. It asserts W5: (1) producer roots (aius_audio_service_run, AiusAudioProcessor::consume_block) reach no SdFat, SD, FS.h File, FNET, NativeEthernet, usb_serial, Serial, yield, aius_sdio_wait_hook or aius_producer_yield symbol and no I/O stdio entry (fputs, fputc, fwrite, puts, printf, fprintf, vfprintf, fiprintf, _write and their _r forms); dca_format_log, av_log and avpriv_request_sample are in the image with empty bodies (E14, CR-B3, CR2-B1); (2) the forward closure of every allocator-family, I/O stdio, abort and __assert_func symbol in the image reaches neither aius_sdio_wait_hook nor aius_producer_yield_point (no hook under an allocator or stdio function), its indirect sites exactly the pinned libc row (CR4-B1); (3) every hook-reaching SD caller is a row of the windowed or the accepted unwindowed SD-root table, and every direct caller of aius_producer_yield_point is a yield-root row (4e: platform_checkpoint; 4f, 4g and 4h add theirs); (4) every register-indirect site in a visited function is a W4 row at its recorded count, every vtable symbol is classified, and every cycle is a W6 row; anything unlisted fails. The window flag's placement is optical-cutover's source guard, not this suite's (CR3-B1)"
  - "host: producer-isolation, link half (S7, CR3-N1): the optical ELF carries no .debug_info (only .debug_frame and .debug_line_str at 15edf92), so provenance comes from the link map for out-of-line definitions: every input section defining an SdioCard, DedicatedSpiCard, FatFile, FsBaseFile or SDClass symbol comes from an archive member whose .d names its source under firmware/vendor/SdFat/src or firmware/vendor/SD/src and no framework SD or SdFat path (PlatformIO names the archives libSdFat.a and libSD.a, as a framework build would, so the archive name proves nothing; corrected at the 4e link), none from the framework's SdFat or SD. Header-inline classes (SDFile, defined in SD.h and constructed by the inline SD.open; FsFile, a header-only template, FsLib/FsFile.h) are emitted as weak COMDATs by whichever src/ object includes them, so for them no object's .d (-MMD) names the framework's libraries/SD or libraries/SdFat, and each object that defines an SDFile or FsFile COMDAT (per the link map) names the vendored SD.h and FsFile.h (firmware/vendor/SD/src/SD.h, firmware/vendor/SdFat/src/FsLib/FsFile.h) (CR4-B3, CR5-S1); and aius_sdio_wait_hook resolves to the strong definition in optical_main.cpp's object, whose body calls aius_producer_yield_wait"
  - "host: stack-bound (new): call depth over the same resolved graph (W1 to W6). Frames from -fstack-usage; a symbol without a .su (libc, core) from a declared frame table, an unlisted one failing; a veneer adds no frame; a tail call is charged as a call (an overcount by the caller's frame); an indirect site is charged its deepest W4 target, FS.h File::read and File::openNextFile (cores/teensy4/FS.h:167, :216) resolving to SDFile's vtable entries and the SdFat block-device virtuals to SdioCard's and DedicatedSpiCard's (CR3-S2), each site charged from its own W4 row only; a W6 recursion is charged its bound times the frame sum of its declared guard-admitted chain, plus the deepest exit from the component (CR4-S2); the 512 B bounce frame counted under the hook. The deepest of these paths: loop, storage execute, SD wait, hook, pass; HTTP dispatch, app_read_directory, SD.open or File::openNextFile, wait, hook, pass; and the two unwindowed roots (replay_job_advance, ota_copy_staged_to_psram), charged to the hook without a pass, since their hook call never runs one. Plus the exception chain stacked on the same main stack (per NVIC preemption level among the enabled handlers, the ITCM update() audio ISR, DMA, SDHC, ENET and USB included, the deepest handler path from the same walk plus a 104 B FPU extended frame). Handlers come from a declared table of handler and priority. Rows installed by aius code are checked against every attachInterruptVector and NVIC_SET_PRIORITY call site under firmware/src, firmware/lib and firmware/vendor (at the 4e link: firmware/src/aius_network_teensy.cpp:292, firmware/src/optical_main.cpp:1297-1298, and the vendored SDHC install at firmware/vendor/SdFat/src/SdCard/SdioTeensy.cpp:477-478); an unlisted site there fails. Rows installed inside the Teensy core and framework libraries (DMA channels, USB, ENET, the audio library's software interrupt) cite their install line in the pinned framework source, and the suite checks that line still exists; this scan does not prove a framework handler absent from the table, which the device floor (stack_free_min_bytes) backs (CR3-S5). The total plus 2,048 B fits the DTCM stack zone (_estack minus _ebss) of the canonical teensy41-optical-slot-a image, less the 32 B MPU trap at _ebss (25,632 B usable on slot-a after 4e's 32 B of statics); a dynamic or unbounded frame on any path fails"
  - "host: producer-isolation and stack-bound walk fixtures (W8): three fixture ELFs the suite links from small C sources with the PlatformIO arm-none-eabi toolchain and a two-region link script (flash at 0x60000000, ITCM at 0, so the linker emits veneers), no committed binary. Veneer: a flash function calls an ITCM function through a __X_veneer and the ITCM function calls a stub named yield; isolation fails naming the edge and stack-bound counts the ITCM frame (a walk that stops at the veneer passes it, so this case fails such a walk). Indirect: a function-pointer call with no W4 row fails, the same with a row passes and is charged its deepest target, and an in-function ldr pc jump table is not a site. Allocator: a root calling __wrap_calloc and __wrap_realloc passes (leaves), a chain in which __wrap_malloc reaches a stub named aius_sdio_wait_hook fails W5 (2), and a two-function cycle with no W6 row fails. File-shaped (CR4-B1): a function reachable only through a vtable entry, which reaches the hook stub and whose only caller is an indirect call in a stub named _write, passes when that site is the pinned libc row (the correct-build shape the reverse closure failed), and the same image with a second, unpinned indirect site in _write fails. Relocations (CR4-S1, CR5-S3): a data word equal to an ITCM function's address with no relocation against it does not make that function address-taken, nor does a .debug_frame FDE's R_ARM_ABS32 relocation against a function (the fixture built with -g)"
  - "host: optical-cutover (yield-point and window placement: the SD window flag set only in the source functions of the W2 windowed rows, never in replay_job_route or ota_copy_staged_to_psram, CR3-B1; no yield point at EPOCH_COMPLETE; every function in firmware/src and firmware/lib outside aius_sd_teensy.cpp whose code, comments excluded, names SD., File or FsFile is the source of a W2 row, four at 15edf92: ota_copy_staged_to_psram, http_replay_list_json, app_replay_delete, app_read_directory, W5; no source under firmware/src or firmware/lib, comments excluded, names Print, Stream, EventResponder, vdprintf, dprintf, fopen, fdopen, funopen or fopencookie (the W5 (2) libc row's guard, CR4-B1); the ledger wrapper around every nested pass), status-body-bound (new keys), boot and boot-target (no retained-region or ledger-layout change), memory (the per-env floor table: a synthetic snapshot 1 B under any env's floor fails, an env missing from the table fails, and every env's link of the 4e tree is at or above its 4e-start floor, so OCRAM free is unchanged in each; on the canonical teensy41-optical-slot-a image, ITCM growth at most 1,280 B over its 4a figure of 4,340 B free, DTCM statics growth at most 256 B), itcm-placement, fake-fidelity, runtime, runtime-target, audio-service, audio-ingress, http, optical-app, registry, sd-editor (openapi), dts and eac3 (the native builds keep each log body at its default 1, E14) green; both images build"
  - "device run 1 (owner session, source off, DMA armed, POST /api/clear after arming): content check of every /sd/www file and /aius/dsp.json against the sync source; one full aius-sd-sync.sh; the /sd/www/presets/ and /sd/www/ listings; GET /sd/www/missing.js answers 404 with open_misses +1 and driver_faults and mounts unchanged; one card pull and reinsert (a DMA-mode remount, hook_calls advancing across it; sd_diag read just before the pull and just after the first successful remount, absent_mounts advancing about once per second while the card is out, and the interval's other counters recorded, not judged); driver_faults, protocol_faults and busy_timeouts delta 0 over the run outside that card-out interval, sd_mode DMA and sd_window_nonyield_max_us recorded; the source-off epoch rate recorded from /api/status loop_epoch read at the run's start and end against wall time (R4-N2); last, one OTA stage of the image 1 container and POST /api/ota/rehearse, whose copy into PSRAM and verify run in DMA mode (ota_dark rehearsal verdict COMPLETED; the reset reverts to FIFO)"
  - "device run 2 (owner, AC-3 playing, DMA armed, POST /api/clear after arming): ten web UI loads at least 5 s apart, one full sync and the editor open (full status at 1 Hz) give underrun_episode_count delta 0, producer_pend_latency_max_us at most 2,500, ingress_high_water_words at most 1,264, stack_free_min_bytes at least 2,048, producer_wait_passes above 0, the run 1 fault deltas 0, sd_mode still DMA at the end, and the playback epoch rate recorded from loop_epoch deltas over the polled segment against wall time (R4-N2); then an unpolled segment of 10 min or more (no browser open, http_requests checked) whose sub-window spread p95 is within 10 frames of Phase 4's unpolled baseline (10 frames, 2026-10-07), with clock.fill_trims recorded against the Phase 4 rate"
  - "decision gate (E7): B2 (Phase 4b, Fallback design) only on a content mismatch, driver_faults above 0 outside a card-out interval (absent_mounts is never a fault; a driver fault inside the interval, such as a contact bounce at insertion, is diagnosed first), busy_timeouts above 0 in DMA mode (the DMA hang: a 1 s BUSY_TIMEOUT_MICROS expiry), or a watchdog reset during a DMA run whose pv= is the SD or HTTP checkpoint. Stays on A-DMA and is fixed in 4e: pv= AUDIO (a guard or nested-pass defect), stack_free_min_bytes under 2,048, a timing miss with integrity intact (attributed by the counters to the stretch 4f or a new yield point closes). Any other reset is diagnosed before a verdict"
acceptance-examples:
  - a 100 ms card read inside a download spans about 37 ingress commits, each produced by a pass at a yieldTimeout iteration (the DMA-complete wait) rather than queued behind it
  - a card pulled and reinserted with the source off counts one absent_mounts per 1 Hz probe while out (each probe starts with a mount attempt, optical_main.cpp:8021-8058, and SD.begin with no card fails with a nonzero code), then remounts in DMA mode through an ACMD41 loop that calls the hook each iteration (hook_calls advances; with no ingress pending no pass runs); during live input the presence probe is deferred (optical_main.cpp:8162-8164), so a playback remount happens only at a job BEGIN after a nonzero sdErrorCode
  - GET /sd/www/missing.js answers 404, counts open_misses and leaves sd_diag driver_faults and mounts unchanged
contract-artifact: spec-only
fan-out: # sdfat-vendor, sd-adapter, producer-yield and link-checks concurrently; parent-wiring once the two new headers land; only parent-wiring builds images
  - name: sdfat-vendor
    owned-paths:
      - firmware/vendor/SdFat/**
      - firmware/vendor/SD/**
      - firmware/test/sdfat-vendor/**
  - name: sd-adapter
    owned-paths:
      - firmware/src/aius_sd_teensy.cpp
      - firmware/src/aius_sd_teensy.h
      - firmware/test/sd-target/**
      - firmware/test/fake-fidelity/**
      - firmware/test/runtime-target/**
    handoff: firmware/test/runtime-target/** passes to parent-wiring once sd-adapter reports (R4-S7); sd-adapter adds the adapter API to the fake (begin(cs, dma), clearError, cardDetected, the counters), parent-wiring then adds what its storage window and mode route need and owns the suite's final green
  - name: producer-yield
    owned-paths:
      - firmware/lib/aius_producer_yield/**
      - firmware/test/producer-yield/**
  - name: link-checks
    owned-paths:
      - firmware/test/producer-isolation/**
      - firmware/test/stack-bound/**
  - name: parent-wiring # coordinator: hook body, windows, yield points, pend stamp, mode route, status keys, the new suites in test.sh and README
    owned-paths:
      - firmware/src/optical_main.cpp
      - firmware/src/aius_runtime_storage.cpp
      - firmware/src/aius_runtime_storage.h
      - firmware/src/aius_optical_app.cpp
      - firmware/src/aius_optical_app.h
      - firmware/lib/aius_http/**
      - firmware/sd/www/openapi.json
      - test/sd-editor/openapi.test.js
      - firmware/test/http/**
      - firmware/test/optical-app/**
      - firmware/test/optical-cutover/**
      - firmware/test/status-body-bound/**
      - firmware/test/itcm-placement/**
      - firmware/test/memory/**
      - firmware/test/registry/**
      - firmware/scripts/test.sh
      - firmware/platformio.ini
      - firmware/lib/aius_dts/src/aius_dts.c # E14: the log sink's compile switch only
      - firmware/vendor/ffmpeg-eac3/shim/aius_avshim.c # E14, CR2-B1: the two log bodies' compile switch only
      - firmware/README.md
review-intensity: active
commit-checkpoint:
  green: [sdfat-vendor, sd-target, runtime-target, producer-yield, producer-isolation, stack-bound, optical-cutover, status-body-bound, boot, boot-target, memory, itcm-placement, fake-fidelity, runtime, audio-service, audio-ingress, http, optical-app, registry, sd-editor, dts, eac3]
  buildable: [teensy41-optical, teensy41-optical-slot-a]
  parent-commit: "feat(aius): SD-card waits run the audio producer (vendored SdFat, DMA SDIO) (Phase 4e)"
  coordination: owned-path sets are disjoint at any moment; runtime-target moves from sd-adapter to parent-wiring at sd-adapter's report and is never edited by both at once (R4-S7); host suites build one at a time
```

**Owner confirmation (2026-10-07).** Confirmed: E1 and E3 to E17.
Deferred: E2 (owner "can't judge"; the DTCM bounce stands as
recommended). The owner noted that E4 to E14 packed too much into each
line to weigh the risk.

**Owner ruling 2026-10-07: every device session waits for the owner,**
including OTAs and the source-off checks (4e run 1, 4g run 1).

Implements the owner's A-DMA direction and the prebuild findings that
carry over (B1, B2 in part, S3, S4, S5, S6, S8; S1 is moot without an
NVIC vector, S2 is inside D, S7 is a device comparison in run 2). The
producer stays in thread mode: it runs where it runs today (the two
AUDIO visits) and also at the points below, so outside those points the
main loop and the producer stay mutually exclusive exactly as today.

- **Hook.** The vendored driver calls a weak `aius_sdio_wait_hook()` at
  the sites below; the firmware's strong definition (in `optical_main.cpp`,
  an object file, because a strong definition inside an LDF archive loses
  silently to SdFat's weak one) calls `aius_producer_yield_wait()`, which
  runs one pass of `aius_audio_service_run` (the AUDIO budget, a DTCM
  in-producer flag set around it, the E5 ledger wrapper) only when the SD
  window flag is set, the producer is not already running, IPSR and
  PRIMASK are 0, IRQ_SOFTWARE is enabled, audio is ready, the epoch is
  not recovery-safe, and ingress work is pending. Explicit yield points
  call `aius_producer_yield_point()`, the same guard without the window
  flag.
- **Where the hook fires (E6, B1, S1).** Only between card commands: once
  per `yieldTimeout` iteration (upstream's `yield()` sites: the CMD13
  ready waits before every data command, `waitDmaStatus`, and the
  `isBusyDat` waits after CMD12 in `transferStop` and `stopTransmission`)
  and once per ACMD41 iteration in `SdioCard::begin`. Never in
  `waitTimeout` and so never inside `cardCommand`: `rdWrSectors` and
  `cardCMD6` arm `SDHC_IRQSIGEN` (TCIEN included,
  `SdioTeensy.cpp:58-63`) before `cardCommand`, so a pass between the
  `SDHC_XFERTYP` write and the CC poll could let the transfer complete
  and `sdIrs` (`:282-290`) clear CC, spinning the 1 s
  `BUSY_TIMEOUT_MICROS` into a CMD17 fault. `waitTimeout` stays upstream
  (no split needed), so the FIFO data loops (`readData`, `writeData`)
  are untouched. FIFO mode uses the same sites; FIFO reads pause at the
  block gap (`SABGREQ`) between `readData` calls and the yield sites lie
  before CMD18/CMD25 or after CMD12, so no pass runs while the card
  streams into the FIFO.
- **Windows.** Around the storage execute (every SD job primitive,
  mounts, probes and USAGE_REFRESH included; OTA staging chunks pass
  through it too, which is benign because audio is not quiesced during
  staging) and the three raw scans; never around the replay refill or
  inside an IRQ_SOFTWARE-masked section. Activation and rehearsal copies
  run no pass because `ota_copy_staged_to_psram` reads through a direct
  `SD.open` (`firmware/src/optical_main.cpp:5162`) outside every window,
  not because of the quiesce: both copies run before `ota_quiesce`
  (`:9943` before `:9983`, `:10098` before `:10126`), so audio is still
  ready there and only the absent window flag refuses the hook (CR-S9).
  After the quiesce the guard also refuses, since `ota_quiesce` clears
  audio ready.
- **Yield points.** `platform_checkpoint` before each service, except at
  EPOCH_COMPLETE, so nothing runs after the EPOCH_COMPLETE commit (round
  1 B2).
- **E5 attribution (S6).** A nested pass saves the ledger's
  `last_checkpoint`, records AUDIO and commits
  (`aius_boot_record_checkpoint` has no ordering rule,
  `firmware/lib/aius_boot/src/aius_boot.c:144-152`), runs, then records
  the saved value and commits. A wedge inside it then reads `pv=` AUDIO
  through the existing prev-boot capture (`capture_retained_boot_state`),
  instead of the checkpoint it nested in, and counts as an early
  watchdog reset as any non-EPOCH_COMPLETE stall does. No retained object
  is added. Not for lack of OCRAM (the plain image's 131,776 B free is
  736 B over the `0x1FFE0` heap floor,
  `firmware/link/aius_teensy41.ld:271`, and the A/B images sit at most
  that ledger's size lower, measured at 4e start, R4-B2; the
  "margin is zero" comment at `firmware/test/memory/verify_target.py:54-57`
  is stale, and 4e's memory edit rewrites it with the history block
  through `:63`, R4-N1): a new retained section changes the
  retained-region layout that the ORIGIN(RAM) and `0xC60` ASSERTs (`aius_teensy41.ld:244-249`)
  and the boot ledger contract pin, while the ledger wrapper needs no
  layout change at all (R3-B2).
- **Mount.** The vendored wrapper mounts in either mode in one call, so
  the adapter keeps one primitive per case; `restart()` reuses the stored
  mode (upstream already does), so the 1 Hz presence probe does not
  re-init.
- **Faults (B4).** `driver_fault()`
  (`firmware/src/aius_sd_teensy.cpp:107-123`) today counts a not-found
  OPEN as a driver fault and drops the mount. It splits: with
  `sdErrorCode() == 0` the failure is a miss (`sd_diag.open_misses`, mount
  kept); only `sdErrorCode() != 0` counts `driver_faults` and invalidates
  the mount. `m_errorCode` is sticky (cleared only in `SdioCard::begin`,
  `SdioTeensy.cpp:647`), so the adapter calls the vendored `clearError()`
  before every primitive and the split reads only that primitive's code
  (R3-S3). A failed mount with no card inserted (`cardDetected()` false)
  counts `absent_mounts` instead (E10, R3-B1). The vendored driver counts
  `busy_timeouts` (each 1 s `BUSY_TIMEOUT_MICROS` expiry), the operational
  meaning of "DMA hang".
- **Metrics.** `audio_diag` gains `producer_wait_passes`,
  `producer_point_passes`, `producer_pend_latency_max_us` (first ingress
  commit stamped while the producer is idle, to the next pass start, and
  re-stamped at pass exit when the pass leaves ingress non-empty, so a
  commit landing mid-pass or a budget-limited pass is still timed; round 1
  B1, S3), `producer_late_passes` (latency over D) and
  `stack_free_min_bytes` (boot paint, scanned by the 1 Hz METRICS
  sampler); `sd_diag` gains `sd_mode`, `sd_window_nonyield_max_us`
  (the longest stretch inside a window with no hook pass in it: from the
  window opening to the first hook entry, from each hook's return to the
  next hook's entry, and from the last hook's return to the close; time
  inside a hook, nested pass included, is never counted, so a 7.6 ms pass
  never enters 4f F2's SD-start cost, CR-S1), `hook_calls`,
  `open_misses`, `absent_mounts` and `busy_timeouts`. `POST /api/clear` resets the
  watermarks (`producer_pend_latency_max_us`, `ingress_high_water_words`,
  `sd_window_nonyield_max_us`), so a run measures only itself (S5);
  `stack_free_min_bytes` is a since-boot paint floor and is not reset (an
  earlier low only makes the check stricter). Nested passes add to
  `checkpoint_walltime_us[8]` and can raise
  `runtime_epoch_budget_overruns`; neither is an audio fault.
- **Bound and alarm (S4, E8).** D target 2,500 us in 4e: the full status
  render at the 14,981 B worst body is about 2.1 ms (4e's new keys raise
  the worst body to 15,418 B at the final 4e link, about 2.16 ms scaled by length, still
  inside 2,500 us; run 2 measures it) and stays unsliced
  until 4f, and run 2 polls it at 1 Hz. From 4f, 2,000 us (the REPLAY
  checkpoint, 1,060 us measured, is the next stretch). Ingress alarm =
  (8,000 us producer budget + D) x 96 words per ms + one 256-word chunk:
  1,264 words at 2,500 us, 1,216 at 2,000 us; re-derived from the
  measured D after run 2.
- **Epoch period (R4-N2).** About 115 epochs/s, about 8.7 ms per epoch
  (`firmware/src/optical_main.cpp:8715`, `:9342`). Run 1 (source off)
  and run 2 (playback) measure it from `/api/status` `loop_epoch` deltas
  (`:5945`) against wall time; nested passes lengthen epochs under
  playback, so the SD job rate (one primitive per epoch) falls there.
  Every duration in 4e to 4h that counts epochs (4g's mount fill, about
  17 s; 4h H4's settings save, about 0.78 s, 90 epochs) is stated at 115 epochs/s and
  re-derived from run 2's measured playback rate. A wall-clock bound
  (4f F9's 500 ms) is a `millis()` timer, never an epoch count (Phase 5
  E13).
- **Memory (S8, E11, R3-B3).** Every budget is measured on the canonical
  `teensy41-optical-slot-a` image, the deployed one: its ITCM has 4,340 B
  free below the 196,608 B six-block boundary (Phase 4a; the plain
  image's 4,612 B is not the binding figure), and a seventh block fails
  the link (`ASSERT(_itcm_block_count <= 6)`,
  `firmware/link/aius_teensy41.ld:277`). 4e at most 1,280 B, 4f, 4g and
  4h at most 1,000 B each: 4,280 B in all, 60 B spare. New code is
  `FLASHMEM` unless it runs in the producer. DTCM statics growth is at
  most 256 B per phase; 4e records the canonical image's DTCM free over
  the `24,352` B stack-zone floor (`aius_teensy41.ld:285`) before it
  builds, and re-budgets before building if the four phases' 1,024 B do
  not fit it. OCRAM growth is zero in every optical env, enforced by a
  per-env floor table (R4-B2, superseding R3-B2's single 131,776 B
  constant, which is the plain `teensy41-optical` figure and fails the
  canonical slot-a link on an unchanged tree). 4e's first step, before
  any edit, links each env in `OPTICAL_IMAGE_ENVS` (plain, slot-a,
  slot-b, fault), reads its OCRAM free from the post-link summary and
  records the four figures here and in the table. Ratchet rule: an entry
  rises only to a fresh measurement of that env; nothing in 4e to 4h
  lowers one.
- **4e-start baseline (measured 2026-10-07 on the unchanged firmware at
  aius `d6f4f58`; `build.sh teensy41-optical`, `build_ab.sh
  --with-slot-b`, `build.sh teensy41-optical-fault 0`; every post-link
  guard green).** The floor table's entries:

  | Env | OCRAM free (floor) |
  | --- | --- |
  | `teensy41-optical` | 131,776 B |
  | `teensy41-optical-slot-a` | 131,744 B |
  | `teensy41-optical-slot-b` | 131,744 B |
  | `teensy41-optical-fault` | 131,776 B |

  Canonical slot-a: ITCM 4,340 B free at 6 blocks (as Phase 4a);
  `_ebss` `0x20049ba0`, so the DTCM stack zone is 25,696 B (the plain
  image's is 25,728 B; the earlier 25,760 B figure was 32 B stale).
  Headroom over the 24,352 B floor is 1,344 B, so the four phases'
  1,024 B of statics fit with 320 B spare, and stack-bound's total plus
  2,048 B must fit 25,696 B less the statics each phase adds.
- **After 4e's link (2026-10-07).** OCRAM free equals every env's floor.
  Slot-a ITCM free rose to 13,092 B (used 183,516 B): E14 compiled the
  log bodies out, which dropped newlib's `_vfprintf_r` (8,584 B) and six
  stdio helpers from `.text.itcm`; no hot symbol left ITCM (traced
  symbol by symbol against the 4e-start disassembly), and 4e added
  1,010 B (producer-yield 444 B, SD wrapper and driver additions,
  veneers). DTCM statics +32 B; slot-a stack zone 25,664 B to `_estack`,
  25,632 B usable: the Teensy core maps a 32 B no-access MPU region at
  `_ebss` as an overflow trap (`cores/teensy4/startup.c:313-314`), which
  the stack paint and scan and stack-bound's zone all skip (delivery
  audit D-B1: painting it faulted every boot). **DTCM `.data` cliff:**
  the core's `_VectorsRam` is `aligned(1024)` in DTCM `.data`, so
  `.data` grows in 1,024 B steps once the content before it crosses a
  page; 4e's first rebuild crossed it (about 150 B of new `sd_diag`
  format text in DTCM `.rodata`) and the slot-a stack zone fell 1,024 B,
  recovered by moving that format to flash (`PROGMEM`, as `audio_diag`
  already is). About 150 B of margin remains before the next step, so
  4f to 4h put new format strings and constant tables in flash
  (`PROGMEM`/`FLASHMEM`) and check slot-a's `.data` size and stack zone
  after every link. The 4f, 4g
  and 4h budgets stay at 1,000 B of ITCM each (now far inside the
  headroom) and 256 B of DTCM statics each.

**Implementation call (orchestrator, 2026-10-07): `sd_mode` is the mounted
mode with no adapter field** (delivery audit D-S1). `sd_mode` reads the
mounted mode (`none` while unmounted) and `sd_mode_requested` the next
mount's; the adapter's `dma` equals the mounted mode whenever `mounted`
is true because the only setter (`app_sd_set_mode`) refuses while a file
is open and `aius_sd_teensy_set_mode` with no file open drops the mount.
A new mode setter must keep that discipline or add a `mounted_dma` field
set at a successful BEGIN.

**Implementation call (orchestrator, 2026-10-07): stack-bound bounds the
hook paths only.** Over the whole image the per-function target sets do
not converge: classifying the app dispatcher, CDC, clock backend and the
FNET/NativeEthernet stack opens layer after layer and produces artefact
cycles (one of 215 functions) that a frame sum would grossly overcount.
4e deepens only the paths through the card-wait hook, so stack-bound
charges, per W2 SD root, a declared caller chain from `loop` (each hop
checked as a real edge in the resolved graph; a rerouted chain fails),
then the resolved walk below the root to the hook, the 512 B bounce and
the nested pass, plus the exception chain and 2,048 B. Paths that never
reach the hook are unchanged by 4e and backed by the device floor
`stack_free_min_bytes`, as is an ENET vector FNET may install at run
time (`_fnet_isr_vector_init`, outside the install-site scan). The
SdFat card layer's 27-function component (an artefact of one shared
target set per function) is charged as the sum of its frames. The PIT
handler (FNET under `fnet_poll`, priority 224) is walked through FNET's
registered callbacks (mutex, timer, ISR-unlock and service-poll rows,
each cited) and then charged a declared inclusive depth: its computed
lower bound plus a 2,048 B allowance, since FNET's ops-table layers are
unchanged by 4e and the device floor (`stack_free_min_bytes` at least
2,048 B in runs 1 and 2) backs it. Interrupt install sites are keyed by
file, enclosing function and call text, not line.

**4e link result (slot-a, 2026-10-07).** producer-isolation 32/32 and
stack-bound 15/15. Exception chain 4,044 B (per NVIC level plus a 104 B
FPU frame: stall witness 0, SDHC 96, USB and the four SPDIF DMA ISRs
128, the audio software ISR 208, PIT 224 declared at 3,288 B = 1,240 B
walked plus the 2,048 B FNET allowance). Deepest hook path: `loop` >
runtime epoch > `platform_replay` > storage root job > `advance` >
`aius_sd_teensy_execute` > SdFat > `SdioCard::writeSector` >
`waitDmaStatus` > hook > nested pass, 744 B prefix plus 15,788 B below
the root (the nested pass's decoders dominate), 22,624 B with the
exception chain and the 2,048 B margin, against 25,632 B usable: 3,008 B
spare. The unwindowed roots total 9,608 B and 8,736 B.

**Technical calls taken here** (autonomous; owner-confirmed 2026-10-07
except E2, deferred; see the confirmation note under this phase's
contract):

- **E1, a driver-local wait hook, not an override of the weak global
  `yield()`.** Vendoring puts the hook exactly at SdFat's waits, so USB
  serial, NativeEthernet debug prints, `delay()` and `main`'s post-`loop`
  `yield()` never reach the producer, and the core's `yield()`
  (`serialEvent`, EventResponder) is untouched. The guard keeps every
  condition the DMA investigation listed.
- **E2, DMA only into DTCM; every other buffer bounces (B3).** Upstream
  bounces only misaligned buffers (`readSector`, `writeSector`,
  `SdioTeensy.cpp:877-916`, `:1030-1071`), and `readSectors` and
  `writeSectors` (`:918-939`, `:1073-1095`) DMA an aligned buffer of n
  sectors straight in with CMD18 or CMD25. The patch widens the bounce
  test to "not in DTCM, or misaligned" in all four: the single-sector
  pair copies through the 512 B stack buffer, the multi-sector pair loops
  per sector through it, and `rdWrSectors` refuses a non-DTCM buffer
  outright (fail closed). So OCRAM (DMAMEM) and PSRAM buffers never meet
  the controller and no path needs dcache maintenance. This covers
  `ota_copy_staged_to_psram` (`firmware/src/optical_main.cpp:5160-5184`,
  4,096 B reads that `FatFile::read` hands to `readSectors` for whole
  sectors) and the job path (at most 512 B per primitive,
  `kMaximumTransfer`). This meets "cache maintenance in the driver" by
  keeping the cache out of the DMA path. Run 1 rehearses an OTA in DMA
  mode to prove the copy.
- **E3, runtime arming.** Image 1 boots FIFO; authed
  `POST /api/sd/mode` (body `{"dma": true}`, not persisted) switches at the
  next mount with no file open, else `503 sd_busy`. After both runs pass,
  image 2 boots DMA and keeps the route as the field fallback to FIFO.
  Because the arming is not persisted, a watchdog reset during a run
  silently reverts to FIFO, so each run starts with `POST /api/clear`
  after arming and asserts `sd_mode` DMA at its end (S5).
- **E4, vendored names.** Directories stay `SdFat` and `SD` (the LDF
  names `itcm-placement` expects); `library.json` names become `AiusSdFat`
  and `AiusSD` so `lib_ignore` drops the framework copies, as
  `firmware/vendor/NativeEthernet` does. A `NOTICE.md` records provenance
  and every patch. The producer-isolation link half proves the vendored
  copy is what links (S7).
- **E5, E6 and E8** sit in the design bullets above.
- **E7, the fallback verdict (B4).** B2 is for a DMA mode that cannot be
  trusted with data: a content mismatch, `driver_faults` above 0 outside a
  card-out interval (R3-B1), a `busy_timeouts` rise in DMA mode, or a watchdog reset in a DMA run whose
  `pv=` is the SD or HTTP checkpoint. Defects of 4e's own design stay on
  A-DMA and are fixed here: `pv=` AUDIO, a stack floor under 2,048 B, a
  timing miss with integrity intact. `open_misses` and `absent_mounts`
  are never faults. Any
  other reset is diagnosed before a verdict, so the gate never reads a
  design defect as "DMA unreliable" (the round 2 B1 trap).
- **E9, no `alignas(32)` on `g_replay_transfer`.** It is `DMAMEM`
  (`optical_main.cpp:452`, OCRAM). E2's bounce makes its alignment
  irrelevant to DMA, so the attribute buys nothing; up to 31 B of padding
  would fit the OCRAM margin (736 B on the plain image, the A/B figures
  measured at 4e start), but OCRAM growth stays zero (R4-B2).
  The storage transfer buffer already has it
  (`aius_runtime_storage.h:180`).
- **E10, a mount with no card is not a fault (R3-B1).** With the card
  out, the 1 Hz probe starts with a mount attempt
  (`firmware/src/optical_main.cpp:8021-8058`), and `SD.begin` fails with a
  nonzero code, so today `driver_faults` rises once a second
  (`firmware/src/aius_sd_teensy.cpp:216-222`). The adapter reads the
  vendored `cardDetected()` (DAT3, already pulled down by the failed
  begin, as upstream `mediaPresent` reads it) and counts `absent_mounts`
  instead. Device runs also take fault deltas outside the card-out
  interval, so a contact bounce at insertion is diagnosed, not read as
  "DMA unreliable".
- **E11, ITCM budgets from the canonical image (R3-B3).** §Memory. The
  plain image overstated the shared headroom by 272 B; by 4h the old
  budgets (4,608 B) would fail the six-block ASSERT.
- **E12, OCRAM floors per env, not one minimum (R4-B2).** One constant
  set to the minimum across `OPTICAL_IMAGE_ENVS` would pass the A/B
  envs, but would let the plain env grow by the ledger's difference
  unnoticed. A floor per env holds "OCRAM growth zero" in every image
  the gate already guards, at the cost of four numbers instead of one.
- **Isolation argument (B2).** The decode paths do reach `malloc` and
  stdio (DTS `core_filter`, the DTS allocator shim; E-AC-3 `av_log` and
  `avpriv_request_sample` until E14 empties them), so "the producer
  reaches no malloc" is false and not
  needed. What matters is that a nested pass never interrupts an
  allocator or stdio caller: the hook is reachable only from vendored
  SdFat wait loops, and no allocator or stdio function is on any chain
  to it. The proof asserts that over §Walk specification (W5 (2)); the
  decode-path edge allowlist first named here is withdrawn, since lazily
  created decoders reach the allocator along 24 distinct edges, not
  three (CR3-B2).
- **E14, the DTS log is compiled out of the optical images; libc entry
  points are walk leaves (CR-B3).** `dca_format_log` writes hard decode
  errors with `fputs` and `vfprintf` to `stderr`
  (`firmware/lib/aius_dts/src/aius_dts.c:45-61`; nothing in the firmware
  calls `aius_dts_set_log_quiet`), newlib's `_write` sends fds 0 to 2 to
  `Serial` (`cores/teensy4/Print.cpp:93-95`), and `usb_serial_write`
  spins up to `TX_TIMEOUT_MSEC` (120 ms, `cores/teensy4/usb_serial.c:281`,
  `:354`) calling `yield()` each turn (`:366`) when the host is not
  draining. So one malformed DTS frame during playback could stall the
  producer for 120 ms and reach `yield()`, which the isolation proof
  forbids; a walk that descended into libc would also fail on newlib's
  `fp->_write` indirect call on a correct build. The other choice,
  declaring the stdio set a leaf and accepting the stdio-to-Serial
  `yield()` (E1 does not hook `yield()`, so no pass would nest), was
  rejected because it accepts the 120 ms producer stall too, and the log
  reaches a reader only while a USB host is attached, whereas decode
  failures already surface over HTTP (`pipeline_last_failure`). With the sink empty in the optical envs, the walk
  stops at a leaf table holding only allocator, `abort`,
  `__assert_func` and formatting-only entries, and any I/O stdio entry
  reachable from a producer root fails the proof. `__assert_func` still
  prints inside newlib, accepted only because it ends in `abort`, which
  never returns.
- **E14 covers the E-AC-3 log shim too (CR2-B1).** `av_log` and
  `avpriv_request_sample`
  (`firmware/vendor/ffmpeg-eac3/shim/aius_avshim.c:109-133`) each
  format into a 256 B stack buffer with `vsnprintf` and then call
  `g_log_fn`, and the decoder calls `av_log` on its error paths
  throughout (`ac3dec.c:471-1724`, `eac3dec.c`), all compiled into the
  `aius_eac3_core_frame` unit (`aius_eac3_core.c:21-38`). On a correct
  build the walk would meet an `av_log` to `vsnprintf` edge outside the
  allowlist, and both functions' `g_log_fn` call is an indirect branch
  outside the root table. `aius_avshim_set_log` has no caller anywhere
  in `firmware/`, tests included, so `g_log_fn` is always null and both
  bodies are dead on every build. The optical envs therefore compile
  them empty (`AIUS_AVSHIM_LOG=0`), the same switch shape as the DTS
  sink; native builds keep them (default 1), so the `eac3` suite runs
  the unchanged source. The other choice, keeping the bodies, allowlisting
  `av_log` to `vsnprintf` and declaring the null `g_log_fn` call sites in
  an indirect-branch exemption table, was rejected: it adds a second
  exception class to a rule that is otherwise absolute, keeps a 256 B
  formatting frame under the producer in stack-bound for a log no one
  can read, and buys nothing while no setter exists.
- **E15, one walk specification, measured on the image (CR3-B1 to B3).**
  Three review rounds each found the static walk failing a correct
  build on one unlisted edge class (stdio `yield`, the `av_log` shim,
  then unwindowed SD callers, the allocator family, indirect sites and
  veneers). §Walk specification below defines the graph, roots, leaves,
  indirect sites, recursion and assertions once, and its tables are
  seeded from a census of the optical ELF (W7), so every edge class the
  producer actually reaches is classified before the build. Alternatives
  rejected: treating every address-taken function as a possible
  indirect target (W7: the closure grows to 1,854 functions, 540 of them
  forbidden, so a correct build fails) and exempting sites whose targets
  are address-taken only inside a decoder's units (gives stack-bound no
  target set, and the service, sink and SD-stack sites are outside
  decoder units).

**Walk specification (E15; CR3-B1 to B3, CR3-S2).** One post-link walk
serves producer-isolation, stack-bound and the yield-root checks 4f to
4h extend. It reads `arm-none-eabi-objdump -d` of the linked ELF, its
`.symtab` and the link map.

- **W1, graph.** Nodes are symbol addresses (`STT_FUNC`), never names:
  static functions share names (two `decoder_create`, one liba52 in
  ITCM, one E-AC-3 in flash). Edges: `bl`, `b`, `b.w`, `b<cc>`, `cbz`
  and `cbnz` whose target lies outside the containing symbol; a branch
  into another symbol is a tail call and an edge like a call. A veneer
  (`__<name>_veneer`: `ldr.w pc, [pc]` plus one literal word) is
  replaced by an edge to the symbol at the literal with the Thumb bit
  cleared; a veneer of any other shape fails. A literal-pool load (`ldr
  rN, [pc, #imm]`) is not an edge. A function is address-taken (W4
  input) when an `R_ARM_ABS32` relocation applying to an `SHF_ALLOC`
  input section resolves, symbol plus addend, to its start
  (`-Wl,--emit-relocs`, platformio seam), recorded per referencing
  symbol and object file; veneer literals are linker-made and carry
  none. Relocations against non-loaded sections do not count: under
  `-g` each `.debug_frame` FDE carries an `R_ARM_ABS32` to its
  function's start, which would make every function address-taken
  (CR5-S3). A value scan is not
  used: data words collide with low ITCM addresses (`_stext` at 0,
  `aius_spdif_tx24_dma_read_position` at `0x20`, `stamp` at `0x80`
  inflate the ffmpeg row from 99 to 102, CR4-S1). A switch dispatch (`tbb`, `tbh`, or
  `ldr pc, [rA, rB, lsl #2]` followed by an inline table) whose entries
  all lie inside the symbol is intra-function; an entry outside fails.
  Every other register write to `pc` (`blx rN`, `bx rN` with rN not
  `lr`, `ldr pc` from a non-literal, `mov pc`) is an indirect site (W4).
  Library and object-file membership of a function or data object
  comes from the link map (`-Wl,-Map`, platformio seam), since the
  image has no `.debug_info` (CR3-N1); every row in W2, W4 and W6 names
  its symbol together with that object file, so colliding names (two
  `decoder_create`) resolve to one address (CR4-S3).
- **W2, roots.** Producer roots: `aius_audio_service_run` and
  `AiusAudioProcessor::consume_block` (the PCM sink,
  `firmware/src/optical_main.cpp:2502`). Yield roots: the yield-root
  table (4e: `platform_checkpoint`; 4f, 4g and 4h add rows). SD roots
  are computed, then checked against two tables: every function outside
  the SD stack (vendored SdFat and SD, FS.h `File`,
  `aius_sd_teensy.cpp`) with an edge into it whose resolved closure
  reaches `aius_sdio_wait_hook`. The windowed table holds the storage
  execute (`advance` in `aius_runtime_storage.cpp`, around `:335`;
  mounts, probes and OTA staging chunks included), `app_read_directory`,
  `app_write_replay_list` (into which `http_replay_list_json` inlines)
  and `app_replay_delete`. The accepted unwindowed table holds
  `replay_job_advance` (into which `replay_job_route`'s execute at
  `optical_main.cpp:8294` inlines; round 1 S3 forbids windowing the
  refill) and `ota_copy_staged_to_psram` (`:5162`). Rows name linked
  symbols, with `.isra`, `.part` and `.constprop` suffixes ignored; a
  row matching no symbol, or a hook-reaching SD caller matching no row,
  fails. Adapter calls with no card I/O (`aius_sd_teensy_init` from
  `aius_runtime_storage_construct`, `aius_sd_teensy_invalidate_mount`
  from `platform_replay`) never reach the hook and need no row.
  `setup()` makes no SD call at 15edf92 (the first mount is a windowed
  storage BEGIN), so the setup-time mount the review listed has no row
  yet; one added later joins the unwindowed table. The binary cannot
  tell the two tables apart, so the window flag's placement is
  optical-cutover's source guard (CR3-B1).
- **W3, leaves.** The forward walk never descends into: the allocator
  family (`malloc`, `calloc`, `realloc`, `free` in plain, `__wrap_`,
  `__real_` and `_r` forms), `abort` and `__assert_func` (terminal:
  `abort` never returns, `cores/teensy4/startup.c:828-831`), and the
  formatting-only `vsnprintf`, `snprintf` and their `_r` forms. A leaf
  is reachable from a producer root along any edge; there is no edge
  allowlist (CR3-B2 defect 1). The I/O stdio set is forbidden, not a
  leaf (W5 (1)).
- **W4, indirect sites: one declared site table** (CR3-B2 defect 2).
  A row names a containing symbol and its object file (W1), its site
  count at 15edf92 and one target-set source; the function's sites
  share that row's target set, never another row's (a walk that gives
  every site the union of all rows overstates the closure and the
  stack, see W7). A site
  isolation or stack-bound visits with no row, or a changed count,
  fails. Sources:
  - **Registration rows**, each guarded by a source check that the
    registration names exactly the listed functions: the service
    callbacks (`firmware/lib/aius_audio_service/include/aius_audio_service.h:42-45`,
    registered at `optical_main.cpp:2514-2516`; 7 sites in
    `aius_audio_service_run`): `audio_clock_us`,
    `audio_enter_reset_critical`, `audio_exit_reset_critical`. The PCM
    sink (`firmware/lib/aius_codec_api/include/aius_codec.h:125`; 13
    sites in `aius_pipeline_service_one`, `aius_a52_begin_access_unit`,
    `aius_a52_decode_next_block`, `aius_dts_decode_access_unit` and
    `aius_eac3_decode_access_unit`): `consume_block`. The E-AC-3 buffer
    callback in `ff_get_buffer`: `provide_planes`
    (`firmware/lib/aius_eac3/src/aius_eac3.c:103`, `:154`). The
    AiusOpticalApp and runtime platform ops tables, wherever a
    stack-bound path crosses them.
  - **Library rows**: every function whose address is taken by an
    object of the named library, per W1's relocations and the link map. liba52 (`imdct.c`'s
    `ifft128` and `ifft64`, `firmware/lib/aius_a52/a52/imdct.c:77-78`:
    `ifft128_c`, `ifft64_c`); dcadec-core (`core_filter`'s interpolator
    callbacks, `firmware/vendor/dcadec-core/interpolator.h:27-30`: eight
    `interpolate_*`); ffmpeg-eac3 (`do_imdct`, `decode_audio_block`,
    `aius_eac3_core_frame`, `ff_ac3dsp_downmix`, `reset_ctx`,
    `ff_tx_init_subtx` and the tx codelets that call sub-transforms:
    the DSP and tx pointers, 99 functions measured).
  - **Empty rows**: `a52_block`'s `dynrngcall`
    (`firmware/lib/aius_a52/a52/parse.c:636-637`) and `ta_free`'s
    destructor (`firmware/vendor/dcadec-core/ta.c:52`). Their setters,
    `a52_dynrng` and `ta_set_destructor`, are absent from the image (no
    caller); the row asserts they stay absent.
  - **Vtable rows**: every entry of the named vtables. FS.h `FileImpl`
    calls (`File::read`, `File::openNextFile`,
    `cores/teensy4/FS.h:167`, `:216`, inlined into the SD roots) resolve
    to `_ZTV6SDFile`, the only `FileImpl` vtable linked (CR3-S2); SdFat
    block-device and card calls to `_ZTV8SdioCard` and
    `_ZTV16DedicatedSpiCard`; plus functions address-taken by vendored
    SdFat or SD objects (the SD wrapper's date-time callback). Without
    RTTI the image cannot name implementers, so every `_ZTV` symbol (17
    at 15edf92) is classified in the table, and an unclassified one
    fails.
  A target set is walked like a direct edge: isolation follows into
  it, stack-bound charges its deepest member.
- **W5, assertions.** (1) Producer roots reach no forbidden symbol:
  SdFat, SD, FS.h `File`, FNET, NativeEthernet, `usb_serial`, `Serial`,
  `yield`, `aius_sdio_wait_hook`, `aius_producer_yield_*`, and the I/O
  stdio entries (`fputs`, `fputc`, `fwrite`, `puts`, `printf`,
  `fprintf`, `vfprintf`, `fiprintf`, `_write` and their `_r` forms).
  (2) No hook under an allocator or stdio function, stated forward
  (CR4-B1, Phase 5 E17): the forward closure of every allocator-family,
  I/O stdio, `abort` and `__assert_func` symbol in the image (W1 edges
  and veneers, no leaf stop) reaches neither `aius_sdio_wait_hook` nor
  `aius_producer_yield_point`. That closure is newlib and core code
  (measured on the Oct 6 ELF: 24 such symbols, 77 nodes with the row's
  sites not followed), and its indirect sites form one pinned libc row
  whose sites are not followed, 12 sites in 7 functions: newlib's
  stream and locale pointers (`__sflush_r` 3, `__sfvwrite_r` 3,
  `_vfprintf_r` 1, `_vfiprintf_r` 1, `_wcrtomb_r` 2, `_wcsnrtombs_l`
  1) and `_write`'s Print call (1; descriptors 0 to 2 map to `Serial`,
  `cores/teensy4/Print.cpp:93-95`, any other is cast to a `Print*`).
  `usb_serial` and `yield` lie only behind that unfollowed call, so
  they are outside the closure (following it gives 97 nodes and 15
  sites in 8 functions, `yield`'s event dispatch 3 of them; CR5-S2).
  A site in the closure outside the row, or a row count changed,
  fails. The row rests on optical-cutover's guard that no
  `firmware/src` or `firmware/lib` source names `Print`, `Stream`,
  `EventResponder`, `vdprintf`, `dprintf`, `fopen`, `fdopen`,
  `funopen` or `fopencookie`, so no SD file becomes a stdio descriptor
  and no event responder is attached (none does at 9c12136). This
  holds for all roots, windowed or not. The reverse closure it
  replaces failed a correct build: `File::write` is address-taken only
  by `_ZTV4File`, tail-calls `FatFile::write` (so reaches the hook) and
  is called only from Print-slot sites, `_write` among them. (3) Every hook-reaching SD caller is a W2 row, and every
  direct caller of `aius_producer_yield_point` is a yield-root row. (4)
  The window flag is set only in the windowed rows' source functions
  (optical-cutover source guard, with the guard that every
  `firmware/src` and `firmware/lib` function naming `SD.`, `File` or
  `FsFile` outside the adapter is a W2 row's source).
- **W6, recursion.** Every strongly connected component in a walked
  graph is a recursion row with a depth bound, its source guard and
  its guard-admitted chain (the component functions, qualified by
  object file, that the guard lets nest); stack-bound charges the bound
  times the chain's frame sum, then the deepest exit from any component
  member, which is computable where a longest simple path through a
  dense component is not (CR4-S2). An undeclared cycle fails, and a
  chain function outside the component fails. One row at 15edf92: the
  ffmpeg tx component (43 functions and 1,224 internal edges per row,
  W7, under the ffmpeg-eac3 library row: `ff_tx_init_subtx`, codelet
  inits, and codelets calling sub-transforms), bound 2, chain
  `ff_tx_init_subtx` (1,632 B) and `ff_tx_mdct_init_float_c` (56 B):
  2 x 1,688 B = 3,376 B plus the exit (the FFT codelet init,
  `ff_tx_fft_sr_codelet_init_float_c`, 16 B, and its callees). The
  guard excludes `ff_tx_fft_pfa_init_float_c` (2,112 B), which no
  128 or 256 point transform selects. `ac3dec.c:220-223` requests only
  inverse MDCTs of 128 and 256 points, whose init nests one FFT sub-transform
  (`libavutil/tx_template.c:1244`) and whose power-of-two FFT codelets
  nest none; a source guard holds every `av_tx_init` call in
  `firmware/vendor/ffmpeg-eac3` to those two. `ff_tx_init_subtx`'s
  frame is 1,632 B (`subw sp, sp, #1596` plus nine saved registers),
  and lazy E-AC-3 decoder creation runs it on the producer path, so it
  sits under a nested pass in stack-bound.
- **W7, census** (the Oct 6 `firmware/.pio/build/teensy41-optical/firmware.elf`,
  `arm-none-eabi-objdump -d` walked by a scratch script implementing
  W1 to W6, E14's three log bodies treated as empty). 1,981 code
  symbols in `.text.code` and `.text.itcm`, 171 veneers. From the two
  producer roots: 65 nodes when the walk stops at veneers; 311 with
  veneers resolved (256 functions, 55 veneers crossed, 68 tail-call
  edges); 447 with W4's target sets applied per row (391 functions, 56
  veneers; the first census gave every site the union of all rows and
  counted 449, 393 functions, CR4 counts). Without
  E14 the same walk reaches 366 nodes, 8 of them I/O stdio. Leaf edges:
  31 distinct caller and leaf pairs on direct edges (24 allocator, 3
  `abort`, 4 `__assert_func`), 39 with W4 per row (eight more `abort` and
  `__assert_func` edges from the dcadec interpolators and
  `ff_tx_gen_compound_mapping`). Indirect sites: 40 in 18 functions on
  direct edges, 74 in 47 with W4 per row (the tx codelets; the union
  walk's 75 in 48 included `stamp`, a value-scan collision); 3 switch
  tables in the producer closure (13 image-wide), not sites; 574 sites
  in 249 functions image-wide, 108 in 47 SD-stack functions.
  Address-taken: 518 functions by value scan, an upper bound that W1's
  relocations replace. Forbidden symbols reached from producer roots
  with W4: none. Cycles: the one tx component, 43 functions per row (44
  under the union). SD boundary: 8 callers outside the stack, 6
  reaching card I/O (4 windowed, 2 unwindowed), 2 adapter calls with
  none. W5 (2)'s forward closure: 24 allocator, I/O stdio and terminal
  symbols, 77 nodes, the 12 libc-row sites in 7 functions (CR5-S2),
  hook and yield point absent (neither exists before 4e; the walk reaches no
  SD-stack symbol). These are prototype figures; the implementer's
  census on the real link, with relocations and the link map, is
  authoritative. 4e's first step re-runs the census on its own link
  and seeds the tables from it, and 4f, 4g and 4h each re-run it on
  their own link and update the rows and counts they change (yield
  roots, sites, recursion members) in their own commit, never carried
  into a later phase (CR4-S4).
- **W8, fixtures.** The host case in the targeted proof (veneer,
  indirect site, allocator edge, cycle, the `File`-shaped vtable
  function under `_write`, an unrelocated data word) runs against ELFs
  the suite links itself, so each walk rule has a failing case a
  narrower walk would pass, and the `File`-shaped case a passing case a
  stricter walk would fail (CR4-B1).

**OTA and owner.** Two OTAs (image 1, then image 2 after the gate).
Both runs are owner sessions (ruling above): run 1 source-off, ending
with a staged OTA rehearsal in DMA mode; run 2 with AC-3 playing. After
image 2: healthy-boot self-commit and a short repeat of run 1's content
check.

**Residual risks.**

- DMA reliability on this card is unproven until runs 1 and 2; the gate
  falls back to B2.
- uSDHC DMA into DTCM is assumed (SdFat's own sector cache already lives
  there, per the DMA investigation); the content check proves it.
- The static stack bound covers the call paths the checker resolves, the
  frame table declares and the handler table lists (exception frames
  included, R3-S4); a recursion is charged at its W6 bound, which rests
  on the source guard over `av_tx_init`'s lengths, and `alloca` would
  escape it (none known). A framework-installed handler missing from
  the table escapes the source scan (CR3-S5). The device floor is the
  second check.
- The W4 registration and library rows rest on source guards and the
  link map; a function pointer stored from outside its library's
  objects through a path the guards do not read would widen a target set
  unseen. The site-count check catches a new site, not a new target.
  The W5 (2) libc row's sites are not followed: its guard reads only
  `firmware/src` and `firmware/lib`, so a framework library that
  attached an event responder doing card I/O, or turned an SD file into
  a stdio descriptor, would escape it (none does at 9c12136). The device
  runs measure producer latency directly and back it.
- FIFO mode (the route or a reverted image) keeps the card-bound stretch;
  its cover is 4f's SD-start shedding plus the FIFO-mode deferral of
  every SD job's start and of each root job's primitives, the 4g fill and
  4h flush included (4f F2, R3-S8). Since a FIFO card primitive costs
  more than the ring margin, a root job in FIFO mode in practice waits
  until live input stops owning the ring, that is the gate closes or the
  carrier drops (4f F9, F11, R4-S2, CR-B1).
- D is measured, not proven: a future long checkpoint body raises it, and
  `producer_late_passes` shows it.
- Each nested pass costs two ledger commits (unmeasured, expected to be
  microseconds); the one before the pass delays it and is inside the
  latency watermark.

**Prebuild review round 2 (2026-10-07, `reviewer` `opus`): NOT READY.**

- **B1 (blocker).** Hooking `waitTimeout` breaks every DMA data command
  under load. Plan `:2637`/`:2723` put the hook in `waitTimeout`;
  `cardCommand` uses it for the command-complete wait
  (`SdioTeensy.cpp:395`). For CMD17/18/24/25/CMD6, `rdWrSectors` and
  `cardCMD6` set `SDHC_IRQSIGEN` (TCIEN included, `:58-63`) before
  `cardCommand` (`:509`, `:413`). A producer pass (up to 7.6 ms) run by the
  hook inside that wait lets the transfer complete; `sdIrs` (`:282-290`)
  clears all of IRQSTAT including CC; `isBusyCommandComplete` never sees CC
  and spins out `BUSY_TIMEOUT_MICROS` (1 s, `:36`); the read fails as CMD17
  plus a driver fault. Run 1 is source-off so the guard (needs pending
  ingress) never fires and passes; run 2 (AC-3) faults and the gate
  (`:2654`) sends it to B2, a design defect misread as "DMA unreliable".
  Fix: hook only between commands (`yieldTimeout` loop iterations,
  `waitDmaStatus`, `isBusyDat`, between ACMD41 iterations), never inside
  `cardCommand`; add an sdfat-vendor host case "no hook call between the
  `SDHC_XFERTYP` write and the CC observation".
- **B2 (blocker).** The producer-isolation proof cannot pass as written:
  `:2649` requires no malloc/printf reachable. A direct-call walk of
  today's teensy41-optical ELF from `aius_audio_service_run`, enqueue,
  `consume_block`, `apply_config` and the codec entries reaches
  `__wrap_malloc` to `malloc` (via `ta_zalloc_size` from `core_filter` from
  `aius_dts_decode_access_unit`), `vfprintf` (via `dca_format_log` from
  `core_filter`), `__vsnprintf` (via `avpriv_request_sample` from
  `aius_eac3_core_frame`), `abort` (via `__assert_func`) and `av_malloc_array`
  (`firmware/lib/aius_dts/src/aius_alloc_shim.c:95-99` to `__real_malloc`).
  The implementer must weaken it ad hoc or stall. Fix: an explicit
  allowlist (malloc/stdio only from decode paths) plus the argument why a
  hook site can never sit inside malloc/stdio, and assert that instead: the
  hook is reachable only from vendored SdFat wait loops, which never
  allocate. Disassembly used:
  `.scratch/63e414f8-e0a8-416d-8629-cda946de784a/optical.dis` (Oct 6
  teensy41-optical `firmware.elf`).
- **B3 (blocker).** The E2 bounce misses the multi-sector paths. The seam
  (`:2637`) names only `rdWrSectors`/`readSector`/`writeSector`.
  `ota_copy_staged_to_psram` (`firmware/src/optical_main.cpp:5160-5184`)
  reads 4,096 B at file offset 256+k*4096; full sectors go
  `FatFile::read` to `cacheSafeRead(sector,dst,ns)` (`FsCache.h:82-86`) to
  `readSectors` (`SdioTeensy.cpp:918-930`), a CMD18 DMA straight into
  write-back-cached PSRAM (the parked recorder ring; dirty lines may be
  resident; eviction overwrites DMA data). Activation then fails verify
  (`:9949-9979`); the board stays up but OTA is blocked under image 2 until
  `/api/sd/mode` is flipped to FIFO. No 4e run exercises an activation in
  DMA mode. Fix: add `readSectors`/`writeSectors` to the seam with a
  per-sector DTCM bounce; host source-contract case "no `rdWrSectors` call
  with a non-DTCM buffer"; run 1 stages and rehearses with DMA armed
  (rehearsal copies and verifies).
- **B4 (blocker).** The fallback gate is ambiguous. `driver_fault()`
  (`firmware/src/aius_sd_teensy.cpp:107-123`) increments `driver_faults` for
  a not-found OPEN; the 4e seam changes only the invalidation. So the
  acceptance example "GET /sd/www/missing.js answers 404" (`:2658`) raises
  `driver_faults`, failing "delta 0" (`:2652`), and the gate reads it as "a
  driver fault, so B2". Stack-floor breach and watchdog reset have no
  verdict. Fix: count not-found separately; a driver fault is
  `sdErrorCode() != 0` only; define "DMA hang" as 1 s `BUSY_TIMEOUT_MICROS`
  expiry with a nonzero code; classify stack-floor breach and watchdog reset
  explicitly.
- **S1.** Image 1's "no new risk" (`:2632-2634`) is false: the window and
  hook are live in FIFO mode from boot at upstream yield sites plus "init
  and command waits" (`:2725-2727`); a pass inside `cardCommand` after CMD18
  (infinite FIFO read, `SdioTeensy.cpp:956`) lets the card stream into the
  FIFO for ms. `waitTimeout` is shared with FIFO data waits (`:853`, `:864`,
  `:1015`, `:629`), so "never inside readData/writeData" needs a split
  function. Fix: no hook in FIFO mode on image 1 except at existing
  `yield()` sites; name the split.
- **S2.** Stack facts are wrong: "1,408 B headroom" (`:2533`, `:2595`) is
  DTCM free over the memory gate's committed floor (`:1857`), not stack
  headroom; the stack is the 25,760 B free-for-locals and today's peak is
  unmeasured. The stack-bound proof (`:2650`) needs a call-edge source
  (`-fstack-usage` gives frames only), resolution of SdFat virtual
  block-device calls and codec function pointers, a rule for objects
  without `.su` (libc, core), the 512 B DTCM bounce frame live under the
  hook, and the raw-scan path (HTTP dispatch to `app_read_directory` to
  `SD.open`), deeper than the storage path. 4f, 4g and 4h add yield roots
  (render appends, G3 verify) without stack-bound in their green lists
  (`:2869`, `:2982`, `:3102`).
- **S3.** The B1 metric can still under-report (`:2742`): a commit landing
  during a pass is unstamped; if the pass exits on budget with work left,
  the following wait is never timed. Fix: re-stamp at pass exit when
  ingress is non-empty; host case.
- **S4.** D target vs render: D 2,000 us (`:2630`, `:2653`) but the status
  render at the 14,981 B worst body is about 2.1 ms (`:2418-2419`), and the
  editor polls it at 1 Hz in run 2. Fix: set D for 4e to 2,500 us or drop
  full-status polling from run 2.
- **S5.** Watermarks are not reset at run start (latency, ingress
  high-water and nonyield max carry FIFO/boot periods into run 2). Fix:
  proofs start with `POST /api/clear` after arming and assert `sd_mode ==
  DMA` at run end (a watchdog reset silently reverts to FIFO; E3 is not
  persisted).
- **S6.** The retained producer-active flag (`:2642`) has no named home or
  reader; the retained region has link ASSERTs; neither boot/boot-target
  nor the prev-boot classifier is in seams or green; a yield-point wedge
  after `aius_boot_record_checkpoint` (`firmware/src/optical_main.cpp:9419`)
  is attributed to checkpoint N. Fix: name the home and reader, add the
  boot/boot-target and prev-boot classifier to seams and green.
- **S7.** No post-link proof that the vendored copy is linked. Fix: add a
  check (as for NativeEthernet) that vendored SdioCard is linked, not the
  framework's, and that the strong `aius_sdio_wait_hook` wins (a strong
  definition in an LDF archive loses to SdFat's weak one silently).
- **S8.** Memory headroom is unchecked: memory is absent from the 4e, 4f
  and 4h green lists; ITCM 191,996 B against the 196,608 B six-bank
  boundary leaves about 4.6 KiB shared by all four phases; crossing it
  costs 32 KiB DTCM. Fix: add memory to the green lists and budget the ITCM
  share per phase.
- **S9.** The playback remount example (`:2657`) is unproven: during live
  input the probe is deferred (`firmware/src/optical_main.cpp:8162-8164`),
  so a remount happens at a job BEGIN and run 1 is source-off. Fix: add the
  step to run 2 or drop the example.
- **Smaller.** "Never on the OTA path" (`:2729`) is false (OTA staging
  chunks go through the windowed storage execute,
  `firmware/src/aius_runtime_storage.cpp:335`; benign, fix the text).
  `note_pend` must be `static inline` (called from ITCM `update()`). The S7
  fill-sensor device comparison was dropped: add `clock.fill_trims` and an
  unpolled-spread baseline to run 2. `restart()` already keeps the mode
  upstream (`SdFat.h:104-112`).

**Round 2 resolutions (contract revision 2026-10-07; round 3 to confirm).**

- B1 resolved: §Where the hook fires (E6): `yieldTimeout` and ACMD41
  iterations only, never `waitTimeout` or `cardCommand`; sdfat-vendor
  case for no hook between the `SDHC_XFERTYP` write and CC (Phase 5 E6).
- B2 resolved: producer-isolation rewritten to the hook-caller argument
  plus an explicit decode-path allowlist; §Isolation argument.
- B3 resolved: E2 rewritten (bounce in all four sector functions,
  `rdWrSectors` fails closed), sdfat-vendor case, run 1 ends with a DMA
  rehearsal.
- B4 resolved: §Faults (`open_misses`, `busy_timeouts`), E7 verdict table
  in the decision gate and the missing-file example (Phase 5 E7); the
  card-pull part refuted in round 3, superseded by R3-B1.
- S1 resolved: outcome and §Where the hook fires: FIFO hook only at
  upstream `yield()` sites, all between commands; no split needed since
  `waitTimeout` carries no hook.
- S2 resolved: stack-bound proof names its call-edge source, virtual and
  pointer resolution, the frame table for objects without `.su`, the
  bounce frame, the raw-scan path and the 25,760 B zone; 4f, 4g and 4h
  list stack-bound.
- S3 resolved: §Metrics re-stamp at pass exit; producer-yield case.
- S4 resolved: §Bound and alarm, D 2,500 us and alarm 1,264 words in 4e,
  2,000 us and 1,216 from 4f (Phase 5 E8).
- S5 resolved: §Metrics clear semantics, E3, both device runs clear after
  arming and assert `sd_mode` DMA at the end.
- S6 resolved: E5 ledger wrapper (no retained object), boot and
  boot-target in green (Phase 5 E5); the "OCRAM has zero margin"
  premise refuted in round 3, superseded by R3-B2.
- S7 resolved: producer-isolation link half; the strong hook lives in an
  object file.
- S8 resolved: §Memory budgets; memory in the 4e, 4f, 4g and 4h green
  lists; E9 drops the `alignas` (Phase 5 E9); the budgets and OCRAM
  premise refuted in round 3, superseded by R3-B2 and R3-B3.
- S9 resolved: the remount example now claims a source-off remount with
  `hook_calls` advancing and states the live-input probe deferral.
- Smaller resolved: §Windows OTA text, `note_pend` static inline in the
  producer-yield seam, run 2 unpolled baseline and `clock.fill_trims`,
  Mount bullet credits upstream `restart()`.

**Prebuild review round 3 (2026-10-07, `reviewer` `opus`): NOT READY**
(R3-B1, R3-B2, R3-B3). Each item is followed by its resolution.

- **Refuted: round 2 B4 (card-pull part).** With the card out, each 1 Hz
  probe starts with a mount attempt (`optical_main.cpp:8021-8058`,
  `:8162-8165`), and the failed `SD.begin` counts a driver fault, so run
  1's `driver_faults` delta 0 and E7 send a card pull to B2. Resolved by
  R3-B1.
- **Refuted: round 2 S8 and the "OCRAM margin zero" premise (S6, E9).**
  ITCM 4,612 B is the plain image (canonical 4,340 B, budgets summed to
  4,608 B); OCRAM free is 131,776 B, 736 B over the `0x1FFE0` floor.
  Resolved by R3-B2 and R3-B3.
- **R3-B1 (blocker).** Card pull and reinsert raise `driver_faults`.
  Resolved: E10 (`absent_mounts` from the vendored `cardDetected()`),
  sd-target case, and run 1, E7 and 4b take fault deltas outside the
  card-out interval (Phase 5 E10).
- **R3-B2 (blocker).** The memory proof's "OCRAM free unchanged at 131,040
  B" and the E5/E9 "exactly on the heap-floor ASSERT" are wrong (tree
  131,776 B). Resolved: memory seam raises `COMMITTED_OCRAM_FREE_BYTES` to
  131,776 and asserts no change; E5 and E9 reasoning corrected, choices
  kept (Phase 5 E5, E9 corrected). The single constant is superseded by
  R4-B2 (per-env floors).
- **R3-B3 (blocker, also 4f to 4h).** ITCM budgets from the plain image
  fail `ASSERT(_itcm_block_count <= 6)` by 4h. Resolved: §Memory budgets
  on the canonical image, 4e 1,280 B, 4f to 4h 1,000 B each (4,280 B of
  4,340 B); DTCM and the stack zone also read from it (Phase 5 E11).
- **R3-S1 (4e to 4h).** runtime-target compiles the adapter, storage, HTTP
  service and app against the fake `SD.h` with no owner. Resolved:
  runtime-target in the green list, owned by sd-adapter here and by
  parent-wiring in 4f to 4h.
- **R3-S2.** fake-fidelity compares against the framework `SD.h` that
  `lib_ignore` drops. Resolved: proof line points it at
  `firmware/vendor/SD`.
- **R3-S3.** `SdioCard::errorCode()` is sticky. Resolved: vendored
  `clearError()` before each primitive, §Faults, sd-target case.
- **R3-S4.** The stack bound omits exception stacking. Resolved:
  stack-bound adds the per-priority handler chain with FPU frames from a
  checked handler table; the 2,048 B margin stays on top.
- **R3-S5 (4e part).** Isolation covers only hook callers. Resolved:
  producer-isolation checks every `aius_producer_yield_point` caller
  against a declared root table; 4f to 4h extend it and list the suite.
- **R3-S8 (4e part).** The residual risk's "only cover" is false.
  Resolved: residual risk names the FIFO deferral of root jobs (4f F2).
- **R3-S13.** The outcome omitted the ACMD41 hook. Resolved: outcome
  wording.

**Prebuild review round 4 (2026-10-07, `reviewer` `opus`, closure
review): NOT READY** (R4-B2). R3-B1, R3-B3, R3-S1 to S5, R3-S8 and
R3-S13 confirmed closed. Each item is followed by its resolution.

- **R4-B2 (blocker).** `COMMITTED_OCRAM_FREE_BYTES` 131,776 is the plain
  `teensy41-optical` figure, but the constant gates every env in
  `OPTICAL_IMAGE_ENVS` (`firmware/test/memory/verify_target.py:265-273`),
  and the A/B envs carry the 32 B `.aius_retained_ab` ledger
  (`firmware/src/optical_main.cpp:4521-4537`) in the content-sized
  retained section (`firmware/link/aius_teensy41.ld:151-174`), so the
  canonical slot-a link fails on an unchanged tree. Resolved: the memory
  seam becomes a per-env floor table measured at 4e start before any
  edit, with a rise-only ratchet; memory proof line, §Memory, E5 and E9
  text (Phase 5 E12; E2, E5, E9 margins qualified). Supersedes R3-B2's
  constant.
- **R4-S7.** sd-adapter owns `firmware/test/runtime-target/**`, but
  parent-wiring's storage window and mode route compile in that suite.
  Resolved: a `handoff` on sd-adapter passes the fake to parent-wiring at
  sd-adapter's report; parent-wiring owns the final green; coordination
  line.
- **R4-N1 (found by the round 4 revision; resolved in round 5).**
  `firmware/test/memory/verify_target.py:54-57` still says the OCRAM
  margin is zero; the memory worker's floor-table edit corrects it.
  Resolved: see round 5.
- **R4-N2 (found by the round 4 revision; resolved in round 5).** No
  phase states the epoch period, yet F9's 500 ms, H4's 0.8 s and 4g's
  17 s mount fill all rest on about 8 ms per epoch inferred from 4g.
  State the period (or its measured range) once in 4e and derive those
  figures from it. Resolved: see round 5.

**Prebuild review round 5 (2026-10-07, `reviewer` `opus`, closure
review): NOT READY** only on the R4-N1 and R4-N2 text; R4-B2 and R4-S7
confirmed closed, no new blocker. Each item is followed by its
resolution.

- **R4-N1 adjudicated: the memory worker rewrites the comment.** The
  comment at `firmware/test/memory/verify_target.py:54-57` and the
  history block through `:63` are rewritten in the same edit as the
  floor table, stating the per-env floors and the `0x1FFE0` link floor.
  Resolved: memory seam line and the E5 text name it.
- **R4-N2 adjudicated: state the rate once, measure it, time wall-clock
  bounds by clock.** Resolved: §Epoch period states about 115 epochs/s,
  about 8.7 ms per epoch (`optical_main.cpp:8715`, `:9342`); runs 1 and
  2 record the rate from `loop_epoch` deltas (`:5945`); 4f F9's 500 ms
  is a `millis()` timer (4f seam, F9, optical-app case); 4g's 17 s and
  4h H4's 0.8 s are stated at that rate and re-derived from run 2's
  playback rate (Phase 5 E13, F9 amended).

**Clean-room review (2026-10-07, `reviewer` `opus`): NOT READY** (CR-B3).
Each item is followed by its resolution.

- **CR-B3 (blocker).** The producer-isolation proof forbids `yield`
  while allowlisting stdio, but `dca_format_log` reaches `yield()`
  through `vfprintf(stderr)`, newlib `_write` and `usb_serial_write`, so
  the proof fails on a correct build (a walk into libc also meets
  newlib's indirect `fp->_write`), and if the walk stops at libc the
  contract does not say so. Resolved: E14, the DTS log sink is compiled
  out of the optical images (`AIUS_DTS_LOG_STDERR=0`) and libc entries
  are declared walk leaves with I/O stdio forbidden; seams (platformio,
  `aius_dts.c` in parent-wiring's paths), producer-isolation line,
  `dts` in the green list (Phase 5 E14).
- **CR-S1.** `sd_window_nonyield_max_us` was not defined to exclude hook
  pass time, so one 7.6 ms pass would enter 4f F2's DMA start cost and
  shed every SD start in playback. Resolved: §Metrics defines the three
  stretch kinds and excludes time inside a hook; the stretch clock is a
  producer-yield seam with a fake-clock case; 4f F2 cites it.
- **CR-S9.** §Windows gave the quiesce as why OTA copies run no pass,
  but both copies run before `ota_quiesce`. Resolved: §Windows names the
  direct `SD.open` outside every window (`optical_main.cpp:5162`).
- **CR-S12 (4e part).** The memory seam said "the memory worker", but
  parent-wiring owns `firmware/test/memory/**`. Resolved: the seam names
  parent-wiring (the round 4 and 5 entries above keep their original
  wording as history).

**Clean-room closure (2026-10-07): HOLD** (CR2-B1). Each item is
followed by its resolution.

- **CR2-B1 (blocker).** The rewritten producer-isolation proof could not
  pass on a correct build: the E-AC-3 shim's `av_log` calls `vsnprintf`
  from the decode path and is neither a leaf nor an allowlisted edge,
  and both `av_log` and the allowlisted `avpriv_request_sample` call
  through `g_log_fn`, an indirect branch outside the root table.
  Resolved: E14 extended, `AIUS_AVSHIM_LOG=0` in the optical envs
  empties both bodies (`aius_avshim_set_log` has no caller in
  `firmware/`); `avpriv_request_sample` dropped from the allowlist;
  `aius_avshim.c` in the seams and parent-wiring's paths; `eac3` in the
  targeted proof and green list (Phase 5 E14).

**Clean-room round 3 (2026-10-07): HOLD** (CR3-B1 to B3; CR3-S2,
CR3-S5). The reviewer walked the Oct 6 optical ELF with veneers
resolved. Each item is followed by its resolution; the three blockers
are resolved together by §Walk specification, whose tables were seeded
from a census of the same ELF (W7) rather than item by item.

- **CR3-B1 (blocker).** "Every caller chain of the hook back to a
  windowed root" fails a correct build: the replay refill
  (`optical_main.cpp:8294`) and `ota_copy_staged_to_psram` (`:5162`)
  reach the hook through vendored wait loops with no window, and a
  static walk cannot tell a windowed caller of `aius_sd_teensy_execute`
  from the refill. Resolved: W2 computes hook-reaching SD callers and
  checks them against a windowed and an accepted unwindowed table
  (refill, OTA copy; `setup()` has no SD call, so a setup-time mount has
  no row yet); W5 (2) checks "no hook under an allocator" for all roots;
  the window flag's placement moved to optical-cutover's source guard.
- **CR3-B2 (blocker).** The three-entry allowlist missed most of the 31
  distinct allocator, `abort` and `__assert_func` edges (lazy decoder creation,
  destroy paths, `av_fast_realloc`, `a52_init`), `calloc`, `realloc`
  and the `__wrap_` forms were not leaves, and 18 producer functions
  carry indirect sites the "root table" rule could not classify.
  Resolved: W3 makes the whole allocator family plus `abort` and
  `__assert_func` leaves with no allowlist; W4 picks one rule, a site
  table with registration, library, empty and vtable rows, which also
  gives stack-bound its target sets; W6 declares the one recursion the
  library rows expose (the ffmpeg tx component, bound 2); W8 fixtures
  (Phase 5 E15).
- **CR3-B3 (blocker).** 171 linker veneers (`ldr.w pc, [pc]` plus a
  literal) carry every flash to ITCM call; a walk stopping at them sees
  65 nodes instead of 311 and passes a decoder that reached `Serial`.
  Resolved: W1 resolves each veneer to its literal target in every walk
  and in stack-bound (no frame); the veneer fixture fails a walk that
  stops at veneers.
- **CR3-S2.** Stack-bound named only the SdFat block-device virtuals;
  the raw scans and the OTA copy dispatch through FS.h `FileImpl`.
  Resolved: W4 vtable rows resolve `File::read` and
  `File::openNextFile` (`cores/teensy4/FS.h:167`, `:216`) to `SDFile`,
  the stack-bound line names it, and the unwindowed roots are charged
  without a pass.
- **CR3-S5.** The handler scan found only two aius call sites; DMA, USB
  and ENET install inside core and framework code. Resolved: the
  stack-bound line states the scan scope (aius sources) and gives
  framework-installed rows a cited install line checked in the pinned
  framework source; residual risk records what the scan cannot prove.
- **CR3-N1 (found by this revision).** The link half's "DWARF
  compile-unit path" cannot be read: the optical ELF has no
  `.debug_info`. Resolved: a link map in the platformio seam; the link
  half reads archive provenance from it (`libAiusSdFat.a`,
  `libAiusSD.a`, E4), and W1's library membership uses the same map.

**Clean-room round 3 closure (2026-10-07): HOLD** (CR4-B1, CR4-B3;
CR4-S1 to S4). CR3-B1, B3, B4 and S1 to S5 confirmed closed; CR3-B2
closed for the forward walk. W1, W5 and W8 catch all three falsifying
cases (a decoder reaching `Serial` through a veneer, an unlisted
allocator under the hook, an undeclared indirect site). Resolved under
Phase 5 E16: each item takes the simplest formulation that still
catches those cases and passes a correct build. Each item is followed
by its resolution.

- **CR4-B1 (blocker).** W5 (2)'s reverse closure failed a correct
  build: `File::write` (in `_ZTV4File`) tail-calls `FatFile::write`, so
  it reaches the hook, and its only callers are Print-slot sites, one
  in `_write`; either a W4 row holds `_write` (I/O stdio in the closure)
  or none holds `File::write` (an address-taken ancestor outside every
  row). Resolved: W5 (2) restated forward from the allocator, I/O
  stdio, `abort` and `__assert_func` symbols, with one pinned libc row
  of unfollowed newlib, `_write` and `yield` sites behind an
  optical-cutover guard; the ops-table rows the reverse walk needed are
  dropped; the producer-isolation and optical-cutover proof lines, a
  `File`-shaped W8 fixture and a residual-risk line (Phase 5 E17). The
  reviewer's other fix, the reverse form with `_ZTV4File`'s Print and
  Stream slots as an exempt row, was not taken: it needs the same kind
  of guard and still climbs every virtual layer to `loop()`.
- **CR4-B3 (blocker).** The link half required `SDFile` and `FsFile`
  symbols to come from the vendored archives, but both are
  header-inline, emitted as weak COMDATs by the `src/` objects that
  include them. Resolved as the reviewer proposed: archive provenance
  for out-of-line definitions only (`SdioCard`, `DedicatedSpiCard`,
  `FatFile`, `FsBaseFile`, `SDClass`), and for header-inline classes a
  dependency-file check that every object includes the vendored `SD`
  and `SdFat` headers and never the framework's; `-MMD` joins the
  platformio seam (the build writes no `.d` today).
- **CR4-S1.** Address-taken was a value scan; data words collide with
  low ITCM addresses (ffmpeg row 102 against 99). Resolved: W1 reads
  `R_ARM_ABS32` relocations kept by `-Wl,--emit-relocs` (platformio
  seam), and W8 adds an unrelocated-data-word fixture.
- **CR4-S2.** W6's "bound times the deepest simple path" is a longest
  simple path problem on a dense component, and it charged
  `ff_tx_fft_pfa_init_float_c`, which the `av_tx_init` guard excludes.
  Resolved: a row declares its guard-admitted chain and stack-bound
  charges bound times the chain's frame sum plus the deepest exit: 2 x
  (1,632 B + 56 B) = 3,376 B for the tx row, frames read from the Oct 6
  prologues. Stack fit (findings, per-row target sets): producer
  prefix to the tx component 1,368 B; even charged as the deepest
  simple path at bound 2 (about 12.6 to 13.4 KB) the producer is about
  14 to 15 KB, and with the SD chain and its 512 B bounce, the
  exception chain and the 2,048 B margin about 19 to 21 KB against the
  25,760 B zone, about 24.7 KB after the 4e to 4h statics budgets,
  leaving about 4 KB; the chain bound replaces those 12.6 to 13.4 KB
  with 3,376 B plus the exit. Under the global-union model the deepest
  exit alone is about 8.8 KB and the zone overflows, so W4 and the
  stack-bound line now say each site is charged from its own row.
- **CR4-S3.** W4 rows named containing symbols by name, which collide.
  Resolved: W1 states that every W2, W4 and W6 row names its symbol
  with its object file from the link map.
- **CR4-S4.** Site counts were pinned at 15edf92 with no rule for later
  phases. Resolved: W7, each of 4f, 4g and 4h re-runs the census on
  its own link and updates the rows and counts it changes in its own
  commit.
- **CR4 counts.** The census's 449 nodes, 75 sites in 48 functions and
  44-function component came from a prototype that gave every site the
  union of all rows. Resolved: re-measured per row on the same ELF (447
  nodes, 391 functions; 74 sites in 47 functions; 39 leaf pairs; a
  43-function component with 1,224 internal edges) and W7 states that
  the implementer's census on the real link is authoritative.

**Clean-room round 3 second closure (2026-10-07): READY.** No
blocker; four should-fix items. No further clean-room round: the
closing reviewer was itself clean-room, and implementation carries a
delivery audit per phase. Each item is followed by its resolution.

- **CR5-S1 (link half).** The include check required every object's
  dependency file to name the vendored headers, which an object that
  includes no SD header does not. Resolved: no object's `.d` names the
  framework's `libraries/SD` or `libraries/SdFat`, and each object that
  defines an `SDFile` or `FsFile` COMDAT (per the link map) names the
  vendored `SD.h` and `FsFile.h`.
- **CR5-S2 (W5 (2), W7, Phase 5 E17).** The libc row's figures (97
  nodes, 16 sites in 8 functions) came from a closure that followed the
  row's sites into `_write`'s Print call, `usb_serial` and `yield`,
  which the row itself does not follow. Resolved: re-read from the
  census, 77 nodes and 12 sites in 7 functions, `yield`'s three sites
  out of the row. The review gave 13 sites; the census's per-function
  counts (3, 3, 1, 1, 2, 1, 1) sum to 12.
- **CR5-S3 (W1).** Address-taken counted every `R_ARM_ABS32`
  relocation, and under `-g` each `.debug_frame` FDE carries one to its
  function's start. Resolved: only relocations applying to `SHF_ALLOC`
  sections count; W8's relocation fixture adds the FDE case.
- **CR5-S4 (vendored accessor).** `FatPartition` keeps no volume
  serial, so 4h's identity read on a reused mount would need a card
  read. Resolved: the sdfat-vendor patch stores the boot sector's
  serial in a new `FatPartition` member at mount and the accessor
  returns it; the sdfat-vendor suite checks it, and 4h's sd-target
  asserts the reused-mount BEGIN payload adds no card read (4h H3,
  H17). The patch sits with sdfat-vendor, which alone owns
  `firmware/vendor/SdFat/**` and `firmware/test/sdfat-vendor/**` in
  4e. (4h's install-journal once took those paths for the unlink patch;
  that went with the journal, owner overturn 2026-10-07, and 4h no
  longer edits them. The serial accessor stays: H3 needs it.)

### Phase 4f HTTP is best-effort: shedding, render yields, lighter polling

```yaml
phase: 4f
title: HTTP is best-effort (503 shedding, render yields, compact editor status, self-scheduled lighter polling)
status: built # 2026-10-07: host proof green (14 suites), all four images with every post-link guard and OCRAM at each floor, delivery audit FAIL (F-B1, F-B2) then closures FAIL (F-B2 remainder) and PASS; the two device runs are the owner's (no OTA until the owner's go-ahead)
depends-on: phase-4e
prebuild-review: ready # 2026-10-07: rounds 2 to 5, clean-room, clean-room closure, clean-room round 3 and two closures; second closure READY; CR5-S1 to S4 folded; no further clean-room: the closing reviewer was itself clean-room and implementation carries a delivery audit per phase
outcome: >-
  No HTTP dispatch runs CPU work the transmit ring cannot absorb: while
  live input owns the ring (its gate flag set with the carrier locked), a
  request whose cost (its longest non-yielding stretch, nested producer
  time excluded) exceeds the current ring margin answers an immediate 503
  audio_busy with Retry-After, a new SD job is refused the same way when
  its start cost exceeds the margin, and the full status render yields
  after every append, so its cost is one append and it is almost never
  shed; with no live input (gate closed, or a replay) nothing is shed or
  deferred; in FIFO mode a root SD job waits until live input stops owning
  the ring and an HTTP SD start waits at most 500 ms before a 503. The web UI treats a 503 as a silent skip, shows unreachable only
  after three consecutive failures, schedules every poll after the
  previous answer, polls meters at 4 Hz, and the editor polls a compact
  status, so a 1 Hz full-status poll (at least 95% answered 200) no longer
  widens the sub-window spread, D falls to 2,000 us (or to the measured
  DSP Apply parse stretch if that is longer, F7), and audio never waits
  for a response.
unit: component-behind-interface
seams:
  - "firmware/src/aius_runtime_network_http.cpp aius_runtime_http_service: an admission hook (AiusRuntimeHttpAdmit) consulted at the DISPATCH arm before aius_optical_app_http_dispatch and at the SD_START arm before a job starts"
  - "firmware/src/optical_main.cpp: live_owns_ring(), true only while g_aius_spdif_tx24.gate_open is set (set at the executed open, optical_main.cpp:2730, cleared at close, :2648; the flag under which fill() counts underruns, aius_spdif_tx24.h:317) and g_tx_gate_carrier is true (latched in tx_gate_service, optical_main.cpp:9241; with the carrier locked the gate opens only on live input, aius_tx_gate.h:53-54), F11, CR-B1; the admission callback (margin = aius_spdif_tx24_level minus the 128-frame underrun line while live_owns_ring(), otherwise unbounded, so a closed gate or a replay sheds and defers nothing; cost per route class = bucketed maximum of the longest stretch between yield points, nested producer time subtracted, over four completed buckets of 16 dispatches plus the current bucket, five uint16 us slots per window, F12, CR-B2; each class seeded at boot from a declared cost table until its first measurement, and not reset by POST /api/clear, F8), a yield point after every http_json_append in http_status_json and the http_*_json sections (added to the producer-isolation yield-root table), write_status_brief, the SD-start cost as one bucketed window per mode (F12), each seeded at boot from the declared table and kept across POST /api/clear and a mode change (F2, R4-S1), an sd_fifo_defer() predicate that in FIFO mode defers the start of every SD job and each primitive of a root job (the 4g fill and 4h flush) while live_owns_ring() and the margin is short, and is false whenever live_owns_ring() is false; OTA staging chunk jobs are exempt, a started HTTP job's primitives are never deferred, and an HTTP SD start deferred for 500 ms (a millis() timer from its first deferral, not an epoch count, R4-N2) answers 503 audio_busy with Retry-After (F2, F9, R4-S2), http_shed, http_dispatch_max_us, http_class_cost_us (the admission cost per class) and sd_fifo_defers counters"
  - "firmware/src/aius_optical_app.cpp aius_optical_app_http_dispatch: AIUS_HTTP_ROUTE_STATUS_BRIEF and the 503 audio_busy response"
  - "firmware/lib/aius_http aius_http.c: route /api/status/brief; aius_http_write_headers emits Retry-After on 503"
  - "firmware/sd/www/sd-shared.js (one shared poller), dsp-editor.js (METER_POLL_MS 250, pollStatus on /api/status/brief, Apply retried on 503 audio_busy), status.js, replays.js and upgrade.js (setInterval replaced by the shared poller), openapi.json (both 503 bodies on every sheddable route, /api/replays included)"
  - "firmware/tools/rate_gate_capture/capture.py http_get (capture.py:726-745): a 503 is a skipped sample, counted, not retried and not raised; the run fails only when skips exceed 5% of polls"
targeted-proof:
  - "host: http: /api/status/brief parses; a 503 carries Retry-After; every other header set byte-identical"
  - "host: optical-app: a fake margin source sheds each route class at its cost boundary; a status dispatch whose total time is 9 ms but whose longest stretch between yield points is 0.3 ms costs 0.3 ms (nested pass time excluded); one outlier stays the class cost for at least the next 64 dispatches and is gone after the 80th, placed at the first and at the last position of a bucket (F12, CR-B2); with the ring's gate flag clear and level 0, and with the flag set, the carrier unlocked (a replay) and level 0, every class is admitted and sd_fifo_defer() is false in FIFO mode; with the flag set, the carrier locked and level 0, every sheddable class sheds (F11, CR-B1); a route outside the classes (GET /api/dsp/config, a cache-hit /sd/ GET once 4g lands, GET /api/replays/status, /api/recorder) is admitted at margin 0 and its stretch raises http_dispatch_max_us (F13, CR-S2); at boot a class's first dispatch is judged against its seed; POST /api/clear leaves every class cost unchanged; control routes (start, stop, clear, reboot) and OTA routes never shed; an SD_START under margin answers 503 audio_busy with no job started; in FIFO mode with the margin short, sd_fifo_defer() holds a root job's start and its next primitive one epoch; an HTTP SD start held for 500 ms answers 503 audio_busy with Retry-After and starts no job, timed by the fake millis() clock (the 503 comes at 500 ms of clock time whether 20 or 80 epochs ran, R4-N2); an OTA staging chunk job starts and steps with the margin short; a started HTTP download steps every epoch with the margin short; after POST /api/clear and after a switch to FIFO and back, the first SD start in each mode is judged against that mode's seed or its last window, never against 0 (R4-S1)"
  - "host: optical-cutover: every SD job start site and the root-job primitive step consult sd_fifo_defer(), the OTA staging chunk start site does not (source guard, so 4g's fill and 4h's flush starts inherit it); the admission callback and sd_fifo_defer() both read live_owns_ring() and nothing else derives ownership (CR-B1)"
  - "host: dsp-config: firmware/tools/dsp_model_pad.py turns the committed live-model fixture into a body of exactly 12,288 B (AIUS_DSP_MODEL_MAX_JSON_BYTES) that the host parser accepts and parses to the same model as its input, proved through firmware/test/dsp-config/dsp_model_dump.c, a C harness that parses a body with aius_dsp_config_parse_put and prints every model field, run on input and output with the two dumps compared (CR-S3); an input it cannot pad to the limit fails the tool (F10, R4-S8); the tool fills first by giving every number the longest spelling the parser accepts and pads only the rest with whitespace, and reports the split; the case asserts the reported number-byte share equals the share counted in the output and that no number in the output has a longer accepted spelling left (R5-S3)"
  - "host: optical-cutover: a yield point follows every http_json_append in the status sections; status-body-bound: the brief body stays under 512 B and the new keys fit the full body"
  - "host: sd-editor: per page, a 503 keeps the last values and pill; three consecutive failures show unreachable and one success clears it; polls never overlap and honour Retry-After clamped to 0.25 to 5 s; editor meters tick at 250 ms; the flow ends render from the brief body; a 503 on Apply retries; mixer.test.js stays green with the editor's new polling"
  - "host: rate-gate-capture: a 503 poll is a counted skip; over 5% skips fails the run; transport errors keep today's retry"
  - "host: stack-bound (render-append yield roots), producer-isolation (render-append roots in the yield-root table), runtime-target (aius_runtime_network_http.cpp and aius_optical_app.cpp against the fakes), memory (OCRAM free unchanged; on the canonical image ITCM growth at most 1,000 B, DTCM statics growth at most 256 B, of which the eight cost windows take 96 B, F12), registry and itcm-placement green; both images build"
  - "device (owner session, source off, 4f image and synced www, before the AC-3 run; /api/status tx_gate.open 0 at start, else the U24 de-authorised per CLAUDE.md's bench-loop note and re-authorised with the mixer re-applied after): status.html and the editor page loaded and 60 s of 1 Hz full status, then POST /api/sd/mode to FIFO, one editor page load (its SD downloads and the presets listing), and back to DMA; no request answers 503 and every request answers its expected status at its first attempt (200, except the browser's GET /favicon.ico, which answers 404 since no page links an icon and no route serves one, CR3-B4), http_shed and sd_fifo_defers delta 0 (F11, CR-B1)"
  - "device (owner, AC-3, 4f image and synced www, POST /api/clear at start): polled (full /api/status at 1 Hz, at least 95% answered 200) versus unpolled sub-window spread p95 within 10 frames; the editor page open 10 min, with three DSP Applies of the largest shipped preset and one of a 12,288 B body built by dsp_model_pad.py from the owner's current model (its number and whitespace split recorded, R5-S3; GET /sd/aius/dsp.json saved first; every Apply answered 200 and config_generation advanced, else the run is void), then the saved model PUT back and GET /api/dsp/config compared to it (F10, R4-S8), gives underrun_episode_count delta 0; http_class_cost_us for the settings-parse class recorded (F7); producer_pend_latency_max_us at most D and ingress_high_water_words at most the alarm at D, where D is 2,000 us or the measured settings-parse cost if larger; http_shed per class and http_dispatch_max_us recorded (the editor load's GET /api/dsp/config inside the window, F13)"
contract-artifact: spec-only
fan-out: # ui-polling, http-admit and capture-tool concurrently; parent-wiring after http-admit's hook type lands
  - name: ui-polling
    owned-paths:
      - firmware/sd/www/sd-shared.js
      - firmware/sd/www/dsp-editor.js
      - firmware/sd/www/status.js
      - firmware/sd/www/replays.js
      - firmware/sd/www/upgrade.js
      - test/sd-editor/dsp-editor.test.js
      - test/sd-editor/status.test.js
      - test/sd-editor/replays.test.js
      - test/sd-editor/upgrade.test.js
      - test/sd-editor/mixer.test.js
      - test/sd-editor/fixtures/** # CR-S11: the brief-status fixture
  - name: http-admit
    owned-paths:
      - firmware/src/aius_runtime_network_http.cpp
      - firmware/src/aius_runtime_network_http.h
      - firmware/lib/aius_http/**
      - firmware/test/http/**
  - name: capture-tool
    owned-paths:
      - firmware/tools/rate_gate_capture/capture.py
      - firmware/test/rate-gate-capture/**
  - name: model-pad # F10: the 12,288 B Apply body for the F7 device run
    owned-paths:
      - firmware/tools/dsp_model_pad.py
      - firmware/test/dsp-config/test_dsp_model_pad.py
      - firmware/test/dsp-config/dsp_model_dump.c
    handoff: model-pad runs its harness and case directly until it reports; parent-wiring then registers both in test.sh's dsp-config arm (test.sh:372-393, the harness compiled as test_dsp_config.c is) and owns the suite's green (CR-S3)
  - name: parent-wiring # admission callback, render yields, brief writer, route dispatch, counters, openapi
    owned-paths:
      - firmware/src/optical_main.cpp
      - firmware/src/aius_optical_app.cpp
      - firmware/src/aius_optical_app.h
      - firmware/src/aius_runtime_storage.cpp
      - firmware/sd/www/openapi.json
      - test/sd-editor/openapi.test.js
      - firmware/test/optical-app/**
      - firmware/test/optical-cutover/**
      - firmware/test/status-body-bound/**
      - firmware/test/stack-bound/**
      - firmware/test/producer-isolation/**
      - firmware/test/runtime-target/**
      - firmware/test/memory/**
      - firmware/test/registry/**
      - firmware/scripts/test.sh
      - firmware/README.md
      - firmware/lib/aius_producer_yield/** # D-S7, carried in from 4e's delivery audit
      - firmware/test/producer-yield/** # D-S7
review-intensity: active
commit-checkpoint:
  green: [http, optical-app, optical-cutover, status-body-bound, sd-editor, rate-gate-capture, dsp-config, stack-bound, producer-isolation, producer-yield, runtime-target, memory, itcm-placement, registry]
  buildable: [teensy41-optical, teensy41-optical-slot-a]
  parent-commit: "feat(aius): HTTP is best-effort during playback (Phase 4f)"
  coordination: owned-path sets are disjoint; firmware/sd/www/mixer.html and mixer.js are tracked but owned by the mixer session and not edited (mixer.html inherits dsp-editor.js polling); ui-polling owns only mixer.test.js, to keep it green; ui-polling owns test/sd-editor/fixtures/**, which parent-wiring's openapi.test.js only reads
```

**Owner confirmation (2026-10-07).** Confirmed: F1 to F14.

**Built on 4e before its device verdict (orchestrator, 2026-10-07).**
4f starts on 4e BUILT, not on 4e's device runs, which wait for the
owner. An E7 verdict of B2 (Phase 4b, Fallback design) re-plans 4f to
4h before their device runs; F2's FIFO deferral already covers a board
left in FIFO mode.

**Carried in from 4e's delivery audit:**

- **D-S7 (should-fix, parent-wiring and producer-yield).** An ingress
  commit after `AudioMemory` but before the first epoch's AUDIO visit
  leaves a pend stamp the first pass serves, so the lifetime
  `producer_pend_latency_max_us` and `producer_late_passes` carry a
  boot artefact (device runs are unaffected: each starts with `POST
  /api/clear`). Fix: disarm the stamp when audio becomes ready
  (`aius_producer_yield_plat_audio_ready` turning true), with a
  producer-yield host case.

**Delivery audit (2026-10-07): FAIL, decisions taken (orchestrator):**

- **F-B1, a shed class could stay shed for the session.** A shed adds no
  sample, so a window aged only on admitted work; one 5 ms settings
  parse on PCM (margin at most about 3.7 ms) locked settings PUTs out
  while live, and a saturated 65,535 us slot needs more than the 2,048
  frame ring. Decided: a cost bucket closes after 16 samples or 2,000 ms
  of `millis()`, whichever first, so an outlier ages out within about
  10 s of wall time, admitted or not. The F12 "64 to 79 dispatches"
  bound becomes "64 to 79 samples or about 8 to 10 s".
- **F-B2, FIFO deferral starved bring-up.** FIFO is the boot default, so
  a reboot with a source playing held PROBE, LOAD_CONFIG and LOAD_DSP
  until silence, and a root job paused mid-flight left every HTTP SD
  route on `503 sd_busy`. Decided: bring-up root jobs are exempt from
  `sd_fifo_defer()`, and a root job is deferred only at its start; once
  its first primitive has run it runs to its end (it may cost a dropout
  in FIFO, as a started HTTP job already may).
- **F-S1 to F-S5** (stale upload staging after a shed Apply, the F9
  timer surviving a teardown, the SD-start sample charged to the wrong
  mode, no fetch timeout in the poller, the first-measurement outlier
  bound): fixed as the audit proposed.
- **As built (parent-wiring round 3).** F12's windows gain `opened_ms`
  and are 16 B each (128 B for eight; `g_http_admission` 176 B in DTCM,
  slot-a DTCM statics +192 B over 4e, inside 256 B). A window whose
  buckets all age out empty returns to its seed, not 0, since a zero
  cost would admit a class free (R4-S1). F2 and F9: a root job is held
  only while its phase is BEGIN and no event is pending (a primitive's
  event lands an epoch later), bring-up jobs are never held, and a
  started replay refill is never held. F-S2 restarts the F9 timer on any
  consult that is not a re-consult of the held start, not on an action
  id, because the transaction resets its action ids to 1 at teardown.
  F-S5 needed no code: the first-measurement outlier holds through 79
  further samples. Closure check FAIL on F-B2's remainder (the bring-up
  exemption ended when PROBE found the card, before LOAD_CONFIG and
  LOAD_DSP): a once-per-boot latch set when the first LOAD_DSP ends now
  holds `sd_fifo_defer()` false until it is set; the root-step BEGIN
  hold is dropped (deferral only at the start gate, so no started job is
  parked); and a staging armed by a request that a network teardown
  dropped during its DEFER is cleared from the composition. The web poller times out GETs at 4 s (a mutating
  request has no default timeout: a first SD save can take over 7 s).
- **Device run (F10):** after the 12,288 B Apply, wait about 15 s (the
  cost bucket ages out) before PUTting the owner's model back, and retry
  that restore on `503 audio_busy`; a shed restore otherwise voids the
  run.

Implements the owner's "a UI poll never interrupts audio" and "lighter
polling" decisions, and the write-path investigation's load shedding.
Under A-DMA the producer cannot preempt an HTTP dispatch, so dispatch CPU
work is either cut into appends (the status render) or admitted against
the ring margin.

- **F1, admission.** While live input owns the ring (F11), shed when
  `level - 128 < cost_frames(class) + 48`,
  at 48 frames per ms; otherwise the margin is unbounded and nothing
  sheds. A class's cost is what the producer actually waits
  behind: the longest stretch between yield points (or between hook calls
  inside an SD window) in one dispatch, with the cycles of nested producer
  passes subtracted (F5). It is the bucketed maximum of F12 (about the
  class's last 64 to 80 dispatches), seeded at boot from a declared
  per-class cost until the first measurement replaces it, and `POST
  /api/clear` does not reset it (F8). Classes: full status, brief
  status, meters, events, settings PUT parses, directory listing, SD job
  start. Every other route is never shed (F13): a control 503 would hide
  a state change, and OTA is excluded.
- **F2, SD job start.** The start cost is the F12 bucketed maximum of
  each SD window's longest stretch between hooks (4e's
  `sd_window_nonyield_max_us` definition, pass time excluded, CR-S1), one
  window per mode, so a single outlier ages out instead of shedding every start
  (S10); in DMA mode it almost never sheds. In FIFO mode it is the
  longest card primitive. Like the route classes (F8), each mode's
  window is seeded at boot from the declared table and is never reset:
  neither `POST /api/clear` nor a mode change empties it (a mode change
  only selects the other mode's window), so the first SD start after a
  clear or a switch to FIFO is judged against the seed or the last
  measurement, never admitted at cost 0 (R4-S1). HTTP SD
  starts are admitted at the SD_START arm, but the 4g fill and 4h flush
  are root jobs the scheduler starts with no HTTP request, so admission
  never sees them (R3-S8). So in FIFO mode one predicate,
  `sd_fifo_defer()`, defers every SD job's start, and each next primitive
  of a root job, one epoch while live input owns the ring and the margin
  is short, and never otherwise (F11); 4g and 4h call
  it at their start sites (optical-cutover source guard). Shedding plus
  this deferral is the FIFO fallback's cover; F9 bounds it.
- **F3, the brief route.** `GET /api/status/brief` carries exactly the
  keys `renderFlowEnds` reads (`carrier`, `state`, `sample_rate`,
  `pipeline_diag.codec_pc`, `tx_gate.open`, `tx_gate.replay_running`)
  under the same names and nesting, so the renderer is unchanged; 4h adds
  `sd_flush` to it. `status.html` keeps the full body (it is the
  diagnostics page).
- **F4, UI.** One poller in `sd-shared.js`: self-scheduled after each
  answer, a 503 is a silent skip honouring Retry-After, "unreachable"
  only after three consecutive failures (timeout, network error or a
  non-503 status). Meters at 250 ms (4 Hz). `mixer.html` and `mixer.js`
  are tracked (`4b68103`, the mixer dashboard skin) and owned by the mixer
  session, so 4f does not edit them: `mixer.html` loads `sd-shared.js` and
  `dsp-editor.js` and inherits the new polling, and `mixer.js` makes no
  network poll (its timer is a form sync, `firmware/sd/www/mixer.js:973`,
  CR-S12).
  `test/sd-editor/mixer.test.js` is ui-polling's to keep green.
- **F5, why the cost excludes producer time (B5).** With per-append yields,
  a full status dispatch contains producer passes (up to 7.6 ms each);
  counting them made the status class about 480 frames, shed on PCM
  (margin 128 to 225 frames) nearly always, leaving `status.html` dark
  and the polled-vs-unpolled proofs comparing mostly 503s. The producer's
  own time is not a delay the ring suffers, so it is subtracted; the
  proofs also require at least 95% of 1 Hz full-status polls to answer
  200, so a vacuous comparison fails.
- **F6, the capture tool survives shedding.** `capture.py` `http_get`
  catches `HTTPError` with the URL errors, so today a 503 is retried three
  times and then raises, ending the session. A 503 becomes a counted
  skip; the run fails when skips exceed 5%, the same bar as F5.
- **`/api/replays` refusal (round 1 nit).** The code comment
  (`firmware/src/optical_main.cpp:6241-6245`) and the openapi description
  agree: `503 sd_busy` while a job owns the volume. 4f adds `503
  audio_busy` beside it, and openapi lists both bodies on every sheddable
  route.
- **F7, the DSP Apply parse is measured, then D follows it (R3-S6).**
  `PUT /api/dsp/config` parses a body of up to 12,288 B
  (`AIUS_DSP_MODEL_MAX_JSON_BYTES`,
  `firmware/lib/aius_dsp_config/include/aius_dsp_config.h:18`) at
  dispatch with no yield point, likely the longest producer wait left
  after 4f and unmeasured. Admission already sheds it when the margin is
  short (a 503 on Apply retries, F4), so it threatens the latency
  watermark, not the ring. The device run Applies the largest shipped
  preset and a 12,288 B body (F10) and records the class's
  `http_class_cost_us`; an Apply that is not answered 200 parsed less
  than the whole body and voids the measurement (R4-S8). The Applies
  change what the room hears, so the run saves the owner's model first
  and PUTs it back at the end (the owner runs every device session, so
  this is a restore step, not a question); D for 4f and 4b is 2,000 us or that cost if
  larger, with the ingress alarm re-derived from it. Yields inside the
  parser (portable C, a callback per N tokens) become plan work only if
  the measured cost exceeds the 2.67 ms worst-phase margin.
- **F8, class costs survive a clear (R3-S7).** Zeroing them at `POST
  /api/clear` admitted each class's first dispatch after it unchecked.
  A class cost is a model of the route, not a run watermark, so a clear
  leaves it; at boot each class starts from a declared seed (the class's
  estimated longest stretch, kept beside the class enum) that its first
  measurement replaces. `http_shed` and `http_dispatch_max_us` still
  reset. The per-mode SD-start windows follow the same rule (F2, R4-S1).
- **F9, the FIFO deferral is bounded where a client waits (R4-S2).** In
  FIFO mode the SD-start cost is a whole card primitive (tens of ms)
  against a margin of at most about 4.7 ms, so a deferral keyed on it
  holds while live input owns the ring, until the gate closes or the
  carrier drops (F11; digital silence on a locked carrier still owns the
  ring). That is right for the
  root jobs (a 4g fill or 4h flush waits until the gate closes; 4h states what that
  does to its bound), and wrong where a client is waiting. So: OTA
  staging chunk jobs are exempt at start and per primitive (F1 never
  sheds OTA, and a held chunk would hang the staging PUT until the
  client's timeout); a started HTTP job's primitives are never deferred
  (its headers may be on the wire, so no 503 can follow, and a stall
  mid-response is worse than its cost, admitted at its start); an HTTP SD
  start held for 500 ms answers `503 audio_busy` with `Retry-After: 1`
  and starts no job. The 500 ms is a `millis()` timer from the start's
  first deferral, not an epoch count, because the epoch rate falls under
  playback (4e §Epoch period, R4-N2). `sd_fifo_defers` counts the held
  epochs.
- **F10, the 12,288 B Apply body keeps the owner's sound (R4-S8).** A
  model built from neutral sections cannot reach the limit: two EQ bands
  per channel (`AIUS_DSP_MODEL_EQ_MAX`,
  `firmware/lib/aius_dsp_config/include/aius_dsp_config.h:17`) and the
  live model is 899 B
  (`firmware/test/dsp-config/fixtures/dsp_config_live_2026-10-07.json`),
  and neutral values would change what plays. So `dsp_model_pad.py` takes
  the owner's current model and pads it to exactly 12,288 B, so the
  parser walks a full-size body that decodes to the model already
  playing. Whitespace is the parser's cheapest path per byte, so a
  mostly-whitespace pad would understate the worst cost (R5-S3): the tool
  first gives every number the longest spelling the parser accepts for
  the same value, pads only the remainder with JSON whitespace, and
  reports the split. Built (2026-10-07): every one of the fixture's 29
  numbers reaches the 63 B cap (`AIUS_DSP_CONFIG_NUMBER_BUFFER_MAX` 64;
  the parser rejects leading zeros, so the padding is trailing fraction
  zeros), which is still only 14.9% of the body; the rest is whitespace.
  The schema caps the model's numbers, so no admissible 12,288 B body
  holds many more, and this split is the realistic worst case. The live
  GET fixture lacks `base_generation`, so the device body is built with
  `--base-generation` set to the board's `config_generation`. A dsp-config host case proves the output parses to
  the input's model at exactly the limit and asserts the number-byte
  share. Each PUT, the restore
  included, carries `base_generation` from the board's current
  `config_generation` when the model uses it.
- **F11, the margin exists only while live input owns the ring
  (CR-B1).** With no source the producer commits nothing, `fill()`
  substitutes silence and the level drains to 0
  (`firmware/lib/aius_audio/src/aius_spdif_tx24.h:288-320`, level is
  `written - consumed`, `:240`), so `level - 128` reads -128 and every
  sheddable route would 503 with the board idle: `status.html` dark, a 4g
  page load during the mount fill refused, 4h's card-swap Apply shed, and
  F9's root jobs never run (an empty ring is itself a short margin).
  `live_owns_ring()` is the existing state that says the ring is being
  heard from live input: the ring's own gate flag
  (`g_aius_spdif_tx24.gate_open`, set when the producer executes the open,
  `firmware/src/optical_main.cpp:2730`, cleared at close, `:2648`; the
  flag under which `fill()` counts underruns, `aius_spdif_tx24.h:317`)
  and the latched carrier (`g_tx_gate_carrier`, `optical_main.cpp:9241`;
  with the carrier locked the gate can open only on live input,
  `firmware/lib/aius_replay/include/aius_tx_gate.h:53-54`, so flag set
  and carrier unlocked is a replay). The ring flag, not the latched
  decision `g_tx_gate_open`, because an armed open waits for the
  producer (`tx_gate_actuate`, `:2633-2641`) with the ring possibly empty
  and the pad dark. Outside ownership the margin is unbounded: admission
  admits every class and `sd_fifo_defer()` is false. Replay is excluded
  because it is out of scope (4b) and keeps its SD-sized gate-open
  target. The optical-app cases drive the three states and a source-off
  device step checks `http_shed` and `sd_fifo_defers` stay 0.
- **F12, cost windows are bucketed maxima (CR-B2).** An exact maximum
  over the last 64 dispatches keeps 64 samples per window: eight windows
  (six route classes, two per-mode SD starts) cost 512 B at 1 B or
  1,024 B at `uint16`, against 4f's 256 B DTCM statics budget, with
  OCRAM growth 0 and no EXTMEM object allowed
  (`test/recorder/test_no_extmem_heap.py`), and 4g's PSRAM carve does
  not exist yet in 4f. So each window keeps five `uint16` microsecond
  slots (saturating at 65,535): the maxima of the last four completed
  buckets of 16 dispatches and of the current bucket, plus a one-byte
  position and a one-byte seeded flag, 12 B per window and 96 B for the
  eight. The cost is the largest slot; a completed bucket replaces the
  oldest. An outlier therefore stays the cost for at least 64 and at most
  79 further dispatches (gone after the 80th), which the optical-app case
  pins from both bucket edges. At boot all five slots hold the seed and
  the first measurement overwrites all five, so F8's "first measurement
  replaces the seed" holds. With the counters (`http_shed` per class,
  `http_dispatch_max_us`, `sd_fifo_defers`, F9's timer) 4f's DTCM growth
  is about 140 B of its 256 B.
- **F13, only the listed classes shed (CR-S2).** Every route outside
  them is admitted whatever the margin: control and OTA (F1); a 4g
  cache-hit GET, whose send loop yields every 8 chunks (4g G3, G7); `GET
  /api/dsp/config`, a one-shot model serialise the editor makes at load
  and after Apply, which a 503 would turn into a failed page load;
  `GET /api/replays/status` (replay is out of scope); the five
  `/api/recorder*` routes (authed diagnostics). Their stretches still
  raise `http_dispatch_max_us`, and the 4f AC-3 run records it with the
  editor load inside the window, so a long unlisted route shows as a
  watermark above D and becomes a class, not a silent stall.

**OTA and owner.** One OTA plus a card sync of `firmware/sd/www/`. The
device runs need the owner (the source-off step and AC-3).

**Residual risks.** The margin is read at dispatch; a burst arriving
during the dispatch moves it, which the per-append yields bound. An
outlier sheds its class for up to 79 further dispatches (F12). During a
replay nothing is shed or deferred, so card and HTTP work may starve the
replay (out of scope). The listener backlog
stays 1; self-scheduling lowers the request rate but parallel browser
connections still queue.

**Prebuild review round 2 (2026-10-07, `reviewer` `opus`): NOT READY.**

- **B5 (blocker).** Admission cost includes the producer's own passes, so
  full `/api/status` is shed almost always. F1 (`:2881-2887`) uses "the
  class's measured dispatch maximum" (never decays). With 4f's per-append
  yields running producer passes inside the render, the status class max
  becomes about 2 ms plus one pass (up to 7.6 ms), about 480 frames. Shed
  when `level - 128 < cost + 48`: on PCM (margin 128 to 225 frames) full
  status is shed nearly always; on AC-3 whenever the sawtooth is below about
  650. Consequences: `status.html` dark during playback; the acceptance
  instrument `firmware/tools/rate_gate_capture/capture.py:726-745` retries
  503 three times then raises; the 4f proof (`:2833`) and the 4b proof
  (`:2058`) compare spreads with most polls 503, vacuous. Fix: for
  yield-sliced classes, cost = the longest non-yielding append stretch
  (producer cycles subtracted), not total dispatch; proofs assert at least
  N% of 1 Hz polls answer 200; add `capture.py` 503 handling to the 4f
  seams.
- **S2 (4f part).** 4f adds yield roots (render appends) without
  stack-bound in its green list (`:2869`); add it (see 4e S2).
- **S8 (4f part).** Memory is absent from the 4f green list; add it (see
  4e S8).
- **S10 (F2, `:2888`).** `sd_window_nonyield_max_us` is a lifetime max
  including the image-1 FIFO period and outliers, so one outlier sheds every
  SD job start until a clear. Fix: a windowed max or a declared DMA bound.
- **S11 (F4, with 4g G1).** The `mixer.*` facts are stale: `mixer.*` is
  tracked (commit `4b68103` "mixer dashboard skin over the DSP editor") and
  `test/sd-editor/mixer.test.js` exists; "untracked" and "only when
  present" are wrong, and `mixer.test.js` is not in ui-polling's owned
  paths. Fix: correct the F4 text and the coordination line (`:2872`), add
  `mixer.test.js` to ui-polling's owned paths.
- **Nit.** Reconcile the `/api/replays` `sd_busy` entry with the code
  comment at `firmware/src/optical_main.cpp:6241-6245`.

**Round 2 resolutions (contract revision 2026-10-07; round 3 to confirm).**

- B5 resolved: F1 and F5 cost excludes nested producer time, windowed
  maximum; outcome and device proof require at least 95% of polls 200;
  `capture.py` 503 handling is a seam with its own worker and suite (F6;
  Phase 5 F5, F6).
- S2 (4f part) resolved: stack-bound in the green list with the
  render-append roots.
- S8 (4f part) resolved: memory in the green list with the ITCM and DTCM
  budgets.
- S10 resolved: F2 per-mode windowed maximum, reset on mode change.
- S11 (4f part) resolved: F4 text and the coordination line corrected,
  `mixer.test.js` in ui-polling's paths.
- Nit resolved: the `/api/replays` paragraph after F6.

**Prebuild review round 3 (2026-10-07, `reviewer` `opus`): NOT READY**
only on R3-B3. Each item is followed by its resolution.

- **R3-B3 (4f part).** ITCM budget from the plain image. Resolved: 1,000
  B on the canonical image (4e §Memory), memory proof line.
- **R3-S1 (4f part).** runtime-target compiles `aius_runtime_network_http.cpp`
  and `aius_optical_app.cpp`, which 4f edits. Resolved: in the green list,
  owned by parent-wiring.
- **R3-S5 (4f part).** Render-append yield points are unchecked by
  isolation. Resolved: producer-isolation in the green list and
  parent-wiring paths; the appends join the yield-root table.
- **R3-S6.** The DSP Apply parse is likely the longest producer wait and
  the 4f device run has none. Resolved: F7, device run Applies the
  largest preset and a 12,288 B model, records `http_class_cost_us`, and
  D follows it (Phase 5 F7).
- **R3-S7.** `clear_counters` zeroes class costs. Resolved: F8 seeds at
  boot, no reset on clear; optical-app cases (Phase 5 F8).
- **R3-S8 (4f part).** FIFO mode does not admit root jobs. Resolved: F2
  `sd_fifo_defer()` for every SD job start and primitive, optical-cutover
  source guard (Phase 5 F2 amended).

**Prebuild review round 4 (2026-10-07, `reviewer` `opus`, closure
review): READY** with three should-fix items. R3-B3, R3-S1, R3-S5 to
S8 confirmed closed. Each item is followed by its resolution.

- **R4-S1 (F2).** The SD-start cost was still "reset on a mode change and
  by clear", contradicting F8: after `POST /api/clear` or a FIFO switch
  the first FIFO SD start was admitted at cost 0. Resolved: F2 one window
  per mode, seeded at boot, kept across clear and mode change; seam and
  optical-app case (Phase 5 F2 corrected).
- **R4-S2 (F2).** FIFO cost (a card primitive, tens of ms) exceeds the
  margin (at most about 4.7 ms), so every SD job start and primitive
  deferred forever during live input, OTA staging chunks and in-flight
  responses included. Resolved: F9, OTA staging exempt, started HTTP
  jobs never deferred per primitive, an HTTP start held 500 ms answers
  `503 audio_busy`; root jobs wait for silence, stated in 4e's residual
  risks and 4h H8; optical-app and optical-cutover cases (Phase 5 F9).
- **R4-S8 (F7).** The synthetic 12,288 B model must be valid and answered
  200, or the parse exits early and under-measures; applying it during
  AC-3 changes what the room hears. Resolved: F10 pads the owner's
  current model to the limit (neutral sections cannot reach it), a
  dsp-config host case and the model-pad worker; the device run voids on
  a non-200 Apply and restores the saved model (Phase 5 F7 amended, F10).

**Prebuild review round 5 (2026-10-07, `reviewer` `opus`, closure
review): READY** with one should-fix item. R4-S1, R4-S2 and R4-S8
confirmed closed. Each item is followed by its resolution.

- **R5-S3 (F10).** Whitespace is the parser's cheapest path per byte, so
  a mostly-whitespace pad understates the worst parse cost. Resolved:
  `dsp_model_pad.py` gives every number its longest accepted spelling
  first, whitespace only for the rest, and reports the split; the
  dsp-config case asserts the number-byte share; the device run records
  the split (F10, Phase 5 F10 amended).
- **R4-N2 (4f part, from 4e).** F9's 500 ms rested on an inferred epoch
  period. Resolved: a `millis()` timer from the first deferral; seam,
  F9 and an optical-app case on the fake clock (Phase 5 F9 amended,
  E13).

**Clean-room review (2026-10-07, `reviewer` `opus`): NOT READY** (CR-B1,
CR-B2). Each item is followed by its resolution.

- **CR-B1 (blocker; carries into 4g, 4h, 4b).** With no source the ring
  drains to level 0, so the margin reads -128 and every sheddable route
  503s, `sd_fifo_defer()` holds every FIFO job, and F9's "root jobs wait
  for silence" never ends; no proof caught it (host cases fake the
  margin, the device run is AC-3 only). Resolved: F11, `live_owns_ring()`
  from the ring's gate flag and the latched carrier (both verified in
  source), margin unbounded outside it; outcome, seam, F1, F2 and F9
  restated; optical-app cases for gate closed, replay and live at level
  0; optical-cutover guard that both consumers read the one predicate; a
  source-off device step before the AC-3 run (Phase 5 F11).
- **CR-B2 (blocker).** An exact 64-dispatch maximum needs 64 samples per
  window, 512 to 1,024 B for eight, over the 256 B DTCM budget.
  Resolved: F12, five `uint16` bucket slots per window, 96 B for eight,
  about 140 B with the counters; the host case now pins "at least 64, at
  most 79 further dispatches"; memory proof line and residual risk
  (Phase 5 F12; F1, F2, F5 amended).
- **CR-S2.** Whether unlisted routes shed was unstated. Resolved: F13,
  only the listed classes shed; unlisted routes are admitted and watched
  through `http_dispatch_max_us`; optical-app case and AC-3 record
  (Phase 5 F13).
- **CR-S3.** "Parses to the same model" had no harness and no owner for
  its `test.sh` registration. Resolved: `dsp_model_dump.c` in model-pad's
  paths; a `handoff` makes parent-wiring register it and the case in the
  dsp-config arm.
- **CR-S11 (4f part).** ui-polling did not own the fixtures a
  brief-status case needs. Resolved: `test/sd-editor/fixtures/**` in
  ui-polling's paths; coordination line.
- **CR-S12 (4f part).** F4 cited `mixer.js:783`. Resolved: the form-sync
  timer is `firmware/sd/www/mixer.js:973`.

**Clean-room round 3 (2026-10-07): HOLD** (CR3-B4; CR3-B1 to B3 through
the producer-isolation and stack-bound suites in the green list). Each
item is followed by its resolution.

- **CR3-B1 to B3 (4f part, from 4e).** The render-append yield roots
  join a walk that failed correct builds. Resolved in 4e (§Walk
  specification): the appends are yield-root rows (W2), each direct
  caller of `aius_producer_yield_point` must be one, and W5 (2) holds
  for their chains; nothing else changes here.
- **CR3-B4 (blocker).** The source-off device step required every
  request answered 200, but no page under `firmware/sd/www/` links an
  icon and no route serves `/favicon.ico`, so a cold page load's
  favicon request answers 404 (the 2026-10-07 investigation recorded
  it) and the proof failed a correct build. Resolved with the proof
  text, not an icon: no request answers 503, and every request answers
  its expected status at its first attempt, the favicon 404 excepted.
  Shipping an icon or an empty-icon link in 4g's bundles would change
  every page and rebuild the bundles to satisfy a proof, while the 404
  is the unknown-route answer (`/favicon.ico` matches no route and is
  not under `/sd/`, `firmware/lib/aius_http/src/aius_http.c:237-242`),
  with no card access (Phase 5 F14).

**Clean-room round 3 closure (2026-10-07): HOLD** only via 4e's
producer-isolation suite in the green list (CR4-B1, CR4-B3); CR3-B4
confirmed closed. Each item is followed by its resolution.

- **CR4-B1, CR4-B3 (4f part, from 4e).** Resolved in 4e: W5 (2) is a
  forward property from the allocator and stdio symbols, so the render
  appends' chains need no reverse rows, and the link half splits
  archive provenance from the header-inline include check; nothing
  else changes here.
- **CR4-S4 (4f part).** Resolved by 4e W7: 4f re-runs the census on
  its own link and records its render-append yield-root rows and any
  changed site counts in its own commit.

**Clean-room round 3 second closure (2026-10-07): READY.** No 4f
item. No further clean-room round: the closing reviewer was itself
clean-room, and implementation carries a delivery audit per phase.

- **CR5-S1 to S4 (4f part, from 4e).** Resolved in 4e (link half
  include check, libc row figures, `SHF_ALLOC` relocations, stored
  volume serial); nothing changes here.

### Phase 4g PSRAM web cache with gzip page bundles

```yaml
phase: 4g
title: The web app is served from a CRC-checked PSRAM cache as gzip page bundles with ETag validation
status: built # 2026-10-10: host proof green (18 suites, verifier run), both images with every post-link guard and OCRAM at each floor, delivery audit FAIL then closure PASS, clean-room HOLD (fill restart livelock) then closure; device runs 1 and 2 are the owner's (no OTA until the owner's go-ahead)
depends-on: phase-4f
prebuild-review: ready # 2026-10-07: rounds 2 to 5, clean-room, clean-room closure, clean-room round 3 and two closures; second closure READY; CR5-S1 to S4 folded; no further clean-room: the closing reviewer was itself clean-room and implementation carries a delivery audit per phase
outcome: >-
  A web UI load reads no card once the cache is warm: every /sd/www file
  and the presets listing are served from a cache on PSRAM pad 2 whose
  512 B chunks are CRC32-checked against the chip (dcache lines dropped
  first) at fill, before the headers and again on the copy each chunk is
  sent from (SD fallback and refill on a mismatch before the headers, a
  closed connection and refill after); each page, `mixer.html` included,
  is one gzip bundle (its CSS and scripts inlined by the sync tool's build
  step) sent with Content-Encoding gzip, `Vary: Accept-Encoding`, a strong
  ETag and 304 on revalidation from a RAM manifest; the cache fills one
  file per job after each mount, and an HTTP SD request that finds a fill
  file in flight cancels it at its next primitive boundary and is served
  from the card, the fill held until 2 s after the last HTTP SD job (a
  cancel with no HTTP job after it, from 4h's flush start or freeze,
  holds it until 2 s after the flush ends or the freeze lifts), so a
  page load or a sync during a fill completes and a cancelled fill
  always resumes; the cache follows every
  PUT, DELETE, remount and card swap.
unit: component-behind-interface
seams:
  - "firmware/tools/www_bundle.py (new) and firmware/tools/aius-sd-sync.sh (runs it, uploads <page>.html.gz beside the unmodified sources; retries a 503 honouring Retry-After for up to 15 s per file)"
  - "firmware/lib/aius_web_cache (new, portable C): aius_web_cache_place (pad-2 carve per fitment), _lookup, _lease and _release, _verify (chunk CRCs after a dcache flush-delete, with a yield callback), _fill_begin, _fill_chunk, _publish, _invalidate, _listing"
  - "firmware/lib/aius_http: request parse of If-None-Match and Accept-Encoding; aius_http_write_headers ETag, Content-Encoding and Vary: Accept-Encoding; 304 with no body"
  - "firmware/src/aius_optical_app.cpp dispatch_sd and dispatch_sd_index: cache first (gzip negotiation, 304, verify before the headers, then per chunk copy to a 512 B stack buffer, CRC the copy, send the copy, a producer yield point every 8 chunks, added to the producer-isolation yield-root table; lease released by sd_settle, as the recorder data read does), the SD download job on a miss or a pre-header CRC fault; an HTTP SD request (page miss, upload, delete, listing) that finds a fill file in flight is not refused: it requests the fill's cancel, waits at the SD_START arm (as for an sd_fifo_defer() hold) until the fill stops at its next primitive boundary, then starts (G6, G9, R3-N1); the presets listing from the manifest"
  - "firmware/src/optical_main.cpp: the carve at setup, the fill scheduler (one CACHE_FILL job per file in the SD checkpoint; web_cache_fill_cancel(), the one cancel entry used by an HTTP SD request here, by 4h's flush start and by 4h's H9 freeze, stops the in-flight file at its next primitive boundary, drops its unpublished entry and re-queues the file; no next file starts while the fill hold is set; a cancel latches the hold with no expiry, and the canceller's end (its HTTP SD job's completion, the 4h flush job's end, the H9 unfreeze) calls web_cache_fill_hold_release(), which sets the expiry to 2 s after that end; every HTTP SD job completion also extends it to 2 s after it, and an expiry only ever moves later (R5-S1); each start through 4f's sd_fifo_defer(); each fill primitive reads one 512 B chunk, copies it to PSRAM, flush-deletes it and CRCs that chunk read back from the chip in the same primitive, folding the card bytes into the file's running CRC for the ETag; publish does no per-byte work, it checks every chunk was read back, writes the manifest entry and its CRC, G10, CR-S6), invalidation on upload completion, delete, remount and volume change, www_cache status keys (fill_holds and fill_cancels included)"
  - "firmware/lib/aius_sd (CACHE_FILL root job kind, one file, cancellable at any primitive boundary: it only reads the card, so a cancel closes the file and writes nothing) and firmware/sd/www/openapi.json"
  - "added in build (owner, §Build decisions): firmware/lib/aius_http transaction (cache options, 304, body_chunk source, unions holding HEAD sizes), firmware/src/aius_runtime_network_http.{h,cpp} (write_body_chunk, held DISPATCH for card-reading routes), firmware/link/aius_teensy41.ld (aius_http.c rodata in flash), firmware/test/memory budgets, the replay tools' fill_busy retry, firmware/sd/www/replays.js (no /api/replays poll)"
targeted-proof:
  - "host: www-bundle (new): deterministic output (gzip mtime 0), every stylesheet and script of a page inlined in document order, closing script and style tags escaped, gzip round trip equals the inlined page, a missing asset fails the build, all five pages bundled (mixer.html included unconditionally, tracked since 4b68103), no inlined script reads document.currentScript.src; the sync tool retries a 503 per Retry-After and gives up after 15 s"
  - "host: web-cache (new): placement is the pad-2 tail from 0x70808000 on a 16 MiB fitment and absent on 8 MiB fitments, disjoint from the recorder ring and every aius_ota_staging_place result; fill, read-back and publish; _fill_chunk calls the flush-delete hook and CRCs that chunk before returning, a chunk whose read-back CRC differs from the card bytes fails the fill, and _publish of a 175,913 B entry reads no chunk (a counting fake PSRAM reader sees zero reads at publish, G10, CR-S6); verify calls the flush-delete hook for each chunk before its CRC; a flipped bit in any chunk fails verify and drops the entry; a lease holds an entry across invalidate while the replacement serves the next request; capacity full falls back to SD; the listing comes from the manifest"
  - "host: http: If-None-Match and Accept-Encoding parse (absent, gzip, q=0, lists); ETag, Content-Encoding and Vary headers; a 304 has no body"
  - "host: sd (CACHE_FILL job, one file; a cancel at any primitive boundary closes the file, issues no write and leaves the job idle at the next step), optical-app (hit, miss, 304, gzip negotiated and plain fallback, a pre-header CRC fault falls back to the SD job, a mid-stream fault closes the connection and schedules a refill, a page miss and an upload PUT arriving while a fill file is in flight are not refused: the fill stops at its next primitive boundary, its entry stays unpublished and the file is re-queued, and the request's job starts the epoch after with no fill file started between; a fill file starts only once the hold has lapsed and 2 s have passed since the last HTTP SD job; a cancel followed by a release and no HTTP SD job, the 4h flush and freeze shape driven here through the cancel and release entries, starts the next fill file 2 s after the release and never before it, and a cancel with no release yet keeps the hold, R5-S1), optical-cutover (the fill start site consults sd_fifo_defer()), memory (no EXTMEM, no extmem heap, OCRAM free unchanged; on the canonical image ITCM growth at most 1,000 B, DTCM statics growth at most 256 B), stack-bound (the send loop with its 512 B copy buffer under a yield-point pass), producer-isolation (the send loop in the yield-root table), runtime-target (aius_optical_app.cpp and aius_runtime_storage.cpp against the fakes), status-body-bound (www_cache keys), sd-editor (openapi), itcm-placement and registry green; both images build"
  - "device run 1 (source off, 4g image, synced www): every plain file and every decoded bundle equals the source and www_bundle.py output; a repeat load answers 304; a page load and a full aius-sd-sync.sh started right after a reboot (during the mount fill, about 17 s at one 512 B read per epoch at about 115 epochs/s source-off, 4e §Epoch period; under playback it is longer, re-derived from 4e run 2's measured rate, R4-N2) both complete, the page load with no retry, with www_cache.fill_holds and fill_cancels recorded; http_shed and sd_fifo_defers delta 0 with /api/status tx_gate.open 0 throughout (4f F11, CR-B1); www_cache.crc_faults and stall-witness events delta 0; www_cache.entries equals the synced set"
  - "device run 2 (owner, AC-3): ten cold and warm page loads give underrun_episode_count delta 0 and no SD job primitive during the warm loads"
contract-artifact: spec-only
fan-out: # www-bundle, web-cache and http-cache-headers concurrently; parent-wiring after web-cache's header lands
  - name: www-bundle
    owned-paths:
      - firmware/tools/www_bundle.py
      - firmware/tools/aius-sd-sync.sh
      - firmware/test/www-bundle/**
  - name: web-cache
    owned-paths:
      - firmware/lib/aius_web_cache/**
      - firmware/test/web-cache/**
  - name: http-cache-headers
    owned-paths:
      - firmware/lib/aius_http/**
      - firmware/test/http/**
  - name: parent-wiring # carve, fill job, cache-first dispatch, invalidation, status keys
    owned-paths:
      - firmware/src/optical_main.cpp
      - firmware/src/aius_optical_app.cpp
      - firmware/src/aius_optical_app.h
      - firmware/src/aius_runtime_storage.cpp
      - firmware/src/aius_runtime_storage.h
      - firmware/lib/aius_sd/**
      - firmware/test/sd/**
      - firmware/sd/www/openapi.json
      - test/sd-editor/openapi.test.js
      - firmware/test/optical-app/**
      - firmware/test/optical-cutover/**
      - firmware/test/memory/**
      - firmware/test/stack-bound/**
      - firmware/test/producer-isolation/**
      - firmware/test/runtime-target/**
      - firmware/test/status-body-bound/**
      - firmware/test/registry/**
      - firmware/scripts/test.sh
      - firmware/platformio.ini
      - firmware/README.md
review-intensity: active
commit-checkpoint:
  green: [www-bundle, web-cache, http, sd, optical-app, optical-cutover, memory, stack-bound, producer-isolation, runtime-target, status-body-bound, sd-editor, itcm-placement, registry]
  buildable: [teensy41-optical, teensy41-optical-slot-a]
  parent-commit: "feat(aius): serve the web app from a PSRAM cache as gzip bundles (Phase 4g)"
  coordination: owned-path sets are disjoint
```

**Owner confirmation (2026-10-07).** Confirmed: G1 to G4 and G6 to
G10.

**Starting figures (after 4f's link, 2026-10-07).** Slot-a ITCM 12,900 B
free (4g budget 1,000 B); DTCM statics +192 B over 4e (each phase 256 B;
4g's 256 B is still whole); slot-a stack zone 25,472 B usable;
`.data` 33,472 B with `_VectorsRam` at 0x20008000 (about 150 B before
the next 1,024 B step: new strings and tables go in flash); OCRAM at
every env's floor. The stack-bound deepest hook path is HTTP
`app_write_replay_list` at 22,632 B.

**Carried in from 4e and 4f (found during their builds):**

- **Mechanise the ITCM and DTCM budgets (parent-wiring,
  `firmware/test/memory/**`).** `test/memory` enforces only the per-env
  OCRAM floors; each phase's ITCM (1,000 B) and DTCM statics (256 B)
  budgets and the `.data` step are checked by hand from build logs. Add
  a slot-a ITCM-free floor and a DTCM `_ebss` ceiling to
  `verify_target.py`, set from the post-4f link and moved only by a
  phase commit that records the new figure, plus a `.data`
  `_VectorsRam` step check, each with a synthetic failing case.
- **Rejected HTTP upload start (parent-wiring, `firmware/test/runtime-target/**`).**
  4f's closure check traced that a rejected HTTP SD start
  (`aius_sd_job_start` returning FAILED) leaves its upload staging armed
  until the orphan drop clears it one to two epochs later; add a
  runtime-target case pinning that.
- **Nit (http-admit's file).** `firmware/src/aius_runtime_network_http.h`
  repeats the `deferred_start_present` comment; drop one when the file
  is next touched.

**Build decisions (2026-10-10).**

- **Scope widened to the HTTP transaction (owner).** `aius_http` could
  send no cache header or 304 and had no per-chunk seam for G7, so 4g
  also owns `firmware/lib/aius_http/**` and
  `firmware/src/aius_runtime_network_http.cpp`: the app response carries
  `AiusHttpCacheOptions` and an optional `body_chunk` source; the WRITE
  arm's `write_body_chunk()` fills a 512 B stack buffer from it. FNET
  copies the bytes before `send` returns (`fnet_netbuf.c:264`).
- **All three CRC checks kept (owner):** fill read-back, pre-header
  verify, send-copy CRC. Pad 2 is the marginal chip and a CRC over
  512 B costs microseconds.
- **G8 is a fill-only step gate (owner).** A fill start and each fill
  primitive wait on `sd_fifo_defer()`; every other root job keeps 4f's
  every-epoch rule; a cancelled fill is never paused.
- **A replay start is refused while the fill walk is incomplete
  (owner, after delivery-audit finding 2).** Replay is a test facility,
  never a normal operating mode. A replay during the walk lost the card
  at every file boundary and could underrun, so a replay start answers
  `503 fill_busy` with `Retry-After` until the walk completes, and the
  replay-starting test tools retry on it for up to 120 s. `Retry-After`
  is the HTTP library's fixed 1 s: a computed value needed another
  `aius_http` change for no gain, since the tools' budget is fixed.
- **A playing replay pauses the fill (owner, closure nit A).** The fill
  starts no next file while a replay is playing or switching to replay,
  so replay and fill never share the card in either order.
- **A cancelled fill file resumes at its next chunk (owner, clean-room
  finding 1).** G9 dropped the unpublished entry and restarted the file
  from chunk 0; with `replays.html` open its 5 s poll cancelled every
  cycle, `openapi.json` (262,071 B, about 4.5 s) never finished, the
  walk never completed and the replay gate refused forever. A cancel now
  keeps the checked chunks and the running file CRC; the restart reopens
  and seeks to the next chunk, so a cancel costs at most one primitive.
  Upload, delete, remount and card loss still drop the partial entry.
- **Card swap while powered is unsupported (owner, after audit finding
  6); the cache takes the accidental-disconnect defensive posture.** The
  outcome's "follows every card swap" is narrowed: a card fault that
  means the card is gone invalidates the whole cache and restarts the
  walk after the next mount; a remount or volume-serial change already
  does. The card probe stays skipped during playback (4e).
- **OCRAM unchanged by reuse, not growth.** `AiusHttpRequest` (104 B)
  and `AiusHttpTransaction` (864 B) keep HEAD's size: `If-None-Match` is
  a `uint16_t` offset, the chunk source shares `sd_backup_path` (idle
  for an IMMEDIATE response), a download's ETag rides in
  `sd_temp_path`. `root_cancelling` sits in `AiusRuntimeStorage` tail
  padding.
- **Local calls.** Bundles build to scratch, never into `sd/www`; the
  sync tool's 15 s is summed `Retry-After` (absent counts 1 s), non-503
  failures keep four tries 1 s apart. The CACHE_FILL sink is the SD
  library's `OUTPUT` action, not a callback; a cancelled fill ends
  `FAILED`/`CANCELLED`. The fill walk re-reads one directory window per
  file start rather than keep a path queue in DTCM. `www_cache.entries`
  is counted in `optical_main.cpp`. The memory suite's `_VectorsRam`
  check fires only once the table has moved, not on approach.

Implements the owner's PSRAM web cache (pad 2, CRC per 512 B chunk, SD
fallback, recorder ring unchanged) and gzip bundling with cache
validation. A fill during playback is safe only because 4e makes its card
waits run the producer.

- **G1, one inlined bundle per page.** `index.html`, `status.html`,
  `replays.html`, `upgrade.html` and `mixer.html` (tracked since
  `4b68103`, so bundled unconditionally): its
  stylesheets and scripts inlined in document order, gzip'd, uploaded as
  `/sd/www/<page>.html.gz`. One request per cold page; fonts stay
  separate (already compressed). The sources are uploaded unchanged too,
  and a client without `Accept-Encoding: gzip` (curl, the content check)
  gets the plain file.
- **G2, validation.** Strong ETag `"<crc32 of the file>-<length>"`
  computed at fill. Pages and bundles: `Cache-Control: no-cache` (always
  revalidate; a 304 is cheap). Fonts keep `max-age=86400`, plain `.js`
  and `.css` keep today's 86400; both gain the ETag.
- **G3, verify before serve.** Every chunk's CRC is checked at dispatch
  with a producer yield point every 8 chunks; a mismatch here serves the
  SD download job instead (the owner's SD fallback) and schedules a
  refill. The body then streams per G7. The manifest carries its own CRC;
  the DTCM control block stays under 64 B.
- **G4, carve.** 2 MiB from `0x70808000`, the tail of pad 2 above the
  recorder ring's straddle (the ring spans `0x70008000` to `0x70808000`
  on the 16 MiB fitment, and OTA staging aliases pad 1). The www set is
  about 187 KB gzip plus 145 KB of fonts plus the plain sources; 4h's
  overlay and staging share the carve (about 1.2 MB worst case). No
  EXTMEM object, as `test/recorder/test_no_extmem_heap.py` requires.
- **G5, scope.** `/api/replays` stays a card scan under 4e's window
  (replay is out of scope).
- **G6, the fill yields to HTTP (S12, R3-B4).** A whole-set root job would
  hold `sd_storage_idle()` false for about 1 MB of reads, and
  `aius-sd-sync.sh` gives up after four 503s 1 s apart
  (`firmware/tools/aius-sd-sync.sh:106-121`). So the fill is one
  CACHE_FILL job per file, held off by HTTP and cancelled by it. An HTTP
  SD request that arrives while a fill file owns the volume is not
  refused (round 3 had it answer `503 sd_busy` with `Retry-After`, which
  a browser page load does not retry, R3-N1): it cancels the fill file
  (G9), waits at the SD_START arm for the one epoch the cancel takes, and
  starts; the cancel sets the fill hold, and every HTTP SD job
  completion extends the hold to 2 s after it, so the scheduler starts
  no next file while HTTP is active (a cancel with no HTTP job after it
  expires by G9's release rule, R5-S1). A sync (a burst of PUTs) keeps the
  fill paused until it ends, and the post-upload refill is the same rule.
  The sync tool still honours `503` with `Retry-After` for up to 15 s per
  file, for a volume owned by a job that is not a fill (and 4h's staging
  full). Without the hold the round 2 design failed: the mount fill
  (about 1 MB at one 512 B read per epoch, about 17 s at the source-off
  115 epochs/s of 4e §Epoch period, longer under playback and
  re-derived from 4e run 2's measured rate, R4-N2) outlasts the 15 s
  limit, and every retry could land on the next file. Why not let an
  upload body populate its entry directly: 4h's staging already does
  that for every PUT, so doing it here is work 4h replaces.
- **G8, fill starts in FIFO mode (R3-S8).** Each CACHE_FILL start and
  primitive goes through 4f's `sd_fifo_defer()`, so in FIFO mode a fill
  waits while live input owns the ring and its margin is short; in DMA
  mode, and with the gate closed (the mount fill after a source-off
  reboot), the predicate passes (4f F11, CR-B1). Likewise a page load
  during the mount fill with no source is never shed: admission sees an
  unbounded margin.
- **G10, the fill read-back is per chunk, inside the primitive (CR-S6).**
  "Read-back CRC before publish" could have been one whole-file CRC at
  publish, 175,913 B for `dsp-editor.js`, in the SD checkpoint with no
  yield point. So each fill primitive reads, copies, flush-deletes and
  CRCs its own 512 B chunk from the chip, and the file CRC for the ETag
  accumulates from the card bytes one chunk per primitive; publish only
  confirms every chunk was checked and writes the manifest entry and the
  manifest CRC, so no new yield root is needed.
- **G7, every CRC reads the chip (S13).** PSRAM is write-back,
  write-allocate, so a CRC over lines just written reads dcache.
  `arm_dcache_flush_delete` runs on each chunk before the fill read-back
  CRC and before each pre-header verify CRC. At send, each chunk is
  flush-deleted, copied to a 512 B stack buffer, CRC'd and sent from the
  copy, so the bytes checked are the bytes sent; a mismatch after the
  headers closes the connection (the browser retries), counts
  `www_cache.crc_faults` and schedules a refill. `Vary: Accept-Encoding`
  goes on every cacheable response, since one URL has gzip and plain
  representations.
- **G9, a fill file is cancellable (R3-N1, R4-S3).** A CACHE_FILL job
  only reads the card into PSRAM, so stopping it at a primitive boundary
  loses nothing: the file closes, its unpublished entry keeps its
  checked chunks and the file resumes at its next chunk (amended
  2026-10-10, §Build decisions). One entry point, `web_cache_fill_cancel()`,
  serves every consumer that must not wait out a whole file (about 3 s
  for `dsp-editor.js` at 512 B per epoch): an HTTP SD request here, and
  in 4h the flush start (H8) and the OTA and reboot SD-writes freeze
  (H9). Serving from the card first makes a page load during a fill
  cost one epoch, not a refusal. The hold a cancel sets must expire
  even when no HTTP SD job follows (R5-S1): after an H9 freeze cancels
  a fill and the activation is abandoned with `ota_sd_busy`, a hold
  that only an HTTP job completion extends would never lapse, the
  refill would never resume and every page load would miss the cache.
  So a cancel latches the hold with no expiry, and the canceller's end
  (its HTTP SD job's completion, the flush job's end, the unfreeze)
  calls `web_cache_fill_hold_release()`, which sets the expiry to 2 s
  after that end; HTTP SD job completions still extend it, and an
  expiry only moves later.

**OTA and owner.** One OTA plus a card sync (bundles). Run 1 is
source-off; run 2 needs the owner.

**Residual risks.** Pad 2 is the chip the board investigation flagged
marginal: the CRC catches corrupt data but not a FlexSPI2 bus hold (the
1.79 s family was convicted on the unfitted tail, which
`stall_mpu_tighten()` fences), so the stall-witness delta is a proof
line; if pad 2 misbehaves, P2 (halve the recorder ring to free pad 1) is
an owner call. Inlined bundles duplicate `dsp-editor.js` between the
index and mixer pages (about 50 KB gzip each) and drop per-file browser
caching across pages. The fill hold reads as live for up to 4 s after
a lapse that lands within about 4 s of an exact 2^32 ms `millis()`
wrap (every 49.7 days). A warm cache with no card traffic keeps serving
during an unnoticed disconnect until a card operation faults; the card
probe stays skipped during playback (4e). A cache-served response torn
down between dispatch and accept keeps its lease until the next www GET
drops it, the same bounded window the recorder data read has. A
`presets/` listing the manifest answers still cancels a fill in flight,
since the route cannot know in advance that a CRC-fault fallback will
not read the card.

**Prebuild review round 2 (2026-10-07, `reviewer` `opus`): NOT READY.**

- **Blockers.** None; S10 to S13 are contract corrections (S10, S11 are
  filed under 4f).
- **S2 (4g part).** G3 verify adds a yield root without stack-bound in the
  green list (`:2982`); add it (see 4e S2).
- **S11 (4g part).** The `mixer.*` facts are stale: `mixer.*` is tracked
  (commit `4b68103`) and `test/sd-editor/mixer.test.js` exists, so the
  www-bundle proof's "mixer.html bundled only when present" (`:2941`) and
  G1's "when present" are wrong. Fix: bundle `mixer.html` unconditionally
  and cover it.
- **S12 (`:2938`, `:2930`).** The cache fill is a root job that blocks HTTP
  SD jobs (`sd_storage_idle`); after a mount (about 1 MB) and after every
  upload refill, page misses and the next sync PUT answer 503 `sd_busy`;
  `aius-sd-sync.sh` gives up after 4 attempts 1 s apart
  (`firmware/tools/aius-sd-sync.sh:106-121`), which can fail the 4g, 4h and
  4b sync proofs. Fix: the fill yields to HTTP jobs, or an upload body
  populates its cache entry directly.
- **S13 (G3 and fill read-back).** CRC checks read the cache, not the chip:
  PSRAM is WBWA, so "read-back CRC before publish" reads lines just written
  in dcache and proves nothing about marginal pad 2; the serve-time
  check-then-stream leaves a gap. Fix: `arm_dcache_flush_delete` each chunk
  before the CRC check, and CRC per chunk at send time; add `Vary:
  Accept-Encoding` (the same URL has gzip and plain representations).

**Round 2 resolutions (contract revision 2026-10-07; round 3 to confirm).**

- S2 (4g part) resolved: stack-bound in the green list and parent-wiring
  paths; the send loop's 512 B copy buffer is a named root.
- S8 (4g part, from 4e) resolved: memory proof carries the ITCM and DTCM
  budgets and OCRAM unchanged.
- S11 (4g part) resolved: G1 and the www-bundle proof bundle `mixer.html`
  unconditionally.
- S12 resolved: G6 per-file fill, HTTP priority, deferred refill, sync
  tool `Retry-After`; run 1 syncs during the mount fill (Phase 5 G6);
  refuted in round 3 (nothing queues an HTTP SD request), superseded by
  R3-B4.
- S13 resolved: G7 flush-delete before every CRC, copy-CRC-send, `Vary`
  (Phase 5 G7).

**Prebuild review round 3 (2026-10-07, `reviewer` `opus`): NOT READY**
(R3-B4, R3-B3). Each item is followed by its resolution.

- **Refuted: round 2 S12.** No HTTP SD request waits: a sync PUT during a
  fill answers `503 sd_busy` at once, the scheduler starts the next file,
  and the retry lands on that fill too; the mount fill (about 17 s)
  outlasts the 15 s per-file limit. Resolved by R3-B4.
- **R3-B4 (blocker).** Run 1's sync during the mount fill can fail.
  Resolved: G6 fill hold (`Retry-After` from the in-flight file, hold
  `Retry-After` + 1 s, extended 2 s past each HTTP SD job), seams and an
  optical-app refuse-then-retry case with no fill file between (Phase 5
  G6 amended). The refusal half is superseded by G9 (R3-N1, round 4):
  the request cancels the fill instead; the hold stays.
- **R3-B3 (4g part).** Resolved: 1,000 B on the canonical image (4e
  §Memory), memory proof line.
- **R3-S1 (4g part).** Resolved: runtime-target in the green list, owned
  by parent-wiring.
- **R3-S5 (4g part).** The send loop's yield point is unchecked by
  isolation. Resolved: producer-isolation in the green list and
  parent-wiring paths; the send loop joins the yield-root table.
- **R3-S8 (4g part).** The fill was never admitted in FIFO mode. Resolved:
  G8, the fill start goes through `sd_fifo_defer()`; optical-cutover in
  the green list and parent-wiring paths.
- **Phase 5 G3 (missing).** Resolved: G3 listed in Phase 5. **G4
  (wrong alternative).** Resolved: Phase 5 G4 names flash embedding or P2.
- **R3-N1 (found by the round 3 revision; resolved in round 4).** A
  browser page load that misses the cache during a fill gets `503
  sd_busy` from `dispatch_sd`; pages do not retry the way the sync tool
  does, and no item covers it. Resolved: serve-from-card precedence, G9
  (see round 4).

**Prebuild review round 4 (2026-10-07, `reviewer` `opus`, closure
review): NOT READY** on the R3-N1 contract line only; R3-B4 and R3-B3
confirmed closed. Each item is followed by its resolution.

- **R3-N1 adjudicated: serve from the card first.** A page request that
  misses the cache while a fill file is in flight cancels the fill at its
  next primitive boundary (cache only), starts the download job, sets the
  fill hold and re-queues the file. Resolved: G9 and the rewritten G6
  (every HTTP SD request, uploads included, takes the same path, so no
  fill-caused `503 sd_busy` remains), outcome, the dispatch, scheduler
  and `lib/aius_sd` seams, sd and optical-app cases, run 1 adds a page
  load during the mount fill (Phase 5 G6 corrected, G9).
- **R4-S3 (4g part).** 4h's freeze could not stop a fill file. Resolved:
  `web_cache_fill_cancel()` is the one cancel entry 4h's flush start and
  freeze call (G9; 4h H9).

**Prebuild review round 5 (2026-10-07, `reviewer` `opus`, closure
review): READY** with one should-fix item. R3-N1 and R4-S3 confirmed
closed. Each item is followed by its resolution.

- **R5-S1 (G9, and 4h's cancel callers).** The fill hold expired only on
  an HTTP SD job completion, but 4h's flush start and H9 freeze cancel a
  fill with no HTTP job after it: an activation abandoned with
  `ota_sd_busy` after the freeze cancelled a fill left the hold set for
  good, the refill never resumed and every page load missed the cache.
  Resolved: a cancel latches the hold with no expiry and the canceller's
  end (HTTP job completion, flush end, unfreeze) releases it to 2 s
  later through `web_cache_fill_hold_release()`; outcome, scheduler
  seam, G6, G9 and an optical-app case here, the caller cases in 4h
  (Phase 5 G9 amended).
- **R4-N2 (4g part, from 4e).** The 17 s mount fill rested on an
  inferred epoch period. Resolved: stated at 4e's 115 epochs/s
  source-off and re-derived from 4e run 2's playback rate; G6 and run 1
  (Phase 5 E13).

**Clean-room review (2026-10-07, `reviewer` `opus`): NOT READY** only
via 4f's CR-B1; no blocker of its own. Each item is followed by its
resolution.

- **CR-B1 (4g part, from 4f).** With no source, run 1's page load during
  the mount fill got `503 audio_busy` (the browser does not retry) and
  the sync gave up after 15 s per file, since the empty ring read as a
  short margin. Resolved in 4f (F11, `live_owns_ring()`); here G8 states
  the source-off behaviour and run 1 asserts `http_shed` and
  `sd_fifo_defers` delta 0 with the gate closed.
- **CR-S6.** "Read-back CRC before publish" allowed a whole-file CRC at
  publish with no yield in the SD checkpoint. Resolved: G10, per-chunk
  read-back inside each fill primitive, publish with no per-byte work;
  scheduler seam and a web-cache case (Phase 5 G10).

**Clean-room round 3 (2026-10-07): HOLD** only via 4e's CR3-B1 to B3
(producer-isolation and stack-bound in the green list); CR3-S3 is shared
with 4h. Each item is followed by its resolution.

- **CR3-B1 to B3 (4g part, from 4e).** Resolved in 4e (§Walk
  specification): the send loop is a yield-root row (W2), and its 512 B
  copy buffer is charged on the stack-bound path as before.
- **CR3-S3 (4g part).** G9's serve-from-card precedence cancels fills
  only, so an HTTP SD request arriving while a 4h flush owns the volume
  still answers `503 sd_busy`, which a browser does not retry; on an
  8 MiB fitment (no cache) every page load is a card read. Resolved as
  a residual risk in 4h, which owns the flush; G9 is
  unchanged.

**Clean-room round 3 closure (2026-10-07): HOLD** only via 4e's
producer-isolation suite in the green list (CR4-B1, CR4-B3). Each item
is followed by its resolution.

- **CR4-B1, CR4-B3 (4g part, from 4e).** Resolved in 4e (W5 (2)
  forward, link half split by definition kind); nothing else changes
  here.
- **CR4-S4 (4g part).** Resolved by 4e W7: 4g re-runs the census on
  its own link and records its send-loop yield-root row and any changed
  site counts in its own commit.
- **CR3-S3 follow-on (4g part).** The flush exposure behind CR3-S3 is
  restated in 4h (H16 corrected: 2.3 s or more, not 1.3 s); G9 is
  unchanged.

**Clean-room round 3 second closure (2026-10-07): READY.** No 4g
item. No further clean-room round: the closing reviewer was itself
clean-room, and implementation carries a delivery audit per phase.

- **CR5-S1 to S4 (4g part, from 4e).** Resolved in 4e (link half
  include check, libc row figures, `SHF_ALLOC` relocations, stored
  volume serial); nothing changes here.

### Phase 4h Write-behind settings flush, card-swap hold, copy-before-write saves

```yaml
phase: 4h
title: Settings writes complete into PSRAM and reach the card within seconds
status: planned
depends-on: phase-4g
prebuild-review: ready # copy-before-write replaces the journal per owner 2026-10-07; focused review NOT READY (FR-B1, FR-S1 to S13) folded; rename-gap scope answered by the owner (settings only); closure READY, FR2-S1 to S3 folded
outcome: >-
  Every write during live input (DSP Apply, public config, preset save,
  import and delete, card-sync uploads) answers once its bytes are in the
  PSRAM store, is served from there at once, and reaches the card through
  a background flush, its card waits running the producer (4e). A
  settings file (`/aius/dsp.json`, `/aius/config.json`, a preset) flushes
  ahead of queued sync uploads and is verified on the card within 5 s of
  its accept when no sync file is mid-flush, no other settings file is
  queued ahead of it, and the card runs in DMA mode
  (in FIFO mode a flush waits until live input stops owning the ring,
  4f F9, F11); a
  sync burst drains in the
  background, recorded but unbounded. Apply is heard at accept; the latest
  write per path wins, a DELETE is a tombstone, each flushed file is read
  back from the card (SdFat's sector cache dropped first) and
  CRC-checked, and one USAGE_REFRESH follows a flush burst. A card reload
  (reinsert or remount) never overrides a path the store holds unflushed.
  Pending writes are held across a card swap and written only to the card
  they were accepted for, until it returns or the owner discards them;
  OTA activation, rehearsal and reboot answer 409 until the queue is
  drained, re-checked before the SD-writes freeze and again immediately
  before the one-way step, so a write accepted at any point before it is
  never lost across the reset; and a settings save writes two full
  copies, target and fixed backup, each overwritten in place, writing
  first whichever of the two is not the card's only known-valid copy,
  on FAT and exFAT alike, so a power cut anywhere inside a save, a
  restore's flush or a retry leaves a target or a backup that passes its
  validator, and the next mount restores `/aius/dsp.json` or
  `/aius/config.json` from a valid backup when the target fails its own
  (H4, H10).
unit: component-behind-interface
seams:
  - "firmware/lib/aius_sd_overlay (new, portable C): put (latest wins, versioned), tombstone, lookup for GET and listings, shadows(path) for the reload skip, flush scheduler (500 ms debounce, settings class (/aius/dsp.json, /aius/config.json, /www/presets/) before sync class, oldest first within a class, one file at a time), read-back verify, volume hold and discard, drain query (true only when nothing is dirty, flushing, held or tombstoned, FR-S8), status; remount and volume-change invalidation and the post-mount refill act on clean entries only, so a dirty, flushing, held or tombstoned entry is never dropped or overwritten from a card file (H11); a settings-class entry flushes as one copy-before-write job with its fixed backup path (H4: /aius/.bak-dsp.json, /aius/.bak-config.json, /aius/.bak-preset-<name>.json; a preset name whose backup path would not fit AIUS_SD_PATH_STORAGE is refused at accept as an over-long target path is); each settings entry carries its safe copy (target or backup: the file the next job writes last, FR-B1): an accepted entry starts at target, a restore-created entry at backup; a put on an existing entry keeps its safe copy (FR2-S1); a pass whose CLOSE succeeds makes its file the safe copy, a passing read-back makes it target, a failed read-back makes it backup, a job failing mid-pass leaves it unchanged; a restore-created entry whose version is unchanged flushes target-only (its backup already holds those bytes); a flush pins its version at job start, both passes and the CRC use that version's buffer, which is released only at job end, and a put landing mid-job makes version N+1 and leaves the entry dirty (FR-S4); the input callback serves the pinned bytes at the NEED_INPUT offset (FR-S3); a settings tombstone flushes as two DELETE jobs, the backup's first, so a cut between them leaves the target and no backup, or as the target's DELETE alone when the backup path cannot be formed (a name predating 4h or written with a card reader, FR-S9); aius_sd_overlay_restore_decision(target_valid, backup_result) returns KEEP, RESTORE, FAULT or NOTHING (H10, FR-S5); sync-class entries keep today's REPLACE (owner scope, 2026-10-07)"
  - "firmware/lib/aius_sd: a copy-before-write install mode (H4): the request names the backup path, the order (backup first, or target first when the safe copy is the backup) and whether the backup pass is skipped; the directory walk covers the paths the job writes, one EXISTS per path per directory as today's walk does (prepare_next_directory, aius_sd.c:696-724, does not dedupe; FR-S2); each pass overwrites its file in place, for a file of N > 1 sectors: OPEN_WRITE_BEGIN (O_RDWR | O_CREAT, position 0), a WRITE of a zero sector 0 the job fills itself (no NEED_INPUT), one WRITE per 512 B for sectors 1 to N - 1, TRUNCATE to the new length (frees any old tail and syncs), SEEK 0, the WRITE of the real sector 0, CLOSE, which syncs: N + 5 primitives; a one-sector file: OPEN_WRITE_BEGIN, WRITE, TRUNCATE to the length, CLOSE: 4 primitives (FR-S1); then one CACHE_DROP and the read-back of the target (OPEN_READ, FILE_SIZE, one READ per 512 B, CLOSE) CRC-checked against the bytes written; no temp file, no RENAME and no REMOVE in this mode; AIUS_SD_INSTALL_REPLACE and its two-rename install stay unchanged for the sync class and direct uploads; the BEGIN that opens every job carries the volume identity on its OK event (H3), which the payload rule (aius_sd.h:139-143) admits on BEGIN OK only, as it admits a filesystem on MEDIA_PRESENT only"
  - "firmware/src/aius_sd_teensy.cpp aius_sd_teensy_execute BEGIN: the OK event on both the reused-mount return (aius_sd_teensy.cpp:207-212) and a successful mount (:216-222) carries the volume identity (the CID SdioCard keeps from mount, which readCID returns with no card command, plus 4e's FAT volume-serial accessor, which returns the serial the vendored FatPartition stored from the boot sector at mount, zero on a non-FAT volume, so the reused-mount return adds no card read; H3, CR5-S4), still one primitive per case. One new adapter case, one primitive run inside the windowed storage execute like every other: AIUS_SD_ACTION_CACHE_DROP (cacheClear() of whichever partition FsVolume mounted, FatPartition.h:95 or ExFatPartition.h:78 in the SdFat 2.1.2 the vendor snapshot copies, which syncs a dirty cached sector then drops it; issued before the read-back, H1, CR4-B2). TRUNCATE truncates to the action's offset (file.truncate(offset), which seeks there, frees the tail and syncs, FatFile.h:896-898 and FatFile.cpp:1288-1326; today's case passes 0u, aius_sd_teensy.cpp:313-324, and REPLACE's request carries offset 0, so its behaviour is unchanged; truncate_payload admits an offset as seek_payload does, FR-S1). BEGIN no longer carries a volume type: only the journal read it (H14 overturned and replaced by the filesystem-independence item)"
  - "firmware/src/optical_main.cpp: app_store_dsp_config and app_store_public_config accept into the overlay (g_dsp_apply_pending and g_staged_generation at accept); platform_publish_config acknowledges without applying any path the overlay shadows (B6); 4g's remount and volume-change invalidation and refill routed through the overlay's clean-only rule (H11); the flush job in the SD checkpoint, started through 4f's sd_fifo_defer() and, when a 4g fill file is in flight, after web_cache_fill_cancel() stops it at its next primitive boundary (4g G9), calling web_cache_fill_hold_release() when that flush job ends (R5-S1); sd_settle_http_write no longer arms Apply; volume identity read from each BEGIN OK event, never from SD. or the card directly (the cache drop and identity read are adapter primitives, H1, H3, CR4-B2); discard queues SD_STEP_LOAD_CONFIG for the inserted card; coalesced USAGE_REFRESH; the drain gate at the routes and again in service_ota_activation, service_ota_rehearsal and the g_reboot_requested service, each as the first step after the HTTP drain wait and before any one-way step (the recorder park, optical_main.cpp:9921 and :10077; the reset in service_reboot_request, :9850-9851), latching an SD-writes freeze (no SD job starts: an HTTP SD request answers 503 sd_busy; an in-flight 4g fill file is cancelled through web_cache_fill_cancel()) and waiting for any other in-flight SD job under the freeze's own ceiling, AIUS_SD_FREEZE_MS 2,000 ms from the latch (not the 500 ms AIUS_REBOOT_DRAIN_MS armed at the route's epoch, :9794, :9895); then, in the same call, the drain query again immediately before aius_recorder_bank_park (before reset_application in the reboot service), abandoning and unfreezing if a write was accepted during the wait (R4-B1); every unfreeze calls web_cache_fill_hold_release() (R5-S1); an abandoned activation calls ota_activation_recover() (optical_main.cpp:9885-9890), an abandoned rehearsal calls ota_rehearsal_finish() (:10006-10011) with a new distinct OTA_REHEARSAL_ABANDONED result (the OtaRehearsalResult enum, :4445-4449, and ota_rehearsal_result_name, :4749), so each clears its request and never re-runs, and an abandoned reboot clears g_reboot_requested (R5-S2); an activation or rehearsal abandoned on the drain sets OTA_ACTIVATION_FAULT_FLUSH_PENDING (activation_fault ota_flush_pending, beside the existing names at :4737-4746), one abandoned on the freeze ceiling sets OTA_ACTIVATION_FAULT_SD_BUSY (ota_sd_busy); a reboot whose freeze ceiling expires resets as today's ceiling does (the drain already passed, so no settings write is in flight); the accept and flush yield points in the producer-isolation yield-root table; the sd_flush status object (also in /api/status/brief); the mount restore (H10): platform_publish_config records each load's parse verdict, since a refused parse still completes the job (optical_main.cpp:8194-8209); a SD_STEP_LOAD_CONFIG or SD_STEP_LOAD_DSP that fails (open, size or read fault, or the parser refuses the bytes) on a path the overlay does not shadow is followed by a new SD_STEP_RESTORE_CONFIG or SD_STEP_RESTORE_DSP, a CONFIG_LOAD of that path's backup whose platform_publish_config arm parses with its target's parser (the parser is picked from g_sd_step, :8194, :8200) into a local, checks the overlay's shadow on the target's path, and on a pass applies the bytes as the target's load would and puts them into the overlay as a restore-created accept for the target path (safe copy backup, so its flush writes the target only, FR-B1); the step handler acts on aius_sd_overlay_restore_decision; g_config_error_count advances once for the failed config.json target, as today (:8075 or :8197), and neither the restore nor a failed backup load advances it again (FR-S11); sd_diag settings_restores and settings_restore_faults"
  - "firmware/src/aius_runtime_storage.cpp (FR-S3): a root input source registered beside the publish consumer; route_action's NEED_INPUT arm, for a root job (!authoritative, root_state RUNNING, aius_runtime_storage.cpp:239-241 returns today), calls it for the action's offset and length, passes the bytes to aius_sd_job_supply and steps the job once more in the same advance, which then emits the WRITE, so NEED_INPUT costs no epoch and no primitive and each advance still spends at most one primitive; the flush job's source is the overlay's pinned version, read once per pass"
  - "firmware/src/aius_optical_app.cpp dispatch_store, dispatch_sd_upload and sd_delete: 200 at accept, 503 sd_busy only when staging is full; POST /api/sd/pending/discard (authed); 409 sd_flush_pending on OTA activate, OTA rehearse and POST /api/reboot"
  - "firmware/lib/aius_http (route), firmware/sd/www/openapi.json, firmware/sd/www/dsp-editor.js (pending versus saved from sd_flush), firmware/tools/ota/aius_ota_flash.py (waits out 409 sd_flush_pending up to 30 s; after activate, reports activation_fault ota_flush_pending or ota_sd_busy as an abandoned activation, not a success)"
targeted-proof:
  - "host: sd-overlay (new): latest wins per path; a tombstone hides a file from GET and listings until flushed; a flush of version N with N+1 accepted flushes N+1 next; a settings entry flushes before an older sync entry; a read-back CRC mismatch retries, then counts a failure and keeps the entry dirty; a volume mismatch holds every entry and writes nothing; discard drops them; staging full answers busy; shadows(path) is true for dirty, flushing, held and tombstoned entries only; the drain query is true only when nothing is dirty, flushing, held or tombstoned (an unflushed preset delete keeps it false, FR-S8); one USAGE_REFRESH per burst; POST /api/clear resets max_settings_latency_ms and sync_drain_ms; a preset PUT to /www/presets/<name>.json is settings class; a remount or volume change with a held dsp.json and a held preset drops and refills only clean entries, and a refill reading an older card file never replaces a dirty, flushing, held or tombstoned entry; a settings entry flushes as one copy-before-write job naming its fixed backup path (/aius/.bak-dsp.json, /aius/.bak-config.json, /aius/.bak-preset-<name>.json), a sync entry as today's REPLACE, and a settings tombstone as a DELETE of the backup and then of the target, or the target's alone when the backup path cannot be formed (FR-S9); a preset name whose backup path would not fit is refused at accept; safe-copy order (FR-B1): an accepted entry's job is backup first, a restore-created entry's is target-only, a job failing after its first pass's CLOSE or at a failed read-back makes the retry write the other order, a card pull mid target pass held and returned makes the flush target first; a put before the flush on a restore-created entry, and on an entry held after a failure in its target pass, keeps the safe copy, so that flush writes the target first (FR2-S1); version pin (FR-S4): a put landing mid-pass and one landing between the two passes leave both passes and the CRC on version N and the entry dirty with N+1; job-request cases (mode, order, skip-backup fields) are written after copy-before-write reports (FR-S7); aius_sd_overlay_restore_decision (H10, FR-S5, FR-S10): target valid gives KEEP (no backup read); target not valid with a backup that parses gives RESTORE; target not valid with a backup whose bytes reached the parser and were refused gives FAULT; target not valid with a backup load that failed before its bytes reached the parser (missing, empty, oversized, read fault) gives NOTHING; power-cut model (FR-S6; lib/aius_sd driving sd-overlay's own stateful fake card, firmware/test/sd-overlay/fake_card.c, with the dsp.json and config.json parsers; the card keeps files as sectors plus a size, lands a full-sector WRITE at once, and lands a partial last sector, the size and any allocation at the next sync, data before the directory entry, FatFile.cpp:1406-1458): starting from (target valid, backup valid), (target missing, backup missing), and (target torn, backup valid), a cut before and after every primitive of a save in each order, of a restore's target-only flush and of a retry after a failure in each pass, then the mount load and restore: after every cut the target or the backup passes its validator (from a missing pair: the target is missing or new), every file that passes holds a complete saved version, never a mix of two (the zero sector 0 makes a partly overwritten file fail, FR-S1), a restore installs the newest complete save on the card, and no case renames or removes a file"
  - "host: sd: the copy-before-write mode issues, in order, BEGIN, the directory EXISTS walk (two EXISTS /aius for a full save, one for a target-only save, FR-S2), the first file's pass (OPEN_WRITE_BEGIN, the zero sector 0 WRITE, the WRITEs of sectors 1 to N - 1, TRUNCATE carrying the length, SEEK 0, the sector 0 WRITE, CLOSE), the second file's pass, then exactly one CACHE_DROP and the target's read-back (OPEN_READ, FILE_SIZE, READs, CLOSE), and never a RENAME, a REMOVE or a temp path; backup first and target first issue the same primitives with the passes swapped; a full save of an N-sector file (N > 1) whose paths share one directory takes exactly 3N + 17 primitives, 89 for a 12,288 B dsp.json, the CACHE_DROP included, a one-sector file 16, and a target-only save 2N + 11, 59 for dsp.json (H4); the zero sector 0 WRITE asks for no input; a fault at any primitive fails the job with no RENAME or REMOVE issued; a read-back CRC mismatch fails the job; AIUS_SD_INSTALL_REPLACE's primitive sequence is unchanged; a BEGIN OK event carrying the identity fields is accepted, and any other event carrying them faults AIUS_SD_FAULT_PROTOCOL (CR4-B2, CR5-S4)"
  - "host: sd-target: BEGIN's OK event carries the fake CID and volume serial (serial zero on a non-FAT fake) on the reused-mount and the fresh-mount paths, the reused-mount BEGIN's payload adds no card call and each case stays one primitive (test_source_guard.py and test_aius_sd_teensy.cpp updated; CR5-S4); CACHE_DROP calls the mounted partition's cacheClear once and nothing else, on a FAT and on an exFAT fake, and counts driver_faults only on a nonzero sdErrorCode (CR4-B2); TRUNCATE carrying offset L calls the file's truncate(L) once, and offset 0 still truncates to 0 (FR-S1)"
  - "host: runtime-target (FR-S3): a root copy-before-write job of a 24-sector file against the fakes takes 90 epochs (89 primitives plus the terminal COMPLETE advance), NEED_INPUT resolved by the root input source in the same advance, at most one primitive per advance, and both passes supplied from the same buffer; a root job with no input source registered fails at its first NEED_INPUT instead of stalling"
  - "host: optical-cutover (CR4-B2 part): optical_main.cpp names no cacheClear, readCID or volume-serial call (4e's SD-naming guard still holds with no new W2 row)"
  - "host: optical-app: PUT /api/dsp/config answers 200 at accept and arms Apply; GET /sd/aius/dsp.json returns the accepted bytes before the flush; GET /api/dsp/config (serialised from the model, write_dsp_config) returns the new model once config_generation advances; a preset delete leaves the manifest listing at once; discard needs the token; OTA activate, OTA rehearse and POST /api/reboot answer 409 sd_flush_pending while anything is dirty, flushing, held or tombstoned (a preset delete accepted and not yet flushed included, FR-S8); a PUT accepted after the route check makes service_ota_activation, service_ota_rehearsal and the reboot service refuse without resetting, before the recorder park (the recorder stays live) and with activation_fault ota_flush_pending set; once the first re-check passes, no SD job starts (an HTTP SD request answers 503 sd_busy), an in-flight fill file is cancelled at its next primitive and the direct-SD copy waits for any other in-flight SD job; a PUT /api/dsp/config accepted 200 during that wait makes the final re-check fail, so each service abandons before aius_recorder_bank_park (before the reset in the reboot service), unfreezes, sets ota_flush_pending, and the PUT then flushes and reads back from the card (R4-B1); an in-flight SD job outlasting AIUS_SD_FREEZE_MS abandons activation and rehearsal with ota_sd_busy and the recorder live, while the reboot service resets; an in-flight 4g fill file no longer abandons the activation (R4-S3); an activation and a rehearsal abandoned on the drain and on the freeze ceiling each leave their request cleared (the rehearsal's result OTA_REHEARSAL_ABANDONED), and with the service then called every epoch until the flush drains and after, neither re-runs, parks the recorder or resets (R5-S2); a flush start that cancels a fill file lets the next fill file start 2 s after that flush job ends with no HTTP SD job after it, and a freeze that cancels a fill and then abandons with ota_sd_busy lets it start 2 s after the unfreeze (R5-S1)"
  - "host: optical-app (mount restore, H10): with /aius/dsp.json truncated mid-file and a valid backup, the mount's load fails, the SD_STEP_RESTORE_DSP load applies its model (config_generation advances once) and the flush rewrites the target only, settings_restores 1; the same with /aius/config.json restores the public config instead of the defaults, with g_config_error_count advanced once (FR-S11); a target invalid with a backup that reaches the parser and is refused keeps both files, leaves the active model (dsp.json) or the defaults with g_config_error_count advanced once (config.json), and counts settings_restore_faults; a target invalid with no backup file counts nothing (FR-S10); a fresh card with neither file counts nothing; a path the overlay shadows is neither loaded nor restored, the restore step included"
  - "host: optical-cutover: platform_publish_config consults the overlay before writing g_config or g_pending_dsp_model; a shadowed path is acknowledged and not applied; the flush job start consults sd_fifo_defer() and calls web_cache_fill_cancel() when a fill file is in flight; in both OTA services the first drain re-check precedes the freeze latch and a second drain query is the statement immediately before aius_recorder_bank_park, with no epoch return between them; in the reboot service the second drain query immediately precedes tx_gate_actuate and reset_application (R4-B1)"
  - "host: ota (test_ota_flash_client.py: the 409 wait, and activation_fault ota_flush_pending and ota_sd_busy each reported as abandoned), http, status-body-bound (sd_flush; sd_diag settings_restores and settings_restore_faults), sd-editor (dsp-editor pending and saved states; openapi), stack-bound (accept and flush roots), producer-isolation (accept and flush roots in the yield-root table), runtime-target (aius_optical_app.cpp and aius_runtime_storage.cpp against the fakes), memory (OCRAM free unchanged; on the canonical image ITCM growth at most 1,000 B, DTCM statics growth at most 256 B), registry and itcm-placement green; both images build"
  - "device (owner, AC-3, sd_mode DMA asserted at start and end): one full sync with one Apply during it (its latency recorded, not bounded); once sd_flush.dirty is 0, POST /api/clear, then three Applies, two preset saves and one preset delete, each made only once sd_flush.dirty reads 0 after the previous one, so each is measured with no settings file queued ahead (H8, CR-S10): underrun_episode_count delta 0 throughout, sd_flush.max_settings_latency_ms at most 5,000, sd_flush.sync_drain_ms recorded, sd_flush.failures 0; once drained, and again after a reboot, GET /sd/aius/dsp.json and every saved preset (GET /sd/www/presets/...) equal their PUT bodies and /api/status shows the PUT public config (config.json has no read route)"
  - "device (owner's hands, source off, /api/status tx_gate.open 0): every Apply, save and discard below answered 200 at its first attempt, http_shed and sd_fifo_defers delta 0 (4f F11, CR-B1); card pulled, an Apply and a preset saved (held, sd_flush.held true); the same card reinserted: GET /api/dsp/config still the Apply's model and config_generation not advanced by the reload, the held preset (GET /sd/www/presets/...) still the saved body after the post-mount refill, then flushed and GET /sd/aius/dsp.json and the preset equal to their PUT bodies; sd_diag absent_mounts recorded and fault deltas taken outside the card-out intervals; pulled again, an Apply and a preset saved, a different card inserted: nothing written to it and the running model still the Apply's; discard: dropped, the inserted card's dsp.json applied and its files serve"
contract-artifact: spec-only
fan-out: # sd-overlay, copy-before-write, ota-tool and ui-pending concurrently; parent-wiring after sd-overlay's header lands; sd-overlay's job-request cases and power-cut model cases after copy-before-write reports (FR-S7)
  - name: sd-overlay
    owned-paths:
      - firmware/lib/aius_sd_overlay/**
      - firmware/test/sd-overlay/** # FR-S6: sd-overlay builds the stateful fake card here (fake_card.c); the power-cut model links lib/aius_sd and drives it; firmware/test/sd is a scripted event oracle with no card state
    handoff: parent-wiring adds the sd-overlay arm to firmware/scripts/test.sh and its registry entry, which it owns (FR-S6)
  - name: copy-before-write
    owned-paths:
      - firmware/lib/aius_sd/**
      - firmware/test/sd/**
      - firmware/src/aius_sd_teensy.cpp # CR4-B2: BEGIN's identity payload and the CACHE_DROP case
      - firmware/src/aius_sd_teensy.h # CR4-B2: only if the new case needs a declaration
      - firmware/test/sd-target/**
      - firmware/test/fake-fidelity/** # CR4-B2: the fakes' new methods checked against the vendored headers
    handoff: once copy-before-write reports, parent-wiring adds the methods the new adapter case and BEGIN payload call (cacheClear on the FAT and exFAT partitions, the CID and serial accessors) to firmware/test/runtime-target/fake/SD.h, which it owns (CR4-B2)
  - name: ota-tool
    owned-paths:
      - firmware/tools/ota/aius_ota_flash.py
      - firmware/test/ota/test_ota_flash_client.py
  - name: ui-pending
    owned-paths:
      - firmware/sd/www/dsp-editor.js
      - test/sd-editor/dsp-editor.test.js
      - test/sd-editor/fixtures/** # CR-S11: the sd_flush brief fixtures; parent-wiring's openapi.test.js only reads them
  - name: parent-wiring # store accept, reload skip, flush job, root input source, restore steps, volume identity, drain gates, discard route, status keys, the sd-overlay test.sh arm
    owned-paths:
      - firmware/src/optical_main.cpp
      - firmware/src/aius_optical_app.cpp
      - firmware/src/aius_optical_app.h
      - firmware/src/aius_runtime_storage.cpp
      - firmware/src/aius_runtime_storage.h
      - firmware/lib/aius_http/**
      - firmware/test/http/**
      - firmware/sd/www/openapi.json
      - test/sd-editor/openapi.test.js
      - firmware/test/optical-app/**
      - firmware/test/optical-cutover/**
      - firmware/test/status-body-bound/**
      - firmware/test/stack-bound/**
      - firmware/test/producer-isolation/**
      - firmware/test/runtime-target/**
      - firmware/test/memory/**
      - firmware/test/registry/**
      - firmware/scripts/test.sh
      - firmware/platformio.ini
      - firmware/README.md
review-intensity: active
commit-checkpoint:
  green: [sd-overlay, sd, sd-target, fake-fidelity, optical-app, optical-cutover, ota, http, status-body-bound, sd-editor, stack-bound, producer-isolation, runtime-target, memory, registry, itcm-placement]
  buildable: [teensy41-optical, teensy41-optical-slot-a]
  parent-commit: "feat(aius): write-behind settings flush with card-swap hold and copy-before-write saves (Phase 4h)"
  coordination: owned-path sets are disjoint (aius_sd_teensy.cpp and .h, sd-target and fake-fidelity are copy-before-write's alone; runtime-target is parent-wiring's, edited for the new adapter case only after copy-before-write's report; 4h no longer edits firmware/vendor/SdFat/** or firmware/test/sdfat-vendor/**, which stay 4e's)
```

- **Rename-gap scope (owner, 2026-10-07): settings files only.**
  Copy-before-write covers `/aius/dsp.json`, `/aius/config.json` and
  presets; card-sync and direct uploads keep today's two-rename REPLACE,
  so a cut between a sync upload's renames can leave that
  `firmware/sd/www/` file missing until the next sync, which restores
  it. This narrows 4b's "fix the rename gap" to the settings class.

**Owner overturn (2026-10-07).** The install journal, RECOVER and the
FAT-only gate are withdrawn: H4, H10 and H14 overturned, H15 withdrawn
with them. Owner: "journal sounds over complicated for a rare edge case
with low stakes if lost, filesystem independent backup sounds adequate,
is this a copy before write scenario?" A settings save now writes a
full backup copy, then rewrites the target in place, and the next mount
restores a target that fails its validator from a backup that passes
(H4, H10); a power cut may lose the save in progress. No rename runs
and no file that could share a cluster chain is removed, so round 2
S17's rename-specific cross-link cannot arise; the general SdFat sync
exposure stays, narrowed by the in-place overwrite (residual risks,
FR-S1). Nothing rests on FAT rename semantics, so FAT and exFAT behave
alike (H14). Removed with the
journal: the journal file and its records, RECOVER, the vendored SdFat
unlink-entry patch and its `PATCHED_SHA256` update, the UNLINK_ENTRY
and FIRST_CLUSTER adapter cases, BEGIN's volume-type payload and the
job-local type rule, `sd_diag` `recover_faults` and `recover_aside`,
and the first device step's `sd_filesystem` record. Kept: CACHE_DROP
(the read-back still needs it) and BEGIN's identity payload (the CID
and 4e's FAT serial accessor, H3). The review items below on the
journal, RECOVER, the unlink patch or the FAT-only gate (round 2 S17,
R3-B6, R3-S11, R3-N2, R4-S4 to S6, R4-N3, CR-S4, CR-S5, CR2-S1 and
CR4-B2's UNLINK_ENTRY and FIRST_CLUSTER) are history.

**Owner confirmation (2026-10-07).** Confirmed: H1 to H3, H7 to H9,
H12, H13, H16 and H17. With Phase 5 gone, H12 and H13 are stated in H9,
H16 in the residual risks and H17 in H1 and H3. Overturned and
replaced: H4, H10 and H14 (above).

**Carried in from 4g (found during its build):**

- **The card-swap hold narrows to accidental disconnect (owner,
  2026-10-10).** A card swap while powered is unsupported (4g §Build
  decisions). Keep only what protects data on an accidental disconnect
  (PSRAM-held writes retained and flushed after remount); drop the
  deliberate-swap handling. Revise this contract and its proof lines
  accordingly, then take one focused prebuild review before dispatch.
- **Memory budgets start from 4g's link.** `firmware/test/memory`
  enforces slot-a ITCM free at least 11,900 B (4g used none, so 4h's
  1,000 B is whole), `_ebss` at most 0x20049A00 (4g's 0x20049900 plus
  4h's 256 B) and `_VectorsRam` at 0x20007c00; OCRAM sits exactly at
  every env's floor, so any OCRAM growth needs reuse, as 4g did.
- **`aius_ab` is missing from `EXPECTED_LDF_LIBS`**
  (`firmware/test/itcm-placement/verify_target.py`); it is LDF-resolved
  in both optical envs. Add it with a case.
- **Sync `--file` needs every page bundleable** (4g closure nit B).
  `aius-sd-sync.sh --file x.js --www <tree>` runs `www_bundle.py
  --inlined` unconditionally, so it exits 1 after uploading the file
  when any of the five pages or their assets is missing from the tree.
  Skip the bundle step when no selected file is a page or an inlined
  asset; this phase's card-sync uploads touch the same branch.
- **The `<!--` escape breaks a `u`-flag regex literal** (4g closure nit
  C, `firmware/tools/www_bundle.py:83`). `<\!--` is a SyntaxError inside
  `/.../u`; no current asset contains `<!--`. Escape it as `<\x21--`
  inside regex literals, or make the bundler fail loudly when a script
  contains `<!--`.
- **`Accept-Encoding: *` gets the plain representation** (4g audit nit
  13, `firmware/lib/aius_http/src/aius_http.c:399`). RFC 9110 lets `*`
  match gzip; harmless today because browsers name gzip.
- **Cache keys are case-sensitive, FAT names are not** (4g clean-room
  closure nit; `firmware/lib/aius_web_cache/src/aius_web_cache.c:179-184`,
  `firmware/src/optical_main.cpp:1504`). A raw `PUT /sd/www/INDEX.HTML`
  overwrites `index.html` but invalidates only `INDEX.HTML`, so the old
  entry keeps serving, and an equal-length overwrite during a resumable
  fill could publish mixed bytes. The editor and the sync tool use exact
  names. Case-fold the invalidate compare, or reject case-variant www
  PUTs; this phase's PSRAM store takes every upload, so fix it here.

Implements the owner's write-behind (flushing within seconds during
playback; a power cut may lose the last few seconds), Apply at PSRAM
accept, the card-swap hold, copy-before-write saves and the OTA drain, on top
of 4g's store: an entry is clean (mirrors the card), dirty, flushing or a
tombstone, with the same per-chunk CRC.

- **H1, timing.** A 500 ms debounce after the last accept to a path (a
  burst of Applies writes once), flush start within 1 s, one file per
  job; `sd_flush` reports `dirty`, `held`, `flushed`, `failures`,
  `last_fault`, `oldest_dirty_age_ms`, `max_settings_latency_ms` (accept
  to verified, settings class) and `sync_drain_ms` (last sync burst, first
  accept to drained); `POST /api/clear` resets the last two. Before the
  read-back the save issues a CACHE_DROP adapter primitive
  (`cacheClear()` of the mounted FAT or exFAT partition: syncs, then
  drops the cached sector),
  so a partial last sector is read from the card, not from SdFat's
  `FsCache`, and the sync's write runs inside the windowed storage
  execute (CR4-B2).
- **H2, sync uploads.** PUT bodies stream into PSRAM staging and are
  pointer-swapped into the entry at the end of the body; staging full
  answers `503 sd_busy` with `Retry-After` (`aius-sd-sync.sh` honours it
  for up to 15 s per file, 4g G6; R3-S12). Replay
  uploads (`.iec`) stay direct-to-card jobs: replay is out of scope and
  they are large.
- **H3, volume identity.** CID (manufacturer, OEM, product serial) plus
  the FAT volume serial (4e's vendor accessor; `FatPartition` keeps no
  serial, so the vendored patch stores it from the boot sector at
  mount, CR5-S4), taken at each mount and carried on the BEGIN OK event
  by the adapter with no card read on a reused mount (CR4-B2), so a
  reformatted card is a different volume. On exFAT the serial accessor
  returns zero, so identity is the CID alone and a reformat of the same
  card to exFAT is not told apart. While held, accepted contents
  keep serving (the board's settings stay the owner's latest); discard
  reverts to the inserted card's files.
- **H4, copy-before-write save (owner overturn 2026-10-07).** A
  settings flush is one job of two passes, one per file, then
  CACHE_DROP and the read-back of the target (H1). A pass overwrites its
  file in place: `OPEN_WRITE_BEGIN` (`O_RDWR | O_CREAT` per the
  framework `SD.h:184`, position 0), a `WRITE` of a zero sector 0,
  one `WRITE` per 512 B for sectors 1 to N - 1, `TRUNCATE` to the new
  length (seeks there, frees any old tail and syncs: `FatFile.h:896-898`,
  `FatFile.cpp:1288-1326`; `ExFatFileWrite.cpp:549-602` likewise),
  `SEEK` 0, the `WRITE` of the real sector 0, and `CLOSE`, which syncs;
  a one-sector file (every `config.json`, at most 256 B) is
  `OPEN_WRITE_BEGIN`, `WRITE`, `TRUNCATE`, `CLOSE`. Each target has one
  fixed backup in `/aius`: `/aius/.bak-dsp.json`,
  `/aius/.bak-config.json`, `/aius/.bak-preset-<name>.json`; outside
  `/www`, so 4g's fill and the editor's preset listing never see them;
  and distinct from today's per-upload `/aius/.backup-<id>-<seq>.bak`
  (`firmware/src/aius_optical_app.cpp:356`), which says nothing about
  its target. A settings delete removes the backup, then the target, so
  a cut between leaves the target and no backup; a tombstone whose
  backup path cannot be formed deletes the target alone (FR-S9).
  **Order (FR-B1).** Each settings entry carries its safe copy, the
  file the job writes last: target for an accepted entry, backup for a
  restore-created one. A pass whose `CLOSE` succeeds makes its file the
  safe copy (it now holds the pinned version complete), a passing
  read-back makes it the target, a failed read-back the backup (both
  passes ran by then), and a job that fails mid-pass leaves it
  unchanged. So a save normally runs backup first; it runs target first
  after a job failed past its first pass's `CLOSE` or at the read-back,
  or after a card pull mid target pass (the held entry keeps its safe
  copy across the swap, and a shadowed path is not reloaded); and a
  restore-created entry whose version is unchanged writes the target
  only, since its backup already holds those bytes. The safe copy lives
  in PSRAM; after a power cut the mount load re-derives it (a target
  that validates is safe, a restore makes the backup safe). Why: the
  journal-free design held only from a valid target; a flush that began
  with the target torn and the backup the only good copy (the restore's
  own flush, a retry, a returned card) zeroed the backup first, and a
  cut there left neither file valid.
  **Version pin (FR-S4).** A flush pins the entry's version at job start;
  both passes and the CRC read that version's buffer, released only at
  job end; a put landing mid-job makes version N + 1 and leaves the
  entry dirty. The root job's bytes come from a root input source in
  `aius_runtime_storage.cpp` that answers each `NEED_INPUT` from the
  pinned buffer within the same advance (FR-S3).
  **Cost.** The walk runs one `EXISTS /aius` per path, as
  `prepare_next_directory` does today (`aius_sd.c:696-724`, no dedupe,
  FR-S2). A full save of an N-sector file is 3N + 17 primitives (BEGIN,
  two EXISTS, two passes of N + 5, CACHE_DROP, an N + 3 read-back): 89
  for a 12,288 B `dsp.json`, 90 epochs with the terminal COMPLETE
  advance and no epoch for `NEED_INPUT`, about 0.78 s at 115 epochs/s
  (4e §Epoch period), off the response path under write-behind; a
  one-sector `config.json` save is 16; a restore's target-only flush is
  2N + 11, 59 for `dsp.json`. With the 500 ms debounce and the 1 s start
  window a `dsp.json` save lands in about 2.3 s, inside H8's 5 s bound;
  the sd and runtime-target suites pin the counts. Under playback nested
  passes lower the epoch rate, so the 0.78 s and the 2.3 s are
  re-derived from 4e run 2's measured playback rate before the 4h device
  run, and the 5 s bound is re-checked against them (R4-N2).
  Autonomous calls in this rewrite, each with its why:
  - **The backup holds the new bytes, not a copy of the card's old
    target.** The same N writes, with no N reads of the old file; no
    dependence on the old target being valid (a torn preset, which has
    no mount restore, would otherwise be copied over its good backup);
    and whenever the target fails, the backup holds the newest complete
    save on the card, so a restore cannot roll a file back, and a cut
    during a backup-first save's target write restores the new save. A
    cut during its backup write leaves the old target, which validates:
    the latest save is lost, as the owner accepted. Alternative: read the card's target into the backup first
    (the orchestrator's first wording); reversing: the backup pass reads
    the target instead of the PSRAM entry, plus a validity check before
    it overwrites a good backup.
  - **Overwrite in place, sector 0 zeroed first and written last
    (FR-S1, orchestrator 2026-10-07).** Truncate-first freed and
    reallocated the file's clusters on every save, so a cut inside a
    `CLOSE` that wrote the directory entry before the FAT sector
    (`FatPartition.h:250-252`) could leave a cluster the next save's
    other pass reallocates; in place, a save allocates or frees a
    cluster only when the file's cluster count changes. The `TRUNCATE`
    gains a length (the action's offset; REPLACE passes 0, so its
    behaviour is unchanged). Why the zero sector: the validators alone
    do not reject every partly overwritten file. Both parsers reject
    trailing bytes (`aius_dsp_config.c:1052-1054`, `aius_config.c:254`),
    so new bytes over a longer old tail fail; but SdFat writes each full
    512 B sector straight to the card (`FatFile.cpp:1451-1458`), so a cut
    after sector k of an equal-length `dsp.json` (up to 24 sectors) can
    leave new sectors before it and old sectors after it, which parses
    whenever the edits on both sides keep their lengths (two EQ gains
    changed, say): a model that was never saved. With sector 0 zeroed
    first (a zero byte fails both parsers at the opening brace and the
    editor's `JSON.parse`), the tail and the length synced by `TRUNCATE`,
    and the real sector 0 written last, a cut leaves the old file, a
    file that fails, or the new file. A one-sector file needs no zero
    sector: its sector lands whole at the sync, before the directory
    entry, and a length mismatch leaves a strict prefix or trailing old
    bytes, which fail. Cost: two primitives per pass (N + 5 against
    N + 3). Alternative: a length and CRC record per file (a sidecar or
    a trailer the parsers skip), which costs a format change or a read
    at every mount; reversing: drop the zero sector and the `SEEK`, and
    validate against the record.
  - **Settings class only, confirmed by the owner (FR-S13).** Sync and direct
    uploads keep today's two-rename REPLACE and its rename gap: their
    source is the host's `firmware/sd/www/`, a re-sync restores them,
    they have no validator to restore against, and doubling a 600 KB
    sync's writes buys little. This narrows the owner's 4b "fix the
    rename gap"; the owner chose it on 2026-10-07 (scope line above).
- **H5, OTA.** Activate and rehearse refuse with `409 sd_flush_pending`
  while anything is dirty, flushing, held or tombstoned (a held entry
  needs the card back or a discard first; an unflushed delete would
  otherwise let the preset return after the reset, FR-S8); the flash
  tool waits up to 30 s for the drain.
  H9 adds the re-check and the reboot gate.
- **H6, no more BUSY.** Settings PUTs no longer refuse on
  `sd_job_active()`; the overlay accepts while a job runs.
- **H7, a card reload never overrides the store (B6).** A reinsert or
  remount runs `SD_STEP_LOAD_CONFIG` and `LOAD_DSP`
  (`firmware/src/optical_main.cpp:8032-8041`), whose
  `platform_publish_config` (`:8188-8223`) overwrites `g_config` and
  stages `g_pending_dsp_model` from the card. It now asks the overlay
  first: a path with a dirty, flushing, held or tombstoned entry is
  acknowledged and not applied, so the running model stays the owner's
  latest accept and the flush lands it. A discard queues
  `SD_STEP_LOAD_CONFIG`, so the inserted card's config and DSP apply then,
  as H3 promises. Why: the alternative (reload, then re-apply the overlay)
  briefly applies the older model, an audible change.
- **H8, settings before sync (S15).** The scheduler has two classes:
  settings (`/aius/dsp.json`, `/aius/config.json`, the presets under
  `/www/presets/`, which the editor writes as `/sd/www/presets/<name>.json`,
  `firmware/sd/www/dsp-editor.js:401`; R3-S9) before sync uploads, oldest
  first within a class. The flush job starts through 4f's
  `sd_fifo_defer()`, so in FIFO mode it waits while live input owns the
  ring and its margin is short (R3-S8); since a FIFO primitive costs more
  than the margin, in FIFO mode during live input a flush in practice
  waits until the gate closes or the carrier drops, and the 5 s bound is
  a DMA-mode bound (4f F9, F11, R4-S2, CR-B1). With no live input the
  predicate passes, so a source-off flush, the card-swap step's included,
  runs at once. A fill file in flight
  never delays a flush: the flush start cancels it through 4g's
  `web_cache_fill_cancel()` (about 3 s for `dsp-editor.js` would
  otherwise break the bound).
  A sync of about 600 KB is thousands of one-per-epoch primitives with
  read-back, so no fixed bound fits it; the 5,000 ms bound applies to
  settings writes accepted with no sync file mid-flush and no other
  settings file queued ahead, and a settings write accepted during a
  sync waits at most the one in-flight file (recorded, not bounded).
  Flushes run one file per job, so a settings write with k other
  settings files queued ahead waits about k save durations more
  (about 0.78 s each at 115 epochs/s, H4, longer under playback); that is
  recorded, not bounded, and the device step spaces its writes so each
  is measured alone (CR-S10). `sync_drain_ms` records the burst.
- **H9, the drain is checked where the reset is taken (S16).** The 409 at
  the route is advisory; `service_ota_activation`,
  `service_ota_rehearsal` and the `g_reboot_requested` service re-check
  the drain query and abandon the reset if it is false (R5-S2: an
  activation through `ota_activation_recover()`,
  `optical_main.cpp:9885-9890`; a rehearsal through
  `ota_rehearsal_finish()`, `:10006-10011`, with a new distinct result,
  `OTA_REHEARSAL_ABANDONED`, since a rehearsal that left
  `g_ota_rehearsal_requested` set would re-enter every epoch and, once
  the flush drained, run and reset the board unasked after its fault was
  reported; a reboot clears `g_reboot_requested`), so a PUT accepted between route and
  service is never lost across the reset. The re-check is the first step
  after the HTTP drain wait, before any one-way step: the quiesce
  (`optical_main.cpp:9983`, `:10126`) comes after the one-way recorder
  park (`:9921`, `:10077`) and the direct `SD.open` copy (`:9943`,
  `:10098`), so a re-check there would leave the recorder parked until
  reboot, and a flush job holding the adapter's file open would share
  the volume with the copy (round 3 refuted S16). Once the re-check
  passes, an SD-writes freeze is latched: no SD job starts (an HTTP SD
  request answers `503 sd_busy`, as when a job owns the volume), an
  in-flight 4g fill file is cancelled at its next primitive (4g G9), and
  the service waits for any other in-flight SD job under the freeze's
  own ceiling, `AIUS_SD_FREEZE_MS` 2,000 ms from the latch (R4-S3; the
  500 ms `AIUS_REBOOT_DRAIN_MS`, `optical_main.cpp:9794`, is armed at the
  route's epoch and has usually expired by now, so a 4g fill in flight
  after a sync abandoned nearly every activation). The wait spans epochs,
  and the HTTP checkpoint keeps accepting PUTs into the overlay meanwhile
  while the frozen scheduler never flushes them (R4-B1). So, in the same
  call that finds the volume idle, the drain query runs again as the
  statement immediately before `aius_recorder_bank_park` (before
  `reset_application` in the reboot service), with no epoch return
  between; if a write arrived, the service abandons and unfreezes, and
  the write flushes. Every unfreeze calls 4g's
  `web_cache_fill_hold_release()`, as the flush job's end does, so a
  fill the freeze or a flush cancelled resumes 2 s later with no HTTP
  job needed (R5-S1). The activation already answered 200, so an abandon
  on the drain sets `OTA_ACTIVATION_FAULT_FLUSH_PENDING`
  (`activation_fault` `ota_flush_pending`, in the `ota_` form of the
  existing names, `optical_main.cpp:4737-4746`) and one on the freeze
  ceiling sets `OTA_ACTIVATION_FAULT_SD_BUSY` (`ota_sd_busy`), and
  `aius_ota_flash.py` reports either as an abandoned activation (R3-S10).
  A reboot whose freeze ceiling expires still runs the final drain query
  and then resets, as `service_reboot_request` honours its ceiling today:
  the drain already passed, so the in-flight job carries no settings
  write. The flash tool waits up to 30 s for the drain before activating
  (H5).
  `POST /api/reboot` gets the same `409 sd_flush_pending` gate. An entry
  whose flush keeps failing or that is held stays non-drained, so the
  owner discards it first (`POST /api/sd/pending/discard`): losing it is
  an explicit act. A power cycle or watchdog reset is not gated.
- **H10, mount restore (owner overturn 2026-10-07).** At each mount the
  existing `SD_STEP_LOAD_CONFIG` and `SD_STEP_LOAD_DSP` loads are the
  validators, so a valid target costs no extra primitive:
  `/aius/config.json` passes when `aius_config_parse()` leaves
  `g_config.valid` set, `/aius/dsp.json` when
  `aius_dsp_config_parse_put()` returns `AIUS_DSP_CONFIG_OK`
  (`firmware/src/optical_main.cpp:8194-8209`), each within its publish
  capacity (a missing, empty or oversized file fails the load,
  `:8064-8076`). When a load fails (missing, empty, oversized, a read
  fault or a parse refusal; `platform_publish_config` records the parse
  verdict, since a refused parse still completes the job) on a path the
  overlay does not shadow (H7), a new step, `SD_STEP_RESTORE_CONFIG` or
  `SD_STEP_RESTORE_DSP`, runs a CONFIG_LOAD of its backup; its
  `platform_publish_config` arm parses with the target's parser into a
  local (the parser is picked from `g_sd_step`, `:8194`, `:8200`) and
  checks the shadow on the target's path (FR-S11). The step acts on the
  portable `aius_sd_overlay_restore_decision(target_valid,
  backup_result)` (FR-S5), so the host suite tests the rule
  `optical_main.cpp` runs: target valid, KEEP (no backup read); a backup
  that parses, RESTORE: the bytes apply as the target's load would and
  enter the overlay as a restore-created accept for the target path, so
  the flush writes the target only and reads back (H4), and
  `sd_diag.settings_restores` counts it; a backup whose bytes reached the
  parser and were refused, FAULT: the target stays as found (config.json
  on the defaults, dsp.json on the active model, both as today) and
  `sd_diag.settings_restore_faults` counts it; a backup load that failed
  before its bytes reached the parser (missing, empty, oversized, read
  fault), NOTHING, today's missing-file case. Presence is the backup's
  bytes reaching the parser, since a missing file and an I/O fault both
  fail `OPEN_READ` as a driver fault (`aius_sd_teensy.cpp:276-277`) and
  the job result names no failing action (FR-S10). `g_config_error_count`
  advances once for a failed config.json target (`:8075` or `:8197`), as
  today; neither a restore nor a failed backup load advances it again. A
  restore after a read fault on a good target does no harm: whenever the
  target fails, the backup holds the newest complete save on the card
  (H4), with the two exceptions in the residual risks (FR-S12). Cost:
  nothing when the target validates; a backup load (N + 4) and a
  target-only save (2N + 11) when it restores. A file deleted with a card
  reader comes back from its backup at the next mount unless the backup
  is deleted too. Autonomous call: presets get no mount restore. The
  firmware has no preset loader or validator, and the engine has no
  listing primitive (listings are direct SD, `optical_main.cpp:6256`,
  `:6800`), so a mount pass would add a listing seam and a read of every
  preset at every mount. A preset torn by a power cut fails the editor's
  JSON parse, and `/aius/.bak-preset-<name>.json` holds the save for a
  manual restore (low stakes, owner 2026-10-07).
- **H11, a card reload refills clean entries only (R3-B5).** 4g drops
  cache entries on remount and volume change and refills after each
  mount; 4h stores dirty, flushing, held and tombstoned entries in the
  same store, so a held preset would be dropped or overwritten by the
  card's older file on reinsert. The overlay owns that rule: invalidation
  and refill touch clean entries only, the refill skips any path the
  overlay shadows, and the card-swap device step checks the held preset
  as well as `dsp.json`.
- **H14, filesystem independent (owner overturn 2026-10-07; the
  FAT-only gate is withdrawn).** The save and the restore use only open,
  truncate, write, close, read and `cacheClear()`, which SdFat's `FsFile`
  and both partition types provide, so nothing reads the volume type,
  and BEGIN carries none. `sd_filesystem` in `/api/status` stays the
  probe's value; identity is H3's.

**OTA and owner.** One OTA. The AC-3 run needs the owner and may merge
with Phase 4b's acceptance session; the card-swap check needs the
owner's hands.

**Residual risks.** A power cut loses up to one flush latency of
changes, and held entries are lost on any power cycle or watchdog reset
(by definition). A flush that keeps failing (card full, card fault)
leaves entries dirty and served from PSRAM; OTA and reboot refuse until
a discard, and a power cycle returns the card's older file; `sd_flush`
shows it, with no audio effect. The power-cut model is host-only; no
device power-cut test is planned. A cut during a save's backup write
loses that save (the old target stays and validates); a cut during its
target write is restored at the next mount, for `dsp.json` and
`config.json` only. A torn preset is not restored automatically: its
backup holds the save for a manual restore (H10); presets get no mount
validation, so a preset already torn by an earlier cut starts its next
save backup first, and a cut in that backup pass leaves neither file
valid (FR-B1, low stakes). A target and backup that both fail their
validator keep the target as found and count
`sd_diag.settings_restore_faults` when the backup's bytes reached the
parser; config.json then runs on defaults and dsp.json on the boot
model, as with a missing file today. A cut inside any SdFat sync can
still leave lost clusters, or a directory entry on clusters the FAT
shows free, since SdFat writes the directory sector before the FAT
sector (`FatPartition::cacheSync`, `FatPartition.h:250-252` in SdFat
2.1.2, with the separate FAT cache on ARM); a later allocation by any
file can then take that cluster, so two files share sectors (a
cross-link), and no check-disk runs. Every card write carries that
exposure today. The in-place overwrite narrows it for settings: a save
allocates or frees a cluster only when the file's cluster count changes
(a path's first save, its backup's first save, or a length crossing a
cluster boundary; on cards formatted with clusters of 16 KiB or more a
`dsp.json` of at most 12,288 B never crosses one), but a cut in such a
save can still leave the target or backup on a cluster the FAT shows
free, and a later save of the other file, a sync upload or a recording
can be given it (FR-S1). The backup assumes only firmware writes these
files (FR-S12): a `config.json` or `dsp.json` hand-edited in a card
reader that fails its parser is replaced at the next mount by the older
backup, and after a discard of an entry held by a card pull past its
backup pass's `CLOSE`, the backup holds the discarded write, so a later
mount whose target fails restores it. Sync-class files keep today's
rename gap; a re-sync restores them (H4; owner scope 2026-10-07). In
FIFO mode
during live input settings stay in PSRAM until live input stops owning
the ring (4f F9, F11), so a power cut in that window loses them, and OTA
and reboot answer 409 until then. An HTTP SD request (a page miss, a
listing, an upload)
arriving while a flush job owns the volume answers `503 sd_busy`, as
when any job owns it; a browser page load does not retry, so its
resource fails. A flush starts 0.5 to 1.5 s after a settings accept
(H1's 500 ms debounce, then a start within 1 s) and holds the volume
for about one save (about 0.78 s at 115 epochs/s, H4, longer in
playback), so a page load from about 0.5 s to 2.3 s or more after an
Apply or save is exposed; a warm cache serves pages without the card,
but an 8 MiB fitment has no cache, so there every page load is exposed
in that window (CR3-S3, with the window as corrected at the clean-room
round 3 closure).

**Prebuild review round 2 (2026-10-07, `reviewer` `opus`): NOT READY.**

- **B6 (blocker).** A card reinsert reloads settings from the card,
  bypassing the overlay. Reinsert (`firmware/src/optical_main.cpp:8032-8041`)
  runs `SD_STEP_LOAD_CONFIG` and `LOAD_DSP`; `platform_publish_config`
  (`:8194-8222`) writes `g_pending_dsp_model`, arms Apply and overwrites
  `g_config` from the card file. Same card returning with a dirty or held
  `dsp.json`: the older card model is applied, the newer is flushed but
  never re-applied, so audio diverges from what is saved. A different card
  before discard: that card's config and DSP are applied while H3 promises
  "settings stay the owner's latest". The card-swap proof (`:3064`) uses
  presets only. Fix: CONFIG_LOAD reads through the overlay or skips any
  path with a dirty, flushing or held entry; host case plus a device step
  that holds and reinserts with `dsp.json`.
- **Wrong-fact proofs.** S14 and S15 below, and the stack-bound and memory
  omissions: 4h adds yield roots without stack-bound (`:3102`, see 4e S2)
  and memory is absent from the green list (see 4e S8).
- **S14 (`:3061`).** "GET returns the accepted bytes" is wrong: `GET
  /api/dsp/config` is serialised from memory (`write_dsp_config`,
  `firmware/src/optical_main.cpp:3016`) and changes only at the enqueue
  apply. Fix: name the route each proof reads.
- **S15 (`:3063`).** The 5,000 ms flush bound looks unmeetable for one full
  sync (about 600 KB, one file at a time, read-back; one primitive per epoch
  under playback is 2,400+ primitives, plausibly over 5 s for the last
  file). Fix: bound latency per flush start, or exempt sync bursts.
- **S16 (H5).** The OTA drain check is TOCTOU: the 409 check is at the
  route, activation and rehearsal run later from `loop()`; a PUT accepted in
  between is lost across the reset. Fix: re-check the drain in
  `service_ota_activation` and rehearsal; add the same gate to `POST
  /api/reboot`.
- **S17 (H4 RECOVER, `:3060`).** "Both exist, remove the backup" can free a
  live file's clusters: SdFat rename creates the new entry then removes the
  old (`FatFile.cpp:936-1009`); a power cut between leaves both names on one
  cluster chain; `remove(bak)` frees the target's clusters; the
  per-primitive power-cut model misses this. Fix: journal phase markers, or
  compare first clusters and drop only the directory entry; also remove
  orphan `.tmp` files; an empty journal is a no-op.
- **Nit.** The 4h read-back can hit SdFat's FsCache for a partial last
  sector; read around it or invalidate the cache line before the check.

**Round 2 resolutions (contract revision 2026-10-07; round 3 to confirm).**

- B6 resolved: H7 and the platform_publish_config seam; sd-overlay
  `shadows`, optical-cutover case, card-swap device step with `dsp.json`
  (Phase 5 H7).
- Stack-bound and memory omissions resolved: both in the green list with
  per-phase ITCM and DTCM budgets; owned paths added to parent-wiring.
- S14 resolved: each proof names its route (`GET /sd/aius/dsp.json` for
  accepted bytes, `GET /api/dsp/config` for the applied model,
  `/api/status` for public config).
- S15 resolved: H8 settings class and bound scope, `sync_drain_ms`,
  device run reordered with a clear after the sync drains (Phase 5 H8).
- S16 resolved: H9 re-check in both OTA services and the reboot service,
  reboot route gated, host case for a PUT after the route check (Phase
  5 H9); the re-check placement refuted in round 3, superseded by the
  round 3 S16 resolution and R3-S10, and that in turn by R4-B1.
- S17 resolved: H10 phase byte and first-cluster compare, entry-only
  unlink in the vendored SdFat (install-journal owns it), orphan `.tmp`,
  no-op journal, power cut modelled inside a rename (Phase 5 H10);
  refuted in round 3, superseded by R3-B6 and R3-S11.
- Nit resolved: H1 `cacheClear()` before the read-back.

**Prebuild review round 3 (2026-10-07, `reviewer` `opus`): NOT READY**
(R3-B5, R3-B6, R3-B3). Each item is followed by its resolution.

- **Refuted: round 2 S16.** The quiesce follows the one-way recorder park
  and the direct `SD.open` copy, so a failing re-check there leaves the
  recorder parked, and a flush job holding the file open shares the
  volume with the copy. Resolved: H9 re-check first after the HTTP drain
  wait, before the park, then an SD-writes freeze and a wait for the
  in-flight job; optical-app and optical-cutover cases (Phase 5 H9).
  Superseded by R4-B1 (the wait spans epochs, so a second drain query
  sits immediately before the park) and R4-S3 (the freeze's own
  ceiling).
- **Refuted: round 2 S17.** A cut mid phase-byte rewrite after rename 1
  leaves a bad CRC, and "bad CRC is a no-op" leaves the target missing.
  Resolved by R3-B6.
- **R3-B5 (blocker).** 4g's remount invalidation and refill drop or
  overwrite held entries. Resolved: H11 clean-only rule in the overlay
  seam, sd-overlay case, card-swap step checks the held preset.
- **R3-B6 (blocker).** Resolved: H10 two write-once journal records with
  their own CRCs, recovery from the last valid record, no-op only with no
  backup or temp present, torn-sector power-cut cases (Phase 5 H10).
- **R3-B3 (4h part).** Resolved: 1,000 B on the canonical image (4e
  §Memory), memory proof line.
- **R3-S1 (4h part).** Resolved: runtime-target in the green list, owned
  by parent-wiring.
- **R3-S5 (4h part).** Resolved: producer-isolation in the green list and
  parent-wiring paths; the accept and flush roots join the yield-root
  table.
- **R3-S8 (4h part).** The flush job was never admitted in FIFO mode.
  Resolved: H8, the flush start goes through `sd_fifo_defer()`;
  optical-cutover case.
- **R3-S9.** The preset path was `/aius/www/presets/`. Resolved: H8 and
  the overlay seam use `/www/presets/`; sd-overlay case.
- **R3-S10.** An abandoned activation had answered 200 with no fault.
  Resolved: H9 `OTA_ACTIVATION_FAULT_FLUSH_PENDING`, the flash tool
  reports it; ota case.
- **R3-S11.** An empty new entry from rename 2 made "distinct chains,
  remove the backup" delete both files. Resolved: H10, every backup
  removal depends on the temp's existence and the target's chain; the
  power-cut model creates the new entry with first cluster 0 in every
  sector-write order.
- **R3-S12.** "Retries four times" was stale. Resolved: H2 names the 4g
  `Retry-After` rule and the 15 s limit.
- **Phase 5 H9.** Three service re-checks, not two; H5's 30 s wait
  folded in. Resolved in Phase 5.
- **R3-N2 (found by the round 3 revision; resolved in round 4).** H4's
  "about 5 card calls per REPLACE" predates the two journal writes and
  the temp verify; re-derive it and the 5 s settings bound it feeds.
  Resolved: H4 count (about 90 primitives, about 0.8 s), sd case pins it.
- **R3-N3 (resolved in round 4).** Phase 5 H9 says all three services
  re-check "before the recorder park", but the reboot service has no
  park; it should read "before any one-way step". Resolved: Phase 5 H9
  corrected, with the R4-B1 second query.
- **R3-N4 (resolved in round 4).** No residual-risk line covers RECOVER
  renaming a journal aside and counting `recover_faults`, which needs a
  manual look at the card. Resolved: residual-risk line, `sd_diag`
  `recover_faults` and `recover_aside` (H10, seam, status-body-bound).

**Prebuild review round 4 (2026-10-07, `reviewer` `opus`, closure
review): NOT READY** (R4-B1, R3-N2 to N4). R3-B5, R3-B6 (H11), R3-S9
to S12 confirmed closed; R3-S11 closed with the R4-S5 edge. Each item is
followed by its resolution.

- **R4-B1 (blocker, H9).** The round 3 S16 race is back: re-check, then
  freeze, then wait for the in-flight SD job, which moves one primitive
  per epoch, so the wait spans epochs while the HTTP checkpoint still
  accepts PUTs into the overlay; the frozen scheduler never flushes
  them, and park, copy and reset follow, losing the PUT. Resolved with
  the first of the reviewer's two fixes: a second drain query in the
  same call, immediately before `aius_recorder_bank_park` (before the
  reset in the reboot service); optical-app case for a PUT accepted
  during the wait, optical-cutover placement guard (Phase 5 H9
  corrected, H12). Why not the second fix (the freeze makes store and
  upload routes answer `409 sd_flush_pending`): one check at the one-way
  point closes every interleaving however long the wait runs, while the
  route refusal would need every write route to consult the freeze and
  would refuse an editor Apply mid-OTA, a new failure the UI has no
  handling for. Supersedes round 3's S16 resolution.
- **R4-S3 (H9 freeze).** The in-flight wait was bounded by the 500 ms
  drain ceiling armed at the route's epoch; a 4g fill in flight (about
  3 s for `dsp-editor.js`) abandoned activation as `flush_pending`,
  routinely after a sync. Resolved: the freeze cancels a fill file at
  its next primitive (4g G9), has its own 2,000 ms ceiling from the
  latch, and abandons with `ota_sd_busy`; a reboot resets on expiry as
  today; seam, optical-app case, ota case (Phase 5 H13).
- **R4-S4 (H10).** "No valid record with a temp present" counted a fault
  though no rename ran. Resolved: only a backup present counts; journal
  and temp otherwise removed silently; H10, sd case (Phase 5 H10
  corrected).
- **R4-S5 (H10).** The cluster-0 backup rule unlinked the only copy of an
  empty original after a completed rename 1. Resolved: the rule applies
  only while the target exists; H10, sd case.
- **R4-S6 (H10 table gaps).** (a) orphan `.tmp` removal after a
  set-aside journal, (b) a leaked `.bak` in the INSTALL
  target-holds-temp branch, (c) INSTALL with the target on another
  nonzero chain undefined. Resolved: (a) skipped, (b) backup removed,
  (c) kept-files fault; H10, sd case.
- **R3-N2, R3-N3, R3-N4.** Adjudicated as the reviewer proposed;
  resolved in place above.
- **R4-S2 (4h part).** A flush in FIFO mode during live input waits for
  silence. Resolved: outcome, H8 and residual risks state it; the device
  run asserts `sd_mode` DMA, and H8's flush start also cancels a fill
  file so the 5 s bound holds (4g G9).
- **R4-N3 (found by the round 4 revision; resolved in round 5).**
  RECOVER on a REPLACE of a path that never existed removes the verified
  temp: the card returns to "no file", which the outcome allows, but the
  new file is lost. Decide whether a verified temp with no prior target
  is installed instead. Resolved: see round 5.

**Prebuild review round 5 (2026-10-07, `reviewer` `opus`, closure
review): NOT READY** only on the R4-N3 decision, plus one should-fix
item. R4-B1, R4-S2 to S6 and R3-N2 to N4 confirmed closed; no new
blocker. Each item is followed by its resolution.

- **R4-N3 adjudicated: install the temp only when BEGIN says the path
  never existed.** Resolved: BEGIN gains `had_target`; BEGIN last, the
  target missing, no backup, `had_target` false and the temp matching
  BEGIN's length and CRC renames the temp to target; `had_target` true
  with no backup (or a temp absent or failing the check) is a
  kept-files fault; `lib/aius_sd` seam, H10 and two sd cases (Phase 5
  H10 amended).
- **R5-S2 (H9).** The abandon path read "state back to staged, request
  cleared", but rehearsal's own abandon is `ota_rehearsal_finish()`,
  which clears `g_ota_rehearsal_requested`
  (`optical_main.cpp:10006-10011`); a rehearsal abandoned with the
  request still set re-enters every epoch and, once the flush drains,
  runs and resets the board unasked. Resolved: an abandoned rehearsal
  calls `ota_rehearsal_finish()` with a distinct
  `OTA_REHEARSAL_ABANDONED` result, an abandoned activation calls
  `ota_activation_recover()`; seam, H9 and an optical-app case that
  neither re-runs (Phase 5 H9 amended).
- **R5-S1 (4h part, from 4g).** The flush start and the H9 freeze cancel
  a fill with no HTTP job after it, so the fill hold never lapsed.
  Resolved: the flush job's end and every unfreeze call
  `web_cache_fill_hold_release()`; seam, H9 and optical-app cases for a
  cancel from a flush and from the freeze (4g G9; Phase 5 G9 amended).
- **R4-N2 (4h part, from 4e).** H4's 0.8 s rested on an inferred epoch
  period. Resolved: stated at 115 epochs/s and re-derived, with H8's
  5 s margin, from 4e run 2's playback rate (H4; Phase 5 E13).

**Clean-room review (2026-10-07, `reviewer` `opus`): NOT READY** only
via 4f's CR-B1; no blocker of its own. Each item is followed by its
resolution.

- **CR-B1 (4h part, from 4f).** With no source, the card-swap step's
  Applies were shed and the editor retried forever, and a FIFO flush
  waited on a "silence" that read as a short margin. Resolved in 4f
  (F11); here the outcome, H8 and residual risks say "until live input
  stops owning the ring", and the card-swap step asserts first-attempt
  200s with `http_shed` and `sd_fifo_defers` delta 0.
- **CR-S4.** H3, H10 and the unlink patch rely on FAT rename semantics,
  while the firmware also mounts exFAT. Resolved: H14, journal and
  RECOVER on FAT volumes only, exFAT keeps today's install and a
  CID-only identity; `lib/aius_sd` and vendor seams, an sd case; the
  first device step records the deployed card's `sd_filesystem`
  (Phase 5 H14).
- **CR-S5.** `preAllocate` zeroes nothing, so a reused cluster could
  hold a prior valid BEGIN and INSTALL. Resolved: H15, the journal is
  created by writing two zero sectors and syncing, and RECOVER reads
  only within the recorded size; seam, H4 count (about 92), H10 text and
  a power-cut case with stale records (Phase 5 H15).
- **CR-S10.** The device step did not space its six writes, and one
  file per job lets the last queued file exceed 5 s. Resolved: each
  write waits for `sd_flush.dirty` 0, and H8 bounds only a write with no
  settings file queued ahead, recording the rest.
- **CR-S11 (4h part).** ui-pending did not own the fixtures an
  `sd_flush` case needs. Resolved: `test/sd-editor/fixtures/**` in
  ui-pending's paths.

**Clean-room closure (2026-10-07): HOLD** only via 4e's CR2-B1 (its
producer-isolation proof, which 4h's accept and flush roots extend);
CR2-S1 is 4h's own. Each item is followed by its resolution.

- **CR2-B1 (4h part, from 4e).** Resolved in 4e (E14 covers the E-AC-3
  log shim); nothing changes here.
- **CR2-S1.** The FAT-only gate read the probe's last
  `AiusSdFilesystem`, which the deferred playback probe never refreshes,
  so a card swapped to exFAT and remounted at a job BEGIN would get a
  journal RECOVER never reads. Resolved: H14 keys on the job's own BEGIN,
  which carries the mounted volume's `fatType()`; `lib/aius_sd` and
  `aius_sd_teensy.cpp` seams, the swap case in `sd`, an `sd-target` case,
  install-journal owns the adapter and `sd-target`, `sd-target` in the
  green list (Phase 5 H14).

**Clean-room round 3 (2026-10-07): HOLD** only via 4e's CR3-B1 to B3
(the accept and flush yield roots join that walk); CR3-S3 is shared
with 4g. Each item is followed by its resolution.

- **CR3-B1 to B3 (4h part, from 4e).** Resolved in 4e (§Walk
  specification): the accept and flush yield points are yield-root rows
  (W2) and W5 (2) covers their chains; nothing else changes here.
- **CR3-S3 (4h part).** A page request during a flush answers `503
  sd_busy` with no browser retry, worst on 8 MiB fitments with no cache.
  Resolved: stated as a residual risk with its exposure window rather
  than extending G9's precedence to flushes, since a flush is a
  journalled REPLACE whose primitives should not be interleaved with a
  download mid-file (Phase 5 H16).
- **Phase 5 H4 correction.** H4's reversal said "`lib/aius_sd` REPLACE
  and RECOVER only", contradicting H10 and H14. Resolved in Phase 5 H4.

**Clean-room round 3 closure (2026-10-07): HOLD** (CR4-B2; CR4-B1 and
CR4-B3 via 4e). CR3-B1 to B3 and CR3-S3 confirmed closed except as
below. Each item is followed by its resolution.

- **CR4-B2 (blocker).** The read-back's `FatPartition::cacheClear` and
  the mount-time identity read sat in `optical_main.cpp`, outside the
  adapter and every window: `cacheClear` syncs a dirty sector through
  the `SdioCard` vtable, so it reaches the hook with no W2 row, and the
  identity read names `SD.`, failing optical-cutover's guard. A row
  would leave the sync write unwindowed (in DMA mode its busy wait calls
  the hook with the window flag clear: a write-busy stall of up to
  250 ms during playback). RECOVER's unlink-entry and first-cluster
  reads had the same gap. Resolved as the reviewer proposed: CACHE_DROP,
  UNLINK_ENTRY and FIRST_CLUSTER are adapter cases in
  `aius_sd_teensy.cpp` run through the windowed storage execute, and the
  CID and volume serial ride on BEGIN's OK event beside the volume type
  (one fewer primitive than a separate identity case, same window);
  install-journal's ownership of the adapter widens to those cases plus
  `aius_sd_teensy.h` and `firmware/test/fake-fidelity/**`, with a
  handoff to parent-wiring for the runtime-target fake; owned paths
  stay disjoint; sd-target, sd and optical-cutover cases, fake-fidelity
  in the green list (Phase 5 H17).
- **CR4-B1, CR4-B3 (4h part, from 4e).** Resolved in 4e (W5 (2)
  forward, link half split by definition kind); the accept and flush
  yield roots need nothing more.
- **CR4-S4 (4h part).** Resolved by 4e W7: 4h re-runs the census on its
  own link and records its accept and flush yield-root rows and any
  changed site counts in its own commit.
- **H16 correction.** The exposure was "within about 1.3 s"; a flush
  starts 0.5 to 1.5 s after the accept and lasts about 0.8 s (longer in
  playback), so 2.3 s or more. And the reversal named a dispatch
  condition in `aius_optical_app.cpp`, but the SD_START arm is in
  `aius_runtime_network_http.cpp` (4f's admission seam), and the flush
  end must release a parked request. Resolved: residual-risk text and
  Phase 5 H16.

**Clean-room round 3 second closure (2026-10-07): READY.** No
blocker; one should-fix item shared with 4e. No further clean-room
round: the closing reviewer was itself clean-room, and implementation
carries a delivery audit per phase. Each item is followed by its
resolution.

- **CR5-S4 (4h part).** H3's identity read assumed 4e's accessor could
  return the FAT volume serial without a card command, but
  `FatPartition` keeps no serial. Resolved in 4e: the vendored patch
  stores it at mount. Here H3 and the BEGIN seam say so, and sd-target
  asserts the reused-mount BEGIN payload (type, CID, serial) adds no
  card read (Phase 5 H17).
- **CR5-S1 to S3 (4h part, from 4e).** Resolved in 4e (link half
  include check, libc row figures, `SHF_ALLOC` relocations); nothing
  changes here.

**Focused review (2026-10-07): NOT READY** (FR-B1) on the
copy-before-write design. Interplay with H7, H9 and H11 confirmed
holding. Each item is followed by its resolution; orchestrator
decisions, 2026-10-07.

- **FR-B1 (blocker).** Every flush wrote the backup first even with the
  target torn and the backup the only good copy (the restore's own
  flush, a retry after a target-pass failure, a card pulled mid target
  pass and returned), so a cut in that backup pass left neither file
  valid. Resolved as the reviewer proposed: a per-entry safe copy, the
  target written first when it is not known valid, a restore-created
  entry writing the target only; H4 Order, overlay seam, sd-overlay
  cases and power-cut model cases from (target torn, backup valid)
  around every primitive of a restore's flush and of a retry.
- **FR-S1.** Truncate-first freed and reallocated clusters each save,
  and the cross-link claim was too strong. Resolved: overwrite in place
  with sector 0 zeroed first and written last, `TRUNCATE` to the new
  length; the parsers reject trailing bytes but not an equal-length
  splice, hence the zero sector; N + 5 per pass; residual risks state
  the narrowed cross-link exposure; the owner-overturn line corrected.
- **FR-S2.** The walk does not dedupe. Resolved: re-pinned to two
  EXISTS, 3N + 17 (89).
- **FR-S3.** A root job had no input path. Resolved: a root input source
  in `aius_runtime_storage.cpp` answers `NEED_INPUT` in the same advance;
  90 epochs per `dsp.json` save, pinned by runtime-target.
- **FR-S4.** Resolved: version pinned at job start for both passes and
  the CRC; sd-overlay cases for a put mid-pass and between passes.
- **FR-S5.** Resolved: `aius_sd_overlay_restore_decision` in the
  portable overlay, called by `optical_main.cpp`.
- **FR-S6.** Resolved: sd-overlay builds the stateful fake card in its
  own test directory; parent-wiring adds the `test.sh` arm.
- **FR-S7.** Resolved: sd-overlay's job-request cases follow
  copy-before-write's report (fan-out comment).
- **FR-S8.** Resolved: tombstoned joins the drain query, H5 and the
  drain proofs.
- **FR-S9.** Resolved: a tombstone whose backup path cannot be formed
  deletes the target alone.
- **FR-S10.** Resolved: a restore fault counts on a backup whose bytes
  reached the parser and were refused; a failed backup load is the
  missing case.
- **FR-S11.** Resolved: `SD_STEP_RESTORE_CONFIG` and
  `SD_STEP_RESTORE_DSP` with the target's parser and shadow check;
  `g_config_error_count` advances once for the failed target.
- **FR-S12.** Resolved: the hand-edit and discard exceptions are
  residual risks.
- **FR-S13 (owner scope).** Resolved by the owner 2026-10-07: settings
  files only (scope line at the head of this phase); the 4b row says
  what 4h covers.
- **Stale references.** Resolved: the adapter seam's H14 wording; H16
  and H17 citations replaced by the items the owner confirmation maps
  them to (H1, H3, CR4-B2, the residual risks); the 4b row. History
  blocks left as written.

**Focused review closure (2026-10-07): READY.** FR-B1 and FR-S1 to S13
closed. The probes held: a cut between the final sector-0 `WRITE` and
`CLOSE` leaves the complete new file on FAT and exFAT (`TRUNCATE(L)`
already synced size and FAT; the full sector lands at its `WRITE`), and
a one-sector file leaves either the new bytes over the old tail or a
new prefix, both refused unless the remainder is whitespace.

- **FR2-S1.** A put on an entry whose safe copy is the backup could
  reset it and bring FR-B1 back. Resolved: a put keeps the entry's safe
  copy (overlay seam); sd-overlay cases for a put on a restore-created
  entry and on an entry held after a target-pass failure.
- **FR2-S2.** 4e §Epoch period's 0.73 s is now 0.78 s (90 epochs).
  Resolved.
- **FR2-S3.** A first save cut in its backup pass (target missing,
  backup with sector 0 zeroed) counts `settings_restore_faults`, where
  the earlier rule counted nothing. Accepted: target presence is not
  knowable (FR-S10), the case is a first save cut mid-pass, and an
  over-count on a diagnostic is harmless.

### Phase 4c `test.sh` runs every `--suite` given ✓ SHIPPED 2026-10-06

```yaml
phase: 4c
outcome: firmware/scripts/test.sh runs each --suite given, in order, and fails if any fails
seams: [firmware/scripts/test.sh argument parsing]
targeted-proof: test.sh with two suites, one forced to fail, exits non-zero and reports both
unit: component-behind-interface
```

Found during Phase 4a (2026-10-06): several `--suite` flags silently run
only the last, so a multi-suite proof command can report green while
skipping suites. Independent of the device session.

Shipped: two or more suites each run in their own process, a `==>
summary` lists `PASS` or `FAIL <s> (exit N)`, and the run exits 1 if any
failed; one suite behaves exactly as before. Proof: single clock (exit 0),
clock plus tx-gate (exit 0), unknown plus clock (exit 1, clock still ran),
registry green, shellcheck clean.

### Phase 4d CLAUDE.md promote for the fill counters and signing form ✓ SHIPPED 2026-10-07

```yaml
phase: 4d
outcome: CLAUDE.md names the fill stamps, the write counters and the full signing form as Phase 4a's B1 states
seams: [CLAUDE.md, via the release-promotion step]
targeted-proof: the promoted diff carries exactly B1's lines; no foreign hunk is reverted
unit: parent-only
```

The pending CLAUDE.md edits were this work's (owner, 2026-10-06) and are
committed (`5d7c05d`). B1's promote landed on owner approval; the diff
carries exactly B1's lines (fill stamps, the pending pad counter, the
three write counters, the full signing form, `python3` for the flash
tool).

## Acceptance

### Host

- Tracker and clock-plan golden suites pass all supported rates and boundaries.
- PCM blocks carry the supplied carrier nominal rate.
- AC-3 32/44.1/48 fixtures decode and retain their rate on all six output
  blocks.
- DTS and AC-3 rate mismatch is refused before DSP state advances.
- The current 48 kHz PCM/DTS/AC-3 corpora remain sample-identical.
- DSP rate-change tests prove coefficients and meters rebase at the boundary.
- Status/events/clear semantics are contract-tested.
- Production and checkpoint-9 fault images build with memory and ITCM gates.

### Device

All evidence names the exact flashed build identity and records raw JSON.
The Phase 2 delivery audit also rejects any remaining authoritative claim that
live optical transmit or channel status is fixed at 48 kHz.

1. **48 kHz asynchronous source, at least 10 minutes.** CM6206 optical output
   into Teensy. Require stable nominal 48 kHz, output target following the
   millihertz measurement, absolute least-squares ingress slope no greater than
   0.25 words/s after settling, no ingress drop/reset, no output drop/abandon,
   no service budget overrun and no watchdog reset. Record
   `trim_updates`/`invalid_windows`. **The post-settle trim count is advisory
   as of 2026-09-04 (D4 (b)):** the receiver's ~140 mHz estimate quantum spreads
   the settled carrier ~1.2 Hz over 10 min, so the deadband cannot exceed the
   1,000 mHz follow-error bound and the earlier ≤ 2 post-settle-trim bound is
   physically unmeetable on this source without averaging the estimate (a
   firmware lever declined 2026-09-04); the trims never reach the output (gate 7
   saw none). Gate 1 therefore passes on the checks above with the trim count
   recorded but not gating (§Device evidence — 2026-09-04).
2. **44.1 kHz PCM, at least 120 seconds.** Require accepted nominal 44,100,
   output target following the measured carrier, continuous output, bounded
   ring levels and zero harmful deltas. The external `audio_rate_hz` field is
   recorded but asserts nothing on the U24 loop (design decision 2026-09-02): the
   capture clock locks to the carrier under test, so the field is
   self-referential there and the gate is judged on continuity plus the
   harmful counters.
3. **32 kHz PCM, at least 120 seconds.** Same gates, using an external optical
   PCM source because this CM6206 USB profile exposes only 44.1/48 kHz.
   **Deferred 2026-07-31 by owner decision:** 32 kHz is long-tail scope with
   no real source in the estate; the implementation stays host-proven
   (tracker snap, `k = 21` clock plan, DSP coefficients, ±2% goldens) and
   device acceptance covers 44.1/48. Runs when a 32 kHz-capable source is on
   the bench; the incoming ESI U24 XL lists 32/44.1/48 kHz support.
4. **Rate transitions.** Exercise 48→44.1→48 (the 32 kHz leg is deferred with
   gate 3) and carrier loss→48 kHz replay.
   Each accepted transition emits one event, increments one rate change, resets
   classification once, crosses no old-rate output and returns to continuous
   progress without a device reset. A transient unsupported physical estimate
   and TX-ring drain are permitted only inside the corresponding closed-gate
   transition window. Every unsupported-episode increment must match one
   rate-unsupported event whose measured-rate detail is outside every supported
   ±2% band. An open-gate underrun, an unsupported episode outside a live-rate
   transition or any event overflow still fails the gate.
5. **Encoded non-48 input.** Exercise at least one 44.1 kHz DTS or AC-3 fixture
   through the CM6206. Require carrier, decoder and output nominal rates agree
   and no mismatch/refusal count.
6. **Negative control.** A supported decoder header deliberately paired with a
   different carrier nominal is muted and counted before DSP/output state
   advances.
7. **Trim-boundary waveform.** Feed a constant PCM tone through the complete
   CM6206→Teensy→CM6206 loop and force at least one production same-nominal
   trim. Require carrier lock and capture-frame cadence to remain continuous
   and no sample discontinuity outside the board-declared invalid windows.
   The windows need not be externally visible (design decision 2026-09-02: the
   board may absorb a trim without any output artifact); any window that IS
   visible must be bounded by the exact 512-frame hold and correlate with
   `trim_updates` / `invalid_windows`, and the capture may never show more
   windows than status declares. Do not infer mute behavior from `VALCTRL`
   alone.

### Device evidence in progress

- Exact image `0a6f3a6f3480+teensy41-optical` passed the 48 kHz asynchronous
  gate for 600.000 seconds: ingress slope 0.005207 words/s, maximum follow error
  122 millihertz, and every harmful counter delta zero. That evidence remains
  valid for its observed CM carrier state, but the active-playback discriminator
  below prevents treating it as proof of the CM's current active USB-output
  clock.
- Its first 44.1 kHz run failed: the initially accepted physical estimate was
  44,477,836 millihertz before settling to 44,105,350, so the ten-second trim
  delay drained 640 TX frames. The returned capture contained 29 128-frame zero
  runs, one 372-frame zero run and 16 discontinuity events. This is the
  reproduced reason supported acquisition now requires a stable one-second
  physical-rate interval and quiesces producer ingress while waiting.
- Corrected exact image `d009092556a3+teensy41-optical` then passed the 44.1 kHz
  firmware bracket for 120.000 seconds: accepted nominal 44,100 Hz, 9,468
  committed output blocks, maximum follow error 115 millihertz, ingress and TX
  high-water marks 256 words and 512 frames, and every harmful counter delta
  zero. The returned 121-second capture had no in-band zero run longer than one
  frame, but contained 12 isolated one-frame impulses.
- A simultaneous authenticated flight-recorder comparison reproduced two
  impulses and aligned 970,200 stereo frames between the committed post-DSP
  plane and the CM6206 return. Every aligned sample was bit-exact after 24-to-16
  scaling. The impulses therefore existed before Teensy transmit; the transmit
  ring, Teensy optical output, return fibre, CM capture and ALSA capture did not
  create them. The recorder download header incorrectly remained at 48,000 Hz
  while live status and the bit-exact timeline were 44.1 kHz; resume tooling
  must use the accepted live nominal rate rather than that static field.
  (Fixed 2026-07-31 in `fecc4a0a2473`: the header now serves the
  capture-time stamped rate plus a `rate_changes` counter; device-verified
  at 44.1 kHz.)
- Repeating that comparison with input EQ, matrix, output EQ and limiter
  bypassed reproduced four impulses in 22 seconds. The configurable DSP stages
  are not required. The original SD configuration was restored and verified
  semantically at generation 9; two setup-only round trips account for the
  preceding generation changes.
- A direct CM6206 optical-output-to-input loop then reproduced the same class
  with the Teensy absent:

  | Run | Active duration | Isolated impulses | `>1000` edges | In-band zero runs ≥2 frames | ALSA errors/xruns |
  |---|---:|---:|---:|---:|---:|
  | 44.1 kHz, stereo playback altset 2 | 124.900 s | 9 | 18 | 0 | 0 |
  | 48 kHz, stereo playback altset 2 | 124.906 s | 1 | 2 | 0 | 0 |
  | 44.1 kHz, eight-channel playback altset 1 | 124.887 s | 15 | 30 | 0 | 0 |

  Each source file had zero qualifying impulses, no multi-frame zero runs and
  no clipping. Each return also had sub-threshold cadence anomalies; the 48 kHz
  return began with its right channel one sample behind the left. The
  eight-channel USB alternate setting is not a cleaner workaround, and latency
  tooling must not assume this interface returns a sample-identical stereo
  timeline. The direct-loop reproduction excludes Teensy receive, DSP,
  sample-rate following and transmit from this corruption class. It does not
  distinguish CM playback/transmit, CM capture, the optical jumper, USB
  transport, the Icron/Renesas path, virtualization or their interaction.
- After the hypervisor restart, the same direct 44.1 kHz CM6206 optical
  output-to-input loop reproduced the corruption before any other diagnostic.
  The 124.891-second active return contained seven isolated impulses and 14
  greater-than-1,000 edges; the source contained none. Playback and capture
  both exited cleanly with no ALSA error or xrun. Raw artifacts are under
  `.scratch/019fb103-d707-7e01-adee-0ad1238d3115/cm6206-direct-loop-post-reboot-44k1-125s/`.
  The reboot therefore did not eliminate the corruption class. This deliberate
  repeat does not replace the external-source capture discriminator below.
- The post-restart capture-side discriminator then passed with an external Mac
  optical source configured at 44.1 kHz. A three-second preflight proved the
  expected approximately ±410 identical-stereo tone before the 125.000-second
  capture. The return contained zero isolated impulses, zero
  greater-than-1,000 edges, no zero run longer than one frame, no clipping and
  zero unequal stereo frames. Both channels retained the exact 44/45-frame
  crossing cadence at 996.99996 Hz, and ALSA reported no error or xrun. Raw
  artifacts are under
  `.scratch/019fb103-d707-7e01-adee-0ad1238d3115/cm6206-external-mac-44k1-600s/`.
  This clean capture-only path localizes the direct-loop corruption to CM6206
  USB playback/S/PDIF transmit or full-duplex interaction. It does not yet
  distinguish those two cases.
- The existing Teensy PSRAM flight recorder then reproduced the same corruption
  with **CM6206 USB playback active and its USB capture endpoint closed**. The
  exact `d009092556a3+teensy41-optical` image recorded 972,288 frames through
  transparent DSP during one 22-second playback window. It found two isolated
  one-frame impulses: left `14 → 16852 → -101` at 13.459 seconds and right
  `-2 → -8136 → 113` at 14.971 seconds. Recorder gaps and every tracked harmful
  counter delta were zero. The diagnostic restored DSP semantic configuration
  and left the recorder running. Raw artifacts are under
  `.scratch/019fb103-d707-7e01-adee-0ad1238d3115/cm6206-playback-duplex-ab/`.
  Simultaneous USB capture is therefore not required. The physical optical
  return remained connected, so this window does not yet distinguish CM6206
  USB playback/S/PDIF transmit or upstream USB OUT transport from an internal
  interaction with its locked S/PDIF input.
- Repeating that playback-only recorder window with the Teensy optical output
  physically disconnected from the CM6206 input reproduced one right-channel
  impulse, `-54 → -8188 → 62`, at 21.628 seconds. CM USB capture remained
  closed; the exact image again recorded 972,288 frames with transparent DSP,
  zero recorder gaps and every tracked harmful counter delta zero. DSP semantic
  configuration was restored and the recorder was left running. Raw artifacts
  are under
  `.scratch/019fb103-d707-7e01-adee-0ad1238d3115/cm6206-playback-one-fibre/`.
  Neither USB full-duplex operation nor a locked physical S/PDIF return is
  required. The remaining fault surface is the CM6206 USB playback/S/PDIF
  transmit path or the upstream USB OUT transport through
  snd-usb-audio/xHCI, Renesas, Icron and virtualization.
- A 20-second 44.1 kHz AC-3 preflight on exact image
  `d009092556a3+teensy41-optical` classified and decoded valid type-1 bursts,
  but CM6206 word corruption also caused 38 decode rejects, two parse rejects
  and consequent transmit underruns. This source-sensitive gate remains
  pending: an encoded-word error cannot be waived as a known one-frame PCM
  impulse without raw-word correlation.
- The independent 48 kHz carrier/44.1 kHz AC-3 header negative control then
  exposed a firmware refusal-bookkeeping defect. Its 30-second window counted
  469 mismatched blocks and committed zero output blocks, but every deliberate
  refusal was treated as a fatal sink error, producing 469 output drops, 469 AU
  abandons and repeated classification events. The repair adds an explicit
  `AIUS_REFUSED` sink result: the pipeline consumes and mutes the decoded block
  without committing output, resetting classification or reporting a failure.
  The negative analyzer separately permits TX underruns while the documented
  pad-mux gate is intentionally dark. Pipeline, audio-service, codec-contract,
  rate-runner and optical-cutover host proof and the production cross-build
  pass. Exact image `01477755955b+teensy41-optical` then passed the repeated
  30-second negative control: all 31 samples remained encoded AC-3 type 1 with
  the physical gate closed; 5,628 mismatched blocks were counted, zero output
  blocks committed, and output drops, AU abandons, parser/decode rejects,
  ingress resets, service/runtime faults, event overflows and watchdog resets
  all had zero deltas. The declared dark-pad TX drain advanced 1,440,512
  frames. Raw pre-repair evidence is under
  `.scratch/019fb103-d707-7e01-adee-0ad1238d3115/ac3-negative-48-carrier-441-header/`.
  Passing exact-image evidence is under
  `.scratch/019fb103-d707-7e01-adee-0ad1238d3115/ac3-negative-refusal-014777559/`.
- Exact image `01477755955b+teensy41-optical` then completed a 120.000-second
  44.1 kHz AC-3 firmware bracket with maximum carrier/output follow error of
  one millihertz, 15,774 committed output blocks and zero ingress drop, output
  drop, AU abandon, reset, service/runtime overrun, clock fault, allocation or
  watchdog delta. It does not pass the encoded gate: CM6206 source corruption
  caused 12 parse rejects, 271 decode rejects and 880,000 consequent TX
  underrun frames. Raw evidence is under
  `.scratch/019fb103-d707-7e01-adee-0ad1238d3115/ac3-44100-current-120s/`.
- A fixed-wiring transition subset on that same long-lived boot observed the
  accepted sequence 48→44.1→48 kHz. Both live transitions emitted exactly one
  rate-change event and requested exactly one classification reset. Each
  closed the physical gate, reported three transient unsupported physical
  estimates outside the supported bands, then reopened at the new rate.
  Of 127,616 TX underrun frames, 127,488 occurred in those dark windows; one
  separate 128-frame underrun occurred at 11.719 seconds between two open-gate
  48 kHz samples after the TX level had reached zero. No ingress/drop/abandon,
  parser/decoder, service/runtime, clock-fault, reset or watchdog counter
  advanced. The run remains diagnostic rather than acceptance evidence: it
  omitted 32 kHz and replay, had no external old-rate waveform observation and
  its already-full lifetime event ring recorded eight overwrites. The
  transition analyzer now attributes the deliberately dark effects explicitly
  while retaining hard failures for the open-gate underrun and event overflow.
  Raw evidence is under
  `.scratch/019fb103-d707-7e01-adee-0ad1238d3115/rate-transition-48-441-48/`.
- An authenticated application reboot then provided boot generation 3 with an
  empty event-overflow history on the same exact image. A 660-second 48 kHz PCM
  file kept CM USB playback active across a fresh 600.000-second bracket. The
  audio path itself was clean: 53,431 committed blocks, ingress slope
  0.055937 words/s, bounded 256-word/512-frame rings and every harmful counter
  delta zero, including TX underruns. The formal clock gate failed. There were
  53 post-settle trims, 123 samples outside the 1,000-millihertz follow bound
  and a maximum error of 4,069 millihertz. The CM physical estimate changed
  318 times among ten values spanning 48,015,593–48,021,879 millihertz; the
  old passing run had only two values 139 millihertz apart. Once active USB
  playback stopped, the CM returned to 48,012,799 millihertz, exactly one value
  from the old passing family. Steady accepted-rate tracking code is unchanged
  between the two images, so this is evidence of a different CM source-clock
  state rather than a demonstrated firmware regression. It remains an
  inference, not proof that the earlier source process was idle. Raw evidence
  is under
  `.scratch/019fb103-d707-7e01-adee-0ad1238d3115/pcm-48k-current-10m/`.
- On that fresh boot, a concurrent three-second preflight moved the CM to
  44.1 kHz with PCM classification, an open gate, 17-millihertz follow error
  and no event overflow. The following 120.000-second firmware bracket
  committed 9,466 blocks with maximum follow error 123 millihertz and every
  harmful counter delta zero. Its external observation deliberately remains
  failed: the simultaneous 130-second CM return was non-silent with no silent
  ten-millisecond window after onset, but contained six isolated one-frame
  impulses. Three resemble the characterized source fault; three approximately
  ±29.6k excursions have not been proven to fit the high-byte fingerprint and
  cannot be waived. Raw evidence, source and returned capture are under
  `.scratch/019fb103-d707-7e01-adee-0ad1238d3115/pcm-44100-current-120s/`.
- The 44.1 kHz **firmware bracket** is accepted. The overall 44.1 kHz physical
  waveform and trim-boundary evidence remain pending because the measurement
  path itself reproduces isolated corrupt samples without the Teensy. Do not
  weaken gate 7 or attribute a component fault from the direct-loop result.
- A bit-structure analysis of every recorded impulse then decoded the
  corruption class. All 35 events (the four direct-loop captures across both
  days, both altsets and both rates, plus the three flight-recorder events)
  damage only the high byte of one 16-bit sample: the low byte equals the
  neighbour interpolation exactly, every event sits on a full 0x00↔0xFF
  high-byte transition, and the flip masks are structured, top-bit-weighted
  and repeat across runs and receivers. That is parallel-datapath corruption,
  which excludes host software, the USB wire, the fibre and both capture
  paths; the remaining surface is the CM6206's internal playback FIFO/clock
  crossing (primary; the ~6.3% one-sample L/R misalignment episodes in the
  loop returns implicate the same block) or the Icron remote buffer
  (residual — the deferred non-Icron test still discriminates, with
  reproduction predicted). Full characterization, evidence table, quarantine
  fingerprint and single-unit-versus-systemic assessment:
  [CM6206 playback corruption characterization](../reference/cm6206-playback-corruption.md).

Raw artifacts remain under
`.scratch/019fad89-9c0a-7b51-9c04-c11431d981d1/`, especially:

- `rate-44100-120s-d009.jsonl`;
- `impulse-diagnostic/attempt-1/`;
- `impulse-transparent-dsp-v2/`;
- `cm6206-direct-loop-125s/`;
- `cm6206-direct-loop-48k-125s/`; and
- `cm6206-direct-loop-44k1-altset1-8ch-125s/`.

### Device evidence — 2026-09-02, external capture tool, image `e746cf952dd6`

The first gate runs with the fixed external capture tool
([`aius-rate-gate-capture-tool.md`](aius-rate-gate-capture-tool.md)
§Live-run results — 2026-09-02) on exact image
`e746cf952dd6+teensy41-optical`, U24 XL loop, each with its evidence digest:

- **Gate 2 (44.1 kHz PCM, 120 s): FAIL — open board defect.** The U24
  capture holds exactly one 128-frame zero run and the board's
  `clock.tx_underrun_frames` advanced by exactly 128 at ~19 s into the run;
  every other harmful delta is 0, 12 trims / 12 invalid windows,
  `http_client_drops` 0 (evidence-sha256
  `f67108c070fbeaf5fd5917190ffecf71345815e738ecef5f2c0e0c8759ade53e`).
  Tool and board agree:
  a single 2.9 ms dropout at 44.1 kHz that the 2026-09-01 tool could not see
  (its detector was blind at the ±410 stimulus). The flight recorder froze it
  on its own (`underrun` trigger, slot at 44100), but the slot was recycled
  by two later freezes before it was pulled, so no waveform survives; the
  run's status samples and the firmware are the evidence. `audio_rate_hz
  44100` is capture-clock-locked (self-referential) on this hardware and
  asserts nothing (design decision 2026-09-02, gate 2 above).

  **Root cause (2026-09-02, same day): the transmit-ring floor after a
  relock is producer/ISR phase luck, and in this run it was zero.**
  `clock_apply_desired_plan` flushes the ring to empty on a rate change
  (`firmware/src/optical_main.cpp:7884`, `aius_spdif_tx24_flush_preserve_diag`),
  and the gate's closed→open edge can only trim DOWN to
  `AIUS_TX_GATE_OPEN_TARGET_FRAMES` (128 on the `teensy41-optical` env;
  `tx_gate_actuate`, `optical_main.cpp:2165`) — nothing ever pads the ring
  up, so the pre-commit floor a session settles at is whatever phase the
  producer's first 512-frame burst had against the transmit ISR. The run's
  1 s status polls show it: `tx_fills` advanced 348.5 per 1.011 s poll, so
  each poll landed half a 128-frame block further into the 4-block producer
  cycle and the polled `tx_level_frames` walked 512, 384, 256, 128, 0 in
  pairs with an 8-sample period — an alias of the real 512→0 cycle, not a
  drain — with `tx_high_water_frames 512`, i.e. the bare PCM quantum on a
  zero floor (`max_clock_tx_level_frames 512` in the analysis). Reads of 0
  at 2.7 s, 9.8 s and 15.8–17.9 s are the empty interval widening as the
  clock following walked the producer's commit toward the ISR fill; at
  ~18.5 s the commit lost the race by one block: `tx_underrun_frames` +128,
  after which the floor ratcheted to 128 (`tx_high_water` 640, the polled
  level never read 0 again). The same mechanism produced the gate-4
  idle-gap underruns (+896, +768), the +256 at the 2026-09-02 14:24 UTC
  relock after the U24 re-auth (`gate_open_trims` +1 trimming 256, then the
  underrun 8.3 s later, recorder freeze on it), and the 48 kHz floor of
  1536 after 23 unsupported episodes. Fix: make the floor deterministic at
  the gate-open edge —
  [`aius-output-jitter.md`](aius-output-jitter.md) Phase F, built and
  flashed the same evening (`3570a216216d+teensy41-optical`).

  **Gate 2 re-run on `3570a216216d` (Phase F on board): PASS.** Same
  tool, same stimulus, `--expect-build 3570a216216d+teensy41-optical`:
  `continuous_output true`, no zero run in the U24 capture,
  `clock.tx_underrun_frames` delta 0, every other harmful delta 0, 12 trims
  / 12 invalid windows, `max_follow_error_millihz` 722, and the polled
  `tx_level_frames` never below **128** (min 128, max 512; `tx_high_water`
  640 = target + quantum) across 129 samples — the floor the fix promised
  (evidence-sha256
  `781250dd5d3e06972905473562b4eef9cf4b2706b035fe4e29c0b431fb011d23`).
  `gate_open_padded_frames` advanced 128 → 256 at the 44.1 kHz relock and
  `gate_open_trims` stayed 0. `audio_rate_hz 44100` is recorded, labelled
  self-referential, and asserts nothing.
- **Gate 7 (trim-boundary waveform, 48 kHz): PASS under the contract
  redefined 2026-09-02 (FAIL on the old wording; output clean either
  way).** `discontinuities_outside_invalid 0`, `capture_frame_gaps 0`, no
  zero run, held run, spike or phase slip in 120 s at a detector proven at the
  documented stimulus, while the board logged 12 trims / 12 invalid windows
  and none was visible (`invalid_intervals_frames []`) (evidence-sha256
  `87e9ef415af946d9320808b4391f6c81817593c797acf038f36f70bc55b0ae7b`).
  Owner ruling: the windows need not be externally visible (gate 7 above);
  the updated bench re-analyzes the saved evidence to `pass`, `failures []`.
- **Gate 4 (48→44.1→48, aplay stimulus, no replay leg): FAIL on the
  stimulus grounds already recorded; `old_rate_output_frames = 0` is now a
  real measurement.** Both rate changes stamped, both fiducials resolved
  against the recorded output (the output switches within one 256-frame
  block of the stamp, in both directions), 2 rate changes, 2 events
  (evidence-sha256
  `6dc5f172e0ad2ea91d83da54187b5649e865606dc8e7af22803dd5e12740fda8`).
  The bench's `transition-sequence` (no replay leg
  driven), `transition-unattributed-unsupported` (aplay idle gaps) and
  `event_overflows`-only `harmful-counter` (tool poll cadence) stood as the
  aplay-stimulus limitation, not a board finding. **Owner ruling (d), later
  the same day: the analyzer now judges the sequence the stimulus declared
  (F13 fixed the false unattributed episode; `event_overflows` reported, not
  gated, in `transitions`), and the same evidence re-analyzes to ONE failure,
  `transition-reset-count [2, 1]`** — the first blip cost two
  `service_requested_resets` (rate → unsupported at 2.10 s, carrier gone at
  3.15 s), a real threshold kept. A live `--with-replay` run on
  `b38e8448f5c8` is the remaining step (tool plan §Follow-on work).

### Resume checkpoint after the hypervisor restart

The hypervisor restart is an explicit investigation boundary. Resume in this
order:

Steps 1, 2 and 4 are complete. The clean external-source capture means step 3
was not triggered. The one-fibre playback-only test also reproduced corruption,
excluding the physical return and simultaneous USB capture. The direct,
non-Icron host discriminator is deferred; ~~it remains the next CM6206
diagnostic
but no longer blocks firmware tests~~ — CLOSED AS OVERTAKEN 2026-08-03
(owner decision, register entry): the CM6206 was physically retired from
the bench 2026-08-02 and the qualified clean source is the ESI U24 XL, so
the discriminator no longer gates anything. Continue step 5 with strict firmware
counters. A known isolated one-frame PCM source impulse may be counted and
quarantined only when recorder evidence shows no gap, counter delta, lock/rate
transition or multi-frame corruption, and the corrupt sample fits the
byte-lane fingerprint in
[the CM6206 characterization](../reference/cm6206-playback-corruption.md). Encoded and exact-waveform gates remain
pending when source corruption cannot be correlated at the raw input plane.

1. Confirm the Renesas controller remains exclusively passed through, the
   CM6206 enumerates as `0d8c:0102`, ALSA exposes one playback/capture device,
   and `PCM Capture Source` remains `IEC958 In`. Do not infer controller,
   extender or device fault from enumeration alone.
2. Leave CM optical output disconnected. Feed the CM optical input from an
   external 44.1 kHz PCM source with a continuous identical stereo tone and run
   the same at-least-120-second capture analyzer.

   - Clean capture localizes the corruption to CM USB playback/transmit or its
     full-duplex interaction.
   - Reproduced corruption localizes it to CM capture/USB transport, the
     Icron/Renesas/virtualization path or their interaction.
3. If capture-side corruption remains, collect the guest and hypervisor xHCI/
   USB diagnostics over the exact capture bracket before changing hardware or
   passthrough policy. Elevated commands are run by the user; this guest never
   has `sudo`.
4. Restore the CM6206→Teensy→CM6206 loop only after the capture-side
   discriminator. Reconfirm exact image
   `d009092556a3+teensy41-optical` before continuing acceptance.
5. The deliberate carrier/header mismatch gate passes. The 44.1 kHz encoded
   AC-3 run is firmware-stable but remains failed by characterized CM6206 source
   corruption. Finish external 32 kHz PCM, the 48→44.1→32→48 plus loss→replay
   transitions on a fresh event ring, then the exact trim-boundary waveform.
   Repeat encoded AC-3 with a source path that does not corrupt words. Gate 7
   must use a measurement path that the discriminator has shown does not
   independently create discontinuities. (Scope update: the 32 kHz legs
   were deferred to the long tail 2026-07-31 — gates 3/4 above — and the
   clean source/measurement path exists since 2026-08-02: the qualified
   U24 XL. Remaining in scope: 48→44.1→48 plus loss→replay transitions,
   the trim-boundary waveform, clean-source encoded AC-3, and the gate-1
   rerun, all on one exact image.)
6. Sequencing was deliberately re-scoped on 2026-07-30/31: the e2e-latency
   preliminary and Phase 1 work, including its PCM and AC-3 production rows,
   ran ahead of the remaining device gates by owner priority (PCM and AC-3
   first). Those rows consume interim clock evidence through their own
   settled brackets; final consumption of this plan's clock/status evidence
   still requires the six in-scope gates (gate 3 deferred to the 32 kHz
   long tail). Once they pass, update this plan, commit
   that device-evidence boundary, and continue `aius-e2e-latency.md` Phase 2.
   The four-hour AC-3 gate remains deferred until the OTA investigation is
   complete. (OTA complete 2026-08-01, `67c44a381`; the owner unblocked
   the four-hour gates 2026-08-04 after the WDOG-defect soak — the run now
   waits only on this plan's in-scope device gates and scheduling.)

Two acceptance realities are recorded rather than implied. First, the
in-scope gates must clear on one exact committed image: the passes so far
are split across `0a6f3a6f3480`, `d009092556a3` and `01477755955b`, and the
flashed lineage has since advanced to the observability image, so every
already-passed gate re-runs on the final candidate image. Second, gate 1's
trim-cadence bound (no more than two post-settle trims) is unpassable under
this CM6206's active-playback clock wander (ten values spanning 6.3 Hz, 53
post-settle trims in 600 s with a clean audio path). The bound is not
weakened here. The designated resolution path is the external-source
option: the incoming ESI U24 XL (32/44.1/48 kHz, optical S/PDIF, internal
clock master) runs a ten-minute carrier observation on arrival; a steady
single-value carrier makes it the gate-1 source and a candidate clean
source for gate 7 and encoded AC-3, while a CM-style multi-value hop
reopens the decision (ingress-slope correction term or an owner-approved
bound revision), tracked in
[`aius-open-work-register.md`](aius-open-work-register.md).
**Resolved 2026-08-02: the observation ran on arrival night — 311
samples over 630 s under active USB playback read a single carrier
value, 48,020,342 mHz (+424 ppm), span 0, `trim_updates` 0 — so the
U24 XL is the gate-1 source. Its optical self-loop discriminator
brackets also qualified it clean (bit-exact) at 44.1 and 48 kHz;
32 kHz self-loop FAILED with left-subframe corruption (register entry
carries the signature), so gate 3's long-tail source question stays
open. Gate 1 reruns with the U24 XL on the final candidate image.**
(The CM6206 was physically retired from the bench 2026-08-02 and the
CM-era discriminators closed as overtaken 2026-08-03 — register entry.
Where gates 2, 5 and 7 above name the CM6206, the source is now the
qualified U24 XL.)

**Gate-1 rerun on the final image FAILS at 52 post-settle trims
(2026-09-01).** The pending gate-1 rerun ran on the deployed image
`3824579` with the U24 XL as source, twice — free-running and under active
48 kHz host playback (aplay confirmed streaming for the whole run). Both
runs gave an identical **52 post-settle trims** (bound ≤ 2), max follow
error 0.74 / 1.07 Hz, ingress slope ~0, and every harmful counter 0
(tx_underrun, overruns, output_drops, unsupported_episodes, watchdog).
594/594 samples each — the acceptance runner's new transient-drop retry
let the 10-minute gate complete without aborting. Active vs free-running
playback made **no difference**, so the 52 trims are the carrier's inherent
wander as this image's DPLL tracks it, not the aplay content. This
contradicts the 2026-08-02 line above, which was a carrier-value
observation (span 0, `trim_updates` 0) on an earlier image, not a gate-1
analyzer run on `3824579`. Unresolved, needs bench/firmware investigation:
(a) a trim-cadence change between the qualified image and `3824579` — A/B
the two images with the same source before blaming the source; (b) U24
degradation since 2026-08-02 (the source-qualification README's
marginal-power-stage / re-enumeration caution); (c) the ≤2 bound being
unrealistic for a ±0.5 ppm crystal source, which the owner-approved
bound-revision path already anticipates. Do not weaken the bound without
that A/B. **No evidence artifact is retained**: both runs were written to
session scratch on 2026-09-01 and `.scratch/` is GC'd after 7 days, so the
numbers above are as the analyzer reported them at the time, not a
re-analyzable `--evidence` JSONL. Write the A/B rerun's evidence somewhere
durable before it is used to settle the bound.

If the 48 kHz slope exceeds 0.25 words/s, Phase 2 is not complete. The next
mechanism is a slow correction term driven by ingress slope into the same
numerator actuator; the acceptance bound does not weaken.

### Device evidence — 2026-09-03, gate-1 trim-cadence image A/B (completion plan Phase 3)

Both legs ran on 2026-09-03. Verdict: outcome (b) — the two images show the
same trim cadence on the same source, so the failure is not a regression in
`405ce97..25109fc`, and D4 goes to the owner with the measured span.

**Leg A — deployed image `25109fca3f97+teensy41-optical-slot-a` (built
`2026-09-03T18:01:32Z`, A/B chooser, `ab.slot 2`, `ledger_valid true`):
FAIL, 53 post-settle trims.** `aius_rate_bench.py run 48k-10m --target
192.0.2.66 --expect-build 25109fca3f97+teensy41-optical-slot-a` (the exact
`build_revision`, since the analyzer compares the whole string), started
`2026-09-03T18:19:23Z`, U24 XL as source, free-running 48 kHz, bench (no
host playback). 594/594 samples over 600.0 s, 0 transport retries; the only
failure is `trim-cadence` (53 after the 30 s settle, 56 whole-run); max
follow error 666 mHz (bound 1,000); ingress slope -0.038 words/s; every
harmful counter 0 (`tx_underrun_frames`, `overruns`, `output_drops`,
`unsupported_episodes`, `clock_faults`, `watchdog_early_resets`,
`network_restarts`). Evidence
`.scratch/ca959a0b-d32c-48a4-8a8d-ad0634c7ab96/gate1-legA.jsonl`, sha256
`ebc7a4c9ac3e915a0bf9944485dbbb90c4217586513a949ce60a7b17bcda67e7` (7-day
GC — copy it somewhere durable before the bound is settled on it). Same
number as the two 2026-09-01 runs on `3824579` (52, 52): the cadence is
stable across images from `3824579` to `25109fca3f97`.

What the raw samples add (offline pass over the same JSONL): trims land
every 10.8 s on average (55 gaps, 9.1–14.2 s) — the tracker's
`AIUS_CLOCK_TARGET_INTERVAL_MS` = 10 s ceiling
(`firmware/lib/aius_clock/src/aius_clock.c:17`), i.e. the 1 s window mean
sits ≥ 50 mHz (`AIUS_CLOCK_TARGET_HYSTERESIS_MILLIHZ`, `:18`) from the
applied target at essentially every eligible window. The settled
`carrier_rate_millihz` reads **9 distinct values** between 48,019,644 and
48,020,761 mHz (span 1,117 mHz) in ~140 mHz steps — the measurement's
quantum — hopping between adjacent bins from sample to sample; `trim_ppm`
walks 413–428. The 2026-08-02 observation on the 2026-08-02-era image read
**one** value (48,020,342 mHz, the centre of today's spread) for 311 samples
over 630 s with `trim_updates 0`. So the A/B question is sharper than "trim
policy": does the old image read a steady carrier from today's source? If
it does, something between the images changed the carrier measurement or
its gating (not the trim policy: `firmware/lib/aius_clock/` and
`firmware/lib/aius_audio/src/aius_audio_clock.cpp` have **no commits** in
`405ce97..HEAD`; only `firmware/src/optical_main.cpp` changed there, and the
apply/trim block `:8288-8311` itself has none). If it hops too, the source
wanders and this is D4.

**Leg B — old image `405ce9768532+teensy41-optical` (built
`2026-09-03T17:13:33Z` from the detached scratch worktree at `405ce97`, hex
sha256 `7b70c1e3d997b9d59c8dfb3274f1bfa7884cd92ebfd2806f94d6ef320a76c2d3`,
HalfKay-flashed over the chooser): FAIL, 52 post-settle trims.** The flash
log reads `Verified 405ce9768532+teensy41-optical`; the board booted with
`ab null` and `reset_source 1`, from a pre-flash posture of
`25109fca3f97+teensy41-optical-slot-a`, `ab.slot 2`, `sequence 7`.
`aius_rate_bench.py run 48k-10m --target 192.0.2.66 --evidence
<scratch>/gate1-legB.jsonl --expect-build 405ce9768532+teensy41-optical`,
started ~60 s after boot, U24 XL as source, free-running 48 kHz, bench (no
host playback), mixer at 110,110; `validate_status` accepted the old
image's payload, so the raw-sample fallback was not needed. 545 samples over
600.008 s; **`trim_updates_after_settle` 52** (54 whole-run), and the offline
`trim_timeline.py` pass over the same JSONL agrees — 52 post-settle, 53 gaps
of 7.1–14.6 s, mean 10.9 s. Max follow error 680 mHz (bound 1,000); ingress
slope +0.031 words/s. The settled `carrier_rate_millihz` reads **8 distinct
values** between 48,019,644 and 48,020,622 mHz (span 978 mHz) in the same
~140 mHz steps, with `trim_ppm` walking 415–426 — against leg A's 9 bins over
1,117 mHz and 53 trims. The analyzer's other failures on this image are known
non-faults of it or of its weaker HTTP path, reported separately:
`clock.tx_underrun_frames` +2,176 (idle silence), `http_service_overruns` +2,
`audio_diag.service_budget_overruns` +1,
`audio_diag.runtime_epoch_budget_overruns` +18, 37 transport retries over 18
samples and `http_client_drops_delta` 11 (545 samples against leg A's 594 with
0 drops). `overruns`, `output_drops`, `clock_faults`,
`unsupported_episodes`, `watchdog_early_resets` and `network_restarts` are all
0. Evidence `.scratch/b63df0f9-d85c-412b-b65a-db884c1f77a0/gate1-legB.jsonl`,
sha256 `451c2748d22400e6e081b0a5032c07ee0508628d7e8a8dbb458217bf4c6a3f80`, and
`gate1-legB.log`, sha256
`d7128c5358eb37b7030932222eac537ff7b5704f571b4fe4527c782ba11e3923`. Both
legs' JSONL are kept durably (gzip, LFS) under
`firmware/test/fixtures/gate1-trim-cadence-2026-09-03/` with their
uncompressed digests in that directory's README, since D4 is settled on them.

**A/B verdict — outcome (b), D4 → owner.** Both images hop the carrier
estimate across adjacent ~140 mHz bins and trim at essentially every eligible
10 s window (53 on `25109fc`, 52 on `405ce97`), so the cadence failure is not
a regression introduced in `405ce97..HEAD` and there is nothing to bisect. The
reading below is the orchestrator's, not a proven mechanism: the settled
carrier estimate's quantum (~140 mHz, ≈ 3 ppb at 48 kHz) is larger than the
tracker's hysteresis, 50 mHz (`AIUS_CLOCK_TARGET_HYSTERESIS_MILLIHZ`,
`firmware/lib/aius_clock/src/aius_clock.c:18`), so a one-bin hop of the
estimate alone clears the hysteresis and earns a trim once the interval
ceiling of 10 s (`AIUS_CLOCK_TARGET_INTERVAL_MS`, `:17`) allows one; the ≤ 2
post-settle bound is therefore unreachable while the hysteresis sits below the
measurement quantum, whichever source is used. The 2026-08-02 single-value
observation (span 0 mHz, `trim_updates 0`) is contradicted on the nearest
committed tree to it, so it was either a different tree (the dirty 10:51 build
over `b8c2a74`) or a source in a different state that night; it cannot be
reproduced and is no longer cited as the baseline. D4 is the owner's:
(i) a source-qualified bound — raise the hysteresis to at least one
measurement bin, or bound the gate on carrier span or follow error instead of
trim count; (ii) a different source; (iii) keep the bound and accept gate 1 as
failing. The gate-1 `PASS` criterion does not move until D4 is ruled.

### Device evidence — 2026-09-03, one-image acceptance re-run on 92aae16706f5 (completion plan Phase 4)

Every in-scope device gate re-run 22:53–23:21 UTC on ONE exact image,
`92aae16706f5+teensy41-optical-slot-a` (A/B chooser, `ab.slot 2`), with the
ESI U24 XL as source and capture at unity (`amixer -c 0 cset numid=5
110,110`); every run passed `--expect-build
92aae16706f5+teensy41-optical-slot-a`. Evidence is in session scratch
`.scratch/b63df0f9-d85c-412b-b65a-db884c1f77a0/` (7-day GC) with the sha256
of each artifact below.

- **Gate 6 — negative control: PASS.** Stimulus `gen_iec_stream.py --rate
  44100 --seconds 100 --keep-intermediates`, the raw `.iec` words played as a
  48 kHz carrier carrying a 44.1 kHz AC-3 header (`aplay -D hw:0,0 -t raw -f
  S16_LE -c 2 -r 48000`), judged by `aius_rate_bench.py run negative
  --carrier-rate 48000 --stream-rate 44100`. `mismatch_blocks` delta 5,628
  (4,680 post-settle), `committed_output_blocks_delta 0`,
  `tx_underrun_frames` 0, every harmful delta 0, 2 trims / 2 invalid windows —
  the mismatched stream is counted and muted before any output state advances.
  Evidence `gate6-negative-run3.jsonl`, sha256
  `83b556b1f3f44e302c46c77aad2481aaada3cb5360835395ff27a5aa1e751a8f`. Runs 1
  and 2 (`5e514f7f…`, `16fda8f1…`) failed only because the bench started after
  `aplay` had already finished — inter-call latency, not a board finding; the
  board had counted every played block (`mismatch_blocks` +7,746 ≈ 41.3 s ×
  187.5 blocks/s). Run 3 coupled `aplay` and the bench in one script, which is
  the recipe.
- **Gate 5 — encoded 44.1 kHz AC-3: PASS.** U24 IEC-passthrough smoke first
  (48 kHz `surround51_ident`, 10 s): three status reads all `format 2`,
  `pipeline_diag.class 2`, `codec_pc 1`, `parse_rejects 0`, `decode_rejects 0`
  (`gate5-smoke-status.jsonl`, sha256 `7ef83ef5…`). Then the
  `firmware/tools/ac3_vectors/README.md` recipe — a 130 s 44.1 kHz AC-3 IEC
  61937 wav played at unity, `aius_rate_bench.py run encoded --rate 44100
  --codec ac3`: 120/120 settled samples read `format 2` / class 2 / `codec_pc
  1`, `clock.state tracking`, `stream_rate_hz 44100`, `nominal_rate_hz 44100`,
  `carrier_rate_millihz` 44,118,147–44,118,986 and `output_rate_millihz`
  44,118,359–44,118,889 (carrier, decoder and output nominal rates agree).
  `mismatch_blocks` delta 0, `rejects_unsupported 0`, `rejects_malformed 0`,
  `resyncs 0`, `bursts_ok` +3,447; every harmful delta 0;
  `max_follow_error_millihz` 627; 11 trims / 11 invalid windows;
  `max_clock_tx_level_frames` 1536. Evidence `gate5-ac3-44100.jsonl`, sha256
  `db963b47b6bbf438a3cb90f3b136d4afbb124b4d90b9de37526040120e352dfd`.
  Observed outside the gated window and not a gate failure:
  `tx_underrun_frames` +11,648 during the 10 s settle (the PCM 48 kHz →
  AC-3 44.1 kHz relock) and +11,136 after the stream ended — the closed-gate
  TX drain at a format-plus-rate edge that the negative analyzer already
  permits, not an open-gate underrun. Recipe note: after `aplay` exits the
  U24 holds a 44.1 kHz PCM carrier, so a 3 s 48 kHz tone was played to return
  the bench to 48 kHz before the next gate.
- **Gate 2 — 44.1 kHz PCM, 120 s: PASS.** `capture.py --scenario pcm-120s
  --rate 44100 --seconds 130` then `analyze pcm-120s`: `continuous_output
  true`, zero runs 0, `tx_underrun_frames` delta 0 and every other harmful
  delta 0, 11 trims / 11 invalid windows, `max_follow_error_millihz` 626,
  polled `tx_level_frames` 128–640, `gate_open_padded_frames` 1024,
  `gate_open_trims` 0, ALSA overruns 0. `audio_rate_hz` reads 44100 and
  asserts nothing (design decision 2026-09-02: self-referential on the U24 loop).
  Evidence `gate2/evidence.jsonl`, sha256
  `af1f68a0e08034d0abfcfd65f575ba6c1f619a46d4cabea5d186eeebea227d58`.
- **Gate 7 — trim-boundary waveform, 48 kHz: PASS.** `capture.py --scenario
  waveform --rate 48000 --seconds 130` then `analyze waveform`:
  `discontinuities_outside_invalid 0`, `capture_frame_gaps 0`,
  `invalid_intervals_frames []` against 12 trims / 12 invalid windows declared
  by the board (none externally visible, permitted by the 2026-09-02 ruling),
  held and zero runs 0, amplitude 409.37, every harmful delta 0,
  `max_follow_error_millihz` 487, `max_clock_tx_level_frames` 512. Evidence
  `gate7/evidence.jsonl`, sha256
  `80a2678eb8eab81592f840bf6ee8e662abef721caafa9067546bdcc05bd18b3b`.
- **Gate 4 — transitions 48→44.1→48 plus loss→replay: PASS.**
  `recorder_transitions.py --auth-file … --with-replay` then `analyze
  transitions` (a live replay leg, not a synthetic one): phases observed
  `48000, 44100, 48000, replay` = expected; `rate_changes_delta 2` with 2
  rate-change events; `transition_reset_deltas [1, 1, 1]`;
  `old_rate_output_frames 0` by the `fiducial (slots[].rate_change_frames)`
  method with `lower_bound false`, 1 slot captured (operator trigger, freeze
  on `reference-rate-tracking` at 6.83 s), both fiducials resolved and neither
  rolled off. `unsupported_episodes` delta 6, all inside closed-gate
  transition windows with 6 matching rate-unsupported events;
  `transition_tx_underrun_intervals []`; `au_abandons 0`, `tx_underrun 0`.
  `event_overflows_delta 13` is reported and not gated. Evidence
  `gate4/evidence.jsonl`, sha256
  `03a5f767f72bc9ceb5bf4b8cd47832d2cb2850278693d161805ad87ac82647e7`.
- **Gate 1 — 48 kHz, 10 minutes: FAIL on `trim-cadence` only.**
  `aius_rate_bench.py run 48k-10m`, 594 samples over 600.0 s: **53 post-settle
  trims** (56 whole-run; per-minute 6, 5, 6, 5, 6, 6, 5, 6, 5, 5; `trim_ppm`
  414–429), against the ≤ 2 bound. Everything else is green:
  `ingress_slope_words_per_s` 0.00205 (bound 0.25),
  `max_follow_error_millihz` 693, `tx_underrun_frames` 0, `clock_faults` 0,
  `unsupported_episodes` 0, `output_drops` 0, no service or epoch budget
  overrun, `watchdog_early_resets` 0, `event_overflows` 0,
  `http_client_drops` 0, `tx_level_frames` 128–640. Evidence
  `gate1-92aae16.jsonl`, sha256
  `7b738639e224fcd9fc103b5151dbd2f5f10456ec7351d72b83edd923ab600422`, filed
  durably as gzip under
  `firmware/test/fixtures/gate1-trim-cadence-2026-09-03/gate1-92aae16706f5-7b738639.jsonl.gz`
  — the third data point for D4 (53 / 52 / 53 post-settle trims across three
  images).

Board state at the end of the session: `92aae16706f5+teensy41-optical-slot-a`,
`ab.slot 2`, `clock.state tracking` at 48000; lifetime `au_abandons 1` is the
replay cut at gate 4's re-authorization, outside every evidence window and the
documented non-fault.

**Verdict — five of six in-scope gates PASS on one exact image; gate 1 accepted
2026-09-04.** Gates 2, 4, 5, 6 and 7 all pass on
`92aae16706f5+teensy41-optical-slot-a`; gate 1 failed only its trim-cadence
bound, which was the question D4 put to the owner (§Device evidence —
2026-09-03, gate-1 trim-cadence image A/B). D4 was ruled 2026-09-04: (i) raise
the hysteresis to 500 mHz (`65bb582`, re-run cut the count 53 → 19) then (b)
rebase the criterion, dropping the trim count as a source-unmeetable proxy
(§Device evidence — 2026-09-04). Gate 1 is accepted on the rebased criterion —
the saved evidence's only analyzer failure is `trim-cadence` — so Phase 2 is
`shipped`.

### Device evidence — 2026-09-04, gate-1 D4 follow-up on 65bb5823c835 (hysteresis 500 mHz)

D4 was ruled (i) 2026-09-04: `AIUS_CLOCK_TARGET_HYSTERESIS_MILLIHZ` 50 → 500 mHz
(aius `65bb582`), deployed over OTA. Gate 1 re-ran on the clean-stamp image
`65bb5823c835+teensy41-optical-slot-a`: `aius_rate_bench.py run 48k-10m
--expect-build 65bb5823c835+teensy41-optical-slot-a`, 594 samples over 600.008 s,
started ≥ 4 min after boot. **19 post-settle trims** (21 whole-run; per-minute
2, 2, 3, 4, 3, 2, 3, 0, 0, 2; gaps 9.1–151.9 s, mean 28.5 s) — down from 53 at
50 mHz, still above the ≤ 2 bound. Everything else green:
`max_follow_error_millihz` 798 (bound 1,000), `ingress_slope_words_per_s`
−0.014 (bound 0.25), every harmful/reset/refusal counter delta 0,
`committed_output_blocks_delta` 53,424, `tx_level_frames` ≤ 640,
`ingress_level_words` ≤ 256. Settled `carrier_rate_millihz`: 9 bins spanning
48,019,644–48,020,901 (1,257 mHz), `trim_ppm` alternating a 413–417 and a
425–429 cluster. Evidence filed as
`firmware/test/fixtures/gate1-trim-cadence-2026-09-03/gate1-65bb5823c835-3639fb7b.jsonl.gz`,
uncompressed sha256
`3639fb7be517548ef3f75bcca4462fbd76d27e88c0a7ba44f9ac6723872d3c7c` — the fourth
D4 data point (53 / 52 / 53 / 19 post-settle trims across four images).

**Reading.** The 500 mHz deadband removed the one-bin-hop trims (53 → 19), but
the settled estimate still spreads ~1.2 Hz over ten minutes, and the deadband
cannot grow past the 1,000 mHz follow-error bound (798 mHz already at 500 mHz).
The receiver's ~140 mHz estimate quantum makes the ≤ 2 post-settle trim count
physically unreachable on this source; the only remaining firmware lever is
averaging the estimate over the 10 s cadence interval before judging the
deadband, which the owner declined (no further code changes). The trims never
reach the output — gate 7 saw none externally visible — so the count is a proxy
the source cannot meet, not an audible defect.

**D4 next lever ruled 2026-09-04 (orchestrator's call, delegated by the owner):
option (b) — rebase the gate-1 criterion, no further firmware.** Gate 1 now
passes when the `48k-10m` analyzer reports its full verdict **minus** the
`trim-cadence` check: stable nominal 48 kHz, `max_follow_error_millihz` ≤ 1,000,
absolute ingress slope ≤ 0.25 words/s, ring levels in range, output committing,
and every harmful/reset/refusal counter delta 0. The post-settle trim count is
recorded but no longer gates. An offline re-judgment of the saved evidence with
`aius_rate_bench.py analyze 48k-10m --evidence <file>` confirms `trim-cadence`
is the **sole** entry in `failures`; every other check passes. The tool still
emits `verdict: fail` because the `> 2` trim-cadence line is retained in code
(`firmware/tools/net/aius_rate_bench.py:852`) as an advisory metric, kept
docs-only per the D4 (b) framing. Phase 4a R13 (2026-10-06) made it
advisory in code: the check records post-settle centre commits and never
fails.

**Gate 1 ACCEPTED on the rebased criterion.** With gate 1 accepted and gates 2,
4, 5, 6 and 7 passing on `92aae16706f5` (2026-09-03), every in-scope device
gate is satisfied and Phase 2 is `shipped`.

## Optional recovered-clock hardware

The i.MX RT1062 cannot internally route its recovered S/PDIF clock back to the
S/PDIF transmitter. It can expose `SPDIF_SR_CLK` at Teensy pin 40 and accept
`SPDIF_EXT_CLK` at pin 16. A production-board trace from 40 to 16, with
`TxClk_Source = EXT_CLK` and divide-by-two, could provide zero-control hardware
following.

That is an optional hardware optimization, not part of software completion.
It requires a hardware-spec pin-allocation decision and a mandatory PLL4
fallback while the receiver DPLL is unlocked.

## Outstanding-work audit

`doc-index --validate-outstanding-work` does not discover
`src/hardware/**/docs`; it reports `outstanding.snapshot.plan-absent`. The
required component-wide disposition is recorded manually:

- `aius-ac3-support.md` — `coordinate`: widen AC-3 rate preservation and deliver the clock prerequisite; its four-hour/by-ear gate remains separately scheduled after the OTA investigation verdict.
- `aius-capability-gate.md` — `coordinate`: preserve DTS decode semantics while accepting 32/44.1/48 output clocks.
- `aius-cost-model.md` — `unrelated`: no BOM or supplier change.
- `aius-dsp-config-api.md` — `coordinate`: preserve the single-quantise 24-bit chain and rebase its rate-dependent coefficients at the block boundary.
- `aius-hardware-spec.md` — `coordinate`: software completion needs no board change; the optional recovered-clock trace requires a later pin-allocation decision.
- `aius-network-hardening.md` — `coordinate`: preserve epoch/watchdog bounds, memory floors and the existing defect ledger.
- `aius-open-work-register.md` — `extend`: track this work until target acceptance closes.
- `aius-optical-prototype.md` — `coordinate`: extend its optical hardware evidence without replacing broader prototype gates.
- `aius-ota-upgrade.md` — `coordinate`: scheduling only; sample-rate implementation does not touch OTA, flash or OCOTP.
- `aius-replay-manager.md` — `coordinate`: replay remains exactly 48 kHz and must restore that clock before taking graph ownership.
- `aius-sd-config-web.md` — `coordinate`: preserve the existing status dashboard and API behavior while adding clock truth.
- `aius-tls-termination.md` — `coordinate`: keep its future latency budget based on measured production audio cost.

## Residual risks

- A whole-rate change interrupts the outgoing carrier. Device evidence must
  quantify reacquisition; the plan does not claim seamless source switching.
- Live NUM writes preserved carrier in the 1,857-second experiment but the
  exact boundary waveform remains unobserved; one-block invalid marking stays.
- `SRFM` jitter may require ingress-slope feedback. The 0.25 words/s gate decides
  this from production evidence. **Realised 2026-10-04 on the transmit side:**
  an in-deadband offset drains the TX ring to periodic underruns; Phase 3 adds
  the fill feedback.
- AC-3 widening is pinned by deterministic 32/44.1 kHz decode fixtures plus a
  44.1 kHz low-bitrate CRC-boundary oracle.
- The replay file format still has no rate metadata and remains 48 kHz only.

## Related documents

- [Aius firmware I/O reference](../reference/firmware-io.md)
- [Aius signal path](../reference/signal-path.md)
- [CM6206 playback corruption characterization](../reference/cm6206-playback-corruption.md)
- [Aius AC-3 support](aius-ac3-support.md)
- [Aius OTA firmware upgrade](aius-ota-upgrade.md)
- [Aius hardware specification](aius-hardware-spec.md)
