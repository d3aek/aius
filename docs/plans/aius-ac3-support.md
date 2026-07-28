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
3. ~~**Block shape**: AC-3 access units decode 1536 frames; the enqueue cap
   and DSP staging are 512. Emit three 512-frame blocks per AU.~~
   **SUPERSEDED by phase 2a (gate F6):** the decoder emits liba52's own
   six 256-frame blocks, and `enqueue`'s cap is 512, so 256 is accepted
   directly. **Phase 3 has no splitter to build** — 256 divides 512, so
   nothing is foreclosed, but do not go looking for the 3 × 512 split.
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
  ~~**MEASURED 2026-07-28 and CONFIRMED: they do not fit.** Phase 4 must
  grow the arena or swap to a single resident decoder at class time.~~
  **RESOLVED by the phase 2 contract, and the answer reversed: they DO
  fit.** The 90,464 B measurement was almost entirely adapter overhead, not
  liba52; phase 2a takes AC-3 to ~27 KB, giving 122,304 against 131,072
  with both decoders resident. Neither growing the arena nor swapping is
  needed — and both were investigated and rejected on their own merits.
  See [§Phase 2 contract](#phase-2-contract-2026-07-28--decode-off-the-isr-and-the-footprint-that-makes-it-fit).
  This risk is closed.

## Phases (each gated per house protocol)

1. ~~Corpus + `aius_a52` wrapper + native decode suite (host-only).~~
   **DONE 2026-07-28** — see §Phase 1 delivered.
2. Defect-31 restructure: decode off the ISR, both codecs, with the
   watchdog/starvation interactions re-proven on the pipeline suite.
3. Pipeline dispatch + squelch/re-type suite extension (the
   framed-but-unsupported test moves to a type the box still refuses).
   **No block split** — 2a's 256-frame emission already fits `enqueue`.
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
  `a52_downmix`/`a52_upmix` are still called but an identity request
  matches no case in either switch, so both no-op — which sidesteps both
  liba52's widening behaviour (a 3F2R *request* over a 3F1R *stream* is
  granted as 3F2R, `table[7][5]`) and the upstream 0.7.4 3F1R→3F2R typo
  that widening reaches (`downmix.c:586`,
  `samples + 1027` for `samples + 1024`) that would otherwise ship a latent
  defect-30-class channel fault no phase-1 vector can convict (gate F2).
  Plane mapping is derived from the SYNCINFO flags, which agree with the
  flags `a52_frame` returns for every supported mode.
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
  (1536 frames per AU — ~~3 × 512 emission quanta~~ SUPERSEDED by phase 2a,
  which emits 6 × 256 and asserts that shape instead), metrics sanity
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
- **Arena high-water 90,464 B, zero allocations per decode.** As measured
  this looked decisive against residency: 90,464 + dcadec 62,560 + DSP
  staging 32,768 = 185,792 against a 131,072-byte arena. ~~Two resident
  decoders do not fit — phase 4 must grow the arena or swap at class
  time.~~ **That reading was wrong and the phase 2 contract reverses it:**
  ~73 KB of the 90,464 is adapter overhead (the ~36 KiB parse scratch and
  the ~36 KiB whole-AU planes), both removable, taking AC-3 to ~27 KB and
  both decoders inside today's arena. See
  [§Phase 2 contract](#phase-2-contract-2026-07-28--decode-off-the-isr-and-the-footprint-that-makes-it-fit).
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

### Out of scope for phase 1 (see the phase 2 contract below for 2a)

Pipeline dispatch (phase 3), ISR restructure (phase 2), target build and
arena growth (phase 4), DRC config flag, E-AC-3 (never rides S/PDIF).

## Phase 2 contract (2026-07-28) — decode off the ISR, and the footprint that makes it fit

Phase 2 closes defect 31 for BOTH codecs by moving bitstream decode out of
the audio ISR. Folded into it (operator direction 2026-07-28, after the
phase-1 arena measurement): the AC-3 footprint reduction, because the same
change delivers both — the reason AC-3 costs 90 KiB is that the wrapper
accumulates a whole access unit before returning, and that accumulation is
also the ISR burst defect 31 is about.

**Slice 2a is separable, host-only, and lands first.** 2b is the target-side
restructure.

**Owned paths** (2a): `firmware/vendor/a52-core/{a52.h,bitstream.c,
bitstream.h,a52_internal.h}`, `firmware/lib/aius_a52/{include,src}`,
`firmware/test/a52/{test_a52.c,fuzz_a52.c}`, `firmware/scripts/test.sh`,
`firmware/vendor/a52-core/NOTICE.md`, this plan. Relative to
``.

### The residency question this settles (measured, not assumed)

Phase 1 measured AC-3 at 90,464 B and concluded two resident decoders do not
fit. That framing was wrong: **almost none of it is liba52's.**

| Item | Bytes | Owner |
|---|---|---|
| Parse scratch (`MAX_FRAME + 32 KiB pad`) | 36,608 | adapter, only because liba52's reader is unbounded |
| Output planes (6 × 1536 × 4) | 36,864 | adapter, only because it accumulates a whole AU |
| liba52 sample buffer (256 × 12 × 4) | 12,288 | liba52 |
| State + handle + alignment | ~4,704 | liba52 + adapter |

The DTS adapter allocates NO planes — `block->planes` points straight into
dcadec's own `output_samples` (`aius_dts.c:259`), which is why it is cheap.
Both adapter-side items above are removable, and removing them is what 2a
is. Resulting arithmetic:

- planes 36,864 → **6,144** (emit per 256-frame block, saving 30,720)
- scratch 36,608 → **3,840** (bound the reader, saving 32,768)
- AC-3 resident ≈ **26,976 B**
- dcadec 62,560 + DSP staging 32,768 + 26,976 = **122,304 vs the existing
  131,072-byte arena — both decoders resident, no growth, no swapping,
  8,768 B spare.**

That spare is thin (6.7%), and **it may be the only margin there is** — the
OCRAM fallback is NOT established (gate F13). The arena is a plain static
array in DTCM today (`optical_main.cpp:1048`), so growing it in place was
never available: DTCM's whole free-for-locals zone is 48,800 B and that
region is also the stack. But relocating it to OCRAM is not simply a matter
of free bytes: the 32,768 B DSP staging carve was already VETOED from OCRAM
by the defect-26 link-time heap floor with only 13,312 B of slack
(`optical_main.cpp:1305-1311`), and moving the whole 131,072 B arena is 4×
what was rejected. The gate is the boot-heap floor, not free space. **Phase
4 must corroborate the OCRAM route against that floor before relying on it,
or treat 122,304/131,072 as the budget.**

**Dynamic decoder loading is REJECTED, with the reasoning recorded so it is
not re-opened.** Dynamic code loading buys nothing: decoder code is
cold-placed in flash by an existing build guard and flash is 95% free, while
the constraint is RAM. Dynamic instantiation (create/destroy at class change)
is *nominally* legal under the arena contract, which permits allocation
"during a declared stream-format change", and its re-init cost would be
masked by the 170–255 ms mute a format switch already incurs — **but it does
not actually work here, deterministically (gate F15).** The arena is a bump
allocator with no per-block free, and `__wrap_free` on an arena pointer is a
no-op, so destroy/create at each class change LEAKS the whole footprint
every switch and fails after a handful of them — not probabilistically, but
by construction. Making it work would require a per-decoder sub-arena reset,
i.e. more machinery than 2a, to buy memory 2a shows we do not need. Beyond
that it would introduce an allocation that can fail mid-stream, leaving no
decoder exactly when audio should resume: the silent-death shape defects 28
and 31 taught us to design out.

### Slice 2a — per-block emission + a bounded reader (host-only)

**Three ORDERED steps, each committed separately (gate F2).** The 32 KiB pad
is today's containment and the fuzz suite is the only proof the bound
replaces it; removing the pad in the same commit that rewrites the API the
fuzz drives could ship a green suite over *worse* memory safety than phase 1
— a harness that only reached `begin_access_unit` would leave the entire
mantissa/exponent/bit-allocation surface unfuzzed, and that is exactly where
both phase-1 defects lived. So the bound is proven while the pad still
exists, and only then does the pad go.

**Step 1 — vendored edit 3: bound liba52's bit reader (pad still in place).**
`bitstream_fill_current` (`bitstream.c:44-50`) is the SOLE dereference of
`buffer_start`, `a52_bitstream_get_bh`/`_bh_2` each call it exactly once, the
inline fast paths touch only `current_word`, there are no byte-wise reads,
and max `num_bits` is 24 so one fill always suffices. One choke point, so an
end pointer plus zero-fill is sufficient — provided all five of these hold,
each of which the gate found the first draft did not state:

- **(a) The setter runs in `begin_access_unit`, BEFORE `a52_frame`, and
  `a52_bitstream_set_ptr` must NOT clear the end.** `a52_frame` calls
  `set_ptr` itself (`parse.c:154`), so a naive "reset to unbounded there"
  drops the bound for the whole frame.
- **(b) A BYTE end pointer (`scratch + frame_length`), never a word count.**
  `set_ptr` aligns *down*, so any word-derived bound is off by one word.
- **(c) FAIL-CLOSED.** With edit 2's `memset` an unset end is NULL and
  `if (buffer_start >= buffer_end)` correctly yields zeros. The form
  `if (buffer_end && ...)` fails OPEN and is forbidden.
- **(d) The field lives in `a52_state_s` (`a52_internal.h`).** Safe: `a52.h`
  keeps the type opaque, the adapter and both suites include only `a52.h`,
  and all five vendor TUs compile in one invocation.
- **(e) The setter is declared in `a52.h`, not `bitstream.h`.**
  `bitstream.h` has no include guard and declares inline functions that
  dereference the state, so including it to reach the setter would drag in
  `a52_internal.h` and destroy (d)'s opacity argument. `a52.h` is guarded
  and keeps `a52_state_t` opaque, so it is the only home that preserves
  the property (d) claims.

**Step 1's proof must be POSITIVE, not self-referential (gate R1 — the
blocker on the first amendment).** "Assert `buffer_start` never passes
`scratch + frame_length`" is circular: if the guard is present that holds by
construction, and it cannot distinguish a bound that is armed and binding
from one that was never armed while the 32 KiB pad silently absorbs the
overread — which is exactly the state step 2 makes fatal. Four failure modes
are green under it and out-of-bounds after step 2: setter never called;
setter called *after* `a52_frame`; `set_ptr` clearing the end (the hazard
(a) names); end set to the scratch end rather than the frame end. So step 1
proves instead:

- `bitstream_fill_current` counts total fills and fills SUPPRESSED by the
  bound, and asserts at every fill that `buffer_end` is non-NULL and equals
  `scratch + frame_length` (frame-derived, never pad-derived).
- **The fuzz must report a NON-ZERO suppressed count** — that is the
  evidence the bound is actually reached and binding, which no
  self-consistent assertion can supply.
- For step 1 only, **everything past `frame_length` is filled with `0xFF`**
  rather than zeros — not just the nominal pad region. The bytes between
  `frame_length` and `AIUS_A52_MAX_FRAME` are also past the bound, and at
  typical 48 kHz frame sizes that is up to ~1,280 B in which a short
  overread would otherwise read zeros and fail to diverge. With a correct
  bound the differential and the fuzz's ok/reject counts are unchanged;
  with a broken one they diverge loudly. This turns the pad into an oracle
  for the one commit where it is both net and instrument. (Supplementary:
  all four failure modes above are already caught independently by the
  suppressed-fill counter and the `buffer_end` identity assert.)

**Step 2 — remove the pad**, taking the scratch to one frame (3,840 B).
Same suites, unchanged, must stay green: with the pad gone the bound is now
load-bearing, so this commit is the real test of edit 3. Step 2 also retires
the prose that describes the pad as the safety mechanism, all of it inside
owned paths and all of it now wrong: `NOTICE.md` §3, and `aius_a52.h`'s
SAFETY SHAPE paragraph plus the `AIUS_A52_PARSE_PAD` comment (gate R10).

