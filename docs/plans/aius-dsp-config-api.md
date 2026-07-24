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

1. **Done.** The native `dsp-vectors` suite decodes each of the four committed
   vectors through the portable DTS interface, drives the decoded PCM through the
   public DSP interface (`aius_dsp_downmix` / `aius_dsp_process`), and checks
   deterministic decoded-input and stereo-output golden fixtures
   (`tools/dts_vectors/golden/*.golden`). The sample corpus now runs through the
   pipeline.
2. **Mostly done.** The `dsp-vectors` suite proves, through the public interface
   on real decoded vector content: stereo isolation; FL/FR passthrough; symmetric
   FC contribution; SL-to-L and SR-to-R placement; LFE gain (default-excluded and
   −10 dB fold); clip-free headroom; trim/EQ bypass equivalence; finite output;
   and bounded latency (output frame count == input; seamless block-boundary
   continuity). Limiter threshold/lookahead/attack/release and input trim /
   per-input EQ are **not yet provable** — those seams do not exist in the
   portable interface and land with step 3.
3. Implement the bounded signal path and coherent meter snapshot behind the
   portable DSP interface. The sample-vector tests, allocation guard, cycle
   budget, and queue/latency bounds are the completion gate.
   - **Signal path done.** The input stage (per-channel trim + up to two EQ
     sections, `AiusInputStage`) and the true-peak safety limiter tail
     (`AiusLimiter`, feed-forward stereo-linked lookahead) are additive stages in
     `aius_dsp.h`; both bypass under NULL/zero-init so the target and goldens are
     unchanged. Proven through the public interface (`dsp` + `dsp-vectors`):
     trim/EQ behaviour and bypass equivalence, limiter threshold/attack/release/
     lookahead, prevention of the surround51_impulse full-scale saturation,
     finite/bounded output, seamless block boundaries, and allocation-free via
     the `--wrap` shim. `teensy41-optical` builds.
   - **Meter module done (slice 3b).** `AiusMeter` / `AiusMeterSnapshot`: peak/RMS
     per input, stereo out peak/RMS, limiter gain reduction, ≤128 decimated
     waveform points, sequence number, and a seqlock coherent copy boundary
     (host-tested: a reader never observes a torn snapshot); plus queue-depth /
     estimated-latency / DSP-cycle fields the target adapter populates. Proven
     through the public interface and allocation-free via the `--wrap` shim;
     goldens byte-unchanged. Metering is **caller-driven**, deliberately NOT a
     field of `AiusDspConfig` — the caller taps around `aius_dsp_process` — so the
     meter code is not linked into the hot function until the adapter wires it.
   - **Target memory strategy (for Phase 4).** FlexRAM (512 KB) is fully allocated
     as 32 KB banks shared by ITCM code, DTCM data, and the runtime stack, so
     crossing the 192 KB ITCM boundary grabs a whole bank out of the stack budget.
     **Principle: hot-path data earns its DTCM place and is never relocated for
     space.** The single-cycle, deterministic DTCM access is the whole point for
     the decode/DSP arena and the codec lookup tables; FLASH/OCRAM would add
     cache-miss latency and jitter that the audio cycle budget cannot absorb.
     (The codec tables — `adpcm_coeffs`, `high_freq_samples`, `band_fir_*` — are
     already `const`; they live in DTCM only because Teensy copies `.rodata` to
     fast RAM at boot. Leave them.) So the only levers are:
     - **Cold data → OCRAM.** `g_http_request` / `g_http_response` (16 KB) moved
       DTCM→OCRAM (`DMAMEM`) — off the audio path, written-before-read. DTCM
       306 → 290 KB; FlexRAM stack headroom 22 → 38 KB; OCRAM 88 % free.
     - **Cold code → FLASH.** New config/HTTP/serialize/apply/serve code runs in
       `loop()`/HTTP context, not the audio ISR, so mark it `FLASHMEM`: zero ITCM,
       no audio-latency cost (FLASH 93 % free). The single-config default is also
       `const` + `FLASHMEM` (read only on a config change). Only genuinely
       per-audio-block code (decode, DSP, meter accumulate) stays in ITCM.
     - **Escalation** for space, in order: shrink the model (single-config
       re-model does this ~4×), shift a FlexRAM bank DTCM→ITCM (reallocates the
       split without moving hot data), or fit 16 MB PSRAM (EXTMEM) for large cold
       data. Relocating hot data is not on this list.
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

**Model — single editable config (revised 2026-07-24).** Full parametric control
(input trim/EQ, downmix matrix, output EQ, limiter) makes the old
select-a-preset model redundant, so there are NO `profiles[]` or `mixes[]` arrays
and no active-selection / `mode` / `mix` machinery. There is exactly one
effective config:

- a hardcoded **factory default** — one `const` (and `FLASHMEM`, since it is read
  cold, only on a config change) config baked from `config/optical_dsp.json`,
  immutable in FLASH, active at boot and on reset;
