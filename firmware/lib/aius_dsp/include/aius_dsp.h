#ifndef AIUS_DSP_H
#define AIUS_DSP_H

#include <stddef.h>
#include <stdint.h>

#include "aius_arena.h"
#include "aius_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Aius output DSP: 5.1-to-stereo downmix + parametric EQ.
 *
 * Turns a decoded PCM block (1..6 channels, planar, 24-bit range sign-extended
 * into int32, at the block's own sample rate) into a 2-channel stereo block at
 * that same rate. NO FUNCTION HERE ASSUMES A SAMPLE RATE: every rate-dependent
 * coefficient takes `fs` as a parameter. The path is the stages below, which the
 * caller can run together (aius_dsp_process) or separately:
 *
 *   1. Downmix   (aius_dsp_downmix)  N channels -> L/R per ITU-R BS.775 /
 *                                    ATSC A/52 (see AiusDownmixConfig),
 *                                    including the LFE fold-in and its 4th-order
 *                                    Linkwitz-Riley low-pass (AiusLfeFold). The
 *                                    fold is part of the matrix rather than a
 *                                    stage of its own: it contributes one term to
 *                                    the same accumulator, so it adds no
 *                                    requantisation and no latency.
 *   2. EQ        (aius_dsp_eq_run)   a Direct Form I biquad cascade per channel,
 *                                    coefficients supplied by the caller.
 *   3. Limiter   (aius_dsp_limiter_run)  true-peak safety limiter tail.
 *
 * ONE QUANTIZE PER SAMPLE, AT THE END (defect: inter-stage clipping, fixed
 * 2026-07-28). Samples travel this chain as double from the moment
 * aius_dsp_process converts the decoded int32 planes into the caller's double
 * scratch until the single aius_quantize into the output block after the
 * limiter. No stage in between rounds or clamps: an input-EQ boost that
 * overshoots full scale inter-stage is absorbed by the downmix headroom pad
 * instead of hard-clipping at a cascade boundary, and the limiter sees TRUE
 * unclipped peaks. The 24-bit wire ceiling exists only at that final quantize.
 * Stage helpers therefore take double planes; the int32 <-> double boundary
 * belongs to aius_dsp_process alone.
 *
 * ...and then, at the boundary to the playback engine, the separate output
 * stage:
 *
 *   4. Out width (aius_out_stage_run) the transmit word. At
 *                                    AIUS_DSP_OUT_WIDTH_24 -- the default, and
 *                                    what the S/PDIF transmit registers take --
 *                                    it is a lossless copy that needs no
 *                                    dither. At AIUS_DSP_OUT_WIDTH_16 it is
 *                                    aius_out16_sample()'s 24 -> 16 bit
 *                                    reduction (TPDF dither, round to nearest,
 *                                    saturation) scaled back into transmit
 *                                    units. NOT part of aius_dsp_process: the
 *                                    24-bit block is the module's output, and
 *                                    the word width belongs to whichever engine
 *                                    consumes it.
 *                (aius_out16_run)    the same reduction into int16 planes, for
 *                                    a sink whose words really are 16 bits
 *                                    wide.
 *
 * ARITHMETIC / DETERMINISM. All processing is IEEE-754 double precision. Double
 * was chosen over fixed-point because a biquad's feedback path is precision
 * sensitive and double keeps the design headroom trivial while every 24-bit
 * input sample is represented exactly. The Teensy 4.1 (Cortex-M7) has a
 * hardware double-precision FPU, so host (x86-64 SSE2) and target run the same
 * IEEE-754 correctly-rounded +, -, *, / and are bit-identical PROVIDED the
 * compiler does not contract multiply-add into an FMA and does not use x87
 * extended precision. Build both host and target with -ffp-contract=off (and,
 * on any x87 host, -mfpmath=sse) to guarantee bit-exactness. This module
 * itself writes no fused multiply-add and holds all state in double.
 *
 * THAT PRECONDITION IS NOW MET ON BOTH SIDES. scripts/test.sh passes
 * -ffp-contract=off for the dsp and dsp-vectors suites, and platformio.ini's
 * [common] build_flags now pin it too (platformio.ini:95). It was a real
 * divergence until 2026-07-26, not a theoretical one: GCC defaults to
 * -ffp-contract=fast under -std=gnu++17, and disassembling
 * .pio/build/teensy41-optical/lib1a3/aius_dsp/aius_dsp.c.o showed 25 vfma.f64
 * instructions -- in truepeak_push, run_cascade, aius_dsp_downmix and
 * aius_dsp_limiter_run -- where the host emitted a separate vmul and vadd, so the
 * downmix and EQ had not in fact been bit-identical host-to-target. The fix was
 * that one flag. (One contraction had been benign by construction: the vfma in
 * aius_out16_sample multiplies an exact integer by a power of two, so fused and
 * unfused agree bit-for-bit -- see the output-stage note below.)
 *
 * EVERY NEW STAGE INHERITS THE SAME OBLIGATION, and the LFE fold-in crossover is
 * the newest: a 4th-order IIR in the matrix accumulator is precision-sensitive in
 * exactly the way the biquad feedback path is, its state is double, and its
 * recurrence is written once (biquad_step in aius_dsp.c) so the EQ and the
 * crossover cannot diverge in arithmetic order.
 *
 * NO HEAP ON THE PROCESS PATH. Every function here is allocation-free. Output
 * planes and biquad state are caller-owned; aius_dsp_alloc_stereo is a
 * convenience that carves an output block out of a caller-owned arena once,
 * before the steady-state loop.
 */

/* Output sample range: the AiusPcmBlock sample scale, which is also this stage's
 * input scale, so the DSP neither widens nor narrows. Derived rather than
 * restated -- a second copy of the number is how the pass-through and the decoder
 * came to disagree by 48 dB (see aius_codec.h §SAMPLE SCALE). Retarget there. */
#define AIUS_DSP_SAMPLE_MAX AIUS_PCM_SAMPLE_MAX
#define AIUS_DSP_SAMPLE_MIN AIUS_PCM_SAMPLE_MIN

/* Downmix coefficient defaults (ITU-R BS.775 / ATSC A/52 "Lo/Ro" style).
 *   L' = FL + center*FC + surround*SL + lfe*LFE
 *   R' = FR + center*FC + surround*SR + lfe*LFE
 * center = surround = 1/sqrt(2) (~-3 dB) is the documented, standard default:
 * mixing FC and each surround down 3 dB preserves acoustic power when the two
 * summed sources are uncorrelated. M_SQRT1_2 is not portable under -std=c11, so
 * the value is spelled out. */
#define AIUS_DSP_DOWNMIX_CENTER_DEFAULT   0.70710678118654752440  /* 1/sqrt(2) */
#define AIUS_DSP_DOWNMIX_SURROUND_DEFAULT 0.70710678118654752440  /* 1/sqrt(2) */

/* LFE handling. ATSC A/52 and ITU-R BS.775 EXCLUDE the LFE from the stereo
 * downmix they specify, and this module's default did the same (0.0) until the
 * audio-quality roadmap's item 2. Excluding it is right when a subwoofer follows
 * the decoder and wrong here: THIS DEVICE DOWNMIXES TO STEREO, so a discarded
 * LFE is the bottom octave of film content thrown away — the impact and the
 * rumble — for every listener on ordinary speakers. The default is now a fold-in
 * (AIUS_DSP_LFE_GAIN_DEFAULT below), low-passed by the crossover in
 * AiusDownmixConfig.
 *
 * THE LEVEL IS THE HARD PART, not the filter, and it has a standards answer that
 * this device deliberately does not use.
 *
 *   THE REFERENCE. BS.775 gives the LFE channel 10 dB of additional gain in its
 *   REPRODUCTION path relative to each main channel; that is what buys the
 *   effects channel its extra in-band headroom, and it is a property of the
 *   monitor chain, not of the recorded samples. A stereo fold-in has no such
 *   path, so preserving the mix's intended acoustic balance means re-applying
 *   that +10 dB to the folded signal. ATSC A/52 encodes exactly this: its
 *   optional LFE downmix is scaled by `lfemixlevcod`, gain = 10 - code dB, so
 *   the standard's reference fold-in gain at code 0 is +10 dB, attenuated from
 *   there by the encoder's own judgement. +10 dB is therefore the acoustically
 *   "correct" number, and unity is NOT a neutral choice sitting between two
 *   options — it is already 10 dB below the reference.
 *
 *   WHY NOT +10 dB HERE. Three reasons, in the order they bind:
 *     - Headroom. At lfe = 3.1623 the clip-free headroom
 *       (aius_downmix_clipfree_headroom) falls from 0.414214 to 0.179, 8.9 dB
 *       below the 0.5 this device ships. Honouring the reference costs the WHOLE
 *       PROGRAMME that 8.9 dB, to accommodate a channel whose content is
 *       intermittent.
 *     - The mains already carry their own low frequency. A 5.1 mix is not
 *       bass-managed at the encode side: the main channels are full range and
 *       the LFE is an ADDITIONAL effects channel. A stereo downmix therefore
 *       already reproduces every channel's low frequency except the LFE's, so
 *       folding the LFE in at the reference gain gives the effects channel 10 dB
 *       more weight than the mix's own bass.
 *     - The speakers this serves cannot use it. The reference gain assumes a
 *       driver with 10 dB of spare excursion below 120 Hz. Ordinary stereo
 *       speakers turn that into intermodulation distortion, not weight.
 *
 *   THE CHOICE: -10 dB (AIUS_DSP_LFE_GAIN_MINUS10DB, 0.316228), i.e. 20 dB below
 *   the A/52 reference. It is the level this module already named for exactly
 *   this case before it became the default, so the change is a default flip
 *   rather than a new invention, and it puts the LFE's contribution just below
 *   the mains' own low frequency — added weight on impacts without re-balancing
 *   the mix. What it costs is one bounded, arithmetic thing: the theoretical
 *   worst case a fully correlated full-scale 5.1 input reaches through the matrix
 *   at the shipped 0.5 headroom goes from +1.635 dBFS to +2.700 dBFS, which is
 *   the limiter's job either way (see AIUS_DSP_HEADROOM_DEFAULT). The same
 *   arithmetic is what rules the reference gain out: +10 dB would make that
 *   +8.905 dBFS, i.e. nearly 9 dB of permanent gain reduction on any loud
 *   passage, which is a limiter holding the programme down rather than catching
 *   overs.
 *
 *   WHAT THE CORPUS DID NOT DECIDE, said plainly because the headroom default
 *   above WAS chosen on corpus evidence and this was not. Measured through this
 *   matrix at 0.5 headroom, EVERY candidate from 0.0 to +10 dB clears full scale
 *   on all four committed vectors — the binding vector (surround51_impulse) moves
 *   only from -0.791 to -0.790 dBFS at -10 dB and to -0.782 dBFS at +10 dB,
 *   because its LFE plane peaks at 35,202 and does not coincide with the
 *   full-scale impulse in the mains. So the corpus constrains the HEADROOM
 *   question and cannot constrain the LEVEL question; no committed vector carries
 *   hot LFE under a loud transient. The level rests on the standards and the
 *   worst-case arithmetic above, and a vector that does present that coincidence
 *   would be the way to test it.
 *
 * Still a clearly-marked configurable (AiusDownmixConfig.lfe) and still a PRODUCT
 * DECISION; it is now an evidenced one, and 0.0 remains available and remains
 * exactly the pre-fold behaviour, bit for bit. */
#define AIUS_DSP_LFE_GAIN_MINUS10DB 0.31622776601683793320  /* 10^(-10/20) */
#define AIUS_DSP_LFE_GAIN_DEFAULT   AIUS_DSP_LFE_GAIN_MINUS10DB
#define AIUS_DSP_LFE_GAIN_EXCLUDED  0.0  /* BS.775/A52: no LFE in the downmix */

/* --- the LFE fold-in crossover: a 4th-order Linkwitz-Riley low-pass -------- */

/* WHY A LOW-PASS AT ALL, and it is not decoration. LFE content above the
 * crossover is not meant to be heard: the channel exists to carry 20-120 Hz
 * effects, and everything above that in the plane is either production spill or
 * reconstruction artefact. Folding it in unfiltered adds mud rather than weight,
 * and — the concrete reason for this decoder rather than a general principle —
 * THE DTS CORE DOES NOT BAND-LIMIT ITS LFE PLANE TO 120 Hz. The core carries LFE
 * decimated by 128 or by 64 and reconstructs it with an interpolation FIR
 * (vendor/dcadec-core, interpolate_lfe_*), so the reconstructed plane's Nyquist
 * is fs/256 or fs/128 — 187.5 Hz or 375 Hz at 48 kHz. Up to 1.6 octaves above
 * the LFE's intended band edge therefore arrives in that plane and would go
 * straight into the mains. The low-pass is what makes the fold-in a bass fold
 * rather than a leak.
 *
 * ORDER: 4, as two cascaded 2nd-order Butterworth sections (Q = 1/sqrt(2) each) —
 * a Linkwitz-Riley LR4, 24 dB/octave, -6.02 dB at the corner. Three reasons, and
 * the first is the one that decides it:
 *   - 12 dB/octave leaves too much of the thing the filter exists to remove.
 *     Measured at a 120 Hz corner and 48 kHz, a single Butterworth section is
 *     only -8.43 dB at 187.5 Hz and -19.84 dB at 375 Hz — the two DTS LFE
 *     reconstruction Nyquists above. LR4 is -16.85 dB and -39.69 dB. Since the
 *     entire justification is "that content is not meant to be heard", 2nd order
 *     does not clear the bar. And the content is really there: 21% of the
 *     committed corpus's LFE-plane POWER sits above the corner on both 5.1
 *     vectors (surround51_tones 21.3%, surround51_impulse 20.9%), and
 *     surround51_tones is nominally a 60 Hz tone — where the LR4 should remove
 *     0.53 dB and in fact removes 1.04 dB. The surplus is reconstruction
 *     imaging, measured rather than assumed.
 *   - LR4 is the bass-management crossover shape. Its magnitude at the corner is
 *     exactly -6 dB rather than -3, which is what makes a matched LR4 low-pass
 *     and high-pass sum to unity magnitude, in phase. So if a real crossover is
 *     ever built (see §BASS MANAGEMENT below), this low-pass is already its
 *     correct half rather than something to be replaced.
 *   - It costs two biquads on ONE plane, not per output channel: the fold is
 *     symmetric (the same filtered value is summed into L and into R), because
 *     the LFE is one mono channel and has no image to preserve.
 *
 * CORNER: 120 Hz by default. It is the LFE band edge every relevant document
 * uses — BS.775's LFE band, A/52's nominal LFE bandwidth, and the ubiquitous
 * consumer-receiver default — and at LR4 the corner is 6 dB down, so the region
 * the fold-in exists to recover passes essentially unaltered (measured: -0.007 dB
 * at 20 Hz, -0.107 dB at 40 Hz, -0.53 dB at 60 Hz, -1.57 dB at 80 Hz) while the
 * band above it is gone within an octave. Configurable per
 * AiusDownmixConfig.lfe_crossover_hz; a corner is a room-and-speaker property and
 * pinning it in firmware would be wrong.
 *
 * DESIGNED FROM `fs`, NEVER FROM AN ASSUMED RATE. The sections are derived from
 * the (fs, corner) pair at apply time, and re-derived when either changes, so the
 * corner is the same ANALOG frequency at every rate (the |H(f0)| = 0.5 identity
 * below is exact and rate-independent). That is the same contract every other
 * rate-dependent coefficient in this module holds to. */
#define AIUS_DSP_LFE_CROSSOVER_DEFAULT_HZ 120.0
#define AIUS_DSP_LFE_CROSSOVER_MIN_HZ      20.0
#define AIUS_DSP_LFE_CROSSOVER_MAX_HZ     500.0

/* Two cascaded Butterworth sections IS the LR4; the count is named so the state
 * size and the order cannot drift apart. */
#define AIUS_DSP_LFE_CROSSOVER_SECTIONS 2

/* Butterworth Q for each section of the cascade. 1/sqrt(2) per section gives the
 * maximally-flat, monotonic response whose product is the LR4 alignment; the RBJ
 * low-pass satisfies |H(f0)| = Q exactly at any rate, so the cascade is exactly
 * -6.02 dB at the corner and exactly unity at DC. Both are asserted by the dsp
 * suite as known answers rather than tolerances. */
#define AIUS_DSP_LFE_CROSSOVER_Q 0.70710678118654752440  /* 1/sqrt(2) */

/* The crossover's coefficients and its running state, defined after the biquad
 * types below. Forward-declared here because AiusDownmixConfig refers to it. */
typedef struct AiusLfeFold AiusLfeFold;

/* Overall post-downmix gain applied to L' and R' before the (final) clamp.
 *
 * WHY 0.5 AND NOT 1.0. When this default was chosen the downmix quantized and
 * SATURATED into the 24-bit range at the end of aius_dsp_downmix (and the EQ
 * cascade saturated again), so every later stage — the limiter included — saw
 * an already-clamped signal, and the headroom, not the limiter, was what kept
 * the matrix sum inside full scale. THE PIPELINE NOW CARRIES AN UNCLAMPED
 * DOUBLE INTERMEDIATE (see §ONE QUANTIZE above), so "unity headroom, let the
 * limiter catch the overs" is no longer structurally ruled out — but 0.5 stays:
 * the corpus evidence below still supports it, the exact-power-of-two multiply
 * is still bit-exact, and the pad is now also what gives an input-EQ boost its
 * working room before the wire ceiling.
 *
 * The committed DTS corpus (tools/dts_vectors/vectors, 4 vectors x 5120 frames,
 * measured through this matrix at unity headroom, LFE EXCLUDED — this table
 * predates the fold-in and is kept as the baseline the fold-in is measured
 * against) distributes like this:
 *
 *   vector               sample peak   p50      p99.9   samples over full scale
 *   stereo_tones          -22.50 dBFS  -26.33   -22.50   0
 *   surround51_tones      -17.17 dBFS  -26.34   -17.17   0
 *   stereo_impulse         -2.05 dBFS -100.86   -33.46   0
 *   surround51_impulse     +5.23 dBFS  -86.31   -20.81  10 of 10240
 *
 * So exactly one vector clips, on 10 of its 10240 samples, and it is the
 * synthetic all-channel-simultaneous full-scale impulse: 99.9% of even that
 * vector sits below -20 dBFS. Paying the theoretical clip-free 0.414214
 * (-7.66 dB, aius_downmix_clipfree_headroom) to accommodate those 10 samples
 * would make the device audibly quiet against every other source.
 *
 * 0.5 is the choice: it clears the whole corpus (worst case becomes -0.79 dBFS)
 * with 0.79 dB spare, costs 6.02 dB rather than 7.66, and is an exact power of
 * two, so the headroom multiply is bit-exact (an exponent decrement, no rounding
 * at all) on host and target alike. It is deliberately NOT clip-free in theory —
 * a fully correlated full-scale 5.1 input still reaches 2.4142 * 0.5 = +1.63
 * dBFS — which is why the shipped limiter default is ENABLED (see
 * AiusLimiterConfig): modest headroom plus an active true-peak limiter, not
 * clip-free headroom with the limiter off.
 *
 * RE-MEASURED WITH THE LFE FOLDED IN, because that default was chosen against
 * evidence and adding a sixth contributor to the matrix sum obliges the same
 * evidence again. 0.5 STILL CLEARS THE CORPUS, and comfortably. Measured through
 * this matrix at the shipped fold (lfe = 0.316228, 120 Hz LR4) and 0.5 headroom:
 *
 *   vector               sample peak       true peak   samples on a rail
 *   stereo_tones         -28.520 dBFS      -28.508     0   (no LFE: unchanged)
 *   stereo_impulse        -8.073 dBFS       -8.073     0   (no LFE: unchanged)
 *   surround51_tones     -22.218 dBFS      -22.200     0
 *   surround51_impulse    -0.790 dBFS       -0.790     0
 *
 * The binding vector is still surround51_impulse and it moved by 0.001 dB, from
 * -0.791 to -0.790 dBFS: 0.79 dB of spare margin survives the fold-in intact.
 * surround51_tones is where the fold-in is actually audible in the corpus, and it
 * rises 0.97 dB (-23.186 -> -22.218 dBFS) without approaching the rail.
 *
 * WHAT DID CHANGE is the theoretical worst case, not the measured one: correlated
 * full scale now reaches 2.7303 * 0.5 = +2.70 dBFS instead of +1.63, and 1.09 dB
 * of transient headroom beyond that (see aius_downmix_clipfree_headroom). The
 * limiter absorbs it — that pairing is exactly why it ships enabled — and the
 * -1.0 dBFS threshold has not moved. No headroom change was made and none is
 * needed; 0.5 was chosen against evidence and the evidence still supports it.
 *
 * Still a PRODUCT DECISION, now an evidenced one; a caller wanting the
 * theoretical guarantee instead sets headroom from
 * aius_downmix_clipfree_headroom and may then turn the limiter off.
 *
 * IT IS THE SUMMING-LAYOUT PAD, AND ONLY THAT (2026-07-28). Every number above
 * -- the corpus, the 0.5 choice, the clip-free math -- describes a matrix that
 * SUMS: FC into both sides, SL/SR onto the fronts, the LFE folded in. A layout
 * that sums nothing cannot overshoot, so aius_dsp_downmix resolves an EFFECTIVE
 * headroom per block FROM THE INPUT MASK ALONE (see
 * AiusDownmixConfig.layout_aware): mono and stereo pass-through (FL|FR) take
 * exactly 1.0, and any layout carrying FC, LFE, SL or SR takes this value. A
 * <= full-scale input through a unity route is <= full scale at the output, so
 * removing the pad there costs no protection whatsoever -- and paying it cost
 * every stereo source 6.02 dB for nothing until this landed. */
#define AIUS_DSP_HEADROOM_DEFAULT 0.5

/* Downmix configuration. Fill from aius_downmix_config_default() then override.
 *
 * The last two fields are the LFE fold-in crossover, and they split into a VALUE
 * (the corner) and a POINTER TO STATE (the filter memory) for a reason that is
 * easy to get wrong: this struct is copied by value into a per-block stack local
 * on the audio path, so any filter state embedded in it would be discarded every
 * block. The state therefore lives wherever the caller keeps its own long-lived
 * DSP objects, and this struct only points at it.
 *
 * THE THREE REACHABLE COMBINATIONS, all safe:
 *   lfe == 0.0                       -> the LFE branch is skipped entirely. Bit
 *                                       for bit the pre-fold-in output, whatever
 *                                       the other two fields say. This is the
 *                                       negative control, and it is exact.
 *   lfe != 0, crossover > 0, fold    -> the fold-in: LFE low-passed by the LR4 at
 *                                       `lfe_crossover_hz`, then scaled by `lfe`
 *                                       and summed into both L and R.
 *   lfe != 0, crossover == 0         -> an UNFILTERED fold, no state needed. Kept
 *                                       reachable so a caller that band-limits
 *                                       upstream is not forced through a second
 *                                       filter, and so the filter's effect is
 *                                       measurable against it. The JSON config
 *                                       schema deliberately does NOT permit it
 *                                       (lfe_crossover_hz is bounded to
 *                                       [20, 500]): it is an API capability, not
 *                                       a product option, because it is the mud
 *                                       the crossover exists to prevent.
 *
 * AND THE ONE MISCONFIGURATION, which FAILS CLOSED: lfe != 0 and
 * lfe_crossover_hz > 0 with fold == NULL skips the LFE, exactly as lfe == 0
 * would. It does NOT fall back to an unfiltered fold and it does NOT return an
 * error. Both alternatives were considered and rejected: an unfiltered fallback
 * would make the muddy fold reachable by omission (a wiring mistake that still
 * "works" and sounds worse is the worst failure mode this stage has), and an
 * AIUS_INVALID_ARG would take the whole audio path down on every 5.1 block
 * because a cold config helper forgot a pointer — and audio hot-path reliability
 * decides every trade-off here. Failing closed lands on BS.775's own default,
 * which is a defensible output rather than a broken one, and it is audible as
 * "no sub-bass" the moment anyone listens. */
typedef struct {
    double center;   /* FC gain into both L and R */
    double surround; /* SL->L and SR->R gain */
    double lfe;      /* LFE gain into both L and R, applied AFTER the crossover */
    double headroom; /* output gain before the clamp, on SUMMING layouts only
                      * (see layout_aware and §AIUS_DSP_HEADROOM_DEFAULT) */
    double lfe_crossover_hz; /* LR4 low-pass corner; 0 = fold unfiltered */
    AiusLfeFold *lfe_fold;   /* crossover state; required when the corner is > 0 */
    /* Nonzero: `headroom` is the SUMMING-layout pad, and aius_dsp_downmix
     * resolves 1.0 for a block whose layout sums nothing (mono, and stereo
     * FL|FR). Zero: the legacy uniform pad, applied to every layout including
     * mono -- the diagnostic lever, and what a config written before this field
     * existed asks for.
     *
     * ZERO-INIT CAVEAT: this struct is often memset to zero and then filled
     * field by field, and a zeroed `layout_aware` is the LEGACY UNIFORM PAD, not
     * the shipped behaviour. Construct through aius_downmix_config_default(),
     * which sets it, or set it explicitly. */
    int layout_aware;
} AiusDownmixConfig;

/* The shipped default: BS.775/A52 -3 dB center+surround, the LFE folded in at
 * -10 dB through a 120 Hz LR4 low-pass, AIUS_DSP_HEADROOM_DEFAULT, and
 * layout_aware ON. `lfe_fold` is NULL — the caller owns the crossover state and
 * points this at it (see the fail-closed note above), which is the one field a
 * value-returning constructor cannot supply. */
AiusDownmixConfig aius_downmix_config_default(void);

/* The headroom that makes a fully correlated full-scale input downmix to exactly
 * full scale (no saturation) for the given config: 1/(1 + center + surround +
 * |lfe|), clamped to (0, 1].
 *
 * IT IGNORES THE CROSSOVER, and that is a deliberate, documented approximation
 * rather than an oversight. The formula is a FREQUENCY-DOMAIN bound on a
 * correlated full-scale input, and the LR4 low-pass has |H(f)| <= 1 everywhere
 * (Butterworth cascades are monotonic, and the measured DC gain is exactly
 * 1.000000000), so no steady tone can make the folded LFE contribute more than
 * the flat fold this formula assumes. What the bound does not cover is a
 * TRANSIENT: the measured l1 norm of the LR4's impulse response is 1.133016 at a
 * 120 Hz corner and 48 kHz (+1.085 dB; step-response overshoot +0.526 dB), so a
 * worst-case transient can push the folded contribution 1.085 dB past the
 * flat-fold term. At the default lfe = 0.316228 that widens the whole matrix bound
 * from 2.730335 to 2.772566 — 0.133 dB — against a shipped headroom (0.5) that
 * already sits 2.70 dB above this function's own answer for that config and is
 * backed by an enabled true-peak limiter. The bound was never rigorous under EQ
 * either; the measured corpus peak is the evidence that matters, and it is
 * recorded at AIUS_DSP_HEADROOM_DEFAULT. */
double aius_downmix_clipfree_headroom(const AiusDownmixConfig *cfg);

/* Double-plane view of a planar block: the inter-stage carrier of the
 * single-quantize chain. Meta mirrors AiusPcmBlock; the planes are the
 * caller's double scratch (see AiusDspConfig.scratch). */
typedef struct {
    double  *planes[AIUS_MAX_CHANNELS];
    uint16_t channels;
    uint32_t frames;
    uint32_t sample_rate_hz;
    uint32_t channel_mask;
} AiusPcmBlockD;

/* Downmix `in` (1..6 channels, mask-driven) into the double planes `out_l` /
 * `out_r` (>= in->frames each). NO QUANTIZE: the result stays in double for
 * the stages after it; the caller performs the single final quantize.
 *
 * Channel routing is driven entirely by in->channel_mask (AIUS_CH_*), so
 * narrower inputs are handled without a mode flag:
 *   - mono   (channels == 1): the lone plane is copied to BOTH L and R at unity
 *            gain (and, with layout_aware set, NO headroom pad either: one
 *            replicated plane sums nothing), regardless of which mask bit is
 *            set. The LFE fold-in does NOT apply on this path: a one-channel
 *            block is a mono programme, not an effects channel, whatever its
 *            mask bit.
 *   - stereo (FL|FR):          FL->L, FR->R at unity and, with layout_aware set,
 *                              no pad either -> the route is an exact int32
 *                              round-trip through the whole chain.
 *   - 3.0/4.0/5.0/5.1 etc.:    every present channel routed per its AIUS_CH_*
 *                              rule above; absent channels contribute nothing.
 * Planes must be tightly packed to `channels` in AIUS_CH_* bit order, as the
 * decoder emits them.
 *
 * THE HEADROOM PAD IS RESOLVED PER BLOCK, FROM THE MASK ALONE. With
 * cfg->layout_aware set, cfg->headroom applies when the layout SUMS -- channels
 * > 1 and any of FC, LFE, SL, SR present -- and the effective gain is exactly
 * 1.0 otherwise. Nothing else is consulted: not the resolved plane pointers, not
 * the gain values. A block carrying an LFE plane at cfg->lfe == 0.0, or one
 * whose fold failed closed, therefore takes exactly the pad it would have taken
 * with the fold running -- which is what keeps "gain 0.0 is bit for bit the
 * pre-fold-in output" true across this change, when a level-dependent predicate
 * would have moved that output by 6 dB. With layout_aware clear, cfg->headroom
 * applies to every layout, which is the pre-2026-07-28 behaviour exactly.
 *
 * The LFE fold-in runs here, inside the matrix accumulation and in DOUBLE: the
 * crossover reads the LFE plane's integer sample and its filtered result is
 * summed straight into the l/r accumulators, so the fold adds NO intermediate
 * requantisation (there is exactly one quantize per output sample, as before).
 * cfg->lfe is applied AFTER the filter rather than before, which is
 * mathematically identical for a linear filter but means the crossover state is
 * held in raw sample units — so changing the LFE gain does not invalidate the
 * filter memory and needs no reset. The crossover advances only while an LFE
 * plane is present; a layout change that drops LFE freezes its state rather than
 * decaying it, and the next config apply resets it.
 *
 * in and out planes must not overlap. Returns AIUS_OK, or AIUS_INVALID_ARG
 * (null args/planes, channels/mask mismatch, channels outside
 * 1..AIUS_MAX_CHANNELS). Allocation-free. */
AiusStatus aius_dsp_downmix(const AiusDownmixConfig *cfg,
                            const AiusPcmBlockD *in,
                            double *out_l, double *out_r);

/* A Direct Form I biquad section, a0 normalized to 1:
 *   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]  */
typedef struct {
    double b0, b1, b2, a1, a2;
} AiusBiquad;

/* Per-channel, per-section delay memory. Zero-initialize before first use. */
typedef struct {
    double x1, x2, y1, y2;
} AiusBiquadState;

/* Standard RBJ audio-EQ-cookbook coefficient math. `fs` and `f0` in Hz (0 < f0 <
 * fs/2), `q` the quality factor (> 0), `gain_db` the shelf/peak gain. The
 * returned section has a0 normalized to 1. */
AiusBiquad aius_biquad_bypass(void);                                            /* identity: b0=1 */
AiusBiquad aius_biquad_lowpass(double fs, double f0, double q);
AiusBiquad aius_biquad_highpass(double fs, double f0, double q);
AiusBiquad aius_biquad_peaking(double fs, double f0, double q, double gain_db);
AiusBiquad aius_biquad_lowshelf(double fs, double f0, double q, double gain_db);
AiusBiquad aius_biquad_highshelf(double fs, double f0, double q, double gain_db);

/* The default EQ is bypass (flat): a single identity section, so the EQ path is
 * provable end-to-end without shaping the signal. Populates up to `cap` sections
 * and returns the count used (1). */
size_t aius_eq_default(AiusBiquad *sections, size_t cap);

/* --- LFE fold-in crossover state ------------------------------------------ */

/* The LR4 low-pass the fold-in runs the LFE plane through: its two designed
 * Butterworth sections, their delay memory, and the (corner, fs) pair they were
 * designed for. 160 bytes. Caller-owned and zero-initialized before first use,
 * like every other state object here; it belongs in the caller's long-lived DSP
 * object, NOT in a stack local — the audio path's DTCM free-for-locals zone is
 * the scarcest bank on the target and this state has to survive across blocks
 * anyway.
 *
 * `designed_hz`/`designed_fs` are what make the "designed from fs at apply time"
 * contract self-enforcing: aius_dsp_downmix compares them against the config's
 * corner and the block's rate and re-derives the sections (zeroing the state)
 * when either has moved. So the sections cannot silently belong to a rate the
 * engine no longer runs at, which is the failure the runtime-rate work in
 * aius-sample-rate-following.md would otherwise introduce. The re-derivation
 * costs two cos/sin pairs and happens at most once per change, never per block;
 * a caller that wants even that off the audio path calls aius_lfe_fold_init
 * itself from its cold config-apply path, which is what the target does. */
struct AiusLfeFold {
    AiusBiquad      lp[AIUS_DSP_LFE_CROSSOVER_SECTIONS];
    AiusBiquadState state[AIUS_DSP_LFE_CROSSOVER_SECTIONS];
    double          designed_hz; /* corner these sections were designed for */
    double          designed_fs; /* sample rate they were designed at */
};

/* Design the LR4 low-pass for `crossover_hz` at `fs` and zero the delay memory.
 * Both cascade sections are aius_biquad_lowpass(fs, crossover_hz,
 * AIUS_DSP_LFE_CROSSOVER_Q). Returns AIUS_INVALID_ARG for a NULL fold, a
 * non-positive rate, or a corner outside (0, fs/2); on failure the fold is left
 * RESET AND UNDESIGNED (designed_hz == 0), which the downmix reads as "no usable
 * crossover" and therefore skips the LFE rather than folding it unfiltered.
 * Bounded, allocation-free, and evaluates transcendentals — call it cold. */
AiusStatus aius_lfe_fold_init(AiusLfeFold *fold, double fs, double crossover_hz);

/* Zero the delay memory, keeping the designed sections. For a stream
 * discontinuity that should not re-run the coefficient math. fold == NULL is a
 * no-op. */
void aius_lfe_fold_reset(AiusLfeFold *fold);

/* BASS MANAGEMENT — what is built, what is NOT, and why the difference is a
 * recommendation rather than an omission.
 *
 * BUILT: the low-pass half of an LR4 crossover, on the LFE path, at a
 * configurable corner. Nothing else.
 *
 * NOT BUILT: the crossover network — a matched LR4 high-pass on L/R, a summed
 * mono bass bus, and the redirect. The recommendation is that it should not be
 * built for this device, and the reason is not cost:
 *
 *   BASS MANAGEMENT IS A REDIRECT, AND THIS DEVICE HAS NOWHERE TO REDIRECT TO.
 *   The point of a crossover network is to move low frequency from drivers that
 *   cannot reproduce it to one that can. This firmware has exactly one output —
 *   a stereo S/PDIF pair — so a high-pass on the mains would DELETE the bottom
 *   two octaves from the only signal there is. That is not bass management; it is
 *   a loss, and it would undo the fold-in that this very change exists to add.
 *   The missing prerequisite is a third output (a sub feed, a second I2S sink),
 *   which is hardware, not DSP.
 *
 *   THE SPEAKER-PROTECTION CASE IS ALREADY SERVED, so a dedicated stage would be
 *   duplicate machinery. Someone whose speakers unload below 40 Hz wants a
 *   high-pass on the output, and the model already has one: `output_eq` accepts
 *   two sections and `highpass` is already one of its section types. Two of them
 *   at Q = 1/sqrt(2) and a shared corner IS an LR4 high-pass, expressible today
 *   through the config surface with no firmware change. Building a second,
 *   differently-named way to do the same thing would be a seam invented for a
 *   hypothetical (§Seam arity).
 *
 *   WHAT WOULD BE NEEDED IF IT IS EVER BUILT, so the next reader does not
 *   re-derive it: an output channel to redirect into; the matched LR4 high-pass
 *   on L and R at the SAME corner as this low-pass (which is precisely why LR4
 *   was chosen — matched LR4 halves sum to unity magnitude and stay in phase, so
 *   the two halves reconstruct the input rather than notching or bumping the
 *   crossover region); a mono bass sum of L + R + the folded LFE through this
 *   same low-pass; and a per-output delay to align the two paths acoustically.
 *   The low-pass half of that design is what this change ships, deliberately, so
 *   the remaining work is additive rather than a replacement.
 *
 * A defensible smaller thing with the reasoning recorded, not a speculative
 * network. */

/* COST, PREDICTED, so the next on-device measurement has something to falsify.
 *
 * MEMORY: 160 bytes of AiusLfeFold (two AiusBiquad + two AiusBiquadState + the
 * design key) plus 16 bytes on AiusDownmixConfig, which grew 32 -> 48. The fold
 * belongs in the caller's long-lived DSP object, so on target it lands in the
 * same DTCM .bss the downmix config and limiter already occupy, NOT in the
 * 25,376-byte free-for-locals zone: 160 bytes of state and no new stack local, on
 * a per-block frame that is unchanged. The 16 bytes on AiusDownmixConfig DO land
 * on the audio path's stack, once, because enqueue() builds an AiusDspConfig per
 * block.
 *
 * CYCLES: two double-precision Direct Form I sections per LFE SAMPLE -- 10
 * multiplies, 8 adds and 8 state moves, on a serial dependency chain. Measured on
 * this host (x86-64, -O2, -ffp-contract=off) the crossover costs 9.48 ns/frame,
 * about 38 host cycles; the sixth matrix term without the filter is free inside
 * measurement noise. Scaling to the Cortex-M7 against the one calibrated
 * data point this firmware has -- 71 cycles per aius_out16_sample(), which is a
 * VDIV.F64 plus ~10 cheap operations -- puts the cascade at 60-85 cycles/frame,
 * so 31,000-43,500 cycles per 512-frame block. Predicted:
 *
 *   dsp_cycles_last  534,411 -> ~565,000-578,000
 *   dsp_cycles_max   565,024 -> ~596,000-609,000
 *   share of 6,400,000   8.4-8.8 %  ->  8.8-9.5 %
 *
 * i.e. about half a point of block budget, and only while an LFE plane is
 * present: stereo and PCM passthrough pay the per-block resolve (a few compares)
 * and nothing per sample. If the measurement lands materially above this, the
 * first thing to check is whether the M7 is spilling the cascade's state to the
 * stack each sample rather than keeping it in FPU registers. */

/* Run an `n`-section cascade in place over one plane of `frames` double
 * samples. `state` is an array of `n` AiusBiquadState (one per section),
 * advanced across calls so block boundaries are seamless. NO QUANTIZE and no
 * clamp: the cascade output stays in double (the single final quantize is
 * aius_dsp_process's). n == 0 (or sections == NULL) is a no-op passthrough.
 * Allocation-free. */
void aius_dsp_eq_run(const AiusBiquad *sections, size_t n,
                     AiusBiquadState *state,
                     double *samples, uint32_t frames);

/* --- input stage: per-input trim + EQ, runs BEFORE the downmix matrix ----- */

/* Decibels to a linear amplitude multiplier: 10^(db/20). Used to precompute an
 * input-channel trim from a `gain_db` control (bounded [-60, 12] dB). Evaluated
 * off the process path; the run loop is multiply-only. */
double aius_db_to_linear(double db);

/* The six input channels the model shapes, in AIUS_CH_* bit order. An input
 * stage carries one AiusInputChannelConfig per entry. */
#define AIUS_DSP_INPUT_CHANNELS 6

/* Up to two EQ sections per input channel (per the config contract). */
#define AIUS_DSP_INPUT_EQ_MAX 2

/* Trim + EQ for ONE input channel. `gain_linear` is the linear trim (unity =
 * 1.0, from aius_db_to_linear). `eq` is `eq_sections` biquads (0..
 * AIUS_DSP_INPUT_EQ_MAX) with `eq_sections` states in `eq_state`, advanced
 * across blocks so section boundaries are seamless. gain_linear == 1.0 with
 * eq_sections == 0 is an exact passthrough (a double plane is untouched). */
typedef struct {
    double gain_linear;
    const AiusBiquad *eq;
    size_t eq_sections;
    AiusBiquadState *eq_state;
} AiusInputChannelConfig;

/* Identity input channel: unity trim, no EQ. */
AiusInputChannelConfig aius_input_channel_default(void);

/* Apply one input channel's trim then EQ cascade over the double plane
 * `samples` in place. NO QUANTIZE and no clamp -- the trim folds into the
 * biquad input and the result stays in double for the downmix (the single
 * final quantize is aius_dsp_process's). ch == NULL or samples == NULL is a
 * no-op; eq_sections > 0 with a NULL eq/eq_state skips the EQ (trim still
 * applies). Allocation-free. */
void aius_dsp_input_run(const AiusInputChannelConfig *ch,
                        double *samples, uint32_t frames);

/* Input stage: per-channel trim + EQ, indexed in AIUS_CH_* bit order (FL, FR,
 * FC, LFE, SL, SR); the entry for a channel absent from the decoded mask is
 * ignored. The staging scratch this stage used to carry lives on AiusDspConfig
 * now: staging is unconditional in aius_dsp_process (it IS the int32->double
 * conversion), and this struct is only the OPTIONAL trim/EQ over the staged
 * planes. */
typedef struct {
    AiusInputChannelConfig channels[AIUS_DSP_INPUT_CHANNELS];
} AiusInputStage;

/* An input stage that is an exact passthrough: unity trim and no EQ on every
 * channel. */
AiusInputStage aius_input_stage_default(void);

/* --- true-peak safety limiter: tail stage, after output EQ ---------------- */

/* Lookahead is bounded to [0, 5] ms; at 48 kHz that is at most 240 samples. The
 * delay lines are fixed at this size so the process path never allocates. A
 * higher sample rate clamps the delay to this cap (5 ms at 48 kHz, 2.5 ms at
 * 96 kHz, and so on) — the CONFIGURED UNIT IS MILLISECONDS and the sample count
 * is always derived from the `fs` passed to aius_dsp_limiter_run, so nothing here
 * assumes a rate. */
#define AIUS_DSP_LIMITER_MAX_LOOKAHEAD 240

/* TRUE PEAK, per ITU-R BS.1770-4 Annex 2. The peak follower does NOT read sample
 * magnitudes: a downmix readily generates inter-sample peaks that sit above every
 * sample around them, and those clip in the DAC's reconstruction filter where
 * nothing on this side can see them. The detector therefore 4x-oversamples with
 * the polyphase FIR the standard specifies (4 phases x 12 taps, every coefficient
 * an exact multiple of 2^-14, so the table is bit-exact on host and target) and
 * follows the largest interpolated magnitude.
 *
 * TWO DELIBERATE DEVIATIONS from a literal reading of the standard, both
 * conservative:
 *
 *   - The standard's 12.04 dB pre-attenuation exists to stop a fixed-point
 *     implementation overflowing inside the filter. This one is double precision,
 *     so it is omitted; the arithmetic is otherwise identical.
 *   - The reported peak is max(|x[n]|, |interpolated|) rather than the
 *     interpolated maximum alone. Phase 0 of the specified filter has a 0.9722
 *     centre tap, so a literal 4x meter can read an isolated impulse ~0.2 dB
 *     BELOW its own sample value. Folding the raw sample into the max makes this
 *     detector a strict superset of a sample-peak detector, which is the only
 *     defensible behaviour for a stage whose job is safety.
 *
 * At fs > 48 kHz, 4x is comfortably past the standard's 192 kHz recommendation.
 * At 44.1 kHz it lands at 176.4 kHz, marginally under it; the residual
 * under-read is a small fraction of a dB and the threshold default carries more
 * margin than that.
 *
 * PRIMING, and it is not cosmetic. An FIR history zeroed at reset makes the first
 * sample look like a STEP from silence, and a step has a genuine inter-sample
 * overshoot of about +0.6 dB, so a naive detector reports a peak that the signal
 * does not contain. The limiter state is zeroed on every config apply, so that
 * would have put a spurious ~0.6 dB gain dip -- decaying over a whole release
 * constant -- into the audio every time a control changed. The detector therefore
 * contributes only |x[n]| until the history has been fully populated since the
 * reset (AIUS_DSP_TRUEPEAK_TAPS-1 samples, 229 us at 48 kHz), degrading
 * gracefully to sample-peak behaviour rather than inventing an edge. Since the raw
 * sample is always folded into the max, the priming window still cannot under-read
 * below the sample peak.
 *
 * COST CONTROL. 4 phases x 12 taps is 48 multiply-accumulates per sample per
 * channel and is by far the most expensive thing in this module, so it is gated:
 * the FIR runs only while a sample within the filter's 12-sample window could
 * possibly push the interpolated value over threshold. The bound is rigorous
 * rather than heuristic — the largest phase has an l1 norm of exactly
 * 33142/16384, so a window in which every sample satisfies
 * |x| <= threshold / 33142/16384 CANNOT produce an interpolated magnitude above
 * threshold, and skipping the filter there cannot miss an over. Quiet material
 * pays one compare per sample; only material within ~6.1 dB of threshold pays the
 * full 48 taps. */
#define AIUS_DSP_TRUEPEAK_PHASES 4
#define AIUS_DSP_TRUEPEAK_TAPS   12

/* Ring capacity for the FIR history: the next power of two at or above
 * AIUS_DSP_TRUEPEAK_TAPS, so tap indexing is a mask rather than a modulo and the
 * history costs one store per sample instead of a 12-element shift. */
#define AIUS_DSP_TRUEPEAK_RING 16

/* l1 norm of the largest interpolation phase, as the exact rational the
 * coefficient table gives (sum of |numerator| over 2^14). The skip gate above is
 * only sound if this is an upper bound on every phase's l1 norm; a change to the
 * table must recompute it, and the dsp suite checks the two against each other. */
#define AIUS_DSP_TRUEPEAK_L1_NUM 33142.0
#define AIUS_DSP_TRUEPEAK_L1_DEN 16384.0

/* Safety-limiter defaults. Named so the shipped numbers are reviewable in one
 * place and so a test can pin them.
 *
 * ENABLED BY DEFAULT, unlike every earlier revision, and that pairs with
 * AIUS_DSP_HEADROOM_DEFAULT: 0.5 headroom is modest rather than clip-free, so
 * something has to cover the residual (a fully correlated full-scale 5.1 input
 * still reaches +1.63 dBFS through the matrix) plus the inter-sample peaks no
 * amount of headroom addresses. The two sensible combinations are
 * (modest headroom + limiter on) and (clip-free headroom + limiter off); this is
 * the first.
 *
 * LOOKAHEAD LEADS THE REDUCTION. It used to default to 0 while attack_ms was
 * 1.0, which is the pathological case: the gain is one-pole smoothed toward its
 * target, so with no lookahead the reduction TRAILED the peak it existed to
 * catch and roughly a millisecond of over-threshold signal went out unattenuated.
 * The peak follower reads the incoming sample while the gain is applied to the
 * DELAYED sample, so `lookahead_ms` is exactly how far the reduction leads.
 * 2.0 ms of lead against a 0.5 ms attack is four attack time constants, so the
 * gain is within e^-4 = 1.8% of its target by the time the sample it was computed
 * for is emitted. That residual is a bounded overshoot, not zero: a peak 3 dB
 * over threshold emerges about 0.065 dB over it. The -1.0 dBFS threshold is what
 * makes that safe — the overshoot lands near -0.93 dBFS, still inside full scale,
 * and the final quantize saturation remains the last-ditch clamp. */
#define AIUS_DSP_LIMITER_ENABLED_DEFAULT        1
#define AIUS_DSP_LIMITER_THRESHOLD_DEFAULT_DBFS (-1.0)
#define AIUS_DSP_LIMITER_ATTACK_DEFAULT_MS      0.5
#define AIUS_DSP_LIMITER_RELEASE_DEFAULT_MS     100.0
#define AIUS_DSP_LIMITER_LOOKAHEAD_DEFAULT_MS   2.0

/* Safety-limiter controls (bounds per the config contract). A safety limiter is
 * transparent below `threshold_dbfs` and applies a bounded, stereo-linked gain
 * reduction above it; it is not a loudness normalizer. */
typedef struct {
    int    enabled;          /* 0 = bypass (transparent, no delay) */
    double threshold_dbfs;   /* [-24, 0] dBFS, relative to 24-bit full scale */
    double attack_ms;        /* [0.1, 100] gain-ramp-down time constant */
    double release_ms;       /* [1, 1000] gain-recovery time constant */
    double lookahead_ms;     /* [0, 5] delay so reduction leads the peak */
} AiusLimiterConfig;

/* The shipped default: enabled, -1.0 dBFS, 0.5 ms attack, 100 ms release, 2.0 ms
 * lookahead (the AIUS_DSP_LIMITER_*_DEFAULT set above). */
AiusLimiterConfig aius_limiter_config_default(void);

/* Running state + lookahead delay lines + true-peak FIR history. Zero-initialize
 * before first use; the envelope coefficients and delay length are derived from
 * the config and sample rate inside aius_dsp_limiter_run and re-derived when
 * either changes (reset-safe config apply). `gain`, the delay ring and the FIR
 * history carry across blocks for seamless boundaries.
 *
 * `tp_l` / `tp_r` are the only growth here (2 * 16 doubles = 256 bytes). They are
 * PERSISTENT STATE, not stack: the detector needs the max of the interpolated
 * values, never the values themselves, so it materializes no oversampled buffer
 * and aius_dsp_limiter_run's stack frame is unchanged. That matters because this
 * runs in the bounded AUDIO checkpoint. */
typedef struct {
    double   env;            /* true-peak follower envelope (sample units) */
    double   gain;           /* current smoothed gain reduction, (0, 1] */
    uint32_t delay;          /* active lookahead length in samples */
    uint32_t pos;            /* ring write cursor, mod delay */
    int      primed;         /* gain/env initialized on the first run */
    /* The lookahead rings are DOUBLE, deliberately: int32 rings would be a
     * hidden second quantize on the single-quantize chain, and would make the
     * envelope (which reads the unquantized incoming sample) disagree with
     * the gained output (the quantized delayed sample) by up to half an LSB.
     * Costs 1,920 B over the int32 form, counted in the phase's budget. */
    double   buf_l[AIUS_DSP_LIMITER_MAX_LOOKAHEAD];
    double   buf_r[AIUS_DSP_LIMITER_MAX_LOOKAHEAD];
    double   tp_l[AIUS_DSP_TRUEPEAK_RING]; /* interpolation history, L */
    double   tp_r[AIUS_DSP_TRUEPEAK_RING]; /* interpolation history, R */
    uint32_t tp_pos;         /* history write cursor, masked to the ring */
    uint32_t tp_run;         /* samples left for which the FIR must run */
    uint32_t tp_fill;        /* samples since reset, until the history is full */
} AiusLimiterState;

/* A limiter is its config plus its running state, so the whole tail stage is one
 * caller-owned, allocation-free object. */
typedef struct {
    AiusLimiterConfig cfg;
    AiusLimiterState  state;
} AiusLimiter;

/* Run the true-peak safety limiter over the double stereo pair (`l`, `r`) in
 * place at `fs` Hz. A peak follower (instant attack, release-timed decay)
 * tracks the incoming TRUE peak — max(|sample|, 4x-oversampled interpolated
 * magnitude) per the BS.1770-4 note above, so an inter-sample peak a
 * sample-peak detector cannot see still opens the gain reduction. Fed by the
 * single-quantize chain the input is UNCLIPPED: peaks above full scale arrive
 * intact, which is what makes the reduction honest. A single stereo-linked
 * gain (so the L/R image is preserved) is smoothed toward T/peak with the
 * attack/release time constants and applied to the signal delayed by the
 * lookahead, so the reduction LEADS the peak by cfg.lookahead_ms. Below
 * threshold the gain is unity (transparent). NO QUANTIZE: the gained result
 * stays in double (the final quantize is aius_dsp_process's). A non-finite
 * envelope (only reachable through a pathological direct-API config) is
 * clamped to zero once per run, so NaN cannot latch the limiter open.
 * cfg.enabled == 0 is a pure bypass with no delay and no FIR. Output frame
 * count equals input (the last `delay` samples stay in the ring as bounded
 * latency). Allocation-free; lim == NULL or a NULL plane is a no-op. */
void aius_dsp_limiter_run(AiusLimiter *lim, double fs,
                          double *l, double *r, uint32_t frames);

/* The true peak of one plane, as the limiter's detector measures it: the maximum
 * over every sample of max(|x[n]|, the four BS.1770-4 interpolated phases). In
 * 24-bit sample units, like every other magnitude here. Stateless (it runs its own
 * history), so it is a measurement tool rather than part of the audio path: the
 * tests use it to talk about inter-sample peaks in the same units the limiter
 * thresholds on.
 *
 * Primed the same way the limiter is: the interpolated phases are only counted once
 * the 12-sample window is full, so a buffer that begins part-way up a waveform is
 * not reported as containing the step that splicing it after silence would create.
 * The result is therefore the true peak of the buffer's INTERIOR, and is always at
 * least the buffer's sample peak. samples == NULL or frames == 0 returns 0.0.
 * Allocation-free. */
double aius_dsp_true_peak(const int32_t *samples, uint32_t frames);

/* The same measurement over a double plane, for tests reasoning about the
 * unquantized inter-stage signal. Same priming and units. */
double aius_dsp_true_peak_d(const double *samples, uint32_t frames);

/* Current gain reduction in dB (<= 0), for later metering. 0.0 dB means no
 * reduction. lim == NULL returns 0.0. Reads state only; does not process. */
double aius_dsp_limiter_gain_reduction_db(const AiusLimiter *lim);

/* --- 24 -> 16 bit output stage: TPDF dither, round to nearest, saturate ---- */

/* The word width of a 16-bit playback engine (the Teensy AudioStream engine is
 * the concrete one). Retarget alongside AIUS_DSP_SAMPLE_MAX/MIN; this is a DAC
 * word width, never a sample rate. */
#define AIUS_DSP_OUT16_MAX ((int32_t)32767)
#define AIUS_DSP_OUT16_MIN ((int32_t)-32768)

/* One 16-bit LSB in 24-bit input units: (2^23) / (2^15) = 2^8. */
#define AIUS_DSP_OUT16_LSB 256.0

/* WHY THIS STAGE EXISTS. Handing a 24-bit result to a 16-bit engine with an
 * arithmetic right shift is wrong twice over, and both faults are audible:
 *
 *   - A shift rounds toward -infinity, so negatives round away from zero while
 *     positives round toward it. That is a -0.5 LSB DC offset and an asymmetric
 *     transfer curve — distortion, not noise. (-1 >> 8 is -1, a full 16-bit LSB,
 *     where the correct answer is 0.)
 *   - Undithered truncation makes the quantisation error a deterministic function
 *     OF THE SIGNAL, so the error is correlated with the programme: harmonic and
 *     intermodulation products rather than a benign noise floor, worst on fades
 *     and low-level detail, which is exactly where a decoder gets judged.
 *
 * The fix is the textbook one, in this order: add TPDF dither, then round to
 * nearest (ties away from zero, as aius_quantize), then saturate.
 *
 * DITHER AMPLITUDE. Triangular PDF spanning +-1 LSB of the 16-bit target (2 LSB
 * peak-to-peak), formed as the difference of two independent uniform draws. This
 * is the standard optimal choice: it decorrelates the error from the signal AND
 * makes the total error power independent of the signal, so there is no noise
 * modulation as the level moves. It costs about 4.8 dB of noise floor against
 * undithered truncation, buying a noise floor near -93 dBFS in exchange for
 * removing signal-correlated distortion entirely.
 *
 * NO NOISE SHAPING, and that is a recommendation rather than an oversight.
 * Shaping the dither noise out of the ear's sensitive band would buy the usual
 * 10-15 dB of perceived SNR for two doubles of error-feedback state and a couple
 * of multiplies per sample, which is affordable. It is declined because: flat
 * TPDF already puts the noise floor near -93 dBFS, below both the DTS Core source
 * and any DAC this feeds; the error-feedback path would have to be given defined
 * behaviour when the output saturates (an unshaped stage simply clamps, a shaped
 * one can wind up), which is new contract surface on a safety path; and the
 * feedback state would need clearing on reset, reintroducing the settling
 * transient that this stage's statelessness-apart-from-the-PRNG avoids. Revisit
 * only if measurement shows the 16-bit floor is actually the limit.
 *
 * DETERMINISM, which is the interesting part: dither is random and this module is
 * contractually bit-exact between host and target. Both hold, because the
 * randomness is a seeded PRNG rather than an entropy source.
 *
 *   GENERATOR: xoshiro128++ 1.0 (Blackman & Vigna). 128-bit state in four
 *   uint32, period 2^128-1, output = rotl(s0+s3, 7) + s0. Chosen because it is
 *   INTEGER-ONLY — add, xor, shift, rotate on uint32 — so it is bit-identical on
 *   x86-64 and Cortex-M7 with no FPU, no libc, no locale and no rounding mode in
 *   the loop. Explicitly NOT rand() (implementation-defined, so host and target
 *   would diverge immediately), not any other libc facility, and not the
 *   hardware RNG (a true entropy source would break bit-exactness by
 *   construction and make every golden fixture unreproducible).
 *
 *   ADEQUACY AS A DITHER SOURCE: dither needs a flat spectrum, the right
 *   amplitude distribution, and independence from the signal — not
 *   unpredictability. xoshiro128++ is equidistributed and passes BigCrush on
 *   every bit position (the ++ scrambler is what fixes the low-bit linear
 *   artefacts of the plain-addition variants, which matters here because both
 *   halves of the word are used). Its period is 2.2e26 years at 48 kHz, so there
 *   is no repetition to become tonal. Independence from the signal is structural:
 *   the state advances with the sample index and never sees a sample value, which
 *   is precisely the property that turns quantisation distortion into
 *   quantisation noise. It is not cryptographic, and nothing here needs it to be.
 *
 *   ONE DRAW PER SAMPLE: the two uniform draws the triangular PDF needs are the
 *   high and low 16 bits of one 32-bit output. The map word -> (hi, lo) is a
 *   bijection onto the 2^32 square, so the pair is exactly as uniform as the word
 *   and the resulting PDF is exactly triangular; taking two separate draws would
 *   double the generator cost for no distributional gain.
 *
 *   EXACT ARITHMETIC: the dither offset is (hi - lo) / 65536 LSB, an integer
 *   under 2^17 over a power of two, so it is EXACTLY representable in double and
 *   adds no rounding of its own. The whole conversion is therefore one
 *   well-defined round-to-nearest, identical on both platforms.
 *
 *   L AND R ARE INDEPENDENT STREAMS, seeded from eight consecutive SplitMix32
 *   outputs of the one seed (first four -> L, next four -> R). Correlated dither
 *   across the pair would image as a phantom-centre hiss and would correlate the
 *   two channels' errors; independent streams put it where noise belongs.
 *
 *   ACROSS A RESET: aius_out16_init re-seeds to the same constant, so the
 *   sequence restarts at index 0 and the dither is a pure function of (seed,
 *   sample index). Replaying identical input after a reset therefore reproduces
 *   identical output — which is what makes the golden fixtures possible and what
 *   the bit-exactness contract demands. Audibly this is nil: it is the same noise
 *   ensemble either way, near -93 dBFS. There is no reset transient because the
 *   stage holds no filter memory, which is a direct consequence of declining
 *   noise shaping above. */
#define AIUS_DSP_DITHER_SEED_DEFAULT 0x41555331u /* "AUS1" */

/* One xoshiro128++ stream. Opaque; seed it with aius_out16_init. Never all-zero
 * after seeding (that is the generator's fixed point). */
typedef struct {
    uint32_t s[4];
} AiusDitherState;

/* The output stage: a dither enable plus one independent stream per channel.
 * Caller-owned and allocation-free like every other stage here. */
typedef struct {
    int             dither;   /* 1 = TPDF dither (the default), 0 = round+saturate only */
    AiusDitherState stream[2]; /* [0] = L, [1] = R */
} AiusOut16Stage;

/* Seed both streams from `seed` and set the dither enable. Call once before the
 * steady-state loop, NOT per config apply: re-seeding mid-stream restarts the
 * noise sequence, which is harmless but makes the output depend on config-change
 * history instead of on the sample index alone. */
void aius_out16_init(AiusOut16Stage *st, uint32_t seed, int dither);

/* The shipped default: dither on, AIUS_DSP_DITHER_SEED_DEFAULT. */
AiusOut16Stage aius_out16_stage_default(void);

/* Convert one 24-bit sample to the 16-bit range with dither (when `dither` is
 * non-zero), round to nearest ties away from zero, and saturation to
 * [AIUS_DSP_OUT16_MIN, AIUS_DSP_OUT16_MAX]. Advances `d` by one draw when
 * dithering. `d` may be NULL only when dither is 0. Exposed so a test can pin the
 * per-sample rule directly; the block form below is what the audio path calls. */
int32_t aius_out16_sample(AiusDitherState *d, int dither, int32_t x24);

/* Convert `frames` samples of the 24-bit stereo pair (`l24`, `r24`) into the
 * 16-bit planes (`l16`, `r16`), advancing each channel's dither stream. The input
 * and output planes may not overlap. st == NULL or any NULL plane is a no-op.
 * Allocation-free; no per-sample state beyond the two PRNG words. */
void aius_out16_run(AiusOut16Stage *st,
                    const int32_t *l24, const int32_t *r24,
                    int16_t *l16, int16_t *r16, uint32_t frames);

/* --- output word width: the stage above, made BYPASSABLE ------------------- */

/* The transmit word width, selected at the one boundary that cares.
 *
 * WHY THIS EXISTS. Everything above produces 24-bit integers and the S/PDIF
 * transmit registers take 24 bits, so a 24-bit output path is LOSSLESS FROM THE
 * DSP TO THE WIRE AND NEEDS NO DITHER AT ALL. That demotes the whole out16
 * stage from a correctness fix to a refinement one particular output selection
 * needs -- so it is bypassed rather than deleted, and the selection is named
 * rather than implied.
 *
 * AIUS_DSP_OUT_WIDTH_24 is the native width: a copy, no dither stream touched,
 * no rounding, no saturation (the input is already in range by contract).
 *
 * AIUS_DSP_OUT_WIDTH_16 is the old engine width, expressed in transmit units:
 * aius_out16_sample() per sample -- TPDF dither, round to nearest, saturate --
 * scaled back up by AIUS_DSP_OUT16_LSB. That scaling is the `<< 8` the retained
 * S/PDIF snapshot's ISR performed (output_spdif3.cpp:130), so the 16-bit arm is
 * bit-identical to what the board shipped before the 24-bit path existed, and
 * the low 8 bits of every word are zero. It is a real selection, not a
 * compatibility shim: a 16-bit sink downstream is the case it serves. */
#define AIUS_DSP_OUT_WIDTH_24 24
#define AIUS_DSP_OUT_WIDTH_16 16

/* Convert `frames` samples of the 24-bit stereo pair (`l24`, `r24`) into
 * `width_bits`-wide transmit words in the 24-bit int32 planes (`l_out`,
 * `r_out`). The width test is hoisted out of the sample loop, so a 24-bit
 * selection costs one branch per call and then a straight copy -- CHEAPER than
 * the 16-bit path it replaces, which paid a PRNG draw, a double divide, a floor
 * and two comparisons per sample.
 *
 * Input and output planes may not overlap. st == NULL (when width_bits selects
 * 16), any NULL plane, or an unrecognised width is a no-op, so an unhandled
 * width cannot silently emit garbage. */
void aius_out_stage_run(AiusOut16Stage *st, int width_bits,
                        const int32_t *l24, const int32_t *r24,
                        int32_t *l_out, int32_t *r_out, uint32_t frames);

/* --- coherent meter snapshot: diagnostic taps, never configuration --------- */

/* The six metered input channels, in AIUS_CH_* bit order (FL, FR, FC, LFE, SL,
 * SR) -- the same order the input stage shapes. */
#define AIUS_METER_INPUT_CHANNELS AIUS_DSP_INPUT_CHANNELS

/* Waveform/envelope points are bounded so the snapshot is a fixed size that fits
 * a statically reserved ring (OCRAM on target). The SD editor renders at most
 * this many points and never requests full-rate samples. */
#define AIUS_METER_WAVEFORM_MAX 128

/* Raw-sample analysis window, in mono frames. Distinct from the waveform above
 * in KIND, not just in length: the waveform is a peak-magnitude ENVELOPE, and
 * an envelope has no spectrum -- |x| is not x, so an FFT of it answers a
 * question nobody asked. This window is the signal itself, so a client can
 * transform it.
 *
 * 1024 is chosen against three bounds at once. A spectrum consumer needs a
 * power of two of at least 256 to say anything about 20 Hz..20 kHz (1024 at
 * 48 kHz is 21.3 ms, a 46.9 Hz bin). The publish cadence is 20 Hz (~2400 frames
 * at 48 kHz), so the ring always refills between publishes and the window is
 * never stitched from two eras. And it is the largest power of two whose
 * decimal JSON still fits the runtime's response body beside the rest of the
 * meters payload -- the arithmetic is in optical_main.cpp's serializer. */
#define AIUS_METER_ANALYSIS_WINDOW 1024u

/* Bounded seqlock read retries: a reader contended by an in-progress publish
 * retries this many times, then reports failure rather than returning a torn
 * snapshot. The audio-path writer completes a publish in bounded time, so a
 * single-reader HTTP path succeeds on the first uncontended attempt. */
#define AIUS_METER_READ_RETRIES 8

/* A fixed-size, allocation-free diagnostic snapshot of the pipeline. Peak and
 * RMS are absolute linear sample magnitudes (24-bit sample units), so a reader
 * converts to dBFS with 20*log10(v / AIUS_DSP_SAMPLE_MAX). All fields are finite.
 *
 * Meter values are DIAGNOSTIC and never become configuration fields; the audio
 * path is bit-identical whether or not a meter is attached.
 *
 * `queue_frames`, `estimated_latency_us`, `dsp_cycles_last`, and `dsp_cycles_max`
 * are populated by the target adapter (it owns the audio queue and the DWT cycle
 * counter); the portable path carries them through unmeasured as staged fields.
 *
 * SIZE. `analysis_window` is 4 KiB of the ~4.7 kB total, so every publish and
 * every read copies it. Both are cold: publish runs at the adapter's cadence
 * (20 Hz on target -> ~80 KiB/s of memcpy) and read runs per HTTP request
 * (10 Hz from the SD editor -> ~47 KiB/s). Neither is per-block, and a 4 KiB
 * word copy is ~1 k stores; at 600 MHz that is single-digit microseconds. A
 * caller that puts a snapshot on the stack should think again -- 4.7 kB is a
 * large automatic on a microcontroller.
 */
typedef struct {
    uint32_t sequence;      /* strictly increasing publish counter (0 = never published) */
    uint32_t sample_rate_hz;
    double   input_peak[AIUS_METER_INPUT_CHANNELS]; /* |sample| per input, AIUS_CH_* order */
    double   input_rms[AIUS_METER_INPUT_CHANNELS];  /* sqrt(mean(sample^2)) per input */
    double   output_peak[2];                        /* stereo out |sample|, [L, R] */
    double   output_rms[2];                         /* stereo out RMS, [L, R] */
    double   limiter_gain_reduction_db;             /* <= 0.0; 0 = no reduction */
    uint32_t waveform_count;                        /* valid points, <= AIUS_METER_WAVEFORM_MAX */
    int32_t  waveform[AIUS_METER_WAVEFORM_MAX];     /* per-bucket peak-magnitude envelope */
    /* Raw mono output samples, OLDEST FIRST, in the same 24-bit sample units as
     * the peaks above (full scale AIUS_DSP_SAMPLE_MAX). `analysis_count` is 0
     * until the capture ring has been fed at all and saturates at the window
     * size thereafter; samples past it are zero. A consumer that needs a power
     * of two takes the last one it wants -- oldest-first ordering makes the
     * NEWEST samples the tail, which is what a rolling analyser wants. */
    uint32_t analysis_count;                        /* valid samples, <= AIUS_METER_ANALYSIS_WINDOW */
    int32_t  analysis_window[AIUS_METER_ANALYSIS_WINDOW];
    uint32_t queue_frames;                          /* target adapter: audio queue depth */
    uint32_t estimated_latency_us;                  /* target adapter: estimated path latency */
    uint32_t dsp_cycles_last;                       /* target adapter: last block DWT cycles */
    uint32_t dsp_cycles_max;                        /* target adapter: peak block DWT cycles */
} AiusMeterSnapshot;

/* Accumulator + coherent publisher. Caller-owned, allocation-free, zero-init
 * before first use. The audio path calls the cheap accumulate/set functions per
 * block (scalar peak/sum-of-squares updates and a bounded waveform decimation);
 * a separate publish step commits a complete snapshot behind a seqlock so a
 * reader never observes a torn mix of two windows. HTTP copies the last complete
 * snapshot with aius_meter_read outside the audio critical path.
 *
 * COHERENCE. `seq` is a seqlock counter: publish makes it odd (write in
 * progress), writes the payload, then makes it even (complete). A reader samples
 * `seq`, copies the payload, re-samples `seq`, and accepts the copy only if it
 * was even and unchanged -- otherwise it retries. On target the adapter places
 * this in OCRAM and issues the memory barriers the producer/reader protocol needs;
 * the portable logic is single-thread host-testable via the same protocol. */
typedef struct {
    /* live accumulators for the current (unpublished) window */
    double   in_peak[AIUS_METER_INPUT_CHANNELS];
    double   in_sqsum[AIUS_METER_INPUT_CHANNELS];
    double   out_peak[2];
    double   out_sqsum[2];
    uint64_t in_frames;   /* frames accumulated into the input channels */
    uint64_t out_frames;  /* frames accumulated into the stereo output */
    int32_t  wave[AIUS_METER_WAVEFORM_MAX];
    uint32_t wave_count;
    /* Raw mono capture ring, written one sample per output frame. ROLLING, not
     * windowed: unlike every accumulator above it, this deliberately survives
     * aius_meter_reset_accumulators(), because the window is "the newest N
     * frames the pipeline produced", not "the frames since the last publish".
     * Resetting it would leave the first publish of every window analysing a
     * partially refilled buffer whenever a block/rate pairing puts fewer than
     * AIUS_METER_ANALYSIS_WINDOW frames in a publish window. */
    int32_t  analysis[AIUS_METER_ANALYSIS_WINDOW];
    uint32_t analysis_cursor;  /* next write index, wraps at the window size */
    uint32_t analysis_filled;  /* samples ever written, saturating at the window size */
    double   limiter_gr_db;
    uint32_t sample_rate_hz;
    /* target-supplied telemetry, staged for the next publish */
    uint32_t queue_frames;
    uint32_t estimated_latency_us;
    uint32_t dsp_cycles_last;
    uint32_t dsp_cycles_max;
    /* coherent published state (seqlock-guarded) */
    volatile uint32_t seq;       /* even = stable, odd = publish in progress */
    uint32_t publishes;          /* completed publishes -> snapshot.sequence */
    AiusMeterSnapshot published;
} AiusMeter;

/* Zero the meter and set the sample rate carried into every snapshot. */
void aius_meter_init(AiusMeter *m, uint32_t sample_rate_hz);

/* Zero only the windowed accumulators (peak/RMS sums, frame counts, waveform),
 * leaving the seqlock, published snapshot, limiter GR, staged telemetry, and
 * the rolling analysis capture ring intact. Called by publish; exposed for an
 * explicit window reset. */
void aius_meter_reset_accumulators(AiusMeter *m);

/* Accumulate per-input peak + sum-of-squares from a decoded/trimmed block. The
 * block's present planes are mapped to AIUS_CH_* ordinals via channel_mask, so a
 * narrower layout leaves absent channels at zero. m/block NULL is a no-op. */
void aius_meter_accumulate_input(AiusMeter *m, const AiusPcmBlock *block);

/* Accumulate stereo-output peak + sum-of-squares, refresh the decimated waveform
 * envelope, and write this block's frames into the rolling raw-sample analysis
 * ring as mono ((l + r) / 2). m or a NULL plane is a no-op.
 *
 * The mono fold is the SIGNAL, not a magnitude: max(|l|, |r|) is what the
 * waveform envelope already carries, and it has no meaningful spectrum. The
 * fold costs one 64-bit add, one arithmetic shift and one store per frame --
 * the widening is what makes it total for any int32 input rather than only for
 * the documented 24-bit domain, and the shift (rather than a divide) keeps the
 * rounding error a constant half-LSB offset in bin 0 instead of a
 * signal-correlated error smeared across the spectrum. */
void aius_meter_accumulate_output(AiusMeter *m, const int32_t *l, const int32_t *r,
                                  uint32_t frames);

/* Stage the current limiter gain reduction (dB, <= 0) for the next publish. */
void aius_meter_set_limiter_gr(AiusMeter *m, double gain_reduction_db);

/* Stage target-adapter telemetry (queue depth, estimated latency, cycle counts)
 * for the next publish. The portable path does not measure these. */
void aius_meter_set_telemetry(AiusMeter *m, uint32_t queue_frames,
                              uint32_t estimated_latency_us,
                              uint32_t dsp_cycles_last, uint32_t dsp_cycles_max);

/* Decimate a stereo (or mono, r == NULL) block of `frames` samples into at most
 * `cap` per-bucket peak-magnitude envelope points, written to `out`. The block
 * is split into min(frames, cap) contiguous, near-equal buckets; each output
 * point is the peak of max(|l|, |r|) within its bucket. Deterministic and
 * bounded. Returns the number of points written (0 if out is NULL, cap is 0, or
 * frames is 0). Allocation-free. */
uint32_t aius_meter_decimate(const int32_t *l, const int32_t *r, uint32_t frames,
                             int32_t *out, uint32_t cap);

/* Commit a coherent snapshot: bump the sequence, compute RMS from the window's
 * sum-of-squares, copy peaks/waveform/limiter-GR/telemetry into the published
 * snapshot behind the seqlock, unroll the analysis capture ring into the
 * snapshot oldest-first, then reset the windowed accumulators. m NULL is a
 * no-op. Allocation-free. */
void aius_meter_publish(AiusMeter *m);

/* Copy the last complete published snapshot into `out` behind the seqlock. Returns
 * 1 on a coherent copy, 0 if m/out is NULL or a concurrent publish could not be
 * read cleanly within AIUS_METER_READ_RETRIES (the caller retries later; a torn
 * snapshot is never returned). Reads only; allocation-free. */
int aius_meter_read(const AiusMeter *m, AiusMeterSnapshot *out);

/* Full output DSP config: an optional input stage (per-channel trim + EQ) before
 * the downmix, the downmix matrix, an optional output EQ cascade run on BOTH
 * stereo channels, and an optional true-peak safety limiter tail.
 *
 * `eq` is `eq_sections` sections; `eq_state` is 2*eq_sections states,
 * channel-major (all L-channel section states, then all R-channel). eq_sections
 * == 0 (or eq == NULL) runs downmix only.
 *
 * `scratch` and `work_l`/`work_r` are the double staging the single-quantize
 * chain runs in, caller-owned like every state object here. `scratch[p]` must
 * hold the block's frame count of doubles for every present plane index `p`
 * (packed in ascending AIUS_CH_* bit order, as the decoder emits); the work
 * pair holds the stereo result between the downmix and the final quantize.
 * The staging convert-copy is UNCONDITIONAL -- it is the int32 -> double
 * boundary, not part of the optional trim/EQ -- so these planes are required
 * whenever aius_dsp_process runs. On the target they are carved from the
 * audio pipeline's DTCM arena at bring-up (the OCRAM home was vetoed by the
 * defect-26 link-time heap floor); never on the runtime stack.
 *
 * `input` == NULL skips the trim/EQ over the staged planes (the staging itself
 * still happens). `limiter` == NULL bypasses the limiter tail. Both default to
 * NULL under aggregate zero-initialization.
 *
 * Metering is deliberately NOT a field here: the AiusMeter taps are driven by the
 * caller around aius_dsp_process (accumulate the decoded/trimmed input and the
 * stereo output, then publish at the adapter's cadence), so the meter code is not
 * linked into this hot function on the target until the adapter wires it. */
typedef struct {
    AiusDownmixConfig downmix;
    const AiusBiquad *eq;
    size_t eq_sections;
    AiusBiquadState *eq_state;
    const AiusInputStage *input;
    AiusLimiter *limiter;
    double *scratch[AIUS_MAX_CHANNELS];
    double *work_l;
    double *work_r;
} AiusDspConfig;

/* The single-quantize chain: convert `in`'s int32 planes into cfg->scratch
 * (double), run trim + input EQ over the staged planes (if cfg->input), downmix
 * into cfg->work_l/work_r, run the output EQ cascade, then the limiter (if
 * any), all in double -- then perform THE one saturating quantize per sample
 * into `out`'s int32 planes. `out` must have planes[0]/planes[1] pointing at
 * caller-owned buffers of at least in->frames int32 samples; its other fields
 * are overwritten (channels=2, mask=FL|FR, frames, sample_rate_hz). Returns
 * AIUS_OK, or AIUS_INVALID_ARG for the downmix's reasons, a missing scratch
 * plane for a present channel, a missing work plane, or eq_sections > 0 with
 * eq/eq_state NULL. Allocation-free. */
AiusStatus aius_dsp_process(const AiusDspConfig *cfg,
                            const AiusPcmBlock *in,
                            AiusPcmBlock *out);

/* Carve a stereo output block (two int32 planes of `frames` samples) out of a
 * caller-owned arena and fill `out`'s fields (channels=2, mask=FL|FR,
 * sample_rate_hz=sample_rate_hz). Call once before the steady-state loop; the
 * process functions then reuse the block with no further allocation. Returns
 * AIUS_OK, AIUS_NO_SPACE if the arena cannot fit both planes, or
 * AIUS_INVALID_ARG. */
AiusStatus aius_dsp_alloc_stereo(AiusArena *arena, uint32_t frames,
                                 uint32_t sample_rate_hz, AiusPcmBlock *out);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_DSP_H */
