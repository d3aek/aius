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
 *                                    ATSC A/52 (see AiusDownmixConfig).
 *   2. EQ        (aius_dsp_eq_run)   a Direct Form I biquad cascade per channel,
 *                                    coefficients supplied by the caller.
 *   3. Limiter   (aius_dsp_limiter_run)  true-peak safety limiter tail.
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
 * THAT PRECONDITION IS CURRENTLY UNMET ON THE TARGET, and it is a real divergence
 * rather than a theoretical one. scripts/test.sh passes -ffp-contract=off for the
 * dsp and dsp-vectors suites, but platformio.ini's build_flags do not, and GCC
 * defaults to -ffp-contract=fast under -std=gnu++17. Disassembling
 * .pio/build/teensy41-optical/lib1a3/aius_dsp/aius_dsp.c.o shows 25 vfma.f64
 * instructions -- in truepeak_push, run_cascade, aius_dsp_downmix and
 * aius_dsp_limiter_run -- where the host emits a separate vmul and vadd. So the
 * downmix and EQ have not in fact been bit-identical host-to-target, and the
 * 48-tap true-peak FIR is now the largest single source of it. THE FIX IS ONE
 * FLAG: add -ffp-contract=off to platformio.ini [common] build_flags. It is very
 * unlikely to cost anything and may even be faster on the Cortex-M7, whose
 * double-precision VFMA latency is longer than a VMUL/VADD pair that can
 * pipeline. (One contraction is benign by construction: the vfma in
 * aius_out16_sample multiplies an exact integer by a power of two, so fused and
 * unfused agree bit-for-bit -- see the output-stage note below.) platformio.ini is
 * not this module's to edit; the flag is owed.
 *
 * NO HEAP ON THE PROCESS PATH. Every function here is allocation-free. Output
 * planes and biquad state are caller-owned; aius_dsp_alloc_stereo is a
 * convenience that carves an output block out of a caller-owned arena once,
 * before the steady-state loop.
 */

/* Output sample range. The DTS Core reconstructs to 24 significant bits, so the
 * stereo output is clamped (saturated) into the signed 24-bit range. Change
 * these two constants together to retarget a different DAC word width. */
#define AIUS_DSP_SAMPLE_MAX ((int32_t)8388607)   /*  2^23 - 1 */
#define AIUS_DSP_SAMPLE_MIN ((int32_t)-8388608)  /* -2^23     */

/* Downmix coefficient defaults (ITU-R BS.775 / ATSC A/52 "Lo/Ro" style).
 *   L' = FL + center*FC + surround*SL + lfe*LFE
 *   R' = FR + center*FC + surround*SR + lfe*LFE
 * center = surround = 1/sqrt(2) (~-3 dB) is the documented, standard default:
 * mixing FC and each surround down 3 dB preserves acoustic power when the two
 * summed sources are uncorrelated. M_SQRT1_2 is not portable under -std=c11, so
 * the value is spelled out. */
#define AIUS_DSP_DOWNMIX_CENTER_DEFAULT   0.70710678118654752440  /* 1/sqrt(2) */
#define AIUS_DSP_DOWNMIX_SURROUND_DEFAULT 0.70710678118654752440  /* 1/sqrt(2) */

/* LFE handling. ATSC A/52 and ITU-R BS.775 EXCLUDE the LFE from the default
 * stereo downmix, so the default gain is 0.0. The common alternative for small
 * stereo playback that would otherwise lose all sub-bass is to fold the LFE in
 * at -10 dB; AIUS_DSP_LFE_GAIN_MINUS10DB is provided for that. This is a
 * clearly-marked configurable (AiusDownmixConfig.lfe) and a PRODUCT DECISION the
 * owner should confirm. */
#define AIUS_DSP_LFE_GAIN_DEFAULT   0.0
#define AIUS_DSP_LFE_GAIN_MINUS10DB 0.31622776601683793320  /* 10^(-10/20) */

