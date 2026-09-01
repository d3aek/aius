---
title: Aius rate-gate external capture tool
type: plans
scope: component
component: aius
status: in-progress
subtype: design
tags: [aius, firmware, teensy, audio, spdif, acceptance, tooling]
updated: 2026-09-01
toc: true
---

# Aius rate-gate external capture tool

A build plan, written for a clean session to execute. It specifies the missing
board→U24 optical capture-and-reduce tool that produces the
`--external-observation` JSON `aius_rate_bench.py` needs to run sample-rate gates
2, 4 and 7. Everything a fresh context needs to build it without re-deriving is
here: the spec, the reusable code, the two genuinely hard parts, the per-gate
fields, the coupled analyzer fix, and the live bench/repo state as of
2026-09-01.

## Why this exists

`firmware/tools/net/aius_rate_bench.py` is read-only over HTTP — it polls only
`GET /api/status` and `GET /api/events`. Three acceptance gates assert
properties of the board's **actual optical output** that HTTP status cannot
self-report (a device's own claim that its output is continuous is not
evidence). Those claims are read from an `--external-observation` JSON file the
bench consumes but nothing produces. This tool produces that file: it records
the board's S/PDIF output through the U24's optical input and reduces the
waveform to the exact fields each gate checks. Until it exists, gates 2, 4 and 7
cannot pass — this is "blocker #2" referenced in
[`aius-sample-rate-following.md`](aius-sample-rate-following.md) and
[`aius-network-hardening.md`](aius-network-hardening.md).

## Current state (2026-09-01 session)

- **The optical return loop is now wired** (operator, 2026-09-01): board optical
  OUT → U24 optical IN, in addition to the existing U24 OUT → board IN source
  feed. So the full duplex loop U24→board→U24 is in place — this tool's
  precondition is met. Confirm both fibres before a run regardless.
- **Deployed board image: `fc674c6`** (`3824579` + defects 39/22). Reachable at
  `http://192.0.2.66`; SD auth token at repo-root `.secrets/aius-sd-auth`
  (`X-Aius-SD-Auth` on mutating routes; reads need nothing).
- **U24 XL** is ALSA card 0 = `hw:0,0`, USB sysfs `3-1.1` (`0a92:00d1`). Its
  mixer resets on any re-enumeration — re-apply every session:
  `amixer -c 0 cset numid=5 110,110` (playback 0 dB) and
  `amixer -c 0 cset numid=8 1` (capture source = IEC958 In). It free-runs its
  optical TX at ~48.02 kHz (+420 ppm) even with no `aplay`.
- **USB deauthorize is available** for inducing carrier loss (loss→replay leg):
  `echo 0 > /sys/bus/usb/devices/3-1.1/authorized` (0 = down, 1 = up). Path-guard
  permits it; in this harness each write also needs manual permission-mode
  approval. Deauth drops the board's carrier (`carrier:0`, `state:"unlocked"`);
  re-auth restores it.
- **`aius_rate_bench.py` already retries transient transport drops** (committed
  this session) so a 10-minute gate survives the board's ~0.5 % connection
  drops. Do not re-add that.
- **Build**: `PIO=$HOME/.venvs/platformio/bin/pio firmware/scripts/build.sh teensy41-optical`
  (warm cache, ~25 s). **Flash**: `firmware/scripts/flash_optical.sh` (CDC route,
  self-verifying; the operator is in `dialout`+`plugdev`).
- **Separate open finding a clean session must know**: gate 1 (`48k-10m`) fails
  on the deployed image at 52 post-settle trims (bound ≤ 2), everything else
  clean. It is NOT this tool's concern — see
  [`aius-sample-rate-following.md`](aius-sample-rate-following.md) §Acceptance;
  the recorded next step is an image-vs-image trim-cadence A/B before any bound
  change.

## Progress — 2026-09-01

The off-board build is done and unit-tested; only the live-board integration
(step 6) and two operator rulings remain.

- **Shipped: the transitions descope** (aius `93610f6`). The analyzer no longer
  asserts the 32 kHz leg; `firmware/test/rate-runner/test_rate_bench.py` is
  green at 24 tests. Coupled blocker #1 below is cleared.
- **Shipped: the capture tool** `firmware/tools/rate_gate_capture/capture.py`
  plus `firmware/test/rate-gate-capture/test_capture.py` (12 tests green). It
  implements the CLI/orchestration, all tractable reducers
  (`continuous_output`, `capture_frame_gaps`, `invalid_intervals_frames`,
  discontinuity detection), bounded-local-alignment correlation
  (`correlated_trim_updates`, `correlated_invalid_windows`,
  `discontinuities_outside_invalid`), inferred `channel_status_rate_hz`, and a
  provisional `old_rate_output_frames`. Build-sequence steps 1–5 are done.
