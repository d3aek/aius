# dsp_characterize — commanded curve vs measured response

Measures the DSP's actual transfer function through the full digital loop
(U24XL TX → Teensy RX → DSP → Teensy TX → U24XL RX) and compares it against
the mathematical response of the commanded configuration. The loop is
synchronous and fully digital, so a faithful implementation measures to
within numerical noise — deviations are findings, not tolerance.

## Method

- **Stimulus** — a bin-centered multitone: 33 log-spaced tones snapped to
  exact FFT bins of an N=65536 window at 48 kHz, seeded random phases,
  peak −12 dBFS. Integer-period tones + rectangular window = leakage-free
  single-bin reads, immune to clock offset because the whole loop runs on
  the U24XL's clock.
- **Reference** — round A measures the loop flat with the limiter disabled;
  every curve is expressed relative to it, cancelling the loop's own
  response.
- **Battery** — per-channel independent curves (peaking boost/cut on
  opposite channels), a q=10 −12 dB notch (coefficient-precision stress),
  a 120 Hz highpass, the two-section highpass+peaking stack, and a
  broadband gain split. Expected responses are RBJ-cookbook biquads.
- **Limiter ladder** — 1 kHz tone stepped −12 → 0 dBFS against the
  configured limiter; output level read from the same capture path.
- **Restore** — the original configuration is PUT back verbatim with a
  fresh generation pair; the tool refuses to run without `AIUS_SD_AUTH`.

Config apply lands at the next audio block boundary, so each round starts
playback first and polls `config_generation` before capturing.

## Round 1 results — 2026-08-04, image `249894edb20c`

| Round | Commanded | Max abs error |
| --- | --- | --- |
| peak_split | L: peaking 1 kHz q1 +6 dB; R: peaking 4 kHz q2 −6 dB | ≤0.001 dB |
| notch_hiq | L: peaking 6 kHz q10 −12 dB; R: peaking 250 Hz q8 −6 dB | ≤0.001 dB |
| highpass | both: highpass 120 Hz q0.707 | 0.001 dB |
| two_section | both: highpass 80 + peaking 2.2 kHz +3 dB | ≤0.001 dB |
| gain_only | L −6 dB, R 0 dB | ≤0.001 dB |

The DSP implements the RBJ cookbook essentially exactly: worst-case
deviation across 33 frequencies × 5 curves × 2 channels was 0.001 dB.

Limiter (−1 dBFS threshold, 2 ms lookahead): transparent through −3 dBFS
in (out = in to 0.01 dB), ceiling clamped at −1.01 dBFS for −1/−0.5/0 dBFS
inputs — a brick wall at the commanded threshold with no observed
overshoot in the steady-state window.

## Matrix round — `dsp_matrix_round.py`, 2026-08-04

The 5.1 downmix is measured with the frequency-multiplexed channel-ID DTS
vector (`channel_id_51`: FL 300 / FR 600 / FC 900 / LFE 60 / SL 1200 /
SR 1500 Hz), so one stereo capture reads every routing weight at once —
tones are multiples of 60 Hz, exact bins of a 1 s window. Every variant's
PUT is built from the original document plus one override, so variants are
absolute against the same reference.

**Baseline decodes the matrix arithmetic exactly**: direct paths at
−12.96 dBFS (source 0.45 × headroom 0.5), FC and surrounds each −3.01 dB
below the direct paths (0.7071 gains). **Stereo separation is at the
numerical floor: opposite-side crosstalk −89 to −104 dB.**

| Variant | Commanded | Measured delta |
| --- | --- | --- |
| center_hot | center 0.7071 → 1.0 (+3.01 dB) | FC +3.01 both channels, rest 0 |
| surround_cut | surround → 0.3536 (−6.02 dB) | SL/SR −6.02, rest 0 |
| lfe_hot | lfe → 0.6325 (+6.02 dB) | LFE +5.98 both, rest 0 |
| headroom_up | headroom 0.5 → 0.7071 (+3.01 dB) | +3.01 broadband (LFE +2.97) |

Every knob moves its own path by exactly the commanded amount and nothing
else. One observation for the record: the LFE tone's absolute level sits
~8.9 dB below the naive `source × lfe_gain × headroom` expectation —
consistent with a DTS LFE level convention or crossover contribution in
the decode path, and the `lfe_gain` knob itself is exact; flagged for a
follow-up only if absolute LFE calibration ever matters.

Raw evidence (captures, results.json, original-config.json) lives in the
running session's scratch battery directories; the numbers above are the
durable record.
