---
title: Aius sample-rate following
type: plans
scope: component
component: aius
status: in-progress
subtype: implementation
tags: [aius, firmware, teensy, audio, clocking, spdif]
updated: 2026-10-06
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

## Current state

Updated 2026-10-06 during an orchestration run; replace wholesale on the
next handover.

- **Orchestration posture: autonomous** (owner, 2026-10-06, second run):
  resolve every call on its recommendation, record it with its why in the
  phase it settles, and collect calls wanting confirmation in the closing
  `Interrogate the user: confirm autonomous calls` phase. Device sessions
  and anything destructive still go to the owner. The Phase 4 device
  session runs in the listening-room, and the owner waives the
  audio-interruption cost statements for that session.
- **Status.** Phases 1, 2 and 3 DONE. Phase 4 BUILT; Phase 4a SHIPPED
  (2026-10-06) on host proof, with delivery review, closure and clean-room
  all passing. Deployed 2026-10-06: `df966f17b370` by OTA (container
  sha256 `bb3e090a8e8b…`), slot 1, healthy-boot self-commit,
  `rollback_observed` false; `clock.centre_writes` and
  `clock.gate_open_pending_padded_frames` present.
- **Next action.** Repeat the Phase 4 device session in full (the
  2026-10-06 run passed every criterion but ran 33.7 min polled and 0
  min unpolled; see Phase 4 §Device evidence 2026-10-06), then gate 7 at
  fill-write cadence (4a's moved bench proof); that needs the owner.
  Phase 4c shipped. Phase 4d (B1)
  waits for `CLAUDE.md`'s foreign edits to be committed
  (re-check the diff, never from memory).
- **Hook item moved.** `stop-guard.action-reverify` is item 11 of
  `src/tools/hook/docs/plans/hook-daemon-feature-backlog.md` (owner:
  plan, do not dispatch).
- **What landed this session** (aius commits; upstream pointer bumps
  follow each):

| Commit | Contents | Proof |
| --- | --- | --- |
| `443a0ae` | Phase 3 fill sensor, status keys, `optical-cutover` refresh, capture reducer, register entry for the replay trim | tx-fill-sensor, tx-gate, tx-governor, clock-target, registry, sd-editor, optical-cutover, itcm-placement green; adversarial and clean-room reviews PASS |
| `9a145b7`, `0f03beb` | deploy records (`443a0ae5cb26`, then `0af7a8244081`) | healthy-boot self-commit, `rollback_observed` false |
| `0af7a82` | output-jitter Phase J (governor replay target), `status_render_failures` | tx-governor and the green list above; ITCM unchanged |
| `a10e2ff` | Phase J shipped; output-jitter Phases K and L planned; `measure_run.py --auth-file`, `replay` | three 10 s device replays, `governor_trims` 0 |
| `c4c6200` | Phase 3 shipped on the capture; reducer underrun fixes; register update | reducer `--self-test` ok; capture reduced end to end |

- **Uncommitted tree.** `CLAUDE.md` is dirty and foreign;
  leave it.
- **Capture data** (scratch, reaped about 2026-10-12, so the design pass
  must read it before then):
  `.scratch/2682a96a-3760-4d00-b9e0-f71f66b336ba/capture-polled.jsonl`,
  `capture-unpolled.jsonl`, `capture-pcm-topup.jsonl`, the reducer reports
  `fill-analysis-final.*`, and the recorder pulls in `recorder-2026-10-05/`.
  Everything durable is in Phase 3 §Measured.
- **Editor diagnostics.** clangd reports missing `Arduino.h` and the
  library headers on `firmware/src/` and `firmware/test/` files: an include
  path gap in the editor only; `firmware/scripts/test.sh` and the PlatformIO
  builds are the truth.
- **Evidence outside the repo** (7-day scratch GC):
  `.scratch/a2a9bb23-23dd-4b7a-beb4-02ddc24fabff/session-2026-10-04.jsonl`
  (1 Hz status, 20:42 to 21:32 UTC) and `recorder-underrun-2105.wav`,
  `recorder-underrun-2114.wav`, `recorder-underrun.wav` (21:22). The numbers
  that matter are already in the register entry and Phase 3's defect text.
- **Traps.**
  - The host clock is UTC; the owner speaks UK local time (BST in
    October). Map board `millis()` stamps to wall time from one bracketed
    `/api/status` read (`dark_open_start_ms + dark_open_ms` is "now" while
    the carrier is down).
  - Switching the source off freezes zero-frame `carrier_loss` captures that
    evict real ones. Pull with `aius-recorder-pull.py --auth-file
    .secrets/aius-sd-auth` before switch-off; `--slot 99` writes the bank
    header (`--header-json`) and exits without pulling audio.
  - The SD token never goes on a command line, nor through `$(cat …)` (the
    bash guard denies command substitution); use the tools' `--auth-file`.
  - A streaming follow (`tail -f`) is hook-denied; a Monitor over the
    poller's change log uses a `wc -l` polling loop.
  - `clock.trim_updates` 0 through a whole session is expected inside the
    500 mHz deadband, not a dead tracker; that is the defect Phase 4 fixes.
  - The recorder taps the stream before the TX ring, so a capture does not
    contain a TX underrun's gap, only the programme around it.
  - Phase 4's commit edits `CLAUDE.md` (the fill stamps),
    a protected file: write it via `the release-promotion step`.
  - `aius_ota_sign.py sign` also needs `--image`, `--output` and
    `--build-revision` (the 12-char HEAD, at most 32 chars, e.g.
    `443a0ae5cb26`); `CLAUDE.md` shows only `--secret`.
    The fix is planned in Phase 4 (CLAUDE.md promote).
  - Polling `/api/status` is not neutral: a render delays the AC-3
    producer and can trigger the underrun being measured. Note each poll
    instant against `underrun_episodes[].start_ms` when reading a capture.
  - A fill window that contains an underrun blends pre- and post-step
    minima (+8.5 instead of +127). Read the step across the next window.
  - An AV-chain wake or CEC hypothesis does not apply: the owner confirms
    no HDMI-CEC link and no optical carrier when idle. False replay carrier
    stops are on-board (output-jitter Phase K).

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

### Phase 3 Phase-corrected fill sensor and noise capture ✓ SHIPPED 2026-10-05

```yaml
phase: 3
title: Phase-corrected transmit-ring fill statistic, observed only
status: shipped # 2026-10-05: deployed 443a0ae then 0af7a82; capture recorded in §Measured below
prebuild-review: ready # clean-room 2026-10-05: text-only findings folded (DMA-position sensor, statistic spec, status limit, device criteria)
outcome: >-
  GET /api/status serves a transmit-ring fill statistic with about one-frame
  resolution that reads the same floor for PCM and AC-3 (DTS by host proof
  only), and one listening-room session has measured its window-to-window noise
  and drift, the inputs Phase 4 derives its gains from. No audio behaviour
  changes and no ISR change.
unit: component-behind-interface
seams:
  - TX DMA read position (AiusSpdifTx24Hook accessor, aius_spdif_sources.cpp)
  - fill statistic at the governor tap (optical_main.cpp)
  - GET /api/status clock and firmware/sd/www/openapi.json
targeted-proof:
  - host: tx-fill-sensor suite (cases below)
  - target: tx-gate, tx-governor, clock-target, itcm-placement, registry, sd-editor green
  - device: one listening-room session of 45 min or more spanning a PCM to AC-3 change, polled and unpolled segments, criteria below
contract-artifact: spec-only
fan-out:
  - name: fill-sensor
    owned-paths:
      - firmware/lib/aius_audio/src/aius_tx_fill_sensor.h
      - firmware/test/tx-fill-sensor/**
review-intensity: active
commit-checkpoint:
  green: [tx-fill-sensor, tx-gate, tx-governor, clock-target, itcm-placement, registry, sd-editor, optical-cutover]
  buildable: [teensy41-optical, teensy41-optical-slot-a]
  parent-commit: "feat(aius): phase-corrected transmit-ring fill statistic"
```

**Owner decision (2026-10-05): build now, deploy over OTA when the board is
idle** (no carrier, nothing playing; check `/api/status` `carrier` 0 first),
then the capture session at the owner's next viewing.

**Remaining steps, in order:**

1. ~~OTA the committed image when the board is idle.~~ Done 2026-10-05
   08:19 UTC: `443a0ae5cb26+teensy41-optical-slot-a`, slot 2 to 1, `ab.sequence`
   13 after the healthy-boot self-commit, `rollback_observed` false. Idle
   first read: fill keys present, `fill_level_q4` null and `fill_windows` 0
   (gate closed, as designed), `status_body_peak_bytes` 8,592 with no carrier.
2. ~~Run the capture session and reduce it.~~ Done 2026-10-05 on
   `0af7a8244081`: 20:20:45 to 21:07:21 UTC (35 min at 1 Hz, 11.6 min at
   30 s) plus an 8 min 1 Hz PCM top-up (21:25:04 to 21:33:03).
3. ~~Record the inputs and ship.~~ See §Measured below.

**Delivery decisions (2026-10-05).**

- **Governor replay target: real.** Code read confirms the governor trims a
  clean replay from its 768-frame floor toward 128 about 4 s in. Filed in the
  [register](aius-open-work-register.md#open-defects--runtime), fix planned as
  [`aius-output-jitter.md`](aius-output-jitter.md) Phase J. Not fixed here.
- **Invalid window serves `null`** (owner): a window that closes with fewer
  than 8 valid sub-windows clears `fill_level_q4` until the next valid window,
  so a 1 Hz reader never records a stale value. "The last valid window
  statistic" below means the latest window, when valid.
- **Status size** (owner): top-level `status_body_peak_bytes` serves the
  largest body of the previous renders this boot (a body cannot state its
  own length, so the first render reads 0); the build-time bound moved to
  Phase 4. The fill keys add at most 162 B and the peak key about 35 B, to a
  live body of 10,488 B (older image).
- **Stall unit per class** follows the IEC 61937 repetition period from
  `pipeline_diag.codec_pc`: PCM the block, AC-3 1,536, E-AC-3 6,144, DTS
  512, 1,024 or 2,048 by Pc. The mapping lives in the pure header so the host
  suite exercises the firmware's own table. Known limits: E-AC-3 and
  unknown-compressed units make the stall test lenient (up to 138 ms) for
  sources sending shorter units. The tap's choice of class (it reads
  `codec_pc`, which can be stale from an earlier session at AC-3 onset) and the
  held-block skip of the post-commit call are untested on the host. A wrong
  exclusion shows in the capture's per-class exclusion rate; check it there.
- **Gate close resets the sensor** at `tx_gate_actuate(false)` as well as at
  the next held block, because a closed gate may never deliver one.
- **A governor trim empties the window but keeps the fault latches**, so a
  fault between the trim block and the next still excludes.
- **The plan-apply ring flush needs no sensor reset of its own.**
  `clock_flush_ring_preserve_diag()` (one caller, `clock_apply_desired_plan`)
  always runs after `close_stream_rate_gate()`, whose gate close resets the
  sensor. Sampling resumes only at the next gate open.
- **The reducer classes by codec**, not just PCM versus compressed: `ac3`,
  `eac3`, `dts` from `codec_pc`, `compressed-unknown` before the first burst.
- **`optical-cutover` refreshed:** its clock key set and event table were
  stale before this phase (25 keys, events 14 to 18); now green and in this
  phase's green list.

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

**Prebuild review, round 1 (2026-10-04): HOLD.** The first design (window
minimum of the pre-commit level, proportional 500 mHz per 128 frames, 256-frame
setpoint) was refuted on four blocking counts, all verified against the code:

- the ring level only moves in 128-frame TX periods
  (`firmware/lib/aius_audio/src/aius_spdif_tx24.h:293-320`), so a
  proportional law at that gain is a ±500 mHz relay that limit-cycles and
  never converges;
- it had no authority past ±500 mHz, where the governor (reading 512 > 384)
  or an underrun takes over;
- the tracker's `applied_target_millihz` and cadence timer are refreshed from
  the applied PLL target (`firmware/src/optical_main.cpp:8556-8564`,
  `firmware/lib/aius_clock/src/aius_clock.c:344-345`), so a composed target
  would trip the 500 mHz hysteresis every 10 s and starve centre retargets;
- the -500 mHz opening transient from a 128-frame gate-open floor breaches
  gate 1's 1,000 mHz follow-error bound.

The should-fix findings (event flood through the `target_update` branch,
unhandled trim refusals, a reset leaving the offset on the wire, replay
emission dropping `g_clock_replay_ready`, a composed target outside the plan
band faulting, a host proof blind to all of these, an incomplete green list,
the 256 setpoint's +2.67 ms latency) are folded in below.

**Prebuild review, round 2 (2026-10-04): HOLD on one count.** The round-1
revision used a window MEAN of phase-corrected samples against a setpoint
learned at gate open. The mean depends on decoder class and scheduling: an
AC-3 access unit yields six 256-frame blocks, one per service call
(`firmware/lib/aius_pipeline/src/aius_pipeline.c:725-726`), committed in quick
succession (the capture confirms how quick), so its samples
read F, F+256, ..., F+1280 (mean about F+640), while PCM reads about F. A
PCM-to-AC-3 change inside one lock does not close the gate, so a PCM-learned
setpoint would drive the output to the clamp and pull the real floor down by
about 640 frames. The governor header rejects a mean for the same reason
(`firmware/lib/aius_audio/src/aius_tx_governor.h:24-28`). Resolved below with
a phase-corrected minimum and a constant setpoint. Round 2 also showed the PI
targets depend on the statistic's noise, which no one has measured; hence
this phase measures before Phase 4 actuates.

**Prebuild review, round 3 and clean-room (2026-10-04 and 05).** Round 3 found
no blocking issue; its should-fix items (per-relock occupancy spread,
class-aware stall test, empty-ring reading, trough-count bias, the ISR's
non-TRIM-is-HOLD branch) are folded below and into Phase 4. The clean-room
confirmation held Phase 3 on text only: an ISR stamp needs a period
measurement that cannot be built as written, the status limit was the wrong
constant, and the statistic was underspecified in six places. It held Phase 4
on two counts: a constant setpoint against a random gate-open occupancy pins
the output at the clamp after every relock, and "commit on request success"
was ambiguous between controller and emission state. All are resolved below.

**Sensor.** The ring level is the integral of the rate error. Plant: a rate
error of `u` mHz moves the ring by `u / 1000` frames/s.

- **Played fraction from the DMA position, no ISR change.** The TX DMA's
  current source address (`dma.TCD->SADDR`) says exactly how far it has read
  into the half it is playing; the ISR already uses it to pick the half to
  fill (`aius_spdif_sources.cpp:234-239`). A static accessor on the existing
  access shim (`AiusSpdifTx24Hook`, `:209-214`) returns that offset; `phi` is
  the offset within the current half over the half's 128 frames. No stamp,
  no period measurement, no ITCM bytes. The accessor is `FLASHMEM` (it runs
  per producer block from the flash-resident `enqueue`); the map file is
  checked so neither it nor the reduction is emitted into `optical_main`'s
  default `.text` (ITCM).
- **Read.** At the governor tap (`firmware/src/optical_main.cpp:2059`) read
  `ring->fills`, the level, the DMA offset, then `ring->fills` again; retry if
  it moved, three attempts in all. A sample whose three attempts all tear is
  dropped and counted (`fill_torn_reads`). The one tear this cannot see (DMA
  already in the next half, ISR not yet run) reads 128 high, which a
  minimum ignores.
- **Correction.** `fill()` hands each DMA half one period ahead
  (`aius_spdif_sources.cpp:230-269`), so the unsent frames are the ring level,
  128 in the next half and `(1 - phi) * 128` still being read: occupancy is
  `level + 256 - phi * 128`. `fill()` pads an underrun with silence and
  advances `consumed` only by what it took (`aius_spdif_tx24.h:296-320`), so
  an empty ring reads `256 - phi * 128` (128 to 256); the arithmetic is
  signed as a defence only. Measured 2026-10-05: healthy AC-3 troughs read
  134 to 174, inside that band, so an empty ring at a pre-commit sample is
  normal for AC-3 and not a fault. The padded silence was played from no
  input, so once the producer resumes the occupancy stands **128 higher and
  stays there**: each underrun is a +128-frame step, not a low reading.
- **Statistic.** Its own 192,000-frame window of committed frames, with its
  own boundaries (not the governor's, which voids a whole window on an
  underrun, `aius_tx_governor.h:73-75`), split into 16 sub-windows of 12,000
  frames. Each sub-window's value is the minimum occupancy over its pre-commit
  samples. The window statistic is the mean of the non-excluded sub-window
  minima, exact in 1/16 frame; a window with fewer than 8 valid sub-windows
  is invalid (served as such, and Phase 4 treats it per its all-excluded
  rule). A minimum reads the floor for every class; averaging minima recovers
  resolution.
- **Exclusion** (per sub-window, counted): an underrun, a missed fill, or a
  production stall. The stall test: a gap between accepted blocks longer than
  the duration of the last accepted block's frames at the stream rate (PCM's
  512-frame block, an AC-3 burst's 1,536, the pipeline's unit for E-AC-3 and
  DTS) plus a fixed 10 ms allowance. Not the 40 ms `AIUS_INPUT_GAP_ALERT_US`
  alert (`firmware/src/optical_main.cpp:1539-1540`), which normal DTS-2048
  and long-burst E-AC-3 cadence exceeds every unit and AC-3 (32 ms plus
  service lag) exceeds intermittently.
- **Reset** (window emptied, statistic invalid until the next full window):
  gate close, governor trim (the level jumps, so it is a reset, not an
  exclusion), carrier loss and any replay block.
- **Bias to measure.** A fixed frame count holds different numbers of troughs
  per class (about 23 for PCM, 7.8 for AC-3, 2 to 8 for E-AC-3), and the
  minimum of fewer troughs sits slightly higher; the capture measures it.
- **Placement.** The reduction is a pure header (`aius_tx_fill_sensor.h`, in
  the governor's style), `FLASHMEM` where compiled into a cold object.
- **Observability.** `clock` gains `fill_level_q4` (signed, the last valid
  window statistic in 1/16 frame; `null` before the first valid window and
  after a reset), `fill_subwindow_spread_frames` (max minus min of its
  minima), `fill_windows`, `fill_subwindows_excluded` and `fill_torn_reads`.
  The keys land in `firmware/sd/www/openapi.json` (the `sd-editor` drift
  gate). The live status is already about 10.5 KB, past the 8,192-byte
  `AIUS_HTTP_MAX_RESPONSE` and streamed; the binding limit is
  `AIUS_RUNTIME_TEENSY_APP_BODY`, 12,288 bytes
  (`firmware/src/aius_runtime_teensy.h:48`), about 1.8 KB of headroom for
  Phases 3 and 4 together. Phase 3 serves the boot's peak body size; the
  build-time check is Phase 4's (see Delivery decisions).
- **Tooling.** The capture's derived numbers come from a committed analysis
  script over the poller's JSONL (`firmware/tools/device/`, parent-owned), not
  ad-hoc queries. The parent also owns registering the `tx-fill-sensor` suite
  (`firmware/scripts/test.sh` header list and case arm, the README suites
  table), which the `registry` suite checks.

**Host proof (tx-fill-sensor suite).** A torn read retries, and three tears
drop and count the sample; the DMA offset maps to `phi` at both half
boundaries and mid-half; an empty ring reads `256 - phi * 128`, never wrapped;
PCM 512-frame commits and AC-3 6 × 256 bursts at the same true floor, with
trough-timing jitter injected, give statistics whose difference matches the
trough-count bias; normal AC-3, DTS-2048 and long-burst E-AC-3 cadence
excludes nothing, a real stall is excluded; fewer than 8 valid sub-windows
make the window invalid; every reset path empties the window and serves
`null`.

**Device proof and noise capture.** One listening-room session of 45 min or more
that spans a PCM-to-AC-3 change (menus then programme) and several gate opens
(relocks), polled at 1 Hz with `firmware/tools/device/aius_status_poll.py`,
plus an unpolled segment of 10 min or more (`--interval 30`), because the
~10.5 KB status serialisation lengthens epochs and may bias the trough
samples. Pass criteria:

- **Resolution is real:** across each 128-frame step of the `tx_level_frames`
  floor (the slow precession; on 2026-10-04 it stepped at 20:53 and 21:04 UTC),
  `fill_level_q4` stays continuous within a few frames, and its slope matches
  the underrun cadence (about 0.23 frames/s on this source). A `phi`
  inversion or half-period error shows here as a 128-frame sawtooth.
- **No audio change:** `underrun_episode_count`, `governor_trims` and every
  harmful counter behave as on the previous image.

Recorded here, as Phase 4's inputs (by the committed analysis script):

- the steady `fill_level_q4` after each gate open, and their spread: the
  gate-open trim sets the level at an instant unrelated to the TX DMA, so
  occupancy opens anywhere in 256 to 384 and differs per relock
  (`firmware/src/optical_main.cpp:2524-2536`);
- its window-to-window standard deviation in steady state (de-duplicated by
  `fill_windows`), polled and unpolled, and the sub-window spread (the
  trough-timing jitter);
- the PCM and AC-3 floors and their difference, and the shape of the
  PCM-to-AC-3 transition (Phase 4's host model of that switch is built from
  it);
- the exclusion and torn-read rates per class.

**Measured, 2026-10-05** (`0af7a8244081`; reduced by
`firmware/tools/device/aius_fill_capture_analysis.py`; raw JSONL and reports
in `.scratch/2682a96a-3760-4d00-b9e0-f71f66b336ba/`, 7-day GC; owner-accepted
plan: main session plus a PCM top-up).

- **Resolution: pass.** Floor steps clear of an underrun read continuous
  within window noise: 20:37 −2.2 frames, 20:32 +4.06 (the reducer's 4-frame
  tolerance is tighter than the AC-3 σ of 4.3). Steps that straddle an
  underrun carry the real +128 step and get no verdict. Every episode-free
  drain slope matches the underrun cadence. No 128-frame sawtooth.
- **No audio change: pass.** `governor_trims`, `tx_missed_fills`,
  `clock_faults`, CRC rejects, overruns and torn reads all 0. Five 128-frame
  underruns (20:47:16, 20:58:51, 21:04:51, one between 21:07 and 21:25
  outside the captures, and 21:25:16) are the pre-existing drain defect this
  plan fixes; the first three were pulled from the recorder.
- **Underrun step:** +126.9 and +126.8 frames at window resolution (the
  window containing the episode blends to +8.5; ignore it). 30 s samples
  read +119 and +122 (the step less 30 s of drain).
- **Status polls trigger the underruns** (measured): every episode starts 29
  ms after the producer's last commit before a status render
  (`input_diag.dark_open_start_ms`). The ~10 KB render delays the next AC-3
  burst; once the drift has thinned the margin, a poll empties the ring.
  Polling therefore brings each underrun forward and is not neutral.
- **Drain:** AC-3 programme −0.157 to −0.275 frames/s between underruns
  (mean −0.186), steepening; output 48,001,902 against carrier 48,001,623
  mHz at the end (279 mHz, 5.8 ppm), matching the late slope. The plant
  model (u mHz moves u/1000 frames/s) holds to about 10 percent. The PCM
  top-up's reported +0.38 frames/s rise is a reducer artefact (Phase 4
  entry-gate design pass, 2026-10-06): `episode_free_slope`
  (`firmware/tools/device/aius_fill_capture_analysis.py:344-366`) does not
  split a span at a gate close, so its fit spanned the 21:27:59 relock's
  +146-frame re-base. Per segment the source is 48,001,666 mHz (AC-3, before,
  −0.236 frames/s) and 48,001,656 mHz (PCM, after, +0.033 frames/s): one
  clock. The relock's dark full apply re-centred the output on the estimate
  (48,001,902 to 48,001,623, `trim_updates` 0), about 33 mHz below the
  source. Earlier the source warmed from about 48,001,866 (20:31) to
  48,001,650 (21:00), steepest about −14 mHz/min.
- **Steady fill after a gate open:** 292.6 (AC-3, 20:27:25), 306 to 310
  (PCM), 328 to 339 (AC-3), 379 to 382 (PCM) frames over four real relocks:
  range 90 frames, inside the predicted 256 to 384. The reducer's
  "21:25:04 ac3 244.62 gate open" is the top-up file's start
  (`gate_open_padded_frames` unchanged), not a relock.
- **Noise (lag-1 window-to-window σ, polled):** AC-3 1.17 frames (412
  pairs, P plus top-up, windows around an underrun excluded; the reducer's
  4.33 included two underrun-step pairs), PCM 1.42 (61 pairs). 16 of 412
  clean AC-3 pairs exceed 4 frames, the largest 7.06 (one render-dip
  window). Sub-window spread median and p95: AC-3 7 and
  47, PCM 13.5 and 54 frames. Unpolled AC-3 (lag 7 to 8): white-model σ
  0.88, random-walk-model σ 0.29.
- **Exclusions and tears:** AC-3 4 of 625 windows (0.0016 per s), PCM 0 of
  91; torn reads 0.
- **Class floors and difference:** every PCM and AC-3 change on this source
  closed the gate and relocked, so the gate-open trim reset the fill at each
  switch; no in-lock class switch occurred and no drift-free class
  difference is derivable. Cross-time medians (PCM 384.8, AC-3 255.8) are
  drift-confounded and not a class bias.

### Phase 4 Fill-referenced rate trim

```yaml
phase: 4
title: Close the output-rate loop on transmit-ring fill
status: built # 2026-10-06: five slices landed, verifier green (20 suites, both images); delivery audit FAIL on B1 only, fixes in Phase 4a; device session after 4a
depends-on: phase 3 noise capture (setpoint spread across gate opens, PCM-versus-AC-3 floor bias)
prebuild-review: ready # 2026-10-06: rounds 4 to 6 NOT READY and folded; round 7 READY; clean-room NOT READY (1 blocker) folded, closure READY with N1 to N3 folded. Delivery audit adds: the parent's arm counter and unit_frames input, and the apply-started note flag (host-untested)
outcome: >-
  A live source offset inside the 500 mHz target deadband no longer drains
  (or fills) the transmit ring: the output rate converges on the source's
  true rate, so a steady-state session shows no periodic 128-frame underrun,
  at unchanged mean standing latency (per-session setpoint, mean 320 frames).
unit: component-behind-interface
seams:
  - AiusRateTracker (centre and fill integral as separate state; returns one composed request; told the centre only)
  - clock_refresh_applied -> aius_rate_tracker_note_target_applied (optical_main.cpp:8751-8759; a FILL readback never notes a target)
  - same-nominal NUM trim request with a stamp-kind argument (aius_audio_clock_request_trim)
  - fill sensor closed-window sequence number and exclusion-reason bits (aius_tx_fill_sensor.h)
  - TX gate open reads the opening occupancy in the masked section and opens only at a burst start (tx_gate_execute_open, optical_main.cpp)
  - GET /api/status clock and firmware/sd/www/openapi.json
targeted-proof:
  - host: clock suite plant model (cases below)
  - target: clock-target, tx-gate, tx-governor, tx-fill-sensor, itcm-placement, registry, sd-editor green; ISR ITCM bytes recorded
  - device: one listening-room AC-3 session with criteria below; bench gates 1 and 7 and the stable-phase transition gates
contract-artifact: spec-only
fan-out: # host-only slices against §Slice interfaces: fill-sensor-window, clock-kind, unit-start concurrently; fill-loop-core once fill-sensor-window's pure opening functions land; parent-wiring last; the parent runs every target build
  - name: fill-loop-core
    owned-paths:
      - firmware/lib/aius_clock/**
      - firmware/test/clock/**
  - name: clock-kind
    owned-paths:
      - firmware/lib/aius_audio/include/aius_audio_clock.h
      - firmware/lib/aius_audio/src/aius_audio_clock.cpp
      - firmware/lib/aius_audio/src/aius_audio_clock_internal.h
      - firmware/lib/aius_audio/src/aius_spdif_sources.cpp
      - firmware/lib/aius_audio/src/aius_spdif_tx24.h
      - firmware/test/clock-target/**
      - firmware/test/itcm-placement/verify_target.py
  - name: fill-sensor-window
    owned-paths:
      - firmware/lib/aius_audio/src/aius_tx_fill_sensor.h
      - firmware/test/tx-fill-sensor/**
  - name: unit-start
    owned-paths:
      - firmware/lib/aius_codec_api/include/aius_codec.h
      - firmware/lib/aius_pipeline/**
      - firmware/test/pipeline/**
      - firmware/test/codec-api/**
      - firmware/test/capability/**
      - firmware/test/dsp/test_dsp.c
  - name: parent-wiring # coordinator: gate open, sample_rate_service, status keys, openapi, test.sh, reducer fix
    owned-paths:
      - firmware/src/optical_main.cpp
      - firmware/sd/www/openapi.json
      - firmware/test/optical-cutover/**
      - firmware/test/status-body-bound/**
      - firmware/scripts/test.sh
      - firmware/tools/device/aius_fill_capture_analysis.py
review-intensity: active
commit-checkpoint:
  green: [clock, clock-target, tx-gate, tx-governor, tx-fill-sensor, rate-runner, itcm-placement, registry, sd-editor, optical-cutover, status-body-bound, memory, pipeline, codec-api-contract, capability, dsp, a52, eac3, dts]
  buildable: [teensy41-optical, teensy41-optical-slot-a]
  parent-commit: "feat(aius): trim the output rate on transmit-ring fill"
```

**Mechanism.**

- **Entry gate: decided 2026-10-06 from the Phase 3 capture** (design pass
  `opus`; each decision put to the owner and accepted as recommended).
  Loop figures are closed form (continuous PI, 4 s window, plant u/1000
  frames/s); the host plant model confirms them before the device session.
  1. **Setpoint 320 frames** (6.67 ms at 48 kHz, the centre of today's 256
     to 384 gate-open band, so mean standing latency is unchanged). The
     underrun line is occupancy 128, so the earlier "256 plus 50" floor was
     conservative. Underruns struck at window statistics of 134 to 174; the
     deepest status-render dip below its window was 71 frames (sub-window
     spread p95: AC-3 47, PCM 54). 320, less that dip, a 700 mHz step's
     34.5-frame peak and 3σ of noise, sits 82 frames clear (46 if the 16 KiB
     status body deepens the dip 1.5 times). Alternatives 288 (less
     margin) and 352 (+0.67 ms latency) declined.
     **Amended 2026-10-06 (prebuild review round 4, owner): the setpoint is
     per session, mean 320.** Every fill and commit moves the ring in
     multiples of 128 frames, so the residue `r = level mod 128` is fixed
     for a session; a pad to a constant occupancy spreads `r` over 0 to 127,
     and the delay tolerance at a trough is `O - 128 - r`. At a constant 320
     that is 64 to 192 frames, short of the 109-frame budget (71 dip, 34.5
     step, 3σ) in about 35 percent of sessions; "82 clear" was the best
     phase. So the gate open keeps its level-128 pad (`r` = 0) and the
     setpoint is the occupancy measured at the open, `384 - phi_0 * 128`
     (256 to 384, mean 320, so mean standing latency is unchanged; it varies
     ±1.33 ms between sessions). Worst-phase tolerance is 128 frames, 19
     clear of the 109 budget; with the 16 KiB body's estimated 1.5 times
     dip (about 145) it falls short when `phi_0` > 0.87, about 13 percent
     of sessions, so Phase 4b, not the setpoint, is the real mitigation for
     a deeper dip (round 5 N8). After one underrun the trough level is 256,
     under the governor's 384 trigger (`firmware/lib/aius_audio/src/aius_tx_governor.h:84`), and a
     governor trim (which lands at level 128) lands on the setpoint, also in
     a session whose open saw a pending boundary (F3 pads that open to level
     128 after the late ISR, so its trough is 256 as well). The constant alternative (about 368 frames, about 1 ms more latency)
     declined.
  2. **Class bias: none.** All three class changes closed the gate and
     relocked, so the occupancy pad resets the floor. The budget allows
     3.8 frames (50 mHz over `Kp`); the estimated trough-count bias is 2 to
     3 frames (inferred, unmeasured). The host in-lock switch case injects a
     4-frame bias and a floor re-base and asserts no underrun. A per-class
     setpoint is the fallback if a source is seen switching in lock.
  3. **The PCM top-up's +0.38 frames/s was a reducer artefact** (Phase 3
     §Measured, Drain): one source clock, the relock's full apply re-centred
     on the estimate. No change to gains or authority; the warm-up ramp
     (about −14 mHz/min) leaves a steady tracking error of about 2.7 frames,
     and a 150 mHz post-relock residual with `I` zeroed peaks at about
     7 frames.
  4. **Gains: `Kp` 13 mHz per frame, `Ki` 345 µHz per frame per window**
     (damping 0.70). Statistic unchanged (mean of the 16 sub-window minima,
     4 s window at 48 kHz; 4.35 s at 44.1, 6 s at 32, so slower there).
     Noise: AC-3 1.17 frames, PCM 1.42, unpolled 0.88, 3 to 5 times under the
     4-frame bound. Modulation 15 to 18.5 mHz rms; a single render-dip window
     (up to 7 frames) kicks about 91 mHz for 4 s, accepted. A 700 mHz step
     peaks at 34.5 frames, is inside 16 frames by about 3.5 min and settled
     by 10.3 min. Fixed point: `P = 13 * e_q4 / 16` mHz,
     `I += 345 * e_q4 / 16` µHz, rounded. Kp 8 / Ki 130 µHz (56-frame peak,
     17 min) and Kp 20 to 26 (more modulation and writes) declined.
  5. **Emit threshold 20 mHz** (12 NUM LSBs). Noise moves the composed
     target 21 to 26 mHz rms per window, so writes are not rare: expect 35
     to 45 percent of windows polled (300 to 400 per hour), about 20 percent
     unpolled, at a cost of 1.5 frames of fill error. Hard bound: one FILL
     write per window (900 per hour at 48 kHz, 827 at 44.1, 600 at 32).
     Gate 7 tests carrier continuity at one write per 4 s.
  6. **An underrun is a +128-frame step, rejected by plain PI, not a reset**
     (+126.94 and +126.81 measured; one sub-window excluded each, no trim).
     The window holding it is used as is: it reads the step times the share
     of valid sub-windows after it (+8.50 is 1 of 15), a linear partial
     step. `Kp * 128` is 1,664 mHz, so the output rides the ±900 clamp with
     `I` held. Clipping the error at ±64 frames, and subtracting known
     underrun frames (latency then stays 128 high), declined. Round 4
     correction: the step is `128 - r`, which is 128 once `r` = 0 (always,
     under the item 1 amendment). Recovery depends on `I` (reviewer plant
     sim, +128 step): `I` 0 back within 16 frames in 144 s with 15 frames
     undershoot; `I` +670 mHz 491 s, about 4; `I` −670 mHz 99 s, 26. So
     the worst recovery is about 8 min, not 3 to 4. The undershoot is
     covered by the 128-frame worst-phase tolerance with the measured dip
     (26 + 71 + 3.5, about 100), not with a 1.5 times dip (about 137):
     Phase 4b again.
