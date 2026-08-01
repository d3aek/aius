---
title: Aius E-AC-3 (DD+) decode
type: plans
scope: component
component: aius
status: in-progress
subtype: design
tags: [aius, firmware, eac3, ddplus, dolby, decode, arena, iec61937]
updated: 2026-08-01
toc: true
---

# Aius E-AC-3 (DD+) decode

Dolby Digital Plus decode — the last decoder
([`aius-ac3-support.md`](aius-ac3-support.md) §The format space records why
the roadmap ends here). Originally a pre-decision scoping capture;
**promoted to an implementation plan by owner decision 2026-08-01
("let's implement the last decoder")**, which supplies the entry decision
the scoping draft was parked on. The OTA sequencing gate cleared the same
day ([`aius-ota-upgrade.md`](aius-ota-upgrade.md) shipped end-to-end).

**Decisions adopted from the scoping record, now binding for this plan:**

- **Decoder: the FFmpeg `ac3dec` subset** (LGPL-2.1, OSS-Fuzz'd) — the one
  candidate that decodes E-AC-3; bespoke was ruled out of reach and liba52
  is architecturally AC-3-only. Driven without `AVCodecContext` (the
  decoder touches nine context fields), accepting the calling-convention
  fork and treating upstream resync as manual — the same posture the a52
  fork already took deliberately.
- **Scope: 5.1 only.** Decode the independent substream; skip dependent
  (7.1) substreams. `AIUS_MAX_CHANNELS` stays 6; the 18-site channel-mask
  surface is untouched. The operator's speakers are 5.1.
- **Arena: the codec-slot swap restructure** (below) rather than growing
  the arena — three resident decoders are dead on arrival (~221 KB vs
  131,072) and the swap brings DD+ to marginal-but-plausible inside the
  existing arena, pending the real trim measurement, while freeing ~27 KB
  on day one even before the new decoder lands.
- **Live hardware evidence remains gated on a real DD+ source** on the
  bench (phase 5); everything before it is host- and replay-provable.

## Phases

1. **Codec-slot arena restructure** — **done 2026-08-01.** Decoders are
   no longer co-resident: `aius_pipeline` carves one fixed
   `AIUS_PIPELINE_CODEC_SLOT_BYTES` (64 KiB) sub-arena at create and
   lazily creates the ONE decoder the stream needs into it at its Pc
   family's first completed burst; a family change destroys the
   resident, resets the slot (high-water retained), and creates the
   other decoder (`ensure_codec_resident`, hooked at burst routing in
   `dts_ingest`). Heap-path pipelines swap by plain libc create/destroy
   -- arming a per-instance malloc'd slot would poison the alloc shim's
   single owned address range. New surfaces:
   `aius_pipeline_resident_codec()` and a `codec_swaps` service-diag
   counter; a non-resident codec's metrics read all-zero with `AIUS_OK`;
   the compatibility projection's `arena_high_water` is now the slot's
   cumulative high-water. Delivered evidence: swap oracle -- 64 pushes
   (16 strict alternations + 48 seeded-fuzz family picks), 41 swaps,
   every residency-opening decode bit-identical to a fresh arena
   pipeline's, repeat pushes structure-checked (in-residency
   synthesis/overlap history legitimately carries across AUs); capacity
   oracle -- cumulative slot high-water 60,880/65,536 B (margin 4,656)
   across both families' tallest committed vectors; pipeline,
   audio-service, a52, dts, arena suites green; target builds; memory +
   ITCM gates green. The freed headroom vs the old eager pair lands in
   the parent arena for Phase 2's slot re-derivation.
