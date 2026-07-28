/* Aius output DSP: 5.1-to-stereo downmix + Direct Form I biquad EQ.
 *
 * See lib/aius_dsp/include/aius_dsp.h for the contract, the standards followed
 * (ITU-R BS.775 / ATSC A/52 downmix, RBJ cookbook biquads), and the
 * double-precision determinism rationale. This file writes no fused
 * multiply-add and keeps all state in double so host and target stay
 * bit-identical under -ffp-contract=off.
 */

#include "aius_dsp.h"

#include <math.h>
#include <string.h>

/* The saturation range and the declared sample width must describe the same
 * format. Retargeting AIUS_PCM_SAMPLE_BITS without its MAX/MIN (or vice versa)
 * would leave the DSP saturating to one width while every producer left-aligns to
 * another -- silently, and 8 bits at a time. That is the 48 dB pass-through defect
 * exactly (aius_codec.h §SAMPLE SCALE), so it is a compile error rather than a
 * comment. Lives here and not in the header because aius_codec.h is included from
 * C++ translation units, where _Static_assert is not the spelling. */
_Static_assert(AIUS_DSP_SAMPLE_MAX == ((int32_t)1 << (AIUS_PCM_SAMPLE_BITS - 1)) - 1,
               "AIUS_PCM_SAMPLE_MAX disagrees with AIUS_PCM_SAMPLE_BITS");
_Static_assert(AIUS_DSP_SAMPLE_MIN == -((int32_t)1 << (AIUS_PCM_SAMPLE_BITS - 1)),
               "AIUS_PCM_SAMPLE_MIN disagrees with AIUS_PCM_SAMPLE_BITS");

/* --- helpers -------------------------------------------------------------- */

/* popcount over the low channel-mask bits (mask is at most 6 bits). */
static int aius_popcount(uint32_t v)
{
    int n = 0;
    while (v) {
        v &= v - 1u;
        n++;
    }
    return n;
}

/* Plane index of channel `bit` within a tightly-packed mask, or -1 if absent.
 * Channels are packed in ascending AIUS_CH_* bit order, so the index is the
 * count of present channels below `bit`. */
static int aius_plane_index(uint32_t mask, uint32_t bit)
{
    if (!(mask & bit)) {
        return -1;
    }
    return aius_popcount(mask & (bit - 1u));
}

/* One Direct Form I biquad section, in double, advancing the caller's delay
 * memory: y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2].
 *
 * THE ONLY DEFINITION of the recurrence in this module. Both consumers go through
 * it: run_cascade (the EQ and input stages) and the LFE fold-in crossover. Since
 * the single-quantize chain landed, NEITHER quantizes its result -- everything
 * stays in double until aius_dsp_process's final quantize. The expression is
 * written out in one place precisely because the two must stay bit-identical --
 * and because -ffp-contract=off means what is written is what executes, term by
 * term, with no fused multiply-add on either host or target. */
static double biquad_step(const AiusBiquad *q, AiusBiquadState *st, double x)
{
    double y = q->b0 * x + q->b1 * st->x1 + q->b2 * st->x2
               - q->a1 * st->y1 - q->a2 * st->y2;
    st->x2 = st->x1;
    st->x1 = x;
    st->y2 = st->y1;
    st->y1 = y;
    return y;
}

/* Round to nearest (ties away from zero) and saturate into the 24-bit range.
 * Deterministic: no library rounding-mode dependence. THE audio path calls this
 * exactly once per output sample, at the end of aius_dsp_process -- the wire
 * ceiling lives here and nowhere between the stages. */