- **Status body size, checked at build.** Moved here from Phase 3 (owner,
  2026-10-05). No build or test today bounds `/api/status` against
  `AIUS_RUNTIME_TEENSY_APP_BODY` (12,288 B); the only `static_assert`s cover
  the meters and recorder documents. Phase 3 serves `status_body_peak_bytes`
  (the largest earlier body this boot) so the capture measures real headroom. This
  phase adds the worst-case bound with a `static_assert` (or a host render of
  the worst case) before it spends that headroom on its own keys, and adds its
  keys to the `optical-cutover` expected set.
  **Pre-capture slice, 2026-10-05.** `status_render_failures` (top level,
  counted in `app_write_status()`) shipped in the Phase J redeploy, so an
  overflow is visible after the next successful render. The bound is built
  but red: `firmware/test/status-body-bound/` (a host extractor that walks
  every function `http_status_json()` writes through and sums worst-case
  widths, refusing what it cannot bound) puts the worst case at 14,723 B
  against 12,288 B, mostly counters at full 10-digit width (`input_diag`
  3,337 B, `clock` 2,160 B).
  **OCRAM slice: done 2026-10-06** (owner decision 2026-10-05: fund the
  OCRAM, do not lower a floor). OCRAM free was really 131,072 B (32 B over
  the 131,040 B heap floor; the 131,360 `teensy_size` figure omits 288 B of
  `.bss.aius_retained`), and DTCM free 25,472 B against the 24,352 B
  stack floor (`firmware/link/aius_teensy41.ld:285`), so DTCM had to be
  funded first. Owner chose (2026-10-06) PROGMEM format strings over a
  per-object linker rule or a lower DTCM floor:
  1. `http_meters_json()::snapshot` (`AiusMeterSnapshot`, 4,776 B) moved
     from OCRAM `DMAMEM` to DTCM `.bss` (not a DMA buffer, not hot path).
  2. Twelve cold format literals in `firmware/src/optical_main.cpp`
     (5,106 B) became function-local `static const char ...[] PROGMEM`, the
     `http_clock_json` pattern; all in `FLASHMEM` HTTP render paths, none
     read from an ISR or ITCM. Rendered text byte-identical.
  3. `AIUS_RUNTIME_TEENSY_STATUS_BODY` 16,384 B
     (`firmware/src/aius_runtime_teensy.h`) now sizes `app_body`;
     `AIUS_DSP_MODEL_MAX_JSON_BYTES` stays 12,288.
  4. The suite is committed; `test.sh` requires it to pass at 16,384 and to
     refuse a limit equal to the worst case (and 12,288).
  Linked: DTCM free 25,824 B (+1,472 over the floor), OCRAM free 131,776 B
  (+736), ITCM unchanged at 191,804 B (`teensy41-optical`). Status headroom
  is now 1,660 B; Phase 4's own `clock` keys spend part of it, and the
  suite bounds them when they land.
