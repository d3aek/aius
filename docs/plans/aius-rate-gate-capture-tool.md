---
title: Aius rate-gate external capture tool
type: plans
scope: component
component: aius
status: in-progress
subtype: design
tags: [aius, firmware, teensy, audio, spdif, acceptance, tooling]
updated: 2026-09-03
toc: true
---

# Aius rate-gate external capture tool

A build plan, written for a clean session to execute. It specifies the missing
board→U24 optical capture-and-reduce tool that produces the
`--external-observation` JSON `aius_rate_bench.py` needs to run sample-rate gates
2, 4 and 7. Everything a fresh context needs to build it without re-deriving is
here: the spec, the reusable code, the two genuinely hard parts, the per-gate
fields, the coupled analyzer fix, and the live bench/repo state as of
2026-09-01, revised 2026-09-02.

**Read §Tool defects found in review before using any recorded live number.**
A 2026-09-02 review found nine confirmed defects in the shipped tool, the
first live re-run found three more in the gate-4 path (F10–F12), and adapting
the `transitions` analyzer to the aplay stimulus found one more (F13). All are
fixed and unit-tested. §Live-run results — 2026-09-02 carries the only results
produced by the fixed tool, each with its evidence digest; the 2026-09-01
section is kept for comparison only and none of it is evidence.

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

## Progress — 2026-09-01, revised 2026-09-02

The off-board build is done and unit-tested. A 2026-09-02 review found eight
confirmed defects in the tool (§Tool defects found in review); they are fixed
and unit-tested, but **every live result recorded below was produced by the
defective tool and must be re-run** before any of it is treated as evidence.

- **Shipped: the transitions descope** (aius `93610f6`). The analyzer no longer
  asserts the 32 kHz leg; `firmware/test/rate-runner/test_rate_bench.py` is
  green (46 cases in the file as of 2026-09-02, 64 through the suite's own
  discovery of `test_recorder_pull.py`; it was 24 when this line was first
  written). Coupled blocker #1 below is cleared.
- **Shipped: the capture tool** `firmware/tools/rate_gate_capture/capture.py`
  plus `firmware/test/rate-gate-capture/test_capture.py`. It implements the
  CLI/orchestration, all tractable reducers (`continuous_output`,
  `capture_frame_gaps`, `invalid_intervals_frames`, discontinuity detection),
  bounded-local-alignment correlation (`correlated_trim_updates`,
  `correlated_invalid_windows`, `discontinuities_outside_invalid`), the
  inferred gate-2 rate, and `old_rate_output_frames` via the flight recorder.
  Build-sequence steps 1–5 are done. The suite is **72 tests green**
  (2026-09-02, `scripts/test.sh --environment native --suite
  rate-gate-capture`); it was 19 before the review fixes, 59 before the
  gate-4 live-run fixes (F10–F12) and 69 before the stimulus adaptation.
- **Hard part 2 resolved (design decision 2026-09-01).** `amixer -c0 controls` on
  the U24 XL shows no IEC958 capture-status control, so a true channel-status
  decode is not available on this interface. Owner ruling: **redefine the
  gate-2 external field to `audio_rate_hz`** — the honest name for a
  tone-frequency inference. The bench gate, the tool, and their tests now use
  `audio_rate_hz`. The evidence carries `audio_rate_hz_source`, which is
  `"inferred-from-tone"` only when the capture rate differs from the expected
  output rate; when they match it reads `"capture-clock-locked"` with
  `self_referential: true`, because the U24 capture clock locks to the carrier
  under test and the "measurement" then just returns the rate `arecord` was
  opened at (defect F1 below).
- **Hard part 1 resolved (design decision 2026-09-01).** Bounded local alignment
  is the gate-7 correlation fiducial (implemented, unit-tested).
- **Live integration (step 6) — re-run with the fixed tool on
  `e746cf952dd6+teensy41-optical` (2026-09-02): gate 2 FAIL (real board
  dropout, root-caused the same day and fixed by output-jitter Phase F —
  PASS on `3570a216216d` that evening), gate 7 PASS under the contract the
  owner redefined on 2026-09-02 (the saved evidence re-analyzes clean), gate
  4 FAIL on the aplay-stimulus grounds with `old_rate_output_frames`
  genuinely measured at 0.** Each with its digest in §Live-run results —
  2026-09-02.

## Live-run results — 2026-09-02 (fixed tool, image `e746cf952dd6`)

All three gates were run against the deployed board on the exact image
`e746cf952dd6+teensy41-optical` (`--expect-build`), with the tool at aius
`82674e4` (gates 2 and 7) and `ce84850` plus the F12 reducer fix (gate 4).
The evidence directories are session scratch (7-day GC); the digests below
are the citation. Every verdict is a real bench verdict, not a tool error.

- **Gate 2 (`pcm-120s`, 44.1 kHz): FAIL — a real board dropout.**
  `evidence-sha256: f67108c070fbeaf5fd5917190ffecf71345815e738ecef5f2c0e0c8759ade53e`.
  External: `audio_rate_hz 44100` (`audio_rate_hz_source:
  "capture-clock-locked"`, `self_referential: true`, raw 44100.98,
  measured tone 996.978 Hz — a tautology, as the tool now says),
  `continuous_output false` with exactly **one 128-frame zero run** in the
  U24 capture. Bench: `harmful-counter` (`clock.tx_underrun_frames` +128)
  and `continuous-output`; every other counter delta 0, 12 trims / 12
  invalid windows, `http_client_drops` 0. Tool and board agree: a single
  2.9 ms dropout at ~19 s into the run. Last week's PASS came from a
  detector blind at the ±410 stimulus (F3). Opened as a board defect in
  [`aius-sample-rate-following.md`](aius-sample-rate-following.md) and
  root-caused there the same day (post-relock transmit-ring floor is phase
  luck; fix phase in [`aius-output-jitter.md`](aius-output-jitter.md)
  Phase F). **Owner ruling 2026-09-02 on the rate field:** `audio_rate_hz`
  stays in the evidence with its `capture-clock-locked` /
  `self_referential` label and asserts nothing on this hardware; gate 2 is
  judged on continuity and the harmful counters, which is exactly where this
  run fails.
- **Gate 2 re-run on `3570a216216d+teensy41-optical` (output-jitter Phase
  F on board, same evening): PASS.**
  `evidence-sha256: 781250dd5d3e06972905473562b4eef9cf4b2706b035fe4e29c0b431fb011d23`.
  External: `audio_rate_hz 44100` (self-referential, as labelled),
  `continuous_output true`, no zero run. Bench: no failures; underrun delta
  0, 12 trims / 12 invalid windows, `max_follow_error_millihz` 722,
  `max_clock_tx_level_frames` 512 with the polled level never below 128.
  The dropout above was the post-relock ring floor being phase luck;
  Phase F sets it to the target at the gate-open edge.
