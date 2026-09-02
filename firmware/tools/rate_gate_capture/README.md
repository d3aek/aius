# Rate-gate capture — board optical output reducer

Host-side tool that produces the `--external-observation` JSON three of
`aius_rate_bench.py`'s sample-rate acceptance gates need. The bench is
read-only over HTTP and cannot self-report properties of the board's actual
optical output; this tool records board optical OUT → U24 optical IN via
`arecord`, drives the scenario stimulus via `aplay`, polls `/api/status` in
lockstep, and reduces the waveform to the exact per-gate fields.

Design and provenance:
[`../../../docs/plans/aius-rate-gate-capture-tool.md`](../../../docs/plans/aius-rate-gate-capture-tool.md).
The capture chassis mirrors
[`../source_qualification/direct_loop_bracket.py`](../source_qualification/direct_loop_bracket.py);
the gate reducers and their JSON schema are new.

## Fields produced per gate

| Gate | Scenario | external-observation.json |
|---|---|---|
| 2 | `pcm-120s` | `audio_rate_hz`, `continuous_output` |
| 4 | `transitions` | `old_rate_output_frames` |
| 7 | `waveform` | `capture_frame_gaps`, `discontinuities_outside_invalid`, `invalid_intervals_frames`, `correlated_trim_updates`, `correlated_invalid_windows` |

A hard capture fault adds `capture_failure` (a list of markers) and
neutralises the scenario's gate-critical field — see §Capture health.

## Gate 2 — the rate model, and when it measures nothing

**The physical model is `R_out = 997 × R_capture / f_measured`.** Frames the
board rendered at `R_out`, read back at `R_capture`, carry
`997 × R_capture / R_out` Hz; the output rate is recovered by inverting that.
(The inverse form `R_expected × f_measured / 997` reports 52245 Hz for a
44.1 kHz stream read at 48 kHz, and was the defect this replaced.)

The raw estimate is snapped to the nearest `NOMINAL_RATES` entry
(32000/44100/48000/88200/96000) within `NOMINAL_RATE_TOLERANCE` (2 %). Both
numbers reach the evidence: `audio_rate_hz_raw` is the estimate,
`audio_rate_hz` is the snapped value, and `capture_rate_hz` is the rate
`arecord` was actually opened at. An estimate no nominal rate is close to is
reported as `null` rather than rounded, so gate 2's exact-equality check
cannot pass on a rate nothing ran at.

**The inference is self-referential whenever `arecord` is opened at the rate
under test.** The U24 capture clock locks to the carrier under test, so
`f_measured` is 997 Hz *by construction* and the reported rate is just the
rate `arecord` was opened at — the measurement carries no independent
information about the board's output rate. The evidence says so explicitly:

```json
{
  "audio_rate_hz_source": "capture-clock-locked",
  "self_referential": true,
  "rate_measurement_note": "audio_rate_hz is capture-clock-locked, not independently measured: ..."
}
```

A gate-2 pass on a self-referential run is therefore **not** evidence that
the output ran at the declared rate; it only says the capture did not fail
and the output was continuous. An independent reading needs `--rate`
different from `--expected-rate` (the tone then genuinely carries the rate),
or a channel-status decode. The U24 XL exposes no IEC958 capture-status
control (`amixer -c0 controls` — none present), so a true decode needs a
dedicated S/PDIF protocol analyzer; that is why the field is named
`audio_rate_hz` rather than `channel_status_rate_hz` (design decision
2026-09-01).

## Gate 7 — two time origins and an amplitude-derived detector

**Capture↔status correlation uses bounded local alignment.** Each status
counter increment (`trim_updates`, `invalid_windows`) is matched to the
nearest capture event inside a drift-padded wall-time window around the poll
interval, not a global frame→wall-time map (which accumulates
output-vs-capture clock-drift error over a 600 s run). `ALIGNMENT_MARGIN_S`
bounds the padding at 1.5 s.

