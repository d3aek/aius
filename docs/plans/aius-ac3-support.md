---
title: Aius AC-3 decode support
type: plans
scope: component
component: aius
status: in-progress
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
   the contract. **Done 2026-07-28: `tools/ac3_vectors/` (95b20c140),
   every recipe explicitly mapped, authoring Goertzel-proven; phase 1's
   remaining scope is the `aius_a52` wrapper + native suite. IEC
   wrapping joins phase 3 with the dispatch work.**

## The two named risks (the reason this is multi-phase)

- **ISR budget — the defect-31 fold-in.** A 1536-frame AU decoded in one
  audio callback is three times DTS's already-over-budget spike (defect
  31's prime suspect). This phase moves bitstream decode OFF the audio
  ISR (epoch-context decode into the transmit ring, or sliced decode),
  closing defect 31 for both codecs rather than inheriting a worse copy.
  Transmit-ring depth must be verified against the AU cadence.
- **Arena sizing.** ~~Two resident decoders may exceed the 128 KiB arena~~
  **MEASURED 2026-07-28 and CONFIRMED: they do not fit.** a52 peaks at
  90,464 B; with dcadec's 62,560 and the 32,768 DSP staging carve that is
  185,792 against 131,072. Phase 4 must grow the arena (OCRAM has ~128 KiB
  spare after the analyzer landed) or swap to a single resident decoder at
  class time. No longer a risk to assess — an input to a decision.

## Phases (each gated per house protocol)

1. ~~Corpus + `aius_a52` wrapper + native decode suite (host-only).~~
   **DONE 2026-07-28** — see §Phase 1 delivered.
2. Defect-31 restructure: decode off the ISR, both codecs, with the
   watchdog/starvation interactions re-proven on the pipeline suite.
3. Pipeline dispatch + block split + squelch/re-type suite extension
   (the framed-but-unsupported test moves to a type the box still
   refuses).
4. Target integration: arena measurement, cycle meters, flash, live TV
   validation (the actual mission test: nightly content, dialogue lifted
   via center gain).

## Phase 1 contract (2026-07-28) — vendor + wrapper + native suite

Host-only; no target build changes. Owned paths: `firmware/vendor/a52-core/`,
`firmware/lib/aius_a52/{include,src}/`, `firmware/test/a52/`,
`firmware/scripts/test.sh` (+ its README suites table), this plan.

### Vendor: liba52 from a52dec 0.7.4, verbatim

- **Source**: upstream a52dec 0.7.4 (liba52), the release every distro ships.
  Fetched as Debian's untouched `a52dec_0.7.4.orig.tar.gz` from
  `deb.debian.org/debian/pool/main/a/a52dec/` — the VideoLAN GitLab archive
  endpoint requires auth from this network and `liba52.videolan.org` no
  longer resolves. sha256
  `a21d724ab3b3933330194353687df82c475b5dfb997513eef4c25de6c865ec33`.
  License GPL-2 (`COPYING` vendored; acceptable for this personal,
  non-commercial build — recorded in the design sketch above).
- **Subset** (the decoder, nothing else): sources `bitstream.c`, `imdct.c`,
  `bit_allocate.c`, `parse.c`, `downmix.c`; headers `a52_internal.h`,
  `bitstream.h`, `tables.h` (from `liba52/`) plus `a52.h`, `attributes.h`,
  `mm_accel.h` (from `include/`). No autotools, no `libao`, no `src/a52dec.c`.
- **Zero edits, accommodate from outside** (the dcadec-core discipline): the
  wrapper supplies the `config.h` liba52 includes. It deliberately does NOT
  define `HAVE_MEMALIGN` — parse.c then allocates the sample buffer via
  `malloc`, so the existing `aius_alloc_shim` link-time wrap
  (`--wrap=malloc,...`) routes every liba52 allocation through the arena,
  exactly as it does for dcadec's `ta.c`. It does NOT define `LIBA52_DOUBLE`
  — `sample_t` stays `float`, matching the M7's single-precision FPU.
  `config.h` also `#include <stdio.h>` FIRST and then defines
  `#define fprintf(...) ((void)0)`: `a52_imdct_init` unconditionally
  fprintf's "No accelerated IMDCT transform found" to stderr on every init
  (`imdct.c:428`) with no flag to silence it; the macro no-ops it at the
  vendor-compile seam only. The include-before-define ordering is
  load-bearing — `imdct.c` includes `config.h` before `<stdio.h>`, so a
  bare macro would mangle glibc's own `fprintf` declaration when stdio is
  included later; pre-including it arms the include guard first (gate F6 +
  exhaustion-pass fix). `NOTICE.md` records
  all of this plus the tarball provenance.