- **Hard part 2 resolved empirically.** `amixer -c0 controls` on the U24 XL
  shows no IEC958 capture-status control, so a true channel-status decode is
  not available on this interface. The tool infers the rate from the tone and
  marks it `channel_status_rate_hz_source: "inferred"`. The open gate-2
  question is therefore narrowed: accept the inferred rate, redefine the field
  to `audio_rate_hz`, or add a dedicated S/PDIF protocol analyzer.
- **Hard part 1 decision.** Bounded local alignment is implemented as the
  correlation fiducial (the plan's "try this first"). Confirm it is acceptable
  for gate 7 before the live run, or swap in the flight-recorder fiducial.
- **Remaining: step 6 live integration** — run each gate end-to-end on the
  live board and confirm the bench reaches a real verdict. Needs the board,
  the fibre confirmation, and USB-deauth permission approvals; operator-gated.
  `old_rate_output_frames`'s detection model also needs live calibration.

## The tool's job

Produce the `--external-observation` JSON that `aius_rate_bench.py run <scenario>
--external-observation <file>` reads. Exact fields per gate, from
`firmware/test/rate-runner/test_rate_bench.py` (the only place the shapes are
pinned):

| Gate | Scenario | Required JSON | Asserts about the output |
|---|---|---|---|
| 2 | `pcm-120s` | `{"channel_status_rate_hz": 44100, "continuous_output": true}` | Output declares the right S/PDIF rate; no dropouts |
| 4 | `transitions` | `{"old_rate_output_frames": 0}` | No stale-rate frames emitted across a 48→44.1→48 transition |
| 7 | `waveform` | `{"capture_frame_gaps": 0, "discontinuities_outside_invalid": 0, "invalid_intervals_frames": [512], "correlated_trim_updates": N, "correlated_invalid_windows": N}` | Gap-free output; every discontinuity is a declared trim/invalid window, each exactly one 512-frame PCM block |

`N` must equal the `clock.trim_updates` / `clock.invalid_windows` deltas the
bench observes over the same run — that equality is the whole point of the
"correlated" fields.

## Reusable code — start from direct_loop_bracket.py

`firmware/tools/source_qualification/direct_loop_bracket.py` is ~80 % of the
capture chassis. It plays a tone and captures the return via ALSA, then reduces.
Reuse verbatim:

- ALSA orchestration: `arecord -D hw:0,0 -f S16_LE -r <rate> -c 2 -d <secs>` +
  `aplay -D hw:0,0 -r <rate>`, with `arecord.log`/`aplay.log` xrun capture and
  `stop_process()` cleanup.
- `read_wav()` → numpy `int32` frames.
- `active_bounds()` (RMS gating to the tone region).
- `true_runs()` on `np.all(frame==0)` → dropout/gap runs → **`continuous_output`**
  and **`capture_frame_gaps`**, and the run-length geometry for
  **`invalid_intervals_frames`** (assert each run `== 512`).
- `isolated_impulses()` / adjacent-jump detection → **discontinuity** locations.
- `cadence()` (positive-going zero-crossings → estimated Hz) → the ingredient for
  rate inference (see channel-status below).

Its output schema (`summary.json`, `source-validation.json`) is unrelated and is
NOT the gate JSON — write a new reducer, do not extend that schema.

`loop_refine.py` adds stereo-identity / bit-exact checks; reuse its bit-exact
comparison if a clean-source content check helps the discontinuity pass.

## Build sequence

1. **New tool** `firmware/tools/rate_gate_capture/capture.py` (own directory,
   mirror the source-qualification layout). CLI: `--device hw:0,0`, `--scenario
   {pcm-120s,transitions,waveform}`, `--rate`, `--seconds`, `--status-url`,
   `--output <dir>`, `--auth`. It captures via `arecord`, drives the stimulus via
   `aplay` (and transition stop/starts), polls `/api/status` in lockstep for the
   correlation series, reduces, and writes `<dir>/external-observation.json` in
   the exact per-gate shape above plus a full `<dir>/capture-analysis.json` of
   evidence.
2. **Reducers** for the tractable fields first (`continuous_output`,
   `capture_frame_gaps`, `invalid_intervals_frames`) over the reused primitives.
   Unit-test them against synthetic WAVs (gap-injected, 512-run-injected) the way
   `test_dsp_config.c` authors wire bytes — a new
   `firmware/test/rate-gate-capture/` python `unittest`.
3. **Time correlation** (hard part 1, below) — implement the chosen fiducial,
   then `discontinuities_outside_invalid`, `correlated_trim_updates`,
   `correlated_invalid_windows`.
4. **`channel_status_rate_hz`** (hard part 2) — implement the chosen approach and
   flag whether it is a true channel-status read or a tone-frequency inference.
5. **`old_rate_output_frames`** (gate 4) — windowed frequency estimate across the
   transition boundary; count frames on the old-rate side (0 expected).
6. **Integration test** on the live board: run each gate end-to-end and confirm
   `aius_rate_bench` reaches a verdict with the produced JSON.

## Hard part 1 — capture↔status time correlation

`discontinuities_outside_invalid` and the two `correlated_*` fields require
mapping a capture-domain event (a glitch at capture frame N) onto a status-domain
event (a `trim_updates`/`invalid_windows` increment at poll time T). The board's
output clock drifts against the host capture clock over 600 s, so a naive
`frame ÷ rate → wall-time` alignment accumulates error and mis-attributes
glitches to the wrong window. Options, cheapest first:

- **Board flight recorder as the fiducial.** The board timestamps its own output
  internally (`/api/recorder`, s24 frames). Align the U24 capture to the recorder
  stream by cross-correlation on a shared marker, then use the board's own
  timestamps for the status correlation. Most accurate; most work.
- **Stimulus marker edges.** Author the tone with periodic known amplitude
  markers; both the capture and the board's meters see them, giving periodic
  re-sync points that bound drift between them.
- **Bounded local alignment.** Only require correlation within a small window
  around each status-reported event (the analyzer already reports event times),
  not global frame alignment. Likely sufficient for gate 7's counts and the
  simplest to land — try this first.

## Hard part 2 — channel_status_rate_hz

PCM `arecord` yields the audio payload, NOT the IEC 61937 channel-status bits,
and the U24 capture path most likely does not expose the *incoming* channel
status via ALSA (verify: `amixer -c0 contents` for an IEC958 capture-status
control; probably absent). Options:

- **True read** — a dedicated S/PDIF protocol path that decodes channel status.
  Only if an ALSA control or a hardware analyzer exposes it. Confirm first.
- **Tone-frequency inference** — play a known 997 Hz tone; the captured tone
  frequency (`cadence()`) gives the effective rate
  `rate ≈ capture_rate × f_measured / 997`. This is an *inference of the audio
  rate*, not the channel-status word — emit it with an explicit
  `"channel_status_rate_hz_source": "inferred"` marker and get an design decision on
  whether gate 2 accepts an inferred rate, or whether gate 2's external field
  should be redefined to `audio_rate_hz`.

## Coupled blocker #1 — the transitions analyzer still hard-codes 32 kHz

Gate 4 cannot pass even with this tool until `aius_rate_bench.py`'s `transitions`
scenario is descoped from the 32 kHz leg (deferred by owner 2026-07-31, never
propagated to the analyzer). This is a separate, off-board, unit-testable fix —
do it alongside the tool. Exact edits in `firmware/tools/net/aius_rate_bench.py`:

- Line ~767: `expected = ["48000","44100","32000","48000","replay"]` →
  `["48000","44100","48000","replay"]`.
- Line ~798: `for transition in range(1, 4):` (three live rate transitions) →
  `range(1, 3)` (two: 48→44.1, 44.1→48).
- Line ~809: `live_transitions = [44100, 32000, 48000]` → `[44100, 48000]`.
- Lines ~812-814: `rate_delta != len(live_transitions)` message hard-codes
  "expected 3" — make it `len(live_transitions)`.
- Line ~819: `event_details != live_transitions` now compares to `[44100, 48000]`.

Then rework `firmware/test/rate-runner/test_rate_bench.py`: the transition
sample-builder (~lines 60-152) and ~5 `test_transition_*` cases bake the 5-phase
sequence and `phase_index <= 3` / `unsupported_details` triple. Descope to the
4-phase sequence, keeping the open-gate-underrun and unsupported-event cases
meaningful. Run `python3 firmware/test/rate-runner/test_rate_bench.py` (24 tests
today) to green.

Do NOT weaken the ≤2 trim bound or any acceptance threshold as part of this — the
descope only removes a leg the owner already removed from scope.

## Integration

Runs alongside `aius_rate_bench run <scenario>` (or the bench orchestrates it):
start `arecord` from the U24, run the scenario's stimulus (`aplay` at rate,
transitions via stop/start, loss→replay via USB deauth then
`POST /api/replays/start`), stop capture, reduce to
`external-observation.json`, pass via `--external-observation`. The bench's
analyzer combines it with the status series for the verdict. The tool's only job
is producing that JSON faithfully.

## Open questions for the operator

- Gate 2 `channel_status_rate_hz`: accept an inferred audio rate (marked), or
  require a true channel-status decode? (Blocks gate 2's exact field.)
- Time-correlation fiducial: is the flight recorder the intended reference, or is
  bounded local alignment around reported event times acceptable for gate 7?
- Confirm the board→U24 return fibre is on the same U24 optical input the capture
  device reads (`hw:0,0` IEC958 In), not a second interface.

## Acceptance for this tool

- The three reducer field-groups are unit-tested against synthetic WAVs.
- Each of gates 2, 4, 7 reaches a `pass`/`fail` verdict end-to-end on the live
  board with the produced JSON (a real verdict, not a tool error), on one exact
  committed image with `--expect-build`.
- The transitions descope lands with its tests green in the same change set.
