---
title: Aius sample-rate following
type: plans
scope: component
component: aius
status: draft
subtype: design
tags: [aius, firmware, teensy, audio, clocking, spdif, resampling]
updated: 2026-07-29
toc: true
---

# Aius sample-rate following

How the `teensy41-optical` firmware stops assuming 48 kHz. The immediate need is
44.1 kHz input support; the production goal is that the input clock drives the
output. This document costs three mechanisms against the measured per-block
budget and recommends one: **a fixed 48 kHz output carrier with an
adaptive-ratio upsampler on non-native sources**, which delivers rate-following
in the digital domain and needs no PLL retune, no vendor-tree change and no DTCM.

It is a companion to [`firmware-io.md`](../reference/firmware-io.md), which
records the buffers and banks this design spends, and to
[`aius-network-hardening.md`](aius-network-hardening.md), whose §Deciding
principle is the tie-breaker applied throughout. Neither is restated here.

## Deciding principle, applied

**Audio hot-path reliability decides every trade-off.** Two consequences bind
this design before any cost is counted:

- **No resampler state in DTCM.** The free-for-locals zone is 9,248 bytes and it
  is the entire stack, main loop and audio ISR both. `enqueue()` already spends
  ~4 kB of it. Every buffer this design adds goes to `DMAMEM`.
- **Audio that is known to be broken is muted, not shipped.** A 44.1 kHz source
  that stutters is worse than one cleanly refused and reported. This is why
  Phase 1 is observability plus mute rather than a half-working resampler.

## The defect being designed away

`AUDIO_SAMPLE_RATE_EXACT` is now pinned to 48 kHz for `teensy41-optical` and
verified exact in the clock tree. What is *not* fixed is the mechanism that hid
the original error.

**Nothing in the audio path resamples, and nothing counts a mismatch.** The
receiver DMAs `SPDIF_SRL`/`SRR` straight into `audio_block_t`s at the carrier's
rate (`evidence/source-snapshots/PJRC-SPDIF-RX-SOURCE/input_spdif3.cpp:52`) and
the transmitter clocks blocks out at the engine rate. The retained receive
snapshot predates Frank Bösing's asynchronous converter — no `Resampler`, no
`resample()` — and is byte-identical to the installed framework copy, so
`lib_ignore = Audio` is not concealing a newer one.

The input holds `update_responsibility` because `g_spdif_in` is constructed
before `g_spdif_out`, so with a carrier present the whole graph is pulled at the
*carrier's* rate while the transmit DMA runs at the *engine* rate.
`AudioOutputSPDIF3::update()` holds two blocks per channel and, when both are
full, releases the oldest (`output_spdif3.cpp:181`); its ISR falls back to
`block_silent` when it has none (`output_spdif3.cpp:122`). **Both the drop and
the repeat happen inside vendor code that touches none of our counters.** At a
48 kHz engine against a 44.1 kHz source the transmitter falls through to
`block_silent` about 8 % of the time; before the pin, a 48 kHz carrier against a
44.1 kHz engine discarded roughly 30 blocks per second. Nothing counted either.

`AudioInputSPDIF3::sampleRate()` is served at UDP status payload offset 25 and
compared to nothing.

**Live same-nominal evidence, 2026-07-29.** The CM6206 optical loop reports
48,021 Hz while the Teensy output remains fixed at 48,000 Hz. During continuous
DTS decode the new ingress ring grew linearly by 1,280 words every 30 seconds,
42.7 words/s, matching the predicted two IEC words for each of the ~21 excess
input frames/s. It reached 8,192 words and dropped one 256-word chunk after
about 137 seconds. AUDIO stayed below 2.25 ms, the normal epoch stayed below
2.90 ms, and no output drop, AU abandon, budget overrun, watchdog reset or
recorder gap occurred. This converts same-nominal asynchronous drift from a
design risk into a reproduced hardware defect and gives phases 2/3 a live
feedback loop on this board.

The tagged +438 ppm isolation trim subsequently held the same loop for 1,857
seconds of live DTS without an ingress drop or generation reset. Ingress
high-water stayed at 442 words from the first minute through the final sample;
because every one-second level sample lies between zero and that unchanged
ceiling, the worst possible least-squares magnitude over the run is below 0.36
words/s, inside the 1 word/s gate. AUDIO max was 2.401 ms and the normal epoch
max 3.036 ms. This is long-run evidence for the selected live PLL numerator
control, not a production implementation: the tagged write remains absent from
the working tree and proper slew/mute/control policy remains Phase 3b here.

**There is a second uncounted loss, closer to home.** `emit_block()` zero-fills
when the ring is empty (`src/optical_main.cpp:932`) and increments no counter.
That is the firmware's own underrun, it is already on the right side of the
vendor boundary, and it is currently invisible. Counting it is a one-line change
and the single highest-value observability item in this plan.

## Sample-rate dependency inventory

`RT` = already runtime-parameterised, `CT` = genuinely compile-time, `CT→RT` =
compile-time today and required to become runtime.

### The output clock

