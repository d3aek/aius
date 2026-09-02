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
host-proven, and the firmware fixes were validated on the deployed board
(`http://192.0.2.66`, plain single-image posture, flashed over USB CDC). This document
carries the state; the per-topic plans carry the design.

## State — resume here

**Status.** Firmware fixes: DONE and on the board (`e746cf952dd6+teensy41-optical`).
Host tools: DONE, pull tool and rate bench validated live; rate-gate capture tool
validated live for gates 2 and 7, gate 4 blocked by a tool bug (below). OTA upgrade
page: DONE at host level, NOT yet synced to the SD card, device validation owed.
Latency budget: MEASURED, inside target. Plan docs: updated for the code changes;
the live results below are NOT yet promoted into the plans.

### What landed (all pushed to `origin/master` of `git_modules/aius`)

| Commit | Contents | Proof |
|---|---|---|
| `f6e2e35` | latency harness resolves its root after the submodule move | latency-runner 114 rows |
| `701791b` | `bench.py --expect-build` regression rows against a newer image | latency-runner 114 rows |
| `f7e1af0` | recorder library: epoch-history inheritance, thaw honours lease, clean settle, bounded deferral, pending config, `newest_frozen`, rate-change fiducials | recorder 27 + bank 22 rows |
| `d2d058c` | lease released on teardown, newest-frozen read default, status fields, defect 22 consistency, `ab.booted_slot` | http 44, optical-app 38, runtime-target teardown e2e; cross-build fits |
| `e746cf9` | DSP-config parser accepts `staged_generation` / `config_pending` on round-trip (found live: renderer refused the new document) | dsp-config + latency-runner green |
| `804ced5` | rate-bench retry visibility + budget, pull tool newest-frozen default and exit codes, `test_recorder_pull.py` registered | rate-runner 53 rows, registry green |
| (this commit series) | rate-gate capture tool fixes; OTA upgrade page container-absolute fix + docs + `CLAUDE.md` posture rule; this handover | rate-gate-capture 59 rows; sd-editor vitest 369; network-transport green |

upstream repository side (uncommitted at the time of writing, see below): `vitest.config.js`
include for `test/**`, `swagger.json` +
`swagger.test.js` for the new keys (11/11), submodule pointer bump.

### Device validation results (board `e746cf952dd6+teensy41-optical`, 2026-09-02)

- **Latency (e2e PCM production row, `bench.py --expect-build`)**: 823 frames /
  17.15 ms raw, 24/24 codes accepted, every zero-delta counter clean. Board share
  ≈ 15.3 ms after the 90-frame adapter, inside the ≤16 ms owner target
  (`aius-e2e-latency.md:998`) and below the standing 17.4 ms. Baseline on the
  previous image `fc674c60f06c` measured 1508 frames / 31.42 ms because that boot had
  ratcheted the transmit ring to 1152 frames (governor held off); after the fresh
  boot the ring peaks at 512. Evidence dirs: session scratch only (7-day GC).
- **Harmful-counter smoke (`pcm-48k-smoke`, 120 s)**: pass on both `d2d058c` and
  `e746cf9`, zero deltas, no transport retries, `max_clock_tx_level_frames` 512.
- **Newest-frozen read default (H5)**: after `POST /api/recorder/freeze`,
  `active_slot` 1 / `newest_frozen_slot` 0; `GET /api/recorder/data?offset=0&length=4096&generation=0`
  without `slot` → 200 / 4096 B (was a guaranteed 409). Note `freeze_generation`
  starts at 0. Pull tool default chose slot 0 "newest frozen slot reported by the
  device", 2 MiB in 32 chunks, `gaps=0`; afterwards `lease_leaks 0`,
  `deferred_epochs 0`, `lease_skips 0`.
- **Lease leak on teardown (H1)**: NOT provoked on device — no route triggers
  `AIUS_NET_ACTION_TEARDOWN`; it needs a physical link drop mid-pull. Proven by the
  runtime-target end-to-end test (real composition, borrowed body streaming when the
  link drops, mutation-checked).
