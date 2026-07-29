---
title: Aius sample-rate following
type: plans
scope: component
component: aius
status: in-progress
subtype: implementation
tags: [aius, firmware, teensy, audio, clocking, spdif]
updated: 2026-07-29
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
- Require eight consecutive locked samples with the same nominal result.
  At the current approximately 115 epoch/s this is about 70 ms.
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

The tracker returns decisions; it never writes a register. Golden tests cover
lock flap, unsupported input, all three rates, the ±2% boundary, integer wrap,
one-second rate limiting, millihertz averaging and trim hysteresis.
The in-lock 48→44.1 and supported→unsupported goldens prove readiness falls on
sample 1 and cannot return before sample 8 plus successful plan apply/reset and
new stream-rate agreement.

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
status: pending
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
  green: [clock, clock-target, rate-runner, pipeline, a52, dsp, optical-cutover, memory, itcm-placement, registry]
  buildable: [teensy41-optical]
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
3. **32 kHz PCM, at least 120 seconds.** Same gates, using the external optical
   PCM source because this CM6206 USB profile exposes only 44.1/48 kHz.
4. **Rate transitions.** Exercise 48→44.1→32→48 and carrier loss→48 kHz replay.
   Each accepted transition emits one event, increments one rate change, resets
   classification once, crosses no old-rate output and returns to continuous
   progress without a reset.
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
- [Aius AC-3 support](aius-ac3-support.md)
- [Aius OTA firmware upgrade](aius-ota-upgrade.md)
- [Aius hardware specification](aius-hardware-spec.md)
