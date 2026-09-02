---
title: Aius review-fix series (2026-09-02) — handover
type: plans
scope: component
component: aius
status: in-progress
subtype: how-to
tags: [aius, firmware, handover, review, recorder, ota, rate-gate, latency]
updated: 2026-09-02
toc: true
---

# Aius review-fix series (2026-09-02) — handover

**Transient baton, not a record.** Delete this document in the commit that lands the
last item under §Next action and §Pending decisions, after promoting anything durable
per the handover skill's retirement rule. Resumers: check §What landed against
`git log` first; anything after it is invisible here.

An adversarial review of the week's `git_modules/aius` work (78cb738..9abb25b:
recorder rolling bank Phases A–D, rate-gate capture tool, rate-bench retry, OTA
upgrade page defect 39, defect 22) found 6 high / 7 medium findings. All were fixed,
host-proven, and the firmware fixes validated on the deployed board
(`http://192.0.2.66`, plain single-image posture, flashed over USB CDC). A second
session (same day) fixed three more gate-4 tool defects found by the first live
run, re-ran gate 4, synced the upgrade page to the card, and promoted the device
results into the plans. This document carries the remaining state; the per-topic
plans carry the design and the evidence.

## State — resume here

**Status.** Firmware fixes: DONE and on the board (`e746cf952dd6+teensy41-optical`).
Host tools: DONE; rate-gate capture tool validated live for gates 2, 4 and 7 with
digests recorded. OTA upgrade page: DONE — synced to the card (12/12 files,
2026-09-02 11:07 UTC) and resume validated on device (first PUT after a killed
tab starts at the device's `next_offset`). Plan docs: device
results promoted (recorder Phase C/D, latency Phase 3 regression row, sample-rate
gate 2/4/7 evidence, capture-tool live-run section). Session 3 (same day):
H2/defect-22 provoked on device and recorded (recorder Phase D, network-hardening
defect 22); the four owner decisions below taken; gate-2 dropout root-caused from
the retained status samples plus the firmware. Remaining: write the gate-7 /
gate-2 rewording (decisions 1–2) and the gate-2 diagnosis + new firmware phase
(decision 4) into the plans, then retire this document.

### What landed (all pushed to `origin/master` of `git_modules/aius`)

Session 1 (see the plans for detail): `f6e2e35`, `701791b`, `f7e1af0`, `d2d058c`,
`e746cf9`, `804ced5`, `82674e4`, `88b9203`, `4173c2b`. upstream repository side landed in
`f4ef0ad85` (vitest include, swagger pair, pointer).

Session 2:

| Commit | Contents | Proof |
|---|---|---|
| `ce84850` | gate-4 tool: per-slot `(slot, freeze_generation)` capture identity (F10), `aplay --samples` fractional phases + `stimulus-not-followed` diagnosis (F11), honest ring budget, `unresolved_fiducials` lower bound | rate-gate-capture 68 rows |
| `5555876` | gate-4 reducer: old rate from a 4096-frame pre-fiducial window (F12); plans: live-run results section with gate 2/4/7 digests, F10–F12, gate-2 defect opened in `aius-sample-rate-following.md` | rate-gate-capture 69 rows; offline reduction of the saved frames + second live run both resolve both fiducials |
| `a74a465` | recorder plan Phase C/D device evidence, latency plan 823-frame regression row, upgrade-page fresh-load resume vitest case, this handover | vitest `upgrade.test.js` 64 rows |

upstream repository side: pointer at `a74a465` landed in `0508d9174`.

Session 3 (docs only, no firmware or tool change):

| Commit | Contents | Proof |
|---|---|---|
| (this commit) | H2 device provocation → recorder Phase D; defect-22 two-PUT live proof → network-hardening; handover state | live board reads, saved in session scratch |

### Device validation results (board `e746cf952dd6+teensy41-optical`, 2026-09-02)

Session 1's results (latency 823 frames / 17.15 ms, smoke pass, newest-frozen read
default, lease counters) are now recorded in `aius-recorder-rolling-buffer.md`
Phase C/D and `aius-e2e-latency.md` Phase 3; gate 2 (real 128-frame dropout at
44.1 kHz) and gate 7 (output clean, contract open) in
`aius-rate-gate-capture-tool.md` §Live-run results — 2026-09-02 and
`aius-sample-rate-following.md` §Device evidence — 2026-09-02, with digests.

