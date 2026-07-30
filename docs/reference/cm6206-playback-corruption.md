---
title: CM6206 playback corruption characterization
type: reference
scope: component
component: aius
status: current
tags: [aius, cm6206, spdif, usb-audio, evidence]
updated: 2026-07-30
toc: true
---

# CM6206 playback corruption characterization

The CM6206 optical loop reproduces isolated one-frame impulses without the
Teensy in the path ([sample-rate plan §Device evidence](../plans/aius-sample-rate-following.md#device-evidence-in-progress)).
Bit-level analysis of every recorded event shows the impulses are not opaque:
they carry a decodable structure that identifies a byte-lane corruption
mechanism on the CM6206 playback side, upstream of S/PDIF serialization. This
document records the verified model, the full evidence table, what it
excludes, the quarantine fingerprint for device gates, and the
single-unit-versus-systemic assessment.

## Verified properties

Every recorded impulse — 35 events across four direct-loop captures (both
days, both altsets, both rates, spanning the hypervisor reboot) plus the
three Teensy PSRAM flight-recorder events — was tested against the two
neighbour samples. The 997 Hz ±410 tone is locally linear at a zero crossing
(curvature well under 1 LSB), so `round((prev + next) / 2)` recovers the true
sample to ±1. Three falsifiable properties hold 35/35:

1. **Only the high byte of one little-endian 16-bit sample is damaged.** The
   corrupt value's low byte equals the interpolated low byte exactly. Chance
   probability across 35 independent events is negligible (< 256⁻³⁵ scaled by
   the ±1 tolerance); the low byte demonstrably still carries the true signal.
2. **Every event sits on a full 0x00↔0xFF high-byte transition.** All 70
   neighbours satisfy |x| ≤ 120 on a ±410 tone, i.e. every event is a sign
   change at small amplitude — exactly where all eight upper bit lines switch
   simultaneously. The detector is amplitude-agnostic
   (`.scratch/019fad89-9c0a-7b51-9c04-c11431d981d1/cm6206-direct-loop.py:82`
   accepts a >1000 double-edge at any neighbour level), so this is physical
   clustering, not selection bias: under uniform phase the probability is
   below 10⁻²⁴.
3. **The damage is a structured bitwise mixture, not a random byte.** Reading
   the flipped bits as lines that failed to complete the transition (held
   their previous state), bit 15 is the weakest line (17/35 events), then 14
   and 13; the same masks repeat across runs and days (`{15}`, `{15,14}`,
   `{15,14,13}`, `{15,13}`, `{12}`), and the same corrupt byte values (0xE0,
   0x41, 0x1F, 0x7F, 0x80) recur through both independent receivers. A random
   replacement byte reproduces neither the reuse nor the top-bit weighting.

One honesty note: because every observed event crosses between complementary
high bytes (0x00/0xFF), the "flipped bits equal the previous sample's bits"
clause is implied by properties 1–2 for these events; it becomes independently
falsifiable only on program material where consecutive high bytes are not
complementary. The load-bearing verified facts are properties 1 and 2 plus
the mask structure.

## Evidence

Run labels: `loop-44k1-a` = pre-reboot 44.1 kHz stereo altset 2;
`loop-48k` = 48 kHz stereo; `loop-8ch` = 44.1 kHz eight-channel altset 1;
`loop-44k1-b` = post-reboot 44.1 kHz stereo; `teensy-ab` = flight-recorder
window, CM USB capture closed; `teensy-1f` = flight-recorder window, one
fibre, no optical return to the CM. All values are decimal 16-bit samples;
"true" is the neighbour interpolation whose low byte the corrupt value
carries; "high" is expected→observed; "held bits" are the flipped positions.

| Run | Ch | Prev | Corrupt | Next | True | High | Held bits |
|---|---|---:|---:|---:|---:|---|---|
| loop-44k1-a | L | 45 | 16883 | -71 | -13 | FF→41 | 15,13,12,11,10,9 |
| loop-44k1-a | R | -15 | -8149 | 100 | 43 | 00→E0 | 15,14,13 |
| loop-44k1-a | R | 45 | 8179 | -71 | -13 | FF→1F | 15,14,13 |
| loop-44k1-a | L | -8 | 4658 | 107 | 50 | 00→12 | 12,9 |
| loop-44k1-a | R | -27 | -8161 | 89 | 31 | 00→E0 | 15,14,13 |
| loop-44k1-a | R | 7 | 32717 | -108 | -51 | FF→7F | 15 |
| loop-44k1-a | R | -57 | -16384 | 58 | 0 | 00→C0 | 15,14 |
| loop-44k1-a | R | 30 | 24548 | -86 | -28 | FF→5F | 15,13 |
| loop-44k1-a | R | 24 | 24542 | -92 | -34 | FF→5F | 15,13 |
| loop-48k | L | 2 | -15412 | -104 | -52 | FF→C3 | 13,12,11,10 |
| loop-8ch | L | 23 | -15907 | -93 | -35 | FF→C1 | 13,12,11,10,9 |
| loop-8ch | R | -2 | -32712 | 113 | 56 | 00→80 | 15 |
| loop-8ch | R | 27 | 8161 | -89 | -31 | FF→1F | 15,14,13 |
| loop-8ch | R | 4 | 32714 | -111 | -54 | FF→7F | 15 |
| loop-8ch | R | -33 | -32743 | 83 | 25 | 00→80 | 15 |
| loop-8ch | R | -24 | -16350 | 92 | 34 | 00→C0 | 15,14 |
| loop-8ch | R | -18 | -32728 | 97 | 40 | 00→80 | 15 |
| loop-8ch | R | -5 | -1995 | 110 | 53 | 00→F8 | 15,14,13,12,11 |
| loop-8ch | L | 45 | -4110 | -71 | -14 | FF→EF | 12 |
| loop-8ch | R | -13 | -8147 | 102 | 45 | 00→E0 | 15,14,13 |
| loop-8ch | R | 4 | 8138 | -111 | -54 | FF→1F | 15,14,13 |
| loop-8ch | R | 3 | 8137 | -112 | -55 | FF→1F | 15,14,13 |
| loop-8ch | L | -47 | 7691 | 69 | 11 | 00→1E | 12,11,10,9 |
| loop-8ch | R | -2 | -8136 | 112 | 56 | 00→E0 | 15,14,13 |
| loop-8ch | L | -42 | 32272 | 74 | 16 | 00→7E | 14,13,12,11,10,9 |
| loop-44k1-b | R | -31 | -32741 | 85 | 27 | 00→80 | 15 |
| loop-44k1-b | R | 32 | 8166 | -83 | -26 | FF→1F | 15,14,13 |
| loop-44k1-b | R | 55 | 24572 | -61 | -4 | FF→5F | 15,13 |
| loop-44k1-b | R | -25 | -2015 | 91 | 33 | 00→F8 | 15,14,13,12,11 |
| loop-44k1-b | R | 31 | 32741 | -84 | -27 | FF→7F | 15 |
| loop-44k1-b | R | 10 | 32720 | -105 | -48 | FF→7F | 15 |
| loop-44k1-b | L | 13 | -4141 | -102 | -45 | FF→EF | 12 |
| teensy-ab | L | 14 | 16852 | -101 | -44 | FF→41 | 15,13,12,11,10,9 |
| teensy-ab | R | -2 | -8136 | 113 | 56 | 00→E0 | 15,14,13 |
| teensy-1f | R | -54 | -8188 | 62 | 4 | 00→E0 | 15,14,13 |

Two ancillary observations from the same table:

- **Channel bias.** 27 of the 32 loop events are on the right channel (the
  second word of each frame), a ~5:1 asymmetry.
- **Mode-dependent rate.** Same host, same extender, same tone content:

| Device mode | Events per ~125 s |
|---|---:|
| 44.1 kHz stereo, altset 2 | 9 (pre-reboot), 7 (post-reboot) |
| 44.1 kHz eight-channel, altset 1 | 15 |
| 48 kHz stereo, altset 2 | 1 |

The Teensy windows (44.1 kHz, ~22 s each) saw 2 and 1 events, consistent
with the 44.1 kHz loop rate. Eight-channel mode moves 4.5× the USB data of
stereo yet raises the observed rate only 1.7×, while switching 44.1→48 kHz
drops it ~9× with near-identical data volume: the rate tracks the device's
clock mode, not the transported byte count.

## What the signature excludes

- **Host software (tone generation, `aplay`, ALSA, `snd-usb-audio`).** A
  software fault corrupts at byte, word or period granularity with foreign
  values; nothing in a copy path produces per-bit-line failed transitions
  locked to the data's own 0x00↔0xFF transitions. The playback content and
  software were identical across modes whose event rates differ ~9×.
- **The serial domains: USB wire (NRZI/bit-stuffing) and optical S/PDIF
  (biphase-mark).** Serial errors produce contiguous burst or bit-shift
  damage, not byte-lane-aligned bitwise mixtures of two adjacent words. Two
  independent receivers (CM6206 capture and the Teensy S/PDIF RX) decoded
  identical corrupt words — including the same foreign bytes 0x41 and 0xE0 in
  unrelated runs — so the word was already corrupt before biphase encoding.
- **Both capture paths.** External Mac source into CM capture: zero events,
  zero unequal stereo frames in 600 s. Teensy transmit into CM capture:
  970,200 frames bit-exact.
- **Host RAM, xHCI DMA, virtualization.** Host-side timing and data patterns
  are invariant across the device modes; only hardware clocked by the audio
  mode can express the 44.1-versus-48 kHz rate asymmetry.

The remaining fault surface is a parallel 16-bit datapath on the playback
side whose timing depends on the audio clock mode: the CM6206's internal
playback FIFO/clock-domain crossing between its USB engine (post-CRC) and
its DAC/S-PDIF engine (primary candidate), or the Icron remote (REX) buffer
(residual candidate — it also holds data in parallel RAM and regenerates the
downstream USB packet). The deferred direct, non-Icron host discriminator in
the sample-rate plan separates the two; this analysis predicts it still
reproduces. Against the Icron: the clean 600 s inbound path traverses the
same extender, and the extender's link timing is identical in the two device
modes that differ ~9× in rate.

## Companion defect: L/R word-phase slips

The impulse counts hid a second, far more frequent defect with the same
localization. In the 44.1 kHz loop returns, ~6.3% of frames are unequal
between channels (347,620 of ~5.5M pre-reboot; 345,833 eight-channel) with
first unequal pairs differing by exactly one sample of tone slope (−3/−61,
−1/−59), and ~1,600 crossing intervals per channel fall outside the source's
44/45 cadence. The 48 kHz return began with its right channel one sample
behind the left. These are one-sample L/R misalignment episodes that enter
and leave (~13 boundaries/s): a word-phase slip in the playback/transmit
pipeline. The Mac→CM capture (zero unequal frames, 600 s) and Teensy→CM
capture (bit-exact) rule out the capture side, so the slips also originate in
CM6206 playback/transmit. Word-pointer slips and mixed-bit word reads are
both classic failure modes of one block: an under-margined asynchronous FIFO
crossing between clock domains.

## Single unit or systemic?

The evidence cannot conclusively separate a marginal individual device from a
design-level erratum, but it leans systemic:

- **Two distinct symptom classes point at one block.** Held-bit word
  corruption and self-correcting word-pointer slips are both
  clock-domain-crossing failure modes; a single weak RAM cell or bond
  explains neither the slips nor the mode dependence.
- **The rate tracks the clock relationship, not the data.** The playback
  endpoint is adaptive (`proc-asound` shows `ADAPTIVE`, momentary-frequency
  tracking), so the device must synthesize its audio clock against USB
  delivery — a mandatory async crossing. 44.1 kHz demands a fractional
  relationship to the local oscillator where 48 kHz is near-integer; a
  phase-sweeping race fires often in the fractional mode and rarely in the
  integer one, which is what the ~9× asymmetry shows.
- **Unit-level factors modulate, not create.** Process corner, this unit's
  +438 ppm crystal, and the dongle's supply integrity all shift how often a
  marginal window is hit, so a replacement unit would likely show the same
  class at a different — possibly much lower, possibly similar — rate rather
  than none.

A purely unit-specific fault (one weak die or board) remains possible: bit 15
dominance with repeatable masks is also consistent with one specific slow
line. The discriminator is cheap: run the existing 125 s direct-loop bracket
and analyzer on a second CM6206. Same fingerprint at any rate → systemic;
clean impulse count *and* zero unequal-stereo fraction → this unit is
marginal. A clean 125 s run only bounds the rate, so the stereo-identity
fraction (sensitive at ~10⁵ events per run) is the stronger of the two
checks. Either way the measurement-path posture stands: waveform-critical
gates use a source the discriminator has shown clean.

## Quarantine fingerprint for device gates

A source impulse may be attributed to this CM6206 class and quarantined only
when it matches the verified structure, in addition to the existing recorder
conditions (no gap, no counter delta, no lock/rate transition, single frame):

```text
mid  = round((prev + next) / 2)
qualifies iff exists true in {mid - 1, mid, mid + 1} such that
    (corrupt & 0xFF) == (true & 0xFF)              # low byte intact
and (corrupt ^ true) & 0xFF00 != 0                 # damage confined to high byte
and (corrupt ^ prev) & (corrupt ^ true) & 0xFF00 == 0
    # every flipped bit equals the previous sample's bit (held line);
    # vacuous when prev/true high bytes are complementary, load-bearing otherwise
```

Any low-byte damage, multi-frame run, both-channel event, or high byte that
is not a prev/true mixture is **not** this fault and must not be waived.
Encoded-stream gates: the AC-3 preflight's decode/parse rejects are consistent
with the same fault landing in IEC 61937 payload words, where dense bit
transitions offer far more trigger sites; raw-word correlation should find
the same odd-offset high-byte damage before waiving an encoded reject.

## Provenance

Mechanical verification of all 35 events:
`.scratch/a529e18a-a876-466f-b077-0a628284265d/verify_impulse_structure.py`
(reads the four run `summary.json` files; the three flight-recorder triples
are inlined). Raw runs:

- `.scratch/019fad89-9c0a-7b51-9c04-c11431d981d1/cm6206-direct-loop-125s/`
- `.scratch/019fad89-9c0a-7b51-9c04-c11431d981d1/cm6206-direct-loop-48k-125s/`
- `.scratch/019fad89-9c0a-7b51-9c04-c11431d981d1/cm6206-direct-loop-44k1-altset1-8ch-125s/`
- `.scratch/019fb103-d707-7e01-adee-0ad1238d3115/cm6206-direct-loop-post-reboot-44k1-125s/`
- `.scratch/019fb103-d707-7e01-adee-0ad1238d3115/cm6206-playback-duplex-ab/`
- `.scratch/019fb103-d707-7e01-adee-0ad1238d3115/cm6206-playback-one-fibre/`

Scratch directories are garbage-collected after seven days; the evidence
table above is therefore complete rather than a pointer.

## Related documents

- [Aius sample-rate following](../plans/aius-sample-rate-following.md)
- [Aius signal path](signal-path.md)