- **`CLAUDE.md` promote.** The commit that writes the fill
  stamps into CLAUDE.md (`the release-promotion step`) also corrects §Device access and
  OTA's signing line: `aius_ota_sign.py sign` needs `--image`, `--output`
  and `--build-revision` (12-char HEAD, at most 32 chars). Today the line
  shows only `--secret`; both 2026-10-05 deploys needed the full form. If
  the file still carries foreign uncommitted edits, wait for their owner.
- **Gate open keeps the level pad; the setpoint is the opening occupancy**
  (entry gate item 1 amendment, which replaced the earlier "pad to a
  constant occupancy" design). `tx_gate_execute_open`
  (`firmware/src/optical_main.cpp:2630-2652`) still pads the ring to level
  128, so `r` = 0. Against a constant setpoint the opening occupancy (256 to
  384) would be a ±64-frame error pinning the output at the clamp after
  every relock, breaking gate 1 and the stable-phase labelling
  (`firmware/tools/net/aius_rate_bench.py:397-424`, `:642-664`); a setpoint
  equal to the opening occupancy has near-zero opening error. Not exactly
  zero (round 5 N9): the statistic, a mean of sub-window minima including
  late commits, reads a few frames below the open sample (about 7 AC-3, 13
  PCM at median spread), a one-off kick of about 90 to 175 mHz after each
  open, which the host cases bound.
  - *Read phi inside the masked section* that sets the level: read outside,
    an ISR between read and set skews it by 127 frames. Inside, the Phase 3
    tear check is blind (`fills` cannot move), so the open detects a pending
    period boundary from ONE TX DMA SADDR read returning `{half, offset}`:
    pending means SADDR lies in `g_aius_tx_fill_witness.last_half` (the DMA
    already reads the half the ISR last filled, and the half it just played
    is stale). The late ISR takes one half on the unmask, so a pending open
    padded only to level 128 would be left 1 to 128 frames. A pending live
    open therefore pads one half more, to level 256
    (`aius_tx_fill_open_level_frames`, Phase 4a F3): after the late ISR the
    level is 128 and the setpoint is `384 - phi' * 128`, the normal case's
    `384 - phi * 128` shape. The extra half counts in
    `gate_open_pending_padded_frames`, so `gate_open_padded_frames` still
    advances by one target per live relock. (Round 5 N1: the round
    4 "add 128" had the sign wrong and would set the setpoint 256 frames
    high, a repeating governor splice. Reading SADDR and the DMA interrupt
    flag separately can straddle a crossing, so the flag is not used, N4.)
    The accessor lives in `aius_spdif_sources.cpp` (slice clock-kind).
  - *Open only at a unit start* (owner, 2026-10-06; mechanism round 5 N3).
    An open armed between the six one-per-call AC-3 blocks executes at block
    k and lands k × 256 frames low; today level 128 at k ≥ 1 underruns at
    the open. The block carries a `first_in_unit` flag set by the
    pipeline/decoder (`AiusPcmBlock`,
    `firmware/lib/aius_codec_api/include/aius_codec.h:76-82` sees no unit
    position today). Unit start per class: AC-3 block 0 of 6
    (`a52_blocks_decoded == 1`); E-AC-3 the first block of each decoded
    access unit (`retained_offset == 0`; the pipeline's unit is one access
    unit of at most 1,536 frames, `aius_eac3.h:20`, so nothing counts a
    6,144-frame unit); DTS the first 512-frame slice of each access unit
    (`retained_offset == 0`; a DTS-2048 unit arrives as four slices,
    `aius_pipeline.c:1277-1294,1334-1340`, so marking every slice would
    reopen k × 512 low, round 6 R1); PCM and replay every block; a CRC
    silence substitution marks its first substituted block of each unit
    (`aius_pipeline.c:1275,1331`). The sensor's 6,144-frame E-AC-3 figure
    (`aius_tx_fill_sensor.h:182`) only makes the cap lenient (128 ms); a
    type-21 burst is one access unit (`aius_eac3.h:17-23`), so the marker is
    right (round 6 R8). The deferral is capped at one unit of committed
    frames since the arm (`aius_tx_fill_unit_frames(pc, block_frames,
    is_pcm)`, `aius_tx_fill_sensor.h:173`), then the open takes any block, so a class or a stale
    `codec_pc` that never marks a start cannot hold the room silent. No
    interaction with the 512-frame invalid hold: the open arms only after
    `transition_complete`.
- **Setpoint: per session, arithmetic.** `384 - phi_0 * 128` in frames from
  the corrected open read, fixed for the lock; a governor trim (level 128)
  leaves it valid. Valid across 48, 44.1 and 32 kHz because the open is in
  frames too (44.1 kHz unproven until a 44.1 source is observed). No
  learning to poison.
- **Invalid window** (fewer than 8 valid sub-windows,
  `firmware/lib/aius_audio/src/aius_tx_fill_sensor.h:257-290`; an invalid
  window serves `fill_level_q4` 0 with `fill_level_valid` 0, and
  `fill_windows` counts valid windows only). The loop gates on
  `fill_level_valid`, never reads q4 alone (e = −setpoint would slew to
  −900). The sensor gains a closed-window sequence number (valid or not)
  and per-window exclusion-reason bits (underrun, stall, missed fill, no
  sample), so the loop sees every close and the window cannot go silent
  (the round-1 governor-void trap). Rule (owner, 2026-10-06): an invalid
  window whose exclusions include an underrun and no stall reads as **128**,
  the true occupancy at an underrun (level 0 at phi 1); the output goes to
  the −900 clamp with `I` held, no special case. A stall, or no underrun:
  hold `I`, emit nothing. The step that follows is read as is. Never
  observed in the capture.
- **Control law: PI.** Per window, `I += Ki * e` and
  `offset = clamp(Kp * e + I)`, with `e` the window statistic minus the
  setpoint. A positive error (ring filling) speeds the output up. `I` is held
  in fixed point (µHz), because `Ki` is below 1 mHz per frame and would
  truncate to a P-only loop in integer mHz. Anti-windup: `I` holds while the
  clamp is active, and is itself clamped to ±900 mHz.
- **Gains.** Derived in the host plant model with the Phase 3 noise. Targets,
  in the order they give way: output modulation from sensor noise within
  ±50 mHz and peak fill excursion ≤ 64 frames for a ±700 mHz step hold;
  settling may relax from 10 min to 30 min. Reference point from review
  round 2: damping 0.7 and a 10 min settle give `Kp` about 13 mHz per frame,
  `Ki` about 0.36 mHz per frame per window and a 34-frame peak, which needs
  statistic noise of about 4 frames or less to meet ±50 mHz; a noisier
  statistic means a slower loop.
- **Authority.** Clamp ±900 mHz: the in-lock offset is at most the 500 mHz
  hysteresis plus the estimate error (about 170 mHz at most on 2026-10-05,
  670 together; the earlier "bias 55 plus wander 280, 835" double-counted),
  plus margin; well inside the plan band (±2 %). The composed target is also
  clamped into the plan band and into the centre's structural cell:
  `request_trim` refuses a plan that differs structurally from the applied
  one (`firmware/lib/aius_audio/src/aius_audio_clock.cpp:573-579`), so a
  target across an integer-divider crossing would be refused forever (at
  48 kHz the crossing is at 48,549 Hz, so this is a guard, not a case). A plan
  failure on a fill request is a no-op, never a fault.
- **Controller state and emission state are separate.**
  - *Controller state* is `centre` (the carrier-estimate target; hysteresis
    and the 10 s cadence judged against it alone, logic unchanged) and `I`.
    It updates every window (`I`) and every centre retarget, whether or not
    anything is written. A retarget is bumpless, `I -= centre_new -
    centre_old` (computed in int64 µHz: a full ±2 % band move is 1.92e9 µHz,
    near `INT32_MAX`), then `I` is re-clamped (any residual self-heals at the
    next retarget, because the centre follows the estimate). A committed
    centre also updates `g_clock_desired_plan`, so the fault-retry full apply
    (`firmware/src/optical_main.cpp:8767`) uses the current centre, with `I`
    zeroed.
  - *Retarget under saturation* (orchestrator calls, round 4 finding 7 and
    round 5 N2, N6). Saturated, `I -= Δ` leaves the offset at the clamp, so
    the composed target jumps by the whole Δcentre (500 mHz or more). A
    retarget whose Δcentre has the **same sign** as the saturated offset
    commits at once: the composed target moves toward what the loop already
    asks for, never past it. One of **opposite sign** is deferred, because
    that saturation resolves itself (the ring drains faster); the deferral
    is capped at 120 windows (8 min, the worst recovery), then commits.
    Deferring every retarget was declined: a source wandering past the
    clamp during an underrun recovery would keep the loop saturated forever
    with the retarget that would end it deferred (N2). The cap counts from
    the first proposal of a deferral episode; `_step` re-proposes every
    10 s while the reference is the old centre (`aius_clock.c:258-267`), and
    a re-proposal refreshes the pending value only, never the count (R5). Slew-limiting the
    composed step was declined: every legitimate retarget would take 25
    windows. `fill_reset(discard_pending = false)` commits a pending centre
    into `g_clock_desired_plan`, and is called **before**
    `aius_audio_clock_apply` at exactly two sites: the same-nominal
    structural full apply (`firmware/src/optical_main.cpp:8903`) and the
    fault retry (`:8767`); the apply is dark, so the bump is inaudible (N6).
    Every other `I`-zeroing event discards it (`discard_pending = true`):
    an accept (`:8848`, which replaces the desired plan with a fresh
    estimate), a replay (its desired plan must stay exactly 48,000,000 mHz,
    `:8744-8749`, `:8924-8937`, or `g_clock_replay_ready` stays false) and
    carrier loss (round 6 R3). `clock_apply_desired_plan` has four call
    sites (`:8767`, `:8848`, `:8903`, `:8936`); carrier loss is a discard
    event, not an apply site (round 7 F3). A pending centre is also dropped
    when a 1 s window mean returns inside the hysteresis of the current
    reference, so a cap or end of saturation never commits a stale value.
  - *Existing centre-path behaviour survives* (round 6 R7). The trim branch
    at `:8851-8900` keeps its invalid-plan fault for a centre target
    (`:8854-8857`), and a committed centre whose plan differs structurally
    takes the structural full apply (`:8900-8905`) with `I` zeroed, never a
    composed target in a cell `request_trim` refuses forever.
  - *The tracker is told the centre only.* `clock_refresh_applied`
    (`firmware/src/optical_main.cpp:8751-8759`) passes the applied target to
    `aius_rate_tracker_note_target_applied`, which sets
    `applied_target_millihz` and resets `last_target_request_ms`
    (`firmware/lib/aius_clock/src/aius_clock.c:344-345`). **The centre and
    the hysteresis reference are one field** (`applied_target_millihz`,
    round 6 R2: two fields left the new centre uninitialised after a
    relock, so the first FILL write was composed on the pre-relock centre,
    and at a nominal change every request was refused at
    `aius_audio_clock.cpp:575`). Accept sets it to the accept target; a
    centre commit sets it; the parent's note sets it once, on the pass
    where `g_clock_dark_transition_pending` goes from true to false (not on
    "applied differs from noted", which would fire on every FILL write),
    keeping today's guards `!replay_selected` and applied nominal equal to
    `g_clock_nominal_rate_hz` (`optical_main.cpp:8751-8753`), so a replay's
    dark transition never notes 48,000,000 as the live centre. "Note iff
    applied equals desired" was declined: after a bumpless retarget the
    composed target almost never equals the centre, so it would never fire
    (interface draft, 2026-10-06). A FILL readback never notes, or every
    fill write restarts the 10 s cadence and moves the hysteresis reference
    onto the composed value (the round-1 starvation blocker through unnamed
    wiring). `accept_candidate` today zeroes `applied_target_millihz`
    (`aius_clock.c:221`), and `observe_measurement_window` returns early
    while it is 0 (`:258`); Phase 4 changes accept to set it to the accept
    target instead (round 7 F1), and the note becomes idempotent,
    re-asserting the full apply's readback (including a fault retry that
    committed a pending centre).
  - *Emission state is the applied-plan readback on every pass*, never a
    value committed on a true return: the applied plan lags a true return by
    up to one 128-frame period, and a queued trim can be superseded by a full
    apply. A true return only spends that window's single write; a refusal
    spends nothing and the next pass retries. The tracker API carries both
    states, rather than `optical_main` wiring.
