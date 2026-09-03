---
title: Aius loose-end remediation (2026-09-02, late) — handover
type: plans
scope: component
component: aius
status: in-progress
subtype: how-to
tags: [aius, firmware, handover, itcm, dsp-config, recorder, rate-gate, spdif-rx]
updated: 2026-09-03
toc: true
---

# Aius loose-end remediation (2026-09-02, late) — handover

**Transient baton, not a record.** Delete this document in the commit that lands the
last item under §Next action and §Pending decisions, after promoting anything durable
per the handover skill's retirement rule. Resumers: check §What landed against
`git -C git_modules/aius log` first; anything after it is invisible here.

The 2026-09-02 review-fix handover was retired the same day and its loose ends moved to
[`aius-open-work-register.md`](aius-open-work-register.md) §Waiting on the operator.
The 2026-09-02 session worked that list; the 2026-09-03 session ran the one live step it
left (gate 4 with the replay leg), which found and fixed a board defect on the way.

## State — resume here

**Status.** DONE: everything in the 2026-09-02 list; the live gate-4 `--with-replay`
runs (three); the RX pool corruption under replay found by run 1, fixed
(`aius_spdif_rx.h` + `AiusSpdifRxHook`) and verified live on `baef793d0063`; the tool
defect (evidence polled through the U24 restore) fixed; two new `/api/status`
`input_diag` instruments. Nothing is in flight. One owner decision is open (§Pending
decisions); gate 4's green pass waits on it.

### What landed (all pushed)

