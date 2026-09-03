---
title: Aius flight recorder — debounce + rolling multi-slot buffer
type: plans
scope: component
component: aius
status: shipped
tags: [aius, firmware, audio, recorder, psram, ota]
updated: 2026-09-03
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
  DRAINING/FROZEN itself or reaches into the next slot's state. The
  active-slot indicator the producer reads is a plain `uint32_t`
  (`AiusRecorderBank.active`), written only from this epoch-side advance:
  both the producer tap (the AUDIO checkpoint) and the advance (the
  epoch/replay checkpoint) are main-loop checkpoints, not interrupt
  contexts, so the single-writer discipline is what makes it safe and no
  `volatile` or atomic is needed. (An earlier draft of this bullet claimed
  the field was `volatile`; it never was.)
- **The incoming slot inherits the outgoing slot's epoch-side edge
  history.** The bank forwards each epoch to the active slot ONLY, so a
  re-armed slot would otherwise edge-detect the carrier against its own
  stale past: a slot last active while locked keeps `carrier_prev == 1` and
  fires a spurious `CARRIER_LOSS` on its first epoch under a carrier that
  has been gone for seconds, while a virgin slot (`carrier_prev == 0`)
  silently drops a real falling edge on that same first epoch.
  `aius_recorder_inherit_epoch_state()` copies the three epoch-owned words
  (`carrier_prev`, `gate_open`, `underrun_base_epoch`) at every advance,
  recycle included; `reset_capture()` deliberately leaves them alone, since
  a thaw in place has not moved the recorder to a different point in the
  stream.