/* Overall post-downmix gain applied to L' and R' before the saturating clamp.
 *
 * WHY 0.5 AND NOT 1.0. The downmix quantizes and SATURATES into the 24-bit range
 * at the end of aius_dsp_downmix (and the EQ cascade saturates again), so every
 * later stage — the limiter included — sees an already-clamped signal. A limiter
 * downstream of a saturating matrix cannot recover an over the matrix has
 * already destroyed, so the headroom, not the limiter, is what has to keep the
 * matrix sum inside full scale. That rules out "unity headroom, let the limiter
 * catch the overs" until the pipeline carries an unclamped intermediate.
 *
 * The committed DTS corpus (tools/dts_vectors/vectors, 4 vectors x 5120 frames,
 * measured through this matrix at unity headroom) distributes like this:
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
 * Still a PRODUCT DECISION, now an evidenced one; a caller wanting the
 * theoretical guarantee instead sets headroom from
 * aius_downmix_clipfree_headroom and may then turn the limiter off. */
#define AIUS_DSP_HEADROOM_DEFAULT 0.5

/* Downmix configuration. Fill from aius_downmix_config_default() then override. */
typedef struct {
    double center;   /* FC gain into both L and R */
    double surround; /* SL->L and SR->R gain */
    double lfe;      /* LFE gain into both L and R */
    double headroom; /* overall output gain before the clamp */
} AiusDownmixConfig;

/* The documented default: BS.775/A52 -3 dB center+surround, LFE excluded, unity
 * headroom. */
AiusDownmixConfig aius_downmix_config_default(void);

/* The headroom that makes a fully correlated full-scale input downmix to exactly
 * full scale (no saturation) for the given config: 1/(1 + center + surround +
 * |lfe|), clamped to (0, 1]. */
double aius_downmix_clipfree_headroom(const AiusDownmixConfig *cfg);

/* Downmix `in` (1..6 channels, mask-driven) into stereo `out`.
 *
 * Channel routing is driven entirely by in->channel_mask (AIUS_CH_*), so
 * narrower inputs are handled without a mode flag:
 *   - mono   (channels == 1): the lone plane is copied to BOTH L and R at unity
 *            gain (headroom still applies), regardless of which mask bit is set.
 *   - stereo (FL|FR):          FL->L, FR->R at unity -> exact passthrough.
 *   - 3.0/4.0/5.0/5.1 etc.:    every present channel routed per its AIUS_CH_*
 *                              rule above; absent channels contribute nothing.
 * Planes must be tightly packed to `channels` in AIUS_CH_* bit order, as the
 * decoder emits them.
 *
 * `out` must have out->planes[0] (L) and out->planes[1] (R) pointing at
 * caller-owned buffers of at least in->frames int32 samples each; the other
 * fields are overwritten (channels=2, mask=FL|FR, frames=in->frames,
 * sample_rate_hz=in->sample_rate_hz). in and out planes must not overlap.
 *
 * Returns AIUS_OK, or AIUS_INVALID_ARG (null args/planes, channels/mask
 * mismatch, channels outside 1..AIUS_MAX_CHANNELS). Allocation-free. */
AiusStatus aius_dsp_downmix(const AiusDownmixConfig *cfg,
                            const AiusPcmBlock *in,
                            AiusPcmBlock *out);

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

/* Run an `n`-section cascade in place over one plane of `frames` int32 samples.
 * `state` is an array of `n` AiusBiquadState (one per section), advanced across
 * calls so block boundaries are seamless. Output is rounded to nearest (ties
 * away from zero) and saturated to [AIUS_DSP_SAMPLE_MIN, AIUS_DSP_SAMPLE_MAX].
 * n == 0 (or sections == NULL) is a no-op passthrough. Allocation-free. */
void aius_dsp_eq_run(const AiusBiquad *sections, size_t n,
                     AiusBiquadState *state,
                     int32_t *samples, uint32_t frames);

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
 * eq_sections == 0 is an exact passthrough. */
typedef struct {
    double gain_linear;
    const AiusBiquad *eq;
    size_t eq_sections;
    AiusBiquadState *eq_state;
} AiusInputChannelConfig;

/* Identity input channel: unity trim, no EQ. */
AiusInputChannelConfig aius_input_channel_default(void);

/* Apply one input channel's trim then EQ cascade over `samples` in place, with a
 * SINGLE saturating quantize per sample (the trim is folded into the biquad
 * input, so there is no intermediate double-rounding). ch == NULL or samples ==
 * NULL is a no-op; eq_sections > 0 with a NULL eq/eq_state skips the EQ (trim
 * still applies). Allocation-free. */
