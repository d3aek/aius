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
 * higher sample rate clamps the delay to this cap. */
#define AIUS_DSP_LIMITER_MAX_LOOKAHEAD 240

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

/* A disabled (transparent) limiter with neutral time constants. */
AiusLimiterConfig aius_limiter_config_default(void);

/* Running state + lookahead delay lines. Zero-initialize before first use; the
 * envelope coefficients and delay length are derived from the config and sample
 * rate inside aius_dsp_limiter_run and re-derived when either changes (reset-safe
 * config apply). `gain` and the ring carry across blocks for seamless boundaries.
 */
typedef struct {
    double   env;            /* peak-follower envelope (sample units) */
    double   gain;           /* current smoothed gain reduction, (0, 1] */
    uint32_t delay;          /* active lookahead length in samples */
    uint32_t pos;            /* ring write cursor, mod delay */
    int      primed;         /* gain/env initialized on the first run */
    int32_t  buf_l[AIUS_DSP_LIMITER_MAX_LOOKAHEAD];
    int32_t  buf_r[AIUS_DSP_LIMITER_MAX_LOOKAHEAD];
} AiusLimiterState;

/* A limiter is its config plus its running state, so the whole tail stage is one
 * caller-owned, allocation-free object. */
typedef struct {
    AiusLimiterConfig cfg;
    AiusLimiterState  state;
} AiusLimiter;

/* Run the true-peak safety limiter over the stereo pair (`l`, `r`) in place at
 * `fs` Hz. A peak follower (instant attack, release-timed decay) tracks the
 * incoming true peak; a single stereo-linked gain (so the L/R image is
 * preserved) is smoothed toward T/peak with the attack/release time constants
 * and applied to the signal delayed by the lookahead, so the reduction leads the
 * peak. Below threshold the gain is unity (transparent). cfg.enabled == 0 is a
 * pure bypass with no delay. Output frame count equals input (the last `delay`
 * samples stay in the ring as bounded latency). Allocation-free; lim == NULL or
 * a NULL plane is a no-op. */
void aius_dsp_limiter_run(AiusLimiter *lim, double fs,
                          int32_t *l, int32_t *r, uint32_t frames);

/* Current gain reduction in dB (<= 0), for later metering. 0.0 dB means no
 * reduction. lim == NULL returns 0.0. Reads state only; does not process. */
double aius_dsp_limiter_gain_reduction_db(const AiusLimiter *lim);

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
