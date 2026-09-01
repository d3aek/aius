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

## Two pinned design choices

- **Gate 2's rate is `audio_rate_hz`, inferred not decoded.** The U24 XL
  exposes no IEC958 capture-status control (`amixer -c0 controls` — none
  present), so the incoming S/PDIF channel-status word is not readable via
  ALSA on this interface. The rate is inferred from the captured tone
  frequency (`rate ≈ expected_rate × f_measured / 997`); since that is an
  audio-rate inference and not a channel-status decode, the field is named
  `audio_rate_hz` (design decision 2026-09-01), with
  `audio_rate_hz_source: "inferred-from-tone"` in the evidence. A true decode
  needs a dedicated S/PDIF protocol analyzer.

- **Capture↔status correlation uses bounded local alignment.** Each status
  counter increment (`trim_updates`, `invalid_windows`) is matched to the
  nearest capture event inside a drift-padded wall-time window around the
  poll interval, not a global frame→wall-time map (which accumulates
  output-vs-capture clock-drift error over a 600 s run). `ALIGNMENT_MARGIN_S`
  bounds the padding.

## Gate 4 — flight-recorder capture

`old_rate_output_frames` cannot be measured through the U24 ALSA path: the
capture clock locks to the incoming carrier (a fixed-rate `hw:0,0` arecord
errors when the carrier rate changes) and `plughw` resampling would preserve
997 Hz across every rate, erasing the signature. `recorder_transitions.py`
uses the board's flight recorder (`/api/recorder`) instead — it stages the
board's own OUTPUT frames (s32le/24-bit) into a PSRAM ring WITHOUT
resampling, so reading that flat stream at a fixed 48 kHz reference makes the
output rate visible as the tone frequency: a 997 Hz tone output at 44.1 kHz
reads 997 × 48000/44100 = 1085 Hz (calibrated live), at 48 kHz reads 997 Hz.
The reducer classifies each block's output rate and counts stale old-rate
frames trailing a switch; a clean board yields 0.

```bash
python3 recorder_transitions.py \
    --tone-48 /tmp/tone-997-48000.wav --tone-44 /tmp/tone-997-44100.wav \
    --output <outdir> --expect-build fc674c60f06c+teensy41-optical
```

It drives 48→44.1→48 via aplay while polling `/api/status` and `/api/events`
in a background thread (for `evidence.jsonl`), operator-freezes the recorder
(the captured slot is the one just behind the advanced active pointer), reads
it, and reduces. Live result 2026-09-01: output-rate runs
`[48000, 44100, 48000]`, `old_rate_output_frames = 0` — the board transitions
cleanly. A full bench PASS additionally needs the loss→replay leg (USB
deauth), which the status poller spans over the network.

## Usage

Generate the 997 Hz stimulus with the source-qualification tone generator:

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
    --tone /tmp/tone-997-44100.wav --output <outdir> \
    --status-url http://192.0.2.66/api/status \
    --expect-build fc674c60f06c+teensy41-optical
python3 ../net/aius_rate_bench.py analyze pcm-120s --evidence <outdir>/evidence.jsonl
```

A single capture run owns the board: it drives the stimulus, records the
waveform, and polls `/api/status` and `/api/events` in lockstep. It writes
three artifacts to `<outdir>`:

- `external-observation.json` — the exact per-gate fields.
- `evidence.jsonl` — a bench-format evidence file (schema 1) of the same
  run's status/events samples with the external observation embedded, so
  `aius_rate_bench.py analyze` reaches a verdict offline. This avoids two
  processes polling the one-client-at-a-time board concurrently and the
  read-external-at-start race of `run`.
- `capture-analysis.json` — the full supporting evidence (zero-run lengths,
  discontinuity frames, correlation counts).

`--expect-build` pins the gate to the exact deployed `build_revision`.

## Tests

`../../test/rate-gate-capture/test_capture.py` drives the reducers against
synthetic gap-injected, 512-run-injected and frequency-step waveforms:

```bash
python3 ../../test/rate-gate-capture/test_capture.py
```
