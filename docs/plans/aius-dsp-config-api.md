---
title: Aius DSP pipeline, SD editor, and configuration API
type: plans
scope: component
component: aius
status: in-progress
tags: [firmware, dsp, audio, http-api, sd]
updated: 2026-07-25
toc: true
---

# Aius DSP pipeline, SD editor, and configuration API

## Shared delivery sequence — 2026-07-24

**This section is identical in [`aius-network-hardening.md`](aius-network-hardening.md)
and [`aius-dsp-config-api.md`](aius-dsp-config-api.md). Change both together.** It
supersedes the separate execution handoffs both documents previously carried. Both
plans are active, they share one critical path, and neither finishes without the
other.

### Resume here

**B3, Phase 14, is 7 of 11 slices in.** S0, P1a, P1b, R1, R2, P2 and S1 are
delivered and pushed; the breakdown and per-slice commits are in
[§Phase 14](#phase-14--optical-runtime-cutover) under "Scoped slice breakdown",
with the findings that forced the re-scope under "Prebuild review findings".

**Next up is S2a**, which constructs the runtime and is the first slice that stops
being additive. It needs the replay decision below settled first, because S2a is
where `replay_start` must start refusing. S2b then deletes the legacy path; S3,
S4 and S5 follow in order.

Decisions already taken, so do not re-open them:

- **A `PUT` returns 200 only after the SD write commits, and the delivered
  transaction already does this** — it buffers the formatted response and reaches
  `SEND` only on `AIUS_HTTP_SD_COMPLETE`. What is optimistic is the dispatcher's
  immediate 200. P1a relaxes one admission rule; P1b moves the two config routes
  onto an SD job.
- **The large DSP model streams; `AIUS_HTTP_MAX_RESPONSE` does not move.**
  Raising it would grow two legacy DTCM stack frames while the legacy path is
  still the only working one.
- **Replay refill is deferred to Phase 14c**, landing before B6. It is a redesign
  of the portable `aius_replay` reader seam, not a port, so it does not belong in
  a slice serialized on `optical_main.cpp`. **`POST /api/replays/start` is refused
  from S2a until 14c lands** — its start path blocks, and refusing it also keeps
  any SD job from re-mounting the card under an open replay handle.
- **The `/api/meters` wire shape is settled**, in
  [`aius-dsp-config-api.md`](aius-dsp-config-api.md) §Meters endpoint: dBFS
  floored at `-120.0`, six positional inputs, `[L, R]` output arrays, per-mille
  integer waveform, `sequence == 0` served as a valid 200.
- **The the app dashboard is ignored until B9.** Its browser scenario fails
  today and that is expected.

Everything through B2 is delivered and pushed. Nothing is in flight and no
question is outstanding.

### Why the two strands are coupled

A firmware flash attempt during the DSP `PUT /api/dsp/config` work found the
deployed board crash-looping through DHCP on the legacy pre-cutover
`optical_main.cpp` path — the exact unbounded-recovery failure the network plan
exists to close. Only Phase 14 fixes it.

In the same window the DSP strand built its config PUT, its `/aius/dsp.json`
override load, and its `dsp_upload` job bookkeeping directly on top of the legacy
ad-hoc SD job machine and the legacy blocking HTTP path — all of which Phase 14
deletes. So the DSP strand cannot ship without the cutover, and the cutover now
carries DSP behaviour it did not originally own. Everything else in the DSP strand
(the SD-hosted control surface) is genuinely independent and runs in parallel.

### Corrections to earlier claims in both plans

- **`runtime-target` was wrongly listed as a delivered regression suite.** It did
  not exist when this section was written; Phase 12 has since created it, along
  with `optical-app` from Phase 11. The standing guard is now `net`, `boot`,
  `network-transport`, `sd`, `sd-target`, `http`, `usb-control`, `runtime`,
  `runtime-target`, `optical-app`, `boot-target`, and `fault-target`.
  `optical-cutover`, `flash`, and `recovery-runner` still do not exist and are
  created by Phases 14, 15, and 16.
- **`PUT /api/dsp/config` was not described in OpenAPI.** The write phase shipped
  the firmware without reaching `swagger.json`, so the DSP plan's
  OpenAPI gate was quietly unmet. Closed at step B5. `GET /api/meters` is still
  absent and lands with 14b.
- **Phase 10a's stated justification was wrong in one detail.** The DSP config
  `PUT` does not need the SD publish mechanism: it parses and validates the model
  from the HTTP body itself and writes the accepted bytes verbatim. The real
  second publish caller is the **boot / hot-reinsert load** of `/aius/dsp.json`,
  once that moves onto a portable `CONFIG_LOAD` job in Phase 14. Phase 10a is
  still required — for that path. It is delivered (`df9ea4483`).

### Structural obstacles the cutover must clear

Recorded here because they are cheap to miss and expensive to hit mid-phase:

- **The legacy SD symbols collide with the portable header in two places.**
  `optical_main.cpp:118` declares its own `struct AiusSdJob`, unrelated to the
  portable one in `aius_sd.h`, and `optical_main.cpp:115` declares an
  `AIUS_SD_JOB_DELETE` enumerator that redefines `aius_sd.h:20`. It compiles
  today only because the file never includes that header — but
  `aius_optical_app.h:21` includes it unconditionally, so merely naming the
  callback table pulls it in. Both legacy names must be renamed before any
  cutover work, which is what slice S0 is for.
- **The composed storage service cannot start a job on its own.** It accepts
  `AIUS_HTTP_ACTION_SD_START` and nothing else (`aius_runtime_storage.cpp:250`),
  and it acknowledges `AIUS_SD_ACTION_PUBLISH_CONFIG` into a saturating
  `publish_dropped` counter rather than delivering the payload
  (`aius_runtime_storage.cpp:134`). Every boot-time SD load in Phase 14 needs
  both gaps closed first.
- **`AiusRecoveryStatus` carries no IP, link state or DHCP counters**
  (`aius_net.h:146`), while `optical_main.cpp` reaches `Ethernet.localIP()` /
  `linkStatus()` in fourteen places including the status JSON, the UDP status
  payload and USB `ID`. Dropping the vendor include without first adding an
  accessor to `aius_network_teensy` leaves the board undiscoverable headless.
- **Any SD job remounts the card.** `aius_sd_job_start` enters
  `AIUS_SD_PHASE_BEGIN` for every job kind (`aius_sd.c:407`) and the adapter's
  `AIUS_SD_ACTION_BEGIN` calls `SD.begin(BUILTIN_SDCARD)`
  (`aius_sd_teensy.cpp:137`), guarded only by its own `file_open` flag. It cannot
  see a foreign handle such as `g_replay_file` (`optical_main.cpp:68`), so a
  config `PUT` or a probe re-mounts the volume beneath an open replay file.
- **`stage_dsp_sd_override()` is a blocking synchronous read.**
  `optical_main.cpp:279` reads up to 12,288 bytes one byte at a time from
  `setup()` and from `service_sd()`, and puts a 12,288-byte array on the stack
  while the RAM1 free-for-locals zone is 22,684 B. It violates the normative
  one-SD-primitive-per-epoch rule head-on. Porting it is part of Phase 14, not
  optional cleanup.
- **The legacy watchdog interval is 16 s, not the pinned 8 s.**
  `watchdog_init()` writes `WDOG_WCR_WT(31)`, and the feed has three call sites —
  `setup:2593`, `loop:2607` and `loop:2618`. The normative contract is one 8 s
  interval with exactly one feed after a complete epoch.
- **ITCM headroom is 2,120 bytes** (98.92 % used) as of slice S1. Every new
  per-epoch service, dispatcher, and adapter must be `FLASHMEM` unless it
  genuinely runs per audio block. Apply the attribute on each definition — GCC
  does not inherit a `section` attribute from a forward declaration, so a
  prototype-only `FLASHMEM` silently leaves the code in ITCM. Phases 11 and 12
  both held to zero ITCM this way.

  The figure was 1,416 bytes until S1, which **gained** 704 bytes of headroom
  rather than spending any. Referencing the previously single-use static
  serializers from the new callbacks changed GCC's inlining decisions, pushing
  code that had been inlined into ITCM callers out into the FLASHMEM adapters.
  Behaviour is unchanged; codegen is not. Expect the same effect to run in
  reverse at S2b, when the legacy callers those serializers shared are deleted.
- **The bank report does not yet include Phases 11 or 12.** Nothing references
  the dispatcher or the composed runtime, so `--gc-sections` drops every section
  and `nm` on `firmware.elf` finds none of their symbols. Phase 14 is where
  roughly 9.4 KB of FLASH and ~907 bytes of DTCM string data become real, and
  where the ~35 KB runtime instance must be placed in `DMAMEM` rather than the
  22,684-byte DTCM free-for-locals zone.

### Critical path

Two tracks. Track A has no firmware-source overlap and runs concurrently
throughout. Track B is serialized because every phase in it touches
`optical_main.cpp` or the files that replace it.

**Track A — SD control surface** (independent, no dependencies)

| Step | Work | Gate |
|---|---|---|
| A1 | **Done** — `firmware/sd/www/index.html` + assets: every config control mapped to the documented JSON model, meters and canvas waveform from `GET /api/meters`, matching the existing `replays.*` precedent | `test/sd-editor/`, 36 tests green |
| A2 | Target asset smoke — the page loads and drives the real device | folded into the single on-device session (B6) |

**Track B — bounded runtime cutover** (strictly ordered)

| Step | Work | Gate |
|---|---|---|
| B0 | **Done** (`df9ea4483`) — Phase 10a, SD job publish-capacity generalization | `sd` + `http` suites, target build |
| B1 | **Done** — Phase 11, optical application dispatch **including the DSP routes** | `optical-app` suite green |
| B2 | **Done** — Phase 12, Teensy runtime service composition | `runtime-target` suite green |
| B3 | Phase 14 — optical runtime cutover **plus the DSP SD-override load port**, eleven slices after the 2026-07-25 re-scope | `optical-cutover` + all component suites |
| B4 | Phase 14b — meters endpoint and accumulate wiring | `optical-app` + ITCM budget |
| B5 | OpenAPI catch-up — `PUT /api/dsp/config` **done**, `GET /api/meters` with 14b | Vitest OpenAPI contract suite |
| B5a | Phase 14c — replay refill onto a bounded SD job, deferred out of Phase 14 | `replay` + `sd` + `runtime-target` |
| B6 | Phase 16a — live recovery smoke on the deployed board, entered by **one accepted manual Program-button flash** | the smoke's own pass/fail per gate |
| B7 | Phase 15 — button-free flash tooling | `flash` + `usb-control` suites |
| B8 | Phase 16b — the zero-button-press flash gate, re-run once B7 exists | one clean button-free flash and restore |
| B9 | Retire the the app dashboard, keeping `swagger.json` and its contract suite | `ui-contract` and Vitest suites green after removal |

Phase 13's code baseline is delivered and unchanged. Phases 1–10 and 10a are
delivered and are not re-reviewed. Each remaining phase's gate is its green
native suite plus an owned-diff review — nothing heavier. The per-phase
source-blind oracle chains and clean-room delivery audits described in the
delivered-phase blocks are historical and are not re-run.

### Scope moved by this reconciliation

- **Phase 11 owns the DSP routes.** Its route inventory is every current HTTP
  route *plus* `GET /api/dsp/config`, `PUT /api/dsp/config`, and `GET /api/meters`.
  Writing the dispatcher without them means writing it twice.
- **Phase 14 absorbs the DSP SD-override load.** `stage_dsp_sd_override()`'s
  blocking read is replaced by a portable `CONFIG_LOAD` job with
  `publish_capacity = AIUS_DSP_MODEL_MAX_JSON_BYTES`, and the staged model still
  applies at the audio block boundary. This is what Phase 10a exists for. The
  `dsp_upload` write path moves onto the portable `UPLOAD/REPLACE` job with
  `publish = false` — the model is already parsed and validated in RAM, so
  re-publishing the written bytes would buy nothing.
- **The meters phase becomes Phase 14b**, owned by the network plan's phase
  sequence rather than standing alone in the DSP plan, because it wires into the
  post-cutover runtime and audio path. Its contract stays the DSP plan's.
- **The two live-hardware gates merge into one session.** The DSP plan's target
  validation (default audio unchanged, config round-trip, meters render, editor
  loads, short soak) and the network plan's recovery smoke run against one flash,
  as B6.
- **Phase 15 moves after the first live validation.** One manual button press is
  accepted to get the post-cutover image onto a board that currently cannot be
  reached any other way; the button-free tooling then removes the need for the
  next one.
- **The the app dashboard is out of scope until B9.** It is superseded by the
  device's own SD-hosted control surface and is queued for deletion as the last
  step, not repaired along the way. Two consequences to expect rather than
  investigate: the `control-shell.json` browser scenario **fails today** and stays
  failing until B9, because it asserts `/api/mode` and `/api/mix` are present in
  `swagger.json` while the OpenAPI suite asserts they are absent; and the
  dashboard's mode/mix buttons already 404 and will simply keep doing so. Do not
  spend time on either. B9 removes the dashboard, that scenario, and the
  `ui-contract-allowlist.json` entry for its stylesheet together, and coordinates
  with the ui-kit accessibility rollout that currently enrols the app. Nothing
  earlier in the sequence depends on it, including Phase 14's removal of the
  firmware-side `MODE`/`MIX` route enumerators.

## DSP work breakdown

The signal path below (steps 1–3) and the write/SD-persistence phase (PUT
`/api/dsp/config`) are done. The remaining DSP-side work is the SD
control-surface editor (Track A above, unblocked now), the meters endpoint
(B4, after the cutover), and the OpenAPI catch-up (B5). The committed sample
vectors are the executable specifications:

- `firmware/tools/dts_vectors/vectors/stereo_impulse.dts`;
- `firmware/tools/dts_vectors/vectors/surround51_impulse.dts`;
- `firmware/tools/dts_vectors/vectors/stereo_tones.dts`;
- `firmware/tools/dts_vectors/vectors/surround51_tones.dts`.

The ordering below is the DSP model's own build order and is complete through
step 4. Where it conflicts with anything else in this document, the shared
delivery sequence above wins:

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
matrix from this plan. This plan's hardware validation — default audio
unchanged, config round-trip, meters rendering, editor loading, and the
latency/audio-continuity gates — no longer runs as a separate session: it is
folded into the single on-device session at B6 above, against one flash.

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

The delivery ordering is the [shared delivery sequence](#shared-delivery-sequence--2026-07-24)
at the top; this is the single-config work breakdown. **Steps 1–3 (the DSP model
— signal path, true-peak limiter, coherent meter module) are done and
committed**, as are the config model, read endpoint, and the write / SD
persistence / block-boundary apply phase. Remaining:

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
freed, still comfortable margin).

**PUT `/api/dsp/config` + SD persistence + block-boundary apply done.** A new
bounded, allocation-free parser library (`lib/aius_dsp_config/`, mirroring
`lib/aius_config`'s hand-rolled recursive-descent style) validates the PUT wire
shape — the same 8 top-level keys `GET /api/dsp/config` returns plus a 9th,
`base_generation` (the optimistic-concurrency token) — against exactly
`scripts/build_config.py`'s bounds, and is proven by a new native `dsp-config`
suite (40+ assertions: every bound, every unknown/missing key at every nesting
level, wrong types, `>2` EQ sections, `source != "sd"`, malformed JSON, duplicate
keys, oversize). `aius_http.c`'s route gate now allows `PUT` on `DSP_CONFIG`
(POST/DELETE still rejected); `AIUS_HTTP_MAX_REQUEST`/`AIUS_HTTP_MAX_BODY` raised
to fit the 12,288-byte model plus header overhead — `g_http_request`/
`g_http_response` stay `DMAMEM`, unaffected by the earlier ITCM budget.
`http_dsp_config_put_request()` (`FLASHMEM`) checks `X-Aius-SD-Auth`, parses and
bounds-checks the body, 409s on a stale `base_generation` without touching SD or
the active snapshot, then reuses the existing `AiusSdJob` transactional
mkdir→write→backup→install state machine (a new `dsp_upload` job-kind bit,
parallel to the existing `config_upload`) to write `/aius/dsp.json` verbatim. A
new `g_pending_dsp_model`/`g_dsp_upload_pending`/`g_dsp_apply_pending` staging
triple (mirroring `g_pending_config`/`g_config_upload_pending`, all `DMAMEM`
except the `volatile bool` flag) hands the validated model from SD-job success to
`AiusAudioProcessor::enqueue()`, which is the actual block boundary: it applies
the staged model, sets `source: sd`, and increments `config_generation` there,
never in loop/HTTP context. `config_generation` is a per-boot-session counter
(0 at boot for the default default, +1 per applied change) — not itself
persisted; only the client-supplied `base_generation` on a PUT is a trusted
concurrency token, so a re-loaded SD file's stale `config_generation` is safely
ignored. `stage_dsp_sd_override()` (`FLASHMEM`) loads `/aius/dsp.json` through
the *same* parser at boot and on SD hot-reinsert (mirroring `load_sd_config()`'s
two call sites) — the persisted file already carries `source":"sd"` and a
`base_generation`, so it round-trips through the identical validator. `GET
/api/dsp/config` now serializes the live `g_active_model` (RAM mirror, `DMAMEM`,
kept in sync at the same block-boundary apply point) and reports the real
`source`/`config_generation` instead of hardcoded defaults, via a dedicated
`DMAMEM` `g_dsp_config_body[12288]` buffer instead of the shared 8 KB per-route
stack array (that array was previously too small for the model's true 12,288-byte
cap — a latent truncation bug this closes).

Two budget findings worth keeping in mind for future FLASHMEM work in this
codebase: (1) GCC does **not** merge the `section` attribute from a forward
declaration onto a later definition that omits it — `FLASHMEM` must be applied
directly on every definition, not via prototypes, or the code silently stays in
ITCM. (2) `strtod()` was dropped from the new parser in favor of a small
hand-rolled decimal-to-double decoder: pulling in newlib's general `strtod`/
`_dtoa_r` machinery cost ~7.7 KB of ITCM — nothing else in this firmware
previously needed floating-point string parsing, so it wasn't already paid for.
The custom decoder is not bit-exact with `strtod()` for pathological inputs
(very long digit runs, denormals) but is exact within double precision for every
value this schema's bounds allow.

`teensy41-optical` and `teensy41-optical-fault` both build with these changes;
RAM1 free-for-locals is back to 22,684 B, matching the input-stage-wiring
baseline above (net near-zero additional ITCM/DTCM cost once the parser and new
statics were correctly FLASHMEM/DMAMEM-routed). Native `dsp-config`, `http`,
`dsp`, `dsp-vectors`, and `config` suites are all green. Genuine on-device smoke
(a live PUT over the network, an actual SD write/reboot/reload cycle) has not
been run against real hardware in this session — target build success is the
proof available without physical device access; live-hardware smoke is a
follow-up before shipping this to the actual unit.

### Meters endpoint + accumulate wiring

**This work is now Phase 14b of
[`aius-network-hardening.md`](aius-network-hardening.md) (step B4).** It wires
into the audio callback and adds a dispatch route, both of which Phase 14
rewrites; landing it before the cutover means writing it twice against two
runtime shapes. The contract stays this plan's; the phase slot, owned paths, and
gate live with the cutover sequence. Its route is declared by Phase 11's
dispatcher (step B1) so the dispatcher is written once.

`GET /api/meters` from the coherent snapshot; wire the per-block meter accumulate
into the audio path — the one genuinely hot addition, and ITCM free space is
1,352 bytes, so the accumulate tap is the only part that may stay in ITCM.
**Gate:** coherent bounded snapshots, no audio-thread allocation, ITCM within
budget.

#### Settled wire shape (2026-07-24)

Three of the four points this section previously left open were never really
open: `AiusMeterSnapshot` already fixes them. `input_peak[]` / `input_rms[]` are
six entries in `AIUS_CH_*` bit order, `output_peak[2]` / `output_rms[2]` are
already per-channel `[L, R]`, and `waveform` is a non-negative per-bucket
peak-magnitude envelope with an explicit count, capped at 128. The JSON mirrors
the struct rather than reshaping it.

What was genuinely open was **units**, because the struct's header delegates the
`20·log10(v / AIUS_DSP_SAMPLE_MAX)` conversion to the reader. That delegation is
reversed: **the firmware converts.** The deciding argument is that `log10(0)` is
`-Infinity`, which is not valid JSON, and silence is an audio meter's resting
state rather than an edge case. Converting once on the device with a documented
floor beats every client reinventing that clamp.

```json
{
  "sequence": 1234,
  "sample_rate_hz": 48000,
  "input": [
    { "peak_dbfs": -6.0, "rms_dbfs": -12.3 }
  ],
  "output": {
    "peak_dbfs": [-3.1, -3.2],
    "rms_dbfs": [-9.4, -9.5]
  },
  "limiter_gain_reduction_db": -1.5,
  "waveform": [0, 12, 340, 998, 1000, 640],
  "queue_frames": 0,
  "estimated_latency_us": 0,
  "dsp_cycles_last": 0,
  "dsp_cycles_max": 0
}
```

- **Levels are dBFS, floored at `-120.0`.** Every `*_dbfs` field is finite;
  silence reports exactly `-120.0`, never `-Infinity` or `null`. Computed in the
  HTTP path, which is cold `FLASHMEM` code, so the ~16 `log10` calls per snapshot
  cost nothing the audio budget can feel.
  `limiter_gain_reduction_db` is already dB (`<= 0`) and passes through unconverted.
- **`input` is exactly six positional entries** in `AIUS_CH_*` order: `fl`, `fr`,
  `fc`, `lfe`, `sl`, `sr`. No per-entry channel key; position is the identity.
- **`output` keeps both channels** as two-element `[L, R]` arrays. Flattening to a
  scalar would discard information the pipeline already has.
- **`waveform` is per-mille integers, `0..1000`**, of full scale: non-negative
  magnitudes, drawn mirrored about the centre line. Chosen over raw sample units
  and over normalised floats because it is both the smallest encoding (~640 bytes
  against ~900 raw) and the cheapest to format, avoiding 128 float conversions per
  snapshot in the HTTP path. The array's length is the count; there is no separate
  `waveform_count` on the wire.
- **`features.meters` false is not a special case.** The route always returns 200
  with a valid snapshot. Nothing publishes while metering is bypassed, so
  `sequence` stays `0`, which the struct already documents as "never published".
  Clients render `sequence == 0` as "no data" rather than as zero-level signal.
  This reuses an existing sentinel instead of inventing a status-code convention.

**Both consumers need a change, and 14b owns both.** `replays.js` is wrong on two
counts today, independent of which shape was chosen: `line 18` does
`Number(output.peak_dbfs).toFixed(1)`, which yields `NaN` on a two-element array,
and its `y = h/2 - point*h/2` waveform maths assumes points in roughly `[-1, 1]`
while the struct holds `int32_t` sample units, so raw values would draw thousands
of pixels off-canvas. The SD editor accepts the chosen shape already but degrades
on a non-2xx rather than on `sequence == 0`, so it needs the "no data" branch.

### SD control-surface editor

**Track A — delivered, except the on-device asset smoke.**
`firmware/sd/www/index.html` plus `dsp-editor.css` / `dsp-editor.js` — static
HTML/CSS/JS using only the documented API; every control mapped to the config
JSON; meters + canvas waveform from `/api/meters`. It follows the existing
`firmware/sd/www/replays.*` precedent (self-contained classic script, no
shared-module imports, no `mr-*` kit) and links `replays.css` before its own
sheet so shared chrome cannot drift between the two SD pages. The device serving
the page off its own SD card, unable to reach the host application's shared mounts, is the
one standing exception to the repository's UI-kit reuse rule.

The regression is `test/sd-editor/dsp-editor.test.js` — vitest
plus happy-dom, loading the real page body and evaluating the real script against
a stubbed device contract, so it needs neither a served URL nor a `web.json`
mount. 36 tests cover the field-to-DOM mapping for every model field, a
byte-level PUT round trip, every documented bound (12 scalar cases + 6 EQ cases,
each asserting the inline message, `aria-invalid`, and that Apply refuses to
send), the 409 stale-generation path and its re-read recovery, the
pending→applied transition polling for a `config_generation` change, canvas
waveform geometry and the 128-point cap, `/api/meters` 404 degradation with
back-off, and hard transport failure on both read and write surfacing visibly.

Two page-level modelling decisions worth keeping: the EQ type select carries a
page-level `none` (section omitted from `eq[]`) alongside the real firmware
`bypass` type, because the wire model has no "absent" type; and with `clipfree`
checked the headroom input goes read-only and displays the resolved
`1 / (1 + centre + surround + |lfe|)`, which is what gets sent.

**Remaining:** the target asset smoke — the page loading and driving the real
device — folds into B6. The meter path is proven against authored-to-contract
fixtures rather than captured device responses, because `GET /api/meters` does
not exist yet; recheck it against the real endpoint once 14b lands and the wire
shape above is settled.

### OpenAPI catch-up

**Step B5. The `PUT` half is done; `GET /api/meters` waits for 14b** and the wire
shape settled above. `swagger.json` gains `putDspConfig` with the `SdAuth`
parameter, a `DspConfigUpdate` request body and schema, and an `SdJobError`
schema; the request reuses the existing `DspFeatures` / `DspInputChannels` /
`DspInputChannel` / `DspEqSection` / `DspMatrix` / `DspLimiter` sub-schemas
rather than forking a parallel request-side tree, and the suite asserts that
reuse by deep equality so a future divergence fails loudly.

**Documenting the route against the firmware, rather than against this plan,
found six places the plan was wrong or silent.** They are corrections to the
contract, not to the code:

- **"Validates and commits atomically" hides a split.** The handler never sends
  the 200. On success `http_dsp_config_put_request()` only *starts* an SD job;
  `{"ok":true}` is emitted later from the job's cleanup state
  (`optical_main.cpp:649`), and even then the model is only *staged* — the apply
  and the `config_generation` increment happen at the next audio block boundary.
  A client that reads 200 as "active" is wrong; it means "durably written".
- **The response set is larger than 400/401/409/503.** The transport can emit
  `413 request_too_large` and `413 body_too_large` before the handler runs, and
  the SD job can emit `500 sd_commit` or `507 sd_write` asynchronously with a
  five-field body (`ok`, `error`, `received`, `written`, `expected`), not the
  two-field error shape every other route uses.
- **Every parser failure collapses to one wire string.**
  `AiusDspConfigErrorCode` distinguishes unknown key, duplicate key, out of
  range, too many EQ sections, invalid source, invalid EQ type, and oversize —
  the wire returns `dsp_config_invalid` for all of them. The plan's validation
  section reads as though those distinctions are observable. They are not.
- **The 12,288-byte cap is not the observable boundary.**
  `AIUS_HTTP_MAX_REQUEST` is 13,312 for headers plus body, so a body over
  12,288 still reaches the parser and returns 400; only one that fills the
  transport buffer returns 413.
- **`config_generation` is required on a PUT and then ignored.**
  `parse_top_value()` parses it into a discarded double
  (`aius_dsp_config.c:1131`). Only `base_generation` drives the 409. The plan
  never says the request must carry it at all.
- **Request EQ sections are laxer than response ones.** `parse_eq_section()`
  requires only `type`; `frequency_hz`, `q`, and `gain_db` each default to 0
  when absent. For a non-bypass type the 10 Hz and 0.1 lower bounds make
  frequency and Q effectively required, but `gain_db` may be omitted for any
  type, and `{"type":"bypass"}` alone is accepted. `GET` always emits all four.
  **The schema deliberately stays stricter than the parser here** and records
  the laxity in prose: everything the document permits, the device accepts, and
  forking a laxer `DspEqSectionUpdate` would cascade into duplicate channel and
  channel-map schemas for no client benefit.

**Gate:** the Vitest OpenAPI contract suite (green, 2 tests) plus the Prettier
JSON check. Not proven: no OpenAPI schema validator runs over this document in
the repository — the suite is hand-rolled assertions — and the 500/507 field
list is read from `sd_job_send_error()`'s format string, not from a captured
device response.

### Unfinished mode/mix removal in the the host application app

The single-config work removed `POST /api/mode` and `POST /api/mix` from the
firmware, the UDP verbs, the embedded flash page, and `swagger.json` — but not
from the the host application app at ``. Three loose ends remain, found
while assembling Phase 11's route inventory:

- **Two checked-in tests contradict each other.**
  `frontend/test/scenarios/control-shell.json:13` asserts
  `/api/mode` and `/api/mix` **are** present in `swagger.json` (under
  `AIUS.APP.OPENAPI.1`), while `test/swagger.test.js` asserts they
  are **absent**. `swagger.json` does not contain them, so the browser scenario
  fails whenever it runs.
- **The dashboard still wires dead buttons.** `frontend/src/app.js`
  lines 92–93 POST to both routes. The firmware no longer dispatches either, so
  both controls now produce a 404.
- **The firmware parser still accepts them.** `aius_http.c` matches `/api/mode`
  and `/api/mix` into live route enumerators that nothing dispatches. Phase 11
  deliberately left them parsing and gave each a bounded 404, because removing
  them breaks the `http` suite (`test/http/test_http.c` depends on them in six
  places, four only incidentally as a POST-with-body vehicle) and that suite is
  not Phase 11's to edit.

None of this affects the device. The firmware-side half (retiring the `MODE` and
`MIX` route enumerators and their parser entries) lands with Phase 14, whose
owner holds `test/http/`.

**Decision (2026-07-24): retire the the app dashboard, keep the OpenAPI
document.** The device now serves its own full control surface from SD, so the
101-line dashboard at `frontend/` duplicates it and is the part
that has rotted. `swagger.json`, its Vitest contract suite, and the two
`web.json` mounts stay: the API contract is useful whether or not a device is
reachable, and it is the only machine-readable description of the wire format.
The flash page, not the host application, is the no-SD fallback, so nothing is lost as a
backstop.

**Queued as step B9, the last thing in the sequence.** The dashboard is out of
scope until then: not repaired, not partially fixed, not investigated. Its
`control-shell.json` scenario fails today and stays failing, and its mode/mix
buttons 404 and keep doing so. Both are known and neither blocks anything.

Deferring it to the end is also what makes the deletion cheap, because it is not
a local change: the aius dashboard is an enrolled participant in the ui-kit
accessibility rollout, an active workstream owned elsewhere.

- `src/libs/frontend-shared/test/fixtures/ui-contract-allowlist.json` carries an
  entry for `frontend/src/style.css` under `ownerTask:
  aius-adoption` with a `removeByPhase`. `ui-contract.test.js` matches allowlist
  entries against findings scanned from the tree, so deleting the file fails
  that suite until the entry goes too. The fixture also pins a
  `baselineCommit`, so it is not a file to edit casually.
- `docs/reference/ui-kit-compliance-catalogue.md` carries four aius rows,
  including a sequenced Phase 8 adoption whose manifest row the catalogue states
  "the sequenced Phase 8 manifest coordinator alone owns".
- `docs/plans/ui-kit-accessibility-rollout.md` is currently uncommitted in the
  shared tree, i.e. that rollout is mid-flight right now.

Two things make this cheaper than it looks: the planned contract scenario
`src/libs/frontend-shared/test/scenarios/contracts/aius.json` does not exist
yet, and `contracts/manifest.json` has no aius row. So the retirement cancels
planned work rather than unpicking landed work. It still needs the rollout
owner's agreement before anything is deleted, and by B9 that rollout may have
moved on far enough that the question answers itself.

`AIUS.APP.SHELL.1`, `AIUS.APP.CONTROL.1`, and `AIUS.APP.TELEMETRY.1` retire with
the dashboard; `AIUS.APP.OPENAPI.1` survives and needs rewording, since it says
the document describes "every device route" and the stale route list is exactly
what `control-shell.json` was asserting.

### Target validation

**Merged into step B6**, the single on-device session shared with the network
plan's recovery smoke: default audio unchanged, config round-trip
(GET/PUT/SD/reset), meters render, editor loads, no audio/HTTP overruns under a
short soak — all against one flash rather than a separate DSP-only session.

## Test scope and evidence

There is no `Aius.Regress` project or hardware requirements registry in this
repository. The authoritative regression scope is the existing
`firmware/scripts/test.sh --environment native --suite http`, `--suite
dsp-config` (the PUT /api/dsp/config bounded validator, added with the SD
persistence phase), the generated config validation test added to that runner,
the Vitest OpenAPI contract test, the target PlatformIO build, and live target
smoke/soak evidence. The embedded page contract will also have a Playwright
fixture regression that loads a captured model response and asserts the
field-to-DOM mapping; target-only network checks remain a separate hardware
gate.

## Acceptance gates

- All four committed DTS vectors drive decoder-to-stereo known-answer tests for
  routing, coefficients, EQ, limiter behaviour, finite output, and latency.
  (Done — the `dsp-vectors` suite.)
- `GET /api/dsp/config` returns the complete single effective config (`source`,
  input channels, output EQ, matrix, limiter, features) with resolved units.
  (Done — serializes `g_active_model`, real `source`/`config_generation`.)
- Authenticated `PUT /api/dsp/config` validates and commits atomically; invalid
  or stale (`base_generation`) writes leave SD and the active block-boundary
  snapshot unchanged. (Done — native-proven; live-hardware smoke still open.)
- `GET /api/meters` returns coherent bounded snapshots without audio-thread
  allocation or unbounded sample transfer.
- OpenAPI schemas and example describe every route and returned field; no
  `mode`/`mix` selection endpoints remain. (**Partly met** — `GET` and `PUT
  /api/dsp/config` are both documented as of step B5; `GET /api/meters` lands
  with 14b, once its wire shape is settled.)
- The `/aius/dsp.json` override is loaded through the bounded SD job path, not a
  synchronous read, so the boot and hot-reinsert paths respect the
  one-SD-primitive-per-epoch contract. (Closed by step B3.)
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
