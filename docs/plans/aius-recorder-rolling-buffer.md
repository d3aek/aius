---
title: Aius flight recorder — debounce + rolling multi-slot buffer
type: plans
scope: component
component: aius
status: draft
tags: [aius, firmware, audio, recorder, psram, ota]
updated: 2026-08-28
---

# Aius flight recorder — debounce + rolling multi-slot buffer

Fix the flight recorder's two compounding defects: the underrun trigger is
sensitive to the power-on transient and burns its only shot before a session
exists (already logged in
[`aius-open-work-register.md`](aius-open-work-register.md) lines 222-237), and
the single-slot one-shot-until-manual-thaw model means even a debounced
trigger still blinds the rest of the session on its first genuine fire.
Replace the single ~21.8 s ring with a debounced, auto-advancing rolling bank
of shorter captures, at unchanged total PSRAM footprint.

**No device flash until Phase D's adversarial review + bench proof is
clean.** The owner's explicit constraint: prove the audio path is not
interfered with and stability is not harmed before any of this touches the
*device*. Host-only implementation and host-tested proof are not gated by
this — Phases A-C build and test on the host as they land; what's gated is
flashing a build to hardware, which Phase D's bench-proof step is the first
point any of this does. Two live risk factors sharpen that bar further —
see §Deployment constraints.

## Problem statement

- **Trigger too sensitive to power-on.** Every boot so far (`at_ms 2929`,
  `3289` across different builds) freezes on the trivial first-block
  underrun during bring-up, before the session the recorder exists to watch
  has started. Confirmed again 2026-08-27 (`.scratch`
  session pull): `state: frozen`, trigger `underrun`, `at_ms 2929`,
  `postroll_captured: 0`.
- **One-shot-until-manual-thaw wastes the debounce fix's benefit.** Even
  once the power-on trigger is suppressed, the single ring still spends its
  one capture on the first *real* fire of the evening and stays FROZEN,
  blind to everything after, until an operator notices and thaws it by hand.
  This is the state that caused the 2026-08-16 listening-room pop
  (`aius-output-jitter.md` Phase E) to go uncaptured.
- **The 20 s figure is incidental, not a debug requirement.** Ring size is
  `psram_bytes / 8` after the 32 KiB staging carve
  (`aius_recorder_ring_bytes_after_staging()`); it lands at 21.8 s on the
  current 16 MB-fitted, OTA-sharing-halved 8 MiB pad-1 allocation by
  arithmetic, not by design intent. Every capture on record so far is short
  and punctate: the 2026-08-27 SD-primitive stall (~590 ms), the 2026-08-16
  pop (384 frames, 8 ms), the defect-27 overrun flood. The one case that
  motivated "long history" (the 1.4-semitone pitch drift with clean
  counters, `aius-optical-prototype.md:158`) is a slow-drift class that a
  few extra seconds of audio pre-roll would not have shown either — it wants
  a slower-sampled trend log, not more ring depth, and is out of scope here.

## Deployment constraints

- **No USB/HalfKay physical recovery available right now** (device has not
  been on USB for some time). Any image built under this plan ships only
  through the fallback that is *actually running on the device today*: the
  **old per-slot-signed A/B scheme** (`aius-ota-resilience.md` Phase 3,
  shipped and bench-proven 2026-08-16, predates `cee8b46`) — sign with
  `--slot`, read `/api/status`'s `ab.slot`, per `aius/CLAUDE.md`'s live
  operator instructions. That fallback does not itself depend on the
  unflashed rework below, which is the reassuring half of this; the risky
  half is git hygiene, not mechanism — see the next bullet. The listening-room
  board is the *last* place any of this lands — bench-proven first, on a
  spare/available slot.
