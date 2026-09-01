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
| 2 | `pcm-120s` | `channel_status_rate_hz`, `continuous_output` |
| 4 | `transitions` | `old_rate_output_frames` |
| 7 | `waveform` | `capture_frame_gaps`, `discontinuities_outside_invalid`, `invalid_intervals_frames`, `correlated_trim_updates`, `correlated_invalid_windows` |

## Two pinned design choices

- **`channel_status_rate_hz` is inferred, not decoded.** The U24 XL exposes
  no IEC958 capture-status control (`amixer -c0 controls` — none present), so
  the incoming S/PDIF channel-status word is not readable via ALSA on this
  interface. The rate is inferred from the captured tone frequency
  (`rate ≈ expected_rate × f_measured / 997`) and the field carries a
  `channel_status_rate_hz_source: "inferred"` marker. A true decode needs a
  dedicated S/PDIF protocol analyzer.

- **Capture↔status correlation uses bounded local alignment.** Each status
  counter increment (`trim_updates`, `invalid_windows`) is matched to the
  nearest capture event inside a drift-padded wall-time window around the
  poll interval, not a global frame→wall-time map (which accumulates
  output-vs-capture clock-drift error over a 600 s run). `ALIGNMENT_MARGIN_S`
  bounds the padding.

`old_rate_output_frames` uses a provisional tone-frequency-shift detection
model that needs live calibration against the board to confirm the stale-rate
signature (the U24 capture path's rate-lock behaviour is not yet characterised
off-board).

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

Run a gate capture, then feed the JSON to the bench:

```bash
python3 capture.py --scenario pcm-120s --rate 44100 --seconds 130 \
    --tone /tmp/tone-997-44100.wav --output <outdir> \
    --status-url http://192.0.2.66/api/status
aius_rate_bench.py run pcm-120s --external-observation <outdir>/external-observation.json
```

`external-observation.json` carries the exact gate fields;
`capture-analysis.json` carries the full supporting evidence (zero-run
lengths, discontinuity frames, correlation counts).

## Tests

`../../test/rate-gate-capture/test_capture.py` drives the reducers against
synthetic gap-injected, 512-run-injected and frequency-step waveforms:

```bash
python3 ../../test/rate-gate-capture/test_capture.py
```
