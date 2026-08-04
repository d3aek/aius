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

Raw evidence (captures, results.json, original-config.json) lives in the
running session's scratch battery directory; the numbers above are the
durable record.
