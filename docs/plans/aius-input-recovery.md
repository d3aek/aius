---
title: Aius input recovery ladder
type: plans
scope: component
component: aius
status: in-progress
tags: [aius, firmware, audio, spdif, recovery]
updated: 2026-09-03
---

# Aius input recovery ladder

Give Aius a level-triggered recovery path for the real use pattern: hours with no
carrier while the entertainment stack is off, then a burst of heterogeneous
material as the OSD is navigated, then hours of stability on one decoder. Every
reset the firmware has today is edge-triggered, so a box that ends up stuck with
a healthy carrier stays stuck until the carrier drops again.

## State — resume here

**Status:** Phase 1 SHIPPED 2026-09-03, host-proven; its deploy over OTA is
pending ([§Gates](#gates)). Premise re-checked in code at aius HEAD
`a6f7e07` on 2026-09-03 ([§Premise re-check at HEAD](#premise-re-check-at-head-2026-09-03)):
all three stuck states are STILL REACHABLE; none is closed by `dc16e75`
(guarded RX staging), `b977488` (acceptance-reset skip), `018c000`/`3570a21`
(Phase F gate-open floor) or `4c9eb3d` (replay floor). Phase 1 is the plan's
own "observe first" recommendation delivered: instrumentation only, no
behaviour change (event-ring depth/page split, the dark-interval record, the
`input_diag` gate-closed companion watermark, swagger). Phases 2–4 are gated
on that record showing a stuck state in the field. The pure rule and its suite
(`firmware/lib/aius_audio/src/aius_input_liveness.h`,
`firmware/test/input-liveness/`, upstream repository `70009a13f`) are shipped and stay;
nothing calls the rule. The two pending decisions below are D11 in
[`aius-outstanding-work-2026-09.md`](aius-outstanding-work-2026-09.md)
§Decisions, needed only if Phases 2–4 open.

The deployed image is whatever `/api/status` `build_revision` says; posture and
deploy path are in [§Gates](#gates).

| Commit | Contents | Proof |
|---|---|---|
| `2658efcff` | This plan: stuck-state inventory, decisions, phases | docs only; `doc-index --validate-only` clean |
| `70009a13f` | `aius_input_liveness.h` pure rule + `input-liveness` suite + registration in all four registry places | `input-liveness` suite green via the firmware runner (`-Werror`, ASan, UBSan); `registry` guard PASS at 58 arms; 3 mutations each caught at the row that owns the behaviour |
| Phase 1 (2026-09-03, aius firmware commit + upstream repository swagger commit) | `aius_event_ring.h` (ring 8 → 32, cursor paging), `aius_input_dark.h` (dark-interval record + per-phase gate-closed watermark), `input_diag` fields, `event-ring` + `input-dark` suites, swagger | see [Phase 1](#phase-1--see-the-stuck-states-before-acting-on-them---shipped-2026-09-03-host-proven-deploy-over-ota-pending) record |

**Uncommitted tree state:** at the time of writing the Phase 1 change is in
the working tree, owned by this thread, awaiting the orchestrator's commit:
aius `firmware/lib/aius_net/include/aius_event_ring.h`,
`firmware/lib/aius_audio/src/aius_input_dark.h`, `firmware/src/optical_main.cpp`,
`firmware/test/event-ring/`, `firmware/test/input-dark/`,
`firmware/scripts/test.sh`, `firmware/README.md`, this plan; upstream repository
`swagger.json`, `test/swagger.test.js`. Dirty
paths elsewhere belong to other sessions and are foreign.

**Next action:** deploy the Phase 1 image over OTA per [§Gates](#gates) (a
clean-stamp build, signed WITHOUT `--slot` while `/api/status` `ab` is null),
then read `input_diag.dark_*` ([Phase 1](#phase-1--see-the-stuck-states-before-acting-on-them---shipped-2026-09-03-host-proven-deploy-over-ota-pending)
record) after the next OSD session. The evidence rule: stuck states 1 (gate
never opens, either clause) and 3 (rate latched unsupported) show as
`dark_phases[1|2|4].over_count > 0` (a completed gate-closed run past
`dark_alert_ms` in rate_unsupported, clock_acquiring or awaiting_block), or
live as `dark_phase ∈ {1, 2, 4}` with `dark_phase_ms > dark_alert_ms`; stuck
state 2 (class latched and silent) shows as a `dark_intervals` entry, or an
open `dark_open` interval, with `entry_gate 1` (production stopped under an
open gate). A dark interval closes only on a commit, never on a carrier drop,
so an interval's exit snapshot is always the recovery; `exit_gate 0` there is
the normal armed-not-yet-executed shape, not evidence. Any of those readings
opens Phase 2. Wiring the
rule is Phase 3 and stays gated. When that gate opens, the wiring recipe is:
call `aius_input_liveness_step()` once per epoch beside `input_fault_service()`
(`firmware/src/optical_main.cpp:8273`, called at `:8314` from the
`AIUS_BOOT_CHECKPOINT_REPLAY` checkpoint service), feeding it
`committed_blocks` and `sink_attempts` from `aius_pipeline_get_service_diag`,
`bursts_ok` from `aius_pipeline_get_parser_diag` (`AiusIecDiag`,
`firmware/lib/aius_iec61937/include/aius_iec61937.h:140-148`),
`input_words_consumed` from `aius_audio_service_get_diag`
(`firmware/lib/aius_audio_service/include/aius_audio_service.h:72`), and
`AudioInputSPDIF3::pllLocked()`. Latch per-class max duration, over-count, last
duration and last-at into new `input_diag` fields in `http_input_diag_json()`
(`firmware/src/optical_main.cpp:3183`), then update
`swagger.json`.

**Decisions taken this session (owner, in chat, 2026-08-18):**

- **The fault test is input-referenced.** Silence is suspect only when input
  words arrive and are consumed while nothing reaches the sink. Completed burst
  outcomes and a confirmed PCM run both count as healthy framing and mean
  source intent, so a deliberately quiet source is never fought. Chosen because
  pause and null bursts complete by design and a rule that cannot tell them
  from a fault would fire during every quiet passage.
- **Automatic escalation is capped at a pipeline reset.** Rung 0 reclassify,
  rung 1 full pipeline plus parser plus codec-slot reset. Clock re-accept, PLL
  replan, receiver re-init and reboot are all out of scope for automatic
  action. Chosen because a reboot mid-film is worse than the fault.
- **Settling mode.** Recovery is rate-limited and partly suppressed for a
  bounded window after a carrier acquisition and tightens once one decoder has
  been producing. Chosen to avoid reset storms while the source thrashes
  through the OSD.
- **No firmware reaches the device until host proof is green and an adversarial
  review has been exhausted** (owner instruction, 2026-08-18). This is a hard
  gate on Phase 4, not a preference.
- **The plan was committed on its own**, against the usual "commit a plan with
  its first useful delivery boundary" preference, because the firmware gate
  above made a docs-only boundary the natural one.

**Traps:**

- **`bash-guard.deprecated-test-shim` fires on `scripts/test.sh` at a statement
  boundary**, and `command_anchor: statement_wide` prefixes
  `(?:^|[;&|({\`])\s*` (`src/tools/hook/src/handlers/bash-policy/PrimitiveEvaluator.cs:134`).
  A `bash -c 'cd … && scripts/test.sh …'` invocation therefore matches on the
  `&&` and is denied. Run the firmware suites by their full repo-relative path
  from the repo root instead, which has no boundary before `scripts/`:
  `firmware/scripts/test.sh --environment native --suite <name>`.
  This is not a false positive and needs no hook change.
- **A repository-layer allow-rule can never unblock a denied shape.**
  `RuleEvaluator.Evaluate` is first-match-wins *with deny shadow*
  (`src/tools/hook/src/handlers/bash-policy/RuleEvaluator.cs:24-37`): it keeps
  scanning past the first match and any deny that matches at all wins,
  regardless of layer or position. Do not reach for `.claude/hook.json`
  `policy.rules` to clear a denial; only tightening or disabling the deny works.
- **The suite's counters must only ever go up, and that is enforced by shape.**
  Rows advance a cursor by deltas and may not name absolute values. The first
  draft used fixed baselines, so a row after one that advanced a counter drove
  it backwards; the rule's wrap-correct unsigned subtraction read that as a
  ~4-billion delta and the de-escalation row passed while exercising nothing. A
  mutation permitting de-escalation passed the whole suite. Only the two wrap
  scenarios may name absolute values.
- **The flight recorder was found FROZEN from boot bring-up** (trigger
  `underrun` at `at_ms` 2942, boot generation 4) and captured none of the
  2026-08-18 event. It has been thawed and is recording. After any reboot it
  must be thawed again (`POST /api/recorder/thaw`, SD auth); boot bring-up
  spends its one-shot freeze. A frozen recorder counts nothing,
  `suppressed_triggers` included.
- **`production_gap_*` is blind to every stuck state this plan targets.** The
  watermark resets its sentinel whenever the transmit gate is closed
  (`firmware/src/optical_main.cpp:1800-1801`), by design, so an unchanged
  `production_gap_over_count` is not evidence of continuity across a
  gate-closed interval. Since Phase 1 the `input_diag.dark_*` companion
  measures exactly that region; read it instead.
- **GET `/api/events` pages since Phase 1.** A page after `cursor` starts at
  the OLDEST retained entry, not the newest eight, and `next` is the last
  entry served when the page is full. A one-shot read with `cursor=0` on a
  busy ring therefore returns the oldest eight of thirty-two; poll with `next`
  until a page comes back short. The soak, HTTP runner and rate bench already
  do.
- The deployed image's `gate_open_target_frames` is 128 from a
  `platformio.ini` override; the source default reads 256. Do not reconcile
  either to the other.
- `tx_underrun_frames` is gate-open-only, so lifetime comparisons across images
  see a semantic step.

### Findings and loose ends

Each is a proposal, none agreed.

- ~~**`http_events_json` conflates ring depth with page size**~~ Resolved in
  Phase 1: `firmware/lib/aius_net/include/aius_event_ring.h` separates
  `AIUS_EVENT_RING_DEPTH` (32) from `AIUS_EVENT_RING_HTTP_PAGE` (8) and
  `AIUS_EVENT_RING_UDP_PAGE` (4); the three `% 8u` literals are gone and both
  readers page via `cursor`.
- ~~**Deepening the event ring has a placement cost.**~~ Decided in Phase 1:
  the ring stays a plain DTCM `.bss` static. OCRAM was checked and is closed
  (`firmware/test/memory/verify_target.py` holds OCRAM free at the defect-26
  128 KiB heap floor with 32 B of margin, so a DMAMEM ring would need funding
  first), and `.bss` zero-initialisation keeps the reader's
  `event.sequence == sequence` staleness test exact without a generation
  stamp. Cost: +288 B DTCM for the ring (32 × 12 B + 2 counters, minus the old
  8-entry ring), decided against the memory report, not by feel.
- **The live board reports `config_valid: 0`, `config_error_code: 10`,
  `config_error: "not loaded"` with `model_origin: "put"`.** It is running a
  PUT DSP model and has never loaded one off the card. Pre-existing, unrelated
  to this thread, not investigated. Proposed: investigate separately.
- **`allow_dark_tx_underrun` in
  `firmware/tools/net/aius_rate_bench.py` is live, not dead**: removing it
  failed 9 `rate-runner` and `rate-gate-capture` cases on 2026-10-18, whose
  transition fixtures advance gate-open `tx_underrun_frames`. It stays
  ([`aius-output-jitter.md`](aius-output-jitter.md) §State traps).

### Pending decisions

**1. Does the automatic ceiling get an exception for the tracker-never-accepted
case?** Exact words to re-ask: *"The cap you set, pipeline reset as the
automatic ceiling, does not cover the stuck state most likely in your pattern.
A pipeline reset re-enters `enqueue()` and can re-arm `g_clock_live_ready`, but
only when the plan is applied and the block rate matches. When the blocker is
the rate tracker never accepting, a pipeline reset cannot help at all. Observe
first, or carve one exception?"*

- **Observe first (recommended).** Ship the Phase 1 instrumentation, leave the
  clock rung unbuilt, and let the field record say whether the case occurs.
  Respects the cap and matches this project's evidence-first habit.
- **Carve one exception.** Allow a clock re-accept rung for this clause only,
  pipeline reset stays the ceiling everywhere else. Fixes the likely case now,
  at the cost of a transmit-ring disturbance on a false positive.

Blocked until answered: Phase 3's rung table only. Phases 1 and 2 proceed
either way.

**2. Should the recorder's `carrier_loss` trigger be disabled before the next
deliberate pause?** Exact words to re-ask: *"The recorder's `carrier_loss`
trigger is armed, and a pause kills the carrier, so your next deliberate pause
will burn the one-shot freeze and leave it frozen through the resume you want
captured. Disable that trigger, or leave it armed to capture the pause edge?"*

- **Disable `carrier_loss`, keep overrun and underrun.** Keeps the freeze
  available for the resume, which is where the symptom is.
- **Leave it armed.** Captures the pause edge instead.

The exact `PUT /api/recorder/config` body is in the swagger
(`swagger.json`), every key required; do not hand-roll a partial
body. Blocked until answered: nothing in this plan; it only affects what the
next live event captures.

## Why the existing machinery does not cover this

Every reset path in the firmware is an edge:

| Trigger | Site | Fires on |
|---|---|---|
| Debounced carrier relock | `firmware/src/optical_main.cpp:8227-8229` | unlocked for at least 3 epochs (~27 ms), then locked. Born saturated, so the first lock after power-on resets too |
| Rate quiesce | `firmware/src/optical_main.cpp:8102-8111` | tracker sees a changed rate sample: closes the gate, requests a reset |
| Nominal acceptance | `firmware/src/optical_main.cpp:8115` | 8 samples spanning at least 1 s inside a 1 mHz band, then replans the PLL |
| Replay start | `firmware/src/optical_main.cpp:5729` | operator action |
| PCM to compressed retype | `firmware/lib/aius_pipeline/include/aius_pipeline.h:45` | two preamble pairs spaced 512 to 8192 words, or one header naming an unsafe type |
| Compressed to PCM starve | `firmware/lib/aius_pipeline/include/aius_pipeline.h:71` | 16,384 words (~170 ms) with no burst outcome |

Nothing is level-triggered. No code asks whether a locked carrier that has
produced nothing for several seconds is the source's intent or the box's
failure.

### The three stuck states

- **Gate never opens.** `g_clock_live_ready` is set true in exactly one place,
  inside `enqueue()` at `firmware/src/optical_main.cpp:1733`, and only after a
  decoded block has already cleared the applied-plan and rate-match guards. It
  is forced false on every nominal acceptance
  (`firmware/src/optical_main.cpp:8130`). The transmit gate needs it
  (`firmware/lib/aius_replay/include/aius_tx_gate.h:54`). A source flapping
  faster than the pipeline can land one block at the newly accepted rate leaves
  the gate dark under a locked carrier, with no timeout anywhere.
- **Class latched and silent.** `aius_pipeline.h:75` states it outright:
  framed-but-unsupported streams, pause bursts and null bursts complete bursts
  and therefore never starve. Correct for a genuinely silent source, and
  indistinguishable from a mis-latched class.
- **Rate latched unsupported.** `g_clock_carrier_unsupported` clears only on
  carrier loss (`firmware/src/optical_main.cpp:8182-8183`) or on a fresh nominal
  acceptance (`:8131`). A locked carrier parked at an unsupported measured rate
  has no timeout.

### Premise re-check at HEAD (2026-09-03)

Code re-check at aius HEAD `a6f7e07` (no hardware), per
[`aius-outstanding-work-2026-09.md`](aius-outstanding-work-2026-09.md) Phase 1
step 1. Verdict per state:

| State | Mechanism at HEAD | HEAD file:line | Verdict | Closing commit |
|---|---|---|---|---|
| Gate never opens | `g_clock_live_ready` true only in `enqueue()` after the plan-applied and rate-match guards; forced false at acceptance, on `!carrier_ready`, in `close_stream_rate_gate()`; `aius_tx_gate_permitted` needs it | `optical_main.cpp:1721-1733`, `:8100`, `:8130`, `:364`, `:8238`; `aius_tx_gate.h:54` | STILL REACHABLE | none |
| Gate never opens (tracker clause) | tracker `carrier_ready` true only in `accept_candidate()` after `AIUS_CLOCK_ACCEPT_SAMPLES` agreeing samples; every disagreeing sample clears it; no timer | `aius_clock.c:200`, `:266-292` | STILL REACHABLE | none |
| Class latched and silent | completed bursts (AC-3, pause, null) never starve the DTS class back to UNKNOWN; the only pipeline commit since the plan re-labels a cut burst TRUNCATED | `aius_pipeline.h:71-77`; `dd9520e` | STILL REACHABLE | none |
| Rate latched unsupported | set on `result.unsupported_accepted`; cleared only at acceptance or when `!carrier`; no timer | `optical_main.cpp:8172-8183`, `:8131` | STILL REACHABLE | none |

What the cited commits did, and why none closes a row: `dc16e75` re-points the
receive DMA vector at a guarded stager (`aius_spdif_rx.h`) and adds
`rx_dropped_halves` / `pool_blocks_used*` to `input_diag`; it closes a fourth
wedge (RX pool corruption under replay, "pipeline never typed the stream
again") that was never in this inventory. `b977488` adds
`g_audio_ingest_since_reset` (`optical_main.cpp:8121-8124`) so acceptance skips
a redundant pipeline reset; the `g_clock_live_ready = false` at `:8130` is
untouched. `018c000`/`3570a21` (Phase F) defer the physical gate open to
`enqueue()` (`tx_gate_actuate(true)` only arms at `:2227`;
`tx_gate_execute_open()` runs at `:1827`, after the `:1733` set), so the open
still requires a block to land; `4c9eb3d` sizes the replay floor only. No
level-triggered path exists in `firmware/lib/aius_clock` or `optical_main.cpp`
(no timeout, stall or re-accept clause). Disposition: `status: in-progress`;
Phase 1 (instrumentation) is next; Phases 2–4 gated as stated in §State.

### The existing instrument is blind exactly here

The production-gap watermark at `firmware/src/optical_main.cpp:1798-1819` is already
the right measurement: micros() between accepted blocks. It resets its sentinel
whenever the transmit gate is closed (`:1800-1801`), by design, so that a source or
rate switch never spans a bogus gap. Every stuck state above lives in the region
that reset deliberately ignores. This is why `production_gap_over_count` read
unchanged through a real 36-minute dark window on 2026-08-18.

### Field evidence from the 49-hour boot read on 2026-08-18

- `squelched_frames` 64,682, which is 129,364 words of confirmation squelch at
  8,192 words per run, so roughly 16 PCM runs have entered. The OSD phase is
  real and is happening.
- `retype_count` 3. Three of those re-typed to compressed mid-flight.
- `reclassify_count` 0. The compressed-to-PCM starvation path has never fired.
  The other PCM runs ended by carrier loss, not by in-pipeline self-heal.
- `unsupported_episodes` 101, `invalid_windows` 99, `service_requested_resets` 147.
- `event_overflows` 126 on an 8-entry ring.

Read together: in the field so far, essentially every class transition has been
carried by the carrier dropping. The self-heal paths that would cover a format
switch with the carrier held up are close to untested, and that is precisely the
OSD case.

## Decisions taken

- **The fault test is input-referenced.** Silence is suspect only when input
  words are arriving and being consumed while nothing reaches the sink.
  Completed burst outcomes and a confirmed PCM run emitting both count as
  healthy framing and mean "source intent", so a deliberately quiet source is
  never fought. Pause and null bursts complete, so they read as intent.
- **Automatic escalation is capped at a pipeline reset.** Rung 0 reclassify,
  rung 1 full pipeline plus parser plus codec-slot reset. Clock re-accept, PLL
  replan, receiver re-init and reboot are all out of scope for automatic action
  this cycle.
- **Settling mode.** Recovery is rate-limited and partly suppressed for a
  bounded window after a carrier acquisition, and tightens once one decoder has
  been producing continuously. Avoids reset storms while the source is
  legitimately thrashing through the OSD, and stays responsive during the long
  stable period where a stuck state costs a whole film.

Vocabulary follows the glossary's existing degradation-ladder shape
([`docs/reference/glossary.md`](../../../../docs/reference/glossary.md)): a
**ladder** of ordered **rungs**, recovery being the mirror image of degradation.
"Governor" is already the latency rule (`firmware/lib/aius_audio/src/aius_tx_governor.h`)
and "supervisor" is already the network stack's, so neither is reused here.

## Open decision

**The capped ceiling does not cover the stuck state most likely in this
pattern.** A pipeline reset re-enters `enqueue()` and can therefore re-arm
`g_clock_live_ready`, but only when `g_clock_plan_applied` is true and the block
rate matches the accepted nominal. When the blocker is `g_clock_carrier_ready`
staying false because the rate tracker never accepted, a pipeline reset cannot
help at all, and that is the failure mode a flapping OSD produces.

Options:

1. **Observe first (recommended).** Ship Phase 1 instrumentation, leave the
   clock rung unbuilt, and let the field record say whether the tracker-never-
   accepted case actually occurs. Respects the cap and matches this project's
   evidence-first habit.
2. **Carve one exception.** Allow a clock re-accept rung for this clause only,
   keeping pipeline reset as the ceiling everywhere else.

Blocked until answered: Phase 3's rung table only. Phases 1 and 2 proceed either
way.

## Proposed thresholds

Derived from the legitimate worst cases already in the code, not picked freely:

- Nominal acceptance needs `AIUS_CLOCK_ACCEPT_SAMPLES` 8 samples spanning
  `AIUS_CLOCK_ACCEPT_STABLE_MS` 1000 ms (`firmware/lib/aius_clock/include/aius_clock.h:15`).
- PCM confirmation squelch is 8,192 words, about 170 ms.
- Compressed starvation is 16,384 words, about 170 ms.
- The worst legitimate E-AC-3 outcome gap is 12,288 words, about 256 ms.

So a legitimate acquisition can honestly take about 1.5 s. Proposed: **3 s**
dark-with-carrier in steady phase, **10 s** during settling, settling ends after
**5 s** of continuous committed output, and at most one ladder walk per window
with exponential backoff that resets on reaching steady.

## Phases

### Phase 1 — see the stuck states before acting on them — ✓ SHIPPED 2026-09-03 (deployed over OTA, image `25109fca3f97`)

- outcome: the three stuck states are observable in the field. The event ring
  is deep enough to survive an OSD navigation burst, a structured recovery
  record names each dark interval with its phase, class, carrier-ready and
  gate state at entry and exit, and the production-gap instrument gains a
  companion watermark that measures the gate-closed region it currently
  discards. No behaviour changes.
- seams: the event ring and `record_event()`
  (`firmware/src/optical_main.cpp:1235`), the production-gap block at
  `firmware/src/optical_main.cpp:1798-1819`, the `input_diag` serializer
  `http_input_diag_json()` at `firmware/src/optical_main.cpp:3183`, swagger.
- targeted-proof: host suite rows for the new watermark across gate-open,
  gate-closed and transition sequences; a swagger extractor row for the new
  fields; ring-depth row proving an OSD-rate event burst does not overflow.
- unit: parent-only

**Shipped record (2026-09-03).** Two pure headers, one epoch service, no
behaviour change: `request_audio_reset()` keeps its five call sites, nothing in
`firmware/src` references `aius_input_liveness`, and the only new per-epoch
work is `input_dark_service()` after `input_fault_service()` in the REPLAY
checkpoint (field writes and one 44-byte service-diag copy).

- **Event ring** — `firmware/lib/aius_net/include/aius_event_ring.h`:
  `AIUS_EVENT_RING_DEPTH 32`, `AIUS_EVENT_RING_HTTP_PAGE 8`,
  `AIUS_EVENT_RING_UDP_PAGE 4`; `g_event_ring` (392 B DTCM `.bss`, +288 B over
  the old ring). Sized from the 2026-08-18 read: 134 ring events over 49 h in
  ~16 OSD sessions with no poller, at most four entries per polled transition
  (two state edges, one rate episode, one clock fault or overrun edge), so 32
  holds eight transitions between two polls. `GET /api/events` and UDP
  GET_EVENTS page from the oldest retained entry after `cursor`; `next` is the
  last entry of a full page, else the newest sequence.
- **Dark-interval record + gate-closed watermark** —
  `firmware/lib/aius_audio/src/aius_input_dark.h`: `AiusInputDarkPhase`
  (0 no_carrier, 1 rate_unsupported, 2 clock_acquiring, 3 plan_pending,
  4 awaiting_block, 5 gate_armed, 6 gate_open), `AIUS_INPUT_DARK_ALERT_MS
  3000`, `AIUS_INPUT_DARK_RECORD_MIN_MS 500`, `AIUS_INPUT_DARK_RECORD_DEPTH 8`;
  `g_input_dark` (248 B DTCM `.bss`, `nm` 0xf8). `POST /api/clear` rebases
  its `last_committed` beside the pipeline service-diag clear so the counter
  drop never reads as a commit. A dark interval is commit-bounded (the
  liveness boundary), so production stopping under an open gate is recorded;
  the watermark is per gate-closed phase run. "Phase" is the gate ladder's
  position, "class" the pipeline's `AiusPipelineClass` (0/1/2), so the words
  do not collide with `aius_pipeline.h`.
- **`input_diag` fields** — `dark_alert_ms`, `dark_record_min_ms`,
  `dark_phase`, `dark_phase_ms`, `dark_open`, `dark_open_ms`,
  `dark_open_start_ms`, `dark_open_entry_{phase,class,ready,gate}`,
  `dark_interval_count`, `dark_phases[6]` (`{phase, max_ms, over_count,
  last_ms, last_at_ms}`), `dark_intervals[≤8]` (`{start_ms, ms,
  entry_{phase,class,ready,gate}, exit_{phase,class,ready,gate}}`), oldest
  first. The serializer's format strings are `PROGMEM` (flash) so the ~1 KB of
  new literals stays out of DTCM `.data`; the swagger extractor resolves a
  `static const char <name>[] PROGMEM` format by name.
- **Memory** (`scripts/build.sh teensy41-optical`, SUCCESS): ITCM 190,956 used
  / 5,652 free, unchanged from baseline (0 B of the ≤ 512 B budget); DTCM
  301,760 used / 25,920 free for locals (+544 B `.bss`; the old serializer
  literal left `.data` and every new literal is in flash);
  OCRAM 393,216 used / 131,072 free, unchanged. Every post-link guard PASS
  (hot-object ITCM still 560 audio-block bytes in 5 pinned sections).
- **Proof** — `event-ring` (8 scenario blocks: empty ring, 32-event OSD burst
  with zero overflows paged in four full pages, mid-ring / current / stale
  cursors, 33rd-event overflow, 2× depth burst, reset) and `input-dark` (12:
  ladder order, stable decode, stack-off then OSD walk with each phase's run in
  its own bucket, stuck states 1/2/3 each parked in its phase, AC-3 inter-frame
  gaps not recorded, the 500 ms floor, ring overwrite, millis and counter
  wraps, reset) green under `-Werror`/ASan/UBSan; `registry` PASS at 63 arms;
  `optical-cutover` 44/44 unchanged with no pin re-pinned; `itcm-placement`,
  `memory`, `net`, `input-liveness` green; upstream repository
  `test/swagger.test.js` 11/11 with the new `InputDiag`,
  `InputDarkPhase`, `InputDarkInterval` and `Events` paging assertions.
- **Deployed 2026-09-03** over untagged OTA to the bench board
  (`25109fca3f97+teensy41-optical-slot-a`, A/B slot 1 → 2, `rollback_observed
  false`; ITCM 191,308 used / 5,300 free on the slot env). Live on the wire
  9 s after boot: `input_diag` `dark_alert_ms 3000`, `dark_record_min_ms 500`,
  `dark_phase 6` (gate_open, tracking), `dark_phase_ms 8249`, `dark_open 1`,
  `dark_interval_count 1`, `dark_phases[6]` (phase 2 `max_ms 1645`, phase 3
  `593`, phase 4 `95`, phase 5 `11`), `dark_intervals[]` = one boot interval
  `{start_ms 646, ms 2333, entry_phase 2, entry_class 0, exit_phase 5,
  exit_class 1, exit_ready 1, exit_gate 0}`. `GET /api/events` → `{cursor 0,
  next 4, events[4]}` (sequences 1–4: state 2/detail 10, rate 48000, state
  3/detail 1, state 3), `GET /api/events?cursor=4` → `{cursor 4, next 4,
  events[]}`.

### Phase 2 — the recovery rule, pure and host-proven

- outcome: a portable pure rule that consumes carrier lock, gate state, input
  words consumed, framing-evidence deltas, committed-block deltas, class,
  carrier-ready and a timestamp, and returns which rung to fire or none.
  Settling mode, backoff and the suppression clauses live inside it. Nothing
  calls it yet.
- seams: a new `firmware/lib/aius_audio` rule header and its suite, following
  the `aius_tx_governor.h` precedent.
- targeted-proof: a new suite covering each stuck state, each legitimate
  silence case that must be suppressed (pause bursts, null bursts, digital
  black PCM, deliberate operator pause), the settling boundary, backoff and
  its reset. Property-style sweep over interleavings of the delta inputs.
  The rule itself (`aius_input_liveness.h` + `input-liveness` suite) already
  shipped in upstream repository `70009a13f`; what remains here is re-proving it against
  the Phase-1 record's actual stuck-state shapes.
- unit: parent-only
- dependencies: Phase 1, AND the Phase-1 record showing a stuck state in the
  field (gate set 2026-09-03; until then Phases 2–4 stay unstarted, per
  [`aius-outstanding-work-2026-09.md`](aius-outstanding-work-2026-09.md)
  Phase 1 step 2)

### Phase 3 — wire it in, default off

- outcome: the rule is called from the epoch, its rungs actuate through the
  existing `request_audio_reset()` path, and every walk lands in the Phase 1
  record. Gated by a build flag defaulted OFF, matching how the latency
  governor shipped.
- seams: the epoch checkpoint that owns `tx_gate_service()`, `request_audio_reset()`
  (`firmware/src/optical_main.cpp:899`), the status serializer.
- targeted-proof: host runtime-oracle trace showing the rung firing at the
  right epoch and the reset sequence advancing exactly once per walk; a trace
  proving the flag OFF path is inert.
- unit: parent-only
- dependencies: Phase 2

### Phase 4 — prove on hardware, then enable

- outcome: the mechanism is verified against the real pattern (stack off, OSD
  navigation, long stable decode) with the flight recorder armed, then the flag
  is turned on in a signed A/B image for the inactive slot.
- seams: no source change expected beyond the flag default.
- targeted-proof: a live session covering all three phases of the pattern with
  the recovery record read after each; zero unexplained rung firings during
  legitimate silence.
- unit: parent-only
- dependencies: Phase 3

## Gates

By owner instruction (2026-08-18): **no firmware reaches the device until its
host proof is green and an adversarial review has been run against the changed
seam and exhausted.** Phase 1 (instrumentation, no behaviour change) cleared
that gate on 2026-09-03 (host proof green, delivery audit's six findings
folded) and deploys over OTA. Phases 2 and 3 are host-only. Phase 4 is the
first BEHAVIOUR change that touches the board. Deploy posture re-baselined
2026-09-03 without changing the gate: the board ran the plain single-image build (`/api/status` `"ab": null`,
no A/B, no chooser) after the 2026-09-01 plain-image flashes, an interim bench
posture; the "sign for the inactive slot with `--slot`" wording applied to the
per-slot A/B scheme, and under the slot-agnostic chooser a container is signed
WITHOUT `--slot`. A re-cutover to the slot-agnostic A/B posture is
scheduled (design decision D1, 2026-09-03;
[`aius-outstanding-work-2026-09.md`](aius-outstanding-work-2026-09.md) Phase
5), and that plan makes OTA the deploy path for every firmware phase after it:
Phase 4 therefore lands as an untagged signed container staged, verified and
activated over OTA, never a USB flash. Read `/api/status` `ab` first and sign
per [`../../CLAUDE.md`](../../CLAUDE.md) §Device access and OTA.

## Traps carried in from the jitter thread

- The deployed image's `gate_open_target_frames` is 128 from a `platformio.ini`
  override; the source default reads 256. Do not reconcile either to the other.
- `tx_underrun_frames` is gate-open-only, so lifetime comparisons across images
  see a semantic step.
- A frozen flight recorder counts nothing, `suppressed_triggers` included. It
  was thawed on 2026-08-18 after being found frozen from boot bring-up.
- After any reboot the recorder must be thawed again
  (`POST /api/recorder/thaw`, SD auth): boot bring-up spends its one-shot
  freeze.
