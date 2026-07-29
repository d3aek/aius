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
  liba52; phase 2a takes AC-3 to ~27 KB, giving ~127,504 against 131,072
  (including the ~5.2 KB pipeline instance) with both decoders resident. Neither growing the arena nor swapping is
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

`doc-index --validate-outstanding-work` cannot validate this plan because
document discovery does not yet index `src/hardware/**/docs`; it reports
`outstanding.snapshot.plan-absent`. The required component-wide disposition
is therefore recorded manually below, using the same canonical grammar.

## Outstanding-work audit

- `docs/plans/aius-capability-gate.md` — `coordinate`: Preserve the shipped DTS capability contract; AC-3 reuses its codec ABI, arena, and target timing evidence while phase 2 moves DTS decode out of the ISR.
- `docs/plans/aius-cost-model.md` — `unrelated`: Decoder firmware does not change the prototype BOM, layer-count decision, or supplier pricing.
- `docs/plans/aius-dsp-config-api.md` — `coordinate`: AC-3 feeds the existing layout-aware DSP and phase 3 updates its status labels; DSP configuration semantics remain owned by that plan.
- `docs/plans/aius-hardware-spec.md` — `coordinate`: Phase 4 must preserve the hardware spec's optical path and memory gates while proving the decoder's target footprint.
- `docs/plans/aius-network-hardening.md` — `coordinate`: AC-3 phase 2 owns the defect-31 decode-off-ISR fix and proof; the hardening plan remains the defect ledger and retains defect 34.
- `docs/plans/aius-open-work-register.md` — `coordinate`: The register remains a projection of authoritative work and must strike defect 31 when phase 2 closes it.
- `docs/plans/aius-optical-prototype.md` — `coordinate`: Phase 4's card replay and live-TV checks extend the existing optical validation without replacing its broader prototype gates.
- `docs/plans/aius-ota-upgrade.md` — `unrelated`: AC-3 adds no update format, rollback state, or OTA transport surface.
- `docs/plans/aius-replay-manager.md` — `coordinate`: Phase 4 adds AC-3 IEC fixtures through the existing replay transport without changing replay-manager scheduling or storage semantics.
- `docs/plans/aius-sample-rate-following.md` — `coordinate`: AC-3 remains 48 kHz-only and its bounded-reader proof relies on 48 kHz frame alignment; later rate following must revisit that invariant deliberately.
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
2. **Add CRC-16/ANSI validation to the adapter (~30 lines).** AC-3 frames
   carry CRC1/CRC2 and liba52 ignores both; validating the whole frame
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
- **F3c — CRC**, with the fuzz change below, which is not optional.

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

**F3a entry-point resolution:** `aius_a52_decode_access_unit` calls the new
`a52_bitstream_set_end` declared in guarded `a52.h`, after `a52_syncinfo`
validates the exact payload length and 48 kHz rate and immediately before
`a52_frame`. `a52_bitstream_set_ptr` does not clear the end. F3c reuses this
already validated frame length for CRC. The byte-exact bound relies on a
**48 kHz-only
invariant** — `set_ptr` aligns down, and 48 kHz frame sizes are always a
multiple of 4 — so relaxing the rate guard later would make the bound off by
up to 3 bytes.

**Step F4 — REDUCES TO NOTHING; keep it only as an explicit decision.** The
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

**Owned paths** (2a): `firmware/lib/aius_a52/a52/{a52.h,bitstream.c,
bitstream.h,a52_internal.h,NOTICE.md}`, `firmware/lib/aius_a52/{include,src}`,
`firmware/test/a52/{test_a52.c,fuzz_a52.c}`, `firmware/scripts/test.sh`,
this plan. Relative to ``.

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
never available: DTCM's whole free-for-locals zone is 48,800 B and that
region is also the stack. But relocating it to OCRAM is not simply a matter
of free bytes: the 32,768 B DSP staging carve was already VETOED from OCRAM
by the defect-26 link-time heap floor with only 13,312 B of slack
(`optical_main.cpp:1305-1311`), and moving the whole 131,072 B arena is 4×
what was rejected. The gate is the boot-heap floor, not free space. **Phase
4 must corroborate the OCRAM route against that floor before relying on it,
or treat 127,504/131,072 as the budget.**

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

