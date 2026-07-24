---
title: Aius DSP pipeline, SD editor, and configuration API
status: in_progress
---

# Aius DSP pipeline, SD editor, and configuration API

## Execution handoff — 2026-07-24

This is the active, primary Aius plan. The network hardening work in
[`aius-network-hardening.md`](aius-network-hardening.md) is now a
finish-the-cutover-then-stop effort and secondary to this DSP work; its
receipt/reproducibility apparatus has been removed. The next session starts with
the signal path and uses the committed DSP samples as executable specifications
before extending the configuration API or control surface:

- `firmware/tools/dts_vectors/vectors/stereo_impulse.dts`;
- `firmware/tools/dts_vectors/vectors/surround51_impulse.dts`;
- `firmware/tools/dts_vectors/vectors/stereo_tones.dts`;
- `firmware/tools/dts_vectors/vectors/surround51_tones.dts`.

The critical-path order below supersedes the older phase numbering where they
conflict:

1. Extend the native DSP harness to decode each vector into the portable DSP
   interface and retain deterministic decoded-input and stereo-output
   known-answer fixtures.
2. Prove channel routing and signal math before UI work: stereo isolation;
   FL/FR passthrough; symmetric FC contribution; SL-to-L and SR-to-R placement;
   LFE gain; clip-free headroom; trim/EQ bypass equivalence; limiter threshold,
   lookahead, attack, and release; finite output; and bounded latency.
3. Implement the bounded signal path and coherent meter snapshot behind the
   portable DSP interface. The sample-vector tests, allocation guard, cycle
   budget, and queue/latency bounds are the completion gate.
4. Expose the proven model through `GET/PUT /api/dsp/config` and
   `GET /api/meters`, including transactional SD persistence and
   block-boundary apply.
5. Complete the SD-hosted control surface against captured responses from the
   same vector/config matrix. Test every control-to-JSON mapping, validation
   error, stale generation, pending/apply transition, meter, and waveform
   rendering before target smoke.

Do not resume the deferred network clean-room audits or production receipt
matrix from this plan. Hardware validation here is limited to the DSP sample
signal path, control surface, and their latency/audio-continuity gates.

UI routing is resolved: the canonical editor route is `/sd/www/index.html`,
served from the SD filesystem directory `firmware/sd/www/`. The firmware already
serves `/sd/www/` static assets — `optical_main.cpp` matches the `www/` resource
prefix and `firmware/sd/www/` already holds the replays surface. The separate
`/aius/config.json` feature-flag file is not the editor. There is one UI
surface; do not create a second.

## Objective

Expose an industry-standard, bounded audio pipeline with an editable model
served from the SD card. Keep the flash-resident page as a recovery/status
surface and serve the full editor from `/sd/www/index.html`.

## Current gap

`firmware/config/optical_dsp.json` is converted at build time into bounded
profile and mix tables by `scripts/build_config.py`. The firmware currently
returns only numeric `mode` and `mix` selections from `/api/status`; the
embedded page shows names but not EQ sections or resolved mix coefficients.
The SD `/aius/config.json` file is a separate feature-flag document and is not
the DSP schema. The current compiled model is read-only and has no limiter,
loudness meter, or runtime DSP persistence.

## Contract decisions

- Keep `GET /api/dsp/config` as the effective model read endpoint.
- Add authenticated `PUT /api/dsp/config` with a bounded JSON body. It validates
  the complete model, writes `/aius/dsp.json` transactionally, and applies it
  only at an audio block boundary. Invalid input never changes active DSP.
- The SD editor is static HTML/CSS/JavaScript under `firmware/sd/www/` (served at
  `/sd/www/`); it uses only the documented HTTP API and can be replaced without
  firmware flashing.
- The signal path is:
  `decode -> input channel trim/EQ -> downmix matrix -> output EQ -> true-peak
  safety limiter -> output`, with metering taps that do not alter audio.