- **Emission.** One composed request per `sample_rate_service` pass, at most
  once per window, and only when the composed target differs from the
  emission state by more than the emit threshold (20 mHz, entry gate item
  5). At the measured noise writes are not rare: the bound is one FILL write
  per window.
- **Kinds and diagnostics.** A centre commit raises the CENTRE flag. If the
  commit pass needs no write (sub-threshold, bumpless), the flag clears at
  the end of that pass, so a noise write minutes later is not labelled
  CENTRE with a false `trim_last_at_ms`. If it needs a write, CENTRE
  persists until that write is accepted, through refusals and spent
  windows, and a CENTRE write bypasses the one-FILL-per-window budget
  (round 6 R4). The flag also clears on any later pass that needs no write
  and on `fill_reset`, so a needed write that stops being needed cannot
  leave it up for a noise write minutes later (round 7 F2). Every other
  write is FILL. A centre retarget counts `trim_updates`
  and records `AIUS_EVENT_CLOCK_TRIM` when it commits, whether or not a write
  follows, so centre activity stays visible. A CENTRE write stamps
  `trim_last_at_ms` and `trim_applied_at_ms`
  (`aius_spdif_sources.cpp:184`); a FILL write counts `fill_trims` and stamps
  `fill_trim_last_at_ms` and `fill_applied_at_ms`, with no event. The
  glitch-reading stamp list in `CLAUDE.md` gains the two
  fill stamps in the Phase 4 commit (a protected file: `the release-promotion step`).
