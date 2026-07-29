---
title: Aius AC-3 decode support
type: plans
scope: component
component: aius
status: in-progress
subtype: design
tags: [aius, firmware, ac3, dolby, decode, iec61937]
updated: 2026-07-29
toc: true
---

# Aius AC-3 decode support

Decode AC-3 (Dolby Digital) bitstreams alongside DTS Core, because AC-3 is
the mission format: the project exists to replace the TV's own unintelligible
2-channel downmix, TVs emit AC-3 5.1 over TOSLINK (DD+/Atmos is transcoded
down to AC-3 by most TVs, though E-AC-3 CAN ride S/PDIF — see §The format
space and defect 34), and a TV set to PCM output hands the box
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

1. **Decoder**: the Aius-owned identity-only liba52 fork (mature portable C,
   GPL — fine for this personal, non-commercial build) under
   `firmware/lib/aius_a52/a52/`, wrapped as `aius_a52` mirroring the
   `aius_dts` shape: arena-backed create, one access-unit decode call,
   metrics, no-heap steady state via the alloc shim. Float output scaled to
   the 24-bit int32 plane contract.
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

- **ISR budget — closed by Slice 2b.** Bitstream ingress alone remains in the
  audio callback; parser, decoder, DSP, recorder and transmit work now run in
  the bounded AUDIO epoch checkpoint. The 30-minute DTS gate held AUDIO service
  to 2.401 ms and normal epochs to 3.036 ms without a watchdog reset, closing
  defect 31 before AC-3 dispatch.
- **Arena sizing.** ~~Two resident decoders may exceed the 128 KiB arena~~
  ~~**MEASURED 2026-07-28 and CONFIRMED: they do not fit.** Phase 4 must
  grow the arena or swap to a single resident decoder at class time.~~
  **RESOLVED by the phase 2 contract, and the answer reversed: they DO
  fit.** The 90,464 B measurement was almost entirely adapter overhead, not
  liba52; phase 2a takes AC-3 to ~27 KB, giving ~127,504 against 131,072
  (including the ~5.2 KB pipeline instance) with both decoders resident. Neither growing the arena nor swapping is
  needed — and both were investigated and rejected on their own merits.
  See [§Phase 2 contract](#phase-2-contract-2026-07-28--decode-off-the-isr-and-the-footprint-that-makes-it-fit).
  This risk is closed.

## Phases (each gated per house protocol)

1. ~~Corpus + `aius_a52` wrapper + native decode suite (host-only).~~
   **DONE 2026-07-28** — see §Phase 1 delivered.
2. ~~Defect-31 restructure: decode off the ISR, both codecs, with the
   watchdog/starvation interactions re-proven on the pipeline suite.~~
   **DONE 2026-07-29** — Slice 2b1–2b3, commit `cb43fd560`.
3. ~~Pipeline dispatch + squelch/re-type suite extension (the
   framed-but-unsupported test moves to a type the box still refuses).
   **No block split** — 2a's 256-frame emission already fits `enqueue`.~~
   **DONE 2026-07-29** — parser scope, deferred dispatch, observability,
   target integration and UI truth are complete; live AC-3 remains phase 4.
4. ~~Bounded target integration: arena measurement, cycle meters, exact-image
   flash, SD replay, CM6206 optical input, and committed-wire channel
   identity.~~ **DONE 2026-07-29.** The remaining mission test is the
   four-hour live-TV run plus by-ear dialogue validation, deliberately
   scheduled after the OTA investigation verdict.

`doc-index --validate-outstanding-work` cannot validate this plan because
document discovery does not yet index `src/hardware/**/docs`; it reports
`outstanding.snapshot.plan-absent`. The required component-wide disposition
is therefore recorded manually below, using the same canonical grammar.

## Outstanding-work audit

- `docs/plans/aius-capability-gate.md` — `coordinate`: Preserve the shipped DTS capability contract; AC-3 reuses its codec ABI, arena, and target timing evidence while phase 2 moves DTS decode out of the ISR.
- `docs/plans/aius-cost-model.md` — `unrelated`: Decoder firmware does not change the prototype BOM, layer-count decision, or supplier pricing.
- `docs/plans/aius-dsp-config-api.md` — `coordinate`: AC-3 feeds the existing layout-aware DSP and phase 3 updates its status labels. Slice 2b moves the block producer to epoch context but preserves block-boundary apply, meter publication, and the rule that a refused block applies no configuration and advances no DSP state; configuration semantics remain owned by that plan.
- `docs/plans/aius-hardware-spec.md` — `coordinate`: Phase 4 must preserve the hardware spec's optical path and memory gates while proving the decoder's target footprint.
- `docs/plans/aius-network-hardening.md` — `coordinate`: AC-3 phase 2 owns the defect-31 decode-off-ISR fix, the new AUDIO checkpoint, and the sustained DTS proof; the hardening plan remains the defect ledger and retains defect 34. Existing one-feed watchdog and recovery-safe scheduling semantics do not change.
- `docs/plans/aius-open-work-register.md` — `coordinate`: The register remains a projection of authoritative work and must strike defect 31 only after Slice 2b's capture-plus-HTTP hardware soak, never at code completion alone.
- `docs/plans/aius-optical-prototype.md` — `coordinate`: Slice 2b moves the measurement and flight-recorder block producer from audio-ISR to epoch context without changing their committed-wire tap; phase 4's card replay and live-TV checks extend the existing optical validation without replacing its broader prototype gates.
- `docs/plans/aius-ota-upgrade.md` — `coordinate`: AC-3 adds no update format, rollback state, or OTA transport surface, but the owner has deliberately scheduled its four-hour live-TV and by-ear gates after the OTA investigation verdict.
- `docs/plans/aius-replay-manager.md` — `coordinate`: Slice 2b preserves replay's audio-ISR consumer, exact 256-word pop cadence, short-pop underrun semantics, and epoch refill ownership; only the pop destination changes from synchronous decode to the ingress ring. Phase 4 adds AC-3 IEC fixtures through the same transport.
- `docs/plans/aius-sample-rate-following.md` — `coordinate`: AC-3 remains 48 kHz-only. Slice 2b's 96,000-word/s ingress and 187.5-block/s service arithmetic is explicitly 48 kHz-only, so later rate following must resize or derive the quotas and ring rather than inherit hidden 48 kHz constants.
- `docs/plans/aius-sd-config-web.md` — `coordinate`: Phase 3 edits the shared status UI and must preserve the shipped dashboard and SD-served page contracts.
- `docs/plans/aius-tls-termination.md` — `coordinate`: TLS is parked, but its future epoch-latency budget must consume phase 4's measured decode cost rather than its current DTS-only assumption.

## The format space after AC-3 — why this is the last decoder

Recorded 2026-07-28 in answer to "what else could we conceivably support,
7.1 varieties perhaps", so the reasoning is not re-derived. **The decode
roadmap ends with AC-3**, and one number decides most of it: optical S/PDIF
at 48 kHz carries 2 × 16 bits per frame, so **1.536 Mbit/s of IEC 61937
payload, total.**

| Format | Fits optical? | Max channels |
|---|---|---|
| PCM 2.0 | Yes (it is the baseline) | 2 |
| AC-3 (Dolby Digital) | Yes, ≤ 640 kbps | 5.1 |
| DTS Core | Yes, ≤ ~1.5 Mbps | 5.1 (6.1 via ES extensions) |
| E-AC-3 (DD+) | **Yes**, up to the full 1.536 Mbps — most TVs transcode to AC-3, but one configured to pass DD+ emits it. See defect 34. | 7.1 |
| TrueHD / Atmos, DTS-HD MA / DTS:X | No — 6–18 Mbps, HDMI only | 7.1+ |
| MPEG-1/2 audio, AAC / HE-AAC | Yes technically (DVB broadcast) | 5.1 |

**The E-AC-3 row corrects an error this document carried from its first
draft** ("E-AC-3 never rides S/PDIF"), caught by the phase-3 gate and
verified arithmetically: a type-21 burst has a 6,144-frame period, so it
carries 6,144 × 2 × 16 = 24,576 B per burst at 7.8125 bursts/s = **192,000
B/s = 1.536 Mbps**, which is the entire S/PDIF payload rate. Broadcast and
streaming DD+ runs 384–768 kbps and fits easily; Blu-ray's typical 7.1 DD+
rate (~1.7 Mbps) does not, and E-AC-3's own ceiling is far higher (6.144
Mbps), so the transport bounds this, not the codec. The reason the box rarely meets E-AC-3 is TV
*configuration* — sets transcode DD+ to AC-3 for legacy receiver
compatibility — not physics. This is what makes defect 34 a real hazard
rather than a hypothetical one.

**So 7.1 over optical is possible after all, through exactly one door:
E-AC-3.** AC-3 is 5.1 by specification and DTS Core reaches 6.1 only through
matrixed ES extensions, so of the formats that fit, DD+ alone carries 7.1.
Three things stand between that observation and a feature:

1. **liba52 cannot decode E-AC-3.** It is an AC-3 decoder; E-AC-3's
   enhancements (transient pre-noise processing, spectral extension,
   channel coupling changes) need a different codebase entirely — realistic
   candidates are heavier than liba52 by a wide margin, against an arena
   already at ~2.7% spare.
2. **`AIUS_MAX_CHANNELS` is 6**, with a 6-bit channel mask referenced across
   18 sites spanning the codec ABI, pipeline, DSP matrix, downmix and
   meters. 7.1 is a data-model change before it is a decoder change.
3. **Defect 34 must be fixed first regardless**, because today an E-AC-3
   link is not merely undecodable but actively harmful.

HDMI eARC remains the only route to the lossless 7.1 formats (TrueHD/Atmos,
DTS-HD MA, DTS:X) and is a hardware redesign well beyond this board. The 7.1
USB audio interface on the bench is an output and measurement device, not a
route to 7.1 content.

Of the remaining formats, **AAC is the only one with a real-world case**
(some DVB broadcast chains), and televisions transcode it to AC-3 or PCM
before the optical port regardless. PCM plus AC-3 covers essentially
everything a TV will hand this box; DTS covers discs and media players.

**Strategic conclusion:** once AC-3 is live on target, further capability is
better spent on the DSP and the dialogue-intelligibility mission this
project exists for than on additional decoders. **E-AC-3 is the one
defensible exception** — not for 7.1, which the operator's 5.1 speakers
cannot use anyway, but because defect 34 means the box must eventually do
*something* deliberate about a type-21 link, and "decode it" is one of the
two honest answers. The other is "stay dark", which is far cheaper and is
the recommended path unless a real DD+ source appears on this bench.

## Decoder alternatives — the comparison phase 1 should have carried

Investigated 2026-07-28, after phase 1 shipped, because liba52 fit less well
than expected: two memory-safety defects in the first two fuzz runs, an
unbounded reader costing a 32 KiB pad, no CRC validation, four compile
carve-outs. The choice was made without a comparison; this is that
comparison, recorded so it is not re-opened on discomfort. The table preserves
the candidates at that comparison boundary; F3a has since bounded the fork's
reader.

| Candidate | License | Maintained / fuzzed | Reader bounded | E-AC-3 | RAM working set | Effort | Per-frame alloc |
|---|---|---|---|---|---|---|---|
| liba52 0.7.4 (shipped) | GPL-2 | dead since 2002 | No | No | 26,976 B post-2a | done | 0 (measured) |
| liba52 0.8.0 | GPL-2 | released 2022, Adélie | No (byte-identical reader) | No | same | drop-in, API-compatible | 0 |
| Rockbox liba52 | GPL-2 | 2005 CVS snapshot | No | No | lower (const tables) but fixed-point | 11 files | 0 |
| FFmpeg `ac3dec` subset | LGPL-2.1 | HEAD; OSS-Fuzz per-decoder | **Yes** (+64 B pad) | **Yes** | **≥50 KB trimmed, 235 KB untrimmed** | 92 files / 34,655 lines | **15 malloc/free per frame** |
| Bespoke, written to our constraints | ours | n/a | Yes by construction | No (out of reach) | **~20 KB** | ~1,500–2,000 lines, 8–12 sessions | 0 |

**FFmpeg is the only technically superior library, and it does not fit.**
`sizeof(AC3DecodeContext)` is **235,216 B** against **30,544 B** of actual
arena headroom (131,072 − dcadec 62,560 − staging 32,768 − pipeline 5,200).
The trim ladder bottoms out at ~50 KB for AC-3 alone — still 1.7× over —
and ~94 KB with E-AC-3. Two further blockers make it worse than the
arithmetic suggests: it performs **15 malloc/free pairs per frame**
(~14 KB/s against a bump allocator that never frees, breaking the
`alloc_count == 0` contract our suites assert), and `av_tx` reserves
~262 KB of BSS in one object file. Driving it without `AVCodecContext` is
feasible — the decoder touches only 9 context fields — but that means
forking the calling convention, which forfeits the "verbatim vendor,
accommodate from outside" discipline that made dcadec cheap and turns every
upstream resync into a manual merge. The one real gain we would get is
bit-exact goldens, since our 1906-LSB tolerance exists only because FFmpeg
is the reference.

**Bespoke is more viable than it sounds, and stays the contingency.**
~20 KB working set beats liba52's 27 KB; a bounded reader and CRC come free;
per-block emission is ours to define; tables live `const` in flash. AC-3 is
a frozen format (unchanged since 1995), so ownership carries no maintenance
treadmill, and the corpus, differential harness, Goertzel identity check and
fuzz oracle already exist to drive development. The cost is ~10× the
vendoring effort, the bit-allocation path must be bit-exact or the stream
desynchronises, and **E-AC-3 would remain out of reach** — so it does not
answer defect 34 either.

### Decision: stay with liba52, and take the two cheap wins

1. **Rebase the vendor tree to 0.8.0.** It fixes the `samples + 1027` typo
   we currently dodge by construction (it stops being a latent
   defect-30-class fault), zeroes `current_word` at init, corrects a
   `uint8_t` underflow in three places, and resets `clev`/`slev` per frame.
   It is API-compatible in float mode. **Cost: the dither level changes
   0.707 → 0.75, so the differential must be re-baselined.** Both of our
   edits remain necessary — 0.8.0's reader is byte-identical and its
   `baptab` sites are still unclamped, which means our fuzz findings are
   live against maintained upstream rather than against a 2002 fossil.
2. **CRC-16/ANSI validation now lives at the fork's frame entry point
   (F3c DONE).** AC-3 frames
   carry CRC1/CRC2 and upstream liba52 ignores both; validating the whole frame
   before decode rejects most corruption before liba52 sees a byte. This
   closes the gap that most justified looking elsewhere, without changing
   decoders.

