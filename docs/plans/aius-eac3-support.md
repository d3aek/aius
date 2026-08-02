---
title: Aius E-AC-3 (DD+) decode
type: plans
scope: component
component: aius
status: in-progress
subtype: design
tags: [aius, firmware, eac3, ddplus, dolby, decode, arena, iec61937]
updated: 2026-08-02
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
2. **FFmpeg E-AC-3 subset + `aius_eac3` adapter + native proof** — **done
   2026-08-02; fork landed 2026-08-01** at `firmware/vendor/ffmpeg-eac3/`
   (FFmpeg 7.1.5, Debian orig tarball; 93 av* files of which 87
   byte-for-byte, 5 edited under tagged `AIUS FORK EDIT` markers, plus the
   fork-owned core TU and framework shim — see its `NOTICE.md`). The
   calling-convention fork stands alone (no codec registry, no AVOption,
   no threading) and decodes a 29-frame 5.1 E-AC-3 vector **bit-identically
   to upstream's own minimal build** (FNV-1a-64 `47b3267c5d527165` both
   sides). Measured (host, -O2): ~87 KB text + ~33 KB rodata; BSS 16.8 MB
   → ~38 KB via the tx table/codelet ladder trims; context 235,232 →
   161,504 B via `EAC3_MAX_CHANNELS` 16→7 (5.1-only: dependent
   substreams consumed, never decoded) → **132,960 B** with the
   `input_buffer` rung taken 2026-08-02 (`[input-buffer-cap]`: one max
   syncframe + 128 B tail-header room instead of a whole-superframe copy,
   `buf_size` clamped to the copy); +8,672 B one-time init allocs;
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
   ASan/UBSan. **Measured slot need: 179,232 B arena high-water**
   (context 132,960 + adapter handle/planes ~38.7 KB + init allocs;
   207,776 before the `input_buffer` rung landed 2026-08-02 with suite +
   fuzz re-proof, 87,999 decodes clean) — the 64 KiB Phase-1 slot
   constant must grow accordingly (see Phase 4). The fuzz suite
   (E-AC-3 CRC semantics + defect-31 termination oracle) landed
   2026-08-01. Type-21 IEC wrapping landed 2026-08-02, closing the
   phase: `eac3_surround51_ident` in the `iec_corpus` — two real 5.1
   E-AC-3 AUs, byte-count Pd, Pa spacing exactly 12,288 words —
   de-frames byte-identically under all three chunkings; the
   known-answer expectation was UNSUPPORTED,UNSUPPORTED until Phase 3
   took type 21 in scope (now OK,OK).
3. **Dispatch + classification constants** — **done 2026-08-02.** Type 21
   joined the parser's `in_scope` unconditionally (zero-Pd type 21 is now
   MALFORMED; the 4,096 B payload cap equals the max legal syncframe, so
   independent-substream bursts always fit). The third dispatch route is
   compile-optional (`AIUS_PIPELINE_EAC3`, OTA-precedent, default off):
   the ~120 KB fork must not join the gated target image before Phase 4's
   FLASH placement, so on the target a completed type-21 burst is refused
   at dispatch (decode_rejects, AIUS_UNSUPPORTED, dark) — proven by the
   flag-off probe in the audio-service suite. Flag on (the `pipeline`
   suite), E-AC-3 mirrors the DTS single-shot flow, sliced ≤512 frames.
   **Residency is gated before the resident is touched**
   (`residency_possible`): the 64 KiB slot cannot hold the measured
   179,232 B need (named `AIUS_PIPELINE_EAC3_SLOT_BYTES` = 180,224), so an
   arena-backed pipeline refuses type-21 residency AIUS_NO_SPACE without
   evicting a warm DTS/AC-3 decoder or dirtying the slot high-water
   (suite-proven interleave). **Classification constants: all four
   unchanged.** The defect-34 single-pair exit SET is unchanged (every
   type outside {1,11,12,13}); type 21 stays in it because its 12,288-word
   worst-case period exceeds `RETYPE_MAX_SPACING_WORDS` 8,192 — the
   re-derivation lives in `single_pair_exit_header`'s comment (the renamed
   `unsupported_burst_header`), including the error-flagged-header
   residual. `DTS_STARVE_WORDS` 16,384 covers the 12,288-word worst
   type-21 outcome gap (1.33x). Decoration: `codec_pc == 21` flows through
   the status JSON (`eac3_decode_cycles`/`eac3_alloc_count`, three-way
   `shared_arena_high_water`) and both SD pages name `E-AC-3`; oracles
   extended (`test_bitstream_ui.py`, dsp-editor vitest). T6 moved to the
   new genuinely-refused `unsupported_type7` vector. Delivered evidence:
   pipeline suite — wire-to-sink bit-identity vs the direct adapter decode
   (2 AUs → 6×512), the 115-phase dark sweep re-proved on the arena
   (refused-residency) path, a 27-phase decode sweep proving every entry
   phase of a real DD+ carrier locks and decodes 5.1 on the heap path;
   iec61937 + fuzz, audio-service, optical-app, sd-editor vitest green;
   target image builds with memory/ITCM gates.
   **Scope note (from the parser cap):** a burst carrying dependent
   (>5.1) substreams exceeds the 4,096 B cap → parser NO_SPACE → dark. A
   7.1-configured DD+ source therefore stays silent by design; the
   adapter's 24,576 B burst headroom is unreachable through this
   pipeline. Phase 5's live evidence should configure the TV to 5.1
   output.
