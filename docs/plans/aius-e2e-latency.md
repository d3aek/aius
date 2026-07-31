---
title: Aius end-to-end audio latency
type: plans
scope: component
component: aius
status: in-progress
subtype: implementation
tags: [aius, firmware, audio, latency, spdif, cm6206]
updated: 2026-07-31
toc: true
---

# Aius end-to-end audio latency

Measure causal audio latency through the physical CM6206 → Teensy → CM6206
optical loop for:

- PCM with the processing stages bypassed;
- PCM with the production DSP configuration;
- DTS Core decode plus the production DSP; and
- AC-3 decode plus the production DSP.

The result reports both the raw host round trip and the direct-loop-corrected
Teensy contribution. `/api/measure` is not this measurement: it captures the
post-DSP committed-wire plane and therefore cannot include optical input,
classifier/decoder scheduling, the transmit ring, S/PDIF output, or the
CM6206 capture path.

## Measurement boundaries

Four quantities stay separate:

| Quantity | Start | End |
|---|---|---|
| raw PCM loop | PCM playback-device frame index | correlated decoded/DSP output frame returned by CM6206 capture |
| raw encoded loop | IEC 61937 `Pa` playback-device frame index | correlated decoded/DSP output frame returned by CM6206 capture |
| adapter PCM baseline | PCM playback-device frame index | correlated PCM frame in a direct optical capture |
| adapter encoded baseline | IEC 61937 `Pa` playback-device frame index | the same parsed `Pa` in a direct optical capture |
| Teensy latency | raw per-occurrence latency minus the matching adapter transport estimate | same causal output |
| DSP incremental cost | corrected PCM-production occurrences minus corrected PCM-bypass occurrences | identical stereo programme and block geometry |

For PCM, the causal input is reference frame zero of a uniquely coded probe.
For DTS and AC-3, it is the first IEC 61937 `Pa` word of the uniquely coded
access unit. The decoder result reports:

- first-causal-output latency; and
- access-unit-tail latency, the time from `Pa` until the last output frame
  belonging to that unit reaches the capture boundary.

Here, "first" and "tail" are operational decoder boundaries, not amplitude
thresholds. The native reference renderer records the continuous output indices
corresponding to decoded frame 0 and decoded frame N-1 of each IEC access unit
after the exact DSP state machine. Correlation locates a coded interior span and
projects those recorded indices into capture time. Consecutive units remain in
one continuous render, so codec priming, overlap, limiter lookahead and DSP state
are not reset or guessed per unit. The AC-3 tail figure is not substituted for
first output. One AC-3 access unit has 1,536 frames, so conflating the two hides
up to 31.98 ms at 48 kHz. The selected DTS Core type-I fixture has 512 frames.

The direct-loop capture never pretends that encoded IEC is decoded PCM. Its IEC
parser matches each playback `Pa` and payload identity to the captured `Pa`;
that supplies only the CM6206 playback/capture transport term. PCM uses the
matching direct-loop PCM probe. For each topology and carrier kind, a robust
affine fit yields transport offset, ratio, residual distribution and bootstrap
95% confidence interval. Every Teensy occurrence subtracts the baseline median
transport estimate before the corrected min/median/p95/max are calculated.
Independent quantiles are never subtracted; baseline uncertainty is reported
beside the corrected distribution.

## Preliminary fixed-wiring evidence, 2026-07-30

The exact sample-rate image `01477755955b+teensy41-optical` was exercised with
the existing CM6206 → Teensy → CM6206 optical wiring. No physical rewire and no
persistent DSP mutation were available. A linked `S16_LE`, 48 kHz stereo ALSA
probe used 256-frame periods, a 4,096-frame buffer and 24 deterministic
occurrences per run.