- a single **SD override** — the full custom config parsed from `/aius/dsp.json`
  into RAM. `source` (`default` | `sd`) reports which is active; a block-boundary
  apply swaps it and increments `config_generation`.

The config shape is:

- `source`, `config_generation`, `sample_rate_hz`
- `features`: `input_eq`, `matrix`, `output_eq`, `limiter`, `meters` (bypass flags)
- `input_channels`: fixed `fl`, `fr`, `fc`, `lfe`, `sl`, `sr`, each `gain_db` + `eq[]`
- `output_eq[]`: up to two bounded EQ sections
- `matrix`: `center_gain_linear`, `surround_gain_linear`, `lfe_gain_linear`,
  `headroom_linear`, `clipfree`
- `limiter`: `enabled`, `threshold_dbfs`, `attack_ms`, `release_ms`, `lookahead_ms`

Bounds are unchanged (input trim `[-60,12]` dB; EQ freq `[10,24000]`, Q `[0.1,20]`,
gain `[-24,24]`; limiter threshold `[-24,0]`, attack `[0.1,100]`, release
`[1,1000]`, lookahead `[0,5]`; two EQ sections per table; all finite). The whole
config is well under the 12,288-byte cap. `POST /api/mode`, `POST /api/mix`, and
the embedded page's mode/mix selectors are removed (no consumers). Named presets,
if ever wanted, are SD config files loaded via the editor, not firmware arrays.

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
  a trim and up to two EQ sections. Output EQ is a stereo post-mix section.
  Matrix controls cover centre, surround, LFE, and headroom coefficients.
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
- Model shape, bounds, `source`, and `config_generation` are defined in the
  single-config block at the top of this section. All values are finite; the
  whole config is well under the 12,288-byte cap.
- A `PUT` includes `base_generation`; a mismatch returns 409 without touching SD
  or active DSP. `PUT /api/dsp/config` requires `X-Aius-SD-Auth`.
- The body is capped at 12,288 bytes. Because `AIUS_HTTP_MAX_RESPONSE` also sizes
  stack `body[]` arrays, the config response uses a dedicated OCRAM (`DMAMEM`)
  buffer sized to the model, not a 16 KB stack bump, to respect the FlexRAM stack
  budget. At boot the const default is active; a valid `/aius/dsp.json` override is
  parsed outside the audio callback and becomes pending only after transactional
  SD commit; the snapshot swaps at the next audio block boundary and increments
  `config_generation`. Invalid JSON, SD removal, failed rename, or invalid ranges
  leave the last valid snapshot active.
- `GET /api/meters` is a separate schema: `sequence`, `sample_rate_hz`, `input[]`,
  `output`, `limiter_gain_reduction_db`, `queue_frames`, `estimated_latency_us`,
  `dsp_cycles_last`, `dsp_cycles_max`, and `waveform[]` capped at 128 points.
  Meter values are diagnostic snapshots and never become configuration fields.
- The build and runtime validators reject out-of-range values, non-finite
  numbers, and unknown input-channel/limiter keys; the config is capped at 12,288
  JSON bytes.
- `headroom_linear` is the resolved runtime value. `clipfree` indicates the config
  selected the DSP clip-free headroom policy; the response does not expose the
  build-time string sentinel. For the default downmix coefficients clip-free
  resolves to `1 / (1 + 0.7071067811865475 + 0.7071067811865475 +
  abs(lfe_gain_linear))`; unity otherwise.
- `center_gain_linear` is the shared FC-to-L/R coefficient (raises dialogue; zero
  removes centre). `surround_gain_linear` is the shared SL-to-L / SR-to-R
  coefficient (blends the rears while preserving their L/R placement; zero removes
  surround). The coefficients are symmetric by design; independent SL/SR gains are
  excluded because they would shift image balance rather than control blend.
  Matrix gains are finite, `0 <= gain <= 4` linear; configured non-clipfree
  headroom is `0 < h <= 1`.
- OpenAPI marks objects `additionalProperties: false`, arrays with exact
  `maxItems`, and numeric fields with units and finite-number expectations. It is
  the canonical external model description (schemas, field descriptions, enums,
  example, path).
- The embedded flash page gains a compact monospace DSP section: `source`, input
  trims, output-EQ rows, matrix coefficients, limiter, and pending state. It
  fetches `/api/dsp/config` independently of the status poll and stays usable
  without SD. Every `mode`/`mix` selector and every reference to `POST /api/mode`
  or `POST /api/mix` is removed from the page.

## Phases

The critical-path ordering is in the Execution handoff at the top; this is the
single-config work breakdown. **Steps 1–3 (the DSP model — signal path, true-peak
limiter, coherent meter module) are done and committed.** Remaining:

### Config model + read endpoint