- **There is no separate integration point to build this plan's changes
  against — `origin/master` IS the unflashed rework.** `git_modules/aius`
  HEAD and `origin/master` are both `78cb738` (confirmed 2026-08-28), the
  tip of the 9-commit slot-agnostic A/B rework (`aius-ota-slot-agnostic.md`)
  that is itself unflashed and unverified past `cee8b46` — the revision
  pinned in the upstream pointer and the last one bench-verified end to
  end. That rework already touches two of this plan's own seams
  (`service_ota_activation` / `service_ota_rehearsal` in
  `optical_main.cpp`, astride both `aius_recorder_park()` call sites this
  plan cites). **Building "the repo" at any point in Phases A-D therefore
  bundles the unverified rework by default, silently, unless this plan
  explicitly isolates its own commits.** Phase D's build step names the
  concrete mechanism: a dedicated branch cut from `cee8b46`, with only this
  plan's commits cherry-picked onto it — never a build from HEAD or
  `origin/master` as they stand. The slot-agnostic rework earns its own
  independent bench pass on `78cb738` directly, not as a passenger on this
  plan's flash.
- **PSRAM is shared with OTA staging, not exclusive to the recorder.**
  `aius_recorder_park()` (one-way, any state → ABSENT) hands the whole pad-1
  region — "the ring's content is expendable by definition once an
  activation is acknowledged" (`aius-ota-resilience.md` Phase 2) — to
  `ota_psram_staging()` for the SD→PSRAM image copy. Any redesign of the
  ring's internal shape must keep the *total* footprint identical to today's
  single ring at every board fitment, or the pad-1 capacity static-assert in
  `optical_main.cpp` (~line 4242) and the OTA placement host tests silently
  stop covering what's actually on the device.

## Design

**Round-robin bank of N equal power-of-two sub-rings, sharing the existing
single 32 KiB producer staging carve.** Only one slot is ever
RECORDING/POSTROLL at a time (no concurrent producers, no new hot-path
state) — the existing single-writer-per-transition `AiusRecorder` state
machine is reused unmodified per slot; a thin bank wrapper owns which slot
is active and advances the index on FROZEN instead of the caller having to
thaw. This is a deliberate choice over letting all N slots record
concurrently: concurrent producers would need N staging carves or a
sharing scheme that doesn't exist today, multiplies the audio-path cost,
and buys nothing — the fault population is single, punctate events, not
overlapping ones.

- **Total ring bytes unchanged at every fitment.** `N` and per-slot bytes
  are chosen so `N * slot_bytes == aius_recorder_ring_bytes_after_staging(...)`
  exactly, preserving the OTA pad-1 capacity contract with no change to
  `ota_psram_staging()` or its static asserts. Default `N = 4`: 8 MiB ring
  (16 MB board) → 4 × 2 MiB slots ≈ 5.46 s each; 4 MiB ring (8 MB board) → 4
  × 1 MiB ≈ 2.73 s each. Each slot's frame count must independently be a
  power of two (the existing `ring_mask = ring_frames - 1` trick relies on
  it), which `N` as a power of two guarantees for free.
- **Auto-advance on FROZEN, not auto-thaw, executed from the epoch/replay
  checkpoint only — never the producer.** `DRAINING -> FROZEN` is set
  inside `aius_recorder_drain()`, which only ever runs from the epoch/replay
  checkpoint context (never the audio ISR). The reinit of the *next* slot
  happens immediately after that call returns FROZEN, in that same
  non-ISR context — the existing `reset_capture` + generation-increment
  path, re-targeted at a different base pointer. This keeps the reinit a
  single epoch-side writer, consistent with the header's existing "ONE
  WRITER PER TRANSITION" discipline; the producer never detects
  DRAINING/FROZEN itself or reaches into the next slot's state. The active-
  slot indicator the producer reads is one `volatile` word, written only
  from this epoch-side reinit — the same shape as today's single-writer
  `state` field, just one level up.
