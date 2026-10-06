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
`invalid_windows` increment (a full apply) is matched to the nearest capture
event inside a drift-padded wall-time window around the poll interval, not a
global frame→wall-time map (which accumulates output-vs-capture clock-drift
error over a 600 s run). `ALIGNMENT_MARGIN_S` bounds the padding at 1.5 s.

**Only full applies open match windows.** Trim and fill writes are
carrier-continuous numerator steps and never excuse a jump: at fill cadence
their windows would cover the whole capture. `correlated_trim_updates` keeps
its name but counts jumps matched to a full apply. The bench still sees every
write through the `trim_applied_at_ms` / `fill_applied_at_ms` stamp changes
(`trim-not-observed`; a sample lacking a stamp is skipped, and the stamp
fallback to `trim_updates` applies only when no sample carries it), checks
`invalid_windows` delta at least `structural_replans` delta (`invalid_windows`
rises on every successful apply: nominal accept, fault retry, replay apply;
`structural_replans` only on a replan), and asserts `fill_trims` delta at most
`fill_window_seq` delta plus 1 (`fill-cadence`). Gate 1's trim-cadence is
advisory per design decision D4 (b): the receiver's ~140 mHz estimate quantum
makes at most 2 post-settle commits unmeetable (about 52 are expected on a
healthy 10 min run). The bench records `centre_commits_after_settle`
(`trim_updates` delta minus `structural_replans` delta, else `trim_updates`),
`centre_writes_after_settle` and `trim_cadence_advisory` as metrics and never
fails on them.

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
    --with-replay --expect-build <revision>+teensy41-optical
```

It drives 48→44.1→48 via aplay (`--phase-seconds first,middle,last`, default
`2,3,1.5`; a single value applies to all three) while polling `/api/status`
and `/api/events` in a background thread (for `evidence.jsonl`), freezes the
recorder once the board is seen tracking 48 kHz again, reads the captured
slot and reduces. Operational contracts:

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
  tone's rate from its WAV header and passes `aplay --samples`; the evidence
  carries `phase_seconds_per_phase`, `phase_tones`, `phase_rates_hz` and the
  per-phase `phase_frames` list (`phase_seconds` keeps its scalar meaning
  and is `null` when the phases differ).
- **The middle phase must outlast the board's acceptance latency by three
  polls.** After a carrier rate switch the board sits `unsupported` at the
  old stream rate for 1.05–1.85 s (48→44.1, 2026-09-02/03 runs) before it
  tracks the new one, and the bench labels a phase from `tracking` polls
  only with `phase_samples 2` — two polls are guaranteed inside a tracking
  window only once it exceeds two 0.25 s intervals. The default 3 s middle
  clears `ACCEPTANCE_LATENCY_BUDGET_S` (1.85 s) by that rule; the old 2 s
  lost it by 0.15 s on the 2026-09-03 run 1 (`observed ['48000', 'replay']`).
- **The freeze is gated on acceptance, not on the trailing phase length.**
  After the trailing aplay returns the tool waits (≤ 1 s) for a poll stamped
  since the switch back showing the board tracking 48 kHz — the second
  fiducial is then stamped — dwells 0.3 s and freezes; on timeout it freezes
  anyway. The U24 keeps the carrier up after aplay exits, so a 1.5 s trailing
  phase is enough. The decision is in the evidence as
  `switch_back_elapsed_s`, `reference_tracking_seen_elapsed_s`,
  `freeze_elapsed_s` and `freeze_trigger`.
- **A phase the board never followed is named as such.** The evidence
  carries `stream_rate_timeline` (every change of `clock.stream_rate_hz`
  across the polls) and `stimulus_followed`; when no slot was captured and
  the polls never saw 44100, `failure_reason` is `stimulus-not-followed`
  (lengthen the middle value of `--phase-seconds`), distinct from a freeze
  that failed to land.
- **The run records the phase sequence it actually drove.** The evidence
  metadata carries `analysis_options.expected_phases` — `["48000", "44100",
  "48000"]`, plus `"replay"` only under `--with-replay` — and the bench's
  `analyze` reads it back when `--expected-phases` is absent. Without it the
  analyzer asserts the historic four-phase sequence and a run that never drove
  the loss→replay leg fails `transition-sequence` for a phase nobody played.
- **The evidence ends with the declared stimulus.** Under `--with-replay` the
  poller is stopped and joined BEFORE the U24 is re-authorized: the restore is
  bench housekeeping, not a driven phase, and its re-enumeration edge (four
  garbage-rate `unsupported` events, the interrupted replay unit abandoned once
  with `pipeline_last_failure` `TRUNCATED` — `MALFORMED` on images before
  2026-09-03) is a replay→live transition `expected_phases` never declares.
  Polled, the analyzer judged it as `transition-unattributed-unsupported` and
  a `harmful-counter` on `au_abandons` (live run 1, 2026-09-03).
- **Ring capacity is checked against what the measurement needs.** The
  frames from just before the *first* rate boundary to the freeze must
  survive. The recorder stages the board's output frames at the output rate
  and only while the board is tracking (an `unsupported` board stages
  nothing — live 2026-09-03: 8192 frames of 44.1 kHz in the slot on the run
  whose polls showed 0.26 s of tracking, ~37700 on the runs that showed
  0.78 s), so the budget is `middle_s × 44100 + last_s × 48000 + 1 s margin
  × 48000` for the instant-acceptance worst case; the lead-in's body is
  free (only its last classify block is read). The tool refuses (exit 3)
  unless `--allow-rolloff`. Against the board's 262144-frame slot the
  default `2,3,1.5` needs 252300 frames; symmetric 2 s phases need 232200;
  symmetric 3 s phases (324300) and a 3 s middle with a 2 s trailing phase
  (276300) do not fit.
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

## Read-lease-deferred recycle — `lease_deferral_probe.py`

Device evidence for
[`../../../docs/plans/aius-recorder-rolling-buffer.md`](../../../docs/plans/aius-recorder-rolling-buffer.md)
Phase D item 2, **deferral half**. Item 2's *release* half is proven on device;
the deferral half needs a read lease held **open across a carrier drop**, and
no sequence of HTTP verbs produces that from outside. The board serves one
transaction at a time, so the client holding the lease is the only client and
cannot also drive the drop. This tool is a single process for that reason: no
tool round-trip sits inside the lease window, and the carrier drop is a local
sysfs write rather than another request the board would have to answer first.

`slow_reader.py` is the lease holder. It issues one
`GET /api/recorder/data` and refuses to finish reading the 200: a tiny
`SO_RCVBUF` keeps the board's TCP send window nearly closed, and the board's
`RESPONSE_TIMEOUT_MS` (5000) deadline is re-armed on every completed write, so
a trickle read keeps the transaction — and the addressed slot's read lease —
alive until the process is killed. It counts and discards the bytes; use
`aius-recorder-pull.py` to actually retrieve a capture.

```bash
python3 lease_deferral_probe.py \
    --target http://192.0.2.66 \
    --evidence <outdir>/lease-deferral.json