- **Gate 7 (`waveform`, 48 kHz): PASS under the contract redefined
  2026-09-02 (was FAIL on the old contract, output clean either way).**
  `evidence-sha256: 87e9ef415af946d9320808b4391f6c81817593c797acf038f36f70bc55b0ae7b`.
  Owner ruling: gate 7 is "no discontinuity outside the board-declared
  invalid windows; the windows need not be externally visible". The bench's
  `waveform` check now accepts an empty `invalid_intervals_frames` and
  requires the correlated counts to be at most the status deltas (never
  more: an undeclared artifact is the failure); every visible window must
  still be the exact 512-frame hold. Re-analyzing this run's saved
  `evidence.jsonl` offline with the updated bench gives `verdict pass`,
  `failures []` (same digest). On the old contract it failed only
  `external-observation` ("must be a non-empty integer array"); the detail
  below is unchanged.
  External: `discontinuities_outside_invalid 0`, `capture_frame_gaps 0`,
  `correlated_invalid_windows 0`, `correlated_trim_updates 0`,
  `invalid_intervals_frames []`; the fixed detector (jump threshold 85 LSB,
  held-run floor 4.3 LSB, phase-slip 0.065 rad, `capture_start_offset_s`
  1.75) saw no zero run, held run, spike or phase slip anywhere in 120 s
  while the board logged 12 trims / 12 invalid windows. Every counter
  delta 0. This time the "output is continuous through the invalid windows"
  reading is supported — the detector is proven at the documented stimulus
  — and the contract question is closed (§Operator rulings).
- **Gate 4 (`transitions`, 48→44.1→48, `--phase-seconds 2`, no replay
  leg): FAIL on the aplay-stimulus grounds; `old_rate_output_frames = 0`
  is now a real measurement.**
  `evidence-sha256: 6dc5f172e0ad2ea91d83da54187b5649e865606dc8e7af22803dd5e12740fda8`.
  The flight recorder captured slot 0 at `freeze_generation 1` (a recycled
  slot — the F10 identity path exercised live), `trigger operator`,
  `rate_changes 2`, both fiducials retained (`rate_change_frames` at pulled
  indexes 173056 and 210944, none rolled off) and both resolved
  (`rate_change_frames_detected [173056, 210944]`, `unresolved_fiducials
  []`, `lower_bound false`). Output-rate runs `[48000 95744, 44100 37888,
  48000 41728]` frames; `stream_rate_timeline` `[[0.011, 48000], [3.146, 0],
  [3.406, 44100], [5.234, 48000]]`. Bench: `transition-sequence` (expects the
  `replay` leg, not driven), `transition-unattributed-unsupported` (aplay
  idle gaps, 6 episodes), `harmful-counter` on `event_overflows` alone (10;
  tool cadence, F8 note), `tx_underrun_frames` +768 in the idle gaps. A
  first run with the reducer at `ce84850` (digest
  `132c94e1850ace90b860b9ae9d8b74250279b536193d692b0739853b41095d25`)
  captured slot 3 with the same fiducials but left the first one
  unresolved — that was F12, fixed and re-run. The earlier run at
  `--phase-seconds 1.2` (`a289bf3c…`) failed `no-transition-captured` for
  F10 and F11, not the board.
- **Offline re-analysis (analyzer F13 + `expected_phases` +
  `event_overflows` reported), 2026-09-02: both saved gate-4 runs still
  FAIL, each now on one failure, and both are real.** The evidence files
  are untouched and their digests are unchanged
  (`6dc5f172e0ad2ea91d83da54187b5649e865606dc8e7af22803dd5e12740fda8` for
  run 2, `132c94e1850ace90b860b9ae9d8b74250279b536193d692b0739853b41095d25`
  for the `ce84850` run); only the analyzer changed.
  Run 2 with `--expected-phases 48000,44100,48000`: the three baseline
  failures (`harmful-counter` on `event_overflows` +10,
  `transition-unattributed-unsupported`, `transition-sequence`) are gone,
  leaving `transition-reset-count` — "per-transition requested reset deltas
  are [2, 1], expected one each". That is a real observation, not a bench
  artifact: `audio_diag.service_requested_resets` advances **twice** across
  the first rate change (7→8 when the stream goes `unsupported` at 2.10 s,
  8→9 at 3.15 s when the U24 carrier disappears entirely,
  `stream_rate_hz 0`) and once across the second, where no carrier-gone
  sample was polled. The one-reset-per-transition bound is an acceptance
  threshold and was left alone. Everything else is clean:
  `live_transition_windows [[7,13],[15,20]]` (both attributed),
  `transition_reset_deltas [2,1]`,
  `live_transition_rate_change_deltas [1,1]`, `rate_changes_delta 2`,
  rate events `[44100, 48000]`, phase output blocks `[172, 45, 147]`,
  `event_overflows_delta 10` reported. Without the flag the same run fails
  `transition-sequence` alone.
  The earlier `ce84850` run, same flag: fails `transition-open-gate-underrun`
  alone — `tx_underrun_frames` +128 between two **open-gate** samples at
  5.50→5.76 s, after the final relock, on the pre-Phase-F image. That is the
  same post-relock transmit-ring floor gate 2 caught, i.e. the defect
  output-jitter Phase F fixes, so the fail is legitimate; its reset deltas
  are `[1, 1]`.

## Live-run results — 2026-09-03 (`--with-replay`, images `b38e8448f5c8` and `baef793d0063`)

Three live gate-4 runs with the replay leg driven, all `--phase-seconds 2`,
`--expect-build` exact, tone fixtures in session scratch. The first one found
a board defect and a tool defect; the third is the first run on which every
check but one is green.

- **Run 1 (`b38e8448f5c8`, tool at `b0bc64e`): FAIL, two failures, both on
  the U24 restore edge.**
  `evidence-sha256: d385ade4349338fbeee9054a24401ead788fc90d7be558b30f194d4fb4927def`.
  Slot 1 captured (`trigger operator`, `rate_changes 2`, both fiducials
  resolved at 173056 / 210944, `old_rate_output_frames 0`), phases observed
  `48000, 44100, 48000, replay`, `transition_reset_deltas [1, 1, 1]`,
  `tx_underrun_frames` delta 0 (the pre-Phase-F open-gate underrun of the
  `ce84850` run is gone), both live transitions attributed. The failures:
  `harmful-counter` (`audio_diag.au_abandons` +1, `pipeline_last_failure 3`
  MALFORMED: the interrupted replay unit) and
  `transition-unattributed-unsupported` (+4 at 12.85→13.11 s, details
  65535 / 27336 / 54249 / 46003), both at the single poll where the tool
  re-authorized the U24 — a replay→live transition the declared stimulus
  ends before. Tool defect: the poller ran through the restore. Fixed: the
  poller is stopped and joined before `usb_authorize(..., 1)`
  (`recorder_transitions.py`, README contract bullet).