- **A slot cannot recycle while an HTTP read against its current generation
  is outstanding.** `aius_recorder_read()` today returns a raw pointer into
  ring PSRAM, documented stable "for as long as the state stays FROZEN...
  a thaw is necessarily a later request" — true today only because nothing
  but a separate, explicit HTTP thaw can move a slot off FROZEN. Under
  auto-advance, a slot moves off FROZEN on trigger cadence alone, with no
  operator action, which can happen *while* a chunk already handed to the
  HTTP layer is still streaming from that same slot's PSRAM bytes — exactly
  under burst-trigger conditions (a repeat of the defect-27 overrun flood)
  where the recorder matters most. Fix: a per-slot `reads_in_flight` count,
  incremented when `AIUS_HTTP_APP_IMMEDIATE` borrows the pointer and
  decremented when that response completes; the epoch-side reinit of a
  FROZEN slot with a nonzero count defers one epoch at a time (bounded,
  same shape as the existing reclaim-epochs wait) rather than recycling out
  from under a live response. This is new state this plan owns — not
  optional, and not covered by the generation check alone (which stops a
  *new* request from reading stale data but does nothing for a chunk
  already in flight).
- **Recycling is counted honestly as recycling, not as "unpulled."**
  Nothing marks a generation as having been read — `aius_recorder_read()`
  is `const` and non-mutating, deliberately (the read-lease counter above
  is concurrency control, not a read-history log). A counter named
  `slots_overwritten_unpulled` cannot be computed as such without adding a
  "read at least once" bit this plan doesn't otherwise need; naming it that
  would silently become "always increments on every recycle," the opposite
  of the diagnostic-honesty goal it's modeled on (`gap_count`,
  `suppressed_triggers`). Ship it as `slots_recycled` — a plain count of
  N-cycle wraps, honest about what it actually measures — and treat
  "did the operator pull this slot before it was recycled" as unanswerable
  without further scoped work, not as a free byproduct of this design.
- **`postroll_blocks`' existing max-bound clamp needs a second, per-slot
  ceiling.** A postroll longer than the slot's own capacity is nonsensical
  once slots shrink from ~21.8 s to ~5.46 s; `aius_recorder_set_config`
  gains a slot-capacity-aware clamp alongside the existing
  `AIUS_RECORDER_POSTROLL_MAX_BLOCKS` one.
- **`park()`/`unpark()` operate on the whole bank.** A parked bank forgets
  every slot's FROZEN metadata on the next init (the underlying PSRAM bytes
  are gone once OTA staging has written over pad-1 — no different from
  today's single-ring park contract, just applied N times over). The
  trickier case or a new one: park() firing while the active slot is
  mid-POSTROLL or mid-DRAINING, not just RECORDING — today's contract
  already covers this (`park()` is any-state → ABSENT), Phase B's proof
  just needs a bank-shaped row for it.

## Phases

### Phase A — trigger settle window (debounce) ✓ SHIPPED 2026-08-28

- outcome: the underrun trigger (both the block-evaluable producer path and
  the epoch fallback) does not arm until `AIUS_RECORDER_SETTLE_BLOCKS`
  (8, ≈85 ms at 512-frame blocks) have staged cleanly under an open gate
  since the last closed→open edge (or init/thaw) — not a bare edge-latch.
  **Revised from the original edge-only design (2026-08-28, caught during
  implementation, before commit):** the register entry's field data shows
  the boot bring-up trigger firing at `at_block 1` — the very first
  committed block — which means the gate can already be well past its
  "first edge" when the stall hits (a replay session opens the gate almost
  immediately at boot; the SD mount-class stall that causes the underrun
  follows a couple of seconds later). An edge-only latch would not have
  suppressed that. Requiring N clean blocks since the edge does, regardless
  of how much time separates the edge from the stall. A bring-up or
  post-thaw underrun before the session has demonstrated N clean blocks no
  longer freezes the recorder; a genuine underrun after settling still
  triggers exactly as today.
