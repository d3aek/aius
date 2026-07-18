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
 * into int32, at 48 kHz) into a 2-channel 48 kHz stereo block. The path is two
 * stages the caller can run together (aius_dsp_process) or separately:
 *
 *   1. Downmix   (aius_dsp_downmix)  N channels -> L/R per ITU-R BS.775 /
 *                                    ATSC A/52 (see AiusDownmixConfig).
 *   2. EQ        (aius_dsp_eq_run)   a Direct Form I biquad cascade per channel,
 *                                    coefficients supplied by the caller.
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
 * 1.0 leaves level untouched and relies on the clamp as a clip safety net (a
 * fully correlated full-scale 5.1 signal will saturate). A caller wanting a
 * guaranteed clip-free downmix sets headroom to 1/(1 + center + surround + lfe)
 * — see aius_downmix_clipfree_headroom. Default is unity; the clip-free trade
 * (quieter but never saturating) is a PRODUCT DECISION the owner should
 * confirm. */
#define AIUS_DSP_HEADROOM_DEFAULT 1.0

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

/* Full output DSP config: downmix + optional EQ cascade run on BOTH stereo
 * channels. `eq` is `eq_sections` sections; `eq_state` is 2*eq_sections states,
 * channel-major (all L-channel section states, then all R-channel). eq_sections
 * == 0 (or eq == NULL) runs downmix only. */
typedef struct {
    AiusDownmixConfig downmix;
    const AiusBiquad *eq;
    size_t eq_sections;
    AiusBiquadState *eq_state;
} AiusDspConfig;

/* Downmix `in` into `out` then run the EQ cascade over L and R. Same `out`
 * buffer contract and return values as aius_dsp_downmix, plus AIUS_INVALID_ARG
 * when eq_sections > 0 but eq or eq_state is NULL. Allocation-free. */
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
