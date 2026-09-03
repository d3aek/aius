---
title: Aius outstanding work — completion plan (2026-09)
type: plans
scope: component
component: aius
status: in-progress
tags: [aius, firmware, backlog, bench, register]
updated: 2026-09-03
toc: true
---

# Aius outstanding work — completion plan (2026-09)

Every open item on aius as of 2026-09-03, in the order it is worth taking, with
the disposition each one gets: a fix, a bench proof, a decision with a default,
or a record correction. The plan is worked to completion by an orchestrator
driving background implementers; it is done when every phase below is marked
SHIPPED and the [open-work register](aius-open-work-register.md) carries no
entry this plan does not account for. Inventory sources: the register, every
`status: in-progress` / `draft` plan under `docs/plans/`, the
[network-hardening](aius-network-hardening.md) defect ledger, and the rate-gate
tool plan's follow-on list, swept 2026-09-03 and adversarially reviewed the same
day (17 findings folded; the review's corrections are cited inline as "review").

## Ground rules for this plan

- **One board, one bench.** Every phase with a bench leg serializes on the
  deployed board at `http://192.0.2.66`; a flash reboots it and stops audio
  (no audio-interruption ask on the bench, owner 2026-09-03). Firmware phases
  share one build tree; `firmware/scripts/build.sh` holds a flock and exits
  75 when another build is running — wait, never clear it. Until Phase 5 the
  proof build is `teensy41-optical`; from Phase 5 it is the canonical
  slot-agnostic image `build_ab.sh` produces (ITCM figures are reported from
  that build).
- **Firmware phases before bench-only phases.** Every firmware change to the
  checkpoint-7 / clock / replay code in `firmware/src/optical_main.cpp`
  re-baselines every later bench leg, so firmware-bearing phases land (and
  clean-stamp flash) before the bench-only proofs that would otherwise have to
  be re-run.
- **OTA is the deploy path; USB HalfKay is for the cutover only** (owner,
  2026-09-03: "OTA will be the primary update method … you should be
  exercising the OTA deploy rather than default to USB"; and the A/B
  deployment "should never have been superseded"). Until Phase 5 restores the
  slot-agnostic A/B posture, no firmware phase deploys. After it, every
  firmware commit deploys as: build the canonical image, sign WITHOUT
  `--slot` (`firmware/tools/ota/aius_ota_sign.py`), stage + verify + activate
  + commit over HTTP (`firmware/tools/ota/aius_ota_flash.py`), then
  `--expect-build`. `flash_optical.sh` is used only for `recovery.hex`
  (chooser + canonical slot A) and, in Phase 3, for the old leg-B image —
  which overwrites the chooser, so leg B is followed by a re-cutover.
- **Clean-stamp deploy after every firmware commit.** The 12-char
  `build_revision` is the HEAD commit with no dirty marker; a dirty-tree build
  stamps its parent. Commit, rebuild, deploy, then run the bench leg with
  `--expect-build`.
- **Building an old commit never checks it out in the shared submodule tree.**
  The orchestrator creates a detached worktree under session scratch
  (`git -C git_modules/aius worktree add --detach <scratch>/aius-<sha> <sha>`),
  builds there, flashes the hex by path, removes the worktree. Sub-agents are
  never spawned with worktree isolation (repo rule); this is an orchestrator
  build step, not agent isolation.
- **Later implemented work supersedes a stale plan** (owner, 2026-09-03)
  unless the plan still carries real value or a proven defect. A plan whose
  premise was overtaken by work that shipped after it was written is marked
  `superseded` / `abandoned` with the pointer, not implemented for
  completeness. Every phase below that would build something first re-checks
  its premise in code at HEAD (file:line), and says which way it went.
- **Evidence ends up in the plan that owns the behaviour**, with the
  `evidence-sha256` digest; scratch evidence dirs are 7-day GC.
- **Decisions carry defaults where a default is the orchestrator's to take.**
  §Decisions marks each as default-able or owner-only. A default is applied
  when its phase starts and recorded as "applied by default"; an owner-only
  decision blocks only the phase that needs it.
- **Record corrections are commits too**, one per plan file, each citing its
  evidence. Commit SHAs written before the 2026-08-19 extraction
  (`70009a13f`, `c8f0c2571`, `cee8b46`, …) are upstream repository SHAs; resolve them in
  aius history with `git -C git_modules/aius log --grep=<subject>` before
  citing them as aius commits.
- **Sub-agents never run git.** The orchestrator commits and pushes each phase
  at its boundary (aius commit, then upstream pointer bump).
- **Doc validation.** `the repo tooling tool doc-index` does not walk
  `docs`, so front-matter here is checked against
  `.claude/rules/markdown.md` §Front-matter by listing the fields
  (`title`, `type`, `scope`, `component`, `status`, `updated`) in the commit
  message of each record-correction commit.

## Priority order and why

1. **Reconcile the record** (Phase 0) — working from a wrong ledger wastes
   bench time; several "open" items are already fixed or superseded.
2. **Code re-checks that need no bench** (Phase 1) — input recovery's premise
   at HEAD; decides whether that plan is superseded before anything is built.
3. **Restore the A/B posture; OTA becomes the deploy path** (Phase 5) — the
   first firmware phase, because every later firmware phase deploys through
   it and the owner wants the OTA path exercised.
4. **Cheapest bench items that retire markers** (Phase 2) — the recorder's
   three owed device items, ~30 min, on the A/B posture the recorder plan
   assumed.
5. **Gate 1** (Phase 3) — the open sample-rate acceptance failure; a firmware
   fix, if one lands, re-baselines everything after it.
6. **OTA IVT-bounds check** (Phase 6) — real value now that OTA is primary
   (D12); lands via OTA, before the acceptance re-run.
7. **One-image acceptance re-run** (Phase 4) — what actually unblocks the
   AC-3 four-hour and by-ear gates (review finding 8); gate 1 alone does not.
8. **Measurements that need the acceptance image** (Phase 7).
9. **Proposals the owner has not asked for** (Phase 8) — receive-DMA gating,
   recorded, not built by default.
10. **Hardware and owner-scheduled gates** (Phase 9).

The governor (output-jitter Phase E) is not a phase: D9 ruled it unwanted, and
its disposition is a record correction in Phase 0 (orchestrator).

## Phases

### Phase 0 — Reconcile the record ✓ SHIPPED 2026-09-03

**Shipped 2026-09-03** in four aius commits by concern: `6626c82` (register +
network-hardening ledger reconcile; open on both sides: 4, 21, 23, 25, 27, 40,
41), `e90ad5c` (plan statuses; input-recovery gate re-baselined), `d56352e`
(optical-prototype per D10; raw-bypass rows re-homed to H4), `a6f7e07`
(output-jitter A–D markers, Phase E closed with the governor retired per D9).
Left open on purpose: R-N5 (UX placement is an owner call), the
Ethernet-control-loss half of the optical-prototype state-table gate, the
`dts` bench mode (no artefact defines it), and ledger 21/25 (bodies read
resolved, no close statement). Two dated snapshots outside the named files
still carry stale 36/37/38 wording (`docs/reference/project-scale.md:167-171`,
`aius-ota-resilience.md:48-50,75-78`); the register is authoritative.

- outcome: every register entry and plan status below states what is true
  today. Each item is struck / marked FIXED with the pointer to the shipping
  phase and evidence, kept open with a one-line reason, or handed to a phase
  here. The ledger sweep is bidirectional: every unstruck register gist is
  checked against the network-hardening ledger AND every ledger defect opened
  since the register's last sweep is added to the register.
  - Register §Deferred `Deployed image revision stamp is a commit behind`
    (`aius-open-work-register.md:33-39`): strike — clean-stamp reflash is the
    standing convention and the deployed image is read from `build_revision`.
  - Register §Contracted `Input-side capture tap` (`:84-85`): keep as
    contracted follow-up, not v1; no phase here.
  - Register §Waiting `Loose ends from the 2026-09-02 review-fix series`
    (`:89-182`): re-check each unstruck sub-bullet; proposals (ITCM census /
    ldscript, `final_replay_output_blocks_delta` naming, register length) move
    to §Decisions here as standing proposals.
  - Register §Waiting `Bench first-signal test` (`:232-242`): the remaining
    steps 2–4 and 6 need an external source/sink — hand to Phase 9 as
    owner-scheduled.
  - Register §Open defects — runtime `Flight recorder freezes on the boot
    bring-up underrun` (`:442-457`): mark FIXED, two causes removed —
    [`aius-recorder-rolling-buffer.md`](aius-recorder-rolling-buffer.md)
    Phase A's settle window (`AIUS_RECORDER_SETTLE_BLOCKS 8u`,
    `firmware/lib/aius_recorder/include/aius_recorder.h:103`, at HEAD) and
    output-jitter Phase F's pad (`aius-output-jitter.md:409-413`, "all four
    slots stayed recording" across a fresh boot).
  - Register `OTA container verify is a one-epoch ~80 ms stall` (`:419-440`):
    hand to Phase 6 / D2 / D12; leave open until then.
  - Register `Boot-adjacent interrupts-masked stall` (`:747-760`): relabel
    OBSERVATION — no fix owed until recurrence.
  - Register `Periodic watchdog reset …` (`:762-771`): heading reads open, body
    records conviction, tripwire fix and a 10 h clean soak; reword to FIXED
    with the standing-state pointer.
  - Register `resilience drills (a)(b)` (`:472-477`), `production-key cutover`
    + formal `dts` bench mode (`:1083-1085`, `:1400-1403`), `FEC TX-spin
    decision / wild-address source / pad-2 straddle / AC-3 four-hour run
    scheduling` (`:1194-1199`), `user-interface.md captures + hero anchor`
    (`:1305-1310`), `ears round + lfe_gain A/B` (`:1448-1453`): each gets a
    one-line disposition — done (strike with pointer), owner-scheduled (hand
    to Phase 9), or superseded (strike with the rule-1 pointer).
  - Register §Larger backlog `B9 — the app dashboard retirement` (`:1454`):
    strike — done 2026-07-31 (upstream repository `c8f0c2571`; optical-prototype plan
    line 493).
  - Register nested ledger gists (`:1458-1638`): reconcile against
    [`aius-network-hardening.md`](aius-network-hardening.md) §Open defects —
    ledger 7 is fixed but unstruck (`:282`); gists 37/38 still read "NOT YET
    FLASHED" (`:1503`, `:1523`) and have been on the board for weeks; ledger
    defects 40 and 41 (`:2071-2113`, opened 2026-09-01) are absent from the
    register — add them.
  - Register `UX NICE-tier residue` R-N5 (`:1284-1290`): check the shipped
    replay page; strike or keep with the exact residue.
  - Register `Cost-model rows carry the old Cliff quotes` (`:1311-1313`):
    update [`aius-cost-model.md`](aius-cost-model.md) to RS PRO
    805-1671 / 805-1680 and strike.
  - Register `48 kHz gate 1 is unpassable under active CM playback` (`:1707`):
    heading still reads open; the source was requalified (U24 XL) — reword the
    heading to RESOLVED (source) and point the remaining gate-1 failure at
    Phase 3. The 32 kHz U24 left-subframe failure (`:1795-1808`) stays with
    the deferred 32 kHz leg (sample-rate gate 3).
  - Register `Open-gate 128-frame TX underrun, unexplained` (`:1680-1685`):
    re-read against output-jitter Phase F's mechanism (ring emptied at a rate
    change, the open trimmed down and never padded, drain landing between two
    open-gate samples). If it matches, mark EXPLAINED + FIXED by Phase F; if
    not, keep open with the residual.
  - Register `ITCM ingress-regression mechanism unestablished` (`:1823-1829`):
    keep as recorded residual risk (guard enforces the safe placement); no
    phase.
  - Plan statuses:
    [`aius-ota-slot-agnostic.md`](aius-ota-slot-agnostic.md) → D1 ruled
    2026-09-03: the re-cutover is scheduled (Phase 5); status stays
    `in-progress` with a line saying the 2026-09-01 plain-image flashes were
    an interim bench posture, not a decision.
    [`aius-optical-prototype.md`](aius-optical-prototype.md) → D10 ruled
    2026-09-03: descope the unshipped items (NTP, deterministic static
    fallback, UDP auth, raw-bypass rows, `:744-747`, `:816-819`, `:835-839`;
    never-run acceptance gates `:878-879`, `:884-886`) IF they serve no
    particular purpose — per item, one line: purpose today (what depends on
    it) or "descoped 2026-09-03: <reason>"; `status: shipped` only if nothing
    is kept.
    [`aius-rate-gate-capture-tool.md`](aius-rate-gate-capture-tool.md) →
    `shipped` (gates 2, 4, 7 pass on the fixed tool; every follow-on struck).
    [`aius-eac3-support.md`](aius-eac3-support.md): its Phase 5 is blocked on
    a real DD+ source on the bench (`:225-231`); leave `in-progress` with that
    one line as the status paragraph.
    [`aius-tls-termination.md`](aius-tls-termination.md): `draft`, parked
    2026-07-26; say so in one line, no phase.
    [`aius-hardware-spec.md`](aius-hardware-spec.md) §Open hardware questions
    (`:156-168`) says all three were resolved as planned → strike, nothing
    to Phase 9.
    [`aius-e2e-latency.md`](aius-e2e-latency.md) `dts-production:
    deferred-by-priority` rows (`:810`, `:820`) → Phase 7.
    [`aius-output-jitter.md`](aius-output-jitter.md) (orchestrator, after
    the docs agent returns): D9 ruled 2026-09-03 — the owner does not want
    the governor ("it introduces systematic latency with repeated triggers; I
    preferred a silent output but not a buzz/stall in the output stream").
    Phase E is marked `✓ SHIPPED` as a decomposition with the governor
    RETIRED: `AIUS_TX_GOVERNOR_ENABLED` stays 0, the pure rule and its
    `tx-governor` suite stay as host-proven code, and the standing behaviour
    on a stall is Phase F's silence pad. The acceptance evening (`:504-509`)
    and the pop verdict (`:65-78`) are re-read against that preference: a
    stall must produce silence, never a buzz; anything else is a defect to
    record. Both go to Phase 9 as owner-scheduled listening.
    [`aius-input-recovery.md`](aius-input-recovery.md) gate at line 359
    ("no firmware reaches the device until …", A/B slot wording) →
    re-baseline to the plain-image posture without changing its intent.
- seams: docs only — `docs/plans/aius-open-work-register.md`,
  `docs/plans/aius-cost-model.md`, targeted line edits in
  `docs/plans/aius-network-hardening.md` §Open defects, front-matter and
  status paragraphs of the plans named above. Statuses that stay as they are,
  said so in one line each: `aius-cost-model.md` (living model,
  `in-progress`), `aius-network-hardening.md` (`in-progress` while its ledger
  has open ids), `aius-e2e-latency.md` (`in-progress` until Phase 7 and D5).
- targeted-proof: each disposition cites its evidence (aius commit, phase,
  digest, or the code line); `git grep` for each struck item's title finds no
  other doc still calling it open; every ledger defect id in
  `aius-network-hardening.md` §Open defects appears in the register with the
  same open/closed state OR the mismatch is recorded in the register (known
  today: ledger 7, 19/20 (register `:1544-1552`), 34
  (`aius-ac3-support.md:136-137`) and 10 (`:456` vs heading `:418`) are
  closed but unstruck in the ledger; ledger 22 is FIXED at `:935` while the
  register `:1557-1558` reads open; ledger 39 is fixed at `:2002` while the
  register `:1510-1519` reads open — targeted line edits on whichever side is
  stale; the ledger is a 434 KB file, never rewrite it). Network-hardening
  phases 11, 12, 14b–f and 16 carry no status marker: mark `completed` where
  the body proves it, else list as unshipped at the heading. Output-jitter
  Phases A–D get their `✓ SHIPPED` markers where proven (D is closed per
  register `:41-48`). `aius-hardware-spec.md` §Open hardware questions
  (`:156-168`) says all three resolved: strike, not Phase 9. List the ids
  checked in the commit message.
- unit: parent-only (one docs agent may draft; the orchestrator reviews the
  diff line by line before committing)
- commit-checkpoint: one `docs(aius)` commit per plan file touched; the
  register lands in one commit.

### Phase 1 — Input recovery: premise re-check at HEAD, then finish or supersede ✓ SHIPPED 2026-09-03

**Step 2 shipped 2026-09-03** (aius `f0a2005`, swagger upstream repository `02b64b681`;
deployed over OTA in `25109fca3f97`): 32-deep paged event ring, dark-interval
record + per-phase gate-closed watermark, 14 `input_diag` keys, 0 B ITCM.
Delivery audit found six issues (a `/api/clear` false commit boundary, a
`cursor=UINT32_MAX` walk, plan text), all folded before commit. Live on the
wire 9 s after boot with a real 2.3 s boot interval recorded. Input recovery's
own Phases 2–4 stay gated on the instrumentation record showing a stuck state
in the field; that plan remains `in-progress` by design.

**Step 1 done 2026-09-03** (aius `f28a347`): all three stuck states are STILL
REACHABLE at HEAD `a6f7e07` — `g_clock_live_ready` is set only in `enqueue()`
after a landed block (`optical_main.cpp:1733`) and cleared at acceptance
(`:8130`) / `!carrier_ready` (`:8100`); the tracker's `carrier_ready` has no
timer (`aius_clock.c:266-292`); completed bursts never starve the class
(`aius_pipeline.h:71-77`); `g_clock_carrier_unsupported` clears only at
acceptance or carrier loss (`:8172-8183`). None of `dc16e75`, `b977488`,
`018c000`/`3570a21`, `4c9eb3d` closes a row (`dc16e75` closed a fourth wedge
not in the inventory). The plan is NOT superseded; step 2 (its Phase 1
instrumentation, no behaviour change) is real value and deploys over OTA after
Phase 5. Its Phases 2–4 stay gated on the instrumentation record.

- outcome: [`aius-input-recovery.md`](aius-input-recovery.md) is completed or
  superseded, decided on code evidence, not on failing to reproduce. Its
  three stuck states (`:193-210`) are code-derived; field evidence is indirect
  (`:221-235`). Step 1 (no bench): re-derive each mechanism at HEAD with
  file:line — e.g. `g_clock_live_ready` is still cleared at acceptance
  (`firmware/src/optical_main.cpp:8130`); `b977488` only skips a redundant
  reset; the RX fix is `dc16e75`; Phase F is `018c000` / `3570a21`; the replay
  floor `4c9eb3d`. For each state: still reachable at HEAD (name the path) or
  closed by a named commit. If all three are closed, mark the plan
  `superseded` (its shipped pure rule and suite stay) — done. Step 2, if any
  state is reachable: that plan's own "observe first" recommendation
  (`:151-153`) is the value — finish its Phase 1 (event-ring depth/page
  split, structured recovery record, `input_diag` watermark, swagger; no
  behaviour change), flash, and let the record run on the bench (a
  listening-room redeploy is owner-scheduled, Phase 9). Its Phases 2–4 (rule, wiring behind a flag defaulted OFF, live
  proof then enable) are gated on the Phase-1 record showing a stuck state in
  the field; until then they stay unstarted with that gate written into the
  plan. Its two pending decisions (`:143-174`: ceiling exception;
  `carrier_loss` trigger) go to §Decisions as D11.
- seams: step 1 none; step 2 as that plan's Phase 1 lists, at today's lines —
  `record_event()` (`optical_main.cpp:1235`), the production-gap block
  (`:1804-1813`), the `input_diag` serializer (`:3194`), swagger (upstream repository
  `swagger.json`); the source plan's own cites (`:1684`,
  `:7605`, `:7657`) are HEAD `:1733`, `:8130`, `:8100` — refresh them in
  step 1's commit.
