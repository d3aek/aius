---
title: Aius gate-4 replay leg and S/PDIF receive fix (2026-09-03) — handover
type: plans
scope: component
component: aius
status: in-progress
subtype: how-to
tags: [aius, firmware, handover, rate-gate, spdif-rx, replay]
updated: 2026-09-03
toc: true
---

# Aius gate-4 replay leg and S/PDIF receive fix (2026-09-03) — handover

**Transient baton, not a record.** Delete this document in the commit that lands the
last item under §Next action and §Pending decisions, after promoting anything durable
per the handover skill's retirement rule. Resumers: check §What landed against
`git -C git_modules/aius log` first; anything after it is invisible here.

This session closed the 2026-09-02 loose-end handover (retired in `b977488`): the live
gate-4 `--with-replay` run it left owed was run five times, found and fixed a board
defect and a tool defect on the way, took the owner's reset-count ruling, and ended with
the first green gate 4. The durable homes are
[`aius-open-work-register.md`](aius-open-work-register.md) (§Waiting on the operator,
§Open defects — runtime) and
[`aius-rate-gate-capture-tool.md`](aius-rate-gate-capture-tool.md) (§Live-run results —
2026-09-03).

## State — resume here

**Status.** DONE: the S/PDIF receive fix (RX pool corruption under replay), the
acceptance-reset fix (design decision (b)), the tool fix (evidence ends before the U24
restore), two new `input_diag` instruments, gate 4 GREEN (run 5), docs, the clean-stamp
flash. IN FLIGHT: nothing. OPEN: one queued `CLAUDE.md` promote; two runtime defects the
runs surfaced, recorded and not started (§Pending decisions).

### What landed (all pushed)

| Commit | Contents | Proof |
|---|---|---|
| `git_modules/aius` `baef793` | `CLAUDE.md` deployed-image line `b38e8448f5c8` (landed promote) | docs only |
| `git_modules/aius` `dc16e75` | `firmware/lib/aius_audio/src/aius_spdif_rx.h` (pure guarded stager) + `AiusSpdifRxHook` in `aius_spdif_sources.cpp` re-pointing the receive DMA vector; `input_spdif3.h` block statics protected; `optical_main.cpp` installs the hook and emits `input_diag.rx_dropped_halves` / `pool_blocks_used` / `pool_blocks_used_max`; `recorder_transitions.py` stops the poller before the U24 restore; optical-cutover re-pin + receive pin; clock-target case; README bullet; register entry; tool plan §Live-run results — 2026-09-03 | clock-target green; optical-cutover 33/1/1/2/6; itcm-placement 29 + 5 target guards; `teensy41-optical` SUCCESS, ITCM 190,956 / 5,652 free; live run 3: board back on PCM after replay + carrier return, `overruns 0`, pool 2/4 |
| `git_modules/aius` `00b3ba0` | (retired) handover named the clean-stamp image | docs only |
| `git_modules/aius` `1e4224a` | `CLAUDE.md`: deployed image `dc16e7522250`; "on the bench there is never audio to interrupt" (landed promote) | docs only |
| `git_modules/aius` `b977488` | `g_audio_ingest_since_reset` gates the acceptance reset (`optical_main.cpp`); cutover pin + three mutations; register (ruling (b), gate 4 green, two new runtime defects); tool plan runs 4–5; the 2026-09-02 handover deleted | optical-cutover 33/1/1/2/6; `teensy41-optical` SUCCESS, ITCM unchanged; live run 4 `[1,1,1]` + one replay-start underrun, run 5 `verdict pass` (digest `0cf5eeda…`) |
| upstream repository `3bd69d68e`, `84ef63032`, `ae1a4def9`, `c4b4cd3b0` | pointer bumps; `84ef63032` also `swagger.json` `InputDiag` + the three fields | `swagger.test.js` 11 green; prettier clean |

**Deployed image: `b977488b3080+teensy41-optical`** (flashed 2026-09-03 09:05 UTC from
the committed tree; healthy on boot: PCM tracking at 48 kHz, gate open, `overruns 0`,
`pool_blocks_used 2 / max 4`, `rx_dropped_halves 1`).

### Uncommitted tree state

`git_modules/aius`: clean. A `the release-promotion step` of `CLAUDE.md` is queued (deployed-image
line → `b977488b3080`): `git -C git_modules/aius status --short CLAUDE.md` clean means
still queued, dirty means it landed and wants a one-path `docs(aius)` commit plus a
upstream pointer bump. Do not restate the approval; check.

upstream repository root: every dirty path (`.claude/**`, `data/*.sqlite`, `docs/plans/almanac.md`,
`docs/plans/deploy-backlog-and-red-suite.md`, `scripts/CLAUDE.md`, `src/apps/tiro/CLAUDE.md`,
other submodule pointers, untracked ``, `.codex/`, …) is foreign.
`the repo tooling test` cannot compile the repo graph while the foreign untracked almanac
tests exist; run vitest directly (`pnpm exec vitest run test/swagger.test.js`).

### Next action

1. `git -C git_modules/aius status --short CLAUDE.md`; if dirty, commit it alone
   (`git -C git_modules/aius commit -F <msg> -- CLAUDE.md`), push, bump the pointer.