**Step 1 — fork edit 4: bound liba52's bit reader (pad still in place).
DONE 2026-07-29.**
`bitstream_fill_current` (`bitstream.c:44-50`) is the SOLE dereference of
`buffer_start`, `a52_bitstream_get_bh`/`_bh_2` each call it exactly once, the
inline fast paths touch only `current_word`, there are no byte-wise reads,
and max `num_bits` is 24 so one fill always suffices. One choke point, so an
end pointer plus zero-fill is sufficient — provided all five of these hold,
each of which the gate found the first draft did not state:

- **(a) The setter runs in `aius_a52_decode_access_unit`, BEFORE
  `a52_frame`, and `a52_bitstream_set_ptr` must NOT clear the end.**
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
- A mid-AU error disarms the iterator.
- **`aius_a52_abandon(dec)` is part of the API** (added after the phase-3
  gate, N1). Implicit abandon-on-`begin` is not sufficient: a caller that
  stops mid-AU for a reason of its own — above all a FULL TRANSMIT RING,
  which is an ordinary condition, not an error — must be able to stop
  cleanly. Without it the iterator stays armed and the next
  `begin_access_unit` abandons at a point that is not a stream
  discontinuity, corrupting the next AU's block 0 through the IMDCT overlap.
- **`abandon` RESETS THE OVERLAP; disarming a flag is not enough** (gate
  R1 — the second defect this API's own introduction created). The harm is
  the stale overlap in `state->samples[1536..3071]`, and neither clearing an
  `armed` flag nor `a52_frame` touches it, so an implementation of
  `dec->armed = 0; return AIUS_OK;` would satisfy a naive contract, satisfy
  the fuzz property below, and still ship a ~5.3 ms seam artifact on every
  ring-full. `abandon` therefore zeroes 1,536 floats at
  `a52_samples(state) + 1536` — reachable through the public `a52.h`, and
  consistent with phase 1's edit 2, which already zeroes all 3,072 at init.
- **The assertion that actually proves it**: after `abandon` + `begin` on a
  fresh AU, block 0 is **bit-identical to a fresh decoder's block 0 on the
  same AU**. A block-count-and-disarm check cannot distinguish a real reset
  from a flag flip; this can. (Gate-verified against the vendor source: all
  six IMDCT delay slots resolve to `state->samples[1536..3071]` with or
  without LFE, so 1,536 floats is exactly the right span — no over- or
  under-write.)
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
- `begin_access_unit` while an AU is in flight ABANDONS and re-arms.
  Recovery is correct (`a52_frame` re-arms the reader) but **lossy**: the
  IMDCT overlap in `state->samples[1536..3071]` carries the abandoned
  block's un-overlapped tail into the next AU's block 0 — the same hazard
  the differential's fresh-decoder-per-vector rule already documents.
  **Abandonment is permitted at a stream discontinuity — carrier edge,
  squelch, re-type — OR at a caller-driven stop that has reset the overlap
  via `aius_a52_abandon`** (gate R1b: the earlier wording listed `abandon`
  itself among the permitted discontinuities, so the rule licensed exactly
  what it constrained). The reason survives the withdrawal of the discard
  policy (gate N3): truncation merely loses audio, whereas abandoning
  mid-AU with a stale overlap POISONS the next AU's block 0. Truncate
  freely; never leave an armed iterator behind.
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
per-sample, and later edits retain the synthesized zeros F3a's bounded reader
already returns for a past-end read.

### Slice 2b — decode off the audio ISR (target)

- **AC-3 slices; DTS moves.** dcadec decodes an AU in one `core_parse` +
  `core_filter` pair and cannot be sliced without vendor surgery we are not
  doing, so DTS's fix is relocation to epoch context. AC-3 gets both. Record
  the asymmetry rather than pretending the codecs are symmetric.
- Decode runs in the runtime's epoch/service context, feeding the existing
  transmit ring; the audio ISR's job shrinks to moving words. **The whole-AU
  hold-back an earlier draft required is withdrawn** along with the discard
  policy it served, so `enqueue` keeps its per-block free check and
  per-block commit unchanged — **but the ring-depth arithmetic that draft
  carried survives the withdrawal and must not be deleted with it (gate
  N1b)**: six back-to-back `enqueue` calls occupy the same 1,536 of 2,048
  frames whether or not the commit is deferred, leaving AC-3 512 frames
  (10.7 ms) of drain tolerance against DTS's 1,536. Ring depth is sized
  against worst service latency, jitter, **and that per-AU occupancy**;
  ring-full mid-AU is an expected condition to handle, not an edge case.
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

Pipeline dispatch by Pc data type (phase 3), arena relocation (phase 4, only
if the ~3,568 B spare proves uncomfortable), DRC config flag.

## Phase 3 contract (2026-07-28) — dispatch, and the observability it forces

**Owned paths:** `firmware/lib/aius_iec61937/`, `firmware/lib/aius_pipeline/`,
`firmware/tools/iec_corpus/`, `firmware/test/{pipeline,iec61937}/`,
`firmware/src/optical_main.cpp` (status JSON only),
`firmware/sd/www/{status.js,dsp-editor.js,replays.html,index.html}`,
**`firmware/scripts/test.sh`**, **`firmware/platformio.ini`**, this plan.
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
of 131,072). `dts_feed`'s burst
completion (`aius_pipeline.c:320-334`) routes on `AiusBurst.data_type`:
11/12/13 to `aius_dts`, **1 to `aius_a52`**, everything else skipped exactly
as today. The class stays `AIUS_PIPELINE_DTS` — "non-linear bitstream" — with
no new classification state, because classification is preamble logic and
format-agnostic.

