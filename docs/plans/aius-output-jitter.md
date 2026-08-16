---
title: Aius output jitter elimination
type: plans
scope: component
component: aius
status: in-progress
tags: [aius, firmware, audio, latency, jitter, monitoring]
updated: 2026-08-16
---

# Aius output jitter elimination

Eliminate the audible output dropouts observed on 2026-08-16 during live 5.1
decode, without adding a frame of output latency. The owner's constraint is
explicit: the current 13 ms 5.1 residence is below the perception floor and
must not grow; the fix is to remove the loop-jitter sources, not to widen the
transmit cushion.

## Evidence (2026-08-16 session)

Deployed image `197523cbab76+teensy41-optical` (built 2026-08-15 00:26), boot
generation 4, ~16.4 h up. Owner observed repeated momentary output dropouts
with input continuing, during ~2 h of live AC-3 5.1 decode and during earlier
viewing the same evening.

- **One dropout is device-convicted.** The flight recorder froze on a
  gate-open TX underrun at uptime 47,007,931 ms. The pulled capture
  (21.8 s, full ring, zero gaps) contains **zero silent frames** and loud
  program (median −8.6 dBFS) right up to the freeze: committed-block flow
  stopped abruptly while the gate was open. Capture + header saved to session
  scratch.
- **The absence was in the 2.7–100 ms band.** Both stall-witness events this
  boot are accounted for elsewhere (boot-time network stack start, 1.558 s,
  matching `stack_start_blocking_ms`; and a dark-carrier SD stall below), so
  the dropout's cause was too short for the witness but longer than the
  transmit cushion.
- **The transmit cushion is 2.67 ms.** `AIUS_TX_GATE_OPEN_TARGET_FRAMES=128`
  (`firmware/platformio.ini`, tuned 2026-08-03 against sub-millisecond
  measured jitter). This boot's own maxima break that assumption: audio
  service 6,464 µs, HTTP 5,603 µs, `http_service_overruns` 6.
- **The meters analysis window is the dominant recurring HTTP cost.** 1024
  int32 samples serialized to ~7–10 KB of JSON inside one HTTP epoch, per
  poll, at the dashboard's 10 Hz — running exactly and only when the owner is
  watching, which is when the dropouts were observed. The idle (empty-window)
  response measured 3.9 KB.
- **An SD primitive is unbounded.** The witness caught a 582,524 µs epoch at
  checkpoint 8 with the preempted PC in `SdioCard::readData`
  (`sd_diag.longest_primitive_us` 582,363 concurs). Card-internal
  stall (wear-leveling class); it landed while the carrier was dark (zero
  ingress drops proves no live-carrier stall that long this boot), but the
  exposure is a once-per-second hot-plug probe
  (`AIUS_SD_PROBE_INTERVAL_MS 1000`; `root_jobs` ≈ uptime seconds).
- **Dropouts tonight were uncountable.** `tx_underrun_frames` counts idle
  silence at ~48 k/s and wraps (register entry of 2026-08-15); the recorder
  was frozen from the first dropout onward, and a frozen recorder counts
  nothing (`suppressed_triggers` only ticks during post-roll).

Related but distinct: the open register entry "Open-gate 128-frame TX
underrun, unexplained (2026-07-31)" is this same class, observed once before
the tuning to 128 even landed.

## Locked decisions (owner, 2026-08-16)

- No added output latency; the 128-frame gate-open target stays.
- The spectrum/spectrogram feed (`analysis_window`) is dropped from
  `/api/meters` entirely. Meter bars and limiter GR stay. The EQ curve is
  client-side and unaffected.
- A second audio-service pass is interleaved into the epoch.
- PCM pass-through block drops 512 → 128 frames (~8 ms mean PCM latency cut),
  with a live before/after decomposition.
- SD hot-plug probing pauses while the TX gate is open.

## Phases

### Phase A — drop the meters analysis window

- outcome: `/api/meters` no longer serves `analysis_window` /
  `analysis_count` / `analysis_full_scale`; response shrinks to bars, GR and
  telemetry (~500 B). The dsp-editor renders without the spectrum underlay and
  spectrogram; no console errors, no dead polling of absent fields.
- seams: meters serializer in `firmware/src/optical_main.cpp`; swagger
  `Meters` schema + drift gate emitter table; `firmware/sd/www/dsp-editor.js`
  spectrum/spectrogram paths; dsp-editor vitest suite.
- targeted-proof: the swagger `Meters` schema (additionalProperties: false,
  no analysis fields) never documented the window, so the removal FIXES a
  served-but-undocumented drift; the schema-pinning row in
  `test/swagger.test.js` and the dsp-editor suite (which
  feature-detects the window and degrades) stay green unchanged. The meters
  route remains outside the format-string extractor gate (pre-existing scope;
  its two array loops need extractor shapes it does not have — recorded, not
  expanded here). On-device: post-flash `/api/meters` body ≤ ~1.5 KB with no
  `analysis_window` key.
- unit: parent-only

### Phase B — interleave a second audio-service pass

- outcome: pipeline absence per epoch is bounded by one heavy segment, not
  the sum: an extra TX-refill/audio pass runs after the HTTP checkpoint, so
  the worst absence chain no longer stacks HTTP onto the light checkpoints
  plus SD.