- Input channel controls cover `FL`, `FR`, `FC`, `LFE`, `SL`, and `SR`. Each has
  a trim and up to two EQ sections. Output EQ remains a stereo post-mix
  profile. Mix controls cover centre, surround, LFE, and headroom coefficients.
- Limiter controls are enabled, threshold, attack, release, and fixed bounded
  lookahead. Loudness normalization is not automatic in the critical path;
  BS.1770-style loudness/true-peak telemetry is exposed first, with target gain
  changes applied only as slow, explicit configuration changes.
- Feature flags can independently bypass input EQ, output EQ, matrix shaping,
  limiter, and meters for latency/diagnostic comparisons. DTS framing and
  decoding cannot be bypassed while consuming DTS input.
- Status reports queue depth, estimated latency, DSP cycle counts, limiter gain,
  and peak telemetry alongside the existing counters.
- `GET /api/meters` returns a coherent fixed-size snapshot: peak/RMS per input
  channel, stereo output peak/RMS, limiter gain reduction, and up to 128
  decimated waveform/envelope points. The SD editor polls it at 10 Hz and
  renders meters plus a canvas waveform; it never requests full-rate samples.
- The version-2 editable model is bounded to four profiles, four mixes, six
  named input channels (`fl`, `fr`, `fc`, `lfe`, `sl`, `sr`), two EQ sections
  per input channel, and two stereo output EQ sections. Input trim is
  `gain_db` in `[-60,12]`; EQ frequency is `[10,24000]` Hz, Q is `[0.1,20]`,
  and EQ gain is `[-24,24]` dB. Limiter threshold is `[-24,0]` dBFS, attack
  `[0.1,100]` ms, release `[1,1000]` ms, and lookahead `[0,5]` ms. All values
  are finite.
- The effective model includes `source` (`compiled` or `sd`),
  `config_generation`, `features`, `profiles`, `mixes`, and `active`. A write
  includes `base_generation`; a mismatch returns 409 without touching SD or
  active DSP. `PUT /api/dsp/config` requires `X-Aius-SD-Auth`.
- The body is capped at 12,288 bytes; request and response buffers are 16,384
  bytes in OCRAM. At boot compiled configuration is active immediately. A
  valid `/aius/dsp.json` override is parsed outside the audio callback and
  becomes pending only after transactional SD commit; the snapshot swaps at
  the next audio block boundary and increments `config_generation`. Invalid
  JSON, SD removal, failed rename, or invalid ranges leave the last valid
  snapshot active.
- The v2 response/write shape is:
  - `schema_version: 2`, `source`, `config_generation`, `sample_rate_hz`
  - `features`: `input_eq`, `matrix`, `output_eq`, `limiter`, `meters`
  - `profiles[]`: `id`, `input_channels`, `output_eq[]`, `limiter`
  - `input_channels`: fixed `fl`, `fr`, `fc`, `lfe`, `sl`, `sr` objects, each
    with `gain_db` and `eq[]`
  - `output_eq[]`: the same bounded EQ section shape
  - `limiter`: `enabled`, `threshold_dbfs`, `attack_ms`, `release_ms`,
    `lookahead_ms`
  - `mixes[]`: `id`, `center_gain_linear`, `surround_gain_linear`,
    `lfe_gain_linear`, `headroom_linear`, and `clipfree`
  - `active`: `profile_id`, `profile_index`, `mix_id`, `mix_index`,
    `profile_pending`, and `mix_pending`
- `GET /api/meters` is a separate schema with `sequence`, `sample_rate_hz`,
  `input[]`, `output`, `limiter_gain_reduction_db`, `queue_frames`,
  `estimated_latency_us`, `dsp_cycles_last`, `dsp_cycles_max`, and
  `waveform[]` capped at 128 points. Meter values are diagnostic snapshots and
  never become configuration fields.
