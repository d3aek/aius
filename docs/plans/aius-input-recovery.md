---
title: Aius input recovery ladder
type: plans
scope: component
component: aius
status: draft
tags: [aius, firmware, audio, spdif, recovery]
updated: 2026-08-18
---

# Aius input recovery ladder

Give Aius a level-triggered recovery path for the real use pattern: hours with no
carrier while the entertainment stack is off, then a burst of heterogeneous
material as the OSD is navigated, then hours of stability on one decoder. Every
reset the firmware has today is edge-triggered, so a box that ends up stuck with
a healthy carrier stays stuck until the carrier drops again.

## State — resume here

**Status:** planned, nothing built. Design decisions below are taken (owner, in
chat, 2026-08-18). One decision remains open (§Open decision). No firmware
change exists yet, and by owner instruction none reaches the device until it is
host-proven and adversarially reviewed.

The device currently runs slot A of `ceffc20dae6c+teensy41-optical-slot-a`
(built 2026-08-16T19:31:13Z), `ab.sequence` 11, boot generation 4.

## Why the existing machinery does not cover this

Every reset path in the firmware is an edge:

| Trigger | Site | Fires on |
|---|---|---|
| Debounced carrier relock | `firmware/src/optical_main.cpp:7699` | unlocked for at least 3 epochs (~27 ms), then locked. Born saturated, so the first lock after power-on resets too |
| Rate quiesce | `firmware/src/optical_main.cpp:7582` | tracker sees a changed rate sample: closes the gate, requests a reset |
| Nominal acceptance | `firmware/src/optical_main.cpp:7595` | 8 samples spanning at least 1 s inside a 1 mHz band, then replans the PLL |
| Replay start | `firmware/src/optical_main.cpp:5452` | operator action |
| PCM to compressed retype | `firmware/lib/aius_pipeline/include/aius_pipeline.h:45` | two preamble pairs spaced 512 to 8192 words, or one header naming an unsafe type |
| Compressed to PCM starve | `firmware/lib/aius_pipeline/include/aius_pipeline.h:71` | 16,384 words (~170 ms) with no burst outcome |

Nothing is level-triggered. No code asks whether a locked carrier that has
produced nothing for several seconds is the source's intent or the box's
failure.

### The three stuck states

- **Gate never opens.** `g_clock_live_ready` is set true in exactly one place,
  inside `enqueue()` at `firmware/src/optical_main.cpp:1684`, and only after a
  decoded block has already cleared the applied-plan and rate-match guards. It
  is forced false on every nominal acceptance
  (`firmware/src/optical_main.cpp:7605`). The transmit gate needs it
  (`firmware/lib/aius_replay/include/aius_tx_gate.h:53`). A source flapping
  faster than the pipeline can land one block at the newly accepted rate leaves
  the gate dark under a locked carrier, with no timeout anywhere.
- **Class latched and silent.** `aius_pipeline.h:75` states it outright:
  framed-but-unsupported streams, pause bursts and null bursts complete bursts
  and therefore never starve. Correct for a genuinely silent source, and
  indistinguishable from a mis-latched class.
- **Rate latched unsupported.** `g_clock_carrier_unsupported` clears only on
  carrier loss (`firmware/src/optical_main.cpp:7657`) or on a fresh nominal
  acceptance (`:7606`). A locked carrier parked at an unsupported measured rate
  has no timeout.

### The existing instrument is blind exactly here

The production-gap watermark at `firmware/src/optical_main.cpp:1744` is already
the right measurement: micros() between accepted blocks. It resets its sentinel
whenever the transmit gate is closed (`:1751`), by design, so that a source or
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

### Phase 1 — see the stuck states before acting on them

- outcome: the three stuck states are observable in the field. The event ring
  is deep enough to survive an OSD navigation burst, a structured recovery
  record names each dark interval with its phase, class, carrier-ready and
  gate state at entry and exit, and the production-gap instrument gains a
  companion watermark that measures the gate-closed region it currently
  discards. No behaviour changes.
- seams: the event ring and `record_event()`
  (`firmware/src/optical_main.cpp:1207`), the production-gap block at
  `firmware/src/optical_main.cpp:1744`, the `input_diag` serializer at
  `firmware/src/optical_main.cpp:3041`, swagger.
- targeted-proof: host suite rows for the new watermark across gate-open,
  gate-closed and transition sequences; a swagger extractor row for the new
  fields; ring-depth row proving an OSD-rate event burst does not overflow.
- unit: parent-only

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
- unit: parent-only
- dependencies: Phase 1

### Phase 3 — wire it in, default off

- outcome: the rule is called from the epoch, its rungs actuate through the
  existing `request_audio_reset()` path, and every walk lands in the Phase 1
  record. Gated by a build flag defaulted OFF, matching how the latency
  governor shipped.
- seams: the epoch checkpoint that owns `tx_gate_service()`, `request_audio_reset()`
  (`firmware/src/optical_main.cpp:872`), the status serializer.
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
seam and exhausted.** Phases 1 to 3 are host-only. Phase 4 is the first that
touches the board, and signs for the inactive slot per
[`../../CLAUDE.md`](../../CLAUDE.md) ("Sign an OTA container for an A/B board
with `--slot`, always").

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
