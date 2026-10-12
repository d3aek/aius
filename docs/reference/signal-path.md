---
title: Aius audio signal path
type: reference
scope: component
component: aius
status: current
tags: [aius, firmware, teensy, audio, dsp, signal-path, dither, limiter]
updated: 2026-08-04
toc: true
---

# Aius audio signal path

What the audio path of the `teensy41-optical` image actually is: every stage from the
optical receiver to the S/PDIF transmit register, the module and function that
implements it, its buffers and their memory bank, its arithmetic domain, and its share
of the per-block time budget. It is the companion to
[`firmware-io.md`](firmware-io.md), which answers "what I/O exists and why it cannot
disturb the audio path"; this answers what the audio path *is*, and does not restate
the isolation argument.

It is also the living record the audio-quality roadmap in
[`aius-dsp-config-api.md` §Audio-quality roadmap](../plans/aius-dsp-config-api.md#audio-quality-roadmap--accepted-2026-07-26)
attaches to: **every item on that roadmap updates this document in the commit that
ships the change.**

## How to read this

**Every claim carries a `path:line` citation, and every number says where it came
from.** Firmware paths are relative to `firmware/` unless the path
says otherwise. Line numbers are correct as of this revision; the cited symbol name
is the durable handle. The 2026-08-04 audit re-verified the structural claims against
the then-current tree (E-AC-3 route, one-quantize chain, LFE fold, decorrelator,
gate-open trim); ELF-measured byte and cycle figures still name the link they were
read from and have not been re-measured since.

**Numbers come from one of three places, and the text always says which:**

- **From the linked image** — read out of
  `.pio/build/teensy41-optical/firmware.elf`. The historical tables name their
  revision; the current sample-rate-following placement figures name their
  2026-07-29 target link. `tools/memory/mem_report.py --elf` and the post-link verifiers are the
  authorities, not prose copied from an earlier image.
- **From the device** — the DWT cycle counters the audio path publishes through
  `GET /api/meters`, recorded in the roadmap against build revision `f76baa87c67d`,
  and the loopback quality figures in
  [§Measured output quality](#measured-output-quality--the-loopback-figures), read from
  `GET /api/measure` against build revision `b5f9a1351adf`.
- **Derived from source** — a struct size or a period computed from constants that are
  cited. Marked *derived* where it matters.

**Anything not measured is labelled.** [§Figures that are not measured](#figures-that-are-not-measured)
collects every one in a single list, so a reader can see the edge of the evidence
without reading the whole document.

**Bank vocabulary** matches [`firmware-io.md` §How to read this](firmware-io.md#how-to-read-this):
`FLASH` is program flash, `ITCM`/`DTCM` are the two halves of the 512 KiB FlexRAM, and
`OCRAM` is RAM2 holding both `DMAMEM` statics and the `malloc`/`new` heap.

## The three things called a block

**Conflating these is how the 44.1 kHz defect hid for months**, so they are stated
first. `optical_main.cpp:1160` carries the same distinction beside the code.

- **128 frames — the engine block.** `AUDIO_BLOCK_SAMPLES`, the Teensy core default,
  which this build does not override. It sizes both DMA buffers, the `audio_block_t`
  pool, the exact ingress chunk and the transmit interrupt's drain of the 24-bit
  ring. `AiusAudioProcessor::update()` therefore runs at the active rate divided
  by 128, **375 times a second at 48 kHz** (`src/optical_main.cpp:1321`), and
  publishes 256 IEC words = 512 bytes each time.
- **512 frames — the decoded block, and the one the budget belongs to.** A DTS Core
  access unit with `npcmblocks = 16` reconstructs to 16 × 32 = 512 samples per channel
  (`vendor/dcadec-core/core_decoder.c:962`), and PCM passthrough stages the same 512
  (`AIUS_PIPELINE_PCM_BLOCK_FRAMES`, `lib/aius_pipeline/include/aius_pipeline.h:135`).
  On a continuous stream the AUDIO service therefore reaches `enqueue()` once per
  four published chunks, at the active rate divided by 512 (**93.75 blocks per
  second at 48 kHz**). This is what the
  `blocks` counter counts and what the DWT bracket measures. The two newer
  compressed routes divide differently: an AC-3 access unit is emitted as six
  independent 256-frame blocks (`AIUS_A52_BLOCK_SAMPLES`,
  `lib/aius_a52/include/aius_a52.h:55`), and an E-AC-3 access unit
  (1–6 × 256 frames) is sliced on output to at most
  `AIUS_PIPELINE_MAX_OUTPUT_FRAMES` = 512
  (`lib/aius_pipeline/include/aius_pipeline.h:136`), so `blocks` counts 512-frame
  units on DTS/PCM and smaller units on the Dolby routes.
- **`queue_frames` in the meters — neither of the above.** It is
  `aius_spdif_tx24_level()`, the transmit ring's occupancy at the publish instant.
  Since the e2e-latency Phase 3 gate-open trim (2026-08-03), every closed→open TX-gate
  edge trims the ring down to `AIUS_TX_GATE_OPEN_TARGET_FRAMES`
  (`src/optical_main.cpp:1872`; code default 256, **the shipped image pins 128** via
  `platformio.ini:310`), and steady occupancy then re-forms through each session's own
  refill equilibrium — the measured mean moved only ~15 frames when the target halved,
  but the old 1,792-frame excursions are impossible by construction
  ([`aius-e2e-latency.md` §Phase 3](../plans/aius-e2e-latency.md#phase-3-tx-ring-gate-open-normalization-and-dhcp-schedule-visibility)).

**Three IEC 61937 families are supported end to end: DTS Core burst type I, AC-3, and
E-AC-3.** `enqueue()` rejects `block->frames > AUDIO_BLOCK_SAMPLES * 4` with
`AIUS_INVALID_ARG` (`src/optical_main.cpp:1467`), and that cap is exactly 512. The
parser de-frames data types 1 (AC-3), 11/12/13 (DTS Core burst types I/II/III) and 21
(E-AC-3) (`lib/aius_iec61937/include/aius_iec61937.h:34`); the pipeline routes Pc
family 1 to the AC-3 decoder, 11 to dcadec and 21 to the vendored E-AC-3 fork
(`lib/aius_pipeline/src/aius_pipeline.c:55`). A DTS type II or III burst decodes to
1,024 or 2,048 samples per channel and is still refused one stage later. The two
constants remain coupled: changing the engine block changes the decoder block this
file will accept.

## The chain

```mermaid
flowchart TD
    OPT["Optical TOSLINK in<br/>CORE_PIN15 / SPDIF_IN"]
    RX["AudioInputSPDIF3 receive DMA ISR<br/>spdif_rx_buffer, 2048 B OCRAM"]
    BLK["audio_block_t pair, int16<br/>AudioMemory(24) pool, OCRAM"]
    UPD["AiusAudioProcessor::update<br/>fs/128, ingress publish only"]
    ING["Audio ingress SPSC<br/>8192 words / 16 KiB OCRAM"]
    AUD["Runtime AUDIO checkpoint 9<br/>reset/flush + bounded drain/service"]
    CLS{"aius_pipeline_classify<br/>Pa/Pb preamble in 512 words?<br/>+ mid-flight re-type and starvation re-classify"}
    IEC["IEC 61937 de-frame<br/>aius_iec61937_push<br/>data types 1, 11/12/13, 21"]
    DTS["DTS Core decode<br/>aius_dts_decode_access_unit<br/>512-frame AU, 24-bit planar, up to 6 planes"]
    A52["AC-3 decode<br/>aius_a52 (identity-only liba52 fork)<br/>6 x 256-frame blocks per AU"]
    EAC3["E-AC-3 decode<br/>aius_eac3 (ffmpeg fork, flash-pinned)<br/>AU sliced to &le;512 frames"]
    PCM["PCM passthrough<br/>pcm_feed, 16 left-aligned onto 24-bit"]
    ENQ["AiusAudioProcessor::enqueue<br/>&le;512 frames per block<br/>DWT bracket opens"]
    STG["Stage to double scratch<br/>the one int32-to-double conversion<br/>+ input trim/EQ when configured"]
    DMX["Downmix matrix, 6 to 2, double accumulator<br/>aius_dsp_downmix<br/>LFE fold via 120 Hz LR4 low-pass<br/>surround decorrelation delay (config, default 0 ms)<br/>headroom 0.5 on summing layouts, 1.0 on mono and FL|FR"]
    OEQ["Output EQ cascade, L and R, double<br/>aius_dsp_eq_run"]
    LIM["True-peak safety limiter, double<br/>aius_dsp_limiter_run<br/>4x oversampled, 2 ms lookahead"]
    QNT["THE final quantize<br/>aius_quantize: round + saturate<br/>the chain's only rounding and clamp"]
    OUT["Output word width<br/>aius_out_stage_run<br/>24-bit: copy. 16-bit: TPDF dither"]
    RING["g_aius_spdif_tx24<br/>2 x 2048 int32, 16396 B DMAMEM<br/>trimmed to 128 frames at gate-open (shipped image)"]
    TXI["AiusSpdifTx24Hook::isr<br/>transmit DMA, fs/128"]
    TXB["SPDIF_tx_buffer, int32<br/>2048 B OCRAM, double-buffered"]
    WIRE["SPDIF_STL / SPDIF_STR<br/>CORE_PIN14, 24-bit<br/>live 32/44.1/48 kHz"]
    MET["AiusMeter taps<br/>accumulate + publish at 20 Hz<br/>never alters audio"]
    REC["PSRAM flight recorder + measure taps<br/>committed planes, PSRAM-fitted boards only"]

    OPT --> RX --> BLK --> UPD --> ING --> AUD
    AUD --> CLS
    CLS -->|"non-linear"| IEC
    IEC -->|"Pc 11"| DTS --> ENQ
    IEC -->|"Pc 1"| A52 --> ENQ
    IEC -->|"Pc 21"| EAC3 --> ENQ
    CLS -->|PCM| PCM --> ENQ
    ENQ --> STG --> DMX --> OEQ --> LIM --> QNT --> OUT --> RING
    RING --> TXI --> TXB --> WIRE
    STG -.-> MET
    LIM -.-> MET
    OUT -.-> MET
    OUT -.-> REC
```

The dotted edges are diagnostic taps, not audio. There is exactly one output — the
optical S/PDIF transmitter on `CORE_PIN14`. **There is no analogue output on this
board**; the acceptance criterion's "analogue and digital output" describes an intent
the hardware does not carry, and the wire is the last stage the firmware owns.

## Stage inventory

| # | Stage | Implementation | Arithmetic | Buffers, bank |
|---|---|---|---|---|
| 1 | Optical receive | `AudioInputSPDIF3::isr` | int32 subframe → int16 | `spdif_rx_buffer` 2,048 B OCRAM |
| 2a | Engine ingress publish | `AiusAudioProcessor::update` | int16 → uint16 words | direct to `g_audio_ingress_words`, 16,384 B OCRAM |
| 2b | Deferred audio service | `aius_audio_service_run` | integer control | bounded AUDIO checkpoint, cold service object |
| 3 | Classify + re-type | `aius_pipeline_classify`, re-type/starve counters | integer compare | `clsbuf[512]` 1,024 B, DTCM arena |
| 4a | IEC 61937 de-frame | `aius_iec61937_push` | integer | 4,096 B payload, DTCM arena |
| 4b | Compressed decode (one resident) | `aius_dts_decode_access_unit` / `aius_a52_*` / `aius_eac3_*` | codec-native → int32, 24-bit scale | 180,224 B codec slot, DTCM arena |
| 4c | PCM passthrough | `pcm_feed` | int16 → int32, left-aligned onto 24-bit | `pcm_l`/`pcm_r` 4,096 B, DTCM arena |
| 5 | Stage to double + input trim/EQ | `stage_input` → `aius_dsp_input_run` → `run_cascade` | int32 → double, then double | arena double staging, 32,768 B DTCM carve |
| 6 | Downmix matrix + LFE fold + decorrelation | `aius_dsp_downmix` | double | `AiusLfeFold` + `AiusSurroundDelay` state in `g_dsp_runtime` |
| 7 | Output EQ | `aius_dsp_eq_run` → `run_cascade` | double | `g_eq_state` in `g_dsp_runtime`, DTCM |
| 8 | True-peak limiter | `aius_dsp_limiter_run` | double | `AiusLimiter` ≈4,200 B DTCM (*derived*) |
| 9 | Final quantize | `aius_dsp_process` tail, `aius_quantize` | double → int32, round + saturate | writes the caller's output planes |
| 10 | Output word width | `aius_out_stage_run` | int32 copy (24-bit arm) | writes the ring in place |
| 11 | Transmit ring | `aius_spdif_tx24_stage`/`_commit` | int32, no arithmetic | `g_aius_spdif_tx24` 16,396 B OCRAM |
| 12 | Transmit interrupt | `AiusSpdifTx24Hook::isr` | int32 copy | `SPDIF_tx_buffer` 2,048 B OCRAM |

`run_cascade` is the shared core of stages 5 and 7, and `biquad_step`
(`lib/aius_dsp/src/aius_dsp.c:61`) is the one biquad recurrence shared by the input
EQ, the output EQ and the LFE crossover, so the three cannot diverge in arithmetic
order. **Stages 5 through 9 form the one-quantize chain (fixed 2026-07-28):** samples
travel as double from `stage_input`'s conversion until the single `aius_quantize`
after the limiter (`lib/aius_dsp/src/aius_dsp.c:1401`); no stage in between rounds or
clamps, so the limiter sees true unclipped peaks and the 24-bit wire ceiling exists
only at stage 9.

### Stage 1 — optical receive and the IEC 60958 words

`AudioInputSPDIF3`, compiled from the checked-in snapshot at
`evidence/source-snapshots/PJRC-SPDIF-RX-SOURCE/input_spdif3.cpp`, DMAs receiver words
into `spdif_rx_buffer[AUDIO_BLOCK_SAMPLES * 4]` (2,048 B, `DMAMEM`, 32-byte aligned,
`:37`) and its ISR narrows each subframe into an `audio_block_t`:

```c
*dest_left++  = (*src++) >> 8;   /* input_spdif3.cpp:118, dest_left is int16_t * */
*dest_right++ = (*src++) >> 8;
```

**That `>> 8` into an `int16_t` is the input-side ceiling** — see
[§Open at this revision](#open-at-this-revision). For a DTS carrier it is harmless: 16-bit
IEC 61937 packing puts the burst payload in the top 16 bits of the 24-bit subframe, so
`>> 8` extracts exactly the payload word. For a 24-bit linear-PCM carrier it discards
the low 8 bits before the DSP ever sees them.

The pool is `AudioMemory(24)` — 24 blocks of 260 B, 6,240 B in `DMAMEM`
(`src/optical_main.cpp:3638`, `cores/teensy4/AudioStream.h:110`). Two blocks are taken
and released per engine block pass.

### Stage 2 — ingress publish and deferred AUDIO service

`AiusAudioProcessor::update()` (`src/optical_main.cpp:1321`, **280 B of ITCM** in the
2026-07-29 target link) reserves one exact 256-word ingress slot and interleaves the
two `int16` planes directly into it. There is no intermediate stack array and no
classify, decode, DSP, meter, recorder or transmit-ring production in the software
audio ISR. It releases the two pool blocks after either committing the slot or
recording a bounded gap.

**Live input is the only audio source.** The SD replay source that once substituted
here was retired by
[`aius-sample-rate-following.md`](../plans/aius-sample-rate-following.md) Phase 4i
(2026-10-12).

At runtime AUDIO checkpoint 9, `aius_audio_service_run()` first applies any monotonic
reset or ingress-generation discontinuity, then drains at most 4,096 input words and
performs at most eight pipeline output steps inside the declared 8 ms allowance
(`lib/aius_audio_service/src/aius_audio_service.c:85`). Classification, decode,
`enqueue()`, DSP, meter publication, transmit commit, measurement capture and recorder
production all belong to this thread-context service. The transmit DMA ISR consumes
the committed ring independently.

### Stage 3 — classification

`aius_pipeline_classify` scans up to `AIUS_PIPELINE_CLASSIFY_MAX_WORDS` = 512 words for
the IEC 61937 preamble pair `Pa` = `0xF872`, `Pb` = `0x4E1F` in either wire byte order
(`lib/aius_pipeline/include/aius_pipeline.h:102`, `src/aius_pipeline.c:242`). A
preamble within the window means non-linear (`AIUS_PIPELINE_DTS`, which since the AC-3
and E-AC-3 routes names the whole IEC 61937 class); 512 words with no preamble means
linear PCM; fewer than 512 words and none found yet stays `AIUS_PIPELINE_UNKNOWN`.
Staging is the instance's `clsbuf[512]` (1,024 B).

**Classification is no longer once-per-lock; the pipeline self-heals both ways.** A
PCM-classified stream that starts carrying bursts re-types to non-linear when two
same-form preamble pairs arrive with plausible burst spacing
(`AIUS_PIPELINE_RETYPE_MIN/MAX_SPACING_WORDS`, 512–8,192,
`lib/aius_pipeline/include/aius_pipeline.h:120`); staged pass-through frames are
dropped at that instant so raw burst words never continue to the wire. The reverse
transition is starvation-driven: 16,384 words consumed by the non-linear route with no
parser burst outcome (`AIUS_PIPELINE_DTS_STARVE_WORDS`, `:132`) re-classifies the next
words. PCM emission is deliberately withheld for the first
`AIUS_PIPELINE_PCM_CONFIRM_WORDS` = 8,192 words while the re-type detector runs
(`:191`). Both transitions are counted (`retype_count`, `reclassify_count`, served as
`retype_count`/`reclassify_count` on `GET /api/status`).

The classification is what `GET /api/status`'s `state` renders: 2 unlocked or
unclassified, 3 PCM, 4 non-linear (`src/optical_main.cpp:1900`).

### Stage 4a — IEC 61937 de-framing

`aius_iec61937_push` unwraps one burst per call into the parser instance's fixed
`AIUS_IEC61937_MAX_PAYLOAD` = 4,096 B payload buffer, normalising to canonical
big-endian bytes regardless of wire byte order
(`lib/aius_iec61937/include/aius_iec61937.h:44`). Wire byte order is auto-detected per
burst from `Pa`; payload packing is create-time configuration and the optical image
selects `AIUS_IEC61937_PACK_16` (`src/optical_main.cpp:1782`).

A per-burst fault — error flag, invalid length, an over-long length code — is
non-fatal: the burst is skipped, the stream re-locks and the status is recorded
(`lib/aius_pipeline/include/aius_pipeline.h:105`). Nothing in the audio path stalls on
a corrupt burst.

### Stage 4b — compressed decode: DTS Core, AC-3, E-AC-3

**One decoder is resident at a time**, created lazily from the codec sub-arena on a
burst family's first completed burst and swapped on a family change
(`lib/aius_pipeline/src/aius_pipeline.c:55`, `:76`):

- **DTS Core (Pc 11/12/13).** `aius_dts_decode_access_unit`
  (`lib/aius_dts/src/aius_dts.c:53`) drives the vendored dcadec core with
  `DCADEC_FLAG_CORE_ONLY | DCADEC_FLAG_CORE_BIT_EXACT` and publishes decoder-owned
  planar PCM: `block->frames = core->npcmsamples`,
  `block->sample_rate_hz = core->output_rate`, planes in `AIUS_CH_*` order, tightly
  packed to `channels`. Layouts wider than 5.1 are refused with `AIUS_UNSUPPORTED`.
- **AC-3 (Pc 1).** `aius_a52_*` (`lib/aius_a52/include/aius_a52.h:21`) drives the
  identity-only liba52 fork: one call arms an access unit, six next-calls emit its
  256-frame blocks, all stamped with the accepted AU rate.
- **E-AC-3 (Pc 21, `-DAIUS_PIPELINE_EAC3=1`).** `aius_eac3_*`
  (`lib/aius_eac3/include/aius_eac3.h:20`) drives the vendored ffmpeg-eac3 fork
  (text and rodata flash-pinned by `link/aius_teensy41.ld`); an AU decodes to
  1–6 × 256 frames and the pipeline slices output to ≤ 512
  (`PENDING_EAC3_OUTPUT`, `lib/aius_pipeline/src/aius_pipeline.c:49`).

**All three emit the same unit: 24-bit range sign-extended into `int32`**
(`AIUS_PCM_SAMPLE_BITS`, `lib/aius_codec_api/include/aius_codec.h`). That is the unit
every later stage's magnitudes, thresholds and meter values are expressed in.

**The decoder's memory lives in the DTCM arena — since 2026-07-28, grown for E-AC-3.**
`AiusAudioProcessor::g_arena_memory` is now **218,688 B** (*derived*: instance bound
5,632 + 32,768 B DSP staging carve + `AIUS_PIPELINE_CODEC_SLOT_BYTES` = 180,224 + 64
slack, `src/optical_main.cpp:1402`), roughly two thirds of the DTCM bank;
`begin_pipeline()` creates the pipeline through `aius_pipeline_create_arena`. The
codec slot is sized by the E-AC-3 decoder's measured 179,232 B cumulative arena
high-water rounded up to a 4 KiB boundary; DTS 5.1 core high-water is 62,560 B and
AC-3 27,024 B inside the same slot
(`lib/aius_pipeline/include/aius_pipeline.h:138`–`:158`). The growth was funded
DTCM-internally by pinning the fork's text and cold rodata to flash (−99,328 B of
DTCM `.data`), with a link-time `ASSERT` flooring the stack/locals zone at 24,352 B
(`link/aius_teensy41.ld:266`) — [`aius-eac3-support.md`](../plans/aius-eac3-support.md)
phase 4 carries the funding arithmetic. Routing the vendored decoders' allocation into
the arena depends on the `--wrap` link-time shim (`lib/aius_dts/src/aius_alloc_shim.c`),
live for `[env:teensy41-optical]` (`platformio.ini:104`).

**Until 2026-07-28 the wrap set was `build_unflags`'d**, the linked image contained
plain `malloc`/`calloc`/`realloc`/`free` and no `__wrap_malloc` at all (measured with
`arm-none-eabi-nm`), only the ~5.2 kB `AiusPipeline` instance was arena-allocated, and
~126 kB of the then-131,072 B arena was never touched. Any measurement or reasoning
predating 2026-07-28 rests on that older state.

### Stage 4c — PCM passthrough

`pcm_feed` treats the words as interleaved L,R and left-aligns each onto the
`AiusPcmBlock` sample scale:

```c
const int32_t wire = (int32_t)(int16_t)word;              /* sign-extend 16 -> 32 */
return (int32_t)((uint32_t)wire << PCM_WIRE_SHIFT);       /* left-align onto 24 bits */
```

Through `uint32_t` because a left shift of a negative `int` is undefined behaviour
(C17 6.5.7p4) and the host suites build with UBSan; the two's-complement result is
identical, which is what makes the unsigned round-trip correct rather than merely quiet.

Blocks are emitted at `AIUS_PIPELINE_PCM_BLOCK_FRAMES` = 512 frames. Their
runtime sample rate defaults to 48,000 Hz, then follows the tracker-accepted
32, 44.1 or 48 kHz carrier through `aius_pipeline_set_pcm_sample_rate()`. The
value survives pipeline resets so a carrier-loss reset retains the last
accepted rate.

**Both branches now enter the DSP at the same full scale, and the fix landed at the
contract rather than at this call site.** Until 2026-07-26 there was no left shift, so
the PCM branch entered at 16-bit magnitude against the DTS branch's 24-bit — a 48.16 dB
level step, with the DSP's headroom and limiter threshold both expressed in 24-bit units.
The root cause was that `AiusPcmBlock` documented its planes only as "signed 32-bit" and
never declared a *sample* scale, so each producer picked its own and neither violated the
contract. `aius_codec.h` §SAMPLE SCALE now declares it (`AIUS_PCM_SAMPLE_BITS` = 24 plus
`_MAX`/`_MIN`), `aius_dsp.h` derives `AIUS_DSP_SAMPLE_MAX`/`_MIN` from it instead of
restating the number, and both `aius_dsp.c` and `aius_pipeline.c` bind to it with a
`_Static_assert`.

`test/pipeline/test_pipeline.c` previously asserted the block against a copy of
`pcm_feed`'s own expression, so it printed "1024 frames byte-exact" throughout — a test
that cannot catch a scale error because both sides move together. It now carries a
second, deliberately separate check (`pcm-scale`) written purely in the contract's terms:
a full-scale wire word must reach full scale, with no mention of 16 bits, of a shift
count, or of the producer's expression. Reverting the fix leaves the original check green
and fails only the new one, reporting the measured −48.16 dB.

### Stage 5 — staging to double, input trim and EQ

`stage_input` (`lib/aius_dsp/src/aius_dsp.c:631`) is **the one int32 → double boundary
of the chain**: every present plane is convert-copied into the caller's double scratch
unconditionally — staging *is* the conversion, not part of the optional trim/EQ — and
`aius_dsp_input_run` (`:599`) then applies that channel's linear trim and EQ cascade
over the staged plane, in double, with **no quantize at all**: the trim is folded into
the biquad input as `run_cascade`'s `pregain`, and rounding waits for the chain's
single final quantize ([stage 9](#stage-9--the-final-quantize)).

Buffers: the scratch is the arena's 32,768 B DSP staging carve — six 512-frame double
input planes plus the stereo work pair, carved from `g_arena_memory` in
`begin_pipeline()` (`src/optical_main.cpp:1440`, `DSP_CARVE_BYTES` at `:1402`); the
old `g_input_scratch[6][512]` `int32` array it replaced no longer exists. Coefficient
and state storage lives in `AiusDspRuntimeConfig`
(`lib/aius_dsp_runtime_config/include/aius_dsp_runtime_config.h`).

**With the shipped default the trim/EQ pass is skipped, and until 2026-07-26 it was
not.** `config/optical_dsp.json` sets `features.input_eq: true` with every channel at
`gain_db: 0` and `eq: []`. The model resolution — now in
`aius_dsp_runtime_config_apply` (`lib/aius_dsp_runtime_config/src/aius_dsp_runtime_config.c:116`)
— requires that some channel would actually change a sample: unity gain with no
sections is an identity and `dsp.input` binds null, so `stage_input` performs only the
convert-copy. Unity is tested exactly because `aius_db_to_linear(0.0)` is
`pow(10, 0) == 1.0`, the value the conversion produces rather than a tolerance. A
present-but-*bypassed* EQ section is deliberately not an identity: `eq_sections`
counts what the cascade walks, and the config distinguishes "no section" from "a
bypass section".

### Stage 6 — the downmix matrix, LFE fold-in and surround decorrelation

`aius_dsp_downmix` (`lib/aius_dsp/src/aius_dsp.c:307`; the pre-change link measured it
at 1,254 B of ITCM, the largest single function in the DSP) is mask-driven rather than
mode-driven: routing follows `in->channel_mask`, so mono, stereo, 3.0, 4.0, 5.0 and
5.1 are all handled without a layout flag. **The headroom pad is mask-driven too**,
resolved once per block by `resolve_headroom` (`:295`) rather than per sample. It
consumes and produces double planes; nothing here rounds or clamps.

Two sub-stages live inside the matrix accumulation, both shipped since the last
revision of this document:

- **The LFE fold-in (audio-quality item 2).** The LFE plane is low-passed by a
  4th-order Linkwitz-Riley crossover — two cascaded Butterworth biquads through the
  shared `biquad_step` recurrence — at the configured corner (shipped 120 Hz), then
  scaled by `lfe` (shipped −10 dB) and summed into both L and R
  (`lfe_fold_sample`, `lib/aius_dsp/src/aius_dsp.c:127`; design rationale at
  `lib/aius_dsp/include/aius_dsp.h:190`). The fold is one term in the same
  accumulator, so it adds no requantisation and no latency. Misconfiguration fails
  *closed* (a missing state pointer skips the LFE, landing on BS.775's own default;
  an unfiltered fold is never reachable by omission, `aius_dsp.h:384`).
- **The surround decorrelation delay (audio-quality item 4).** SL and SR are delayed
  symmetrically by `surround_delay_ms` before their fold gain
  (`surround_delay_step`, `lib/aius_dsp/src/aius_dsp.c:210`), bounded at 12 ms / 576
  samples per side (`AIUS_DSP_SURROUND_DELAY_MAX_MS`, `aius_dsp.h:271`); the ring is
  float deliberately (24-bit-origin content fits a float mantissa; error floor
  ≈ −150 dBFS, far beneath the fold gain that follows). **The shipped config sets
  `surround_delay_ms: 0`** (`config/optical_dsp.json:29`), which is the pre-feature
  path bit for bit — the ring is never touched. Misconfiguration fails *open*
  (a null ring folds the surrounds undelayed — yesterday's correct output, not
  missing content, `aius_dsp.h:421`). Landed and verified sample-exact on hardware
  2026-08-04 (`fd65a5303`, `5efdce47d`).

The routing rules are stated authoritatively once in
[§Channel layout and downmix rules](#channel-layout-and-downmix-rules).

### Stage 7 — output EQ

`aius_dsp_eq_run` runs the same `run_cascade` over L and then R with channel-major
state — all L section states, then all R (`src/aius_dsp.c:572`) — in double, over the
work planes, with no per-stage quantize. Sections are Direct Form I biquads with `a0`
normalised to 1 and coefficients from the standard RBJ audio-EQ-cookbook math. State
lives in `AiusDspRuntimeConfig.output_eq_state`, advanced across blocks so section
boundaries are seamless.

**The shipped default runs no output EQ at all.** `output_eq` is empty in
`config/optical_dsp.json`, so `eq_sections` is 0 and `aius_dsp_process` skips the
stage entirely (`src/aius_dsp.c:1388`). RBJ biquads are minimum-phase, so group delay varies
with frequency; a linear-phase FIR alternative is recorded as deliberately unscheduled
in [`aius-dsp-config-api.md` §Noted for later](../plans/aius-dsp-config-api.md#noted-for-later-deliberately-not-scheduled).

### Stage 8 — the true-peak safety limiter

`aius_dsp_limiter_run` (`lib/aius_dsp/src/aius_dsp.c:796`; the pre-change link
measured 764 B of ITCM as `.part.0`, plus `truepeak_push` at 156 B). It consumes and
produces the double work planes — the last processing stage before the final
quantize. Shipped defaults, from `config/optical_dsp.json` and pinned in
`lib/aius_dsp/include/aius_dsp.h`: enabled, threshold −1.0 dBFS, attack 0.5 ms,
release 100 ms, lookahead 2.0 ms.

**It is genuinely true-peak, per ITU-R BS.1770-4 Annex 2.** The detector 4×-oversamples
with the standard's 4-phase, 12-tap polyphase FIR; every coefficient is an exact
multiple of 2⁻¹⁴ and is spelled as its integer numerator over 16384, so the table is
bit-identical on host and target with no transcription rounding
(`lib/aius_dsp/src/aius_dsp.c:422`). The table is 384 B and lands in DTCM `.data`
(measured), because Teensy copies `.rodata` to fast RAM at boot.

Two deliberate deviations from a literal reading, both conservative and both recorded
at `lib/aius_dsp/include/aius_dsp.h:283`:

- **The standard's 12.04 dB pre-attenuation is omitted.** It exists to stop a
  fixed-point implementation overflowing inside the filter; this one is double
  precision.
- **The reported peak is `max(|x[n]|, |interpolated|)`, not the interpolated maximum
  alone.** Phase 0 has a 0.9722 centre tap, so a literal 4× meter can read an isolated
  impulse about 0.2 dB *below* its own sample value. Folding the raw sample in makes
  this a strict superset of a sample-peak detector, which is the only defensible
  behaviour for a stage whose job is safety.

**Cost is controlled by a rigorous bound, not a heuristic.** 4 phases × 12 taps is 48
multiply-accumulates per sample per channel and would dominate the block. The largest
phase has an ℓ1 norm of exactly 33142/16384, so a 12-sample window in which every
sample satisfies `|x| <= threshold / (33142/16384)` provably cannot interpolate above
threshold. Below that gate the FIR is skipped with no possibility of missing an over;
above it, it runs for the whole window the loud sample participates in
(`lib/aius_dsp/src/aius_dsp.c:532`, `:569`). Quiet material pays **one compare per
sample**; only material within about 6.1 dB of threshold pays the full 48 taps. The
`dsp` suite checks the constant against the table, because the gate's soundness depends
on it (`lib/aius_dsp/include/aius_dsp.h:341`).

**The FIR history is primed rather than zeroed into service.** A history zeroed at
reset makes the first sample look like a step from silence, and a step has a genuine
inter-sample overshoot of about +0.6 dB. Since limiter state is zeroed on every config
apply (`lib/aius_dsp_runtime_config/src/aius_dsp_runtime_config.c:48`), a naive
detector would have put a spurious ~0.6 dB
gain dip — decaying over a whole release constant — into the audio every time a control
changed. The detector contributes only `|x[n]|` until the 12-sample window has filled
since the reset, 229 µs at 48 kHz (`lib/aius_dsp/include/aius_dsp.h:305`).

**Lookahead leads the reduction, and the numbers are chosen together.** The peak
follower reads the incoming sample while the gain is applied to the *delayed* sample,
so `lookahead_ms` is exactly how far the reduction leads
(`lib/aius_dsp/src/aius_dsp.c:602`). 2.0 ms against a 0.5 ms attack is four attack time
constants, so the gain is within e⁻⁴ = 1.8 % of target by the time the sample it was
computed for is emitted. The residual is a bounded overshoot, not zero: a peak 3 dB
over threshold emerges about 0.065 dB over it, landing near −0.93 dBFS, and the final
quantize saturation remains the last-ditch clamp.

Buffers: `AiusLimiter` ≈ **4,200 B in DTCM** (*derived* from the struct — config 40 B,
`buf_l`/`buf_r` 240 doubles each at 3,840 B since the one-quantize chain made the
lookahead lines double (`lib/aius_dsp/include/aius_dsp.h:887`), `tp_l`/`tp_r` 16
doubles each at 256 B, scalars). The lookahead lines are fixed at `AIUS_DSP_LIMITER_MAX_LOOKAHEAD` = 240
samples — 5 ms at 48 kHz — so the process path never allocates
(`lib/aius_dsp/include/aius_dsp.h:281`). The configured unit is milliseconds and the
sample count is always derived from the `fs` passed in, so nothing here assumes a rate.
The interpolation history is **persistent state, not stack**: the detector needs only
the maximum, never the interpolated values, so it materialises no oversampled buffer and
the function's stack frame is flat — which matters because this runs in the bounded
AUDIO checkpoint
(`lib/aius_dsp/include/aius_dsp.h:396`).

**Latency added: 2 ms, or 96 samples at 48 kHz** (*derived*). Output frame count equals
input; the last `delay` samples stay in the ring as bounded latency.

### Stage 9 — the final quantize

The tail of `aius_dsp_process` (`lib/aius_dsp/src/aius_dsp.c:1401`) is **the chain's
only rounding and its only clamp**: one `aius_quantize` per output sample — round to
nearest, ties away from zero, then saturate (`:76`) — converting the double work
planes into the caller's 24-bit `int32` output planes. Everything upstream of this
point has carried unclamped double since `stage_input`'s conversion (defect:
inter-stage clipping, fixed 2026-07-28 — `aius_dsp.h:34` §ONE QUANTIZE); an input-EQ
boost that overshoots full scale inter-stage is absorbed by the downmix headroom pad
instead of hard-clipping at a cascade boundary, and the limiter sees true unclipped
peaks.

### Stage 10 — the output word width

`aius_out_stage_run` (`lib/aius_dsp/src/aius_dsp.c:1059`; 192 B of ITCM in the
pre-change link) is the one boundary that cares about the transmit word, and it is
deliberately not part of `aius_dsp_process`: the 24-bit block is the DSP module's
output, and word width belongs to whichever engine consumes it
(`lib/aius_dsp/include/aius_dsp.h:45`).

- **`AIUS_DSP_OUT_WIDTH_24` — the shipped selection.** A plain copy: no dither, no
  rounding, no saturation, because the input is already in range by contract — the
  final quantize just wrote it. The width test is hoisted out of the sample loop, so this
  costs one branch per call and then a straight copy.
- **`AIUS_DSP_OUT_WIDTH_16` — a real selection, not a shim.** `aius_out16_sample()` per
  sample — TPDF dither, round to nearest ties away from zero, saturate — scaled back up
  by `AIUS_DSP_OUT16_LSB` = 256. That multiply is bit-for-bit the `<< 8` the retained
  snapshot's ISR performed
  (`evidence/source-snapshots/PJRC-SPDIF-TX-SOURCE/output_spdif3.cpp:130`), so this arm
  reproduces the wire words the board shipped before the 24-bit path existed, exactly.

The selection is `AiusAudioProcessor::OUTPUT_WIDTH_BITS`, a `constexpr` rather than a
config field (`src/optical_main.cpp:1421`), because nothing on the device has a reason to
ask for 16 bits: the sink is a 24-bit register either way. `aius_out_stage_run` is not
inlined across translation units, so the 16-bit arm and `aius_out16_sample` (188 B of
ITCM, measured) are **linked but never executed** on the shipped image.

**Dither exists, is deterministic, and is bypassed.** It is a property of the 16-bit
selection, not of the chain. TPDF spanning ±1 LSB of the 16-bit target, formed as the
difference of two independent uniform draws, which decorrelates the error from the
signal *and* makes total error power signal-independent so there is no noise modulation
(`lib/aius_dsp/include/aius_dsp.h:482`). The generator is xoshiro128++ 1.0 — integer
only, so bit-identical on x86-64 and Cortex-M7 with no FPU, libc, locale or rounding
mode in the loop; period 2¹²⁸−1, which is 2.2 × 10²⁶ years at 48 kHz, so there is no
repetition to become tonal. Both uniform draws come from the high and low halves of one
32-bit output, a bijection onto the 2³² square, so the pair is exactly as uniform as the
word. L and R are independent streams seeded from eight consecutive SplitMix32 outputs,
because correlated dither across the pair would image as a phantom-centre hiss. The
streams are seeded **once** in `begin_pipeline()` and deliberately not in
`apply_config()` (`src/optical_main.cpp:1691`), so the output is a function of the sample
index rather than of config-change history.

### Stages 11 and 12 — the transmit ring and the wire

`g_aius_spdif_tx24` is an `AiusSpdifTx24Ring`: 2 × 2,048 `int32` planes plus three
cursors, **16,396 B in `DMAMEM`** (measured at `0x20200000`, defined at
`lib/aius_audio/src/aius_spdif_sources.cpp:30`). `enqueue()` stages into it with at most
one wrap and publishes the whole block with a single `aius_spdif_tx24_commit()`
(`src/optical_main.cpp:1582`–`:1595`). `AiusSpdifTx24Hook::isr` (**228 B of ITCM**,
`lib/aius_audio/src/aius_spdif_sources.cpp:57`) drains 128 frames per interrupt into the
half of `SPDIF_tx_buffer` the DMA is not reading, flushes that half once, and then
pends the graph if it holds `update_responsibility`.

**Every closed→open TX-gate edge trims the ring to `AIUS_TX_GATE_OPEN_TARGET_FRAMES`**
by advancing the consumer cursor (`src/optical_main.cpp:1885`) — safe by construction
because the gate was closed, so the dropped frames were dark. The code default is 256
(`:1873`); **the shipped image pins 128** (`-DAIUS_TX_GATE_OPEN_TARGET_FRAMES=128`,
`platformio.ini:310`, tuned 2026-08-03). Trim events and trimmed frames are served on
the clock document; the measured effect is tail elimination (the old 128–1,792-frame
per-session ring lottery is gone) rather than a large mean shift —
[`aius-e2e-latency.md` §Phase 3](../plans/aius-e2e-latency.md#phase-3-tx-ring-gate-open-normalization-and-dhcp-schedule-visibility).

The buffer sizes, the placement argument, the cache-maintenance argument, the
priority-inversion argument and the underrun counter are all covered in
[`firmware-io.md` §S/PDIF output](firmware-io.md#spdif-output--the-transmit-half-of-the-hot-path)
and are not restated here. The two decisions that belong to the *signal path* rather
than to I/O are in [§Decisions recorded](#decisions-recorded-with-their-alternatives).

### The meter taps — deliberately not stages

Metering is caller-driven and is **not** a field of `AiusDspConfig`
(`lib/aius_dsp/include/aius_dsp.h:767`): the caller taps around `aius_dsp_process`, so
the meter code is not linked into the hot function until the adapter wires it. The audio
path is bit-identical whether or not a meter is attached.

`enqueue()` accumulates input peak/RMS and output peak/RMS after a successful process,
so one window always covers the same set of blocks on both sides
(`src/optical_main.cpp:921`), and publishes a coherent snapshot behind a seqlock once
per `g_meter_window_frames` — `rate / AIUS_METER_PUBLISH_HZ`, so 2,400 frames at 20 Hz
(`src/optical_main.cpp:427`, `:1065`). Measured ITCM cost of the taps:
`aius_meter_accumulate_input` 168 B, `accumulate_output` 212 B, `decimate` 254 B,
`publish` 344 B, `set_telemetry` 18 B, `set_limiter_gr` 8 B. Those function sizes are
pre-cutover measurements; `g_meter` is **9,584 B in OCRAM** in the 2026-07-29 link
because its coherent snapshot now includes the 1,024-sample analysis window. Two
design points worth carrying:

- **The gain-reduction accessor is sampled once per window, not per block**, because it
  takes a `log10` and a diagnostic field does not buy a libm call in the block callback
  (`src/optical_main.cpp:961`). The reported value is therefore the *instantaneous* gain
  reduction at the publish instant, not the window's peak.
- **`estimated_latency_us` costs no division.** Microseconds per frame is resolved to
  Q16.16 by `apply_config()`, so the field is one 32×32 multiply and a shift
  (`src/optical_main.cpp:968`).

## Measured output quality — the loopback figures

**These are the first quality figures ever measured on the device itself**, from the
audio-quality roadmap's item 3 (design contract:
[`aius-dsp-config-api.md` §Item 3 design](../plans/aius-dsp-config-api.md#item-3-design--the-measurement-loopback-software-tap-first)).
The instrument is a software tap in `enqueue()` after the tx24 commit — at the shipped
24-bit width the committed words ARE the wire's, so the capture measures everything from
the stimulus input through the whole DSP chain to the transmit register, sharing one sample
grid with the stimulus (immune to physical clock error by construction; the clock/content
*ratio* surface stays with the future physical loop). Analysis: an IEEE 1057 3-parameter
sine fit at the declared f0 (THD+N; SNR additionally excludes harmonics 2–10) plus
Hann-windowed Goertzel (harmonic amplitudes, FR probes) over the last 8,192 frames of a
16,384-frame capture (`lib/aius_dsp/include/aius_measure.h`), served by
`GET /api/measure`.

**Measured 2026-07-27 from the device, build `b5f9a1351adf`, boot generation 250–252**,
against the loop-coherent 16-bit PCM stimulus corpus (`tools/measure_vectors/`), shipped
SD config (headroom 0.5, limiter on at −1 dBFS, bypass EQ) except where stated. The
stimulus floors bind everything below: a 16-bit vector bottoms THD+N/SNR near the
quantization floor at the tone's own level, so a figure at its floor reads "at least
this good".

**These are PRE-CHANGE measurements, kept as history.** Every vector here is 16-bit PCM
pass-through — stereo FL|FR, the exact layout the headroom pad stopped applying to on
2026-07-28 ([§Channel layout and downmix rules](#channel-layout-and-downmix-rules)) — so
the *levels* in the table are 6.02 dB below what the same run reads today, and the
`pcm440_full` row's raised headroom is no longer how you engage the limiter (a full-scale
stereo vector now reaches it at the shipped config). The distortion, noise and flatness
figures are level-relative and stand as measured; the fundamentals are superseded.

| Vector, channel | Fundamental | THD+N | SNR | THD (2nd–10th) | Stimulus floor at this level |
|---|---|---|---|---|---|
| `pcm440_m6`, L | −12.04 dBFS @ 440 Hz | **−91.7 dB** | 91.8 dB | −107.9 dBc | ≈ −92.1 dB |
| `pcm440_m6`, R | −12.04 dBFS @ 440 Hz | **−91.7 dB** | 91.8 dB | −107.9 dBc | ≈ −92.1 dB |
| `pcm440_660_split`, L | −12.04 dBFS @ 440 Hz | −91.7 dB | 91.8 dB | −108.0 dBc | ≈ −92.1 dB |
| `pcm440_660_split`, R | −18.06 dBFS @ 660 Hz | −86.0 dB | 86.1 dB | −103.8 dBc | ≈ −86.1 dB |
| `pcm440_full` + headroom 1.0, L/R | −1.01 dBFS @ 440 Hz (limited) | −95.6 dB | 95.7 dB | −109.2 dBc | ≈ −98.1 dB |
| `pcm_fr_multitone`, 40 Hz–16 kHz | seven probes, both channels | — | — | — | — |

What the numbers say, in order of what they prove:

- **The path is transparent to the stimulus floor.** THD+N lands within 0.4 dB of the
  16-bit stimulus's own quantization floor at every level tried (−91.7 vs ≈ −92.1 at
  −6 dBFS stimulus; −86.0 vs ≈ −86.1 at −12 dBFS). Whatever the chain adds sits below a
  16-bit transport's ability to resolve — the −140 dB inter-stage claims stay
  host-proven, as the design says they must.
- **Levels are arithmetic-exact** *(superseded: the pad these figures pay no longer
  applies to a stereo layout)*. Every fundamental reads stimulus level − 6.02 dB of
  shipped headroom to the hundredth of a dB, and L/R figures on identical stimulus are
  identical to the last printed digit (bit-identical DSP on bit-identical input). The
  arithmetic-exactness is the durable claim; the 6.02 dB term is not.
- **Frequency response is flat to 0.01 dB, 40 Hz to 16 kHz** (all seven multitone
  probes at −24.08 dBFS on both channels, bypass EQ).
- **The limiter's steady-state gain reduction is distortion-free at this floor.** With
  headroom raised to 1.0 so the full-scale vector engages it — the layout-aware pad makes
  that step unnecessary now — the limiter holds the tone
  at −1.01 dBFS (threshold plus its documented e⁻⁴ smoothing residual,
  [stage 8](#stage-8--the-true-peak-safety-limiter)) and THD+N still reads −95.6 dB. A
  *transient* limiter-distortion figure needs a modulated hot vector; deliberately not
  scheduled.
- **The arm-time latches work live.** The very first run retired `failed (generation)`
  because arming preceded the board's first-ever audio block, whose block-boundary
  config apply bumped the generation — exactly the splice the latch exists to refuse.
- **The tap's ISR cost is invisible inside the block-to-block spread** — the
  acceptance the design demanded, as a distribution comparison with meters ON:
  `dsp_cycles_max` over 10 s windows read 322,215 unarmed vs 309,236 with captures
  armed back-to-back (delta −13.0 k against the ±30 k gate; `dsp_cycles_last` p50
  280,696 vs 280,657). Those p50s are also **the first measured per-block DSP cost
  with audio flowing** — ≈ 281 k cycles ≈ 4.4 % of the block budget on the PCM
  passthrough path, replay looping (replay since retired), meters on, zero overruns across the whole
  20,492-block session.

One `DMAMEM` note for the maintainer contract below: the 128 KiB capture planes are the
first large `DMAMEM` addition since the retained region was carved out ahead of
`.bss.dma`, and the retained-slot guard passed — `boot_generation` survived both flashes
(247 → 250 across three flashes and a reboot), so the item-1-era "every large `DMAMEM`
object restarts the ledger" hazard is closed by the linker carve-out, not merely
tolerated.

## Channel layout and downmix rules

**This is the authoritative statement; nothing else in this document restates it.**

Channel order is `AIUS_CH_*` bit order throughout — FL, FR, FC, LFE, SL, SR
(`lib/aius_codec_api/include/aius_codec.h:38`) — and planes are tightly packed to
`channels` in that order, as the decoder emits them. The same ordinal indexes the input
stage's per-channel config and the meter's per-input arrays.

The matrix, per ITU-R BS.775 / ATSC A/52 "Lo/Ro"
(`lib/aius_dsp/include/aius_dsp.h:107`):

```text
L' = pad * (FL + center*FC + surround*delay(SL) + lfe*LR4(LFE))
R' = pad * (FR + center*FC + surround*delay(SR) + lfe*LR4(LFE))

LR4  = 4th-order Linkwitz-Riley low-pass at lfe_crossover_hz (identity when lfe = 0)
delay = surround_delay_ms pre-fold delay (identity at the shipped 0 ms)
pad  = headroom  when the layout SUMS  (channels > 1 and any of FC, LFE, SL, SR)
pad  = 1.0       otherwise             (mono, and stereo FL|FR)
```

| Coefficient | Shipped value | Source |
|---|---|---|
| `center` | 0.70710678118654752440 (1/√2, −3.01 dB) | `aius_dsp.h:111`, `optical_dsp.json:22` |
| `surround` | 0.70710678118654752440 (1/√2, −3.01 dB) | `aius_dsp.h:111`, `optical_dsp.json:23` |
| `lfe` | 0.316228 (−10 dB), folded in through the LR4 low-pass | `aius_dsp.h:186`, `optical_dsp.json:24` |
| `lfe_crossover_hz` | 120 (LR4 corner; config-bounded to [20, 500]) | `aius_dsp.h:242`, `optical_dsp.json:25` |
| `headroom` | 0.5 (−6.02 dB) **on summing layouts only**; 1.0 elsewhere | `aius_dsp.h:354`, `optical_dsp.json:26` |
| `clipfree` | false | `optical_dsp.json:27` |
| `layout_aware` | true (absent in a PUT body also reads true) | `optical_dsp.json:28`, `aius_dsp_config.h:60` |
| `surround_delay_ms` | 0 (decorrelation off — the bit-exact pre-feature fold) | `optical_dsp.json:29`, `aius_dsp_config.h:68` |

- **1/√2 on centre and surround is the standard default**, not a taste choice: mixing FC
  and each surround down 3 dB preserves acoustic power when the two summed sources are
  uncorrelated.
- **The coefficients are symmetric by design.** `center` is the shared FC-to-L/R
  coefficient and `surround` the shared SL-to-L / SR-to-R coefficient. Independent SL/SR
  gains are excluded because they would shift image balance rather than control blend.
- **LFE at −10 dB through a 120 Hz LR4 is the shipped fold-in** (audio-quality item 2,
  landed 2026-07-26, `72bb041a5`). BS.775 and A/52 exclude LFE from the default stereo
  downmix; on a two-speaker system that throws the bottom octave of film content away,
  so the fold ships at `AIUS_DSP_LFE_GAIN_MINUS10DB` — 20 dB below BS.775's +10 dB
  reference, the standard small-stereo compromise — band-limited by the crossover
  because the DTS core does not band-limit its LFE plane
  (`aius_dsp.h:117`–`:255` carries the full level and filter-shape rationale).
  `lfe = 0.0` remains the exact negative control. This is the LFE's own bass
  management only; there is still no main-channel crossover.
- **Narrower inputs need no mode flag.** Mono copies the lone plane to both channels at
  unity — **and takes no headroom pad**, whichever mask bit it carries; stereo FL|FR is an
  exact passthrough at unity and takes no pad either; absent channels contribute nothing.
- **The headroom pad applies only to layouts that sum**, resolved per block from the input
  mask alone (`aius_dsp.c`'s `resolve_headroom`). FC and the LFE fold land on both outputs
  and SL/SR land on one each, so a layout carrying any of them can overshoot and pays the
  pad; mono and FL|FR route 1:1, and a ≤ full-scale input through a unity route is
  ≤ full scale at the output, so paying it there bought no protection and cost every
  stereo source 6.02 dB. The decision reads the mask and nothing else — not the resolved
  plane pointers, not the gains — so an LFE plane at `lfe_gain_linear = 0` still pads and
  stays bit-for-bit the same block without it. `matrix.layout_aware` (default on, absent
  reads on) turns the rule off and restores the uniform pad on every layout, mono
  included.
- **Headroom 0.5 is an evidenced product decision** *for the summing case*, and the
  evidence is in [§Decisions recorded](#decisions-recorded-with-their-alternatives).
  It was re-measured after the LFE fold-in obliged the same evidence again: 0.5 still
  clears the whole corpus, with the binding vector (`surround51_impulse`) at
  −0.790 dBFS — 0.79 dB of spare margin surviving the fold intact
  (`aius_dsp.h:317`–`:338`). `aius_downmix_clipfree_headroom()` returns the
  theoretical `1/(1 + center + surround + |lfe|)`, which at the shipped fold is
  1/2.730335 = 0.366 (−8.72 dB; it was 0.414214 / −7.66 dB before the fold), and is
  selectable via `clipfree`, at which point a caller may reasonably turn the limiter
  off. Both describe the summing matrix; neither applies to a pass-through layout.

The two sensible configurations are **(modest headroom + limiter on)** and **(clip-free
headroom + limiter off)**. The shipped one is the first.

## Fixed-point and floating-point boundaries

**The chain enters as 24-bit signed integer, travels the DSP as IEEE-754 double, and
is quantized exactly once at the end** (defect: inter-stage clipping, fixed
2026-07-28). There is exactly one integer sample unit in the system — the decoders'
24-bit range, `AIUS_DSP_SAMPLE_MAX` = 2²³−1 and `AIUS_DSP_SAMPLE_MIN` = −2²³, derived
from `AIUS_PCM_SAMPLE_BITS` (`lib/aius_dsp/include/aius_dsp.h:104`) — and every
magnitude, threshold, peak and RMS value in the DSP and in `GET /api/meters` is
expressed in it. A reader converts to dBFS with `20*log10(v / AIUS_DSP_SAMPLE_MAX)`.

Where each conversion happens:

| Boundary | Direction | Function | Note |
|---|---|---|---|
| Receiver → engine | int32 subframe → int16 | `input_spdif3.cpp:118` | `>> 8`, truncating; the input ceiling |
| Engine → pipeline | int16 → uint16 words | `AiusAudioProcessor::update` | reinterpret, no arithmetic |
| DTS decode → DSP | dcadec fixed point → int32 | `aius_dts.c` | 24-bit range, sign-extended |
| AC-3 decode → DSP | liba52 float → int32 | `aius_a52.c` | same 24-bit scale |
| E-AC-3 decode → DSP | fork float → int32 | `aius_eac3.c` | same 24-bit scale |
| PCM → DSP | int16 → int32 | `pcm_wire_to_sample` | sign-extend then left-align onto `AIUS_PCM_SAMPLE_BITS` |
| Staging | int32 → double | `stage_input`, `aius_dsp.c:631` | the one widening conversion |
| Input stage, downmix, EQ, limiter | double → double | `aius_dsp.c` | **no rounding, no clamping** |
| Final quantize | double → int32 | `aius_dsp.c:1401` | the chain's only rounding and clamp |
| Output stage, 24-bit | int32 → int32 | `aius_dsp.c:1059` | **no conversion at all** |
| Output stage, 16-bit | int32 → double → int32 | `aius_dsp.c:1013` | dither, round, saturate, × 256 |
| Ring → wire | int32 → int32 | `aius_spdif_tx24.h` | copy into `SPDIF_STL`/`STR` |

**Double was chosen over fixed point deliberately.** A biquad's feedback path is
precision sensitive, double keeps the design headroom trivial, and every 24-bit input
sample is represented exactly (`lib/aius_dsp/include/aius_dsp.h:47`). The Cortex-M7 has
a hardware double-precision FPU, so this is not an emulation cost.

**The one quantizer rule is `aius_quantize`: round to nearest, ties away from zero, then
saturate** (`lib/aius_dsp/src/aius_dsp.c:76`). `aius_out16_sample` uses
the identical rule, so the module's two quantizers cannot disagree about a boundary
(`:1013`).

**Inter-stage requantisation was removed on 2026-07-28** — the "carry double end to
end and quantise once" item the config-API plan recorded as unscheduled shipped as
part of the inter-stage-clipping fix. Until then each stage rounded and *saturated*
on the way out (error ≈ −140 dBFS per step, inaudible, but the saturation was a
correctness problem: the downmix and EQ clamped upstream of the limiter, so the
limiter could never see the over it exists to catch). Now no stage between the
staging conversion and the final quantize rounds or clamps, the limiter sees true
unclipped peaks, and the 24-bit wire ceiling exists only at the final quantize
(`aius_dsp.h:34` §ONE QUANTIZE).

### `-ffp-contract=off` is load-bearing, not cosmetic

**The DSP's contract is that host and target produce bit-identical output, and until
2026-07-26 that contract was false on the target.** `scripts/test.sh` passed
`-ffp-contract=off` for the `dsp` and `dsp-vectors` suites, but `platformio.ini` did
not, and GCC defaults to `-ffp-contract=fast` under `-std=gnu++17`. Disassembly of
`aius_dsp.c.o` showed **25 `vfma.f64` instructions** — in `truepeak_push`,
`run_cascade`, `aius_dsp_downmix` and `aius_dsp_limiter_run` — where the host emitted
separate `vmul` and `vadd`.

A fused multiply-add keeps full intermediate precision, so it does not merely round
differently: **it rounds once where the host rounds twice.** The downmix and the EQ were
therefore never bit-identical host-to-target despite that being the stated contract, and
the 48-tap true-peak FIR had become the largest single source of divergence. The fix is
one flag, now pinned in `[common] build_flags` with a do-not-remove note
(`platformio.ini:77`). It is what makes every `dsp` golden vector a statement about *this
image* rather than only about the host.

One contraction was benign by construction and is worth knowing so nobody chases it: the
`vfma` in `aius_out16_sample` multiplies an exact integer by a power of two, so fused and
unfused agree bit-for-bit.

**The header's comment block has since been corrected** and now reads "THAT
PRECONDITION IS NOW MET ON BOTH SIDES" (`lib/aius_dsp/include/aius_dsp.h:75`) — the
contradiction recorded at
[§Contradictions](#contradictions-found-against-the-plans) item 1 is closed.

## The clock

**Boot is exactly 48 kHz; live output follows 32, 44.1 or 48 kHz
input, including its oscillator error.** `AUDIO_SAMPLE_RATE_EXACT=48000.0f`
still supplies the bootstrap clock and the generated config default.
The `static_assert` keeps those two roles coherent; it no longer defines the
live output rate.

At checkpoint 7, the tracker reads the receiver's `SPDIF_SRFM` measurement in
millihertz, accepts one of the three nominal rates after eight consistent
locked samples, and asks the pure planner for PLL4 integer/fractional fields,
root dividers, TX divider and IEC channel status. A whole-rate or structural
change closes the pad and applies the complete plan with a 1,000 microsecond
lock deadline. A same-structure correction queues only
`CCM_ANALOG_PLL_AUDIO_NUM` for the next TX boundary. That trim keeps the
carrier and ring continuous while IEC validity is invalid for the next four
complete 128-frame periods. There is no resampler or intermediate
requantisation (`lib/aius_clock/src/aius_clock.c`,
`lib/aius_audio/src/aius_audio_clock.cpp`).

Runtime replans deliberately leave `CCM_CCGR5_SPDIF` enabled. That gate stops
the live receiver DMA as well as the transmitter; it is safe in
`config_spdif3()` before RX starts, but gating it after boot caused persistent
missing-live gaps and transmit starvation. The closed physical pad and invalid
IEC status are the runtime quiescence boundary while PLL4 and the TX root move.

The receiver is the clock master (`clock.master` reads the constant `input`).
The transmit half is described in
[`firmware-io.md` §S/PDIF output](firmware-io.md#spdif-output--the-transmit-half-of-the-hot-path)
and is not restated.

## Memory by bank

**Measured banks**, from `tools/memory/mem_report.py --elf` over
`.pio/build/teensy41-optical/firmware.elf` built from the sample-rate-following
Phase 2 working tree on 2026-07-29:

```text
BANK           USED         FREE     CAPACITY    USED%   FREE%
FLASH        439296      7687168      8126464    5.41%   94.6%
ITCM         191548         5060       196608   97.43%    2.6%
DTCM         291072        36608       327680   88.83%   11.2%
FlexRAM      487680        36608       524288   93.02%    7.0%
OCRAM        392832       131456       524288   74.93%   25.1%
```

**DTCM free-for-locals is 36,608 bytes and OCRAM free is 131,456 bytes in that
link.** The former is the entire budget for every thread and interrupt stack. The
post-link memory gate's OCRAM ratchet stood at 131,424 B then; it has since been
lowered stepwise to 131,232 B (OTA dispatcher seam, cache-line pinning, the retained
poll probe, the decorrelator's config growth — each dated beside the constant,
`test/memory/verify_target.py:46`).

**The figure has moved repeatedly, and stale copies remain historical evidence**:
9,280 → 17,152 → 25,376 → 43,328 → 47,552 → 36,608 → **31,040** (E-AC-3 phase 4,
the latest committed figure, against the link-time floor of 24,352 B —
`link/aius_teensy41.ld:266`, [`aius-eac3-support.md`](../plans/aius-eac3-support.md)).
So a bare number in prose is not usable evidence — [`firmware-io.md`](firmware-io.md)'s
table reports 9,280 (correct at *its* revision, pre-`FNET_CFG_TLS=0`), and this
section previously reported 25,376 then 43,328. They are snapshots, not
contradictions. **The whole bank table above predates E-AC-3 phase 4 (which pinned
~99 KiB of rodata plus the fork's text to flash and grew the arena to 218,688 B), the
OTA slices and the decorrelator; no bank table for the current image is committed.
Take the current value from `tools/memory/mem_report.py` against the linked ELF**, and
state the revision beside any number copied out of it.

Every audio-path object, with its measured size and bank:

| Object | Size | Bank | Citation |
|---|---|---|---|
| `AiusAudioProcessor::g_arena_memory` | 218,688 B (*derived* since E-AC-3 phase 4; 131,072 B measured in the 2026-07-29 ELF) | DTCM `.bss` | `optical_main.cpp:1402` |
| — of which DSP scratch, six input + two work double planes | 32,768 B | DTCM arena | `optical_main.cpp:1402` |
| — of which the codec slot (one resident decoder) | 180,224 B | DTCM arena | `aius_pipeline.h:145` |
| — of which pipeline, parser and PCM staging | ≤ 5,632 B instance bound | DTCM arena | `aius_pipeline.h:166` |
| `g_audio_processor` | 5,784 B (2026-07-29 measurement; has since grown with the LFE fold and decorrelator state) | DTCM `.bss` | `optical_main.cpp:1810` |
| — of which `g_limiter` (*derived*) | ≈4,200 B | DTCM | `aius_dsp.h`, `AiusLimiter` |
| `k_truepeak` | 384 B | DTCM `.data` | `aius_dsp.c:422` |
| `processed_left`/`_right` in `enqueue()` | 4,096 B | DTCM stack | `optical_main.cpp:1142` |
| `g_audio_ingress_words` | 16,384 B | OCRAM `DMAMEM` | `optical_main.cpp:194` |
| `g_aius_spdif_tx24` | 16,396 B | OCRAM `DMAMEM` | `aius_spdif_sources.cpp:30` |
| `g_meter` | 9,584 B | OCRAM `DMAMEM` | `optical_main.cpp:438` |
| `g_measure_capture_l`/`_r` | 131,072 B | OCRAM `DMAMEM` | `optical_main.cpp:472` |
| `g_active_model` | 656 B | OCRAM `DMAMEM` | `optical_main.cpp:395` |
| `g_pending_dsp_model` | 656 B | OCRAM `DMAMEM` | `optical_main.cpp:413` |
| `spdif_rx_buffer` | 2,048 B | OCRAM `DMAMEM` | `input_spdif3.cpp:37` |
| `SPDIF_tx_buffer` | 2,048 B | OCRAM `DMAMEM` | `output_spdif3.cpp:44` |
| `AudioMemory(24)` pool | 6,240 B | OCRAM `DMAMEM` | `optical_main.cpp:5166` |
| recorder sample staging | 32,768 B | first fitted PSRAM | `optical_main.cpp:492` |
| `g_recorder_staging_frames` | 32 B | DTCM `.bss` | `optical_main.cpp:504` |
| `AIUS_OPTICAL_CONFIG` | 656 B | FLASH `PROGMEM` | generated header |

Measured ITCM for the audio path, by function:

| Function | ITCM | Note |
|---|---|---|
| `aius_dsp_downmix.part.0` | 1,338 B | largest in the DSP |
| `aius_dsp_limiter_run.part.0` | 828 B | + `truepeak_push` 156 B |
| `aius_pipeline_ingest.part.0` | 590 B | AUDIO checkpoint |
| `aius_pipeline_service_one` | 578 B | AUDIO checkpoint |
| `aius_iec61937_push` | 580 B | AUDIO checkpoint |
| `aius_dsp_process` | 540 B | AUDIO checkpoint |
| `aius_recorder_producer_block` | 470 B | AUDIO checkpoint |
| `aius_dts_decode_access_unit` | 296 B | AUDIO checkpoint |
| `AiusAudioProcessor::update` | 280 B | pinned ISR-hot section |
| `AiusSpdifTx24Hook::isr` | 228 B | not policed by the ITCM guard |
| `aius_out_stage_run` | 192 B | both arms linked |
| `aius_out16_sample` | 188 B | linked, never executed |
| `aius_measure_producer_block` | 168 B | AUDIO checkpoint |
| `run_cascade` | 152 B | shared by input stage and output EQ |
| `aius_quantize` | 112 B | |
| `aius_dsp_input_run` | 56 B | |
| `audio_ingress_publish_live` | 36 B | pinned ISR-hot section |

The optical object's two pinned ISR-hot sections total **316 B**. The mixed ingress
object contributes exactly five producer sections, also 316 B; nine declared cold
objects contribute zero owned ITCM. `enqueue()` (808 B), `consume_block()` (8 B) and
`aius_audio_service_run()` (534 B) link in **FLASH** and execute at AUDIO
(`test/itcm-placement/verify_target.py:48`).

## The per-block budget

**The unit is the 512-frame decoded block: 10,666.67 µs, or 6,400,000 cycles at
600 MHz** (*derived*: 512 / 48,000 × 600 × 10⁶). The engine block is 128 frames =
2,666.67 µs = 1,600,000 cycles, and only the transmit interrupt is measured against
that.

**Measured on the device before the Slice 2b3 cutover**, build revision
`f76baa87c67d`, 20 s of replay (since retired) at 93.3–94.5 blocks/s with zero overruns and zero
underruns:

| Figure | Cycles | Share of 6,400,000 |
|---|---|---|
| `dsp_cycles_last` | **534,411** | 8.35 % |
| `dsp_cycles_max` | **565,024** | 8.83 % |

That pre-cutover chain used **8.4–8.8 % of the block budget and left about 91 %
free.** The
figures come from the DWT cycle counter, which brackets the whole per-block body of
`enqueue()` — DSP, both meter accumulates, the ring write and any publish — and reports
through `GET /api/meters` (`src/optical_main.cpp:911`, `:982`). Two volatile PPB loads
per block is the entire instrumentation cost.

The current image retains that decoded-block DWT bracket and adds a second timer around
the whole AUDIO checkpoint. The service must drain/classify/decode/output inside 8 ms,
and the complete normal epoch must stay inside 25 ms; status exposes both maxima and
overrun counters (`src/optical_main.cpp:7039`). Since defect 35's substep attribution
(`930f003c6`), `audio_diag` additionally serves `service_max_step_us` and
`service_max_ingest_us` (`src/optical_main.cpp:2632`), which is how a single
pathological decode step (measured 118.8 ms on a mid-burst source cut, ingest never
above 48 µs) is attributed on sight — the bounded silent budget overrun per source
discontinuity is an accepted residual
([open-work register #35](../plans/aius-open-work-register.md)). Those are the
Slice 2b3 hardware-gate figures; the table above remains dated evidence, not proof of
the cutover build.

### There is no per-stage breakdown, and this document will not invent one

**The DWT bracket covers `enqueue()` as a whole. No stage inside it is separately
measured, and no instrumentation exists that would separate them.** Anyone needing a
per-stage figure must add a second bracket; the honest statement today is the total.

What *is* separately known are two whole-path differences between builds, both measured
on the device, and one instruction-count estimate:

| Change | Before | After | Delta | Provenance |
|---|---|---|---|---|
| Dither + headroom 0.5 + true-peak limiter + `-ffp-contract=off`, together | 398,591 | 607,086 | **+208,495** | device, both builds |
| 16-bit output stage → 24-bit output stage | 607,086 | 534,411 | **−72,675** | device, both builds |
| The output stage's own cost, 16-bit vs 24-bit | ~51,000 | ~6,000–7,000 | — | **estimated** by instruction counting |

The first row is four changes landing together and **cannot be attributed to any one of
them.** The second row is the one measured figure that isolates a single stage, and it
attributes the whole 72,675-cycle saving to removing 1,024 `aius_out16_sample()` calls
per block — each about 45–60 cycles of xoshiro, two `vcmpe`/`vmrs` pairs and a
`vrintm`. The third row is the same change costed by hand and is **the only estimate in
this section**; it under-predicted the measured saving, which is why the measurement,
not the estimate, is what this document leads with.

The prediction for the 24-bit path was ~560,000 / ~590,000 cycles and the device came in
better on both axes. The prediction for the three earlier fixes bracketed 6.6–7.1 %
quiet and 10.0–13.0 % worst case, and the measured 9.5–10.0 % landed between them —
consistent with cinematic material opening the true-peak gate often rather than rarely.

### Latency

| Contributor | Frames | Time at 48 kHz | Provenance |
|---|---|---|---|
| Limiter lookahead | 96 | 2.00 ms | *derived* from `lookahead_ms` |
| Transmit ring, gate-open trim target | 128 (shipped image; code default 256) | 2.67 ms | `platformio.ini:310`, `optical_main.cpp:1873` |
| Transmit ring, capacity | 2,048 | 42.67 ms | *derived* from the ring size |
| Whole physical loop, PCM (ESI U24 XL → board → U24 XL) | 926 | 19.29 ms | device, e2e-latency Phase 3 |
| — of which board share (loop minus adapter baseline) | 836 | 17.4 ms | device, e2e-latency Phase 3 |

The end-to-end decomposition, its per-session determinism (the gate-open trim removed
the 128–1,792-frame ring lottery) and the identified levers live in
[`aius-e2e-latency.md`](../plans/aius-e2e-latency.md); the pre-trim "~512-frame steady
ring" figure this table used to carry described the old equilibrium and is superseded.
`estimated_latency_us` in the meter snapshot is the ring contribution only —
`queue_frames × 10⁶ / fs` — and does not include the limiter's lookahead or the decoder's
own block latency.

## Decisions recorded, with their alternatives

**Headroom, not the limiter, caught the downmix over — historically.** When the 0.5
default was chosen, the downmix quantised and saturated at the end of
`aius_dsp_downmix` and the EQ cascade saturated again, so every later stage — the
limiter included — saw an already-clamped signal, and a limiter downstream of a
saturating matrix cannot recover an over the matrix has already destroyed. **That
structural argument dissolved on 2026-07-28** when the chain moved to one quantize at
the end ([§Fixed-point and floating-point boundaries](#fixed-point-and-floating-point-boundaries)):
"unity headroom, let the limiter catch the overs" is no longer ruled out — but 0.5
stays, because the corpus evidence below still supports it, the exact-power-of-two
multiply is still bit-exact, and the pad is now also what gives an input-EQ boost its
working room before the wire ceiling (`aius_dsp.h:280`).

**Why 0.5 and not 1.0, and not the clip-free 0.414214.** Unity clipped: `dsp-vectors`
reported `surround51_impulse … out_peak=8388607/8388607`, exactly
`AIUS_DSP_SAMPLE_MAX`, so the *committed corpus was clipping*. The measured corpus peak
distribution through this matrix at unity headroom (4 vectors × 5,120 frames,
`lib/aius_dsp/include/aius_dsp.h:117`):

| Vector | Sample peak | p50 | p99.9 | Samples over full scale |
|---|---|---|---|---|
| `stereo_tones` | −22.50 dBFS | −26.33 | −22.50 | 0 |
| `surround51_tones` | −17.17 dBFS | −26.34 | −17.17 | 0 |
| `stereo_impulse` | −2.05 dBFS | −100.86 | −33.46 | 0 |
| `surround51_impulse` | +5.23 dBFS | −86.31 | −20.81 | **10 of 10,240** |

Exactly one vector clips, on 10 of its 10,240 samples, and it is the synthetic
all-channel-simultaneous full-scale impulse: 99.9 % of even that vector sits below
−20 dBFS. Paying the theoretical −7.66 dB to accommodate those 10 samples would make the
device audibly quiet against every other source. **0.5 clears the whole corpus** (worst
case becomes −0.79 dBFS, 0.79 dB spare), costs 6.02 dB rather than 7.66, and is an exact
power of two — so the headroom multiply is an exponent decrement with no rounding at
all, bit-exact on host and target alike. It is deliberately *not* clip-free in theory: a
fully correlated full-scale 5.1 input still reaches 2.4142 × 0.5 = +1.63 dBFS, which is
why the shipped limiter default is enabled.

**Why the pad became layout-aware, and why that is not "unity headroom" by another
name.** The evidence above is a *summing* argument: every over in it comes from FC, the
surrounds and the LFE landing on top of the fronts. A stereo pass-through sums nothing —
FL→L and FR→R at unity — so it could never produce the peak the pad exists to absorb, and
paid 6.02 dB for protection it could not need; the operator's report was simply that
PCM pass-through was quiet. The rejected alternative was dropping `headroom` to 1.0
globally, which is the choice this evidence rules out for 5.1. Resolving the pad per
block from the input mask keeps 5.1 exactly as measured (both 5.1 vector goldens are
byte-identical across the change) and gives every non-summing layout its source level
back. The trade accepted with it: an EQ boost on hot stereo material now reaches the
limiter, or the wire ceiling with the limiter off, instead of pad room.

**Why the true-peak detector folds in the raw sample.** Phase 0 of the BS.1770-4 filter
has a 0.9722 centre tap, so a literal 4× meter can read an isolated impulse about 0.2 dB
*below* its own sample value. Reporting `max(|x[n]|, |interpolated|)` makes the detector
a strict superset of a sample-peak detector, whether the skip gate is open or shut and
whether the history has primed or not. For a stage whose job is safety, a detector that
can read below the sample peak is not defensible.

**Why noise shaping was declined for the 16-bit stage, and it is a recommendation rather
than an oversight** (`lib/aius_dsp/include/aius_dsp.h:490`). Shaping the dither noise out
of the ear's sensitive band would buy the usual 10–15 dB of perceived SNR for two doubles
of error-feedback state and a couple of multiplies per sample, which is affordable. It is
declined because flat TPDF already puts the noise floor near −93 dBFS, below both the DTS
Core source and any DAC this feeds; because the error-feedback path would need defined
behaviour when the output saturates (an unshaped stage simply clamps, a shaped one can
wind up), which is new contract surface on a safety path; and because the feedback state
would need clearing on reset, reintroducing exactly the settling transient this stage's
statelessness-apart-from-the-PRNG avoids. It remains **the hedge** if the 24-bit path had
proved too invasive — it did not — and is worth revisiting only if measurement shows the
16-bit floor is actually the limit.

**Why the transmit ring lives in `DMAMEM` and not DTCM.** The `int16` ring was 8,192 B in
DTCM; the honest 24-bit one is 16,384 B, which would have spent the *entire*
free-for-locals zone as it stood. Moving it to OCRAM **returned** 8,224 B of the scarcest
bank and cost 16,416 B of a bank with 293 kB free. A DMA ISR reads it, which is the
reason to think twice and also the reason it is still right: the read is strictly
sequential, 256 words per interrupt, so each 32-byte OCRAM line fill amortises over eight
words — 32 line fills at roughly 20–30 cycles is under 1,000 cycles (*estimated*) against
a 128-frame interrupt period of 1,600,000 cycles.

**Why the ring is stage/commit rather than a read/write/count triple, and this is the
subtle one.** The producer and consumer are different contexts. `enqueue()` runs in
thread context at AUDIO; the transmit DMA interrupt runs at the startup-default NVIC
priority 128 (`cores/teensy4/startup.c:136`), so **the consumer can preempt the
producer mid-block**. The old `int16` ring had both ends inside `update()` and could
afford a plain count; that would be a race here. Instead the ring keeps two
free-running 32-bit frame counters, each written by exactly one side, with the level
as their unsigned difference — correct across the counters' own wrap, and single-copy
atomic on the Cortex-M7. Staged frames become visible only at
`aius_spdif_tx24_commit()`, so a preempting consumer never observes a half-written block
(`lib/aius_audio/src/aius_spdif_tx24.h:68`).

**Why 24 bits is not a config field.** The sink is a 24-bit `SPDIF_STL` register either
way, so selecting 16 could only throw away 8 bits. `OUTPUT_WIDTH_BITS` is a `constexpr`;
promoting it to `AiusOpticalConfig` is a config-surface change and belongs with whoever
next widens that model (`src/optical_main.cpp:1421`).

**Why the 16-bit arm was bypassed rather than deleted.** A 16-bit sink downstream is a
real case, the arm reproduces the old wire words bit-exactly so it is a regression
reference, and the `dsp` suite still pins its per-sample rule. Deleting it would have made
the 24-bit path unfalsifiable against the board's own history.

## Open at this revision

**The input side is now the remaining ceiling.** `audio_block_t` is off the *output* path
and the chain is lossless from `aius_dsp_process` to `SPDIF_STL`, but
`AudioInputSPDIF3` still stages through `int16` (`input_spdif3.cpp:118`), so passthrough
of a 24-bit linear-PCM carrier loses 8 bits **on the way in**. The DTS path is unaffected
because 16-bit IEC 61937 packing occupies exactly the bits the `>> 8` keeps. Fixing it is
the mirror of what the transmit side already did — a sibling receive path alongside the
retained snapshot rather than a vendor patch — and no plan currently owns it.

**~~PCM passthrough sits 48.16 dB below the DTS path in the DSP's own units.~~ Fixed
2026-07-26** — see [§Stage 4c](#stage-4c--pcm-passthrough). Both branches now reach the
DSP at the same full scale, so the limiter, the true-peak skip gate and the meters mean
the same thing on both. Retained here for the two lessons rather than the defect:

- **The fix belonged at the contract, not the call site.** The predicted remedy was "a
  `<< 8` at the passthrough boundary", which would have worked and would have left the
  next producer free to make the identical mistake. `AiusPcmBlock` described its planes
  only as "signed 32-bit" and never declared a *sample* scale, so the DTS decoder's
  24-bit choice and the pass-through's 16-bit choice were both conformant. Declaring the
  scale once and deriving `AIUS_DSP_SAMPLE_MAX`/`_MIN` from it is the enforcement rung
  that a shift at one call site does not reach.
- **The golden-fixture update the entry predicted was not needed, and that was
  diagnostic.** All four `dsp-vectors` goldens are DTS, so they were byte-unchanged. The
  only test that moved was the pass-through's own — and it had been asserting the block
  against a copy of the producer's expression, which is why it printed "byte-exact"
  throughout. A defect that changes real behaviour while touching no golden vector is a
  sign the corpus does not cover that branch, not a sign the change was small.

**Do not fix it by widening the limiter threshold** — recorded because that remains the
tempting wrong answer if the symptom ever recurs on the input side.

**The decoders use the DTCM arena the image reserves — since 2026-07-28, grown for
E-AC-3.** 218,688 B is set aside (instance + 32,768 B DSP staging carve + the
180,224 B codec slot, `src/optical_main.cpp:1402`), and the `--wrap` shim that routes
the vendored decoders' allocation into it is live (`platformio.ini:104`; the wrap set
was `build_unflags`'d until 2026-07-28, which left the decoder allocating from the
OCRAM heap with most of the then-reserved arena never touched). Exactly one decoder is
resident in the slot at a time — E-AC-3 high-water 179,232 B, DTS 5.1 core 62,560 B,
AC-3 27,024 B — swapped on a Pc family change
(`lib/aius_pipeline/include/aius_pipeline.h:138`);
[`aius-eac3-support.md`](../plans/aius-eac3-support.md) phase 4 carries the funding
arithmetic and why growing or relocating this arena is not the free move it appears to
be.

**All five bypass flags now work; two of them did nothing until 2026-07-26.**
`features.matrix` and `features.output_eq` were parsed, stored, round-tripped through
`GET`/`PUT /api/dsp/config` and serialised into the status JSON while `apply_config()`
read neither. Output-EQ bypass appeared to work by leaving `output_eq[]` empty — which
is agreement rather than function — and matrix bypass had no mechanism at all.

`output_eq` is now the section count's selector, since zero sections is already the
bypass. **`matrix` needed its meaning decided**, because the downmix is structurally
unavoidable: six planes have to become two, so a bypassed matrix cannot simply not run.
It is defined as **FL→L, FR→R at unity** — zero centre, zero surround, zero LFE, unity
headroom — which is exactly what the accumulate loop in `aius_dsp_process` reduces to
with those values, so it needs no second code path. Substituting the *default* matrix
was the alternative and is rejected: it would make the flag mean "ignore my settings"
rather than "remove this stage", and would leave no way to hear the stereo pair without
the fold. The flags exist "for latency and diagnostic comparisons", and that is the
comparison worth having.

**~~There is still no way to measure output quality on this device.~~ Closed
2026-07-27** by item 3's software-tap loopback — see
[§Measured output quality](#measured-output-quality--the-loopback-figures). What
remains open is the narrower residue: the tap shares the transmit register's sample
grid, so it is immune to physical clock error by construction, and the clock/content
*ratio* surface — the class the 1.4-semitone pitch error lived in — still needs a
physical loop feeding the S/PDIF output back into the input. That loop is now owned by
the optical bench bring-up:
[`proto-optical-wiring.md`](../guides/proto-optical-wiring.md) step 5 patches the
OTJ-5 to the ORJ-5 across real glass and re-runs the item 3 measurement flow.

**Also absent, and recorded so nobody assumes otherwise:** no DC blocker; no
main-channel bass management or crossover (the LFE fold-in's 120 Hz LR4 band-limits
only the LFE plane); no DTS dialnorm/DRC handling (`core_decoder.h:61` parses
`drc_present` and the dynamic-range data is read and discarded, so loudness varies
between sources). ~~No surround decorrelation~~ — closed 2026-08-04 by item 4
([stage 6](#stage-6--the-downmix-matrix-lfe-fold-in-and-surround-decorrelation)),
though the shipped config leaves `surround_delay_ms` at 0, so the fold is undelayed
until an operator sets it.

## Figures that are not measured

Every number in this document that is not read from the linked image or the device:

- **Block period, 10,666.67 µs / 6,400,000 cycles** — *derived* from 512 frames,
  48,000 Hz and 600 MHz.
- **Block rates, 375/s and 93.75/s** — *derived* the same way.
- **`AiusLimiter` ≈ 4,200 B, the decorrelator ring 4,608 B, and the arena's 218,688 B
  total** — *derived* from struct members and compile-time constants, not individual
  symbol sizes. The containing `g_audio_processor`'s 5,784 B is a 2026-07-29
  measurement that predates the LFE fold and decorrelator state.
- **Pipeline, IEC parser and dcadec `ta` subdivision inside the 131,072-byte arena**
  — not measured individually. They are arena allocations, not heap objects;
  `aius_dts_get_metrics().arena_high_water` would report decoder use but is not
  currently surfaced on a wire field.
- **`enqueue()`'s total stack frame** — the two output planes are 4,096 B of it,
  measured from their declared size; the frame's true depth is not measured here.
  `firmware-io.md` states 4,096 B, which is the arrays rather than the frame.
- **The output stage's isolated 16-bit vs 24-bit cost, ~51,000 → ~6,000–7,000 cycles** —
  *estimated* by instruction counting. The whole-path delta of −72,675 cycles that
  supersedes it **is** measured.
- **OCRAM line-fill cost in the transmit ISR, under 1,000 cycles per interrupt** —
  *estimated* from 32 line fills at 20–30 cycles.
- **Priming window 229 µs, lookahead 96 samples, ring capacity 42.67 ms** — *derived*
  from tap counts, milliseconds and frame counts at 48 kHz.
- **The 48.16 dB PCM-vs-DTS level step** — *derived* as `20*log10(2^23 / 2^15)`.
- **The −140 dB inter-stage requantisation floor** — a historical figure: it described
  the per-stage rounding the one-quantize chain removed on 2026-07-28, and was
  host-proven only (`scripts/test.sh --suite measure`'s pure-sine canary read the
  analysis floor near −140 dB on 24-bit synthesis); no 16-bit stimulus can verify the
  chain's floor on-device, so the loopback figures above bottom out at the stimulus
  floor instead, by design.

Everything else — every bank figure, every object size, every ITCM function size, and
both DWT cycle counts — comes from the ELF or the board.

## Contradictions found against the plans

This is the first end-to-end read of the chain since three of its stages changed on
2026-07-26. Each item below is a place where a checked-in description disagrees with the
source, verified against the source.

1. **~~`aius_dsp.h` still says `-ffp-contract=off` is missing.~~ Closed** — the header
   now reads "THAT PRECONDITION IS NOW MET ON BOTH SIDES"
   (`lib/aius_dsp/include/aius_dsp.h:75`). Originally: lines 58–73 read "THAT
   PRECONDITION IS CURRENTLY UNMET ON THE TARGET" while the flag had already landed in
   `[common] build_flags`.
2. **~~`aius_downmix_config_default()`'s doc comment says "unity headroom."~~ Closed** —
   the comment now describes the shipped default in full ("the LFE folded in at −10 dB
   through a 120 Hz LR4 low-pass, `AIUS_DSP_HEADROOM_DEFAULT`, and layout_aware ON",
   `lib/aius_dsp/include/aius_dsp.h:433`).
3. **`firmware-io.md` §S/PDIF input still places the playback ring in DTCM.** It says the
   processor object holds "a 2,048-frame stereo ring at 16,384 B", which its own
   §S/PDIF output then correctly describes as having moved to `DMAMEM`. The ring is not a
   member of `AiusAudioProcessor` any more (`src/optical_main.cpp:820`). The same
   paragraph's "9,280 bytes of stack" is superseded by the measured 25,376.
4. **`firmware-io.md`'s measured bank table is from an earlier image than its prose.**
   The table (revision `9f5b8dee8767`) reports DTCM free 9,280 and OCRAM free 432,832,
   while the prose two sections later correctly cites 293 kB of OCRAM free and the ring
   move. Current measured values are DTCM 25,376 and OCRAM 293,504.
5. **`aius-dsp-config-api.md` §Budget for all of it is stale within its own document.**
   It states "Measured 398,591–431,995 cycles … 6.2–6.8 % used" and "DTCM at **9,248
   bytes**", both superseded by the two measurement tables 80 lines above it (534,411 /
   565,024 and 25,376). A reader planning the next item against that paragraph would
   under-count the budget already spent and under-count the DTCM available.
6. **~~The feature-flag contract is broader than the implementation.~~ Closed
   2026-07-26** by `e88112d82`: all five flags are live in `apply_config()`
   (`src/optical_main.cpp:1067`, `:1072`, `:1092`) — the commit that made the two
   dead bypass flags work updated §Open but left this item standing.
7. **~~"The decode/DSP arena is in DTCM" does not hold.~~ Closed 2026-07-28** — the
   `--wrap` shim went live and every decoder allocation has come from the DTCM arena
   since; the arena has since grown to hold the E-AC-3 slot — see
   [§Open at this revision](#open-at-this-revision). "No heap on the process path"
   held throughout.
8. **~~The PCM passthrough level step is undescribed anywhere.~~ Closed 2026-07-26** —
   the fix declared the sample scale at the contract (`aius_codec.h` §SAMPLE SCALE)
   and [§Stage 4c](#stage-4c--pcm-passthrough) records both the defect and the lesson.

As of the 2026-08-04 audit only items 3, 4 and 5 remain open; all are stale
*documents* rather than stale source, and items 3 and 4 are addressed by
[`firmware-io.md`](firmware-io.md)'s own staleness caveats.

## What a future maintainer must update here

The roadmap's obligation is that **every audio-quality item updates this document in the
commit that ships the change.** Concretely, when the next item lands:

- **The stage inventory table and the mermaid diagram** — a new stage means a new row and
  a new node, with its module, function, arithmetic domain and buffer bank. A stage that
  becomes bypassed rather than deleted stays listed and is marked bypassed, as the
  16-bit output arm is.
- **[§The per-block budget](#the-per-block-budget)** — re-read `dsp_cycles_last` and
  `dsp_cycles_max` off the device and add a row to the whole-path difference table. Do
  not replace a measured figure with a prediction; the prediction columns exist so the
  two can be compared afterwards.
- **[§Memory by bank](#memory-by-bank)** — re-run `tools/memory/mem_report.py --elf` and
  `arm-none-eabi-nm -S` and refresh both tables, including DTCM free-for-locals and
  OCRAM free. A `DMAMEM` change also moves `g_aius_spdif_tx24`'s neighbours: adding the
  16,396-byte ring at `0x20200000` pushed `g_aius_boot_slots` from `0x20201120` to
  `0x20205140` and invalidated the retained ledger, so **check the retained-slot guard
  after any `DMAMEM` addition.**
- **[§Channel layout and downmix rules](#channel-layout-and-downmix-rules)** — the LFE
  fold-in changes the `lfe` row and adds a low-pass stage; that table is the single
  authoritative statement and must not be duplicated elsewhere.
- **[§Fixed-point and floating-point boundaries](#fixed-point-and-floating-point-boundaries)** —
  any new stage adds a row, and removing inter-stage requantisation would rewrite most
  of the table at once.
- **[§Decisions recorded](#decisions-recorded-with-their-alternatives)** — record the
  alternative that was *not* taken and why. A decision recorded only as its outcome gets
  re-litigated.
- **[§Open at this revision](#open-at-this-revision)** — strike what the item closes.
  An open-items list that still names a fixed defect misleads the next session into
  fixing it twice.
- **[§Figures that are not measured](#figures-that-are-not-measured)** — add or remove
  entries so the edge of the evidence stays visible in one place.

**The provenance line in [§How to read this](#how-to-read-this) is part of the update.**
It names the revision the ELF was built from and the build revision the device figures
came from; leaving it stale is what turns this document from a record into a rumour.

## Related documents

- [`firmware-io.md`](firmware-io.md) — every I/O operation, its buffers and bank, and
  why none of them can disturb this path. The companion, not a duplicate.
- [`aius-dsp-config-api.md`](../plans/aius-dsp-config-api.md) — the config surface,
  bounds, meter schema, and the audio-quality roadmap this document is the record for.
- [`aius-sample-rate-following.md`](../plans/aius-sample-rate-following.md) — the
  delivered runtime-rate, PLL-following and hardware-acceptance contract.
- [`aius-network-hardening.md`](../plans/aius-network-hardening.md) — the phase sequence
  this document lands in, and the live-hardware session that will re-measure it.
- [`aius-replay-manager.md`](../plans/aius-replay-manager.md): the retired replay
  source (history).