- **Between runs 1 and 2 the board wedged** (`state 2`, `stream_rate_hz 0`,
  gate closed, `AIUS_EVENT_OVERRUN` flooding at ~190/s, every drop
  `ingress_drop_missing_live`), so run 2 (`a0b4f2c4…`) reads
  `stimulus-not-followed` and is a reproduction, not a measurement. Root
  cause and fix: the register's *RX pool corruption under replay* (the
  retained receive ISR writes past the block pair while replay drives the
  graph and the unlocked receiver storms at ~2,460 update calls/s; fixed in
  `baef793d0063` by re-pointing the receive DMA vector at a guarded stager).
  This run is what surfaced it: no earlier gate-4 run drove the replay leg
  on a board whose carrier then returned.
- **Run 3 (`baef793d0063`, fixed tool): FAIL on `transition-reset-count`
  alone; everything else green.**
  `evidence-sha256: 062c344609510381ebeca365802c05a617c802dd9408a2e65157123665723826`.
  Slot captured, `old_rate_output_frames 0` through two resolved fiducials,
  observed phases `48000, 44100, 48000, replay`, `live_transition_windows
  [[7,13],[15,20]]` both attributed, `tx_underrun_frames` 0,
  `au_abandons` 0 inside the evidence, `event_overflows` 7 reported,
  `unsupported_episodes` 6 all inside live windows, `final_replay_output_
  blocks_delta 276`. `transition_reset_deltas [1, 2, 1]`: the second blip
  cost a reset at the unsupported edge (4.19 s) and another at acceptance
  (5.23 s). After the run the board was back on live PCM at 48 kHz with
  `overruns 0`, `pool_blocks_used 2 / max 4`, `rx_dropped_halves 7,195`
  (the replay window's storm, every half the old ISR would have written out
  of bounds).
- **Reset count across the three live runs: `[2, 1]`, `[1, 1, 1]`,
  `[1, 2, 1]`.** One or two per blip, depending on whether the carrier
  passed through an unsupported measured rate before the new nominal was
  accepted; the acceptance then resets again because
  `g_clock_relock_reset_nominal_hz` no longer matches (`sample_rate_service`,
  `reset_for_acceptance`). Owner ruling: (b), skip the redundant acceptance
  reset in firmware (`g_audio_ingest_since_reset`), thresholds untouched.
- **Run 4 (`1e4224a7e3c3`, acceptance reset gated): FAIL on
  `transition-open-gate-underrun` alone.**
  `evidence-sha256: 06945e481125efda0ed6a90b70721519f57beafc0d8bc582a82dca2ab54602ff`.
  `transition_reset_deltas [1, 1, 1]` (the acceptance reset is gone), slot
  captured, `old_rate_output_frames 0`, both live windows attributed. The
  one failure: `tx_underrun_frames` +128 between two open-gate samples at
  replay start (9.40→9.67 s), the ring at 0 when the gate opened and the
  128-frame pad drained before the first replay block. Register: *Replay
  start can open the gate on an empty ring*.
- **Run 5 (`1e4224a7e3c3`): PASS — the first green gate 4.**
  `evidence-sha256: 0cf5eeda18a4c28c6daefe2d50a9461fde09b4b248f8d61effb03cc16a5ecc9f`.
  `verdict pass`, `failures []`, `transition_reset_deltas [1, 1, 1]`,
  `tx_underrun_frames` 0, `au_abandons` 0, `overruns` 0, `unsupported_
  episodes` 6 all inside live windows, `event_overflows` 9 reported, rate
  events `[44100, 48000]`, phase output blocks `[172, 45, 147, 299]`, slot 1
  captured with both fiducials resolved (173056 / 210944), `old_rate_output_
  frames 0`. After the run: PCM tracking at 48 kHz, gate open, `overruns 0`,
  pool 2 / 4.

## Live-run results — 2026-09-03, replay floor (`--with-replay`, image `4c82434311f0`)

Three runs on the replay gate-open floor (`gate_open_replay_target_frames
768`; register §Open defects — runtime, *replay start could open the gate on
an empty ring*), same recipe, fresh flash before run 1, mixer re-set to
110,110 after every run. The fix's instrument is the replay window of every
run: `tx_underrun_frames` stayed 0 lifetime across all three replay starts,
`gate_open_padded_frames` advanced +768 at each (384→1152, 1536→2304,
2688→3456) and the ring rode 1024–1280 frames through the whole replay while
`replay_diag.longest_primitive_us` climbed to 7,222 (`OPEN_READ`) — the
primitive that drained the 256-frame ring of run 4.

- **Run 1: FAIL on the stimulus, not the fix.**
  `evidence-sha256: 641fc3affdec1f55f5d20908f89751b8f67f53cd0b0a35f9f07a2c40fbc67ff3`.
  `transition-sequence` (`observed ['48000', 'replay']`) plus two
  `transition-unattributed-unsupported`: the board accepted 44.1 kHz at
  3.93 s, 1.8 s after the switch (run 5: 1.05 s), leaving one sample inside
  the 2 s phase where the analyzer wants `phase_samples 2`, so no live
  window formed and the six rate-switch blips (the same six runs 3–5
  attributed) had nothing to attribute to. Slot captured,
  `old_rate_output_frames 0`, replay window clean as above.
- **Run 2: PASS.**
  `evidence-sha256: 9dc6785ca8103e6cb75b01af321aacb582196f6827f706d39108cface2ce17ee`.
  `transition_reset_deltas [1, 1, 1]`, `live_transition_windows
  [[7,13],[15,20]]`, slot captured, `old_rate_output_frames 0`.