**Step 3 — iterator API + suite rewrite.** Replace whole-AU decode with
`aius_a52_begin_access_unit()` (validates, runs `a52_frame`, arms the
iterator) + `aius_a52_decode_next_block()` (decodes ONE 256-frame block into
reusable planes). The caller decides when each block is decoded — the
slicing 2b needs, taking worst-case work per service slice from a whole AU
to a sixth of one.

**The invariant this trades away, stated because it is not free (gate F1).**
Today a malformed AU emits NOTHING: `a52_block` returns 1 from eight sites
reachable from a corrupt bitstream (`parse.c:584,592,664,680,693,701,733,
737`), the adapter aborts the whole AU, and `fuzz_a52.c` asserts that as a
property. Under the iterator, blocks 0..k-1 are already in the caller's
hands when block k fails. **Policy: `decode_next_block` returns the typed
error, the iterator disarms, and the caller MUST treat every prior block of
that AU as discardable.** The fuzz gets a matching property (see below).

**How phase 3 holds the AU, named here because the naive answer gives back
2a's whole saving (gate R3).** A pipeline-side accumulator would be
6 × 1536 × 4 = 36,864 B — precisely the 30,720 B this slice exists to
recover, handed straight back. **It is therefore forbidden.** The transmit
ring already provides the mechanism: `aius_spdif_tx24_stage` / `_commit`
guarantee a preempting consumer never sees a half-written region until
commit, so phase 3 stages all six blocks and commits after block 6, holding
zero extra buffer. On a mid-AU error it simply never commits.