This is provisional raw-loop evidence, not Phase 1 or Phase 2 acceptance. The
exploratory Python renderer does not yet implement the shared native
parser/decoder/pipeline/DSP reference, the full ambiguity/clipping/guard gates,
the PCM bypass row or a matching direct-loop baseline. Its accepted
distributions exclude any run whose measurement-window status bracket changed
a harmful audio or clock counter.

| Production path | Process / counter-clean runs | Counter-clean occurrences | Raw median | Raw p95 | Raw range |
|---|---:|---:|---:|---:|---:|
| PCM | 8 / 6 | 144 | 17.740 ms | 18.010 ms | 17.208–18.042 ms |
| AC-3, `Pa` to first output | 8 / 6 | 144 | 17.438 ms | 18.792 ms | 16.833–19.062 ms |
| AC-3, `Pa` to AU tail | 8 / 6 | 144 | 49.417 ms | 50.771 ms | 48.812–51.042 ms |

Every counter-clean run had an affine ratio of 1 within floating error, effectively
zero within-run residual jitter, PCM correlation 1.0 and AC-3 correlation at
least 0.9999768. Two runs in each row crossed one clock trim/invalid window and
were excluded. The AC-3 first-output median is 0.302 ms below PCM, so this
fixture shows no material codec-path onset penalty. AU-tail latency remains a
separate decoder buffering boundary and is not added to steady-state
presentation delay.

The raw figures include both CM6206 directions, USB transport and host start
phase. They cannot yet support a Teensy-only lip-sync budget. Counter-clean AC-3 runs
also split by the initial Teensy transmit-queue snapshot: four 640-frame runs
had an 827-frame latency median, while two 768-frame runs had an 889-frame
median. That small sample suggests queue phase contributes to run-to-run
spread, but it is not a correction model.

Separate 51.456-second sparse-code diagnostics deliberately crossed the clock
correction windows rather than excluding them. Across a 47.104-second measured
span, all 24 PCM occurrences remained exactly 821 frames / 17.104 ms while five
trim/invalid windows advanced; all 24 AC-3 occurrences remained exactly 831
frames / 17.3125 ms while four advanced. Both affine ratios remained 1 within
floating error, with no other harmful counter delta. At this cadence the trim
mechanism therefore caused no measurable latency walk, step or codec-path
jitter. AC-3 onset was only 10 frames / 0.208 ms behind PCM.

Those long diagnostics remain non-acceptance rows because their status brackets
intentionally include invalid windows. They also quantified incomplete
post-start clock convergence: output-versus-carrier error ranged from -3,901
millihertz to +5 millihertz at the four measurement endpoints, so not every
snapshot met the eventual 1,000-millihertz follow gate even though physical
capture timing stayed exact over the measured span.

After a 14-second in-stream settle, one AC-3 measurement window met the follow
gate at -603 millihertz in both snapshots, advanced no harmful counter and held
all 24 codes at 890 frames / 18.542 ms. PCM repeated with the expanded harmful
counter set at zero and every code fixed within each run, but two stable
snapshots plateaued at -1,080 and -1,097 millihertz. They miss the eventual
follow gate by only 80–97 millihertz, so they remain provisional despite raw
latencies of 18.042 and 18.625 ms. Repeating the same settled row did not turn
that small PCM offset into latency drift; it should be handled with the
characterized sample-rate-following symptom rather than hidden in the latency
distribution.

Matched 600.8-second production-path soaks then extended the measured code span
to 560.832 seconds:

- PCM held all 24 occurrences at exactly **842 frames / 17.542 ms** on both
  channels. The affine ratio was 1 within floating error, residual was below
  `1.2e-8` frame, channel skew and unequal-stereo frames were zero across the
  complete 28.8-million-frame capture, and correlation was 1.0. It crossed 54
  trim/invalid windows; every other harmful counter delta was zero. The
  initial/final output-minus-carrier errors were -871 and -65 millihertz, both
  inside the eventual follow gate.