- targeted-proof: step 1 — a per-state table (mechanism, HEAD file:line,
  verdict) written into the plan's inventory. Step 2 — that plan's Phase 1
  rows (watermark across gate-open/closed/transition sequences, swagger
  extractor row, ring-depth row for an OSD-rate burst), canonical build
  SUCCESS with ITCM reported (budget: ≤ 512 B of the 5,652 B headroom or stop
  and report), clean-stamp OTA deploy, one `/api/status` read showing the new
  fields.
- unit: parent-only
- dependencies: Phase 0; step 2 also Phase 5 (deploys over OTA)
- commit-checkpoint: step 1 is a one-path docs commit; step 2 is one firmware
  commit + swagger commit (upstream repository) + clean-stamp OTA deploy.

### Phase 2 — Recorder rolling-buffer Phase D: the three owed device items ✓ SHIPPED 2026-09-03

**Shipped 2026-09-03.** First pass on `f28a347d0a68` proved lease release on
every terminal and found a proven defect: the post-roll was never captured
(`AIUS_RECORDER_RECLAIM_EPOCHS` calibrated for ~115 epoch/s against a ~6,500
epoch/s loop) — fixed by wall-clock reclaim `AIUS_RECORDER_RECLAIM_MS 280`
(aius `25109fc`, +5 rows, ITCM 0 B), deployed over OTA, live
`postroll_captured 188 == 188`. Items 1 and 3 then PASS on `25109fca3f97`:
`park()` mid-POSTROLL proven (slot in `postroll` with 33 blocks when parked),
rehearsal `pass` with the bank `absent` and metadata retained, `bank_init()`
re-arming after reboot. Item 2's deferral half is owner-hands (the classifier
blocks the U24 sysfs write for agents; recipe in the recorder plan). The
recorder plan is `shipped`. Follow-up recorded in the register (proposal,
default implement): the rehearsal ends in a watchdog reset because its
READY_TO_RESET is emulated and the quiesce stops feeding WDOG1 — end it in
`reset_application()` like real activation. Serialized after Phase 6 (both
touch the OTA service in `optical_main.cpp`).