**Iterator state machine, specified (gate F8):**

- `decode_next_block` without a live `begin` → `AIUS_INVALID_ARG`.
- A 7th call errors and never runs a 7th `a52_block`.
- A mid-AU error disarms the iterator.
- `block->planes` are valid only until the next `decode_next_block`,
  `begin_access_unit`, or `destroy`; the caller must not hold them across a
  call.
- `begin_access_unit` while an AU is in flight ABANDONS and re-arms.
  Recovery is correct (`a52_frame` re-arms the reader) but **lossy**: the
  IMDCT overlap in `state->samples[1536..3071]` carries the abandoned
  block's un-overlapped tail into the next AU's block 0 — the same hazard
  the differential's fresh-decoder-per-vector rule already documents.
  Abandonment also emits a partial AU with NO error, so the discard policy
  above does not reach it (gate R8): **abandonment is permitted only at a
  stream discontinuity** — carrier edge, squelch, or re-type — where the
  partial output is being dropped anyway.
- **Scratch lifetime (gate F7): the next AU must NOT be copied into scratch
  until block 6 of the current AU completes**, because all six `a52_block`
  calls read from it through the bitstream state. Easy to violate in 2b's
  ring-fed service loop, and it produces plausible garbage audio rather than
  a crash. `begin_access_unit`'s abandon-and-re-arm rule is what makes the
  violation impossible to express accidentally.
- The shim is armed and disarmed PER CALL, not held across the AU
  (`g_arena` is a single global and 2b runs decode beside other allocators).
- `begin_access_unit` derives the channel mapping from the SYNCINFO flags
  (as the current adapter does); they agree with the flags `a52_frame`
  returns for every supported mode, but begin is exactly where a cached
  mapping could take the wrong one, so the source is named.