- **Compile carve-outs, enumerated up front** (gate F3/F4; dcadec precedent
  of scoped, documented accommodations): the a52 suite arms add
  `-Wno-unused-parameter` (unused `mm_accel` in `a52_imdct_init`),
  `-Wno-implicit-fallthrough` (deliberate cascades in `parse.c` exponent
  decode and `downmix.c` mixers), `-Wno-shift-negative-value` (`tables.h`'s
  `(-2 << 15)` quantizer macros fire the compile-time warning under
  `-Werror`, found at first build), `-fno-sanitize=shift`
  (`current_word << 32` on every frame entry plus signed-left-shift
  mantissa idioms — UBSan noise by design, not defects), `-D_GNU_SOURCE`
  (`M_PI`, as for dcadec), and `-lm`. Everything else runs under the full
  dts-suite flag set.

### Wrapper: `aius_a52`, mirroring the `aius_dts` shape

- API: `aius_a52_decoder_create` / `_create_arena` / `_decode_access_unit` /
  `_get_metrics` / `_destroy`, same status vocabulary, same arena contract,
  same metrics fields (`arena_high_water`, `alloc_count`, per-decode timing
  staged for the target's cycle meters).
- One access unit in (the IEC parser's complete burst payload), planes out:
  the wrapper rejects AUs shorter than 7 bytes BEFORE `a52_syncinfo`
  (syncinfo reads `buf[0..6]` unconditionally), and `a52_frame` runs ONLY
  on a syncinfo-accepted buffer — `a52_frame` re-derives header fields with
  no bsid guard (`halfrate[buf[5] >> 3]` indexes out of bounds for
  bsid ≥ 12), so syncinfo's acceptance is the bounds check (gate F7).
  Length mismatch → `AIUS_MALFORMED`; sample rate ≠ 48 kHz →
  `AIUS_UNSUPPORTED`. Then `a52_frame` + 6 × `a52_block` produce 1536
  frames.
- **The decode buffer is a padded scratch, not the parser's AU** (gate F1,
  the load-bearing finding): liba52's bit reader is UNBOUNDED — no end
  pointer, no bits-remaining check — and worst-case syntax consumption on a
  hostile payload is ~25 KB against a 128-byte minimum frame. The wrapper
  copies each AU into a zero-filled scratch of
  `AIUS_A52_MAX_FRAME + AIUS_A52_PARSE_PAD` (3840 + 32 KiB; the pad bound
  and its derivation recorded in NOTICE.md), so an overread parses zeros
  inside owned memory and returns a bounded, typed result (possibly a
  legal-looking OK — zero exponents parse; the guarantee is bounded and
  typed, never OOB). The pad tail is re-zeroed on every decode so the
  parse is deterministic, not shaped by the previous AU. This is ~36 KiB
  of arena the phase-4 sizing decision must carry.
- **Output flags request the stream's OWN coding**:
  `flags = syncinfo_flags & (A52_CHANNEL_MASK | A52_LFE)`, `level = 1.0`,
  `bias = 0`, no `A52_ADJUST_LEVEL` (dead weight under identity grants).
  Identity requests never enter `a52_downmix`/`a52_upmix`, which both
  sidesteps liba52's widening behaviour (a 3F1R request row returns 3F2R,
  not the stream coding) and the upstream 0.7.4 3F1R→3F2R typo
  (`downmix.c:586`, `samples + 1027` for `samples + 1024`) that would
  otherwise ship a latent defect-30-class channel fault no phase-1 vector
  can convict (gate F2). Plane mapping keys off the flags `a52_frame`
  RETURNS.
- **Channel order is liba52's trap to pin**: with `A52_LFE` granted, the
  output block order is LFE FIRST (LFE, L, C, R, SL, SR). The wrapper maps
  to `AIUS_CH_*` planes; the corpus `surround51_ident` tone test convicts
  any permutation on day one (defect 30's lesson, applied before the defect
  can exist).
- Scale: `float` in [-1, 1] × 8388607, saturating quantize to the 24-bit
  int32 plane contract (one quantize — the DSP chain re-widens to double at
  its boundary; acceptable double quantization at a 24-bit floor, same as
  the DTS path).
- **DRC: liba52's default (dynrng applied) is KEPT for phase 1.** Dolby's
  home-listening compression narrows loud-effect-to-dialogue range, which
  is the mission's direction; a config flag over `a52_dynrng` is a later,
  cheap addition if the operator wants line mode. Recorded so it is a
  decision, not an accident. Flag implementers note: `a52_frame` resets
  `dynrnge` every frame, so the disable call must follow EVERY
  `a52_frame`, not run once at create (gate F9); and the ffmpeg corpus
  carries no dynrng codes, so the differential suite exercises DRC only
  trivially.

