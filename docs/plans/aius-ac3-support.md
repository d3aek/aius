---
title: Aius AC-3 decode support
type: plans
scope: component
component: aius
status: draft
subtype: design
tags: [aius, firmware, ac3, dolby, decode, iec61937]
updated: 2026-07-28
toc: false
---

# Aius AC-3 decode support

Decode AC-3 (Dolby Digital) bitstreams alongside DTS Core, because AC-3 is
the mission format: the project exists to replace the TV's own unintelligible
2-channel downmix, TVs emit AC-3 5.1 over TOSLINK (DD+/Atmos is transcoded
down; E-AC-3 never rides S/PDIF), and a TV set to PCM output hands the box
the already-ruined stereo — the discrete 5.1 the dialogue lever
(`center_gain_linear` + center EQ) needs only arrives as AC-3. Owner
decision 2026-07-28: next phase after the USB-attached batch closes;
**the defect-31 restructure (decode off the audio ISR) is folded in**, not
sequenced after.

## What today's machinery already covers (verified 2026-07-28)

- The IEC 61937 parser frames AC-3 bursts correctly today (data type 1;
  they complete as UNSUPPORTED only because no decoder consumes them).
- Classification, re-type, starvation, squelch and the carrier-edge reset
  are format-agnostic preamble logic. Constants hold: AC-3's 3072-word
  burst period sits inside `AIUS_PIPELINE_RETYPE_MAX_SPACING_WORDS` (8192)
  and `AIUS_PIPELINE_PCM_CONFIRM_WORDS` (8192) — worst-phase re-type at
  6144 words, still inside the squelch. Nothing re-opens.
- The DSP chain (layout-aware downmix, LFE fold, limiter, single final
  quantize) consumes decoded planes identically.

## Design sketch

1. **Decoder**: vendor liba52 (mature portable C, GPL — fine for this
   personal, non-commercial build) under `firmware/vendor/`, wrapped as
   `aius_a52` mirroring the `aius_dts` shape: arena-backed create, one
   access-unit decode call, metrics, no-heap steady state via the alloc
   shim. Float output scaled to the 24-bit int32 plane contract.
2. **Dispatch**: the pipeline routes each completed burst payload by the
   parser's Pc data type — DTS types to `aius_dts`, type 1 to `aius_a52`,
   everything else UNSUPPORTED as today. The class stays "non-linear
   bitstream"; no new classification states.
3. **Block shape**: AC-3 access units decode 1536 frames; the enqueue cap
   and DSP staging are 512. Emit three 512-frame blocks per AU.
4. **Channel order**: the a52 plane order vs `AIUS_CH_*` mapping is pinned
   by per-channel tone tests from day one — defect 30's lesson, applied
   before the defect can exist.
5. **Corpus**: ffmpeg-encoded AC-3 vectors through the existing
   iec-corpus/vector tooling (tone-identity and impulse programmes
   mirroring the DTS set). Committed ahead of the phase so it starts at
   the contract.

## The two named risks (the reason this is multi-phase)

- **ISR budget — the defect-31 fold-in.** A 1536-frame AU decoded in one
  audio callback is three times DTS's already-over-budget spike (defect
  31's prime suspect). This phase moves bitstream decode OFF the audio
  ISR (epoch-context decode into the transmit ring, or sliced decode),
  closing defect 31 for both codecs rather than inheriting a worse copy.
  Transmit-ring depth must be verified against the AU cadence.
- **Arena sizing.** Two resident decoders may exceed the 128 KiB arena
  (dcadec peak 62,560 + the 32,768 DSP staging carve already sit in it).
  Measure a52's peak; grow the arena or adopt single-resident-decoder
  swapping at class time (measured decision, not assumed).

## Phases (each gated per house protocol)

1. Corpus + `aius_a52` wrapper + native decode suite (host-only).
2. Defect-31 restructure: decode off the ISR, both codecs, with the
   watchdog/starvation interactions re-proven on the pipeline suite.
3. Pipeline dispatch + block split + squelch/re-type suite extension
   (the framed-but-unsupported test moves to a type the box still
   refuses).
4. Target integration: arena measurement, cycle meters, flash, live TV
   validation (the actual mission test: nightly content, dialogue lifted
   via center gain).