- seams: `aius_recorder.h` — new `volatile uint32_t settled_blocks` field
  and `AIUS_RECORDER_SETTLE_BLOCKS` constant. `aius_recorder_epoch()` in
  `firmware/lib/aius_recorder/src/aius_recorder.c` — reset
  `settled_blocks` to 0 on the same `gate_open && !prev_gate` edge that
  already resets `baseline_sync`; gate the epoch-fallback underrun branch
  on `settled_blocks >= AIUS_RECORDER_SETTLE_BLOCKS`.
  `aius_recorder_producer_block()` — gate the block-evaluable underrun
  branch the same way, and increment `settled_blocks` (capped) on any
  block that evaluates with no trigger firing while the gate is open.
  `reset_capture()` — zero `settled_blocks` on init/thaw, same as
  `baseline_sync`. This is a two-writer field (producer increments, epoch
  resets on the edge) — same "KNOWN, ACCEPTED interleaving" shape the
  header already documents for `freeze_token`/`suppressed_triggers`: a
  collision costs at most one block's delay in arming, never a corrupt
  transition, because the state word each writer touches has exactly one
  writer per edge.
- targeted-proof: new host rows in `firmware/test/recorder/test_recorder.c`
  — underrun before N clean blocks does not trigger, on both paths, even
  when the gate has been open for a while first (the case the edge-only
  design missed): `test_underrun_suppressed_before_settled` and
  `test_underrun_epoch_fallback_suppressed_before_settled`, new. On-device
  (bench, not listening-room): `/api/recorder` reports `trigger.which: none`
  through the bring-up window instead of freezing at ~3 s — **still owed**,
  no device touched yet per this plan's no-implementation-before-Phase-D
  gate on the flash step specifically (host-only work is unblocked).
- unit: parent-only
- **shipped 2026-08-28, host-only:** implemented as designed above.
  Correction against the pre-implementation assumption in the previous
  paragraph: `test_underrun_gate_qualification`,
  `test_underrun_epoch_fallback_and_reclaim` and
  `test_second_trigger_first_wins` did **not** already exercise enough
  clean blocks post-edge — each fed only 1-2 blocks after the gate opened,
  well under the 8-block threshold — so implementing this phase required
  updating all three to settle first, not leaving them untouched as
  assumed. `test_underrun_epoch_fallback_and_reclaim`'s small 4-slot test
  fixture also needed draining inside the new settle loop (unrelated to
  the debounce logic itself): without it, the fixture's own staging
  capacity gaps most of an 8-block burst, which the settle counter doesn't
  care about (it counts evaluated blocks, not staged ones) but the test's
  `end_frame` assertion does. Native `recorder` suite green, 27 rows
  (23 existing + 2 new), ASan/UBSan, `-Werror`.

### Phase B — N-slot rolling bank at unchanged PSRAM footprint ✓ SHIPPED 2026-08-28

- outcome: the recorder maintains `N` (default 4) independently-addressable
  captures instead of one; the active slot auto-advances to the next on
  FROZEN with no operator action, deferred one epoch at a time while a read
  lease is outstanding on the slot being recycled; a slot survives until
  pulled or evicted by a full wrap, with wraps counted honestly
  (`slots_recycled`, not a claim about whether it was pulled). Total PSRAM
  footprint identical to today's single ring at every fitment (16/8/0 MB) —
  verified, not assumed.