- **The kind is a separate field, not a new request value.** The boundary
  ISR treats every request other than `NONE` and `TRIM` as a HOLD
  (`aius_spdif_sources.cpp:162-197`, enum at
  `aius_audio_clock_internal.h:8-12`): it marks validity invalid and arms
  the four-period hold without writing the numerator, the 2026-09-07 mute.
  So the request stays `TRIM` and a separate stamp-kind field picks only
  which stamp the ISR writes. The kind is an argument of
  `aius_audio_clock_request_trim`, stored inside its critical section after
  the pending check (`firmware/lib/aius_audio/src/aius_audio_clock.cpp:567-599`),
  never a global set before the call: a refused FILL call must not overwrite
  the kind of a CENTRE trim still pending. `clock-target` builds with
  `-DAIUS_AUDIO_CLOCK_FAKE_TARGET` (`firmware/scripts/test.sh:523`), which
  compiles out the ISR stamp (`aius_spdif_sources.cpp:184-189`), so the stamp
  moves behind a fakeable clock and `clock-target` asserts a FILL write
  writes NUM, leaves `VALCTRL` and `invalid_periods_remaining` untouched,
  and stamps only the fill pair (and a CENTRE write only the centre pair).
  Cost: one load, one compare, one DTCM store in the ITCM-resident ISR,
  recorded against ITCM headroom.