**AC-3 consumption follows phase 2a's iterator**: six `decode_next_block`
calls, each delivered straight through `AiusPcmSink.consume` exactly as the
DTS path delivers its one block — **six sink calls per AU, no accumulator,
no hold-back, no ring restructure** (phase 2a's emit-as-decoded revision;
the transmit ring is post-DSP stereo and unreachable from here). A mid-AU
error truncates the AU after the blocks already delivered.

**This section describes the PRE-2b synchronous shape and must be rebased
when 2b lands (gate F3).** Two things change under epoch-context decode:
`AiusBurst.payload` is a single parser-owned buffer overwritten by the next
push, so an AU cannot outlive its push without a copy; and an iterator left
armed across service slices collides with 2a's "abandon only at a stream
discontinuity" rule. **The abandon call sites must be named and wired** —
`aius_pipeline_reset`, `retype_to_dts`, and the starvation branch in
`aius_pipeline_push` are three discontinuities, and none of them calls
anything today.

**A FOURTH site, which is not a discontinuity at all and is the one that
will actually fire (gate N1):** `aius_pipeline.c:330-331` returns
immediately when the sink refuses a block, and `enqueue` returns
`AIUS_NO_SPACE` whenever the block exceeds `aius_spdif_tx24_free()`. With
six sink calls per AU that is a routine ring-full, and it currently returns
out of `dts_feed` with the iterator armed at block k+1 — so the next burst's
`begin_access_unit` abandons mid-AU and poisons its block 0. **Phase 3 must
call `aius_a52_abandon` before `return cs`.** The API for that is added by
phase 2a specifically because phase 3 cannot express the fix without it.

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
- **`aius_pipeline_get_metrics` must be extended here, not in phase 4
  (gate F5)**: it is hardwired to `aius_dts_get_metrics(p->decoder, …)`
  (`aius_pipeline.c:513`) and `optical_main.cpp` publishes `decode_cycles` /
  `alloc_count` from it, so with AC-3 decoding the box would report stale
  DTS figures — and phase 4's "decode cycles per codec" would have no
  accessor to read. `lib/aius_pipeline/` is phase-3-owned and phase 4 does
  not own it, so the extension belongs in this phase. (`arena_high_water` is
  unaffected: it reads whole-arena high water either way. `alloc_count` is a
  single global shim counter, so with two decoders arming per call its
  meaning needs restating rather than doubling.)
- **`g_overruns` changes meaning under AC-3 and must not be left to lie
  (gate R2).** `optical_main.cpp:1010,1022` increments it on any non-OK
  `aius_pipeline_push`, and by this contract's own arithmetic AC-3 refuses
  above ring level 512 where DTS refuses above 1,536 — so identical clock
  drift moves the counter roughly 3× more readily, on a condition this
  contract explicitly calls "NOT an error". The counter is published on
  status and fed to the flight recorder (`:1226`), so a silent 3× shift
  would mislead exactly the diagnostics defect 31 depends on. Either
  suppress the increment for a truncating refusal or record the meaning
  change beside the counter — decide, do not inherit.