- seams: epoch service order (`lib/aius_runtime` + `optical_main.cpp` service
  wiring); checkpoint ledger semantics (`docs/reference/usb-control-protocol.md`
  checkpoint table note); audio service re-entry contract.
- targeted-proof: host runtime suite pins the new service order; checkpoint
  walltime vector semantics unchanged (same 11 domains); adversarial hot-path
  review before commit (same bar as the meter-cadence change); on-device
  soak in the acceptance run below.
- unit: parent-only

### Phase C — pause SD hot-plug probe while the gate is open

- outcome: no routine card primitive runs while live audio is being emitted;
  `root_jobs` stops advancing during playback. Hot insert/removal detection
  resumes on gate close; `sd_present`, the usage figures and the card-swap
  config reload may be stale while deferred (documented at the guard).
- delivery-audit fix (2026-08-16): the gate latch alone loses the open edge
  (the guard runs before `tx_gate_service()` in the same checkpoint-7
  callback), which would have run one probe under a freshly opened gate at
  every session start. The guard now also tests `pllLocked()` — the
  current-instant carrier signal that precedes every carrier-driven open by
  at least the classify window. A replay-driven open at the start edge can
  coincide with a just-started probe, harmlessly: the probe's root job
  blocks the first refill (`sd_storage_idle()`), so the replay is
  necessarily still in its PREBUFFERING silence-carrier phase while any
  probe primitive runs — no program material is at risk — and once open,
  the gate latch defers all subsequent probes. Consequence: probes also
  pause while a carrier is merely locked (source powered, gate closed),
  which is acceptable for a probe with no urgency.
- seams: `sd_lifecycle_service()` scheduling in `optical_main.cpp`.
- targeted-proof: the deferral is a three-line guard in target-only lifecycle
  code with no host harness; proof is the optical image build plus on-device
  observation: `root_jobs` flat across a gate-open playback window, advancing
  again after gate close.
- unit: parent-only

### Phase D — gate-open-only underrun counter

- outcome: `tx_underrun_frames` counts substituted-silence frames only while
  the TX gate is open — a true glitch metric that neither wraps in idle nor
  hides real dropouts; the recorder underrun trigger keeps firing identically
  (it already qualifies on gate-open).
- seams: `aius_spdif_tx24_fill()` ISR (gate-visible flag), tx24 suite,
  recorder trigger suite; the 2026-08-15 register entry closes.
- targeted-proof: tx24 host suite pins idle-silence non-counting and
  gate-open counting; recorder suite still passes its underrun-trigger rows;
  adversarial hot-path review together with Phase B.
- unit: parent-only

### Phase E — PCM latency decomposition (BLOCKED on live data; 512→128 attempted and reverted)

**Finding, 2026-08-16 (the attempt's value):** `AIUS_PIPELINE_PCM_BLOCK_FRAMES`
at 512 is **load-bearing for re-type containment**, not just a buffer size. A
PCM→compressed switch validates one burst period (~1024 words) after the
first preamble; a 128-frame block completes inside that window and carries
IEC burst data to the sink as "PCM" — the `retype-switch` row caught 3 leaked
burst blocks, and at 512 the block boundary coincides with the worst
validation point so the staged tail is dropped instead of played. The
constant now documents this; the `eac3-entry` stuffing bound was re-pinned as
a frame budget (≤ 2048 frames) so a future quantum change is measured in
audio time, not block count.

**Also corrected:** staging latency is invisible to the editor's number
(`estimated_latency_us` is transmit-ring occupancy only), so the 512 quantum
cannot explain the observed 2× PCM display. The observed 26 ms is real ring
residence with an unexplained regulator.

- outcome: the PCM 2× display number is decomposed from live data: a 60 s
  poll of `tx_level_frames` / `estimated_latency_us` / `queue_frames` during
  genuine PCM playback names where the extra ~13 ms of ring residence comes
  from (gate-open trim not firing on the PCM path, commit lumping, or clock
  regulator behaviour). Only then is a latency cut designed — with a
  containment story (candidate-pending emission hold or equivalent) if it
  touches the quantum.
- seams: none yet (measurement first); candidate seams recorded above.
- targeted-proof: the live decomposition itself; any later change re-runs
  `retype-switch` and the `eac3-entry` frame-budget row as its containment
  gate.
- unit: parent-only

§ACTION REQUIRED (when convenient): play a genuine PCM source (TV menu /
stereo content) for ~2 minutes so the decomposition poll can run.

## Acceptance

One evening of live 5.1 with the dashboard open on the new image:
`tx_underrun_frames` (now gate-open-only) delta 0, no audible dropout, no
recorder freeze. The re-armed flight recorder is the independent witness —
any dropout freezes a capture.

## Residual risks

- The SD primitive tail remains unbounded for owner-initiated SD traffic
  during playback (dashboard file serves, uploads, recorder pulls). Accepted:
  owner-initiated, and Phase B halves its blast radius. A true async sector
  layer is recorded as disproportionate; the vendored FAT layer is
  synchronous to its core.
- Audio-service peaks (6.46 ms max this boot) can still exceed the 2.67 ms
  cushion in the worst decode+DSP epoch even after A–C; Phase B does not
  shorten the audio service itself. If the acceptance run still counts
  underruns, the next lever is bounding the meter-publish and DSP work per
  pass, not the cushion.