void aius_dsp_input_run(const AiusInputChannelConfig *ch,
                        int32_t *samples, uint32_t frames);

/* Input stage: per-channel trim + EQ for every decoded plane, plus caller-owned
 * scratch planes the process path stages into before the downmix (downmix reads
 * a const input). `channels` is indexed in AIUS_CH_* bit order (FL, FR, FC, LFE,
 * SL, SR); the entry for a channel absent from the decoded mask is ignored.
 * `scratch[p]` must hold at least the block's frame count int32 samples for each
 * present plane index `p` (packed in ascending bit order, as the decoder emits).
 * Carve them once from an arena before the steady-state loop. */
typedef struct {
    AiusInputChannelConfig channels[AIUS_DSP_INPUT_CHANNELS];
    int32_t *scratch[AIUS_MAX_CHANNELS];
} AiusInputStage;

/* An input stage that is an exact passthrough: unity trim and no EQ on every
 * channel. `scratch` is zeroed; the caller fills in the plane pointers. */
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
 * runs in the audio ISR. */
typedef struct {
    double   env;            /* true-peak follower envelope (sample units) */
    double   gain;           /* current smoothed gain reduction, (0, 1] */
    uint32_t delay;          /* active lookahead length in samples */
    uint32_t pos;            /* ring write cursor, mod delay */
    int      primed;         /* gain/env initialized on the first run */
    int32_t  buf_l[AIUS_DSP_LIMITER_MAX_LOOKAHEAD];
    int32_t  buf_r[AIUS_DSP_LIMITER_MAX_LOOKAHEAD];
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

/* Run the true-peak safety limiter over the stereo pair (`l`, `r`) in place at
 * `fs` Hz. A peak follower (instant attack, release-timed decay) tracks the
 * incoming TRUE peak — max(|sample|, 4x-oversampled interpolated magnitude) per
 * the BS.1770-4 note above, so an inter-sample peak a sample-peak detector cannot
 * see still opens the gain reduction. A single stereo-linked gain (so the L/R
 * image is preserved) is smoothed toward T/peak with the attack/release time
 * constants and applied to the signal delayed by the lookahead, so the reduction
 * LEADS the peak by cfg.lookahead_ms. Below threshold the gain is unity
 * (transparent). cfg.enabled == 0 is a pure bypass with no delay and no FIR.
 * Output frame count equals input (the last `delay` samples stay in the ring as
 * bounded latency). Allocation-free; lim == NULL or a NULL plane is a no-op. */
void aius_dsp_limiter_run(AiusLimiter *lim, double fs,
                          int32_t *l, int32_t *r, uint32_t frames);

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
 * this in OCRAM and issues the memory barriers the ISR/reader pair needs; the
 * portable logic is single-thread host-testable via the same protocol. */
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
 * leaving the seqlock, published snapshot, limiter GR, and staged telemetry
 * intact. Called by publish; exposed for an explicit window reset. */
void aius_meter_reset_accumulators(AiusMeter *m);

/* Accumulate per-input peak + sum-of-squares from a decoded/trimmed block. The
 * block's present planes are mapped to AIUS_CH_* ordinals via channel_mask, so a
 * narrower layout leaves absent channels at zero. m/block NULL is a no-op. */
void aius_meter_accumulate_input(AiusMeter *m, const AiusPcmBlock *block);

/* Accumulate stereo-output peak + sum-of-squares and refresh the decimated
 * waveform envelope from this block's L/R result. m or a NULL plane is a no-op. */
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
 * snapshot behind the seqlock, then reset the windowed accumulators. m NULL is a
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
 * `input` == NULL bypasses the input stage (decoded planes feed the downmix
 * directly). `limiter` == NULL bypasses the limiter tail. Both default to NULL
 * under aggregate zero-initialization, so a config built only through the
 * downmix/EQ fields behaves exactly as before these stages existed.
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
} AiusDspConfig;

/* Stage `in` through the input stage (if any) into the stage's scratch planes,
 * downmix into `out`, run the EQ cascade over L and R, then the limiter (if any).
 * Same `out` buffer contract and return values as aius_dsp_downmix, plus
 * AIUS_INVALID_ARG when eq_sections > 0 but eq or eq_state is NULL, or when the
 * input stage is present but a scratch plane for a decoded channel is NULL.
 * Allocation-free. */
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
