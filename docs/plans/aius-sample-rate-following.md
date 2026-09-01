---
title: Aius sample-rate following
type: plans
scope: component
component: aius
status: in-progress
subtype: implementation
tags: [aius, firmware, teensy, audio, clocking, spdif]
updated: 2026-09-01
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
status: device-acceptance-pending
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
are complete. Phase status remains `device-acceptance-pending` until the exact
committed production image clears the in-scope device gates in
[§Acceptance](#acceptance) (gate 3 and gate 4's 32 kHz leg deferred
2026-07-31 to the 32 kHz long tail by owner decision).

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
   `trim_updates`/`invalid_windows`; after the first 30 seconds, no more than
   two further trim updates may occur during the run.
2. **44.1 kHz PCM, at least 120 seconds.** Require accepted nominal 44,100,
   output target following the measured carrier, continuous output, correct
   channel-status rate, bounded ring levels and zero harmful deltas.
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
   trim. Require carrier lock and capture-frame cadence to remain continuous,
   no sample discontinuity outside the declared invalid interval, and any
   affected run to be bounded by the exact 512-frame hold. Correlate the capture with
   `trim_updates` and `invalid_windows`; do not infer mute behavior from
   `VALCTRL` alone.

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
that A/B. Evidence retained in session scratch (two 594-sample runs).

If the 48 kHz slope exceeds 0.25 words/s, Phase 2 is not complete. The next
mechanism is a slow correction term driven by ingress slope into the same
numerator actuator; the acceptance bound does not weaken.

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
  this from production evidence.
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