- **UI**: labels become bitstream-generic with the codec named from that
  field — "bitstream (DTS)" / "bitstream (AC-3)", "unknown" until a burst
  has decoded. **Four files, not one** (gate F6): `status.js:36-37`,
  `dsp-editor.js:2141` (`CARRIER_ENCODINGS = {3:'PCM', 4:'DTS'}`),
  `replays.html:17`, `index.html:34`.
- Swagger/status schema debt grows by both fields; record it with the others.

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

## Phase 4 contract (2026-07-28) — target integration

**Owned paths:** `firmware/test/itcm-placement/verify_target.py`,
`firmware/src/optical_main.cpp`, `firmware/tools/replay_samples/`,
`firmware/README.md`, this plan — plus, **if the placement measurement below
sends us down route 2 OR route 3**, `firmware/link/aius_teensy41.ld` and its
drift test (route 2 adds the `_itcm_block_count` assertion, route 3 the
cold-placement rule; both edit that file). (`platformio.ini` moved to phase 3: the target must stay
buildable from the commit that adds dispatch, so a52's sources and include
path join the build there, not here.)

**Build-order dependency neither phase stated:** phase 4's arena and cycle
measurements cannot start until the target links, which is phase 3's
`platformio.ini` change. Phase 3 first, always.

### Placement — the decision, on a premise the gate had to correct

**An earlier draft claimed the cold-placement guard "already lists every
dcadec object" and that the five a52 objects just join it. Both halves were
false (gate F14).** `verify_target.py:44-53`'s `COLD_OBJECTS` is eight
objects — `aius_boot`, `aius_http` ×2, `aius_net`, `aius_runtime`,
`aius_sd`, and two Teensy adapters — with **no dcadec object and no
`aius_dts`**, and `COLD_TREES` covers no vendor tree. The mechanism is
source annotation (`FLASHMEM`), and grepping the dcadec sources returns
zero: **dcadec's code is in ITCM today.** Adding a52 objects to that list
would fail immediately, since they emit `.text` and phase 1's zero-edit
discipline forbids annotating vendored sources.

**And the consequence of overflow is not a build failure but something
worse (gate F15).** ITCM is allocated in 32 KiB blocks, and the linker
derives the stack top from the block count:
`_estack = ORIGIN(DTCM) + ((16 - _itcm_block_count) << 15)`
(`link/aius_teensy41.ld:177-179`). ITCM currently sits at 6 blocks with
23,028 B of padding. Exceeding that padding takes a 7th block and **moves
`_estack` down 32,768 B, taking that much off the STACK** — the
free-for-locals zone above `_ebss`, which drops from 48,800 B to 16,032 B
for the main loop and the audio ISR together. **This does NOT touch the
residency budget** (gate N6): `g_arena_memory` is a fixed static in `.bss`
(`optical_main.cpp:1048`) and is unaffected. Stating it as an arena problem
would send phase 4 to the wrong number — the harm is stack exhaustion, which
is arguably worse and certainly harder to diagnose. The existing ld ASSERTs
guard OCRAM only; nothing guards the DTCM locals zone.

So the real decision, which phase 4 must take deliberately:

1. **Measure first** (`scripts/build.sh` + the per-bank report makes this
   nearly free): does liba52's `.text` fit inside the 23,028 B ITCM
   headroom? That number decides everything below and costs one build.
2. **If it fits**: accept ITCM placement, matching dcadec's existing
   (undocumented) treatment, and add an `_itcm_block_count` assertion to the
   linker script so a future growth fails loudly instead of silently
   stealing 32 KB of stack.
3. **If it does not**: place the tree cold via a linker rule rather than
   source annotation. **The glob must match the POST-FORK path** — after
   F1 the tree is `lib/aius_a52/a52/`, so a rule written
   `*a52-core*.o(.text*) > FLASH` matches nothing and becomes **a no-op
   that still passes**, putting liba52 in ITCM and stealing 32 KB of stack
   exactly as gate F15 describes. Take ownership of
   `link/aius_teensy41.ld` **and its byte-for-byte drift test**, neither of
   which this contract currently owns — add both before starting. Note the
   fork also changes route 3's cost: source annotation is no longer
   forbidden, because there is no zero-edit vendoring discipline left to
   violate.