Replace the profiles/mixes preset model with the single default config in
`config/optical_dsp.json` + `build_config.py`; serialize it via
`GET /api/dsp/config` (`FLASHMEM`); apply the single config in the audio path
(output EQ + downmix, default bit-identical to today); remove `POST /api/mode` /
`POST /api/mix` and every mode/mix reference from the embedded page; update swagger
+ the native `config` and Vitest OpenAPI suites. **Gate:** those suites green +
target build + default audio unchanged on device.

**Landed.** Single-config model + `PROGMEM` FLASH-resident default (`FLASHMEM` is
for code, not const data — the const config uses `PROGMEM`); serializer + apply
helper `FLASHMEM`; mode/mix routes, UDP verbs, and all embedded-page references
removed. `config` + swagger suites green; `teensy41-optical` builds with ITCM
headroom *improved* (98.5 %, 2968 B free — the smaller model + FLASH placement net
freed fast RAM). Default audio is bit-identical by construction (matrix = default
coefficients; EQ bypass = no-op); on-device audio confirmation is part of the live
smoke. Input trim/EQ + limiter application and PUT/SD are the next phases.

### Write + SD persistence + block-boundary apply

Authenticated `PUT /api/dsp/config` (`X-Aius-SD-Auth`) validates the bounded body,
writes `/aius/dsp.json` transactionally, and swaps the active snapshot at an audio
block boundary (`base_generation`/409, `source: sd`, `config_generation`
increment). Cold code is `FLASHMEM`; the config response uses a dedicated OCRAM
buffer, not a stack bump. Wire the input-stage trim/EQ and the limiter into the
audio apply here (they exist in `aius_dsp` but are not yet applied). **Gate:**
native config / SD-round-trip tests + target smoke, no flash writes.

**Input trim/EQ + limiter wiring done.** `AiusAudioProcessor::apply_config()`
(`src/optical_main.cpp`) now resolves `AIUS_OPTICAL_CONFIG.input_channels[]` into
an `AiusInputStage` (per-channel trim + up to two EQ sections, backed by static
scratch planes sized to the largest decoded block) and `AIUS_OPTICAL_CONFIG.limiter`
into an `AiusLimiter`; `enqueue()` passes both into `AiusDspConfig` gated by
`features.input_eq` / `features.limiter`. `apply_config()` moved out-of-line (it is
a class member so an in-body definition has vague/comdat linkage, which no longer
compiles once it is too large to always-inline alongside the plain `FLASHMEM` free
functions already sharing the `.flashmem` section — GCC's "section type conflict").
Default config is still bypass-equivalent (unity trim, no EQ, disabled limiter), so
default audio is unchanged by construction. `teensy41-optical` and
`teensy41-optical-fault` both build; RAM1 free-for-locals is 22,684 B (the ~12 KB
input-stage scratch consumes most of the 22→38 KB headroom the earlier OCRAM move
freed, still comfortable margin). PUT `/api/dsp/config`, SD persistence, and the
block-boundary generation swap are still open below.

### Meters endpoint + accumulate wiring

`GET /api/meters` from the coherent snapshot; wire the per-block meter accumulate
into the audio path — the one genuinely hot addition, so watch ITCM. **Gate:**
coherent bounded snapshots, no audio-thread allocation, ITCM within budget.

### SD control-surface editor

`firmware/sd/www/index.html` — static HTML/CSS/JS using only the documented API;
every control mapped to the config JSON; meters + canvas waveform from
`/api/meters`. Built and tested (Playwright) against captured vector/config
responses. **Gate:** the committed Playwright regression + target asset smoke.

### Target validation

Live smoke: default audio unchanged, config round-trip (GET/PUT/SD/reset), meters
render, editor loads, no audio/HTTP overruns under a short soak.

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
  routing, coefficients, EQ, limiter behaviour, finite output, and latency.
  (Done — the `dsp-vectors` suite.)
- `GET /api/dsp/config` returns the complete single effective config (`source`,
  input channels, output EQ, matrix, limiter, features) with resolved units.
- Authenticated `PUT /api/dsp/config` validates and commits atomically; invalid
  or stale (`base_generation`) writes leave SD and the active block-boundary
  snapshot unchanged.
- `GET /api/meters` returns coherent bounded snapshots without audio-thread
  allocation or unbounded sample transfer.
- OpenAPI schemas and example describe every returned field; no `mode`/`mix`
  selection endpoints remain.
- The SD editor maps every control to the documented JSON model and renders
  meters/waveform data from captured vector-driven responses.
- The embedded page reports the effective config (`source`, trims, EQ, matrix,
  limiter, pending state) without depending on SD, and carries no mode/mix
  selectors.
- The config stays within the request/response capacity and the DSP cycle/latency
  budgets without disrupting audio; hot-path data stays in DTCM, cold code/data in
  FLASH/OCRAM.
- Existing `/api/status`, the fallback page, and SD feature configuration continue
  to work; removing the `mode`/`mix` selection is intentional (no consumers).