- outcome: [`aius-recorder-rolling-buffer.md`](aius-recorder-rolling-buffer.md)
  Phase D is SHIPPED. The second review pass already ran 2026-09-01
  (`:668-677`) and the H1 read-lease fix carries four terminal tests
  (`:702-705`); a third whole-diff pass is completeness work and is not done.
  What is owed is the three device items at `:753-755`: (1) `park()` entered
  mid-POSTROLL then reboot, (2) the read-lease-deferred recycle, (3) the
  rehearsal-activation re-verification of `park()` / `ota_psram_staging()`
  against the bank shape. D6 is already decided in that doc (`:695-697`:
  plain-image flash replaces the per-slot A/B constraint) — record, do not
  re-decide.
- seams: bench only; `/api/recorder` header + `firmware/tools/recorder/`
  egress tool; no source change expected.
- targeted-proof: for each of the three items the `/api/recorder` header
  before and after and the relevant counters (`slots_recycled`, lease state,
  `freeze_generation`), pasted into Phase D with the image hash; the phase
  heading flips to `✓ SHIPPED <date>`. Budget ~30 min plus the container
  signing + staging the rehearsal needs; item (2) may need a carrier-loss
  trigger (the U24 sysfs de-auth — the tool's path, or the owner's hand if the
  permission classifier refuses). A failure on any item is recorded as a
  new register defect and the phase stays IN-PROGRESS.