- The finite generator limits are four profiles, four mixes, two EQ sections
  per input/output table, 24 ASCII characters per ID, and unique IDs within
  each table. The model is capped at 12,288 JSON bytes below the 16,384-byte
  HTTP response capacity. The build and runtime validators reject over-limit
  counts, IDs with unsupported characters, duplicate IDs, out-of-range values,
  and non-finite numeric values.
- OpenAPI marks objects `additionalProperties: false`, arrays with their exact
  `maxItems`, numeric fields with units and finite-number expectations, and
  `schema_version` as the compatibility discriminator.
- `id` is the stable config name, not an array position. Indices are included
  only as diagnostic compatibility fields.
- `headroom_linear` is the resolved runtime value. `clipfree` indicates that
  the configured mix selected the DSP clip-free headroom policy; the response
  does not expose the build-time string sentinel. For the current default
  downmix coefficients, clip-free resolves to
  `1 / (1 + 0.7071067811865475 + 0.7071067811865475 + abs(lfe_gain_linear))`;
  standard and lfe-minus10 retain unity headroom.
- `center_gain_linear` is the shared FC-to-L/R coefficient. Increasing it
  raises dialogue/voice relative to FL/FR; zero removes the centre channel.
- `surround_gain_linear` is the shared SL-to-L and SR-to-R coefficient.
  Increasing it blends the rear channels more strongly while preserving their
  left/right placement; zero removes the surround contribution.
- The coefficients are symmetric by design. Independent SL/SR gains are not
  part of this model because they would change image balance rather than
  control surround blend.
- Mix coefficients are finite and bounded to `0 <= gain <= 4` linear
  (`-inf` is not a valid control value; the upper bound is approximately
  `+12 dB`). Configured non-clipfree headroom is bounded to `0 < h <= 1`.
- The effective model includes the compiled table or a valid SD override;
  `source` reports which is active and `config_generation` changes after a
  successful block-boundary apply.
- The built-in page adds a compact monospace DSP section: sample rate, active
  profile/mix, pending state, profile EQ rows, and mix rows. It fetches the
  endpoint independently of the status poll and remains usable without SD.
- OpenAPI is the canonical external model description. It includes schemas,
  field descriptions, enums, examples, and the new path. The device page
  links to the route and includes the field names so the recovery interface is
  self-describing even without the app dashboard.

## Phases

### Phase 1: contract artifact and adversarial review

**unit:** response model, bounded JSON shape, and OpenAPI schemas
**seams:** generated DSP tables ↔ model fields; model ↔ OpenAPI/page contract
**contract-artifact:** this plan, `swagger.json`, model example,
SD editor schema
**fan-out:** OpenAPI/test owner edits app contract; firmware owner consumes only the resolved field list
**review-intensity:** field naming, numeric units, enum drift, response-size bound, and future write/persistence confusion
**commit-checkpoint:** JSON validation, OpenAPI tests, and the RED review resolutions recorded before firmware implementation

Add the model schemas and example to OpenAPI, extend the app regression to
assert the route, required fields, bounds, enums, and example, and record the
12,288-byte model bound. The clean-room review found the initial contract
RED; these finite bounds, route requirements, and resolved headroom semantics
are the amendments required before Phase 2.

### Phase 2: bounded firmware endpoint

**unit:** read-only DSP model serializer and HTTP route
**seams:** `AIUS_OPTICAL_*` generated tables ↔ serializer; serializer ↔ HTTP response
**contract-artifact:** `GET /api/dsp/config` response and native route tests
**fan-out:** HTTP parser/tests own route recognition; optical firmware owns serialization and runtime selection fields
**review-intensity:** every supported EQ/mix variant, unknown enum guard, truncation, pending transitions, and no SD/flash calls
**commit-checkpoint:** native HTTP route/method tests, generator validation tests, target build, and response-size assertion green

Implement the route and serializer using fixed buffers and `snprintf`-style
capacity checks. Serialize the generated table rather than duplicating values
in firmware code. Return a deterministic error if the model cannot fit the
response budget, and add a build-time/native assertion covering the configured
maximum table dimensions.