- AC-3 held all 24 occurrences at exactly **803 frames / 16.729 ms** on both
  channels and every projected AU tail at **2,338 frames / 48.708 ms**.
  The affine ratio was 1 within floating error, residual was below `1.5e-8`
  frame, channel skew was zero and minimum per-channel correlation was
  0.999956. It crossed 55 trim/invalid windows; every other harmful counter
  delta, including all parser/decoder/resync counters, was zero. Its
  initial/final output-minus-carrier errors were +51 and -1,015 millihertz,
  with the latter missing the eventual follow gate by 15 millihertz.

These remain diagnostic rather than formal acceptance because clock
trim/invalid counters advanced and no direct-loop subtraction exists. They are
nevertheless the strongest current lip-sync result: neither codec path showed
one frame of latency walk, step, drift or L/R skew over more than nine minutes.
The 39-frame difference between their fixed offsets is not attributed to codec
cost because the runs began at different transmit-queue phases.

The later
[CM6206 playback-corruption characterization](../reference/cm6206-playback-corruption.md)
localizes the known impulses and L/R word-phase slips to the adapter playback
path, upstream of S/PDIF serialization. That mechanism does not explain a
repeatable 800–900-frame offset, but it can contaminate individual waveform
codes or encoded payloads. Reanalysis therefore keeps left and right
correlations separate. The 51.456-second PCM capture had zero unequal stereo
frames and both channels located every code at exactly 821 frames. The AC-3
capture, whose decoded channels are not sample-identical after independent
codec quantization, still located both channels of every code at exactly 831
frames with zero skew and advanced no parse, decode or resync counter. The
provisional timing conclusions are therefore not carrying an observed CM6206
impulse or word-phase slip, while no bit-perfect waveform claim is made.

Formal rows must retain this quarantine boundary. PCM associations and
direct-loop baselines are analyzed per channel and reject any phase-slip
interval. Encoded rows use the Teensy PSRAM raw-ingress record to distinguish a
payload fault from a decoder fault; only a single word matching the exact
high-byte-held/low-byte-intact fingerprint may be quarantined. Any low-byte
damage, multi-frame run, both-channel event, unmatched payload damage or
pipeline reject remains a hard failure. A replacement source discriminator is
still required before waveform-critical acceptance.

The PCM bypass row remains blocked by restoration safety. The runtime reports
an SD-backed DSP model, but the current `/sd/` download route admits only the
`/aius/www/` subtree: attempts to retain the exact `/aius/dsp.json` bytes
return `sd_path`. The persistent model must not be overwritten until exact
byte restoration is possible. DTS remains deferred behind the user-prioritized
PCM and AC-3 work.

## Clock and synchronization model

One duplex ALSA helper opens `hw:ICUSBAUDIO7D,0` for playback and capture and
requests exact `S16_LE`, two channels and 48 kHz. It configures explicit hardware
period/buffer sizes, capture `avail_min`, and a playback start threshold that
prevents implicit start during prefill. It links the two handles, verifies the
link, prepares both, prefills the stopped playback buffer, arms the capture
reader, and calls `snd_pcm_start()` once on the linked capture group. Playback
device frame zero is the first prefilled frame at that verified group trigger;
host `writei` acceptance is not called the physical start. The reader drains
capture concurrently from the trigger until the final guard has returned.

The manifest records negotiated hardware/software parameters, prefill frames,
ALSA status trigger timestamps, `snd_pcm_delay` samples and device positions at
start/end. A failed link, non-identical trigger, implicit playback start, wrong
exact format/rate/channel count, short transfer, XRUN or recovery is a hard
failure rather than an unsynchronized fallback.

The helper has no compile-time ALSA-header dependency. It
`dlopen("libasound.so.2")`, checks every required `snd_pcm_*` symbol through a
checked-in minimal ABI declaration boundary, and fails with the missing symbol
name. Installing another ALSA package is not a prerequisite:
`alsa-utils` 1.2.9, `libasound.so.2`, `aplay`, `arecord`, `amixer`, `alsabat`
and its `--roundtriplatency` mode are already present.