Phase 2a's bounded-reader edit stays regardless: it is the right call, and
FFmpeg would have forced an equivalent structural edit anyway.

**Revisit if** a third memory-safety defect appears (the existing stop
condition), or if the arena ever gains ~25 KB of headroom, at which point
FFmpeg's E-AC-3 coverage would change the defect-34 calculus.

### SUPERSEDED 2026-07-28 by the decision to FORK (operator-approved)

The "stay with liba52 as a verbatim vendor + documented edits" posture above
is replaced by: **fork it, gut what we do not use, and bring it up to our
spec.** The two cheap wins survive inside the fork; what changes is the
discipline.

**Why forking beats both alternatives.** We have already de facto forked —
two edits shipped, a third contracted — and we are paying the costs of
pretending otherwise. The verbatim-vendor discipline is inherited from
dcadec, where it earns its keep because dcadec needs zero edits. Here it
buys upstream resync worth approximately nothing: two releases in
twenty-four years, and **both defects we found are still live in the newest
one**. Against a from-scratch decoder (~20 KB, 8–12 sessions) the fork lands
~27 KB in 1–2 sessions while keeping a working decoder, the corpus, both
suites and the differential harness. The 7 KB delta does not buy eight
sessions.

**What the fork gets us that the vendor posture cannot:**

1. **Delete `downmix.c` outright** — 655 lines, 25% of the tree, never
   executed because we always request identity output. The
   `samples + 1027` typo goes with it *permanently*, which is strictly
   better than 0.8.0's fix of it.
2. **Restore sanitizer coverage.** We currently compile the whole tree with
   `-Wno-shift-negative-value` and `-fno-sanitize=shift`, disabling UBSan
   shift checking across the decoder — exactly where both defects lived.
   Fixing `tables.h`'s `(-2 << 15)` quantizer macros and the bitstream
   idioms lets those flags go. **This is the single largest safety win
   available and it is unavailable without forking.**
3. **Put fixes where they belong**: the reader bound inside the reader, CRC
   validation inside the decoder rather than bolted to the adapter, and a
   real overlap-reset entry point for phase 2a's `abandon` instead of
   reaching into `a52_samples(state) + 1536` and depending on internal
   layout.

**Placement:** the tree moves OUT of `vendor/`, which in this repo means
verbatim upstream (dcadec, NativeEthernet), into `lib/`. `NOTICE.md` becomes
a divergence log — upstream origin, what we deleted, what we changed, why —
rather than a list of apologies for edits. GPL-2 obligations are unchanged;
a fork is still a derivative work.

#### The identity-only reduction (surveyed 2026-07-28, before any edit)

Deleting `downmix.c` is not just removing a file: `parse.c` calls into it in
four places, and one of them performs the output-flags negotiation. The
survey, so the implementer does not have to re-derive it:

- `parse.c:168` — `state->output = a52_downmix_init(acmod, *flags, level,
  clev, slev)` is the flags negotiation. Under our identity request it
  returns the stream's own coding. Replacement: grant
  `*flags & A52_CHANNEL_MASK` (~~`state->acmod` directly~~ — superseded by
  correction 2 below; the raw `state->acmod` is wrong for A52_DOLBY), plus
  the `A52_LFE` bit at `:172-173` unchanged, leaving `*level` untouched.
- `parse.c:776` — `chanbias = a52_downmix_coeff(...)`. Under identity
  (`acmod == output`) this sets `coeff[0..4] = level` and returns **0**
  (`downmix.c:157-169`). Replacement: exactly that, inline.
- `parse.c:848` — `nfchans_tbl[output & A52_CHANNEL_MASK] < nfchans` is
  **always false** under identity, so `i` stays 0 and the `i < nfchans`
  branch is **always taken**. The entire second branch (`:883-903`) is dead
  code, as is the `a52_upmix` call at `:856` and the `a52_downmix` call at
  `:881` (neither switch has an identity case, so both are already no-ops).
- Consequently `chanbias` is always 0 → `bias = state->bias` for every
  channel, and `coeff[i]` is `state->dynrng` for every channel.
- `state->downmixed` (and its `a52_init` initialiser at `parse.c:83`)
  becomes vestigial once the upmix/downmix calls go — prune the field
  deliberately. **This is the ONLY field safe to prune on sight**; see the
  bitstream trap below for the ones that are not.

**All five points above were verified by the gate against source AND by
building the reduction and re-decoding the corpus: plane hashes came back
bit-identical, including for a synthesised `dsurmod=2` (A52_DOLBY) variant.**
The reduction is sound. What follows is where it goes wrong in the hands of
an implementer.

#### THE BITSTREAM TRAP — the one that silently destroys audio

`state->clev` / `state->slev` are assigned from `bitstream_get(state, 2)`
(`parse.c:161,164`), and `dsurmod` is read inside a condition at `:157`.
**Those reads consume bitstream bits. The fields are vestigial; the READS
ARE NOT.** Deleting them desynchronises the bitstream and every subsequent
frame is garbage. The gate demonstrated both failures:

- delete `:160-164` → stereo still bit-identical, `surround51_*` fails
  `AIUS_MALFORMED`;
- delete `:157-158` → `stereo_tones` fails `AIUS_MALFORMED`.

**And `-Werror` actively pushes the implementer into it**: once the fields
are pruned, the function-local `static sample_t clev[4]/slev[4]`
(`parse.c:145-146`) become unused and the build FAILS with
`-Werror=unused-variable`. The obvious way to clear that error takes the
reads with it. So, verbatim: **delete the tables and the assignments, keep
the reads** —

```c
if ((acmod & 1) && (acmod != 1))
    bitstream_get (state, 2);   /* cmixlev: consumed, value unused */
if (acmod & 4)
    bitstream_get (state, 2);   /* surmixlev: consumed, value unused */
```

and leave `if ((acmod == 2) && (bitstream_get (state, 2) == 2)) acmod =
A52_DOLBY;` intact. Other value-unused reads that must never be pruned for
the same reason: `parse.c:155` (acmod skip), `:186-208` (bsi), `:195`,
`:694` (gainrng), `:766-769` (skipl).

#### Three corrections to the first draft of this survey

1. **`mm_accel.h` is NOT unreferenced.** `imdct.c:41` includes it and
   `MM_ACCEL_DJBFFT` is used at `imdct.c:421`. Deleting it breaks the build.
   F1 must also delete `imdct.c:41` and the `#ifdef LIBA52_DJBFFT` blocks
   (`:31-33, :420-426`), or keep the header. Only `attributes.h` is
   genuinely unreferenced.
2. **Grant `*flags & A52_CHANNEL_MASK`, NOT `state->acmod`.**
   `state->acmod` is the RAW acmod (`parse.c:152`); the local `acmod`
   becomes `A52_DOLBY` at `:158` while `state->acmod` stays 2. Granting
   `state->acmod` yields `A52_STEREO` where upstream yields `A52_DOLBY`,
   contradicting `aius_a52.h:40-41`'s stated agreement between syncinfo
   flags and `a52_frame`'s out-flags. Audio is unaffected, no test reads the
   out-flags, and no corpus vector sets `dsurmod=2` — **so this would ship
   silently and F1's own oracle could not convict it.** Real Dolby Surround
   stereo from a TV hits it.
3. **Add a fail-closed guard.** After the reduction, a caller requesting
   anything but the stream's own coding silently gets wrong audio instead of
   `a52_downmix_init`'s `return 1`. One branch per frame turns the fork's
   premise into an enforced invariant:
   `if ((*flags & A52_CHANNEL_MASK) != acmod) return 1;` (post-Dolby local
   `acmod`).

**A fifth identity-dependent site the survey missed:** `parse.c:840-844`
(the skip-LFE-coefficients branch) is dead only because the adapter always
grants `A52_LFE` when the stream has it. It is not a `downmix.c` call site,
so it STAYS — flagged so nobody prunes it as "dead under identity" and
desyncs an LFE stream on some future non-identity request.

#### Steps, each separately committed

**Step F0 — pin a byte-level oracle, BEFORE anything moves. DONE
2026-07-29.** The pre-F0 suite could not prove what F1 claims:
`test_a52.c` asserted only a max per-sample LSB diff and an SNR printed to
0.1 dB, so a few LSB of drift in non-peak samples moved neither figure. F0
adds a per-vector FNV-1a checksum over all decoded planes, printed and
asserted in its own commit against the UNMODIFIED tree.

**The construction must be pinned with the values, or the baselines are
unreproducible and the oracle degenerates into a tautology** (gate B: a
rerun under a different basis produced four different digests, and on first
mismatch an implementer simply re-baselines). Fully determined:

- 64-bit FNV-1a, basis `0xcbf29ce484222325`, prime `0x100000001b3`;
- one hash per vector, starting at the basis and accumulating in the fully
  nested order access unit in stream order, then channel, then sample;
- each sample taken as `(uint32_t)block.planes[c][i]` and fed as **4
  little-endian bytes**.
- the corpus walker asserts that the final declared frame ends exactly at
  the file size, so a trailing partial AU or the frame cap cannot silently
  shorten the hashed input.

| Vector | FNV-1a over all planes |
|---|---|
| `stereo_tones` | `7d7d27ae1ae01351` |
| `stereo_impulse` | `e5ace9204e8da8bb` |
| `surround51_impulse` | `27ca745607a8b834` |
| `surround51_ident` | `d549a217bb66b3c6` |

F1 must then reproduce all four exactly, with `a52-fuzz` green.

**The probe already exists** — `firmware/tools/a52_probes/hash_a52.c`, kept
from the gate that measured these baselines, alongside `tabcheck.c` (F2's
table-rewrite proof) and the reproduction recipes for the two synthetic
inputs the gate used: a `dsurmod = 2` corpus variant (byte 6 of every frame
`0x43` → `0x53`, which is the only way to convict the A52_DOLBY flags
defect, since no committed vector sets it) and an E-AC-3 IEC stream. F0 is
therefore turning an existing, measured probe into an asserted suite check,
not writing one.

**Step F1 — move and prune, NO decoded-sample change. DONE 2026-07-29.**
`vendor/a52-core/` → `lib/aius_a52/a52/`; delete `downmix.c` and
`attributes.h` (NOT
`mm_accel.h` — see correction 1); apply the identity-only reduction with
the bitstream trap, the `*flags` grant and the fail-closed guard above;
`NOTICE.md` becomes the divergence log. **F0 must have landed first** — F1's
byte-identity proof is the four checksums, so without them it has no oracle.
Two core-level semantic assertions complete F1's proof because decoded
samples cannot convict flag negotiation:

- derive a synthetic `dsurmod=2` stereo AU from the recorded gate recipe,
  call `a52_frame` directly, and assert its returned coding is
  `A52_DOLBY`, matching `a52_syncinfo`;
- request a non-identity coding directly from `a52_frame` and assert the
  reduced core rejects it, pinning the fail-closed guard. This assertion
  joins F1 rather than F0 because the unmodified core legitimately supports
  downmix requests.

**Path references F1 updated:**
`scripts/test.sh:125,135,142`; `lib/aius_a52/include/aius_a52.h:16,34`;
`lib/aius_a52/include/config.h:1,4`; `lib/aius_a52/src/aius_a52.c:7`; this
plan's phase-2a Owned-paths block and its NOTICE.md relative link; and the
two live comparison paths in
`evidence/source-snapshots/A52DEC-0.8.0-UPSTREAM/README.md`, which the first
inventory missed. Nothing under `test/` or `tools/` referenced the vendor
path. The `registry` suite and the target build are unaffected (a52 is not
in `platformio.ini`, and `lib_ldf_mode = off` stops the LDF discovering the
new tree).

**Delivered proof:** the two direct-core flag assertions pass; all four F0
hashes remain exact; `a52-fuzz` passes 50,000 iterations. Removing the three
vestigial state fields lowers the current padded adapter's host arena
high-water from 90,464 B to 90,448 B.

**Step F2 — fix the idioms, drop the carve-outs. DONE 2026-07-29.** Remove
`-Wno-shift-negative-value`, `-fno-sanitize=shift`,
`-Wno-implicit-fallthrough`, `-Wno-unused-parameter` from `a52_build`, and
fix what they were hiding. **The gate compiled and ran with the carve-outs
off, so this is the measured inventory rather than an estimate** — do not
discover it mid-step:

*Compile time:* `tables.h:49,78,79,118-132,177-181` — **297**
`-Wshift-negative-value`, all from `(-n << 15)`, and all rewritable as
`n * 32768`, **verified value-identical across all 36 distinct constants
(0 bit-level differences)**. `parse.c:240,242,254,256,268,270` — exactly 6
`-Wimplicit-fallthrough`. `imdct.c:365` — the single `-Wunused-parameter`.

*Runtime UBSan, on the CLEAN corpus, 8 sites* — note three are outside
"the bitstream idioms" and would be missed by a narrow reading:
`bitstream.c:49:27` (`swab32`'s signed overflow — **not previously
mentioned**), `bitstream.c:66:36` and `:66:64` (the `shift exponent 32`
site), `bitstream.c:84:47` and `:90:19`, `bitstream.h:71:43`, and
**`parse.c:414:49` and `:521:44`** (`bitstream_get_2(...) << (16 - bapi)`
on a negative value — **not in `bitstream.c` at all**). All six
non-shift-exponent sites are wrap-preserving (eight reports, two of which
are the two operands of the single line `bitstream.c:66`): rewrite each as
an unsigned shift cast back to `int32_t`, which is value-identical and
unflagged. So
**byte-identity is achievable through F2** and the F1 checksums must still
hold.

**Correcting a premise about `current_word << 32`:** it is NOT a latent
x86-vs-ARM portability defect. `bits_left == 0` is reachable only
immediately after `a52_bitstream_set_ptr`, and that sole call site
(`bitstream.c:41`) **discards the return value**; everywhere else
`bits_left ∈ [8,32]`. x86's mod-32 garbage and an M7's zero are both thrown
away, today and after the fix. Fix it locally —
`result = state->bits_left ? (...) : 0;` in `_bh`/`_bh_2` only — and do NOT
"generalise" it by masking `result` to `num_bits`, which would break
`bitstream_get_2`'s deliberate sign extension.

Also delete the `config.h` `fprintf` hack by deleting the offending
`fprintf` in `imdct.c` — we own it now. **`config.h` itself survives**: its
`HAVE_MEMALIGN` / `LIBA52_DOUBLE` non-definitions are what route allocation
through the arena shim and keep `sample_t` as `float`. Update its header
comment in the same commit.