### Phase 3: bounded DSP pipeline and latency instrumentation

**unit:** channel trim/EQ, matrix, output EQ, metering, limiter, and latency
snapshot
**seams:** decoded planes ↔ input processing; matrix ↔ output processing;
audio block boundary ↔ coherent telemetry
**contract-artifact:** DSP config model, feature flags, status/meter fields,
native known-answer tests
**fan-out:** portable DSP module; optical adapter; latency test harness
**review-intensity:** allocation freedom, limiter stability, bypass equivalence,
cycle budget, queue growth, and reset-safe config apply
**commit-checkpoint:** native DSP tests, target build, and cycle/latency report

Meter snapshots use a fixed ring in OCRAM, a sequence number, and a coherent
copy boundary. The audio callback only updates scalar accumulators/ring slots;
HTTP copies the last complete snapshot outside the critical path.

### Phase 4: SD editor and runtime persistence

**unit:** self-hosted editor and authenticated DSP JSON update path
**seams:** browser controls ↔ JSON model; HTTP body ↔ SD transactional write;
SD model ↔ block-boundary DSP snapshot
**contract-artifact:** `firmware/sd/www/index.html`, editor schema, PUT examples
**fan-out:** static UI; HTTP/config loader; firmware application seam
**review-intensity:** malformed JSON, bounded body, invalid ranges, stale
generation, SD removal, and editor recovery
**commit-checkpoint:** mocked API/editor tests, target asset smoke, and config
round-trip without flash writes

### Phase 5: embedded model view and target validation

**unit:** built-in DSP configuration panel and live endpoint smoke/soak checks  
**seams:** page fetch ↔ model rendering; endpoint polling ↔ audio critical path  
**contract-artifact:** embedded page field list, OpenAPI example, soak evidence  
**fan-out:** embedded HTML/JS owner edits `optical_main.cpp`; validation owner runs target smoke and soak  
**review-intensity:** absent SD, malformed feature config, pending changes, large model, and concurrent endpoint polling during soak  
**commit-checkpoint:** page smoke, OpenAPI regression, target response comparison, and soak with zero audio/HTTP-service overruns

Render the effective model and status labels in the flash recovery page, link
to the SD editor when present, and validate that endpoint polling and editor
updates continue to work with the HTTP service active during audio soak.

## Test scope and evidence

There is no `Aius.Regress` project or hardware requirements registry in this
repository. The authoritative regression scope is the existing
`firmware/scripts/test.sh --environment native --suite http`, the generated
config validation test added to that runner, the Vitest OpenAPI contract test,
the target PlatformIO build, and live target smoke/soak evidence. The embedded
page contract will also have a Playwright fixture regression that loads a
captured model response and asserts the field-to-DOM mapping; target-only
network checks remain a separate hardware gate.

## Acceptance gates

- All four committed DTS vectors drive decoder-to-stereo known-answer tests for
  routing, coefficients, EQ, limiter behavior, finite output, and latency.
- `GET /api/dsp/config` returns the complete effective profile/EQ/mix model
  with stable IDs and resolved units.
- Authenticated `PUT /api/dsp/config` validates and commits atomically; invalid
  or stale writes leave SD and the active block-boundary snapshot unchanged.
- `GET /api/meters` returns coherent bounded snapshots without audio-thread
  allocation or unbounded sample transfer.
- OpenAPI schemas and examples describe every returned field and enum.
- The SD editor maps every control to the documented JSON model and renders
  meters/waveform data from captured vector-driven responses.
- The embedded recovery page reports the same active profile, EQ sections, mix
  coefficients, and pending state without depending on SD.
- The model stays within the request/response capacity and DSP cycle/latency
  budgets without disrupting audio processing.
- Existing `/api/status`, mode/mix controls, fallback page, and SD feature
  configuration continue to work unchanged.