Playback and capture clocks are fitted rather than assumed equal:

```text
capture_index = offset + ratio × playback_index
```

At least 20 accepted codes spanning the capture are required. A robust initial
fit rejects no points; only a predeclared residual bound may invalidate the
whole run. Least-squares fit then provides `ratio`; each code residual provides
latency and jitter. The allowed playback/capture clock ratio is
`0.9995..1.0005`; values outside it fail rather than being fitted away.

A periodic tone and one repeated aperiodic probe are both inadmissible. Exactly
24 codes are emitted. Code `i` is a distinct deterministic bipolar sequence
derived from the recorded SHA-256 seed and index and begins with a 127-frame
index-bearing sync prefix. Geometry is mode-specific:

| Mode | Active code | Unit association | Quiet guard |
|---|---:|---|---:|
| PCM | 2,048 frames | one PCM occurrence | 4,096 frames on each side |
| DTS type I | 384 frames | wholly inside one 512-frame `Pc=11` decoded AU | two complete quiet AUs on each side |
| AC-3 | 1,024 frames | wholly inside one 1,536-frame `Pc=1` decoded AU | two complete quiet AUs on each side |

No code crosses an AU boundary, so every encoded timing oracle retains its own
`Pa`→frame-zero/frame-N-minus-one mapping. General-validator tests cover
`Pc=12/13`; the physical DTS row is type I / `Pc=11`.

Source peak is at most -12 dBFS. After capturing the production config and
before encoding, deterministic gain selection must make the native rendered
true peak at least 6.0 dB below its enabled limiter threshold. With the limiter
disabled, rendered true peak must be at most -6 dBFS. If that gain cannot also
meet the 30 dB signal-to-guard gate, the config is rejected before collection;
the limiter is never exercised and called a latency-neutral probe.

Associations must be monotonic and one-to-one. Normalized correlation must be
at least 0.35 and its absolute peak at least twice the next candidate after
excluding a ±64-frame main lobe. Signal-to-guard RMS must be at least 30 dB;
the guard RMS floor is one 16-bit LSB. Clipping means any captured sample equal
to -32768 or 32767 and is forbidden. Maximum absolute affine residual is 128
frames; no residual is silently rejected. At least 20 of 24 codes must remain
after excluding a code whose mode-specific active span overlaps a declared
clock invalid interval; fewer fails and the run is repeated. These constants are
serialized in the manifest before collection. Unit tests pin equality and
one-step-failing cases for every numeric threshold, plus missing, duplicate,
swapped and false peaks.

## Stimulus and references

The runner creates all session artifacts under the caller-selected evidence
directory and records SHA-256 hashes.

- PCM uses 24 deterministic broadband codes below the captured production
  limiter threshold. Each occurrence has a distinct SHA-256 counter-derived
  sequence and index-bearing sync prefix.
- DTS and AC-3 use the same six-channel programme and per-access-unit unique
  codes. The recorded generator seed and source WAV fully determine it.
- Encoding uses pinned, recorded `ffmpeg` arguments. DTS uses
  `-c:a dca -strict -2`; AC-3 uses `-c:a ac3`. Both fix rate, bitrate, channel
  layout, sample format and metadata. `ffmpeg -version`, command argv, stdout
  and stderr are retained.
- The existing replay-sample validator is reused only for its supported AC-3
  contract (`Pc=1`, 6,144-byte period, fixed fixture shape). A new general IEC
  validator uses the landed public IEC parser as the common authority for
  AC-3 and DTS, and records byte order, data type (`Pc=11` for the selected
  physical DTS fixture and `Pc=1` for AC-3), period words, payload bit count and
  every `Pa` frame index. The native decoder supplies decoded frame count and
  channel order. Tests cover variable DTS payloads/periods including
  `Pc=12/13`, and cross-check the AC-3 result against the old validator. The
  latency generator explicitly supersedes only the old repeated-tone stimulus.