FLASH is the irrelevant bank throughout (95% free); ITCM headroom and the
DTCM stack zone are the constraints.

### What the target run must establish

- **Arena, measured on hardware, not inferred from the host.** The phase-2a
  figure is host LP64-derived while dcadec's 62,560 is an on-target ILP32
  measurement; the two must be re-measured together on target. If the real
  total exceeds 131,072, the ordered fallbacks are: trim the parse scratch,
  then relocate the arena to OCRAM **only after corroborating it against the
  defect-26 boot-heap floor** (which already vetoed a 32,768 B carve with
  13,312 B of slack — the floor, not free bytes, is the gate).
- **Decode cycles per codec** through `AiusMetrics.decode_cycles` (DWT).
  It surfaces on **`/api/status`'s `pipeline_diag`, not `/api/meters`**
  (`optical_main.cpp:2256, 2274` — gate F16), and reading it per codec
  depends on phase 3 having extended `aius_pipeline_get_metrics` past its
  DTS hardwiring. This is the number that says whether phase 2b actually
  fixed defect 31: AC-3 is 1536 frames (32 ms of audio) per AU, so a decode
  that fits the epoch service budget with margin is the pass condition.
- **Per-bank report green** (`build.sh` fails the build on over-capacity),
  with the OCRAM delta from phase 2a's arena occupancy recorded.

### Live validation — the actual mission test

Bench first, TV second, because the bench is scriptable and the TV is not:

- **IEC-wrapped AC-3 replay — gate-verified end to end, not assumed.**
  `tools/replay_samples/generate.sh` already produces `.iec` via ffmpeg's
  `spdif` muxer for DTS, and the reviewer ran the AC-3 route: one second
  muxes to 196,608 B (32 bursts × 6,144 B) with `Pa/Pb/Pc/Pd =
  F872/4E1F/0001/3800`, an exact 3,072-word period, zero-stuffed to that
  period. **No endianness problem**: the file is LE on disk with the payload
  byte-swapped, so read through `aius_replay_word_from_le` the words are the
  canonical big-endian wire form and the parser locks with `swap = 0` —
  identical to the DTS route, and 16-vs-24-bit is a non-issue. Word rate is
  96,000 words/s, exactly the replay ring's existing budget. The only recipe
  change is `-c:a dca -strict -2` → `-c:a ac3 -b:a <rate>`; every `join`
  already states `map=`. This gives a repeatable on-card AC-3 source with
  **known channel identity** — the defect-30 lesson applied to bring-up, so
  the first AC-3 decode on hardware is validated against a tone programme
  rather than by ear on unknown content. `tools/replay_samples/README.md`
  is titled "DTS replay sample pack" and needs re-titling with the on-card
  budget stated (AC-3 `.iec` is the same 192,000 B/s).
- **Then the TV**: nightly content, dialogue lifted via `center_gain_linear`
  plus centre EQ. That is the reason this project exists, and it is the only
  test that can confirm the mission.
- By-ear checks already owed (squelch seam silence, layout-aware stereo
  level) fold into the same session.

### Debt this phase must close or record

`firmware/README.md` lacks the `sd_diag` / `boot_fuse` fields added on
2026-07-28; the swagger Status schema owes `pipeline_diag` (+4 from defect
28/33, + phase 3's codec field), `matrix.layout_aware`, `sd_diag` (+3),
`boot_fuse_0x6e0`, and `/api/meters`'s `analysis_window` /
`analysis_full_scale`. Record what is not closed rather than letting it grow
silently.

## Done, and what would make us stop

**AC-3 is done when the TV's own bitstream decodes on the box and dialogue
is intelligible with the centre lifted** — not when the suites are green.
Everything before that is instrumentation. Concretely:

1. `a52` + `a52-fuzz` + `pipeline` + `iec61937` green (phases 1–3).
2. On-target arena inside 131,072 B and per-bank report green (phase 4).
3. An IEC-wrapped AC-3 replay from the card decodes with correct channel
   identity, verified against the tone programme rather than by ear.
4. Sustained live AC-3 from the TV for a full evening with no watchdog reset
   — which is also defect 31's acceptance test.
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