- unit: parent-only
- dependencies: Phase 0, Phase 5 (the recorder plan's Phase D assumed the A/B
  posture; the rehearsal-activation item needs a real inactive slot)
- commit-checkpoint: one docs commit.

### Phase 3 — Gate 1: trim-cadence image A/B, then root cause

- outcome: the 52-post-settle-trim gate-1 failure on `3824579`
  ([`aius-sample-rate-following.md`](aius-sample-rate-following.md)
  `:1004-1021`, the only gate-1 analyzer runs on record) is explained. Either
  (a) a trim-cadence regression between the 2026-08-02 image and `3824579` is
  bisected to a commit and fixed, or (b) both images show the same cadence
  with the U24 XL, in which case D4 goes to the owner with the measured span.
  Gate 1 then re-runs on the deployed image and the verdict is recorded with
  the digest. Leg B's image: the 2026-08-02 result was a 630 s carrier-value
  observation (`trim_updates 0`), not a gate-1 run (`:991-998`, register
  `:1809-1822`); the stamp named that day, `5151bb21f8be` (register `:772`),
  is a upstream repository-era stamp that does not resolve in aius history, so leg B is
  built from the nearest committed tree, aius `405ce97` (committed 11:00:29;
  the 10:51 build may have been a dirty tree over its parent `b8c2a74` — say
  "nearest committed tree", not "the same image"; its tree has
  `firmware/scripts/build.sh` and `flash_optical.sh [firmware.hex]`).
  Constraints on leg B, so they are not read as regressions: that image
  carries the periodic WDOG reset at ~1,904 s (register `:762-801`), so its
  600 s leg must start within minutes of boot; the HEAD analyzer's
  `validate_status` (`aius_rate_bench.py:264-316`) requires every
  `CLOCK_FIELDS` / `AUDIO_FIELDS` key, `tx_gate.open` and
  `pipeline_diag.codec_pc`, and whether the old image emits them all is
  unverified — if it rejects the payload, compute the `clock.trim_updates`
  delta offline from the raw samples instead of calling the leg failed;
  its `tx_underrun_frames` counts idle silence (register `:41-64`), so any
  gate closure fails the HEAD analyzer's harmful-counter check for a
  non-fault; the current card `dsp.json` (keys added from 2026-08-04) reads
  `config_error` on it, cosmetic.