- **Run 3: PASS.**
  `evidence-sha256: b599b118c847f62b508b2f7ad34033ff466aad6466b344b67ba8c0e548f4001e`.
  Same shape as run 2; `event_overflows` 9 reported. After the three runs:
  PCM tracking at 48 kHz, gate open, `overruns 0`, `tx_underrun_frames 0`,
  pool 2 / 4, `au_abandons 3` (one per carrier return, outside every run's
  evidence — the register's open `au_abandons` item, unchanged).

Finding for the tool: the 2 s phase is marginal against the board's
44.1 kHz acceptance latency (1.05–1.8 s observed), and 2 s is the longest
phase the 262144-frame slot admits without `--allow-rolloff`; a run that
reads `observed ['48000', 'replay']` with `carrier_rate_millihz` at 44118 kHz
for the whole phase is this, not a board fault.

### Stimulus fix — per-phase durations and an acceptance-gated freeze (2026-09-03, tool only)

**The board stages nothing into the recorder while it sits `unsupported`,
so a phase costs ring frames at its own rate and only for the time the board
tracks it; the lead-in costs nothing.** Read straight off the three runs'
`capture-analysis.json`: the slot's 44.1 kHz run was 8192 frames on run 1
(whose polls show 0.26 s of 44.1 kHz `tracking`) and 37632 / 37888 on runs 2
and 3 (0.78 s), and the frames after the second fiducial were 51200 on every
run — `(2 s − 1.13 s + 0.2 s) × 48000`, i.e. the 44.1→48 acceptance took
~1.13 s each time while the 48→44.1 acceptance ranged 1.3 s (runs 2–3) to
1.85 s (run 1). The old budget `phase_seconds × (phases − 1) + 1 s` at
48 kHz for every phase therefore over-counted the middle phase by 8 % and,
more to the point, was symmetric where the stimulus need not be.

- **Corrected ring budget** (`required_capture_frames`):
  `Σ over phases after the first of (seconds × that phase's tone rate) +
  1 s margin × 48000`, for the instant-acceptance worst case (a slow board
  only lowers it). The margin itemises to ~0.6 s: two aplay gaps ≤ 0.1 s,
  the 0.3 s freeze dwell plus one poll of detection latency, the freeze POST,
  and the 4096-frame pre-fiducial window of the lead-in's tail. Symmetric
  2 s phases need 232200 (fit); symmetric 3 s need 324300 (do not); the new
  default needs **252300** against 262144. A 3 s middle with a 2 s trailing
  phase would need 276300 and does not fit — hence the short trailing phase
  and the gated freeze below.
- **CLI:** `--phase-seconds` takes one value for all three aplay phases (as
  before) or `first,middle,last`. **Default `2,3,1.5`**: the 48 kHz lead-in
  stays 2 s (free in the ring; settles the analyzer's first phase from any
  idle carrier); the 44.1 kHz middle is 3 s, which outlasts the 1.85 s
  acceptance budget by more than three 0.25 s polls (the analyzer labels a
  phase from `tracking` polls only and wants `phase_samples 2`, so the
  tracking window must exceed two full intervals; 2 s lost this by 0.15 s on
  run 1); the trailing 48 kHz phase is 1.5 s.
- **Freeze gated on acceptance, not on phase length.** After the trailing
  aplay returns the tool waits (≤ 1 s) for a status poll stamped since the
  switch back that shows the board tracking 48 kHz — the second fiducial is
  then stamped — dwells 0.3 s and freezes; the U24 keeps the 48 kHz carrier
  up after aplay exits, so acceptance completes after a short phase. On
  timeout it freezes anyway (the old fixed-delay behaviour, later). This is
  what lets the trailing phase be shorter than the acceptance latency
  without losing the second fiducial.
- **Evidence:** `phase_seconds` keeps its scalar meaning and is `null` when
  the phases differ; `phase_seconds_per_phase`, `phase_tones`,
  `phase_rates_hz` and `phase_frames` (now a per-phase list) are
  authoritative; `switch_back_elapsed_s`, `reference_tracking_seen_elapsed_s`,
  `freeze_elapsed_s`, `freeze_trigger` (`reference-rate-tracking` |
  `acceptance-timeout`) and `acceptance_latency_budget_s` record the freeze
  decision. No verdict threshold and no `phase_samples` moved.
- **Offline re-check:** runs 2 and 3 re-analysed with the bench read
  `verdict pass`, `transition_reset_deltas [1, 1, 1]`,
  `live_transition_windows [[7, 13], [15, 20]]`; run 1 still reads
  `transition-sequence` `observed ['48000', 'replay']` plus two
  `transition-unattributed-unsupported` (it cannot be rescued offline — the
  point is the same diagnosis). The suite grew 72 → 91 (`scripts/test.sh
  --environment native --suite rate-gate-capture`), including a synthetic
  reproduction of the run-1 shape (one tracking poll in the middle) and an
  unequal-phase pass at the same thresholds.
- **Live proof — run 1 at the new defaults, image `4c9eb3d497ac`: PASS.**
  `evidence-sha256: 5a8ff1aaf9ec56f77c5dee8377b7f918bd546819ac60c5fd955c0c1ae6011c60`.
  `phase_frames [96000, 132300, 72000]`, `required_capture_frames 252300`
  of 262144, `freeze_trigger reference-rate-tracking`: the board accepted
  44.1 kHz at 3.38 s (1.3 s after the switch) and 48 kHz at 6.24 s (1.2 s
  after the 5.02 s switch back), the freeze landed at 6.83 s, slot 0
  captured with `rate_changes 2`, `old_rate_output_frames 0`.
  `observed ['48000', '44100', '48000', 'replay']`, `transition_reset_deltas
  [1, 1, 1]`, `live_transition_windows [[7, 13], [19, 24]]`, every counter
  delta 0 inside the evidence (`au_abandons`, `tx_underrun_frames`,
  `overruns`), `event_overflows` 7. After the run: PCM tracking at 48 kHz,
  gate open, `tx_underrun_frames 0`, `gate_open_padded_frames` 128 → 1280
  (three live relocks + one replay start), pool 2 / 4, `au_abandons` 0 → 1
  with `pipeline_last_failure 3` — the one carrier return of the run, the
  register's closed `au_abandons` item (a DTS replay's cut burst, by design;
  from the next image the latch reads `4` truncated, not `3` malformed — the
  register entry records the relabel).

## Live-run results — 2026-09-01 (INVALIDATED, re-run required)

**Every number in this section was produced by a tool carrying defects
F1/F2/F3/F5 (§Tool defects found in review, 2026-09-02) and none of it is
evidence.** The numbers are kept verbatim so the re-run has something to
compare against; each entry names the defects that invalidate it and why.
**No evidence artifact exists for any of these results** — the runs predate
the tool's evidence-digest emission, so there is no `evidence.jsonl` path or
SHA-256 to cite for any of them, and none can be reconstructed. Do not treat
any figure below as reproducible until a fresh run supplies a digest
(§Evidence retention).