static int32_t aius_quantize(double x)
{
    double r = (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
    if (r > (double)AIUS_DSP_SAMPLE_MAX) {
        return AIUS_DSP_SAMPLE_MAX;
    }
    if (r < (double)AIUS_DSP_SAMPLE_MIN) {
        return AIUS_DSP_SAMPLE_MIN;
    }
    return (int32_t)r;
}

/* --- LFE fold-in crossover (LR4 low-pass) ---------------------------------
 *
 * Ahead of the downmix because the downmix consumes it. aius_lfe_fold_init calls
 * aius_biquad_lowpass, whose definition is further down; it is declared in
 * aius_dsp.h, so the ordering is a reading choice rather than a constraint. */

void aius_lfe_fold_reset(AiusLfeFold *fold)
{
    if (fold == NULL) {
        return;
    }
    memset(fold->state, 0, sizeof fold->state);
}

AiusStatus aius_lfe_fold_init(AiusLfeFold *fold, double fs, double crossover_hz)
{
    if (fold == NULL) {
        return AIUS_INVALID_ARG;
    }
    /* Leave an unusable request RESET AND UNDESIGNED rather than half-built: a
     * zero designed_hz is what the downmix reads as "no usable crossover", and it
     * then skips the LFE instead of folding it unfiltered. Zero first, so an
     * early return cannot leave stale coefficients behind. */
    memset(fold, 0, sizeof *fold);
    if (!(fs > 0.0) || !(crossover_hz > 0.0) || !(crossover_hz < fs / 2.0)) {
        return AIUS_INVALID_ARG;
    }
    for (int k = 0; k < AIUS_DSP_LFE_CROSSOVER_SECTIONS; k++) {
        fold->lp[k] = aius_biquad_lowpass(fs, crossover_hz,
                                          AIUS_DSP_LFE_CROSSOVER_Q);
    }
    fold->designed_hz = crossover_hz;
    fold->designed_fs = fs;
    return AIUS_OK;
}

/* Run one raw LFE sample through the designed cascade. No quantize: the result is
 * summed into the matrix accumulator while still in double, so the fold-in adds no
 * requantisation step of its own. */
static double lfe_fold_sample(AiusLfeFold *fold, double x)
{
    for (int k = 0; k < AIUS_DSP_LFE_CROSSOVER_SECTIONS; k++) {
        x = biquad_step(&fold->lp[k], &fold->state[k], x);
    }
    return x;
}

/* Resolve the crossover for this block, off the per-sample path.
 *
 * Returns the fold to run the LFE plane through, NULL for an unfiltered fold, or
 * -- via `*skip_lfe` -- the fail-closed verdict: a configured corner with no state
 * to hold it, or a corner the rate cannot carry, drops the LFE entirely rather
 * than folding it unfiltered (see aius_dsp.h §AiusDownmixConfig).
 *
 * Re-derives the sections when the corner or the rate has moved since they were
 * designed, which is what keeps "designed from fs, never from an assumed rate"
 * true when the engine rate becomes runtime data. At most once per change. */
static AiusLfeFold *resolve_lfe_fold(const AiusDownmixConfig *cfg, double fs,
                                     int *skip_lfe)
{
    *skip_lfe = 0;
    if (cfg->lfe == 0.0) {
        return NULL; /* branch is skipped anyway; nothing to resolve */
    }
    if (cfg->lfe_crossover_hz <= 0.0) {
        return NULL; /* explicitly unfiltered fold: no state required */
    }
    AiusLfeFold *fold = cfg->lfe_fold;
    if (fold == NULL) {
        *skip_lfe = 1; /* fail closed: LFE excluded, never folded unfiltered */
        return NULL;
    }
    if (fold->designed_hz != cfg->lfe_crossover_hz || fold->designed_fs != fs) {
        if (aius_lfe_fold_init(fold, fs, cfg->lfe_crossover_hz) != AIUS_OK) {
            *skip_lfe = 1; /* corner unusable at this rate: same fail-closed rule */
            return NULL;
        }
    }
    return fold;
}

/* --- downmix -------------------------------------------------------------- */

AiusDownmixConfig aius_downmix_config_default(void)
{
    AiusDownmixConfig c;
    c.center = AIUS_DSP_DOWNMIX_CENTER_DEFAULT;
    c.surround = AIUS_DSP_DOWNMIX_SURROUND_DEFAULT;
    c.lfe = AIUS_DSP_LFE_GAIN_DEFAULT;
    c.headroom = AIUS_DSP_HEADROOM_DEFAULT;
    c.lfe_crossover_hz = AIUS_DSP_LFE_CROSSOVER_DEFAULT_HZ;
    c.lfe_fold = NULL; /* caller-owned state; see aius_dsp.h */
    return c;
}

double aius_downmix_clipfree_headroom(const AiusDownmixConfig *cfg)
{
    if (cfg == NULL) {
        return AIUS_DSP_HEADROOM_DEFAULT;
    }
    double lfe = cfg->lfe < 0.0 ? -cfg->lfe : cfg->lfe;
    double denom = 1.0 + cfg->center + cfg->surround + lfe;
    if (denom <= 1.0) {
        return 1.0;
    }
    return 1.0 / denom;
}

AiusStatus aius_dsp_downmix(const AiusDownmixConfig *cfg,
                            const AiusPcmBlockD *in,
                            double *out_l, double *out_r)
{
    if (cfg == NULL || in == NULL || out_l == NULL || out_r == NULL) {
        return AIUS_INVALID_ARG;
    }
    if (in->channels < 1 || in->channels > AIUS_MAX_CHANNELS) {
        return AIUS_INVALID_ARG;
    }
    if ((int)in->channels != aius_popcount(in->channel_mask)) {
        return AIUS_INVALID_ARG;
    }

    double *outL = out_l;
    double *outR = out_r;
    double headroom = cfg->headroom;

    /* Mono: replicate the single plane to both channels at unity gain. */
    if (in->channels == 1) {
        const double *m = in->planes[0];
        if (m == NULL) {
            return AIUS_INVALID_ARG;
        }
        for (uint32_t i = 0; i < in->frames; i++) {
            double v = m[i] * headroom;
            outL[i] = v;
            outR[i] = v;
        }
    } else {
        /* Resolve plane pointers for every possible 5.1 channel (NULL if
         * absent). */
        int iFL = aius_plane_index(in->channel_mask, AIUS_CH_FL);
        int iFR = aius_plane_index(in->channel_mask, AIUS_CH_FR);
        int iFC = aius_plane_index(in->channel_mask, AIUS_CH_FC);
        int iLFE = aius_plane_index(in->channel_mask, AIUS_CH_LFE);
        int iSL = aius_plane_index(in->channel_mask, AIUS_CH_SL);
        int iSR = aius_plane_index(in->channel_mask, AIUS_CH_SR);

        const double *pFL = iFL >= 0 ? in->planes[iFL] : NULL;
        const double *pFR = iFR >= 0 ? in->planes[iFR] : NULL;
        const double *pFC = iFC >= 0 ? in->planes[iFC] : NULL;
        const double *pLFE = iLFE >= 0 ? in->planes[iLFE] : NULL;
        const double *pSL = iSL >= 0 ? in->planes[iSL] : NULL;
        const double *pSR = iSR >= 0 ? in->planes[iSR] : NULL;

        /* LFE fold-in crossover, resolved once per block rather than per sample:
         * the design/redesign decision, the unfiltered selection and the
         * fail-closed rule all cost nothing in the loop. `skip_lfe` folds into
         * pLFE so the per-sample path keeps exactly the one null test it had. */
        int skip_lfe = 0;
        AiusLfeFold *fold =
            resolve_lfe_fold(cfg, (double)in->sample_rate_hz, &skip_lfe);
        if (skip_lfe) {
            pLFE = NULL;
        }

        for (uint32_t i = 0; i < in->frames; i++) {
            double l = 0.0;
            double r = 0.0;
            if (pFL) {
                l += pFL[i];
            }
            if (pFR) {
                r += pFR[i];
            }
            if (pFC) {
                double c = pFC[i] * cfg->center;
                l += c;
                r += c;
            }
            if (pLFE) {
                /* Low-pass the raw sample, THEN apply the fold gain, so the
                 * crossover state stays in raw sample units and a gain change
                 * needs no reset. Symmetric into both: one mono channel, no
                 * image. */
                double e = pLFE[i];
                if (fold != NULL) {
                    e = lfe_fold_sample(fold, e);
                }
                e *= cfg->lfe;
                l += e;
                r += e;
            }
            if (pSL) {
                l += pSL[i] * cfg->surround;
            }
            if (pSR) {
                r += pSR[i] * cfg->surround;
            }
            /* NO quantize: the pad's result stays in double for the output EQ,
             * limiter and the chain's single final quantize. */
            outL[i] = l * headroom;
            outR[i] = r * headroom;
        }
    }

    return AIUS_OK;
}

/* --- biquad coefficient math (RBJ audio EQ cookbook) ---------------------- */

AiusBiquad aius_biquad_bypass(void)
{
    AiusBiquad q;
    q.b0 = 1.0;
    q.b1 = 0.0;
    q.b2 = 0.0;
    q.a1 = 0.0;
    q.a2 = 0.0;
    return q;
}

/* Normalize by a0 and store into an AiusBiquad (negating the feedback taps so
 * the run loop subtracts them). */
static AiusBiquad aius_biquad_norm(double b0, double b1, double b2,
                                   double a0, double a1, double a2)
{
    AiusBiquad q;
    q.b0 = b0 / a0;
    q.b1 = b1 / a0;
    q.b2 = b2 / a0;
    q.a1 = a1 / a0;
    q.a2 = a2 / a0;
    return q;
}

AiusBiquad aius_biquad_lowpass(double fs, double f0, double q)
{
    double w0 = 2.0 * M_PI * f0 / fs;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double b1 = 1.0 - cw;
    double b0 = b1 / 2.0;
    double b2 = b0;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cw;
    double a2 = 1.0 - alpha;
    return aius_biquad_norm(b0, b1, b2, a0, a1, a2);
}

AiusBiquad aius_biquad_highpass(double fs, double f0, double q)
{
    double w0 = 2.0 * M_PI * f0 / fs;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double b0 = (1.0 + cw) / 2.0;
    double b1 = -(1.0 + cw);
    double b2 = b0;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cw;
    double a2 = 1.0 - alpha;
    return aius_biquad_norm(b0, b1, b2, a0, a1, a2);
}

AiusBiquad aius_biquad_peaking(double fs, double f0, double q, double gain_db)
{
    double A = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * M_PI * f0 / fs;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double b0 = 1.0 + alpha * A;
    double b1 = -2.0 * cw;
    double b2 = 1.0 - alpha * A;
    double a0 = 1.0 + alpha / A;
    double a1 = -2.0 * cw;
    double a2 = 1.0 - alpha / A;
    return aius_biquad_norm(b0, b1, b2, a0, a1, a2);
}

AiusBiquad aius_biquad_lowshelf(double fs, double f0, double q, double gain_db)
{
    double A = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * M_PI * f0 / fs;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double tsa = 2.0 * sqrt(A) * alpha;
    double b0 = A * ((A + 1.0) - (A - 1.0) * cw + tsa);
    double b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
    double b2 = A * ((A + 1.0) - (A - 1.0) * cw - tsa);
    double a0 = (A + 1.0) + (A - 1.0) * cw + tsa;
    double a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
    double a2 = (A + 1.0) + (A - 1.0) * cw - tsa;
    return aius_biquad_norm(b0, b1, b2, a0, a1, a2);
}

AiusBiquad aius_biquad_highshelf(double fs, double f0, double q, double gain_db)
{
    double A = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * M_PI * f0 / fs;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double tsa = 2.0 * sqrt(A) * alpha;
    double b0 = A * ((A + 1.0) + (A - 1.0) * cw + tsa);
    double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
    double b2 = A * ((A + 1.0) + (A - 1.0) * cw - tsa);
    double a0 = (A + 1.0) - (A - 1.0) * cw + tsa;
    double a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
    double a2 = (A + 1.0) - (A - 1.0) * cw - tsa;
    return aius_biquad_norm(b0, b1, b2, a0, a1, a2);
}

size_t aius_eq_default(AiusBiquad *sections, size_t cap)
{
    if (sections == NULL || cap == 0) {
        return 0;
    }
    sections[0] = aius_biquad_bypass();
    return 1;
}

/* --- EQ run --------------------------------------------------------------- */

/* Shared cascade core: multiply each sample by `pregain`, run the `n`-section
 * Direct Form I cascade, and quantize once. n == 0 applies pregain + quantize
 * only. Callers guarantee `state` holds `n` sections when n > 0. */
/* NO QUANTIZE: the cascade's result stays in double. Inter-stage rounding and
 * clamping were the defect the single-quantize chain removed -- a boost that
 * overshoots full scale here is absorbed by the downmix pad instead of
 * hard-clipping before the limiter can see it. */
static void run_cascade(double pregain, const AiusBiquad *sections, size_t n,
                        AiusBiquadState *state, double *samples,
                        uint32_t frames)
{
    for (uint32_t i = 0; i < frames; i++) {
        double s = samples[i] * pregain;
        for (size_t k = 0; k < n; k++) {
            s = biquad_step(&sections[k], &state[k], s);
        }
        samples[i] = s;
    }
}

void aius_dsp_eq_run(const AiusBiquad *sections, size_t n,
                     AiusBiquadState *state,
                     double *samples, uint32_t frames)
{
    if (sections == NULL || state == NULL || samples == NULL || n == 0) {
        return;
    }
    run_cascade(1.0, sections, n, state, samples, frames);
}

/* --- input stage: per-channel trim + EQ (pre-downmix) --------------------- */

double aius_db_to_linear(double db)
{
    return pow(10.0, db / 20.0);
}

AiusInputChannelConfig aius_input_channel_default(void)
{
    AiusInputChannelConfig ch;
    ch.gain_linear = 1.0;
    ch.eq = NULL;
    ch.eq_sections = 0;
    ch.eq_state = NULL;
    return ch;
}

void aius_dsp_input_run(const AiusInputChannelConfig *ch,
                        double *samples, uint32_t frames)
{
    if (ch == NULL || samples == NULL) {
        return;
    }
    const AiusBiquad *sec = ch->eq;
    AiusBiquadState *st = ch->eq_state;
    size_t n = ch->eq_sections;
    if (n > 0 && (sec == NULL || st == NULL)) {
        n = 0; /* trim still applies; EQ skipped when misconfigured */
    }
    run_cascade(ch->gain_linear, sec, n, st, samples, frames);
}

AiusInputStage aius_input_stage_default(void)
{
    AiusInputStage stage;
    memset(&stage, 0, sizeof stage);
    for (int c = 0; c < AIUS_DSP_INPUT_CHANNELS; c++) {
        stage.channels[c] = aius_input_channel_default();
    }
    return stage;
}

/* Stage every decoded plane into cfg's double scratch -- the ONE int32->double
 * conversion of the chain, unconditional -- then run each present channel's
 * trim + EQ over its staged plane when an input stage is configured. Channels
 * are packed in ascending AIUS_CH_* bit order, so the k-th present bit is
 * plane index k and its per-channel config is channels[bit ordinal]. Fills
 * `staged` as the double view of `in`. Returns AIUS_INVALID_ARG on a
 * null/absent plane or scratch. */
static AiusStatus stage_input(const AiusDspConfig *cfg,
                              const AiusPcmBlock *in, AiusPcmBlockD *staged)
{
    static const uint32_t order[AIUS_DSP_INPUT_CHANNELS] = {
        AIUS_CH_FL, AIUS_CH_FR, AIUS_CH_FC, AIUS_CH_LFE, AIUS_CH_SL, AIUS_CH_SR,
    };
    if (in->channels < 1 || in->channels > AIUS_MAX_CHANNELS) {
        return AIUS_INVALID_ARG;
    }
    if ((int)in->channels != aius_popcount(in->channel_mask)) {
        return AIUS_INVALID_ARG;
    }

    memset(staged, 0, sizeof *staged);
    staged->channels = in->channels;
    staged->frames = in->frames;
    staged->sample_rate_hz = in->sample_rate_hz;
    staged->channel_mask = in->channel_mask;

    int p = 0;
    for (int b = 0; b < AIUS_DSP_INPUT_CHANNELS; b++) {
        if (!(in->channel_mask & order[b])) {
            continue;
        }
        const int32_t *src = in->planes[p];
        double *dst = cfg->scratch[p];
        if (src == NULL || dst == NULL) {
            return AIUS_INVALID_ARG;
        }
        for (uint32_t i = 0; i < in->frames; i++) {
            dst[i] = (double)src[i];
        }
        if (cfg->input != NULL) {
            aius_dsp_input_run(&cfg->input->channels[b], dst, in->frames);
        }
        staged->planes[p] = dst;
        p++;
    }
    return AIUS_OK;
}

/* --- true-peak safety limiter --------------------------------------------- */

AiusLimiterConfig aius_limiter_config_default(void)
{
    AiusLimiterConfig c;
    c.enabled = AIUS_DSP_LIMITER_ENABLED_DEFAULT;
    c.threshold_dbfs = AIUS_DSP_LIMITER_THRESHOLD_DEFAULT_DBFS;
    c.attack_ms = AIUS_DSP_LIMITER_ATTACK_DEFAULT_MS;
    c.release_ms = AIUS_DSP_LIMITER_RELEASE_DEFAULT_MS;
    c.lookahead_ms = AIUS_DSP_LIMITER_LOOKAHEAD_DEFAULT_MS;
    return c;
}

/* ITU-R BS.1770-4 Annex 2, Table 3: the 4-phase, 12-tap polyphase FIR that
 * 4x-oversamples for true-peak measurement. Every coefficient is an exact
 * multiple of 2^-14, so each is spelled as its integer numerator over 16384 --
 * exactly representable in double, evaluated by the compiler, and therefore
 * bit-identical on host and target with no transcription rounding to argue about.
 *
 * Two structural properties worth knowing, because they are what a reviewer can
 * check the table against: phase 3 is phase 0 reversed and phase 2 is phase 1
 * reversed (the filter is linear phase), and the largest phase's l1 norm is
 * exactly 33142/16384 -- the constant the skip gate in aius_dsp_limiter_run
 * depends on for soundness. */
static const double k_truepeak[AIUS_DSP_TRUEPEAK_PHASES][AIUS_DSP_TRUEPEAK_TAPS] = {
    {    28.0 / 16384.0,   180.0 / 16384.0,  -322.0 / 16384.0,   544.0 / 16384.0,
       -974.0 / 16384.0,  2250.0 / 16384.0, 15924.0 / 16384.0, -1676.0 / 16384.0,
        780.0 / 16384.0,  -436.0 / 16384.0,   244.0 / 16384.0,  -136.0 / 16384.0 },
    {  -478.0 / 16384.0,   480.0 / 16384.0,  -848.0 / 16384.0,  1460.0 / 16384.0,
      -2728.0 / 16384.0,  7620.0 / 16384.0, 12776.0 / 16384.0, -3282.0 / 16384.0,
       1664.0 / 16384.0,  -954.0 / 16384.0,   542.0 / 16384.0,  -310.0 / 16384.0 },
    {  -310.0 / 16384.0,   542.0 / 16384.0,  -954.0 / 16384.0,  1664.0 / 16384.0,
      -3282.0 / 16384.0, 12776.0 / 16384.0,  7620.0 / 16384.0, -2728.0 / 16384.0,
       1460.0 / 16384.0,  -848.0 / 16384.0,   480.0 / 16384.0,  -478.0 / 16384.0 },
    {  -136.0 / 16384.0,   244.0 / 16384.0,  -436.0 / 16384.0,   780.0 / 16384.0,
      -1676.0 / 16384.0, 15924.0 / 16384.0,  2250.0 / 16384.0,  -974.0 / 16384.0,
        544.0 / 16384.0,  -322.0 / 16384.0,   180.0 / 16384.0,    28.0 / 16384.0 },
};

#define AIUS_TP_MASK ((uint32_t)(AIUS_DSP_TRUEPEAK_RING - 1))

/* Write `x` into a channel's interpolation history at `pos` and return the
 * true-peak magnitude for this sample: max(|x|, |each interpolated phase|).
 *
 * `run_fir` is the gate. When it is 0 only |x| is returned, which is why the raw
 * sample is folded into the max unconditionally -- the detector is then never
 * worse than a sample-peak detector, whether the gate is open or shut, and the
 * FIR's zero-primed startup cannot under-read either.
 *
 * The history is a power-of-two ring so tap k is a mask rather than a modulo and
 * pushing a sample is one store rather than a 12-element shift. No oversampled
 * buffer is materialized: only the maximum is wanted, so this adds nothing to the
 * caller's stack frame. */
static double truepeak_push(double *hist, uint32_t pos, double x, int run_fir)
{
    hist[pos & AIUS_TP_MASK] = x;
    double m = x < 0.0 ? -x : x;
    if (!run_fir) {
        return m;
    }
    for (int p = 0; p < AIUS_DSP_TRUEPEAK_PHASES; p++) {
        double acc = 0.0;
        for (uint32_t k = 0; k < AIUS_DSP_TRUEPEAK_TAPS; k++) {
            acc += k_truepeak[p][k] * hist[(pos - k) & AIUS_TP_MASK];
        }
        double a = acc < 0.0 ? -acc : acc;
        if (a > m) {
            m = a;
        }
    }
    return m;
}

double aius_dsp_true_peak(const int32_t *samples, uint32_t frames)
{
    if (samples == NULL || frames == 0) {
        return 0.0;
    }
    /* Own history: this is a measurement helper, not the audio path (the limiter
     * carries its history in AiusLimiterState so its own frame stays flat). */
    double hist[AIUS_DSP_TRUEPEAK_RING];
    memset(hist, 0, sizeof hist);
    double pk = 0.0;
    for (uint32_t i = 0; i < frames; i++) {
        /* Interpolate only over full windows, so the zeroed history is not read as
         * a step into the first sample. */
        int run_fir = i >= (uint32_t)(AIUS_DSP_TRUEPEAK_TAPS - 1);
        double m = truepeak_push(hist, i, (double)samples[i], run_fir);
        if (m > pk) {
            pk = m;
        }
    }
    return pk;
}

double aius_dsp_true_peak_d(const double *samples, uint32_t frames)
{
    if (samples == NULL || frames == 0) {
        return 0.0;
    }
    double hist[AIUS_DSP_TRUEPEAK_RING];
    memset(hist, 0, sizeof hist);
    double pk = 0.0;
    for (uint32_t i = 0; i < frames; i++) {
        int run_fir = i >= (uint32_t)(AIUS_DSP_TRUEPEAK_TAPS - 1);
        double m = truepeak_push(hist, i, samples[i], run_fir);
        if (m > pk) {
            pk = m;
        }
    }
    return pk;
}

/* One-pole smoothing coefficient for a time constant `ms` at `fs` Hz:
 * exp(-1 / (ms * fs / 1000)). Larger ms -> coefficient nearer 1 (slower). */
static double limiter_coeff(double ms, double fs)
{
    double samples = ms * fs / 1000.0;
    if (samples < 1e-9) {
        samples = 1e-9;
    }
    return exp(-1.0 / samples);
}

void aius_dsp_limiter_run(AiusLimiter *lim, double fs,
                          double *l, double *r, uint32_t frames)
{
    if (lim == NULL || l == NULL || r == NULL) {
        return;
    }
    if (!lim->cfg.enabled || fs <= 0.0) {
        return; /* pure bypass: no delay, no gain change */
    }

    /* Derived, off the per-sample path. */
    double thr_lin =
        pow(10.0, lim->cfg.threshold_dbfs / 20.0) * (double)AIUS_DSP_SAMPLE_MAX;
    double atk = limiter_coeff(lim->cfg.attack_ms, fs);
    double rel = limiter_coeff(lim->cfg.release_ms, fs);
    double look = lim->cfg.lookahead_ms * fs / 1000.0;
    uint32_t delay = (uint32_t)(look + 0.5);
    if (delay > AIUS_DSP_LIMITER_MAX_LOOKAHEAD) {
        delay = AIUS_DSP_LIMITER_MAX_LOOKAHEAD;
    }

    /* Skip gate for the 48-tap true-peak FIR. The largest interpolation phase has
     * l1 norm 33142/16384, so a 12-sample window in which every sample satisfies
     * |x| <= thr_lin / (33142/16384) provably cannot interpolate above thr_lin.
     * Below the gate the FIR is skipped with no possibility of missing an over;
     * above it, the FIR runs for the whole window the loud sample participates in.
     * This is what keeps quiet material at one compare per sample instead of 48
     * multiply-accumulates. */
    double gate_lin =
        thr_lin * (AIUS_DSP_TRUEPEAK_L1_DEN / AIUS_DSP_TRUEPEAK_L1_NUM);

    AiusLimiterState *s = &lim->state;
    if (!s->primed || s->delay != delay) {
        s->gain = 1.0;
        s->env = 0.0;
        s->delay = delay;
        s->pos = 0;
        memset(s->buf_l, 0, sizeof s->buf_l);
        memset(s->buf_r, 0, sizeof s->buf_r);
        memset(s->tp_l, 0, sizeof s->tp_l);
        memset(s->tp_r, 0, sizeof s->tp_r);
        s->tp_pos = 0;
        s->tp_run = 0;
        s->tp_fill = 0;
        s->primed = 1;
    }
    /* Unclipped doubles can carry a NaN from a pathological direct-API config
     * (the validated config surface bounds everything); once in `env` it would
     * latch the limiter open forever, so it is cleared per run, not per sample. */
    if (!isfinite(s->env)) {
        s->env = 0.0;
    }
    if (!isfinite(s->gain)) {
        s->gain = 1.0;
    }

    for (uint32_t i = 0; i < frames; i++) {
        double cur_l = l[i];
        double cur_r = r[i];

        /* True-peak follower on the incoming (lookahead-leading) sample: instant
         * attack, release-timed decay. `m` is the 4x-oversampled magnitude, not
         * the sample magnitude, so an inter-sample peak that sits above every
         * sample around it still opens the reduction. */
        double xl = cur_l;
        double xr = cur_r;
        double raw_l = xl < 0.0 ? -xl : xl;
        double raw_r = xr < 0.0 ? -xr : xr;
        double raw = raw_l > raw_r ? raw_l : raw_r;

        /* Arm the FIR for the TAPS samples this sample stays inside the window.
         * Set before the push so the current sample is covered, decremented after
         * it, so exactly samples n..n+TAPS-1 run. Shared across the pair because
         * the gain is stereo-linked anyway. */
        if (raw > gate_lin) {
            s->tp_run = AIUS_DSP_TRUEPEAK_TAPS;
        }
        /* Interpolate only once the history has filled since the last reset, so a
         * config apply mid-stream does not read its own zeroed history as a step
         * and dip the gain for a release constant afterwards. */
        int run_fir = s->tp_run > 0u &&
                      s->tp_fill >= (uint32_t)(AIUS_DSP_TRUEPEAK_TAPS - 1);
        double tp_l = truepeak_push(s->tp_l, s->tp_pos, xl, run_fir);
        double tp_r = truepeak_push(s->tp_r, s->tp_pos, xr, run_fir);
        s->tp_pos = (s->tp_pos + 1u) & AIUS_TP_MASK;
        if (s->tp_fill < (uint32_t)AIUS_DSP_TRUEPEAK_TAPS) {
            s->tp_fill++;
        }
        if (s->tp_run > 0u) {
            s->tp_run--;
        }

        double m = tp_l > tp_r ? tp_l : tp_r;
        if (m > s->env) {
            s->env = m; /* instant attack: never miss a peak */
        } else {
            s->env = m + (s->env - m) * rel; /* release-timed decay toward level */
        }

        /* Desired stereo-linked gain, then smooth it for a click-free ramp. */
        double desired = 1.0;
        if (s->env > thr_lin) {
            desired = thr_lin / s->env;
        }
        double coeff = (desired < s->gain) ? atk : rel;
        s->gain = desired + (s->gain - desired) * coeff;

        /* Apply the gain to the delayed sample so the reduction leads the peak.
         * NO quantize: the gained result stays in double for the chain's single
         * final quantize (an int32 ring here would be a hidden second quantize
         * and would make the envelope disagree with the output it gains). */
        double del_l = cur_l;
        double del_r = cur_r;
        if (delay > 0) {
            del_l = s->buf_l[s->pos];
            del_r = s->buf_r[s->pos];
            s->buf_l[s->pos] = cur_l;
            s->buf_r[s->pos] = cur_r;
            s->pos++;
            if (s->pos >= delay) {
                s->pos = 0;
            }
        }
        l[i] = del_l * s->gain;
        r[i] = del_r * s->gain;
    }
}

double aius_dsp_limiter_gain_reduction_db(const AiusLimiter *lim)
{
    if (lim == NULL) {
        return 0.0;
    }
    double g = lim->state.primed ? lim->state.gain : 1.0;
    if (g <= 0.0) {
        return -1000.0; /* fully closed; defensive, gain stays > 0 in practice */
    }
    if (g > 1.0) {
        g = 1.0;
    }
    return 20.0 * log10(g);
}

/* --- 24 -> 16 bit output stage: TPDF dither, round to nearest, saturate ---- */

/* See aius_dsp.h for why this is a real stage with a seeded PRNG rather than a
 * shift at the call site, and for the determinism argument. Everything below is
 * integer-only except the final scale/round, which is exact.
 *
 * ROTATE: k is only ever 7 or 11 here, so 32 - k is never 32 and the shift is
 * always defined. */
static uint32_t aius_rotl32(uint32_t x, unsigned k)
{
    return (uint32_t)((x << k) | (x >> (32u - k)));
}

/* xoshiro128++ 1.0 (Blackman & Vigna), verbatim. Period 2^128-1; all 32 output
 * bits are scrambled, which is what lets both halves of the word be used as
 * independent uniform draws. uint32 arithmetic wraps by definition, so there is
 * no signed overflow for UBSan to find. */
static uint32_t aius_xoshiro128pp(AiusDitherState *st)
{
    uint32_t *s = st->s;
    const uint32_t result = aius_rotl32(s[0] + s[3], 7) + s[0];
    const uint32_t t = s[1] << 9;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = aius_rotl32(s[3], 11);
    return result;
}

/* SplitMix32, used only to expand one seed into well-separated xoshiro states.
 * A seed expanded by copying would leave the four words correlated and the
 * generator's first outputs poor. */
static uint32_t aius_splitmix32(uint32_t *x)
{
    uint32_t z = (*x += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
}

void aius_out16_init(AiusOut16Stage *st, uint32_t seed, int dither)
{
    if (st == NULL) {
        return;
    }
    st->dither = dither ? 1 : 0;
    /* Eight consecutive SplitMix32 outputs of the one seed: the first four seed
     * L, the next four R. One seed, two streams, and the pair is reproducible
     * from that seed alone -- which is what makes a reset replay the same
     * sequence. */
    uint32_t x = seed;
    for (int c = 0; c < 2; c++) {
        for (int k = 0; k < 4; k++) {
            st->stream[c].s[k] = aius_splitmix32(&x);
        }
        /* All-zero is xoshiro's fixed point (it would emit zeros forever).
         * SplitMix32 will not produce it from any seed, but a stage that can only
         * fail silently should not depend on that. */
        if ((st->stream[c].s[0] | st->stream[c].s[1] | st->stream[c].s[2] |
             st->stream[c].s[3]) == 0u) {
            st->stream[c].s[0] = 1u;
        }
    }
}

AiusOut16Stage aius_out16_stage_default(void)
{
    AiusOut16Stage st;
    aius_out16_init(&st, AIUS_DSP_DITHER_SEED_DEFAULT, 1);
    return st;
}

int32_t aius_out16_sample(AiusDitherState *d, int dither, int32_t x24)
{
    /* Into 16-bit units. Exact: AIUS_DSP_OUT16_LSB is a power of two, so this is
     * an exponent decrement and loses nothing. */
    double q = (double)x24 / AIUS_DSP_OUT16_LSB;

    if (dither && d != NULL) {
        /* Two uniform draws from one word. Their difference is triangular on
         * (-65536, 65536), i.e. +-1 LSB after the divide -- 2 LSB peak-to-peak
         * TPDF, the amplitude that makes the error power signal-independent.
         * Both operands are in [0, 65535] so the difference cannot overflow, and
         * an integer under 2^17 over 65536 is exactly representable, so the
         * dither adds no rounding of its own. */
        uint32_t w = aius_xoshiro128pp(d);
        int32_t hi = (int32_t)(w >> 16);
        int32_t lo = (int32_t)(w & 0xFFFFu);
        q += (double)(hi - lo) / 65536.0;
    }

    /* Round to nearest, ties away from zero -- the same rule as aius_quantize, so
     * the two quantizers in this module cannot disagree about a boundary. This is
     * the half of the fix that removes the shift's -0.5 LSB DC offset. */
    double r = (q >= 0.0) ? floor(q + 0.5) : ceil(q - 0.5);
    if (r > (double)AIUS_DSP_OUT16_MAX) {
        return AIUS_DSP_OUT16_MAX;
    }
    if (r < (double)AIUS_DSP_OUT16_MIN) {
        return AIUS_DSP_OUT16_MIN;
    }
    return (int32_t)r;
}

void aius_out16_run(AiusOut16Stage *st,
                    const int32_t *l24, const int32_t *r24,
                    int16_t *l16, int16_t *r16, uint32_t frames)
{
    if (st == NULL || l24 == NULL || r24 == NULL || l16 == NULL || r16 == NULL) {
        return;
    }
    const int dither = st->dither;
    for (uint32_t i = 0; i < frames; i++) {
        l16[i] = (int16_t)aius_out16_sample(&st->stream[0], dither, l24[i]);
        r16[i] = (int16_t)aius_out16_sample(&st->stream[1], dither, r24[i]);
    }
}

void aius_out_stage_run(AiusOut16Stage *st, int width_bits,
                        const int32_t *l24, const int32_t *r24,
                        int32_t *l_out, int32_t *r_out, uint32_t frames)
{
    if (l24 == NULL || r24 == NULL || l_out == NULL || r_out == NULL) {
        return;
    }
    if (width_bits == AIUS_DSP_OUT_WIDTH_24) {
        /* The bypass, and the whole point of the 24-bit path: the DSP's result
         * IS the transmit word. Not memcpy, because the planes are int32 and a
         * word loop is what the compiler wants here anyway; the point is that
         * no arithmetic touches the sample. */
        for (uint32_t i = 0; i < frames; i++) {
            l_out[i] = l24[i];
            r_out[i] = r24[i];
        }
        return;
    }
    if (width_bits == AIUS_DSP_OUT_WIDTH_16) {
        if (st == NULL) {
            return;
        }
        const int dither = st->dither;
        for (uint32_t i = 0; i < frames; i++) {
            /* aius_out16_sample() then the `<< 8` the S/PDIF snapshot's ISR
             * used to perform, as a multiply by the same exact power of two.
             * The low 8 bits are zero by construction, which is what a 16-bit
             * selection means on a 24-bit wire. */
            l_out[i] = aius_out16_sample(&st->stream[0], dither, l24[i]) *
                       (int32_t)AIUS_DSP_OUT16_LSB;
            r_out[i] = aius_out16_sample(&st->stream[1], dither, r24[i]) *
                       (int32_t)AIUS_DSP_OUT16_LSB;
        }
        return;
    }
    /* An unrecognised width emits nothing rather than guessing. */
}

/* --- coherent meter snapshot ---------------------------------------------- */

/* AIUS_CH_* bit order for the six metered input channels, matching the input
 * stage's channel indexing. */
static const uint32_t k_meter_order[AIUS_METER_INPUT_CHANNELS] = {
    AIUS_CH_FL, AIUS_CH_FR, AIUS_CH_FC, AIUS_CH_LFE, AIUS_CH_SL, AIUS_CH_SR,
};

void aius_meter_init(AiusMeter *m, uint32_t sample_rate_hz)
{
    if (m == NULL) {
        return;
    }
    memset(m, 0, sizeof *m);
    m->sample_rate_hz = sample_rate_hz;
}

void aius_meter_reset_accumulators(AiusMeter *m)
{
    if (m == NULL) {
        return;
    }
    memset(m->in_peak, 0, sizeof m->in_peak);
    memset(m->in_sqsum, 0, sizeof m->in_sqsum);
    memset(m->out_peak, 0, sizeof m->out_peak);
    memset(m->out_sqsum, 0, sizeof m->out_sqsum);
    memset(m->wave, 0, sizeof m->wave);
    m->wave_count = 0;
    m->in_frames = 0;
    m->out_frames = 0;
}

void aius_meter_accumulate_input(AiusMeter *m, const AiusPcmBlock *block)
{
    if (m == NULL || block == NULL || block->frames == 0) {
        return;
    }
    int p = 0;
    for (int c = 0; c < AIUS_METER_INPUT_CHANNELS; c++) {
        if (!(block->channel_mask & k_meter_order[c])) {
            continue;
        }
        const int32_t *plane = block->planes[p];
        p++;
        if (plane == NULL) {
            continue; /* misconfigured plane: leave this channel at zero */
        }
        double peak = m->in_peak[c];
        double sqsum = m->in_sqsum[c];
        for (uint32_t i = 0; i < block->frames; i++) {
            double v = (double)plane[i];
            double a = v < 0.0 ? -v : v;
            if (a > peak) {
                peak = a;
            }
            sqsum += v * v;
        }
        m->in_peak[c] = peak;
        m->in_sqsum[c] = sqsum;
    }
    m->in_frames += block->frames;
}

void aius_meter_accumulate_output(AiusMeter *m, const int32_t *l, const int32_t *r,
                                  uint32_t frames)
{
    if (m == NULL || l == NULL || r == NULL || frames == 0) {
        return;
    }
    double peak_l = m->out_peak[0];
    double peak_r = m->out_peak[1];
    double sq_l = m->out_sqsum[0];
    double sq_r = m->out_sqsum[1];
    for (uint32_t i = 0; i < frames; i++) {
        double vl = (double)l[i];
        double vr = (double)r[i];
        double al = vl < 0.0 ? -vl : vl;
        double ar = vr < 0.0 ? -vr : vr;
        if (al > peak_l) {
            peak_l = al;
        }
        if (ar > peak_r) {
            peak_r = ar;
        }
        sq_l += vl * vl;
        sq_r += vr * vr;
    }
    m->out_peak[0] = peak_l;
    m->out_peak[1] = peak_r;
    m->out_sqsum[0] = sq_l;
    m->out_sqsum[1] = sq_r;
    m->out_frames += frames;
    m->wave_count = aius_meter_decimate(l, r, frames, m->wave, AIUS_METER_WAVEFORM_MAX);
}

void aius_meter_set_limiter_gr(AiusMeter *m, double gain_reduction_db)
{
    if (m == NULL) {
        return;
    }
    m->limiter_gr_db = gain_reduction_db;
}

void aius_meter_set_telemetry(AiusMeter *m, uint32_t queue_frames,
                              uint32_t estimated_latency_us,
                              uint32_t dsp_cycles_last, uint32_t dsp_cycles_max)
{
    if (m == NULL) {
        return;
    }
    m->queue_frames = queue_frames;
    m->estimated_latency_us = estimated_latency_us;
    m->dsp_cycles_last = dsp_cycles_last;
    m->dsp_cycles_max = dsp_cycles_max;
}

uint32_t aius_meter_decimate(const int32_t *l, const int32_t *r, uint32_t frames,
                             int32_t *out, uint32_t cap)
{
    if (out == NULL || cap == 0 || frames == 0 || l == NULL) {
        return 0;
    }
    uint32_t points = frames < cap ? frames : cap;
    for (uint32_t b = 0; b < points; b++) {
        /* Contiguous, near-equal buckets: [start, end). Computed in 64-bit so the
         * product cannot overflow before the divide. */
        uint32_t start = (uint32_t)((uint64_t)b * frames / points);
        uint32_t end = (uint32_t)((uint64_t)(b + 1) * frames / points);
        int32_t pk = 0;
        for (uint32_t i = start; i < end; i++) {
            int32_t al = l[i] < 0 ? -l[i] : l[i];
            if (al > pk) {
                pk = al;
            }
            if (r != NULL) {
                int32_t ar = r[i] < 0 ? -r[i] : r[i];
                if (ar > pk) {
                    pk = ar;
                }
            }
        }
        out[b] = pk;
    }
    return points;
}

void aius_meter_publish(AiusMeter *m)
{
    if (m == NULL) {
        return;
    }
    AiusMeterSnapshot *s = &m->published;

    /* Seqlock write boundary: make seq odd before touching the payload. */
    m->seq++;

    m->publishes++;
    s->sequence = m->publishes;
    s->sample_rate_hz = m->sample_rate_hz;

    for (int c = 0; c < AIUS_METER_INPUT_CHANNELS; c++) {
        s->input_peak[c] = m->in_peak[c];
        s->input_rms[c] = m->in_frames > 0
                              ? sqrt(m->in_sqsum[c] / (double)m->in_frames)
                              : 0.0;
    }
    for (int k = 0; k < 2; k++) {
        s->output_peak[k] = m->out_peak[k];
        s->output_rms[k] = m->out_frames > 0
                               ? sqrt(m->out_sqsum[k] / (double)m->out_frames)
                               : 0.0;
    }
    s->limiter_gain_reduction_db = m->limiter_gr_db;

    s->waveform_count = m->wave_count;
    for (uint32_t i = 0; i < AIUS_METER_WAVEFORM_MAX; i++) {
        s->waveform[i] = i < m->wave_count ? m->wave[i] : 0;
    }

    s->queue_frames = m->queue_frames;
    s->estimated_latency_us = m->estimated_latency_us;
    s->dsp_cycles_last = m->dsp_cycles_last;
    s->dsp_cycles_max = m->dsp_cycles_max;

    /* Seqlock write boundary: make seq even to mark the snapshot complete. */
    m->seq++;

    aius_meter_reset_accumulators(m);
}

int aius_meter_read(const AiusMeter *m, AiusMeterSnapshot *out)
{
    if (m == NULL || out == NULL) {
        return 0;
    }
    for (int tries = 0; tries < AIUS_METER_READ_RETRIES; tries++) {
        uint32_t s1 = m->seq;
        if (s1 & 1u) {
            continue; /* publish in progress: retry */
        }
        *out = m->published;
        uint32_t s2 = m->seq;
        if (s1 == s2) {
            return 1; /* coherent: no publish straddled the copy */
        }
    }
    return 0; /* contended past the retry bound: reject, never return torn */
}

/* --- full process --------------------------------------------------------- */

AiusStatus aius_dsp_process(const AiusDspConfig *cfg,
                            const AiusPcmBlock *in,
                            AiusPcmBlock *out)
{
    if (cfg == NULL || in == NULL || out == NULL) {
        return AIUS_INVALID_ARG;
    }
    if (cfg->eq_sections > 0 && (cfg->eq == NULL || cfg->eq_state == NULL)) {
        return AIUS_INVALID_ARG;
    }
    if (cfg->work_l == NULL || cfg->work_r == NULL) {
        return AIUS_INVALID_ARG;
    }
    if (out->planes[0] == NULL || out->planes[1] == NULL) {
        return AIUS_INVALID_ARG;
    }

    /* The one int32 -> double boundary: every present plane is convert-copied
     * into cfg->scratch (and trim/EQ'd there when an input stage is present).
     * Unconditional -- staging IS the conversion, not part of the optional
     * trim/EQ. */
    AiusPcmBlockD staged;
    AiusStatus ist = stage_input(cfg, in, &staged);
    if (ist != AIUS_OK) {
        return ist;
    }

    AiusStatus st = aius_dsp_downmix(&cfg->downmix, &staged,
                                     cfg->work_l, cfg->work_r);
    if (st != AIUS_OK) {
        return st;
    }

    if (cfg->eq_sections > 0) {
        aius_dsp_eq_run(cfg->eq, cfg->eq_sections, cfg->eq_state,
                        cfg->work_l, in->frames);
        aius_dsp_eq_run(cfg->eq, cfg->eq_sections,
                        cfg->eq_state + cfg->eq_sections,
                        cfg->work_r, in->frames);
    }

    if (cfg->limiter != NULL) {
        aius_dsp_limiter_run(cfg->limiter, (double)in->sample_rate_hz,
                             cfg->work_l, cfg->work_r, in->frames);
    }

    /* THE final quantize: the chain's only rounding and its only clamp. The
     * 24-bit wire ceiling exists here and nowhere upstream. */
    int32_t *outL = out->planes[0];
    int32_t *outR = out->planes[1];
    for (uint32_t i = 0; i < in->frames; i++) {
        outL[i] = aius_quantize(cfg->work_l[i]);
        outR[i] = aius_quantize(cfg->work_r[i]);
    }
    out->channels = 2;
    out->frames = in->frames;
    out->sample_rate_hz = in->sample_rate_hz;
    out->channel_mask = AIUS_CH_FL | AIUS_CH_FR;

    return AIUS_OK;
}

/* --- arena output allocation ---------------------------------------------- */

AiusStatus aius_dsp_alloc_stereo(AiusArena *arena, uint32_t frames,
                                 uint32_t sample_rate_hz, AiusPcmBlock *out)
{
    if (arena == NULL || out == NULL) {
        return AIUS_INVALID_ARG;
    }
    size_t bytes = (size_t)frames * sizeof(int32_t);
    int32_t *l = (int32_t *)aius_arena_alloc(arena, bytes, sizeof(int32_t));
    int32_t *r = (int32_t *)aius_arena_alloc(arena, bytes, sizeof(int32_t));
    if ((frames > 0 && (l == NULL || r == NULL))) {
        return AIUS_NO_SPACE;
    }
    memset(out, 0, sizeof *out);
    out->planes[0] = l;
    out->planes[1] = r;
    out->channels = 2;
    out->frames = frames;
    out->sample_rate_hz = sample_rate_hz;
    out->channel_mask = AIUS_CH_FL | AIUS_CH_FR;
    return AIUS_OK;
}