- **The underrun trigger is blind for the settle window after every
  advance, by design.** A re-armed slot starts with `settled_blocks == 0`,
  so it cannot arm the underrun trigger until it has staged
  `AIUS_RECORDER_SETTLE_BLOCKS` consecutive clean blocks (~85 ms at the
  production block size, longer on a dirty stream). That is the same
  debounce a boot or a manual thaw gets, for the same reason — an underrun
  seen in the first blocks after a capture re-arms says more about the
  transition than about the stream — and the cost is real: a second burst
  arriving inside that window is not captured by the incoming slot (the
  outgoing slot's capture already holds the first). Named here and in
  `aius_recorder_bank.h`, and pinned by
  `test_bank_underrun_blind_window_after_advance`.
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
  FROZEN slot with a nonzero count defers one epoch at a time rather than
  recycling out from under a live response. This is new state this plan owns
  — not optional, and not covered by the generation check alone (which stops
  a *new* request from reading stale data but does nothing for a chunk
  already in flight). **The deferral is bounded, by a drain count.** After
  `AIUS_RECORDER_BANK_DEFER_DRAINS` (32; until 2026-09-03 it borrowed the
  reclaim window's retired epoch constant, which on this runtime is ~5 ms
  of drains, not the ~280 ms the design intended — harmless, since the leased
  slot is skipped, never touched; a wall-clock bound needs `now_ms` threaded
  into `aius_recorder_bank_drain()`) deferred drains the advance skips
  *past* the leased slot to the first
  candidate that is not leased — the leased capture stays intact for its
  reader and recording resumes one slot further on — and only when every
  candidate is leased does it keep deferring. `deferred_epochs` (cumulative
  deferred drains) and `lease_skips` are bank counters for the status
  document, so a stall is visible rather than inferred. The operator thaw
  (`aius_recorder_bank_thaw()`) honours the same lease and refuses with
  `AIUS_REFUSED` while the active slot is leased: it recycles exactly the
  bytes the deferral protects.
- **A config a slot refuses mid-capture is replayed, not dropped.**
  `aius_recorder_set_config()` refuses in POSTROLL/DRAINING by design ("a
  capture in flight keeps its config"), which in a bank means an operator
  reconfigure silently fails to reach whichever slot happened to be
  capturing, and that slot then re-arms with the *previous* settings. The
  bank remembers the refused config per slot and applies it when that slot
  next thaws or is recycled; `config_pending` reports the in-between state
  for the status document.
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
  `AIUS_RECORDER_POSTROLL_MAX_BLOCKS` one. The clamp applies to the
  **default** post-roll installed at init as well, not only to an operator's
  value — 188 blocks is as capable of overrunning a short slot as 500 is,
  and nothing would otherwise correct it until someone called set_config.
- **`park()` operates on the whole bank; there is no `unpark()`.** A parked
  bank forgets every slot's FROZEN metadata on the next init (the underlying
  PSRAM bytes are gone once OTA staging has written over pad-1 — no
  different from today's single-ring park contract, just applied N times
  over). Park is ONE-WAY by contract: a reset, and the `bank_init()` that
  runs at boot, is the only way back to RECORDING. The
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
  branch the same way, and advance `settled_blocks` (capped) on a CLEAN
  block under an open gate: one that fired no trigger, carried no underrun
  delta of its own, and actually reached staging. A dirty block (its own
  underrun delta, or a gap) resets the window to zero — the contract is N
  *consecutive* clean blocks, so a stream that is stalling the whole time
  never settles. `reset_capture()` — zero `settled_blocks` on init/thaw,
  same as `baseline_sync`. This is a two-writer field (producer advances and
  resets on the block, epoch resets on the gate edge) — same "KNOWN,
  ACCEPTED interleaving" shape the header already documents for
  `freeze_token`/`suppressed_triggers`, and asymmetric: a lost producer
  increment costs one block's delay in arming, while a lost *epoch reset*
  leaves the pre-edge count standing and can arm up to N-1 blocks EARLY — a
  spurious trigger, not a delay. Accepted because both writers run from
  main-loop checkpoints on this firmware, never an interrupt, so the
  interleaving is theoretical.
- targeted-proof: new host rows in `firmware/test/recorder/test_recorder.c`
  — underrun before N clean blocks does not trigger, on both paths, even
  when the gate has been open for a while first (the case the edge-only
  design missed): `test_underrun_suppressed_before_settled` and
  `test_underrun_epoch_fallback_suppressed_before_settled`, new; plus
  `test_settle_requires_consecutive_clean_blocks` (added 2026-09-02 with the
  clean-block fix below). On-device (bench, not listening-room): `/api/recorder`
  reports no bring-up freeze at ~3 s — **delivered 2026-09-01** as part of
  the Phase C/D bench run, not as a separate Phase A exercise: the recorder
  froze at `at_block 416` rather than `at_block 1` (see Phase C's
  bench-proven note). The evidence is that run's own observation recorded in
  this plan; no separate artifact was retained.
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
  capacity gaps most of an 8-block burst, which the settle counter did not
  care about at the time (it counted evaluated blocks, not staged ones) but
  the test's `end_frame` assertion does. Native `recorder` suite green, 25
  rows (23 existing + 2 new), ASan/UBSan, `-Werror`. (An earlier revision of
  this bullet said 27; `test_recorder.c`'s `main()` listed 25.)
- **Corrected 2026-09-02 (review finding).** The shipped code counted an
  underrun-bearing or gapped block as clean, so `settled_blocks` reached the
  threshold on a stream that was stalling continuously — the window
  measured elapsed blocks, not clean ones, and armed the very trigger it
  exists to hold off. The producer now advances the counter only on a block
  with no underrun delta and no gap, and resets it on a dirty one; header,
  this plan and the code now state the same rule. Two existing rows
  (`test_underrun_gate_qualification`, `test_second_trigger_first_wins`)
  needed a drain inside their settle loops, for the same fixture-capacity
  reason `test_underrun_epoch_fallback_and_reclaim` already did — their
  loops were gapping, which is now dirty. New row:
  `test_settle_requires_consecutive_clean_blocks`. The two-writer note in
  `aius_recorder.h` was also inverted (it claimed a collision costs "one
  block's delay"; a lost epoch reset arms EARLY) and is corrected there and
  in the seams above.

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
  core (25 host rows at the time, Phase A's debounce included) was untouched
  byte-for-byte — the 2026-09-02 review corrections below are the first
  change to it since — and `AiusRecorderBank` owns `N` `AiusRecorder` slot
  contexts plus
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
  25 existing host rows (Phase A included) already prove it: the deletion
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
- **Review corrections 2026-09-02 (host-only, no device).** A findings pass
  over the bank turned up five real defects and one undocumented behaviour,
  all fixed in `aius_recorder_bank.{h,c}` (plus one new core entry point) and
  pinned by new rows in `firmware/test/recorder/test_recorder_bank.c`:
  - **Recycled slot fired a spurious `CARRIER_LOSS`** from stale per-slot
    epoch history, and a virgin slot dropped a real falling edge on its first
    epoch — the Design section's inheritance bullet. New core API
    `aius_recorder_inherit_epoch_state()`, called on every advance;
    `test_bank_recycled_slot_inherits_carrier_state`,
    `test_bank_advance_captures_first_epoch_carrier_edge`.
  - **`aius_recorder_bank_thaw()` ignored the read lease** that `drain()`
    honours, so an operator thaw could recycle a slot mid-response. It now
    returns `AIUS_REFUSED` while the active slot is leased;
    `test_bank_thaw_refused_while_active_slot_leased`.
  - **The read-lease deferral was unbounded** despite the Design section
    calling it bounded. It now skips past a leased slot after
    `AIUS_RECORDER_BANK_DEFER_DRAINS` (was `AIUS_RECORDER_RECLAIM_EPOCHS`,
    retired 2026-09-03) deferred drains, and exposes
    `aius_recorder_bank_deferred_epochs()` / `aius_recorder_bank_lease_skips()`;
    `test_bank_lease_deferral_is_bounded`,
    `test_bank_lease_deferral_when_every_slot_leased`.
  - **Config skew after a recycle was silent.** A refused config is now
    remembered per slot and applied on that slot's next thaw or recycle, with
    `aius_recorder_bank_config_pending()` for the status document;
    `test_bank_pending_config_applies_on_recycle`,
    `test_bank_pending_config_applies_on_thaw`.
  - **`bank_init()` never clamped the default post-roll** (188 blocks)
    against slot capacity, only `set_config`'s operator value;
    `test_bank_init_clamps_default_postroll`.
  - **The post-advance underrun blindness window** is now a stated contract
    in `aius_recorder_bank.h` and the Design section rather than an
    accident; `test_bank_underrun_blind_window_after_advance`.
  - **New for the Phase C status/data surface:**
    `aius_recorder_bank_newest_frozen()` — `active` advances inside the same
    drain that freezes, so "read the active slot" always lands on a RECORDING
    one; this reports the most recently frozen slot that is still frozen,
    falling back to the highest freeze generation once it isn't
    (`test_bank_newest_frozen_tracks_and_falls_back`). Per-capture
    `rate_change_frames[AIUS_RECORDER_RATE_CHANGE_MAX]` +
    `rate_change_count` in the core snapshot, so a host can count the
    old-rate frames a switch left behind rather than only reject the capture
    (`test_rate_change_fiducials`).

  Native `recorder` suite green — `recorder` 27 rows, `recorder-bank` 22
  rows, `-Wall -Wextra -Werror`, ASan/UBSan, plus `test_no_extmem_heap.py`.
  Per-slot `AiusRecorder` grew 16 bytes (the fiducial array alone);
  `AiusRecorderBank` grew 120 bytes in total (4 × 16 for the slots, 56 for
  the bank's own new fields) — DTCM, allocation-free, no new statics.

### Phase C — HTTP surface + egress tool for multi-slot addressing ✓ SHIPPED 2026-09-01

- outcome: `GET /api/recorder` reports the bank (active slot index +
  per-slot summaries: state, trigger, generation, valid/gap counts,
  `slots_recycled`); `GET /api/recorder/data` gains a `slot` parameter
  (**default: the NEWEST FROZEN slot**, `aius_recorder_bank_newest_frozen()`,
  409 when nothing is frozen — this preserves today's single-shot mental
  model, "read the capture that just froze", for an unmodified client. The
  default was originally written as "active slot" and shipped that way; that
  is dead on arrival, because the bank advances `active` inside the same
  drain that sets FROZEN, so an unmodified client got 409 for every fresh
  capture. Corrected 2026-09-02), acquires that slot's read lease
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
  against it. `aius-recorder-pull.py`'s `--slot` logic was originally
  verified only by hand, against a mocked bank response shaped like the C
  emitter's output, because no harness existed to extend. **Both halves of
  that sentence are now stale (corrected 2026-09-02):** the tool defaults to
  the **newest frozen** slot, not the active one — it reads the device's own
  `newest_frozen_slot`, scans `slots[]` for the highest `freeze_generation`
  against pre-`newest_frozen_slot` firmware, and only falls back to
  `active_slot` when nothing is frozen — and it now has committed coverage,
  `firmware/test/rate-runner/test_recorder_pull.py` (18 rows). Host/bench-only
  — no device flash.
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
  (27+22), `runtime-target` green; `teensy41-optical` cross-build green.
- **Review corrections 2026-09-02 (host-only, no device).** A second findings
  pass over the shipped Phase-C diff turned up two more HIGH defects in the
  HTTP surface itself, plus the status document's missing observables:
  - **The read lease leaked permanently on a network teardown mid-stream.**
    `aius_runtime_network_http.cpp`'s `http_init()` handles
    `AIUS_NET_ACTION_TEARDOWN` by calling `aius_http_transaction_init()`,
    which `memset`s `sd_settle` **without firing it** — only
    `reset_client`/`restart_idle` fire it, and a teardown reaches neither.
    `reads_in_flight[slot]` then stayed 1 for the rest of the boot and the
    bank deferred forever once its round-robin reached that slot: recording
    stopped until reboot. So "releasing exactly once at send-completion /
    recycle / abort" above was **incomplete — teardown is a fourth terminal**.
    Fixed both halves: new `aius_http_transaction_discard()` fires an armed
    hook uncommitted and is called by `http_init()` *before* the re-init
    (refusing a not-yet-`valid` transaction, so the construction-time call is
    a no-op); and `dispatch_recorder()` now releases any still-active lease
    before acquiring a new one, counting it in
    `AiusOpticalAppDispatcher::recorder_lease_leaks` — a diagnostic for a path
    that should not exist, not a supported mode.
  - **The "no `&slot` = active slot" default was dead** (see the corrected
    outcome bullet). Now `aius_recorder_bank_newest_frozen()`, 409 only when
    nothing is frozen. The three source comments claiming a "byte-identical
    pre-bank contract" (`optical_main.cpp`, `aius_optical_app.h`,
    `aius_http.c`) were corrected with it, as was the stale
    "released at the next dispatch" comment on `app_recorder_read_release`.
  - **Status document gained the bank's own observables:**
    `newest_frozen_slot` (int|null — the slot a defaulted read resolves to),
    `deferred_epochs`, `lease_skips`, `config_pending`, `lease_leaks` (the
    dispatcher's repair counter, which should stay 0 forever), and per slot
    `stream_end_frame`, `rate_change_count` + `rate_change_frames[]` (clamped to
    `AIUS_RECORDER_RATE_CHANGE_MAX`; `rate_change_count` may exceed it, and a
    short array is the client's signal that the positions are incomplete). A
    `static_assert` now bounds the worst-case body against
    `AIUS_RECORDER_BANK_SLOTS` × `AIUS_RECORDER_RATE_CHANGE_MAX` and
    `AIUS_RUNTIME_TEENSY_APP_BODY`, so raising either constant cannot silently
    outgrow the buffer.
  - **`stream_end_frame` makes every other stream-indexed field usable.**
    `rate_change_frames` and `trigger.at_frame` are indices in the capture's
    free-running stream, but the pulled WAV is the ring unrolled from
    `oldest_frame` for `valid_frames` frames, so a host could not place a
    fiducial in the file it had just downloaded. With the slot's stream end
    published, `wav_index = stream_index - (stream_end_frame - valid_frames)`,
    and a negative result is a fiducial the ring has already overwritten.
    Emitted from `AiusRecorder::frames_drained` — **not** `end_frame` or
    `frames_staged`, deliberately: `valid_frames`/`oldest_frame` are derived
    from `frames_drained`, so only that counter makes the identity true in
    every state. For a FROZEN slot all three are equal (DRAINING exits to
    FROZEN only once staging is empty); for a still-recording slot
    `frames_staged` would overshoot by the undrained staging depth and place
    every fiducial too early.
  - Malformed `slot` (`-1`, `1abc`, empty, non-numeric, >u32) stays
    ROUTE_UNKNOWN rather than 400 — unchanged behaviour, matching
    `offset`/`length`, now pinned as a stated contract by a five-row table in
    `test_http.c`.

  Host proof: `http` (44 transaction oracle rows, +1: the settle hook on
  re-init/discard, on RESPONSE_TIMEOUT mid-send, and on the **stream** path
  both to completion and to a mid-stream abort — the pre-existing row covered
  only a 2-byte copied body), `optical-app` (38 rows, +1 for the
  double-acquire release), `runtime-target` (+1 registry key,
  `teardown.recorder-lease-released`, end-to-end through the real composition
  with the borrowed body still streaming when the link drops — mutation-checked
  by removing the discard call, which fails it). `teensy41-optical` cross-build
  `[SUCCESS]`. **`optical-cutover` fails identically to its pre-change
  baseline** (a `micros()` in `AiusAudioProcessor::update()`, commit 225253d,
  unrelated) — byte-identical output, no new failure.
  ~~**Owed, not evidenced:** the `newest_frozen_slot` default and every new
  status field live in `optical_main.cpp`, which no native suite compiles;
  only the cross-build proves them. Live re-verification of a defaulted
  `GET /api/recorder/data` against a fresh capture is owed before the next
  deploy.~~ **Device-verified 2026-09-02** on `e746cf952dd6+teensy41-optical`
  (flashed over USB CDC, plain single-image posture): after
  `POST /api/recorder/freeze` the header read `active_slot 1` /
  `newest_frozen_slot 0`, and `GET /api/recorder/data?offset=0&length=4096&generation=0`
  without `slot` returned 200 / 4096 B (a guaranteed 409 before the fix);
  `aius-recorder-pull.py` with no `--slot` chose slot 0 "newest frozen slot
  reported by the device" and pulled 2 MiB in 32 chunks, `gaps=0`, after
  which the header read `lease_leaks 0`, `deferred_epochs 0`,
  `lease_skips 0`. Every new status field (`newest_frozen_slot`,
  `deferred_epochs`, `lease_skips`, `config_pending`, `lease_leaks`, per-slot
  `stream_end_frame` / `rate_change_count` / `rate_change_frames[]`) is
  served, and the fiducial pair was consumed live by the gate-4 capture tool
  ([`aius-rate-gate-capture-tool.md`](aius-rate-gate-capture-tool.md)
  §Live-run results — 2026-09-02): `wav_index = stream_index −
  (stream_end_frame − valid_frames)` placed both `rate_change_frames` inside
  the pulled ring at the frames where the output actually switched. Note
  `GET /api/recorder` requires `X-Aius-SD-Auth` (`401 sd_auth` otherwise): every
  recorder route is token-gated by this plan's contract, the GETs included, and
  `CLAUDE.md`'s "reads need nothing" rule was corrected to say so (2026-09-02).
- **Bench-proven 2026-09-01:** recorder OTA'd as one agnostic image and
  self-committed (auto-rollback unneeded). Phase A debounce suppressed the
  boot-transient underrun (froze at `at_block 416`, not `at_block 1`); the
  4-slot bank auto-advanced twice; two distinct triggers (underrun +
  carrier_loss, the latter induced by a USB deauthorize of the U24 source)
  produced two frozen slots pulled as two distinct complete WAVs (different
  sha256), with slot 2 recording throughout and the read-lease holding both
  slots through their live HTTP pulls (`gaps=0`, no wedge). The data route
  honoured `slot` on-device (out-of-range → 400). One gap the bench surfaced
  and closed: `aius-recorder-pull.py` was stale (never sent `&slot`, refused
  any non-active slot) — fixed to send the param and pull any frozen slot.
  **Its default-slot contract has since changed**: the tool no longer defaults
  to `active_slot` (on a bank that just froze and auto-advanced, that is the
  slot still recording) but to the newest frozen slot — `newest_frozen_slot`
  from the bank header, falling back to the highest-`freeze_generation` frozen
  entry in `slots[]` on firmware without the field, and only then to
  `active_slot`. **No evidence artifact is retained** for this bench run: it
  was recorded to session scratch on 2026-09-01 and `.scratch/` is GC'd after
  7 days, so `at_block 416`, `gaps=0` and the two distinct WAV sha256s are as
  the tools reported them at the time and are not reproducible from this
  repository.
- unit: parent-only

### Phase D — adversarial review + bench proof ✓ SHIPPED 2026-09-03

**Reopened 2026-09-02, closed 2026-09-03.** Its targeted-proof items were owed,
not done, and its own fix carried this review's H1: the `sd_settle` read-lease
release it introduced never fired on a network teardown, so the lease still
leaked permanently mid-stream. The bench items closed on 2026-09-03 (last
bullet): items 1 and 3 PASS on device; item 2's release half PASS, its deferral
half owner-hands (the permission classifier blocks the U24 sysfs write for
agents).

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
  N-slot wrap, one `park()`-then-reboot cycle entered mid-POSTROLL (there is
  no `unpark()` — park is one-way by contract and `bank_init()` at boot is the
  only way back to RECORDING; see the Design section), one read-lease-
  deferred recycle (hold a slow pull open across a trigger that would
  otherwise recycle its slot), and the Phase A debounce — all before the
  listening-room board is touched.
- unit: parent-only
- **✓ Done 2026-09-01.** The second adversarial pass ran against the actual
  A–C diff and caught a real HIGH bug: the Phase-C read-lease released on the
  next dispatch, so a one-shot pull that closed without a follow-up request
  pinned its slot forever and, once the ring wrapped onto it, silently stopped
  recording. Fixed to release via the transaction's true-completion hook
  (`http_response.sd_settle`). The board was then flashed (single agnostic OTA,
  now that the slot-agnostic cutover is live and proven — no cee8b46-isolation
  branch needed), and the full on-device cycle passed (see Phase C's
  bench-proven note). The mid-window power-cut and the DSP transfer-function
  battery were not part of this recorder proof.
- **⚠ Proof still owed — corrected 2026-09-02.** The Done bullet above cited
  Phase C's bench-proven note as evidence for "the full on-device cycle". That
  note records a 4-slot auto-advance, two triggers, two distinct WAVs, the
  read-lease holding through two live pulls, `slot` honoured on-device, and the
  Phase A debounce. It records **none** of the following, and no other evidence
  for them exists in this repository. Each is therefore **not evidenced;
  owed**, not done:
  - `park()` entered **mid-POSTROLL** on device, then the reboot that
    re-arms the bank (there is no `unpark()`).
  - A **read-lease-deferred recycle across a trigger** — holding a slow pull
    open through a trigger that would otherwise recycle its slot. (The bench
    note's "read-lease holding both slots through their live HTTP pulls"
    is the *lease held* case, not the *recycle deferred by it* case.)
  - `park()`/`ota_psram_staging()` re-verified against the bank shape with a
    **rehearsal activation mid-cycle** — proof-item (3). The Phase B note
    already flagged this as target-only code no host suite reaches and
    deferred it here; it was not then carried out.
  - Proof-item (2)'s branch-isolation check is **moot, not met**: the build
    shipped from master after the slot-agnostic cutover went live, which is a
    deliberate supersession of that requirement, not a satisfaction of it.
    Re-read 2026-09-03: the board is back on the slot-agnostic chooser
    (re-cutover, stamp `acfd6e3baba9`,
    [`aius-ota-slot-agnostic.md`](aius-ota-slot-agnostic.md) §Status), so the
    dedicated-branch / per-slot-signed-fallback constraint is moot and Phase
    D's remaining bench items run on this posture, deployed over untagged OTA.
  In addition, this pass **missed a HIGH defect in its own fix**: the
  `sd_settle` release it introduced does not fire on a network teardown
  (`aius_http_transaction_init()` zeroes the hook without calling it), so the
  lease still leaked permanently mid-stream. Found and fixed 2026-09-02 — see
  Phase C's review-corrections bullet. A review that ships a fix should carry a
  test for the fix's own terminals; the four that exist now
  (`response.settle-hook-abort-paths`, `teardown.recorder-lease-released`) are
  that test, added after the fact.
- **Device evidence 2026-09-02 (image `e746cf952dd6+teensy41-optical`, the
  H1–H5 review fixes on board).** What is and is not now evidenced on the
  listening-room board:
  - **Harmful-counter smoke** (`pcm-48k-smoke`, 120 s): pass on both
    `d2d058c` and `e746cf9`, every zero-delta counter clean, no transport
    retries, `max_clock_tx_level_frames` 512. **E2e latency** (PCM production
    row, `bench.py --expect-build`): 823 frames / 17.15 ms raw, 24/24 codes
    — the recorder's per-block work costs nothing visible
    ([`aius-e2e-latency.md`](aius-e2e-latency.md) Phase 3).
  - **Newest-frozen default read and lease counters**: see Phase C's
    device-verified note above.
  - **Two live recycles while locked, no spurious trigger.** During the
    gate-4 runs the bank went from four frozen slots to `slots_recycled 2`:
    slot 0 (frozen by `carrier_loss`) and then slot 1 (`underrun`) were
    recycled to RECORDING at `freeze_generation 1`, and the next operator
    freeze landed on the recycled slot 0 with `trigger operator`, no
    `carrier_loss` fired on either recycle, `deferred_epochs 0`,
    `lease_skips 0`, `lease_leaks 0`. This is the epoch-history inheritance
    path running on device; the H2 provocation proper is the next bullet.
  - **H2 provoked on device, 2026-09-02 14:22 UTC (real carrier drop with
    three slots frozen while locked).** Bank before: slots 0 (`operator`), 1
    and 2 (`underrun`) frozen at `freeze_generation 1`, slot 3 recording,
    `slots_recycled 4`. U24 de-authorised (`/sys/bus/usb/devices/3-1.1/
    authorized` ← 0): `carrier 0`, gate closed, and the bank read slot 3
    frozen by `carrier_loss` (`at_ms 13785862`, `postroll_captured 1`), slot
    0 — the oldest frozen — recycled to RECORDING at `freeze_generation 2`,
    slots 1 and 2 untouched at generation 1, `slots_recycled 5`,
    `deferred_epochs 0`, `lease_skips 0`, `lease_leaks 0`. Exactly one
    `carrier_loss` fired, on the active slot only. Re-authorised ~1.5 min
    later: relock to `tracking` at 48020, and the recycled slot 0 then froze
    on its own on the relock underrun (`underrun`, `at_block 776` = 8.3 s
    after relock, `stream_end_frame 397824`, a fresh epoch counted from 0,
    `gap_count 0`), recycling slot 1 to generation 2 (`slots_recycled 6`).
    The same carrier-less window carried the defect-22 two-PUT proof
    ([`aius-network-hardening.md`](aius-network-hardening.md) defect 22).
    Cost: ~1.5 min of silence in the room; the U24 mixer reset to 70,70 on
    re-enumeration and was restored to 110,110.
  - **The flight recorder caught the gate-2 defect on its own**: an
    `underrun` freeze at 44100 with the 128-frame dropout inside it, plus a
    `carrier_loss` freeze at the 44.1 kHz stimulus start. First real
    field use of the bank.
  - **H1 (teardown lease release) is accepted on its host proof** (the
    `teardown.recorder-lease-released` runtime-target end-to-end row through
    the real composition), 2026-09-02: no route triggers
    `AIUS_NET_ACTION_TEARDOWN`, so provoking it on device needs a physical
    Ethernet unplug mid-pull, which is not scheduled and no longer holds this
    phase.
- **Device evidence 2026-09-03 (bench, image
  `f28a347d0a68+teensy41-optical-slot-a`, slot-agnostic A/B posture,
  `booted_slot 1`; HTTP only, no flash, no activation).** The three owed items
  were attempted; two are blocked at the same two places, so Phase D stays
  IN-PROGRESS:
  - **(1) `park()` mid-POSTROLL then reboot — NOT RUN, blocked twice.** (a) No
    route reaches `park()`: `aius_recorder_bank_park()` is called only from
    `service_ota_activation()` and `service_ota_rehearsal()`
    (`firmware/src/optical_main.cpp:8896`, `:9050`). The operator path is
    `POST /api/ota/rehearse`, which `app_ota_rehearse()` (`:5044-5061`) admits
    only with the enable-activation latch set and the session ARMED, and which
    then erases and programs the inactive flash slot (the A/B scratch window,
    `:9015-9023`) and leaves the bank parked and audio down until reboot. That
    is outside this run's stage-only bound and needs an explicit go-ahead.
    (b) On this image there is no POSTROLL window to enter. The recorder epoch
    runs at ~6,500/s (`/api/status` `loop_epoch` 17831470 → 17894820 over
    9.70 s while `blocks` advanced 910 = 93.8 blocks/s), so
    `AIUS_RECORDER_RECLAIM_EPOCHS` (32; retired the same day for the
    wall-clock `AIUS_RECORDER_RECLAIM_MS 280`, register §Open defects —
    runtime) elapses in ~4.9 ms — inside one
    10.67 ms block period — and the reclaim in `aius_recorder_epoch()`
    (`firmware/lib/aius_recorder/src/aius_recorder.c:385-398`) fires between
    any two consecutive blocks. Every operator freeze today read
    `postroll_captured 0` against `postroll_blocks 188`; the two underrun
    freezes read 4 and 9 (post-stall bursts). `aius_recorder.h:85-89`
    calibrates the constant to "~115 epoch/s, ~280 ms". The configured
    post-roll is therefore effectively never captured — a new defect for the
    register (not recorded here).
  - **(2) Read-lease-deferred recycle — release half PASS, deferral half NOT
    RUN.** Holding a lease on device works: a trickle reader (`SO_RCVBUF`
    1024, 512 B/s, session-scratch script) kept one
    `GET /api/recorder/data?offset=0&length=65536&generation=0&slot=0` open
    for 130 s — `RESPONSE_TIMEOUT_MS` (5 s) is re-armed per completed write,
    so the hold rate has a floor: at 128 B/s the board dropped the client at
    34 s (`http_client_drops` +1). Lease release on every terminal reached —
    PASS: two clean completions, one board-side RST at 65263/65536 B, one
    board-side RESPONSE_TIMEOUT drop, and one client kill mid-stream (own PID,
    26240 B received; `http_client_drops` 1 → 2). Each was followed by a fresh
    `GET /api/recorder/data` (200, 4096 B) and the header read `lease_leaks 0`,
    `deferred_epochs 0`, `lease_skips 0`, slot 0 `frozen` at `freeze_generation
    0` intact. The client kill is the mid-stream-abort terminal, not H1's
    `AIUS_NET_ACTION_TEARDOWN` (still a physical Ethernet unplug). The deferral
    itself was NOT exercised: the board is single-transaction, so no HTTP verb
    can fire while the lease is held, and every out-of-band trigger was refused
    by the permission classifier (U24 sysfs de-auth via `Write` and via shell;
    `speaker-test` / `ffmpeg` / python tone generation for the 44.1 kHz
    rate-switch carrier loss). Expected when it runs against a frozen, leased
    candidate slot (`aius_recorder_bank.c:159-190`): `deferred_epochs 33`,
    `lease_skips 1`, the leased slot untouched, the next unleased slot recycled;
    then the next round-robin pass recycles the released slot.
  - **(3) Rehearsal activation vs the bank — stage-only half PASS, rehearsal
    NOT RUN.** `aius_ota_flash.py` without `--activate` staged
    `canonical-2.aiuo` (575,744 B, image `build_revision acfd6e3baba9`, 0
    retried faults) → verify → arm → `GET /api/ota` `state armed`,
    `activation_available false`; `POST /api/ota/abort` → `state idle`,
    `staged_bytes 0`. Staging never calls `ota_psram_staging()` or `park()`
    (both live only in activation/rehearsal, `optical_main.cpp:8898`,
    `:9052`), and the header confirmed exactly that: no slot ABSENT,
    `deferred_epochs 0`, `lease_skips 0`, `lease_leaks 0`,
    `config_pending false`. What staging did do is the documented SD-traffic
    cost: `tx_underrun_frames` 1280 → 8576 (+152 ms) and two honest `underrun`
    freezes — slot 3 (`at_ms 2679478`, `postroll_captured 4`) recycled slot 0
    to generation 1, which froze 3.5 s later (`at_block 324`,
    `valid_frames 165888`) and recycled slot 1 to generation 1;
    `slots_recycled 0 → 2`. Header before: `active_slot 3`, slots 0/1/2
    `frozen` gen 0 (`underrun` / `operator` / `operator`), slot 3 `recording`.
    After: `active_slot 1` (`recording`, gen 1), slot 0 `frozen` gen 1
    `underrun`, slot 2 `frozen` gen 0 `operator`, slot 3 `frozen` gen 0
    `underrun`. The `park()` / `ota_psram_staging()` half needs
    `POST /api/ota/rehearse` (item 1a).
  - End state: recorder recording on slot 1, OTA `idle`, board image and slot
    unchanged, U24 not re-enumerated, mixer 110,110.
  - **Not evidenced on device at that point** (superseded the same evening,
    next bullet): `park()` mid-POSTROLL (item 1), the deferral/skip itself
    (item 2), and the rehearsal's `park()` / `ota_psram_staging()` against the
    populated bank (item 3).
- **Device evidence 2026-09-03 evening (bench, image
  `25109fca3f97+teensy41-optical-slot-a` deployed over untagged OTA that
  evening — the `AIUS_RECORDER_RECLAIM_MS` fix on board, A/B slot 1 → 2,
  `rollback_observed false`). Items 1 and 3 PASS; item 2 as above.**
  - **Post-roll window exists now.** First operator freeze on the new image:
    slot 0 `frozen`, `trigger operator` (`at_block 4305`, `at_ms 48878`),
    `postroll_captured 188` == `postroll_blocks 188`; the two `underrun`
    freezes the OTA staging run then provoked read 188 as well.
  - **(3) Rehearsal vs the populated bank — PASS, run twice.** Sequence each
    time: `aius_ota_flash.py` stage-only → `state armed`; `POST
    /api/ota/enable-activation` (`activation_enabled true`); operator freeze;
    `POST /api/ota/rehearse` → `{"ok":true,"rehearsing":true}`. Header read
    3 ms after the rehearse 200: all four slots `absent`, `newest_frozen_slot
    null`, `deferred_epochs 0`, `lease_skips 0`, `lease_leaks 0`, every
    slot's frozen metadata (trigger, `postroll_captured`, `stream_end_frame`)
    still readable — `aius_recorder_park()` is the one-store state change the
    Design section describes, nothing else is touched. Retained
    `ota_dark` after the window: `state 4` (COMPLETED), `phase 4`
    (READY_TO_RESET, the emulated reset), `fault 0`, `rehearsal true`,
    `programmed_bytes 577536`, `verified_bytes 577536`, `steps 2397/2397`,
    `image_digest_prefix b4ca6fb227937e09` (= the staged payload's sha256
    prefix), `dark_window_cycles` 3,798,722,947 / 3,819,917,400. Live
    verdict caught on run 2 at +6.98 s: `ota_diag.rehearsal "pass"`,
    `rehearsal_dark_us 6366529`, `rehearsal_max_step_us 45381`,
    `activation_fault none`, `tx_gate.open 1`, `clock tracking`. `ab`
    unchanged throughout (`slot 2`, `booted_slot 2`, `sequence 7`).
  - **(1) `park()` mid-POSTROLL then reboot — PASS on run 2; run 1 was a
    different, also useful, row.** Run 1 posted freeze and rehearse
    back-to-back (~5 ms apart): `aius_recorder_bank_freeze()` only sets
    `freeze_token` and the producer latches it at the next block (≤ 10.67 ms),
    while `service_ota_rehearsal()` parks as soon as
    `reboot_drain_complete()` (a few epochs), so the park reached slot 3 still
    RECORDING with the token pending — it read `absent`, `trigger none`,
    `postroll_captured 0` (RECORDING-with-token → ABSENT, the token discarded
    with the slot). Run 2 waited 0.35 s: the header at +0.391 s read slot 2
    `postroll`, `trigger operator`, `postroll_captured 33`; rehearse at
    +0.397 s; at +0.400 s slot 2 `absent` with `operator` / 33 retained —
    park mid-POSTROLL. **The reboot is the watchdog's, not the operator's:**
    both runs, ~8 s after the rehearsal returned, `boot_generation` +1 with
    `reset_source 16` (`SRC_SRSR_WDOG_RST_B` → `AIUS_RESET_WATCHDOG`,
    `aius_boot_teensy.cpp:396`; WDOG1 `WDOG_WCR_WT(15)` = 8 s) and
    `prev_boot {first_abort 9}`: the quiesce leaves the audio service stopped,
    the epoch aborts at the AUDIO checkpoint and never feeds
    (`optical_main.cpp:9262-9268`), so the "reboot the rehearsal already owes
    for audio" (`:9185-9188`) arrives on its own. `POST /api/reboot` was
    therefore not needed and not sent. After each reboot `bank_init()` re-armed
    all four slots `recording`, `active_slot 0`, every prior capture's
    metadata gone (`trigger none`, `freeze_generation 0`, `valid_frames 0` on
    slots 1–3) — exactly the Design section's "a parked bank forgets every
    slot's FROZEN metadata on the next init"; `/api/ota` `state idle`,
    `staged_bytes 0`; `ota_diag.rehearsal` back to `never-run` (RAM verdict;
    the retained `ota_dark` stamp is the durable one).
  - **(2) Read-lease-deferred recycle**: release half PASS (earlier bullet);
    deferral half owner-hands — the classifier blocks the sysfs write for
    agents, and no HTTP verb can fire while the board's single transaction
    holds the lease.
  - Side observation: `/api/ota` read `activation_available true` on the boot
    after the first rehearsal (the COMPLETED `ota_dark` stamp unlocks it) and
    `false` again after the second reboot with a fresh stage; not pursued.
  - End state: recorder recording on slot 0, OTA `idle`, board on
    `25109fca3f97+teensy41-optical-slot-a` slot 2, `tx_underrun_frames 0`,
    U24 not touched.

## Residual risks / non-goals

- **Total capture window across all slots is unchanged (~21.8 s split into
  4 × ~5.46 s), not extended.** This plan trades one long window for
  several shorter independent ones; it does not add PSRAM budget.
- **Auto-eviction of an unpulled capture after N wraps is an accepted
  trade**, not eliminated — counted (`slots_recycled`; the Design section
  explains why it is not named `slots_overwritten_unpulled`), not silent. An
  operator who wants zero eviction risk still needs to pull promptly; this
  only dilutes the single-shot risk by a factor of N.
- **`N` is compile-time, not runtime-configurable**, to keep Phase B's
  blast radius bounded. A configurable `N` (mirroring how `postroll_blocks`
  is already runtime-configurable) is a plausible follow-up, not scoped
  here.
- **The slow-drift diagnostic class (the 1.4-semitone pitch error
  precedent) is explicitly out of scope** — it wants a slower-sampled trend
  log, not more ring depth or more slots.
- **A slot with an outstanding read lease defers its own recycling, for a
  bounded window.** Under a burst of triggers faster than an operator (or a
  slow network path) can finish pulling, the round-robin waits for the lease
  rather than overwriting a slot mid-response — correctness over liveness —
  but only for `AIUS_RECORDER_BANK_DEFER_DRAINS` drains, after which the
  advance skips past the leased slot (fixed 2026-09-02; the earlier
  implementation deferred indefinitely despite this plan calling it
  bounded). Residual: while every slot is leased at once there is nowhere to
  skip to and the bank keeps deferring, so a set of pathological stuck HTTP
  clients can still stall recording. That is now visible rather than silent
  (`deferred_epochs`, `lease_skips`); a per-lease timeout is the next step
  if it becomes real.
- **The underrun trigger is blind for the settle window after every
  auto-advance** (~85 ms of clean blocks, longer on a dirty stream). By
  design and now documented in `aius_recorder_bank.h` and the Design
  section, but it is a real gap: a second underrun burst arriving inside
  that window goes uncaptured by the incoming slot. A shorter window for a
  re-armed slot than for a boot would need its own justification: the
  transient a re-arm produces is the same shape as a boot's, so it keeps the
  same window until evidence says otherwise.
- **`slots_recycled` answers "how many wraps happened," not "was evidence
  lost."** Distinguishing those needs a "read at least once" bit this plan
  deliberately does not add (see Design). If that distinction becomes
  operationally important, it's a scoped follow-up, not a free byproduct of
  this design.