**Two origins separate the status domain from the capture domain and both are
subtracted.** For the board→U24 loop, `aplay` must start and the board must
lock to the target rate *before* `arecord` opens, so the run clock is up to
`lock_budget_s` (20 s) ahead of capture frame 0; and `active_bounds()` then
trims the silent lead-in, so active frame 0 is further in again. The tool
takes its capture origin from the moment `arecord`'s WAV first grows past its
header (`capture_start_offset_s`, `capture_start_source`) and passes both
offsets into the alignment. Dropping either offsets every increment by
seconds and nothing inside a 1.5 s margin can ever correlate.

**The discontinuity detector is derived from the measured stimulus, never
from a fixed LSB floor.** `tone_gen.py` defaults to peak ±410, whose largest
adjacent-sample jump at 997 Hz / 48 kHz is only ~54 LSB — an absolute floor
above that is blind to every artifact by construction. Three signatures, all
scaled from the measured amplitude `A` and the tone's own peak slope
`S = 2A·sin(πf/Fs)`:

- **Held block** — a run of near-zero first difference against a
  known-nonzero expected slope (`held_slope_floor`, `held_min_frames`). This
  is what a repeated / sample-and-hold PCM block looks like; its interior is
  flat and no amplitude threshold sees it.
- **Spike / splice edge** — an adjacent jump over `JUMP_SLOPE_FACTOR × S`.
- **Sample slip** — a step in the fitted tone's phase. A dropped or repeated
  sample steps the demodulated phase by `2πf/Fs` wherever in the cycle it
  lands, which is why 1- and 2-sample slips near the waveform's turning
  points are invisible to any amplitude test.

Every threshold plus `stimulus_amplitude` and `expected_peak_slope` is
written into `capture-analysis.json`, so a reader can see what the detector
was actually able to see on that run.

## Capture health

`arecord`'s exit status and its log are inspected, not discarded:

- ALSA `overrun`/`xrun` notices in `arecord.log` are counted
  (`alsa_overruns`) and added to `capture_frame_gaps` — they are capture
  frame gaps by definition.
- A non-zero `arecord` exit, or a WAV shorter than `--seconds` by more than
  `CAPTURE_SHORTFALL_TOLERANCE_S`, is a hard failure: the tool records the
  marker in `capture_failure`, neutralises the scenario's gate-critical
  field (`audio_rate_hz: null` + `continuous_output: false` for gate 2,
  extra `capture_frame_gaps` for gate 7), prints the failure to stderr and
  exits 2.

## Gate 4 — flight-recorder capture with a fiducial

`old_rate_output_frames` cannot be measured through the U24 ALSA path: the
capture clock locks to the incoming carrier (a fixed-rate `hw:0,0` arecord
errors when the carrier rate changes) and `plughw` resampling would preserve
997 Hz across every rate, erasing the signature. `recorder_transitions.py`
uses the board's flight recorder (`/api/recorder`) instead — it stages the
board's own OUTPUT frames (s32le/24-bit) into a PSRAM ring WITHOUT
resampling, so the recorded frames carry the output rate directly as
**samples per cycle** of the 997 Hz tone: 48.14 at 48 kHz, 44.23 at
44.1 kHz, 32.10 at 32 kHz, whatever rate they are read back at. A block at a
third rate is *counted at that rate*, never discarded as ambiguous.

**The measurement needs a fiducial, and run-length analysis cannot substitute
for one.** Stale old-rate frames emitted after a switch are contiguous with
the pre-switch run and carry the same samples-per-cycle, so run-length
analysis merges the two and always answers 0 whatever the board did. Gate 4
asks for a *distance*, and a distance needs an origin the board declares:

```text
wav_index              = fiducial − (stream_end_frame − valid_frames)
old_rate_output_frames = Σ over converted fiducials of
    max(0, (frame where samples-per-cycle actually changes) − wav_index)
```