- Each carrier contains at least one second of valid warm-up and continuous
  consecutive access units before the first measured unit. Encoder delay and
  padding are measured from the emitted elementary stream and recorded; no
  source-WAV index is assumed to be an IEC access-unit boundary.

A native reference renderer consumes the exact carrier through the landed
public IEC parser, AC-3/DTS decoder, pipeline and DSP interfaces. Phase 1 first
extracts the private production model-to-runtime DSP resolution from
`optical_main.cpp` into one public `aius_dsp_runtime_config` adapter used by
both firmware and renderer; a tool-local copy is forbidden. The adapter owns
feature bypass, input gain/EQ, matrix/LFE, output EQ, limiter and meter
resolution while the hot process interface remains in `aius_dsp`. Differential
tests perturb every model field and feature flag and require the firmware and
native renderer to resolve identical coefficients, state-reset decisions and
output for the checked-in production fixture.

The renderer loads the exact captured effective DSP model through the
production config parser, normalizing only the wire-only `source` and
concurrency fields while hashing both original and normalized JSON. It keeps one
decoder and DSP instance across warm-up and all measured units, writes the
continuous stereo reference, and logs for every parsed IEC unit:

- carrier `Pa` index, `Pc`, payload bits and elementary-stream hash;
- decoder call/status, output frame count and channel map;
- continuous pre-DSP frame range for that unit; and
- continuous post-DSP reference indices for its frame-zero and frame-N-minus-one
  timing oracles.

The renderer output supplies correlation shape and the deterministic mapping
from a measured IEC unit to output-frame boundaries. It is not a wall-clock
timing measurement; only physical playback/capture indices determine latency.
The original programme, encoded elementary stream, IEC carrier, native decoded
planes, rendered DSP reference and physical capture are separate hashed
artifacts.

## Configurations

Every run captures:

- firmware `build_revision` and `build_utc`;
- complete `GET /api/dsp/config` response and SHA-256;
- complete initial/final `GET /api/status`;
- ALSA card/device, stream formats, rate, period and buffer geometry;
- mixer controls, especially `PCM Capture Source=IEC958 In`;
- stimulus, carrier, reference and capture hashes; and
- tool versions and UTC timestamps.

The PCM-bypass configuration disables input EQ, matrix customization, output
EQ, limiter and dither-affecting width changes while retaining the normal
stereo unity route. PCM production uses the identical stereo programme and block
geometry with the exact production config. DTS/AC-3 use the same six-channel
programme through the same production matrix and DSP. Their corrected results
are combined decoder-plus-multichannel-pipeline end-to-end latency. No physical
six-channel PCM row exists, so neither a decoder-only nor an encoded incremental
subtraction is reported.

The authoritative production config is the exact effective model returned by
the board at the start of the session. A production row is inadmissible if that
GET is invalid or cannot be rendered natively. Before any PUT, the runner must:

1. authenticate and download `/sd/aius/dsp.json`, retaining its original bytes,
   status and hash as evidence;
2. require that the file exists, parses, and is the active `source:"sd"` model;
3. locally parse a fresh-token restoration body without issuing a mutating dry
   run; and
4. install signal/exception cleanup that attempts that exact restore on success,
   failure and interruption, then verifies the restored semantic model after
   canonical serialization, effective config hash and active source after
   block-boundary settle/reload.

The public API cannot delete an absent DSP override. Therefore an initial 404,
`source:"default"`, invalid file or mismatch aborts before mutation and requires
an explicit preparation decision; it is never "restored" by inventing a default
PUT. `PUT /api/dsp/config` necessarily writes a new request with the current
`base_generation`, so original raw-byte identity is not promised after a
generation advance. The bypass PUT uses the captured generation, waits for
block-boundary application, and brackets measurement only after status/config
settle. The production model is semantically restored with a fresh token and
re-verified before the encoded rows.