4. **Target integration** — **GATE CLEARED 2026-08-02** (owner go, same
   day): the boot-lottery defect was convicted and fixed (S/PDIF DPLL
   junk-lock gate at bring-up, `relocks` sentinel;
   [`aius-open-work-register.md`](aius-open-work-register.md) §Open
   defects — runtime carries the conviction chain and the 28/28
   flash-boot evidence), meeting the 2026-08-01 condition that boots be
   deterministically healthy. **Build-side landed 2026-08-02** (parts
   1–3): the funded route CHANGED from the recorded OCRAM-relocation
   sketch — the memory gate pins OCRAM free at its 131,328 B baseline
   (only 256 B above the defect-26 heap-floor ASSERT), so nothing
   meaningful can move there. The route actually taken funds the arena
   from DTCM-internal swaps: the framework ldscript copies ALL .rodata
   into DTCM, and a new `.text.aius_flashro` claim pins the vendored
   decoder tables (dcadec ~79 KB, liba52 ~4 KB) + two string-heavy
   first-party objects to flash (−99,328 B of DTCM .data), the fork's
   CRC tables went flash-const (`CONFIG_HARDCODED_TABLES`, −32,800 B of
   would-be BSS), and a new link-time ASSERT floors the DTCM
   stack/locals zone at 24,352 B so an unfunded growth is a link error.
   `AIUS_PIPELINE_CODEC_SLOT_BYTES` = `AIUS_PIPELINE_EAC3_SLOT_BYTES`
   (180,224); `ARENA_BYTES` = instance bound (5,632, published as
   `AIUS_PIPELINE_INSTANCE_MAX_BYTES` and asserted in both builds) +
   32,768 carve + slot + 64 = **218,688 B**, compile-time-asserted.
   The optical env compiles the route (`AIUS_PIPELINE_EAC3=1`, the fork
   TU set, per-source include scoping via `scripts/fork_eac3_flags.py`
   so liba52's and the fork's `config.h` can never cross-resolve); the
   fork's text+rodata are ldscript-pinned to flash (anchor-checked
   post-link), the adapter is FLASHMEM + COLD_OBJECTS. Post-build:
   DTCM free-for-locals 31,040 B (floor 24,352), ITCM +624 B for the
   dispatch (1,364 free), OCRAM untouched, all gates green; pipeline
   suite reworked (slot-swap now 3 families, 66 pushes / 45 swaps
   bit-identical, capacity oracle 179,232/180,224, arena residency +
   flipped dark sweep) — all green.
   **Acceptance shape for the on-box evidence (recorded BEFORE
   flashing):** the replay seam drains a fixed 96,000 words/s, but a
   type-21 burst period is 12,288 words for 32 ms of audio — a 4×
   shortfall by construction (real DD+ rides a 4×-rate link the replay
   seam does not model), and no live source exists (the TX transmits
   DSP PCM, never type 21). SD-replay therefore proves DECODE
   structurally — codec_pc 21, decode counters advancing,
   `eac3_decode_cycles` against the 8 ms budget — while gapless audible
   playback is impossible and tx underruns are an artifact of the
   source, not a defect. DTS/AC-3 decode cycles are re-measured beside
   it (their tables moved to flash-cached reads).
5. **Live DD+ evidence** — a real DD+ source (TV configured to pass DD+,
   or the incoming bench interface if it can emit it); the four-hour and
   by-ear mission gates extend to type 21. Blocked on hardware until
   then. Note from Phase 4: replay cannot substitute (4× link-rate
   shortfall by construction), and the source must be configured for
   5.1 output — dependent-substream (7.1) bursts exceed the parser cap
   and stay dark by design (Phase 3 scope note).

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