The fiducial is the recorder header's per-slot `slots[].rate_change_frames`
(up to 4) with `slots[].rate_change_count`. **Those are STREAM indexes** —
frames staged since the slot armed (`aius_recorder.h:163-171`) — **not ring
indexes.** The pulled array is a different index space: `read_slot` unrolls
the ring from `oldest_frame`, so it runs `0..valid_frames-1` with 0 = the
oldest *retained* frame. `slots[].stream_end_frame` (frames staged at
POSTROLL exit) gives the origin, and the conversion is the subtraction above.

A fiducial converting outside `[0, valid_frames)` rolled off the front of the
ring before the freeze; it is dropped and listed in `fiducials_rolled_off`, so
a ring too short for the transition is visible in the evidence instead of
silently shrinking the measurement.

The reducer falls back to run-length analysis — `"method": "run-length (no
fiducial)"`, `"lower_bound": true` — in three cases: the header carries no
fiducial at all; it carries fiducials but no `stream_end_frame` (their origin
is then unknown, and guessing one would fabricate the answer); or every
fiducial rolled off. **That number is a lower bound, not a measurement**, and
a 0 from it is not a clean pass.

**Zero captured slots is a failure, not a clean 0.** The reducer emits
`old_rate_output_frames: null`, `captured_slots: 0` and
`failure: "no-transition-captured"`; the bench's gate-4 check rejects the
non-integer ("old_rate_output_frames must be an unsigned integer") rather
than reading a vacuous pass, and the tool exits 2.

```bash
python3 recorder_transitions.py \
    --tone-48 /tmp/tone-997-48000.wav --tone-44 /tmp/tone-997-44100.wav \
    --phase-seconds 2 --expect-build <revision>+teensy41-optical
```

It drives 48→44.1→48 via aplay while polling `/api/status` and `/api/events`
in a background thread (for `evidence.jsonl`), operator-freezes the recorder,
reads the captured slot and reduces. Operational contracts:

- **The captured slot is the header's `newest_frozen_slot`** (fallback: the
  frozen slot with the highest `freeze_generation`), and its
  `(slot, freeze_generation)` identity must not already have been frozen at
  the pre-run baseline. `freeze_generation` is **per slot** (it counts that
  slot's thaws and starts at 0), so a generation compared across slots says
  nothing: on a fresh boot every frozen slot reads 0, and the earlier
  "newer than the baseline generation" check rejected every real capture.
  The evidence records `baseline_frozen_identities` and `captured_trigger`.
  The old `(active_slot − 1) % slot_count` guess assumed the bank's
  auto-advance had landed within a 0.6 s sleep, which is a race, not a fact.
- **Phases are played by frame count, not whole seconds.** `aplay -d`
  truncates `--phase-seconds 1.2` to 1 s, which is shorter than the board's
  follow latency: the board sits in `unsupported` for the whole 44.1 kHz
  phase and stamps no rate change (observed 2026-09-02). The tool reads each
  tone's rate from its WAV header and passes `aplay --samples`; the frame
  counts are in the evidence as `phase_frames`.
- **A phase the board never followed is named as such.** The evidence
  carries `stream_rate_timeline` (every change of `clock.stream_rate_hz`
  across the polls) and `stimulus_followed`; when no slot was captured and
  the polls never saw 44100, `failure_reason` is `stimulus-not-followed`
  (lengthen `--phase-seconds`), distinct from a freeze that failed to land.
- **The run records the phase sequence it actually drove.** The evidence
  metadata carries `analysis_options.expected_phases` — `["48000", "44100",
  "48000"]`, plus `"replay"` only under `--with-replay` — and the bench's
  `analyze` reads it back when `--expected-phases` is absent. Without it the
  analyzer asserts the historic four-phase sequence and a run that never drove
  the loss→replay leg fails `transition-sequence` for a phase nobody played.
- **Ring capacity is checked against what the measurement needs.** The
  frames from just before the *first* rate boundary to the freeze must
  survive: `--phase-seconds × 2 + 1 s margin` against the slot's
  `ring_frames` (the first phase's body is not needed, only its last block).
  The tool refuses (exit 3) unless `--allow-rolloff`. Against the board's
  262144-frame slot this admits 2 s phases (240000 frames).
- **A fiducial with no measurable change is a lower bound, not a 0.** When
  the output never changes within the search window after a declared rate
  change (or the pre-fiducial rate cannot be classified), the slot lists it
  in `unresolved_fiducials` and the evidence marks `lower_bound: true`.
- **The recorder config is restored.** `disable_auto_triggers` mutates the
  board's *persistent* recorder config; the previous config is read from the
  header before the run and re-PUT in a `finally`.
- **`clock.event_overflows` is partly this tool's own poll cadence.** Events
  the board rings out between two polls are lost and counted as an overflow.
  The poll interval is 0.25 s and is recorded as
  `status_poll_interval_s`; a `harmful-counter` failure citing
  `event_overflows` **alone** is tool-induced, not a board defect, and the
  evidence carries that sentence verbatim in `event_overflows_note`. Since
  operator ruling (d) (2026-09-02) the bench excludes `event_overflows` from
  the harmful gate **in `transitions` only** and reports it as
  `metrics.event_overflows_delta`; every other scenario still fails on it,
  and `unsupported-event-count` remains the guard against lost events
  breaking attribution.

## Usage

Generate the 997 Hz stimulus with the source-qualification tone generator
(default peak amplitude 410 — the level the detector thresholds are proven
at):

```bash
python3 ../source_qualification/tone_gen.py --rate 44100 --seconds 130 --output /tmp/tone-997-44100.wav
```

Re-apply the U24 mixer every session (it resets on re-enumeration):

```bash
amixer -c 0 cset numid=8 1        # PCM Capture Source = IEC958 In
amixer -c 0 cset numid=5 110,110  # PCM Playback Volume = 0 dB
```

Run a gate capture, then hand the verdict to the bench's offline analyzer:

```bash
python3 capture.py --scenario pcm-120s --rate 44100 --seconds 130 \
    --tone /tmp/tone-997-44100.wav \
    --status-url http://192.0.2.66/api/status \
    --expect-build <revision>+teensy41-optical
python3 ../net/aius_rate_bench.py analyze pcm-120s --evidence <outdir>/evidence.jsonl
```

A single capture run owns the board: it drives the stimulus, records the
waveform, and polls `/api/status` and `/api/events` in lockstep.

## Evidence retention

`--output` defaults to a timestamped directory under
`firmware/test/evidence/rate-gate-capture/` (gitignored, so a run's artifacts
never enter a commit). Both tools print the evidence file's absolute path and
its SHA-256 on completion:

```text
evidence: /.../firmware/test/evidence/rate-gate-capture/pcm-120s-20260902T101500Z/evidence.jsonl
evidence-sha256: <64 hex>
```

**Cite that digest whenever a live result is recorded in a plan.** A result
with no digest is an unverifiable claim.

Each run directory holds:

- `external-observation.json` — the exact per-gate fields.
- `evidence.jsonl` — a bench-format evidence file (schema 1) of the same
  run's status/events samples with the external observation embedded, so
  `aius_rate_bench.py analyze` reaches a verdict offline. This avoids two
  processes polling the one-client-at-a-time board concurrently and the
  read-external-at-start race of `run`.
- `capture-analysis.json` — the full supporting evidence (rate model and
  self-referential marking, detector thresholds and measured amplitude,
  zero-run lengths, discontinuity frames, correlation counts, capture
  health).
- `capture.wav`, `arecord.log`, `aplay.log` — the raw capture and its ALSA
  logs.

`--expect-build` pins the gate to the exact deployed `build_revision`.

## Tests

`../../test/rate-gate-capture/test_capture.py` drives the real reducers
against synthetic gap-injected, 512-run-injected, held-block, sample-slip and
rate-step waveforms, plus the flight-recorder fiducial fixtures:

```bash
scripts/test.sh --environment native --suite rate-gate-capture
```