2. Then whichever defect the owner picks (§Pending decisions). For the replay-start
   underrun, start at `tx_gate_actuate()` / `aius_tx_gate_permitted()` in
   `firmware/src/optical_main.cpp` (checkpoint 7 opens the gate for replay on
   `g_clock_replay_ready`; the AUDIO checkpoint enqueues the first replay block later in
   the same epoch, after an SD primitive of ~6.2 ms, and the Phase-F pad is one
   128-frame block). Reproduction: `recorder_transitions.py --with-replay --phase-seconds
   2 --expect-build b977488b3080+teensy41-optical` (tones from `tone_gen.py --rate
   48000|44100 --seconds 5` into session scratch), then `aius_rate_bench.py analyze
   transitions --evidence <run>/evidence.jsonl`; it landed 1 run in 3.

### Decisions taken this session

- **Owner: run the live gate-4 now; on the bench there is never audio to interrupt.**
  In `CLAUDE.md` §Device access and OTA (`1e4224a`). A flash, sync, U24 de-auth or
  replay start needs no audio-interruption ask on the bench.
- **Owner: (b) — skip the redundant acceptance reset in firmware; thresholds untouched.**
  Why: with live ingress closed since the quiesce reset the pipeline is already fresh; the
  second reset only added a PCM-confirm mute, and a real source rate change paid it too.
- **The RX pool corruption is fixed at the receive ISR, not by gating the receiver's
  DMA**: the guarded stager is the vendor bug's fix whoever drives the graph. Gating the
  DMA while the output drives is a recorded proposal.
- **The evidence ends with the declared stimulus.** The U24 restore is bench
  housekeeping; the poller stops before it (README contract bullet).
- **Instruments over inference.** `pool_blocks_used` / `_max` and `rx_dropped_halves`
  are in `input_diag` (swagger `InputDiag`); the wedge was invisible without them.
- **Clean-stamp reflash after each firmware commit** so `CLAUDE.md`'s deployed line
  names a real commit; on the bench it is free.

### Traps

- The optical-cutover suite reads firmware sources as TEXT; re-pin to the current fact,
  never loosen. Its transmit checks slice `function_body(text, "void
  AiusSpdifTx24Hook::isr(void)")`; the receive pin slices `AiusSpdifRxHook::isr` and
  `install`; the acceptance-reset pin anchors on the exact three-line
  `reset_for_acceptance` text (a `text.count(...) == 1` guard names it if it moves).
- `firmware/scripts/build.sh teensy41-optical` is ~25 s and exits 75 when another session
  holds the flock; never clear it. Its memory report is the ITCM proof (headroom 5,652 B).
- `CLAUDE.md` is path-guarded: write to scratch, `the release-promotion step`; the
  promote CONSUMES the scratch file, so re-copy before a second edit.
- The tool's U24 re-auth resets the mixer to 70,70: `amixer -c 0 cset numid=5 110,110`
  after every run. The tool also PUTs the recorder config back (an SD write).
- A status read issued in the same turn as a background capture can land after the
  capture; read the evidence samples, not a concurrent `/api/status`, for run facts.
- No host suite compiles `optical_main.cpp`; the cross-build is its automated proof. The
  RX hook lives in `aius_spdif_sources.cpp`, which the clock-target host suite compiles
  under the fake target (the hook is `#if !defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)`).
- `rx_dropped_halves` reads 1 after every boot (the half before the first pair exists)
  and climbs by thousands during any replay with the carrier down; that is the receiver
  storm being dropped, not a fault.

### Findings and loose ends

All recorded in the register; listed here so nothing is inherited as a commitment:

- **Replay start can open the gate on an empty ring** (register §Open defects —
  runtime, 1 of 3 runs). Proposed (not agreed): open the replay gate only once the ring
  holds the target from replay data, or pad replay's gate-open to cover one replay
  primitive. Until then gate 4 flakes ~1 run in 3 on this code.
- **`au_abandons` +1 at every carrier return during replay** (`aius_pipeline.c:701`,
  `drop_staged_compressed_input()`): a PCM replay should have nothing staged. Proposed:
  one look, likely a false MALFORMED latch on the replay→live discontinuity.
- **Unlocked-receiver DMA storm** (~2,460 ISR entries/s while dark, `SPDIF_SRPC_
  CLKSRC_SEL(1)` fallback). Harmless now. Proposed: gate the receive DMA request while
  the output drives the graph.
- ITCM census / ldscript proposal, `final_replay_output_blocks_delta` naming, register
  length: unchanged proposals from 2026-09-02, in the register.

### Pending decisions

- **Which defect next?** Exact words: *"Gate 4 is green but flakes about one run in three
  on the replay-start open-gate underrun (a real threshold). Fix that next, look at the
  `au_abandons` latch at carrier return, or park both?"* Options: replay-start underrun
  (makes gate 4 repeatable; a checkpoint-7 gate-open ordering change, cross-build +
  live run as proof) / `au_abandons` (small pipeline investigation) / park.
  Recommendation: the replay-start underrun. Blocks: nothing but the next phase; this
  document retires once both are dispositioned or the owner parks them.