| What | Where | Verdict |
|---|---|---|
| Engine and transmit clock | `platformio.ini` | `CT`. Stays `CT` under the recommendation. |
| Engine == generated config | `src/optical_main.cpp:88` | `CT`. Retained; see [§What replaces the static_assert](#what-replaces-the-static_assert). |
| PLL4 programming | `output_spdif3.cpp:229`–`:238` | `CT` by macro only. `config_spdif3()` is already a pure function of `fs`. |
| `SPDIF0_CLK_ROOT` divider | `output_spdif3.cpp:246` | Same — derived from `fs`. |
| `SPDIF_STC[TxClk_DF]` | `output_spdif3.cpp:273`–`:287` | Same. `clkdiv = clock / (fs*64)`, nearest-divider rounding. |
| `set_audioClock(..., force)` | `lib/aius_audio/src/aius_audio_clock.cpp:10` | `RT`-capable. The `force` argument exists; unforced re-entry is a no-op. |

`config_spdif3()` is `FLASHMEM` and takes no argument — it reads the macro.
Making the transmit tree runtime-settable is a signature change, not an
algorithm change. It also early-returns when either DMA is already enabled
(`output_spdif3.cpp:257`), so a naive re-call after `begin()` reprograms the
clock tree but not `SCR`/`SRPC`/`STC`.

### The decode and passthrough rates

| What | Where | Verdict |
|---|---|---|
| PCM passthrough rate tag | `lib/aius_pipeline/include/aius_pipeline.h:48`, stamped `aius_pipeline.c:85` | **`CT→RT`. The one outright falsehood in the chain** — every passthrough block claims 48,000 whatever the carrier is doing. |
| PCM staging block size | `lib/aius_pipeline/include/aius_pipeline.h:45` | Rate-independent (a frame count). Coupled to the `enqueue()` cap. |
| DTS decoded rate | `lib/aius_dts/src/aius_dts.c:231` | **`RT` already.** Taken from the bitstream. |
| DTS `output_rate` derivation | `vendor/dcadec-core/core_decoder.c:959` | `RT`. `synth_x96 = 0` because the adapter passes only `CORE_ONLY \| CORE_BIT_EXACT`, so `output_rate` **is** the header rate. |
| Legal DTS core rates | `vendor/dcadec-core/core_tables.h:25`–`:30` | `RT`. 44,100 is legal, alongside 8/11.025/12/16/22.05/24/32/48 kHz. |
| Decoded frame count | `vendor/dcadec-core/core_decoder.c:962` | `RT`. `npcmsamples = npcmblocks * 32`, a bitstream field. |
| Decoded-block cap | `src/optical_main.cpp:833` | `CT` (512). See the type-I finding below. |
| IEC 61937 payload bound | `lib/aius_iec61937/include/aius_iec61937.h:39`–`:44` | Rate-independent — the bound is in frames. Only the comment's "at 48 kHz" misleads. |

**A pre-existing finding worth recording.** The parser accepts DTS data types
11/12/13, i.e. IEC 61937 burst types I/II/III with 512/1024/2048-frame
repetition periods. A type-II or type-III burst decodes to 1024 or 2048 samples
per channel, and `enqueue()` rejects anything over `AUDIO_BLOCK_SAMPLES * 4` =
512 with `AIUS_INVALID_ARG`. **So only type I is actually supported end to end**,
despite the parser and the buffer being sized for type III. Independent of this
plan; fix it or document it as a limit.

### The DSP chain

| What | Where | Verdict |
|---|---|---|
| Output-EQ biquads | `src/optical_main.cpp:952`, `:955` | `RT`-capable, wrong source. `aius_biquad_*` all take `fs`; they are fed `cfg.sample_rate_hz`, not the stream's. |
| Input-EQ biquads | `src/optical_main.cpp:978`, `:981` | Same. |
| Limiter coefficients | `lib/aius_dsp/src/aius_dsp.c` | **`RT`, and already correct.** Called with the *stream* rate and re-derives internally when `fs` changes. |
| Limiter lookahead bound | `lib/aius_dsp/include/aius_dsp.h:209` | `CT` (240 samples = 5 ms at 48 kHz). Correct for every rate ≤ 48 kHz; clamps above. |
| Meter window | `src/optical_main.cpp:1018` | `RT`-capable, wrong source (`cfg.sample_rate_hz`). |
| Meter µs-per-frame Q16.16 | `src/optical_main.cpp:1021` | Same. |
| Meter publish rate | `src/optical_main.cpp:420` | Rate-independent (20 Hz, counted in frames). |
| Parsed model rate | `lib/aius_dsp_config/src/aius_dsp_config.c:1142` | `RT` already — an SD or `PUT` model may carry any positive rate, unvalidated against the engine. |

**The DSP is in better shape than the headline suggests.** Every stage that needs
`fs` already takes it. The defect is that `apply_config()` sources it from the
*config model* rather than the *stream*, so a 44.1 kHz block gets 48 kHz
coefficients: a 100 Hz highpass lands at 91.9 Hz. A rewiring, not a redesign.

### The replay seam and the generated config

| What | Where | Verdict |
|---|---|---|
| Replay words/second | `lib/aius_replay/include/aius_replay.h:43` | `CT` (48,000). |
| Replay bytes/second | `lib/aius_replay/include/aius_replay.h:44` | `CT` (192,000). |
| `duration_seconds` | `lib/aius_replay/src/aius_replay.c:365` | `CT`. `file_bytes / 192000` — **wrong for any non-48 kHz replay file**, silently. |
| Generated engine rate | `config/optical_dsp.json` → `scripts/build_config.py` | `CT` (generated). |

### Reporting surfaces, all currently one-sided

| What | Where | Verdict |
|---|---|---|
| Measured carrier rate | `input_spdif3.cpp:250` | `RT` measurement, RM-confirmed below. Compared to nothing. |
| UDP offset 25 | `src/optical_main.cpp:1129` | Diagnostic only. |
| `GET /api/status` `sample_rate` | `src/optical_main.cpp:1663` | Diagnostic only. |
| `GET /api/meters` `sample_rate_hz` | `src/optical_main.cpp:1379` | The *engine's* belief. |
| `GET /api/dsp/config` `sample_rate_hz` | `src/optical_main.cpp:1239` | The *config's* belief. |

**Four surfaces each report one party's belief and none reports the
relationship.** That is precisely the shape that let an 8.1 % error hide behind
three clean counters.

### Word width, because it is where this design lands

- `audio_block_t` is `int16_t data[AUDIO_BLOCK_SAMPLES]`
  (`cores/teensy4/AudioStream.h:76`), so the engine is hard 16-bit.
- `enqueue()` reduces 24-bit to 16-bit with `>> 8` and clamps
  (`src/optical_main.cpp:878`) into an `int16`-valued ring (`:781`).
- The transmitter shifts back up: `*dest++ = (*src_left++) << 8`
  (`output_spdif3.cpp:130`).

The accepted 24-bit output path removes the middle two. See
[§Interaction with the 24-bit output path](#interaction-with-the-24-bit-output-path).

### Tests that encode 48 kHz as an expectation

`test/dsp/test_dsp_vectors.c` asserts every vector decodes to exactly 48,000;
`test/pipeline/test_pipeline.c` treats any other rate as a failure;
`test/config/test_build_config.py` pins the generated constant. All three must
widen, and the widening is the cheapest possible proof that the rate became data.

## The clock mechanism — what the silicon actually supports

### Recovered-clock slaving is not available on-chip

**`SPDIF_STC[TxClk_Source]` has no recovered-clock option.** RM Rev. 3
§40.6.1.19.4 (p. 2077) enumerates the transmit clock mux exhaustively:

```text
010b - (reserved)
000b - REF_CLK_32K input (XTALOSC 32 kHz clock)
001b - tx_clk input (from SPDIF0_CLK_ROOT)
011b - SPDIF_EXT_CLK, from pads
101b - ipg_clk input (frequency divided)
```

The DPLL's recovered `SPDIF_RxClk` is absent. Nor is it reachable one level up:
`CCM_CDCDR[SPDIF0_CLK_SEL]` offers only PLL4, PLL3 PFD2, PLL5 and `pll3_sw_clk`
(p. 1059).

**The recovered clock is routable — to SAI, not to SPDIF.** RM Table 38-2
(p. 1985) lists `spdif_srclk` as a selectable SAI1/2/3 `MCLK3` source via
`IOMUXC_GPR1`. The Audio Overview's claim that the recovered clock drives "the
SPDIF transmitter, SAI ports…" (§37.1.4, p. 1983) is true only by way of the
pads: **the SPDIF transmitter is reached externally, not internally.**

**The dependency in this image currently runs the other way.**
`SPDIF_SRPC[ClkSrc_Sel]` is set to `0001b` (`output_spdif3.cpp:270`), which RM
§40.6.1.4.4 (p. 2058) defines as *"if (DPLL Locked) SPDIF_RxClk else tx_clk"* —
the **receiver** falls back to the transmit clock.

**The external variant does exist, and it is rate-independent.** RM Table 40-4
(p. 2050) gives `SPDIF_SR_CLK` as an output and `SPDIF_EXT_CLK` as an input;
§40.3.1.7 (p. 2049) states that after DPLL lock the recovered pulse rate is
**128 × fs**. On Teensy 4.1:

| Signal | Pad | Teensy pin | Mux |
|---|---|---|---|
| `SPDIF_SR_CLK` (out) | `GPIO_AD_B1_04` | 40 | ALT3 |
| `SPDIF_LOCK` (out) | `GPIO_AD_B1_06` | 17 | ALT3 |
| `SPDIF_EXT_CLK` (in) | `GPIO_AD_B1_07` | 16 | ALT3 |

So one board trace from pin 40 to pin 16, plus `STC[TxClk_Source] = 011b` and
`TxClk_DF = 1` (divide by 2, taking 128·fs to the 64·fs the transmitter needs),
gives genuine hardware slaving. **The divider ratio does not depend on fs**,
which is the whole attraction: no rate detection, no drift management, ever.

### Runtime PLL retune is sanctioned but drops the carrier

- **The RM's normative procedure powers the PLL down.** §14.6.1.5.3 (p. 1030)
  requires transitioning dependent clocks away and powering the PLL down before
  modification. The shipped helper already does bypass → write → spin on `LOCK`
  → un-bypass.
- **Lock time is bounded and small.** PLL4 locks in `< 11250` reference cycles at
  24 MHz (CEC Rev. 4 Table 15) = **< 469 µs**, 4.4 % of one decoded block.
- **The expensive part is not ours.** Bypassing PLL4 gates `SPDIF0_CLK_ROOT`,
  which clocks the transmit DMA, so the outgoing carrier stops. A downstream DAC
  or AVR re-acquires over orders of magnitude longer than 469 µs, muting while it
  does. That duration is a property of the connected device.
- **Blocks in flight are lost or stale.** A correct retune must mute
  (`SPDIF_SCR[VALCTRL]`, exposed as `mute_PCM()`), drain, retune, unmute.

**A live ppm trim is the interesting sub-case and it is unproven.** PLL4 is
fractional-N — `PLL output = Fref × (DIV_SELECT + NUM/DENOM)` (§14.6.1.3.4) with
a 30-bit numerator (§14.8.9) — so writing `CCM_ANALOG_PLL_AUDIO_NUM` alone could
trim the transmit rate by fractions of a ppm with no bypass and no re-lock, which
would be true clock following with no resampler. **The RM neither sanctions nor
forbids it.** See [§What could not be established](#what-could-not-be-established).

### Asynchronous resampling in vendor code is the expensive way

Adopting the newer PJRC/Bösing `input_spdif3` costs:

- **The pinned snapshot.** It is compiled directly and its byte-identity to the
  framework copy is a checked claim. Replacing it moves `PATCHED_SHA256` and the
  source-contract suite.
- **The wrong side of the chain.** The conversion lands on the *input*, before
  classification, so it would resample the IEC 61937 word stream itself. **That
  is not merely lossy, it is destructive** — a DTS burst is a bitstream, and
  resampling it as audio destroys it. It would have to be bypassed for DTS anyway.
- **The test contract.** Its filter is not designed by us, not host-tested by us,
  and not bit-exact under our `-ffp-contract=off` discipline.

**Rejected.** Recorded so the rejection is on the record rather than an omission.

## Verified: the resampling burden is PCM-only, and narrower still

Checked against `lib/aius_iec61937/` and `lib/aius_pipeline/`.

- **DTS branch** — words are de-framed into bursts and each payload is a raw DTS
  Core access unit. **The parser contains no rate at all.** The decoded rate
  comes from the bitstream header, can legally be any of nine values including
  44,100, and equals the header rate exactly because `synth_x96` is 0. **Already
  runtime data.**
- **PCM branch** — words are treated as interleaved L/R and stamped
  `AIUS_PIPELINE_PCM_SAMPLE_RATE_HZ`. **Hardcoded 48,000, unconditionally.**

**One correction to the premise, and it matters.** For a *conforming* IEC 61937
stream the carrier rate and the decoded rate are equal by construction: the
type-I burst repetition period is 512 IEC frames and a DTS core frame with
`npcmblocks = 16` decodes to 16 × 32 = 512 samples per channel. So a 44.1 kHz DTS
stream arrives on a 44.1 kHz carrier and decodes to 44.1 kHz PCM. **A DTS stream
whose header rate differs from its carrier rate is non-conforming and should be
detected and refused, not silently resampled.**

| Branch | Rates presentable | Frames per block |
|---|---|---|
| PCM passthrough | whatever the carrier is; RM §40.3.1.6 gives full compliance 12–96 kHz | 512 fixed |
| DTS Core | 8, 11.025, 12, 16, 22.05, 24, 32, 44.1, 48 kHz — in practice 44.1 and 48 | `npcmblocks × 32`; 512 for type I |
| Replay | 48 kHz only, implied by `AIUS_REPLAY_WORDS_PER_SECOND` | 512 |

**Consequence, exactly as hoped.** The overwhelmingly common path — 48 kHz PCM or
48 kHz DTS — stays at unity and must remain **bit-identical to today**, which a
single `if (stream_rate == engine_rate)` guarantees and which preserves every
committed golden vector for free. The resampler is an occasional path whose cost
lands only when a non-native source is connected.

## The structural change nobody had named

**Upsampling alone fixes nothing, because the drain side is still clocked by the
input.**

`emit_block()` drains a fixed 128 frames per engine block
(`src/optical_main.cpp:926`) and is called from `AudioStream::update_all()`,
which is called by whichever peripheral holds `update_responsibility` — today the
input, at the carrier's rate. Feed a 44.1 kHz source through a perfect
44.1→48 kHz upsampler and the ring fills at 48,000 frames/s while draining at
44,100: **it overflows by 3,900 frames/s.**

**The transmitter must become the permanent clock master.** Its DMA is clocked by
PLL4 through `SPDIF0_CLK_ROOT`, not by the receiver, and fires once per
`AUDIO_BLOCK_SAMPLES` frames regardless of whether a carrier exists — the
argument `firmware-io.md` already makes for the replay handover. So
`emit_block()` runs at exactly 48000/128 = 375 Hz always. The input becomes a
pure producer: `AudioInputSPDIF3::update()` is polled at 375 Hz while its DMA
delivers at 344.5 Hz, so 8.1 % of polls take the "DMA didn't fill blocks" arm
(`input_spdif3.cpp:220`), keeping its blocks and transmitting nothing. **No data
is lost** — the DMA keeps filling the same block — and the receiver's two-block
staging plus the 2,048-frame ring absorb the jitter.

The change is to `replay_clock_service()`: make `output_drives` true whenever the
stream rate differs from the engine rate, instead of only during replay. It
reuses the `AiusSpdifInputClock` / `AiusSpdifOutputClock` protected-static shims,
the single critical section, and the once-per-epoch reconciler that already exist
and already run on hardware. **The cheapest structural change in this plan and
the load-bearing one.**

## Cost — the spine

Baseline: **398,591–431,995 cycles of 6,400,000** for a 512-frame block at
48 kHz, 6.2–6.8 % used, ~93 % free. At a 44.1 kHz source the block period is
512/44,100 = 11.61 ms = **6,966,000 cycles**, so the same DSP is 5.7–6.2 % and
the denominator is 8.9 % larger. All resampler figures are against 6,966,000.

### Filter design

Ratio 48,000/44,100 = **160/147** exactly (gcd 300). Polyphase
interpolate-by-160, decimate-by-147; prototype designed at 7.056 MHz.

| Stopband `A` | Total taps | Taps/phase | Image rejection versus |
|---|---|---|---|
| 90 dB | 18,289 | 114 | below the 16-bit floor |
| 100 dB | 20,517 | **128** | comfortably below the 16-bit floor |
| 120 dB | 24,974 | 156 | — |
| 145 dB | 30,323 | 190 | below a 24-bit floor |

**128 taps/phase is the design point.** Passband ripple at that attenuation is
**±0.0001 dB** — two orders better than the RBJ biquads already in the chain.
Being linear it adds **no harmonic distortion**; THD+N is set entirely by the
final requantiser. **192 taps/phase buys ~145 dB for 1.5× the cycles and
memory**, and that trade should be revisited once the 24-bit path lands.

Per output sample per channel: 128 MACs. At 48,000 output frames/s stereo,
**12.29 M MAC/s**.

### Arithmetic width, and why integer wins outright

| Representation | Cycles/MAC (est.) | Cycles/block | % of 6,966,000 | Host/target bit-exact? |
|---|---|---|---|---|
| `double`, `-ffp-contract=off` | ~5 | 713,500 | **10.2 %** | yes, by the existing discipline |
| `float` + VFMA | ~1.25 | 178,400 | 2.6 % | only if contraction is pinned identically both sides |
| **`int32` Q31, `int64` accumulate** | ~2 | **285,400** | **4.1 %** | **yes, by construction** |

**Recommend `int32` Q31 with 64-bit accumulation.** Integer arithmetic is
bit-identical host-to-target *by definition* — no `-ffp-contract` question, no
FMA question, no libm. It is cheaper than the double path the rest of the DSP
uses, and it composes natively with the 24-bit domain: `int24 × Q31` = 55 bits,
plus 7 for 128 accumulations = 62 bits, inside `int64`, then `>> 31` and saturate.

**The coefficient table is generated offline, not computed on target.** A
checked-in generator emits a C table, exactly as `scripts/build_config.py`
already emits `aius_optical_config.h`. The firmware contains no filter-design
mathematics — it contains a committed artifact.

### Memory, by bank

| Item | Bytes | Bank |
|---|---|---|
| Coefficients, 128 × 160 × `int32` | 81,920 | `DMAMEM` (OCRAM) |
| Delay lines, 128 × 2 ch × `int32` | 1,024 | `DMAMEM` |
| Output staging, 640 × 2 ch × `int32` | 5,120 | `DMAMEM` |
| Phase/accumulator/control state | < 64 | `DMAMEM` |
| **Total** | **~88 kB of ~310 kB free OCRAM** | |
| **DTCM cost** | **0** | |

Two placement warnings:

- **A plain `static const` table would land in DTCM.** The Teensy 4 linker maps
  `.rodata` to DTCM. 82 kB into a bank with 9,248 bytes free is fatal. The table
  must be explicitly `DMAMEM`.
- **FLASH placement was considered and rejected.** The phase steps by 147 mod
  160, so all 160 phases are touched within 160 output samples — an 82 kB working
  set against a 32 kB FlexSPI cache. Thrashing risk with no upside.

### Drift, and the two variants that differ over it

A fixed 160/147 ratio converts at the *nominal* ratio. The source runs at
44,100 × (1+ε) and our transmitter at exactly 48,000 nominal, so the ring level
drifts at 48,000 · ε frames/s:

| Source tolerance | Drift | Ring endurance at half-full (1,024 frames) |
|---|---|---|
| ±20 ppm | 0.96 frames/s | ~1,067 s |
| ±100 ppm | 4.8 frames/s | ~213 s |

The CM6206 loop now measures about **+437.5 ppm**, or 21 excess frames/s. Slice
2b's 8,192-word compressed ingress ring reaches full in about 137 seconds from
the observed starting level. Its larger capacity exposes the mismatch; it does
not correct it.

**Fixed-ratio therefore still needs drift correction** — one single-frame drop or
duplicate every 208 ms at 100 ppm. A single frame at 48 kHz is a 20.8 µs
discontinuity, inaudible at a zero crossing, but it is the *same defect class* as
today, merely 300× slower.

**Adaptive ratio removes it entirely for 2× the FIR cost.** Make the phase
increment a Q32 fractional value driven by a slow control loop on ring level.
Without interpolation the phase-quantisation error at 20 kHz is **−42 dB**,
unacceptable; with linear interpolation between adjacent phases it falls to
roughly −84 dB, at one extra coefficient set per output sample:

| Variant | Cycles/block | % of 6,966,000 | With existing DSP | Drift corrections |
|---|---|---|---|---|
| Fixed 160/147, 128 taps | 285,400 | 4.1 % | **9.8–10.3 %** | ~4.8/s at 100 ppm |
| **Adaptive, 128 taps + phase lerp** | 570,700 | 8.2 % | **13.9–14.4 %** | **none** |
| Adaptive, 192 taps + phase lerp | 856,000 | 12.3 % | 18.0–18.5 % | none |

Memory is unchanged across all three. 512-phase and 4,096-phase tables without
interpolation were costed and rejected: −52 dB and −70 dB, the latter needing
2 MB. A Farrow structure was rejected at 3–4× a single FIR.

**Host-testability survives adaptivity**, which is what makes it acceptable. The
resampler is a pure function `(input, phase increment, state) → output`. The
increment is an explicit parameter, so golden vectors pin it. The control loop
that *chooses* the increment is a separate pure state machine — the same shape as
the network supervisor oracle in `test/net/`.

### The costs the alternatives carry

| Mechanism | CPU | Memory | Vendor | Board | Validatable now | Drift |
|---|---|---|---|---|---|---|
| **Fixed 48 kHz + adaptive upsampler** | 8.2 % on non-native, **0 % at 48 kHz** | 88 kB OCRAM, 0 DTCM | **none** | none | **yes, 44.1 kHz replay fixture** | none |
| Fixed 48 kHz + fixed-ratio upsampler | 4.1 % | 88 kB OCRAM | none | none | yes | ~4.8/s |
| External recovered-clock slaving | **0** | **0** | none | **pin 40 → pin 16 trace** | no — needs a flying lead | **none, ever** |
| Runtime PLL retune per change | ~0 steady-state | ~0 | none | none | partly | none |
| Newer vendor input unit | unknown | unknown | **`PATCHED_SHA256` + contract suite** | none | no | none |

## Recommendation

**Adopt a fixed 48 kHz output carrier with an adaptive-ratio polyphase upsampler
on non-native sources.** `int32` Q31, 128 taps/phase, 160 phases with linear
phase interpolation, table generated offline into `DMAMEM`, sited after the
downmix and before the output EQ, bypassed exactly at unity, with the transmitter
as permanent clock master.

**8.2 % of the per-block budget and 88 kB of OCRAM, on a path that engages only
when a non-48 kHz source is connected. Zero DTCM. Zero vendor change. No PLL
retune, no mute on the steady-state path, no receiver re-lock.**

1. **It is rate-following, achieved digitally.** The output *carrier* stays at
   exactly 48,000 — already verified on the board — while the output *sample
   stream* is generated at exactly the rate the input delivers, in the long-run
   average, because the control loop locks the phase increment to the measured
   input. The two goals are not in tension; this is their reconciliation.
2. **The common path is bit-identical to today.** One `if` at unity preserves
   every committed golden vector and every measured cycle count. No other option
   can say that.
3. **It costs no board change and can be proven on this board.**
4. **It needs no vendor movement.**
5. **It handles every case with one mechanism** — PCM at any rate, DTS at any
   header rate, replay, and no-carrier.
6. **It nearly dissolves the `static_assert` problem.**

**What would overturn it**, in priority order:

- ~~**A live `PLL_AUDIO_NUM` trim proves glitch-free on the device.**~~
  **Confirmed for carrier continuity on 2026-07-29.** A tagged one-shot
  +438 ppm numerator write after receiver lock produced no carrier event,
  reclassification, reset or capture-process failure. On the same CM6206 loop,
  ingress slope changed from +42.667 words/s untrimmed to +0.200 words/s over
  185.1 seconds, output committed at 48,021.783 frames/s against the measured
  48,017–48,021 Hz source, and ingress HWM stayed 256 words with zero drops.
  The write boundary was not sampled for waveform continuity, so the production
  transition still mutes one block. The phase-3 drift actuator is therefore
  PLL4 trim; adaptive FIR remains the fallback for a non-native nominal rate
  until a whole-rate retune is separately proven.
- **Measured cycles/MAC comes in at 4+ rather than 2.** Then adaptive costs
  ~16 % and fixed-ratio-plus-PLL-trim becomes clearly preferable.
- **The production board accepts the pin 40 → pin 16 trace.** Then hardware
  slaving is strictly better for PCM passthrough at zero CPU. **Recommend routing
  that trace regardless** — it is one trace, costs nothing to have, and keeps the
  option open. It needs a decision against the expansion allocation in
  [`aius-hardware-spec.md`](aius-hardware-spec.md); pin 16 is half a UART pair
  and pin 40 is an ADC.

## The two hardware questions, and why they do not block

Both open questions — whether a live `PLL_AUDIO_NUM` trim is glitch-free, and
whether the production board accepts the pin 40 → pin 16 trace — need hardware
answers that are not available yet. **Neither blocks implementation, and the
difference is not material to what gets built next.** Recorded 2026-07-26 so the
reasoning is not re-derived.

**Phases 1 and 2 are common substrate under every option.** Making the rate
runtime data, counting the underrun, publishing `ratio_ppm`, refusing an
unsupported rate, the offline coefficient generator, the Q31 polyphase kernel,
the placement between downmix and output EQ, and the transmitter becoming
permanent clock master — none of that changes whichever way the two hardware
questions resolve. So build them.

**Where the options actually diverge is one thing: what actuates the drift
correction.** Every candidate needs the same measurement (ring level trend) and
the same slow control loop; they differ only in what the loop pushes on:

| Future | Actuator | Cost |
|---|---|---|
| Fixed-ratio, digital correction | drop or duplicate one frame | ~4.8 corrections/s at 100 ppm |
| Adaptive ratio | the Q32 phase increment | +4.1 % of block budget, no corrections |
| PLL trim (if the trim is glitch-free) | `CCM_ANALOG_PLL_AUDIO_NUM` | ~0 %, no corrections, literal clock-following |

### The seam: `AiusClockCorrector`

**One interface, at the actuator, because that is the only place the futures
differ.** Shape it as the plan's other seams are shaped — a small vtable the
composition root wires, with the control loop above it and the actuator below:

```c
typedef struct {
    void *context;
    /* Signed parts-per-million the output is running fast (+) or slow (-)
     * relative to the source, computed by the shared control loop. */
    void (*apply)(void *context, int32_t error_ppm);
    /* Observable, for the `clock` object; a corrector with no discrete
     * corrections reports 0 forever. */
    uint32_t (*corrections)(const void *context);
} AiusClockCorrector;
```

Three implementations become possible without touching the loop, the kernel or
the reporting surface: a frame drop/duplicate corrector (Phase 2), a phase-increment
corrector (Phase 3), and a PLL-trim corrector (Phase 3b, if the measurement
permits). **Two of those are concrete needs today** — Phase 2 ships the first and
Phase 3 needs the second regardless of how the PLL question resolves — so this
clears the seam-arity rule rather than being a hypothetical.

### Why the FIR kernel needs no seam

The adaptive kernel is **additive to the fixed-ratio kernel, not a replacement**:
same 160-phase table, same MAC structure, same Q31 arithmetic. Adaptive adds a
Q32 phase accumulator, a second coefficient read and a lerp between adjacent
phases. So there is nothing to abstract — Phase 3 extends Phase 2's kernel in
place, behind the existing bypass-at-unity guard.

Putting a seam in the hot loop to hide that would cost an indirect call per output
sample for no benefit, which §Deciding principle rejects outright.

### Why the pin 40 → pin 16 question needs no seam either

`STC[TxClk_Source]` is already a runtime register write, and Phase 4 already
specifies the mandatory PLL4 fallback for the DPLL-unlocked case. Selecting the
clock source is therefore configuration, not architecture. **The only thing that
would foreclose the option is designing it out**, so:

- Do not assume the transmit clock is always PLL4 in any new code; read it from
  the clock-source state.
- Keep the Phase 4 note in the hardware plan live so the trace is considered
  before the production layout is released, per §Hardware release gates.

### The path selected by the live measurement

**Phase 1, then Phase 2 with the frame drop/duplicate corrector behind the seam.**
That is the most likely path on the evidence: it is the common substrate and is
fully provable on this board. Replay still has zero drift because its producer
and consumer share one crystal; the CM6206 optical loop now supplies the
independent-clock hardware proof and makes the drift corrector directly
testable.

The deferred choice is now resolved for same-nominal input: Phase 3 uses the
PLL-trim corrector. The adaptive phase-increment implementation remains the
fallback for a non-native nominal rate until a live whole-rate PLL retune is
proven. Both still plug into the same seam.

## Placement, and why it costs no DTCM

```text
decoded block (N <= 512 frames @ fs_src, up to 6 planes)
  -> input trim + input EQ          at fs_src   (rate-dependent, optional)
  -> downmix 6 -> 2                 rate-free
  -> RESAMPLE fs_src -> 48,000      2 channels only, bypassed at unity
  -> output EQ                      at exactly 48,000, always
  -> true-peak limiter              at exactly 48,000, always
  -> requantise + write ring
```

- **`enqueue()`'s 512-frame cap is untouched.** The cap tests the *source* block.
  A 512-frame 44.1 kHz block upsamples to 557.4 frames — over the cap — so a
  resampler placed *before* the cap would force it up. Raising it to 640 would
  cost 1,024 more bytes of `enqueue()` stack and 3,072 more of DTCM `.bss` —
  **4 kB out of a 9,248-byte zone, for nothing.** This is the single largest cost
  avoided by placement.
- **Only two channels are resampled, not six.** Before the downmix it would be
  36.9 M MAC/s, ~3× the cost.
- **Everything after the resampler runs at exactly 48,000 forever**, so the
  output EQ, limiter and meter window never need re-deriving on a rate change.
  Only the *input* EQ is rate-dependent, and it is an optional feature.
- **The 2,048-frame ring absorbs the variable output length.**
- **`emit_block()` was rejected as a site** — it would put a per-sample
  fractional interpolator in the tightest ISR and make the ring hold source-rate
  audio, destroying the invariant that makes ring level a drift signal.

**Cost of the split.** `aius_dsp_process()` needs a hook between downmix and EQ.
`lib/aius_dsp/` is under concurrent edit for dither, headroom and the true-peak
limiter — **sequence this after those three.**

**A synergy worth banking.** The true-peak limiter needs 4× oversampling. A
polyphase interpolator is exactly that infrastructure. Build the resampler so the
limiter's oversampler is the same kernel with `L = 4, M = 1`.

## Interaction with the 24-bit output path

**The overlap is favourable in every direction.**

**Word width — no conflict, by construction.** The resampler is `int32` in,
`int64` accumulate, `int32` out, all at the 24-bit scale. It is **natively
24-bit** and needs no change when the output widens. **A 16-bit resampler feeding
a 24-bit transmit path would waste both changes; this one does not.** The
`>> 8`-and-clamp and the `int16` ring are the two things that must widen, and
they belong to the 24-bit change.

**Sequence 24-bit first**, so the resampler is written once against the final
ring format.

**Bypassing `audio_block_t` helps the observability requirement decisively.**
Once the final hop is ours: the transmit queue occupancy **is** our ring level; an
underflow **is** our counter, not a silent `block_silent` substitution; and **the
drop-or-repeat behaviour ceases to exist as vendor behaviour** — there is no
two-deep queue to overflow. That converts §Observability from "reach into vendor
state" into "read our own ring", and argues for doing 24-bit first.

**A route that works today, before that lands.** `block_left_1st` /
`block_left_2nd` are `protected static` on `AudioOutputSPDIF3`, so a
private-inheritance shim can read them — **exactly the trick already shipped for
`update_responsibility`**. Three lines, no vendor patch, and transmit queue
occupancy becomes observable in Phase 1.

## Handling a rate change safely

### Detection

**Prefer the stream over the carrier.** `block->sample_rate_hz` is what the
samples in hand actually are.

**The carrier measurement is sound and RM-confirmed.** `sampleRate()` computes
`F_BUS_ACTUAL × SRFM / (0x8000000 × dpll_Gain())`. RM §40.3.1.8 gives
`FreqMeas[23:0] = FreqMeas_CLK / BUS_CLK × 2^10 × GAIN` and §40.3.1.7 gives
`FreqMeas_CLK = 128 × fs`, so with `GAIN = 8 × 2^10`, `fs = BUS_CLK × SRFM /
2^30` — **algebraically identical to the vendor expression.** One SRFM LSB is
`150 MHz / 2^30` = **0.14 Hz**, so resolution is not the limitation; filter
settling and jitter are. It returns 0 when `!pllLocked()`.

**Snap and reject.** Map to the nearest member of `{32000, 44100, 48000}` within
±2 %. Anything outside is `unsupported`, not a rate.

### Debounce

**In thread context at checkpoint 7, beside `replay_clock_service()`, never in
the ISR.** That keeps `pllLocked()` out of the audio path and the ITCM allowlist
at exactly three sections.

- Require **8 consecutive agreeing samples** with `pllLocked()` true throughout.
  At ~672 epochs/s that is ~12 ms — shorter than any audible mute, longer than a
  flap.
- **Carrier loss holds the last rate and mutes.** `sampleRate()` returning 0 is
  not a rate change.
- Rate-limit accepted changes to one per second.

### Apply

**At the existing block boundary, through the existing seam.** `enqueue()`
already consumes a staged model at the top of the block callback via one
`volatile` flag. Add a pending-rate alongside it. **Reuse, do not invent** — that
seam is proven and its race argument is already written down.

| Item | Recomputed? | Why |
|---|---|---|
| Input-EQ biquads | **yes** | They run at `fs_src`. |
| Output-EQ biquads | **no** | Post-resampler, always 48,000. |
| Limiter coefficients | **no** | Post-resampler; self-derives anyway. |
| Meter window and µs/frame | **no** | Post-resampler; counted in frames. |
| Resampler phase increment | **yes** | Reset to the new nominal. |
| Resampler delay lines | **yes** | Zeroed — they hold old-rate samples. |
| Biquad state | **yes** | Already zeroed by `apply_config()`. |

**Only the optional input EQ is rate-dependent.** That is the payoff of the
chosen placement.

### Muting across the transition

**Mute for one block, and only one.** The output carrier never changes, so no
receiver re-lock is required. `mute_PCM(true)`, apply at the boundary,
`mute_PCM(false)` next block. **Cost: one 10.7 ms block of silence per rate
change**, against a source-switch transient that is invariably longer.

### The replay path

A replay file is a raw IEC word stream with **no header and therefore no declared
rate**. Under this design a replay is already at the engine rate, takes the unity
path, and nothing changes.

- **A non-48 kHz replay file would report a wrong duration**, silently.
- **Supporting one requires the rate to become selection metadata.**

Out of scope, **except** that a 44.1 kHz replay fixture is what makes Phase 2
testable on this board. Keep that tag in the fixture manifest, not the replay
service.

## Observability

**Publish the relationship as a first-class object** — a `clock` object on
`GET /api/status`:

```json
{
  "carrier_locked": true,
  "carrier_rate_hz": 44100,
  "stream_rate_hz": 44100,
  "engine_rate_hz": 48000,
  "ratio_ppm": -81250,
  "clock_master": "output",
  "resampler": { "active": true, "taps": 128, "corrections": 0 },
  "ring": { "frames": 1021, "min": 998, "max": 1049 },
  "mismatch_blocks": 0,
  "unsupported_rate_blocks": 0,
  "underruns": 0
}
```

**`ratio_ppm` is the field that would have caught the original defect.** 44,100
against 48,000 is −81,250 ppm; no plausible threshold misses it.

- **`underruns` — free, exact, available today.** `emit_block()` already knows
  when the ring is empty and zero-fills. One line in that `else` arm. **Do this
  in Phase 1 regardless of everything else.**
- **Transmit queue occupancy — available today via the shim** described above.
- **Dropped and repeated blocks — not directly countable today, and say so.**
  Both are vendor-internal and reached from the transmit ISR. Patching the
  retained snapshot costs `PATCHED_SHA256` and the contract suite for a metric the
  24-bit path makes free. **Serve them as absent, state that they are absent, and
  use ring level plus `underruns` as the proxies.**
- **`ring.min` / `ring.max` — the drift signal.** A monotonic trend in ring level
  *is* clock drift, and the adaptive loop already computes it.

**Events.** Append `AIUS_EVENT_RATE_CHANGE = 11` and
`AIUS_EVENT_RATE_UNSUPPORTED = 12`, so no existing value moves — the same
discipline `REPLAY_STOP_FAULT` followed.

**UDP.** Adding the engine rate and a mismatch counter needs the reserved bytes
at 88..90, which the runner currently rejects as non-zero — so it is a
coordinated payload version bump across `lib/aius_net` and `aius_net_runner.py`,
not a free extension.

## What replaces the static_assert

**Mostly nothing, and that is a genuine argument for the recommendation.**

Under a fixed 48 kHz output the engine clock and the generated DSP rate **both
remain compile-time constants and both remain equal**, so the assertion stays
correct and stays. It is not retired.

**What it never covered is the hole that must be closed.** The file's own comment
says it: nothing compares `block->sample_rate_hz` against either. So the
replacement is an addition:

**A runtime three-way reconciliation in `enqueue()`, at the block boundary:**

1. `block->sample_rate_hz == engine_rate` → unity path, bit-identical to today.
2. Differs and is a supported ratio → resample; count the block.
3. Differs and is unsupported → increment `unsupported_rate_blocks`, mute, emit
   `AIUS_EVENT_RATE_UNSUPPORTED`, and **do not process the block**.

Arm 3 is what turns an inaudible 8 % block loss into a reported refusal.

**Three compile-time guards to add:**

```c
static_assert(AIUS_RESAMPLE_MAX_OUT_FRAMES >=
    (AUDIO_BLOCK_SAMPLES * 4 * AIUS_RESAMPLE_L + AIUS_RESAMPLE_M - 1) / AIUS_RESAMPLE_M,
    "resampler output staging must hold the largest block enqueue() accepts");

static_assert(AIUS_REPLAY_WORDS_PER_SECOND == AIUS_OPTICAL_SAMPLE_RATE_HZ,
    "the replay seam is clocked by the engine rate");
static_assert(AIUS_REPLAY_BYTES_PER_SECOND == AIUS_REPLAY_WORDS_PER_SECOND * 4u,
    "four bytes per stereo frame");
```

`192000` and `48000` are currently independent literals, and `duration_seconds`
divides by one of them. Tying all of it to the one generated constant applies the
existing assert's discipline to the seam it missed.

**And retire one thing outright.** `AIUS_PIPELINE_PCM_SAMPLE_RATE_HZ` must be
deleted, not widened. A constant that claims to be the passthrough rate and is
not is worse than no constant; the rate becomes a field the pipeline is told.

## Phases

**Sequence after the three in-flight audio-quality fixes and after the 24-bit
output path.**

### Phase 1 — make the rate data, and make the mismatch loud

No resampler. The increment that makes 44.1 kHz *recognised, reported and cleanly
refused*.

1. Delete `AIUS_PIPELINE_PCM_SAMPLE_RATE_HZ`; stamp the measured carrier rate.
2. Detection, snapping and debounce as a pure state machine in a new `clock` host
   suite, driven at checkpoint 7.
3. `apply_config()` sources its rate from the stream.
4. The runtime three-way in `enqueue()`, arm 2 unimplemented.
5. **Count `emit_block()`'s underrun.**
6. The transmit-queue-occupancy shim.
7. The `clock` object, `ratio_ppm`, and the two new event types.
8. The three new `static_assert`s; widen the three suites that pin 48,000.

**Provable on this board, no carrier needed.** After Phase 1 a 44.1 kHz source is
muted and reported instead of stuttering silently.

### Phase 2 — fixed-ratio upsampling, 44.1 kHz plays correctly

1. Offline coefficient generator plus its suite.
2. The polyphase kernel, `int32`/`int64`, with golden vectors.
3. Split `aius_dsp_process()` and site the resampler.
4. Make the transmitter permanent clock master.
5. Enable arm 2 for 44,100.
6. Ring-level drift reporting, and the `AiusClockCorrector` seam with its first
   implementation: drop or duplicate one frame, counted and rate-limited.

**Provable via a 44.1 kHz replay fixture and the CM6206 optical loop.** Both
clocks come from the same crystal on replay, so replay drift is identically
zero. The live loop provides the asynchronous case and must show non-zero
corrections without ingress growth or unexplained drops.

### Phase 3 — eliminate drift with the measured PLL actuator

The control loop is common and the live 2026-07-29 measurement selects the
PLL-trim corrector for same-nominal input. The adaptive corrector remains the
non-native-rate fallback. Both plug into the seam Phase 2 established.

1. The control loop on ring level, as a separately tested pure state machine.
2. **3b — the selected same-nominal path:** a bounded PLL-trim corrector writes
   `CCM_ANALOG_PLL_AUDIO_NUM`, at ~0 % and delivering literal clock-following.
   Mute the transition for one block because the measurement proved carrier
   continuity but did not capture the trim boundary's waveform.
3. **3a — retained non-native fallback:** a phase-increment corrector uses a Q32
   phase accumulator plus linear interpolation between adjacent phases,
   extending Phase 2's kernel in place at +4.1 % of block budget.
4. Extend the supported set to 32 kHz.

3b is strictly better for the measured 48 kHz case: no FIR cost and no discrete
corrections. 3a remains the path that needs no whole-rate PLL assumption.

**The CM6206 optical loop can validate either on this board.** Two independent
clocks are the entire point, and the loop now reproduces their relative drift.

### Phase 4 — the hardware option, if the board accepts it

1. Route pin 40 to pin 16 on the production board.
2. `STC[TxClk_Source] = 011b` with `TxClk_DF = 1` when the DPLL is locked;
   `001b` when not. **The fallback is mandatory**: with the DPLL unlocked,
   `SRPC[ClkSrc_Sel] = 1` makes the recovered clock fall back to `tx_clk`, and
   feeding that into `EXT_CLK` is a self-referential loop.
3. PCM passthrough then runs at zero CPU with no drift.

## What needs the device versus what can be proven here

**Provable on this board today:** every host suite; the unity path's bit-identity
via the existing corpora; the full conversion path via a 44.1 kHz replay fixture;
the clock-master inversion (already proven during replay); all counters and
`static_assert` changes; and the resampler's real per-block cost via the existing
DWT bracket.

**Needs live hardware:** carrier detection and debounce against a real flapping
source; whether a live `PLL_AUDIO_NUM` trim is glitch-free; the Phase 4
recovered-clock loopback; any downstream receiver's behaviour on a carrier
interruption. The CM6206 loop supplies the ordinary asynchronous source/sink
case on this board.

## What could not be established

1. ~~**Whether a live `CCM_ANALOG_PLL_AUDIO_NUM` write preserves the
   carrier.**~~ **Resolved 2026-07-29:** the +438 ppm live write kept the
   CM6206 capture path and carrier locked and eliminated measured ring drift.
   Waveform continuity exactly at the write boundary was not captured, so the
   production transition remains one-block muted.
2. **Actual cycles per MAC for an `int32` Q31 FIR reading coefficients from
   OCRAM.** 2 cyc/MAC is an estimate; OCRAM latency could make it 3–4. **Every
   figure in §Cost scales linearly on it.** Measure before committing Phase 2.
3. **Whether `SPDIF_SR_CLK` on pin 40 emits 128·fs, and whether it follows
   `SRPC[ClkSrc_Sel]`'s unlocked fallback.** The RM does not state whether the pad
   carries the raw DPLL clock or the mux output. Phase 4 depends on it.
4. **Whether real 44.1 kHz DTS presents 512-frame blocks.** `npcmsamples` is a
   bitstream field; 512 is the type-I case but not guaranteed. There is **no
   44.1 kHz DTS vector in `tools/dts_vectors/`**.
5. **The measured jitter and settling time of `SRFM`.** It sets the debounce
   window, currently a guess of 8 samples.
6. ~~**The live passthrough path has never executed at all.**~~ **Resolved
   2026-07-29:** the CM6206 loop ran live PCM and DTS through the optical input
   and exposed the +437.5 ppm same-nominal drift described above.
7. **The cost of one `fnet_poll` against the widened block budget.** Already open
   in `firmware-io.md`.

## Related documents

- [Aius network and firmware recovery hardening](aius-network-hardening.md) —
  §Deciding principle, the UDP payload contract, the DTCM measurement history.
- [Aius firmware I/O reference](../reference/firmware-io.md) — the buffers, banks
  and execution contexts this design spends.
- [Aius DSP config API](aius-dsp-config-api.md) — §Audio-quality roadmap, which
  schedules the 24-bit output path this design composes with.
- [Aius hardware specification](aius-hardware-spec.md) — the pin allocation
  Phase 4 needs a decision against.
