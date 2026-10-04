---
title: Aius sample-rate following
type: plans
scope: component
component: aius
status: in-progress
subtype: implementation
tags: [aius, firmware, teensy, audio, clocking, spdif]
updated: 2026-10-04
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

### Phase 3 Fill-referenced rate trim

```yaml
phase: 3
title: Close the output-rate loop on transmit-ring fill
status: planned
outcome: >-
  A live source offset inside the 500 mHz target deadband no longer drains
  (or fills) the transmit ring: the output rate converges on the source's
  true rate, so a steady-state session shows no periodic 128-frame underrun.
unit: component-behind-interface
seams:
  - AiusRateTracker (gains a ring-fill observation; owns the composed target)
  - transmit-ring floor sample at the governor tap (optical_main.cpp)
  - same-nominal NUM trim request (aius_audio_clock_request_trim)
  - GET /api/status clock (fill-trim observability)
targeted-proof:
  - host: clock suite drift-loop cases (below), zero underruns, bounded floor
  - target: clock-target, tx-gate, itcm-placement, registry green
  - device: one listening-room AC-3 session polled at 1 Hz, underrun delta 0
contract-artifact: spec-only
fan-out:
  - name: fill-loop-core
    owned-paths:
      - firmware/lib/aius_clock/**
      - firmware/test/clock/**
review-intensity: active
commit-checkpoint:
  green: [clock, clock-target, tx-gate, rate-runner, itcm-placement, registry]
  buildable: [teensy41-optical, teensy41-optical-slot-a]
  parent-commit: "feat(aius): trim the output rate on transmit-ring fill"
```

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

**Mechanism.** The ring level is the integral of the rate error, so steering
on it removes whatever bias the carrier estimate carries.

- **Floor estimator.** The same pre-commit level sample the governor reads
  (`firmware/src/optical_main.cpp:2059`), reduced to a window minimum over the
  governor's 192,000-frame window, phase-immune to the 1,536-frame AC-3 burst
  sawtooth. Unlike the governor, an underrun inside a window reads as floor 0
  (the strongest low-fill evidence), not a void.
- **Control law.** The composed target is the tracker's centre (the applied
  carrier-estimate target, unchanged) plus a fill offset. The offset is
  proportional to `floor - setpoint`. A higher floor means the output is
  falling behind the source, so the output speeds up; a lower floor slows it
  down. The setpoint is 256 frames, midway between the 128-frame gate-open
  target and the governor's 384-frame trim threshold. Gain: 500 mHz (about
  10 ppm) per 128 frames of floor error, so a worst-case in-band offset of
  500 mHz parks the floor within ±128 frames of the setpoint, never reaching
  the underrun or governor thresholds. Loop time constant about 260 s
  (128 frames / 0.5 frames/s), so the correction tracks oscillator drift, not
  programme content.
- **Bounds.** Offset clamped to ±600 mHz (the deadband plus margin; a larger
  error is the centre's job). The offset is applied through the existing
  same-nominal NUM trim, which has been carrier-continuous with no validity
  window since the 2026-09-07 correction (§Correction 2026-09-07), so the
  deadband's original cost argument does not apply to it. Requests are
  emitted at most once per window, and only when the offset moves by at least
  25 mHz (about 0.5 ppm), to bound NUM writes and events.
- **Scope.** Live input only, gate open. A closed gate, a carrier loss, a
  nominal change or a replay (48 kHz, output is graph master) resets the
  offset to 0 and the estimator to empty. A centre retarget keeps the offset;
  it is relative to the centre.
- **Cleanup.** The `AIUS_CLOCK_TARGET_HYSTERESIS_MILLIHZ` comment
  (`firmware/lib/aius_clock/src/aius_clock.c:18-34`) loses its drift-bound
  claim and points here.

**Observability.** `clock` gains `fill_floor_frames` (last window floor),
`fill_offset_millihz` (signed, current), `fill_trims` and
`fill_trim_last_at_ms`. Fill trims are counted apart from `trim_updates`, so
gate 1's trim-cadence metric keeps its meaning. The swagger drift gate takes
the new keys. No new event kind: `fill_trim_last_at_ms` is the stamp, so the
ring's 2026-10-03 overflow pressure is not added to.

**Host proof (clock suite).** A ring model driven by the tracker over two
simulated hours, with the carrier estimate quantised to 140 mHz bins and
1.2 Hz settled spread:

- true offsets of -10, -5, 0, +5 and +10 ppm: zero underruns after
  settle, floor within [128, 384], and the governor never fires;
- a 2,048-frame stall backlog: the governor trims, and the fill loop neither
  fights it nor overshoots into an underrun;
- an underrun mid-window: reads as floor 0, so the offset moves to slow the
  output;
- gate close, carrier loss, nominal change and replay each zero the offset;
- NUM-write count bounded (offset quantum and once-per-window limit).

**Device proof.** One listening-room AC-3 session of 45 min or more polled with
`firmware/tools/device/aius_status_poll.py`: `underrun_episode_count` delta 0
after the first window, `fill_offset_millihz` settling near the source's true
offset, and `governor_trims` delta 0. Gate 1's rebased criterion must stay
green on the bench: the loop moves the output from the carrier estimate onto
the true rate, so `max_follow_error_millihz` (≤ 1,000) should read about the
estimate's own spread, and the run confirms it.

The prebuild review attacks:

- the loop sign inverted, so it accelerates the drain;
- an underrun voiding the window as the governor does, hiding the fault the
  loop exists for;
- the fill loop and the governor fighting across the 384-frame threshold;
- the offset surviving a nominal change, a replay or a carrier loss;
- a NUM write per producer block instead of per window;
- the clamp letting a stall backlog drive the output far off the carrier;
- signed millihertz overflow or rounding in the composed target; and
- the new code leaking into ITCM.

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
(`firmware/tools/net/aius_rate_bench.py:852`) as an advisory metric — kept
docs-only per the D4 (b) framing; a later host-tool change could make it
advisory in code.

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