## Acceptance

All topology rows require exact ALSA parameters and verified linked start, no
capture/playback XRUN or recovery, no clipping, adequate probe SNR, one
unambiguous monotonic association per expected code, at least 20 valid codes,
the bounded finite affine ratio, and all raw evidence retained on failure.

Direct-loop rows have no Teensy status gate. PCM direct-loop fits playback code
indices to captured code indices. Each encoded direct-loop row parses and
one-to-one matches playback and captured `(Pa, Pc, payload_bits, payload_hash)`
tuples. It reports transport min/median/p95/max, affine residuals and bootstrap
95% confidence interval.

Every Teensy row additionally requires:

- exact committed latency-harness Phase 1 `build_revision` and captured
  `build_utc`, identical in both status snapshots; the accepted
  sample-rate-following evidence commit must be a proved Git ancestor of that
  revision;
- unchanged `boot_generation`, `reset_source` and `watchdog_early_resets`;
- `carrier == 1`, `clock.carrier_locked == true`,
  `clock.carrier_ready == true`, `clock.nominal_rate_hz == 48000`,
  `clock.stream_rate_hz == 48000`, `clock.master == "input"`,
  `clock.state == "tracking"`, and absolute output/carrier difference no greater
  than 1,000 millihertz;
- PCM: `pipeline_diag.class == 1`, `pipeline_diag.pcm_confirmed == 1`; encoded:
  `pipeline_diag.class == 2` and `pipeline_diag.codec_pc == 1` for AC-3 or
  exactly 11 for the selected DTS type-I row;
- positive progress during the bracket: `audio_diag.input_words_consumed`,
  `audio_diag.service_steps_committed`,
  `audio_diag.committed_output_blocks` and `audio_diag.decode_units` for
  encoded input, plus `pipeline_diag.bursts_ok` for encoded input;
- unsigned 32-bit zero deltas, with a lower final value accepted only as a
  single proved wrap, for `overruns`, `event_overflows`,
  `audio_diag.ingress_dropped_chunks`, `audio_diag.ingress_dropped_words`,
  `audio_diag.source_changes`, `audio_diag.source_flushes`,
  `audio_diag.gap_flushes`, `audio_diag.service_generation_resets`,
  `audio_diag.service_requested_resets`,
  `audio_diag.service_budget_overruns`,
  `audio_diag.runtime_epoch_budget_overruns`,
  `audio_diag.transmit_stops`, `audio_diag.transmit_stalls`,
  `audio_diag.parse_rejects`, `audio_diag.decode_rejects`,
  `audio_diag.output_drops`, `audio_diag.au_abandons`,
  `clock.tx_underrun_frames`, `clock.rate_changes`,
  `clock.structural_replans`, `clock.clock_faults`,
  `clock.mismatch_blocks`, `clock.unsupported_blocks`,
  `clock.unsupported_episodes`, `pipeline_diag.decode_rejects`,
  `pipeline_diag.rejects_no_space`, `pipeline_diag.rejects_malformed`,
  `pipeline_diag.rejects_unsupported` and `pipeline_diag.resyncs`; and
- unchanged zero `audio_diag.service_last_failure`,
  `audio_diag.pipeline_last_failure`, `clock.last_clock_fault` and
  `pipeline_diag.last_status`;
- zero values and zero deltas for `pipeline_diag.alloc_count`,
  `pipeline_diag.dts_alloc_count` and `pipeline_diag.ac3_alloc_count`; and
- exact zero deltas for `clock.trim_updates` and `clock.invalid_windows`. The
  runner settles before its short bracket and restarts the whole row if either
  advances; HTTP counters cannot locate the exact 512-frame validity interval
  inside a CM6206 S16 capture, so overlapping codes are never guessed away.

The runner captures status only after configuration settle and carrier warm-up,
then brackets the measurement with initial/final snapshots. It reports raw and
adapter-corrected latency min/median/p95/max plus baseline uncertainty.