- seams: **built as a new, separate module** —
  `firmware/lib/aius_recorder/include/aius_recorder_bank.h` +
  `firmware/lib/aius_recorder/src/aius_recorder_bank.c` — rather than the
  originally-planned in-place rework of `aius_recorder.h`/`.c` themselves.
  This is a stronger form of "reused unmodified": the existing single-ring
  core (27 host rows, Phase A's debounce included) is untouched byte-for-
  byte, and `AiusRecorderBank` owns `N` `AiusRecorder` slot contexts plus
  the new per-slot `reads_in_flight` lease count and `slots_recycled`,
  forwarding producer/epoch/drain to `bank->slots[bank->active]` unchanged.
  The reinit-next-slot step lives in `aius_recorder_bank_drain()`, called
  only from the epoch/replay-checkpoint context, immediately after the
  active slot's `aius_recorder_drain()` returns FROZEN — never from the
  producer. **`firmware/src/optical_main.cpp` wiring, done in a follow-up
  commit the same day:** `g_recorder` (single `AiusRecorder`) replaced by
  `g_recorder_bank` (`AiusRecorderBank`) at every call site — the AUDIO
  checkpoint's producer tap, the REPLAY checkpoint's epoch+drain, both
  `park()` sites (`service_ota_activation`, `service_ota_rehearsal`),
  `ota_psram_staging()`'s park-ordering assert (now
  `aius_recorder_bank_present()`, representative across all N slots since
  park()/init() only ever run from this single epoch/controller context,
  never interleaved with a producer), and all four `app_recorder_*` HTTP
  handlers. The pad-1 capacity static assert (~line 4253) needed no
  change — it keys on the flat `RECORDER_STAGING_BYTES`, independent of
  `N`, exactly as the adversarial review's "holds up" finding predicted.
  **One addition beyond the original seam list:** `aius_recorder_bank_thaw()`
  (thaws the active slot in place, no round-robin advance) — `POST
  /api/recorder/thaw` is a live operator-facing endpoint (used mid-session
  this same day, before this plan existed) and auto-advance alone would
  have silently orphaned its contract; this preserves it as the explicit
  override for the case auto-advance defers (a held read lease) or an
  operator simply wants the current slot back. **HTTP JSON shape
  unchanged, deliberately:** every handler now reads through the ACTIVE
  slot only (`g_recorder_bank.slots[g_recorder_bank.active]` /
  `aius_recorder_bank_snapshot_slot(..., g_recorder_bank.active, ...)`),
  so today's single-slot response contract is byte-identical — exposing
  the other N-1 slots (bank status document, `slot`-addressed data reads)
  is Phase C's still-owed HTTP reshaping, kept out of this wiring
  commit's blast radius on purpose.