2. **FFmpeg E-AC-3 subset + `aius_eac3` adapter + native proof** — **in
   progress; fork landed 2026-08-01** at `firmware/vendor/ffmpeg-eac3/`
   (FFmpeg 7.1.5, Debian orig tarball; 93 av* files of which 87
   byte-for-byte, 5 edited under tagged `AIUS FORK EDIT` markers, plus the
   fork-owned core TU and framework shim — see its `NOTICE.md`). The
   calling-convention fork stands alone (no codec registry, no AVOption,
   no threading) and decodes a 29-frame 5.1 E-AC-3 vector **bit-identically
   to upstream's own minimal build** (FNV-1a-64 `47b3267c5d527165` both
   sides). Measured (host, -O2): ~87 KB text + ~33 KB rodata; BSS 16.8 MB
   → ~38 KB via the tx table/codelet ladder trims; context 235,232 →
   **161,504 B** via `EAC3_MAX_CHANNELS` 16→7 (5.1-only: dependent
   substreams consumed, never decoded); +8,672 B one-time init allocs;
   **zero per-frame heap churn** (20 allocs total over 29 frames — the
   scoping's 15-per-frame estimate refuted, so the per-AU watermark rewind
   is unnecessary; a plain create-time slot fill suffices). **Adapter +
   native proof landed 2026-08-01**: `lib/aius_eac3` (single-shot
   DTS-shaped contract — one AU in, one 256..1536-frame `AiusPcmBlock`
   out, quantized in place at the aius sample scale; dual-mono,
   back-centre modes, and reduced rates AIUS_UNSUPPORTED), the committed
   `tools/eac3_vectors` corpus (6 vectors, mono/stereo/5.1 at all three
   full rates), and the `eac3` suite: differential vs FFmpeg's own decode
   worst 2 LSB / 107.1 dB SNR, pinned FNV regression hashes, arena pass
   with ZERO warmup allocs and steady-state 0, malformed family under
   ASan/UBSan. **Measured slot need: 207,776 B arena high-water**
   (context 161,504 + adapter handle/planes ~38.7 KB + init allocs; the
   `input_buffer` rung, 32,832 → ~4.2 KB, brings it to ~179 KB) — the
   64 KiB Phase-1 slot constant must grow accordingly (see Phase 4).
   Remaining in this phase: the input_buffer rung, type-21 IEC wrapping
   through `iec_corpus` (feeds Phase 3), fuzz with E-AC-3 CRC semantics +
   termination oracle.
3. **Dispatch + classification constants** — type 21 joins `in_scope`,
   third dispatch route, `codec_pc == 21` metrics/UI decoration,
   squelch/re-type constants re-derived for the 12,288-word burst period
   against the shipped defect-34 stay-dark rule, T6's
   framed-but-unsupported vector moves to a genuinely refused type.
4. **Target integration** — FLASH-cold code placement (the subset cannot
   join the six-block ITCM budget), decode cycle cost measured against
   the 8 ms AUDIO budget, arena/slot gates on target, SD-replay E-AC-3
   IEC fixtures played on the box. **Scope revision (2026-08-01, from the
   Phase-2 measurements):** the slot needs 207,776 B as measured
   (~179 KB after the `input_buffer` rung), so the 131,072 DTCM arena
   must GROW, not merely re-partition. Ground truth from the current
   build: RAM1 = 287,684 B variables + 6 ITCM banks, **39,996 B DTCM
   free**; OCRAM has 131,488 B free. The funded route is the
   `input_buffer` rung plus relocating ~70–90 KB of non-audio-path DTCM
   buffers to OCRAM (defect 26 vetoed OCRAM for *audio-path* state;
   HTTP/network/scratch buffers are not that) and growing `ARENA_BYTES`
   to ~220 KB. The CRC tables (32,800 B BSS, lazily generated) are a
   further flash-const rung if DTCM stays tight.
5. **Live DD+ evidence** — a real DD+ source (TV configured to pass DD+,
   or the incoming bench interface if it can emit it); the four-hour and
   by-ear mission gates extend to type 21. Blocked on hardware until
   then.

## Historical scoping record (2026-07-30)

What adding Dolby Digital Plus decode would involve, recorded 2026-07-30 in answer
to "what is involved in adding DD support", plus the arena-architecture question a
third large decoder forces: decoder residency stops being affordable and the
justified design becomes swapping decoder state in and out of the arena at class
time. The standing recommendation at capture time was
**stay dark unless a real DD+ source appears on this bench** — superseded by
the 2026-08-01 owner decision above. Owner decision
2026-07-31: OTA ([`aius-ota-upgrade.md`](aius-ota-upgrade.md)) is the next
key feature; E-AC-3 implementation is considered only after OTA lands —
**that gate cleared 2026-08-01**.
Defect 34 below remains a standalone prerequisite independent of that
sequencing.

## Entry condition and the prerequisite that stands alone

- **Defect 34 first, regardless of any decode decision.** ~~A type-21 link
  today types as PCM in ~96% of entry phases (12,288-word burst period
  exceeds `AIUS_PIPELINE_RETYPE_MAX_SPACING_WORDS` = 8,192) and plays burst
  data as full-scale noise.~~ **Fixed 2026-07-31** with the generalised
  stay-dark rule (single validated Pa/Pb/Pc pair exits PCM at any phase;
  constants untouched; 115-phase sweep proves no payload reaches the sink);
  hardware reproduction still awaits a DD+-capable source. Full record: the
  hardening ledger entry 34. Candidate fixes recorded in
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