### Native suite: `a52` + `a52-fuzz`

- `test/a52/test_a52.c` — differential decode of the four
  `tools/ac3_vectors` vectors against FFmpeg's `.ref.s32`: liba52 float vs
  FFmpeg float is NOT bit-exact (unlike the fixed-point DTS goldens), so
  the suite asserts a measured tolerance (stated in-test: max per-sample
  abs diff in 24-bit LSBs + minimum SNR, bounds set from the observed
  slack at implementation with margin), plus EXACT tone identity per plane
  (Goertzel, the `surround51_ident` assignment), block shape
  (1536 frames = 3 × 512 emission quanta), metrics sanity
  (steady-state `alloc_count` 0 after warmup; `arena_high_water` reported —
  the phase-4 arena decision's input), and decode-vs-DTS coexistence
  (both decoders arena-resident where the arena fits, else the measured
  answer feeds the phase-4 swap decision). Gate-hardened mechanics: a
  FRESH decoder per vector (the IMDCT overlap delay carries a previous
  vector's tail across a shared state — false red on the next first frame,
  F5); frames walked by `a52_syncinfo` length, never by scanning the
  2-byte 0x0B77 sync (payload false-positives, F12); the ffmpeg ref's
  FL,FR,FC,LFE,BL,BR interleave permuted against liba52's LFE-first plane
  order at the compare; and `ref_samples == 1536 × nframes` asserted as a
  precondition (the corpus is 4 frames per vector — trust syncinfo
  walking, not README arithmetic; F9/F10).
- `test/a52/fuzz_a52.c` — the dts-fuzz shape ported: 0x0B77 seams,
  deterministic seam-splice phase (loop seam byte-exact, adjacent step-16),
  random corruption strategies, the termination oracle (1 s soft / 8 s
  alarm) from the defect-31 hunt, typed-status + sane-block + stale-PCM
  poisoning contracts. liba52 does not CRC-check frames, so the fuzz
  posture is load-bearing, not ceremonial.
- `scripts/test.sh` gains `a52` and `a52-fuzz` arms; the suites table in
  the firmware README and the `registry` suite stay consistent (the
  registry suite fails the commit otherwise).

### Phase 1 delivered (2026-07-28)

Built to the contract above and green: `a52` (differential + identity +
arena) and `a52-fuzz` (seam hunt + random under the termination oracle),
both registered and `registry`-consistent.

- **Differential vs FFmpeg**: worst 1906 LSB / 69.3 dB SNR across the four
  vectors. Wider than the DTS goldens because AC-3 **dither** dominates —
  the spec reconstructs zero-bit mantissas with decoder-specific noise, so
  two correct decoders legitimately differ at low level. Bounds set at
  8192 LSB / 55 dB (~4x / ~14 dB margin), which still convicts a channel
  permutation, a gain error, or a block misalignment — the differential's
  actual job. Channel identity is asserted EXACTLY (Goertzel per plane).
- **Arena high-water 90,464 B, zero allocations per decode.** The phase-4
  input, and it is already decisive: a52 90,464 + dcadec 62,560 + DSP
  staging 32,768 = 185,792 against the current 131,072-byte arena.
  **Two resident decoders do not fit — phase 4 must grow the arena or
  swap at class time**, as the risk section anticipated. The ~36 KiB
  parse scratch is inside that 90,464.
- **The fuzz suite paid for itself twice, on its first two runs**, finding
  two memory-safety defects in stock liba52 0.7.4 (unfixed upstream and in
  Debian's patch series). Both required vendored edits — neither could be
  accommodated from outside — so this vendor dir carries TWO documented
  modifications where dcadec-core carries none
  ([`vendor/a52-core/NOTICE.md`](../../firmware/vendor/a52-core/NOTICE.md)):
  1. `a52_bit_allocate` indexes `baptab` up to 4 entries below the table
     for `floorcod == 7` — **reachable from a LEGAL stream**, so no input
     validation could have prevented it. Clamped at all five sites.
  2. `a52_init` leaves the state uninitialized and nothing rejects a
     first block coding `EXP_REUSE`, so `factor[exp[i]]` (25 entries) read
     garbage exponents (`exp[i] == 190` caught). Zeroed at init, which
     bounds the class by construction.

  Neither changes a conformant decode (the differential is identical
  before and after). **Consequence for phase 3/4: keep the fuzz suite in
  the pre-flash gate** — liba52 is materially less hardened than dcadec,
  and the box's input is arbitrary S/PDIF from a TV.

### Out of scope for phase 1

Pipeline dispatch (phase 3), ISR restructure (phase 2), target build and
arena growth (phase 4), DRC config flag, E-AC-3 (never rides S/PDIF).