- **Gate 4 (fixed tool, `--phase-seconds 2`, no replay leg)**: captured slot 0 at
  `freeze_generation 1` (a recycled slot), `trigger operator`, `rate_changes 2`,
  both fiducials retained and resolved, **`old_rate_output_frames 0`,
  `lower_bound false`** (`evidence-sha256 6dc5f172…0fda8`, full digest in the
  plan). Bench: `fail` on the known aplay-stimulus grounds only
  (`transition-sequence` wants the replay leg; idle-gap `unsupported`;
  `event_overflows`-only `harmful-counter`). The 1.2 s run's
  `no-transition-captured` was F10 + F11 (the board never followed a 1 s phase:
  status polls show `unsupported` at 48000 throughout), not the board.
- **Recorder bank after the gate-4 runs**: `slots_recycled 2` (slots 0 and 1
  recycled to `freeze_generation 1` while locked, no spurious `carrier_loss`),
  `lease_leaks 0`, `deferred_epochs 0`, `lease_skips 0`, auto-triggers restored.
  Not the H2 provocation (needs a real carrier drop with three frozen slots).
- **SD sync**: `aius-sd-sync.sh` 12/12 `{"ok":true}`; served `upgrade.js` now
  byte-identical to the repo (`curl --upload-file` is bash-guard-denied; the
  script is the route). `tx_underrun_frames` read 3840 afterwards (was 1536
  before the session; the two gate-4 runs account for 896 + 768 of the delta in
  their idle gaps).
