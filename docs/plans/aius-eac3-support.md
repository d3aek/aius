---
title: Aius E-AC-3 (DD+) decode — scoping and the residency-vs-swap question
type: plans
scope: component
component: aius
status: draft
subtype: brain-dump
tags: [aius, firmware, eac3, ddplus, dolby, decode, arena, iec61937]
updated: 2026-07-30
---

# Aius E-AC-3 (DD+) decode — scoping and the residency-vs-swap question

What adding Dolby Digital Plus decode would involve, recorded 2026-07-30 in answer
to "what is involved in adding DD support", plus the arena-architecture question a
third large decoder forces: decoder residency stops being affordable and the
justified design becomes swapping decoder state in and out of the arena at class
time. Pre-decision capture, not a commitment; the standing recommendation from
[`aius-ac3-support.md`](aius-ac3-support.md) §The format space is unchanged —
**stay dark unless a real DD+ source appears on this bench**.

## Entry condition and the prerequisite that stands alone

- **Defect 34 first, regardless of any decode decision.** A type-21 link today
  types as PCM in ~96% of entry phases (12,288-word burst period exceeds
  `AIUS_PIPELINE_RETYPE_MAX_SPACING_WORDS` = 8,192) and plays burst data as
  full-scale noise. Candidate fixes recorded in
  [`aius-network-hardening.md`](aius-network-hardening.md) defect 34: raise the
  re-type spacing to cover 12,288 (lengthens every genuine PCM confirmation mute,
  since `PCM_CONFIRM_WORDS` must stay >= MAX), or detect type 21 in the classify
  window and refuse to leave the dark state. The dark-state fix IS the "stay dark"
  answer; everything below is only relevant if decode is later chosen.
- **Entry condition for decode:** a real DD+ source on the bench (a TV configured
  to pass DD+, or a device that only emits it). Do not build ahead of that.

## What decode involves