No universal numeric latency ceiling is invented before the first honest run.
The first session establishes the baseline and compares it with the currently
derived contributors: 96 limiter-lookahead frames, roughly 512 steady-state
transmit-ring frames, 512 DTS frames and 1,536 AC-3 frames. A later product
limit can be selected from those measured distributions.

## Delivery

### Preliminary fixed-wiring probe

```yaml
phase: preliminary
status: complete
unit: parent-only
delivered:
  - linked ALSA S16_LE physical collector
  - deterministic PCM and AC-3 exploratory generators
  - counter-clean aggregation, per-channel skew and queue-phase context
  - provisional PCM and AC-3 raw-loop distributions
  - matched 600.8-second PCM and AC-3 drift soaks
deferred-to-phase-1:
  - native production reference renderer
  - full manifest and signal-quality acceptance contract
deferred-to-phase-2:
  - direct-loop baselines
  - PCM bypass and DTS physical rows
```

This boundary provides an early lip-sync risk measurement under the only
available fixed wiring. It does not satisfy either numbered phase and must not
be used to subtract an adapter baseline or claim Teensy-only latency.

### Phase 1: synchronized harness, native renderer and analyzer

```yaml
phase: 1
status: implementation-complete-device-smoke-pending
unit: component-behind-interface
seams:
  - linked ALSA duplex capture
  - shared production DSP runtime-config adapter
  - native decoder/DSP reference renderer
  - affine probe analyzer
  - evidence manifest
completed-slices:
  - shared production DSP runtime-config adapter
  - native exact-carrier parser/decoder/DSP renderer core and runner switchover
  - native renderer per-unit payload hashing and evidence completion
  - formal probe analyzer and evidence manifest
  - synchronized collector acceptance hardening
remaining-slices:
  - post-refactor 120-second 48 kHz optical device smoke
owned-paths:
  - firmware/lib/aius_dsp_runtime_config/**
  - firmware/src/optical_main.cpp
  - firmware/tools/latency/**
  - firmware/test/latency-runner/**
  - firmware/test/dsp/**
  - firmware/test/optical-cutover/**
  - firmware/scripts/test.sh
  - firmware/README.md
commit-checkpoint:
  green: [latency-runner, dsp, optical-cutover, registry, memory, itcm-placement]
  buildable: [teensy41-optical]
  parent-commit: "feat(aius): add optical latency bench"
```

The shared runtime-config slice is delivered: the generated factory default and
runtime parser now expose one canonical `AiusDspModel`; firmware resolves it
through `aius_dsp_runtime_config`, and the adapter owns all five feature bypass
flags, accepted-rate coefficient design, state resets and meter cadence.
Native differential tests perturb every field category, assert reset/bind
semantics, and run two retained DSP consumers across consecutive blocks with
byte-identical output. The Teensy target builds with zero memory delta. This
does not close Phase 1 until the exact committed image clears the 120-second
device smoke.

The native renderer core is delivered. It scans the exact carrier with the
public IEC parser, then retains one production pipeline, resident decoder and
shared DSP runtime across the whole carrier. It records each unit's actual
byte order, `Pc`, `Pd`, period, decoder result and continuous pre/post-DSP timing
oracles. The live PCM/AC-3 runner now hashes the board's exact effective config
response and its concurrency-normalized parser input, uses the native stereo
reference for correlation, and maps every AC-3 code back through the renderer's
unit oracle. FFmpeg remains the pinned encoder/wrapper but is no longer the
decode authority.