- **OTA upgrade-page resume (defect 39 fix, validated on device)**: headless
  Chromium via `the repo tooling tool playwright-inspect` against
  `http://192.0.2.66/sd/www/upgrade.html` with the signed container
  `e746cf9.aiuo` (570624 B, untagged), `window.fetch` wrapped to record every
  `Content-Range`. First run: `bytes 0-255/570624`, `bytes 256-4351/570624`,
  `bytes 4352-8447/570624`, … 12 chunks, then a page reload killed the in-flight
  PUT (`net::ERR_ABORTED`). Device after the kill: `state staging`,
  `staged_bytes 49408`, `next_offset 49408`, `total_bytes 570624`
  (container-absolute). Fresh page load, same container, start: **first PUT
  `bytes 49408-53503/570624`** — the device's `next_offset`, not 0 — then
  `53504-57599`, `57600-61695`. `POST /api/ota/abort` → 200; the page's next PUT
  got 409, it stopped with "Upload stopped at 60 KiB of 557 KiB -- the device is
  not in a state that accepts this step" (`upload:fail`), and the device read
  `idle`, `fault none`, `activation_available false`. Never armed, never
  activated. Transcript: `.scratch/9b96cdc1-…/inspect-ota-3.txt`; scenario
  `ota-resume.json` beside it (a `Scenario` object with an eval that sets
  `#token` — the runner's `localStorage` pre-seed did not populate the box).
- **H2 / defect 22 (session 3, 14:22–14:24 UTC)**: U24 de-auth with slots 0/1/2
  frozen → only slot 3 fired `carrier_loss`, slot 0 recycled to generation 2,
  `slots_recycled` 4→5, no lease counters moved; two carrier-less PUTs advanced
  `staged_generation` 1→2→3 with `config_generation` pinned at 7 and both
  documents agreeing; on re-auth the model applied (`config_generation` 8). The
  relock itself underran 256 frames 8.3 s later (`gate_open_trims` +1, +256
  trimmed) and the recorder froze the recycled slot 0 on it — live evidence for
  the gate-2 diagnosis below. Full numbers in the two plans.
- **Gate-2 dropout root cause (session 3, from `gate2-e746cf9/evidence.jsonl` +
  firmware)**: `clock_apply_desired_plan` flushes the transmit ring to 0 on a
  rate change (`optical_main.cpp:7884`) and the gate's reopen can only trim
  DOWN to `AIUS_TX_GATE_OPEN_TARGET_FRAMES` (128 on this env,
  `tx_gate_actuate` `:2165`), so the pre-commit ring floor after a relock is
  producer/ISR phase luck. In gate 2 it was 0 (`tx_high_water 512` = the bare
  512-frame PCM quantum; the polled level is an alias of the 4-block cycle —
  `tx_fills` advances 348.5 per 1.011 s poll, so each poll walks half a block,
  giving the 8-sample "sawtooth"); 18 s of clock following walked the commit
  one block past the ISR fill → 128 frames of silence, floor ratchets to 128
  (`tx_high_water 640`). Same mechanism as the gate-4 idle-gap underruns and
  the 48 kHz floor at 1536 after 23 episodes. The frozen slot was evicted
  before this session (bank recycled twice), so no waveform; the status
  samples and the code are the evidence. Owner decision 4: record + open a
  firmware phase (deterministic floor at the gate-open edge, set at the
  pre-commit tap by pad-or-trim), no flash this session.

### Uncommitted tree state

`git_modules/aius`: clean after this commit series except
`firmware/test/memory/fixtures/teensy41-optical.elf` (1788 → 129 B, foreign,
untouched by both sessions — leave it).

upstream repository root: this session owns only the `git_modules/aius` pointer. Every other
dirty path at the root (`.claude/**`, `data/*.sqlite`, `docs/plans/almanac.md`,
`src/apps/almanac/**`, `src/services/almanac-server/**`, other submodule pointers,
untracked ``, etc.) is foreign. `the repo tooling test` cannot compile
the repo graph while the foreign untracked almanac tests exist (`frontend-test.
input.candidate`), so vitest was run directly (`pnpm exec vitest run <file>`).

Session 3 evidence lives in `.scratch/ffe0223b-b9bc-4249-9585-af24839a1a20/`
(`status-before/after.json`, `recorder-before/after.json`,
`dsp-config-before.json`, the PUT body). Session 2's lives in
`.scratch/9b96cdc1-b48a-4ff1-943a-37bbd052f84e/`
(gate-4 runs `gate4-ce84850/` and `gate4-run2/` with `evidence.jsonl`, the pulled
slot frames `gate4-ce84850-slot3.npy`, bench verdict JSONs, the OTA probe
`inspect-ota-2.txt`) and session 1's in
`.scratch/0178356f-7996-445d-8aa2-c81dd27cbc1b/` (gate 2/7 evidence, latency
rows, smoke JSONL, signed container `e746cf9.aiuo`, stimulus tones). Both are GC'd
after 7 days; the digests are in the plans.

### Next action

1. ~~Provoke H2/defect-22 on device~~ **Done 2026-09-02 14:22 UTC** (session 3;
   recorded in `aius-recorder-rolling-buffer.md` Phase D and
   `aius-network-hardening.md` defect 22).
2. Reword per decisions 1 and 2: gate 7 = "no discontinuity outside the
   board-declared windows; the windows need not be visible" (passes on the
   2026-09-02 evidence → flip the gate-7 verdict lines); gate 2 keeps
   `audio_rate_hz` labelled self-referential and is judged on continuity +
   harmful counters. Files: `aius-rate-gate-capture-tool.md` §Follow-on work +
   §Live-run results — 2026-09-02, `aius-sample-rate-following.md` §Device
   evidence — 2026-09-02.