**Proofs that must be preserved verbatim through the suite rewrite
(gate F4)** — each exists because it caught something:

- The termination oracle stays scoped to ONE ACCESS UNIT (`begin` + 6
  blocks bounded as a unit). Porting it per-block would sextuple the
  effective bound — gutting the defect-31 oracle inside the phase that
  exists to close defect 31.
- Block-shape assertion becomes "exactly 6 blocks of 256 frames per AU, and
  a 7th call errors" — the 1536-frame shape proof must not simply vanish.
- The full-plane XOR read (ASan's plane-sizing tripwire) runs over all 6
  blocks; the block is poisoned before EVERY call, now 7 per AU.
- The differential's whole-vector Goertzel needs 6144 contiguous samples per
  channel, so the 6 × 256 accumulation must be in order.
- `ref_samples == naus × 1536` precondition and the fresh-decoder-per-vector
  rule both stay.
- NEW: a counter for feeds that actually reached `a52_frame`/`a52_block`.
  The fuzz currently cannot distinguish "rejected at the adapter's length
  guard" from "parsed and rejected", so a stricter guard could gut its reach
  and still print green (gate F16).

**Step 3's numeric proof, ASSERTED not printed (gate F5):** the arena pass
must assert `arena_high_water <= 28000`, not merely report it. An unasserted
headline number is free to regress silently. Expect ~26,992 rather than the
26,976 computed above once the iterator's per-AU state crosses an allocator
bucket; the figure is host LP64-derived, while dcadec's 62,560 is an
on-target ILP32 measurement, so treat single-byte agreement between them as
coincidence, not proof (gate F12).

**The differential must remain byte-identical** (1906 LSB / 69.3 dB) at
every step, and this is provable rather than hopeful: decode order and count
are unchanged so `dither_gen`'s LFSR advances identically, the quantize is
per-sample, and edit 3's synthesized zeros are exactly what the tail-zeroed
pad already returns for a past-end read.

### Slice 2b — decode off the audio ISR (target)

- **AC-3 slices; DTS moves.** dcadec decodes an AU in one `core_parse` +
  `core_filter` pair and cannot be sliced without vendor surgery we are not
  doing, so DTS's fix is relocation to epoch context. AC-3 gets both. Record
  the asymmetry rather than pretending the codecs are symmetric.
- Decode runs in the runtime's epoch/service context, feeding the existing
  transmit ring; the audio ISR's job shrinks to moving words. **Ring depth
  must be sized against the stage/commit HOLD-BACK, not just latency and
  jitter (gate R4)**: the ring is 2048 frames, and holding a whole AU
  uncommitted occupies 1536 of them (75%), leaving 512 committed frames —
  10.7 ms — for the ISR to drain while an AU is in flight. That, plus worst
  service latency and jitter, is the real criterion. `enqueue`'s free-space
  check is per-block today; under stage/commit the whole-AU free must be
  checked up front instead.
- Watchdog, starvation, squelch and carrier-edge interactions re-proven on
  the `pipeline` suite — the restructure moves the code that those state
  machines drive, so their oracles are the regression surface.
- **Per-AU fixed overhead rises** (gate F17): sink calls go from 1 to 6 per
  access unit, and each `enqueue` re-runs the config-apply check and its
  per-call setup. Budget for it in the cycle measurement rather than
  discovering it on the meters.
- **The defect-31 hypothesis this tests.** The native seam hunt
  (`bfa73a6f8`) found no non-terminating dcadec parse across 29,435
  seam-shaped feeds, leaving the ISR-time storm as prime suspect. 2b removes
  that suspect. If sustained decode still dies by watchdog afterwards, the
  hypothesis was wrong and the next suspect is the transmit path, not the
  parser.

### Out of scope for phase 2

Pipeline dispatch by Pc data type and the 512-frame staging split (phase 3),
arena relocation (phase 4, only if the 8,768 B spare proves uncomfortable),
DRC config flag.