```

The candidate slot is the frozen slot the next recycle would take,
`(active_slot + 1) % slot_count`. The sequence is: header before → start the
reader on the candidate → de-authorize the U24 after `--hold-before-drop` (6 s)
→ hold `--hold-after-drop` (15 s) while the bank defers → kill the reader →
header after release → re-authorize → header after `--relock-wait` (12 s) →
`/api/status`. One JSON line is printed per step and the whole record is
written to `--evidence`.

**Expected counters.** While the lease is held:

- `deferred_epochs` and `lease_skips` **advance** — the carrier drop fires the
  `carrier_loss` trigger, which freezes the active slot; the bank then wants to
  recycle the leased candidate and defers instead, skipping past it.
- `lease_leaks` stays **0**. A non-zero value is the read-lease leak Phase D's
  own review found and closed, not a property of the deferral.
- The candidate slot's `freeze_generation` is **unchanged until the lease is
  released**. This is the actual claim: it says the leased capture survived the
  recycle the bank wanted to perform. The evidence carries it as
  `candidate_generation_before` / `_after_release` / `_after_relock`.

A candidate slot that is not frozen means there is nothing for the bank to
defer over, so the tool exits **2** rather than reporting a vacuous pass.

**The U24 is always re-authorized**, including on an exception path (a
`finally`). A tool that exits leaving the bench source de-authorized silently
takes the room's audio down and every later run reads a carrier that is not
there.

**Restore the mixer after every run.** U24 re-enumeration resets its PCM
playback volume to 70,70 and every bench here assumes unity:

```bash
amixer -c 0 cset numid=5 110,110
```

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