- **Spurious CARRIER_LOSS on recycle (H2)**: NOT provoked on device (needs three
  frozen slots then a carrier drop via U24 USB de-authorisation; procedure below).
  Host-proven by probe-backed rows.
- **Defect 22**: `/api/status` and `/api/dsp/config` agree (`source: sd`,
  `config_generation 1`, `staged_generation 1`, pending false). The carrier-less
  staged branch was not exercised (needs the U24 de-auth + a PUT).
- **Gate 2 (44.1 kHz pcm-120s) with the fixed tool**: FAIL, honestly. The tool found
  exactly one 128-frame zero run in the U24 capture and the board's
  `clock.tx_underrun_frames` advanced by exactly 128 at ~19 s into the run. Tool and
  board agree: a real 2.9 ms dropout at 44.1 kHz. `audio_rate_hz 44100` is
  `capture-clock-locked` (self-referential, as the tool now says). Last week's PASS
  came from a detector blind at ±410.
- **Gate 7 (48 kHz waveform)**: `discontinuities_outside_invalid 0`,
  `capture_frame_gaps 0`, `correlated_invalid_windows 0`; the board logged 12 trims /
  12 invalid windows; the bench FAILS with "waveform invalid_intervals_frames must be a
  non-empty integer array". Contract question, not a tool defect (see Pending).
- **Gate 4 (transitions, `--phase-seconds 1.2`, no `--with-replay`)**: the tool refused
  with `no-transition-captured` (the fail-shaped path works), but wrongly: the board's
  slot 2 IS frozen by the operator freeze with `rate_changes 1` and fiducial
  `rate_change_frames [6885376]` / `stream_end_frame 15302656`. Cause: the tool's
  "newer than the pre-run baseline" check compares `freeze_generation` across slots,
  but the generation is per slot (every slot reads 0). See Findings.
- Incidental: the flight recorder captured the gate-2 underrun on its own
  (slot 1 `frozen`, trigger `underrun`, 44100) and a carrier-loss at the 44.1 kHz
  stimulus start (slot 0, `carrier_loss`).

### Uncommitted tree state

`git_modules/aius`: clean after this commit series except
`firmware/test/memory/fixtures/teensy41-optical.elf` (1788 → 129 B, a real tiny ELF,
foreign, untouched by this session — leave it).

upstream repository root (this session owns only these): `vitest.config.js`,
`swagger.json`, `test/swagger.test.js`, the
`git_modules/aius` pointer. Every other dirty path at the root (`.claude/**`,
`data/*.sqlite`, `docs/plans/almanac.md`, `src/apps/almanac/**`,
`src/services/almanac-server/**`, other submodule pointers, untracked
``, etc.) is foreign.

Session evidence (latency rows, smoke JSONL, gate 2/4/7 evidence dirs with sha256
lines, pulled WAV, signed container `e746cf9.aiuo`) lives in
`.scratch/0178356f-7996-445d-8aa2-c81dd27cbc1b/` and is GC'd in 7 days; copy anything
worth keeping under `firmware/test/evidence/` (gitignored) or cite digests in the
plans before then.

### Next action

1. Sync the fixed upgrade page to the card and validate resume on the board:
   `firmware/tools/aius-sd-sync.sh --device http://192.0.2.66 --www firmware/sd/www --token <.secrets/aius-sd-auth>`
   (both `upgrade.js` and `upgrade.html` changed; costs ~59 ms of underrun). Then in
   a browser against `http://192.0.2.66/sd/www/upgrade.html`: pick the signed
   container (sign fresh: `aius_ota_sign.py sign --image .pio/build/teensy41-optical/firmware.hex --secret firmware/config/ota-home-signing-key.sec --build-revision e746cf952dd6 --output <new path>`,
   no `--slot`), upload, kill the tab after a few chunks, reopen, re-upload, and check
   the first `PUT /api/ota/image` `Content-Range` starts at `/api/ota`'s
   `next_offset` (not 0); then `POST /api/ota/abort`. Do NOT activate.