- targeted-proof: host suite (`recorder-bank` binary within the `recorder`
  suite case, `firmware/scripts/test.sh`) — slot advance on freeze,
  including a full N-cycle wrap with recycling counted; a row proving
  recycling defers while `reads_in_flight` is nonzero on the target slot
  and completes once it drops to zero (during which nothing records — the
  correctness-over-liveness trade the Residual risks section documents);
  power-of-two slot-size assertions at init for every fitment (16/8/0 MB),
  plus confirming each fitment's total ring extent matches
  `aius_recorder_ring_bytes_after_staging()`'s single-ring output exactly;
  park() reaching every slot including one FROZEN and one mid-POSTROLL
  simultaneously, and park() mid-DRAINING; the config clamp against slot
  capacity. **Scoped down from "re-run the 23 existing per-ring rows
  parametrized over the active slot"**: the underlying per-ring state
  machine is unmodified code reached only through an unmodified API, so its
  27 existing host rows (Phase A included) already prove it: the deletion
  test says re-running them here would prove the bank's *forwarding*, which
  one lifecycle smoke test already covers, not re-prove the state machine
  itself. 12 host rows (the wiring commit added `aius_recorder_bank_thaw()`'s
  own row), all green, `-Wall -Wextra -Werror`, ASan/UBSan. **Real
  cross-build, not just host tests:** `scripts/build.sh teensy41-optical`
  — `[SUCCESS]`, every post-link guard green including the
  recorder-specific ones (`recorder OCRAM staging=0 B`, `recorder-object
  ITCM placement: PASS`) — the zero-OCRAM-recorder-staging pin this bullet
  originally deferred to "once the wiring lands" is now directly proven,
  not assumed. `itcm-placement` (29 rows), `memory` (2 rows) and
  `optical-app` (35 rows including `test_recorder_routes`, which pins the
  dispatcher's callback-table shape unchanged) re-run against the fresh
  cross-build, all green; `ota` (host-portable staging-placement suite,
  20 rows) green, though `ota_psram_staging()`'s own park-ordering assert
  is target-only code no host suite reaches directly — its correctness
  rests on the single-threaded-epoch-context argument in the source
  comment plus this successful link, with real verification (an activation
  rehearsal against the bank) belonging to Phase D's bench proof, not
  here. Host-only — no device flash in this phase.
- unit: parent-only

### Phase C — HTTP surface + egress tool for multi-slot addressing IN-PROGRESS (status document + data-route slot addressing shipped host-only; on-device bench proof gated under Phase D)

- outcome: `GET /api/recorder` reports the bank (active slot index +
  per-slot summaries: state, trigger, generation, valid/gap counts,
  `slots_recycled`); `GET /api/recorder/data` gains a `slot` parameter
  (default: active slot, preserving today's single-shot mental model for an
  unmodified client), acquires that slot's read lease
  (`reads_in_flight++`) for the response's lifetime and releases it on
  completion, and still refuses a stale/evicted generation with 409;
  `firmware/tools/aius-recorder-pull.py` gains `--slot`. Swagger schema +
  its drift gate stay pinned to the new shape.
- seams: `app_write_recorder` / `app_recorder_read` and the `Recorder`
  swagger schema in `firmware/src/optical_main.cpp`;
  `firmware/tools/aius-recorder-pull.py`; checked `firmware/sd/www/` for a
  recorder-aware dashboard surface — confirmed absent, not just assumed.
  **`GET /api/recorder/data`'s `slot` parameter is real plumbing depth, not
  found until implementation:** `offset`/`length`/`generation` are parsed in
  `lib/aius_http/src/aius_http.c` into fixed `AiusHttpRequest` fields, threaded
  through `src/aius_optical_app.cpp`'s dispatcher, into the
  `AiusOpticalApp::recorder_read` function-pointer interface
  (`src/aius_optical_app.h`) — a `slot` parameter touches all four of those
  plus their own host suites (`http`, `http-transaction`, `optical-app`), not
  just `optical_main.cpp`. Scoped as its own remaining increment of this
  phase rather than rushed alongside the status-document reshape.
- targeted-proof (status document, done): the swagger drift gate
  (`compareWithSchema` against the freshly-extracted `app_write_recorder`
  format literals) is genuine structural proof, not a hand-checked
  assertion — it re-derives the served key tree from the C source and
  fails on any drift in either direction; 11/11 vitest rows green,
  including the new `RecorderSlot` required-field and array-shape checks.
  **Found and fixed en route:** all four drift-gate tests (`status`,
  `recorder`, `replayStatus`, `dspConfig`) were failing identically before
  this change, unrelated to it — `swagger.test.js`'s `firmwareRoot`
  pointed at `firmware`, a stale untracked pre-migration
  leftover with no `firmware/src/` at all, instead of the real submodule
  at `firmware`. Fixed as part of landing this phase
  since the drift gate had to actually run to verify the new schema.
  Real cross-build (`teensy41-optical`) `[SUCCESS]`; `itcm-placement` (29
  rows) and `optical-app` (35 rows, `test_recorder_routes` included) green
  against it. `aius-recorder-pull.py`'s new `--slot` logic (default-to-
  active, matching-slot success, mismatched-slot refusal, out-of-range
  refusal) verified against a mocked bank response shaped exactly like the
  C emitter's actual output — no existing test harness to extend, so this
  was direct functional verification, not a committed regression (the
  puller has no prior test coverage either). Host/bench-only — no device
  flash.
- **Shipped host-only (2026-09-01):** the `GET /api/recorder/data` `slot`
  parameter through `aius_http.c` / `aius_optical_app.{h,cpp}` /
  `optical_main.cpp` (default = active slot, response byte-identical);
  out-of-range slot → 400, stale/evicted generation of the *addressed* slot
  → 409; `aius-recorder-pull.py --slot` (earlier). The read-lease
  acquire/release rides the transaction's true completion hook
  (`http_response.sd_settle`, generalized to immediate responses — a
  contained `aius_http_transaction.{c,h}` change), releasing exactly once at
  send-completion / recycle / abort. A Phase-D adversarial review caught the
  first implementation's next-dispatch release as a silent-recording-stall
  bug — a one-shot pull that closes without a follow-up request pinned the
  slot forever once the ring wrapped onto it; the `sd_settle` release fixes
  it and matches this plan's "release on completion" contract. Host proof:
  `http`, `http-transaction` (43 rows), `optical-app` (37 rows), `recorder`
  (27+12), `runtime-target` green; `teensy41-optical` cross-build green.