- **Scope and reset.** Emission only for a live source with
  `g_clock_plan_applied`; replay blocks emit nothing, so
  `g_clock_replay_ready`'s exact-48,000,000 check is untouched. `I` is kept
  across a same-nominal gate blip (a real source change drops lock, and the
  re-accept's dark full apply zeroes it) and zeroed on a nominal change, a
  replay, or a dark full apply. The last-applied composed value is read back
  from the applied plan, never assumed.
- **Governor.** Unchanged; with the ring held at the gate-open floor it only
  sees stall backlogs, its intended job.
- **Cleanup.** The `AIUS_CLOCK_TARGET_HYSTERESIS_MILLIHZ` comment
  (`firmware/lib/aius_clock/src/aius_clock.c:18-34`) loses its drift-bound
  claim and points here.

**Observability.** `clock` gains `fill_offset_millihz` (signed, composed
minus centre), `fill_setpoint_frames` (this lock's setpoint),
`fill_open_boundary_pending` (which setpoint formula the open used), `fill_trims`,
`fill_trim_last_at_ms` and `fill_applied_at_ms`; the fill sensor's block
gains the closed-window sequence number and the last window's
exclusion-reason bits. Same openapi check; the body bound is
`AIUS_RUNTIME_TEENSY_STATUS_BODY` (16,384 B, 1,660 B headroom before these
keys), enforced by `status-body-bound`. No new event kind.
`gate_open_target_frames` and the `CLAUDE.md` line "advances
by one `gate_open_target_frames` per live relock" stay true, because the pad
stays constant.

**Slice interfaces** (fixed 2026-10-06 by the interface draft; the parent
holds every slice to them). No slice edits a file another owns. A type that
crosses a slice boundary has a copy in each owner's header with equal
numeric values, and the parent `static_assert`s the match in
`firmware/src/optical_main.cpp`. The tracker never includes
`aius_tx_fill_sensor.h`; `aius_audio_clock.h` never takes the tracker's kind
enum. The decoders do not change: the pipeline already knows each block's
position.

```c
/* aius_clock.h (fill-loop-core) */
#define AIUS_CLOCK_FILL_EXCL_UNDERRUN    UINT8_C(0x01)
#define AIUS_CLOCK_FILL_EXCL_STALL       UINT8_C(0x02)
#define AIUS_CLOCK_FILL_EXCL_MISSED_FILL UINT8_C(0x04)
#define AIUS_CLOCK_FILL_EXCL_NO_SAMPLE   UINT8_C(0x08)
#define AIUS_CLOCK_FILL_RETARGET_DEFER_MAX_WINDOWS 120u
typedef enum { AIUS_RATE_TRACKER_TRIM_NONE = 0, AIUS_RATE_TRACKER_TRIM_CENTRE = 1,
               AIUS_RATE_TRACKER_TRIM_FILL = 2 } AiusRateTrackerTrimKind;
typedef struct {            /* copied from AiusTxFillSnapshot by the parent */
	uint32_t window_seq;    /* closed windows, valid or not; wraps */
	int32_t level_q4;       /* read iff level_valid */
	bool level_valid;
	uint8_t exclusions;     /* AIUS_CLOCK_FILL_EXCL_*, last closed window */
} AiusRateTrackerFillWindow;
typedef struct {
	bool emit_allowed;                /* live, g_clock_plan_applied, gate open */
	bool have_applied;
	uint32_t emission_readback_millihz; /* applied plan readback = emission state;
	                                       never written into the tracker's centre */
	uint32_t setpoint_frames;         /* this lock's; 0 = none yet */
	AiusRateTrackerFillWindow window;
} AiusRateTrackerFillInput;
typedef struct {
	AiusRateTrackerTrimKind emit_kind;  /* NONE: write nothing this pass;
	                                       field below is the centre, never the readback */
	uint32_t composed_target_millihz;   /* clamped to band and centre's cell */
	uint32_t centre_millihz;            /* g_clock_desired_plan target */
	int32_t fill_offset_millihz;        /* composed minus centre */
	int32_t fill_integral_microhz;      /* clamped to +/-900000 */
	bool centre_committed;              /* this pass: trim_updates++, event, desired plan */
	bool retarget_pending;
	uint8_t retarget_pending_windows;   /* 0..120 */
	bool saturated;
} AiusRateTrackerFillResult;
/* AiusRateTracker gains: pending_centre_millihz, pending_centre_windows
 * (counts from the episode's first proposal), fill_integral_microhz,
 * fill_offset_millihz, fill_last_seq, fill_spent_seq, fill_have_seq,
 * fill_spent, fill_saturated, centre_flag (persists until a CENTRE write
 * is accepted). The centre IS applied_target_millihz (one field). The
 * setpoint is input only (in->setpoint_frames governs; no tracker copy). */
AiusRateTrackerFillResult aius_rate_tracker_fill_step(
	AiusRateTracker *tracker, const AiusRateTrackerFillInput *in); /* every pass, after _step */
void aius_rate_tracker_note_emit(AiusRateTracker *tracker, bool accepted); /* true spends the window */
bool aius_rate_tracker_fill_reset(AiusRateTracker *tracker, bool discard_pending,
	uint32_t *committed_centre_millihz); /* zero I, clear the CENTRE flag; true if a
	pending centre committed. On true the parent, before clock_apply_desired_plan,
	counts trim_updates, records AIUS_EVENT_CLOCK_TRIM and rebuilds
	g_clock_desired_plan from the committed centre; if that plan is invalid it
	faults as the centre path does today (clock_record_fault(INVALID_PLAN)
	marks the desired plan invalid, so the following apply returns false;
	orchestrator call 2026-10-06: unreachable in band, as every in-band
	centre has a valid plan, so today's behaviour is kept). */
/* (no tracker setpoint function: aius_tx_fill_open_setpoint_frames is the one) */
/* _step: a proposal goes to pending_centre (target_update still reported);
 * a committed centre sets applied_target_millihz itself; accept_candidate
 * sets it to the accept target and zeroes I. note_target_applied:
 * signature unchanged, called once when g_clock_dark_transition_pending
 * goes true -> false, and only for a transition clock_apply_desired_plan
 * started (clock_record_fault also raises the flag with no apply,
 * optical_main.cpp:8662; that edge never notes). */

/* aius_audio_clock.h (clock-kind) */
typedef enum { AIUS_AUDIO_CLOCK_TRIM_CENTRE = 1, AIUS_AUDIO_CLOCK_TRIM_FILL = 2 }
	AiusAudioClockTrimKind;                 /* 0 or other: refused */
bool aius_audio_clock_request_trim(const AiusSpdifClockPlan *plan,
	AiusAudioClockTrimKind kind);
/* aius_audio_clock_internal.h: AiusAudioClockSharedState gains
 * volatile uint32_t queued_kind (stored after the pending check). */
extern "C" volatile uint32_t g_aius_tx_trim_applied_at_ms; /* defined in FAKE too */
extern "C" volatile uint32_t g_aius_tx_fill_applied_at_ms;
/* aius_audio_clock_internal.h: static inline, so the ITCM ISR never calls
 * into flash; millis() on target, a fake variable under FAKE_TARGET. */
static inline uint32_t aius_audio_clock_now_ms(void);
#if defined(AIUS_AUDIO_CLOCK_FAKE_TARGET)
void aius_audio_clock_fake_set_now_ms(uint32_t now_ms);
#endif
/* aius_spdif_sources.cpp: the CALLER holds the interrupt mask; the accessor
 * never touches PRIMASK (an unconditional interrupts() would reopen the
 * parent's masked section); FASTRUN, not FLASHMEM, not inline (another TU).
 * ONE SADDR read; half 0/1; pending decided by the shared pure predicate. */
extern "C" uint32_t aius_spdif_tx24_dma_read_position(uint32_t *half,
	uint32_t *offset_frames);

/* aius_tx_fill_sensor.h (fill-sensor-window) */
#define AIUS_TX_FILL_EXCL_UNDERRUN 0x01u   /* = AIUS_CLOCK_FILL_EXCL_* */
#define AIUS_TX_FILL_EXCL_STALL 0x02u
#define AIUS_TX_FILL_EXCL_MISSED_FILL 0x04u
#define AIUS_TX_FILL_EXCL_NO_SAMPLE 0x08u  /* sub-window closed with cur_min INT32_MAX */
/* AiusTxFillSnapshot += uint32_t fill_window_seq (every close, lifetime);
 *                       uint32_t fill_window_exclusions (OR over the last window).
 * AiusTxFillSensor += uint32_t cur_reasons, win_reasons; reset zeroes both and
 * the served exclusions and keeps the seq. */
static inline void aius_tx_fill_sensor_exclude(AiusTxFillSensor *s, uint32_t reason);
/* replaces the one-argument exclude, which is removed */
/* Shipped opening rules, pure, tested in tx-fill-sensor and called (not
 * modelled) by the test/clock plant and by optical_main (clean-room 1).
 * Always-inline and WITHOUT AIUS_TX_FILL_SENSOR_COLD (FLASHMEM in both
 * callers, optical_main.cpp:130, aius_spdif_sources.cpp:23-26), so neither
 * the FASTRUN accessor nor the masked gate open runs from flash (N2): */
static inline bool aius_tx_fill_boundary_pending(uint32_t saddr,
	uint32_t ring_base, uint32_t last_half_base);  /* SADDR lies in last_half */
static inline uint32_t aius_tx_fill_open_setpoint_frames(uint32_t level_frames,
	uint32_t offset_frames, bool boundary_pending); /* level + 256 - offset,
	minus 128 if pending (384 - phi*128, or 384 - phi'*128 pending: the pending
	open sets level 256) */
static inline uint32_t aius_tx_fill_open_level_frames(uint32_t target_frames,
	bool boundary_pending); /* target, plus 128 if pending (F3) */
typedef enum { AIUS_TX_FILL_OPEN_WAIT = 0, AIUS_TX_FILL_OPEN_NOW = 1 } AiusTxFillOpenDecision;
static inline AiusTxFillOpenDecision aius_tx_fill_open_decide(bool first_in_unit,
	uint32_t committed_since_arm_frames, uint32_t unit_frames); /* NOW at a unit
	start, or once committed_since_arm_frames >= unit_frames (the cap) */

/* aius_codec.h (unit-start); appended so positional initialisers stay valid;
 * AIUS_CODEC_ABI_VERSION 1 -> 2 */
typedef struct { int32_t *planes[AIUS_MAX_CHANNELS]; uint16_t channels;
	uint32_t frames; uint32_t sample_rate_hz; uint32_t channel_mask;
	uint8_t first_in_unit; /* 1: a gate open may land here */ } AiusPcmBlock;
```

- **Before spawning**, the parent lands `-I$fw_root/lib/aius_audio/src` on
  the clock line at `firmware/scripts/test.sh:517` (done 2026-10-06,
  uncommitted until the fill-loop-core slice first runs the suite; add
  `-lm` there if the plant needs it).
- **fill-loop-core** consumes nothing new. The `test/clock` plant includes
  the Phase 3 sensor header read-only for its arithmetic and derives
  `window_seq` and `exclusions` from its own injected faults (a window
  closes every 192,000 frames). It **calls** the shipped
  `aius_tx_fill_boundary_pending`, `aius_tx_fill_open_setpoint_frames` and
  `aius_tx_fill_open_decide` rather than modelling them, so a sign or
  deferral error in the shipped rule fails the clock suite (clean-room 1).
  Its only other sensor calls are `aius_tx_fill_saddr_offset_frames`
  (`aius_tx_fill_sensor.h:153`), `aius_tx_fill_unit_frames` (`:173`),
  `note_underrun`, `note_missed_fill` and `commit`, whose signatures
  fill-sensor-window must not change; it never calls `exclude` (clean-room
  5). A host case feeds a FILL readback that differs from the centre and
  asserts the centre is unchanged (clean-room 4).
- **clock-kind** updates `test_clock_target.cpp:501,544,612,614,649` to the
  new signature and adds a FILL stamp, a CENTRE stamp and a refused FILL
  keeping a pending CENTRE's kind; `itcm-placement` records the ISR and
  accessor bytes.
- **fill-sensor-window** consumes nothing. It lands the three pure opening
  functions first (the plant depends on them), with tx-fill-sensor cases:
  pending true and false at both halves and at the half edges, setpoint
  `384 - phi' * 128` pending (level 256) and `384 - phi * 128` not, the open
  level (`aius_tx_fill_open_level_frames`), one setpoint case
  at a level other than 128 (N3), and the open
  decision at a unit start, mid-unit below the cap and at the cap.
- **unit-start** sets the flag after `aius_pipeline_slice_pcm_block`, in
  `fill_pcm_descriptor` (`aius_pipeline.c:1124-1133,1281-1294`; a retried
  block keeps its `sink_block`, so the flag survives a retry) and in
  `fill_silence_descriptor` (`:748-775`, which memsets `sink_block` at
  `:767`): a substituted block is marked when its first frame starts a unit
  of the shape `note_substitution_shape` records, so a substitution
  covering several units marks one block per unit (clean-room 3). Pipeline
  suite cases: AC-3 marks block 0 only; E-AC-3 the first slice only;
  DTS-2048 1 of 4 slices; DTS-512 every slice; PCM every block; a retry
  keeps the flag; a multi-unit substitution marks once per unit. It bumps
  `AIUS_CODEC_ABI_VERSION` to 2 (a layout change; orchestrator call) with
  `test_codec_api_contract.c:10` and `test_capability.c:330`. It converts
  the nine positional `AiusPcmBlock` initialisers in
  `firmware/test/dsp/test_dsp.c` (`:594`, `:1304`, … `:3215`) to designated
  initialisers, since the appended field trips
  `-Wmissing-field-initializers` under `-Werror` (`test.sh:77`).
- **parent-wiring** consumes everything: local `extern "C"` declarations
  (the `optical_main.cpp:1440` pattern), the enum and exclusion-bit
  `static_assert`s, the trim branch at `:8851-8900` replaced by `fill_step`,
  plan, `request_trim(kind)`, `note_emit`; `first_in_unit` ignored on replay
  blocks (`:2119`). `tx_gate_execute_open`, inside its masked section,
  calls the accessor (which alone calls `aius_tx_fill_boundary_pending` on
  its one SADDR read; never a second SADDR read in the parent), then
  `aius_tx_fill_open_setpoint_frames` and `aius_tx_fill_open_decide` with
  the accessor's pending result, and publishes `fill_open_boundary_pending`
  with the setpoint (clean-room closure N1). It bounds the new keys in `status-body-bound`, and runs every target build itself, one
  at a time; slices run host suites only.

**Host proof (clock suite).** The tracker API owns the centre/integral
wiring, so the model exercises it end to end. Plant: TX consumes in 128-frame
periods at the output rate; the producer commits 512-frame PCM blocks and
6 × 256-frame AC-3 bursts at the source rate with service jitter; the sensor
is the Phase 3 header with the Phase 3 noise; the carrier estimate is
quantised to 140 mHz bins with 1.2 Hz spread and slow wander (280 mHz over
8 min). Cases, two simulated hours each unless noted:

- offsets against the centre of 0, ±225, ±480, ±550, ±650 and ±800 mHz,
  each with the gate opening at `phi` 0, 0.47 (the worst phase under the old
  design) and 1: zero underruns, the setpoint equals the opening occupancy,
  fill error ≤ 64 frames throughout, no governor trim, modulation within
  ±50 mHz, writes within the threshold's bound, and at steady state the
  offset is carried by `I` (P term near 0);
- the opening kick: the first windows after an open stay within 16 frames
  of the setpoint and 200 mHz of the centre;
- a gate open with a pending period boundary (SADDR in `last_half`): the
  open pads to level 256 (`gate_open_pending_padded_frames` +128), the
  setpoint is `384 - phi' * 128` (not 128 or 256 higher), no underrun at
  the open, no governor trim over the following hour;
- a gate open armed at AC-3 block k = 1 to 5 and at an E-AC-3 mid-unit
  block: deferred to the next unit start, no underrun at the open; a class
  that never marks a unit start opens after one unit duration;
- a source drifting past centre + 900 mHz during an underrun recovery: the
  same-sign retarget commits at once, no deadlock, no governor splice; an
  opposite-sign retarget defers and commits by 120 windows;
- a full apply (relock or fault retry) during a deferral: the pending centre
  lands in the desired plan, `I` zeroed, the readback notes once (dark
  transition ends), and retargets resume after the relock;
- a relock at a different rate (same nominal and across a nominal change):
  the first FILL write is composed on the new centre, never the old;
- a `clock_record_fault` dark transition with no apply: its end does not
  note, and the centre is unchanged;
- a replay or carrier loss during a deferral: the pending centre is
  discarded, the replay plan stays exactly 48,000,000 mHz, replay ready;
- a deferral with re-proposals every 10 s: the retarget commits by window
  120;
- a CENTRE-needing commit whose write is refused, then lands in a spent
  window: the eventual write is CENTRE and stamps the centre pair;
- a CENTRE-needing commit refused, then sub-threshold, then a later noise
  write: that write is FILL; a full apply while the flag is up clears it;
- a deferred centre whose estimate drifts back inside the old centre's
  hysteresis: the pending centre is dropped, nothing commits;
- a committed centre whose plan is invalid faults as today; one that is
  structurally different takes the structural full apply with `I` zeroed;
- a gate open armed at DTS-2048 slice k = 1 to 3: deferred to the next
  access unit's first slice, no underrun at the open;
- one injected underrun at the setpoint with `I` at −670, 0 and +670 mHz:
  no second underrun, no governor trim, worst recovery within 16 frames
  stated (about 8 min expected);
- a centre retarget during clamp saturation (underrun recovery), each sign:
  composed target never steps past the loop's own demand;
- an invalid window with underrun bits: reads 128; with stall bits or no
  underrun: `I` held, nothing emitted; a q4 0 with `fill_level_valid` 0 is
  never read as a level;
- a FILL write's readback does not note a target: `last_target_request_ms`
  and the hysteresis reference unchanged;
- a refused FILL call while a CENTRE trim is pending: the pending kind is
  kept (clock-target);
- ±950 mHz past the clamp, with an injected estimate bias of at least
  450 mHz so the centre stays 950 off (without it the centre retargets within
  10 s): offset pinned at the clamp, and the 50 mHz residual either drains
  the ring to an underrun or fills it to a governor splice about every
  85 min ((384 - 128) / 0.05 s); both signs stated, neither worse than today;
- a mid-lock same-nominal gate blip, re-opening at AC-3 block k ≠ 0: the
  open defers to the burst start, the setpoint is re-read, `I` kept, no
  transient beyond the modulation bound;
- the trough-timing jitter measured in Phase 3, injected into the sensor
  model for every case;
- a PCM-to-AC-3 switch with the gate open, modelled from the transition the
  capture recorded (the gate stays open through a class change: the open
  decision `aius_tx_gate_permitted(carrier, g_clock_live_ready,
  g_clock_replay_ready, source)` at `firmware/src/optical_main.cpp:8973-8977`
  takes no class input; and the real gap may re-base the
  floor): its assertion is set from that capture, not assumed;
- a centre retarget with non-zero `I`: composed target continuous;
- a 2,048-frame stall backlog: the governor trims once, the loop neither
  fights it nor overshoots into an underrun;
- replay with the gate open: no emission, replay readiness kept;
- a same-nominal gate blip keeps `I`; a nominal change, replay or full apply
  zeroes it;
- a trim refusal: emission state uncommitted and retried next pass, while
  controller state keeps advancing; a sub-threshold window still integrates;
- a bumpless centre retarget that writes nothing: committed once, not
  re-fired every 10 s, and the desired plan follows it;
- a composed target outside the plan band or the structural cell: clamped,
  no fault, no permanent refusal;
- a source warm-up ramp of about −14 mHz/min (−250 mHz over 30 min, as
  measured 2026-10-05): tracking error within about 3 frames, no underrun;
- an in-lock class switch with a 4-frame floor bias and a floor re-base:
  no underrun, modulation within the bound;
- every case at 48, 44.1 and 32 kHz window lengths (4, 4.35 and 6 s),
  because `Ki` is per window: no underrun, fill error ≤ 64 frames;
- the closed-form figures in entry gate items 4 to 6 (34.5-frame step peak,
  10.3 min settle, write fraction at 20 mHz, the item 6 recovery times)
  reproduced within 20 percent;
- fixed point: negative `e_q4` rounds symmetrically; `345 * e_q4` and the
  int64 retarget term do not overflow across the plan band.

**Device proof.** One listening-room AC-3 session of 45 min or more polled with
`firmware/tools/device/aius_status_poll.py`, plus an unpolled segment of
10 min or more (`--interval 30`). Pass criteria:

- `underrun_episode_count` delta 0 and `governor_trims` delta 0;
- settled |`fill_offset_millihz`| of at least 100 mHz, or the session is
  marked inconclusive (a source near zero offset proves nothing);
- settled `fill_level_q4` within ±16 frames of `fill_setpoint_frames`, and
  the setpoint matching the formula for the logged
  `fill_open_boundary_pending` (257 to 384 in both cases;
  the "unchanged standing latency" outcome; clean-room 6);
- settled mean |fill error| ≤ 4 frames over the last 10 min (noise σ 1.2,
  threshold 1.5): a P-only regression (`Ki` truncated to 0) holds offsets up
  to 208 mHz inside ±16 frames and would otherwise pass;
- `fill_trims` delta at most the `fill_window_seq` delta (all closes, N10)
  plus 1 (R12), and at most half of it
  in a polled session (entry gate item 5).

Before reading the session, fix the reducer
(`firmware/tools/device/aius_fill_capture_analysis.py:344-366`): split
`episode_free_slope` spans at a gate close, so a relock's re-base is not
fitted as drift (the 2026-10-05 +0.38 frames/s artefact). Proof: its
`--self-test` gains a case with a gate close inside a span. Done: the
split and its case landed with the Phase 4 build (`e019dc0`); 2026-10-06
added a split on `gate_open_pending_padded_frames` (F3) as well
(`PAD_KEYS`), `--self-test` ok.

Bench: gate 1's rebased criterion (`max_follow_error_millihz` should stay
about the estimate's own spread, 798 of 1,000 on 2026-09-04; if it breaches,
the owner rules on the criterion before acceptance), the transition gates
whose stable-phase labelling uses the same bound
(`firmware/tools/net/aius_rate_bench.py:410-424`), and gate 7's
carrier-continuity check at fill-write cadence, since the existing evidence
covers isolated NUM writes only.

**Device evidence 2026-10-06, `df966f17b370`, SHORT.** listening-room AC-3,
11:35:22 to 12:15:36 UTC: 33.7 min polled at 1 Hz (a 7 min unpolled gap
at 12:00 from a late segment launch; counters continuous), 0 min
unpolled (source switched off at 12:15:37 before that segment). Every
criterion that could run passed:

- `underrun_episode_count`, `governor_trims`, `tx_underrun_frames`,
  `tx_missed_fills` deltas 0;
- settled `fill_offset_millihz` about −233 mHz (not inconclusive);
- setpoint 377 throughout (`fill_open_boundary_pending` false, inside 257
  to 384), max |fill error| 8 frames, last-10-min mean 0.99 frames;
- `fill_trims` 184 against `fill_window_seq` 604 (30.5%);
- `centre_writes`, `trim_updates`, `structural_replans`,
  `invalid_windows`, both pad counters: delta 0; window-to-window σ 1.14
  frames;
- polled sub-window spread p95 52 frames (median 7, max 75); no unpolled
  figure, so Phase 4b's comparison is open;
- two owner seeks (11:44 and 11:59 UTC) left no trace: gate open, no
  counter moved, fill level 370 to 379 frames.

Not yet proven: 45 min polled, 10 min unpolled, and gate 7 at fill-write
cadence. Next session: one polled run of 45 min or more and an unpolled
run of 10 min or more, launched back to back (a background job ends at
about 30 min, so two polled jobs with no gap), then gate 7. Raw data:
`.scratch/2682a96a-3760-4d00-b9e0-f71f66b336ba/session-4a/` (reaped about
2026-10-13).

Prebuild review: rounds 1 to 3 and the clean-room confirmation are done
(records in Phase 3). Round 4 (2026-10-06, against the decided entry gate):
NOT READY, two blockers (the constant-occupancy pad's fixed residue; the
open's phi read racing the ISR) and nine contract-text findings, all folded
above; owner settled the setpoint scheme, the invalid-window rule and the
burst-start open, and accepted the orchestrator's recommendations for the
rest of the phase. Round 5 (same reviewer, 2026-10-06): NOT READY, three
blockers the round 4 fixes opened (pending-boundary sign, deferral
deadlock, an unobservable burst start) and four contract-text holes, folded
above with §Slice interfaces. Round 6 (2026-10-06): NOT READY, two
blockers (DTS slices all marked; two centre references after a relock) and
five contract-text holes, folded. Round 7 (2026-10-06): READY; three text
fixes (accept sets the reference, the CENTRE flag's clear paths, four apply
sites and a stale-pending drop) folded. Clean-room (2026-10-06): NOT
READY, one blocker (the opening rules were tested only against the plant's
own copy, so the round 5 sign error or a DTS mis-mark passed every host
case; now three shipped pure functions the plant calls) and nine text
fixes, folded. Closure check (2026-10-06): READY; N1 (the accessor alone
reads SADDR), N2 (opening functions always-inline, not COLD) and N3 (a
non-128 level case) folded. Prebuild review complete. Phase 4's delivery audit re-attacks: the opening
read and setpoint; the controller and emission state split under refusal,
saturation and sub-threshold windows; the stamp-kind field on every write
path; and the fill loop against the governor.

**Build record (2026-10-06).** Slices fill-sensor-window, clock-kind,
unit-start and fill-loop-core landed with their host suites green; the
clock suite carries the full host-proof list. Measured against the entry
gate's closed form: 700 mHz step peak 36.0 frames (34.5), back inside 16
frames in 277 s (the gate text's "about 3.5 min" should read about
4.7 min, as the continuous PI gives 283 s), settled in 405 s (618 s was
the envelope bound), write fraction 0.39, modulation 17.1 mHz rms,
underrun recovery 101 / 145 / 489 s for `I` −670 / 0 / +670 (gate: 99 /
144 / 491), ramp tracking −2.71 frames. Plant readings to know: anti-windup
holds `I` when |P + I_old| > 900; the offset matrix holds the centre fixed
(estimate noise covered separately up to ±480, because ±650 to ±800 plus
the full 1.2 Hz estimate noise exceed the 670 authority budget); only the
offset matrix runs at all three rates. The tracker gained
`fill_proportional_millihz` (the last P, needed to hold the offset at the
clamp across a saturated retarget). Delivery-audit items from the slices:
- **Untested unit-start cases.** DTS-2048 and a multi-unit substitution
  cannot be driven from the pipeline API (the corpus has only DTS-512, and
  every substitution today is one unit); the multi-slice
  `retained_offset == 0` path is covered through E-AC-3. Add a DTS-2048
  vector when one is captured.
- **Cap opens mid-unit.** A class that never marks a unit start opens at
  the cap and took 7 underruns at the open in the plant: the accepted cost
  of the guard, worth confirming against real classes in the audit.
- **Sensor sample between a DMA crossing and its ISR** reads 128 high;
  harmless at real ISR latency, but the plant models latency only at the
  open.

Parent-wiring landed (2026-10-06): both images build; ISR 496 B (+12),
accessor 92 B, ITCM 191,996 B (+192), DTCM free 25,760 B (1,408 over the
floor), OCRAM free 131,776 B; status worst case 14,981 B (1,402 B
headroom). Recorded deviations: `aius_tx_fill_open_decide` runs before the
mask (it reads no ISR state; the setpoint is still computed inside);
`clock_fill_service` runs before `clock_refresh_applied` (on the dark-end
pass emission is blocked anyway); a centre commit now counts
`trim_updates` and records the event at commit, not at an accepted trim;
carrier loss and the rate-change quiesce also call `fill_reset(true)`; the
reducer also splits a span when `gate_open_padded_frames` changes between
two open polls.

Gate 1's 1,000 mHz follow-error bound was set on a bench with no underruns;
an underrun's clamp excursion (entry gate item 6) would breach it, so a
listening-room session reads gate 1 only across underrun-free spans.

### Phase 4a Delivery-audit fixes before the device session ✓ SHIPPED 2026-10-06

```yaml
phase: 4a
title: Close the Phase 4 delivery audit before the listening-room session
status: shipped
depends-on: phase 4 build (committed 2026-10-06, host suites and both images green)
outcome: >-
  The Phase 4 build carries no known path to a periodic underrun, its bench
  tooling can see fill writes, and its stamps and counters say what
  CLAUDE.md tells a reader they say.
unit: component-behind-interface
seams:
  - aius_tx_fill_open_setpoint_frames and tx_gate_execute_open pad (F3)
  - clock_fill_service / clock_refresh_applied order, centre commit gating (N5 to N8)
  - gate 1 and gate 7 bench tooling (F2, N12)
targeted-proof:
  - host: clock (new cases per item), tx-fill-sensor, clock-target, optical-cutover, status-body-bound; both images build
  - bench: aius_rate_bench gate 7 at fill-write cadence with the fixed matcher (moved to the Phase 4 device session; needs the image deployed)
review-intensity: active
```

Shipped on host proof: clock, tx-fill-sensor, tx-gate, tx-governor,
clock-target, optical-cutover, status-body-bound, itcm-placement,
registry and sd-editor green; both images build (ITCM free 4,612 B
plain, 4,340 B canonical); tool tests rate-gate-capture 93,
rate-runner 71, latency-runner 119. Delivery review FAIL, then same-reviewer
closure PASS, then clean-room PASS. B1 moved to Phase 4d. The owner
confirmed the autonomous calls R2, R3 and R13 (2026-10-06).

Delivery audit (2026-10-06, `reviewer` `opus`) of the Phase 4 build:
FAIL on B1 only; no blocking code defect. Each item decided by the
orchestrator under the owner's Phase 4 delegation:

- **B1, CLAUDE.md promote.** Moved to Phase 4d. Add `fill_trim_last_at_ms` and
  `fill_applied_at_ms` to the glitch-reading stamp list and the full
  `aius_ota_sign.py sign --image --output --build-revision` form, via
  `the release-promotion step`. Also add `clock.gate_open_pending_padded_frames`
  (F3 and R3: 0 to 128 per pending live open, 0 when the ring already
  holds 256 or more; by design) to the counters that read as faults and
  are not. Name the write counters: `clock.fill_trims` counts FILL writes,
  `clock.centre_writes` accepted CENTRE writes, and `trim_updates` every
  centre commit since N12, most of which write nothing (bumpless). Blocked while the file
  carries another session's uncommitted edits (re-check `git -C git_modules/aius diff --stat --
  CLAUDE.md` before acting; never from memory).
- **F2, gate 7 cannot see fill writes.** `rate_gate_capture/capture.py:559-576`
  and `net/aius_rate_bench.py:1033-1044` match waveform jumps against
  `trim_updates` only. Match against the two applied-at stamps
  (`trim_applied_at_ms`, `fill_applied_at_ms`) and add a fill-cadence
  assertion.
- **F3, pending-boundary open leaves 1 to 128 frames.** Decided: when
  pending, pad to the level-128 target plus 128, so after the late ISR the
  level is 128 and the setpoint is `384 - phi' * 128` as in the normal
  case; count the extra pad separately (new counter) so the CLAUDE.md
  `gate_open_padded_frames` line stays true. Update
  `aius_tx_fill_open_setpoint_frames` and its tx-fill-sensor cases, the
  plant, and §Mechanism's pending text in the same change.
- **N5.** Run `clock_refresh_applied` before `clock_fill_service`, matching
  the plant (`test/clock/test_clock.c:1543-1573`), so the dark-end note
  never resets a centre committed in the same pass.
- **N6.** Commit a centre only when `in->have_applied`, so a failing
  accept's apply does not spawn full applies outside the 1 s retry throttle.
- **N7.** Validate the plan before `clock_note_centre_commit`; an invalid
  centre neither counts nor stays in the tracker.
- **N8.** The CENTRE flag also clears on a pass with emission disallowed
  and nothing to write.
- **N9.** Carrier loss and quiesce clear `fill_open_boundary_pending` with
  the setpoint.
- **N10.** Device criterion compares `fill_trims` with `fill_window_seq`
  (all closes), not `fill_windows`; add a `centre_writes` counter (accepted
  CENTRE writes) so the one-write-per-window bound is checkable.
- **N11.** The accessor calls `aius_tx_fill_saddr_offset_frames` instead of
  re-implementing the arithmetic. Residual risk, recorded: the parent's
  `clock_fill_service`, `clock_fill_reset`, arm counter and note flag are
  copied into the plant, not called; a wiring divergence passes the host
  suite, so the device session and this audit carry it.
- **N12.** `trim_updates` now counts commits (including no-write and
  structural); gate 1's trim-cadence check (`aius_rate_bench.py:849-854`)
  moves to `centre_writes`. `firmware/tools/latency/linked_probe.py:457`
  was already stale; fix it in the same pass.
- **N4.** Correct the `tx_gate_execute_open` comment: the masked section
  runs inlined in `.flashmem.audio_enqueue` (already true of `set_level`);
  record its duration. Derived from the `teensy41-optical` disassembly,
  not measured: 7 Thumb-2 instructions per padded frame, about 3 to 5 us
  for a live open's worst 256 frames and 9 to 11 us for a replay's 768.
- **Residual risk, recorded (F2).** The bench tools infer writes from
  changes of `trim_applied_at_ms` / `fill_applied_at_ms` between polls, so
  two writes inside one poll interval count as one; the fill-cadence
  assertion therefore bounds from below. `centre_writes` is the exact
  count where a gate needs one.

Delivery review of the 4a build (2026-10-06, `reviewer` `opus`): FAIL on
the bench tooling, no firmware underrun path. Decided (autonomous):

- **R1, gate 7 `invalid-window-count`** compared `invalid_windows` with
  every write, but only a full apply raises it. Compare with full applies
  only. `invalid_windows` rises on every successful apply (nominal
  accept, fault retry, replay apply) and `structural_replans` only on a
  replan, so the bench asserts `invalid_windows` delta at least the
  `structural_replans` delta; equality is not expected.
- **R2, match windows.** A FILL or trim write is a carrier-continuous
  numerator step and must never excuse a waveform jump; only full applies
  (invalid windows) open match windows. Fill writes stay visible to the
  trim-observed check. This supersedes F2's "match against the stamps".
- **R3, F3 zeros.** A pending open with ring level of 256 or more trimmed
  to 128 and then padded 128 zeros, putting 2.67 ms of silence between
  kept audio and the first block. One `set_level` to the final open level;
  `gate_open_pending_padded_frames` and `gate_open_trimmed_frames` count
  only frames actually padded or dropped.
- **R4, N5 incomplete.** The end-of-pass `clock_refresh_applied` (kept for
  the replay apply) can still fire the dark-end note after a same-pass
  centre commit. The note must not overwrite a centre committed in the
  same pass; the plant models both refreshes.
- **R5, openapi text.** Pending setpoint, `fill_trims` and
  `centre_writes` descriptions to post-F3 and post-N12 truth; drop the
  unenforced one-per-window claim for `centre_writes` (a FILL and a CENTRE
  may share a window).
- **R6, fill cadence.** Assert `fill_trims` delta at most `fill_window_seq`
  delta (the one-FILL-per-window rule), not only `centre_writes`.
- **R7, plant ISR order.** The plant ran the late ISR after the block
  commit, so deleting F3's pad passed every host suite. Run it at the
  unmask, before the commit, so that deletion fails.
- **R8, latency tools.** `latency/bench.py` zero-delta also checks
  `fill_trims`; `linked_probe.py` edge mode no longer requires an
  `invalid_windows` step for a CENTRE write.
- **R9, N7 deferral.** An invalid pending centre is dropped at once, never
  deferred under saturation.

Closure re-review (same reviewer): PASS. The R3 straddle (boundary
crossed during the pad after a not-pending read, about 0.05 to 0.2% of
opens) leaves the setpoint correct to one frame; not an underrun path.
Folded before commit:

- **R10.** The latency bench no longer rejects a bracket on a
  `fill_trims` delta (a 20 mHz continuous step; FILL writes land in up to
  half the 4 s windows); it reports it.
- **R11.** openapi `trim_updates` text says it counts every centre commit
  since N12.
- **R12.** Fill cadence bound is `fill_window_seq` delta plus 1: a window
  closed just before the first poll can be written after it.
- **Residual risk, recorded.** The plant skips the R4 note only on
  `centre_committed`, the firmware on every `clock_note_centre_commit`;
  equivalent in every traced case, the same copy-not-call risk as N11.

Clean-room confirmation: PASS, no firmware underrun path; doc errors in
this plan, the gate 7 README and one code comment fixed. Decided:

- **R13, gate 1 trim-cadence advisory in code.** N12 moved the bound to
  `centre_writes`, which is near-vacuous: centre commits are bumpless, so
  an unsaturated commit writes nothing. Bounding commits instead would
  fail every healthy run (D4 (b): the estimate quantum makes at most two
  post-settle commits unmeetable). The check now records post-settle
  commits and `centre_writes` and never fails, completing D4 (b).
- **R14.** The stamp-derived write count falls back to `trim_updates`
  only when no sample carries the stamps, not when one sample lacks one.
- **R15.** `linked_probe.py` edge mode fires on a centre commit
  (`trim_updates` +1), not on `centre_writes`.
- **Coverage gap, recorded.** No host case drives a pre-filled ring
  through `tx_gate_execute_open`'s pending trim and pad; the order is
  checked by reading. The Phase 4 device session's
  `gate_open_pending_padded_frames` readings carry it.

### Phase 4b Status render must not delay AC-3 production

```yaml
phase: 4b
title: Keep the /api/status render off the AC-3 producer's critical path
status: planned
depends-on: phase 4 device session (does the per-session setpoint, worst-phase tolerance 128 frames, already absorb the render dip?)
prebuild-review: not-started
outcome: >-
  An /api/status render no longer delays the next AC-3 burst, so a poll
  cannot choose when a thin ring margin empties; the sub-window dip a render
  causes (71 frames worst, 2026-10-05) shrinks to the non-polled level.
unit: parent-only # design not started
seams:
  - app_write_status / http_status_json render scheduling (firmware/src/optical_main.cpp)
  - AC-3 producer service order in the main loop
targeted-proof:
  - device: polled versus unpolled sub-window spread p95 within 10 frames of each other
```

Found by the Phase 4 entry-gate design pass (2026-10-06): Phase 3 measured
the render delaying production, and Phase 4 only adds margin against it.
The 16 KiB status body makes the render longer (dip estimated 1.5 times).
First step after Phase 4's device session: if polled sessions show no
underrun and the dip margin holds, put to the owner whether 4b is still
wanted; otherwise write its design (chunked render, or render after the
producer's commit).

### Phase 4c `test.sh` runs every `--suite` given ✓ SHIPPED 2026-10-06

```yaml
phase: 4c
outcome: firmware/scripts/test.sh runs each --suite given, in order, and fails if any fails
seams: [firmware/scripts/test.sh argument parsing]
targeted-proof: test.sh with two suites, one forced to fail, exits non-zero and reports both
unit: component-behind-interface
```

Found during Phase 4a (2026-10-06): several `--suite` flags silently run
only the last, so a multi-suite proof command can report green while
skipping suites. Independent of the device session.

Shipped: two or more suites each run in their own process, a `==>
summary` lists `PASS` or `FAIL <s> (exit N)`, and the run exits 1 if any
failed; one suite behaves exactly as before. Proof: single clock (exit 0),
clock plus tx-gate (exit 0), unknown plus clock (exit 1, clock still ran),
registry green, shellcheck clean.

### Phase 4d CLAUDE.md promote for the fill counters and signing form

```yaml
phase: 4d
outcome: CLAUDE.md names the fill stamps, the write counters and the full signing form as Phase 4a's B1 states
seams: [CLAUDE.md, via the release-promotion step]
targeted-proof: the promoted diff carries exactly B1's lines; no foreign hunk is reverted
unit: parent-only
```

Blocked: the file carries another session's uncommitted edits. Re-check
`git -C git_modules/aius diff --stat -- CLAUDE.md` before acting, never
from memory. Content: Phase 4a §B1.

### Phase 5 Interrogate the user: confirm autonomous calls

```yaml
phase: 5
outcome: every call taken under the autonomous posture confirmed or overturned; overturned calls are new plan work
seams: [this plan's phase contracts]
targeted-proof: no entry below remains; each answer is recorded in the phase it settles
unit: parent-only
```

Executed once, at the end of the run or at handover. Each entry: phase,
call, alternative, cost of reversing.

- (none open; Phase 4a R2, R3 and R13 confirmed by the owner 2026-10-06)

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
(`firmware/tools/net/aius_rate_bench.py:852`) as an advisory metric, kept
docs-only per the D4 (b) framing. Phase 4a R13 (2026-10-06) made it
advisory in code: the check records post-settle centre commits and never
fails.

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