**Delivered proof:** the common `-Werror` build passes without the four
F2 carve-outs. The source-coupled `tables.h` probe compares all 36 distinct
quantizer constants with zero bit-level differences. With
`UBSAN_OPTIONS=halt_on_error=1`, the `a52` suite preserves the four exact
F0 hashes (`7d7d27ae1ae01351`, `e5ace9204e8da8bb`,
`27ca745607a8b834`, `d549a217bb66b3c6`) and `a52-fuzz` passes 50,000
iterations.

**Steps F3a / F3b / F3c — bounded reader, then pad removal, then CRC.**
Re-issued as three commits after the gate returned NO-GO on the single-step
version, for two independent reasons.

*Why it splits (gate finding 4):* phase 2a already established — and this
project already paid a gate round for — that the bound must be proven WHILE
the 32 KiB pad still exists, using the pad as an oracle. A single step that
adds the bound and removes the pad together cites evidence that is
unobtainable once the pad is gone.

- **F3a — the bound. DONE 2026-07-29.** End pointer, fail-closed,
  byte-derived, set before `a52_frame`, pad still in place and filled `0xFF`
  past `frame_length`. Proof is the non-zero suppressed-fill count phase 2a
  specifies, plus the F1 checksums.
- **F3b — remove the pad. DONE 2026-07-29.** Scratch falls to one frame.
  Suites unchanged.
- **F3c — CRC. DONE 2026-07-29.** Includes the mandatory fuzz repair and
  reach proof below.

**THE BLOCKER F3 originally shipped (gate finding 1): a CRC gate silently
destroys the fuzz suite.** `fuzz_a52.c` mutates real access units
(strategies 0, 2, 6) and depends on them still passing syncinfo to reach
`a52_frame`/`a52_block`. The gate simulated the exact PRNG stream over all
50,000 shipped iterations:

- reaching `a52_frame` today: **17,115 / 50,000**
- surviving a CRC gate: **6**

That is a **99.96% loss of coverage over the mantissa, exponent and
bit-allocation surface where BOTH phase-1 memory-safety defects lived** —
and the suite would still print `RESULT: passed`. It would also contradict
this plan's own standing conclusion that the fuzz suite belongs in the
pre-flash gate. **F3c must therefore restore the CRCs after corrupting**, so the suite tests
what it exists to test: a CRC-*valid* hostile stream, which is precisely the
shape of phase-1 defect 1 (reachable from a legal frame). Land the gate-F16
reach counter with it and **assert a floor**, or this class of silent
gutting recurs on the next guard tightening.

**"Recompute and patch back" has no working naive implementation — name the
mechanism or the fix reproduces the blocker** (gate A). CRC2 is an appended
CRC and recomputes trivially. **CRC1 does not: it sits at bytes 2–3, BEFORE
the region it protects, so the field must be SOLVED for, not computed.**
Measured on `surround51_ident` frame 0 with a bit flipped at offset 100:
storing `crc16(frame[4:fs58])` at bytes 2–3 and recomputing CRC2 yields a
whole-frame residue of `0x4b06` — rejected by the very gate F3c installs,
i.e. decoder reach collapses to ~0 again. Two acceptable mechanisms:

- **Solve the 16×16 GF(2) system** for the CRC1 field (equivalently
  ffmpeg's `mul_poly` pre-division), ~25 lines, and cache the map per frame
  length since it depends only on `fs58`. Verified to give
  `seg1 = seg2 = whole = 0` with the corrupted byte intact.
- **Cheap fallback:** corrupt only at or after `fs58` and recompute CRC2
  alone. Works naively, but **forfeits coverage of blocks 0–2 — where
  phase-1 defect 2 (first-block `EXP_REUSE`) lived** — so take it only as a
  stopgap, and never without the reach-floor assertion.

**The CRC formulation is verified**, so implement it as stated: CRC-16/ANSI
(poly `0x8005`, init 0, MSB-first, no reflection, no xorout) over
`buf[2 .. frame_size)` yields 0 on all eight committed frames, and each
segment (`[2, 5/8)` and `[5/8, end)`) is independently 0 — so a single pass
genuinely chains CRC1 and CRC2.

**F3a entry-point resolution:** at the F3a boundary,
`aius_a52_decode_access_unit` called the new `a52_bitstream_set_end` declared
in guarded `a52.h`; Step 3 now makes the same call in
`aius_a52_begin_access_unit`. It runs after `a52_syncinfo` validates the exact
payload length and 48 kHz rate and immediately before `a52_frame`.
`a52_bitstream_set_ptr` does not clear the end. F3c reuses this already
validated frame length for CRC. The byte-exact bound relies on a
**48 kHz-only
invariant** — `set_ptr` aligns down, and 48 kHz frame sizes are always a
multiple of 4 — so relaxing the rate guard later would make the bound off by
up to 3 bytes.

**F3c delivered proof:** `a52_frame` validates CRC1, CRC2, and their chained
zero-residue before header or bitstream parsing, after rejecting NULL,
reversed, <7 B, and >3,840 B spans using integer-address checks. The
independent test repair solves CRC1 with a cached 16×16 GF(2) inverse and
appends CRC2; every repaired frame asserts both segment residues and the whole
residue are zero. The 50,000-iteration stream repairs 17,118 mutations and
keeps 16,690 random feeds reaching the core against an asserted 16,000 floor.
Its new exact baseline is 5,616 OK / 60,184 rejected, 3,889,955 fills,
54,909 suppressions, and zero end mismatches. All four clean hashes remain
exact.

**Step F4 — REDUCES TO NOTHING; decision 2026-07-29: no dither change.** The
gate checked all three proposed 0.8.0 fixes and none is work:

- `bitstream_init` zeroing `current_word` — already covered by edit 2's
  `memset` at `a52_init`, and if 0.8.0's is per-frame it only rewrites a
  value proven discarded (see F2's `<< 32` correction).
- the `uint8_t do_bit_alloc = -1` underflow — `-1` to `uint8_t` is
  well-defined modular conversion to 255, which IS the intended
  all-bits-set, and every use is a mask test. Cosmetic; cannot move the
  differential.
- the per-frame `clev`/`slev` reset — **unreachable after F1**, which prunes
  those fields.

The `1027` typo fix is moot; `downmix.c` is gone. So F4 is not a step but a
single question: **do we take the dither change (0.707 → 0.75) and
re-baseline the differential and the F1 checksums?** It is cleanly separable
(one line in `dither_gen`). Default answer: **no** — it buys nothing and
costs the pinned baselines. Note also that the 0.8.0 endpoint is unreachable
from this network, so any of this would be applied from prose rather than a
diff.

#### What this does NOT solve

E-AC-3 remains out of reach, so defect 34's answer is unchanged. The arena
is unaffected — the fork lands at roughly the same ~27 KB, and the residency
arithmetic in the phase 2 contract stands.

## Phase 1 contract (2026-07-28) — vendor + wrapper + native suite

Host-only; no target build changes. Owned paths: `firmware/vendor/a52-core/`,
`firmware/lib/aius_a52/{include,src}/`, `firmware/test/a52/`,
`firmware/scripts/test.sh` (+ its README suites table), this plan.

### Vendor: liba52 from a52dec 0.7.4, verbatim

- **Source**: upstream a52dec 0.7.4 (liba52), the release most distros ship
  — **not the newest; 0.8.0 (2022) exists, see §Decoder alternatives**.
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
- **Phase-1 baseline, superseded by F3a: the decode buffer is a padded
  scratch, not the parser's AU** (gate F1, the load-bearing finding):
  liba52's bit reader was UNBOUNDED — no end
  pointer, no bits-remaining check — and worst-case syntax consumption on a
  hostile payload is ~25 KB against a 128-byte minimum frame. The wrapper
  copies each AU into a zero-filled scratch of
  `AIUS_A52_MAX_FRAME + AIUS_A52_PARSE_PAD` (3840 + 32 KiB; the pad bound
  and its derivation recorded in NOTICE.md), so an overread parses zeros
  inside owned memory and returns a bounded, typed result (possibly a
  legal-looking OK — zero exponents parse; the guarantee is bounded and
  typed, never OOB). The pad tail was re-zeroed on every decode so the
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
  poisoning contracts. At the phase-1 boundary liba52 did not CRC-check
  frames, so the fuzz posture was load-bearing, not ceremonial.
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
  two memory-safety defects in stock liba52 0.7.4 (unfixed in 0.8.0 and in
  Debian's patch series). Both required source edits — neither could be
  accommodated from outside. They originated in the phase-1 vendor tree and
  remain recorded in the active fork's divergence log
  ([`lib/aius_a52/a52/NOTICE.md`](../../firmware/lib/aius_a52/a52/NOTICE.md)):
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
arena growth (phase 4), DRC config flag, E-AC-3 (which DOES ride S/PDIF —
see §The format space and defect 34 — but needs a decoder liba52 cannot
provide, and is out of scope for every phase here).

## Phase 2 contract (2026-07-28) — decode off the ISR, and the footprint that makes it fit

Phase 2 closes defect 31 for BOTH codecs by moving bitstream decode out of
the audio ISR. Folded into it (operator direction 2026-07-28, after the
phase-1 arena measurement): the AC-3 footprint reduction, because the same
change delivers both — the reason AC-3 costs 90 KiB is that the wrapper
accumulates a whole access unit before returning, and that accumulation is
also the ISR burst defect 31 is about.

**Slice 2a is separable, host-only, and lands first.** 2b is the target-side
restructure.

**Owned paths** (2a): `firmware/lib/aius_a52/a52/{a52.h,parse.c,bitstream.c,
bitstream.h,a52_internal.h,NOTICE.md}`, `firmware/lib/aius_a52/{include,src}`,
`firmware/test/a52/{test_a52.c,fuzz_a52.c}`,
`firmware/tools/a52_probes/hash_a52.c`, `firmware/scripts/test.sh`, this plan.
Relative to ``.

### The residency question this settles (measured, not assumed)

Phase 1 measured AC-3 at 90,464 B and concluded two resident decoders do not
fit. That framing was wrong: **almost none of it is liba52's.**

| Item | Bytes | Owner |
|---|---|---|
| Parse scratch (`MAX_FRAME + 32 KiB pad`) | 36,608 | phase-1 adapter; F3b has now removed the proof pad |
| Output planes (6 × 1536 × 4) | 36,864 | adapter, only because it accumulates a whole AU |
| liba52 sample buffer (256 × 12 × 4) | 12,288 | liba52 |
| State + handle + alignment | ~4,704 | liba52 + adapter |

At the current F3b boundary the parse-scratch line is 3,840 B; the output-plane
line remains until Step 3.

The DTS adapter allocates NO planes — `block->planes` points straight into
dcadec's own `output_samples` (`aius_dts.c:259`), which is why it is cheap.
Both adapter-side items above are removable, and removing them is what 2a
is. Resulting arithmetic:

- planes 36,864 → **6,144** (emit per 256-frame block, saving 30,720)
- scratch 36,608 → **3,840** (bound the reader, saving 32,768)
- AC-3 resident ≈ **26,976 B**
- dcadec 62,560 + DSP staging 32,768 + 26,976 = 122,304, **plus the
  `AiusPipeline` instance itself at ~5,200 B** (`aius_pipeline.c:586` —
  `clsbuf` 1,024 + `pcm_l`/`pcm_r` 4,096 + fields), which the first draft
  omitted and which is ~60% of the apparent slack (phase-3 gate F12):
  **≈ 127,504 vs the existing 131,072-byte arena — both decoders resident,
  no growth, no swapping, ~3,568 B spare.**

**That spare is now ~2.7%, not 6.7%** — under 4 KB — and **it may be the
only margin there is**, because the OCRAM fallback is NOT established (gate
F13). At this margin the on-target re-measurement in phase 4 is not a
formality: the ~27 KB figure is host LP64-derived while dcadec's 62,560 is
an on-target ILP32 measurement, and a few hundred bytes of struct-layout
drift is now material. Phase 4 must be prepared to trim (the parse scratch
is the obvious candidate) rather than assume. The arena is a plain static
array in DTCM today (`optical_main.cpp:1048`), so growing it in place was
never available: the Phase-3 target build leaves 37,856 B in DTCM's
free-for-locals zone and that
region is also the stack. But relocating it to OCRAM is not simply a matter
of free bytes: the 32,768 B DSP staging carve was already VETOED from OCRAM
by the defect-26 link-time heap floor with only 13,312 B of slack
(`optical_main.cpp:1305-1311`), and moving the whole 131,072 B arena is 4×
what was rejected. The gate is the boot-heap floor, not free space. **Phase
4 must corroborate the OCRAM route against that floor before relying on it,
or treat 127,504/131,072 as the budget.**

**Dynamic decoder loading is REJECTED, with the reasoning recorded so it is
not re-opened.** Dynamic code loading is not supported and would not solve the
data-residency constraint; decoder text currently fits six ITCM blocks while
flash is 95% free. Dynamic instantiation (create/destroy at class change)
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

**Step 1 — fork edit 4: bound liba52's bit reader (pad still in place).
DONE 2026-07-29.**
`bitstream_fill_current` (`bitstream.c:44-50`) is the SOLE dereference of
`buffer_start`, `a52_bitstream_get_bh`/`_bh_2` each call it exactly once, the
inline fast paths touch only `current_word`, there are no byte-wise reads,
and max `num_bits` is 24 so one fill always suffices. One choke point, so an
end pointer plus zero-fill is sufficient — provided all five of these hold,
each of which the gate found the first draft did not state:

- **(a) At the Step-1 boundary the setter ran in
  `aius_a52_decode_access_unit`, BEFORE `a52_frame`, and
  `a52_bitstream_set_ptr` must NOT clear the end.**
  `a52_frame` calls `set_ptr` itself (`parse.c:154`), so a naive "reset to
  unbounded there" drops the bound for the whole frame. Step 3 moves the
  same call into its new `begin_access_unit` entry point.
- **(b) A BYTE end pointer (`scratch + frame_length`), never a word count.**
  `set_ptr` aligns *down*, so any word-derived bound is off by one word.
- **(c) FAIL-CLOSED.** With edit 2's `memset` an unset end is NULL and
  `if (buffer_start >= buffer_end)` correctly yields zeros. The form
  `if (buffer_end && ...)` fails OPEN and is forbidden.
- **(d) The field lives in `a52_state_s` (`a52_internal.h`).** Safe: `a52.h`
  keeps the type opaque, the adapter and both suites include only `a52.h`,
  and all four fork TUs compile in one invocation.
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

**Delivered proof:** fail-fast `a52` preserves all four F1 hashes and the
direct seven-byte unarmed case suppresses its only attempted fill under ASan.
The 50,000-iteration fuzz run preserves its exact 5,617 OK / 60,183 rejected
outcome while observing 3,889,968 fills, 54,922 non-zero suppressions, and
zero frame-end mismatches. The retained `0xFF` pad is therefore an exercised
oracle, not the containment mechanism.

**Step 2 — remove the pad. DONE 2026-07-29.** Scratch is one frame
(3,840 B).
Same suites, unchanged, must stay green: with the pad gone the bound is now
load-bearing, so this commit is the real test of edit 3. Step 2 also retires
the prose that describes the pad as the safety mechanism, all of it inside
owned paths and all of it now wrong: `NOTICE.md`'s external-accommodations
section, and `aius_a52.h`'s SAFETY SHAPE paragraph plus
`AIUS_A52_PARSE_PAD` (gate R10).

**Delivered proof:** with the allocation reduced from 36,608 B to 3,840 B,
fail-fast `a52` preserves all four exact hashes and `a52-fuzz` preserves the
exact 5,617/60,183 outcome, 3,889,968 fills, 54,922 suppressions, and zero
end mismatches under ASan/UBSan. Arena high-water falls from 90,448 B to
57,680 B in the probe build. The reproducible production configuration
(`AIUS_A52_BOUND_PROBE=0`) passes the same suite at 57,664 B.

**Step 3 — iterator API + suite rewrite. DONE 2026-07-29.** Replace
whole-AU decode with
`aius_a52_begin_access_unit()` (validates, runs `a52_frame`, arms the
iterator) + `aius_a52_decode_next_block()` (decodes ONE 256-frame block into
reusable planes). The caller decides when each block is decoded — the
slicing 2b needs, taking worst-case work per service slice from a whole AU
to a sixth of one. Retain `AIUS_A52_FRAME_SAMPLES == 1536` as the access-unit
total, add `AIUS_A52_BLOCK_SAMPLES == 256` and
`AIUS_A52_BLOCKS_PER_ACCESS_UNIT == 6`, and allocate adapter planes by the
block constant.

**The invariant this trades away, stated because it is not free (gate F1).**
Today a malformed AU emits NOTHING: `a52_block` returns 1 from eight sites
reachable from a corrupt bitstream (`parse.c:584,592,664,680,693,701,733,
737`), the adapter aborts the whole AU, and `fuzz_a52.c` asserts that as a
property. Under the iterator, blocks 0..k-1 are already in the caller's
hands when block k fails. **Policy: `decode_next_block` returns the typed
error and the iterator disarms.** The fuzz gets a matching property (see
below).

**What the caller does with the blocks already delivered — REVISED
2026-07-28 after the phase-3 gate.** An earlier draft required them to be
discarded and named `aius_spdif_tx24_stage`/`_commit` as the hold-back. That
was wrong on the mechanism: the pipeline delivers through
`AiusPcmSink.consume`, not the ring; the ring holds POST-DSP stereo frames,
not decoder planes; and `enqueue` commits per block
(`optical_main.cpp:1205`), so blocks 0..k-1 are published to the transmit
ISR before block k can fail. A hold-back would mean restructuring `enqueue`
for a whole-AU deferred commit.

**It is not worth it, and the discard was the wrong instinct.** Blocks
0..k-1 were validly decoded; a later block failing does not retroactively
corrupt them. So the policy is: **emit each block as it decodes, and a
mid-AU error simply TRUNCATES the AU** — at most 32 ms of audio is cut
short (plus, on the sink-refusal path, the unconsumed words of that push,
bounded at 256 on target but enough to swallow the next burst's preamble —
gate R3), nothing garbage or stale is emitted, and no accumulator, hold-back
or ring restructure is needed anywhere. This also keeps the standing operator
principle intact ("burst noise is more acceptable than extra latency"): a
deferred commit would add up to a whole AU of latency to buy atomicity
nobody asked for. The invariant that actually matters — *a non-OK status
never emits fresh PCM for the block it failed on* — is preserved exactly.

**A sink refusal truncates the same way but is NOT an error** (gate N1). A
full transmit ring is an ordinary condition, and AC-3 meets it more often
than DTS: six `enqueue` calls per AU occupy 1,536 of the ring's 2,048
frames, so AC-3 has 512 frames of tolerance where DTS had 1,536. The caller
must therefore call `aius_a52_abandon` before returning, or the iterator
stays armed into the next burst.

**The fuzz property that replaces "a malformed AU emits nothing"** (gate
N3): on a mid-AU error, **exactly k blocks were emitted, the iterator is
disarmed, and no 7th call succeeds.** That is the concrete suite change
emit-as-decoded forces, and it must be written or the old property is simply
deleted with nothing in its place. **It is not sufficient on its own** — pair
it with the fresh-decoder equivalence assertion in the iterator rules below,
which is what distinguishes a real overlap reset from a flag flip.

**Iterator state machine, specified (gate F8):**

- `decode_next_block` without a live `begin` → `AIUS_INVALID_ARG`.
- A 7th call errors and never runs a 7th `a52_block`.
- A mid-AU `a52_block` error invokes the shared discontinuity reset and then
  disarms the iterator.
- **`aius_a52_abandon(dec)` is part of the API** (added after the phase-3
  gate, N1). Implicit abandon-on-`begin` is not sufficient: a caller that
  stops mid-AU for a reason of its own — above all a FULL TRANSMIT RING,
  which is an ordinary condition, not an error — must be able to stop
  cleanly. Without it the iterator stays armed and the next
  `begin_access_unit` must preempt it explicitly before overwriting scratch.
- **Every incomplete-AU exit uses one real core reset; disarming a flag is
  not enough** (gate R1 plus Step-3 prebuild finding 1). Add
  `a52_reset_discontinuity()` to the fork's guarded public `a52.h`, implemented
  in `parse.c`. It zeroes the 1,536-float IMDCT delay at
  `state->samples[1536..3071]`, resets `lfsr_state` to its fresh value 1, and
  fail-closes the reader (`buffer_start`, `buffer_end`, `bits_left`, and
  `current_word`; the probe's expected end is cleared while cumulative
  counters remain). This is invoked by explicit `aius_a52_abandon`, by a
  live `begin_access_unit` before scratch overwrite, and by an `a52_block`
  failure before disarming. A deeper `a52_frame` failure invokes it too.
  `aius_a52_abandon(NULL)` returns `AIUS_INVALID_ARG`; abandon on a valid idle
  decoder is idempotent `AIUS_OK` and deliberately does not reset continuity
  after a normally completed AU.
- **The assertion that actually proves it**: after `abandon` + `begin` on a
  fresh AU, block 0 is **bit-identical to a fresh decoder's block 0 on the
  same AU**. A block-count-and-disarm check cannot distinguish a real reset
  from a flag flip; this can. Resetting only the overlap is insufficient
  because conformant dither advances `lfsr_state`. (Gate-verified against
  the fork source: all
  six IMDCT delay slots resolve to `state->samples[1536..3071]` with or
  without LFE, so 1,536 floats is exactly the right span — no over- or
  under-write.) The test sequence is fixed to avoid a fresh-state
  false-green: begin `surround51_ident` AU0 and successfully decode its first
  dither-exercising block, abandon while five blocks remain, then begin
  `surround51_ident` AU0 again and capture block 0. A newly created decoder
  begins the same AU and captures block 0; shape and every sample must be
  bit-identical. That corpus path already records the strongest dither
  contribution and exercises all six IMDCT delay slots, so this convicts both
  a flag-only reset and an overlap-only reset that leaves `lfsr_state`
  advanced.
- **What `abandon` deliberately does NOT reset**: `state->fbw_expbap` /
  `lfe_expbap` still hold the truncated AU's exponents rather than a fresh
  decoder's zeros. Harmless as specified — conformant AC-3 codes new
  exponents in block 0, so the assertion passes and real audio is
  unaffected, and edit 2's init zeroing bounds the malformed case. Recorded
  so that widening `abandon`'s contract, or running the assertion over a
  fuzz-corrupted AU, does not trip on it unexpectedly.
- `block->planes` are valid only until the next `decode_next_block`,
  `begin_access_unit`, or `destroy`; the caller must not hold them across a
  call.
- After the basic `dec` / `data` / non-zero-size guards,
  `begin_access_unit` on a live iterator invokes the discontinuity reset
  before any scratch copy, then validates and re-arms. Any later
  length/sync/rate/mode/CRC/frame failure leaves it disarmed. Preemption is
  intentionally lossy, but cannot carry stale overlap or dither state into
  the replacement AU.
- **Scratch lifetime (gate F7): the next AU must NOT be copied into scratch
  until block 6 of the current AU completes**, because all six `a52_block`
  calls read from it through the bitstream state. Easy to violate in 2b's
  ring-fed service loop, and it produces plausible garbage audio rather than
  a crash. `begin_access_unit`'s reset-before-copy preemption is what makes
  the violation impossible to express accidentally.
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
- Preserve the FNV order exactly as AU → channel → all 1,536 samples. Hashing
  each block immediately would silently change it to AU → block → channel →
  256 and invalidate every clean digest despite identical PCM. The retained
  `firmware/tools/a52_probes/hash_a52.c` consumer is ported with the same
  six-block accumulation and channel-major hash order.
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

The A52 build also adds `-fno-sanitize-recover=undefined`; the suites already
enable UBSan, but without non-recovering mode the claimed fail-fast proof is
not reproducible (Step-3 prebuild finding 5).

**The differential must remain byte-identical** (1906 LSB / 69.3 dB) at
every step, and this is provable rather than hopeful: decode order and count
are unchanged so `dither_gen`'s LFSR advances identically, the quantize is
per-sample, and later edits retain the synthesized zeros F3a's bounded reader
already returns for a past-end read.

**Step 3 delivered proof:** `begin` plus six `next` calls preserve all four
exact FNV hashes and the 1906 LSB / 69.3 dB differential; every seventh call
returns `AIUS_INVALID_ARG` without changing its poisoned block. A
`surround51_ident` block primes all six delay slots and dither state; explicit
abandon and live-begin preemption then reproduce a fresh decoder's block 0
bit-for-bit. Arena high-water is asserted below 28,000 B and measures 26,992 B
with the bound probe, 26,976 B without it. The full iterator fuzz preserves
5,616 OK / 60,184 rejected, 3,889,955 fills, 54,909 suppressions, zero end
mismatches, 16,690/50,000 core reach, and 17,118 repaired mutations while
bounding each complete AU as one termination unit. It also asserts the exact
11,074 core block rejections, each with its emitted-block count and disarmed
7th-call proof. The retained hash probe compiles against the iterator and
reproduces the same four digests.

### Slice 2b — decode off the audio ISR (target)

**Owned paths:** `firmware/lib/{aius_audio,aius_audio_ingress,
aius_audio_service,aius_pipeline,aius_boot,aius_runtime,aius_replay,
aius_recorder,aius_dsp}/`,
`firmware/src/{aius_runtime_teensy.h,aius_runtime_teensy.cpp,optical_main.cpp}`,
`firmware/test/{audio-ingress,audio-service,pipeline,runtime,runtime-target,
replay,tx-gate,recorder,measure,optical-cutover,memory,itcm-placement}/`,
`firmware/scripts/test.sh`, `firmware/README.md`, `firmware/platformio.ini`,
`docs/plans/{aius-ac3-support,aius-network-hardening,aius-open-work-register}.md`.
Relative to ``.

#### The two contexts and the one handoff

The audio software ISR remains the sole ingress producer. The runtime AUDIO
checkpoint is the sole ingress consumer, pipeline owner, decoder caller, DSP
producer and transmit-ring producer. The transmit DMA ISR remains the sole
transmit-ring consumer. Decode never runs from `AiusAudioProcessor::update()`.

The handoff is a portable `AiusAudioIngress` SPSC with caller-provided word
storage. The target places exactly **8,192 `uint16_t` words (16 KiB) in
`DMAMEM`/OCRAM**; its cursors and counters are ordinary 32-bit state. Capacity
is a power of two and exactly 32 engine chunks:

- `AIUS_AUDIO_INGRESS_CHUNK_WORDS = 256` (128 stereo frames);
- `AIUS_AUDIO_INGRESS_CAPACITY_WORDS = 8192`;
- producer-owned free-running `written`, discontinuity generation, drop
  counters and high-water;
- consumer-owned free-running `consumed`, observed generation and flush
  counters.

Aligned 32-bit cursor loads/stores are single-copy atomic on Cortex-M7. As with
the transmit ring, staged words become visible only when the producer publishes
`written`. A reservation always covers one complete, contiguous 256-word slot
(the chunk divides the ring), and commit is all-or-none. The ISR performs at
most one reservation and one 256-word fill per callback:

- live optical interleaves the 128 left/right samples directly into the
  reserved slot;
- replay keeps `aius_replay_service_pop_words` in the ISR and pops exactly 256
  words directly into that slot;
- if no slot is free, live drops the whole callback and replay advances the
  same 256-word timeline through a new bounded discard API;
- a short replay pop, a missing live channel, or a full ring commits no partial
  chunk and records one complete gap.

The ISR does no parser reset, classification, decoder call, sink call, DSP,
meter, recorder, measurement, transmit-ring write, wall-clock read, or loop.
`optical-cutover` asserts those absences structurally.

At 48 kHz the producer rate is 96,000 words/s. The 8,192-word ring therefore
holds 85.33 ms and its 32 chunks make wrap/full arithmetic exact. This is
elasticity, not permission to fall behind: the AUDIO checkpoint must sustain
more than 375 chunks/s.

The current linked image has only **131,424 B OCRAM free**, 352 B above the
linker's enforced 128 KiB network-heap floor, so the ingress ring cannot be a
net new OCRAM allocation. Slice 2b removes the recorder's existing 16,384-byte
sample staging from the OCRAM `.aius_recorder_staging` linker section and
creates **eight 512-frame slots (32 KiB)** in the recorder's PSRAM reservation
in the same commit:

- when PSRAM is present, the first aligned 32 KiB holds eight staging slots;
  the recorder ring receives the largest power-of-two byte capacity that fits
  after that carve (4 MiB on an 8 MiB board, 8 MiB on a 16 MiB board), satisfying
  `aius_recorder_init`'s power-of-two frame-mask invariant explicitly;
- when PSRAM is absent, the recorder stays absent and binds no staging, as
  today;
- both staging producer and drain now run serially in epoch context, so the old
  "ISR must not touch EXTMEM" reason no longer applies;
- both ends are CPU accesses through the same write-back cache and no DMA sees
  the staging, so no cache maintenance is introduced.

Eight slots are required because AUDIO may commit eight output steps after the
epoch's REPLAY drain has already run; four slots would deterministically gap
the fifth through eighth blocks. The recorder suite drives exactly eight
producer blocks before the next drain and requires zero staging gaps. The
smaller power-of-two recorder ring is a deliberate capacity trade: on the
common 8 MiB board it retains 4 MiB, about 10.9 s at the recorder's 384 KiB/s
stereo rate, rather than failing initialization with a non-power-of-two
remainder.

Adding the 16 KiB ingress and removing the old 16 KiB OCRAM recorder staging is
OCRAM net-zero even though the replacement PSRAM staging is 32 KiB. The
decoder/DSP arena is deliberately not used for the relocation: phase 3 needs
its remaining headroom for liba52, and an arena workaround would make Slice 2b
build while pre-breaking AC-3 residency.

#### Gaps and source changes

Ingress overflow drops the whole 256-word chunk. The producer increments a
monotonic discontinuity generation before accepting any later chunk and records
`dropped_chunks += 1`, `dropped_words += 256`, and the cause. The consumer reads
that generation before every drain and between original chunks. On a change it
discards every queued word, calls the one pipeline discontinuity helper, updates
its observed generation, and rechecks before parsing. Thus a parser can never
join words across a missing chunk.

Replay/optical selection remains owned by the replay manager. `update()` latches
the effective source used for each callback; an actual OPTICAL<->REPLAY change
records the same producer discontinuity before the first new-source chunk. The
epoch-owned carrier/replay reset request becomes a sequence rather than the
current set/clear boolean. AUDIO consumes it before any ring peek by briefly
masking the audio update, publishing `consumed = written`, and acknowledging the
sequence; it then unmasks and resets the pipeline in epoch context. A racing
first chunk may be deliberately included in that flush, but an old-source chunk
is never decoded after the reset. `source_flushes` and `source_flush_words`
make that disposition visible.

Replay refill, clock reconciliation, source state and underrun ownership do not
move. A successful replay callback still consumes exactly 256 words. A short
pop keeps the replay service's existing underrun transition, records an ingress
gap, and commits nothing. Ring-full replay discard keeps media time moving
instead of silently turning service lag into replay backpressure.

#### Deferred pipeline contract

The pipeline gains two deepened operations while the existing synchronous
`aius_pipeline_push` remains as a host/backward-compatibility driver:

1. `aius_pipeline_ingest(..., size_t *consumed)` classifies and de-frames only.
   It never calls a decoder or sink. It consumes until the offered words end or
   one work item becomes pending, and reports the exact prefix consumed.
2. `aius_pipeline_service_one(...)` performs at most one output work unit and
   reports idle, progressed, or transmit-blocked separately from its typed
   status.

The target offers at most the remainder of one original 256-word engine chunk
to each ingest call and never coalesces across chunk boundaries. A completed
burst may stop mid-chunk; the ingress cursor advances only by `consumed`, the
service resolves the pending item, and the suffix is re-presented. The only
extra call boundary is therefore immediately after a parser burst outcome,
which already zeroes DTS starvation. Sterile DTS input still reaches the
pipeline in exact 256-word segments, preserving the starvation oracle's
granularity. Starvation accumulation changes from offered words to exact
consumed words so partial progress cannot over-count.

Parser payload is not copied: when an IEC burst completes, ingestion stops and
the parser-owned `AiusBurst.payload` remains valid until service resolves it.
No later parser push may overwrite it. PCM likewise stops when its existing
512-frame planes become pending. This avoids a second 4 KiB compressed scratch
inside the 131,072-byte arena.

`service_one` owns these states:

- PCM pending: offer the existing 512-frame staging planes once;
- DTS AU pending: perform one whole `aius_dts_decode_access_unit`, retain its
  decoder-owned `AiusPcmBlock`, then offer at most 512 frames from it;
- decoded output pending: retry the same retained descriptor without another
  decoder call;
- phase 3 AC-3: `begin` plus its first `decode_next_block` is one work unit,
  each later 256-frame block is one unit, and a refused block stays retained
  without advancing the iterator. Exactly six committed units complete an AU.

dcadec cannot be interrupted inside `aius_dts_decode_access_unit`, so one DTS
decode remains one atomic operation. Its decoder-owned PCM **can** be exposed
without copying as consecutive sink views of at most
`AIUS_PIPELINE_PCM_BLOCK_FRAMES` (512) frames: advance each plane pointer by the
committed offset, retain the offset on `AIUS_NO_SPACE`, and do not ingest the
next AU until every view commits. Decode plus the first view is one service
step; each later view is one step. AC-3 is both relocated and decoded blockwise.
The asymmetry is atomic compressed decode versus blockwise decode, not an
oversized sink call.

Sink `AIUS_NO_SPACE` is backpressure, not an AU fault. The pending decoded block
and decoder storage remain untouched and the next epoch retries the same block
without re-decode. Any other sink failure records the status, abandons the
current AU, and invokes the shared discontinuity reset before later input.
Decode/parse rejection is non-fatal and advances the appropriate rejection
counter without emitting stale PCM.

`aius_pipeline_reset`, PCM->DTS retype, DTS starvation, source flush, ingress
gap and fatal service failure all use the same pending-work cleanup. Once phase
3 arms AC-3, that helper calls `aius_a52_abandon` before clearing parser,
retained-block and iterator state. No discontinuity can leave an iterator armed.

The synchronous compatibility driver repeatedly ingests, services pending work,
and resumes the exact suffix **only while service succeeds**. Its legacy
signature has no consumed result and owns no suffix buffer, so backpressure
cannot be resumable: if its sink returns `AIUS_NO_SPACE`, it abandons/reset the
pending item, drops the unconsumed suffix of that call exactly as the old
synchronous path did, and returns `AIUS_NO_SPACE`. A regression pins the loss
and clean-next-push behavior. The target never uses this lossy compatibility
path; only `ingest` plus the ingress cursor may retain a suffix. `finish` first
drains pending work or reports it, never overwrites a parser payload or PCM
block that service still owns.

#### Transmit backpressure and block side effects

`enqueue` moves to AUDIO checkpoint context. It validates the block and checks
`aius_spdif_tx24_free()` **before** applying staged configuration or touching
DSP state. The epoch is the transmit ring's only producer and the DMA ISR only
increases free space, so a successful preflight cannot become false before the
same producer commits; there is no second producer race.

On `AIUS_NO_SPACE`, none of these advance: active config/generation, DSP
biquad/limiter/dither state, input/output meter accumulators, measurement
capture, flight recorder, transmit `written`, or committed block count. On
success they run exactly once, in this order:

1. apply a staged config at the accepted block boundary;
2. DSP and meter accumulation;
3. stage and atomically commit the transmit block;
4. measurement and recorder taps over exactly the committed wire planes;
5. committed-block and cycle metrics.

The measurement and recorder APIs/comments are renamed from `*_isr_block` to
`*_producer_block`; their state machines retain one block producer and one
epoch controller, but both now run serially in runtime context. Recorder epoch
evaluation/drain remains at REPLAY before AUDIO, so a block produced this epoch
is observed by its controller on the next epoch. Its sample staging grows to
eight slots in the first 32 KiB of the existing PSRAM reservation and its
power-of-two recorder ring starts after that carve; PSRAM status/capacity
reports use the reduced ring capacity.
Meter publication keeps its single-writer snapshot protocol, with AUDIO as
writer. The transmit-ring SPSC documentation changes its producer from audio
ISR to AUDIO checkpoint.

Six AC-3 blocks can occupy 1,536 of the 2,048-frame transmit ring, leaving 512
frames (10.67 ms) of drain tolerance. Ring-full mid-AU is routine: it retains
the current 256-frame block and resumes when the DMA drain creates room. A DTS
decode that returns 1,536 frames occupies the same amount through three
back-to-back 512-frame commits, never one oversized sink call. Ring sizing
remains a function of service latency, jitter and this per-AU occupancy, not
merely average rate.

#### AUDIO checkpoint, quota and watchdog

Add `AIUS_BOOT_CHECKPOINT_AUDIO = 9` while preserving the existing REPLAY=7 and
SD=8 identifiers; move only EPOCH_COMPLETE to 10. Normal execution is explicitly
`0..7, AUDIO(9), SD(8), EPOCH_COMPLETE(10)`. The non-monotonic numeric trace is
deliberate: replay/source/gate state settles before audio, audio runs before the
potentially blocking SD primitive, and retained reports keep the established SD
identifier. Recovery-safe execution remains USB_RX, USB_TX, EPOCH_COMPLETE and
does not run audio.

`AiusRuntimeTeensyPlatformOps` gains one required `audio` callback. Its budget
row is `{AUDIO, 1 call, 0 transport RX bytes, 0 transport TX bytes, 8 ms}`.
Audio's own non-transport quotas live with the audio seam:

- `AIUS_AUDIO_INPUT_WORDS_PER_EPOCH = 4096` (16 original chunks);
- `AIUS_AUDIO_OUTPUT_STEPS_PER_EPOCH = 8`;
- `AIUS_AUDIO_SERVICE_BUDGET_US = 8000`.

The pre-change ~115 epochs/s is **not** used as a throughput proof because AUDIO
adds work to the epoch that schedules it. Slice 2b declares and instruments a
healthy post-change normal-epoch ceiling of **25,000 us**, hence a minimum
40 epochs/s. The existing timed non-AUDIO healthy allowances total 5.75 ms;
AUDIO's 8 ms leaves 11.25 ms inside that ceiling for untimed network/replay work
and the one bounded SD primitive. Every completed normal epoch is timed around
the real `aius_runtime_teensy_epoch`; `runtime_epoch_max_us` and
`runtime_epoch_budget_overruns` expose the bound, and the hardware gate fails on
any overrun during steady state.

At the derived 40 epochs/s floor, ingress capacity is 163,840 words/s, 70.7%
over the 96,000-word/s source, and eight output steps supply 320 blocks/s against
AC-3's 187.5. The controller may stop early on its 8 ms wall budget, so the AC-3
target gate must additionally prove at least six service steps fit in 8 ms and
that long-run committed output stays at 187.5 blocks/s without ingress growth;
the numeric maximum alone is not credited as throughput. `begin` plus block 0
counts as one AC-3 step, so an AU costs exactly six, not seven. The 85.33 ms ring
absorbs bounded jitter only; it is not the steady-rate argument.

The checkpoint alternates pending service and ingress until either quota is
spent. It stops immediately on transmit backpressure. Elapsed cycles/time is
checked between atomic work units; a whole DTS decode cannot be preempted, but
an over-budget unit prevents any second unit or further ingress that epoch. A
budget overrun is counted and the callback still returns success after putting
the pipeline in a recoverable state, so the runtime reaches its sole watchdog
feed. Only invalid construction returns false. A truly non-returning decoder
would still be caught by the 8 s watchdog; the retained 29,435-case DTS seam
hunt and per-call termination oracle are the evidence that relocation plus a
measured unit bound is the correct control.

This scheduling logic is not target-private. A portable `AiusAudioService` owns
the ingress consumer cursor, observed discontinuity/reset sequences, partial
chunk suffix, ingest/service alternation, word/step quotas, time checks and
diagnostics. It is constructed with the real `AiusAudioIngress`,
`AiusPipeline`, and a monotonic-microsecond callback; `platform_audio` is only a
thin call into it. The portable suite runs the real controller with a fake clock
and controlled pipeline/sink outcomes. Generation reset, partial consumption,
quota exhaustion, time exhaustion and transmit backpressure therefore exercise
the same control flow the target uses, not a runtime callback mock or a source
text assertion.

#### Observability and proof

Do not add another meaning to `g_overruns`. The legacy `overruns` status becomes
a compatibility projection of ingress chunks actually dropped. New committed
diagnostics report:

- ingress level/high-water, generation, dropped chunks/words by overflow,
  missing-live and short-replay cause, source flushes/words;
- checkpoint calls, input words consumed, service steps attempted/committed,
  service max cycles/us and budget overruns;
- transmit stalls, retained retries, output drops, AU abandons and last typed
  failure;
- attempted versus committed output blocks, while `blocks` remains committed
  blocks only.

The full AUDIO checkpoint cycle meter covers parser/decoder plus every sink-side
effect. Existing DSP-cycle fields continue to mean DSP/sink cost only; phase 3's
six-call fixed overhead is visible in both, rather than being mislabelled as
decode or hidden.

Ingress producer counters and high-water are cleared only through
`aius_audio_ingress_clear_producer_diag`, called by `/api/counters` while audio
updates are briefly masked. It resets cause counters and sets high-water to the
current level; it never changes cursors or generation, so `high_water >= level`
remains true. AUDIO-service counters are epoch-owned and clear directly after
the producer diagnostic clear. `g_blocks` is now epoch-owned; `g_overruns` is
read from the ingress drop projection. Status snapshots use the same brief mask
for producer-owned fields. The ingress suite interleaves publish, snapshot,
clear and a post-clear publish to prove no post-clear event is lost or charged
to the old interval.

Portable proofs:

- `audio-ingress`: power-of-two/wrap/full arithmetic, exact all-or-none
  256-word publish, SPSC cursor wrap, generation on every gap, source flush,
  high-water and counter ownership, concurrent-clear interleavings, no
  allocation;
- `audio-service`: the real portable controller's generation/reset consumption,
  partial ingress suffix, ingest/service alternation, 4,096-word and eight-step
  quotas, fake-clock 8 ms stop, transmit-backpressure stop/retry, exact
  attempted/committed counters and no allocation;
- `pipeline`: ingest invokes neither decoder nor sink; exact consumed prefixes
  and original-chunk segmentation; pending parser payload lifetime; one bounded
  DTS decode unit; a decoder-owned 1,536-frame DTS block exposed as three exact
  512-frame sink views; retained PCM and DTS output retry without re-decode or
  slice-offset advance;
  `AIUS_NO_SPACE`; reset/retype/starvation/squelch/carrier-discontinuity cleanup;
  attempted/committed/reject metrics; legacy-push backpressure drops its suffix
  then accepts a clean next push; no allocation. Phase 3 adds the exact six-step
  AC-3 resumability oracle against this same service API before target dispatch
  lands;
- `runtime` + `runtime-target`: exact `0..7,9,8,10` normal trace, recovery-safe
  exclusion, AUDIO budget units/callback validation, quota stop, over-budget
  success and one final watchdog feed;
- `replay`, `tx-gate`, `recorder`, `measure`: unchanged 256-word replay cadence
  and underrun transition; renamed producer semantics; gate/trigger ordering;
  eight consecutive 512-frame producer blocks before the next drain with zero
  staging gap;
- `optical-cutover`: `update()` contains ingress movement only and no pipeline,
  decoder, sink, reset or output side effects; AUDIO owns those calls;
- target: `pipeline`, `replay`, `tx-gate`, `recorder`, `measure`,
  `runtime`, `runtime-target`, `optical-cutover`, `memory`, `itcm-placement`,
  both `teensy41-optical` and `teensy41-optical-fault` PlatformIO images, plus
  stack/heap/arena and per-bank reports. The link/map proof must show the
  16,384-byte ingress in OCRAM, no recorder sample-staging symbol there,
  unchanged-or-better OCRAM free versus the pre-Slice-2b image, and an untouched
  decoder arena ceiling.

Buildable commit boundaries:

1. **2b1 portable defer/backpressure:** add the ingress SPSC, real portable
   AUDIO controller and deferred pipeline operations beside the retained
   compatibility driver; land native ingress/controller/pipeline/replay tests
   and registry/docs. Existing target calls still compile and behave
   synchronously. **Complete:** native `audio-ingress`, `audio-service`,
   `pipeline`, `replay`, `tx-gate`, `optical-app`, and `registry` proofs pass;
   `shellcheck` and the owned diff check are clean. Delivery review additionally
   pinned rejected-work accounting, producer source-change attribution,
   typed-suffix finalization, loop-free replay discard, the real eight-step
   stop/resume, narrow reset masking, and exact 1,536-frame-to-three-512-frame
   slicing.
2. **2b2 scheduler seam:** add AUDIO checkpoint/budget/callback and exact
   runtime tests; wire a temporary no-work target callback so both target images
   remain buildable. **Complete:** `boot`, `boot-target`, `fault-target`,
   `runtime`, `runtime-target`, `net`, `net-runner`, `recovery-runner`, and
   `build-metadata` pass; both
   `teensy41-optical` images build with exact normal/recovery traces, the
   COMPLETE=10 recovery-status domain, AUDIO=9 fault injection, and the
   temporary callback in place.
3. **2b3 cutover and evidence:** switch `update()` to ingress, implement AUDIO
   drain/service, move and rename block-writer effects, publish diagnostics,
   update structural/target proofs, flash, and run the hardware gate below.
   Remove the temporary callback in the same diff that makes it real.
   **Complete:** all focused native suites, both target images, memory/ITCM
   placement, registry and shell checks pass. On the tagged +438 ppm
   clock-isolation image, 1,857 seconds of live DTS remained green across 1,831
   status polls while cumulative status reached 184,448 committed blocks: AUDIO
   max 2.401 ms, normal epoch max 3.036 ms, ingress high-water 442 words with
   zero drops/resets, TX high-water 640 frames, zero
   stalls/retries/output drops/AU abandons, and unchanged boot/watchdog
   generation. Thirty-one measurement captures started;
   the state-machine evidence proves at least thirty completed, and recorder
   generation 52 retained 45,568 post-thaw frames with zero gaps. The corrected
   run-owned-ID harness then passed the complete aligned EOF path. The
   adversarial reviewer accepted the composite evidence and explicitly waived a
   redundant second soak.

The defect-31 hardware gate is **30 minutes** of continuous live DTS through the
CM6206 optical loop while recorder capture/drain and repeated HTTP status/capture
requests run. It must show no watchdog/boot-generation change, AUDIO max service
time at or below 8 ms, normal epoch max at or below 25 ms with zero epoch-budget
overruns, transmit high-water at or below capacity with every sampled and final
transmit level below capacity, and zero output drops or AU abandons. Recorder
ring position must advance, at least two measurement captures must complete,
and all gate assertions use interval deltas from a no-pending baseline rather
than clearing counters under live retained work.

The 2026-07-29 preflight reproduced the separate
[`aius-sample-rate-following.md`](aius-sample-rate-following.md) defect: the
CM6206's measured 48,021 Hz input gains about 42 IEC words/s against the fixed
48,000 Hz output, fills the 8,192-word ingress ring and causes one explained
overflow/reset about every three minutes after the first fill. A tagged,
uncommitted +438 ppm PLL4 trim then reduced that slope from +42.667 to +0.200
words/s over 185 seconds without a carrier event, reset or drop. The defect-31
gate runs on that instrumented image and requires zero ingress growth or drops;
the trim changes only the output clock ratio and leaves the ingress, scheduler,
decoder, DSP, recorder and HTTP paths under test unchanged. The tagged probe is
removed from the working tree and a post-commit production reflash/smoke remains
mandatory. This isolates the ISR-watchdog regression without closing or
excusing the separately recorded rate-following defect.
Operationally, "zero growth" means the least-squares slope across every
one-second ingress-level sample has magnitude at most 1 word/s, more than 40
times below the reproduced untrimmed slope.

The mandatory exact-production reflash then verified
`cb43fd560c7a+teensy41-optical`, boot generation 3 and zero watchdog resets.
After a diagnostic clear, the untrimmed image reproduced the same independent
clock-domain defect: the transmit ring reached its 2,048-frame capacity and
retained stalls/retries rose equally by 1,425. Ingress drops, service-generation
resets, output drops and AU abandons all remained zero. This is not accepted as
a rate-following pass, but it completes the production-image smoke and confirms
that Slice 2b's backpressure path retains rather than loses work. The
sample-rate-following plan remains authoritative for the clock fix.

Playback uses one raw IEC file pre-expanded beyond the gate duration, not a
real-time demux loop, and the harness owns both ALSA processes and lets the
complete source reach natural EOF. The real-time loop produced a
recorder-confirmed TX underrun 239.813 seconds after DTS start, within 230 ms of
the eighth 30.005-second file boundary, while carrier, class, ingress generation
and scheduler metrics stayed clean. Arbitrarily terminating playback mid-burst
is also not an acceptance transition: a 2026-07-29 preflight kept every measured
DTS poll clean, then its SIGTERM-cut burst produced one 120.438 ms AUDIO service,
14 ingress overflows / 3,584 dropped words and a recorder underrun. The same
2,813-burst source reaching natural EOF produced none of those changes across
202 samples and ten seconds after EOF. That abrupt-source-loss defect remains
separate from the steady-state defect-31 gate.

Recorder load is explicit: at least twice during the full gate, request a
freeze, wait for `FROZEN`, read and validate a 4 KiB generation-bound data chunk,
then thaw and require ring position to resume advancing. Natural trigger freezes
use the same drain/thaw path and remain counted.

At arbitrary steady-state snapshots, the interval deltas of
`transmit_stalls` and `retained_retries` may differ by at most one. After the
aligned source reaches natural EOF, this CM6206 keeps optical carrier present and
supplies zero PCM, so the transmit ring remains active rather than draining to
zero. Four consecutive post-EOF polls must instead show PCM class, no additional
DTS decode unit, transmit level below capacity and equal stall/retry deltas
across the uninterrupted gate generation. That proves retained DTS work drained
without pretending the carrier-valid PCM producer stopped. Only then may defect
31 be struck from
`aius-network-hardening.md` and the open-work register. If the board still dies,
the ISR-storm hypothesis is false and the next diagnosis starts at transmit
backpressure/producer timing, not parser termination.

### Out of scope for phase 2

Pipeline dispatch by Pc data type (phase 3), arena relocation (phase 4, only
if the ~3,568 B spare proves uncomfortable), DRC config flag.

## Phase 3 contract (2026-07-28) — dispatch, and the observability it forces

```yaml
phase: 3
title: AC-3 parser scope, deferred dispatch and live codec observability
unit: component-behind-interface
seams:
  - AiusBurst.data_type dispatch
  - aius_pipeline_get_codec_metrics
  - pipeline_diag codec and per-codec metrics
contract-artifact: spec-only
fan-out:
  - name: phase3-parser-corpus
    owned-paths:
      - firmware/lib/aius_iec61937/
      - firmware/tools/iec_corpus/
      - firmware/tools/ac3_vectors/README.md
      - firmware/test/iec61937/
  - name: phase3-pipeline
    owned-paths:
      - firmware/lib/aius_pipeline/
      - firmware/test/pipeline/
  - name: phase3-status-ui
    owned-paths:
      - firmware/src/optical_main.cpp
      - firmware/sd/www/status.js
      - firmware/sd/www/dsp-editor.js
      - firmware/sd/www/replays.html
      - firmware/sd/www/index.html
      - firmware/test/optical-app/
      - docs/plans/aius-open-work-register.md
  - name: phase3-build-registration
    owned-paths:
      - firmware/scripts/test.sh
      - firmware/platformio.ini
review-intensity: active
commit-checkpoint:
  green:
    - a52
    - a52-fuzz
    - iec61937
    - pipeline
    - replay
    - tx-gate
    - optical-app
    - registry
  buildable:
    - teensy41-optical
    - teensy41-optical-fault
  parent-commit: "feat(aius): dispatch AC-3 bitstreams"
```

**Coordination:** owned paths are disjoint. Build registration may be authored
in parallel from the locked source/API contract, but the parent alone runs the
combined native and target proofs after all other seams return. The parent owns
this plan, the integration commit and the target-compiler repair to
`firmware/lib/aius_a52/a52/imdct.c`; workers never mutate Git.

**Owned paths:** `firmware/lib/aius_iec61937/`, `firmware/lib/aius_pipeline/`,
`firmware/tools/{iec_corpus,ac3_vectors/README.md}`,
`firmware/test/{pipeline,iec61937,optical-app}/`,
`firmware/lib/aius_a52/a52/imdct.c` (target-compiler repair),
`firmware/tools/memory/mem_report.py` (shared decoder-arena report wording),
`firmware/src/optical_main.cpp` (status JSON only),
`firmware/sd/www/{status.js,dsp-editor.js,replays.html,index.html}`,
**`firmware/scripts/test.sh`**, **`firmware/platformio.ini`**, this plan.
The matching projection entry in `aius-open-work-register.md` is also owned so
the new status-schema debt is not left only in this phase narrative.
Relative to ``.

**The last two are not optional (gate F1) — without them phase 3 does not
build, host or target.** `aius_pipeline.c` gaining `aius_a52` calls breaks
three suite arms that link only dcadec + `aius_dts` (`pipeline`, and
`replay`/`tx-gate` which compile `aius_pipeline.h`), and
`teensy41-optical`'s `build_src_filter` has no a52 sources or include path,
so the target link breaks from phase 3's commit until phase 4 — violating
the phase-boundary rule that the affected artifact still builds. **Compile
the fork TUs to objects separately and link them**, rather than merging
a52's carve-outs into the shared arm: `-fno-sanitize=shift` folded into the
`pipeline` arm would silence UBSan's shift checks over `aius_pipeline.c` and
`aius_iec61937.c` as collateral. (**After fork step F2 the carve-outs are
gone entirely**, which removes this hazard at the source — sequence the
fork before phase 3 if convenient.) **That requirement is HOST-ONLY** (gate
N8): the target build carries neither `-Werror` nor a sanitizer, so
`platformio.ini` needs only the sources plus their include path and
`-Ilib/aius_a52/include` (the `config.h` seam) — no carve-out flags, and
none should be added env-wide.

### The parser rejects AC-3 today — that is the first change

`aius_iec61937.c:202` sets `in_scope = (data_type == 11 || 12 || 13)`, so an
AC-3 burst is framed and then counted in `rejects_unsupported`. Phase 3
widens that to include **type 1**. Two properties to preserve rather than
assume:

- **Pd is a BIT count for AC-3 as for DTS** (gate-verified by muxing a real
  stream: Pc `0x0001`, Pd `0x3800` = 14,336 bits = 1,792 B, matching
  `frmsizecod`), so `payload_bytes = (length_bits + 7) / 8` is already
  correct.
- **The payload cap holds with room**: `AIUS_IEC61937_MAX_PAYLOAD` is 4,096
  and AC-3's maximum at 48 kHz is 2,560 B (640 kbps × 32 ms). Note the
  asymmetry with `AIUS_A52_MAX_FRAME` = 3,840, which is the spec maximum
  across ALL sample rates; the adapter rejects non-48 kHz before decode, so
  2,560 is the operative bound and 3,840 is the scratch's safety size.
- **`in_scope` gates more than the return code (gate F4)**, and both effects
  must be stated rather than discovered: the `length_bits == 0` MALFORMED
  check at `aius_iec61937.c:204` is in-scope-only, so a zero-Pd AC-3 burst
  flips from UNSUPPORTED to MALFORMED; and the burst now tallies `bursts_ok`
  instead of `rejects_unsupported`. The second is the dangerous one — **an
  AC-3 stream the ADAPTER refuses (non-48 kHz, unmappable mode) would show
  `bursts_ok` climbing while the box sits silent, with no reject counter
  anywhere.** Phase 3 therefore adds a decode-reject tally to
  `pipeline_diag` beside the codec field below.

### Dispatch

The pipeline holds both decoders, created together at
`aius_pipeline_create_arena` (this is where phase 2a's residency arithmetic
lands: 62,560 + 32,768 + ~26,976 + the ~5,200 B pipeline instance ≈ 127,504
of 131,072). The deferred ingress burst completion routes on
`AiusBurst.data_type`:
11/12/13 to `aius_dts`, **1 to `aius_a52`**, everything else skipped exactly
as today. The class stays `AIUS_PIPELINE_DTS` — "non-linear bitstream" — with
no new classification state, because classification is preamble logic and
format-agnostic.

**AC-3 consumption follows phase 2a's iterator inside Slice 2b's deferred
state machine.** Completing a type-1 burst arms an AC-3 pending state.
`aius_a52_begin_access_unit` copies the parser payload into decoder-owned
scratch, so no second pipeline payload copy is required and ingestion remains
blocked while the AU is pending. The first service unit begins the AU and
decodes its first block; five later units call `decode_next_block`. Each
256-frame block is delivered directly through `AiusPcmSink.consume`: **six
sink calls per AU, no accumulator, no splitter and no ring restructure**.

`AIUS_NO_SPACE` is retained work, not an abandon: keep the same decoded block
descriptor and iterator position, then retry without re-decoding. The legacy
synchronous `aius_pipeline_push` compatibility wrapper may still abandon its
unaccepted suffix when it returns `AIUS_NO_SPACE`, using the existing reset
path. Every real discontinuity funnels through one pending-work cleanup that
calls `aius_a52_abandon`: explicit pipeline reset, source/generation gap,
reclassification/starvation at `begin_ingest_call`, and fatal service or sink
failure. A mid-AU decode error truncates only the remaining blocks and records
one decode rejection plus one AU abandonment.

### The false-green trap this creates — the finding that most needs recording

`test_pipeline.c:1039-1062` (**T6, `test_unsupported_never_starves`**) uses
**AC-3 bursts as its unsupported stimulus**, over a committed vector
`unsupported_ac3` whose payload is 48 bytes of SYNTHETIC data
(`gen_iec_corpus.c:301-307`), not a real AC-3 frame. Once type 1 is in
scope:

- the vector stops being "rejected by the parser as out of scope" and
  becomes "accepted, handed to liba52, rejected as malformed";
- **every assertion in T6 still passes** — no starve, class still DTS, zero
  blocks emitted — so the suite stays green while the test silently changes
  meaning and no longer covers framed-but-unsupported at all.

Phase 3 must therefore do BOTH, and neither substitutes for the other:

1. **Add a genuinely unsupported vector** to keep T6 honest — a well-formed
   burst of a type the box still refuses. **Type 21 (E-AC-3) is the right
   vector but for the OPPOSITE reason the first draft gave** (gate F7): its
   Pd is a BYTE count, not bits (measured Pc `0x0015`, Pd `0x0700` = 1,792
   B), so the manifest entry must not reuse the `len*8` convention; and it
   is emphatically NOT a type the box merely "stays dark for" — see the
   hazard below. It is constructible by `add_preamble` as-is and exercises
   the same finalize/starve path T6 covers.
2. **Add a real AC-3 burst vector that DECODES** — `gen_iec_corpus.c` already
   wraps real DTS access units from `dts_vectors`; it does the same for a
   real access unit from `tools/ac3_vectors`, and the new positive test
   asserts blocks emitted, 1536 frames per AU, and the `surround51_ident`
   channel assignment surviving the whole parser → dispatch → decoder path.
   Two mechanics the gate flagged: derive the AU length from
   `frmsizecod` (`buf[4]`) rather than scanning for the next 2-byte `0x0B77`
   sync, which is one regeneration away from a silent short AU; and note the
   `pipeline` suite's capture holds 64 blocks, so at 6 blocks/AU a positive
   test tops out at 10 AUs.

### HAZARD found by this gate: a real E-AC-3 link plays full-scale noise

Not a phase-3 deliverable, recorded here because the gate found it while
checking the type-21 vector and it is the same class of harm defect 33 was
built to prevent. **E-AC-3's burst period is 12,288 words, which exceeds
`AIUS_PIPELINE_RETYPE_MAX_SPACING_WORDS` (8,192).** A stream that enters
outside the 512-word classify window — roughly 96% of phases — therefore
types as PCM and **can never re-type**, because the re-type detector needs
two validated pairs within MAX spacing. It then clears the 8,192-word
confirmation squelch and plays E-AC-3 burst data as full-scale noise on
powered speakers, which is precisely the outcome the operator described as
"the worst thing" when specifying the squelch.

The box does not encounter E-AC-3 today only because nothing on this bench
sources it; a TV whose optical output is set to pass "Dolby Digital Plus"
would, and the transport carries it comfortably (see §The format space —
the "never rides S/PDIF" claim this document opened with was wrong).
**Recorded as defect 34 in
[`aius-network-hardening.md`](aius-network-hardening.md)**, with the
mechanism, the ~4% safe case, and two candidate fixes — raising the re-type
spacing bound to cover 12,288 (which lengthens every genuine PCM source's
confirmation mute, since `PCM_CONFIRM_WORDS` must stay >= MAX) or detecting
type 21 in the classify window and refusing to leave the dark state. **The
decision is deliberately NOT taken here** and must not be folded into
phase 3 silently.

### The UI is about to start lying, and the fix belongs here

`status.js:36-37` hard-codes `AUDIO_STATES {4: 'DTS'}` and `STREAM_FORMATS
{2: 'DTS (IEC 61937)'}` — codec-specific names over a codec-AGNOSTIC firmware
class. The moment AC-3 decodes, the box reports "DTS" while playing Dolby.

- **Firmware**: expose the data type of the last successfully decoded burst
  on `pipeline_diag` (the parser already carries it as
  `AiusBurst.data_type`), so the codec is a read of live state rather than a
  UI guess, plus the decode-reject tally from the `in_scope` note above.
  The state is exact: Pc 1 means AC-3, Pc 11/12/13 means DTS and 0 means
  unknown. A DTS decode sets it after its access unit decodes successfully;
  AC-3 sets it only after all six blocks of the first successful AU. Pipeline
  reset, source/generation gap, starvation and reclassification clear it to 0.
  A later malformed burst does not erase an earlier successful codec unless
  one of those stream discontinuities occurs.
- **`aius_pipeline_get_metrics` must be extended here, not in phase 4
  (gate F5)**: it is hardwired to `aius_dts_get_metrics(p->decoder, …)`
  (`aius_pipeline.c:513`) and `optical_main.cpp` publishes `decode_cycles` /
  `alloc_count` from it, so with AC-3 decoding the box would report stale
  DTS figures — and phase 4's "decode cycles per codec" would have no
  accessor to read. `lib/aius_pipeline/` is phase-3-owned and phase 4 does
  not own it, so the extension belongs in this phase. Add a separately
  addressable codec accessor that accepts Pc 1 or Pc 11/12/13 and returns the
  corresponding adapter's `AiusMetrics`; the existing accessor remains the
  compatibility projection for the last successfully decoded codec and returns
  zero operation fields plus pipeline `last_status` while the codec is unknown.
  `pipeline_diag` publishes `codec_pc`, `dts_decode_cycles`,
  `dts_alloc_count`, `ac3_decode_cycles`, `ac3_alloc_count` and
  `decode_rejects`. Its existing `decode_cycles` / `alloc_count` remain aliases
  for `codec_pc`, and `arena_high_water` is the maximum of the two adapter
  readings because it describes the shared arena. Thus no caller or operator
  must infer one codec's measurements from the other's last operation.
- **The old `g_overruns` decision is obsolete after Slice 2b.** The public
  `overruns` field now projects ingress `dropped_chunks`; transmit backpressure
  has distinct `transmit_stalls`, `retained_retries`, `output_drops` and
  `au_abandons` counters. AC-3 dispatch must preserve those meanings. A routine
  `AIUS_NO_SPACE` retry increments stalls/retries only and never ingress
  overruns, output drops or AU abandons.
- **UI**: labels become bitstream-generic with the codec named from that
  field — "bitstream (DTS)" / "bitstream (AC-3)", "unknown" until a burst
  has decoded. **Four files, not one** (gate F6): `status.js:36-37`,
  `dsp-editor.js:2141` (`CARRIER_ENCODINGS = {3:'PCM', 4:'DTS'}`),
  `replays.html:17`, `index.html:34`.
- Swagger/status schema debt grows by six fields: `codec_pc`, the four
  per-codec cycle/allocation fields and `decode_rejects`; record all six in the
  open-work register. Extend the `optical-app` structural suite to prove all
  four UI surfaces use the generic bitstream label, decorate it from the live
  codec, render unknown before a successful decode, and contain no stale
  DTS-only class label.

### Proof

The `pipeline` suite is the regression surface: T6 re-pointed and honest, the
new positive AC-3 path test, and the squelch/re-type constants re-asserted
against AC-3's **3,072-word burst period** (worst-phase re-type at 6,144
words, inside both `RETYPE_MAX_SPACING_WORDS` and `PCM_CONFIRM_WORDS` =
8,192). **That period must be built, not assumed (gate F9):**
`gen_iec_corpus.c:321-322` stuffs at most 8 words between bursts, so a
vector wrapping a 2,560 B AU repeats at ~1,284-word spacing and pins nothing
about 3,072 — each burst must be stuffed out to the real period.

The `iec61937` suite covers the widened `in_scope`, including that a type-21
burst is still refused. **It is a hard RED until phase 3 updates it
(gate F8):** `test/iec61937/test_iec61937.c:144` expects `AIUS_UNSUPPORTED`
for `unsupported_ac3`, and the same stale claim appears in
`tools/iec_corpus/vectors/manifest.txt:10`, `tools/iec_corpus/README.md:33`,
`gen_iec_corpus.c:307`, and `lib/aius_iec61937/include/aius_iec61937.h:33-36`.
All five move in the commit that widens the scope.

The AC-3 adapter suite continues to prove exactly six 256-frame blocks; update
`tools/ac3_vectors/README.md` in the same commit because its current
"three 512-frame blocks" statement is stale. The phase boundary runs `a52`,
`a52-fuzz`, `iec61937`, `pipeline`, `replay`, `tx-gate`, `optical-app` and
`registry`, then builds both Teensy optical images. All four liba52 fork
translation units (`parse.c`, `bit_allocate.c`, `bitstream.c`, `imdct.c`) remain
separately object-compiled from pipeline/parser code in native tests so no
sanitizer carve-out leaks onto those shared sources. Target memory and ITCM
placement must remain green; live AC-3 playback remains phase 4.

**Delivered 2026-07-29.** Pc 1 now de-frames and dispatches to the resident
AC-3 decoder while Pc 11/12/13 retain DTS and Pc 21 remains unsupported. The
real `surround51_ident` corpus proves two 2,560-byte AC-3 access units at the
exact 3,072-word IEC period, twelve 256-frame blocks, channel identity and zero
steady-state allocation. Deferred tests prove identical retained retries,
six-unit service, every abandon/reset path, a controlled mid-AU failure and the
last-successful codec/metrics lifecycle. Status and all four SD UI surfaces use
generic bitstream vocabulary with live DTS/AC-3/unknown decoration; all six
new schema fields are recorded as debt.

The first GCC 15 target build exposed legacy liba52 IMDCT pointer arithmetic
that formed pointers before the root tables and then indexed back into range.
`ifft_pass` now receives each real table base and uses equivalent forward
indices; all four decoder reference hashes remain byte-identical. `a52`,
`a52-fuzz`, `iec61937`, `pipeline`, `audio-service`, `replay`, `tx-gate`,
`optical-app` and `registry` pass, as do shell checks. Both
`teensy41-optical` and AUDIO-checkpoint-9 `teensy41-optical-fault` build with
target memory/placement gates green: FLASH 430,080 B, ITCM 190,460 B with
6,148 B padding, DTCM 289,824 B with 37,856 B for stack/locals, and OCRAM
392,832 B with 131,456 B free. The memory report now names the 131,072-byte
allocation as the shared codec-decoder arena rather than attributing it only
to DTS. Live AC-3 playback is deliberately still phase 4; this phase proves
the built dispatch path, not the hardware source.

## Phase 4 contract (2026-07-28) — target measurement and live validation

```yaml
phase: 4
title: Target cycle metrics, ITCM ceiling, replay source and live acceptance
unit: component-behind-interface
seams:
  - AiusCycleClock target timing
  - _itcm_block_count link ceiling
  - replay-sample transport validator
  - committed-wire measurement oracle
contract-artifact: spec-only
fan-out:
  - name: phase4-cycle-metrics
    owned-paths:
      - firmware/lib/aius_codec_api/include/aius_codec.h
      - firmware/lib/aius_dts/
      - firmware/lib/aius_a52/
      - firmware/lib/aius_pipeline/
      - firmware/test/dts/
      - firmware/test/a52/
      - firmware/test/pipeline/
      - firmware/test/itcm-placement/verify_target.py
      - firmware/src/optical_main.cpp
  - name: phase4-itcm-ceiling
    owned-paths:
      - firmware/link/aius_teensy41.ld
      - firmware/test/boot-target/test_ldscript_drift.py
  - name: phase4-replay-source
    owned-paths:
      - firmware/tools/replay_samples/generate.sh
      - firmware/tools/replay_samples/README.md
      - firmware/tools/replay_samples/validate.py
      - firmware/test/replay-samples/
      - firmware/scripts/test.sh
review-intensity: active
commit-checkpoint:
  green:
    - a52
    - a52-fuzz
    - dts
    - pipeline
    - iec61937
    - memory
    - itcm-placement
    - boot-target
    - replay-samples
    - registry
  buildable:
    - teensy41-optical
    - teensy41-optical-fault
  parent-commit: "feat(aius): validate AC-3 on target"
```

**Coordination:** the three implementation seams are disjoint. The parent owns
this plan, `firmware/README.md`, combined native/target proof, exact-image flash,
hardware evidence, delivery audits, commits and pushes. Workers never mutate
Git. The foreign untracked `firmware/tools/replay_samples/generated/` directory
is an output cache and is never staged.

**Owned paths:** the union of the fan-out paths above,
`firmware/README.md`, and this plan. `platformio.ini` moved to phase 3: the
target has already linked both decoders at the preceding buildable boundary.

**Build-order dependency satisfied by phase 3:** both target images now link
both decoders. Phase 4 starts from that buildable boundary and owns only the
runtime arena/cycle measurements, linker guard and live validation below.

### Placement — the decision, on a premise the gate had to correct

**An earlier draft claimed the cold-placement guard "already lists every
dcadec object" and that the five a52 objects just join it. Both halves were
false (gate F14).** `verify_target.py`'s `COLD_OBJECTS` is now nine
objects — `aius_boot`, `aius_http` ×2, `aius_net`, `aius_runtime`,
`aius_sd`, `aius_audio_service`, and two Teensy adapters — with **no dcadec object and no
`aius_dts`**, and `COLD_TREES` covers no vendor tree. The mechanism is
source annotation (`FLASHMEM`), and grepping the dcadec sources returns
zero: **dcadec's code is in ITCM today.** Adding a52 objects to that list
would fail immediately because they emit `.text`; the fork is now first-party,
so source annotation is possible but remains only route 3's fallback.

**The phase-3 target build answered the placement branch:** liba52 fits in the
existing six ITCM blocks. Phase 4 retained the six-block split and added the
linker ceiling. Its production build uses 191,100 B ITCM with 5,508 B padding,
while DTCM retains 37,856 B for stack/locals.

**The consequence of future overflow is not a build failure without that guard
but something worse (gate F15).** ITCM is allocated in 32 KiB blocks, and the linker
derives the stack top from the block count:
`_estack = ORIGIN(DTCM) + ((16 - _itcm_block_count) << 15)`
(`link/aius_teensy41.ld:177-179`). Phase 4 sits at 6 blocks with
5,508 B of padding. Exceeding that padding takes a 7th block and **moves
`_estack` down 32,768 B, taking that much off the STACK** — the
free-for-locals zone above `_ebss`, which drops from 37,856 B to 5,088 B
for the main loop and the audio ISR together. **This does NOT touch the
residency budget** (gate N6): `g_arena_memory` is a fixed static in `.bss`
(`optical_main.cpp:1048`) and is unaffected. Stating it as an arena problem
would send phase 4 to the wrong number — the harm is stack exhaustion, which
is arguably worse and certainly harder to diagnose. The existing ld ASSERTs
guard OCRAM only; nothing guards the DTCM locals zone.

The measured decision and retained fallbacks are:

1. ~~**Measure first:** does liba52 fit the existing ITCM allocation?~~
   **Done in phase 3:** yes, with 6,148 B padding remaining.
2. ~~**Selected:** accept ITCM placement, matching dcadec's existing
   (undocumented) treatment, and add
   `ASSERT(_itcm_block_count <= 6, ...)` to the linker script so a future
   growth fails loudly instead of silently stealing 32 KB of stack. Equality
   is deliberately not required: code shrinkage remains legal. The
   byte-for-byte drift oracle must require the assertion and mutation-test
   both its removal and a weakened ceiling.~~ **Done in phase 4:** the
   production and checkpoint-9 fault images pass the ceiling, and mutation
   tests reject both removal and weakening to seven blocks.
3. **Fallback only if the selected route later fails:** place the tree cold via a linker rule rather than
   source annotation. **The glob must match the POST-FORK path** — after
   F1 the tree is `lib/aius_a52/a52/`, so a rule written
   `*a52-core*.o(.text*) > FLASH` matches nothing and becomes **a no-op
   that still passes**, putting liba52 in ITCM and stealing 32 KB of stack
   exactly as gate F15 describes. The fork changes route 3's cost: source
   annotation is no longer forbidden because there is no zero-edit vendoring
   discipline left to violate.

FLASH is the irrelevant bank throughout (95% free); ITCM headroom and the
DTCM stack zone are the constraints.

### What the target run must establish

- **Arena, measured on hardware, not inferred from the host.** The target must
  report `create_status == AIUS_OK`, `metrics_valid == 1`, shared
  `arena_high_water <= 131072`, and both codec `alloc_count` values at zero
  after warm-up. The arena is fixed DTCM `.bss`; its runtime high-water is a
  DTCM-resident usage measurement, not an OCRAM delta. If it exceeds 131,072,
  the ordered fallbacks are: trim the parse scratch, then relocate the arena to
  OCRAM **only after corroborating it against the defect-26 boot-heap floor**.
- ~~**Decode cycles per codec require implementation, not merely publication.**
  Both adapters previously left `AiusMetrics.decode_cycles` at zero. Add one
  shared `AiusCycleClock` callback type in the codec API, configured through
  the pipeline: a zeroed clock keeps native/default metrics at zero; the target
  reader returns `ARM_DWT_CYCCNT`. DTS records the wrap-safe 32-bit delta for
  one complete AU. AC-3 accumulates `begin_access_unit` plus all six
  `decode_next_block` calls and publishes only the last completed AU. Reset,
  abandon and failure discard a partial accumulator; retained sink retries do
  not call the reader or decoder again. Deterministic tests cover default zero,
  DTS delta, six-block AC-3 aggregation, 32-bit wrap, partial-AU discard,
  retained retry, and both per-codec pipeline projections.~~ **Implemented and
  covered in phase 4.** The target reader's exact 12-byte section is pinned as
  intentionally hot by the post-link placement oracle.
- **The live cycle result** surfaces on `/api/status`'s `pipeline_diag`, not
  `/api/meters`. A valid AC-3 run has nonzero `ac3_decode_cycles`; because that
  value is the sum of six bounded service units, the hard runtime gate remains
  `audio_diag.service_max_us < 8000` with no service-budget overrun. The
  clock-coherent SD bench requires the completed-AU aggregate below 4,800,000
  cycles: all six units may execute inside one 8 ms AUDIO checkpoint at
  600 MHz, so the checkpoint budget must not be multiplied by six.
- **Every link-time bank gate remains green.** Record Phase-4 FLASH, ITCM,
  DTCM and OCRAM figures and deltas against Phase 3's 430,080 / 190,460 /
  289,824 / 392,832-byte baseline. OCRAM retains its independent 128 KiB heap
  floor; it is not evidence for decoder-arena occupancy.

The Phase-4 production and checkpoint-9 fault builds are green at
FLASH/ITCM/DTCM/OCRAM **431,104 / 191,100 / 289,824 / 392,832 bytes**:
**+1,024 / +640 / 0 / 0 bytes** against Phase 3. The link leaves 37,856 B for
stack/locals and 131,456 B free in OCRAM. These figures prove placement and
capacity only; runtime arena high-water and AC-3 cycles remain exact-image
hardware evidence.

### Live validation — the actual mission test

Bench first, TV second, because the bench is scriptable and the TV is not:

- **IEC-wrapped AC-3 replay — gate-verified end to end, not assumed.**
  Preserve every existing DTS output and add raw `<name>.ac3` plus
  `<name>_ac3.iec`; never reuse the ambiguous legacy `<name>.iec` ID. Pin
  FFmpeg to 48 kHz, 5.1 and `-c:a ac3 -b:a 448k`. The generated AC-3 transport
  validator proves every 6,144-byte period has `Pa/Pb/Pc/Pd =
  F872/4E1F/0001/3800`, a 1,792-byte byte-swapped AC-3 payload, zero padding,
  48 kHz 5.1 headers and byte identity with the raw stream **after undoing the
  IEC payload's pairwise byte swap**. A parser-backed native oracle consumes
  those generated periods with no rejection. The stream rate is 192,000 B/s;
  32 AC-3 frames are 1.024 seconds, so file size is not asserted from an integer
  requested duration.
- **The bench replay uses `channel_id_51_ac3`.** Upload and loop it from the
  SD replay manager under that distinct ID. Poll `/api/status` across at least
  120 seconds and require: `codec_pc == 1`, completed output and `bursts_ok`
  increase, nonzero AC-3 cycles below the aggregate bound, both codec
  allocation counts zero, stable boot generation, no watchdog reset, carrier
  not required for the SD source, and zero deltas for parse/decode rejects,
  service-budget overruns, output drops and AU abandons.
- **Channel identity is an on-device committed-wire measurement, not an ear
  check or dependence on the physical reverse fibre.** Record the DSP
  configuration (layout-aware 5.1 matrix, EQ bypass, fixed headroom and limiter
  state), arm `/api/measure` over `channel_id_51_ac3`, and probe 60, 300, 600,
  900, 1200 and 1500 Hz. Expected-side tones must exceed -40 dBFS; 300/1200 Hz
  must favour left over right by at least 40 dB, 600/1500 Hz must favour right
  over left by at least 40 dB, and the centre 900 Hz plus LFE 60 Hz must match
  left/right within 0.5 dB. This proves FL/FR/C/LFE/SL/SR identity through
  decode, DSP and the committed transmit words.
- **External optical bench replay precedes the TV.** Play the same
  `<name>_ac3.iec` through the CM6206 IEC958 output for at least 120 seconds;
  require continuous carrier, `codec_pc == 1`, decode/output progress and the
  same reject/drop/abandon invariants. The known 48,021-versus-48,000 clock
  defect may increase `transmit_stalls` only when `retained_retries` increases
  by the same amount; it may not cause output drops, AU abandons, service
  overruns, ingress drops/overflow, service-generation resets, gap/source
  flushes or a watchdog reset.
- **Sample-rate following is the technical clock prerequisite for the
  four-hour TV gate.**
  The known 48,021-versus-48,000 mismatch eventually exhausts retained
  capacity and produces ingress overflow/reset behavior, so paired
  stalls/retries cannot make a four-hour run a valid AC-3 acceptance. Phase 4
  completes the clock-coherent SD bench and bounded external optical proof;
  it explicitly requires `aius-sample-rate-following.md` to be delivered. A
  temporary clock-matched image cannot satisfy it.
- **The owner schedules that otherwise-ready gate after the OTA investigation
  verdict** (2026-07-29). “OTA investigated” is the finite verdict gate in
  `aius-ota-upgrade.md`; it does not mean OTA must be implemented. After both
  prerequisites, run the TV for four continuous hours on an exact committed
  production image containing the decoder and clock-following changes:
  nightly AC-3 content, dialogue lifted via `center_gain_linear` plus centre
  EQ. Poll at fixed intervals and record deltas for boot/watchdog generation,
  carrier, `bursts_ok`, completed output, codec Pc, service maxima/overruns,
  parse/decode rejects, output drops, AU abandons, ingress drops/overflow,
  service-generation resets and gap/source flushes. Stable boot generation,
  continuous carrier/decode progress and zero harmful deltas are mandatory.
  This is the only gate that confirms the mission rather than the instrument.
- By-ear checks already owed (squelch seam silence, layout-aware stereo
  level) fold into the same session.

### Phase 4 exact-image evidence (2026-07-29)

Phase 4's bounded hardware gates pass on production image
`bc05232880a5+teensy41-optical`, built `2026-07-29T21:37:13Z`. Independent
Intel-HEX parsing and a fresh live CDC identity read report the same revision
and build timestamp.

- **CM6206 external optical, 120.005 seconds / 61 polls:** continuous carrier,
  `codec_pc == 1`, 3,749 accepted bursts and 22,498 committed blocks. Completed
  AC-3 AU timing was 1,114,200 cycles, arena high-water 77,688 B, both codec
  allocation counts zero and AUDIO service maximum 2.070 ms. Parse/decode
  rejects, service overruns, output drops, AU abandons, ingress drops,
  generation resets and gap/source flushes all had zero delta; boot generation
  stayed 4 and the watchdog count stayed zero. The live final snapshot caught
  one retained transmission between its stall and retry
  (848,374 / 848,373 delta); across all polls the absolute difference was never
  above one, and the final TX queue was at its 2,048-frame high-water. This is
  evidence of a bounded one-item in-flight state, not an accumulating unpaired
  failure.
- **Clock-coherent SD replay, 120.007 seconds / 61 polls:** carrier absent,
  replay gate open, `codec_pc == 1`, 3,747 accepted bursts and 22,482 committed
  blocks. Completed AC-3 AU timing was 1,114,373 cycles, arena high-water
  77,688 B, both codec allocation counts zero and AUDIO service maximum
  5.008 ms. Parse/decode rejects, AUDIO service overruns, output drops, AU
  abandons, ingress drops and gap flushes all had zero delta; boot generation
  stayed 4 and the watchdog count stayed zero. Replay start intentionally
  requested one service-generation reset and two source flushes relative to
  the pre-start baseline; they completed by the 2.013-second poll and remained
  flat through 120 seconds.
- **Committed-wire channel oracle:** the running DSP configuration was
  layout-aware with 0.5 fixed headroom, 0.707107 centre/surround gains,
  0.316228 LFE gain at 120 Hz, empty per-input EQ, output EQ disabled and the
  -1 dBFS limiter enabled. `/api/measure` completed 16,384 captured and 8,192
  analysed frames with zero metrics-service overruns:

  | Tone / source | Left dBFS | Right dBFS | Result |
  | --- | ---: | ---: | --- |
  | 60 Hz / LFE | -23.48 | -23.48 | 0.00 dB L/R |
  | 300 Hz / FL | -12.96 | -127.54 | left +114.58 dB |
  | 600 Hz / FR | -129.02 | -12.96 | right +116.06 dB |
  | 900 Hz / FC | -15.97 | -15.97 | 0.00 dB L/R |
  | 1200 Hz / SL | -15.97 | -128.62 | left +112.65 dB |
  | 1500 Hz / SR | -116.49 | -15.97 | right +100.52 dB |

  Every expected side exceeds -40 dBFS, directional rejection exceeds 40 dB
  and centre/LFE matching is inside 0.5 dB. This proves all six AC-3 identities
  through decode, layout-aware DSP and committed transmit words without using
  the reverse optical fibre as an oracle.

The bounded Phase 4 gates are complete. Sample-rate following remains the
technical clock prerequisite for the four-hour TV mission gate; the owner has
additionally scheduled that gate and the by-ear checks after the OTA
investigation verdict. This evidence does not substitute a temporary
clock-matched run for production acceptance.

### Phase boundary and exact-image proof

The code boundary runs `a52`, `a52-fuzz`, `dts`, `pipeline`, `iec61937`,
`memory`, `itcm-placement`, `boot-target`, `replay-samples` and `registry`,
plus shell checks. It builds production and AUDIO-checkpoint-9 fault images,
completes a clean-room delivery audit, commits and pushes. Hardware proof starts
only from a rebuilt and flashed copy of that exact committed production
revision, verified through the image-reported build identity. The bench and TV
evidence update this plan and `firmware/README.md` in audited evidence commits:
Phase 4 records the SD and bounded CM6206 benches; the four-hour TV result lands
only after sample-rate following and the OTA investigation verdict.

### Debt this phase must close or record

`firmware/README.md` lacks the `sd_diag` / `boot_fuse` fields added on
2026-07-28; the swagger Status schema owes `pipeline_diag` (+4 from defect
28/33, + phase 3's codec field, four per-codec metric fields and
`decode_rejects`), `matrix.layout_aware`, `sd_diag` (+3),
`boot_fuse_0x6e0`, and `/api/meters`'s `analysis_window` /
`analysis_full_scale`. Record what is not closed rather than letting it grow
silently. The same README update replaces its stale 9,248-byte
free-for-locals claim with the exact Phase-4 build figure, documents the
six-block link ceiling, corrects `a52-fuzz`'s obsolete claim that liba52 lacks
CRC/bounded reads, registers the `replay-samples` suite, and removes the
now-false statement that the optical input is unsoldered and unexercised.

## Done, and what would make us stop

**AC-3 is done when the TV's own bitstream decodes on the box and dialogue
is intelligible with the centre lifted** — not when the suites are green.
Everything before that is instrumentation. Concretely:

1. `a52` + `a52-fuzz` + `pipeline` + `iec61937` green (phases 1–3).
2. On-target arena inside 131,072 B and per-bank report green (phase 4).
3. An IEC-wrapped AC-3 replay from the card decodes with correct channel
   identity under the committed-wire spectral oracle, followed by the same
   transport through the CM6206 optical input.
4. Sustained live AC-3 from the TV for four hours with continuous progress,
   no watchdog reset, and no harmful diagnostic delta — which is also defect
   31's acceptance test.
5. The operator can hear the dialogue.

**Stop conditions, decided in advance so they are not rationalised away
mid-flight:**

- **The on-target arena does not fit** after trimming the parse scratch, and
  the OCRAM relocation fails to corroborate against the defect-26 heap
  floor. Then AC-3 residency is a real design problem, not a tuning
  exercise, and it goes back to design rather than being forced.
- **liba52's ITCM footprint takes a 7th block** and no cold-placement route
  is acceptable — that silently costs 32 KB of stack, and shipping it would
  trade a working box for a decoding one.
- **Phase 2b does not fix defect 31.** If sustained decode still dies by
  watchdog with decode off the ISR, the ISR-storm hypothesis was wrong; stop
  and re-diagnose rather than adding AC-3 load to an unexplained crash.
- **A third memory-safety defect surfaces in liba52.** Two were found in the
  first two fuzz runs. A third would say the library needs either a
  systematic audit or replacement, and continuing to patch site-by-site
  would be the wrong response to a pattern.

**Rollback posture:** every phase is separately committed and the target
image is flashable from any of them, so the fallback at any point is the
last flashed DTS-only image. AC-3 adds no config surface a rollback would
strand — `features.*` gains nothing, and a board flashed backwards simply
refuses type 1 again as it does today.
