# Source qualification — optical self-loop bracket

Host-side discriminator bracket for qualifying a candidate S/PDIF source
against the CM6206 corruption classes
([characterization](../../../docs/reference/cm6206-playback-corruption.md)):
play a 997 Hz ±410 tone out of a USB interface whose optical output is
fibre-looped into its own optical input, capture the return, and check for
isolated impulses, dropouts, stereo word-phase slips, cadence anomalies and
bit-exactness. `direct_loop_bracket.py` is the CM6206 bracket analyzer made
reusable; `loop_refine.py` adds the stereo-identity, cadence and bit-exact
checks (the last is the strongest check a digital self-loop allows — a clean
loop returns the tone bit-exact).

A self-loop cannot measure carrier wander: the capture clock is slaved to
the device's own transmit clock. Carrier-stability observation (the gate-1
qualification in
[`aius-sample-rate-following.md`](../../../docs/plans/aius-sample-rate-following.md)
§Acceptance) needs the source's TX into the Teensy RX with the board's
millihertz measurement.

## Usage

Generate a tone per rate (165 s so a 125 s capture is fully covered):

```bash
python3 tone_gen.py --rate 48000 --output /tmp/tone-997-48000-165s.wav
```

Prepare the device. For the ESI U24XL: switch capture to the optical input
and set unity playback volume (numids from `amixer -c <card> contents`):

```bash
amixer -c 0 cset numid=8 1        # PCM Capture Source = IEC958 In
amixer -c 0 cset numid=5 110,110  # PCM Playback Volume = 0 dB
```

**Re-verify both before EVERY measurement session.** The U24XL resets its
mixer to defaults (volume 70 = −20 dB) on any USB re-enumeration, including
a momentary cable blink. A −20 dB digital output passes every
amplitude-normalized PCM gate while silently destroying IEC 61937 sync
arithmetic — the failure looks like a codec/classification fault, not a
level fault (2026-08-03 session record in `aius-e2e-latency.md`).

Run the bracket (~130 s) and the refine:

```bash
python3 direct_loop_bracket.py --device hw:0,0 --tone /tmp/tone-997-48000-165s.wav --output <outdir> --rate 48000 --seconds 125
python3 loop_refine.py --tone /tmp/tone-997-48000-165s.wav --capture <outdir>/direct-return.wav --rate 48000 --output <outdir>/refined-analysis.json
```

A clean run reports `isolated_impulse_count` 0, `unequal_stereo_frames` 0,
`intervals_outside_legal` [0, 0] and `bit_exact` true. Judge any failure
against the quarantine fingerprint in the characterization doc before
attributing it to the device: host-side starvation (an ALSA underrun during
a CPU/container burst) produces a bounded held-sample episode with a net
frame slip, not the CM6206's single-frame high-byte impulses or persistent
L/R word-phase slips.

## Provenance

`direct_loop_bracket.py` is the analyzer that produced the CM6206 evidence
table (35/35 impulse events; scratch original `cm6206-direct-loop.py`),
with only the capture filename and docstring generalized. The U24XL
qualification runs of 2026-08-02 (44.1 kHz pristine bracket, interrupted
48 kHz partial) used these exact checks; results are recorded in
[`aius-open-work-register.md`](../../../docs/plans/aius-open-work-register.md).