| Commit | Contents | Proof |
|---|---|---|
| `git_modules/aius` `22fe1ae` … `bcf2233`, upstream repository `b91a507fc` | the 2026-09-02 series (recorder-auth rule, CONTRIBUTING sweep, SD fixture, H1 accepted, `staged_generation` at SD commit, ITCM pass 195,500 → 190,764 B) | see the register §Waiting on the operator |
| `git_modules/aius` `baef793` | `CLAUDE.md` deployed-image line `b38e8448f5c8` (operator-approved promote) | docs only |
| upstream repository `3bd69d68e` | pointer bump for `baef793` | — |
| `git_modules/aius` (this session's fix commit; see `git log`) | `firmware/lib/aius_audio/src/aius_spdif_rx.h` (pure guarded stager) + `AiusSpdifRxHook` in `aius_spdif_sources.cpp` re-pointing the receive DMA vector; `input_spdif3.h` block statics protected; `optical_main.cpp` installs the hook and emits `input_diag.rx_dropped_halves` / `pool_blocks_used` / `pool_blocks_used_max`; `recorder_transitions.py` stops the poller before the U24 restore; optical-cutover re-pin (transmit checks scoped to the transmit handler, receive-hook pin + self-test mutations); clock-target case `test_rx_stage_fills_only_an_empty_pair`; README contract bullet; register entry (fixed) + decision text; tool plan §Live-run results — 2026-09-03 | clock-target green; optical-cutover 33/1/1/2/6 green; itcm-placement 29 + 5 target guards green; `teensy41-optical` SUCCESS, ITCM 190,956 / 5,652 free; flashed `baef793d0063`, run 3 live: board back to PCM after replay + carrier return, `overruns 0`, pool 2/4 |
| upstream repository `84ef63032` | `swagger.json` `InputDiag` + the three fields; pointer bump to `dc16e75` | `swagger.test.js` 11 green; prettier clean |

The fix commit is `git_modules/aius` `dc16e75`. After it landed the board was reflashed
from the committed tree for a clean stamp: **`dc16e7522250+teensy41-optical` is the
deployed image** (2026-09-03 06:28 UTC; healthy on boot, `pool_blocks_used 2 / max 4`,
`overruns 0`). The queued `CLAUDE.md` promote names it.

### Uncommitted tree state

`git_modules/aius`: clean after the fix commit. `CLAUDE.md` has a queued `the maintainer
promote` (bench note: no audio to interrupt on the bench; deployed-image line) —
`git -C git_modules/aius status --short CLAUDE.md` clean means still queued, dirty means
landed and wants a one-path `docs(aius)` commit. Do not restate the approval; check.

upstream repository root: every dirty path other than `swagger.json` and the
`git_modules/aius` pointer is foreign (`.claude/**`, `data/*.sqlite`,
`docs/plans/almanac.md`, other submodule pointers, untracked ``, …).
`the repo tooling test` cannot compile the repo graph while the foreign untracked almanac
tests exist, so vitest is run directly (`pnpm exec vitest run <file>`).

### Next action

Take the reset-count decision (§Pending decisions), implement (b) if chosen (the
acceptance reset in `sample_rate_service()` is redundant when live ingress stayed
closed since the quiesce reset), rebuild, flash, and run
`firmware/tools/rate_gate_capture/recorder_transitions.py --with-replay
--phase-seconds 2 --expect-build <rev>+teensy41-optical` once more, then
`aius_rate_bench.py analyze transitions --evidence <run>/evidence.jsonl`. Expect
`verdict pass`. Record the digest in `aius-rate-gate-capture-tool.md` §Live-run
results and retire this document. Tone fixtures: `tone_gen.py --rate 48000|44100
--seconds 5` into session scratch; re-apply `amixer -c 0 cset numid=5 110,110`
after every run (the tool's re-auth resets the U24 mixer).

### Decisions taken (2026-09-02, still binding)

- `GET /api/recorder` auth: the route is right, the rule was wrong (`CLAUDE.md`).
- `dsp-config-default.json` keeps no runtime keys by design; `dsp-config-sd.json` is
  the wire example.
- `staged_generation` advances at SD commit, not at PUT accept.
- H1 accepted on its host proof.
- The optical-cutover OTA/recorder pin follows the pad-1 alias model.
- Registry guard: repo-root suites are first-class.
- ITCM pass scope: the three first-party libs only.
- Gate 4: adapt the `transitions` analyzer to the aplay-blip stimulus, thresholds
  untouched (ruling (d)).

### Decisions taken (2026-09-03)

- **Owner: run the live gate-4 now; on the bench there is never audio to interrupt.**
  Persisted to `CLAUDE.md` §Device access and OTA (promote queued): a flash, sync,
  U24 de-auth or replay start needs no audio-interruption ask while the board is on
  the bench; the cost-stating rule is for the installed listening-room posture.
- **The evidence ends with the declared stimulus.** The U24 restore is bench
  housekeeping; the poller stops before it. Its re-enumeration edge is not judged.
- **RX pool corruption is fixed at the receive ISR, not by gating the receiver's
  DMA.** The guarded stager is the vendor bug's fix regardless of who drives the
  graph; gating the DMA while the output drives is recorded as a proposal.
- **Instruments over inference.** `pool_blocks_used` / `_max` and
  `rx_dropped_halves` now sit in `input_diag`; the wedge was invisible without them.

### Traps

- The optical-cutover suite reads firmware sources as TEXT; re-pin to the current
  fact, never loosen. Its transmit-module checks now slice
  `function_body(text, "void AiusSpdifTx24Hook::isr(void)")` for the `audio_block_t`
  and `<< 8` pins; the receive pin slices `AiusSpdifRxHook::isr` and `install`.
- `firmware/scripts/build.sh teensy41-optical` is ~25 s incremental and exits 75 when
  another session holds the flock; never clear it. The memory report it prints is the
  ITCM proof.
- `CLAUDE.md` is path-guarded: write to scratch, `the release-promotion step`.
- A flash reboots the board and stops audio; on the bench that is free (owner).
- The tool's U24 re-auth resets the mixer to 70,70: re-apply 110,110 after every run.
- `recorder_transitions.py` restores the recorder config with a PUT after the run (an
  SD write, ~600 ms checkpoint-8 stall observed); expect `sd_diag.http_jobs` +1.
- No host suite compiles `optical_main.cpp`; the cross-build is the automated proof
  for its edits. The RX hook lives in `aius_spdif_sources.cpp`, which the
  clock-target host suite DOES compile under the fake target (the hook itself is
  `#if !defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)`).

### Findings and loose ends

- **The unlocked receiver's DMA storm** (~2,460 `audio_update_calls`/s while the
  carrier is down, six and a half times the block rate; `SPDIF_SRPC_CLKSRC_SEL(1)`
  falls the receiver back to `tx_clk`). Harmless since the fix; proposal (not
  agreed): gate the receive DMA request while the output drives the graph.
- **`au_abandons` +1 at every carrier return during replay** (`pipeline_last_failure
  3`, the interrupted replay unit dropped by `drop_staged_compressed_input`). Outside
  the declared stimulus now; a PCM replay should have no staged compressed input to
  abandon, so this is worth one look (`aius_pipeline.c:701`).
- **The double service reset per aplay blip** — §Pending decisions.
- ITCM census / ldscript proposal, `final_replay_output_blocks_delta` naming, register
  length: unchanged from 2026-09-02 (proposals, not agreed).

### Pending decisions

- **Reset count on gate 4.** Exact words: *"Three live runs read `[2, 1]`, `[1, 1, 1]`,
  `[1, 2, 1]`: an aplay blip costs one or two service resets depending on whether the
  carrier passes through an unsupported measured rate before the new nominal is
  accepted; the acceptance then resets a pipeline nothing has been ingested into since
  the quiesce reset. (a) allow ≤2 per transition for the declared aplay-blip stimulus
  in `transitions` only, or (b) skip the redundant acceptance reset in firmware and
  keep the threshold?"* Recommendation: (b). Blocks: gate 4's green pass, retiring this
  document.