3. ~~Investigate the gate-2 dropout~~ **Root-caused** (§Device validation
   results above). Owed: write it into `aius-sample-rate-following.md` §Device
   evidence — 2026-09-02 (replace "Cause not yet investigated") and open the
   firmware phase in `aius-output-jitter.md` (deterministic ring floor at the
   gate-open edge; target policy is the same knob as the held governor's).
4. Retire this document once 2–3 are done.

### Decisions taken

- **Deploy route is `scripts/flash_optical.sh` over USB CDC, plain `teensy41-optical`
  env.** The board reports `"ab": null`; `CLAUDE.md` §Device access and OTA states
  both postures. Re-cutover to A/B is owed but not scheduled.
- **Latency regression rows use `bench.py --expect-build <rev>`**, recorded with
  `expected_build_override: true`; the Phase 1 acceptance row stays pinned.
- **`GET /api/recorder/data` without `slot` targets the newest frozen slot**; the
  pull tool follows the same order.
- **Settle window semantics**: a dirty block resets the window; a fired block
  neither advances nor resets it.
- **`http_client_drops` is reported, not a harmful counter.**
- **`staged_generation` / `config_pending` are accepted-and-ignored by the
  DSP-config parser** so GET documents round-trip.
- **Gate-4 capture identity is the per-slot `(slot, freeze_generation)` pair**, not
  a generation compared across slots; **phases are played by frame count**; the
  ring budget is `(phases − 1) × phase + 1 s`; the old rate comes from a 4096-frame
  window before the fiducial (F10–F12 in the tool plan).
- **Phase D of the recorder plan stays `IN-PROGRESS`** on the unevidenced items
  listed in its last bullet.
- **No fabricated evidence**: plan lines whose artifacts were never retained say so.
- **Owner decisions 2026-09-02 (session 3)**: (1) gate 7 is redefined as "no
  discontinuity outside the board-declared windows; the windows need not be
  externally visible"; (2) gate 2 keeps `audio_rate_hz` labelled
  self-referential and is judged on continuity + harmful counters; (3) H2 was
  provoked on device (done); (4) the gate-2 dropout gets its diagnosis recorded
  and a firmware phase opened, with no flash this session.

### Traps

- `bench.py` refuses a dirty harness: commit before a formal row.
- The transmit ring ratchets upward on each stimulus start while the governor is
  held off; measure latency after a fresh boot or note `tx_level_frames`.
- `freeze_generation` is per slot and starts at 0; a data read with the wrong
  generation is a 409 `recorder_state`, indistinguishable from "not frozen".
- `/api/recorder` GET requires `X-Aius-SD-Auth` on this build (contrary to the
  "reads need nothing" rule in `CLAUDE.md`).
- A 1 s stimulus phase is shorter than the board's follow latency (it sits in
  `unsupported` for the whole phase); 2 s phases follow within ~1.2 s.
- The board stamps `rate_change_frames` at block granularity ~400 frames *after*
  the output has switched; the blocks adjacent to the stamp are unclassifiable.
- `optical-cutover` suite fails at HEAD on a pre-existing `micros()` in `update()`
  (commit 225253d), unrelated.
- ITCM has 1108 B free; the cold recorder-bank helpers landed in ITCM. A
  placement follow-up is owed before anything else grows.
- The U24 PCM playback mixer resets on re-enumeration; benches run at unity
  (`amixer -c 0 cset numid=5 110,110`). It read 110,110 at session-2 start.
- `curl --upload-file` is bash-guard-denied; card uploads go through
  `aius-sd-sync.sh` (which uploads all 12 files unconditionally).
- `playwright-inspect` scenario files are a `Scenario` object
  (`{"localStorage": …, "steps": [...]}`), not a bare step array.

### Findings and loose ends (not agreed, owner's call)

- `CLAUDE.md` says reads need no auth; `GET /api/recorder` returns
  `sd_auth`. Proposed: correct the rule or the route.
- `staged_generation` bumps at PUT accept, before the SD commit
  (`optical_main.cpp:5577`), so a PUT that fails to commit still advances it while
  `config_pending` stays false. Proposed: bump at commit.
- `CONTRIBUTING.md` still carries ~12 stale `…` paths (lines 20,
  47, 84-86, 124, 204, 214-219). Proposed: sweep.
- `test/sd-editor/fixtures/dsp-config-default.json` (the swagger `DspConfig`
  example) lacks `staged_generation` / `config_pending`. Proposed: refresh from a
  live GET.
- The transmit-ring ratchet (governor held off by owner decision 2026-08-16) made
  the previous image read 31 ms. Proposed: note for the governor decision.
- A gate-4 *pass* is unreachable with the aplay stimulus (idle gaps between phases);
  needs a source that cuts the carrier between phases or an analyzer decision
  (already recorded in the tool plan).
- `firmware/test/memory/fixtures/teensy41-optical.elf` is dirty in the working tree
  (foreign). Its owner decides.

### Pending decisions

All four taken 2026-09-02 (§Decisions taken, last bullet). Still open, not yet
asked: whether H1 (teardown lease release) needs the physical Ethernet unplug
mid-pull or the host end-to-end proof stands; the recorder plan's Phase D lists
it as not evidenced on device.
