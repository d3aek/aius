---
title: Aius rate-gate external capture tool
type: plans
scope: component
component: aius
status: in-progress
subtype: design
tags: [aius, firmware, teensy, audio, spdif, acceptance, tooling]
updated: 2026-09-02
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
A 2026-09-02 review found nine confirmed defects in the shipped tool; they are
fixed and unit-tested, but every live result in this document was produced
before the fixes and none of it is evidence until re-run.

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
  green (53 tests as of 2026-09-02; it was 24 when this line was first
  written). Coupled blocker #1 below is cleared.
- **Shipped: the capture tool** `firmware/tools/rate_gate_capture/capture.py`
  plus `firmware/test/rate-gate-capture/test_capture.py`. It implements the
  CLI/orchestration, all tractable reducers (`continuous_output`,
  `capture_frame_gaps`, `invalid_intervals_frames`, discontinuity detection),
  bounded-local-alignment correlation (`correlated_trim_updates`,
  `correlated_invalid_windows`, `discontinuities_outside_invalid`), the
  inferred gate-2 rate, and `old_rate_output_frames` via the flight recorder.
  Build-sequence steps 1–5 are done. The suite is **56 tests green**
  (2026-09-02, `scripts/test.sh --environment native --suite
  rate-gate-capture`); it was 19 before the review fixes.
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
- **Live integration (step 6) — gate 2 PASS, gate 7 FAIL (real verdict),
  gate 4 blocked on redesign.** Owner authorised running all gates.

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
green (24 tests when this was written; 53 as of 2026-09-02).

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

## Acceptance for this tool

- The three reducer field-groups are unit-tested against synthetic WAVs.
  **Done** — 56 tests in `firmware/test/rate-gate-capture/` (2026-09-02;
  19 before the review fixes), including three that round-trip a synthetic
  `evidence.jsonl` through the real bench analyzer and two that drive the real
  bench `transitions` check. Run as `scripts/test.sh --environment native
  --suite rate-gate-capture`.
- Each of gates 2, 4, 7 reaches a `pass`/`fail` verdict end-to-end on the live
  board with the produced JSON (a real verdict, not a tool error), on one exact
  committed image with `--expect-build`. **All three reached a real verdict on
  2026-09-01, but with the defective tool — NOT MET pending a re-run.** The
  recorded verdicts (gate 2 pass, gate 7 fail, gate 4 fail with
  `old_rate_output_frames = 0`) are invalidated by F1/F2/F3/F5 as detailed in
  §Live-run results, and no evidence artifact exists for any of them.
- The transitions descope lands with its tests green in the same change set.
  **Done** (aius `93610f6`).
- Every recorded live result cites an `evidence.jsonl` path and SHA-256.
  **Not met for the 2026-09-01 runs** (§Evidence retention).

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

Nine confirmed findings against the shipped tool. All are fixed and
unit-tested off-board; none has been re-proven on the board.

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

## Follow-on work (owner input needed)

- **Re-run gates 2, 4 and 7 with the fixed tool** on one exact committed
  image, and record each verdict with its evidence digest. Everything below
  depends on that.
- **Gate 7 contract.** Decide whether the board's invalid windows are meant to
  be externally observable at all. If not, redefine or drop gate 7's external
  fields; if so, specify the artifact signature the tool should detect. The
  2026-09-01 evidence for "they are not observable" is retracted (F3).
- **Gate 2 scope.** Decide whether a capture-clock-locked (self-referential)
  `audio_rate_hz` satisfies gate 2, or whether the gate requires an
  independent reading (capture rate ≠ expected rate, or a protocol analyzer).
- **Gate 4 `old_rate_output_frames` fiducial — reference chosen, measurement
  pending firmware.** The flight recorder is the reference
  (`recorder_transitions.py`), but the measurement needs **both** new per-slot
  fields in `GET /api/recorder`: `slots[].rate_change_frames` (with
  `rate_change_count`) and `slots[].stream_end_frame`, which supplies the
  origin that converts those stream indexes into pulled-frame indexes. Until
  the firmware emits both the reducer reports a lower bound, and the
  previously recorded "measured 0 live" is retracted (F5).
- **Gate 4 green pass.** Reached a real fail verdict end-to-end. A pass needs
  either a stimulus source that cuts the S/PDIF carrier cleanly between rate
  phases (a programmable generator; the aplay-on-U24 idle at ~48.02 kHz
  between phases produces the unsupported/underrun artifacts that trip the
  analyzer), or an owner decision to adapt the `transitions` analyzer to the
  aplay-blip stimulus — without weakening any acceptance threshold. Note that
  the recorded `harmful-counter` failure cited `event_overflows`, which is
  partly the tool's own poll cadence (F8).