Full end-to-end runs on image `fc674c60f06c+teensy41-optical` via
`capture.py … --expect-build fc674c60f06c+teensy41-optical` then
`aius_rate_bench.py analyze <scenario> --evidence <dir>/evidence.jsonl`
(the tool emits a bench-format `evidence.jsonl`, so one capture owns the
board and `analyze` is offline). A loop-order fix was required first: for
the board→U24 loop the capture clock locks to the board's output, so aplay
must start and the board must lock to the target rate **before** arecord
opens, or arecord gets an immediate read I/O error (fixed in `7baa909`).

- **Gate 2 (`pcm-120s`, 44.1 kHz): recorded PASS — invalidated by tool
  defects F1, re-run required.** Recorded: `audio_rate_hz` 44100,
  `continuous_output` true; measured tone 997.0003 Hz, zero dropouts over
  129.4 s / 129 samples, all counter deltas 0, follow error 444 mHz. Why it
  is not evidence: the run captured at 44100 with `--expected-rate` 44100, so
  the rate check was **self-referential** — the U24 capture clock locks to the
  carrier under test, the measured tone is 997 Hz by construction (997.0003
  is that tautology, not a measurement), and the inverted rate model then
  returned the capture rate back to itself. The pass says the output was
  continuous; it says nothing about the rate. A re-run must either accept
  `audio_rate_hz_source: "capture-clock-locked"` as the honest scope of the
  gate or capture at a different rate.
- **Gate 7 (`waveform`, 48 kHz): recorded FAIL — invalidated by tool defects
  F2 and F3, re-run required.** Why it is not evidence: the run's status
  series was offset from the capture by the board's rate-lock wait (up to
  20 s) with no origin correction, so no increment could land inside the
  1.5 s alignment window and the correlation was structurally guaranteed to
  be zero (F2); and the discontinuity detector's 1000-LSB floor was ~18x the
  ±410 stimulus's own peak slope, so it could not have seen a held block or a
  1–2 sample slip either (F3). The recorded verdict is a tool artifact, and
  the conclusion drawn from it below is unsupported. The observed detail was:
  The board reported **12 trim updates and 12 invalid windows** in status,
  but the U24 capture observed **no** zero-runs, discontinuities, or
  correlations (`invalid_intervals_frames: []`), so the gate tripped
  `external-observation` ("must be a non-empty integer array"). This is a
  genuine finding: **the board absorbs its invalid/trim events cleanly and
  emits no externally observable output artifact.** Gate 7's external
  contract assumes each invalid window manifests as a muted 512-frame PCM
  block; that premise does not hold for this board's clean rate-following.
  The discontinuity detector is amplitude-adaptive and would catch a
  512-frame held/muted block (two large edge jumps) — it saw none, so the
  output really is continuous through the invalid windows.

  **That last sentence was wrong and is retracted (F3).** The detector's
  floor was `max(1000, 3 × p95(adjacent jump))`; at the documented ±410
  stimulus the tone's own largest adjacent jump is ~54 LSB, so the 1000-LSB
  floor always won and a 512-frame held block, a 1-sample drop and a
  2-sample drop were **all invisible**. "It saw none" was a statement about
  the detector, not about the output. Nothing here supports "the output
  really is continuous"; the correlation figure is separately void under F2.
  The gate-7 contract question stays genuinely open, but it must be re-asked
  from a re-run: either the board's invalid windows are genuinely not meant
  to disturb the output (then gate 7's external fields should be redefined or
  dropped), or a subtler artifact signature must be specified for the tool to
  look for.