2. Fix the gate-4 baseline bug in
   `firmware/tools/rate_gate_capture/recorder_transitions.py` (per-slot
   `freeze_generation`; compare the (slot, generation) pair or the
   `newest_frozen_slot` identity against the pre-run header), re-run gate 4, then
   record gate 2/4/7 results + digests in `docs/plans/aius-rate-gate-capture-tool.md`.
3. Provoke H2/defect-22 on device (owner-visible, ~1 min of no audio): freeze twice
   more so three slots are frozen while locked, write `0` to
   `/sys/bus/usb/devices/3-1.1/authorized` (U24 de-auth = carrier loss), confirm only
   the active slot fires `carrier_loss`, the older frozen slots keep their
   `freeze_generation`, and `slots_recycled` advances by 1 only; while carrier-less,
   PUT the round-tripped `/api/dsp/config` and confirm both documents report
   `config_pending` / `staged_generation 2`; write `1` back, then re-apply
   `amixer -c 0 cset numid=5 110,110` (the U24 resets its mixer on re-enumeration).
4. Commit the upstream repository side (`vitest.config.js`, swagger pair, pointer) with
   `.scratch/…/commit-msg-upstream repository.txt` if this session did not.
5. Promote the device results above into `aius-recorder-rolling-buffer.md`
   (Phase C/D evidence), `aius-e2e-latency.md` (the 823-frame row), and
   `aius-sample-rate-following.md` (gate 2's real 44.1 kHz dropout), then retire
   this document.

### Decisions taken this session

- **Deploy route is `scripts/flash_optical.sh` over USB CDC, plain `teensy41-optical`
  env.** The board reports `"ab": null`; the 2026-08-31 slot-agnostic chooser cutover
  was superseded on 2026-09-01 by plain-image HalfKay flashes. `CLAUDE.md` §Device
  access and OTA now states both postures. Re-cutover to A/B is owed but not
  scheduled.
- **Latency regression rows use `bench.py --expect-build <rev>`**, recorded with
  `expected_build_override: true`; the Phase 1 acceptance row stays pinned.
- **`GET /api/recorder/data` without `slot` targets the newest frozen slot** (409 only
  when nothing is frozen); the pull tool follows the same order (`--slot` →
  `newest_frozen_slot` → highest `freeze_generation` frozen → `active_slot`).
- **Settle window semantics**: a dirty block (own underrun delta or gap) resets the
  window; a fired block neither advances nor resets it. Header, plan and code agree.
- **`http_client_drops` is reported, not a harmful counter** (the retried drop never
  reaches the DAC); every rate-bench sample now carries `attempts`.
- **`staged_generation` / `config_pending` are accepted-and-ignored by the DSP-config
  parser** so GET documents round-trip (native renderer, bypass PUT).
- **Phase D of the recorder plan is `IN-PROGRESS`, plan `status: in-progress`** — its
  targeted-proof items are unevidenced and its own fix carried H1.
- **No fabricated evidence**: plan lines whose artifacts were never retained now say so.

### Traps

- `bench.py` refuses a dirty harness (`firmware/tools/latency`,
  `firmware/test/latency-runner`): commit before a formal row. `__pycache__/` is now
  ignored so it no longer counts as dirt.
- The transmit ring ratchets upward on each stimulus start while the governor is
  held off (`AIUS_TX_GOVERNOR_ENABLED=0`); a latency row taken late in a boot can read
  +10 ms of pure ring residence. Measure after a fresh boot or note
  `tx_level_frames` from the manifest.
- `freeze_generation` is per slot and starts at 0; a data read with the wrong
  generation is a 409 `recorder_state`, indistinguishable from "not frozen" by code.
- `/api/recorder` GET requires `X-Aius-SD-Auth` on this build (contrary to the
  "reads need nothing" rule in `CLAUDE.md`).
- `optical-cutover` suite fails at HEAD on a pre-existing `micros()` in `update()`
  (commit 225253d), unrelated to this series.
- ITCM has 1108 B free; the new cold recorder-bank helpers
  (`aius_recorder_bank_newest_frozen`, `_deferred_epochs`, `_lease_skips`,
  `_config_pending`, `aius_recorder_inherit_epoch_state`) landed in ITCM because the
  lib is ITCM-allowlisted and portable C has no `FLASHMEM`. A placement follow-up is
  owed before anything else grows.
- The U24 PCM playback mixer was at −20 dB at session start; benches were run at
  unity (`amixer -c 0 cset numid=5 110,110`). Re-check after any re-enumeration.

### Findings and loose ends

- `firmware/tools/rate_gate_capture/recorder_transitions.py` baseline check treats
  `freeze_generation` as global; gate 4 cannot pass on a board where every slot reads
  0. Proposed: compare `(newest_frozen_slot, freeze_generation)` before/after or
  require `rate_changes >= 1` on a slot whose `trigger.which == "operator"` and
  `trigger.at_ms` is later than the run start. Not agreed.
- Gate 4 run captured only one rate change (`rate_changes 1`) on slot 2 for a
  48→44.1→48 sequence at `--phase-seconds 1.2`; either the freeze landed before the
  second switch or the second switch did not stamp. Proposed: investigate with the
  header's fiducials after the baseline fix. Not agreed.
- `CLAUDE.md` says reads need no auth; `GET /api/recorder` returns
  `sd_auth`. Proposed: correct the rule or the route. Not agreed.
- `staged_generation` bumps at PUT accept, before the SD commit
  (`optical_main.cpp:5577`), so a PUT that fails to commit still advances it while
  `config_pending` stays false. Proposed: bump at commit. Not agreed.
- `CONTRIBUTING.md` still carries ~12 stale `…` paths (lines 20,
  47, 84-86, 124, 204, 214-219). Proposed: sweep. Not agreed.
- `test/sd-editor/fixtures/dsp-config-default.json` (the swagger `DspConfig`
  example) lacks `staged_generation` / `config_pending`. Proposed: refresh the
  fixture from a live GET. Not agreed.
- The gate-2 44.1 kHz 128-frame dropout at ~19 s is a real board defect, previously
  invisible. Proposed: open it under `aius-sample-rate-following.md`. Not agreed.
- The transmit-ring ratchet (governor held off by owner decision 2026-08-16) is what
  made the previous image read 31 ms; unchanged by this series. Proposed: nothing
  here; note for the governor decision. Not agreed.
- `firmware/test/memory/fixtures/teensy41-optical.elf` is dirty in the working tree
  (foreign). Proposed: its owner decides. Not agreed.

### Pending decisions

1. **Gate 7 contract.** "The board logs each PLL trim as an invalid window (12 in
   120 s) while the sensitive output detector sees no artifact at all. Should gate 7
   require the U24 capture to show those intervals (current bench contract, which
   fails), or should it pass when the output is continuous and the invalid windows
   are the board's own bookkeeping?" Options: (a) keep the contract, gate 7 stays
   red until the U24's handling of the validity bit is characterised; (b) redefine
   gate 7 as "no discontinuity outside the board-declared windows, and the windows
   need not be visible", which passes today. Recommendation: (b); the trims are
   sub-sample and inaudible by construction, and the capture proves it. Blocks: the
   gate-7 line in `aius-rate-gate-capture-tool.md` and `aius-sample-rate-following.md`.
2. **Gate 2 scope.** "`audio_rate_hz` is capture-clock-locked on this hardware and can
   never be an independent measurement; is gate 2's rate field to be dropped, kept as
   a self-referential sanity check, or measured via a channel-status decode?"
   Recommendation: keep it, labelled self-referential (as the tool now does), and
   judge gate 2 on continuity + harmful counters. Blocks: the plan's gate-2 wording.
3. **Physical validation of H1/H2.** "Do you want the teardown lease-leak fix (needs
   an Ethernet unplug mid-pull) and the recycle carrier-loss fix (needs the U24
   de-auth, ~1 min of silence) provoked on the listening-room board, or is the host
   end-to-end proof sufficient?" Recommendation: run the U24 de-auth procedure (item
   3 of Next action) when the room is idle; skip the unplug. Blocks: nothing in code;
   only the evidence lines.