- **Still owed (bench, gated under Phase D):** induce 2 real triggers across
  one boot, pull both frozen slots, confirm two distinct complete WAVs and
  that the *next* active slot is reported as RECORDING throughout.
- unit: parent-only

### Phase D — adversarial review + bench proof, before any flash

- outcome: a first design-level adversarial review already ran (2026-08-28,
  see the Design section's read-lease/reinit-context/recycle-naming
  changes and the Deployment constraints section's git-isolation
  requirement — all findings folded in, not left open). This phase is the
  *second* pass, against the actual A-C diff rather than the design doc,
  plus a clean bench proof. Only after this is clean does deployment to the
  listening-room board proceed, as its own explicit, separately-approved step.
- seams: whole A-C diff.
- targeted-proof: a first review pass (2026-08-28) already ran against the
  design in this doc and its findings are folded into Phases A-C above
  (the read-lease requirement, the `slots_recycled` rename, the reinit-
  context and direct-field-access seams); this phase's proof is a *second*
  pass against the actual diff, checking whether the implementation matches
  what was folded in, plus: (1) no new per-block work in the AUDIO
  checkpoint beyond a slot-index dereference, no allocation, no growth of
  the 2.67 ms transmit-cushion worst case; (2) the build step used a
  dedicated branch cut from `cee8b46` with only this plan's commits
  cherry-picked, never `origin/master`/`78cb738` (the slot-agnostic OTA
  rework) as they stand as of 2026-08-28 — restated because `git status` on
  that revision changes over time and this check must be re-run against
  whatever `origin/master` is *at the time of the build*, not assumed from
  this doc's snapshot; ships via the old per-slot-signed A/B fallback
  (`aius-ota-resilience.md` Phase 3) only, never a raw single-image flash;
  (3) `park()`/`ota_psram_staging()` correctness re-verified against the
  bank shape with a rehearsal activation mid-cycle. Then: full native suite
  green (`recorder`, `memory`, `runtime-target`), `teensy41-optical` build,
  bench-only flash to a spare/available slot, on-device proof of one full
  N-slot wrap, one park()/unpark() cycle mid-POSTROLL, one read-lease-
  deferred recycle (hold a slow pull open across a trigger that would
  otherwise recycle its slot), and the Phase A debounce — all before the
  listening-room board is touched.
- unit: parent-only

## Residual risks / non-goals

- **Total capture window across all slots is unchanged (~21.8 s split into
  4 × ~5.46 s), not extended.** This plan trades one long window for
  several shorter independent ones; it does not add PSRAM budget.
- **Auto-eviction of an unpulled capture after N wraps is an accepted
  trade**, not eliminated — counted (`slots_overwritten_unpulled`), not
  silent. An operator who wants zero eviction risk still needs to pull
  promptly; this only dilutes the single-shot risk by a factor of N.
- **`N` is compile-time, not runtime-configurable**, to keep Phase B's
  blast radius bounded. A configurable `N` (mirroring how `postroll_blocks`
  is already runtime-configurable) is a plausible follow-up, not scoped
  here.
- **The slow-drift diagnostic class (the 1.4-semitone pitch error
  precedent) is explicitly out of scope** — it wants a slower-sampled trend
  log, not more ring depth or more slots.
- **A slot with an outstanding read lease defers its own recycling.** Under
  a burst of triggers faster than an operator (or a slow network path) can
  finish pulling, the round-robin can stall waiting for a lease to clear
  rather than silently overwriting a slot mid-response. This is the
  correct trade for correctness over liveness, but worth naming: a
  pathological stuck HTTP client (never closes the response) could hold a
  lease indefinitely. Not fixed here; if it becomes real, the lease needs a
  bounded timeout, same shape as the existing reclaim-epochs bound.
- **`slots_recycled` answers "how many wraps happened," not "was evidence
  lost."** Distinguishing those needs a "read at least once" bit this plan
  deliberately does not add (see Design). If that distinction becomes
  operationally important, it's a scoped follow-up, not a free byproduct of
  this design.