The remaining host implementation is delivered. The versioned preflight
serializes exact mode geometry and every numeric threshold before collection.
The native evidence retains normalized per-unit elementary hashes, continuous
decoded multichannel PCM and rendered stereo separately. The analyzer requires
24 unique index-bearing probes unless explicit invalid-window exclusions leave
at least 20; it fails on ambiguous second peaks, less than 30 dB guard SNR,
clipping, non-monotonic association, clock ratio outside `0.9995..1.0005` or
more than 128 frames of robust/least-squares residual. The linked collector
requires exact format/rate/period/buffer and start thresholds, monotonic ALSA
status timestamps with one nonzero trigger for both streams, zero XRUN
recovery/short transfer/overrange, the exact harness build, the accepted
sample-rate ancestor, continuous mode-specific progress and zero harmful
counter deltas. Failed collection or analysis leaves a rejected manifest
rather than losing the raw evidence.

Native tests cover known integer and fractional lags, affine clock error,
periodic ambiguity, silence, clipping, missing/swapped/duplicate/false codes,
encoded `Pa` matching, AU frame-zero/tail projection, XRUN evidence, counter
wrap/reset semantics, config normalization/build identity and malformed
manifests. Fixture proof runs the same public parser/decoder/pipeline/DSP
renderer for DTS and AC-3.
`alsabat --roundtriplatency` is a physical cross-check, not the evidence
authority.

Because extracting the shared DSP adapter changes the firmware composition, the
Phase 1 image receives a post-refactor 120-second 48 kHz optical smoke before
the latency matrix. It must retain the accepted sample-rate commit as an
ancestor, meet the 1,000 millihertz follow bound, make continuous PCM progress,
and show zero harmful clock/audio deltas. This smoke proves the new exact
latency-harness identity without pretending it is byte-identical to the earlier
sample-rate image.

The 2026-07-31 implementation boundary cannot run that smoke: the host
currently reports no cards in `/proc/asound/cards` and no `/dev/snd` entries,
while the Teensy remains reachable at its HTTP endpoint. No replug is permitted
during the remote window. This records only an unavailable ALSA path; it does
not attribute a new adapter, controller or extender fault.

### Phase 2: physical matrix and durable result

```yaml
phase: 2
status: pending
unit: parent-only
depends-on: [phase-1, sample-rate-following-device-acceptance]
matrix:
  - direct-loop-pcm
  - direct-loop-dts-carrier
  - direct-loop-ac3-carrier
  - pcm-bypass
  - pcm-production
  - dts-production
  - ac3-production
commit-checkpoint:
  evidence: [raw-captures, manifests, latency-summary]
  parent-commit: "docs(aius): record optical latency"
```

The three direct-loop calibrations share one temporary fibre rewire and run as
one uninterrupted topology slice. The four Teensy rows use the normal full loop.
Each physical topology is confirmed before collection and left unchanged until
its slice finishes.

## Outstanding-work audit

- `docs/plans/aius-ac3-support.md` — `extend`: Add physical first-output and AU-tail latency to the completed decoder evidence while its deferred four-hour gate remains pending OTA work.
- `docs/plans/aius-dsp-config-api.md` — `extend`: Measure the bypass and production feature combinations for which that plan exposed comparison flags.
- `docs/plans/aius-eac3-support.md` — `unrelated`: The physical matrix covers PCM, AC-3 and DTS Core, not the separate E-AC-3 decoder work.
- `docs/plans/aius-network-hardening.md` — `coordinate`: Reuse its exact-image, counter-delta and committed-wire evidence conventions without changing network behavior.
- `docs/plans/aius-ota-upgrade.md` — `coordinate`: Preserve the deferred AC-3 four-hour gate until OTA is investigated; latency neither closes nor supersedes it.
- `docs/plans/aius-sample-rate-following.md` — `coordinate`: Use only the exact image after all sample-rate device gates pass and consume its clock/status evidence.
- `docs/plans/aius-sd-config-web.md` — `coordinate`: Obey its persistent DSP-file semantics and refuse mutation when exact restoration is impossible.
- `docs/plans/aius-tls-termination.md` — `unrelated`: Latency reads existing device status over the current LAN transport and does not change TLS termination.