- seams: bench first (`firmware/tools/net/aius_rate_bench.py` scenario
  `48k-10m`, U24 XL as source, `--expect-build`); firmware only if (a): the
  trim policy in `firmware/lib/aius_audio/src/aius_audio_clock.cpp:564`
  (`aius_audio_clock_request_trim`, called from `optical_main.cpp:8158`) and
  its host suite (`clock-target`).
- targeted-proof: leg A — gate-1 run on the deployed image (`48k-10m`: 600 s
  with the 30 s settle inside it, `aius_rate_bench.py:46`), digest recorded;
  leg B — gate-1 run on `405ce97` built in a scratch worktree and flashed by
  hex path, same source, same day, digest recorded. Budget: leg A ≈ 11 min;
  leg B and every bisect leg add a COLD `.pio` build (a fresh worktree
  compiles everything; framework packages are shared under `~/.platformio`,
  `lib_deps` is a local symlink, so no network) — budget minutes, not the
  warm 25 s; the worktree's flock (`.pio/optical-build.lock`) is per-tree and
  does not serialize against the main tree, harmless with one bench. ≈ 30 min
  for the A/B, ≤ 6 bisect legs (+90 min) before pausing to report. Leg B and
  every bisect leg are HalfKay flashes of pre-chooser images, so each
  overwrites the chooser: the leg sequence ends with a re-cutover
  (`recovery.hex` from HEAD, Phase 5's step 2–3) before any other phase
  touches the board. A fix ships with a `clock-target` host row pinning the
  cadence policy, deploys over OTA, and gets one green gate-1 run on the
  clean-stamp image. The `PASS` criterion (≤ 2 post-settle trims) does not
  move unless D4 rules otherwise.
- unit: parent-only (each bench leg is >60 s and runs through an Agent; one
  agent at a time on the board)
- dependencies: Phase 0, Phase 5 (so the re-cutover recipe exists and the
  deployed image being A/B'd is the canonical one), Phase 1 step 2 if it
  lands firmware (re-baseline)
- commit-checkpoint: SRF plan §Device evidence updated with both runs;
  register entry updated; a firmware fix is its own commit with a clean-stamp
  OTA deploy.

### Phase 4 — One-image sample-rate acceptance re-run

- outcome: SRF acceptance is judged as written: every in-scope device gate on
  ONE exact image (`aius-sample-rate-following.md:959-961`, `:975-979`;
  Phase 2 `status: device-acceptance-pending` `:476`). Today gates 2 and 7
  passed on `3570a21` / `e746cf9`, gate 4 on `dd9520e`, gate 6 only on
  `01477755955b`, gate 5 (clean-source encoded AC-3 at 44.1 kHz) never on the
  U24 XL. After Phase 3 lands (fix or ruling), gates 1, 2, 4, 5, 6, 7 run on
  the then-deployed clean-stamp image in one bench session; SRF Phase 2 flips
  to shipped or the failing gate is named. This is what unblocks
  [`aius-ac3-support.md`](aius-ac3-support.md)'s four-hour + by-ear gates
  (`:157-160`), which are then handed to Phase 9 (owner presence).
- seams: bench only (`aius_rate_bench.py`, `recorder_transitions.py`,
  `capture.py`). Gate 5 is encoded 44.1 kHz INPUT to the board's RX
  (`aius-sample-rate-following.md:596-598`), so its fixture is host-side — an
  120 s IEC 61937 AC-3 stream at 44.1 kHz played through the U24 XL with
  `aplay` at unity — not a card replay (replays are the board's output side).
  No such fixture exists yet: `firmware/tools/ac3_vectors` holds 0.1 s raw
  AC-3 vectors and `iec_corpus` a two-AU parser vector, both 48 kHz; the only
  host IEC generator is `firmware/tools/latency/bench.py:1648-1656` (ffmpeg
  `-c:a ac3 … -f spdif`) pinned to 48 kHz. Generate the fixture with the same
  ffmpeg spdif mux at 44.1 kHz (a small script beside `ac3_vectors`, or the
  `bench.py` rate parameter if Phase 7 lands first). Precondition to prove
  first: the U24 passes an IEC 61937 bitstream intact at unity — its
  qualification was PCM bit-exactness and `source_qualification/README.md:37-42`
  records that −20 dB destroys IEC sync — a 30 s `encoded` smoke showing the
  board classifies AC-3.
- targeted-proof: the U24 passthrough smoke, then six digests on one
  `build_revision`, each verdict recorded in SRF §Device evidence; the
  register's sample-rate section updated; the gate-1 scenario and gate-2
  (120 s) dominate the budget (~15 min + set-up). Any firmware that lands
  after this phase (a late D9/D12 yes) re-runs the six gates.
- unit: parent-only
- dependencies: Phase 3; D9 and D12 ruled (or explicitly deferred) so their
  firmware, if any, is on the image being judged
- commit-checkpoint: one docs commit (SRF + register), plus the AC-3 plan
  status paragraph.

### Phase 5 — Restore the slot-agnostic A/B posture; OTA becomes the deploy path ✓ SHIPPED 2026-09-03

**Shipped 2026-09-03.** Board on the slot-agnostic chooser again from HEAD
`acfd6e3baba9` (`recovery.hex` sha256 `c5c4f091…`, the one HalfKay flash);
two untagged OTA round-trips A→B→A (`canonical-1` `aea34ce8…`, `canonical-2`
`ef256904…`) with `rollback_observed false`, `relocks 0`, `tx_underrun_frames`
0 across both activations; `409 ota_digest` refusal reproduced with the
corrupt container RETAINED (`6d3b518b…`), abort → idle. Cleanup done small
(`--slot` deprecated, `build_ab.sh` canonical by default, signer evidence
carries `slot_id`); full dual-slot retirement scoped as the slot-agnostic
plan's Phase 7 with its owner-visible (d) decision. Observed and recorded, not
a regression: 1280 underrun frames during the 141-chunk SD staging with the
carrier live — the documented un-gated card-traffic cost. `booted_slot` is now
on the wire. Record: `aius-ota-slot-agnostic.md` §Status. From here every
firmware phase deploys over OTA.

- outcome: the board runs the slot-agnostic chooser and a canonical image
  built from HEAD, and every later firmware phase in this plan deploys over
  OTA. [`aius-ota-slot-agnostic.md`](aius-ota-slot-agnostic.md) Phases 1–6
  are already proven (bench acceptance 2026-08-31, `:85-99`); this phase is
  the re-cutover it says is owed (`:43-45`) plus the OTA round-trip that
  proves the path on today's firmware. Steps: (1) `build_ab.sh` from a clean
  HEAD → `.pio/build/aius-ab/recovery.hex` (chooser + canonical slot A) and
  the canonical image; (2) HalfKay flash `recovery.hex` via
  `flash_optical.sh <hex>` — the one USB flash; (3) confirm `/api/status`
  `ab` is an object, `ledger_valid true`, `ab.slot` = the physical slot, and
  chunk 0 of an untagged container is accepted (the dependable check per
  `CLAUDE.md`); (4) full OTA round-trip with a byte-distinct canonical image
  (fresh `AIUS_BUILD_UTC`): sign without `--slot`, stage, verify, activate,
  commit, `--expect-build` on the new stamp, then once more back, so both
  A→B and B→A are exercised on HEAD; (5) retire the dual slot-B env,
  `gen_slot_ldscript.py` and the `--slot` requirement in the signer + CLI
  (the plan's owed cleanup, `:72-75`) — or, if that cleanup is larger than a
  session, record it as the plan's next phase and leave the dual path
  coexisting. The mid-window power-cut gate (`:95-97`) stays owner-skipped
  unless the owner asks for it.
- seams: `firmware/scripts/build_ab.sh`, `merge_hex.py`, `chooser/`,
  `lib/aius_ab`, `platformio.ini` envs (`teensy41-optical-slot-a/-b`),
  `tools/ota/aius_ota_sign.py` + `aius_ota_flash.py`, `CLAUDE.md` §Device
  access and OTA (path-guarded: scratch + `the release-promotion step` — rewrite the
  posture paragraph so it no longer describes the plain image as deployed;
  keep it version-free).
- targeted-proof: `/api/status` before (`ab: null`) and after (`ab` object,
  `ledger_valid true`); two OTA round-trips with `--expect-build` on each
  new stamp, `rollback_observed false`, `relocks 0`, `tx_underrun_frames`
  unchanged across both activations except the boot pad; the `409
  ota_digest` refusal of a corrupted offer reproduced once with the artifact
  RETAINED this time (the 2026-08-31 observations kept none, `:71-72`);
  optical-cutover suite green if `optical_main.cpp` changes; host suites for
  any signer/CLI change. Record the whole sequence in the slot-agnostic plan's
  §Status with the image stamps, and re-read the recorder plan's Phase D
  build constraint (it wanted exactly this posture).
- unit: parent-only (one implementer; bench serialized)
- dependencies: Phase 0 (record current), Phase 1 step 2 if it landed
  firmware (so the canonical image carries it)
- commit-checkpoint: cleanup commits (if any) + docs commit in aius;
  `CLAUDE.md` promote; upstream pointer bump.

### Phase 6 — OTA verify: IVT-bounds check, stall disposition

- outcome: D12 ruled 2026-09-03 — OTA is the primary update method, so this
  is real value: `POST /api/ota/verify` refuses a staged payload whose IVT is
  out of bounds before any flash is spent. The verify path today sees only
  digest + header (`firmware/lib/aius_ota/src/aius_ota.c:534-574`); the check
  reads the staged PSRAM image at `+0x1000` and reuses
  `aius_ab_slot_ivt_sane` (`firmware/lib/aius_ab/src/aius_ab.c:342`),
  posture-aware (slot window under the chooser; the NULL/boot window case
  `optical_main.cpp:8944-8946` refuses rather than guesses, since on that
  posture a byte-verified garbage image bricks the board). The 2026-08-16
  incident (register `:434-440`) is the regression fixture. D2 applies to the
  ~80 ms Ed25519 verify stall (default: accept and document — staging is
  owner-initiated; the register entry becomes "known cost, mitigated by
  procedure: stage during silence"); revisit only if the owner wants the
  scalar arithmetic sliced across epochs.
- seams: `firmware/lib/aius_ota/src/aius_ota.c` (+ header),
  `firmware/lib/aius_ab/` (shared IVT predicate), `firmware/test/ota/` host
  suite, swagger `ota` error vocabulary (upstream repository `swagger.json`,
  `pnpm exec vitest run test/swagger.test.js`).
- targeted-proof: host rows — valid IVT accepted; stack pointer or reset vector
  outside the slot's flash range refused with the new status; the
  garbage-ASCII incident payload refused; canonical build SUCCESS with ITCM
  reported (verify is flash-resident; ITCM must not grow); clean-stamp OTA
  deploy of the fix itself (which is also this phase's good-path proof); then
  one staging round on the bench with a deliberately bad IVT refused at
  verify, nothing armed.
- unit: parent-only
- dependencies: Phase 0, Phase 5 (deploys over OTA); lands BEFORE Phase 4
  (firmware-before-bench).
- commit-checkpoint: firmware commit + swagger commit + OTA deploy + docs.

### Phase 7 — Non-48 kHz latency rows and the Phase-3 owner target

- outcome: D8 (owner-only) applied to the `Non-48 kHz latency rows
  unmeasured` register entry (`:1830-1834`). The rows are NOT bench-only:
  `firmware/tools/latency/bench.py:36` pins `RATE = 48000` and uses it at
  `:1587` and `:2271`, so measuring them means harness work (rate parameter,
  44.1 kHz stimulus and renderer path, host row) — new tool scope the
  register entry did not anticipate. Recommendation: strike as superseded
  unless the owner names a consumer for a 44.1 kHz latency figure; if named,
  build the harness path and measure one 44.1 kHz row per pinned scenario on
  the acceptance image. Separately
  [`aius-e2e-latency.md`](aius-e2e-latency.md) Phase 3's "owner target not
  met" residue (16.9 ms board share on the Phase F image, `:1085`;
  limiter-lookahead lever) goes to the
  owner as D5 and is marked as ruled; its `dts-production:
  deferred-by-priority` rows (`:810`, `:820`) are re-dispositioned in the
  same commit (measure alongside, or keep deferred with the reason).
- seams: `firmware/tools/latency/bench.py` (+ its host tests), the e2e plan,
  the register.
- targeted-proof: D8 recorded; if measuring — host row proving the harness
  renders and analyzes at 44,100 Hz, one 44.1 kHz row per pinned scenario with
  digest; either way the register entry is struck with the ruling and D5 is
  recorded; `aius-e2e-latency.md` flips to `shipped` once D5 is ruled and its
  deferred rows are dispositioned.
- unit: parent-only
- dependencies: Phase 4 (the acceptance image), D8 ruled
- commit-checkpoint: tool commit, then docs commit with the rows.

### Phase 8 — Receive-DMA gating during dark replay (proposal, not built by default)

- outcome: D3 applied. Default (review finding 5): leave as recorded — the
  storm is harmless (register `:328-331`), the RX ISR with
  `update_responsibility` false only clears the interrupt and stages-or-drops
  (`firmware/lib/aius_audio/src/aius_spdif_sources.cpp:274-300`), and gating
  the DMA request would let the receiver FIFO overrun and latch SIS error
  bits on every dark replay — a new false board fault in `input_diag`. The
  register entry's proposal line is amended with that risk. Only on owner
  request: implement with the `DMA_CERQ`/`SERQ` primitive that already exists
  in `spdif_engine_reacquire()` (`optical_main.cpp:338-354`), FLASHMEM
  checkpoint-7 code (ITCM ≈ 0), and prove `rx_dropped_halves` flat through a
  dark replay, `audio_update_calls` at the block rate, carrier return still
  relocks (`[1,1,1]`), AND the SIS error latch unchanged through the replay.
- seams: docs by default; if built, `aius_spdif_rx.h`, `aius_spdif_sources.cpp`,
  `replay_clock_service()` / `replay_source_service()` in `optical_main.cpp`,
  optical-cutover text pins.
- targeted-proof: default — register line amended (a docs disposition; this
  proof cannot fail and is not meant to). If built — `clock-target`
  host row for the enable/disable rule, `teensy41-optical` SUCCESS, clean-stamp
  flash, one `--with-replay` gate-4 run with the four measurements above.
- unit: parent-only
- dependencies: Phase 0; serialized after Phase 1 step 2 if both touch
  `optical_main.cpp`.
- commit-checkpoint: docs commit; or firmware commit + flash + docs.

### Phase 9 — Owner-scheduled: hardware, by-ear gates, external-source steps

- outcome: recorded here so the inventory is complete; this loop does not
  drive them. [`aius-h2-h3-manufacturing.md`](aius-h2-h3-manufacturing.md)
  H3b–H3e (that plan's own headings) and the H2 footprint gate (register
  `:1639-1643`) under the hardware release gates in `CLAUDE.md` (D7); the
  raw-bypass rows re-homed from the optical-prototype plan to H4 EVT
  bring-up (D10); the AC-3 four-hour + by-ear gates once Phase 4 passes
  (`aius-ac3-support.md:157-160`); the output-jitter acceptance evening and
  pop verdict if D9 keeps the governor held; the first-signal test's
  external-source/sink steps 2–4, 6 (register `:232-242`); the ears round +
  `lfe_gain` A/B (`:1448-1453`); anything Phase 0 dispositions as
  owner-scheduled.
- seams: `hardware/**` (KiCad), the plans named.
- targeted-proof: as each plan defines; this phase is marked SHIPPED when D7
  and the by-ear scheduling are answered and each item points at its date or
  its "not scheduled" ruling.
- unit: parent-only
- dependencies: none in this plan; owner scheduling.
- commit-checkpoint: docs commits as rulings arrive.

## Decisions

| Id | Decision | Default-able? | Default / note |
|---|---|---|---|
| D1 | `aius-ota-slot-agnostic.md` disposition | RULED 2026-09-03 | Re-cutover scheduled (Phase 5): "we should never have superseded the hard-won A/B deployment". |
| D2 | OTA verify ~80 ms Ed25519 stall | default-able | Accept and document (stage during silence). Alternatives: slice the scalar arithmetic; verify under a gate-aware moment. |
| D3 | Receive-DMA gating during dark replay | default-able | Leave as recorded (harmless; gating risks SIS error latches). Build only on request. |
| D4 | Gate-1 trim bound | default-able | No bound change until Phase 3's A/B has run; if both images match, owner chooses source-qualified bound vs different source. |
| D5 | e2e latency owner target (limiter lookahead) | owner-only | Phase 7 reports the numbers. |
| D6 | Recorder Phase D build constraint | decided | Already retired in the doc (`aius-recorder-rolling-buffer.md:695-697`); record. |
| D7 | Hardware H3 scheduling | owner-only | — |
| D8 | Non-48 kHz latency rows | owner-only | Recommendation: strike as superseded unless a consumer is named (measuring needs new harness scope). |
| D9 | TX governor | RULED 2026-09-03 | Not wanted: "systematic latency with repeated triggers"; silence on a stall, never a buzz. Governor stays compiled out; Phase E closed as decomposition, governor retired (Phase 0, orchestrator). |
| D10 | `aius-optical-prototype.md` disposition | RULED 2026-09-03 | Descope each unshipped item that serves no particular purpose; keep any that does, with what depends on it. |
| D11 | Input recovery: ceiling exception; `carrier_loss` trigger (`aius-input-recovery.md:143-174`) | owner-only | Only needed if Phase 1 reaches its Phases 2–4. |
| D12 | OTA on the deployed posture | RULED 2026-09-03 | OTA is the primary update method and must be exercised; USB is for the cutover only (Phase 5); the IVT check is real value (Phase 6). |

Standing proposal carried from earlier sessions, not agreed, no default: the
ITCM census / ldscript rework (register `:144-148`). Two items the 2026-09-03
handover listed as proposals (`final_replay_output_blocks_delta` naming;
splitting the register by era) have no source in `docs/` and are dropped here;
raise them again only with a written rationale.

## Completion

This plan is `shipped` when Phases 0–8 are marked `✓ SHIPPED` (Phase 9 when
its items each carry a date or a "not scheduled" ruling), every register entry
is struck with its pointer or names a phase here, every open ledger defect in
`aius-network-hardening.md` appears in the register with the same state, and
the plans this plan touched carry truthful `status` fields. The register's
§Open defects sections then list only Phase 9 items, owner-only decisions
awaiting a ruling, and the standing proposals.