1. **A different decoder, and it does not fit today's arena.** liba52 is
   architecturally AC-3-only; bespoke E-AC-3 was ruled out of reach. The one
   examined candidate is an FFmpeg `ac3dec` subset (LGPL-2.1, OSS-Fuzz'd):
   measured ~94 KB trimmed RAM working set with E-AC-3, 15 malloc/free pairs per
   frame (~14 KB/s churn against a bump allocator that never frees), ~262 KB
   `av_tx` BSS in one object, 92-file vendoring surface, and a calling-convention
   fork to drive it without `AVCodecContext`. Full comparison:
   [`aius-ac3-support.md`](aius-ac3-support.md) §Decoder alternatives.
2. **A channel-count decision.** `AIUS_MAX_CHANNELS` is 6 with a 6-bit mask across
   ~18 sites. The sane scope is 5.1-only: decode the independent substream and
   ignore dependent 7.1 substreams (the operator's speakers are 5.1). Substream
   handling is a design point to verify against the spec, not a given.
3. **Parser and dispatch, mostly cheap.** Type 21 joins `in_scope` (byte-count Pd
   asymmetry already handled at `aius_iec61937.c:196`); dispatch grows a third
   route; `codec_pc == 21` flows through the phase-3 per-codec metrics and UI
   decoration. Squelch/re-type constants need re-derivation for the 12,288-word
   period, interacting with whichever defect-34 fix landed. T6 loses
   `unsupported_eac3` as its honest framed-but-unsupported vector and needs a new
   genuinely refused type.
4. **Code placement.** The FFmpeg subset will not join the six-block ITCM budget
   (`ASSERT(_itcm_block_count <= 6)`); it goes cold in FLASH (95% free), with the
   cycle cost of flash execution measured against the 8 ms AUDIO service budget.
   Note this is a *code placement* question, static at link time — distinct from
   the *state arena* question below. Nothing here swaps code at runtime.
5. **The proof apparatus, ported.** ffmpeg-encoded E-AC-3 corpus through
   `iec_corpus`, new differential baselines (FFmpeg becomes both reference and
   implementation, weakening the differential's independence — worth an
   independent second oracle), fuzz suite with E-AC-3 CRC semantics (different
   frame/CRC layout than AC-3's solved CRC1), termination oracle, target cycle
   and arena gates, bench and TV evidence.

Rough shape: a larger lift than the entire AC-3 effort, with the arena and
placement questions as genuine stop-condition risks, for a format the TV emits
only when deliberately configured to.

## The residency question a third large decoder re-opens

[`aius-ac3-support.md`](aius-ac3-support.md) §Phase 2 contract rejected dynamic
decoder instantiation (gate F15) on two grounds: the bump allocator cannot free,
so destroy/create at class change leaks the whole footprint per switch and fails
deterministically after a handful of switches; and a mid-stream allocation that
can fail leaves no decoder exactly when audio should resume. Both objections were
raised **while the premise held that residency needs no more memory than the
arena has** ("more machinery than 2a, to buy memory 2a shows we do not need").
A third large decoder breaks that premise, so re-opening the decision is a
changed requirement, not discomfort.

### The arithmetic

All-resident with a trimmed FFmpeg E-AC-3 decoder:

| Item | Bytes |
|---|---|
| dcadec (on-target) | 62,560 |
| aius_a52 (measured 2026-07-30) | 27,024 |
| FFmpeg E-AC-3 subset (trim-ladder estimate) | ~94,000 |
| DSP staging | 32,768 |
| pipeline instance | ~5,200 |
| **Total** | **~221,552 vs 131,072** |

Dead on arrival: ~90 KB over even if the (defect-26-vetoed) OCRAM relocation
succeeded. Reserving worst-case residency for codecs that sit quiet for days or
months does not survive a third decoder.

One instantiated decoder in a fixed codec slot, swapped at class time:

| Configuration | Resident | Slot | Total |
|---|---|---|---|
| DTS or AC-3 active (today's decoders) | 37,968 | 62,560 (max of the two) | 100,528 — ~30.5 KB headroom |
| DD+ in the mix | 37,968 | ~94,000 | ~131,968 — ~896 B over today's arena |

Swap brings the requirement from ~221 KB to max-decoder + ~38 KB, i.e. roughly
today's DTCM arena. DD+ becomes marginal-but-plausible inside the existing
131,072 (pending the real trim measurement) instead of impossible; without DD+
the same restructure frees ~27 KB — which incidentally crosses the recorded
"~25 KB of headroom" revisit trigger for FFmpeg in the AC-3 plan's decision log.

### What the swap design must provide (answering F15's objections, not waving at them)

- **Deterministic reclamation: a two-level reset, not free().** A fixed codec
  slot with its own bump pointer, reset to empty at class change (answers the
  leak objection by construction); plus a per-AU watermark reset inside the slot
  to absorb FFmpeg's per-frame malloc/free churn (free stays a no-op; the
  watermark rewinds after each AU). Slot reset joins the existing single
  pending-work cleanup (`aius_a52_abandon` et al.) so no discontinuity can leave
  a half-built decoder armed.
- **Infallible creation: statically proven slot capacity.** Each decoder's
  create + steady-state footprint is already suite-asserted (a52 <= 28,000
  style). A create into an empty slot of proven capacity cannot fail, which
  dissolves the "allocation fails mid-stream, no decoder when audio resumes"
  objection deterministically rather than probabilistically.
- **Bounded create cost at the class boundary.** Create/init runs in epoch
  context as one bounded unit inside the 170–255 ms format-switch mute the
  pipeline already incurs; measure create cycles per decoder (FFmpeg table init
  is the heavy one — prefer `const` flash tables) against the 8 ms AUDIO budget
  and the over-budget-unit rule.
- **Swap trigger identity.** The burst's Pc at dispatch selects the decoder;
  classification stays format-agnostic. DTS<->AC-3 flapping (the bench replay
  alternation) becomes the stress case: it needs a dedicated oracle — N
  destroy/create cycles then a first block bit-identical to a fresh decoder's,
  generalizing the existing abandon-equivalence test — plus fuzz over
  class-change sequences, per the defect-28/31 lineage.
- **Observability semantics change.** Per-codec metrics exist only while
  instantiated; the status projection (`arena_high_water` = max of adapters,
  both-alloc-counts-zero gates) and the hardware-gate assertions need
  re-specifying around slot high-water per codec.

A side effect worth recording: swapping dissolves the currently unmeasured
combined-residency risk (audit finding 2026-07-30 — no single-boot DTS+AC-3
arena measurement exists), because combined residency stops existing.

### When to build it

Not now. The two-decoder residency is proven, green, and simpler. The swap
restructure is DD+'s phase 0/1 design work, contingent on the entry condition
above — with one exception worth weighing early: if arena pressure appears from
any other direction, the ~27 KB the slot model frees today is the cheapest
headroom on the table.