- **Gate 4 (`transitions`): `old_rate_output_frames` measured through the
  flight recorder — the recorded `0` is invalidated by tool defect F5 (and
  F4 if no slot was captured), re-run required.** The U24 ALSA path cannot
  measure it (capture clock locks to the incoming carrier — fixed-rate
  arecord errors on a rate change; `plughw` resampling erases the tone
  signature). `recorder_transitions.py` instead reads the board's own OUTPUT
  frames from `/api/recorder` (staged without resampling, per-slot rate
  stamp): read at a 48 kHz reference, a 44.1 kHz-output segment shows 1085 Hz
  (997 × 48000/44100, calibrated live) and a 48 kHz segment 997 Hz, so the
  output-rate timeline is recoverable. **Recorded live result 2026-09-01:**
  output-rate runs `[48000, 44100, 48000]`, `old_rate_output_frames = 0`.
  Why the `0` is not evidence: it came from run-length analysis, and stale
  old-rate frames after a switch are **contiguous with the pre-switch run at
  the same samples-per-cycle**, so the run-length reducer merges them into
  that run and returns 0 however many there were (F5). Separately, a run that
  captured no frozen slot also returned `0` and the bench accepted it as
  clean (F4). The measurement now needs the board's per-slot fiducial
  (`slots[].rate_change_frames`) *and* `slots[].stream_end_frame` to place it
  in the pulled frames; until the firmware emits both the reducer reports a
  `"run-length (no fiducial)"` **lower bound**, which is not a
  pass. The tool drives 48→44.1→48 via aplay while polling status/events
  (background thread) for the evidence file.

  The full scenario now runs end-to-end (`--with-replay`): USB-deauth `3-1.1`
  (the U24, which the recorder path does NOT depend on) drops the carrier,
  `POST /api/replays/start channel_id_51` returns 200, the U24 is restored,
  and the status evidence carries the full `48000→44100→48000→replay`
  sequence (the replay phase reads `carrier_locked:false, master:output,
  state:replay`). **The bench reaches a real `fail` verdict** — gate 4's
  tool-acceptance (a real verdict, not a tool error) is met. The
  `old_rate_output_frames = 0` that verdict carried is invalidated by F5
  above; the *sequence* half of the verdict (the failures listed next) does
  not depend on that field and stands.

  Observed failures below cite `harmful-counter` on `event_overflows`. Note
  that counter is partly this tool's own poll cadence: events the board rings
  out between two `/api/events` polls are lost and counted as an overflow.
  The cadence was 0.7 s at the time of this run and is now 0.25 s, recorded
  per run as `status_poll_interval_s`. **An `event_overflows`-only
  `harmful-counter` failure is tool-induced, not a board defect** (F8).

  A clean `pass` is NOT reachable with the aplay-driven stimulus and is a
  stimulus-source limitation, not a tool defect: `aplay` stop/start leaves
  the U24 idling at ~48.02 kHz between rate phases, so the board logs
  `unsupported` episodes and `tx_underrun` in the gaps, and
  `stable_phase_labels` (built for a stimulus that cleanly cuts the carrier
  between phases, as the analyzer's own fixture does) collapses the observed
  sequence. Observed failures: `transition-sequence` (phase labeler sees
  `[48000, replay]`), `transition-unattributed-unsupported` (idle-gap
  episodes outside the live windows), `harmful-counter` (`event_overflows`
  from the event burst outpacing polling; one `au_abandons` at the replay
  cut). Longer/denser phases made it worse (more idle underruns). A green
  pass needs a stimulus source that cuts the carrier between rate phases (a
  programmable S/PDIF generator), or an owner decision to adapt the
  `transitions` analyzer to the aplay-blip pattern — NOT weakening any
  threshold (forbidden by this plan).

## The tool's job

Produce the `--external-observation` JSON that `aius_rate_bench.py run <scenario>
--external-observation <file>` reads. Exact fields per gate, from
`firmware/test/rate-runner/test_rate_bench.py` (the only place the shapes are
pinned):

| Gate | Scenario | Required JSON | Asserts about the output |
|---|---|---|---|
| 2 | `pcm-120s` | `{"audio_rate_hz": 44100, "continuous_output": true}` | Output runs at the right rate (inferred from the tone; see hard part 2); no dropouts |
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
- `isolated_impulses()` / adjacent-jump detection → **discontinuity**
  locations. **Do NOT reuse its 1000-LSB floor** (F3): at the documented ±410
  `tone_gen` stimulus the tone's own peak adjacent slope is ~54 LSB, so that
  floor cannot see a held block or a 1–2 sample slip. The gate detector
  derives every threshold from the measured amplitude instead.
- `cadence()` (positive-going zero-crossings → estimated Hz) → the ingredient for
  rate inference (see hard part 2 below). Note the direction of the model:
  `R_out = 997 × R_capture / f_measured`, not its inverse (F1).

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
4. **`audio_rate_hz`** (hard part 2; the field was renamed from
   `channel_status_rate_hz` by the 2026-09-01 design decision) — implement the
   chosen approach and flag in `audio_rate_hz_source` whether it is a true
   channel-status read, an independent tone-frequency inference, or a
   capture-clock-locked (self-referential) reading.
5. **`old_rate_output_frames`** (gate 4) — the distance from the board's own
   per-slot rate-change fiducial to the frame where the recorded output's
   samples-per-cycle actually changes (0 expected). A windowed frequency
   estimate with no fiducial cannot measure this: stale old-rate frames are
   contiguous with the pre-switch run at the same samples-per-cycle.
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

## Hard part 2 — the gate-2 rate field (was `channel_status_rate_hz`)

PCM `arecord` yields the audio payload, NOT the IEC 61937 channel-status bits,
and the U24 capture path does not expose the *incoming* channel status via
ALSA (confirmed 2026-09-01: `amixer -c0 controls` has no IEC958
capture-status control). Options:

- **True read** — a dedicated S/PDIF protocol path that decodes channel status.
  Only if an ALSA control or a hardware analyzer exposes it. Not available on
  this interface; needs a protocol analyzer.
- **Tone-frequency inference** — play a known 997 Hz tone; the captured tone
  frequency gives the output rate `R_out = 997 × R_capture / f_measured`.
  (Note the direction: the inverse form `R_capture × f_measured / 997` is the
  *reader's* view and reports 52245 Hz for a 44.1 kHz stream read at 48 kHz —
  defect F1.) This is an *inference of the audio rate*, not the
  channel-status word.

**Resolved: the field is `audio_rate_hz`** (design decision 2026-09-01) with
`audio_rate_hz_source` in the evidence. One consequence the ruling did not
anticipate: on the board→U24 loop the capture clock **locks to the carrier
under test**, so opening `arecord` at the rate under test makes `f_measured`
997 Hz by construction and the inference degenerates to reporting the capture
rate back to itself. That case is marked `self_referential: true` /
`audio_rate_hz_source: "capture-clock-locked"`, and a gate-2 pass on it
asserts continuity only — the rate half is not independently measured. An
independent reading needs `--rate ≠ --expected-rate` or a channel-status
decode.

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
meaningful. Run `scripts/test.sh --environment native --suite rate-runner` to
green (24 tests when this was written; 64 as of 2026-09-02).

Do NOT weaken the ≤2 trim bound or any acceptance threshold as part of this — the
descope only removes a leg the owner already removed from scope.

**Superseded in shape by operator ruling (d), 2026-09-02.** The edits above
landed and then stopped being literal: the sequence is no longer a constant in
the analyzer but the `expected_phases` option (default
`48000,44100,48000,replay`), and the live transitions are derived from those
labels rather than from `range(1, 3)`. Do not re-hard-code either.

## Integration

Runs alongside `aius_rate_bench run <scenario>` (or the bench orchestrates it):
start `arecord` from the U24, run the scenario's stimulus (`aplay` at rate,
transitions via stop/start, loss→replay via USB deauth then
`POST /api/replays/start`), stop capture, reduce to
`external-observation.json`, pass via `--external-observation`. The bench's
analyzer combines it with the status series for the verdict. The tool's only job
is producing that JSON faithfully.

## Operator rulings (2026-09-01)

- **Gate 2 field: RESOLVED — redefine to `audio_rate_hz`.** A true
  channel-status decode is unavailable on this U24 (no IEC958 capture-status
  control), so the field honestly names the tone-frequency inference. **The
  naming is resolved; what the field can assert is not** — see hard part 2's
  self-referential case, still open for an design decision.
- **Time-correlation fiducial: RESOLVED — bounded local alignment** around
  reported event times for gate 7 (not the flight recorder). The mechanism is
  settled; its two time origins were wrong in the shipped tool (F2) and are
  fixed but not yet re-proven on the board.
- **Live run: AUTHORISED — run all gates.** Still requires confirming the
  board→U24 return fibre is on the same U24 optical input the capture device
  reads (`hw:0,0` IEC958 In), and per-write USB-deauth approval for gate 4's
  loss→replay leg.

## Operator rulings (2026-09-02)

- **Gate 7 contract: RESOLVED — (b).** Gate 7 is "no discontinuity outside
  the board-declared invalid windows; the windows need not be externally
  visible". The bench `waveform` check accepts an empty interval list and
  bounds the correlated counts by the status deltas from above; visible
  windows must still be exactly 512 frames and pair with a correlated
  window. The 2026-09-02 evidence passes.
- **Gate 2 rate field: RESOLVED — keep `audio_rate_hz`, labelled.** It is
  capture-clock-locked on this hardware and asserts nothing; the evidence
  keeps it with `audio_rate_hz_source` / `self_referential`, and gate 2 is
  judged on continuity plus the harmful counters. No channel-status decode
  is sought on other hardware.
- **Gate 4 stimulus: RESOLVED — (d) adapt the `transitions` analyzer to the
  aplay-blip stimulus, thresholds untouched.** No programmable S/PDIF
  generator is bought. The analyzer stops asserting things the stimulus never
  declared: the phase sequence is an `expected_phases` analysis option the
  capture tool records per run (three labels without `--with-replay`, four
  with it), live rate windows come from the observed labels (F13), and
  `event_overflows` is reported rather than gated **in `transitions` only**
  (F8 — it measures the tool's poll cadence against the transition's event
  burst; `unsupported-event-count` remains the real guard against lost events
  breaking attribution). Every acceptance threshold inside every check is
  unchanged, and every other scenario still gates `event_overflows`.

## Acceptance for this tool

- The three reducer field-groups are unit-tested against synthetic WAVs.
  **Done** — 91 tests in `firmware/test/rate-gate-capture/` (2026-09-03;
  72 on 2026-09-02, 19 before the review fixes), including five that round-trip a synthetic
  `evidence.jsonl` through the real bench analyzer (two of them the
  recorder tool's recorded `expected_phases`) and two that drive the real
  bench `transitions` check. Run as `scripts/test.sh --environment native
  --suite rate-gate-capture`.
- Each of gates 2, 4, 7 reaches a `pass`/`fail` verdict end-to-end on the live
  board with the produced JSON (a real verdict, not a tool error), on one exact
  committed image with `--expect-build`. **Met 2026-09-02** on
  `e746cf952dd6+teensy41-optical` with the fixed tool (§Live-run results —
  2026-09-02): gate 2 fail (real dropout; pass on `3570a216216d` after the
  Phase F fix the same evening), gate 7 pass (under the contract redefined
  the same day; fail on the old one), gate 4 fail (stimulus grounds) with
  `old_rate_output_frames = 0` measured through resolved fiducials.
- The transitions descope lands with its tests green in the same change set.
  **Done** (aius `93610f6`).
- Every recorded live result cites an `evidence.jsonl` path and SHA-256.
  **Met for the 2026-09-02 runs; not met for the 2026-09-01 runs**, which
  stay non-evidence (§Evidence retention).

## Evidence retention

**A live result recorded in this plan must cite the digest of the evidence
file it came from.** Both tools write their run directory under
`firmware/test/evidence/rate-gate-capture/<scenario>-<utc>/` (gitignored, so
artifacts never enter a commit) and print, on completion:

```text
evidence: <absolute path>/evidence.jsonl
evidence-sha256: <64 hex>
```

`--output` overrides the directory; the default is the timestamped path
above. Quote both lines beside any number claimed from a run. A figure with
no digest is an unverifiable claim — which is exactly the state the
2026-09-01 results are in, and why they cannot simply be re-blessed once the
tool is fixed.

## Tool defects found in review — 2026-09-02

Nine confirmed findings against the shipped tool (F10–F13 follow). All are
fixed and unit-tested off-board; none has been re-proven on the board.

- **F1 — inverted gate-2 rate model.** `reduce_pcm` computed
  `expected_rate × f_measured / 997`; the physical inverse is
  `997 × R_capture / f_measured`. 44.1 kHz frames read at 48 kHz reported
  52245 Hz. Fixed, plus nominal-rate snapping (`audio_rate_hz_raw` /
  `audio_rate_hz`), `capture_rate_hz`, and the `self_referential` /
  `audio_rate_hz_source: "capture-clock-locked"` marking that keeps a gate-2
  pass from reading as an independent rate measurement.
- **F2 — capture↔status time origin.** The alignment mapped status
  `elapsed_s` straight onto capture frames, but the run clock starts at the
  stimulus and `arecord` opens only after the board locks (up to 20 s later);
  `active_bounds()` then trims further. Against a 1.5 s margin, gate 7's
  correlation could never match. The capture origin is now taken from
  `arecord`'s first frames (`capture_start_offset_s`) and the active-bounds
  trim is subtracted too.
- **F3 — discontinuity detector blind at the documented stimulus.** The
  1000-LSB floor was ~18x the ±410 `tone_gen` stimulus's own peak adjacent
  slope (~54 LSB), so a 512-frame held block and 1- and 2-sample slips were
  all invisible. Replaced with three amplitude-derived signatures (held run
  of near-zero first difference, slope-scaled spike threshold, fitted-tone
  phase slip); `stimulus_amplitude` and every effective threshold are now in
  the evidence. **This invalidated the gate-7 conclusion** quoted in
  §Live-run results.
- **F4 — vacuous gate-4 pass on zero captured slots.** No frozen slot meant
  `slots = []` and `old_rate_output_frames: 0`, which the bench accepted as
  clean. Now `null` + `captured_slots: 0` + `failure:
  "no-transition-captured"`, which the bench rejects as a non-integer.
- **F5 — `stale_old_rate_frames` could not measure the gate-4 quantity.**
  Stale old-rate frames after a switch are contiguous with the pre-switch run
  at the same samples-per-cycle, so run-length analysis merges them and
  always returns 0. Redesigned around the board's per-slot fiducial
  (`slots[].rate_change_frames` / `rate_change_count`): the count is the
  distance from each fiducial to the frame where samples-per-cycle actually
  changes. A third rate is counted rather than dropped.

  Those fiducials are **stream** indexes (frames staged since the slot armed,
  `firmware/lib/aius_recorder/include/aius_recorder.h:163-171`), while the
  pulled array is `0..valid_frames-1` from the oldest *retained* frame, so
  each is converted with
  `wav_index = fiducial − (stream_end_frame − valid_frames)` using the
  header's new per-slot `stream_end_frame` (frames staged at POSTROLL exit).
  A fiducial landing outside `[0, valid_frames)` rolled off before the freeze
  and is dropped into `fiducials_rolled_off`. With no fiducial in the header,
  no `stream_end_frame` to give the origin (guessing one would fabricate the
  measurement), or every fiducial rolled off, the reducer marks
  `"method": "run-length (no fiducial)"` and `lower_bound: true`.
- **F6 — inverted transition read-frequency model.** `reduce_transitions`
  used `997 × old/new`; stale 48 kHz frames read at 44.1 kHz show
  `997 × 44100/48000 ≈ 916 Hz`, not 1085 Hz. Fixed, and the test fixture that
  baked the inversion was rebuilt physically.
- **F7 — arecord exit status and log never inspected.** Now: non-zero exit
  and a WAV short of `--seconds` are hard failures (marker in
  `capture_failure`, gate-critical field neutralised, exit 2), and ALSA
  overrun lines are counted into `capture_frame_gaps` / `alsa_overruns`.
- **F8 — recorder-tool operational faults.** The persistent recorder config
  is now restored in a `finally`; the sysfs USB write no longer interpolates
  a path into `sh -c`; ring capacity is checked against `--phase-seconds × 3
  phases + aplay latency` (refuse, or `--allow-rolloff`); the captured slot
  comes from `newest_frozen_slot` (fallback: highest `freeze_generation`)
  and must be newer than the pre-run baseline; the status poll is 0.25 s and
  the evidence records that `event_overflows` alone is a tool-induced
  `harmful-counter` failure.
- **F9 — the suite was never registered.** `test/rate-gate-capture/` had no
  arm in `firmware/scripts/test.sh`, so it had never run under the runner.
  Registered as `rate-gate-capture` with its header and README rows.

Three more, found by the first live gate-4 run with the fixed tool
(2026-09-02, aius `ce84850` and the follow-up reducer commit):

- **F10 — `freeze_generation` treated as global.** The captured-slot check
  required a `freeze_generation` newer than the pre-run baseline, but the
  generation is per slot (it counts that slot's thaws and starts at 0), so on
  a fresh boot every frozen slot reads 0 and every real capture was rejected.
  The captured slot is now the newest frozen slot whose `(slot,
  freeze_generation)` identity was not frozen at baseline; the evidence
  carries `baseline_frozen_identities` and `captured_trigger`.
- **F11 — `aplay -d int(phase_seconds)`.** `--phase-seconds 1.2` played 1 s
  phases, shorter than the board's follow latency: the status polls show the
  board in `unsupported` at 48000 for the whole 44.1 kHz phase and
  `rate_changes` unchanged, so nothing was ever stamped. Phases are now played
  by frame count (`aplay --samples` from the tone's WAV rate, recorded as
  `phase_frames`), the evidence carries the `stream_rate_timeline`, and a run
  whose polls never saw 44100 reports `failure_reason: stimulus-not-followed`
  instead of blaming the freeze. The ring-capacity refusal was also
  over-conservative (it demanded all three phases fit the slot; only the
  frames from just before the first rate boundary to the freeze matter), so it
  now budgets `(phases − 1) × phase + 1 s` and admits 2 s phases against the
  board's 262144-frame slot.
- **F12 — old rate read from the block adjacent to the fiducial.** The board
  stamps `rate_change_frames` at block granularity, ~400 frames *after* the
  output has already switched (live capture: the blocks from −448 to −64
  frames before the stamp straddle the discontinuity, one reading 97
  samples/cycle), so the adjacent-block classification returned nothing and
  the fiducial went unresolved exactly when the switch was clean. The old rate
  is now the first classifiable block in a 4096-frame window before the
  fiducial, searched forward from there; a switch preceding its stamp measures
  0. A fiducial that still resolves to nothing is listed in
  `unresolved_fiducials` and marks the count `lower_bound: true` rather than
  contributing a silent 0.

One more, found while adapting the `transitions` analyzer to the aplay-blip
stimulus (2026-09-02, operator ruling (d) in §Operator rulings (2026-09-02)):

- **F13 — live transition windows taken by position, not by label.**
  `check_transition_dark_effects()` computed
  `live_windows = transition_windows[:-1]`, which assumes the last window
  always leads into `replay`. On a run that drives no replay leg the real
  44.1→48 window is the last one, so it was dropped from the live set and its
  dark `unsupported_episodes` advance was reported as
  `transition-unattributed-unsupported` — a false failure for a window the
  board handled exactly like the one before it (run 2: the 15→16 episode sits
  in window `[15, 20]`). A window is now live iff **both** bounding observed
  phases are numeric rates (`live_transition_positions`), so a window into or
  out of `replay` stays non-live and every genuine rate window is attributed.
  The live set is reported as `metrics.live_transition_windows`. No threshold
  moved: the open-gate rules and the unsupported-event accounting are
  unchanged.

## Follow-on work (owner input needed)

- ~~Re-run gates 2, 4 and 7 with the fixed tool~~ **Done 2026-09-02**
  (§Live-run results — 2026-09-02).
- ~~Gate 7 contract (decision owed)~~ **Resolved 2026-09-02: (b)** — see
  §Operator rulings (2026-09-02); the bench check and its tests carry it.
- ~~Gate 2 scope (decision owed)~~ **Resolved 2026-09-02: keep
  `audio_rate_hz`, labelled** — see §Operator rulings (2026-09-02).
- ~~Gate 4 `old_rate_output_frames` fiducial — measurement pending
  firmware~~ **Done**: the firmware emits `slots[].rate_change_frames`,
  `rate_change_count` and `stream_end_frame` (recorder Phase C/D fixes), and
  the 2026-09-02 run measured 0 through two resolved fiducials.
- ~~Gate 4 green pass — stimulus decision owed~~ **Resolved 2026-09-02: (d)**
  — adapt the `transitions` analyzer to the aplay-blip stimulus, no threshold
  weakened (§Operator rulings (2026-09-02)). Shipped: `expected_phases`
  (analysis option, `--expected-phases`, recorded per run by
  `recorder_transitions.py`), label-derived live windows (F13), and
  `event_overflows` reported rather than gated in `transitions` alone (F8).
  Offline re-analysis of the two saved runs is in §Live-run results —
  2026-09-02.
- ~~**Gate 4 green pass — remaining step: one live re-run.**~~ **Run
  2026-09-03, three times** (§Live-run results — 2026-09-03): the
  pre-Phase-F open-gate underrun is gone (`tx_underrun_frames` 0 on every
  run), the run surfaced and reproduced the RX pool corruption (fixed
  `baef793d0063`) and one tool defect (fixed), and the double service reset
  recurs on either blip. **Owner ruled (b); gate 4 GREEN on run 5**
  (`verdict pass`, digest `0cf5eeda…`). The replay-start open-gate underrun
  run 4 caught is fixed (replay gate-open floor, §Live-run results —
  2026-09-03, replay floor: two more green runs, zero underruns across
  three replay starts); gate 4 is repeatable on the board side. The
  remaining flake — the tool's 2 s phase against the 44.1 kHz acceptance
  latency — is fixed in the tool (§Live-run results — 2026-09-03, replay
  floor, *Stimulus fix*: `--phase-seconds 2,3,1.5` default, per-rate ring
  budget, acceptance-gated freeze) and proved live 2026-09-03: one
  `--with-replay` run at the new defaults, `verdict pass` first time
  (digest `5a8ff1aa…`). Gate 4 has no open item.
